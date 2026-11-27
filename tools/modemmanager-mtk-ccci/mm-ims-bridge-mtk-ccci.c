/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MTK CCCI IMS bridge (see header). Line protocol over a SOCK_STREAM Unix
 * socket, one client at a time (the newest connection replaces the old one).
 *
 * Adapted from Zinwa Q25's mm-ims-bridge-mtk-ccci.c (GPL-2.0-or-later).
 *
 * client -> MM:
 *   AT <id> <command>                  run <command> on the primary AT port
 *   REG <0|1>                          IMS registration state of the client
 *   MT <smsc|-> <tpdu-hex>             SMS received over IMS (SMS-DELIVER TPDU)
 *   MORES <id> OK|ERR [text]           result of an MO SMS request
 *   CALLRES <id> OK|ERR [text]         result of a VoLTE call request
 *   CALL <mo|mt> <state> [number]      VoLTE call state update
 *     state: dialing | ringing-out | ringing-in | active | terminated
 * MM -> client:
 *   ATRES <id> OK <response>           (CR/LF escaped as \n)
 *   ATRES <id> ERR <message>
 *   URC <line>                         +EIF / IMS indications
 *   MO <id> <tpdu-hex>                 send this SMS-SUBMIT TPDU over IMS
 *   MTRES OK|ERR [text]
 *   DIAL <id> <number>                 place outgoing VoLTE call
 *   ANSWER <id>                        answer incoming VoLTE call
 *   HANGUP <id>                        hang up / decline / cancel VoLTE call
 *   DTMF <id> <digit>                  send RFC 4733 DTMF digit
 */
#include <config.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gio/gunixsocketaddress.h>

#include "mm-ims-bridge-mtk-ccci.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-messaging.h"
#include "mm-iface-modem-voice.h"
#include "mm-modem-helpers.h"
#include "mm-sms-part-3gpp.h"
#include "mm-log-object.h"

#define MO_TIMEOUT_S       60
#define CALL_REQ_TIMEOUT_S 30
#define IMS_CALL_INDEX     1

struct _MMMtkCcciImsBridge {
    MMBaseModem       *modem;      /* owner; not referenced */
    GSocketService    *service;
    GSocketConnection *conn;
    GDataInputStream  *in;
    GOutputStream     *out;
    GSocket           *sock;       /* client socket, written non-blocking */
    GCancellable      *cancel;
    guint              drop_idle;  /* deferred drop after a failed write */
    dev_t              path_dev;   /* identity of the socket file we bound */
    ino_t              path_ino;
    gboolean           registered;
    guint              next_mo;
    GHashTable        *mo;         /* id -> GTask */
    guint              next_call_req;
    GHashTable        *call_reqs;  /* id -> GTask */
    gboolean           call_active;
    MMCallDirection    call_dir;
    MMCallState        call_state;
    gchar             *call_number;
};

static MMMtkCcciImsBridge *
bridge_of (gpointer modem)
{
    return modem ? g_object_get_data (G_OBJECT (modem), MM_MTK_CCCI_IMS_BRIDGE_KEY) : NULL;
}

