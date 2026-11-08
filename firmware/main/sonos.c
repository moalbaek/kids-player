#include "sonos.h"
#include "settings.h"
#include "config.h"
#include "kp_util.h"
#include "http_util.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "sonos";

#define AVT "/MediaRenderer/AVTransport/Control", "AVTransport"
#define RC "/MediaRenderer/RenderingControl/Control", "RenderingControl"
#define CD "/MediaServer/ContentDirectory/Control", "ContentDirectory"
#define ZGT "/ZoneGroupTopology/Control", "ZoneGroupTopology"

static SemaphoreHandle_t s_mutex;
static char s_leader_ip[16], s_leader_uid[48];
static int64_t s_leader_at;

static void lock(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

void sonos_reset_cache(void)
{
    lock();
    s_leader_at = 0;
    s_leader_ip[0] = 0;
    unlock();
}

// One SOAP call. On success resp holds the body (caller frees). *fault gets the UPnP error code or -1.
static esp_err_t soap(const char *ip, const char *path, const char *service, const char *action,
                      const sonos_arg_t *args, int nargs, http_resp_t *resp, int *fault)
{
    if (fault) *fault = -1;
    memset(resp, 0, sizeof(*resp));
    if (!ip || !ip[0]) return ESP_ERR_INVALID_ARG;
    size_t cap = 6144;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return ESP_ERR_NO_MEM;
    size_t blen = sonos_build_envelope(body, cap, service, action, args, nargs);
    if (!blen) {
        heap_caps_free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    char url[96], soapaction[128];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, path);
    snprintf(soapaction, sizeof(soapaction), "\"urn:schemas-upnp-org:service:%s:1#%s\"", service, action);
    http_header_t hdr[] = {{"Content-Type", "text/xml; charset=\"utf-8\""}, {"SOAPACTION", soapaction}};
    esp_err_t err = http_request("POST", url, hdr, 2, body, blen, 512 * 1024, 8000, resp);
    heap_caps_free(body);
    if (err != ESP_OK || resp->status < 0) {
        http_resp_free(resp);
        return ESP_FAIL;
    }
    if (resp->status != 200) {
        int code = resp->data ? sonos_fault_code(resp->data) : -1;
        if (fault) *fault = code;
        ESP_LOGW(TAG, "%s on %s failed: HTTP %d, UPnP %d", action, ip, resp->status, code);
        http_resp_free(resp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t soap_simple(const char *ip, const char *path, const char *service, const char *action,
                             const sonos_arg_t *args, int nargs)
{
    http_resp_t r;
    esp_err_t err = soap(ip, path, service, action, args, nargs, &r, NULL);
    http_resp_free(&r);
    return err;
}

// Fetch ZoneGroupState from a speaker and parse it into rooms.
static int fetch_topology(const char *ip, sonos_room_t *rooms, int max)
{
    http_resp_t r;
    if (soap(ip, ZGT, "GetZoneGroupState", NULL, 0, &r, NULL) != ESP_OK) return 0;
    size_t len = 0;
    const char *p = kp_xml_tag_ptr(r.data, "ZoneGroupState", &len);
    int n = 0;
    if (p && len) {
        char *zgs = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (zgs) {
            memcpy(zgs, p, len);
            zgs[len] = 0;
            kp_html_unescape(zgs);
            n = sonos_parse_topology(zgs, rooms, max);
            heap_caps_free(zgs);
        }
    }
    http_resp_free(&r);
    return n;
}

// Room info straight from one speaker (used if topology is unavailable).
static bool device_room(const char *ip, sonos_room_t *room)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%.15s:1400/xml/device_description.xml", ip);
    http_resp_t r;
    if (http_request("GET", url, NULL, 0, NULL, 0, 64 * 1024, 5000, &r) != ESP_OK || r.status != 200) {
        http_resp_free(&r);
        return false;
    }
    char udn[64] = "", name[64] = "";
    kp_xml_tag(r.data, "UDN", udn, sizeof(udn));
    kp_xml_tag(r.data, "roomName", name, sizeof(name));
    http_resp_free(&r);
    if (!udn[0] || !name[0]) return false;
    kp_html_unescape(name);
    memset(room, 0, sizeof(*room));
    kp_strlcpy(room->uid, strncmp(udn, "uuid:", 5) == 0 ? udn + 5 : udn, sizeof(room->uid));
    kp_strlcpy(room->name, name, sizeof(room->name));
    kp_strlcpy(room->ip, ip, sizeof(room->ip));
    kp_strlcpy(room->coord_uid, room->uid, sizeof(room->coord_uid));
    kp_strlcpy(room->coord_ip, ip, sizeof(room->coord_ip));
    return true;
}

// SSDP search for Sonos players. Returns number of unique IPs found.
static int ssdp_search(char ips[][16], int max)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return 0;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(1900)};
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");
    static const char msg[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                              "MX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    int n = 0;
    char buf[768];
    int64_t end = esp_timer_get_time() + 3000000;
    int sends = 0;
    while (esp_timer_get_time() < end) {
        if (sends < 3) {
            sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
            sends++;
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        buf[r] = 0;
        if (!strstr(buf, "Sonos") && !strstr(buf, "ZonePlayer")) continue;
        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        bool dup = false;
        for (int i = 0; i < n; i++) if (strcmp(ips[i], ip) == 0) dup = true;
        if (!dup && n < max) kp_strlcpy(ips[n++], ip, 16);
    }
    close(sock);
    ESP_LOGI(TAG, "SSDP found %d player(s)", n);
    return n;
}

int sonos_discover(sonos_room_t *rooms, int max)
{
    char ips[12][16];
    int nip = ssdp_search(ips, 12);
    if (nip == 0 && g_set.speaker_ip[0]) {
        kp_strlcpy(ips[0], g_set.speaker_ip, 16);
        nip = 1;
    }
    for (int i = 0; i < nip; i++) {
        int n = fetch_topology(ips[i], rooms, max);
        if (n > 0) return n;
    }
    int n = 0;  // no topology service: ask each speaker for its room name
    for (int i = 0; i < nip && n < max; i++) {
        if (device_room(ips[i], &rooms[n])) n++;
    }
    return n;
}

// Find the group leader for the chosen room; refresh the room's IP if it changed.
static bool resolve_leader(char *spk_ip, char *leader_ip, char *leader_uid)
{
    settings_lock();
    kp_strlcpy(spk_ip, g_set.speaker_ip, 16);
    char uid[48];
    kp_strlcpy(uid, g_set.speaker_uid, sizeof(uid));
    settings_unlock();
    if (!uid[0]) return false;
    if (s_leader_ip[0] && esp_timer_get_time() - s_leader_at < 30LL * 1000000) {
        kp_strlcpy(leader_ip, s_leader_ip, 16);
        kp_strlcpy(leader_uid, s_leader_uid, 48);
        return true;
    }
    sonos_room_t *rooms = calloc(KP_MAX_ROOMS, sizeof(sonos_room_t));
    if (!rooms) return false;
    int n = spk_ip[0] ? fetch_topology(spk_ip, rooms, KP_MAX_ROOMS) : 0;
    if (n == 0) n = sonos_discover(rooms, KP_MAX_ROOMS);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(rooms[i].uid, uid) == 0) {
            kp_strlcpy(leader_ip, rooms[i].coord_ip, 16);
            kp_strlcpy(leader_uid, rooms[i].coord_uid, 48);
            if (strcmp(rooms[i].ip, spk_ip) != 0) {
                ESP_LOGI(TAG, "room '%s' moved to %s", rooms[i].name, rooms[i].ip);
                kp_strlcpy(spk_ip, rooms[i].ip, 16);
                settings_lock();
                kp_strlcpy(g_set.speaker_ip, rooms[i].ip, sizeof(g_set.speaker_ip));
                settings_unlock();
                settings_save();
            }
            found = true;
            break;
        }
    }
    free(rooms);
    if (!found && spk_ip[0]) {
        // Topology not available: assume the speaker leads itself.
        kp_strlcpy(leader_ip, spk_ip, 16);
        kp_strlcpy(leader_uid, uid, 48);
        found = true;
    }
    if (found) {
        kp_strlcpy(s_leader_ip, leader_ip, sizeof(s_leader_ip));
        kp_strlcpy(s_leader_uid, leader_uid, sizeof(s_leader_uid));
        s_leader_at = esp_timer_get_time();
    }
    return found;
}

// Spotify account serial on this Sonos system (sn=N in queued song URIs). 0 = not probed yet, -1 = unknown.
static int s_sn;

// Learn the Spotify service type (and account serial) from Spotify items in the Sonos favourites or queue.
static int probe_service_type(const char *leader_ip)
{
    static const char *objs[] = {"FV:2", "Q:0"};
    int type = 0;
    for (size_t i = 0; i < 2 && (type == 0 || s_sn <= 0); i++) {
        sonos_arg_t args[] = {{"ObjectID", objs[i]}, {"BrowseFlag", "BrowseDirectChildren"}, {"Filter", "*"},
                              {"StartingIndex", "0"}, {"RequestedCount", "100"}, {"SortCriteria", ""}};
        http_resp_t r;
        if (soap(leader_ip, CD, "Browse", args, 6, &r, NULL) != ESP_OK) continue;
        size_t len = 0;
        const char *p = kp_xml_tag_ptr(r.data, "Result", &len);
        if (p && len) {
            char *t = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (t) {
                memcpy(t, p, len);
                t[len] = 0;
                for (int k = 0; k < 3; k++) kp_html_unescape(t);
                int sid = sonos_parse_spotify_sid(t);
                if (sid > 0 && type == 0) type = sid * 256 + 7;
                if (sid > 0 && s_sn <= 0) s_sn = sonos_parse_spotify_sn(t, sid);
                heap_caps_free(t);
            }
        }
        http_resp_free(&r);
    }
    if (s_sn == 0) s_sn = -1;
    if (type) ESP_LOGI(TAG, "Spotify on Sonos: type %d, account sn=%d", type, s_sn);
    return type;
}

// Service types to try, best guess first.
static int service_types(const char *leader_ip, int types[4])
{
    int nt = 0;
    if (g_set.service_type > 0) {
        types[nt++] = g_set.service_type;
        if (s_sn == 0) probe_service_type(leader_ip);  // only for the account serial
    } else {
        int t = probe_service_type(leader_ip);
        if (t > 0) types[nt++] = t;
    }
    const int defaults[] = {2311, 3079};  // Spotify sid 9 and the older sid 12
    for (int i = 0; i < 2; i++) {
        bool have = false;
        for (int j = 0; j < nt; j++) if (types[j] == defaults[i]) have = true;
        if (!have) types[nt++] = defaults[i];
    }
    return nt;
}

static void remember_type(int type)
{
    if (g_set.service_type == type) return;
    settings_lock();
    g_set.service_type = type;
    settings_unlock();
    settings_save();
}

// A successful seek can stand on its own when GetPositionInfo cannot identify the track.
static bool current_track_is(const char *leader_ip, const char *track_id)
{
    sonos_arg_t a[] = {{"InstanceID", "0"}};
    http_resp_t r;
    bool same = true;
    if (soap(leader_ip, AVT, "GetPositionInfo", a, 1, &r, NULL) == ESP_OK) {
        char uri[256], cur[23];
        if (kp_xml_tag(r.data, "TrackURI", uri, sizeof(uri))) {
            kp_html_unescape(uri);
            if (sonos_track_id_from_uri(uri, cur)) same = strcmp(cur, track_id) == 0;
        }
    }
    http_resp_free(&r);
    return same;
}

// Where a song ended up in the queue. Spotify and Sonos can disagree on which entries of a playlist are
// playable, so the position can differ from the index on screen. Looks near the expected position.
static int find_in_queue(const char *leader_ip, const char *track_id, int expected)
{
    char start[12];
    snprintf(start, sizeof(start), "%d", expected > 40 ? expected - 40 : 0);
    sonos_arg_t a[] = {{"ObjectID", "Q:0"}, {"BrowseFlag", "BrowseDirectChildren"}, {"Filter", "res"},
                       {"StartingIndex", start}, {"RequestedCount", "100"}, {"SortCriteria", ""}};
    http_resp_t r;
    int pos = -1;
    if (soap(leader_ip, CD, "Browse", a, 6, &r, NULL) == ESP_OK) {
        size_t len = 0;
        const char *p = kp_xml_tag_ptr(r.data, "Result", &len);
        char *t = (p && len) ? heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (t) {
            memcpy(t, p, len);
            t[len] = 0;
            kp_html_unescape(t);
            pos = sonos_queue_position(t, track_id);
            heap_caps_free(t);
        }
    }
    http_resp_free(&r);
    return pos;
}

// AddURIToQueue at the end of the queue. Returns the number of songs added (0 on failure); *first gets the
// position of the first one.
static int enqueue(const char *leader_ip, const char *uri, const char *meta, int *first)
{
    sonos_arg_t add[] = {{"InstanceID", "0"}, {"EnqueuedURI", uri}, {"EnqueuedURIMetaData", meta},
                         {"DesiredFirstTrackNumberEnqueued", "0"}, {"EnqueueAsNext", "0"}};
    http_resp_t r;
    int fault = -1;
    if (soap(leader_ip, AVT, "AddURIToQueue", add, 5, &r, &fault) != ESP_OK) {
        ESP_LOGW(TAG, "AddURIToQueue rejected (UPnP %d): %s", fault, uri);
        return 0;
    }
    char v[16];
    int f = kp_xml_tag(r.data, "FirstTrackNumberEnqueued", v, sizeof(v)) ? atoi(v) : 1;
    int count = kp_xml_tag(r.data, "NumTracksAdded", v, sizeof(v)) ? atoi(v) : 0;
    http_resp_free(&r);
    if (first) *first = f > 0 ? f : 1;
    return count;
}

// Switch the room to its queue, select song number pos (1-based) and play.
static esp_err_t play_queue_at(const char *leader_ip, const char *leader_uid, int pos, const char *track_id)
{
    char queue_uri[80], target[16];
    snprintf(queue_uri, sizeof(queue_uri), "x-rincon-queue:%s#0", leader_uid);
    sonos_arg_t set[] = {{"InstanceID", "0"}, {"CurrentURI", queue_uri}, {"CurrentURIMetaData", ""}};
    if (soap_simple(leader_ip, AVT, "SetAVTransportURI", set, 3) != ESP_OK) return ESP_FAIL;
    snprintf(target, sizeof(target), "%d", pos);
    sonos_arg_t seek[] = {{"InstanceID", "0"}, {"Unit", "TRACK_NR"}, {"Target", target}};
    esp_err_t err = soap_simple(leader_ip, AVT, "Seek", seek, 3);
    // Without a song to check, a refused Seek is harmless: the fresh queue starts at the first song anyway.
    if (track_id && track_id[0]) {
        if (err != ESP_OK || !current_track_is(leader_ip, track_id)) {
            int found = find_in_queue(leader_ip, track_id, pos - 1);
            if (found <= 0) {
                ESP_LOGW(TAG, "could not find requested song %s in the queue", track_id);
                return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
            }
            ESP_LOGI(TAG, "seeking to verified queue position #%d", found);
            snprintf(target, sizeof(target), "%d", found);
            err = soap_simple(leader_ip, AVT, "Seek", seek, 3);
        }
        if (err != ESP_OK) return err;  // the speaker refused to select the song
    }
    sonos_arg_t play[] = {{"InstanceID", "0"}, {"Speed", "1"}};
    return soap_simple(leader_ip, AVT, "Play", play, 2);
}

esp_err_t sonos_play_container(sonos_sp_kind_t kind, const char *id, int index, const char *track_id)
{
    lock();
    char spk_ip[16], leader_ip[16], leader_uid[48];
    if (!resolve_leader(spk_ip, leader_ip, leader_uid)) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    int types[4];
    int nt = service_types(leader_ip, types);
    char uri[128], *meta = malloc(1024);
    if (!meta) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    sonos_spotify_uri(uri, sizeof(uri), kind, id, 0, 0);
    esp_err_t result = ESP_FAIL;
    for (int i = 0; i < nt && result != ESP_OK; i++) {
        sonos_spotify_didl(meta, 1024, kind, id, types[i]);
        sonos_arg_t clr[] = {{"InstanceID", "0"}};
        soap_simple(leader_ip, AVT, "RemoveAllTracksFromQueue", clr, 1);
        int first = 1;
        int count = enqueue(leader_ip, uri, meta, &first);
        if (count <= 0) continue;
        if (index < 0) index = 0;
        int pos = first + (index < count ? index : count - 1);
        result = play_queue_at(leader_ip, leader_uid, pos, track_id);
        if (result == ESP_OK) remember_type(types[i]);
        ESP_LOGI(TAG, "%s: %d songs queued, playing #%d -> %s", uri, count, pos, result == ESP_OK ? "ok" : "failed");
        // This account type queued the list; the song just isn't in it. Another type would only rebuild the
        // queue (or empty it) and still not find the song.
        if (result == ESP_ERR_NOT_FOUND) break;
    }
    free(meta);
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) s_leader_at = 0;  // the speaker may have moved
    unlock();
    return result;
}

