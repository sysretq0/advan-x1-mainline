/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mtk_volte_rtp: Real-time VoLTE RTP audio engine for Advan X1 (MT6789).
 *
 * Bridges bidirectional 20 ms RTP frames on the IMS PDN (IPv4 or IPv6,
 * optionally SO_BINDTODEVICE to wwan1) to PipeWire/ALSA raw s16le PCM
 * via stdin/stdout or spawned pw-cat/aplay/arecord streams.
 *
 * Supported codecs:
 *   - AMR-WB (16 kHz, RFC 4867 octet-aligned & bandwidth-efficient)
 *     via dlopen("libopencore-amrwb.so.0") + dlopen("libvo-amrwbenc.so.0")
 *   - AMR-NB (8 kHz, RFC 4867 octet-aligned & bandwidth-efficient)
 *     via dlopen("libopencore-amrnb.so.0")
 *   - PCMU (G.711 u-law, 8 kHz) — built-in
 *   - PCMA (G.711 A-law, 8 kHz) — built-in
 *   - RFC 4733 telephone-event DTMF TX
 *
 * Usage:
 *   mtk_volte_rtp --local-ip <ip> --local-port <port> \
 *                 --remote-ip <ip> --remote-port <port> \
 *                 --codec <amr-wb|amr-nb|pcmu|pcma> --pt <payload_type> \
 *                 [--octet-align 1|0] [--dtmf-pt <pt>] [--ifname wwan1] \
 *                 [--uid <desktop_uid>]
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ---- G.711 u-law / A-law ---- */

static uint8_t linear_to_ulaw(int16_t pcm)
{
	int sign = (pcm < 0) ? 0x80 : 0;
	int mag = (pcm < 0) ? -pcm : pcm;
	int exp = 7;

	if (mag > 32635)
		mag = 32635;
	mag += 0x84;
	for (int mask = 0x4000; exp > 0 && !(mag & mask); exp--, mask >>= 1)
		;
	int mant = (mag >> (exp + 3)) & 0x0f;
	return (uint8_t)~(sign | (exp << 4) | mant);
}

