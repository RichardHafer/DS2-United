"""Enemy variant packs: one folder per variant under variants/packs/<Name>/.

    python tools/variant_pack.py list
    python tools/variant_pack.py new <Name> --family Zombie [--package zomb_cct/slasher.str] [--source GUID]
    python tools/variant_pack.py fields <Name>
    python tools/variant_pack.py check [<Name>]
    python tools/variant_pack.py skin export|check|apply <Name>
    python tools/variant_pack.py sounds export|setup <Name>
    python tools/variant_pack.py model check <Name>
    python tools/variant_pack.py build [--write]
    python tools/variant_pack.py migrate

"retarget": {"package": "zomb_cct/twitcher_security.str"} builds the variant from
another character package of the same family (model, rig, model stream and
dismemberment prop); the package is forced resident by the pool TOC. "contexts"
limits a model_sources "all" variant to direct, vent and/or scripted spawns.

A pack is variant.json plus its files: skin/ holds edited DDS textures, sounds/
holds WAV files for the variant's own sound bank, model/ a character package
rebuilt with other meshes (the DS2-3 Modding Toolkit by 4Kodda, Blender), which
the skin copy then takes everything from. Paths inside variant.json are
relative to the pack folder. Only packs with "enabled": true go into a build;
every built pack costs pool memory whether the INI lets it spawn or not.

build merges the enabled packs into variants/build/ds2_variants.generated.json
and runs tools/build_variants.py on it (a dry run on a scratch copy without
--write), then verify_skin_pool.py and embed_stream_requirements.py. Rebuild the
plugin afterwards (build_ds2coop.cmd) and install the pool, its TOC and
ds2_randomizer_models.str/.toc together with it.
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKS = ROOT / "variants/packs"
GENERATED = ROOT / "variants/build/ds2_variants.generated.json"
LEGACY = ROOT / "variants/ds2_variants.json"
CATALOG = ROOT / "analysis/catalog/entities.jsonl"
NAME = re.compile(r"^[A-Za-z][A-Za-z0-9]{1,23}$")
VALUE = re.compile(r"^(x-?[0-9.]+|=-?[0-9.]+|i-?[0-9]+)$")
# Features a build refuses unless the pack says "allow_unstable": true.
UNSTABLE = {
    "drop_list": "custom drop lists: GoldenLeaper crashed in ItemDropListComponent",
    "ghost": "another package's material (skin.ghost): GhostLeaper crashed in ItemDropListComponent",
}
DEFAULTS = {"enabled": False, "family": "", "source": "", "model_sources": "all", "chance": 25, "speed": 1.0, "scale": 1.0, "fields": {}}


def run_tool(script, *extra):
    cmd = [sys.executable, "-I", str(ROOT / "tools" / script), *extra]
    print("> " + " ".join(cmd[2:]))
    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode:
        raise SystemExit("%s failed with %d" % (script, result.returncode))


def packs():
    found = {}
    for folder in sorted(PACKS.glob("*")) if PACKS.exists() else []:
        spec = folder / "variant.json"
        if spec.is_file():
            found[folder.name] = (folder, json.loads(spec.read_text(encoding="utf-8")))
    return found


def save(folder, variant):
    (folder / "variant.json").write_text(json.dumps(variant, indent=2) + "\n", encoding="utf-8", newline="\n")


def catalog_entity(guid):
    with CATALOG.open(encoding="utf-8") as stream:
        for line in stream:
            if guid in line:
                entity = json.loads(line)
                if entity.get("guid") == guid:
                    return entity
    return None


def field_keys(entity):
    return {image.get("class", "") + "." + (f.get("name") or "") for image in entity["images"] for f in image.get("fields", [])}


def model_of(entity):
    for image in entity["images"]:
        for f in image.get("fields", []):
            if f.get("name") == "CMD_SetModel" and f.get("guid_candidate"):
                return f["guid_candidate"]
    return None


_model_keys = {}


def model_field_keys(entity):
    """Every field authored on any record of the source's class and model: with
    "model_sources": "all" each copy keeps the fields its own record carries."""
    key = (entity.get("class"), model_of(entity))
    if key not in _model_keys:
        keys = set()
        with CATALOG.open(encoding="utf-8") as stream:
            for line in stream:
                if '"class": "%s"' % key[0] not in line:
                    continue
                other = json.loads(line)
                if other.get("class") == key[0] and model_of(other) == key[1]:
                    keys |= field_keys(other)
        _model_keys[key] = keys
    return _model_keys[key]


def character_packages():
    """{package: (path under char_str/npc, classes)} from the native character streams."""
    data = json.loads((ROOT / "analysis/native_character_streams.json").read_text(encoding="utf-8"))
    out = {}
    for package in data["packages"]:
        parts = package["toc_name"].split("\\")
        out[package["package"]] = (parts[-2] + "/" + parts[-1] + ".str", package.get("classes") or [])
    return out


def global_targets():
    """[(source GUID, package, family)] for the records the pool carries per model."""
    text = (ROOT / "src/generated_global_targets.h").read_text(encoding="utf-8")
    rows = re.findall(r'\{\{[^}]*\},\{0x([0-9A-F]{8})u,0x([0-9A-F]{8})u,0x([0-9A-F]{8})u,0x([0-9A-F]{8})u\},\{[^}]*\},"([^"]*)","([^"]*)"', text)
    return [("-".join(r[:4]), r[4], r[5]) for r in rows]


def float_of(value):
    if not isinstance(value, int):
        return None
    f = struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0]
    return f if f == f and 1e-6 <= abs(f) <= 1e7 or value == 0 else None


# --- commands ------------------------------------------------------------------------------
def cmd_list(_args):
    rows = packs()
    if not rows:
        print("no packs in %s (python tools/variant_pack.py migrate, or new)" % PACKS)
        return
    print("%-20s %-3s %-11s %6s %6s %6s  %s" % ("name", "on", "family", "chance", "speed", "scale", "has"))
    for name, (folder, v) in rows.items():
        has = []
        if v.get("fields"): has.append("%d field(s)" % len(v["fields"]))
        if v.get("skin"): has.append("skin(%d dds)" % len(v["skin"].get("files", {})) if v["skin"].get("files") else "skin(tint)")
        if v.get("skin", {}).get("custom_sounds"): has.append("own sounds")
        if v.get("skin", {}).get("sounds"): has.append("sound swaps")
        if v.get("model"): has.append("model")
        unstable = [k for k in UNSTABLE if k in v or k in v.get("skin", {})]
        if unstable: has.append("UNSTABLE:" + ",".join(unstable))
        print("%-20s %-3s %-11s %6s %6.2f %6.2f  %s" % (name, "yes" if v.get("enabled") else "no", v.get("family", "?"), v.get("chance", "?"),
                                                    float(v.get("speed", 1.0)), float(v.get("scale", 1.0)), ", ".join(has)))


def cmd_new(args):
    if not NAME.match(args.name):
        raise SystemExit("name: letters and digits, 2..24 characters, starting with a letter")
    folder = PACKS / args.name
    if folder.exists():
        raise SystemExit("%s exists already" % folder)
    pkgs = character_packages()
    package = args.package
    if not package:
        for name, (path, classes) in pkgs.items():
            if args.family in classes and not name.startswith("super_") and "dementia" not in name:
                package = path
                break
    if not package:
        raise SystemExit("no character package for family %s; pass --package" % args.family)
    stem = package.rsplit("/", 1)[-1][:-4]
    source = args.source
    if not source:
        # the authored record the pool copied for this model
        source = next((guid for guid, pkg, family in global_targets() if pkg == stem and family == args.family), "")
    if not source:
        raise SystemExit("no authored record of %s found for %s; pass --source GUID" % (stem, args.family))
    used_tags = {v.get("skin", {}).get("tag") for _f, v in packs().values() if v.get("skin", {}).get("package") == package}
    tag = next(c for c in "123456789abcdefghijklmnopqrstuvwxyz" if c not in used_tags)
    variant = dict(DEFAULTS, family=args.family, source=source,
                   skin={"package": package, "tag": tag, "tint": None, "tints": {}, "files": {}},
                   notes="What it is and how it plays. Fields: python tools/variant_pack.py fields %s" % args.name)
    (folder / "skin").mkdir(parents=True)
    (folder / "sounds").mkdir()
    save(folder, variant)
    print("created %s (family %s, source %s, package %s, skin tag %s), disabled" % (folder, args.family, source, package, tag))
    print("next: fields %s, edit variant.json, skin export %s, check %s, then enable and build" % (args.name, args.name, args.name))


def cmd_fields(args):
    folder, variant = packs().get(args.name, (None, None))
    if not variant:
        raise SystemExit("no pack " + args.name)
    entity = catalog_entity(variant["source"])
    if not entity:
        raise SystemExit("source %s is not in the catalog" % variant["source"])
    print("%s: source %s (%s) in %s" % (args.name, entity["guid"], entity.get("class"), entity["archive"]))
    for image in entity["images"]:
        for field in image.get("fields", []):
            name = field.get("name")
            if not name or field.get("string_candidate") or field.get("guid_candidate"):
                continue
            key = image.get("class", "") + "." + name
            f = float_of(field.get("value"))
            current = variant.get("fields", {}).get(key, "")
            print("  %-62s %12s  %s%s" % (key, ("%.4g" % f) if f is not None else field.get("value"),
                                          "int %d" % field["value"] if isinstance(field.get("value"), int) and f is None else "",
                                          ("   <- " + current) if current else ""))


def problems(name, folder, variant, others):
    out = []
    if not NAME.match(name):
        out.append("name: letters and digits, 2..24 characters")
    if variant.get("stream_only"):
        # a package shipped as a loose stream only (Gabe's patrol helmet), no record copies
        stream = variant.get("stream", {})
        if not (folder / stream.get("package_file", "")).is_file():
            out.append("stream package_file %s missing" % stream.get("package_file"))
        if not re.match(r"^ds2_[a-z0-9_]{1,40}$", stream.get("stream_name", "")):
            out.append("stream_name must be ds2_ plus lower-case letters, digits, _")
        return out
    for key in ("family", "source"):
        if not variant.get(key):
            out.append("missing " + key)
    entity = catalog_entity(variant.get("source", "")) if variant.get("source") else None
    if variant.get("source") and not entity:
        out.append("source %s is not in the catalog" % variant["source"])
    if entity and entity.get("class") != variant.get("family"):
        out.append("source is a %s, not a %s" % (entity.get("class"), variant.get("family")))
    if entity:
        present = field_keys(entity)
        model_keys = model_field_keys(entity) if variant.get("model_sources") == "all" else present
        for key, value in variant.get("fields", {}).items():
            if key not in model_keys:
                out.append("field %s is on no record of this model (see: fields %s)" % (key, name))
            if not VALUE.match(str(value)):
                out.append("field %s: '%s' is not x<factor>, =<float> or i<int>" % (key, value))
    chance = variant.get("chance", 0)
    if not isinstance(chance, int) or not 0 <= chance <= 100:
        out.append("chance must be a whole number 0..100")
    if not 0.25 <= float(variant.get("speed", 1.0)) <= 3.0:
        out.append("speed must be 0.25..3")
    if not 0.5 <= float(variant.get("scale", 1.0)) <= 2.0:
        out.append("scale must be 0.5..2 (the collision capsule keeps its size)")
    skin = variant.get("skin")
    if skin:
        sys.path.insert(0, str(ROOT / "tools"))
        from ds2_paths import MODDING_TOOLS  # noqa: E402
        sys.path.insert(0, str(MODDING_TOOLS))
        from export_skin_textures import CHARACTER_STREAMS  # noqa: E402
        if not (CHARACTER_STREAMS / skin.get("package", "")).is_file():
            out.append("skin package %s not found under %s" % (skin.get("package"), CHARACTER_STREAMS))
        tag = skin.get("tag", "")
        if not re.match(r"^[0-9a-z]$", str(tag)):
            out.append("skin tag must be one character 0-9 or a-z")
        for other, (_f, v) in others.items():
            if other != name and v.get("skin", {}).get("package") == skin.get("package") and v.get("skin", {}).get("tag") == tag:
                out.append("skin tag '%s' is used by %s on the same package" % (tag, other))
        for stem, path in skin.get("files", {}).items():
            if not (folder / path).is_file():
                out.append("skin file %s missing (%s)" % (path, stem))
        cs = skin.get("custom_sounds")
        if cs and not any((folder / cs.get("folder", "sounds")).glob("*.wav")):
            out.append("custom_sounds: no WAV files in %s" % cs.get("folder", "sounds"))
    retarget = variant.get("retarget")
    if retarget:
        sys.path.insert(0, str(ROOT / "tools"))
        from ds2_paths import MODDING_TOOLS  # noqa: E402
        sys.path.insert(0, str(MODDING_TOOLS))
        from export_skin_textures import CHARACTER_STREAMS  # noqa: E402
        if skin:
            out.append("retarget and skin exclude each other")
        if retarget.get("package_file"):
            if not (folder / retarget["package_file"]).is_file():
                out.append("retarget package_file %s missing" % retarget["package_file"])
            for key in ("model", "rcb", "prop", "stream_name"):
                if not retarget.get(key):
                    out.append("retarget with a package_file needs %s" % key)
        elif not (CHARACTER_STREAMS / retarget.get("package", "")).is_file():
            out.append("retarget package %s not found under %s" % (retarget.get("package"), CHARACTER_STREAMS))
        if retarget.get("bnk_from") and not (folder / retarget["bnk_from"]).is_file():
            out.append("retarget bnk_from %s missing" % retarget["bnk_from"])
        if (retarget.get("package_file") or retarget.get("bnk_from")) and not re.match(r"^ds2_[a-z0-9_]{1,40}$", retarget.get("stream_name", "")):
            out.append("retarget stream_name must be ds2_ plus lower-case letters, digits, _")
    contexts = variant.get("contexts")
    if contexts is not None and (not isinstance(contexts, list) or not contexts or any(c not in ("direct", "vent", "scripted") for c in contexts)):
        out.append("contexts must list direct, vent and/or scripted")
    model = variant.get("model")
    if model:
        if not skin:
            out.append("model: needs a skin entry (the model rides on the skin copy)")
        if not (folder / model.get("package_file", "")).is_file():
            out.append("model: package_file %s missing" % model.get("package_file"))
    if not variant.get("allow_unstable"):
        for key, why in UNSTABLE.items():
            if key in variant or key in (skin or {}):
                out.append("unstable feature %s (%s); set \"allow_unstable\": true to build it anyway" % (key, why))
    return out


def cmd_check(args):
    rows = packs()
    names = [args.name] if args.name else list(rows)
    bad = 0
    for name in names:
        if name not in rows:
            raise SystemExit("no pack " + name)
        folder, variant = rows[name]
        issues = problems(name, folder, variant, rows)
        state = "enabled" if variant.get("enabled") else "disabled"
        if issues:
            # a disabled pack is reported but blocks nothing, unless asked for by name
            bad += 1 if variant.get("enabled") or args.name else 0
            print("%s (%s):" % (name, state))
            for issue in issues:
                print("   - " + issue)
        else:
            print("%s (%s): ok" % (name, state))
    if bad:
        raise SystemExit("%d pack(s) with problems" % bad)


def cmd_skin(args):
    rows = packs()
    if args.name not in rows:
        raise SystemExit("no pack " + args.name)
    folder, variant = rows[args.name]
    if not variant.get("skin"):
        raise SystemExit("%s has no skin" % args.name)
    sys.path.insert(0, str(ROOT / "tools"))
    import skin_edit  # noqa: E402
    resolved = json.loads(json.dumps(variant))
    if variant.get("model"):
        resolved["skin"]["package_file"] = str((folder / variant["model"]["package_file"]).relative_to(ROOT)).replace("\\", "/")
    resolved["skin"]["files"] = {k: str(folder / v) for k, v in variant["skin"].get("files", {}).items()}
    for stem, path in resolved["skin"]["files"].items():
        resolved["skin"]["files"][stem] = str(Path(path).relative_to(ROOT)).replace("\\", "/")

    def store(found):
        files = dict(variant["skin"].get("files", {}))
        for stem, path in found.items():
            files[stem] = str(Path(path).relative_to(folder)).replace("\\", "/")
        variant["skin"]["files"] = files
        save(folder, variant)

    (folder / "skin").mkdir(exist_ok=True)
    skin_edit.run(args.action, args.name, resolved, folder / "skin", store)


def package_path(folder, variant):
    sys.path.insert(0, str(ROOT / "tools"))
    from export_skin_textures import CHARACTER_STREAMS  # noqa: E402
    if variant.get("model"):
        return folder / variant["model"]["package_file"]
    return CHARACTER_STREAMS / variant["skin"]["package"]


def cmd_sounds(args):
    rows = packs()
    if args.name not in rows:
        raise SystemExit("no pack " + args.name)
    folder, variant = rows[args.name]
    if not variant.get("skin"):
        raise SystemExit("%s has no skin: own sounds ride on the skin's character copy" % args.name)
    sys.path.insert(0, str(ROOT / "tools"))
    from ds2_paths import MODDING_TOOLS  # noqa: E402
    sys.path.insert(0, str(MODDING_TOOLS))
    import ds2str  # noqa: E402
    import skin_assets  # noqa: E402
    package = package_path(folder, variant)
    if args.action == "export":
        import export_sounds  # noqa: E402
        export_sounds.export(package, folder / "sounds" / "original")
        print("edit copies of these WAVs (same names, 16 bit mono) into %s; then: sounds setup %s" % (folder / "sounds", args.name))
        return
    banks = sorted(it["meta"]["name"] for it in skin_assets.load_package(ds2str.StreamSet(str(package))) if it["meta"]["type"] == "SBK")
    if not banks:
        raise SystemExit("the package has no SBK sound bank")
    character = banks[0][:-4].split("_")[0]
    tag = variant["skin"].get("tag", "1")
    private = character[:-1] + tag   # same length: the animation bank's name block keeps its size
    if private == character:
        private = character[:-1] + ("2" if tag != "2" else "3")
    variant["skin"]["custom_sounds"] = {
        "folder": "sounds",
        "banks": {bank: bank.replace(character, private) for bank in banks},
        "rename": {character: private},
    }
    save(folder, variant)
    wavs = len(list((folder / "sounds").glob("*.wav")))
    print("custom_sounds set: banks %s, events '%s' -> '%s'; %d WAV(s) in sounds/ replace their originals" % (
        ", ".join(banks), character, private, wavs))


def cmd_model(args):
    rows = packs()
    if args.name not in rows:
        raise SystemExit("no pack " + args.name)
    folder, variant = rows[args.name]
    if not variant.get("model") or not variant.get("skin"):
        raise SystemExit("%s has no model (\"model\": {\"package_file\": \"model/<name>.str\"} and a skin)" % args.name)
    sys.path.insert(0, str(ROOT / "tools"))
    from ds2_paths import MODDING_TOOLS  # noqa: E402
    sys.path.insert(0, str(MODDING_TOOLS))
    import ds2str  # noqa: E402
    import skin_assets  # noqa: E402
    from export_skin_textures import CHARACTER_STREAMS  # noqa: E402

    def table(path):
        return {(it["meta"]["type"], it["meta"]["name"]): len(it["payload"]) for it in skin_assets.load_package(ds2str.StreamSet(str(path)))}

    retail = table(CHARACTER_STREAMS / variant["skin"]["package"])
    own = table(folder / variant["model"]["package_file"])
    missing = sorted(k for k in retail if k not in own)
    added = sorted(k for k in own if k not in retail)
    changed = sorted(k for k in own if k in retail and own[k] != retail[k])
    print("%s: %d resources in the retail package, %d in %s" % (args.name, len(retail), len(own), variant["model"]["package_file"]))
    for label, keys in (("changed", changed), ("added", added), ("missing", missing)):
        for t, n in keys[:40]:
            print("  %-8s %-14s %s%s" % (label, t, n, "  %d -> %d bytes" % (retail[(t, n)], own[(t, n)]) if label == "changed" else ""))
    if missing:
        raise SystemExit("%d retail resource(s) missing: the skin copy needs every resource the MetaModel names" % len(missing))
    if not changed and not added:
        print("identical to the retail package: nothing custom in it yet")


def generated_spec(rows):
    variants, streams = [], []
    for name, (folder, variant) in rows.items():
        if not variant.get("enabled"):
            continue
        issues = problems(name, folder, variant, rows)
        if issues:
            raise SystemExit("%s: %s (run: check %s)" % (name, issues[0], name))
        if variant.get("stream_only"):
            stream = dict(variant["stream"])
            stream["package_file"] = str((folder / stream["package_file"]).resolve().relative_to(ROOT)).replace("\\", "/")
            stream["pack"] = name
            streams.append(stream)
            continue
        v = {k: val for k, val in variant.items() if k not in ("enabled", "notes", "allow_unstable", "not_authored")}
        v["name"] = name
        if v.get("retarget"):
            retarget = dict(v["retarget"])
            for key in ("package_file", "bnk_from"):
                if retarget.get(key):
                    retarget[key] = str((folder / retarget[key]).resolve().relative_to(ROOT)).replace("\\", "/")
            v["retarget"] = retarget
        if v.get("skin"):
            skin = dict(v["skin"])
            skin["files"] = {k: str((folder / p).relative_to(ROOT)).replace("\\", "/") for k, p in skin.get("files", {}).items()}
            if variant.get("model"):
                skin["package_file"] = str((folder / variant["model"]["package_file"]).relative_to(ROOT)).replace("\\", "/")
                v.pop("model", None)
            if skin.get("custom_sounds"):
                cs = dict(skin["custom_sounds"])
                cs["folder"] = str((folder / cs.get("folder", "sounds")).relative_to(ROOT)).replace("\\", "/")
                skin["custom_sounds"] = cs
            v["skin"] = skin
        variants.append(v)
    legacy = json.loads(LEGACY.read_text(encoding="utf-8")) if LEGACY.exists() else {}
    banks_manifest = ROOT / "variants/build/anims/manifest.json"
    banks = json.loads(banks_manifest.read_text(encoding="utf-8")) if banks_manifest.exists() else []
    return {"scope": "Generated by tools/variant_pack.py from variants/packs; do not edit.",
            "notation": legacy.get("notation", ""), "variants": variants, "streams": streams, "banks": banks}


def cmd_build(args):
    run_tool("build_anim_banks.py")   # variants/anims -> the mod's own animation banks for the pool
    spec = generated_spec(packs())
    GENERATED.parent.mkdir(parents=True, exist_ok=True)
    GENERATED.write_text(json.dumps(spec, indent=1) + "\n", encoding="utf-8", newline="\n")
    names = [v["name"] for v in spec["variants"]] + [s["pack"] for s in spec.get("streams", [])]
    print("%d enabled pack(s): %s" % (len(names), ", ".join(names) or "none (the build removes every variant)"))
    if spec["banks"]:
        print("%d animation bank(s): %s" % (len(spec["banks"]), ", ".join(b["name"] for b in spec["banks"])))
    run_tool("build_variants.py", "--spec", str(GENERATED), *(["--write"] if args.write else []))
    if not args.write:
        print("dry run done; pass --write to build")
        return
    run_tool("verify_skin_pool.py")
    run_tool("embed_stream_requirements.py")
    print("now: build_ds2coop.cmd, then install the plugin, analysis\\runtime_stream\\ds2_randomizer_pool.str/.toc and")
    print("ds2_randomizer_models.str/.toc into <game>\\randomizer\\ (game closed). INI: [DS2Randomizer] Variants=1,")
    print("[Variants] <Name>=<percent of the family's spawns> (0 keeps a built pack from spawning).")


def cmd_migrate(_args):
    legacy = json.loads(LEGACY.read_text(encoding="utf-8"))
    made = 0
    for variant in legacy["variants"]:
        name = variant["name"]
        folder = PACKS / name
        if folder.exists():
            print("%-20s exists, kept" % name)
            continue
        (folder / "skin").mkdir(parents=True)
        (folder / "sounds").mkdir()
        v = {"enabled": False}
        v.update({k: val for k, val in variant.items() if k != "name"})
        skin = v.get("skin")
        if skin:
            files = {}
            for stem, path in skin.get("files", {}).items():
                source = ROOT / path
                target = folder / "skin" / source.name
                shutil.copyfile(source, target)
                files[stem] = "skin/" + source.name
            edit = ROOT / "variants/skins" / name / "edit"
            for dds in sorted(edit.glob("*.dds")) if edit.exists() else []:
                if not (folder / "skin" / dds.name).exists():
                    shutil.copyfile(dds, folder / "skin" / dds.name)
            skin["files"] = files
            cs = skin.get("custom_sounds")
            if cs:
                shutil.copytree(ROOT / cs["folder"], folder / "sounds", dirs_exist_ok=True)
                cs["folder"] = "sounds"
        unstable = [k for k in UNSTABLE if k in v or k in (skin or {})]
        if unstable:
            v["notes"] = (v.get("notes", "") + " Needs allow_unstable: " + ", ".join(UNSTABLE[k] for k in unstable)).strip()
        save(folder, v)
        made += 1
        print("%-20s -> %s%s" % (name, folder.relative_to(ROOT), "  (unstable: " + ",".join(unstable) + ")" if unstable else ""))
    print("%d pack(s) made, all disabled; variants/ds2_variants.json is no longer read by the build" % made)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    p = sub.add_parser("new"); p.add_argument("name"); p.add_argument("--family", required=True); p.add_argument("--package"); p.add_argument("--source")
    p = sub.add_parser("fields"); p.add_argument("name")
    p = sub.add_parser("check"); p.add_argument("name", nargs="?")
    p = sub.add_parser("skin"); p.add_argument("action", choices=["export", "check", "apply"]); p.add_argument("name")
    p = sub.add_parser("sounds"); p.add_argument("action", choices=["export", "setup"]); p.add_argument("name")
    p = sub.add_parser("model"); p.add_argument("action", choices=["check"]); p.add_argument("name")
    p = sub.add_parser("build"); p.add_argument("--write", action="store_true")
    sub.add_parser("migrate")
    args = parser.parse_args()
    {"list": cmd_list, "new": cmd_new, "fields": cmd_fields, "check": cmd_check, "skin": cmd_skin, "sounds": cmd_sounds, "model": cmd_model, "build": cmd_build, "migrate": cmd_migrate}[args.command](args)


if __name__ == "__main__":
    main()
