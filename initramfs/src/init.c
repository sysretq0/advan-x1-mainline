// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADVAN X1 (MediaTek MT6789 / Helio G99) Mainline Linux Initramfs PID 1.
 *
 * Minimal dracut-shaped static C /init — does ONLY early-boot root bring-up
 * and hands everything else (fstab, configfs, debugfs, USB gadget, runtime PM,
 * journald/pstore/logind, diagnostics) to systemd on the real rootfs:
 *
 *   1. Base virtual mounts (/proc, /sys, /dev, /dev/pts, /run)
 *   2. /proc/cmdline parsing (root= [PARTLABEL/PARTUUID/UUID/LABEL/block:/
 *      /dev/disk/by-*], rootfstype=, rootflags= [VFS + FS split], rootdelay=,
 *      rootwait, ro/rw, init=, noswitch, rd.break=, rd.nomodules,
 *      rd.blacklist=/module_blacklist=, rd.shell=0, rd.emergency=, -- args)
 *   3. Stage initramfs /lib/firmware -> /run/firmware and set
 *      firmware_class.path (when unset) so firmware survives switch_root
 *      with zero /sysroot mutation
 *   4. Native kernel module loading via SYS_finit_module (/etc/modules with
 *      modules.dep resolution + out-of-tree .ko in /lib/modules/extra/,
 *      honoring module_blacklist=/rd.blacklist=)
 *   5. Root resolution + multi-FS superblock probe (ext2/3/4, F2FS, Btrfs,
 *      XFS, EROFS, SquashFS):
 *        - Explicit root= (PARTLABEL=, PARTUUID=, UUID=, LABEL=, /dev/<node>,
 *          /dev/disk/by-{uuid,partuuid,label,partlabel}/...)
 *        - Option A nested GPT on xnu_data via native kernel loop parser
 *          (BLKSSZGET + LOOP_CONFIGURE with LO_FLAGS_PARTSCAN -> /dev/loopNpM,
 *           zero hardcoded byte offsets, interleaved with root= polling)
 *   6. Mount rootfs on /sysroot with auto-detected fstype and split VFS/FS
 *      mount flags (no mutation of /sysroot)
 *   7. Native C switch_root -> /sbin/init (systemd), forwarding `--` args
 *   8. Emergency rescue shell on failure / rd.break / noswitch (lazily brings
 *      up USB ACM on /dev/ttyGS0 + /dev/tty0 only because systemd wasn't reached)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MS_LAZYTIME
#define MS_LAZYTIME (1UL << 25)
#endif

#define NEWROOT              "/sysroot"

/* On-disk superblock constants verified against Linux kernel headers */
#define EXT4_SUPER_OFFSET    1024
#define EXT4_SUPER_SIZE      1024
#define EXT4_MAGIC_OFF       0x38
#define EXT4_UUID_OFF        0x68
#define EXT4_LABEL_OFF       0x78
#define EXT4_LABEL_LEN       16
#define EXT4_SUPER_MAGIC     0xEF53U

#define F2FS_SUPER_OFFSET    1024
#define F2FS_SUPER_MAGIC     0xF2F52010U
#define F2FS_UUID_OFF        0x6C
#define F2FS_LABEL_OFF       0x7C
#define F2FS_LABEL_MAX_UTF16 256

#define EROFS_SUPER_OFFSET   1024
#define EROFS_SUPER_MAGIC    0xE0F5E1E2U
#define EROFS_UUID_OFF       0x30
#define EROFS_LABEL_OFF      0x40
#define EROFS_LABEL_LEN      16

#define BTRFS_SUPER_OFFSET   0x10000
#define BTRFS_SUPER_READ_SZ  560
#define BTRFS_UUID_OFF       0x20
#define BTRFS_MAGIC_OFF      0x40
#define BTRFS_LABEL_OFF      0x12B
#define BTRFS_LABEL_LEN      256

#define XFS_UUID_OFF         0x20
#define XFS_LABEL_OFF        0x6C
#define XFS_LABEL_LEN        12

#define SQUASHFS_MAGIC_LE    0x73717368U

#define MAX_BLACKLIST        16
#define MAX_INIT_ARGS        16

struct fs_info {
	char fstype[16];
	char uuid[37];
	char label[128];
};

struct boot_cfg {
	char root[256];
	char rootfstype[64];
	char rootflags[256];
	char real_init[256];
	char rd_break[64];
	char rd_emergency[32];
	char blacklist[MAX_BLACKLIST][64];
	int  n_blacklist;
	char init_args[MAX_INIT_ARGS][128];
	int  n_init_args;
	int  rootdelay;
	bool rootwait;
	bool root_ro;
	bool noswitch;
	bool rd_nomodules;
	bool rd_shell;
};

struct part_info {
	char devname[128];
	char partname[128];
	char partuuid[64];
	unsigned int major;
	unsigned int minor;
	unsigned int partn;
	bool has_dev;
};

static int g_kmsg_fd = -1;
static int g_tty0_fd = -1;

/* ---------------------------------------------------------------- Logging */

static void open_log_sinks(void)
{
	if (g_kmsg_fd < 0)
		g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NOCTTY);
	if (g_tty0_fd < 0)
		g_tty0_fd = open("/dev/tty0", O_WRONLY | O_CLOEXEC | O_NOCTTY);
}

static void init_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void init_log(const char *fmt, ...)
{
	char msg[768];
	char line[800];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	open_log_sinks();

	if (g_kmsg_fd >= 0) {
		int klen = snprintf(line, sizeof(line), "<6>[INIT] %s\n", msg);
		if (klen > 0) {
			ssize_t unused = write(g_kmsg_fd, line, (size_t)klen);
			(void)unused;
		}
	}
	if (g_tty0_fd >= 0) {
		int tlen = snprintf(line, sizeof(line), "[INIT] %s\n", msg);
		if (tlen > 0) {
			ssize_t unused = write(g_tty0_fd, line, (size_t)tlen);
			(void)unused;
		}
	}
#ifdef INIT_UNIT_TEST
	fprintf(stderr, "[INIT] %s\n", msg);
#endif
}

/* --------------------------------------------------------------- FS Utils */

static int mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len;

	if (!path || !path[0])
		return -1;
	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);

	while (len > 1 && tmp[len - 1] == '/')
		tmp[--len] = '\0';

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, mode) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t len;
	ssize_t w;

	if (fd < 0)
		return -1;
	len = strlen(val);
	w = write(fd, val, len);
	close(fd);
	return (w == (ssize_t)len) ? 0 : -1;
}