esp_err_t sonos_play_tracks(const char *const *ids, int n)
{
    if (n <= 0) return ESP_ERR_INVALID_ARG;
    lock();
    char spk_ip[16], leader_ip[16], leader_uid[48];
    if (!resolve_leader(spk_ip, leader_ip, leader_uid)) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    int types[4];
    int nt = service_types(leader_ip, types);
    char uri[160], *meta = malloc(1024);
    if (!meta) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_FAIL;
    int type = 0, sn = 0;
    for (int i = 0; i < nt && result != ESP_OK; i++) {
        sonos_spotify_didl(meta, 1024, SONOS_SP_TRACK, ids[0], types[i]);
        // The form with the account serial first (what Sonos itself uses), then the bare form.
        const int sns[] = {s_sn, -1};
        for (int f = (s_sn > 0 ? 0 : 1); f < 2 && result != ESP_OK; f++) {
            sonos_arg_t clr[] = {{"InstanceID", "0"}};
            soap_simple(leader_ip, AVT, "RemoveAllTracksFromQueue", clr, 1);  // each try starts empty: no doubles
            sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_TRACK, ids[0], types[i] >> 8, sns[f]);
            int first = 1;
            if (enqueue(leader_ip, uri, meta, &first) <= 0) continue;
            result = play_queue_at(leader_ip, leader_uid, first, NULL);
            if (result == ESP_OK) {
                type = types[i];
                sn = sns[f];
            }
        }
    }
    if (result == ESP_OK) {
        remember_type(type);
        // The first song is playing; queue the rest behind it.
        for (int k = 1; k < n; k++) {
            sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_TRACK, ids[k], type >> 8, sn);
            sonos_spotify_didl(meta, 1024, SONOS_SP_TRACK, ids[k], type);
            enqueue(leader_ip, uri, meta, NULL);
        }
        ESP_LOGI(TAG, "%d songs queued one by one", n);
    } else {
        s_leader_at = 0;
    }
    free(meta);
    unlock();
    return result;
}

