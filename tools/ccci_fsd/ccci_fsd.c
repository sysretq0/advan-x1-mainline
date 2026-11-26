// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean userspace CCCI filesystem daemon for MediaTek MT6789 (Gen95).
 *
 * Bridges /dev/ccci_fs (CCIF0 Q4 ch=14 <-> ch=15, PORT_F_USER_HEADER) to a
 * sandboxed drive root (/run/ccci_fs/{Z,X,Y,W,V,U,T,S,R,Q}) backed by
 * kernel-enforced read-only NVRAM partitions with RAM tmpfs overlayfs upper
 * layers.
 *
 * Implements bidirectional multi-packet fragmentation (3456 B payload /
 * 3476 B wire packets, bit 31 continuation flag in ccci_header.data[0],
 * 5 concurrent slots indexed by ccci_header.reserved, up to 16,604 B
 * reassembled messages) matching Advan X1 stock ccci_fsd.
 *
 * Copyright (c) 2026 sysretq0 <sysretq0@proton.me>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CCCI_CH_FS_RX		14U
#define CCCI_CH_FS_TX		15U

#define FS_MAX_SLOTS		5U
#define FS_WIRE_HDR_LEN		20U	/* 16 B ccci_header + 4 B op_id */
#define FS_WIRE_PAYLOAD_MAX	3456U	/* CCCI_MTU */
#define FS_WIRE_PKT_MAX		3476U	/* 20 + 3456 */
#define FS_MSG_BUF_MAX		16604U	/* 0x40dc reassembled message cap */
#define FS_FRAG_CONT_BIT	0x80000000U
#define FS_API_RESP_MASK	0xffff0000U
#define FS_MAX_ARGS		8U
#define FS_MAX_HANDLES		128
#define FS_MAX_FIND_HANDLES	32

/* MTK Nucleus/KAL FS error codes */
#define FS_NO_ERROR		0
#define FS_ERROR_RESERVED	(-1)
#define FS_PARAM_ERROR		(-2)
#define FS_INVALID_FILENAME	(-3)
#define FS_DRIVE_NOT_FOUND	(-4)
#define FS_TOO_MANY_FILES	(-5)
#define FS_NO_MORE_FILES	(-6)
#define FS_FILE_NOT_FOUND	(-9)
#define FS_PATH_NOT_FOUND	(-19)
#define FS_FILE_EXISTS		(-36)
#define FS_ACCESS_DENIED	(-44)
#define FS_INVALID_FILE_HANDLE	(-46)
#define FS_DISK_FULL		(-49)

/* MTK FS_Open flags */
#define MTK_FS_READ_ONLY	0x00000100U
#define MTK_FS_OPEN_DIR		0x00000800U
#define MTK_FS_CREATE		0x00010000U
#define MTK_FS_CREATE_ALWAYS	0x00020000U

/* MTK FS_GetDrive types */
#define FS_DRIVE_I_SYSTEM	0x00000004U

/* MTK DOS attributes */
#define FS_ATTR_READ_ONLY	0x01U
#define FS_ATTR_DIR		0x10U
#define FS_ATTR_ARCHIVE		0x20U

/* CMPT opid_map bits */
#define FS_CMPT_OPEN		(1U << 0)
#define FS_CMPT_GETSIZE		(1U << 1)
#define FS_CMPT_SEEK		(1U << 2)
#define FS_CMPT_READ		(1U << 3)
#define FS_CMPT_CLOSE		(1U << 4)
#define FS_CMPT_WRITE		(1U << 5)

enum fs_ccci_op {
	FS_OP_OPEN		= 0x1001,
	FS_OP_SEEK		= 0x1002,
	FS_OP_READ		= 0x1003,
	FS_OP_WRITE		= 0x1004,
	FS_OP_CLOSE		= 0x1005,
	FS_OP_CLOSEALL		= 0x1006,
	FS_OP_CREATEDIR		= 0x1007,
	FS_OP_REMOVEDIR		= 0x1008,
	FS_OP_GETFILESIZE	= 0x1009,
	FS_OP_GETFOLDERSIZE	= 0x100a,
	FS_OP_RENAME		= 0x100b,
	FS_OP_MOVE		= 0x100c,
	FS_OP_COUNT		= 0x100d,
	FS_OP_GETDISKINFO	= 0x100e,
	FS_OP_DELETE		= 0x100f,
	FS_OP_GETATTRIBUTES	= 0x1010,
	FS_OP_OPENHINT		= 0x1011,
	FS_OP_FINDFIRST		= 0x1012,
	FS_OP_FINDNEXT		= 0x1013,
	FS_OP_FINDCLOSE		= 0x1014,
	FS_OP_LOCK_FAT		= 0x1015,
	FS_OP_UNLOCK_ALL	= 0x1016,
	FS_OP_SHUTDOWN		= 0x1017,
	FS_OP_XDELETE		= 0x1018,
	FS_OP_CLEARDISKFLAG	= 0x1019,
	FS_OP_GETDRIVE		= 0x101a,
	FS_OP_GETCLUSTERSIZE	= 0x101b,
	FS_OP_SETDISKFLAG	= 0x101c,
	FS_OP_OTP_WRITE		= 0x101d,
	FS_OP_OTP_READ		= 0x101e,
	FS_OP_OTP_QUERYLEN	= 0x101f,
	FS_OP_OTP_LOCK		= 0x1020,
	FS_OP_RESTORE		= 0x1021,
	FS_OP_CMPT_READ		= 0x1022,
	FS_OP_BIN_REGION_ACCESS	= 0x1023,
	FS_OP_CMPT_WRITE	= 0x1024,
	FS_OP_GETFILEDETAIL	= 0x1025,
};

struct __attribute__((packed)) ccci_header {
	uint32_t data[2];
	uint16_t channel;
	uint16_t seq_assert;
	uint32_t reserved;
};

struct __attribute__((packed)) fs_msg_hdr {
	struct ccci_header ccci;
	uint32_t op_id;
	uint32_t para_num;
};

struct __attribute__((packed)) fs_disk_info {
	char label[24];
	char drive_letter;
	uint8_t write_protect;
	uint8_t reserved[2];
	uint32_t serial_number;
	uint32_t first_physical_sector;
	uint32_t fat_type;
	uint32_t fat_count;
	uint32_t max_dir_entries;
	uint32_t bytes_per_sector;	/* +0x30 */
	uint32_t sectors_per_cluster;	/* +0x34 */
	uint32_t total_clusters;	/* +0x38 */
	uint32_t bad_clusters;		/* +0x3c */
	uint32_t free_clusters;		/* +0x40 */
	uint32_t files;			/* +0x44 */
	uint32_t file_chains;		/* +0x48 */
	uint32_t free_chains;		/* +0x4c */
	uint32_t largest_free_chain;	/* +0x50 */
};