static void
send_line (MMMtkCcciImsBridge *b, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void drop_client (MMMtkCcciImsBridge *b);

static gboolean
drop_client_idle (MMMtkCcciImsBridge *b)
{
    b->drop_idle = 0;
    drop_client (b);
    return G_SOURCE_REMOVE;
}

static void
send_line (MMMtkCcciImsBridge *b, const gchar *fmt, ...)
{
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    gsize len, done = 0;
    va_list ap;

    if (!b->out || !b->sock)
        return;
    va_start (ap, fmt);
    line = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    len = strlen (line);
    line = g_realloc (line, len + 2);
    line[len++] = '\n';
    line[len] = '\0';
    while (done < len) {
        gssize n = g_socket_send_with_blocking (b->sock, line + done, len - done,
                                                FALSE, NULL, &error);
        if (n <= 0)
            break;
        done += (gsize) n;
    }
    if (done < len) {
        mm_obj_warn (b->modem, "ims bridge: client not reading (%s); disconnecting it",
                     error ? error->message : "short write");
        b->out = NULL;
        if (!b->drop_idle)
            b->drop_idle = g_idle_add ((GSourceFunc) drop_client_idle, b);
    }
}

static void
fail_all_tasks (GHashTable *table, const gchar *why)
{
    GHashTableIter it;
    gpointer key, task;

    g_hash_table_iter_init (&it, table);
    while (g_hash_table_iter_next (&it, &key, &task)) {
        g_task_return_new_error (G_TASK (task), MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED, "IMS: %s", why);
        g_hash_table_iter_remove (&it);
    }
}

static void
terminate_active_ims_call (MMMtkCcciImsBridge *b)
{
    MMCallInfo info;

    if (!b->call_active)
        return;
    info.index = IMS_CALL_INDEX;
    info.direction = b->call_dir;
    info.state = MM_CALL_STATE_TERMINATED;
    info.number = b->call_number;
    b->call_active = FALSE;
    b->call_state = MM_CALL_STATE_TERMINATED;
    mm_iface_modem_voice_report_call (MM_IFACE_MODEM_VOICE (b->modem), &info);
    g_clear_pointer (&b->call_number, g_free);
}

static void
drop_client (MMMtkCcciImsBridge *b)
{
    if (b->cancel) {
        g_cancellable_cancel (b->cancel);
        g_clear_object (&b->cancel);
    }
    g_clear_object (&b->in);
    b->out = NULL;
    b->sock = NULL;
    if (b->conn) {
        g_io_stream_close (G_IO_STREAM (b->conn), NULL, NULL);
        g_clear_object (&b->conn);
    }
    if (b->registered)
        mm_obj_msg (b->modem, "ims bridge: client gone, voice/SMS fall back to CS/AT");
    b->registered = FALSE;
    fail_all_tasks (b->mo, "client disconnected");
    fail_all_tasks (b->call_reqs, "client disconnected");
    terminate_active_ims_call (b);
}

/* ------------------------------------------------------------- AT passthrough */
typedef struct {
    gchar *id;
} AtCtx;

static void
at_ready (MMBaseModem *modem, GAsyncResult *res, AtCtx *ctx)
{
    g_autoptr(GError) error = NULL;
    const gchar *response = mm_base_modem_at_command_finish (modem, res, &error);
    MMMtkCcciImsBridge *b = bridge_of (modem);

    if (b && b->out) {
        if (response) {
            g_autofree gchar *flat = g_strdup (response);
            g_autoptr(GString) esc = g_string_new (NULL);
            const gchar *p;

            for (p = flat; *p; p++) {
                if (*p == '\r')
                    continue;
                if (*p == '\n')
                    g_string_append (esc, "\\n");
                else
                    g_string_append_c (esc, *p);
            }
            send_line (b, "ATRES %s OK %s", ctx->id, esc->str);
        } else {
            send_line (b, "ATRES %s ERR %s", ctx->id, error->message);
        }
    }
    g_free (ctx->id);
    g_free (ctx);
}

/* -------------------------------------------------------------- MT injection */
static guint8 *
smsc_field (const gchar *smsc, gsize *len)
{
    GByteArray *a = g_byte_array_new ();
    const gchar *d = smsc;
    guint8 b = 0;
    gsize n, i;

    if (!smsc || g_str_equal (smsc, "-") || !*smsc) {
        guint8 zero = 0;
        g_byte_array_append (a, &zero, 1);
        *len = a->len;
        return g_byte_array_free (a, FALSE);
    }
    if (*d == '+')
        d++;
    n = strlen (d);
    b = (guint8) (1 + (n + 1) / 2);
    g_byte_array_append (a, &b, 1);
    b = smsc[0] == '+' ? 0x91 : 0x81;
    g_byte_array_append (a, &b, 1);
    for (i = 0; i < n; i += 2) {
        guint8 lo = (guint8) (d[i] - '0');
        guint8 hi = (i + 1 < n) ? (guint8) (d[i + 1] - '0') : 0x0F;
        b = (guint8) (hi << 4 | lo);
        g_byte_array_append (a, &b, 1);
    }
    *len = a->len;
    return g_byte_array_free (a, FALSE);
}

static void
handle_mt (MMMtkCcciImsBridge *b, const gchar *smsc, const gchar *hex)
{
    g_autoptr(GError) error = NULL;
    g_autofree guint8 *sc = NULL;
    g_autofree guint8 *tpdu = NULL;
    g_autofree guint8 *pdu = NULL;
    gsize sclen = 0, tlen = 0;
    MMSmsPart *part;

    tpdu = mm_utils_hexstr2bin (hex, -1, &tlen, &error);
    if (!tpdu) {
        send_line (b, "MTRES ERR %s", error->message);
        return;
    }
    sc = smsc_field (smsc, &sclen);
    pdu = g_malloc (sclen + tlen);
    memcpy (pdu, sc, sclen);
    memcpy (pdu + sclen, tpdu, tlen);
    part = mm_sms_part_3gpp_new_from_binary_pdu (SMS_PART_INVALID_INDEX, pdu,
                                                 sclen + tlen, b->modem, FALSE,
                                                 &error);
    if (!part) {
        send_line (b, "MTRES ERR %s", error->message);
        return;
    }
    if (!mm_iface_modem_messaging_take_part (MM_IFACE_MODEM_MESSAGING (b->modem),
                                             part, MM_SMS_STATE_RECEIVED,
                                             MM_SMS_STORAGE_UNKNOWN)) {
        send_line (b, "MTRES ERR not taken");
        return;
    }
    mm_obj_msg (b->modem, "ims bridge: SMS received over IMS");
    send_line (b, "MTRES OK");
}

/* ------------------------------------------------------------------- MO */
static gboolean
mo_timeout (gpointer data)
{
    MMMtkCcciImsBridge *b = g_object_get_data (
        G_OBJECT (g_task_get_task_data (data)), MM_MTK_CCCI_IMS_BRIDGE_KEY);
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (data), "mo-id"));

    if (b) {
        GTask *task = g_hash_table_lookup (b->mo, GUINT_TO_POINTER (id));
        if (task) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_TIMEOUT,
                                     "IMS: no SMS result");
            g_hash_table_remove (b->mo, GUINT_TO_POINTER (id));
        }
    }
    g_object_unref (data);
    return G_SOURCE_REMOVE;
}

