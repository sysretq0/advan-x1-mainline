// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean userspace CCCI RPC daemon for MediaTek MT6789 (Gen95).
 *
 * Bridges /dev/ccci_rpc (CCIF0 Q1 ch=32 <-> ch=33, PORT_F_USER_HEADER +
 * mmap of SMEM_USER_MD_DRDI 64 KiB @ NC_OFF_MD_DRDI) to handle:
 *   - 0x4014 IPC_RPC_AMMS_DRDI_CONTROL: copies RF DRDI calibration slices
 *            from the read-only md1img partition (md1drdi MKIMG sub-image)
 *            into the mmap'd 64 KiB SMEM_USER_MD_DRDI buffer.
 *   - 0x400f IPC_RPC_QUERY_AP_SYS_PROPERTY: logs & returns safe defaults.
 *   - 0x4010 IPC_RPC_SAR_TABLE_IDX_QUERY_OP: returns 0.
 *   - 0x4015 IPC_RPC_SAVE_MD_CAPID: logs & returns 0 without writing flash.
 *
 * Copyright (c) 2026 sysretq0 <sysretq0@proton.me>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CCCI_CH_RPC_RX			32U
#define CCCI_CH_RPC_TX			33U
#define RPC_API_RESP_MASK		0xffff0000U
#define RPC_WIRE_PKT_MAX		3476U
#define RPC_MAX_ARGS			6U
#define DRDI_SHM_SIZE			0x10000U /* 64 KiB */

#define MKIMG_MAGIC			0x58881688U
#define MKIMG_HDR_SIZE			512U

#define IPC_RPC_QUERY_AP_SYS_PROPERTY	0x400fU
#define IPC_RPC_SAR_TABLE_IDX_QUERY_OP	0x4010U
#define IPC_RPC_AMMS_DRDI_CONTROL	0x4014U
#define IPC_RPC_SAVE_MD_CAPID		0x4015U

#define AMMS_CMD_INIT			1U
#define AMMS_CMD_DRDI_COPY		2U
#define AMMS_DRDI_VERSION		3U
#define AMMS_MAX_SETS			15U
#define AMMS_REQ_LEN			0xbcU /* 188 B */

struct __attribute__((packed)) ccci_header {
	uint32_t data[2];
	uint16_t channel;
	uint16_t seq_assert;
	uint32_t reserved;
};

struct __attribute__((packed)) rpc_msg_hdr {
	struct ccci_header ccci;
	uint32_t op_id;
	uint32_t para_num;
};

struct __attribute__((packed)) mkimg_hdr {
	uint32_t magic;
	uint32_t dsize;
	char name[32];
};

struct rpc_arg {
	uint32_t len;
	const uint8_t *buf;
};

static volatile sig_atomic_t g_stop;
static bool g_verbose;
static int g_md1img_fd = -1;
static uint64_t g_drdi_off;
static uint32_t g_drdi_size;
static uint8_t *g_drdi_shm;
static uint32_t g_set_total_num;
static bool g_drdi_copied;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint32_t align4(uint32_t v)
{
	return (v + 3U) & ~3U;
}

static uint64_t align16(uint64_t v)
{
	return (v + 15ULL) & ~15ULL;
}

static int locate_md1drdi(int fd, uint64_t *out_off, uint32_t *out_sz)
{
	uint64_t off = 0;
	int i;

	for (i = 0; i < 64; i++) {
		struct mkimg_hdr hdr;
		ssize_t n = pread(fd, &hdr, sizeof(hdr), (off_t)off);

		if (n != (ssize_t)sizeof(hdr)) {
			fprintf(stderr, "[ccci_rpcd] pread md1img @0x%llx failed: %s\n",
				(unsigned long long)off, strerror(errno));
			return -1;
		}
		if (hdr.magic != MKIMG_MAGIC)
			break;

		if (strncmp(hdr.name, "md1drdi", sizeof(hdr.name)) == 0) {
			*out_off = off + MKIMG_HDR_SIZE;
			*out_sz = hdr.dsize;
			return 0;
		}
		off += align16((uint64_t)MKIMG_HDR_SIZE + hdr.dsize);
	}

	fprintf(stderr, "[ccci_rpcd] md1drdi sub-image not found in md1img\n");
	return -1;
}

