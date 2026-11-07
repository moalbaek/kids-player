#include "sonos_proto.h"
#include "kp_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>

size_t sonos_build_envelope(char *out, size_t outlen, const char *service, const char *action,
                            const sonos_arg_t *args, int nargs)
{
    int n = snprintf(out, outlen,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                     "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">",
                     action, service);
    if (n < 0 || (size_t)n >= outlen) return 0;
    size_t o = (size_t)n;
    for (int i = 0; i < nargs; i++) {
        n = snprintf(out + o, outlen - o, "<%s>", args[i].key);
        if (n < 0 || (size_t)n >= outlen - o) return 0;
        o += (size_t)n;
        if (!kp_xml_escape(args[i].value ? args[i].value : "", out + o, outlen - o)) return 0;
        o += strlen(out + o);
        n = snprintf(out + o, outlen - o, "</%s>", args[i].key);
        if (n < 0 || (size_t)n >= outlen - o) return 0;
        o += (size_t)n;
    }
    n = snprintf(out + o, outlen - o, "</u:%s></s:Body></s:Envelope>", action);
    if (n < 0 || (size_t)n >= outlen - o) return 0;
    return o + (size_t)n;
}

static bool location_ip(const char *loc, char *ip, size_t iplen)
{
    const char *p = strstr(loc, "://");
    if (!p) return false;
    p += 3;
    size_t n = 0;
    while (p[n] && p[n] != ':' && p[n] != '/' && n + 1 < iplen) {
        ip[n] = p[n];
        n++;
    }
    ip[n] = 0;
    return n > 0;
}

int sonos_parse_topology(const char *zgs, sonos_room_t *rooms, int max)
{
    int count = 0;
    const char *g = zgs;
    while ((g = strstr(g, "<ZoneGroup ")) != NULL && count < max) {
        const char *gtag_end = strchr(g, '>');
        const char *gend = strstr(g, "</ZoneGroup>");
        if (!gtag_end || !gend) break;
        char coord[48] = "";
        kp_xml_attr(g, gtag_end, "Coordinator", coord, sizeof(coord));

        // First pass: find the coordinator's IP within this group.
        char coord_ip[16] = "";
        for (const char *m = g; (m = strstr(m, "<ZoneGroupMember ")) != NULL && m < gend; m++) {
            const char *me = strchr(m, '>');
            if (!me) break;
            char uid[48] = "", loc[160] = "";
            kp_xml_attr(m, me, "UUID", uid, sizeof(uid));
            if (strcmp(uid, coord) == 0 && kp_xml_attr(m, me, "Location", loc, sizeof(loc))) {
                location_ip(loc, coord_ip, sizeof(coord_ip));
            }
        }
        // Second pass: one room per visible member.
        for (const char *m = g; (m = strstr(m, "<ZoneGroupMember ")) != NULL && m < gend && count < max; m++) {
            const char *me = strchr(m, '>');
            if (!me) break;
            char inv[8] = "", uid[48] = "", name[64] = "", loc[160] = "", ip[16] = "";
            kp_xml_attr(m, me, "Invisible", inv, sizeof(inv));
            if (strcmp(inv, "1") == 0) continue;
            kp_xml_attr(m, me, "UUID", uid, sizeof(uid));
            kp_xml_attr(m, me, "ZoneName", name, sizeof(name));
            kp_xml_attr(m, me, "Location", loc, sizeof(loc));
            if (!uid[0] || !name[0] || !location_ip(loc, ip, sizeof(ip))) continue;
            kp_html_unescape(name);
            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(rooms[i].name, name) == 0) { dup = true; break; }
            }
            if (dup) continue;
            sonos_room_t *r = &rooms[count++];
            kp_strlcpy(r->uid, uid, sizeof(r->uid));
            kp_strlcpy(r->name, name, sizeof(r->name));
            kp_strlcpy(r->ip, ip, sizeof(r->ip));
            kp_strlcpy(r->coord_uid, coord[0] ? coord : uid, sizeof(r->coord_uid));
            kp_strlcpy(r->coord_ip, coord_ip[0] ? coord_ip : ip, sizeof(r->coord_ip));
        }
        g = gend;
    }
    return count;
}

int sonos_parse_spotify_sid(const char *didl)
{
    const char *p = didl;
    while ((p = strstr(p, "x-sonos-spotify:")) != NULL) {
        const char *s = strstr(p, "sid=");
        const char *lim = strpbrk(p, "<\" ");
        if (s && (!lim || s < lim)) {
            int sid = atoi(s + 4);
            if (sid > 0) return sid;
        }
        p += 16;
    }
    return -1;
}