static esp_err_t transport_state(const char *leader_ip, char *state, size_t len)
{
    sonos_arg_t a[] = {{"InstanceID", "0"}};
    http_resp_t r;
    esp_err_t err = soap(leader_ip, AVT, "GetTransportInfo", a, 1, &r, NULL);
    if (err == ESP_OK) {
        if (!kp_xml_tag(r.data, "CurrentTransportState", state, len)) state[0] = 0;
    }
    http_resp_free(&r);
    return err;
}

esp_err_t sonos_toggle_play(void)
{
    lock();
    char spk_ip[16], leader_ip[16], leader_uid[48];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (resolve_leader(spk_ip, leader_ip, leader_uid)) {
        char state[32];
        err = transport_state(leader_ip, state, sizeof(state));
        if (err == ESP_OK) {
            sonos_arg_t a[] = {{"InstanceID", "0"}, {"Speed", "1"}};
            if (strcmp(state, "PLAYING") == 0 || strcmp(state, "TRANSITIONING") == 0)
                err = soap_simple(leader_ip, AVT, "Pause", a, 1);
            else
                err = soap_simple(leader_ip, AVT, "Play", a, 2);
        }
        if (err != ESP_OK) s_leader_at = 0;
    }
    unlock();
    return err;
}

static int get_volume(const char *ip)
{
    sonos_arg_t a[] = {{"InstanceID", "0"}, {"Channel", "Master"}};
    http_resp_t r;
    int vol = -1;
    if (soap(ip, RC, "GetVolume", a, 2, &r, NULL) == ESP_OK) {
        char v[8];
        if (kp_xml_tag(r.data, "CurrentVolume", v, sizeof(v))) vol = atoi(v);
    }
    http_resp_free(&r);
    return vol;
}