static int send_rpc_reply(int dev_fd, const struct rpc_msg_hdr *req_hdr,
			  const struct rpc_arg *resp, uint32_t resp_num)
{
	uint8_t buf[RPC_WIRE_PKT_MAX];
	struct rpc_msg_hdr *out;
	uint32_t pos = sizeof(*out);
	uint32_t i;
	ssize_t n;

	memset(buf, 0, sizeof(*out));
	for (i = 0; i < resp_num; i++) {
		uint32_t aligned = align4(resp[i].len);

		if (pos + 4U + aligned > sizeof(buf)) {
			fprintf(stderr, "[ccci_rpcd] reply overflow op=0x%04x\n",
				req_hdr->op_id);
			return -1;
		}
		memcpy(buf + pos, &resp[i].len, 4U);
		pos += 4U;
		if (resp[i].len && resp[i].buf)
			memcpy(buf + pos, resp[i].buf, resp[i].len);
		if (aligned > resp[i].len)
			memset(buf + pos + resp[i].len, 0, aligned - resp[i].len);
		pos += aligned;
	}

	out = (struct rpc_msg_hdr *)buf;
	out->ccci.data[0] = 0;
	out->ccci.data[1] = pos;
	out->ccci.channel = CCCI_CH_RPC_TX;
	out->ccci.seq_assert = 0;
	out->ccci.reserved = req_hdr->ccci.reserved;
	out->op_id = req_hdr->op_id | RPC_API_RESP_MASK;
	out->para_num = resp_num;

	n = write(dev_fd, buf, pos);
	if (n != (ssize_t)pos) {
		fprintf(stderr, "[ccci_rpcd] write reply op=0x%04x len=%u failed: %zd (%s)\n",
			req_hdr->op_id, pos, n, strerror(errno));
		return -1;
	}
	return 0;
}

static int32_t handle_amms_drdi(const uint8_t *req, uint32_t req_len,
				uint8_t resp_msg[8])
{
	uint8_t cmd, seq_id;
	uint32_t i;

	memset(resp_msg, 0, 8);
	if (req_len != AMMS_REQ_LEN) {
		fprintf(stderr, "[ccci_rpcd] AMMS_DRDI bad req len=%u (expected %u)\n",
			req_len, AMMS_REQ_LEN);
		resp_msg[0] = 0xff;
		return -1;
	}

	cmd = req[0];
	seq_id = req[1];
	resp_msg[1] = seq_id;

	if (cmd == AMMS_CMD_INIT) {
		uint8_t ver = req[4];
		uint8_t set_num = req[5];

		if (ver != AMMS_DRDI_VERSION || set_num > AMMS_MAX_SETS) {
			fprintf(stderr, "[ccci_rpcd] AMMS INIT invalid ver=%u sets=%u\n",
				ver, set_num);
			resp_msg[0] = 0xff;
			resp_msg[4] = AMMS_DRDI_VERSION;
			resp_msg[6] = 0xff;
			return -1;
		}
		resp_msg[4] = AMMS_DRDI_VERSION;
		g_set_total_num = set_num;

		for (i = 0; i < set_num; i++) {
			uint32_t off, len;

			memcpy(&off, req + 8 + i * 8, 4);
			memcpy(&len, req + 12 + i * 8, 4);
			if ((uint64_t)off + len > g_drdi_size) {
				fprintf(stderr,
					"[ccci_rpcd] AMMS INIT set[%u] out of range off=0x%x len=0x%x drdi=0x%x\n",
					i, off, len, g_drdi_size);
				resp_msg[0] = 0xfe;
				resp_msg[6] = 0xff;
				return -1;
			}
		}

		resp_msg[0] = 0;
		resp_msg[5] = g_drdi_copied ? 0xff : 0x00;
		resp_msg[6] = 0;
		if (g_verbose) {
			fprintf(stdout,
				"[ccci_rpcd] op=0x4014 AMMS_INIT seq=%u ver=%u sets=%u -> OK (copied=%d)\n",
				seq_id, ver, set_num, g_drdi_copied ? 1 : 0);
			fflush(stdout);
		}
		return 0;
	}

	if (cmd == AMMS_CMD_DRDI_COPY) {
		uint8_t last_cpy = req[4];
		static uint8_t tmp[DRDI_SHM_SIZE];
		uint32_t total_copied = 0;

		if (g_drdi_copied) {
			if (g_verbose) {
				fprintf(stdout,
					"[ccci_rpcd] op=0x4014 AMMS_COPY seq=%u already copied -> OK\n",
					seq_id);
				fflush(stdout);
			}
			resp_msg[0] = 0;
			return 0;
		}

		for (i = 0; i < g_set_total_num; i++) {
			uint32_t src_off, dst_off, copy_len;

			memcpy(&src_off, req + 8 + i * 12, 4);
			memcpy(&dst_off, req + 12 + i * 12, 4);
			memcpy(&copy_len, req + 16 + i * 12, 4);
			if (copy_len == 0)
				continue;
			if (copy_len > DRDI_SHM_SIZE ||
			    (uint64_t)dst_off + copy_len > DRDI_SHM_SIZE ||
			    (uint64_t)src_off + copy_len > g_drdi_size) {
				fprintf(stderr,
					"[ccci_rpcd] AMMS COPY set[%u] invalid src=0x%x dst=0x%x len=0x%x\n",
					i, src_off, dst_off, copy_len);
				resp_msg[0] = 0xff;
				return -1;
			}
		}

		resp_msg[4] = (last_cpy == 1U) ? 0x00 : 0xff;
		for (i = 0; i < g_set_total_num; i++) {
			uint32_t src_off, dst_off, copy_len;
			ssize_t n;

			memcpy(&src_off, req + 8 + i * 12, 4);
			memcpy(&dst_off, req + 12 + i * 12, 4);
			memcpy(&copy_len, req + 16 + i * 12, 4);
			if (copy_len == 0)
				continue;

			n = pread(g_md1img_fd, tmp, copy_len,
				  (off_t)(g_drdi_off + src_off));
			if (n != (ssize_t)copy_len) {
				fprintf(stderr,
					"[ccci_rpcd] AMMS COPY pread set[%u] failed: %zd (%s)\n",
					i, n, strerror(errno));
				resp_msg[0] = 0xff;
				return -1;
			}
			memcpy(g_drdi_shm + dst_off, tmp, copy_len);
			total_copied += copy_len;
		}
		__sync_synchronize();
		if (last_cpy == 1U)
			g_drdi_copied = true;

		resp_msg[0] = 0;
		if (g_verbose) {
			fprintf(stdout,
				"[ccci_rpcd] op=0x4014 AMMS_COPY seq=%u last=%u sets=%u copied=%u B -> OK\n",
				seq_id, last_cpy, g_set_total_num, total_copied);
			fflush(stdout);
		}
		return 0;
	}

	fprintf(stderr, "[ccci_rpcd] AMMS_DRDI unknown cmd=%u seq=%u\n", cmd, seq_id);
	resp_msg[0] = 0xff;
	return -1;
}

