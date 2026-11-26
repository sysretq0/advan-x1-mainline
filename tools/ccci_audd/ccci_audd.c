/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6789 Gen95 CCCI Voice Speech Control Daemon (ccci_audd).
 *
 * Bridges ModemManager Voice call state over system D-Bus to:
 *   1. ALSA UCM2 Voice Call routing (mt6789-mt6366, Lineout Volume <= 8)
 *   2. Hostless_Speech DPCM PCM2 crossbar (/dev/snd/pcmC<card>D<pcm>{p,c})
 *   3. /dev/ccci_aud (CCIF0 Q0 ch=4 RX / ch=5 TX, PORT_F_USER_HEADER)
 *
 * Wire format verified directly against stock MT6789 Gen95 AudioHAL
 * (SpeechDriverNormal / SpeechMessengerNormal):
 *   - 16-byte mailbox TX:
 *       data[0]  = 0xffffffff
 *       data[1]  = (msg_id << 16) | param16
 *       channel  = 5 (CCCI_CH_PCM_TX)
 *       reserved = param32
 *   - Variable-length payload TX (16 + 6 + data_size bytes):
 *       data[0]  = 0
 *       data[1]  = data_size + 6
 *       channel  = 5 (CCCI_CH_PCM_TX)
 *       reserved = (msg_id << 16) | (data_size + 6)
 *       payload  = [u16 0xa2a2, u16 data_type, u16 data_size, data...]
 *   - Key Gen95 message IDs:
 *       MSG_A2M_SPH_ON          = 0x2f20 (payload data_type=0x19, 128 B sph_info)
 *       MSG_A2M_SPH_OFF         = 0x2f21 (16 B mailbox, param16=0, param32=0)
 *       MSG_A2M_SET_SPEECH_MODE = 0x2f2b (payload data_type=0x19, 128 B sph_info)
 *       MSG_A2M_MD_ALIVE_ACK    = 0x2fa0 (16 B mailbox, param16=0, param32=0)
 *       MSG_M2A_SPH_ON_ACK      = 0xaf20
 *       MSG_M2A_SPH_OFF_ACK     = 0xaf21
 *       MSG_M2A_MD_ALIVE        = 0xaf70
 *       MSG_M2A_EM_DATA_REQUEST = 0xafa0 (replied with 0x2fa0)
 *
 * Copyright (c) 2026 sysretq0 <sysretq0@proton.me>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#include <gio/gio.h>
#include <glib-unix.h>

#define CCCI_CH_PCM_TX			5U
#define CCCI_MAILBOX_MAGIC		0xffffffffU
#define CCCI_AUD_PAYLOAD_SYNC_AP	0xa2a2U
#define CCCI_AUD_PAYLOAD_SYNC_MD	0x1234U
#define CCCI_AUD_MAX_PKT		3476U

#define SHARE_BUFF_DATA_TYPE_SPH_INFO	0x0019U

#define MSG_A2M_SPH_ON			0x2f20U
#define MSG_A2M_SPH_OFF			0x2f21U
#define MSG_A2M_SET_SPEECH_MODE		0x2f2bU
#define MSG_A2M_MD_ALIVE_ACK		0x2fa0U

#define MSG_M2A_SPH_ON_ACK		0xaf20U
#define MSG_M2A_SPH_OFF_ACK		0xaf21U
#define MSG_M2A_SET_SPEECH_MODE_ACK	0xaf2bU
#define MSG_M2A_MD_ALIVE		0xaf70U
#define MSG_M2A_EM_DATA_REQUEST		0xafa0U

#define MM_DBUS_SERVICE			"org.freedesktop.ModemManager1"
#define MM_DBUS_PATH			"/org/freedesktop/ModemManager1"
#define MM_DBUS_IFACE_VOICE		"org.freedesktop.ModemManager1.Modem.Voice"
#define MM_DBUS_IFACE_CALL		"org.freedesktop.ModemManager1.Call"