static ssize_t read_file_str(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t total = 0;

	if (fd < 0 || sz == 0)
		return -1;
	while (total + 1 < sz) {
		ssize_t r = read(fd, buf + total, sz - 1 - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		total += (size_t)r;
	}
	close(fd);
	buf[total] = '\0';
	return (ssize_t)total;
}

static void strip_newline(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
	size_t n;

	if (!dst || dst_sz == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	n = strlen(src);
	if (n >= dst_sz)
		n = dst_sz - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static bool join_path2(char *dst, size_t dst_sz, const char *a, const char *b)
{
	int n = snprintf(dst, dst_sz, "%s/%s", a, b);
	return n >= 0 && (size_t)n < dst_sz;
}

static bool join_path3(char *dst, size_t dst_sz, const char *a, const char *b,
		       const char *c)
{
	int n = snprintf(dst, dst_sz, "%s/%s/%s", a, b, c);
	return n >= 0 && (size_t)n < dst_sz;
}

static bool has_suffix(const char *str, const char *suffix)
{
	size_t slen = strlen(str);
	size_t xlen = strlen(suffix);
	return slen >= xlen && strcmp(str + slen - xlen, suffix) == 0;
}

/* ----------------------------------------------------- Pure Testable API */

static void normalize_mod_name(const char *src, char *dst, size_t dst_sz)
{
	size_t i = 0;

	if (!src || dst_sz == 0) {
		if (dst_sz)
			dst[0] = '\0';
		return;
	}
	const char *base = strrchr(src, '/');
	src = base ? base + 1 : src;

	while (src[i] && i + 1 < dst_sz) {
		if (strcmp(src + i, ".ko") == 0 ||
		    strcmp(src + i, ".ko.gz") == 0 ||
		    strcmp(src + i, ".ko.xz") == 0 ||
		    strcmp(src + i, ".ko.zst") == 0)
			break;
		dst[i] = (src[i] == '-') ? '_' : src[i];
		i++;
	}
	dst[i] = '\0';
}

static void add_blacklist_csv(struct boot_cfg *cfg, const char *csv)
{
	char tmp[256];
	char *saveptr = NULL;

	copy_str(tmp, sizeof(tmp), csv);
	for (char *tok = strtok_r(tmp, ",", &saveptr);
	     tok && cfg->n_blacklist < MAX_BLACKLIST;
	     tok = strtok_r(NULL, ",", &saveptr)) {
		char norm[64];
		normalize_mod_name(tok, norm, sizeof(norm));
		if (!norm[0])
			continue;
		bool dup = false;
		for (int i = 0; i < cfg->n_blacklist; i++) {
			if (strcmp(cfg->blacklist[i], norm) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup)
			copy_str(cfg->blacklist[cfg->n_blacklist++],
				 sizeof(cfg->blacklist[0]), norm);
	}
}

static bool is_mod_blacklisted(const struct boot_cfg *cfg, const char *mod_name)
{
	char norm[64];

	if (!cfg || !mod_name || !mod_name[0])
		return false;
	normalize_mod_name(mod_name, norm, sizeof(norm));
	for (int i = 0; i < cfg->n_blacklist; i++) {
		if (strcmp(cfg->blacklist[i], norm) == 0)
			return true;
	}
	return false;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

/* Decode udev \xNN escapes (e.g. \x20 -> space) in /dev/disk/by-* paths. */
static void decode_udev_escapes(const char *src, char *dst, size_t dst_sz)
{
	size_t o = 0;

	if (!dst || dst_sz == 0)
		return;
	while (src && *src && o + 1 < dst_sz) {
		if (src[0] == '\\' && src[1] == 'x' && src[2] && src[3]) {
			int hi = hex_nibble(src[2]);
			int lo = hex_nibble(src[3]);
			if (hi >= 0 && lo >= 0) {
				dst[o++] = (char)((hi << 4) | lo);
				src += 4;
				continue;
			}
		}
		dst[o++] = *src++;
	}
	dst[o] = '\0';
}

/* Normalize dracut root= forms (block:, /dev/disk/by-{uuid,partuuid,label,partlabel}/). */
static void normalize_root_spec(const char *raw, char *out, size_t out_sz)
{
	static const struct {
		const char *prefix;
		const char *tag;
	} by_map[] = {
		{ "/dev/disk/by-uuid/",      "UUID=" },
		{ "/dev/disk/by-partuuid/",  "PARTUUID=" },
		{ "/dev/disk/by-label/",     "LABEL=" },
		{ "/dev/disk/by-partlabel/", "PARTLABEL=" },
	};

	if (!out || out_sz == 0)
		return;
	if (!raw) {
		out[0] = '\0';
		return;
	}
	if (strncmp(raw, "block:", 6) == 0)
		raw += 6;

	for (size_t i = 0; i < sizeof(by_map) / sizeof(by_map[0]); i++) {
		size_t plen = strlen(by_map[i].prefix);
		if (strncmp(raw, by_map[i].prefix, plen) == 0 && raw[plen] != '\0') {
			char decoded[224];
			decode_udev_escapes(raw + plen, decoded, sizeof(decoded));
			snprintf(out, out_sz, "%s%s", by_map[i].tag, decoded);
			return;
		}
	}
	copy_str(out, out_sz, raw);
}

/*
 * Split rootflags= into kernel mount(2) VFS bitmask flags and FS-specific
 * data string. Passing VFS flags like "noatime" or "defaults" in mount(2)'s
 * data string causes ext4/btrfs/f2fs to fail with -EINVAL.
 */
static void parse_mount_flags(const char *opt_str, bool default_ro,
			      unsigned long *flags_out,
			      char *fs_data_out, size_t fs_data_sz)
{
	unsigned long flags = default_ro ? MS_RDONLY : 0;
	char tmp[256];
	char *saveptr = NULL;
	size_t dlen = 0;

	if (fs_data_out && fs_data_sz > 0)
		fs_data_out[0] = '\0';

	if (opt_str && opt_str[0]) {
		copy_str(tmp, sizeof(tmp), opt_str);
		for (char *tok = strtok_r(tmp, ",", &saveptr);
		     tok;
		     tok = strtok_r(NULL, ",", &saveptr)) {
			if (!tok[0] || strcmp(tok, "defaults") == 0) {
				continue;
			} else if (strcmp(tok, "ro") == 0) {
				flags |= MS_RDONLY;
			} else if (strcmp(tok, "rw") == 0) {
				flags &= ~(unsigned long)MS_RDONLY;
			} else if (strcmp(tok, "noatime") == 0) {
				flags |= MS_NOATIME;
			} else if (strcmp(tok, "atime") == 0) {
				flags &= ~(unsigned long)MS_NOATIME;
			} else if (strcmp(tok, "nodiratime") == 0) {
				flags |= MS_NODIRATIME;
			} else if (strcmp(tok, "diratime") == 0) {
				flags &= ~(unsigned long)MS_NODIRATIME;
			} else if (strcmp(tok, "relatime") == 0) {
				flags |= MS_RELATIME;
			} else if (strcmp(tok, "norelatime") == 0) {
				flags &= ~(unsigned long)MS_RELATIME;
			} else if (strcmp(tok, "strictatime") == 0) {
				flags |= MS_STRICTATIME;
			} else if (strcmp(tok, "nostrictatime") == 0) {
				flags &= ~(unsigned long)MS_STRICTATIME;
			} else if (strcmp(tok, "lazytime") == 0) {
				flags |= MS_LAZYTIME;
			} else if (strcmp(tok, "nolazytime") == 0) {
				flags &= ~(unsigned long)MS_LAZYTIME;
			} else if (strcmp(tok, "sync") == 0) {
				flags |= MS_SYNCHRONOUS;
			} else if (strcmp(tok, "async") == 0) {
				flags &= ~(unsigned long)MS_SYNCHRONOUS;
			} else if (strcmp(tok, "dirsync") == 0) {
				flags |= MS_DIRSYNC;
			} else if (strcmp(tok, "nosuid") == 0) {
				flags |= MS_NOSUID;
			} else if (strcmp(tok, "suid") == 0) {
				flags &= ~(unsigned long)MS_NOSUID;
			} else if (strcmp(tok, "nodev") == 0) {
				flags |= MS_NODEV;
			} else if (strcmp(tok, "dev") == 0) {
				flags &= ~(unsigned long)MS_NODEV;
			} else if (strcmp(tok, "noexec") == 0) {
				flags |= MS_NOEXEC;
			} else if (strcmp(tok, "exec") == 0) {
				flags &= ~(unsigned long)MS_NOEXEC;
			} else if (strcmp(tok, "mand") == 0) {
				flags |= MS_MANDLOCK;
			} else if (strcmp(tok, "nomand") == 0) {
				flags &= ~(unsigned long)MS_MANDLOCK;
			} else if (fs_data_out && fs_data_sz > 0) {
				size_t tlen = strlen(tok);
				size_t need = (dlen > 0 ? 1 : 0) + tlen + 1;
				if (dlen + need <= fs_data_sz) {
					if (dlen > 0)
						fs_data_out[dlen++] = ',';
					memcpy(fs_data_out + dlen, tok, tlen);
					dlen += tlen;
					fs_data_out[dlen] = '\0';
				}
			}
		}
	}

	if (flags_out)
		*flags_out = flags;
}

static void cfg_defaults(struct boot_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->rootdelay = 5;
	cfg->root_ro = true; /* dracut default: mount ro, systemd remounts rw per fstab */
	cfg->rd_shell = true;
}

static void parse_cmdline_str(const char *cmdline, struct boot_cfg *cfg)
{
	const char *p = cmdline;
	bool has_real_init = false;
	bool after_dashdash = false;
	bool has_rd_timeout = false;

	cfg_defaults(cfg);
	if (!cmdline)
		return;

	while (*p) {
		char tok[512];
		size_t tlen = 0;
		bool in_quotes = false;

		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;

		while (*p && (in_quotes || !isspace((unsigned char)*p))) {
			if (*p == '"') {
				in_quotes = !in_quotes;
				p++;
				continue;
			}
			if (tlen + 1 < sizeof(tok))
				tok[tlen++] = *p;
			p++;
		}
		tok[tlen] = '\0';

		if (after_dashdash) {
			if (cfg->n_init_args < MAX_INIT_ARGS && tok[0])
				copy_str(cfg->init_args[cfg->n_init_args++],
					 sizeof(cfg->init_args[0]), tok);
			continue;
		}
		if (strcmp(tok, "--") == 0) {
			after_dashdash = true;
			continue;
		}

		if (strncmp(tok, "root=", 5) == 0) {
			normalize_root_spec(tok + 5, cfg->root, sizeof(cfg->root));
		} else if (strncmp(tok, "rootfstype=", 11) == 0) {
			copy_str(cfg->rootfstype, sizeof(cfg->rootfstype), tok + 11);
		} else if (strncmp(tok, "rootflags=", 10) == 0) {
			copy_str(cfg->rootflags, sizeof(cfg->rootflags), tok + 10);
		} else if (strncmp(tok, "rootdelay=", 10) == 0) {
			char *end = NULL;
			long v = strtol(tok + 10, &end, 10);
			if (end && *end == '\0' && v >= 0 && v <= 600)
				cfg->rootdelay = (int)v;
		} else if (strncmp(tok, "rd.timeout=", 11) == 0) {
			char *end = NULL;
			long v = strtol(tok + 11, &end, 10);
			if (end && *end == '\0' && v >= 0 && v <= 600) {
				cfg->rootdelay = (int)v;
				has_rd_timeout = true;
			}
		} else if (strcmp(tok, "rootwait") == 0) {
			cfg->rootwait = true;
		} else if (strcmp(tok, "ro") == 0) {
			cfg->root_ro = true;
		} else if (strcmp(tok, "rw") == 0) {
			cfg->root_ro = false;
		} else if (strncmp(tok, "real_init=", 10) == 0) {
			copy_str(cfg->real_init, sizeof(cfg->real_init), tok + 10);
			has_real_init = true;
		} else if (strncmp(tok, "init=", 5) == 0 && !has_real_init) {
			copy_str(cfg->real_init, sizeof(cfg->real_init), tok + 5);
		} else if (strcmp(tok, "noswitch") == 0) {
			cfg->noswitch = true;
		} else if (strcmp(tok, "rd.nomodules") == 0 || strcmp(tok, "rd.nomodules=1") == 0) {
			cfg->rd_nomodules = true;
		} else if (strncmp(tok, "rd.blacklist=", 13) == 0) {
			add_blacklist_csv(cfg, tok + 13);
		} else if (strncmp(tok, "module_blacklist=", 17) == 0) {
			add_blacklist_csv(cfg, tok + 17);
		} else if (strcmp(tok, "rd.shell=0") == 0) {
			cfg->rd_shell = false;
		} else if (strcmp(tok, "rd.shell") == 0 || strcmp(tok, "rd.shell=1") == 0) {
			cfg->rd_shell = true;
		} else if (strncmp(tok, "rd.emergency=", 13) == 0) {
			copy_str(cfg->rd_emergency, sizeof(cfg->rd_emergency), tok + 13);
		} else if (strcmp(tok, "rd.break") == 0) {
			copy_str(cfg->rd_break, sizeof(cfg->rd_break), "pre-mount");
		} else if (strncmp(tok, "rd.break=", 9) == 0) {
			copy_str(cfg->rd_break, sizeof(cfg->rd_break), tok + 9);
		}
	}

	if (cfg->rootwait && !has_rd_timeout && cfg->rootdelay < 30)
		cfg->rootdelay = 30;
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void format_uuid(const uint8_t u[16], char out[37])
{
	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void copy_fixed_label(const uint8_t *src, size_t max_len,
			     char *dst, size_t dst_sz)
{
	size_t n = 0;

	if (!dst || dst_sz == 0)
		return;
	while (n < max_len && n + 1 < dst_sz && src[n] != 0) {
		dst[n] = (char)src[n];
		n++;
	}
	dst[n] = '\0';
}

static void copy_utf16le_label(const uint8_t *src, size_t max_units,
			       char *dst, size_t dst_sz)
{
	size_t o = 0;

	if (!dst || dst_sz == 0)
		return;
	for (size_t i = 0; i < max_units; i++) {
		uint16_t ch = read_le16(src + i * 2);
		if (ch == 0)
			break;
		if (ch < 0x80) {
			if (o + 1 >= dst_sz)
				break;
			dst[o++] = (char)ch;
		} else if (ch < 0x800) {
			if (o + 2 >= dst_sz)
				break;
			dst[o++] = (char)(0xC0 | (ch >> 6));
			dst[o++] = (char)(0x80 | (ch & 0x3F));
		} else {
			if (o + 3 >= dst_sz)
				break;
			dst[o++] = (char)(0xE0 | (ch >> 12));
			dst[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
			dst[o++] = (char)(0x80 | (ch & 0x3F));
		}
	}
	dst[o] = '\0';
}

/*
 * Probe Linux rootfs superblock magic, UUID, and LABEL directly from fd.
 * Supported: ext2/3/4, F2FS, EROFS, XFS, SquashFS, Btrfs.
 * Btrfs (64 KiB offset) is checked last to avoid false positives from stale
 * unwiped Btrfs superblocks under freshly formatted ext4/f2fs/xfs partitions.
 */
static bool probe_fs_fd(int fd, struct fs_info *out)
{
	struct fs_info info;
	uint8_t sb1k[EXT4_SUPER_SIZE];
	uint8_t sb0[256];
	uint8_t sb_btrfs[BTRFS_SUPER_READ_SZ];

	memset(&info, 0, sizeof(info));

	if (pread(fd, sb1k, sizeof(sb1k), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(sb1k)) {
		if (read_le16(sb1k + EXT4_MAGIC_OFF) == EXT4_SUPER_MAGIC) {
			copy_str(info.fstype, sizeof(info.fstype), "ext4");
			format_uuid(sb1k + EXT4_UUID_OFF, info.uuid);
			copy_fixed_label(sb1k + EXT4_LABEL_OFF, EXT4_LABEL_LEN,
					 info.label, sizeof(info.label));
			if (out)
				*out = info;
			return true;
		}
		if (read_le32(sb1k) == F2FS_SUPER_MAGIC) {
			copy_str(info.fstype, sizeof(info.fstype), "f2fs");
			format_uuid(sb1k + F2FS_UUID_OFF, info.uuid);
			copy_utf16le_label(sb1k + F2FS_LABEL_OFF, F2FS_LABEL_MAX_UTF16,
					   info.label, sizeof(info.label));
			if (out)
				*out = info;
			return true;
		}
		if (read_le32(sb1k) == EROFS_SUPER_MAGIC) {
			copy_str(info.fstype, sizeof(info.fstype), "erofs");
			format_uuid(sb1k + EROFS_UUID_OFF, info.uuid);
			copy_fixed_label(sb1k + EROFS_LABEL_OFF, EROFS_LABEL_LEN,
					 info.label, sizeof(info.label));
			if (out)
				*out = info;
			return true;
		}
	}

	if (pread(fd, sb0, sizeof(sb0), 0) == (ssize_t)sizeof(sb0)) {
		if (memcmp(sb0, "XFSB", 4) == 0) {
			copy_str(info.fstype, sizeof(info.fstype), "xfs");
			format_uuid(sb0 + XFS_UUID_OFF, info.uuid);
			copy_fixed_label(sb0 + XFS_LABEL_OFF, XFS_LABEL_LEN,
					 info.label, sizeof(info.label));
			if (out)
				*out = info;
			return true;
		}
		if (read_le32(sb0) == SQUASHFS_MAGIC_LE) {
			copy_str(info.fstype, sizeof(info.fstype), "squashfs");
			if (out)
				*out = info;
			return true;
		}
	}

	if (pread(fd, sb_btrfs, sizeof(sb_btrfs), BTRFS_SUPER_OFFSET) ==
	    (ssize_t)sizeof(sb_btrfs) &&
	    memcmp(sb_btrfs + BTRFS_MAGIC_OFF, "_BHRfS_M", 8) == 0) {
		copy_str(info.fstype, sizeof(info.fstype), "btrfs");
		format_uuid(sb_btrfs + BTRFS_UUID_OFF, info.uuid);
		copy_fixed_label(sb_btrfs + BTRFS_LABEL_OFF, BTRFS_LABEL_LEN,
				 info.label, sizeof(info.label));
		if (out)
			*out = info;
		return true;
	}

	return false;
}

static bool probe_fs_path(const char *devpath, struct fs_info *out)
{
	int fd = open(devpath, O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = probe_fs_fd(fd, out);
	close(fd);
	return ok;
}

static uint32_t detect_gpt_sector_size_fd(int fd, uint32_t hw_blksize)
{
	char sig[8];
	uint32_t candidates[2];
	int nc = 0;

	if (hw_blksize >= 512 && (hw_blksize & (hw_blksize - 1)) == 0)
		candidates[nc++] = hw_blksize;
	if (nc == 0 || candidates[0] != 512)
		candidates[nc++] = 512;

	for (int i = 0; i < nc; i++) {
		if (pread(fd, sig, sizeof(sig), (off_t)candidates[i]) == 8 &&
		    memcmp(sig, "EFI PART", 8) == 0)
			return candidates[i];
	}
	return 0;
}

static bool parse_uevent_buf(const char *buf, struct part_info *info)
{
	const char *p = buf;

	memset(info, 0, sizeof(*info));
	while (p && *p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char line[256];

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';

		if (strncmp(line, "DEVNAME=", 8) == 0) {
			copy_str(info->devname, sizeof(info->devname), line + 8);
			info->has_dev = true;
		} else if (strncmp(line, "MAJOR=", 6) == 0) {
			info->major = (unsigned int)strtoul(line + 6, NULL, 10);
		} else if (strncmp(line, "MINOR=", 6) == 0) {
			info->minor = (unsigned int)strtoul(line + 6, NULL, 10);
		} else if (strncmp(line, "PARTN=", 6) == 0) {
			info->partn = (unsigned int)strtoul(line + 6, NULL, 10);
		} else if (strncmp(line, "PARTNAME=", 9) == 0) {
			copy_str(info->partname, sizeof(info->partname), line + 9);
		} else if (strncmp(line, "PARTUUID=", 9) == 0) {
			copy_str(info->partuuid, sizeof(info->partuuid), line + 9);
		}

		if (!nl)
			break;
		p = nl + 1;
	}
	return info->has_dev;
}

static int copy_file_data(const char *src, const char *dst, mode_t mode)
{
	int sfd = open(src, O_RDONLY | O_CLOEXEC);
	int dfd;
	unsigned char buf[16384];
	int rc = 0;

	if (sfd < 0)
		return -1;
	dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		   (mode & 0777) ? (mode & 0777) : 0644);
	if (dfd < 0) {
		close(sfd);
		return -1;
	}

	for (;;) {
		ssize_t r = read(sfd, buf, sizeof(buf));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			rc = -1;
			break;
		}
		if (r == 0)
			break;
		ssize_t off = 0;
		while (off < r) {
			ssize_t w = write(dfd, buf + off, (size_t)(r - off));
			if (w < 0) {
				if (errno == EINTR)
					continue;
				rc = -1;
				break;
			}
			off += w;
		}
		if (rc < 0)
			break;
	}

	close(sfd);
	if (close(dfd) < 0)
		rc = -1;
	if (rc < 0)
		unlink(dst);
	return rc;
}

static int copy_tree_rec(const char *src_dir, const char *dst_dir, int depth)
{
	DIR *d;
	struct dirent *de;
	int count = 0;

	if (depth > 8)
		return 0;
	d = opendir(src_dir);
	if (!d)
		return 0;

	while ((de = readdir(d)) != NULL) {
		char src_path[PATH_MAX];
		char dst_path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (!join_path2(src_path, sizeof(src_path), src_dir, de->d_name) ||
		    !join_path2(dst_path, sizeof(dst_path), dst_dir, de->d_name))
			continue;
		if (lstat(src_path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			count += copy_tree_rec(src_path, dst_path, depth + 1);
		} else if (S_ISREG(st.st_mode)) {
			if (mkdir_p(dst_dir, 0755) == 0 &&
			    copy_file_data(src_path, dst_path, st.st_mode) == 0)
				count++;
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len = readlink(src_path, target, sizeof(target) - 1);
			if (len > 0) {
				target[len] = '\0';
				if (mkdir_p(dst_dir, 0755) == 0) {
					unlink(dst_path);
					if (symlink(target, dst_path) == 0)
						count++;
				}
			}
		}
	}
	closedir(d);
	return count;
}

/*
 * Stage initramfs /lib/firmware into /run/firmware (which do_switch_root()
 * MS_MOVEs into /sysroot/run) and point firmware_class.path at it when unset.
 *
 * Linux's fw_get_filesystem_firmware() checks fw_path_para BEFORE the rootfs
 * /lib/firmware paths, so both async driver fw_work and PM_SUSPEND_PREPARE
 * firmware caching work on any rootfs with zero mutation of /sysroot.
 */
static int stage_initramfs_firmware_from(const char *src_dir, const char *dst_dir,
					 const char *param_path)
{
	int count = copy_tree_rec(src_dir, dst_dir, 0);
	char cur[256] = { 0 };

	if (count <= 0)
		return 0;

	if (read_file_str(param_path, cur, sizeof(cur)) >= 0) {
		strip_newline(cur);
		if (cur[0] != '\0') {
			init_log("staged %d firmware file(s) -> %s (keeping firmware_class.path=%s)",
				 count, dst_dir, cur);
			return count;
		}
	}

	if (write_sysfs(param_path, dst_dir) == 0)
		init_log("staged %d firmware file(s) -> %s (firmware_class.path set)",
			 count, dst_dir);
	else
		init_log("staged %d firmware file(s) -> %s (warn: %s unwritable)",
			 count, dst_dir, param_path);

	return count;
}

#ifndef INIT_UNIT_TEST

/* ----------------------------------------------------- Stage 1: Base Mounts */

static void try_symlink(const char *target, const char *linkpath)
{
	if (symlink(target, linkpath) < 0) {
		/* Best-effort symlink (EEXIST / optional configfs node). */
	}
}

static int early_base_mounts(void)
{
	static const char *const dirs[] = {
		"/proc", "/sys", "/dev", "/dev/pts", "/run", "/tmp", NEWROOT
	};

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		mkdir_p(dirs[i], 0755);

	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0 &&
	    errno != EBUSY)
		return -1;

	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0 &&
	    errno != EBUSY)
		return -1;

	if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") < 0 &&
	    errno != EBUSY) {
		if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755") < 0 &&
		    errno != EBUSY)
			return -1;
	}

	mkdir_p("/dev/pts", 0755);
	(void)mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
		    "gid=5,mode=0620");

	(void)mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");

	/* Seed loop nodes if devtmpfs hasn't populated them yet (Option A). */
	if (access("/dev/loop-control", F_OK) < 0)
		(void)mknod("/dev/loop-control", S_IFCHR | 0660, makedev(10, 237));
	for (unsigned int i = 0; i < 8; i++) {
		char lp[32];
		snprintf(lp, sizeof(lp), "/dev/loop%u", i);
		if (access(lp, F_OK) < 0)
			(void)mknod(lp, S_IFBLK | 0660, makedev(7, i));
	}

	try_symlink("/proc/self/fd", "/dev/fd");
	try_symlink("/proc/self/fd/0", "/dev/stdin");
	try_symlink("/proc/self/fd/1", "/dev/stdout");
	try_symlink("/proc/self/fd/2", "/dev/stderr");
	return 0;
}

/* ----------------------------------------------------- Stage 3: Kernel Mods */

static int insmod_file(const char *path, const char *params, bool quiet_eexist)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	long ret;

	if (fd < 0) {
		init_log("WARN: insmod open(%s): %s", path, strerror(errno));
		return -1;
	}

	ret = syscall(SYS_finit_module, fd, params ? params : "", 0);
	int saved_errno = errno;
	close(fd);

	if (ret == 0) {
		init_log("insmod: %s", path);
		return 0;
	}
	if (saved_errno == EEXIST) {
		if (!quiet_eexist)
			init_log("insmod: %s (already loaded)", path);
		return 0;
	}
	init_log("WARN: insmod %s failed: %s", path, strerror(saved_errno));
	return -1;
}

static int modprobe_dep(const struct boot_cfg *cfg, const char *kver,
			const char *mod_name, const char *params, int depth)
{
	char dep_path[PATH_MAX];
	char want[128];
	FILE *fp;
	char line[2048];
	int rc = -1;

	if (depth > 16)
		return -1;

	normalize_mod_name(mod_name, want, sizeof(want));
	if (is_mod_blacklisted(cfg, want)) {
		init_log("skip blacklisted module: %s", want);
		return 0;
	}
	snprintf(dep_path, sizeof(dep_path), "/lib/modules/%s/modules.dep", kver);
	fp = fopen(dep_path, "re");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char *colon = strchr(line, ':');
		char cur_norm[128];

		if (!colon)
			continue;
		*colon = '\0';
		strip_newline(colon + 1);
		normalize_mod_name(line, cur_norm, sizeof(cur_norm));
		if (strcmp(cur_norm, want) != 0)
			continue;

		char *deps = colon + 1;
		char *dep_list[32];
		int ndep = 0;
		char *saveptr = NULL;
		for (char *tok = strtok_r(deps, " \t", &saveptr);
		     tok && ndep < 32;
		     tok = strtok_r(NULL, " \t", &saveptr))
			dep_list[ndep++] = tok;

		for (int i = ndep - 1; i >= 0; i--) {
			char dnorm[128];
			normalize_mod_name(dep_list[i], dnorm, sizeof(dnorm));
			(void)modprobe_dep(cfg, kver, dnorm, "", depth + 1);
		}

		char ko_path[PATH_MAX];
		if (line[0] == '/')
			copy_str(ko_path, sizeof(ko_path), line);
		else
			snprintf(ko_path, sizeof(ko_path), "/lib/modules/%s/%s", kver, line);

		rc = insmod_file(ko_path, params, true);
		break;
	}

	fclose(fp);
	return rc;
}

static void insmod_dir(const struct boot_cfg *cfg, const char *dir_path)
{
	struct dirent **entries = NULL;
	int n = scandir(dir_path, &entries, NULL, alphasort);

	if (n <= 0)
		return;

	for (int i = 0; i < n; i++) {
		const char *name = entries[i]->d_name;
		if (name[0] != '.' && has_suffix(name, ".ko")) {
			if (is_mod_blacklisted(cfg, name)) {
				init_log("skip blacklisted module: %s", name);
			} else {
				char full[PATH_MAX];
				if (join_path2(full, sizeof(full), dir_path, name))
					(void)insmod_file(full, "", true);
			}
		}
		free(entries[i]);
	}
	free(entries);
}

static void load_modules(const struct boot_cfg *cfg, const char *kver)
{
	FILE *fp = fopen("/etc/modules", "re");

	if (fp) {
		char line[512];
		while (fgets(line, sizeof(line), fp)) {
			char *p = line;
			strip_newline(p);
			while (*p && isspace((unsigned char)*p))
				p++;
			if (*p == '\0' || *p == '#')
				continue;

			char *mod = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			char *params = "";
			if (*p) {
				*p++ = '\0';
				while (*p && isspace((unsigned char)*p))
					p++;
				params = p;
			}

			if (is_mod_blacklisted(cfg, mod)) {
				init_log("skip blacklisted module: %s", mod);
				continue;
			}

			if (mod[0] == '/' || has_suffix(mod, ".ko")) {
				if (access(mod, R_OK) == 0)
					(void)insmod_file(mod, params, true);
			} else if (modprobe_dep(cfg, kver, mod, params, 0) != 0) {
				char extra_ko[PATH_MAX];
				snprintf(extra_ko, sizeof(extra_ko),
					 "/lib/modules/extra/%s.ko", mod);
				if (access(extra_ko, R_OK) == 0)
					(void)insmod_file(extra_ko, params, true);
			}
		}
		fclose(fp);
	}

	/* Auto-load any remaining out-of-tree .ko modules (e.g. EXTRA_KMODS). */
	insmod_dir(cfg, "/lib/modules/extra");
	if (kver && kver[0]) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "/lib/modules/%s/updates", kver);
		insmod_dir(cfg, dir);
	}
}