static void
handle_mores (MMMtkCcciImsBridge *b, guint id, gboolean ok, const gchar *text)
{
    GTask *task = g_hash_table_lookup (b->mo, GUINT_TO_POINTER (id));

    if (!task)
        return;
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "SMS over IMS failed: %s",
                                 text && *text ? text : "unknown");
    g_hash_table_remove (b->mo, GUINT_TO_POINTER (id));
}

static gboolean
bridge_can_send (MMMtkCcciImsBridge *b)
{
    return b && b->out && b->registered;
}

static void
bridge_send_tpdu (MMMtkCcciImsBridge *b, const guint8 *tpdu, gsize len,
                  GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (NULL, NULL, callback, user_data);
    g_autofree gchar *hex = mm_utils_bin2hexstr (tpdu, len);
    guint id = ++b->next_mo;

    g_task_set_task_data (task, g_object_ref (b->modem), g_object_unref);
    g_object_set_data (G_OBJECT (task), "mo-id", GUINT_TO_POINTER (id));
    g_hash_table_insert (b->mo, GUINT_TO_POINTER (id), task);
    g_timeout_add_seconds (MO_TIMEOUT_S, mo_timeout, g_object_ref (task));
    send_line (b, "MO %u %s", id, hex);
}

/* --------------------------------------------------------------- VoLTE calls */
static gboolean
call_req_timeout (gpointer data)
{
    MMMtkCcciImsBridge *b = g_object_get_data (
        G_OBJECT (g_task_get_task_data (data)), MM_MTK_CCCI_IMS_BRIDGE_KEY);
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (data), "call-req-id"));

    if (b) {
        GTask *task = g_hash_table_lookup (b->call_reqs, GUINT_TO_POINTER (id));
        if (task) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_TIMEOUT,
                                     "IMS: VoLTE call request timed out");
            g_hash_table_remove (b->call_reqs, GUINT_TO_POINTER (id));
        }
    }
    g_object_unref (data);
    return G_SOURCE_REMOVE;
}

