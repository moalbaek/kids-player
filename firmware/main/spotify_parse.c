#include "spotify_parse.h"
#include "kp_util.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// Choose the image whose width is closest to 300 px (Spotify usually offers 640/300/64).
static void best_image(const cJSON *images, char *out, size_t outlen)
{
    out[0] = 0;
    int best = 1 << 30;
    const cJSON *img;
    cJSON_ArrayForEach(img, images) {
        const char *url = jstr(img, "url");
        if (!url) continue;
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(img, "width");
        int width = cJSON_IsNumber(w) ? w->valueint : 300;  // null width = single image of unknown size
        int d = abs(width - 300);
        if (width < 200) d += 1000;  // avoid tiny thumbnails unless nothing else
        if (d < best) {
            best = d;
            kp_strlcpy(out, url, outlen);
        }
    }
}

static void copy_next(const cJSON *root, char *next, size_t nextlen)
{
    const char *n = jstr(root, "next");
    kp_strlcpy(next, n ? n : "", nextlen);
}

static void first_artist(const cJSON *obj, char *out, size_t outlen)
{
    out[0] = 0;
    const cJSON *a0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, "artists"), 0);
    const char *an = a0 ? jstr(a0, "name") : NULL;
    if (an) kp_strlcpy(out, an, outlen);
}

static bool valid_id(const char *id) { return id && strlen(id) == 22; }

static sp_entry_t *new_entry(sp_entry_t *out, int *count, sp_kind_t kind)
{
    sp_entry_t *e = &out[(*count)++];
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    return e;
}

// A playlist object (from /me/playlists or search). Returns false if it was skipped.
static bool add_playlist(const cJSON *pl, const char *prefix, const char *me_id, sp_entry_t *out, int *count)
{
    if (!cJSON_IsObject(pl)) return false;  // search results can contain null entries
    const char *id = jstr(pl, "id");
    const char *name = jstr(pl, "name");
    if (!valid_id(id) || !name) return false;
    const char *rest = NULL;
    if (prefix && prefix[0] && !kp_has_prefix_ci(name, prefix, &rest)) return false;
    sp_entry_t *p = new_entry(out, count, SP_KIND_PLAYLIST);
    kp_strlcpy(p->id, id, sizeof(p->id));
    kp_strlcpy(p->name, (rest && *rest) ? rest : name, sizeof(p->name));
    best_image(cJSON_GetObjectItemCaseSensitive(pl, "images"), p->img, sizeof(p->img));
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(pl, "owner");
    const char *owner_id = owner ? jstr(owner, "id") : NULL;
    const char *owner_name = owner ? jstr(owner, "display_name") : NULL;
    if (owner_name) kp_strlcpy(p->artist, owner_name, sizeof(p->artist));
    p->can_list = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(pl, "collaborative")) || !me_id || !me_id[0] ||
                  !owner_id || strcmp(owner_id, me_id) == 0;
    return true;
}

// A full track object (with album). Returns false if it was skipped.
static bool add_track(const cJSON *t, sp_entry_t *out, int *count)
{
    if (!cJSON_IsObject(t)) return false;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "is_local"))) return false;
    const char *type = jstr(t, "type");
    if (type && strcmp(type, "track") != 0) return false;
    const char *id = jstr(t, "id");
    const char *name = jstr(t, "name");
    if (!valid_id(id) || !name) return false;
    sp_entry_t *tr = new_entry(out, count, SP_KIND_TRACK);
    kp_strlcpy(tr->id, id, sizeof(tr->id));
    kp_strlcpy(tr->name, name, sizeof(tr->name));
    first_artist(t, tr->artist, sizeof(tr->artist));
    const cJSON *no = cJSON_GetObjectItemCaseSensitive(t, "track_number");
    tr->track_no = cJSON_IsNumber(no) ? (int16_t)no->valueint : 0;
    const cJSON *album = cJSON_GetObjectItemCaseSensitive(t, "album");
    const char *aid = album ? jstr(album, "id") : NULL;
    if (valid_id(aid)) kp_strlcpy(tr->album_id, aid, sizeof(tr->album_id));
    best_image(cJSON_GetObjectItemCaseSensitive(album, "images"), tr->img, sizeof(tr->img));
    return true;
}

