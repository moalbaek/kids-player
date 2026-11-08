// Host unit tests for the pure parts of the firmware.
// Build: see test/run.sh
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "kp_util.h"
#include "sonos_proto.h"
#include "spotify_parse.h"

static int fails = 0, passes = 0;
#define CHECK(cond) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) == 0) passes++; else { fails++; printf("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    fread(b, 1, n, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static void test_util(void)
{
    char buf[256];
    CHECK(kp_url_encode("playlist-read-private playlist-read-collaborative", buf, sizeof(buf)));
    CHECK_STR(buf, "playlist-read-private%20playlist-read-collaborative");
    CHECK(kp_url_encode("https://moalbaek.github.io/wick/callback.html", buf, sizeof(buf)));
    CHECK_STR(buf, "https%3A%2F%2Fmoalbaek.github.io%2Fwick%2Fcallback.html");
    CHECK(!kp_url_encode("abcdef", buf, 4));

    strcpy(buf, "Kids%3A+Frida+%C3%B8%zz");
    kp_url_decode(buf);
    CHECK_STR(buf, "Kids: Frida \xC3\xB8%zz");

    const char *cases[][2] = {{"", ""}, {"f", "Zg"}, {"fo", "Zm8"}, {"foo", "Zm9v"}, {"foob", "Zm9vYg"},
                              {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"}, {"\xfb\xff\xbf", "-_-_"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        kp_base64url((const uint8_t *)cases[i][0], strlen(cases[i][0]), buf, sizeof(buf));
        CHECK_STR(buf, cases[i][1]);
    }
    // RFC 7636 appendix B: SHA256(verifier) -> challenge
    const uint8_t digest[32] = {0x13,0xd3,0x1e,0x96,0x1a,0x1a,0xd8,0xec,0x2f,0x16,0xb1,0x0c,0x4c,0x98,0x2e,0x08,
                                0x76,0xa8,0x78,0xad,0x6d,0xf1,0x44,0x56,0x6e,0xe1,0x89,0x4a,0xcb,0x70,0xf9,0xc3};
    CHECK(kp_base64url(digest, 32, buf, sizeof(buf)) == 43);
    CHECK_STR(buf, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    CHECK(kp_base64url(digest, 32, buf, 43) == 0);  // no room for NUL

    CHECK(kp_xml_tag("<a><CurrentVolume>12</CurrentVolume></a>", "CurrentVolume", buf, sizeof(buf)));
    CHECK_STR(buf, "12");
    CHECK(kp_xml_tag("<u:X><Result>&lt;DIDL&gt;</Result></u:X>", "Result", buf, sizeof(buf)));
    CHECK_STR(buf, "&lt;DIDL&gt;");
    CHECK(kp_xml_tag("<dc:title attr=\"1\">Hej</dc:title>", "dc:title", buf, sizeof(buf)));
    CHECK_STR(buf, "Hej");
    CHECK(!kp_xml_tag("<Track>1</Track>", "TrackURI", buf, sizeof(buf)));
    CHECK(kp_xml_tag("<TrackURI></TrackURI><Track>5</Track>", "Track", buf, sizeof(buf)));
    CHECK_STR(buf, "5");
    CHECK(kp_xml_tag("<x/><TrackMetaData/>", "TrackMetaData", buf, sizeof(buf)));
    CHECK_STR(buf, "");

    strcpy(buf, "Bamse &amp; Kylling &lt;3 &#229;&#xF8; &bogus; &#;");
    kp_html_unescape(buf);
    CHECK_STR(buf, "Bamse & Kylling <3 \xC3\xA5\xC3\xB8 &bogus; &#;");

    CHECK(kp_xml_escape("a&b<c>\"'", buf, sizeof(buf)));
    CHECK_STR(buf, "a&amp;b&lt;c&gt;&quot;&apos;");
    CHECK(!kp_xml_escape("&&&&", buf, 10));

    const char *rest;
    CHECK(kp_has_prefix_ci("  kids:  Frida", "Kids:", &rest));
    CHECK_STR(rest, "Frida");
    CHECK(!kp_has_prefix_ci("Kid Frida", "Kids:", &rest));
    CHECK(!kp_has_prefix_ci("Anything", "", &rest));

    CHECK(kp_version_cmp("1.2.10", "1.2.9") > 0);
    CHECK(kp_version_cmp("0.1.0", "0.1.0") == 0);
    CHECK(kp_version_cmp("0.1.0", "0.2.0") < 0);
    CHECK(kp_version_cmp("1.0", "1.0.1") < 0);
    CHECK(kp_version_cmp("1.0.0-beta", "1.0.0") == 0);
}

static void test_sonos(void)
{
    char *zgs = read_file("fixtures/zgs.xml");
    kp_html_unescape(zgs);
    sonos_room_t rooms[16];
    int n = sonos_parse_topology(zgs, rooms, 16);
    CHECK(n == 3);
    CHECK_STR(rooms[0].name, "Bathroom");
    CHECK_STR(rooms[0].ip, "192.168.1.40");
    CHECK_STR(rooms[0].coord_ip, "192.168.1.40");
    CHECK_STR(rooms[1].name, "Living Room & Kitchen");
    CHECK_STR(rooms[1].uid, "RINCON_B");
    CHECK_STR(rooms[1].coord_uid, "RINCON_B");
    CHECK_STR(rooms[2].name, "Børneværelse");
    CHECK_STR(rooms[2].coord_uid, "RINCON_B");
    CHECK_STR(rooms[2].coord_ip, "192.168.1.41");
    free(zgs);

    char *didl = read_file("fixtures/favorites.xml");
    kp_html_unescape(didl);
    kp_html_unescape(didl);
    CHECK(sonos_parse_spotify_sid(didl) == 9);
    free(didl);
    CHECK(sonos_parse_spotify_sid("<res>x-rincon-mp3radio:foo</res>") == -1);

    char id[23];
    CHECK(sonos_track_id_from_uri("x-sonos-spotify:spotify%3atrack%3a4uLU6hMCjMI75M1A2tKUQC?sid=9&flags=8224&sn=5", id));
    CHECK_STR(id, "4uLU6hMCjMI75M1A2tKUQC");
    CHECK(sonos_track_id_from_uri("x-sonos-spotify:spotify%3Atrack%3A4uLU6hMCjMI75M1A2tKUQC?sid=12", id));
    CHECK(!sonos_track_id_from_uri("x-rincon-mp3radio://foo", id));
    CHECK(!sonos_track_id_from_uri("x-sonos-spotify:spotify%3atrack%3ashort", id));

    // Queue position lookup (Browse Q:0, unescaped)
    const char *q =
        "<DIDL-Lite><item id=\"Q:0/1\" parentID=\"Q:0\"><res>x-sonos-spotify:spotify%3atrack%3aAAAAAAAAAAAAAAAAAAAAAA?sid=9</res></item>"
        "<item id=\"Q:0/2\" parentID=\"Q:0\"><dc:title>BBBBBBBBBBBBBBBBBBBBBB</dc:title>"
        "<res>x-sonos-spotify:spotify%3Atrack%3ACCCCCCCCCCCCCCCCCCCCCC?sid=9</res></item>"
        "<item id=\"Q:0/3\" parentID=\"Q:0\"><res>x-sonos-spotify:spotify%3atrack%3aBBBBBBBBBBBBBBBBBBBBBB?sid=9</res></item>"
        "</DIDL-Lite>";
    CHECK(sonos_queue_position(q, "AAAAAAAAAAAAAAAAAAAAAA") == 1);
    CHECK(sonos_queue_position(q, "CCCCCCCCCCCCCCCCCCCCCC") == 2);
    CHECK(sonos_queue_position(q, "BBBBBBBBBBBBBBBBBBBBBB") == 3);  // a title that looks like an id is skipped
    CHECK(sonos_queue_position(q, "DDDDDDDDDDDDDDDDDDDDDD") == -1);
    CHECK(sonos_queue_position(q, "short") == -1);
    CHECK(sonos_queue_position("<item id=\"Q:0/12\"><res>spotify:track:EEEEEEEEEEEEEEEEEEEEEE</res></item>",
                               "EEEEEEEEEEEEEEEEEEEEEE") == 12);

    char env[2048];
    sonos_arg_t args[] = {{"InstanceID", "0"}, {"EnqueuedURIMetaData", "<DIDL a=\"1\">&</DIDL>"}};
    size_t l = sonos_build_envelope(env, sizeof(env), "AVTransport", "AddURIToQueue", args, 2);
    CHECK(l > 0 && l == strlen(env));
    CHECK(strstr(env, "<u:AddURIToQueue xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") != NULL);
    CHECK(strstr(env, "<EnqueuedURIMetaData>&lt;DIDL a=&quot;1&quot;&gt;&amp;&lt;/DIDL&gt;</EnqueuedURIMetaData>") != NULL);
    CHECK(sonos_build_envelope(env, 100, "AVTransport", "AddURIToQueue", args, 2) == 0);

    char uri[200], meta[1024];
    CHECK(sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_PLAYLIST, "37i9dQZF1DXcBWIGoYBM5M", 9, 5) > 0);
    CHECK_STR(uri, "x-rincon-cpcontainer:1006206cspotify%3aplaylist%3a37i9dQZF1DXcBWIGoYBM5M");
    CHECK(sonos_spotify_didl(meta, sizeof(meta), SONOS_SP_PLAYLIST, "37i9dQZF1DXcBWIGoYBM5M", 2311) > 0);
    CHECK(strstr(meta, "SA_RINCON2311_X_#Svc2311-0-Token") != NULL);
    CHECK(strstr(meta, "id=\"1006206cspotify%3aplaylist%3a37i9dQZF1DXcBWIGoYBM5M\"") != NULL);
    CHECK(strstr(meta, "<upnp:class>object.container.playlistContainer</upnp:class>") != NULL);
    CHECK(sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_ALBUM, "7ggggggggggggggggggggg", 9, 5) > 0);
    CHECK_STR(uri, "x-rincon-cpcontainer:1004206cspotify%3aalbum%3a7ggggggggggggggggggggg");
    CHECK(sonos_spotify_didl(meta, sizeof(meta), SONOS_SP_ALBUM, "7ggggggggggggggggggggg", 2311) > 0);
    CHECK(strstr(meta, "id=\"00040000spotify%3aalbum%3a7ggggggggggggggggggggg\"") != NULL);
    CHECK(strstr(meta, "object.container.album.musicAlbum") != NULL);
    CHECK(sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_TRACK, "4uLU6hMCjMI75M1A2tKUQC", 9, 5) > 0);
    CHECK_STR(uri, "x-sonos-spotify:spotify%3atrack%3a4uLU6hMCjMI75M1A2tKUQC?sid=9&flags=8224&sn=5");
    CHECK(sonos_spotify_uri(uri, sizeof(uri), SONOS_SP_TRACK, "4uLU6hMCjMI75M1A2tKUQC", 9, -1) > 0);
    CHECK_STR(uri, "spotify%3atrack%3a4uLU6hMCjMI75M1A2tKUQC");
    CHECK(sonos_spotify_didl(meta, sizeof(meta), SONOS_SP_TRACK, "4uLU6hMCjMI75M1A2tKUQC", 2311) > 0);
    CHECK(strstr(meta, "id=\"00032020spotify%3atrack%3a4uLU6hMCjMI75M1A2tKUQC\"") != NULL);
    CHECK(sonos_parse_spotify_sn("<res>x-sonos-spotify:spotify%3atrack%3aX?sid=9&amp;flags=8224&amp;sn=5</res>", 9) == 5);
    CHECK(sonos_parse_spotify_sn("<res>x-sonos-spotify:spotify%3atrack%3aX?sid=12&flags=8224&sn=3</res>", 9) == -1);

    CHECK(sonos_fault_code("<s:Fault><detail><UPnPError><errorCode>800</errorCode></UPnPError></detail></s:Fault>") == 800);
    CHECK(sonos_fault_code("<ok/>") == -1);
}

