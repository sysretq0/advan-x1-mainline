/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ModemManager plugin for mainline MediaTek MT6789 CCCI/DPMAIF WWAN modems
 * (/dev/wwan0at0 + wwan0..wwan7).
 *
 * Adapted for the mainline Linux WWAN subsystem from Zinwa Q25's
 * mm-plugin-mtk-ccci.c (GPL-2.0-or-later).
 */
#include <config.h>
#include <gmodule.h>
#include <string.h>

#include "mm-plugin-common.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem.h"
#include "mm-broadband-bearer.h"
#include "mm-bearer-list.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-mtk-ccci.h"

typedef struct {
    MMBaseModemAtCommand commands[9];
    gchar *set_apn;
    gchar *initial_apn;
    GError *error;
} ApnSetup;

static void
apn_setup_free (ApnSetup *ctx)
{
    g_free (ctx->set_apn);
    g_free (ctx->initial_apn);
    g_clear_error (&ctx->error);
    g_free (ctx);
}

static void
apn_unlock_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ApnSetup *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;

    mm_base_modem_at_command_finish (modem, res, &error);
    if (error)
        mm_obj_warn (modem, "APN lock rollback failed: %s", error->message);
    g_task_return_error (task, g_steal_pointer (&ctx->error));
    g_object_unref (task);
}