/* ----------------------------------------- Stage 4: Root & Option A Loop */

static bool ensure_block_node(const struct part_info *pi, char *dev_out, size_t out_sz)
{
	struct stat st;

	if (!pi->has_dev)
		return false;
	snprintf(dev_out, out_sz, "/dev/%s", pi->devname);
	if (stat(dev_out, &st) == 0 && S_ISBLK(st.st_mode))
		return true;
	if (pi->major == 0)
		return false;
	if (mknod(dev_out, S_IFBLK | 0660, makedev(pi->major, pi->minor)) < 0 &&
	    errno != EEXIST)
		return false;
	return stat(dev_out, &st) == 0 && S_ISBLK(st.st_mode);
}

static bool match_part_spec(const char *root_spec, const struct part_info *pi,
			    char *dev_out, size_t out_sz, struct fs_info *fs_out)
{
	char candidate[256];
	struct fs_info fi;

	if (!pi->has_dev)
		return false;

	if (strncmp(root_spec, "PARTLABEL=", 10) == 0) {
		if (strcmp(pi->partname, root_spec + 10) != 0)
			return false;
		if (!ensure_block_node(pi, dev_out, out_sz))
			return false;
		if (fs_out) {
			memset(fs_out, 0, sizeof(*fs_out));
			(void)probe_fs_path(dev_out, fs_out);
		}
		return true;
	}
	if (strncmp(root_spec, "PARTUUID=", 9) == 0) {
		if (strcasecmp(pi->partuuid, root_spec + 9) != 0)
			return false;
		if (!ensure_block_node(pi, dev_out, out_sz))
			return false;
		if (fs_out) {
			memset(fs_out, 0, sizeof(*fs_out));
			(void)probe_fs_path(dev_out, fs_out);
		}
		return true;
	}
	if (strncmp(root_spec, "UUID=", 5) == 0) {
		if (!ensure_block_node(pi, candidate, sizeof(candidate)))
			return false;
		if (!probe_fs_path(candidate, &fi) || !fi.uuid[0])
			return false;
		if (strcasecmp(fi.uuid, root_spec + 5) != 0)
			return false;
		copy_str(dev_out, out_sz, candidate);
		if (fs_out)
			*fs_out = fi;
		return true;
	}
	if (strncmp(root_spec, "LABEL=", 6) == 0) {
		if (!ensure_block_node(pi, candidate, sizeof(candidate)))
			return false;
		if (!probe_fs_path(candidate, &fi) || !fi.label[0])
			return false;
		if (strcmp(fi.label, root_spec + 6) != 0)
			return false;
		copy_str(dev_out, out_sz, candidate);
		if (fs_out)
			*fs_out = fi;
		return true;
	}
	return false;
}

