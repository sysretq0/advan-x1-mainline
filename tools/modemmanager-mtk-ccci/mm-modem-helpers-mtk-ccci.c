/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-modem-helpers-mtk-ccci.h"

#include <string.h>

static gchar *
unquote (gchar *field)
{
    gsize len;

    g_strstrip (field);
    len = strlen (field);
    if (len >= 2 && field[0] == '"' && field[len - 1] == '"') {
        field[len - 1] = '\0';
        return field + 1;
    }
    return field;
}

static gboolean
valid_ipv4 (const gchar *text)
{
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string (text);

    return address && g_inet_address_get_family (address) == G_SOCKET_FAMILY_IPV4 &&
           !g_inet_address_get_is_any (address) && !g_inet_address_get_is_multicast (address);
}

static gboolean
valid_ipv6 (const gchar *text)
{
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string (text);

    return address && g_inet_address_get_family (address) == G_SOCKET_FAMILY_IPV6 &&
           !g_inet_address_get_is_any (address) && !g_inet_address_get_is_multicast (address);
}

/*
 * Normalize a 3GPP TS 27.007 dotted-decimal 16-octet IPv6 string (or a standard
 * colon-hex IPv6 string) into canonical IPv6 text.
 */
static gchar *
normalize_ipv6 (const gchar *text)
{
    g_auto(GStrv) octets = NULL;
    guint8 bytes[16];
    guint i;

    if (!text || !*text)
        return NULL;

    if (valid_ipv6 (text)) {
        g_autoptr(GInetAddress) addr = g_inet_address_new_from_string (text);

        return addr ? g_inet_address_to_string (addr) : NULL;
    }

    octets = g_strsplit (text, ".", -1);
    if (g_strv_length (octets) != 16 && g_strv_length (octets) != 32)
        return NULL;

    for (i = 0; i < 16; i++) {
        gchar *end = NULL;
        guint64 v = g_ascii_strtoull (octets[i], &end, 10);

        if (!*octets[i] || *end || v > 255)
            return NULL;
        bytes[i] = (guint8) v;
    }

    {
        g_autoptr(GInetAddress) addr = g_inet_address_new_from_bytes (bytes, G_SOCKET_FAMILY_IPV6);

        if (!addr || g_inet_address_get_is_any (addr) || g_inet_address_get_is_multicast (addr))
            return NULL;
        return g_inet_address_to_string (addr);
    }
}

gboolean
mm_mtk_ccci_parse_eif (const gchar *line, MMMtkCcciEif *eif)
{
    g_auto(GStrv) fields = NULL;
    const gchar *tag;
    gchar *end, *event;
    guint64 id;

    tag = strstr (line, "+EIF:");
    if (!tag)
        return FALSE;
    fields = g_strsplit (tag + strlen ("+EIF:"), ",", -1);
    if (g_strv_length (fields) < 2)
        return FALSE;
    id = g_ascii_strtoull (g_strstrip (fields[0]), &end, 10);
    if (*end || !*fields[0] || id > G_MAXUINT || id % 100 > 20)
        return FALSE;
    event = unquote (fields[1]);
    if (!g_str_equal (event, "ifup") && !g_str_equal (event, "ifdown"))
        return FALSE;
    memset (eif, 0, sizeof (*eif));
    eif->id = id;
    eif->up = g_str_equal (event, "ifup");
    if (eif->up) {
        const gchar *type_str, *address;
        guint64 addr_type;

        if (g_strv_length (fields) < 4)
            return FALSE;
        type_str = unquote (fields[2]);
        addr_type = g_ascii_strtoull (type_str, &end, 10);
        if (*end || !*type_str || addr_type < 1 || addr_type > 3)
            return FALSE;
        address = unquote (fields[3]);
        eif->addr_type = (guint) addr_type;

        if ((addr_type == 1 || addr_type == 3) && valid_ipv4 (address)) {
            eif->is_ipv6 = FALSE;
            g_strlcpy (eif->address, address, sizeof (eif->address));
            return TRUE;
        }
        if ((addr_type == 2 || addr_type == 3) && valid_ipv6 (address)) {
            eif->is_ipv6 = TRUE;
            g_strlcpy (eif->address, address, sizeof (eif->address));
            return TRUE;
        }
        return FALSE;
    }
    return TRUE;
}

static gboolean
apn_matches (const gchar *reported_apn, const gchar *apn)
{
    if (!g_ascii_strcasecmp (reported_apn, apn))
        return TRUE;
    if (!g_ascii_strncasecmp (reported_apn, apn, strlen (apn)) &&
        g_regex_match_simple ("^\\.mnc[0-9]{3}\\.mcc[0-9]{3}\\.gprs$",
                              reported_apn + strlen (apn), G_REGEX_CASELESS, 0))
        return TRUE;
    return FALSE;
}