struct __attribute__((packed)) fs_dos_dir_entry {
	char filename[8];
	char extension[3];
	uint8_t attributes;
	uint8_t nt_reserved;
	uint8_t create_time_tenth;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t access_date;
	uint16_t first_cluster_hi;
	uint16_t mod_time;
	uint16_t mod_date;
	uint16_t first_cluster_lo;
	uint32_t file_size;
	uint8_t pad[20];
};

struct __attribute__((packed)) fs_cmpt_read_in {
	uint32_t opid_map;
	uint32_t opid_ret;
	int32_t handle;
	uint32_t flag;
	uint32_t file_size;
	int32_t offset;
	int32_t whence;
	uint32_t data_ptr;
	uint32_t read_len;
	uint32_t actual_read_len;
};

struct __attribute__((packed)) fs_cmpt_write_in {
	uint32_t opid_map;
	uint32_t opid_ret;
	int32_t handle;
	uint32_t flag;
	int32_t offset;
	int32_t whence;
	uint32_t data_ptr;
	uint32_t write_len;
	int32_t out_handle;
	uint32_t actual_write_len;
	uint32_t reserved;
};

struct __attribute__((packed)) fs_file_detail {
	uint64_t mtime;
	uint64_t atime;
	uint64_t ctime;
};

struct fs_arg {
	uint32_t len;
	const uint8_t *buf;
};

struct fs_slot {
	uint8_t buf[FS_MSG_BUF_MAX];
	uint32_t len;
	bool in_progress;
};

struct fs_file_slot {
	int fd;
	bool used;
	char path[PATH_MAX];
};

struct fs_find_slot {
	bool used;
	DIR *dir;
	char dir_path[PATH_MAX];
	char pattern[NAME_MAX + 1];
	uint8_t attr;
	uint8_t attr_mask;
};

_Static_assert(sizeof(struct ccci_header) == 16, "ccci_header size");
_Static_assert(sizeof(struct fs_msg_hdr) == 24, "fs_msg_hdr size");
_Static_assert(sizeof(struct fs_disk_info) == 84, "fs_disk_info size");
_Static_assert(sizeof(struct fs_dos_dir_entry) == 52, "fs_dos_dir_entry size");
_Static_assert(sizeof(struct fs_cmpt_read_in) == 40, "fs_cmpt_read_in size");
_Static_assert(sizeof(struct fs_cmpt_write_in) == 44, "fs_cmpt_write_in size");
_Static_assert(sizeof(struct fs_file_detail) == 24, "fs_file_detail size");

static volatile sig_atomic_t g_stop;
static bool g_verbose;
static char g_root_dir[PATH_MAX] = "/run/ccci_fs";
static struct fs_slot g_slots[FS_MAX_SLOTS];
static struct fs_file_slot g_files[FS_MAX_HANDLES];
static struct fs_find_slot g_finds[FS_MAX_FIND_HANDLES];

/* Advan X1 10-drive set (verified from stock ccci_fsd 0xe9c4..0xeb10) */
static const char g_valid_drives[] = "ZXYWVUTSRQ";

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint32_t align4(uint32_t v)
{
	return (v + 3U) & ~3U;
}

static const char *op_name(uint32_t op)
{
	switch (op) {
	case FS_OP_OPEN: return "OPEN";
	case FS_OP_SEEK: return "SEEK";
	case FS_OP_READ: return "READ";
	case FS_OP_WRITE: return "WRITE";
	case FS_OP_CLOSE: return "CLOSE";
	case FS_OP_CLOSEALL: return "CLOSEALL";
	case FS_OP_CREATEDIR: return "CREATEDIR";
	case FS_OP_REMOVEDIR: return "REMOVEDIR";
	case FS_OP_GETFILESIZE: return "GETFILESIZE";
	case FS_OP_GETFOLDERSIZE: return "GETFOLDERSIZE";
	case FS_OP_RENAME: return "RENAME";
	case FS_OP_MOVE: return "MOVE";
	case FS_OP_COUNT: return "COUNT";
	case FS_OP_GETDISKINFO: return "GETDISKINFO";
	case FS_OP_DELETE: return "DELETE";
	case FS_OP_GETATTRIBUTES: return "GETATTRIBUTES";
	case FS_OP_OPENHINT: return "OPENHINT";
	case FS_OP_FINDFIRST: return "FINDFIRST";
	case FS_OP_FINDNEXT: return "FINDNEXT";
	case FS_OP_FINDCLOSE: return "FINDCLOSE";
	case FS_OP_LOCK_FAT: return "LOCK_FAT";
	case FS_OP_UNLOCK_ALL: return "UNLOCK_ALL";
	case FS_OP_SHUTDOWN: return "SHUTDOWN";
	case FS_OP_XDELETE: return "XDELETE";
	case FS_OP_CLEARDISKFLAG: return "CLEARDISKFLAG";
	case FS_OP_GETDRIVE: return "GETDRIVE";
	case FS_OP_GETCLUSTERSIZE: return "GETCLUSTERSIZE";
	case FS_OP_SETDISKFLAG: return "SETDISKFLAG";
	case FS_OP_OTP_WRITE: return "OTP_WRITE";
	case FS_OP_OTP_READ: return "OTP_READ";
	case FS_OP_OTP_QUERYLEN: return "OTP_QUERYLEN";
	case FS_OP_OTP_LOCK: return "OTP_LOCK";
	case FS_OP_RESTORE: return "RESTORE";
	case FS_OP_CMPT_READ: return "CMPT_READ";
	case FS_OP_BIN_REGION_ACCESS: return "BIN_REGION_ACCESS";
	case FS_OP_CMPT_WRITE: return "CMPT_WRITE";
	case FS_OP_GETFILEDETAIL: return "GETFILEDETAIL";
	default: return "UNKNOWN";
	}
}

static int32_t errno_to_fs_err(int err)
{
	switch (err) {
	case 0: return FS_NO_ERROR;
	case ENOENT: return FS_FILE_NOT_FOUND;
	case ENOTDIR: return FS_PATH_NOT_FOUND;
	case EEXIST: return FS_FILE_EXISTS;
	case EACCES:
	case EROFS:
	case EPERM: return FS_ACCESS_DENIED;
	case ENOSPC: return FS_DISK_FULL;
	case EBADF: return FS_INVALID_FILE_HANDLE;
	case EMFILE:
	case ENFILE: return FS_TOO_MANY_FILES;
	case EINVAL:
	case ENAMETOOLONG: return FS_INVALID_FILENAME;
	default: return FS_ERROR_RESERVED;
	}
}