/* ModemManager MMCallState values */
#define MM_CALL_STATE_DIALING		1
#define MM_CALL_STATE_RINGING_OUT	2
#define MM_CALL_STATE_ACTIVE		4
#define MM_CALL_STATE_HELD		5
#define MM_CALL_STATE_TERMINATED	7

struct __attribute__((packed)) ccci_header {
	uint32_t data[2];
	uint32_t channel;
	uint32_t reserved;
};

struct __attribute__((packed)) ccci_aud_payload_hdr {
	uint16_t sync;
	uint16_t data_type;
	uint16_t data_size;
};

/* 128-byte Gen95 SpeechDriverNormal sph_info_t */
struct __attribute__((packed)) mtk_sph_info {
	uint8_t  application;       /* +0:  0 = normal CS/VoLTE speech */
	uint8_t  bt_info;           /* +1:  0 = non-BT */
	uint8_t  sample_rate_enum;  /* +2:  0=8k, 1=16k, 2=32k, 3=48k */
	uint8_t  opendsp_flag;      /* +3:  0 = AP normal speech */
	uint8_t  param_emi_valid;   /* +4:  0 = use MD NVRAM defaults */
	uint8_t  path;              /* +5:  2 = SPH_PATH_NORMAL */
	uint16_t param_size;        /* +6 */
	uint16_t param_index;       /* +8 */
	uint16_t ext_dev_info;      /* +10: 0 = internal codec */
	uint8_t  loopback_flag;     /* +12 */
	uint8_t  loopback_delay;    /* +13 */
	uint16_t echo_ref_delay_ms; /* +14 */
	uint8_t  mic_delay_ms;      /* +16 */
	uint8_t  pad17;             /* +17 */
	uint16_t enh_dynamic_ctrl;  /* +18 */
	uint8_t  reserved[108];     /* +20..127 */
};

_Static_assert(sizeof(struct ccci_header) == 16, "ccci_header must be 16 B");
_Static_assert(sizeof(struct ccci_aud_payload_hdr) == 6, "payload hdr must be 6 B");
_Static_assert(sizeof(struct mtk_sph_info) == 128, "mtk_sph_info must be 128 B");

struct audd_ctx {
	const char *dev_path;
	const char *ucm_card;
	const char *voice_out_dev;
	unsigned int alsa_card;
	unsigned int alsa_pcm;
	unsigned int sample_rate;
	bool verbose;
	bool no_ucm;

	int aud_fd;
	int pcm_pb_fd;
	int pcm_cap_fd;
	bool speech_active;

	GMainLoop *loop;
	GDBusConnection *bus;
	GHashTable *active_calls;
	guint sub_call_added;
	guint sub_call_deleted;
	guint sub_call_state;
	guint watch_mm;
	guint aud_watch_id;
	guint reopen_timer_id;
};

static uint8_t rate_to_enum(unsigned int rate)
{
	switch (rate) {
	case 8000:  return 0;
	case 16000: return 1;
	case 32000: return 2;
	case 48000: return 3;
	default:    return 2;
	}
}

static bool call_state_needs_audio(int32_t state)
{
	return state == MM_CALL_STATE_DIALING ||
	       state == MM_CALL_STATE_RINGING_OUT ||
	       state == MM_CALL_STATE_ACTIVE ||
	       state == MM_CALL_STATE_HELD;
}

static int ccci_aud_write_all(struct audd_ctx *ctx, const void *buf, size_t len)
{
	ssize_t n;

	if (ctx->aud_fd < 0)
		return -ENODEV;

	n = write(ctx->aud_fd, buf, len);
	if (n < 0) {
		int err = -errno;

		fprintf(stderr, "[ccci_audd] write(%zu B) failed: %s\n",
			len, strerror(-err));
		return err;
	}
	if ((size_t)n != len) {
		fprintf(stderr, "[ccci_audd] short write: %zd/%zu B\n", n, len);
		return -EIO;
	}
	return 0;
}