gchar **
mm_mtk_ccci_parse_dns (const gchar *response, const gchar *apn,
                       const gchar *address, gboolean ipv6, GError **error)
{
    g_auto(GStrv) lines = g_strsplit_set (response, "\r\n", -1);
    guint i;

    for (i = 0; lines[i]; i++) {
        g_auto(GStrv) fields = NULL;
        const gchar *tag = strstr (lines[i], "+CGCONTRDP:");
        const gchar *reported_apn, *reported_local;
        GPtrArray *dns;
        guint j;

        if (!tag)
            continue;
        fields = g_strsplit (tag + strlen ("+CGCONTRDP:"), ",", -1);
        if (g_strv_length (fields) < 7)
            continue;
        reported_apn = unquote (fields[2]);
        if (!apn_matches (reported_apn, apn))
            continue;
        reported_local = unquote (fields[3]);

        if (!ipv6) {
            if (*reported_local) {
                g_auto(GStrv) octets = g_strsplit (reported_local, ".", -1);
                g_autofree gchar *local = NULL;

                if (g_strv_length (octets) != 4 && g_strv_length (octets) != 8)
                    continue;
                local = g_strdup_printf ("%s.%s.%s.%s", octets[0], octets[1], octets[2], octets[3]);
                if (address && *address && !g_str_equal (local, address))
                    continue;
            }
            dns = g_ptr_array_new_with_free_func (g_free);
            for (j = 5; j <= 6; j++) {
                const gchar *value = unquote (fields[j]);

                if (valid_ipv4 (value) &&
                    (!dns->len || !g_str_equal (g_ptr_array_index (dns, 0), value)))
                    g_ptr_array_add (dns, g_strdup (value));
            }
        } else {
            if (*reported_local) {
                g_autofree gchar *local_v6 = normalize_ipv6 (reported_local);

                if (!local_v6)
                    continue;
            }
            dns = g_ptr_array_new_with_free_func (g_free);
            for (j = 5; j <= 6; j++) {
                g_autofree gchar *norm = normalize_ipv6 (unquote (fields[j]));

                if (norm &&
                    (!dns->len || !g_str_equal (g_ptr_array_index (dns, 0), norm)))
                    g_ptr_array_add (dns, g_steal_pointer (&norm));
            }
        }

        if (dns->len) {
            g_ptr_array_add (dns, NULL);
            return (gchar **) g_ptr_array_free (dns, FALSE);
        }
        g_ptr_array_unref (dns);
    }

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "No %s DNS for APN %s / EIF address %s in +CGCONTRDP",
                 ipv6 ? "IPv6" : "IPv4", apn, address ? address : "(none)");
    return NULL;
}

gboolean
mm_mtk_ccci_parse_cgact (const gchar *response, guint *active_count)
{
    g_auto(GStrv) lines = NULL;
    guint i, seen = 0, active = 0;

    if (!response)
        return FALSE;

    lines = g_strsplit_set (response, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        g_auto(GStrv) fields = NULL;
        const gchar *tag = strstr (lines[i], "+CGACT:");
        gchar *end1 = NULL, *end2 = NULL;
        guint64 cid, state;

        if (!tag)
            continue;
        fields = g_strsplit (tag + strlen ("+CGACT:"), ",", -1);
        if (g_strv_length (fields) < 2)
            continue;
        cid = g_ascii_strtoull (g_strstrip (fields[0]), &end1, 10);
        state = g_ascii_strtoull (g_strstrip (fields[1]), &end2, 10);
        if (*end1 || !*fields[0] || *end2 || !*fields[1] || cid > 31)
            continue;
        seen++;
        if (state == 1)
            active++;
    }

    if (!seen)
        return FALSE;
    if (active_count)
        *active_count = active;
    return TRUE;
}

gboolean
mm_mtk_ccci_valid_apn (const gchar *apn)
{
    const gchar *p;

    if (!apn || !*apn || strlen (apn) > 100)
        return FALSE;
    for (p = apn; *p; p++) {
        guchar c = (guchar) *p;

        if (c < 0x21 || c > 0x7e || c == '"' || c == ';' || c == ',' || c == '\\')
            return FALSE;
    }
    return TRUE;
}

gboolean
mm_mtk_ccci_valid_cred (const gchar *value)
{
    const gchar *p;

    if (!value || !*value)
        return TRUE;
    if (strlen (value) > 100)
        return FALSE;
    for (p = value; *p; p++) {
        guchar c = (guchar) *p;

        if (c < 0x20 || c > 0x7e || c == '"' || c == ';' || c == ',' || c == '\\')
            return FALSE;
    }
    return TRUE;
}