static void
apn_sequence_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ApnSetup *ctx = g_task_get_task_data (task);

    mm_base_modem_at_sequence_finish (modem, res, NULL, &ctx->error);
    if (ctx->error) {
        mm_base_modem_at_command (modem, "+EAPNLOCK=0", 10, FALSE,
                                  (GAsyncReadyCallback) apn_unlock_ready, task);
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static guint
mtk_auth_type (const gchar *user, const gchar *password, MMBearerAllowedAuth auth)
{
    if ((!user || !*user) && (!password || !*password))
        return 0;
    if (auth == MM_BEARER_ALLOWED_AUTH_PAP)
        return 1;
    if (auth == MM_BEARER_ALLOWED_AUTH_CHAP)
        return 2;
    if (auth == MM_BEARER_ALLOWED_AUTH_NONE)
        return 0;
    return 3; /* PAP or CHAP */
}

static void
configure_apn (MMBaseModem *modem, const gchar *apn,
               const gchar *user, const gchar *password, MMBearerAllowedAuth auth,
               GAsyncReadyCallback callback, gpointer user_data)
{
    static const MMBaseModemAtCommand commands[] = {
        { "+EGTYPE=4", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+EAPNLOCK=1", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+EAPNSET=\"\",1,\"\",\"\"", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { NULL, 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+EAPNLOCK=0", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+ECNCFG=1,,,1,1,1", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { NULL, 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+EIAREG=1", 10, FALSE, mm_base_modem_response_processor_no_result_continue },
        { NULL }
    };
    GTask *task = g_task_new (modem, NULL, callback, user_data);
    ApnSetup *ctx = g_new0 (ApnSetup, 1);
    const gchar *u = user ? user : "";
    const gchar *p = password ? password : "";
    guint authtype = mtk_auth_type (u, p, auth);

    G_STATIC_ASSERT (G_N_ELEMENTS (commands) == G_N_ELEMENTS (ctx->commands));
    memcpy (ctx->commands, commands, sizeof (commands));
    if (*u || *p) {
        ctx->set_apn = g_strdup_printf (
            "+EAPNSET=\"%s\",2,\"type=default,supl;protocol=IPV4V6;roaming_protocol=IPV4V6;"
            "user=%s;password=%s;authtype=%u;carrier_enabled=1;max_conns=0;max_conns_time=0;"
            "wait_time=0;bearer_bitmask=0;inactive_timer=0\"", apn, u, p, authtype);
    } else {
        ctx->set_apn = g_strdup_printf (
            "+EAPNSET=\"%s\",2,\"type=default,supl;protocol=IPV4V6;roaming_protocol=IPV4V6;"
            "authtype=0;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;"
            "bearer_bitmask=0;inactive_timer=0\"", apn);
    }
    ctx->initial_apn = g_strdup_printf ("+EIAAPN=\"%s\",0,\"IPV4V6\",\"IPV4V6\",%u,\"%s\",\"%s\"",
                                        apn, authtype, u, p);
    ctx->commands[3].command = ctx->set_apn;
    ctx->commands[6].command = ctx->initial_apn;
    g_task_set_task_data (task, ctx, (GDestroyNotify) apn_setup_free);
    mm_base_modem_at_sequence (modem, ctx->commands, NULL, NULL,
                               (GAsyncReadyCallback) apn_sequence_ready, task);
}

#define MM_TYPE_BEARER_MTK_CCCI (mm_bearer_mtk_ccci_get_type ())
G_DECLARE_FINAL_TYPE (MMBearerMtkCcci, mm_bearer_mtk_ccci, MM, BEARER_MTK_CCCI, MMBroadbandBearer)

struct _MMBearerMtkCcci {
    MMBroadbandBearer parent;
    GTask *pending;
    guint if_id;
    gboolean have_if_id;
};

G_DEFINE_TYPE (MMBearerMtkCcci, mm_bearer_mtk_ccci, MM_TYPE_BROADBAND_BEARER)

typedef enum {
    CONNECT_CONFIGURE_APN,
    CONNECT_ALLOW,
    CONNECT_ACTIVATE,
    CONNECT_WAIT_EIF,
    CONNECT_ACK_EIF,
    CONNECT_DNS,
    CONNECT_DONE,
} ConnectStep;

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    ConnectStep step;
    gchar *apn;
    gchar *user;
    gchar *password;
    MMBearerAllowedAuth auth;
    gboolean activated;
    gboolean have_eif;
    gboolean link_lost;
    MMMtkCcciEif eif;
    guint wait_id;
    guint wait_count;
    GError *error;
    MMBearerIpConfig *ip_config;
    MMPort *data;
} ConnectContext;

static void connect_step (GTask *task);
static void connect_fail (GTask *task, GError *error);

static void
connect_context_free (ConnectContext *ctx)
{
    g_assert (!ctx->wait_id);
    g_clear_object (&ctx->modem);
    g_clear_object (&ctx->primary);
    g_clear_object (&ctx->ip_config);
    g_clear_object (&ctx->data);
    g_clear_error (&ctx->error);
    g_free (ctx->apn);
    g_free (ctx->user);
    g_free (ctx->password);
    g_free (ctx);
}

static void
connect_command (GTask *task, const gchar *command, guint timeout,
                 GAsyncReadyCallback callback)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    mm_base_modem_at_command_full (ctx->modem, MM_IFACE_PORT_AT (ctx->primary),
                                   command, timeout, FALSE, FALSE, NULL, callback, task);
}

static void connect_cleanup (GTask *task);

static void
connect_cleanup_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_base_modem_at_command_full_finish (modem, res, &error))
        mm_obj_warn (g_task_get_source_object (task), "PDN rollback failed: %s", error->message);
    connect_cleanup (task);
}

static void
connect_cleanup (GTask *task)
{
    MMBearerMtkCcci *self = g_task_get_source_object (task);
    ConnectContext *ctx = g_task_get_task_data (task);

    if (ctx->activated) {
        ctx->activated = FALSE;
        connect_command (task, "+EAPNACT=0,0", 30, (GAsyncReadyCallback) connect_cleanup_ready);
        return;
    }
    self->pending = NULL;
    self->have_if_id = FALSE;
    g_task_return_error (task, g_steal_pointer (&ctx->error));
    g_object_unref (task);
}

static void
connect_fail (GTask *task, GError *error)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    if (ctx->wait_id) {
        g_source_remove (ctx->wait_id);
        ctx->wait_id = 0;
    }
    ctx->error = error;
    connect_cleanup (task);
}

static gboolean
wait_eif (GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    ctx->wait_id = 0;
    ctx->wait_count++;
    connect_step (task);
    return G_SOURCE_REMOVE;
}