static void test_spotify(void)
{
    char *js = read_file("fixtures/playlists.json");
    sp_playlist_t pl[8];
    int n = 0;
    char next[256];
    CHECK(sp_parse_playlists(js, "Kids:", "me", pl, &n, 8, next, sizeof(next)));
    CHECK(n == 2);
    CHECK_STR(pl[0].name, "Frida");
    CHECK_STR(pl[0].id, "1aaaaaaaaaaaaaaaaaaaaa");
    CHECK_STR(pl[0].img, "https://i.scdn.co/image/p300");
    CHECK_STR(pl[1].name, "Godnat");
    CHECK_STR(pl[1].img, "https://mosaic.scdn.co/640/abc");  // only one image with null width
    CHECK_STR(next, "https://api.spotify.com/v1/me/playlists?offset=50&limit=50");
    CHECK(pl[0].kind == SP_KIND_PLAYLIST && pl[0].can_list);
    // Full library: no prefix filter; followed playlists can't be listed (only played).
    n = 0;
    CHECK(sp_parse_playlists(js, "", "me", pl, &n, 8, next, sizeof(next)));
    CHECK(n == 4);
    CHECK_STR(pl[0].name, "Kids: Frida");
    CHECK_STR(pl[1].name, "Discover Weekly");
    CHECK(!pl[1].can_list);
    CHECK(pl[2].can_list);  // no owner given: try
    CHECK_STR(pl[1].img, "");
    free(js);

    js = read_file("fixtures/items.json");
    sp_track_t tr[8];
    n = 0;
    int raw = 0;
    CHECK(sp_parse_tracks(js, tr, &n, 8, next, sizeof(next), &raw));
    CHECK(raw == 5);
    CHECK(n == 2);
    CHECK_STR(tr[0].name, "Bamse og Kylling");
    CHECK_STR(tr[0].artist, "Bamse");
    CHECK_STR(tr[0].img, "https://i.scdn.co/image/a300");
    CHECK_STR(tr[1].id, "3ccccccccccccccccccccc");
    CHECK_STR(tr[1].name, "Mester Jakob");
    CHECK_STR(next, "");
    free(js);

    // Album tracks: images come from the album
    n = 0;
    CHECK(sp_parse_album_tracks("{\"items\":[{\"id\":\"5eeeeeeeeeeeeeeeeeeeee\",\"name\":\"Én\",\"track_number\":1,"
                                "\"artists\":[{\"name\":\"Kim\"}]},{\"id\":null,\"name\":\"x\"},"
                                "{\"id\":\"6fffffffffffffffffffff\",\"name\":\"To\",\"track_number\":2}],\"next\":null}",
                                "7ggggggggggggggggggggg", "https://i.scdn.co/image/al", tr, &n, 8, next, sizeof(next)));
    CHECK(n == 2);
    CHECK_STR(tr[0].name, "Én");
    CHECK_STR(tr[0].artist, "Kim");
    CHECK_STR(tr[1].img, "https://i.scdn.co/image/al");
    CHECK_STR(tr[1].album_id, "7ggggggggggggggggggggg");
    CHECK(tr[1].track_no == 2 && tr[1].kind == SP_KIND_TRACK);

    // Saved albums
    n = 0;
    CHECK(sp_parse_saved_albums("{\"items\":[{\"added_at\":\"x\",\"album\":{\"id\":\"7ggggggggggggggggggggg\",\"name\":\"Plade\","
                                "\"artists\":[{\"name\":\"Band\"}],\"images\":[{\"url\":\"u300\",\"width\":300}]}}],"
                                "\"next\":\"https://api.spotify.com/v1/me/albums?offset=50\"}",
                                tr, &n, 8, next, sizeof(next)));
    CHECK(n == 1);
    CHECK(tr[0].kind == SP_KIND_ALBUM);
    CHECK_STR(tr[0].artist, "Band");
    CHECK_STR(tr[0].img, "u300");
    CHECK_STR(next, "https://api.spotify.com/v1/me/albums?offset=50");

    // Search: songs, then albums, then playlists (null entries skipped)
    n = 0;
    CHECK(sp_parse_search("{\"tracks\":{\"items\":[{\"id\":\"2bbbbbbbbbbbbbbbbbbbbb\",\"name\":\"Sang\",\"type\":\"track\","
                          "\"track_number\":4,\"album\":{\"id\":\"7ggggggggggggggggggggg\",\"images\":[]}}]},"
                          "\"albums\":{\"items\":[{\"id\":\"7ggggggggggggggggggggg\",\"name\":\"Plade\"}]},"
                          "\"playlists\":{\"items\":[null,{\"id\":\"1aaaaaaaaaaaaaaaaaaaaa\",\"name\":\"Mix\","
                          "\"owner\":{\"id\":\"someone\",\"display_name\":\"Someone\"}}]}}",
                          "me", tr, &n, 8));
    CHECK(n == 3);
    CHECK(tr[0].kind == SP_KIND_TRACK && tr[0].track_no == 4);
    CHECK_STR(tr[0].album_id, "7ggggggggggggggggggggg");
    CHECK(tr[1].kind == SP_KIND_ALBUM);
    CHECK(tr[2].kind == SP_KIND_PLAYLIST && !tr[2].can_list);
    CHECK_STR(tr[2].artist, "Someone");
    n = 0;
    CHECK(sp_parse_search("{}", "me", tr, &n, 8) && n == 0);

    // Max respected
    js = read_file("fixtures/items.json");
    n = 0;
    CHECK(sp_parse_tracks(js, tr, &n, 1, next, sizeof(next), &raw));
    CHECK(n == 1);
    free(js);

    char a[64], r[64], e[64];
    int exp = 0;
    CHECK(sp_parse_token("{\"access_token\":\"AT\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"RT\"}",
                         a, sizeof(a), r, sizeof(r), &exp));
    CHECK_STR(a, "AT");
    CHECK_STR(r, "RT");
    CHECK(exp == 3600);
    CHECK(sp_parse_token("{\"access_token\":\"AT2\",\"expires_in\":3600}", a, sizeof(a), r, sizeof(r), &exp));
    CHECK_STR(r, "");
    CHECK(!sp_parse_token("{\"error\":\"invalid_grant\"}", a, sizeof(a), r, sizeof(r), &exp));
    sp_parse_error("{\"error\":\"invalid_grant\",\"error_description\":\"Refresh token revoked\"}", e, sizeof(e));
    CHECK_STR(e, "invalid_grant");
    sp_parse_error("{\"error\":{\"status\":401,\"message\":\"The access token expired\"}}", e, sizeof(e));
    CHECK_STR(e, "The access token expired");
    CHECK(!sp_parse_playlists("not json", "Kids:", "me", pl, &n, 8, next, sizeof(next)));
}

int main(void)
{
    test_util();
    test_sonos();
    test_spotify();
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