static int utf16le_to_ascii(const uint8_t *src, uint32_t src_bytes,
			    char *dst, size_t dst_sz)
{
	size_t out = 0;
	uint32_t i;

	if (!src || !dst || dst_sz == 0)
		return -1;

	for (i = 0; i + 1 < src_bytes && out + 1 < dst_sz; i += 2) {
		uint16_t ch = (uint16_t)src[i] | ((uint16_t)src[i + 1] << 8);

		if (ch == 0)
			break;
		if (ch > 0x7f)
			return -1;
		dst[out++] = (char)ch;
	}
	dst[out] = '\0';
	return (int)out;
}

static uint32_t ascii_to_utf16le(const char *src, uint8_t *dst, size_t dst_sz)
{
	size_t i, n = strlen(src);

	if ((n + 1) * 2 > dst_sz)
		n = (dst_sz / 2) - 1;
	for (i = 0; i < n; i++) {
		dst[i * 2] = (uint8_t)src[i];
		dst[i * 2 + 1] = 0;
	}
	dst[n * 2] = 0;
	dst[n * 2 + 1] = 0;
	return (uint32_t)((n + 1) * 2);
}

/*
 * Resolve a single path component case-insensitively inside dir_path when an
 * exact match does not exist (FAT-style modem paths on case-sensitive ext4).
 */
static void resolve_component_ci(const char *dir_path, const char *comp,
				 char *out_comp, size_t out_sz)
{
	char probe[PATH_MAX];
	struct stat st;
	DIR *d;
	struct dirent *de;

	if (snprintf(probe, sizeof(probe), "%s/%s", dir_path, comp) <
	    (int)sizeof(probe) && stat(probe, &st) == 0) {
		snprintf(out_comp, out_sz, "%s", comp);
		return;
	}

	d = opendir(dir_path);
	if (d) {
		while ((de = readdir(d)) != NULL) {
			if (strcasecmp(de->d_name, comp) == 0) {
				snprintf(out_comp, out_sz, "%s", de->d_name);
				closedir(d);
				return;
			}
		}
		closedir(d);
	}
	snprintf(out_comp, out_sz, "%s", comp);
}

static int resolve_md_path(const uint8_t *u16, uint32_t u16_len,
			   char *out_unix, size_t out_sz,
			   char *out_disp, size_t disp_sz)
{
	char raw[PATH_MAX];
	char cur[PATH_MAX];
	char *p, *tok, *saveptr = NULL;
	char drive;
	int n;

	n = utf16le_to_ascii(u16, u16_len, raw, sizeof(raw));
	if (n < 2 || raw[1] != ':')
		return FS_INVALID_FILENAME;

	drive = (char)toupper((unsigned char)raw[0]);
	if (!strchr(g_valid_drives, drive))
		return FS_DRIVE_NOT_FOUND;

	if (out_disp && disp_sz)
		snprintf(out_disp, disp_sz, "%s", raw);

	for (p = raw + 2; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}

	if (snprintf(cur, sizeof(cur), "%s/%c", g_root_dir, drive) >=
	    (int)sizeof(cur))
		return FS_INVALID_FILENAME;

	p = raw + 2;
	while (*p == '/')
		p++;

	tok = strtok_r(p, "/", &saveptr);
	while (tok) {
		char resolved[NAME_MAX + 1];
		size_t cur_len;

		if (strcmp(tok, ".") == 0) {
			tok = strtok_r(NULL, "/", &saveptr);
			continue;
		}
		if (strcmp(tok, "..") == 0)
			return FS_INVALID_FILENAME;

		resolve_component_ci(cur, tok, resolved, sizeof(resolved));
		cur_len = strlen(cur);
		if (cur_len + 1 + strlen(resolved) >= sizeof(cur))
			return FS_INVALID_FILENAME;
		cur[cur_len] = '/';
		strcpy(cur + cur_len + 1, resolved);

		tok = strtok_r(NULL, "/", &saveptr);
	}

	if (snprintf(out_unix, out_sz, "%s", cur) >= (int)out_sz)
		return FS_INVALID_FILENAME;
	return FS_NO_ERROR;
}