esp_err_t sonos_change_volume(int delta, int *vol_out)
{
    lock();
    char ip[16];
    settings_lock();
    kp_strlcpy(ip, g_set.speaker_ip, sizeof(ip));
    int maxv = g_set.max_vol > 0 ? g_set.max_vol : 100;
    settings_unlock();
    int cur = get_volume(ip);
    esp_err_t err = ESP_FAIL;
    if (cur >= 0) {
        int nv = cur + delta;
        if (delta > 0 && nv > maxv) nv = cur > maxv ? cur : maxv;  // never raise above the cap
        if (nv < 0) nv = 0;
        if (nv > 100) nv = 100;
        char v[8];
        snprintf(v, sizeof(v), "%d", nv);
        sonos_arg_t a[] = {{"InstanceID", "0"}, {"Channel", "Master"}, {"DesiredVolume", v}};
        err = nv == cur ? ESP_OK : soap_simple(ip, RC, "SetVolume", a, 3);
        if (vol_out) *vol_out = nv;
    }
    unlock();
    return err;
}

esp_err_t sonos_get_status(sonos_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->volume = -1;
    lock();
    char spk_ip[16], leader_ip[16], leader_uid[48];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (resolve_leader(spk_ip, leader_ip, leader_uid)) {
        char state[32];
        err = transport_state(leader_ip, state, sizeof(state));
        if (err == ESP_OK) {
            st->playing = strcmp(state, "PLAYING") == 0 || strcmp(state, "TRANSITIONING") == 0;
            sonos_arg_t a[] = {{"InstanceID", "0"}};
            http_resp_t r;
            if (soap(leader_ip, AVT, "GetPositionInfo", a, 1, &r, NULL) == ESP_OK) {
                char uri[256];
                if (kp_xml_tag(r.data, "TrackURI", uri, sizeof(uri))) {
                    kp_html_unescape(uri);
                    if (!sonos_track_id_from_uri(uri, st->track_id)) st->track_id[0] = 0;
                }
            }
            http_resp_free(&r);
            st->volume = get_volume(spk_ip);
        } else {
            s_leader_at = 0;
        }
    }
    unlock();
    return err;
}