static void dispatch_rpc(int dev_fd, const uint8_t *pkt, uint32_t len)
{
	struct rpc_msg_hdr hdr;
	struct rpc_arg args[RPC_MAX_ARGS];
	struct rpc_arg resp[2];
	uint32_t pos = sizeof(hdr), i;
	int32_t ret_val = -1;

	if (len < sizeof(hdr)) {
		fprintf(stderr, "[ccci_rpcd] short pkt len=%u\n", len);
		return;
	}
	memcpy(&hdr, pkt, sizeof(hdr));
	if (hdr.para_num > RPC_MAX_ARGS) {
		fprintf(stderr, "[ccci_rpcd] bad para_num=%u op=0x%04x\n",
			hdr.para_num, hdr.op_id);
		return;
	}

	for (i = 0; i < hdr.para_num; i++) {
		uint32_t alen, aligned;

		if (pos + 4U > len)
			return;
		memcpy(&alen, pkt + pos, 4U);
		pos += 4U;
		aligned = align4(alen);
		if (alen > len || pos + aligned > len)
			return;
		args[i].len = alen;
		args[i].buf = pkt + pos;
		pos += aligned;
	}

	switch (hdr.op_id) {
	case IPC_RPC_AMMS_DRDI_CONTROL: {
		uint8_t resp_msg[8];

		if (hdr.para_num >= 1)
			ret_val = handle_amms_drdi(args[0].buf, args[0].len, resp_msg);
		else
			memset(resp_msg, 0xff, sizeof(resp_msg));
		resp[0].len = sizeof(ret_val);
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(resp_msg);
		resp[1].buf = resp_msg;
		send_rpc_reply(dev_fd, &hdr, resp, 2);
		return;
	}

	case IPC_RPC_QUERY_AP_SYS_PROPERTY: {
		char key[96] = {0};
		char val[92] = {0};
		uint32_t vlen;

		if (hdr.para_num >= 1 && args[0].len > 0) {
			uint32_t klen = args[0].len < sizeof(key) - 1 ?
					args[0].len : (uint32_t)(sizeof(key) - 1);
			memcpy(key, args[0].buf, klen);
			key[klen] = '\0';
		}
		if (!strcmp(key, "persist.vendor.radio.simswitch")) {
			snprintf(val, sizeof(val), "1");
			ret_val = 0;
		} else {
			ret_val = -1;
		}
		vlen = (uint32_t)strlen(val) + 1U;
		if (g_verbose) {
			fprintf(stdout, "[ccci_rpcd] op=0x400f SYS_PROP key='%s' -> val='%s' ret=%d\n",
				key, val, ret_val);
			fflush(stdout);
		}
		resp[0].len = sizeof(ret_val);
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = vlen;
		resp[1].buf = (const uint8_t *)val;
		send_rpc_reply(dev_fd, &hdr, resp, 2);
		return;
	}

	case IPC_RPC_SAR_TABLE_IDX_QUERY_OP: {
		uint32_t sar_idx = 0;

		ret_val = 0;
		if (g_verbose) {
			fprintf(stdout, "[ccci_rpcd] op=0x4010 SAR_TABLE_IDX -> %u (ret=0)\n",
				sar_idx);
			fflush(stdout);
		}
		resp[0].len = sizeof(ret_val);
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(sar_idx);
		resp[1].buf = (const uint8_t *)&sar_idx;
		send_rpc_reply(dev_fd, &hdr, resp, 2);
		return;
	}

	case IPC_RPC_SAVE_MD_CAPID: {
		uint32_t capid = 0, aac = 0, out_st = 0;

		if (hdr.para_num >= 1 && args[0].len >= 8) {
			memcpy(&capid, args[0].buf, 4);
			memcpy(&aac, args[0].buf + 4, 4);
		}
		ret_val = 0;
		if (g_verbose) {
			fprintf(stdout,
				"[ccci_rpcd] op=0x4015 SAVE_MD_CAPID capid=0x%x aac=0x%x -> ignored RO (ret=0)\n",
				capid, aac);
			fflush(stdout);
		}
		resp[0].len = sizeof(ret_val);
		resp[0].buf = (const uint8_t *)&ret_val;
		resp[1].len = sizeof(out_st);
		resp[1].buf = (const uint8_t *)&out_st;
		send_rpc_reply(dev_fd, &hdr, resp, 2);
		return;
	}

	default:
		fprintf(stderr, "[ccci_rpcd] unhandled op=0x%04x para=%u len=%u\n",
			hdr.op_id, hdr.para_num, len);
		return;
	}
}