static int32_t handle_open(const char *path, uint32_t mtk_flags)
{
	int oflags;
	int fd, i, slot = -1;
	struct stat st;

	for (i = 0; i < FS_MAX_HANDLES; i++) {
		if (!g_files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return FS_TOO_MANY_FILES;

	if (mtk_flags & MTK_FS_OPEN_DIR) {
		oflags = O_RDONLY | O_DIRECTORY;
	} else if (mtk_flags & MTK_FS_CREATE_ALWAYS) {
		oflags = O_RDWR | O_CREAT | O_TRUNC;
	} else if (mtk_flags & MTK_FS_CREATE) {
		oflags = O_RDWR | O_CREAT;
	} else if (mtk_flags & MTK_FS_READ_ONLY) {
		oflags = O_RDONLY;
	} else {
		oflags = O_RDWR;
	}

	if ((oflags & O_RDWR) && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		oflags = O_RDONLY | O_DIRECTORY;

	fd = open(path, oflags, 0660);
	if (fd < 0 && (oflags == O_RDWR) && (errno == EACCES || errno == EROFS))
		fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno_to_fs_err(errno);

	g_files[slot].fd = fd;
	g_files[slot].used = true;
	snprintf(g_files[slot].path, sizeof(g_files[slot].path), "%s", path);
	return slot + 1; /* 1-based positive handle */
}

static int get_fd(int32_t handle)
{
	if (handle < 1 || handle > FS_MAX_HANDLES || !g_files[handle - 1].used)
		return -1;
	return g_files[handle - 1].fd;
}

static int32_t handle_close(int32_t handle)
{
	if (handle < 1 || handle > FS_MAX_HANDLES || !g_files[handle - 1].used)
		return FS_INVALID_FILE_HANDLE;
	close(g_files[handle - 1].fd);
	g_files[handle - 1].fd = -1;
	g_files[handle - 1].used = false;
	g_files[handle - 1].path[0] = '\0';
	return FS_NO_ERROR;
}

static void handle_close_all(void)
{
	int i;

	for (i = 0; i < FS_MAX_HANDLES; i++) {
		if (g_files[i].used)
			handle_close(i + 1);
	}
	for (i = 0; i < FS_MAX_FIND_HANDLES; i++) {
		if (g_finds[i].used) {
			if (g_finds[i].dir)
				closedir(g_finds[i].dir);
			g_finds[i].dir = NULL;
			g_finds[i].used = false;
		}
	}
}

static int32_t handle_seek(int32_t handle, int32_t offset, int32_t whence)
{
	int fd = get_fd(handle);
	int pw;
	off_t pos;

	if (fd < 0)
		return FS_INVALID_FILE_HANDLE;
	switch (whence) {
	case 0: pw = SEEK_SET; break;
	case 1: pw = SEEK_CUR; break;
	case 2: pw = SEEK_END; break;
	default: return FS_PARAM_ERROR;
	}
	pos = lseek(fd, offset, pw);
	if (pos < 0)
		return errno_to_fs_err(errno);
	return (int32_t)pos;
}

static int32_t handle_getsize(int32_t handle, uint32_t *out_sz)
{
	int fd = get_fd(handle);
	struct stat st;

	if (fd < 0)
		return FS_INVALID_FILE_HANDLE;
	if (fstat(fd, &st) < 0)
		return errno_to_fs_err(errno);
	*out_sz = (uint32_t)st.st_size;
	return FS_NO_ERROR;
}

static int32_t handle_read(int32_t handle, uint8_t *dst, uint32_t req_len,
			   uint32_t *actual_len)
{
	int fd = get_fd(handle);
	ssize_t n;

	*actual_len = 0;
	if (fd < 0)
		return FS_INVALID_FILE_HANDLE;
	if (req_len > FS_MSG_BUF_MAX - 128U)
		req_len = FS_MSG_BUF_MAX - 128U;
	n = read(fd, dst, req_len);
	if (n < 0)
		return errno_to_fs_err(errno);
	*actual_len = (uint32_t)n;
	return FS_NO_ERROR;
}

static int32_t handle_write(int32_t handle, const uint8_t *src, uint32_t len,
			    uint32_t *written_len)
{
	int fd = get_fd(handle);
	ssize_t n;

	*written_len = 0;
	if (fd < 0)
		return FS_INVALID_FILE_HANDLE;
	n = write(fd, src, len);
	if (n < 0)
		return errno_to_fs_err(errno);
	*written_len = (uint32_t)n;
	return FS_NO_ERROR;
}

static int32_t calc_folder_size_recursive(const char *path)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	int64_t total = 0;

	if (stat(path, &st) < 0)
		return errno_to_fs_err(errno);
	if (!S_ISDIR(st.st_mode))
		return FS_PARAM_ERROR;

	d = opendir(path);
	if (!d)
		return errno_to_fs_err(errno);

	while ((de = readdir(d)) != NULL) {
		char child[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
		    (int)sizeof(child))
			continue;
		if (lstat(child, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int32_t sub = calc_folder_size_recursive(child);

			if (sub > 0)
				total += sub;
		} else {
			total += st.st_size;
		}
	}
	closedir(d);
	return (total > INT32_MAX) ? INT32_MAX : (int32_t)total;
}

static int32_t count_entries_recursive(const char *path, uint32_t flags)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	int32_t cnt = 0;

	if (stat(path, &st) < 0)
		return errno_to_fs_err(errno);
	if (!S_ISDIR(st.st_mode))
		return FS_PARAM_ERROR;

	d = opendir(path);
	if (!d)
		return errno_to_fs_err(errno);

	while ((de = readdir(d)) != NULL) {
		char child[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
		    (int)sizeof(child))
			continue;
		if (lstat(child, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (flags & 0x04U) /* FS_DIR_TYPE */
				cnt++;
			if (flags & 0x10U) { /* FS_RECURSIVE_TYPE */
				int32_t sub = count_entries_recursive(child, flags);

				if (sub > 0)
					cnt += sub;
			}
		} else {
			if ((flags & 0x02U) || flags == 0) /* FS_FILE_TYPE */
				cnt++;
		}
	}
	closedir(d);
	return cnt;
}

static int32_t xdelete_recursive(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *de;

	if (lstat(path, &st) < 0)
		return errno_to_fs_err(errno);
	if (!S_ISDIR(st.st_mode))
		return unlink(path) == 0 ? FS_NO_ERROR : errno_to_fs_err(errno);

	d = opendir(path);
	if (!d)
		return errno_to_fs_err(errno);
	while ((de = readdir(d)) != NULL) {
		char child[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
		    (int)sizeof(child))
			continue;
		xdelete_recursive(child);
	}
	closedir(d);
	return rmdir(path) == 0 ? FS_NO_ERROR : errno_to_fs_err(errno);
}

static void fill_dos_dir_entry(const char *name, const struct stat *st,
			       struct fs_dos_dir_entry *ent)
{
	struct tm tm;
	const char *dot;
	size_t i, base_len;

	memset(ent, 0, sizeof(*ent));
	memset(ent->filename, ' ', sizeof(ent->filename));
	memset(ent->extension, ' ', sizeof(ent->extension));

	dot = strrchr(name, '.');
	base_len = dot ? (size_t)(dot - name) : strlen(name);
	for (i = 0; i < base_len && i < sizeof(ent->filename); i++)
		ent->filename[i] = (char)toupper((unsigned char)name[i]);
	if (dot) {
		dot++;
		for (i = 0; dot[i] && i < sizeof(ent->extension); i++)
			ent->extension[i] = (char)toupper((unsigned char)dot[i]);
	}

	ent->attributes = S_ISDIR(st->st_mode) ? FS_ATTR_DIR : FS_ATTR_ARCHIVE;
	if (!(st->st_mode & S_IWUSR))
		ent->attributes |= FS_ATTR_READ_ONLY;
	ent->file_size = S_ISDIR(st->st_mode) ? 0U : (uint32_t)st->st_size;

	if (localtime_r(&st->st_mtime, &tm)) {
		uint16_t yr = (tm.tm_year >= 80) ? (uint16_t)(tm.tm_year - 80) : 0;
		uint16_t dt = (uint16_t)((yr << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
		uint16_t t = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));

		ent->create_date = dt;
		ent->mod_date = dt;
		ent->access_date = dt;
		ent->create_time = t;
		ent->mod_time = t;
	}
}

static int find_next_match(struct fs_find_slot *fs,
			   struct fs_dos_dir_entry *ent,
			   char *out_name, size_t out_name_sz)
{
	struct dirent *de;

	while ((de = readdir(fs->dir)) != NULL) {
		char full[PATH_MAX];
		struct stat st;
		uint8_t fattr;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (fnmatch(fs->pattern, de->d_name, FNM_CASEFOLD) != 0)
			continue;
		if (snprintf(full, sizeof(full), "%s/%s", fs->dir_path, de->d_name) >=
		    (int)sizeof(full))
			continue;
		if (stat(full, &st) < 0)
			continue;

		fattr = S_ISDIR(st.st_mode) ? FS_ATTR_DIR : FS_ATTR_ARCHIVE;
		if (fs->attr && !(fattr & fs->attr))
			continue;
		if (fs->attr_mask && (fattr & fs->attr_mask))
			continue;

		fill_dos_dir_entry(de->d_name, &st, ent);
		snprintf(out_name, out_name_sz, "%s", de->d_name);
		return FS_NO_ERROR;
	}

	return FS_NO_MORE_FILES;
}

static int send_fs_reply(int dev_fd, uint32_t slot, uint32_t op_id,
			 const struct fs_arg *resp, uint32_t resp_num)
{
	uint8_t msg[FS_MSG_BUF_MAX];
	uint8_t wire[FS_WIRE_PKT_MAX];
	struct fs_msg_hdr *hdr = (struct fs_msg_hdr *)msg;
	uint32_t pos = sizeof(*hdr);
	uint32_t i, payload_total, off = 0;

	if (resp_num > FS_MAX_ARGS)
		return -EINVAL;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ccci.channel = CCCI_CH_FS_TX;
	hdr->ccci.reserved = slot;
	hdr->op_id = op_id | FS_API_RESP_MASK;
	hdr->para_num = resp_num;

	for (i = 0; i < resp_num; i++) {
		uint32_t aligned = align4(resp[i].len);

		if (pos + sizeof(uint32_t) + aligned > sizeof(msg))
			return -ENOSPC;
		memcpy(msg + pos, &resp[i].len, sizeof(uint32_t));
		pos += sizeof(uint32_t);
		if (resp[i].len && resp[i].buf)
			memcpy(msg + pos, resp[i].buf, resp[i].len);
		if (aligned > resp[i].len)
			memset(msg + pos + resp[i].len, 0, aligned - resp[i].len);
		pos += aligned;
	}

	payload_total = pos - FS_WIRE_HDR_LEN;
	do {
		uint32_t chunk = payload_total - off;
		uint32_t wire_len;
		struct ccci_header *wh = (struct ccci_header *)wire;
		uint32_t *wop = (uint32_t *)(wire + sizeof(*wh));
		bool more = false;
		ssize_t wr;

		if (chunk > FS_WIRE_PAYLOAD_MAX) {
			chunk = FS_WIRE_PAYLOAD_MAX;
			more = true;
		}

		wire_len = FS_WIRE_HDR_LEN + chunk;
		memset(wh, 0, sizeof(*wh));
		wh->data[0] = more ? FS_FRAG_CONT_BIT : 0U;
		wh->data[1] = wire_len;
		wh->channel = CCCI_CH_FS_TX;
		wh->reserved = slot;
		*wop = hdr->op_id;
		memcpy(wire + FS_WIRE_HDR_LEN, msg + FS_WIRE_HDR_LEN + off, chunk);

		wr = write(dev_fd, wire, wire_len);
		if (wr != (ssize_t)wire_len) {
			fprintf(stderr,
				"[ccci_fsd] TX write failed op=0x%04x slot=%u chunk=%u wr=%zd errno=%d\n",
				op_id, slot, wire_len, wr, errno);
			return -1;
		}
		off += chunk;
	} while (off < payload_total);

	return 0;
}

static void dispatch_fs_message(int dev_fd, uint32_t slot,
				const uint8_t *msg, uint32_t msg_len)
{
	struct fs_msg_hdr hdr;
	struct fs_arg args[FS_MAX_ARGS];
	struct fs_arg resp[FS_MAX_ARGS];
	uint32_t resp_num = 0;
	uint32_t pos, i;
	int32_t ret_val = FS_NO_ERROR;
	uint32_t u32_out = 0;
	int32_t i32_out = 0;
	char upath[PATH_MAX] = "";
	char upath2[PATH_MAX] = "";
	char dpath[PATH_MAX] = "";

	if (msg_len < sizeof(hdr)) {
		fprintf(stderr, "[ccci_fsd] short msg len=%u slot=%u\n", msg_len, slot);
		return;
	}
	memcpy(&hdr, msg, sizeof(hdr));
	if (hdr.para_num > FS_MAX_ARGS) {
		fprintf(stderr, "[ccci_fsd] bad para_num=%u op=0x%08x\n",
			hdr.para_num, hdr.op_id);
		return;
	}

	pos = sizeof(hdr);
	for (i = 0; i < hdr.para_num; i++) {
		uint32_t alen, aligned;

		if (pos + sizeof(uint32_t) > msg_len) {
			fprintf(stderr, "[ccci_fsd] truncated arg %u op=0x%08x\n",
				i, hdr.op_id);
			return;
		}
		memcpy(&alen, msg + pos, sizeof(uint32_t));
		pos += sizeof(uint32_t);
		aligned = align4(alen);
		if (alen > msg_len || aligned < alen || pos + aligned > msg_len) {
			fprintf(stderr, "[ccci_fsd] bad arg %u len=%u op=0x%08x\n",
				i, alen, hdr.op_id);
			return;
		}
		args[i].len = alen;
		args[i].buf = msg + pos;
		pos += aligned;
	}

	switch (hdr.op_id) {
	case FS_OP_OPEN: {
		uint32_t flags = 0;

		if (hdr.para_num < 2 || args[1].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&flags, args[1].buf, 4);
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = handle_open(upath, flags);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_OPENHINT: {
		uint32_t flags = 0;
		uint8_t hint[8] = {0};

		if (hdr.para_num < 3 || args[1].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&flags, args[1].buf, 4);
			if (args[2].len >= 8)
				memcpy(hint, args[2].buf, 8);
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = handle_open(upath, flags);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = 8;
		resp[1].buf = hint;
		resp_num = 2;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> %d\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath, ret_val);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_SEEK: {
		int32_t fh = 0, off = 0, wh = 0;

		if (hdr.para_num < 3 || args[0].len < 4 || args[1].len < 4 ||
		    args[2].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			memcpy(&off, args[1].buf, 4);
			memcpy(&wh, args[2].buf, 4);
			ret_val = handle_seek(fh, off, wh);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_READ: {
		static uint8_t rbuf[FS_MSG_BUF_MAX];
		int32_t fh = 0;
		uint32_t rlen = 0, actual = 0;

		if (hdr.para_num < 2 || args[0].len < 4 || args[1].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			memcpy(&rlen, args[1].buf, 4);
			ret_val = handle_read(fh, rbuf, rlen, &actual);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = 4;
		resp[1].buf = (const uint8_t *)&actual;
		resp[2].len = actual;
		resp[2].buf = rbuf;
		resp_num = 3;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u fh=%d req=%u -> ret=%d got=%u\n",
				hdr.op_id, op_name(hdr.op_id), slot, fh, rlen, ret_val, actual);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_WRITE: {
		int32_t fh = 0;
		uint32_t wlen = 0, written = 0;

		if (hdr.para_num < 3 || args[0].len < 4 || args[2].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			memcpy(&wlen, args[2].buf, 4);
			if (wlen > args[1].len)
				wlen = args[1].len;
			ret_val = handle_write(fh, args[1].buf, wlen, &written);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = 4;
		resp[1].buf = (const uint8_t *)&written;
		resp_num = 2;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u fh=%d len=%u -> ret=%d wr=%u\n",
				hdr.op_id, op_name(hdr.op_id), slot, fh, wlen, ret_val, written);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_CLOSE: {
		int32_t fh = 0;

		if (hdr.para_num < 1 || args[0].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			ret_val = handle_close(fh);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_CLOSEALL:
	case FS_OP_SHUTDOWN:
		handle_close_all();
		ret_val = FS_NO_ERROR;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_CREATEDIR:
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR) {
				if (mkdir(upath, 0770) < 0 && errno != EEXIST)
					ret_val = errno_to_fs_err(errno);
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_REMOVEDIR:
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR && rmdir(upath) < 0)
				ret_val = errno_to_fs_err(errno);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_GETFILESIZE: {
		int32_t fh = 0;

		if (hdr.para_num < 1 || args[0].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			ret_val = handle_getsize(fh, &u32_out);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = 4;
		resp[1].buf = (const uint8_t *)&u32_out;
		resp_num = 2;
		break;
	}

	case FS_OP_GETFOLDERSIZE:
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = calc_folder_size_recursive(upath);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_RENAME:
	case FS_OP_MOVE:
		if (hdr.para_num < 2) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = resolve_md_path(args[1].buf, args[1].len,
							  upath2, sizeof(upath2),
							  NULL, 0);
			if (ret_val == FS_NO_ERROR && rename(upath, upath2) < 0)
				ret_val = errno_to_fs_err(errno);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_COUNT: {
		uint32_t cflags = 0;

		if (hdr.para_num < 2 || args[1].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&cflags, args[1].buf, 4);
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = count_entries_recursive(upath, cflags);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_GETDISKINFO: {
		struct fs_disk_info di;
		struct statfs sfs;

		memset(&di, 0, sizeof(di));
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR) {
				if (statfs(upath, &sfs) < 0) {
					ret_val = errno_to_fs_err(errno);
				} else {
					/* Matches Advan X1 ccci_fsd 0x159c0..0x159dc */
					di.bytes_per_sector = 512U;
					di.sectors_per_cluster = (uint32_t)(sfs.f_bsize >> 9);
					if (di.sectors_per_cluster == 0)
						di.sectors_per_cluster = 8U;
					di.total_clusters = (uint32_t)sfs.f_blocks;
					di.free_clusters = sfs.f_bfree ?
						(uint32_t)sfs.f_bfree : 512U;
				}
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(di);
		resp[1].buf = (const uint8_t *)&di;
		resp_num = 2;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> ret=%d tot=%u free=%u\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath, ret_val,
				di.total_clusters, di.free_clusters);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_DELETE:
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR && unlink(upath) < 0)
				ret_val = errno_to_fs_err(errno);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_GETATTRIBUTES: {
		struct stat st;

		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR) {
				if (stat(upath, &st) < 0)
					ret_val = errno_to_fs_err(errno);
				else
					ret_val = S_ISDIR(st.st_mode) ?
						  (int32_t)FS_ATTR_DIR :
						  (int32_t)FS_ATTR_ARCHIVE;
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_FINDFIRST: {
		struct fs_dos_dir_entry ent;
		uint8_t name_u16[512];
		uint32_t name_u16_len = 0;
		char matched[NAME_MAX + 1] = "";

		memset(&ent, 0, sizeof(ent));
		if (hdr.para_num < 4 || args[1].len < 1 || args[2].len < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR) {
				char *slash = strrchr(upath, '/');
				int fslot = -1;

				for (i = 0; i < FS_MAX_FIND_HANDLES; i++) {
					if (!g_finds[i].used) {
						fslot = (int)i;
						break;
					}
				}
				if (!slash || fslot < 0) {
					ret_val = fslot < 0 ? FS_TOO_MANY_FILES :
							      FS_INVALID_FILENAME;
				} else {
					*slash = '\0';
					DIR *d = opendir(upath);

					if (!d) {
						ret_val = errno_to_fs_err(errno);
					} else {
						struct fs_find_slot *fs = &g_finds[fslot];

						fs->used = true;
						fs->dir = d;
						snprintf(fs->dir_path, sizeof(fs->dir_path),
							 "%s", upath);
						snprintf(fs->pattern, sizeof(fs->pattern),
							 "%s", slash + 1);
						fs->attr = args[1].buf[0];
						fs->attr_mask = args[2].buf[0];

						ret_val = find_next_match(fs, &ent, matched,
									  sizeof(matched));
						if (ret_val == FS_NO_ERROR) {
							name_u16_len = ascii_to_utf16le(
								matched, name_u16,
								sizeof(name_u16));
							ret_val = fslot + 1;
						} else {
							closedir(fs->dir);
							fs->dir = NULL;
							fs->used = false;
						}
					}
				}
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(ent);
		resp[1].buf = (const uint8_t *)&ent;
		resp[2].len = name_u16_len;
		resp[2].buf = name_u16;
		resp_num = 3;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> ret=%d match=%s\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath, ret_val, matched);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_FINDNEXT: {
		struct fs_dos_dir_entry ent;
		uint8_t name_u16[512];
		uint32_t name_u16_len = 0;
		char matched[NAME_MAX + 1] = "";
		int32_t fh = 0;

		memset(&ent, 0, sizeof(ent));
		if (hdr.para_num < 1 || args[0].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			if (fh < 1 || fh > FS_MAX_FIND_HANDLES || !g_finds[fh - 1].used) {
				ret_val = FS_INVALID_FILE_HANDLE;
			} else {
				ret_val = find_next_match(&g_finds[fh - 1], &ent,
							  matched, sizeof(matched));
				if (ret_val == FS_NO_ERROR)
					name_u16_len = ascii_to_utf16le(matched, name_u16,
									sizeof(name_u16));
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(ent);
		resp[1].buf = (const uint8_t *)&ent;
		resp[2].len = name_u16_len;
		resp[2].buf = name_u16;
		resp_num = 3;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u fh=%d -> ret=%d match=%s\n",
				hdr.op_id, op_name(hdr.op_id), slot, fh, ret_val, matched);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_FINDCLOSE: {
		int32_t fh = 0;

		if (hdr.para_num < 1 || args[0].len < 4) {
			ret_val = FS_PARAM_ERROR;
		} else {
			memcpy(&fh, args[0].buf, 4);
			if (fh < 1 || fh > FS_MAX_FIND_HANDLES || !g_finds[fh - 1].used) {
				ret_val = FS_INVALID_FILE_HANDLE;
			} else {
				closedir(g_finds[fh - 1].dir);
				g_finds[fh - 1].dir = NULL;
				g_finds[fh - 1].used = false;
				ret_val = FS_NO_ERROR;
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_LOCK_FAT:
	case FS_OP_CLEARDISKFLAG:
	case FS_OP_SETDISKFLAG:
		ret_val = 0;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_UNLOCK_ALL:
		ret_val = 1;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_XDELETE:
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR)
				ret_val = xdelete_recursive(upath);
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_GETDRIVE: {
		uint32_t dtype = 0;

		if (hdr.para_num >= 1 && args[0].len >= 4)
			memcpy(&dtype, args[0].buf, 4);
		ret_val = (dtype == FS_DRIVE_I_SYSTEM) ? 'Z' : FS_DRIVE_NOT_FOUND;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_GETCLUSTERSIZE: {
		struct statfs sfs;

		if (snprintf(upath, sizeof(upath), "%s/Z", g_root_dir) < (int)sizeof(upath) &&
		    statfs(upath, &sfs) == 0)
			ret_val = (int32_t)sfs.f_bsize;
		else
			ret_val = 4096;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	case FS_OP_CMPT_READ: {
		static uint8_t rbuf[FS_MSG_BUF_MAX];
		struct fs_cmpt_read_in in;
		uint32_t out_pair[2] = {0, 0}; /* [0]=opid_ret, [1]=ret_val */
		uint32_t fsize = 0, actual = 0;
		int32_t fh = -1;

		if (hdr.para_num < 2 || args[1].len < sizeof(in)) {
			out_pair[1] = (uint32_t)FS_PARAM_ERROR;
		} else {
			memcpy(&in, args[1].buf, sizeof(in));
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val != FS_NO_ERROR) {
				out_pair[1] = (uint32_t)ret_val;
			} else if (!(in.opid_map & FS_CMPT_OPEN)) {
				out_pair[1] = (uint32_t)FS_PARAM_ERROR;
			} else {
				fh = handle_open(upath, in.flag);
				if (fh < 0) {
					out_pair[1] = (uint32_t)fh;
				} else {
					uint32_t done = FS_CMPT_OPEN;
					int32_t op_ret = FS_NO_ERROR;

					if (in.opid_map & FS_CMPT_GETSIZE) {
						op_ret = handle_getsize(fh, &fsize);
						if (op_ret == FS_NO_ERROR)
							done |= FS_CMPT_GETSIZE;
					}
					if (op_ret == FS_NO_ERROR &&
					    (in.opid_map & FS_CMPT_SEEK)) {
						op_ret = handle_seek(fh, in.offset, in.whence);
						if (op_ret >= 0) {
							op_ret = FS_NO_ERROR;
							done |= FS_CMPT_SEEK;
						}
					}
					if (op_ret == FS_NO_ERROR &&
					    (in.opid_map & FS_CMPT_READ)) {
						op_ret = handle_read(fh, rbuf, in.read_len,
								     &actual);
						if (op_ret == FS_NO_ERROR)
							done |= FS_CMPT_READ;
					}
					if (in.opid_map & FS_CMPT_CLOSE) {
						int32_t cr = handle_close(fh);

						if (cr == FS_NO_ERROR)
							done |= FS_CMPT_CLOSE;
						else if (op_ret == FS_NO_ERROR)
							op_ret = cr;
					}
					out_pair[0] = done;
					out_pair[1] = (uint32_t)op_ret;
				}
			}
		}
		resp[0].len = sizeof(out_pair);
		resp[0].buf = (const uint8_t *)out_pair;
		resp[1].len = sizeof(fsize);
		resp[1].buf = (const uint8_t *)&fsize;
		resp[2].len = sizeof(actual);
		resp[2].buf = (const uint8_t *)&actual;
		resp[3].len = actual;
		resp[3].buf = rbuf;
		resp_num = 4;
		if (g_verbose) {
			fprintf(stdout,
				"[ccci_fsd] op=0x%04x (%s) slot=%u path=%s map=0x%x -> done=0x%x ret=%d sz=%u read=%u\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath,
				hdr.para_num >= 2 ? ((const struct fs_cmpt_read_in *)args[1].buf)->opid_map : 0,
				out_pair[0], (int32_t)out_pair[1], fsize, actual);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_CMPT_WRITE: {
		struct fs_cmpt_write_in in;
		uint32_t out_pair[2] = {0, 0}; /* [0]=opid_ret, [1]=ret_val */
		int32_t fh = -1;
		uint32_t written = 0;

		if (hdr.para_num < 3 || args[1].len < sizeof(in)) {
			out_pair[1] = (uint32_t)FS_PARAM_ERROR;
		} else {
			uint32_t done = 0;
			int32_t op_ret = FS_NO_ERROR;

			memcpy(&in, args[1].buf, sizeof(in));
			if (in.handle > 0) {
				fh = in.handle;
			} else if (in.opid_map & FS_CMPT_OPEN) {
				op_ret = resolve_md_path(args[0].buf, args[0].len,
							 upath, sizeof(upath),
							 dpath, sizeof(dpath));
				if (op_ret == FS_NO_ERROR) {
					fh = handle_open(upath, in.flag);
					if (fh < 0)
						op_ret = fh;
					else
						done |= FS_CMPT_OPEN;
				}
			} else {
				op_ret = FS_INVALID_FILE_HANDLE;
			}

			if (op_ret == FS_NO_ERROR && (in.opid_map & FS_CMPT_SEEK)) {
				op_ret = handle_seek(fh, in.offset, in.whence);
				if (op_ret >= 0) {
					op_ret = FS_NO_ERROR;
					done |= FS_CMPT_SEEK;
				}
			}
			if (op_ret == FS_NO_ERROR && (in.opid_map & FS_CMPT_WRITE)) {
				uint32_t wlen = in.write_len;

				if (wlen > args[2].len)
					wlen = args[2].len;
				op_ret = handle_write(fh, args[2].buf, wlen, &written);
				if (op_ret == FS_NO_ERROR)
					done |= FS_CMPT_WRITE;
			}
			if (fh > 0 && (in.opid_map & FS_CMPT_CLOSE)) {
				int32_t cr = handle_close(fh);

				if (cr == FS_NO_ERROR)
					done |= FS_CMPT_CLOSE;
				else if (op_ret == FS_NO_ERROR)
					op_ret = cr;
			}
			out_pair[0] = done;
			out_pair[1] = (uint32_t)op_ret;
		}
		i32_out = fh;
		resp[0].len = sizeof(out_pair);
		resp[0].buf = (const uint8_t *)out_pair;
		resp[1].len = sizeof(i32_out);
		resp[1].buf = (const uint8_t *)&i32_out;
		resp[2].len = sizeof(written);
		resp[2].buf = (const uint8_t *)&written;
		resp_num = 3;
		if (g_verbose) {
			fprintf(stdout,
				"[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> done=0x%x ret=%d fh=%d wr=%u\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath,
				out_pair[0], (int32_t)out_pair[1], i32_out, written);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	case FS_OP_GETFILEDETAIL: {
		struct fs_file_detail fd_out;
		struct stat st;

		memset(&fd_out, 0, sizeof(fd_out));
		if (hdr.para_num < 1) {
			ret_val = FS_PARAM_ERROR;
		} else {
			ret_val = resolve_md_path(args[0].buf, args[0].len,
						  upath, sizeof(upath),
						  dpath, sizeof(dpath));
			if (ret_val == FS_NO_ERROR) {
				if (stat(upath, &st) < 0) {
					ret_val = errno_to_fs_err(errno);
				} else {
					fd_out.mtime = (uint64_t)st.st_mtime;
					fd_out.atime = (uint64_t)st.st_atime;
					fd_out.ctime = (uint64_t)st.st_ctime;
				}
			}
		}
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(fd_out);
		resp[1].buf = (const uint8_t *)&fd_out;
		resp_num = 2;
		if (g_verbose) {
			fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> ret=%d\n",
				hdr.op_id, op_name(hdr.op_id), slot, dpath, ret_val);
			fflush(stdout);
		}
		send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
		return;
	}

	/* Strictly refused hardware-sensitive ops (logged loudly) */
	case FS_OP_OTP_WRITE:
	case FS_OP_OTP_READ:
	case FS_OP_OTP_QUERYLEN:
	case FS_OP_OTP_LOCK:
		fprintf(stderr, "[ccci_fsd] REFUSED OTP op=0x%04x (%s) slot=%u\n",
			hdr.op_id, op_name(hdr.op_id), slot);
		ret_val = FS_ERROR_RESERVED;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_RESTORE:
		if (hdr.para_num >= 1)
			utf16le_to_ascii(args[0].buf, args[0].len, dpath, sizeof(dpath));
		fprintf(stderr,
			"[ccci_fsd] REFUSED RESTORE op=0x%04x slot=%u path=%s (raw nvram immutable)\n",
			hdr.op_id, slot, dpath);
		ret_val = FS_ERROR_RESERVED;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	case FS_OP_BIN_REGION_ACCESS:
		fprintf(stderr,
			"[ccci_fsd] REFUSED BIN_REGION_ACCESS op=0x%04x slot=%u -> -2\n",
			hdr.op_id, slot);
		ret_val = FS_PARAM_ERROR;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;

	default:
		fprintf(stderr, "[ccci_fsd] UNHANDLED op=0x%08x para=%u slot=%u\n",
			hdr.op_id, hdr.para_num, slot);
		ret_val = FS_ERROR_RESERVED;
		resp[0].len = 4;
		resp[0].buf = (const uint8_t *)&ret_val;
		resp_num = 1;
		break;
	}

	if (g_verbose) {
		fprintf(stdout, "[ccci_fsd] op=0x%04x (%s) slot=%u path=%s -> %d\n",
			hdr.op_id, op_name(hdr.op_id), slot, dpath[0] ? dpath : "-", ret_val);
		fflush(stdout);
	}
	send_fs_reply(dev_fd, slot, hdr.op_id, resp, resp_num);
}

static int trigger_modem_boot(const char *sysfs_path)
{
	int fd = open(sysfs_path, O_WRONLY);
	ssize_t n;

	if (fd < 0) {
		fprintf(stderr, "[ccci_fsd] failed to open boot trigger %s: %s\n",
			sysfs_path, strerror(errno));
		return -1;
	}
	n = write(fd, "1\n", 2);
	close(fd);
	if (n != 2) {
		fprintf(stderr, "[ccci_fsd] failed to write boot trigger %s: %s\n",
			sysfs_path, strerror(errno));
		return -1;
	}
	fprintf(stdout, "[ccci_fsd] triggered modem boot via %s\n", sysfs_path);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev_path = "/dev/ccci_fs";
	const char *boot_trigger = NULL;
	uint8_t pkt[FS_WIRE_PKT_MAX];
	int dev_fd, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			dev_path = argv[++i];
		} else if (!strcmp(argv[i], "--root") && i + 1 < argc) {
			snprintf(g_root_dir, sizeof(g_root_dir), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--boot-trigger") && i + 1 < argc) {
			boot_trigger = argv[++i];
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			g_verbose = true;
		} else {
			fprintf(stderr,
				"Usage: %s [--dev /dev/ccci_fs] [--root /run/ccci_fs] [--boot-trigger sysfs] [-v|--verbose]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	dev_fd = open(dev_path, O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "[ccci_fsd] open(%s) failed: %s\n",
			dev_path, strerror(errno));
		return 1;
	}

	fprintf(stdout, "[ccci_fsd] listening on %s (root=%s)\n", dev_path, g_root_dir);
	fflush(stdout);

	if (boot_trigger && trigger_modem_boot(boot_trigger) < 0) {
		close(dev_fd);
		return 1;
	}

	while (!g_stop) {
		struct pollfd pfd = { .fd = dev_fd, .events = POLLIN };
		struct ccci_header ch;
		struct fs_slot *sl;
		uint32_t slot;
		bool more;
		ssize_t n;
		int pr;

		pr = poll(&pfd, 1, 500);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[ccci_fsd] poll error: %s\n", strerror(errno));
			break;
		}
		if (pr == 0 || !(pfd.revents & POLLIN))
			continue;

		n = read(dev_fd, pkt, sizeof(pkt));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "[ccci_fsd] read error: %s\n", strerror(errno));
			break;
		}
		if ((size_t)n < FS_WIRE_HDR_LEN) {
			fprintf(stderr, "[ccci_fsd] short wire pkt len=%zd\n", n);
			continue;
		}

		memcpy(&ch, pkt, sizeof(ch));
		if (ch.channel != CCCI_CH_FS_RX || ch.reserved >= FS_MAX_SLOTS) {
			fprintf(stderr, "[ccci_fsd] drop bad hdr ch=%u slot=%u len=%zd\n",
				ch.channel, ch.reserved, n);
			continue;
		}

		slot = ch.reserved;
		sl = &g_slots[slot];
		more = (ch.data[0] & FS_FRAG_CONT_BIT) != 0;

		if (!sl->in_progress) {
			if ((uint32_t)n > sizeof(sl->buf)) {
				fprintf(stderr, "[ccci_fsd] slot %u overflow (%zd)\n", slot, n);
				continue;
			}
			memcpy(sl->buf, pkt, (size_t)n);
			sl->len = (uint32_t)n;
			if (more) {
				sl->in_progress = true;
				continue;
			}
		} else {
			uint32_t frag_len = (uint32_t)n - FS_WIRE_HDR_LEN;

			if (sl->len + frag_len > sizeof(sl->buf)) {
				fprintf(stderr, "[ccci_fsd] slot %u reassembly overflow (%u + %u)\n",
					slot, sl->len, frag_len);
				sl->in_progress = false;
				sl->len = 0;
				continue;
			}
			memcpy(sl->buf + sl->len, pkt + FS_WIRE_HDR_LEN, frag_len);
			sl->len += frag_len;
			if (more)
				continue;
			sl->in_progress = false;
		}

		dispatch_fs_message(dev_fd, slot, sl->buf, sl->len);
		sl->len = 0;
	}

	handle_close_all();
	close(dev_fd);
	return 0;
}
