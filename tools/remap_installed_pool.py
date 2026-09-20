"""Give every entity in the runtime pool an identity of its own.

The shipped pool carries copies of retail entities under their original GUIDs,
which is what makes an actor outlive the level teardown and fault in the final
cleanup (analysis/POOL_IDENTITY_COLLISION.md). This rewrites every identity in
the pool's SimGroups to a namespace nothing else uses.

The new identity keeps the two low words of the original, so a remapped entity
can still be traced back, and replaces the two high words with "RNDM"/"POOL".
No retail GUID carries that tag, so a collision with shipped content is
impossible by construction rather than by table lookup.

The file is patched byte for byte in place. Every SimGroup payload in the pool
is stored uncompressed, so an identity can be overwritten where it lies: no
chunk changes length, nothing moves, the alignment padding survives and the file
keeps its size. Re-emitting the container through a packer was tried first and
dropped the FILL chunks, which moved every asset after the first one.

By default this only reports. Pass --write to produce the new file.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
import ds2str  # noqa: E402

from ds2_assets import simgroup  # noqa: E402
from remap_pool_guids import guid_bytes, guid_hash, remap  # noqa: E402

TAG = (0x4D444E52, 0x4C4F4F50)  # "RNDM", "POOL"


def new_guid(old):
    words = [int(word, 16) for word in old.split("-")]
    return "%08X-%08X-%08X-%08X" % (words[0], words[1], TAG[0], TAG[1])


def walk(path):
    """Every asset with its payload and the file offsets that payload occupies.

    Yields (header, payload, spans) where spans is a list of (file_offset,
    length) covering the payload in order.
    """
    stream = ds2str.StreamSet(str(path))
    header = None
    spans = []
    payload = bytearray()
    for offset, tag, body in stream.chunks():
        inner = stream.tag(body[:4])
        if inner == b"SHDR":
            if header is not None:
                yield header, bytes(payload[:header["size"]]), spans
            names = body[36:].split(b"\0")
            header = {"name": names[0].decode("latin1"),
                      "path": names[1].decode("latin1"),
                      "type": names[2].decode("latin1"),
                      "size": struct.unpack_from(stream.end + "I", body, 32)[0]}
            spans, payload = [], bytearray()
        elif inner == b"SDAT" and header is not None:
            data = body[4:]
            spans.append((offset + 12, len(data)))
            payload.extend(data)
        elif inner in (b"Rpak", b"LZXD") and header is not None:
            _, data = stream.payload(body)
            spans.append((None, len(data)))
            payload.extend(data)
    if header is not None:
        yield header, bytes(payload[:header["size"]]), spans


def file_offset(spans, position):
    """Translate a payload offset into a file offset, or None if compressed."""
    for start, length in spans:
        if position < length:
            return None if start is None else start + position
        position -= length
    raise ValueError("payload offset past the end of its chunks")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pool", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--mapping", type=Path, help="write the old->new table here")
    args = parser.parse_args()

    items = list(walk(args.pool))
    raw = bytearray(args.pool.read_bytes())

    mapping = {}
    for header, payload, _ in items:
        if header["type"] != "SimGroup":
            continue
        for record in simgroup(payload)[0]:
            mapping.setdefault(record["guid"], new_guid(record["guid"]))
    print("identities: %d" % len(mapping))
    if len({guid_hash(g) for g in mapping.values()}) != len(mapping):
        raise SystemExit("destination hash collision, refusing to write")
    print("destination hashes distinct: yes")

    outside = [(header["name"], header["type"], old)
               for header, payload, _ in items if header["type"] != "SimGroup"
               for old in mapping if guid_bytes(old) in payload]
    for name, kind, old in outside:
        print("  outside a SimGroup: %s in %s (%s)" % (old, name, kind))
    print("occurrences outside SimGroups: %d" % len(outside))

    identities = references = patched_bytes = 0
    for header, payload, spans in items:
        if header["type"] != "SimGroup":
            continue
        patched, report = remap(payload, mapping)
        identities += report["identities"]
        references += report["references"]
        for position in report["edited_offsets"]:
            at = file_offset(spans, position)
            if at is None:
                raise SystemExit("%s: an identity lies in a compressed chunk" % header["name"])
            raw[at] = patched[position]
            patched_bytes += 1
    print("rewritten: %d identities, %d typed references, %d bytes" %
          (identities, references, patched_bytes))

    if args.mapping:
        args.mapping.write_text("".join("%s\t%s\n" % (o, n) for o, n in sorted(mapping.items())))
        print("mapping written to %s" % args.mapping)
    if not args.write:
        return

    out = args.out or args.pool.with_suffix(".remapped.str")
    out.write_bytes(bytes(raw))
    print("wrote %s (%d bytes, original %d)" % (out, out.stat().st_size, len(raw)))
    verify(args.pool, out, mapping)


def verify(original, remapped, mapping):
    before = {h["name"]: (h["type"], p) for h, p, _ in walk(original)}
    after = {h["name"]: (h["type"], p) for h, p, _ in walk(remapped)}
    if set(before) != set(after):
        raise SystemExit("asset set changed")
    changed = [n for n in before if before[n][1] != after[n][1]]
    print("assets: %d, payloads changed: %d (all SimGroup: %s)" %
          (len(after), len(changed), all(after[n][0] == "SimGroup" for n in changed)))

    identities = []
    for name, (kind, payload) in after.items():
        if kind == "SimGroup":
            identities += [r["guid"] for r in simgroup(payload)[0]]
    tags = {g.split("-")[2] + "-" + g.split("-")[3] for g in identities}
    print("identities after: %d, distinct namespaces: %s" % (len(identities), sorted(tags)))
    print("every identity remapped: %s" % (sorted(identities) == sorted(mapping.values())))

    blob = remapped.read_bytes()
    left = [old for old in mapping if guid_bytes(old) in blob]
    print("old identities anywhere in the file: %d" % len(left))
    if left or len(tags) != 1:
        raise SystemExit("refusing: the rewrite is incomplete")


if __name__ == "__main__":
    main()