static int ccci_aud_send_mailbox(struct audd_ctx *ctx, uint16_t msg_id,
				 uint16_t param16, uint32_t param32)
{
	struct ccci_header hdr;

	hdr.data[0] = CCCI_MAILBOX_MAGIC;
	hdr.data[1] = ((uint32_t)msg_id << 16) | param16;
	hdr.channel = CCCI_CH_PCM_TX;
	hdr.reserved = param32;

	if (ctx->verbose)
		fprintf(stdout,
			"[ccci_audd] TX mailbox msg=0x%04x p16=0x%04x p32=0x%08x\n",
			msg_id, param16, param32);
	return ccci_aud_write_all(ctx, &hdr, sizeof(hdr));
}

static int ccci_aud_send_payload(struct audd_ctx *ctx, uint16_t msg_id,
				 uint16_t data_type, const void *data,
				 uint16_t data_size)
{
	uint8_t pkt[sizeof(struct ccci_header) +
		    sizeof(struct ccci_aud_payload_hdr) + 256];
	struct ccci_header *hdr = (struct ccci_header *)pkt;
	struct ccci_aud_payload_hdr *phdr =
		(struct ccci_aud_payload_hdr *)(pkt + sizeof(*hdr));
	uint16_t payload_total = (uint16_t)(data_size + sizeof(*phdr));
	size_t pkt_len = sizeof(*hdr) + payload_total;

	if (data_size > 256)
		return -EINVAL;

	hdr->data[0] = 0;
	hdr->data[1] = payload_total;
	hdr->channel = CCCI_CH_PCM_TX;
	hdr->reserved = ((uint32_t)msg_id << 16) | payload_total;

	phdr->sync = CCCI_AUD_PAYLOAD_SYNC_AP;
	phdr->data_type = data_type;
	phdr->data_size = data_size;
	memcpy(pkt + sizeof(*hdr) + sizeof(*phdr), data, data_size);

	if (ctx->verbose)
		fprintf(stdout,
			"[ccci_audd] TX payload msg=0x%04x type=0x%04x len=%u (pkt=%zu B)\n",
			msg_id, data_type, data_size, pkt_len);
	return ccci_aud_write_all(ctx, pkt, pkt_len);
}

static int ccci_aud_send_sph_on(struct audd_ctx *ctx)
{
	struct mtk_sph_info info;

	memset(&info, 0, sizeof(info));
	info.application = 0;
	info.bt_info = 0;
	info.sample_rate_enum = rate_to_enum(ctx->sample_rate);
	info.opendsp_flag = 0;
	info.param_emi_valid = 0;
	info.path = 2; /* SPH_PATH_NORMAL */

	return ccci_aud_send_payload(ctx, MSG_A2M_SPH_ON,
				     SHARE_BUFF_DATA_TYPE_SPH_INFO,
				     &info, sizeof(info));
}

static int ccci_aud_send_sph_off(struct audd_ctx *ctx)
{
	return ccci_aud_send_mailbox(ctx, MSG_A2M_SPH_OFF, 0, 0);
}

/* ---- Minimal ALSA UAPI Hostless_Speech (hw:<card>,<pcm>) control ---- */

static void mask_set_bit(struct snd_pcm_hw_params *p, int var, unsigned int bit)
{
	struct snd_mask *m = &p->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(m->bits, 0, sizeof(m->bits));
	m->bits[bit / 32U] = 1U << (bit % 32U);
}

static void interval_set_val(struct snd_pcm_hw_params *p, int var,
			     unsigned int val)
{
	struct snd_interval *i =
		&p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	i->min = val;
	i->max = val;
	i->openmin = 0;
	i->openmax = 0;
	i->integer = 1;
	i->empty = 0;
}

