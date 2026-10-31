/* ROM set verification and import. Uses SDL3 only for file I/O. */
#include "romset.h"
#include <SDL3/SDL.h>
#include <string.h>

/* MAME "smashtv" (rev 8.00) set: file name, CRC32, size. */
const rom_entry ROMSET[ROMSET_COUNT] = {
    {"la1.u106", 0x5c718361, 131072}, {"la1.u107", 0x0fba1e36, 131072},
    {"la1.u108", 0xcb0a092f, 131072}, {"la1.u111", 0x72f0ba84, 131072},
    {"la1.u112", 0x436f0283, 131072}, {"la1.u113", 0x4a4b8110, 131072},
    {"la1.u95",  0xe864a44b, 131072}, {"la1.u96",  0x15555ea7, 131072},
    {"la1.u97",  0xccac9d9e, 131072}, {"sl2.u19",  0xac5a402a, 65536},
    {"sl2.u20",  0x875c66d9, 65536},  {"sl2.u4",   0x29d3f6c8, 65536},
    {"u105-la8", 0x48cd793f, 131072}, {"u89-la8",  0x8e7fe463, 131072},
};

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t romset_crc32(uint32_t crc, const void *data, size_t len) {
    crc_init();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *base_name(const char *s, size_t len, size_t *outlen) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) if (s[i] == '/' || s[i] == '\\') start = i + 1;
    *outlen = len - start;
    return s + start;
}

/* found[i] set when ROMSET[i] present with right CRC. Returns count found, or -1 if not a zip. */
static int scan_zip(const uint8_t *z, size_t n, bool found[ROMSET_COUNT]) {
    if (n < 22) return -1;
    size_t lim = n > 65557 ? n - 65557 : 0, eocd = (size_t)-1;
    for (size_t i = n - 22 + 1; i-- > lim;)
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd == (size_t)-1) return -1;
    uint32_t entries = rd16(z + eocd + 10), cd = rd32(z + eocd + 16);
    int count = 0;
    size_t p = cd;
    for (uint32_t e = 0; e < entries; e++) {
        if (p + 46 > n || rd32(z + p) != 0x02014b50) return -1;
        uint32_t crc = rd32(z + p + 16), usize = rd32(z + p + 24);
        uint16_t nl = rd16(z + p + 28), xl = rd16(z + p + 30), cl = rd16(z + p + 32);
        if (p + 46 + nl > n) return -1;
        size_t bl;
        const char *bn = base_name((const char *)z + p + 46, nl, &bl);
        for (int i = 0; i < ROMSET_COUNT; i++) {
            if (!found[i] && strlen(ROMSET[i].name) == bl && SDL_strncasecmp(bn, ROMSET[i].name, bl) == 0
                && crc == ROMSET[i].crc && usize == ROMSET[i].size) {
                found[i] = true;
                count++;
            }
        }
        p += 46 + nl + xl + cl;
    }
    return count;
}

static void list_missing(const bool found[ROMSET_COUNT], char *msg, size_t msglen) {
    size_t used = strlen(msg);
    int shown = 0;
    for (int i = 0; i < ROMSET_COUNT && used + 16 < msglen; i++) {
        if (found[i]) continue;
        if (shown == 4) { SDL_snprintf(msg + used, msglen - used, " ..."); break; }
        SDL_snprintf(msg + used, msglen - used, "%s%s", shown ? ", " : " ", ROMSET[i].name);
        used = strlen(msg);
        shown++;
    }
}

int romset_check_zip(const char *zip_path, char *msg, size_t msglen) {
    bool found[ROMSET_COUNT] = {0};
    size_t n = 0;
    uint8_t *z = SDL_LoadFile(zip_path, &n);
    if (!z) { SDL_snprintf(msg, msglen, "Can't read %s", zip_path); return 0; }
    int c = scan_zip(z, n, found);
    SDL_free(z);
    if (c < 0) { SDL_snprintf(msg, msglen, "Not a valid zip file."); return 0; }
    if (c == ROMSET_COUNT) SDL_snprintf(msg, msglen, "All %d ROM files verified.", ROMSET_COUNT);
    else {
        SDL_snprintf(msg, msglen, "Only %d of %d ROM files match. Missing or wrong:", c, ROMSET_COUNT);
        list_missing(found, msg, msglen);
    }
    return c;
}

