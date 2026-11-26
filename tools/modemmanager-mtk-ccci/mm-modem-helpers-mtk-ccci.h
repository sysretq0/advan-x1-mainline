/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_MODEM_HELPERS_MTK_CCCI_H
#define MM_MODEM_HELPERS_MTK_CCCI_H

#include <gio/gio.h>

typedef struct {
    guint id;
    gboolean up;
    guint addr_type;   /* 1=IPV4, 2=IPV6, 3=IPV4V6 (stock NetAgentService) */
    gboolean is_ipv6;
    gchar address[64];
} MMMtkCcciEif;

gboolean mm_mtk_ccci_parse_eif (const gchar *line, MMMtkCcciEif *eif);
gchar **mm_mtk_ccci_parse_dns (const gchar *response, const gchar *apn,
                               const gchar *address, gboolean ipv6, GError **error);
gboolean mm_mtk_ccci_parse_cgact (const gchar *response, guint *active_count);
gboolean mm_mtk_ccci_valid_apn (const gchar *apn);
gboolean mm_mtk_ccci_valid_cred (const gchar *value);

#endif
