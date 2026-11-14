#!/usr/bin/env python3
"""Inject a verified Smash T.V. ROM set into the base APK as assets/smashtv.zip.

Produces an UNSIGNED apk; bundle-rom.sh signs it afterwards. The ROM never leaves your machine.
Usage: bundle_rom.py <base SmashTV.apk> <your smashtv.zip> <out unsigned.apk>
"""
import sys, zipfile, struct

# MAME "smashtv" set: name -> (crc32, uncompressed size). Must match src/romset.c.
ROMSET = {
    "la1.u106": (0x5c718361, 131072), "la1.u107": (0x0fba1e36, 131072),
    "la1.u108": (0xcb0a092f, 131072), "la1.u111": (0x72f0ba84, 131072),
    "la1.u112": (0x436f0283, 131072), "la1.u113": (0x4a4b8110, 131072),
    "la1.u95":  (0xe864a44b, 131072), "la1.u96":  (0x15555ea7, 131072),
    "la1.u97":  (0xccac9d9e, 131072), "sl2.u19":  (0xac5a402a, 65536),
    "sl2.u20":  (0x875c66d9, 65536),  "sl2.u4":   (0x29d3f6c8, 65536),
    "u105-la8": (0x48cd793f, 131072), "u89-la8":  (0x8e7fe463, 131072),
}


def verify(rom_zip):
    """Return True if the zip's central directory contains the full set (by CRC+size, no decompress)."""
    try:
        zf = zipfile.ZipFile(rom_zip)
    except Exception as e:
        sys.exit(f"'{rom_zip}' is not a valid zip file: {e}")
    have = {}
    for info in zf.infolist():
        base = info.filename.rsplit("/", 1)[-1].rsplit("\\", 1)[-1].lower()
        for name, (crc, size) in ROMSET.items():
            if base == name and info.CRC == crc and info.file_size == size:
                have[name] = True
    missing = [n for n in ROMSET if n not in have]
    if missing:
        print("This zip is missing or has the wrong version of these ROM files:")
        print("   " + ", ".join(missing))
        print("\nYou need the MAME 'smashtv' (rev 8.00) ROM set — the same smashtv.zip the")
        print("desktop and phone apps accept when you pick the file by hand.")
        sys.exit(1)
    return True


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base_apk, rom_zip, out_apk = sys.argv[1:4]
    verify(rom_zip)
    with open(rom_zip, "rb") as f:
        rom_bytes = f.read()

    src = zipfile.ZipFile(base_apk)
    with zipfile.ZipFile(out_apk, "w") as dst:
        added = False
        for info in src.infolist():
            name = info.filename
            if name.startswith("META-INF/") and name.rsplit(".", 1)[-1] in ("RSA", "DSA", "EC", "SF", "MF"):
                continue                                   # drop the old signature
            if name == "assets/smashtv.zip":
                continue                                   # replace any existing bundled ROM
            data = src.read(name)
            # preserve each entry's own compression (native libs stay as the build left them)
            ni = zipfile.ZipInfo(name, date_time=info.date_time)
            ni.compress_type = info.compress_type
            ni.external_attr = info.external_attr
            ni.internal_attr = info.internal_attr
            ni.create_system = info.create_system
            dst.writestr(ni, data)
        # the ROM: stored (already compact), so the app reads it straight from the asset
        ri = zipfile.ZipInfo("assets/smashtv.zip")
        ri.compress_type = zipfile.ZIP_STORED
        dst.writestr(ri, rom_bytes)
        added = True
    src.close()
    if not added:
        sys.exit("Failed to add the ROM to the APK.")
    print(f">> ROM verified and injected -> {out_apk}")


if __name__ == "__main__":
    main()
