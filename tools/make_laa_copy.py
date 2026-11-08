"""Makes deadspace2_4gb.exe: a copy of deadspace2.exe that may use 4 GB.

    python -I tools/make_laa_copy.py ["D:\\...\\Dead Space 2"]

deadspace2.exe is a 32 bit program without the large-address-aware flag, so
Windows gives it 2 GB, for the game, the levels, the randomizer pool and every
plugin together. Windows reads the flag from the file when the process starts;
nothing can change it later. The copy differs from the original only in that
flag (IMAGE_FILE_LARGE_ADDRESS_AWARE, 0x20 in the COFF characteristics) and the
PE checksum, so the original stays untouched for Steam's file check and
updates. Run it again after a game update: an outdated copy is rebuilt.
"""
import struct
import sys
from pathlib import Path

GAME = Path(r"D:\SteamLibrary\steamapps\common\Dead Space 2")
LAA = 0x20


def pe_checksum(data, checksum_at):
    """The checksum imagehlp computes: 16 bit sums over the file without the checksum field, plus the length."""
    total = 0
    padded = data + b"\0" * (len(data) % 2)
    for (word,) in struct.iter_unpack("<H", padded):
        total += word
        total = (total & 0xFFFF) + (total >> 16)
    # the stored checksum field takes no part
    for offset in (checksum_at, checksum_at + 2):
        (word,) = struct.unpack_from("<H", padded, offset)
        total -= word
        if total < 0:
            total += 0xFFFF
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def laa_image(original):
    data = bytearray(original)
    if data[:2] != b"MZ":
        raise SystemExit("not an executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("no PE header")
    characteristics_at = pe + 0x16
    (characteristics,) = struct.unpack_from("<H", data, characteristics_at)
    struct.pack_into("<H", data, characteristics_at, characteristics | LAA)
    checksum_at = pe + 0x18 + 0x40   # optional header + CheckSum
    (old_sum,) = struct.unpack_from("<I", data, checksum_at)
    if old_sum:
        struct.pack_into("<I", data, checksum_at, pe_checksum(bytes(data), checksum_at))
    return bytes(data), characteristics, old_sum


def main():
    game = Path(sys.argv[1]) if len(sys.argv) > 1 else GAME
    source, target = game / "deadspace2.exe", game / "deadspace2_4gb.exe"
    original = source.read_bytes()
    image, characteristics, old_sum = laa_image(original)
    if characteristics & LAA:
        print("deadspace2.exe already has the flag; no copy needed")
        return
    if target.exists() and target.read_bytes() == image:
        print("%s is up to date" % target.name)
        return
    differing = [i for i in range(len(image)) if image[i] != original[i]]
    if len(differing) > 6:
        raise SystemExit("unexpected: %d bytes would differ" % len(differing))
    target.write_bytes(image)
    print("wrote %s: large-address-aware (characteristics %04X -> %04X), checksum %s; %d bytes differ from the original" % (
        target.name, characteristics, characteristics | LAA, "recomputed" if old_sum else "0 as in the original", len(differing)))


if __name__ == "__main__":
    main()