/* ---- writing a "stored" (uncompressed) zip ---- */
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static bool write_zip(const char *dest, void *data[ROMSET_COUNT]) {
    size_t total = 22;
    for (int i = 0; i < ROMSET_COUNT; i++) {
        size_t nl = strlen(ROMSET[i].name);
        total += 30 + nl + ROMSET[i].size + 46 + nl;
    }
    uint8_t *buf = SDL_calloc(1, total), *p = buf;
    if (!buf) return false;
    uint32_t offs[ROMSET_COUNT];
    for (int i = 0; i < ROMSET_COUNT; i++) {
        size_t nl = strlen(ROMSET[i].name);
        offs[i] = (uint32_t)(p - buf);
        wr32(p, 0x04034b50); wr16(p + 4, 10); wr16(p + 6, 0); wr16(p + 8, 0);
        wr16(p + 10, 0); wr16(p + 12, 0x21); /* 1980-01-01 */
        wr32(p + 14, ROMSET[i].crc); wr32(p + 18, ROMSET[i].size); wr32(p + 22, ROMSET[i].size);
        wr16(p + 26, (uint16_t)nl); wr16(p + 28, 0);
        memcpy(p + 30, ROMSET[i].name, nl);
        memcpy(p + 30 + nl, data[i], ROMSET[i].size);
        p += 30 + nl + ROMSET[i].size;
    }
    uint8_t *cd = p;
    for (int i = 0; i < ROMSET_COUNT; i++) {
        size_t nl = strlen(ROMSET[i].name);
        wr32(p, 0x02014b50); wr16(p + 4, 20); wr16(p + 6, 10); wr16(p + 8, 0); wr16(p + 10, 0);
        wr16(p + 12, 0); wr16(p + 14, 0x21);
        wr32(p + 16, ROMSET[i].crc); wr32(p + 20, ROMSET[i].size); wr32(p + 24, ROMSET[i].size);
        wr16(p + 28, (uint16_t)nl); wr32(p + 42, offs[i]);
        memcpy(p + 46, ROMSET[i].name, nl);
        p += 46 + nl;
    }
    wr32(p, 0x06054b50); wr16(p + 8, ROMSET_COUNT); wr16(p + 10, ROMSET_COUNT);
    wr32(p + 12, (uint32_t)(p - cd)); wr32(p + 16, (uint32_t)(cd - buf));
    p += 22;
    bool ok = SDL_SaveFile(dest, buf, (size_t)(p - buf));
    SDL_free(buf);
    return ok;
}

/* Recursively collect files whose size matches a ROM and whose CRC matches. */
static void scan_dir(const char *dir, void *data[ROMSET_COUNT], int depth, const char *dest_zip, char *zipcand, size_t zclen) {
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    if (!names) return;
    for (int k = 0; k < n; k++) {
        char path[2048];
        SDL_snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info)) continue;
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            if (depth < 2) scan_dir(path, data, depth + 1, dest_zip, zipcand, zclen);
            continue;
        }
        if (info.type != SDL_PATHTYPE_FILE) continue;
        size_t len = strlen(names[k]);
        if (len > 4 && SDL_strcasecmp(names[k] + len - 4, ".zip") == 0) {
            if (!zipcand[0] && SDL_strcasestr(names[k], "smash")) SDL_strlcpy(zipcand, path, zclen);
            continue;
        }
        if (info.size != 65536 && info.size != 131072) continue;
        size_t sz = 0;
        void *d = SDL_LoadFile(path, &sz);
        if (!d) continue;
        uint32_t crc = romset_crc32(0, d, sz);
        bool used = false;
        for (int i = 0; i < ROMSET_COUNT; i++)
            if (!data[i] && ROMSET[i].crc == crc && ROMSET[i].size == sz) { data[i] = d; used = true; break; }
        if (!used) SDL_free(d);
    }
    SDL_free(names);
}

bool romset_import(const char *src, const char *dest_zip, char *msg, size_t msglen) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(src, &info)) { SDL_snprintf(msg, msglen, "Can't open %s", src); return false; }
    if (info.type == SDL_PATHTYPE_FILE) {
        if (romset_check_zip(src, msg, msglen) != ROMSET_COUNT) return false;
        size_t n = 0;
        void *z = SDL_LoadFile(src, &n);
        bool ok = z && SDL_SaveFile(dest_zip, z, n);
        SDL_free(z);
        if (!ok) SDL_snprintf(msg, msglen, "Couldn't copy the zip: %s", SDL_GetError());
        return ok;
    }
    void *data[ROMSET_COUNT] = {0};
    char zipcand[2048] = "";
    scan_dir(src, data, 0, dest_zip, zipcand, sizeof zipcand);
    bool found[ROMSET_COUNT];
    int c = 0;
    for (int i = 0; i < ROMSET_COUNT; i++) { found[i] = data[i] != NULL; c += found[i]; }
    bool ok = false;
    if (c == ROMSET_COUNT) {
        ok = write_zip(dest_zip, data);
        if (!ok) SDL_snprintf(msg, msglen, "Couldn't write %s: %s", dest_zip, SDL_GetError());
    } else if (zipcand[0]) {
        ok = romset_import(zipcand, dest_zip, msg, msglen);
    } else {
        SDL_snprintf(msg, msglen, "Found %d of %d ROM files in that folder. Missing:", c, ROMSET_COUNT);
        list_missing(found, msg, msglen);
    }
    for (int i = 0; i < ROMSET_COUNT; i++) SDL_free(data[i]);
    return ok;
}