static void
connect_command_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    const gchar *response;

    response = mm_base_modem_at_command_full_finish (modem, res, &error);
    if (!response) {
        g_prefix_error (&error, "CCCI connect step %u: ", ctx->step);
        connect_fail (task, error);
        return;
    }
    if (ctx->step == CONNECT_DNS) {
        g_auto(GStrv) dns = mm_mtk_ccci_parse_dns (response, ctx->apn, ctx->eif.address, &error);

        if (!dns) {
            connect_fail (task, error);
            return;
        }
        mm_bearer_ip_config_set_dns (ctx->ip_config, (const gchar **) dns);
    }
    ctx->step++;
    connect_step (task);
}

static void
connect_apn_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        connect_fail (task, error);
        return;
    }
    ctx->step++;
    connect_step (task);
}

static void
connect_step (GTask *task)
{
    MMBearerMtkCcci *self = g_task_get_source_object (task);
    ConnectContext *ctx = g_task_get_task_data (task);
    g_autofree gchar *command = NULL;
    GCancellable *cancellable = g_task_get_cancellable (task);

    if (cancellable && g_cancellable_is_cancelled (cancellable)) {
        connect_fail (task, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "CCCI connect cancelled"));
        return;
    }
    if (ctx->link_lost) {
        connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                "PDN disconnected during activation"));
        return;
    }
    switch (ctx->step) {
    case CONNECT_CONFIGURE_APN:
        configure_apn (ctx->modem, ctx->apn, ctx->user, ctx->password, ctx->auth,
                       (GAsyncReadyCallback) connect_apn_ready, task);
        return;
    case CONNECT_ALLOW:
        command = g_strdup ("+EDALLOW=1");
        break;
    case CONNECT_ACTIVATE:
        ctx->activated = TRUE;
        command = g_strdup_printf ("+EAPNACT=1,\"%s\",\"default\",0", ctx->apn);
        break;
    case CONNECT_WAIT_EIF:
        if (!ctx->have_eif) {
            if (ctx->wait_count >= 30) {
                connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_TIMEOUT,
                                                        "No IPv4 +EIF indication after PDN activation"));
                return;
            }
            ctx->wait_id = g_timeout_add_seconds (1, (GSourceFunc) wait_eif, task);
            return;
        }
        command = g_strdup_printf ("wwan%u", ctx->eif.id % 100);
        ctx->data = mm_base_modem_get_port (ctx->modem, command);
        if (!ctx->data || mm_port_get_port_type (ctx->data) != MM_PORT_TYPE_NET ||
            mm_port_get_connected (ctx->data)) {
            connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                            "EIF %u requires available net port %s", ctx->eif.id, command));
            return;
        }
        ctx->ip_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ctx->ip_config, MM_BEARER_IP_METHOD_STATIC);
        mm_bearer_ip_config_set_address (ctx->ip_config, ctx->eif.address);
        mm_bearer_ip_config_set_prefix (ctx->ip_config, 32);
        mm_bearer_ip_config_set_mtu (ctx->ip_config, 1400);
        ctx->step++;
        connect_step (task);
        return;
    case CONNECT_ACK_EIF:
        command = g_strdup_printf ("+EIF=%u,\"ifst\",\"up\",3", ctx->eif.id);
        break;
    case CONNECT_DNS:
        command = g_strdup ("+CGCONTRDP");
        break;
    case CONNECT_DONE:
        self->if_id = ctx->eif.id;
        self->have_if_id = TRUE;
        self->pending = NULL;
        g_task_return_pointer (task, mm_bearer_connect_result_new (ctx->data, ctx->ip_config, NULL),
                               (GDestroyNotify) mm_bearer_connect_result_unref);
        g_object_unref (task);
        return;
    default:
        g_assert_not_reached ();
    }
    connect_command (task, command, ctx->step == CONNECT_ACTIVATE ? 60 : 10,
                     (GAsyncReadyCallback) connect_command_ready);
}