int sonos_parse_spotify_sn(const char *didl, int sid)
{
    const char *p = didl;
    while ((p = strstr(p, "x-sonos-spotify:")) != NULL) {
        const char *lim = strpbrk(p, "<\" ");
        const char *s = strstr(p, "sid=");
        const char *n = strstr(p, "sn=");
        if (s && n && (!lim || (s < lim && n < lim)) && atoi(s + 4) == sid) {
            int sn = atoi(n + 3);
            if (sn > 0) return sn;
        }
        p += 16;
    }
    return -1;
}

bool sonos_track_id_from_uri(const char *uri, char out[23])
{
    // e.g. x-sonos-spotify:spotify%3atrack%3a4uLU6hMCjMI75M1A2tKUQC?sid=9&flags=8224&sn=5
    const char *keys[] = {"spotify%3atrack%3a", "spotify%3Atrack%3A", "spotify:track:"};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = strstr(uri, keys[k]);
        if (p) {
            p += strlen(keys[k]);
            for (int i = 0; i < 22; i++) {
                if (!isalnum((unsigned char)p[i])) return false;
                out[i] = p[i];
            }
            out[22] = 0;
            return true;
        }
    }
    return false;
}


// Values from SoCo's share-link plugin (widely used), except the playlist item id, which matches what the
// iPhone test confirmed on a real system.
static const struct {
    const char *type;    // spotify:<type>:<id>
    const char *prefix;  // enqueue URI prefix
    const char *key;     // DIDL item id prefix
    const char *cls;     // upnp:class
} k_spotify[] = {
    [SONOS_SP_PLAYLIST] = {"playlist", "x-rincon-cpcontainer:1006206c", "1006206c", "object.container.playlistContainer"},
    [SONOS_SP_ALBUM] = {"album", "x-rincon-cpcontainer:1004206c", "00040000", "object.container.album.musicAlbum"},
    [SONOS_SP_TRACK] = {"track", "", "00032020", "object.item.audioItem.musicTrack"},
};

size_t sonos_spotify_uri(char *out, size_t outlen, sonos_sp_kind_t kind, const char *id, int sid, int sn)
{
    int n;
    if (kind == SONOS_SP_TRACK && sid > 0 && sn > 0) {
        // The form Sonos itself uses for queued Spotify songs (confirmed by the iPhone test).
        n = snprintf(out, outlen, "x-sonos-spotify:spotify%%3atrack%%3a%s?sid=%d&flags=8224&sn=%d", id, sid, sn);
    } else {
        n = snprintf(out, outlen, "%sspotify%%3a%s%%3a%s", k_spotify[kind].prefix, k_spotify[kind].type, id);
    }
    return (n > 0 && (size_t)n < outlen) ? (size_t)n : 0;
}

size_t sonos_spotify_didl(char *out, size_t outlen, sonos_sp_kind_t kind, const char *id, int service_type)
{
    int n = snprintf(out, outlen,
                     "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                     "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                     "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                     "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
                     "<item id=\"%sspotify%%3a%s%%3a%s\" parentID=\"\" restricted=\"true\">"
                     "<dc:title></dc:title><upnp:class>%s</upnp:class>"
                     "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
                     "SA_RINCON%d_X_#Svc%d-0-Token</desc></item></DIDL-Lite>",
                     k_spotify[kind].key, k_spotify[kind].type, id, k_spotify[kind].cls, service_type, service_type);
    return (n > 0 && (size_t)n < outlen) ? (size_t)n : 0;
}

int sonos_fault_code(const char *xml)
{
    char code[16];
    if (kp_xml_tag(xml, "errorCode", code, sizeof(code))) return atoi(code);
    return -1;
}

int sonos_queue_position(const char *didl, const char *track_id)
{
    if (!didl || !track_id || strlen(track_id) != 22) return -1;
    for (const char *hit = strstr(didl, track_id); hit; hit = strstr(hit + 1, track_id)) {
        // Must be a track URI ("track%3a<id>" or "track:<id>"), not some other id that happens to match.
        bool uri = (hit - didl >= 8 && strncasecmp(hit - 8, "track%3a", 8) == 0) ||
                   (hit - didl >= 6 && strncmp(hit - 6, "track:", 6) == 0);
        if (!uri) continue;
        for (const char *q = hit; q >= didl; q--) {  // the <item> that contains it
            if (strncmp(q, "<item", 5) == 0) {
                const char *idp = strstr(q, "id=\"Q:0/");
                if (idp && idp < hit) {
                    int n = atoi(idp + 8);
                    return n > 0 ? n : -1;
                }
                break;
            }
        }
    }
    return -1;
}
