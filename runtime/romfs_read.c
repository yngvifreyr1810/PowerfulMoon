// Reading the game's files on the host (romfs_read.h): the RomFS image's tables (header: ten u64 - header size, directory
// hash table offset / size, directory table offset / size, file hash table offset / size, file table offset / size,
// data offset; a directory entry: parent, sibling, first child directory, first file, hash sibling, name length,
// name; a file entry: parent, sibling, data offset (u64), data size (u64), hash sibling, name length, name), Yaz0 and
// SARC as the game's own tools write them.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#include "romfs_read.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static bool read_at(FILE *f, uint64_t off, void *dst, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (long long)off, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
#endif
    return fread(dst, 1, n, f) == n;
}

uint8_t *romfs_read_file(const char *image, const char *path, size_t *size) {
    FILE *f = fopen(image, "rb");
    if (!f) return NULL;
    uint8_t head[0x50], *dm = NULL, *fm = NULL, *out = NULL;
    uint64_t h[10];
    if (!read_at(f, 0, head, sizeof head)) goto done;
    for (int i = 0; i < 10; i++) h[i] = le64(head + 8 * i);
    if (h[4] > (64u << 20) || h[8] > (64u << 20)) goto done;
    dm = malloc(h[4] ? h[4] : 1), fm = malloc(h[8] ? h[8] : 1);
    if (!read_at(f, h[3], dm, h[4]) || !read_at(f, h[7], fm, h[8])) goto done;
    uint32_t cur = 0;  // (the root directory)
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (slash) {  // a directory
            uint32_t child = le32(dm + cur + 8);
            while (child != 0xFFFFFFFFu && child + 0x18 <= h[4]) {
                uint32_t nl = le32(dm + child + 0x14);
                if (nl == len && child + 0x18 + nl <= h[4] && !memcmp(dm + child + 0x18, p, len)) break;
                child = le32(dm + child + 4);
            }
            if (child == 0xFFFFFFFFu || child + 0x18 > h[4]) goto done;
            cur = child, p = slash + 1;
            continue;
        }
        uint32_t fe = le32(dm + cur + 0xC);  // the file in this directory
        while (fe != 0xFFFFFFFFu && fe + 0x20 <= h[8]) {
            uint32_t nl = le32(fm + fe + 0x1C);
            if (nl == len && fe + 0x20 + nl <= h[8] && !memcmp(fm + fe + 0x20, p, len)) {
                uint64_t off = le64(fm + fe + 8), n = le64(fm + fe + 0x10);
                if (n > (1ull << 31)) goto done;
                out = malloc(n ? n : 1);
                if (!read_at(f, h[9] + off, out, n)) { free(out); out = NULL; goto done; }
                *size = n;
                goto done;
            }
            fe = le32(fm + fe + 4);
        }
        goto done;
    }
done:
    free(dm);
    free(fm);
    fclose(f);
    return out;
}

bool romfs_visit_files(const char *image, RomfsFileVisitor visit, void *arg) {
    FILE *f = fopen(image, "rb");
    if (!f) return false;
    uint8_t head[0x50], *table = NULL, *data = NULL;
    bool ok = false;
    if (!read_at(f, 0, head, sizeof head)) goto done;
    uint64_t table_off = le64(head + 0x38), table_size = le64(head + 0x40), data_off = le64(head + 0x48);
    if (table_size > (64u << 20) || !table_size) goto done;
    table = malloc((size_t)table_size);
    if (!table || !read_at(f, table_off, table, (size_t)table_size)) goto done;
    size_t capacity = 0;
    for (uint64_t i = 0; i < table_size;) {
        if (table_size - i < 0x20) goto done;
        const uint8_t *entry = table + i;
        uint64_t name_size = le32(entry + 0x1c), next = i + 0x20 + ((name_size + 3) & ~3ull);
        if (next > table_size) goto done;
        i = next;
        uint64_t offset = le64(entry + 8), size = le64(entry + 0x10);
        if (size < 4) continue;
        if (size > (512u << 20) || offset > UINT64_MAX - data_off) goto done;
        if ((size_t)size > capacity) {
            uint8_t *larger = realloc(data, (size_t)size);
            if (!larger) goto done;
            data = larger;
            capacity = (size_t)size;
        }
        if (!read_at(f, data_off + offset, data, (size_t)size) || !visit(data, (size_t)size, arg)) goto done;
    }
    ok = true;
done:
    free(table);
    free(data);
    fclose(f);
    return ok;
}

uint8_t *yaz0_decode(const uint8_t *in, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(in, "Yaz0", 4)) return NULL;
    size_t size = (size_t)in[4] << 24 | (size_t)in[5] << 16 | (size_t)in[6] << 8 | in[7], i = 16, o = 0;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    while (o < size && i < n) {
        uint8_t code = in[i++];
        for (int b = 0; b < 8 && o < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= n) goto bad;
                out[o++] = in[i++];
                continue;
            }
            if (i + 1 >= n) goto bad;
            uint8_t b1 = in[i], b2 = in[i + 1];
            i += 2;
            size_t dist = ((size_t)(b1 & 0xF) << 8 | b2) + 1, len = b1 >> 4;
            if (!len) {
                if (i >= n) goto bad;
                len = (size_t)in[i++] + 0x12;
            } else len += 2;
            if (dist > o) goto bad;
            while (len-- && o < size) { out[o] = out[o - dist]; o++; }
        }
    }
    if (o != size) goto bad;
    *out_n = size;
    return out;
bad:
    free(out);
    return NULL;
}

// SARC: "SARC", header size, byte order mark, file size, data offset, version; SFAT: "SFAT", header size, node count,
// hash key, then per node: name hash, attributes (name offset / 4 in the low 16 bits), data start, data end; SFNT:
// "SFNT", header size, then the names. (The game's archives are little-endian.)
const uint8_t *sarc_find(const uint8_t *d, size_t n, const char *name, size_t *size) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE) return NULL;
    uint32_t data = le32(d + 0xC), o = 0x14;
    if (memcmp(d + o, "SFAT", 4)) return NULL;
    uint32_t count = (uint32_t)d[o + 6] | (uint32_t)d[o + 7] << 8, names = o + 0xC + count * 16;
    if ((size_t)names + 8 > n || memcmp(d + names, "SFNT", 4)) return NULL;
    names += 8;
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = d + o + 0xC + k * 16;
        uint32_t no = names + (le32(e + 4) & 0xFFFF) * 4, s = le32(e + 8), t = le32(e + 12);
        if (no >= n || (size_t)data + t > n || s > t) continue;
        if (!strncmp((const char *)d + no, name, n - no)) { *size = t - s; return d + data + s; }
    }
    return NULL;
}

bool sarc_visit_files(const uint8_t *d, size_t n, RomfsFileVisitor visit, void *arg) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xff || d[7] != 0xfe || memcmp(d + 0x14, "SFAT", 4)) return false;
    uint32_t count = (uint32_t)d[0x1a] | (uint32_t)d[0x1b] << 8;
    size_t data = le32(d + 0xc);
    if (data < 0x20 + (size_t)count * 16 || data > n) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = d + 0x20 + 16 * i;
        size_t start = le32(entry + 8), end = le32(entry + 12);
        if (start > end || end > n - data) return false;
        if (!visit(d + data + start, end - start, arg)) return false;
    }
    return true;
}