static void
connect_3gpp (MMBroadbandBearer *bearer, MMBroadbandModem *modem,
              MMPortSerialAt *primary, MMPortSerialAt *secondary,
              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (bearer);
    MMBearerProperties *config = mm_base_bearer_peek_config (MM_BASE_BEARER (self));
    ConnectContext *ctx;
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    const gchar *apn = mm_bearer_properties_get_apn (config);
    const gchar *user = mm_bearer_properties_get_user (config);
    const gchar *password = mm_bearer_properties_get_password (config);
    MMBearerIpFamily family = mm_bearer_properties_get_ip_type (config);
    MMBearerAllowedAuth auth = mm_bearer_properties_get_allowed_auth (config);

    if (!mm_mtk_ccci_valid_apn (apn) || !mm_mtk_ccci_valid_cred (user) ||
        !mm_mtk_ccci_valid_cred (password) ||
        (family != MM_BEARER_IP_FAMILY_NONE && family != MM_BEARER_IP_FAMILY_ANY &&
         !(family & (MM_BEARER_IP_FAMILY_IPV4 | MM_BEARER_IP_FAMILY_IPV4V6)))) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "CCCI requires a valid APN and IPv4 or IPv4v6");
        g_object_unref (task);
        return;
    }
    ctx = g_new0 (ConnectContext, 1);
    ctx->modem = g_object_ref (MM_BASE_MODEM (modem));
    ctx->primary = g_object_ref (primary);
    ctx->apn = g_strdup (apn);
    ctx->user = g_strdup (user);
    ctx->password = g_strdup (password);
    ctx->auth = auth;
    g_task_set_task_data (task, ctx, (GDestroyNotify) connect_context_free);
    g_task_set_check_cancellable (task, FALSE);
    self->pending = task;
    connect_step (task);
}

static MMBearerConnectResult *
connect_3gpp_finish (MMBroadbandBearer *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
disconnect_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    MMBearerMtkCcci *self = g_task_get_source_object (task);
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (modem, res, &error))
        g_task_return_error (task, error);
    else {
        self->have_if_id = FALSE;
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *self, MMBroadbandModem *modem,
                 MMPortSerialAt *primary, MMPortSerialAt *secondary, MMPort *data,
                 guint cid, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    mm_base_modem_at_command (MM_BASE_MODEM (modem), "+EAPNACT=0,0", 30, FALSE,
                              (GAsyncReadyCallback) disconnect_ready, task);
}

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
bearer_process_eif (MMBaseBearer *bearer, MMMtkCcciEif *eif)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (bearer);

    if (self->pending) {
        ConnectContext *ctx = g_task_get_task_data (self->pending);

        if (ctx->error || !ctx->activated)
            return;
        if (eif->up) {
            ctx->eif = *eif;
            ctx->have_eif = TRUE;
        } else if (ctx->have_eif && ctx->eif.id == eif->id) {
            ctx->link_lost = TRUE;
        }
    } else if (!eif->up && self->have_if_id && self->if_id == eif->id &&
               mm_base_bearer_get_status (bearer) == MM_BEARER_STATUS_CONNECTED) {
        self->have_if_id = FALSE;
        mm_base_bearer_report_connection_status (bearer, MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
    }
}

static void mm_bearer_mtk_ccci_init (MMBearerMtkCcci *self) {}

static void
mm_bearer_mtk_ccci_class_init (MMBearerMtkCcciClass *klass)
{
    MMBroadbandBearerClass *bearer_class = MM_BROADBAND_BEARER_CLASS (klass);
    MMBaseBearerClass *base_class = MM_BASE_BEARER_CLASS (klass);

    bearer_class->connect_3gpp = connect_3gpp;
    bearer_class->connect_3gpp_finish = connect_3gpp_finish;
    bearer_class->disconnect_3gpp = disconnect_3gpp;
    bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
    base_class->load_connection_status = NULL;
    base_class->load_connection_status_finish = NULL;
#if defined WITH_SUSPEND_RESUME
    base_class->reload_connection_status = NULL;
    base_class->reload_connection_status_finish = NULL;
#endif
}