static void
handle_callres (MMMtkCcciImsBridge *b, guint id, gboolean ok, const gchar *text)
{
    GTask *task = g_hash_table_lookup (b->call_reqs, GUINT_TO_POINTER (id));

    if (!task)
        return;
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "VoLTE call failed: %s",
                                 text && *text ? text : "unknown");
    g_hash_table_remove (b->call_reqs, GUINT_TO_POINTER (id));
}

static void
bridge_send_call_req (MMMtkCcciImsBridge *b, const gchar *verb, const gchar *arg,
                      GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (NULL, NULL, callback, user_data);
    guint id;

    if (!b || !b->out) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "IMS bridge not connected");
        g_object_unref (task);
        return;
    }

    id = ++b->next_call_req;
    g_task_set_task_data (task, g_object_ref (b->modem), g_object_unref);
    g_object_set_data (G_OBJECT (task), "call-req-id", GUINT_TO_POINTER (id));
    g_hash_table_insert (b->call_reqs, GUINT_TO_POINTER (id), task);
    g_timeout_add_seconds (CALL_REQ_TIMEOUT_S, call_req_timeout, g_object_ref (task));
    if (arg && *arg)
        send_line (b, "%s %u %s", verb, id, arg);
    else
        send_line (b, "%s %u", verb, id);
}

static MMCallState
parse_ims_call_state (const gchar *s)
{
    if (g_str_equal (s, "dialing"))
        return MM_CALL_STATE_DIALING;
    if (g_str_equal (s, "ringing-out"))
        return MM_CALL_STATE_RINGING_OUT;
    if (g_str_equal (s, "ringing-in"))
        return MM_CALL_STATE_RINGING_IN;
    if (g_str_equal (s, "active"))
        return MM_CALL_STATE_ACTIVE;
    if (g_str_equal (s, "terminated"))
        return MM_CALL_STATE_TERMINATED;
    return MM_CALL_STATE_UNKNOWN;
}

static void
handle_call_state (MMMtkCcciImsBridge *b, const gchar *dir_s, const gchar *rest)
{
    g_auto(GStrv) parts = g_strsplit (rest, " ", 2);
    const gchar *state_s = parts[0];
    const gchar *num = (parts[0] && parts[1] && *parts[1] && !g_str_equal (parts[1], "-")) ? parts[1] : NULL;
    MMCallDirection dir;
    MMCallState st;
    MMCallInfo info;

    if (!state_s)
        return;

    if (g_str_equal (dir_s, "mo"))
        dir = MM_CALL_DIRECTION_OUTGOING;
    else if (g_str_equal (dir_s, "mt"))
        dir = MM_CALL_DIRECTION_INCOMING;
    else
        return;

    st = parse_ims_call_state (state_s);
    if (st == MM_CALL_STATE_UNKNOWN)
        return;

    if (st == MM_CALL_STATE_TERMINATED) {
        b->call_active = FALSE;
        b->call_state = MM_CALL_STATE_TERMINATED;
    } else {
        b->call_active = TRUE;
        b->call_dir = dir;
        b->call_state = st;
        if (num) {
            g_free (b->call_number);
            b->call_number = g_strdup (num);
        }
    }

    info.index = IMS_CALL_INDEX;
    info.direction = dir;
    info.state = st;
    info.number = (gchar *) (num ? num : b->call_number);

    mm_obj_msg (b->modem, "ims bridge: VoLTE call state %s/%s",
                dir_s, state_s);
    mm_iface_modem_voice_report_call (MM_IFACE_MODEM_VOICE (b->modem), &info);

    if (st == MM_CALL_STATE_TERMINATED)
        g_clear_pointer (&b->call_number, g_free);
}