static int open_hostless_pcm(unsigned int card, unsigned int device,
			     bool capture, unsigned int rate)
{
	char path[64];
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	unsigned int period_frames = rate / 50U; /* 20 ms */
	int fd, i;

	snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%u%c",
		 card, device, capture ? 'c' : 'p');
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "[ccci_audd] open(%s): %s\n",
			path, strerror(errno));
		return -1;
	}

	memset(&hw, 0, sizeof(hw));
	hw.rmask = ~0U;
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(hw.masks[i].bits, 0xff, sizeof(hw.masks[i].bits));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++) {
		hw.intervals[i].min = 0;
		hw.intervals[i].max = UINT_MAX;
	}

	mask_set_bit(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set_bit(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	mask_set_bit(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set_val(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	interval_set_val(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	interval_set_val(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_frames);
	interval_set_val(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 4);

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
		fprintf(stderr, "[ccci_audd] %s HW_PARAMS(%u Hz): %s\n",
			path, rate, strerror(errno));
		close(fd);
		return -1;
	}

	memset(&sw, 0, sizeof(sw));
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = period_frames;
	sw.start_threshold = 1;
	/*
	 * Hostless DPCM playback has no DMA buffer (no WRITEI_FRAMES).
	 * Setting stop_threshold >= runtime->boundary (64-bit on arm64)
	 * tells ALSA's snd_pcm_playback_data() to allow SNDRV_PCM_IOCTL_START
	 * directly without queued frames, matching tinyalsa on MediaTek.
	 */
	sw.stop_threshold = ULONG_MAX;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
		fprintf(stderr, "[ccci_audd] %s SW_PARAMS: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		fprintf(stderr, "[ccci_audd] %s PREPARE: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
		fprintf(stderr, "[ccci_audd] %s START: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

static void close_hostless_pcm(int *fd)
{
	if (*fd >= 0) {
		(void)ioctl(*fd, SNDRV_PCM_IOCTL_DROP);
		close(*fd);
		*fd = -1;
	}
}

static void run_ucm(struct audd_ctx *ctx, const char *verb,
		    const char *out_dev, const char *in_dev)
{
	gchar *argv[12];
	g_autoptr(GError) err = NULL;
	gint status = 0;
	int i = 0;

	if (ctx->no_ucm)
		return;

	argv[i++] = (gchar *)"/usr/bin/alsaucm";
	argv[i++] = (gchar *)"-c";
	argv[i++] = (gchar *)ctx->ucm_card;
	argv[i++] = (gchar *)"set";
	argv[i++] = (gchar *)"_verb";
	argv[i++] = (gchar *)verb;
	if (out_dev) {
		argv[i++] = (gchar *)"set";
		argv[i++] = (gchar *)"_enadev";
		argv[i++] = (gchar *)out_dev;
	}
	if (in_dev) {
		argv[i++] = (gchar *)"set";
		argv[i++] = (gchar *)"_enadev";
		argv[i++] = (gchar *)in_dev;
	}
	argv[i] = NULL;

	if (!g_spawn_sync(NULL, argv, NULL,
			  G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
			  NULL, NULL, NULL, NULL, &status, &err)) {
		fprintf(stderr, "[ccci_audd] alsaucm(%s) failed: %s\n",
			verb, err ? err->message : "unknown");
	}
}

static uint16_t handle_aud_rx(struct audd_ctx *ctx, const uint8_t *buf, size_t len);

static bool ccci_aud_wait_ack(struct audd_ctx *ctx, uint16_t expected_msg,
			      int timeout_ms)
{
	struct pollfd pfd;
	uint8_t buf[CCCI_AUD_MAX_PKT];
	int elapsed = 0;

	if (ctx->aud_fd < 0)
		return false;

	pfd.fd = ctx->aud_fd;
	pfd.events = POLLIN;

	while (elapsed < timeout_ms) {
		int step = timeout_ms - elapsed;
		int r;

		if (step > 100)
			step = 100;
		r = poll(&pfd, 1, step);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (r == 0) {
			elapsed += step;
			continue;
		}
		if (pfd.revents & POLLIN) {
			for (;;) {
				ssize_t n = read(ctx->aud_fd, buf, sizeof(buf));

				if (n <= 0)
					break;
				if (handle_aud_rx(ctx, buf, (size_t)n) == expected_msg)
					return true;
			}
		}
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return false;
	}
	return false;
}

static void speech_start(struct audd_ctx *ctx)
{
	if (ctx->speech_active)
		return;

	fprintf(stdout, "[ccci_audd] activating voice speech (%u Hz, %s)\n",
		ctx->sample_rate, ctx->voice_out_dev);
	run_ucm(ctx, "Voice Call", ctx->voice_out_dev, "Mic");

	ctx->pcm_pb_fd = open_hostless_pcm(ctx->alsa_card, ctx->alsa_pcm,
					   false, ctx->sample_rate);
	ctx->pcm_cap_fd = open_hostless_pcm(ctx->alsa_card, ctx->alsa_pcm,
					    true, ctx->sample_rate);

	if (ccci_aud_send_sph_on(ctx) == 0) {
		if (!ccci_aud_wait_ack(ctx, MSG_M2A_SPH_ON_ACK, 1500))
			fprintf(stderr, "[ccci_audd] timed out waiting for SPH_ON_ACK\n");
		ctx->speech_active = true;
	} else {
		close_hostless_pcm(&ctx->pcm_cap_fd);
		close_hostless_pcm(&ctx->pcm_pb_fd);
		run_ucm(ctx, "HiFi", "Speaker", "Mic");
	}
}

static void speech_stop(struct audd_ctx *ctx)
{
	if (!ctx->speech_active)
		return;

	fprintf(stdout, "[ccci_audd] deactivating voice speech\n");
	if (ccci_aud_send_sph_off(ctx) == 0) {
		/* Wait for MD1 SPH_OFF_ACK before shutting down PCM2 clocks */
		if (!ccci_aud_wait_ack(ctx, MSG_M2A_SPH_OFF_ACK, 1500))
			fprintf(stderr, "[ccci_audd] timed out waiting for SPH_OFF_ACK\n");
	}
	ctx->speech_active = false;
	close_hostless_pcm(&ctx->pcm_cap_fd);
	close_hostless_pcm(&ctx->pcm_pb_fd);
	run_ucm(ctx, "HiFi", "Speaker", "Mic");
}

static void sync_speech_state(struct audd_ctx *ctx)
{
	if (g_hash_table_size(ctx->active_calls) > 0)
		speech_start(ctx);
	else
		speech_stop(ctx);
}

/* ---- /dev/ccci_aud RX handling ---- */

static uint16_t handle_aud_rx(struct audd_ctx *ctx, const uint8_t *buf, size_t len)
{
	const struct ccci_header *hdr = (const struct ccci_header *)buf;
	uint16_t msg_id;

	if (len < sizeof(*hdr))
		return 0;

	if (hdr->data[0] == CCCI_MAILBOX_MAGIC) {
		uint16_t param16 = (uint16_t)(hdr->data[1] & 0xffffU);

		msg_id = (uint16_t)(hdr->data[1] >> 16);
		if (ctx->verbose || msg_id == MSG_M2A_SPH_ON_ACK ||
		    msg_id == MSG_M2A_SPH_OFF_ACK ||
		    msg_id == MSG_M2A_MD_ALIVE ||
		    msg_id == MSG_M2A_EM_DATA_REQUEST)
			fprintf(stdout,
				"[ccci_audd] RX mailbox msg=0x%04x p16=0x%04x p32=0x%08x\n",
				msg_id, param16, hdr->reserved);
	} else {
		msg_id = (uint16_t)(hdr->reserved >> 16);
		if (ctx->verbose || msg_id == MSG_M2A_EM_DATA_REQUEST ||
		    msg_id == MSG_M2A_MD_ALIVE)
			fprintf(stdout,
				"[ccci_audd] RX payload msg=0x%04x len=%zu\n",
				msg_id, len);
	}

	/* Stock SpeechDriverNormal::processModemAlive() ACKs 0xafa0 with 0x2fa0 */
	if (msg_id == MSG_M2A_EM_DATA_REQUEST)
		(void)ccci_aud_send_mailbox(ctx, MSG_A2M_MD_ALIVE_ACK, 0, 0);

	return msg_id;
}

static gboolean try_open_aud(gpointer user_data);

static gboolean aud_fd_ready(gint fd, GIOCondition cond, gpointer user_data)
{
	struct audd_ctx *ctx = user_data;
	uint8_t buf[CCCI_AUD_MAX_PKT];

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		fprintf(stderr, "[ccci_audd] %s hangup/error, scheduling reopen\n",
			ctx->dev_path);
		ctx->aud_watch_id = 0;
		close(ctx->aud_fd);
		ctx->aud_fd = -1;
		if (!ctx->reopen_timer_id)
			ctx->reopen_timer_id = g_timeout_add_seconds(1, try_open_aud, ctx);
		return G_SOURCE_REMOVE;
	}

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				break;
			fprintf(stderr, "[ccci_audd] read(%s): %s\n",
				ctx->dev_path, strerror(errno));
			break;
		}
		if (n == 0)
			break;
		handle_aud_rx(ctx, buf, (size_t)n);
	}

	return G_SOURCE_CONTINUE;
}

static gboolean try_open_aud(gpointer user_data)
{
	struct audd_ctx *ctx = user_data;

	if (ctx->aud_fd >= 0)
		return G_SOURCE_REMOVE;

	ctx->aud_fd = open(ctx->dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx->aud_fd < 0)
		return G_SOURCE_CONTINUE;

	fprintf(stdout, "[ccci_audd] opened %s (rate=%u Hz)\n",
		ctx->dev_path, ctx->sample_rate);
	ctx->aud_watch_id = g_unix_fd_add(ctx->aud_fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
					  aud_fd_ready, ctx);
	ctx->reopen_timer_id = 0;
	return G_SOURCE_REMOVE;
}

/* ---- ModemManager Voice D-Bus monitoring ---- */

static void update_call_state(struct audd_ctx *ctx, const char *path,
			      int32_t state)
{
	if (ctx->verbose)
		fprintf(stdout, "[ccci_audd] call %s state=%d\n", path, state);

	if (call_state_needs_audio(state))
		g_hash_table_add(ctx->active_calls, g_strdup(path));
	else
		g_hash_table_remove(ctx->active_calls, path);

	sync_speech_state(ctx);
}

static void on_call_state_changed(GDBusConnection *conn, const gchar *sender,
				  const gchar *obj_path, const gchar *iface,
				  const gchar *signal_name, GVariant *params,
				  gpointer user_data)
{
	struct audd_ctx *ctx = user_data;
	int32_t old_state = 0, new_state = 0;
	uint32_t reason = 0;

	(void)conn;
	(void)sender;
	(void)iface;
	(void)signal_name;

	g_variant_get(params, "(iiu)", &old_state, &new_state, &reason);
	update_call_state(ctx, obj_path, new_state);
}

static void query_call_state(struct audd_ctx *ctx, const char *call_path)
{
	g_autoptr(GError) err = NULL;
	g_autoptr(GVariant) res = NULL;
	g_autoptr(GVariant) val = NULL;

	if (!ctx->bus)
		return;

	res = g_dbus_connection_call_sync(
		ctx->bus, MM_DBUS_SERVICE, call_path,
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", MM_DBUS_IFACE_CALL, "State"),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
	if (!res)
		return;

	g_variant_get(res, "(v)", &val);
	if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT32))
		update_call_state(ctx, call_path, g_variant_get_int32(val));
}

static void on_call_added(GDBusConnection *conn, const gchar *sender,
			  const gchar *obj_path, const gchar *iface,
			  const gchar *signal_name, GVariant *params,
			  gpointer user_data)
{
	struct audd_ctx *ctx = user_data;
	const gchar *call_path = NULL;

	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface;
	(void)signal_name;

	g_variant_get(params, "(&o)", &call_path);
	if (call_path)
		query_call_state(ctx, call_path);
}

static void on_call_deleted(GDBusConnection *conn, const gchar *sender,
			    const gchar *obj_path, const gchar *iface,
			    const gchar *signal_name, GVariant *params,
			    gpointer user_data)
{
	struct audd_ctx *ctx = user_data;
	const gchar *call_path = NULL;

	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface;
	(void)signal_name;

	g_variant_get(params, "(&o)", &call_path);
	if (call_path)
		update_call_state(ctx, call_path, MM_CALL_STATE_TERMINATED);
}

static void on_mm_appeared(GDBusConnection *conn, const gchar *name,
			   const gchar *owner, gpointer user_data)
{
	struct audd_ctx *ctx = user_data;

	(void)name;
	(void)owner;
	ctx->bus = conn;
	if (ctx->verbose)
		fprintf(stdout, "[ccci_audd] ModemManager appeared on D-Bus\n");

	ctx->sub_call_added = g_dbus_connection_signal_subscribe(
		conn, MM_DBUS_SERVICE, MM_DBUS_IFACE_VOICE, "CallAdded",
		NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_call_added, ctx, NULL);
	ctx->sub_call_deleted = g_dbus_connection_signal_subscribe(
		conn, MM_DBUS_SERVICE, MM_DBUS_IFACE_VOICE, "CallDeleted",
		NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_call_deleted, ctx, NULL);
	ctx->sub_call_state = g_dbus_connection_signal_subscribe(
		conn, MM_DBUS_SERVICE, MM_DBUS_IFACE_CALL, "StateChanged",
		NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_call_state_changed, ctx, NULL);
}

static void on_mm_vanished(GDBusConnection *conn, const gchar *name,
			   gpointer user_data)
{
	struct audd_ctx *ctx = user_data;

	(void)name;
	if (ctx->sub_call_added && conn) {
		g_dbus_connection_signal_unsubscribe(conn, ctx->sub_call_added);
		ctx->sub_call_added = 0;
	}
	if (ctx->sub_call_deleted && conn) {
		g_dbus_connection_signal_unsubscribe(conn, ctx->sub_call_deleted);
		ctx->sub_call_deleted = 0;
	}
	if (ctx->sub_call_state && conn) {
		g_dbus_connection_signal_unsubscribe(conn, ctx->sub_call_state);
		ctx->sub_call_state = 0;
	}
	g_hash_table_remove_all(ctx->active_calls);
	speech_stop(ctx);
}

static gboolean on_signal_quit(gpointer user_data)
{
	struct audd_ctx *ctx = user_data;

	if (ctx->loop)
		g_main_loop_quit(ctx->loop);
	return G_SOURCE_REMOVE;
}

static int run_oneshot_test(struct audd_ctx *ctx, bool turn_on)
{
	struct pollfd pfd;
	uint8_t buf[CCCI_AUD_MAX_PKT];
	int ret;

	ctx->aud_fd = open(ctx->dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx->aud_fd < 0) {
		fprintf(stderr, "[ccci_audd] open(%s): %s\n",
			ctx->dev_path, strerror(errno));
		return 1;
	}

	ctx->verbose = true;
	ret = turn_on ? ccci_aud_send_sph_on(ctx) : ccci_aud_send_sph_off(ctx);
	if (ret != 0) {
		close(ctx->aud_fd);
		return 1;
	}

	pfd.fd = ctx->aud_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 1500) > 0 && (pfd.revents & POLLIN)) {
		ssize_t n = read(ctx->aud_fd, buf, sizeof(buf));

		if (n > 0)
			handle_aud_rx(ctx, buf, (size_t)n);
	} else {
		fprintf(stderr, "[ccci_audd] timed out waiting for MD ACK\n");
	}

	close(ctx->aud_fd);
	return 0;
}