static bool scan_disk_partitions(const char *disk_name, const char *root_spec,
				 char *dev_out, size_t out_sz, struct fs_info *fs_out)
{
	char disk_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;
	size_t dlen = strlen(disk_name);

	snprintf(disk_dir, sizeof(disk_dir), "/sys/block/%s", disk_name);
	d = opendir(disk_dir);
	if (!d)
		return false;

	while ((de = readdir(d)) != NULL) {
		char uev_path[PATH_MAX];
		char buf[1024];
		struct part_info pi;

		if (de->d_name[0] == '.' || strncmp(de->d_name, disk_name, dlen) != 0)
			continue;
		if (!join_path3(uev_path, sizeof(uev_path), disk_dir, de->d_name, "uevent"))
			continue;
		if (read_file_str(uev_path, buf, sizeof(buf)) <= 0)
			continue;
		if (!parse_uevent_buf(buf, &pi))
			continue;
		if (match_part_spec(root_spec, &pi, dev_out, out_sz, fs_out)) {
			closedir(d);
			return true;
		}
	}
	closedir(d);

	char whole_uev[PATH_MAX];
	char buf[1024];
	struct part_info pi;
	if (join_path2(whole_uev, sizeof(whole_uev), disk_dir, "uevent") &&
	    read_file_str(whole_uev, buf, sizeof(buf)) > 0 &&
	    parse_uevent_buf(buf, &pi) &&
	    match_part_spec(root_spec, &pi, dev_out, out_sz, fs_out))
		return true;

	return false;
}