#define MM_TYPE_BROADBAND_MODEM_MTK_CCCI (mm_broadband_modem_mtk_ccci_get_type ())
G_DECLARE_FINAL_TYPE (MMBroadbandModemMtkCcci, mm_broadband_modem_mtk_ccci,
                      MM, BROADBAND_MODEM_MTK_CCCI, MMBroadbandModem)

struct _MMBroadbandModemMtkCcci {
    MMBroadbandModem parent;
    MMBearerProperties *initial_eps_settings;
};

static void iface_modem_init (MMIfaceModemInterface *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMtkCcci, mm_broadband_modem_mtk_ccci,
                        MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init))

static void
initialization_complete (GTask *task, GError *error)
{
    MMBroadbandModem *self = g_task_get_source_object (task);
    gpointer parent_context = g_task_get_task_data (task);

    if (error) {
        MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_stopped (
            self, parent_context, NULL);
        g_task_return_error (task, error);
    } else
        g_task_return_pointer (task, parent_context, NULL);
    g_object_unref (task);
}

static void
initialize_sim_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (modem, res, &error);
    initialization_complete (task, error);
}

static void
initial_power_state_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    const gchar *response;
    guint state;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response || !mm_3gpp_parse_cfun_query_response (response, &state, &error)) {
        initialization_complete (task, error);
        return;
    }
    if (state == 0) {
        /* Cold MOLY boots with SIM powered down; CFUN=4 powers SIM without RF TX. */
        mm_base_modem_at_command (modem, "+CFUN=4", 30, FALSE,
                                  (GAsyncReadyCallback) initialize_sim_ready, task);
        return;
    }
    initialization_complete (task, NULL);
}

static void
parent_initialization_ready (MMBroadbandModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    gpointer parent_context;

    parent_context = MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_started_finish (
        self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, parent_context, NULL);
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CFUN?", 5, FALSE,
                              (GAsyncReadyCallback) initial_power_state_ready, task);
}

static void
initialization_started (MMBroadbandModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_started (
        self, (GAsyncReadyCallback) parent_initialization_ready, task);
}

static gpointer
initialization_started_finish (MMBroadbandModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static MMBearerProperties *
load_initial_eps_settings_finish (MMIfaceModem3gpp *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_initial_eps_settings (MMIfaceModem3gpp *iface, GAsyncReadyCallback callback, gpointer user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (iface);
    GTask *task = g_task_new (self, NULL, callback, user_data);

    if (self->initial_eps_settings)
        g_task_return_pointer (task, g_object_ref (self->initial_eps_settings), g_object_unref);
    else
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "Initial attach APN has not been configured this session");
    g_object_unref (task);
}

static void
initial_eps_apn_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        g_task_return_error (task, error);
    else {
        g_set_object (&self->initial_eps_settings, g_task_get_task_data (task));
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static gboolean
set_initial_eps_settings_finish (MMIfaceModem3gpp *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_initial_eps_settings (MMIfaceModem3gpp *self, MMBearerProperties *config,
                          GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    const gchar *apn = mm_bearer_properties_get_apn (config);
    const gchar *user = mm_bearer_properties_get_user (config);
    const gchar *password = mm_bearer_properties_get_password (config);
    MMBearerIpFamily family = mm_bearer_properties_get_ip_type (config);
    MMBearerAllowedAuth auth = mm_bearer_properties_get_allowed_auth (config);

    if (!mm_mtk_ccci_valid_apn (apn) || !mm_mtk_ccci_valid_cred (user) ||
        !mm_mtk_ccci_valid_cred (password) ||
        (family != MM_BEARER_IP_FAMILY_NONE && family != MM_BEARER_IP_FAMILY_ANY &&
         !(family & (MM_BEARER_IP_FAMILY_IPV4 | MM_BEARER_IP_FAMILY_IPV4V6)))) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "CCCI initial attach requires a valid APN and IPv4 or IPv4v6");
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, g_object_ref (config), g_object_unref);
    configure_apn (MM_BASE_MODEM (self), apn, user, password, auth,
                   (GAsyncReadyCallback) initial_eps_apn_ready, task);
}

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface->load_initial_eps_bearer = NULL;
    iface->load_initial_eps_bearer_finish = NULL;
    iface->load_initial_eps_bearer_settings = load_initial_eps_settings;
    iface->load_initial_eps_bearer_settings_finish = load_initial_eps_settings_finish;
    iface->set_initial_eps_bearer_settings = set_initial_eps_settings;
    iface->set_initial_eps_bearer_settings_finish = set_initial_eps_settings_finish;
}