static int run_hostless_test(struct audd_ctx *ctx)
{
	int ret = 0;

	ctx->verbose = true;
	ctx->aud_fd = open(ctx->dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx->aud_fd < 0) {
		fprintf(stderr, "[ccci_audd] open(%s): %s\n",
			ctx->dev_path, strerror(errno));
		return 1;
	}

	speech_start(ctx);
	if (ctx->pcm_pb_fd < 0 || ctx->pcm_cap_fd < 0 || !ctx->speech_active)
		ret = 1;

	usleep(200000);
	speech_stop(ctx);

	close(ctx->aud_fd);
	ctx->aud_fd = -1;
	return ret;
}

int main(int argc, char **argv)
{
	struct audd_ctx ctx;
	int opt;
	bool test_on = false;
	bool test_off = false;
	bool test_hostless = false;

	static const struct option long_opts[] = {
		{ "dev",            required_argument, NULL, 'd' },
		{ "card",           required_argument, NULL, 'c' },
		{ "pcm",            required_argument, NULL, 'p' },
		{ "ucm-card",       required_argument, NULL, 'u' },
		{ "output",         required_argument, NULL, 'o' },
		{ "rate",           required_argument, NULL, 'r' },
		{ "no-ucm",         no_argument,       NULL, 'n' },
		{ "verbose",        no_argument,       NULL, 'v' },
		{ "test-speech-on", no_argument,       NULL, '1' },
		{ "test-speech-off",no_argument,       NULL, '0' },
		{ "test-hostless",  no_argument,       NULL, 'H' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&ctx, 0, sizeof(ctx));
	ctx.dev_path = "/dev/ccci_aud";
	ctx.ucm_card = "hw:mt6789mt6366";
	ctx.voice_out_dev = "Earpiece";
	ctx.alsa_card = 0;
	ctx.alsa_pcm = 5;
	ctx.sample_rate = 32000;
	ctx.aud_fd = -1;
	ctx.pcm_pb_fd = -1;
	ctx.pcm_cap_fd = -1;

	while ((opt = getopt_long(argc, argv, "d:c:p:u:o:r:nv", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd': ctx.dev_path = optarg; break;
		case 'c': ctx.alsa_card = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'p': ctx.alsa_pcm = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'u': ctx.ucm_card = optarg; break;
		case 'o': ctx.voice_out_dev = optarg; break;
		case 'r': ctx.sample_rate = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'n': ctx.no_ucm = true; break;
		case 'v': ctx.verbose = true; break;
		case '1': test_on = true; break;
		case '0': test_off = true; break;
		case 'H': test_hostless = true; break;
		default:
			fprintf(stderr,
				"Usage: %s [--dev /dev/ccci_aud] [--card 0] [--pcm 5] "
				"[--rate 32000] [--output Earpiece] [-v] "
				"[--test-speech-on|--test-speech-off|--test-hostless]\n",
				argv[0]);
			return 2;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (test_hostless)
		return run_hostless_test(&ctx);
	if (test_on || test_off)
		return run_oneshot_test(&ctx, test_on);

	ctx.active_calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ctx.loop = g_main_loop_new(NULL, FALSE);

	if (try_open_aud(&ctx) == G_SOURCE_CONTINUE)
		ctx.reopen_timer_id = g_timeout_add_seconds(1, try_open_aud, &ctx);

	ctx.watch_mm = g_bus_watch_name(G_BUS_TYPE_SYSTEM, MM_DBUS_SERVICE,
					G_BUS_NAME_WATCHER_FLAGS_NONE,
					on_mm_appeared, on_mm_vanished,
					&ctx, NULL);

	g_unix_signal_add(SIGINT, on_signal_quit, &ctx);
	g_unix_signal_add(SIGTERM, on_signal_quit, &ctx);

	g_main_loop_run(ctx.loop);

	speech_stop(&ctx);
	if (ctx.watch_mm)
		g_bus_unwatch_name(ctx.watch_mm);
	if (ctx.aud_watch_id)
		g_source_remove(ctx.aud_watch_id);
	if (ctx.reopen_timer_id)
		g_source_remove(ctx.reopen_timer_id);
	if (ctx.aud_fd >= 0)
		close(ctx.aud_fd);
	g_hash_table_unref(ctx.active_calls);
	g_main_loop_unref(ctx.loop);
	return 0;
}