static bool resolve_root_spec(const char *root_spec, const char *only_disk,
			      char *dev_out, size_t out_sz, struct fs_info *fs_out)
{
	if (!root_spec || !root_spec[0])
		return false;

	if (strncmp(root_spec, "/dev/", 5) == 0) {
		struct stat st;
		if (stat(root_spec, &st) == 0 && S_ISBLK(st.st_mode)) {
			copy_str(dev_out, out_sz, root_spec);
			if (fs_out) {
				memset(fs_out, 0, sizeof(*fs_out));
				(void)probe_fs_path(dev_out, fs_out);
			}
			return true;
		}
		return false;
	}

	if (only_disk)
		return scan_disk_partitions(only_disk, root_spec, dev_out, out_sz, fs_out);

	DIR *d = opendir("/sys/block");
	struct dirent *de;
	if (!d)
		return false;

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' ||
		    strncmp(de->d_name, "ram", 3) == 0 ||
		    strncmp(de->d_name, "zram", 4) == 0)
			continue;
		if (scan_disk_partitions(de->d_name, root_spec, dev_out, out_sz, fs_out)) {
			closedir(d);
			return true;
		}
	}
	closedir(d);
	return false;
}

static bool is_preferred_root_name(const char *name)
{
	static const char *const names[] = {
		"rootfs", "root", "xnu-root", "rootfs-ext4", "mainline", "linux"
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(name, names[i]) == 0)
			return true;
	}
	return false;
}

