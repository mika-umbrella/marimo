/* tags.c — tiny, dependency-free tag parser.
 * FLAC: fLaC magic, STREAMINFO (duration) + VORBIS_COMMENT (title/artist/
 * album/tracknumber/discnumber). MP3: ID3v2.2/2.3/2.4 frames.
 * Everything else returns -1 and callers fall back to mpv or filename order. */
#include "tags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

static int get_u32be(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static int get_u32le(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static int get_u24be(const unsigned char *p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }

/* parse leading digits of "12" / "12/16" / "A1" -> 12 / 12 / -1 */
static int parse_num(const char *s, int len)
{
    int v = 0, n = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); n++; }
        else break;
    }
    return n ? v : -1;
}

static void utf16_to_utf8(const unsigned char *u, int len, int be, char *out, int outsz)
{
    int o = 0;
    for (int i = 0; i + 1 < len && o < outsz - 4; i += 2) {
        int c = be ? (u[i] << 8) | u[i + 1] : u[i] | (u[i + 1] << 8);
        if (c == 0) continue;
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < len) {
            int lo = be ? (u[i + 2] << 8) | u[i + 3] : u[i + 2] | (u[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63));
            out[o++] = (char)(0x80 | (c & 63));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63));
            out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = 0;
}

static void latin1_to_utf8(const unsigned char *s, int len, char *out, int outsz)
{
    int o = 0;
    for (int i = 0; i < len && o < outsz - 4; i++) {
        int c = s[i];
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

static void trim_space(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = 0;
}

/* ID3v2 text frame payload -> utf8 */
static void id3_text(const unsigned char *data, int dlen, char *out, int outsz)
{
    if (dlen < 1) { out[0] = 0; return; }
    int enc = data[0];
    const unsigned char *txt = data + 1;
    int tlen = dlen - 1;
    if (enc == 1) {
        if (tlen >= 2 && txt[0] == 0xFF && txt[1] == 0xFE) { txt += 2; tlen -= 2; utf16_to_utf8(txt, tlen, 0, out, outsz); }
        else if (tlen >= 2 && txt[0] == 0xFE && txt[1] == 0xFF) { txt += 2; tlen -= 2; utf16_to_utf8(txt, tlen, 1, out, outsz); }
        else utf16_to_utf8(txt, tlen, 0, out, outsz);
    } else if (enc == 2) {
        utf16_to_utf8(txt, tlen, 1, out, outsz);
    } else if (enc == 3) {
        if (tlen > outsz - 1) tlen = outsz - 1;
        memcpy(out, txt, tlen);
        out[tlen] = 0;
    } else {
        latin1_to_utf8(txt, tlen, out, outsz);
    }
    trim_space(out);
}

/* ---------------- FLAC ---------------- */

static int flac_parse(FILE *f, Meta *meta, int *track, int *disc)
{
    unsigned char bh[4];
    unsigned char mg[4];
    int track_ok = 0;
    if (fread(mg, 1, 4, f) != 4 || memcmp(mg, "fLaC", 4)) return -1;
    for (;;) {
        if (fread(bh, 1, 4, f) != 4) return -1;
        int last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        int len = get_u24be(bh + 1);
        if (type == 0 && len >= 34) {
            /* STREAMINFO: duration from total_samples / sample_rate */
            unsigned char si[34];
            if (fread(si, 1, 34, f) != 34) return -1;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v = (v << 8) | si[10 + i];
            int rate = (int)(v >> 44);
            uint64_t total = v & ((1ULL << 36) - 1);
            if (meta && rate > 0 && total > 0)
                meta->duration_ms = (int)(total * 1000 / rate);
        } else if (type == 4) {
            /* VORBIS_COMMENT */
            unsigned char *buf = (unsigned char *)malloc(len);
            int off = 0;
            if (fread(buf, 1, len, f) != (size_t)len) { free(buf); return -1; }
            if (len < 4) { free(buf); return -1; }
            off = 4 + get_u32le(buf);
            if (off + 4 > len) { free(buf); return -1; }
            {
                int n = get_u32le(buf + off);
                off += 4;
                for (int i = 0; i < n && off + 4 <= len; i++) {
                    int clen = get_u32le(buf + off);
                    off += 4;
                    if (off + clen > len) break;
                    const unsigned char *c = buf + off;
                    int eq = -1;
                    for (int j = 0; j < clen; j++) if (c[j] == '=') { eq = j; break; }
                    if (eq > 0) {
                        int vl = clen - eq - 1;
                        const char *v = (const char *)c + eq + 1;
                        if (meta && eq == 5 && !strncasecmp((const char *)c, "TITLE", 5) && !meta->title[0]) {
                            memcpy(meta->title, v, vl < (int)sizeof meta->title - 1 ? vl : (int)sizeof meta->title - 1);
                            meta->title[vl < (int)sizeof meta->title - 1 ? vl : (int)sizeof meta->title - 1] = 0;
                        }
                        else if (meta && eq == 6 && !strncasecmp((const char *)c, "ARTIST", 6) && !meta->artist[0]) {
                            memcpy(meta->artist, v, vl < (int)sizeof meta->artist - 1 ? vl : (int)sizeof meta->artist - 1);
                            meta->artist[vl < (int)sizeof meta->artist - 1 ? vl : (int)sizeof meta->artist - 1] = 0;
                        }
                        else if (meta && eq == 5 && !strncasecmp((const char *)c, "ALBUM", 5) && !meta->album[0]) {
                            memcpy(meta->album, v, vl < (int)sizeof meta->album - 1 ? vl : (int)sizeof meta->album - 1);
                            meta->album[vl < (int)sizeof meta->album - 1 ? vl : (int)sizeof meta->album - 1] = 0;
                        }
                        else if (eq == 11 && !strncasecmp((const char *)c, "TRACKNUMBER", 11) && track_ok == 0) {
                            int t = parse_num(v, vl);
                            if (t > 0) { if (track) *track = t; track_ok = 1; }
                        }
                        else if (eq == 10 && !strncasecmp((const char *)c, "DISCNUMBER", 10) && disc && *disc < 0) {
                            int d = parse_num(v, vl);
                            if (d > 0) *disc = d;
                        }
                    }
                    off += clen;
                }
            }
            free(buf);
            return track_ok || (track && *track > 0) || (disc && *disc > 0)
                   || (meta && meta->have_meta) ? 0 : -1;
        } else {
            if (fseek(f, len, SEEK_CUR)) return -1;
        }
        if (last) break;
    }
    return track_ok || (track && *track > 0) ? 0 : -1;
}

/* ---------------- MP3 / ID3v2 ---------------- */

static int id3_parse(FILE *f, Meta *meta, int *track, int *disc)
{
    unsigned char h[10];
    unsigned char *buf;
    int ver, size, off = 0;
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return -1;
    ver = h[3];
    size = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
    if (size <= 0) return -1;
    buf = (unsigned char *)malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) { free(buf); return -1; }
    if ((h[5] & 0x40) && off + 4 <= size) {          /* extended header */
        int eh = get_u32be(buf + off);
        off += 4 + eh;
    }
    while (off + 6 <= size) {
        int idlen = ver == 2 ? 3 : 4;
        int fsize;
        const char *id;
        if (off + idlen + 3 > size) break;
        id = (const char *)buf + off;
        if (id[0] == 0) break;
        if (ver == 2) {
            fsize = get_u24be(buf + off + 3);
        } else {
            fsize = get_u32be(buf + off + 4);
            if (ver == 4) fsize &= 0x0FFFFFFF;
        }
        if (fsize < 0) fsize = 0;
        if (ver == 2) {
            if (meta && !memcmp(id, "TT2", 3) && !meta->title[0]) id3_text(buf + off + 6, fsize, meta->title, sizeof meta->title);
            else if (meta && !memcmp(id, "TP1", 3) && !meta->artist[0]) id3_text(buf + off + 6, fsize, meta->artist, sizeof meta->artist);
            else if (meta && !memcmp(id, "TAL", 3) && !meta->album[0]) id3_text(buf + off + 6, fsize, meta->album, sizeof meta->album);
            else if (!memcmp(id, "TRK", 3)) { int t = parse_num((const char *)buf + off + 7, fsize - 1); if (t > 0 && track) *track = t; }
            else if (!memcmp(id, "TPA", 3)) { int d = parse_num((const char *)buf + off + 7, fsize - 1); if (d > 0 && disc) *disc = d; }
        } else {
            if (meta && !memcmp(id, "TIT2", 4) && !meta->title[0]) id3_text(buf + off + 10, fsize, meta->title, sizeof meta->title);
            else if (meta && !memcmp(id, "TPE1", 4) && !meta->artist[0]) id3_text(buf + off + 10, fsize, meta->artist, sizeof meta->artist);
            else if (meta && !memcmp(id, "TALB", 4) && !meta->album[0]) id3_text(buf + off + 10, fsize, meta->album, sizeof meta->album);
            else if (!memcmp(id, "TRCK", 4)) {
                int t = parse_num((const char *)buf + off + 11, fsize - 1);
                if (t > 0 && track) *track = t;
            }
            else if (!memcmp(id, "TPOS", 4)) {
                int d = parse_num((const char *)buf + off + 11, fsize - 1);
                if (d > 0 && disc) *disc = d;
            }
        }
        off += idlen + (ver == 2 ? 3 : 6) + fsize;
    }
    free(buf);
    if (meta)
        meta->have_meta = meta->title[0] || meta->artist[0] || meta->album[0] || meta->duration_ms > 0;
    return ((meta && meta->have_meta) || (track && *track > 0) || (disc && *disc > 0)) ? 0 : -1;
}

/* ---------------- public ---------------- */

int tag_read_meta(const char *path, Meta *meta)
{
    unsigned char magic[4];
    FILE *f;
    int track = -1, disc = -1;
    int rc = -1;
    memset(meta, 0, sizeof(*meta));
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return -1; }
    rewind(f);
    if (!memcmp(magic, "fLaC", 4)) {
        rc = flac_parse(f, meta, &track, &disc);
    } else if (!memcmp(magic, "ID3", 3)) {
        rc = id3_parse(f, meta, &track, &disc);
    }
    fclose(f);
    meta->have_meta = meta->title[0] || meta->artist[0] || meta->album[0] || meta->duration_ms > 0;
    if (track > 0) meta->track = track;
    if (disc > 0) meta->disc = disc;
    if (rc == 0 && !meta->have_meta) rc = -1;
    return rc;
}

int tag_trackinfo(const char *path, int *track, int *disc)
{
    unsigned char magic[4];
    FILE *f;
    int rc = -1;
    *track = -1;
    *disc = -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return -1; }
    rewind(f);
    if (!memcmp(magic, "fLaC", 4)) rc = flac_parse(f, NULL, track, disc);
    else if (!memcmp(magic, "ID3", 3)) rc = id3_parse(f, NULL, track, disc);
    fclose(f);
    if (*track <= 0 && *disc <= 0) rc = -1;
    return rc;
}
