#ifndef ROMSET_H
#define ROMSET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROMSET_COUNT 14

typedef struct { const char *name; uint32_t crc; uint32_t size; } rom_entry;
extern const rom_entry ROMSET[ROMSET_COUNT];

uint32_t romset_crc32(uint32_t crc, const void *data, size_t len);

/* Checks a zip's central directory against the expected set (no decompression).
   Returns number of matching files; fills msg with a human-readable summary. */
int romset_check_zip(const char *zip_path, char *msg, size_t msglen);

/* Import from a .zip or a folder (loose files are matched by checksum, so
   renamed files work too). Writes a verified zip to dest_zip.
   Returns true on success; msg explains failure. */
bool romset_import(const char *src, const char *dest_zip, char *msg, size_t msglen);

#endif
