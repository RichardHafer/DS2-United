"""Export every enemy character package's textures as browsable PNG files.

Scope is the NPC character streams under char_str/npc: one folder per package
(one enemy type or model variant), one PNG per texture, plus an index.

How a texture is stored, verified on all 71 npc packages:

    tg4h  header chunk. Layout used here:
            +0x14  identity dword, crc32(group + name)
            +0x1C  size of the block data, matches the tg4d payload exactly
            +0x20  width, height as two uint16
            +0x26  mip count
          then the ASCII format token "DXT1" or "DXT5" followed by NUL
          separated strings; the last two are the texture group and the
          texture name. The string before them is "1" for a 2D texture and
          "cube" for a cube map.
    tg4d  the raw DXT blocks with the full mip chain, nothing else.

So a DDS file is the header the game does not store plus those bytes, which is
what tools/export_skin_textures.py already writes for the variant pipeline.
This script adds the decode step and covers every package at once.

Re-run:
    python tools/extract_enemy_textures.py
    python tools/extract_enemy_textures.py --only exploder,slasher --dds
"""
import argparse
import binascii
import io
import json
import re
import struct
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODDING_TOOLS = Path(r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
NPC_ROOT = Path(r"D:/SteamLibrary/steamapps/common/Dead Space 2/work/ds2dat10/global_assets/global_assets/ds_assets/char_str/npc")

sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402
import skin_assets  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

# Suffix conventions. Only _c and _n are load bearing in the existing variant
# pipeline (skin_assets picks the diffuse by _c and the normal by _n), the rest
# is read off the name and stays a guess.
ROLE_BY_SUFFIX = {
    "_c": ("diffuse", "verified"),
    "_n": ("normal", "verified"),
    "_s": ("specular", "assumed"),
    "_sp": ("specular", "assumed"),
    "_o": ("occlusion", "assumed"),
    "_l": ("glow", "assumed"),
    "_e": ("environment", "assumed"),
    # divider_ca sits in the colour slot of skinned_divider, in front of _n and
    # _sp, so _ca is a diffuse that also uses its alpha.
    "_ca": ("diffuse_alpha", "assumed"),
    "_t": ("tint", "assumed"),
}

# Groups whose textures are not owned by one enemy.
SHARED_GROUP_MARKERS = ("shared_", "_shared_", "shared_textures", "rigs_shared")


def dds_bytes(data, width, height, fourcc, mips):
    """A 128 byte DDS header in front of the stored block data."""
    header = bytearray(124)
    struct.pack_into("<I", header, 0, 124)
    struct.pack_into("<I", header, 4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000)
    struct.pack_into("<II", header, 8, height, width)
    struct.pack_into("<I", header, 16, len(data))
    struct.pack_into("<I", header, 24, mips)
    struct.pack_into("<I", header, 72, 32)
    struct.pack_into("<I", header, 76, 0x4)
    header[80:84] = fourcc
    struct.pack_into("<I", header, 104, 0x1000 | 0x400000 | 0x8)
    return b"DDS " + bytes(header) + data


def chain_size(width, height, block, levels):
    total, w, h = 0, width, height
    for _ in range(levels):
        total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
        w = max(1, w // 2)
        h = max(1, h // 2)
    return total


def expected_size(info, block):
    """Bytes the stored block data should have, from the header's own numbers.

    The byte at +0x26 is the mip count for a 2D texture. On the three cube maps
    in the npc packages it is 6 and the payload is exactly six unmipped faces,
    so a cube map is read as faces, not levels.
    """
    if info["kind"] == "cube":
        return 6 * chain_size(info["width"], info["height"], block, 1)
    return chain_size(info["width"], info["height"], block, info["mips"])


def parse_header(payload):
    """Everything the tg4h chunk states about its texture, or None if unknown."""
    match = re.search(rb"DXT[135]", payload)
    if not match:
        return None
    strings = bytes(payload)[match.end():].split(b"\0")
    while strings and strings[-1] == b"":
        strings.pop()
    if len(strings) < 2:
        return None
    return {
        "format": match.group().decode(),
        "identity": struct.unpack_from("<I", payload, 0x14)[0],
        "data_size": struct.unpack_from("<I", payload, 0x1C)[0],
        "width": struct.unpack_from("<H", payload, 0x20)[0],
        "height": struct.unpack_from("<H", payload, 0x22)[0],
        "mips": payload[0x26],
        "group": strings[-2].decode("ascii", "replace"),
        "name": strings[-1].decode("ascii", "replace"),
        "kind": strings[1].decode("ascii", "replace") if len(strings) > 2 else "1",
    }


def role_of(name):
    for suffix in sorted(ROLE_BY_SUFFIX, key=len, reverse=True):
        if name.endswith(suffix):
            return ROLE_BY_SUFFIX[suffix]
    return ("unknown", "assumed")


def package_textures(path):
    """tg4h/tg4d pairs plus the texture names the package's materials ask for."""
    stream = ds2str.StreamSet(str(path))
    items = skin_assets.load_package(stream)
    pairs = defaultdict(dict)
    referenced = set()
    holds = Counter()
    for item in items:
        kind = item["meta"]["type"]
        holds[kind] += 1
        if kind in ("tg4h", "tg4d"):
            pairs[item["meta"]["name"]][kind] = item
        elif kind == "Material":
            for match in skin_assets.TEXTURE_PATH.finditer(item["payload"]):
                referenced.add(match.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode())
    return pairs, referenced, holds


def declared_mips(info):
    return 1 if info["kind"] == "cube" else info["mips"]


def channel_report(extrema):
    """What the per-channel ranges say about the texture, independent of its name."""
    (r_lo, r_hi), (g_lo, g_hi), (b_lo, b_hi), (a_lo, a_hi) = extrema
    flat = lambda lo, hi: lo == hi  # noqa: E731
    if flat(r_lo, r_hi) and flat(b_lo, b_hi) and a_lo < a_hi and g_lo < g_hi:
        return "red and blue are constant, the data sits in green and alpha: a two channel normal map (X in alpha, Y in green)"
    if a_lo == a_hi == 255:
        return "alpha is fully opaque, the colour channels carry everything"
    if a_lo < a_hi:
        return "alpha varies, it carries a mask or a fourth channel"
    return "alpha is constant at %d" % a_lo


def write_png(out_file, data, info):
    """Decode mip 0 and save it. Returns (status, note, extrema)."""
    if Image is None:
        return "raw_only", "Pillow is not installed", None
    try:
        image = Image.open(io.BytesIO(dds_bytes(data, info["width"], info["height"],
                                                info["format"].encode(), declared_mips(info))))
        image.load()
    except Exception as error:  # noqa: BLE001 (any decode failure falls back to raw)
        return "raw_only", "decode failed: %s" % (repr(error)[:120],), None
    if image.size != (info["width"], info["height"]):
        return "raw_only", "decoder returned %dx%d" % image.size, None
    extrema = image.convert("RGBA").getextrema()
    if image.mode == "RGBA" and extrema[3] == (255, 255):
        image = image.convert("RGB")
    out_file.parent.mkdir(parents=True, exist_ok=True)
    image.save(out_file, optimize=False, compress_level=6)
    note = ""
    if info["kind"] == "cube":
        note = "cube map, PNG shows the first face only"
    return "decoded", note, extrema


def load_packages(tsv):
    rows = []
    lines = tsv.read_text(encoding="utf-8").splitlines()
    head = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        cells = line.split("\t")
        row = dict(zip(head, cells + [""] * (len(head) - len(cells))))
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "analysis/enemy_textures")
    parser.add_argument("--npc-root", type=Path, default=NPC_ROOT,
                        help="folder holding the <name>_cct/<name>.str packages")
    parser.add_argument("--only", default="", help="comma separated package names")
    parser.add_argument("--dds", action="store_true",
                        help="also write the raw .dds next to every PNG, not only on failure")
    parser.add_argument("--no-png", action="store_true", help="index only, write no images")
    args = parser.parse_args()

    catalog = {}
    tsv = ROOT / "analysis/catalog/npc_packages.tsv"
    if tsv.exists():
        for row in load_packages(tsv):
            catalog[Path(row["archive"].replace("\\", "/")).stem] = row
    runtime = {}
    native = ROOT / "analysis/native_character_streams.json"
    if native.exists():
        for entry in json.loads(native.read_text(encoding="utf-8"))["packages"]:
            runtime[entry["package"]] = entry

    only = {s for s in args.only.split(",") if s}
    archives = sorted(args.npc_root.rglob("*.str"))
    if not archives:
        sys.exit("no .str packages under %s" % args.npc_root)

    args.out.mkdir(parents=True, exist_ok=True)
    index = {
        "generator": "tools/extract_enemy_textures.py",
        "source_root": str(args.npc_root),
        "scope": "NPC character packages only; corpses and humans under char_str/corp and char_str/hmn are not enemies and are skipped",
        "enemies": [],
        "not_found": [],
    }
    identity_owners = defaultdict(list)
    started = time.time()
    total_tex = decoded = raw_only = 0

    for archive in archives:
        package = archive.stem
        if only and package not in only:
            continue
        row = catalog.get(package, {})
        entry = {
            "package": package,
            "folder": package,
            "source_archive": str(archive),
            "archive_bytes": archive.stat().st_size,
            "classes": [c for c in (row.get("matched_classes") or "").split("|") if c],
            "authored_records": int(row.get("model_linked_records") or 0),
            "catalog_status": row.get("status", "not listed in analysis/catalog/npc_packages.tsv"),
            "runtime_known_package": package in runtime,
            "textures": [],
        }
        try:
            pairs, referenced, holds = package_textures(archive)
        except Exception as error:  # noqa: BLE001
            index["not_found"].append({"package": package, "source_archive": str(archive),
                                       "tried": "ds2str.StreamSet + skin_assets.load_package",
                                       "error": repr(error)[:200]})
            continue
        if not pairs:
            index["not_found"].append({"package": package, "source_archive": str(archive),
                                       "tried": "read the package, looked for tg4h/tg4d chunks",
                                       "error": "no texture chunks; the package holds only %s"
                                                % ", ".join("%s x%d" % kv for kv in sorted(holds.items()))})
            continue

        out_dir = args.out / package
        for name in sorted(pairs):
            pair = pairs[name]
            record = {"name": name}
            if "tg4h" not in pair or "tg4d" not in pair:
                record.update({"status": "missing", "note": "only the %s half is present"
                               % ("header" if "tg4h" in pair else "data")})
                entry["textures"].append(record)
                continue
            head = pair["tg4h"]["payload"]
            data = pair["tg4d"]["payload"]
            info = parse_header(head)
            if info is None:
                record.update({"status": "unknown_format",
                               "note": "no DXT token in the tg4h header, not decoded and not guessed",
                               "header_hex": head[:0x40].hex()})
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / (name + ".tg4h.bin")).write_bytes(head)
                (out_dir / (name + ".tg4d.bin")).write_bytes(data)
                entry["textures"].append(record)
                total_tex += 1
                raw_only += 1
                continue

            block = 8 if info["format"] == "DXT1" else 16
            role, basis = role_of(name)
            crc = binascii.crc32((info["group"] + info["name"]).encode()) & 0xFFFFFFFF
            record.update({
                "crc32_id": "0x%08X" % info["identity"],
                "crc32_id_is_group_plus_name": crc == info["identity"],
                "group": info["group"],
                "texture_name": info["group"] + "/" + info["name"],
                "format": info["format"],
                "width": info["width"],
                "height": info["height"],
                "mips": 1 if info["kind"] == "cube" else info["mips"],
                "faces": 6 if info["kind"] == "cube" else 1,
                "cube_map": info["kind"] == "cube",
                "stored_bytes": len(data),
                "header_size_field_matches": info["data_size"] == len(data),
                "block_layout_matches": expected_size(info, block) == len(data),
                "material_referenced": name in referenced,
                "role": role,
                "role_basis": {
                    "verified": "name suffix; the variant pipeline binds this suffix to that slot",
                    "assumed": "name suffix only; a guess, nothing in the code confirms it",
                }[basis],
            })
            identity_owners[info["identity"]].append(package)

            if args.no_png:
                record["status"] = "not written"
            else:
                png = out_dir / (name + ".png")
                status, note, extrema = write_png(png, data, info)
                record["status"] = status
                record["png"] = png.name if status == "decoded" else None
                if note:
                    record["note"] = note
                if extrema is not None:
                    record["channel_range"] = {k: list(v) for k, v in zip("rgba", extrema)}
                    record["channel_layout"] = channel_report(extrema)
                if status == "decoded":
                    decoded += 1
                else:
                    raw_only += 1
                if args.dds or status != "decoded":
                    out_dir.mkdir(parents=True, exist_ok=True)
                    raw = out_dir / (name + ".dds")
                    raw.write_bytes(dds_bytes(data, info["width"], info["height"],
                                              info["format"].encode(), declared_mips(info)))
                    record["dds"] = raw.name
            total_tex += 1
            entry["textures"].append(record)
        index["enemies"].append(entry)

    for entry in index["enemies"]:
        for record in entry["textures"]:
            ident = record.get("crc32_id")
            if ident is None:
                continue
            owners = identity_owners[int(ident, 16)]
            record["shared_with"] = sorted(set(owners) - {entry["package"]})

    classes = ROOT / "analysis/catalog/enemy_classes.tsv"
    if classes.exists():
        covered = defaultdict(list)
        for entry in index["enemies"]:
            for name in entry["classes"]:
                covered[name].append(entry["package"])
        index["class_coverage"] = [
            {"class": row["class"],
             "authored_records": int(row["authored_records"] or 0),
             "packages": sorted(covered.get(row["class"], [])),
             "eligibility": row.get("eligibility", "")}
            for row in load_packages(classes)]

    index["summary"] = {
        "packages": len(index["enemies"]),
        "packages_without_textures": len(index["not_found"]),
        "textures": total_tex,
        "decoded_png": decoded,
        "raw_only": raw_only,
        "unique_texture_identities": len(identity_owners),
        "seconds": round(time.time() - started, 1),
    }
    (args.out / "index.json").write_text(json.dumps(index, indent=2), encoding="utf-8")
    write_markdown(args.out, index)
    print(json.dumps(index["summary"], indent=2))


def write_markdown(out, index):
    lines = ["# Dead Space 2 enemy textures", "",
             "Generated by `tools/extract_enemy_textures.py`. See `README.md` for how to re-run it.", "",
             "Source: `%s`" % index["source_root"], ""]
    s = index["summary"]
    lines += ["| | |", "|---|---|",
              "| enemy packages covered | %d |" % s["packages"],
              "| textures exported | %d |" % s["textures"],
              "| decoded to PNG | %d |" % s["decoded_png"],
              "| raw only | %d |" % s["raw_only"],
              "| unique texture identities | %d |" % s["unique_texture_identities"],
              "| packages with no textures | %d |" % s["packages_without_textures"], ""]
    lines += ["Columns: `name` is the texture name inside the package and the file stem.",
              "`id` is the crc32 of group + name, the dword the materials use to bind it.",
              "`role` is read off the name suffix; only `_c` (diffuse) and `_n` (normal) are",
              "used as such by the existing variant pipeline, everything else is a guess.",
              "`mat` means a Material in the same package names the texture; textures without",
              "it are carried by VFX or by another package.",
              "`chan` is read off the decoded pixels, not off the name: `GA-normal` means red",
              "and blue are constant and the vector sits in green and alpha, so the PNG looks",
              "flat green and only those two channels may be edited. `alpha` means the alpha",
              "channel carries data.", ""]
    if index.get("class_coverage"):
        lines += ["## Enemy class coverage", "",
                  "Classes from `analysis/catalog/enemy_classes.tsv` against the packages",
                  "exported here. A class with no package draws its model from another",
                  "package or has no authored instance at all.", "",
                  "| class | authored records | packages with textures |", "|---|---|---|"]
        for row in index["class_coverage"]:
            lines.append("| %s | %d | %s |" % (row["class"], row["authored_records"],
                                               ", ".join(row["packages"]) or "none"))
        lines.append("")
    for entry in sorted(index["enemies"], key=lambda e: e["package"]):
        classes = ", ".join(entry["classes"]) or "no authored class in the catalog"
        lines += ["## %s" % entry["package"], "",
                  "- classes: %s" % classes,
                  "- authored records: %d" % entry["authored_records"],
                  "- archive: `%s`" % entry["source_archive"],
                  "- in the runtime package list: %s" % ("yes" if entry["runtime_known_package"] else "no"),
                  "- note: %s" % entry["catalog_status"], "",
                  "| name | id | size | fmt | mips | role | chan | mat | group | status |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for t in entry["textures"]:
            if "crc32_id" not in t:
                lines.append("| %s | | | | | | | | | %s: %s |" % (t["name"], t.get("status", "?"), t.get("note", "")))
                continue
            flag = "cube " if t["cube_map"] else ""
            layout = t.get("channel_layout", "")
            chan = "GA-normal" if layout.startswith("red and blue") else ("alpha" if layout.startswith("alpha varies") else "-")
            lines.append("| %s | %s | %dx%d | %s%s | %d | %s | %s | %s | %s | %s |" % (
                t["name"], t["crc32_id"], t["width"], t["height"], flag, t["format"], t["mips"],
                t["role"], chan, "yes" if t["material_referenced"] else "-", t["group"], t["status"]))
        lines.append("")
    if index["not_found"]:
        lines += ["## Not found", "",
                  "Packages that were read but produced no texture. What they do hold is in",
                  "the result column; where their model comes from was not chased further.", "",
                  "| package | tried | result |", "|---|---|---|"]
        for miss in index["not_found"]:
            lines.append("| %s | %s | %s |" % (miss["package"], miss["tried"], miss["error"]))
        lines.append("")
    (out / "INDEX.md").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