static void
bearer_new_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    GObject *bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, &error);

    if (!bearer)
        g_task_return_error (task, error);
    else {
        mm_base_bearer_export (MM_BASE_BEARER (bearer));
        g_task_return_pointer (task, bearer, g_object_unref);
    }
    g_object_unref (task);
}

static void
create_bearer (MMIfaceModem *self, MMBearerProperties *properties,
               GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_async_initable_new_async (MM_TYPE_BEARER_MTK_CCCI, G_PRIORITY_DEFAULT, NULL,
                                (GAsyncReadyCallback) bearer_new_ready, task,
                                MM_BASE_BEARER_MODEM, self, MM_BASE_BEARER_CONFIG, properties, NULL);
}

static MMBaseBearer *
create_bearer_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static MMBearerList *
create_bearer_list (MMIfaceModem *self)
{
    return mm_bearer_list_new (1, 0);
}

static void
power_up (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    static const MMBaseModemAtCommand commands[] = {
        { "+EFUN=1", 30, FALSE, mm_base_modem_response_processor_no_result_continue },
        { "+CFUN=1", 30, FALSE, mm_base_modem_response_processor_no_result_continue },
        { NULL }
    };

    mm_base_modem_at_sequence (MM_BASE_MODEM (self), commands, NULL, NULL, callback, user_data);
}

static gboolean
power_up_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    GError *inner_error = NULL;

    mm_base_modem_at_sequence_finish (MM_BASE_MODEM (self), res, NULL, &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }
    return TRUE;
}

static void
power_down (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CFUN=4", 30, FALSE, callback, user_data);
}

static gboolean
power_down_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
load_signal_quality (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CESQ", 5, FALSE, callback, user_data);
}

static guint
load_signal_quality_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    const gchar *response;
    guint rxlev, ber, rscp, ecno, rsrq, rsrp;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response || !mm_3gpp_parse_cesq_response (response, &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp, error))
        return 0;
    if (rsrp <= 97)
        return MIN (rsrp * 100 / 97, 100);
    if (rscp <= 96)
        return MIN (rscp * 100 / 96, 100);
    if (rxlev <= 63)
        return MIN (rxlev * 100 / 63, 100);
    g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "CESQ signal unavailable");
    return 0;
}

