"""Hand editing of a skin's textures, in three commands.

    python tools/skin_edit.py export SuperExploder
        writes the variant's current textures (tinted or from files) and the
        base package's originals as DDS into variants/skins/SuperExploder/edit/
    python tools/skin_edit.py check SuperExploder
        checks every DDS in that folder against the texture it would replace
        (format, size, mip chain) and says which ones are picked up
    python tools/skin_edit.py apply SuperExploder
        writes the matching DDS files into the variant's "files" entry of
        variants/ds2_variants.json, so the next build takes them instead of the
        tint; then run tools/build_variants.py --write, verify_skin_pool.py,
        embed_stream_requirements.py and rebuild the plugin

Edit the DDS with Intel Texture Works (Photoshop) or GIMP/Paint.NET: keep the
format (BC1 = DXT1 for colour and glow maps, BC3 = DXT5 for normal maps), keep
the size, and save with mipmaps ("Auto Generate"). A file named after the base
texture (exploderbody_c.dds) or after the skin's copy (exploderbod1_c.dds)
both work.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
import ds2str  # noqa: E402
import skin_assets  # noqa: E402
from export_skin_textures import dds, CHARACTER_STREAMS  # noqa: E402


def load_spec():
    return json.loads((ROOT / "variants/ds2_variants.json").read_text(encoding="utf-8"))


def variant_of(spec, name):
    for variant in spec["variants"]:
        if variant["name"] == name:
            if "skin" not in variant:
                raise SystemExit("%s has no skin" % name)
            return variant
    raise SystemExit("no variant named " + name)


def package_textures(variant):
    stream = ds2str.StreamSet(str(CHARACTER_STREAMS / variant["skin"]["package"]))
    items = skin_assets.load_package(stream)
    pairs = {}
    for it in items:
        if it["meta"]["type"] in ("tg4h", "tg4d"):
            pairs.setdefault(it["meta"]["name"], {})[it["meta"]["type"]] = it
    return {n: p for n, p in pairs.items() if "tg4h" in p and "tg4d" in p}


def texture_info(pair):
    head = pair["tg4h"]["payload"]
    fmt = re.search(rb"DXT[135]", head).group()
    width, height = struct.unpack_from("<HH", head, 0x20)
    return fmt, width, height, len(pair["tg4d"]["payload"])


def renamed(stem, tag):
    return (stem[:-3] + tag + stem[-2:]) if stem[-2] == "_" else stem[:-1] + tag


def edited_stems(variant):
    """The textures the skin recolours: every diffuse plus what tints/files name."""
    skin = variant["skin"]
    stems = set(skin.get("tints", {})) | set(skin.get("files", {}))
    return stems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["export", "check", "apply"])
    parser.add_argument("variant")
    args = parser.parse_args()
    spec = load_spec()
    variant = variant_of(spec, args.variant)
    skin = variant["skin"]
    tag = skin.get("tag", "1")
    pairs = package_textures(variant)
    folder = ROOT / "variants/skins" / args.variant / "edit"
    # which textures the material copies use: every diffuse plus the extra ones
    materials_use = set()
    manifest_path = ROOT / "analysis/runtime_stream/ds2_randomizer_pool.toc.json"
    copied = None
    if manifest_path.exists():
        report = json.loads(manifest_path.read_text()).get("variants", {}).get("skins", {}).get(args.variant)
        if report:
            copied = {a["source"] for a in report["assets"] if a["type"] == "Material"}
    stream = ds2str.StreamSet(str(CHARACTER_STREAMS / skin["package"]))
    for it in skin_assets.load_package(stream):
        if it["meta"]["type"] == "Material" and (copied is None or it["meta"]["name"] in copied):
            for m in skin_assets.TEXTURE_PATH.finditer(it["payload"]):
                materials_use.add(m.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode())
    editable = sorted(s for s in materials_use if s in pairs and (s.endswith("_c") or s in edited_stems(variant) or s.endswith("_l")))

    if args.command == "export":
        folder.mkdir(parents=True, exist_ok=True)
        for stem in editable:
            pair = pairs[stem]
            fmt, w, h, size = texture_info(pair)
            data = pair["tg4d"]["payload"]
            block = 8 if fmt == b"DXT1" else 16
            mips, ww, hh, total = 0, w, h, 0
            while total < len(data) and ww and hh:
                total += max(1, ww // 4) * max(1, hh // 4) * block; mips += 1; ww //= 2; hh //= 2
            (folder / (stem + ".original.dds")).write_bytes(dds(data, w, h, fmt, mips))
            tint = skin.get("tints", {}).get(stem, skin["tint"] if stem.endswith("_c") else None)
            if stem in skin.get("files", {}):
                current = skin_assets.load_dds(str(ROOT / skin["files"][stem]), fmt, len(data))
            elif tint is not None:
                current = skin_assets.tint_dxt(data, fmt, tint if isinstance(tint, dict) else tuple(tint))
            else:
                current = data
            (folder / (stem + ".dds")).write_bytes(dds(current, w, h, fmt, mips))
            print("%-18s %4dx%-4d %s mips=%d  -> %s.dds (current) and %s.original.dds" % (stem, w, h, fmt.decode(), mips, stem, stem))
        print("wrote %d textures to %s; edit the .dds files (not the .original ones), then: check, apply" % (len(editable), folder))
        return

    found = {}
    for path in sorted(folder.glob("*.dds")) if folder.exists() else []:
        if path.name.endswith(".original.dds"):
            continue
        stem = path.stem
        target = stem if stem in pairs else next((s for s in pairs if renamed(s, tag) == stem), None)
        if target is None:
            print("%-28s ignored: no texture of that name in the package" % path.name); continue
        fmt, w, h, size = texture_info(pairs[target])
        try:
            skin_assets.load_dds(str(path), fmt, size)
            found[target] = path
            print("%-28s ok: replaces %s (%s %dx%d)" % (path.name, target, fmt.decode(), w, h))
        except ValueError as error:
            print("%-28s REJECTED: %s" % (path.name, error))
    if args.command == "check":
        print("%d file(s) would be applied" % len(found))
        return
    files = dict(skin.get("files", {}))
    for target, path in found.items():
        files[target] = str(path.relative_to(ROOT)).replace("\\", "/")
    skin["files"] = files
    (ROOT / "variants/ds2_variants.json").write_text(json.dumps(spec, indent=2), encoding="utf-8", newline="\n")
    print("spec updated: %d file(s) in %s's skin; now build_variants.py --write, verify_skin_pool.py, embed_stream_requirements.py, build_observer.cmd" % (len(found), args.variant))


if __name__ == "__main__":
    main()