gboolean
mm_mtk_ccci_ims_bridge_has_call (MMMtkCcciImsBridge *b)
{
    return b && b->call_active;
}

GList *
mm_mtk_ccci_ims_bridge_dup_call_list (MMMtkCcciImsBridge *b)
{
    MMCallInfo *info;

    if (!b || !b->call_active)
        return NULL;

    info = g_new0 (MMCallInfo, 1);
    info->index = IMS_CALL_INDEX;
    info->direction = b->call_dir;
    info->state = b->call_state;
    info->number = g_strdup (b->call_number);
    return g_list_append (NULL, info);
}

void
mm_mtk_ccci_ims_bridge_hangup_all (MMMtkCcciImsBridge  *b,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
    bridge_send_call_req (b, "HANGUP", NULL, callback, user_data);
}

gboolean
mm_mtk_ccci_ims_bridge_hangup_all_finish (GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* ------------------------------------------------------------- line reader */
static void read_next (MMMtkCcciImsBridge *b);

static void
handle_line (MMMtkCcciImsBridge *b, const gchar *line)
{
    g_auto(GStrv) f = g_strsplit (line, " ", 3);
    guint n = g_strv_length (f);

    if (n >= 3 && g_str_equal (f[0], "AT")) {
        AtCtx *ctx = g_new0 (AtCtx, 1);
        const gchar *cmd = f[2];

        if (g_ascii_strncasecmp (cmd, "AT", 2) == 0)
            cmd += 2;
        ctx->id = g_strdup (f[1]);
        mm_base_modem_at_command (b->modem, cmd, 90, FALSE,
                                  (GAsyncReadyCallback) at_ready, ctx);
    } else if (n >= 2 && g_str_equal (f[0], "REG")) {
        gboolean reg = g_str_equal (f[1], "1");
        if (reg != b->registered)
            mm_obj_msg (b->modem, "ims bridge: IMS %s",
                        reg ? "registered" : "not registered");
        b->registered = reg;
    } else if (n >= 3 && g_str_equal (f[0], "MT")) {
        handle_mt (b, f[1], f[2]);
    } else if (n >= 3 && g_str_equal (f[0], "MORES")) {
        g_auto(GStrv) r = g_strsplit (f[2], " ", 2);
        handle_mores (b, (guint) g_ascii_strtoull (f[1], NULL, 10),
                      g_str_equal (r[0], "OK"), r[1]);
    } else if (n >= 3 && g_str_equal (f[0], "CALLRES")) {
        g_auto(GStrv) r = g_strsplit (f[2], " ", 2);
        handle_callres (b, (guint) g_ascii_strtoull (f[1], NULL, 10),
                        g_str_equal (r[0], "OK"), r[1]);
    } else if (n >= 3 && g_str_equal (f[0], "CALL")) {
        handle_call_state (b, f[1], f[2]);
    } else {
        mm_obj_dbg (b->modem, "ims bridge: unknown request '%s'", f[0] ? f[0] : "");
    }
}

static void
read_ready (GDataInputStream *in, GAsyncResult *res, MMMtkCcciImsBridge *b)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = g_data_input_stream_read_line_finish_utf8 (in, res, NULL, &error);

    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (!line) {
        drop_client (b);
        return;
    }
    handle_line (b, line);
    if (b->in == in)
        read_next (b);
}

static void
read_next (MMMtkCcciImsBridge *b)
{
    g_data_input_stream_read_line_async (b->in, G_PRIORITY_DEFAULT, b->cancel,
                                         (GAsyncReadyCallback) read_ready, b);
}