static void
load_supported_modes (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    static const MMModemModeCombination modes[] = {
        { MM_MODEM_MODE_2G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_3G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_3G },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_4G, MM_MODEM_MODE_4G },
        { MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_4G },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_3G },
        { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_4G },
    };
    GTask *task = g_task_new (self, NULL, callback, user_data);
    GArray *combinations = g_array_sized_new (FALSE, FALSE,
                                              sizeof (MMModemModeCombination),
                                              G_N_ELEMENTS (modes));

    g_array_append_vals (combinations, modes, G_N_ELEMENTS (modes));
    g_task_return_pointer (task, combinations, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static GArray *
load_supported_modes_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_current_modes (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+ERAT?", 5, FALSE, callback, user_data);
}

static gboolean
load_current_modes_finish (MMIfaceModem *self, GAsyncResult *res,
                           MMModemMode *allowed, MMModemMode *preferred, GError **error)
{
    g_autoptr(GMatchInfo) match_info = NULL;
    g_autoptr(GRegex) r = NULL;
    const gchar *response;
    gint erat_mode = -1, erat_pref = -1;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return FALSE;

    r = g_regex_new ("\\+ERAT:\\s*[0-9]+,\\s*[0-9]+,\\s*([0-9]+),\\s*([0-9]+)", 0, 0, error);
    if (!r)
        return FALSE;
    if (!g_regex_match (r, response, 0, &match_info) ||
        !mm_get_int_from_match_info (match_info, 1, &erat_mode) ||
        !mm_get_int_from_match_info (match_info, 2, &erat_pref)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't parse +ERAT response: '%s'", response);
        return FALSE;
    }

    switch (erat_mode) {
    case 0: *allowed = MM_MODEM_MODE_2G; break;
    case 1: *allowed = MM_MODEM_MODE_3G; break;
    case 2: *allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G; break;
    case 3: *allowed = MM_MODEM_MODE_4G; break;
    case 4: *allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_4G; break;
    case 5: *allowed = MM_MODEM_MODE_3G | MM_MODEM_MODE_4G; break;
    case 6: *allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G; break;
    default:
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Unsupported +ERAT mode %d", erat_mode);
        return FALSE;
    }

    switch (erat_pref) {
    case 0: *preferred = MM_MODEM_MODE_NONE; break;
    case 1: *preferred = MM_MODEM_MODE_2G; break;
    case 2: *preferred = MM_MODEM_MODE_3G; break;
    case 3:
    case 4: *preferred = MM_MODEM_MODE_4G; break;
    default:
        *preferred = MM_MODEM_MODE_NONE;
        break;
    }

    return TRUE;
}

static gboolean
set_current_modes_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_current_modes_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (modem, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_current_modes (MMIfaceModem *self, MMModemMode allowed, MMModemMode preferred,
                   GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autofree gchar *command = NULL;
    gint erat_mode = -1, erat_pref = 0;

    if (allowed == MM_MODEM_MODE_ANY) {
        erat_mode = 6;
        erat_pref = 4;
    } else if (allowed == MM_MODEM_MODE_2G) {
        erat_mode = 0;
    } else if (allowed == MM_MODEM_MODE_3G) {
        erat_mode = 1;
    } else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G)) {
        erat_mode = 2;
    } else if (allowed == MM_MODEM_MODE_4G) {
        erat_mode = 3;
    } else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_4G)) {
        erat_mode = 4;
    } else if (allowed == (MM_MODEM_MODE_3G | MM_MODEM_MODE_4G)) {
        erat_mode = 5;
    } else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G)) {
        erat_mode = 6;
    }

    if (erat_mode < 0) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Unsupported RAT mode mask 0x%x", allowed);
        g_object_unref (task);
        return;
    }

    if (allowed != MM_MODEM_MODE_ANY) {
        if (preferred == MM_MODEM_MODE_4G)
            erat_pref = 4;
        else if (preferred == MM_MODEM_MODE_3G)
            erat_pref = 2;
        else if (preferred == MM_MODEM_MODE_2G)
            erat_pref = 1;
        else
            erat_pref = 0;
    }

    command = g_strdup_printf ("+ERAT=%d,%d", erat_mode, erat_pref);
    mm_base_modem_at_command (MM_BASE_MODEM (self), command, 30, FALSE,
                              (GAsyncReadyCallback) set_current_modes_ready, task);
}

static void
load_supported_ip_families (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_return_int (task, MM_BEARER_IP_FAMILY_IPV4 | MM_BEARER_IP_FAMILY_IPV4V6);
    g_object_unref (task);
}

static MMBearerIpFamily
load_supported_ip_families_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_int (G_TASK (res), error);
}

static void
eif_received (MMPortSerialAt *port, GMatchInfo *match, MMBroadbandModem *self)
{
    g_autofree gchar *line = g_match_info_fetch (match, 0);
    g_autoptr(MMBearerList) list = NULL;
    MMMtkCcciEif eif;

    if (!mm_mtk_ccci_parse_eif (line, &eif))
        return;
    g_object_get (self, MM_IFACE_MODEM_BEARER_LIST, &list, NULL);
    if (list)
        mm_bearer_list_foreach (list, (MMBearerListForeachFunc) bearer_process_eif, &eif);
}