int main(int argc, char **argv)
{
	const char *dev_path = "/dev/ccci_rpc";
	const char *md1img_path = "/dev/disk/by-partlabel/md1img_b";
	const char *ready_file = NULL;
	uint8_t pkt[RPC_WIRE_PKT_MAX];
	int dev_fd, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			dev_path = argv[++i];
		} else if (!strcmp(argv[i], "--md1img") && i + 1 < argc) {
			md1img_path = argv[++i];
		} else if (!strcmp(argv[i], "--ready-file") && i + 1 < argc) {
			ready_file = argv[++i];
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			g_verbose = true;
		} else {
			fprintf(stderr,
				"Usage: %s [--dev /dev/ccci_rpc] [--md1img path] [--ready-file path] [-v|--verbose]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	g_md1img_fd = open(md1img_path, O_RDONLY);
	if (g_md1img_fd < 0) {
		fprintf(stderr, "[ccci_rpcd] open(%s, O_RDONLY) failed: %s\n",
			md1img_path, strerror(errno));
		return 1;
	}

	if (locate_md1drdi(g_md1img_fd, &g_drdi_off, &g_drdi_size) < 0) {
		close(g_md1img_fd);
		return 1;
	}

	dev_fd = open(dev_path, O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "[ccci_rpcd] open(%s) failed: %s\n",
			dev_path, strerror(errno));
		close(g_md1img_fd);
		return 1;
	}

	g_drdi_shm = mmap(NULL, DRDI_SHM_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED, dev_fd, 0);
	if (g_drdi_shm == MAP_FAILED) {
		fprintf(stderr, "[ccci_rpcd] mmap(%s, 0x%x) failed: %s\n",
			dev_path, DRDI_SHM_SIZE, strerror(errno));
		close(dev_fd);
		close(g_md1img_fd);
		return 1;
	}

	fprintf(stdout,
		"[ccci_rpcd] listening on %s (md1img=%s md1drdi@0x%llx sz=%u B, shm=64K)\n",
		dev_path, md1img_path, (unsigned long long)g_drdi_off, g_drdi_size);
	fflush(stdout);

	if (ready_file) {
		int rfd = open(ready_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (rfd >= 0) {
			(void)write(rfd, "ready\n", 6);
			close(rfd);
		}
	}

	while (!g_stop) {
		struct pollfd pfd = { .fd = dev_fd, .events = POLLIN };
		ssize_t n;
		int pr;

		pr = poll(&pfd, 1, 500);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[ccci_rpcd] poll error: %s\n", strerror(errno));
			break;
		}
		if (pr == 0 || !(pfd.revents & POLLIN))
			continue;

		n = read(dev_fd, pkt, sizeof(pkt));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "[ccci_rpcd] read error: %s\n", strerror(errno));
			break;
		}
		dispatch_rpc(dev_fd, pkt, (uint32_t)n);
	}

	munmap(g_drdi_shm, DRDI_SHM_SIZE);
	close(dev_fd);
	close(g_md1img_fd);
	return 0;
}
