"""Own sounds for a skinned variant: WAV -> XAS1 -> a private copy of a bank.

A DS2 sound bank (sbnk) is the DS1 layout: header, event chain, two sorted
lookup tables, then "sdat" with one entry per sound. Each sound is an 8 byte
SNR header (big endian: version/codec/channels/rate, type/samples) followed
by the codec payload. Retail uses codec 7 (EALayer3 v2), which nothing
encodes; the engine also carries the XAS1 decoder (codec 4), which
EARSAudio's ears_cli.exe encodes from 16 bit PCM WAV.

The copy gets a new name (its hash sits at +0x54), a new path and identity
(64 bit sdbm of the path), and the events the animation bank names are
renamed with a prefix so a lookup by name hash can only find this bank. The
event table at header word 17 (+0x18, count in word 18) is a sorted binary
search table and is sorted again after the renaming.
"""
import os
import struct
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EARS_CLI = HERE / "ears_cli.exe"
SNU_HEADER = bytes.fromhex("03000001e803000020000000" "00000000")
NAME_HASH_AT = 0x54
ROW = 24


def name_hash(text, bits=32):
    value, mask = 0, (1 << bits) - 1
    for char in text.lower():
        value = (value * 0x1003F + ord(char)) & mask
    return value


def encode_wav(path):
    """One bank sound entry (SNR header + XAS1 payload) from a WAV file."""
    if not EARS_CLI.exists():
        raise ValueError("encoder missing: " + str(EARS_CLI))
    with tempfile.TemporaryDirectory() as folder:
        snu = os.path.join(folder, "sound.exa.snu")
        result = subprocess.run([str(EARS_CLI), "encode", str(path), snu], capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(snu):
            raise ValueError("encoding %s failed: %s %s" % (path, result.stdout.strip(), result.stderr.strip()))
        data = open(snu, "rb").read()
    body_offset = struct.unpack_from("<I", data, 8)[0]
    h1, h2 = struct.unpack_from(">II", data, 16)
    if (h1 >> 24) & 15 != 4:
        raise ValueError("encoder did not produce XAS1 for " + str(path))
    h2 &= ~(3 << 30)   # bank sounds are type 0 (RAM), the file was type 1 (stream)
    return struct.pack(">II", h1, h2) + data[body_offset:]


def bank_parts(payload):
    """(header words, sdat offset, [(index, offset)], sound count)."""
    if payload[:4] != b"sbnk":
        raise ValueError("not a sound bank")
    header = list(struct.unpack_from("<24I", payload, 0))
    sdat = header[4]
    if payload[sdat:sdat + 4] != b"sdat":
        raise ValueError("no sdat header at %d" % sdat)
    count = struct.unpack_from("<I", payload, sdat + 4)[0]
    entries = [struct.unpack_from("<4I", payload, sdat + 8 + i * 16) for i in range(count)]
    for index, marker, _offset, end in entries:
        if marker != 0x0313BABE or end != 0xFEEDFEED:
            raise ValueError("sound entry %d is not shaped like the others" % index)
    return header, sdat, [(e[0], e[2]) for e in entries], count


def sounds_of(payload):
    header, sdat, entries, count = bank_parts(payload)
    out = []
    for i in range(count):
        start = sdat + entries[i][1]
        end = sdat + entries[i + 1][1] if i + 1 < count else len(payload)
        out.append(payload[start:end])
    return out


def event_table(payload):
    """(offset, count) of the sorted event hash table."""
    header = struct.unpack_from("<24I", payload, 0)
    return header[17] + 0x18, header[18]


def event_hashes(payload):
    at, count = event_table(payload)
    return [struct.unpack_from("<I", payload, at + i * ROW)[0] for i in range(count)]


def rename_event(name, rename):
    """The private name of an event: every key of `rename` replaced by its
    value. Same length names keep the animation bank's block size."""
    out = name
    for old, new in rename.items():
        out = out.replace(old, new)
    return out


def build_private_bank(payload, new_stem, wav_dir, wav_stem, event_names, rename):
    """A copy of `payload` named `new_stem` with the sounds replaced from
    `wav_dir/<wav_stem>_<index>.wav` where such a file exists, and every event
    of `event_names` that this bank holds renamed through `rename`.

    Returns (new payload, {old event name: new event name}, replaced count)."""
    header, sdat, entries, count = bank_parts(payload)
    sounds = sounds_of(payload)
    replaced = 0
    for i in range(count):
        wav = Path(wav_dir) / ("%s_%02d.wav" % (wav_stem, i))
        if wav.exists():
            sounds[i] = encode_wav(wav)
            replaced += 1
    prefix_bytes = bytearray(payload[:sdat])
    table_at, table_count = event_table(payload)
    keys = event_hashes(payload)
    renamed = {}
    for name in event_names:
        old = name_hash(name)
        if old not in keys:
            continue
        new_name = rename_event(name, rename)
        if new_name == name:
            raise ValueError("rename leaves event %s unchanged" % name)
        new = name_hash(new_name)
        needle, replacement = struct.pack("<I", old), struct.pack("<I", new)
        if replacement in bytes(prefix_bytes):
            raise ValueError("renamed event hash %08X already present in %s" % (new, new_stem))
        prefix_bytes = bytearray(bytes(prefix_bytes).replace(needle, replacement))
        renamed[name] = new_name
    rows = [bytes(prefix_bytes[table_at + i * ROW:table_at + (i + 1) * ROW]) for i in range(table_count)]
    rows.sort(key=lambda row: struct.unpack_from("<I", row)[0])
    prefix_bytes[table_at:table_at + table_count * ROW] = b"".join(rows)
    struct.pack_into("<I", prefix_bytes, NAME_HASH_AT, name_hash(new_stem))
    out = bytearray(prefix_bytes)
    out += b"sdat" + struct.pack("<I", count)
    offset = 8 + count * 16
    for i, sound in enumerate(sounds):
        out += struct.pack("<4I", i, 0x0313BABE, offset, 0xFEEDFEED)
        offset += len(sound)
    for sound in sounds:
        out += sound
    tail = len(out) - sdat
    struct.pack_into("<I", out, 0x0C, tail)
    struct.pack_into("<I", out, 0x14, tail)
    check_header, check_sdat, check_entries, check_count = bank_parts(bytes(out))
    if check_count != count or check_sdat != sdat:
        raise ValueError("rebuilt bank does not parse")
    if event_hashes(bytes(out)) != sorted(event_hashes(bytes(out))):
        raise ValueError("event table of %s is not sorted after the renaming" % new_stem)
    return bytes(out), renamed, replaced