static int16_t ulaw_to_linear(uint8_t u)
{
	u = (uint8_t)~u;
	int t = ((u & 0x0f) << 3) + 0x84;
	t <<= ((u & 0x70) >> 4);
	return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

static uint8_t linear_to_alaw(int16_t pcm)
{
	int sign = (pcm < 0) ? 0x00 : 0x80;
	int mag = (pcm < 0) ? -(pcm + 1) : pcm;
	int exp, mant;

	if (mag > 32767)
		mag = 32767;
	if (mag >= 256) {
		exp = 7;
		for (int mask = 0x4000; exp > 1 && !(mag & mask); exp--, mask >>= 1)
			;
		mant = (mag >> (exp + 3)) & 0x0f;
	} else {
		exp = 0;
		mant = (mag >> 4) & 0x0f;
	}
	return (uint8_t)((sign | (exp << 4) | mant) ^ 0x55);
}

static int16_t alaw_to_linear(uint8_t a)
{
	a ^= 0x55;
	int t = (a & 0x0f) << 4;
	int seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? (int16_t)t : (int16_t)-t;
}

/* ---- Dynamic AMR-WB / AMR-NB codec bindings ---- */

typedef void *(*fn_amrwb_dec_init)(void);
typedef void (*fn_amrwb_dec_exit)(void *);
typedef void (*fn_amrwb_dec_decode)(void *, const unsigned char *, short *, int);

typedef void *(*fn_amrwb_enc_init)(void);
typedef void (*fn_amrwb_enc_exit)(void *);
typedef int (*fn_amrwb_enc_encode)(void *, int, const short *, unsigned char *, int);

typedef void *(*fn_amrnb_dec_init)(void);
typedef void (*fn_amrnb_dec_exit)(void *);
typedef void (*fn_amrnb_dec_decode)(void *, const unsigned char *, short *, int);

typedef void *(*fn_amrnb_enc_init)(int);
typedef void (*fn_amrnb_enc_exit)(void *);
typedef int (*fn_amrnb_enc_encode)(void *, int, const short *, unsigned char *, int);

enum codec_id {
	CODEC_PCMU,
	CODEC_PCMA,
	CODEC_AMRNB,
	CODEC_AMRWB,
};

struct codec_ctx {
	enum codec_id id;
	int rate;
	int frame_samples;
	int octet_align;
	void *dl_dec;
	void *dl_enc;
	void *dec;
	void *enc;
	fn_amrwb_dec_decode wb_dec;
	fn_amrwb_enc_encode wb_enc;
	fn_amrwb_dec_exit wb_dec_exit;
	fn_amrwb_enc_exit wb_enc_exit;
	fn_amrnb_dec_decode nb_dec;
	fn_amrnb_enc_encode nb_enc;
	fn_amrnb_dec_exit nb_dec_exit;
	fn_amrnb_enc_exit nb_enc_exit;
};

/* AMR-WB frame bit counts for FT 0..8 (3GPP TS 26.201) */
static const uint16_t amrwb_bits[16] = {
	132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 0, 0, 0, 0, 0, 0
};
/* AMR-NB frame bit counts for FT 0..8 (3GPP TS 26.101) */
static const uint16_t amrnb_bits[16] = {
	95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0
};

static int bit_get(const uint8_t *buf, size_t total_bits, size_t pos)
{
	if (pos >= total_bits)
		return 0;
	return (buf[pos >> 3] >> (7 - (pos & 7))) & 1;
}

static void bit_put(uint8_t *buf, size_t pos, int v)
{
	if (v)
		buf[pos >> 3] |= (uint8_t)(1u << (7 - (pos & 7)));
}

/* Convert single-frame RFC 4867 RTP payload -> 3GPP IF1/storage frame for opencore-amr */
static int rfc4867_to_if1(const uint8_t *rtp, size_t rtp_len, int octet_align,
			  const uint16_t *ft_bits, uint8_t *out, size_t out_cap)
{
	if (rtp_len < 2 || out_cap < 64)
		return -1;
	if (octet_align) {
		/* Byte 0: CMR(4)|0000, Byte 1: F(1)|FT(4)|Q(1)|00, Byte 2+: speech */
		uint8_t toc = rtp[1];
		uint8_t ft = (toc >> 3) & 0x0f;
		uint8_t q = (toc >> 2) & 0x01;
		size_t speech_bytes = (ft_bits[ft] + 7) / 8;
		if (2 + speech_bytes > rtp_len || 1 + speech_bytes > out_cap)
			return -1;
		out[0] = (uint8_t)((ft << 3) | (q << 2));
		memcpy(out + 1, rtp + 2, speech_bytes);
		return (int)(1 + speech_bytes);
	} else {
		/* Bandwidth-efficient: CMR(4) + F(1) + FT(4) + Q(1) + speech bits */
		size_t total_bits = rtp_len * 8;
		if (total_bits < 10)
			return -1;
		uint8_t ft = 0;
		for (int i = 0; i < 4; i++)
			ft = (uint8_t)((ft << 1) | bit_get(rtp, total_bits, 5 + (size_t)i));
		uint8_t q = (uint8_t)bit_get(rtp, total_bits, 9);
		size_t nbits = ft_bits[ft];
		size_t speech_bytes = (nbits + 7) / 8;
		if (10 + nbits > total_bits || 1 + speech_bytes > out_cap)
			return -1;
		memset(out, 0, 1 + speech_bytes);
		out[0] = (uint8_t)((ft << 3) | (q << 2));
		for (size_t i = 0; i < nbits; i++)
			bit_put(out + 1, i, bit_get(rtp, total_bits, 10 + i));
		return (int)(1 + speech_bytes);
	}
}

/* Convert 3GPP IF1/storage frame from encoder -> single-frame RFC 4867 RTP payload */
static int if1_to_rfc4867(const uint8_t *in, int in_len, int octet_align,
			  const uint16_t *ft_bits, uint8_t *rtp, size_t rtp_cap)
{
	if (in_len < 1 || rtp_cap < 64)
		return -1;
	uint8_t ft = (in[0] >> 3) & 0x0f;
	uint8_t q = (in[0] >> 2) & 0x01;
	size_t nbits = ft_bits[ft];
	size_t speech_bytes = (nbits + 7) / 8;
	if ((size_t)in_len < 1 + speech_bytes)
		return -1;
	if (octet_align) {
		if (2 + speech_bytes > rtp_cap)
			return -1;
		rtp[0] = 0xf0; /* CMR = 15 (no mode request) */
		rtp[1] = (uint8_t)((ft << 3) | (q << 2));
		memcpy(rtp + 2, in + 1, speech_bytes);
		return (int)(2 + speech_bytes);
	} else {
		size_t out_bytes = (10 + nbits + 7) / 8;
		if (out_bytes > rtp_cap)
			return -1;
		memset(rtp, 0, out_bytes);
		/* CMR = 15 (1111), F = 0 */
		for (int i = 0; i < 4; i++)
			bit_put(rtp, (size_t)i, 1);
		bit_put(rtp, 4, 0);
		for (int i = 0; i < 4; i++)
			bit_put(rtp, 5 + (size_t)i, (ft >> (3 - i)) & 1);
		bit_put(rtp, 9, q);
		for (size_t i = 0; i < nbits; i++)
			bit_put(rtp, 10 + i, bit_get(in + 1, nbits, i));
		return (int)out_bytes;
	}
}

static int codec_init(struct codec_ctx *c, const char *name, int octet_align)
{
	memset(c, 0, sizeof(*c));
	c->octet_align = octet_align;

	if (strcasecmp(name, "amr-wb") == 0) {
		c->id = CODEC_AMRWB;
		c->rate = 16000;
		c->frame_samples = 320;
		c->dl_dec = dlopen("libopencore-amrwb.so.0", RTLD_LAZY);
		c->dl_enc = dlopen("libvo-amrwbenc.so.0", RTLD_LAZY);
		if (!c->dl_dec || !c->dl_enc) {
			fprintf(stderr, "[mtk_volte_rtp] AMR-WB libs missing: %s\n", dlerror());
			return -1;
		}
		fn_amrwb_dec_init di = (fn_amrwb_dec_init)dlsym(c->dl_dec, "D_IF_init");
		c->wb_dec = (fn_amrwb_dec_decode)dlsym(c->dl_dec, "D_IF_decode");
		c->wb_dec_exit = (fn_amrwb_dec_exit)dlsym(c->dl_dec, "D_IF_exit");
		fn_amrwb_enc_init ei = (fn_amrwb_enc_init)dlsym(c->dl_enc, "E_IF_init");
		c->wb_enc = (fn_amrwb_enc_encode)dlsym(c->dl_enc, "E_IF_encode");
		c->wb_enc_exit = (fn_amrwb_enc_exit)dlsym(c->dl_enc, "E_IF_exit");
		if (!di || !c->wb_dec || !c->wb_dec_exit || !ei || !c->wb_enc || !c->wb_enc_exit)
			return -1;
		c->dec = di();
		c->enc = ei();
		return (c->dec && c->enc) ? 0 : -1;
	} else if (strcasecmp(name, "amr-nb") == 0 || strcasecmp(name, "amr") == 0) {
		c->id = CODEC_AMRNB;
		c->rate = 8000;
		c->frame_samples = 160;
		c->dl_dec = dlopen("libopencore-amrnb.so.0", RTLD_LAZY);
		if (!c->dl_dec) {
			fprintf(stderr, "[mtk_volte_rtp] AMR-NB lib missing: %s\n", dlerror());
			return -1;
		}
		fn_amrnb_dec_init di = (fn_amrnb_dec_init)dlsym(c->dl_dec, "Decoder_Interface_init");
		c->nb_dec = (fn_amrnb_dec_decode)dlsym(c->dl_dec, "Decoder_Interface_Decode");
		c->nb_dec_exit = (fn_amrnb_dec_exit)dlsym(c->dl_dec, "Decoder_Interface_exit");
		fn_amrnb_enc_init ei = (fn_amrnb_enc_init)dlsym(c->dl_dec, "Encoder_Interface_init");
		c->nb_enc = (fn_amrnb_enc_encode)dlsym(c->dl_dec, "Encoder_Interface_Encode");
		c->nb_enc_exit = (fn_amrnb_enc_exit)dlsym(c->dl_dec, "Encoder_Interface_exit");
		if (!di || !c->nb_dec || !c->nb_dec_exit || !ei || !c->nb_enc || !c->nb_enc_exit)
			return -1;
		c->dec = di();
		c->enc = ei(0);
		return (c->dec && c->enc) ? 0 : -1;
	} else if (strcasecmp(name, "pcma") == 0) {
		c->id = CODEC_PCMA;
		c->rate = 8000;
		c->frame_samples = 160;
		return 0;
	} else {
		c->id = CODEC_PCMU;
		c->rate = 8000;
		c->frame_samples = 160;
		return 0;
	}
}

static void codec_close(struct codec_ctx *c)
{
	if (c->id == CODEC_AMRWB) {
		if (c->dec && c->wb_dec_exit)
			c->wb_dec_exit(c->dec);
		if (c->enc && c->wb_enc_exit)
			c->wb_enc_exit(c->enc);
	} else if (c->id == CODEC_AMRNB) {
		if (c->dec && c->nb_dec_exit)
			c->nb_dec_exit(c->dec);
		if (c->enc && c->nb_enc_exit)
			c->nb_enc_exit(c->enc);
	}
	if (c->dl_enc)
		dlclose(c->dl_enc);
	if (c->dl_dec)
		dlclose(c->dl_dec);
}

static int codec_encode(struct codec_ctx *c, const int16_t *pcm, uint8_t *out, size_t cap)
{
	uint8_t tmp[64];

	switch (c->id) {
	case CODEC_PCMU:
		if (cap < 160)
			return -1;
		for (int i = 0; i < 160; i++)
			out[i] = linear_to_ulaw(pcm[i]);
		return 160;
	case CODEC_PCMA:
		if (cap < 160)
			return -1;
		for (int i = 0; i < 160; i++)
			out[i] = linear_to_alaw(pcm[i]);
		return 160;
	case CODEC_AMRWB: {
		/* Mode 2 = 12.65 kbps (most universally supported VoLTE AMR-WB mode) */
		int n = c->wb_enc(c->enc, 2, pcm, tmp, 0);
		return if1_to_rfc4867(tmp, n, c->octet_align, amrwb_bits, out, cap);
	}
	case CODEC_AMRNB: {
		/* Mode 7 = MR122 (12.2 kbps) */
		int n = c->nb_enc(c->enc, 7, pcm, tmp, 0);
		return if1_to_rfc4867(tmp, n, c->octet_align, amrnb_bits, out, cap);
	}
	}
	return -1;
}

static int codec_decode(struct codec_ctx *c, const uint8_t *in, size_t len, int16_t *pcm)
{
	uint8_t tmp[64];

	switch (c->id) {
	case CODEC_PCMU: {
		size_t n = len < 160 ? len : 160;
		for (size_t i = 0; i < n; i++)
			pcm[i] = ulaw_to_linear(in[i]);
		return (int)n;
	}
	case CODEC_PCMA: {
		size_t n = len < 160 ? len : 160;
		for (size_t i = 0; i < n; i++)
			pcm[i] = alaw_to_linear(in[i]);
		return (int)n;
	}
	case CODEC_AMRWB: {
		int n = rfc4867_to_if1(in, len, c->octet_align, amrwb_bits, tmp, sizeof(tmp));
		if (n < 1)
			return -1;
		c->wb_dec(c->dec, tmp, pcm, 0);
		return 320;
	}
	case CODEC_AMRNB: {
		int n = rfc4867_to_if1(in, len, c->octet_align, amrnb_bits, tmp, sizeof(tmp));
		if (n < 1)
			return -1;
		c->nb_dec(c->dec, tmp, pcm, 0);
		return 160;
	}
	}
	return -1;
}

/* ---- PipeWire audio subprocess helper ---- */

static pid_t spawn_pw_cat(bool playback, int rate, int uid, int *pipe_fd)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		if (playback) {
			dup2(fds[0], STDIN_FILENO);
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0) {
				dup2(dn, STDOUT_FILENO);
				close(dn);
			}
		} else {
			dup2(fds[1], STDOUT_FILENO);
			int dn = open("/dev/null", O_RDONLY);
			if (dn >= 0) {
				dup2(dn, STDIN_FILENO);
				close(dn);
			}
		}
		close(fds[0]);
		close(fds[1]);

		if (uid > 0 && geteuid() == 0) {
			char xdg[64];
			snprintf(xdg, sizeof(xdg), "/run/user/%d", uid);
			setenv("XDG_RUNTIME_DIR", xdg, 1);
			if (setgid((gid_t)uid) != 0)
				_exit(126);
			if (setuid((uid_t)uid) != 0)
				_exit(126);
		}
		char rate_s[16];
		snprintf(rate_s, sizeof(rate_s), "%d", rate);
		execlp("pw-cat", "pw-cat",
		       playback ? "--playback" : "--record",
		       "--raw", "--format", "s16",
		       "--rate", rate_s, "--channels", "1",
		       "--latency", "20ms",
		       "--media-role", "Communication",
		       "-", (char *)NULL);
		_exit(127);
	}
	if (playback) {
		close(fds[0]);
		*pipe_fd = fds[1];
	} else {
		close(fds[1]);
		*pipe_fd = fds[0];
	}
	return pid;
}

