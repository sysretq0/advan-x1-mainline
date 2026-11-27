/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MTK CCCI IMS bridge: a root-only Unix socket through which mtk-imsd uses
 * ModemManager's serialized AT port (/dev/wwan0at0), receives +EIF / IMS URCs,
 * reports IMS registration state, and bridges SMS over IP (MT/MO) and VoLTE
 * voice calls (dial/answer/hangup/DTMF/call-state).
 *
 * Adapted from Zinwa Q25's mm-ims-bridge-mtk-ccci.h (GPL-2.0-or-later).
 */
#ifndef MM_IMS_BRIDGE_MTK_CCCI_H
#define MM_IMS_BRIDGE_MTK_CCCI_H

#include <glib.h>
#include <gio/gio.h>

#include "mm-base-modem.h"
#include "mm-base-sms.h"
#include "mm-base-call.h"

#define MM_MTK_CCCI_IMS_BRIDGE_PATH "/run/mtk-ims-bridge.sock"
#define MM_MTK_CCCI_IMS_BRIDGE_KEY  "mtk-ims-bridge"

typedef struct _MMMtkCcciImsBridge MMMtkCcciImsBridge;

MMMtkCcciImsBridge *mm_mtk_ccci_ims_bridge_new  (MMBaseModem *modem);
void                mm_mtk_ccci_ims_bridge_free (MMMtkCcciImsBridge *bridge);
void                mm_mtk_ccci_ims_bridge_urc  (MMMtkCcciImsBridge *bridge, const gchar *line);

/* Active VoLTE call state helpers for MMIfaceModemVoice */
gboolean mm_mtk_ccci_ims_bridge_has_call          (MMMtkCcciImsBridge *bridge);
GList   *mm_mtk_ccci_ims_bridge_dup_call_list     (MMMtkCcciImsBridge *bridge);
void     mm_mtk_ccci_ims_bridge_hangup_all        (MMMtkCcciImsBridge  *bridge,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data);
gboolean mm_mtk_ccci_ims_bridge_hangup_all_finish (GAsyncResult *res, GError **error);

/* SMS object whose Send goes over IMS when the bridge client is registered */
#define MM_TYPE_SMS_MTK_IMS (mm_sms_mtk_ims_get_type ())
G_DECLARE_FINAL_TYPE (MMSmsMtkIms, mm_sms_mtk_ims, MM, SMS_MTK_IMS, MMBaseSms)
MMBaseSms *mm_sms_mtk_ims_new (MMBaseModem *modem);

/* Voice call object whose Start/Accept/Hangup/DTMF go over VoLTE when IMS is registered */
#define MM_TYPE_CALL_MTK_IMS (mm_call_mtk_ims_get_type ())
G_DECLARE_FINAL_TYPE (MMCallMtkIms, mm_call_mtk_ims, MM, CALL_MTK_IMS, MMBaseCall)
MMBaseCall *mm_call_mtk_ims_new (MMBaseModem     *modem,
                                 MMCallDirection  direction,
                                 const gchar     *number);

#endif