static int part_cmp(const void *a, const void *b)
{
	const struct part_info *pa = (const struct part_info *)a;
	const struct part_info *pb = (const struct part_info *)b;
	if (pa->partn < pb->partn)
		return -1;
	if (pa->partn > pb->partn)
		return 1;
	return strcmp(pa->devname, pb->devname);
}

/*
 * Option A: Attach xnu_data to a loop device once using the kernel's native
 * GPT partition parser (BLKSSZGET + LOOP_CONFIGURE with LO_FLAGS_PARTSCAN).
 * Zero hardcoded byte offsets.
 */
static bool attach_xnu_data_loop(char *loop_disk_out, size_t loop_disk_sz)
{
	char xnu_dev[256] = { 0 };
	int backing_fd = -1, ctrl_fd = -1, loop_fd = -1;
	int loop_num = -1;
	int hw_blksize = 4096;
	uint32_t gpt_blksize;
	char loop_dev[64];
	char loop_disk[32];

	if (!resolve_root_spec("PARTLABEL=xnu_data", NULL, xnu_dev, sizeof(xnu_dev), NULL)) {
		struct stat st;
		if (stat("/dev/sdc72", &st) == 0 && S_ISBLK(st.st_mode))
			copy_str(xnu_dev, sizeof(xnu_dev), "/dev/sdc72");
	}
	if (!xnu_dev[0])
		return false;

	backing_fd = open(xnu_dev, O_RDWR | O_CLOEXEC);
	if (backing_fd < 0)
		backing_fd = open(xnu_dev, O_RDONLY | O_CLOEXEC);
	if (backing_fd < 0) {
		init_log("WARN: open(%s) failed: %s", xnu_dev, strerror(errno));
		return false;
	}

	if (ioctl(backing_fd, BLKSSZGET, &hw_blksize) < 0 || hw_blksize < 512)
		hw_blksize = 4096;

	gpt_blksize = detect_gpt_sector_size_fd(backing_fd, (uint32_t)hw_blksize);
	if (gpt_blksize == 0) {
		init_log("WARN: no nested GPT on %s (blksize=%d)", xnu_dev, hw_blksize);
		close(backing_fd);
		return false;
	}

	ctrl_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctrl_fd < 0) {
		close(backing_fd);
		return false;
	}
	loop_num = ioctl(ctrl_fd, LOOP_CTL_GET_FREE);
	close(ctrl_fd);
	if (loop_num < 0) {
		close(backing_fd);
		return false;
	}

	snprintf(loop_disk, sizeof(loop_disk), "loop%d", loop_num);
	snprintf(loop_dev, sizeof(loop_dev), "/dev/%s", loop_disk);
	if (access(loop_dev, F_OK) < 0)
		(void)mknod(loop_dev, S_IFBLK | 0660, makedev(7, (unsigned int)loop_num));

	loop_fd = open(loop_dev, O_RDWR | O_CLOEXEC);
	if (loop_fd < 0) {
		close(backing_fd);
		return false;
	}

	struct loop_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd = (uint32_t)backing_fd;
	cfg.block_size = gpt_blksize;
	cfg.info.lo_flags = LO_FLAGS_PARTSCAN;
	copy_str((char *)cfg.info.lo_file_name, LO_NAME_SIZE, xnu_dev);

	if (ioctl(loop_fd, LOOP_CONFIGURE, &cfg) < 0) {
		if (ioctl(loop_fd, LOOP_SET_FD, backing_fd) < 0) {
			init_log("WARN: loop attach %s failed: %s", xnu_dev, strerror(errno));
			close(loop_fd);
			close(backing_fd);
			return false;
		}
		(void)ioctl(loop_fd, LOOP_SET_BLOCK_SIZE, (unsigned long)gpt_blksize);
		struct loop_info64 li;
		memset(&li, 0, sizeof(li));
		li.lo_flags = LO_FLAGS_PARTSCAN;
		copy_str((char *)li.lo_file_name, LO_NAME_SIZE, xnu_dev);
		(void)ioctl(loop_fd, LOOP_SET_STATUS64, &li);
		(void)ioctl(loop_fd, BLKRRPART);
	}
	close(backing_fd);

	init_log("loop attached: %s -> %s (blksize=%u, LO_FLAGS_PARTSCAN)",
		 xnu_dev, loop_dev, gpt_blksize);

	char sys_loop[PATH_MAX];
	snprintf(sys_loop, sizeof(sys_loop), "/sys/block/%s", loop_disk);
	size_t ldlen = strlen(loop_disk);
	bool saw_part = false;

	for (int tries = 0; tries < 30 && !saw_part; tries++) {
		DIR *d = opendir(sys_loop);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] != '.' &&
				    strncmp(de->d_name, loop_disk, ldlen) == 0 &&
				    de->d_name[ldlen] == 'p') {
					saw_part = true;
					break;
				}
			}
			closedir(d);
		}
		if (!saw_part)
			usleep(100000);
	}

	if (!saw_part) {
		init_log("WARN: kernel GPT parser found 0 partitions on %s", loop_dev);
		(void)ioctl(loop_fd, LOOP_CLR_FD, 0);
		close(loop_fd);
		return false;
	}
	close(loop_fd);
	copy_str(loop_disk_out, loop_disk_sz, loop_disk);
	return true;
}

