#include "kp_util.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

size_t kp_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

bool kp_url_encode(const char *in, char *out, size_t outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (o + 1 >= outlen) return false;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= outlen) return false;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    if (o >= outlen) return false;
    out[o] = 0;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void kp_url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)(hexval(r[1]) * 16 + hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

size_t kp_base64url(const uint8_t *in, size_t inlen, char *out, size_t outlen)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t need = (inlen * 4 + 2) / 3;
    if (need + 1 > outlen) return 0;
    size_t o = 0, i = 0;
    while (i + 2 < inlen) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    if (inlen - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
    } else if (inlen - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
    }
    out[o] = 0;
    return o;
}

const char *kp_xml_tag_ptr(const char *xml, const char *name, size_t *len)
{
    size_t nl = strlen(name);
    const char *p = xml;
    while ((p = strchr(p, '<')) != NULL) {
        if (strncmp(p + 1, name, nl) == 0 && (p[1 + nl] == '>' || p[1 + nl] == ' ' || p[1 + nl] == '\t' ||
                                               p[1 + nl] == '\n' || p[1 + nl] == '/')) {
            const char *gt = strchr(p, '>');
            if (!gt) return NULL;
            if (gt[-1] == '/') {  // <name/> : empty element
                *len = 0;
                return gt + 1;
            }
            const char *content = gt + 1;
            // find matching close tag "</name>"
            const char *c = content;
            while ((c = strstr(c, "</")) != NULL) {
                if (strncmp(c + 2, name, nl) == 0 && c[2 + nl] == '>') {
                    *len = (size_t)(c - content);
                    return content;
                }
                c += 2;
            }
            return NULL;
        }
        p++;
    }
    return NULL;
}

bool kp_xml_tag(const char *xml, const char *name, char *out, size_t outlen)
{
    size_t len = 0;
    const char *p = kp_xml_tag_ptr(xml, name, &len);
    if (!p || outlen == 0) return false;
    if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

bool kp_xml_attr(const char *start, const char *end, const char *attr, char *out, size_t outlen)
{
    size_t al = strlen(attr);
    for (const char *p = start; p && p + al + 2 < end; p++) {
        if ((p == start || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && strncmp(p, attr, al) == 0 &&
            p[al] == '=' && (p[al + 1] == '"' || p[al + 1] == '\'')) {
            char q = p[al + 1];
            const char *v = p + al + 2;
            const char *ve = v;
            while (ve < end && *ve != q) ve++;
            if (ve >= end) return false;
            size_t n = (size_t)(ve - v);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

static size_t utf8_put(char *w, unsigned long cp)
{
    if (cp < 0x80) { w[0] = (char)cp; return 1; }
    if (cp < 0x800) { w[0] = (char)(0xC0 | (cp >> 6)); w[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        w[0] = (char)(0xE0 | (cp >> 12)); w[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); w[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    w[0] = (char)(0xF0 | (cp >> 18)); w[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    w[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); w[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void kp_html_unescape(char *s)
{
    static const struct { const char *ent; char ch; } ents[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '&') {
            bool done = false;
            for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
                size_t l = strlen(ents[i].ent);
                if (strncmp(r, ents[i].ent, l) == 0) {
                    *w++ = ents[i].ch;
                    r += l;
                    done = true;
                    break;
                }
            }
            if (!done && r[1] == '#') {
                char *e = NULL;
                unsigned long cp = (r[2] == 'x' || r[2] == 'X') ? strtoul(r + 3, &e, 16) : strtoul(r + 2, &e, 10);
                if (e && *e == ';' && cp > 0 && cp < 0x110000) {
                    // utf8 output is never longer than the entity text it replaces (&#N; is >= 4 chars)
                    w += utf8_put(w, cp);
                    r = e + 1;
                    done = true;
                }
            }
            if (!done) *w++ = *r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

bool kp_xml_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l >= outlen) return false;
            memcpy(out + o, rep, l);
            o += l;
        } else {
            if (o + 1 >= outlen) return false;
            out[o++] = *p;
        }
    }
    if (o >= outlen) return false;
    out[o] = 0;
    return true;
}

bool kp_has_prefix_ci(const char *s, const char *prefix, const char **rest)
{
    while (*s == ' ') s++;
    size_t pl = strlen(prefix);
    if (pl == 0) return false;
    for (size_t i = 0; i < pl; i++) {
        if (!s[i] || tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
    }
    const char *r = s + pl;
    while (*r == ' ') r++;
    if (rest) *rest = r;
    return true;
}

int kp_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!isdigit((unsigned char)*a) && *a && *a != '.') a++;
        if (!isdigit((unsigned char)*b) && *b && *b != '.') b++;
    }
    return 0;
}