static void
setup_ports (MMBroadbandModem *self)
{
    MMPortSerialAt *primary;
    g_autoptr(GRegex) regex = NULL;

    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->setup_ports (self);
    primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    regex = g_regex_new ("\\r\\n\\+EIF:[^\\r\\n]*\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    if (primary)
        mm_port_serial_at_add_unsolicited_msg_handler (primary, regex,
                                                       (MMPortSerialAtUnsolicitedMsgFn) eif_received, self, NULL);
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
    iface->create_bearer_list = create_bearer_list;
    iface->modem_power_up = power_up;
    iface->modem_power_up_finish = power_up_finish;
    iface->modem_power_down = power_down;
    iface->modem_power_down_finish = power_down_finish;
    iface->load_signal_quality = load_signal_quality;
    iface->load_signal_quality_finish = load_signal_quality_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_current_modes = load_current_modes;
    iface->load_current_modes_finish = load_current_modes_finish;
    iface->set_current_modes = set_current_modes;
    iface->set_current_modes_finish = set_current_modes_finish;
    iface->load_supported_ip_families = load_supported_ip_families;
    iface->load_supported_ip_families_finish = load_supported_ip_families_finish;
    iface->setup_flow_control = NULL;
    iface->setup_flow_control_finish = NULL;
}

static void mm_broadband_modem_mtk_ccci_init (MMBroadbandModemMtkCcci *self) {}

static void
modem_finalize (GObject *object)
{
    g_clear_object (&MM_BROADBAND_MODEM_MTK_CCCI (object)->initial_eps_settings);
    G_OBJECT_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->finalize (object);
}

static void
mm_broadband_modem_mtk_ccci_class_init (MMBroadbandModemMtkCcciClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = modem_finalize;
    MM_BROADBAND_MODEM_CLASS (klass)->setup_ports = setup_ports;
    MM_BROADBAND_MODEM_CLASS (klass)->initialization_started = initialization_started;
    MM_BROADBAND_MODEM_CLASS (klass)->initialization_started_finish = initialization_started_finish;
}

#define MM_TYPE_PLUGIN_MTK_CCCI (mm_plugin_mtk_ccci_get_type ())
MM_DEFINE_PLUGIN (MTK_CCCI, mtk_ccci, MtkCcci)

static MMBaseModem *
create_modem (MMPlugin *self, const gchar *uid, const gchar *physdev,
              const gchar **drivers, guint16 vendor, guint16 product,
              guint16 subsystem_vendor, guint16 subsystem_device, GList *probes, GError **error)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MTK_CCCI,
                          MM_BASE_MODEM_DEVICE, uid,
                          MM_BASE_MODEM_PHYSDEV, physdev,
                          MM_BASE_MODEM_DRIVERS, drivers,
                          MM_BASE_MODEM_PLUGIN, mm_plugin_get_name (self),
                          MM_BASE_MODEM_VENDOR_ID, vendor,
                          MM_BASE_MODEM_PRODUCT_ID, product,
                          MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                          MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                          NULL);
}

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_mtk_ccci (void)
{
    static const gchar *subsystems[] = { "wwan", "net", NULL };
    static const gchar *tags[] = { "ID_MM_MTK_CCCI", NULL };

    return g_object_new (MM_TYPE_PLUGIN_MTK_CCCI,
                          MM_PLUGIN_NAME, MM_MODULE_NAME,
                          MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                          MM_PLUGIN_ALLOWED_UDEV_TAGS, tags,
                          MM_PLUGIN_ALLOWED_AT, TRUE,
                          MM_PLUGIN_SEND_LF, TRUE,
                          NULL);
}

static void mm_plugin_mtk_ccci_init (MMPluginMtkCcci *self) {}

static void
mm_plugin_mtk_ccci_class_init (MMPluginMtkCcciClass *klass)
{
    MM_PLUGIN_CLASS (klass)->create_modem = create_modem;
}