static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn,
             GObject *source, MMMtkCcciImsBridge *b)
{
    if (b->drop_idle) {
        g_source_remove (b->drop_idle);
        b->drop_idle = 0;
    }
    drop_client (b);
    b->conn = g_object_ref (conn);
    b->cancel = g_cancellable_new ();
    b->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));
    g_data_input_stream_set_newline_type (b->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    b->out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
    b->sock = g_socket_connection_get_socket (conn);
    mm_obj_msg (b->modem, "ims bridge: client connected");
    read_next (b);
    return TRUE;
}

void
mm_mtk_ccci_ims_bridge_urc (MMMtkCcciImsBridge *b, const gchar *line)
{
    g_autofree gchar *clean = NULL;

    if (!b || !b->out)
        return;
    clean = g_strstrip (g_strdelimit (g_strdup (line), "\r\n", ' '));
    send_line (b, "URC %s", clean);
}

MMMtkCcciImsBridge *
mm_mtk_ccci_ims_bridge_new (MMBaseModem *modem)
{
    MMMtkCcciImsBridge *b = g_new0 (MMMtkCcciImsBridge, 1);
    g_autoptr(GSocketAddress) addr = NULL;
    g_autoptr(GError) error = NULL;

    b->modem = modem;
    b->mo = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
    b->call_reqs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
    b->service = g_socket_service_new ();
    unlink (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    addr = g_unix_socket_address_new (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    if (!g_socket_listener_add_address (G_SOCKET_LISTENER (b->service), addr,
                                        G_SOCKET_TYPE_STREAM,
                                        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        mm_obj_warn (modem, "ims bridge: cannot listen on %s: %s",
                     MM_MTK_CCCI_IMS_BRIDGE_PATH, error->message);
        g_clear_object (&b->service);
        return b;
    }
    if (chmod (MM_MTK_CCCI_IMS_BRIDGE_PATH, 0600) < 0)
        mm_obj_warn (modem, "ims bridge: chmod failed: %s", g_strerror (errno));
    {
        struct stat st;

        if (stat (MM_MTK_CCCI_IMS_BRIDGE_PATH, &st) == 0) {
            b->path_dev = st.st_dev;
            b->path_ino = st.st_ino;
        }
    }
    g_signal_connect (b->service, "incoming", G_CALLBACK (on_incoming), b);
    g_socket_service_start (b->service);
    return b;
}

void
mm_mtk_ccci_ims_bridge_free (MMMtkCcciImsBridge *b)
{
    struct stat st;

    if (!b)
        return;
    if (b->drop_idle) {
        g_source_remove (b->drop_idle);
        b->drop_idle = 0;
    }
    drop_client (b);
    if (b->service) {
        g_socket_service_stop (b->service);
        g_socket_listener_close (G_SOCKET_LISTENER (b->service));
        g_clear_object (&b->service);
        if (stat (MM_MTK_CCCI_IMS_BRIDGE_PATH, &st) == 0 &&
            st.st_dev == b->path_dev && st.st_ino == b->path_ino)
            unlink (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    }
    g_hash_table_unref (b->mo);
    g_hash_table_unref (b->call_reqs);
    g_free (b->call_number);
    g_free (b);
}

/* ================================================================ SMS object */
struct _MMSmsMtkIms {
    MMBaseSms parent;
};

G_DEFINE_TYPE (MMSmsMtkIms, mm_sms_mtk_ims, MM_TYPE_BASE_SMS)

typedef struct {
    MMBaseModem *modem;
    GList       *current;
} SendCtx;

static void
send_ctx_free (SendCtx *ctx)
{
    g_clear_object (&ctx->modem);
    g_free (ctx);
}

static gboolean
sms_send_finish (MMBaseSms *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void send_next_part (GTask *task);

static void
part_sent (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    SendCtx *ctx = g_task_get_task_data (task);

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->current = g_list_next (ctx->current);
    send_next_part (task);
}

static void
send_next_part (GTask *task)
{
    MMSmsMtkIms *self = g_task_get_source_object (task);
    SendCtx *ctx = g_task_get_task_data (task);
    MMMtkCcciImsBridge *b = bridge_of (ctx->modem);
    g_autofree guint8 *pdu = NULL;
    guint len = 0, start = 0;
    GError *error = NULL;

    if (!ctx->current) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (!bridge_can_send (b)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "IMS registration lost while sending");
        g_object_unref (task);
        return;
    }
    pdu = mm_sms_part_3gpp_get_submit_pdu ((MMSmsPart *) ctx->current->data,
                                           &len, &start, self, &error);
    if (!pdu) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    bridge_send_tpdu (b, pdu + start, len - start,
                      (GAsyncReadyCallback) part_sent, task);
}

static void
parent_send_ready (MMBaseSms *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_SMS_CLASS (mm_sms_mtk_ims_parent_class)->send_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
sms_send (MMBaseSms *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    SendCtx *ctx = g_new0 (SendCtx, 1);

    g_object_get (self, MM_BASE_SMS_MODEM, &ctx->modem, NULL);
    g_task_set_task_data (task, ctx, (GDestroyNotify) send_ctx_free);
    if (!bridge_can_send (bridge_of (ctx->modem))) {
        MM_BASE_SMS_CLASS (mm_sms_mtk_ims_parent_class)->send (
            self, (GAsyncReadyCallback) parent_send_ready, task);
        return;
    }
    ctx->current = mm_base_sms_get_parts (self);
    send_next_part (task);
}

MMBaseSms *
mm_sms_mtk_ims_new (MMBaseModem *modem)
{
    return MM_BASE_SMS (g_object_new (MM_TYPE_SMS_MTK_IMS,
                                      MM_BASE_SMS_MODEM, modem, NULL));
}

static void
mm_sms_mtk_ims_init (MMSmsMtkIms *self)
{
}

static void
mm_sms_mtk_ims_class_init (MMSmsMtkImsClass *klass)
{
    MMBaseSmsClass *base = MM_BASE_SMS_CLASS (klass);

    base->send = sms_send;
    base->send_finish = sms_send_finish;
}

/* =============================================================== Call object */
struct _MMCallMtkIms {
    MMBaseCall parent;
    gboolean   is_ims;
};

G_DEFINE_TYPE (MMCallMtkIms, mm_call_mtk_ims, MM_TYPE_BASE_CALL)

static gboolean
call_bool_finish (MMBaseCall *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
ims_call_op_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_call_start_ready (MMBaseCall *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->start_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_start (MMBaseCall *base, GCancellable *cancellable,
            GAsyncReadyCallback callback, gpointer user_data)
{
    MMCallMtkIms *self = MM_CALL_MTK_IMS (base);
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    g_autoptr(MMBaseModem) modem = NULL;
    MMMtkCcciImsBridge *b;
    const gchar *number;

    g_object_get (self, MM_BASE_CALL_MODEM, &modem, NULL);
    b = bridge_of (modem);
    if (!bridge_can_send (b)) {
        self->is_ims = FALSE;
        MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->start (
            base, cancellable, (GAsyncReadyCallback) parent_call_start_ready, task);
        return;
    }

    number = mm_base_call_get_number (base);
    if (!number || !*number) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "VoLTE call requires a non-empty number");
        g_object_unref (task);
        return;
    }

    self->is_ims = TRUE;
    mm_base_call_set_index (base, IMS_CALL_INDEX);
    mm_gdbus_call_set_audio_port (MM_GDBUS_CALL (base), "ims");
    b->call_active = TRUE;
    b->call_dir = MM_CALL_DIRECTION_OUTGOING;
    b->call_state = MM_CALL_STATE_DIALING;
    g_free (b->call_number);
    b->call_number = g_strdup (number);

    mm_obj_msg (modem, "ims bridge: dialing VoLTE call");
    bridge_send_call_req (b, "DIAL", number, (GAsyncReadyCallback) ims_call_op_ready, task);
}

static void
parent_call_accept_ready (MMBaseCall *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->accept_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_accept (MMBaseCall *base, GAsyncReadyCallback callback, gpointer user_data)
{
    MMCallMtkIms *self = MM_CALL_MTK_IMS (base);
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autoptr(MMBaseModem) modem = NULL;

    if (!self->is_ims) {
        MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->accept (
            base, (GAsyncReadyCallback) parent_call_accept_ready, task);
        return;
    }

    g_object_get (self, MM_BASE_CALL_MODEM, &modem, NULL);
    bridge_send_call_req (bridge_of (modem), "ANSWER", NULL,
                          (GAsyncReadyCallback) ims_call_op_ready, task);
}

static void
parent_call_hangup_ready (MMBaseCall *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->hangup_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_hangup (MMBaseCall *base, GAsyncReadyCallback callback, gpointer user_data)
{
    MMCallMtkIms *self = MM_CALL_MTK_IMS (base);
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autoptr(MMBaseModem) modem = NULL;

    if (!self->is_ims) {
        MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->hangup (
            base, (GAsyncReadyCallback) parent_call_hangup_ready, task);
        return;
    }

    g_object_get (self, MM_BASE_CALL_MODEM, &modem, NULL);
    bridge_send_call_req (bridge_of (modem), "HANGUP", NULL,
                          (GAsyncReadyCallback) ims_call_op_ready, task);
}

static void
parent_call_dtmf_ready (MMBaseCall *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->send_dtmf_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_send_dtmf (MMBaseCall *base, const gchar *dtmf,
                GAsyncReadyCallback callback, gpointer user_data)
{
    MMCallMtkIms *self = MM_CALL_MTK_IMS (base);
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autoptr(MMBaseModem) modem = NULL;

    if (!self->is_ims) {
        MM_BASE_CALL_CLASS (mm_call_mtk_ims_parent_class)->send_dtmf (
            base, dtmf, (GAsyncReadyCallback) parent_call_dtmf_ready, task);
        return;
    }

    g_object_get (self, MM_BASE_CALL_MODEM, &modem, NULL);
    bridge_send_call_req (bridge_of (modem), "DTMF", dtmf,
                          (GAsyncReadyCallback) ims_call_op_ready, task);
}

MMBaseCall *
mm_call_mtk_ims_new (MMBaseModem     *modem,
                     MMCallDirection  direction,
                     const gchar     *number)
{
    MMCallMtkIms *self;
    MMMtkCcciImsBridge *b = bridge_of (modem);

    self = g_object_new (MM_TYPE_CALL_MTK_IMS,
                         MM_BASE_CALL_MODEM, modem,
                         MM_BASE_CALL_SKIP_INCOMING_TIMEOUT, TRUE,
                         MM_BASE_CALL_SUPPORTS_DIALING_TO_RINGING, TRUE,
                         MM_BASE_CALL_SUPPORTS_RINGING_TO_ACTIVE, TRUE,
                         "direction", direction,
                         "number", number,
                         NULL);
    if (direction == MM_CALL_DIRECTION_INCOMING && b && b->call_active &&
        b->call_dir == MM_CALL_DIRECTION_INCOMING) {
        self->is_ims = TRUE;
        mm_base_call_set_index (MM_BASE_CALL (self), IMS_CALL_INDEX);
        mm_gdbus_call_set_audio_port (MM_GDBUS_CALL (self), "ims");
    }
    return MM_BASE_CALL (self);
}

static void
mm_call_mtk_ims_init (MMCallMtkIms *self)
{
}

static void
mm_call_mtk_ims_class_init (MMCallMtkImsClass *klass)
{
    MMBaseCallClass *base = MM_BASE_CALL_CLASS (klass);

    base->start = call_start;
    base->start_finish = call_bool_finish;
    base->accept = call_accept;
    base->accept_finish = call_bool_finish;
    base->hangup = call_hangup;
    base->hangup_finish = call_bool_finish;
    base->send_dtmf = call_send_dtmf;
    base->send_dtmf_finish = call_bool_finish;
}