static bool select_nested_gpt_root(const char *loop_disk, const char *root_spec,
				   char *dev_out, size_t out_sz, struct fs_info *fs_out)
{
	struct part_info parts[32];
	int nparts = 0;
	char sys_loop[PATH_MAX];
	size_t ldlen = strlen(loop_disk);
	DIR *d;

	if (root_spec && root_spec[0])
		return resolve_root_spec(root_spec, loop_disk, dev_out, out_sz, fs_out);

	snprintf(sys_loop, sizeof(sys_loop), "/sys/block/%s", loop_disk);
	d = opendir(sys_loop);
	if (!d)
		return false;

	struct dirent *de;
	while ((de = readdir(d)) != NULL && nparts < 32) {
		char uev[PATH_MAX], buf[1024];
		struct part_info pi;
		if (de->d_name[0] == '.' || strncmp(de->d_name, loop_disk, ldlen) != 0)
			continue;
		if (!join_path3(uev, sizeof(uev), sys_loop, de->d_name, "uevent"))
			continue;
		if (read_file_str(uev, buf, sizeof(buf)) > 0 &&
		    parse_uevent_buf(buf, &pi))
			parts[nparts++] = pi;
	}
	closedir(d);
	if (nparts == 0)
		return false;

	qsort(parts, (size_t)nparts, sizeof(parts[0]), part_cmp);

	char fallback_dev[256] = { 0 };
	struct fs_info fallback_fs;
	memset(&fallback_fs, 0, sizeof(fallback_fs));

	for (int i = 0; i < nparts; i++) {
		char cand[256];
		struct fs_info fi;
		if (!ensure_block_node(&parts[i], cand, sizeof(cand)))
			continue;
		if (!probe_fs_path(cand, &fi))
			continue;
		if (is_preferred_root_name(parts[i].partname) ||
		    is_preferred_root_name(fi.label)) {
			copy_str(dev_out, out_sz, cand);
			if (fs_out)
				*fs_out = fi;
			return true;
		}
		if (!fallback_dev[0]) {
			copy_str(fallback_dev, sizeof(fallback_dev), cand);
			fallback_fs = fi;
		}
	}

	if (fallback_dev[0]) {
		copy_str(dev_out, out_sz, fallback_dev);
		if (fs_out)
			*fs_out = fallback_fs;
		return true;
	}
	return false;
}

static bool mount_real_root(const char *root_dev, const struct boot_cfg *cfg,
			    const struct fs_info *probed_fs)
{
	static const char *const fallback_types[] = {
		"ext4", "f2fs", "btrfs", "xfs", "erofs", "squashfs"
	};
	unsigned long flags = 0;
	char fs_data[256] = { 0 };
	const char *fstypes[8];
	int n = 0;

	parse_mount_flags(cfg->rootflags, cfg->root_ro, &flags, fs_data, sizeof(fs_data));

	if (cfg->rootfstype[0])
		fstypes[n++] = cfg->rootfstype;
	if (probed_fs && probed_fs->fstype[0]) {
		bool dup = (n > 0 && strcmp(fstypes[0], probed_fs->fstype) == 0);
		if (!dup)
			fstypes[n++] = probed_fs->fstype;
	}
	for (size_t i = 0; i < sizeof(fallback_types) / sizeof(fallback_types[0]); i++) {
		bool dup = false;
		for (int j = 0; j < n; j++) {
			if (strcmp(fstypes[j], fallback_types[i]) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup)
			fstypes[n++] = fallback_types[i];
	}

	const char *data = fs_data[0] ? fs_data : NULL;
	for (int i = 0; i < n; i++) {
		if (mount(root_dev, NEWROOT, fstypes[i], flags, data) == 0) {
			init_log("Mounted %s on %s (%s, %s%s%s)",
				 root_dev, NEWROOT, fstypes[i],
				 (flags & MS_RDONLY) ? "ro" : "rw",
				 data ? ", " : "", data ? data : "");
			return true;
		}
	}
	init_log("WARN: mount %s on %s failed: %s", root_dev, NEWROOT, strerror(errno));
	return false;
}

/* ------------------------------------------- Stage 6: Rescue & switch_root */

/*
 * Lazily bring up USB ACM (/dev/ttyGS0) ONLY when entering the emergency
 * rescue shell (systemd was not reached). Normal boots leave configfs and
 * USB gadget configuration entirely to systemd.
 */
static void rescue_usb_acm(void)
{
	struct stat st;

	mkdir_p("/sys/kernel/config", 0755);
	(void)mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
	if (stat("/sys/kernel/config/usb_gadget", &st) < 0 || !S_ISDIR(st.st_mode))
		return;

	mkdir_p("/sys/kernel/config/usb_gadget/g1/strings/0x409", 0755);
	mkdir_p("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409", 0755);
	(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b");
	(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0104");
	(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "Advan");
	(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "Advan X1 Rescue");
	(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/configuration", "ACM");
	if (mkdir_p("/sys/kernel/config/usb_gadget/g1/functions/acm.usb0", 0755) == 0)
		try_symlink("/sys/kernel/config/usb_gadget/g1/functions/acm.usb0",
			    "/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.usb0");

	char udc[128] = { 0 };
	for (int tries = 0; tries < 20 && !udc[0]; tries++) {
		DIR *d = opendir("/sys/class/udc");
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] != '.') {
					copy_str(udc, sizeof(udc), de->d_name);
					break;
				}
			}
			closedir(d);
		}
		if (!udc[0])
			usleep(100000);
	}
	if (udc[0])
		(void)write_sysfs("/sys/kernel/config/usb_gadget/g1/UDC", udc);
}