static bool add_album(const cJSON *al, sp_entry_t *out, int *count)
{
    if (!cJSON_IsObject(al)) return false;
    const char *id = jstr(al, "id");
    const char *name = jstr(al, "name");
    if (!valid_id(id) || !name) return false;
    sp_entry_t *a = new_entry(out, count, SP_KIND_ALBUM);
    kp_strlcpy(a->id, id, sizeof(a->id));
    kp_strlcpy(a->name, name, sizeof(a->name));
    first_artist(al, a->artist, sizeof(a->artist));
    best_image(cJSON_GetObjectItemCaseSensitive(al, "images"), a->img, sizeof(a->img));
    return true;
}

bool sp_parse_playlists(const char *json, const char *prefix, const char *me_id, sp_entry_t *out, int *count,
                        int max, char *next, size_t nextlen)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    copy_next(root, next, nextlen);
    const cJSON *pl;
    cJSON_ArrayForEach(pl, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (*count >= max) break;
        add_playlist(pl, prefix, me_id, out, count);
    }
    cJSON_Delete(root);
    return true;
}

bool sp_parse_tracks(const char *json, sp_entry_t *out, int *count, int max, char *next, size_t nextlen,
                     int *raw_items)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    copy_next(root, next, nextlen);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (raw_items) *raw_items = cJSON_GetArraySize(items);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, items) {
        if (*count >= max) break;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "is_local"))) continue;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(entry, "item");
        if (!cJSON_IsObject(t)) t = cJSON_GetObjectItemCaseSensitive(entry, "track");
        add_track(t, out, count);
    }
    cJSON_Delete(root);
    return true;
}

bool sp_parse_album_tracks(const char *json, const char *album_id, const char *album_img, sp_entry_t *out,
                           int *count, int max, char *next, size_t nextlen)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    copy_next(root, next, nextlen);
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (*count >= max) break;
        const char *id = jstr(t, "id");
        const char *name = jstr(t, "name");
        if (!valid_id(id) || !name) continue;
        sp_entry_t *tr = new_entry(out, count, SP_KIND_TRACK);
        kp_strlcpy(tr->id, id, sizeof(tr->id));
        kp_strlcpy(tr->name, name, sizeof(tr->name));
        first_artist(t, tr->artist, sizeof(tr->artist));
        const cJSON *no = cJSON_GetObjectItemCaseSensitive(t, "track_number");
        tr->track_no = cJSON_IsNumber(no) ? (int16_t)no->valueint : 0;
        kp_strlcpy(tr->album_id, album_id ? album_id : "", sizeof(tr->album_id));
        kp_strlcpy(tr->img, album_img ? album_img : "", sizeof(tr->img));
    }
    cJSON_Delete(root);
    return true;
}

bool sp_parse_saved_albums(const char *json, sp_entry_t *out, int *count, int max, char *next, size_t nextlen)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    copy_next(root, next, nextlen);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (*count >= max) break;
        add_album(cJSON_GetObjectItemCaseSensitive(entry, "album"), out, count);
    }
    cJSON_Delete(root);
    return true;
}

bool sp_parse_search(const char *json, const char *me_id, sp_entry_t *out, int *count, int max)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "tracks"), "items")) {
        if (*count >= max) break;
        add_track(o, out, count);
    }
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "albums"), "items")) {
        if (*count >= max) break;
        add_album(o, out, count);
    }
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "playlists"), "items")) {
        if (*count >= max) break;
        add_playlist(o, NULL, me_id, out, count);
    }
    cJSON_Delete(root);
    return true;
}

bool sp_parse_token(const char *json, char *access, size_t alen, char *refresh, size_t rlen, int *expires_in)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const char *a = jstr(root, "access_token");
    const char *r = jstr(root, "refresh_token");
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    bool ok = a != NULL;
    if (a) kp_strlcpy(access, a, alen);
    if (refresh && rlen) {
        if (r) kp_strlcpy(refresh, r, rlen);
        else refresh[0] = 0;
    }
    if (expires_in) *expires_in = cJSON_IsNumber(e) ? e->valueint : 3600;
    cJSON_Delete(root);
    return ok;
}

void sp_parse_error(const char *json, char *err, size_t errlen)
{
    err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(e)) {
        kp_strlcpy(err, e->valuestring, errlen);
    } else if (cJSON_IsObject(e)) {
        const char *m = jstr(e, "message");
        if (m) kp_strlcpy(err, m, errlen);
    }
    cJSON_Delete(root);
}
