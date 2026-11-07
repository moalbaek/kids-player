// Small pure helpers (no ESP-IDF dependencies) so they can be unit-tested on a PC.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Safe string copy that always NUL-terminates. Returns strlen(src).
size_t kp_strlcpy(char *dst, const char *src, size_t size);

// Percent-encode everything except unreserved characters (RFC 3986). Returns false if it did not fit.
bool kp_url_encode(const char *in, char *out, size_t outlen);

// Decode %XX and '+' (form encoding) in place.
void kp_url_decode(char *s);

// Base64url without padding. Returns output length, or 0 if it did not fit.
size_t kp_base64url(const uint8_t *in, size_t inlen, char *out, size_t outlen);

// Find the first <name ...>content</name> and copy the raw content. Returns false if not found.
bool kp_xml_tag(const char *xml, const char *name, char *out, size_t outlen);

// Same as kp_xml_tag but returns a pointer into xml and the length (no copy).
const char *kp_xml_tag_ptr(const char *xml, const char *name, size_t *len);

// Copy attr="value" from the attribute text between start and end. Returns false if not found.
bool kp_xml_attr(const char *start, const char *end, const char *attr, char *out, size_t outlen);

// Decode &lt; &gt; &amp; &quot; &apos; &#NN; &#xNN; in place (numeric entities become UTF-8).
void kp_html_unescape(char *s);

// Escape & < > " ' for XML text. Returns false if it did not fit.
bool kp_xml_escape(const char *in, char *out, size_t outlen);

// Case-insensitive prefix test. On match, *rest points to the text after the prefix with leading spaces skipped.
bool kp_has_prefix_ci(const char *s, const char *prefix, const char **rest);

// Compare dotted versions like "1.2.10" vs "1.2.9". Returns <0, 0, >0.
int kp_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