static pid_t spawn_getty(const char *tty, bool is_serial)
{
	pid_t pid = fork();
	if (pid != 0)
		return pid;

	for (int i = 0; i < 30; i++) {
		char dev[64];
		snprintf(dev, sizeof(dev), "/dev/%s", tty);
		if (access(dev, R_OK | W_OK) == 0)
			break;
		usleep(100000);
	}

	const char *getty_bin = (access("/sbin/getty", X_OK) == 0) ? "/sbin/getty" :
				(access("/bin/busybox", X_OK) == 0) ? "/bin/busybox" : NULL;
	if (getty_bin) {
		if (is_serial) {
			char *const argv[] = {
				"getty", "-L", "-n", "-l", "/bin/sh",
				"115200", (char *)tty, "vt100", NULL
			};
			execv(getty_bin, argv);
		} else {
			char *const argv[] = {
				"getty", "-n", "-l", "/bin/sh",
				"0", (char *)tty, "linux", NULL
			};
			execv(getty_bin, argv);
		}
	}

	char dev[64];
	snprintf(dev, sizeof(dev), "/dev/%s", tty);
	setsid();
	int fd = open(dev, O_RDWR);
	if (fd >= 0) {
		(void)ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	char *const sh_argv[] = { "/bin/sh", "-i", NULL };
	execv("/bin/sh", sh_argv);
	_exit(127);
}

static void emergency_shell(const struct boot_cfg *cfg, const char *reason)
{
	init_log("EMERGENCY: %s", reason);

	if (cfg && !cfg->rd_shell) {
		init_log("rd.shell=0: skipping rescue shell (action=%s)",
			 cfg->rd_emergency[0] ? cfg->rd_emergency : "reboot");
		sync();
		if (strcmp(cfg->rd_emergency, "poweroff") == 0)
			reboot(RB_POWER_OFF);
		else if (strcmp(cfg->rd_emergency, "halt") == 0)
			reboot(RB_HALT_SYSTEM);
		else
			reboot(RB_AUTOBOOT);
		for (;;)
			pause();
	}

	rescue_usb_acm();

	pid_t gs0_pid = spawn_getty("ttyGS0", true);
	pid_t tty0_pid = (access("/dev/tty0", R_OK | W_OK) == 0) ?
			 spawn_getty("tty0", false) : -1;

	for (;;) {
		int status = 0;
		pid_t dead = waitpid(-1, &status, 0);
		if (dead < 0) {
			if (errno == EINTR)
				continue;
			sleep(1);
			continue;
		}
		if (dead == gs0_pid) {
			sleep(1);
			gs0_pid = spawn_getty("ttyGS0", true);
		} else if (dead == tty0_pid) {
			sleep(1);
			tty0_pid = spawn_getty("tty0", false);
		}
	}
}

static void free_initramfs_dir(int dirfd, dev_t root_dev)
{
	int dupfd = dup(dirfd);
	DIR *d;
	struct dirent *de;

	if (dupfd < 0)
		return;
	d = fdopendir(dupfd);
	if (!d) {
		close(dupfd);
		return;
	}

	while ((de = readdir(d)) != NULL) {
		struct stat st;
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		if (st.st_dev != root_dev)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dirfd, de->d_name,
					 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (sub >= 0) {
				free_initramfs_dir(sub, root_dev);
				close(sub);
			}
			(void)unlinkat(dirfd, de->d_name, AT_REMOVEDIR);
		} else {
			(void)unlinkat(dirfd, de->d_name, 0);
		}
	}
	closedir(d);
}

static void move_or_detach(const char *src, const char *dst)
{
	mkdir_p(dst, 0755);
	if (mount(src, dst, NULL, MS_MOVE, NULL) < 0)
		(void)umount2(src, MNT_DETACH);
}

static void do_switch_root(const struct boot_cfg *cfg)
{
	const char *candidates[4] = {
		cfg->real_init[0] ? cfg->real_init : NULL,
		"/sbin/init",
		"/lib/systemd/systemd",
		"/bin/sh",
	};
	const char *target_init = NULL;
	char full_init[PATH_MAX];

	for (size_t i = 0; i < 4; i++) {
		if (!candidates[i])
			continue;
		snprintf(full_init, sizeof(full_init), NEWROOT "%s", candidates[i]);
		if (access(full_init, X_OK) == 0) {
			target_init = candidates[i];
			break;
		}
	}
	if (!target_init)
		emergency_shell(cfg, "no executable init found in " NEWROOT);

	init_log("switch_root -> %s", target_init);

	move_or_detach("/dev", NEWROOT "/dev");
	move_or_detach("/sys", NEWROOT "/sys");
	move_or_detach("/proc", NEWROOT "/proc");
	move_or_detach("/run", NEWROOT "/run");

	struct stat root_st, newroot_st;
	if (stat("/", &root_st) == 0 && stat(NEWROOT, &newroot_st) == 0 &&
	    root_st.st_dev != newroot_st.st_dev) {
		int rfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (rfd >= 0) {
			free_initramfs_dir(rfd, root_st.st_dev);
			close(rfd);
		}
	}

	if (chdir(NEWROOT) < 0 ||
	    mount(".", "/", NULL, MS_MOVE, NULL) < 0 ||
	    chroot(".") < 0 ||
	    chdir("/") < 0)
		emergency_shell(cfg, "switch_root pivot failed");

	char *argv[MAX_INIT_ARGS + 2];
	int argc = 0;
	argv[argc++] = (char *)target_init;
	for (int i = 0; i < cfg->n_init_args && i < MAX_INIT_ARGS; i++)
		argv[argc++] = (char *)cfg->init_args[i];
	argv[argc] = NULL;

	execv(target_init, argv);
	emergency_shell(cfg, "exec real init failed");
}

/* ------------------------------------------------------------------- Main */

int main(void)
{
	struct boot_cfg cfg;
	struct utsname uts;
	struct fs_info probed_fs;
	char cmdline[4096] = { 0 };
	char root_dev[256] = { 0 };
	char xnu_loop_disk[32] = { 0 };
	bool xnu_attached = false;

	memset(&probed_fs, 0, sizeof(probed_fs));
	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	umask(0022);

	if (early_base_mounts() < 0)
		emergency_shell(NULL, "early base mounts failed");

	if (uname(&uts) < 0) {
		memset(&uts, 0, sizeof(uts));
		copy_str(uts.release, sizeof(uts.release), "unknown");
		copy_str(uts.machine, sizeof(uts.machine), "aarch64");
	}
	init_log("ADVAN X1 init: kernel %s (%s)", uts.release, uts.machine);

	(void)read_file_str("/proc/cmdline", cmdline, sizeof(cmdline));
	strip_newline(cmdline);
	parse_cmdline_str(cmdline, &cfg);

	if (strcmp(cfg.rd_break, "cmdline") == 0)
		emergency_shell(&cfg, "rd.break=cmdline");

	stage_initramfs_firmware_from("/lib/firmware", "/run/firmware",
				      "/sys/module/firmware_class/parameters/path");

	if (strcmp(cfg.rd_break, "pre-udev") == 0)
		emergency_shell(&cfg, "rd.break=pre-udev");
	if (!cfg.rd_nomodules)
		load_modules(&cfg, uts.release);

	if (strcmp(cfg.rd_break, "pre-mount") == 0)
		emergency_shell(&cfg, "rd.break=pre-mount");

	/*
	 * Interleave explicit root= scans with Option A nested-GPT attach on
	 * xnu_data so root= targeting a partition inside xnu_data resolves
	 * immediately instead of stalling for the full rootdelay first.
	 */
	int max_polls = cfg.rootdelay * 5;
	if (max_polls < 1)
		max_polls = 1;
	for (int i = 0; i < max_polls && !root_dev[0]; i++) {
		if (cfg.root[0] &&
		    resolve_root_spec(cfg.root, NULL, root_dev, sizeof(root_dev), &probed_fs))
			break;
		if (!xnu_attached && attach_xnu_data_loop(xnu_loop_disk, sizeof(xnu_loop_disk)))
			xnu_attached = true;
		if (xnu_attached &&
		    select_nested_gpt_root(xnu_loop_disk, cfg.root,
					   root_dev, sizeof(root_dev), &probed_fs))
			break;
		if (i + 1 < max_polls)
			usleep(200000);
	}

	if (strcmp(cfg.rd_break, "mount") == 0)
		emergency_shell(&cfg, "rd.break=mount");

	if (!root_dev[0])
		emergency_shell(&cfg, "no root device resolved");

	if (!mount_real_root(root_dev, &cfg, &probed_fs))
		emergency_shell(&cfg, "failed to mount root device");

	if (strcmp(cfg.rd_break, "pre-pivot") == 0)
		emergency_shell(&cfg, "rd.break=pre-pivot");

	if (cfg.noswitch)
		emergency_shell(&cfg, "noswitch");

	do_switch_root(&cfg);
	return 1;
}

#endif /* !INIT_UNIT_TEST */