static ssize_t read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;
	while (got < n && !g_stop) {
		ssize_t r = read(fd, p + got, n - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return (ssize_t)got;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

struct rtp_state {
	int sock;
	struct sockaddr_storage remote;
	socklen_t remote_len;
	struct codec_ctx codec;
	uint8_t pt;
	uint8_t dtmf_pt;
	uint32_t ssrc;
	uint16_t seq;
	uint32_t ts;
	int play_fd;
	int rec_fd;
	volatile int muted;
	pthread_mutex_t tx_lock;
};

static void send_rtp(struct rtp_state *st, uint8_t pt, bool marker,
		     uint32_t ts, const uint8_t *payload, size_t plen)
{
	uint8_t pkt[512];
	if (12 + plen > sizeof(pkt))
		return;
	pthread_mutex_lock(&st->tx_lock);
	uint16_t seq = st->seq++;
	pkt[0] = 0x80;
	pkt[1] = (uint8_t)((marker ? 0x80 : 0x00) | (pt & 0x7f));
	pkt[2] = (uint8_t)(seq >> 8);
	pkt[3] = (uint8_t)(seq & 0xff);
	pkt[4] = (uint8_t)(ts >> 24);
	pkt[5] = (uint8_t)(ts >> 16);
	pkt[6] = (uint8_t)(ts >> 8);
	pkt[7] = (uint8_t)(ts & 0xff);
	pkt[8] = (uint8_t)(st->ssrc >> 24);
	pkt[9] = (uint8_t)(st->ssrc >> 16);
	pkt[10] = (uint8_t)(st->ssrc >> 8);
	pkt[11] = (uint8_t)(st->ssrc & 0xff);
	memcpy(pkt + 12, payload, plen);
	(void)sendto(st->sock, pkt, 12 + plen, 0,
		     (struct sockaddr *)&st->remote, st->remote_len);
	pthread_mutex_unlock(&st->tx_lock);
}

static void send_dtmf(struct rtp_state *st, char ch)
{
	int ev = -1;
	if (ch >= '0' && ch <= '9')
		ev = ch - '0';
	else if (ch == '*')
		ev = 10;
	else if (ch == '#')
		ev = 11;
	else if (ch >= 'A' && ch <= 'D')
		ev = 12 + (ch - 'A');
	if (ev < 0 || !st->dtmf_pt)
		return;

	uint32_t ts = st->ts;
	uint16_t step = (uint16_t)(st->codec.frame_samples);
	for (int i = 0; i < 6; i++) {
		uint8_t p[4];
		uint16_t dur = (uint16_t)((i + 1) * step);
		bool end = (i >= 4);
		p[0] = (uint8_t)ev;
		p[1] = (uint8_t)((end ? 0x80 : 0x00) | 10);
		p[2] = (uint8_t)(dur >> 8);
		p[3] = (uint8_t)(dur & 0xff);
		send_rtp(st, st->dtmf_pt, i == 0, ts, p, 4);
		usleep(20000);
	}
}

static void *tx_thread(void *arg)
{
	struct rtp_state *st = arg;
	int16_t pcm[320];
	uint8_t enc[256];
	size_t frame_bytes = (size_t)st->codec.frame_samples * sizeof(int16_t);
	bool first = true;

	while (!g_stop) {
		ssize_t r = read_full(st->rec_fd, pcm, frame_bytes);
		if (r != (ssize_t)frame_bytes)
			break;
		if (st->muted)
			memset(pcm, 0, frame_bytes);
		int n = codec_encode(&st->codec, pcm, enc, sizeof(enc));
		if (n > 0) {
			send_rtp(st, st->pt, first, st->ts, enc, (size_t)n);
			first = false;
		}
		st->ts += (uint32_t)st->codec.frame_samples;
	}
	return NULL;
}

static void *ctl_thread(void *arg)
{
	struct rtp_state *st = arg;
	char line[128];

	while (!g_stop && fgets(line, sizeof(line), stdin)) {
		if (strncmp(line, "DTMF ", 5) == 0 && line[5]) {
			send_dtmf(st, line[5]);
		} else if (strncmp(line, "MUTE ", 5) == 0) {
			st->muted = (line[5] == '1');
		} else if (strncmp(line, "QUIT", 4) == 0) {
			g_stop = 1;
			break;
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *local_ip = NULL, *remote_ip = NULL, *codec_name = "pcmu", *ifname = NULL;
	int local_port = 0, remote_port = 0, pt = 0, dtmf_pt = 101, octet_align = 1, uid = 1000;

	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--local-ip"))
			local_ip = argv[i + 1];
		else if (!strcmp(argv[i], "--local-port"))
			local_port = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--remote-ip"))
			remote_ip = argv[i + 1];
		else if (!strcmp(argv[i], "--remote-port"))
			remote_port = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--codec"))
			codec_name = argv[i + 1];
		else if (!strcmp(argv[i], "--pt"))
			pt = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--dtmf-pt"))
			dtmf_pt = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--octet-align"))
			octet_align = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--ifname"))
			ifname = argv[i + 1];
		else if (!strcmp(argv[i], "--uid"))
			uid = atoi(argv[i + 1]);
	}

	if (!local_ip || !remote_ip || local_port <= 0 || remote_port <= 0) {
		fprintf(stderr, "Usage: %s --local-ip IP --local-port P --remote-ip IP --remote-port P --codec NAME --pt PT\n", argv[0]);
		return 2;
	}

	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGPIPE, SIG_IGN);

	struct rtp_state st;
	memset(&st, 0, sizeof(st));
	pthread_mutex_init(&st.tx_lock, NULL);
	st.pt = (uint8_t)pt;
	st.dtmf_pt = (uint8_t)dtmf_pt;
	srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
	st.ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
	st.seq = (uint16_t)rand();
	st.ts = (uint32_t)rand();

	if (codec_init(&st.codec, codec_name, octet_align) < 0) {
		fprintf(stderr, "[mtk_volte_rtp] codec init failed for %s\n", codec_name);
		return 1;
	}

	int af = strchr(local_ip, ':') ? AF_INET6 : AF_INET;
	st.sock = socket(af, SOCK_DGRAM, 0);
	if (st.sock < 0) {
		perror("socket");
		return 1;
	}
	int one = 1;
	setsockopt(st.sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ifname && *ifname) {
		if (setsockopt(st.sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname) + 1) < 0)
			perror("SO_BINDTODEVICE");
	}

	struct sockaddr_storage laddr;
	memset(&laddr, 0, sizeof(laddr));
	socklen_t llen;
	if (af == AF_INET6) {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&laddr;
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons((uint16_t)local_port);
		inet_pton(AF_INET6, local_ip, &s6->sin6_addr);
		llen = sizeof(*s6);

		struct sockaddr_in6 *r6 = (struct sockaddr_in6 *)&st.remote;
		r6->sin6_family = AF_INET6;
		r6->sin6_port = htons((uint16_t)remote_port);
		inet_pton(AF_INET6, remote_ip, &r6->sin6_addr);
		st.remote_len = sizeof(*r6);
	} else {
		struct sockaddr_in *s4 = (struct sockaddr_in *)&laddr;
		s4->sin_family = AF_INET;
		s4->sin_port = htons((uint16_t)local_port);
		inet_pton(AF_INET, local_ip, &s4->sin_addr);
		llen = sizeof(*s4);

		struct sockaddr_in *r4 = (struct sockaddr_in *)&st.remote;
		r4->sin_family = AF_INET;
		r4->sin_port = htons((uint16_t)remote_port);
		inet_pton(AF_INET, remote_ip, &r4->sin_addr);
		st.remote_len = sizeof(*r4);
	}

	if (bind(st.sock, (struct sockaddr *)&laddr, llen) < 0) {
		perror("bind");
		return 1;
	}

	pid_t play_pid = spawn_pw_cat(true, st.codec.rate, uid, &st.play_fd);
	pid_t rec_pid = spawn_pw_cat(false, st.codec.rate, uid, &st.rec_fd);
	if (play_pid < 0 || rec_pid < 0) {
		fprintf(stderr, "[mtk_volte_rtp] failed to spawn pw-cat\n");
		return 1;
	}

	fprintf(stderr, "[mtk_volte_rtp] started %s/%dHz pt=%d oa=%d %s:%d <-> %s:%d (dev=%s)\n",
		codec_name, st.codec.rate, pt, octet_align,
		local_ip, local_port, remote_ip, remote_port,
		ifname ? ifname : "-");

	pthread_t tx_tid, ctl_tid;
	pthread_create(&tx_tid, NULL, tx_thread, &st);
	pthread_create(&ctl_tid, NULL, ctl_thread, &st);

	uint8_t pkt[1500];
	int16_t pcm[320];
	struct pollfd pfd = { .fd = st.sock, .events = POLLIN };

	while (!g_stop) {
		int pr = poll(&pfd, 1, 200);
		if (pr <= 0)
			continue;
		ssize_t n = recv(st.sock, pkt, sizeof(pkt), 0);
		if (n < 12 || (pkt[0] >> 6) != 2)
			continue;
		size_t cc = pkt[0] & 0x0f;
		size_t hdr = 12 + cc * 4;
		if (pkt[0] & 0x10) {
			if ((size_t)n < hdr + 4)
				continue;
			uint16_t extlen = (uint16_t)((pkt[hdr + 2] << 8) | pkt[hdr + 3]);
			hdr += 4 + (size_t)extlen * 4;
		}
		if ((size_t)n <= hdr)
			continue;
		size_t plen = (size_t)n - hdr;
		if (pkt[0] & 0x20) {
			uint8_t pad = pkt[n - 1];
			if (pad >= plen)
				continue;
			plen -= pad;
		}
		uint8_t rpt = pkt[1] & 0x7f;
		if (rpt != st.pt)
			continue;
		int samples = codec_decode(&st.codec, pkt + hdr, plen, pcm);
		if (samples > 0) {
			ssize_t wr = write(st.play_fd, pcm, (size_t)samples * sizeof(int16_t));
			(void)wr;
		}
	}

	g_stop = 1;
	close(st.rec_fd);
	close(st.play_fd);
	close(st.sock);
	kill(play_pid, SIGTERM);
	kill(rec_pid, SIGTERM);
	pthread_join(tx_tid, NULL);
	pthread_cancel(ctl_tid);
	pthread_join(ctl_tid, NULL);
	waitpid(play_pid, NULL, 0);
	waitpid(rec_pid, NULL, 0);
	codec_close(&st.codec);
	return 0;
}
