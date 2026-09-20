"""Build stat variants of pool enemies and append them to the resident pool.

A variant is a copy of an authored enemy record with some of its authored
numbers changed: health, limb health, attack damage, chances, stasis
resistance. Everything else, the model, the animations, the sounds, the vent
climb, is the base enemy's, so a variant needs no new resources at all. The
authored numbers live inline in the record's parameter images, each field a
twelve byte row: hash, descriptor, flags, value index, then the dword. Floats
are IEEE singles in that dword and the 0/1 switches are raw dwords.

The spec is variants/ds2_variants.json. Every variant names its source record
by catalog GUID, and the record's closure (drop list, dismemberment prop) is
copied along and remapped like the boss donors', but into its own namespace,
RNDM/VARI, with the second word derived from the variant name, so a variant
never collides with the pool's plain copy of the same source.

A variant is offered next to the base enemy's other models. It counts as a
model of its own for the family draw, and `chance` is the percentage of spawns
it is allowed to compete in at all, decided per spawn by the seed, so a
variant at 25 is a rare sight rather than every fourth slasher.

A variant may also carry a `skin`: a tinted copy of the base enemy's model
(textures, materials, meshes, MetaModel, see tools/skin_assets.py) appended to
the pool as typed resources, and the variant's CMD_SetModel redirected to the
copy. The base package still has to be loaded for the animations, physics and
sounds, which is the case whenever the base enemy is offered.

Appending is the only safe way to change a stream. Run
tools/embed_stream_requirements.py afterwards, then rebuild.
"""
import argparse
import re
import hashlib
import json
import struct
import sys
import uuid
import zlib
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")

from ds2_assets import assets, simgroup  # noqa: E402
from stream_framing import frame_stream, HEADER  # noqa: E402
from remap_pool_guids import guid_hash, remap  # noqa: E402
from resolve_pool_dependencies import closure, compact  # noqa: E402
from build_isolated_pool import isolate_many  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, trailing_padding  # noqa: E402
from append_direct_donors import refresh_manifest  # noqa: E402
from append_boss_donors import pool_identities  # noqa: E402
import skin_assets  # noqa: E402

TAG = (0x4D444E52, 0x56415249)  # "RNDM", "VARI"


def name_hash(text):
    value = 0
    for char in text.lower():
        value = (value * 0x1003F + ord(char)) & 0xFFFFFFFF
    return value


def variant_guid(old, variant):
    """Same first word as the source, second word folded with the variant name,
    then the variant tag. Two variants of one source stay apart, and neither
    collides with the pool's plain copy, which keeps the second word."""
    words = [int(w, 16) for w in old.split("-")]
    # the name hash is mixed first: sdbm of "X_16" and "X_17" differ by one bit,
    # and sibling records differ by one bit too, which collided (13.09.)
    mixed = name_hash(variant)
    mixed = ((mixed ^ (mixed >> 16)) * 0x7FEB352D) & 0xFFFFFFFF
    mixed = ((mixed ^ (mixed >> 15)) * 0x846CA68B) & 0xFFFFFFFF
    mixed ^= mixed >> 16
    return "%08X-%08X-%08X-%08X" % (words[0], (words[1] ^ mixed) & 0xFFFFFFFF, TAG[0], TAG[1])


def apply_value(raw, flags, spec):
    """'x1.5' scales the authored float, '=12' sets a float, 'i3' a raw dword."""
    if spec.startswith("i"):
        return int(spec[1:]) & 0xFFFFFFFF
    if flags not in (0,):
        raise ValueError("only inline dword fields can be scaled or set, flags=%d" % flags)
    current = struct.unpack("<f", struct.pack("<I", raw))[0]
    if spec.startswith("x"):
        value = current * float(spec[1:])
    elif spec.startswith("="):
        value = float(spec[1:])
    else:
        raise ValueError("unknown value spec " + spec)
    return struct.unpack("<I", struct.pack("<f", value))[0]


CHARACTER_STREAMS = Path("ds2dat10/global_assets/global_assets/ds_assets/char_str/npc")


def pool_resource_ids(pool_path):
    """Resource ids of every typed asset the pool already carries."""
    import ds2str
    stream = ds2str.StreamSet(pool_path)
    return {meta["resource_id"] for meta, _chunks in assets(stream) if meta["resource_id"]}


def stream_guid(entity):
    for image in entity["images"]:
        for field in image.get("fields", []):
            if field.get("name") == "CMD_SetModelStream" and field.get("flags") == 12:
                return field["guid_candidate"]
    raise SystemExit("%s has no CMD_SetModelStream" % entity["guid"])


def has_field(entity, name):
    return any(field.get("name") == name and field.get("flags") == 12
               for image in entity["images"] for field in image.get("fields", []))


def field_guid(entity, name):
    for image in entity["images"]:
        for field in image.get("fields", []):
            if field.get("name") == name and field.get("flags") == 12:
                return field["guid_candidate"]
    raise SystemExit("%s has no %s" % (entity["guid"], name))


def model_guid(entity):
    for image in entity["images"]:
        for field in image.get("fields", []):
            if field.get("name") == "CMD_SetModel" and field.get("flags") == 12:
                return field["guid_candidate"]
    raise SystemExit("%s has no CMD_SetModel" % entity["guid"])


def asset_chunks(asset):
    """SHDR + TADS chunks of one typed asset for a stream."""
    header = bytearray(asset["header"])
    struct.pack_into("<I", header, 32, len(asset["payload"]))
    body = bytearray(header)
    body.extend((asset["name"] + "\0" + asset["path"] + "\0" + asset["type"] + "\0").encode("latin1"))
    out = bytearray(chunk(bytes(body)))
    payload = asset["payload"]
    for start in range(0, len(payload), MAX_CHUNK - 12):
        out.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
    return bytes(out)


def uuid_name(text):
    """A stream root's SimGroup is named after its GUID in UUID byte order."""
    w = [int(x, 16) for x in text.split("-")]
    b2, b3 = struct.pack("<I", w[2]), struct.pack("<I", w[3])
    return "{%08X-%04X-%04X-%s-%s%s}" % (w[0], w[1] & 0xFFFF, w[1] >> 16, b2[:2].hex().upper(), b2[2:].hex().upper(), b3.hex().upper())


def build_toc(name, identity, content_size, shared, forced):
    """A one entry TOC like the pool's, but with the root's own identity, so a
    record's CMD_SetModelStream can name this stream."""
    from read_stock_toc import parse
    identities = [(identity, name)] + list(shared) + list(forced)
    reference_start = 36 + 8 * len(identities)
    data = bytearray(reference_start + 4 * (len(identities) - 1))
    struct.pack_into("<IIHHHH", data, 0, 0x9CBA7B28, 10, 1, len(identities), len(identities) - 1, 0)
    struct.pack_into("<II4BII", data, 16, 36, content_size, len(shared), len(forced), 0, 0, 0, reference_start)
    for i, (value, text) in enumerate(identities):
        struct.pack_into("<II", data, 36 + i * 8, value, len(data))
        data.extend(text.encode("ascii") + b"\0")
        if i:
            struct.pack_into("<I", data, reference_start + (i - 1) * 4, 36 + i * 8)
    data.extend(bytes((-len(data)) % 2048))
    parsed = parse(data)["entries"][0]
    if parsed["content_size"] != content_size or int(parsed["identity"]["hash"], 16) != identity:
        raise ValueError("TOC round-trip mismatch")
    return bytes(data)


def append_skin(data, work, entity, variant, taken_ids):
    """Copy the base model with recoloured textures; returns (old model guid,
    new model guid, report) and leaves the asset chunks in report["chunks"]
    for the variant's own model stream."""
    import ds2str
    skin = variant["skin"]
    archive = work / CHARACTER_STREAMS / skin["package"]
    old = model_guid(entity)
    # "model": another package's MetaModel for this record (the look of
    # slasher_var1 on the proven npcs_admin slasher); the record's own model
    # and the package's are both mapped onto the copy
    look = skin.get("model", old)
    stream = ds2str.StreamSet(str(archive))
    new, source, out, report = skin_assets.build_skin(
        stream, int(look.split("-")[0], 16), variant["name"],
        None if skin.get("tint") is None else (skin["tint"] if isinstance(skin["tint"], dict) else tuple(float(v) for v in skin["tint"])),
        skin.get("tag", "1"), taken_ids,
        tints={k: (v if isinstance(v, dict) else tuple(float(x) for x in v)) for k, v in skin.get("tints", {}).items()},
        files={k: str(ROOT / v) for k, v in skin.get("files", {}).items()},
        # a record without CMD_SetBNK (the slasher family: the base banks are
        # the shared "zombie" set) gets no character copy; the skin then rides
        # on the AttachModel hook alone
        character_ids=((int(field_guid(entity, "CMD_SetRCB").split("-")[0], 16),
                        int(field_guid(entity, "CMD_SetBNK").split("-")[0], 16) if has_field(entity, "CMD_SetBNK") else None)
                       if has_field(entity, "CMD_SetRCB") else None),
        vfx=skin.get("vfx", {}), sounds=skin.get("sounds", {}),
        # packages whose SBK banks may hold a swapped event's sound (the base
        # package is searched first, a donor bank travels along unchanged)
        sound_packages=[(package, skin_assets.load_package(ds2str.StreamSet(str(work / CHARACTER_STREAMS / package))))
                        for package in skin.get("sound_packages", [])],
        # own sounds: WAVs in a project folder, encoded into private bank copies
        custom_sounds=({**skin["custom_sounds"], "folder": str(ROOT / skin["custom_sounds"]["folder"])}
                       if skin.get("custom_sounds") else None),
        # "ghost": {"package": ..., "material": ...}: every skinned material is
        # rebuilt on that package's material shader (the dementia pack boy)
        ghost=((skin_assets.load_package(ds2str.StreamSet(str(work / CHARACTER_STREAMS / skin["ghost"]["package"]))), skin["ghost"]["material"],
                bool(skin["ghost"].get("template_textures")), list(skin["ghost"].get("vfx", [])), list(skin["ghost"].get("replace", [])),
                ({**skin["ghost"]["distortion"], "items": skin_assets.load_package(ds2str.StreamSet(str(work / skin["ghost"]["distortion"]["archive"])))}
                 if skin["ghost"].get("distortion") else None))
               if skin.get("ghost") else None))
    if source != look:
        raise SystemExit("%s: the package model %s is not the wanted %s" % (variant["name"], source, look))
    report["record_model"] = old
    chunks = bytearray()
    for asset in out:
        chunks.extend(asset_chunks(asset))
        taken_ids.add(struct.unpack_from("<I", asset["header"], 16)[0])
    report["chunks"] = bytes(chunks)
    report["content_size"] = sum(len(a["payload"]) for a in out)
    report["assets"] = [{"type": a["type"], "name": a["name"], "path": a["path"], "bytes": len(a["payload"]),
                         "sha256": hashlib.sha256(a["payload"]).hexdigest(), "source": a["source"]} for a in out]
    report["archive"] = str(archive.relative_to(work))
    report["tint"] = skin["tint"]
    return old, new, report


# The loose streams are installed below <game>\randomizer\; the engine builds
# the file path from the name, so the TOC root names carry the folder too.
STREAM_FOLDER = "randomizer\\"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=Path(r"D:/SteamLibrary/steamapps/common/Dead Space 2/work"))
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--spec", type=Path, default=ROOT / "variants/ds2_variants.json")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    import ds2str

    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    entities = {}
    entity_copies = {}
    index = defaultdict(list)
    by_archive = defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            entity = json.loads(line)
            node = compact(entity)
            index[node["guid"]].append(node)
            by_archive[node["archive"]].append(node)
            entities.setdefault(entity["guid"], entity)
            # a record GUID recurs in many level streams (the drop list of the
            # emergency slashers sits in 60 archives); a closure edit needs the
            # copy of the archive it is taken from, with that copy's offsets
            entity_copies[(entity["guid"], entity["archive"], entity["asset"])] = entity

    # Start from the pool as it was before any variant: the build appends,
    # so a rerun on its own output would carry every variant twice.
    base_pool = args.pool.with_name(args.pool.stem + ".base.str")
    if base_pool.exists():
        args.pool.write_bytes(base_pool.read_bytes())
        manifest_path = args.pool.with_suffix(".toc.json")
        manifest = json.loads(manifest_path.read_text())
        manifest.pop("variants", None)
        manifest["forced"] = [e for e in manifest["forced"] if not e[1].startswith("ds2_randomizer_")]
        manifest["stream_sha256"] = hashlib.sha256(args.pool.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest, indent=1))
        print("pool restored from %s" % base_pool.name)
    else:
        raise SystemExit("missing %s (the pool before variants); refusing to append twice" % base_pool)
    existing = pool_identities(args.pool)
    data = bytearray(args.pool.read_bytes())
    cut = trailing_padding(data)
    print("pool: %d bytes, real content ends at %d, %d identities" % (len(data), cut, len(existing)))
    del data[cut:]
    rows, appended, skins, model_streams = [], [], {}, []
    # One model stream for every skin. The stream manager registers at most
    # 32 TOCs (0x007494FE: count < 0x20, otherwise the TOC is parsed and
    # silently dropped); pool + 14 skin streams left the level's region TOCs
    # unregistered and the level manager crashed on the null (0x00749609,
    # three dumps on 14.09.). Every skin's assets and root template go into
    # this one stream; every skinned record names its GUID.
    merged_stream = {"name": "ds2_randomizer_models", "guid": variant_guid("36C80000-4D4F4445-574F5242-3130314E", "ds2_randomizer_models"),
                     "chunks": bytearray(), "content_size": 0, "assets": 0, "sources": [], "roots": 0, "root_owners": [], "root_records": 0}
    merged_stream["identity"] = guid_hash(merged_stream["guid"])
    used = set(existing)
    used.add(merged_stream["guid"])
    taken_ids = pool_resource_ids(args.pool)
    # "model_sources": "all": one INI variant that covers every source record
    # of the model (direct, vent, scripted), because the randomizer replaces
    # records per spawn contract, not models. Each source becomes a record
    # copy under a private name; they share the INI name, the skin, the
    # character copy and one model stream.
    catalog_rows = {}
    row_pattern = re.compile(r'\{\{0x([0-9A-F]{8})u,0x([0-9A-F]{8})u,0x([0-9A-F]{8})u,0x([0-9A-F]{8})u\},\{[^}]*\},"([A-Za-z]+)","([A-Za-z_|]*)",(true|false),(true|false)')
    for m in row_pattern.finditer((ROOT / "src/generated_enemy_catalog.h").read_text(encoding="utf-8")):
        catalog_rows["-".join(m.group(1, 2, 3, 4))] = (m.group(5), m.group(6), m.group(7) == "true", m.group(8) == "true")
    expanded = []
    for variant in spec["variants"]:
        variant = dict(variant)
        variant.setdefault("ini_name", variant["name"])
        if variant.get("model_sources") != "all":
            expanded.append(variant)
            continue
        look = model_guid(entities[variant["source"]])
        chosen = []
        for guid, (family, context, ordinary, ambiguous) in sorted(catalog_rows.items()):
            if family != variant["family"] or not ordinary or ambiguous or guid not in entities:
                continue
            direct = context in ("default_NONE_descriptor", "SPAWNMODE_NONE")
            scripted = context in ("SPAWNMODE_ANIM", "SPAWNMODE_ZOMBIE_PLAYDEAD", "SPAWNMODE_INFECTED")
            # the same vent kinds the plugin's ventContext() accepts
            vent = context in ("SPAWNMODE_VENT|Wall", "SPAWNMODE_VENT|Ceiling", "SPAWNMODE_VENT|default_FLOOR_descriptor", "SPAWNMODE_VENT|Floor")
            if not (direct or vent or scripted):
                continue
            try:
                if model_guid(entities[guid]) != look:
                    continue
            except SystemExit:
                continue
            if len(index.get(guid, [])) != 1:
                continue
            # a record authored with CMD_SetTrackingGroup1 = 0 never targets the
            # player (scripted slashers, transport_hub_a); no copy of those
            if any(f.get("name") == "CMD_SetTrackingGroup1" and f.get("value") == 0
                   for img in entities[guid]["images"] for f in img.get("fields", [])):
                continue
            chosen.append(guid)
        if variant["source"] not in chosen:
            chosen.insert(0, variant["source"])
        print("%s: model %s, %d source records" % (variant["name"], look, len(chosen)))
        for i, guid in enumerate(chosen):
            sub = dict(variant)
            sub["name"] = "%s_%02d" % (variant["name"], i)
            sub["source"] = guid
            expanded.append(sub)
    skin_cache = {}
    identity_owner = {}
    shared_maps = {}
    for variant in expanded:
        name, source = variant["name"], variant["source"]
        label = variant["ini_name"]
        matches = index.get(source, [])
        if len(matches) != 1:
            raise SystemExit("%s: source %s has %d catalog matches" % (name, source, len(matches)))
        result = closure({"guid": source, "source_archive": matches[0]["archive"], "package": name}, index)
        nodes = {}
        for node in result["nodes"]:
            if node["spawn_flags"]:
                continue
            nodes.setdefault((node["archive"], node["asset"], node["index"]), node)
        template_nodes = {}
        if variant.get("skin"):
            # the whole package template travels into the model stream: its
            # first record is the character template the figure is built from.
            # One copy per skin: the later source records of a model share it
            # (96 copies of the super slasher template made a 43 MB pool). The
            # template's records are recognised by their archive, whether they
            # came in through by_archive or through the record's closure.
            package_suffix = ("\\" + variant["skin"]["package"].replace("/", "\\")).lower()
            template_archive = str(CHARACTER_STREAMS / variant["skin"]["package"]).replace("/", "\\")
            for node in by_archive.get(template_archive, []):
                if not node["spawn_flags"]:
                    template_nodes.setdefault((node["archive"], node["asset"], node["index"]), node)
            for key, node in list(nodes.items()):
                if node["archive"].lower().endswith(package_suffix):
                    template_nodes.setdefault(key, node)
            if label not in skin_cache:
                nodes.update(template_nodes)
        # Every closure record except the source itself is shared between the
        # source records of one model (the 168 KB global_assets slice with the
        # drop list and dismemberment prop was copied 96 times, 43 MB of pool).
        shared = shared_maps.setdefault(label, {})
        if variant.get("model_sources") == "all":
            nodes = {key: node for key, node in nodes.items() if node["guid"] == source or node["guid"] not in shared}
        mapping = {node["guid"]: variant_guid(node["guid"], name) for node in nodes.values()}
        for old_guid, target in mapping.items():
            if target in used:
                raise SystemExit("%s: identity %s already taken (node %s; first taken by %s)" % (name, target, old_guid, identity_owner.get(target, "the pool")))
            used.add(target)
            identity_owner[target] = "%s node %s" % (name, old_guid)
        if variant.get("model_sources") == "all":
            for old_guid, target in mapping.items():
                if old_guid != source: shared.setdefault(old_guid, target)
            mapping.update(shared)   # already taken by an earlier copy, on purpose
        hashes = [guid_hash(g) for g in used]
        if len(set(hashes)) != len(hashes):
            raise SystemExit("%s: identity hash collision" % name)
        entity = entities[source]
        model_stream = None
        if variant.get("skin") and label in skin_cache:
            # a later source record of the same model: same skin, character
            # and model stream, only the references are mapped
            cache = skin_cache[label]
            old_model = model_guid(entity)
            if cache["source_model"] != old_model:
                mapping[cache["source_model"]] = cache["model"]
                skins[label]["record_model_rewrite"] = (old_model, cache["model"])
            else:
                mapping[old_model] = cache["model"]
                skins[label].pop("record_model_rewrite", None)
            for old_guid, new_guid in cache["character_map"].items():
                mapping[old_guid] = new_guid
            mapping[stream_guid(entity)] = cache["stream"]
        elif variant.get("skin"):
            old_model, new_model, skin_report = append_skin(data, args.work, entity, variant, taken_ids)
            if skin_report["source_model"] != old_model:
                # another package's look: the template's model maps onto the
                # copy, the record's own CMD_SetModel is rewritten in place
                # below (two sources onto one destination would trip remap)
                mapping[skin_report["source_model"]] = new_model
                skin_report["record_model_rewrite"] = (old_model, new_model)
            else:
                mapping[old_model] = new_model
            for old_guid, new_guid in skin_report["character_map"].items():
                mapping[old_guid] = new_guid
            # The walking character is assembled from the model stream's root
            # template, not from the record's CMD_SetModel (skin_build log,
            # 12.09.: both figures came from the base meshes). Retail gives
            # every look its own stream, so the skin gets one too: the copied
            # assets plus the package template as its root SimGroup, and the
            # record's CMD_SetModelStream is pointed at it.
            old_stream = stream_guid(entity)
            new_stream = merged_stream["guid"]
            mapping[old_stream] = new_stream
            merged_stream["chunks"].extend(skin_report.pop("chunks"))
            merged_stream["content_size"] += skin_report.pop("content_size")
            merged_stream["assets"] += len(skin_report["assets"])
            merged_stream["sources"].append(old_stream)
            model_stream = {"name": merged_stream["name"], "guid": new_stream, "identity": merged_stream["identity"],
                            "source": old_stream, "archive": str(CHARACTER_STREAMS / variant["skin"]["package"]).replace("/", "\\"),
                            "skin": label}
            skin_report["model_stream"] = dict(model_stream)
            skins[label] = skin_report
            skin_cache[label] = {"model": new_model, "source_model": skin_report["source_model"],
                                 "character_map": dict(skin_report["character_map"]), "stream": new_stream}
            print("%-16s skin %s -> %s  textures=%s materials=%d meshes=%d volatiles=%d character=%s" % (
                name, old_model, new_model, ",".join(skin_report["textures"].values()),
                skin_report["materials"], skin_report["meshes"], skin_report["volatiles"], skin_report.get("character", {}).get("name")))
        # The record's own fields, addressed by component class and field name.
        wanted = dict(variant["fields"])
        edits = {}
        for image in entity["images"]:
            for field in image.get("fields", []):
                key = image.get("class", "") + "." + (field.get("name") or "")
                if key not in wanted:
                    continue
                edits[field["offset"] + 8] = (key, field["flags"], field["value"], wanted[key])
        missing = sorted(set(wanted) - {k for k, _f, _v, _s in edits.values()})
        if missing and variant.get("model_sources") == "all":
            # source records of one model differ in what they author; a field
            # a record does not carry keeps its engine default there
            print("%-16s without %s" % (name, ", ".join(missing)))
        elif missing:
            raise SystemExit("%s: fields not on the record: %s" % (name, ", ".join(missing)))
        groups = defaultdict(dict)
        for (archive, asset, idx), node in nodes.items():
            groups[(archive, asset)][idx] = node
        changed = []
        for (archive, asset), group in sorted(groups.items()):
            stream = ds2str.StreamSet(args.work / archive)
            payload = None
            for meta, chunks in assets(stream):
                if meta["type"] == "SimGroup" and meta["path"] == asset:
                    raw = stream.asset_payload(chunks, meta["size"])
                    payload, selected, _unused = isolate_many(raw, group)
                    if [r["guid"] for r in selected] != [group[i]["guid"] for i in sorted(group)]:
                        raise SystemExit("closure does not match the source: " + asset)
                    break
            if payload is None:
                raise SystemExit("asset missing in %s: %s" % (archive, asset))
            payload = bytearray(payload)
            # "drop_list": the record's own drop list travels in the closure
            # (ItemDropListComponent.CMD_SetItemDropListGUID); its items and
            # weights are rewritten in the copy. A GUID field's dword is the
            # distance from that dword to the 16 byte GUID it names (checked
            # against the catalog before every write, 14.09.), so an item slot
            # can be pointed at any resident item: the golden variant drops a
            # Power Node, a Large Health Kit or Stasis Energy.
            drop = variant.get("drop_list")
            if drop and has_field(entity, "CMD_SetItemDropListGUID"):
                list_guid = field_guid(entity, "CMD_SetItemDropListGUID")
                list_entity = entity_copies.get((list_guid, archive, asset))
                if list_entity and any(n["guid"] == list_guid for n in group.values()):
                    items_wanted = list(drop["items"])
                    weights = list(drop.get("weights") or [1.0] * len(items_wanted))
                    written_guids = set()
                    drop_changed = []
                    slots = 0
                    for image in list_entity["images"]:
                        if image.get("class") != "ItemDropList":
                            continue
                        for field in image.get("fields", []):
                            fname = field.get("name") or ""
                            if fname == "CMD_DynamicWeight" and field.get("flags") == 0 and "dynamic_weight" in drop:
                                # the engine's own need-based drop (ammo for the
                                # gun in hand, health when low) competes with the
                                # items by this weight; 50 against three ones
                                # never showed the golden items (14.09.). Never 0: with
                                # a zero dynamic weight the component's construction
                                # crashed at spawn (deadspace2+0x418F9D, three dumps 21:02-21:14)
                                struct.pack_into("<I", payload, field["offset"] + 8, apply_value(field["value"], 0, "=%g" % max(0.01, float(drop.get("dynamic_weight", 1.0)))))
                                drop_changed.append(("ItemDropList.CMD_DynamicWeight", field["value"], struct.unpack_from("<I", payload, field["offset"] + 8)[0], 0))
                            elif fname == "CMD_DropChance":
                                struct.pack_into("<I", payload, field["offset"] + 8, apply_value(field["value"], field["flags"], "=%g" % float(drop.get("drop_chance", 1.0))))
                                drop_changed.append(("ItemDropList.CMD_DropChance", field["value"], struct.unpack_from("<I", payload, field["offset"] + 8)[0], 0))
                            elif fname.startswith("CMD_Weight") and fname[10:].isdigit():
                                n = int(fname[10:])
                                weight = weights[n - 1] if n <= len(items_wanted) else 0.0
                                struct.pack_into("<I", payload, field["offset"] + 8, apply_value(field["value"], field["flags"], "=%g" % weight))
                                drop_changed.append(("ItemDropList." + fname, field["value"], struct.unpack_from("<I", payload, field["offset"] + 8)[0], 0))
                            elif fname in ("CMD_HealthList", "CMD_AmmoList") and field.get("flags") == 12:
                                # the low-health and low-ammo sub-lists would
                                # bypass the golden items; point both at the
                                # list itself (remapped to the copy below)
                                at = field["offset"] + 8 + field["value"]
                                old_bytes = struct.pack("<4I", *(int(w, 16) for w in field["guid_candidate"].split("-")))
                                self_bytes = struct.pack("<4I", *(int(w, 16) for w in list_guid.split("-")))
                                if bytes(payload[at:at + 16]) not in (old_bytes, self_bytes):
                                    raise SystemExit("%s: %s does not sit at its reference" % (name, fname))
                                payload[at:at + 16] = self_bytes
                                drop_changed.append(("ItemDropList." + fname, 0, 0, 12))
                            elif fname.startswith("CMD_Item") and fname[8:].isdigit() and field.get("flags") == 12:
                                n = int(fname[8:])
                                if n > len(items_wanted):
                                    continue
                                at = field["offset"] + 8 + field["value"]
                                old_bytes = struct.pack("<4I", *(int(w, 16) for w in field["guid_candidate"].split("-")))
                                new_bytes = struct.pack("<4I", *(int(w, 16) for w in items_wanted[n - 1].split("-")))
                                current = bytes(payload[at:at + 16])
                                if current != old_bytes and current not in written_guids:
                                    raise SystemExit("%s: drop list item %d does not sit at its reference (%s)" % (name, n, field["guid_candidate"]))
                                payload[at:at + 16] = new_bytes
                                written_guids.add(new_bytes)
                                slots += 1
                                drop_changed.append(("ItemDropList." + fname, 0, n, 12))
                    if slots < len(items_wanted):
                        raise SystemExit("%s: drop list has %d item slots, %d items wanted" % (name, slots, len(items_wanted)))
                    print("%-16s drop list %s: %d items, weights %s, %d fields written" % (name, list_guid, slots, weights, len(drop_changed)))
            if asset == entity["asset"] and archive == entity["archive"]:
                for offset, (key, flags, authored, value_spec) in sorted(edits.items()):
                    current = struct.unpack_from("<I", payload, offset)[0]
                    if current != authored:
                        raise SystemExit("%s: %s at %d holds %d, catalog says %d" % (name, key, offset, current, authored))
                    new = apply_value(current, flags, value_spec)
                    struct.pack_into("<I", payload, offset, new)
                    changed.append((key, current, new, flags))
                if label in skins and skins[label].get("record_model_rewrite"):
                    from_guid, to_guid = skins[label]["record_model_rewrite"]
                    from_bytes = struct.pack("<4I", *(int(w, 16) for w in from_guid.split("-")))
                    to_bytes = struct.pack("<4I", *(int(w, 16) for w in to_guid.split("-")))
                    if from_bytes not in payload:
                        raise SystemExit("%s: the record does not name its model %s" % (name, from_guid))
                    payload = bytearray(bytes(payload).replace(from_bytes, to_bytes))
            # Nicole's ghost effect as a component on the record: the
            # DynamicDistortionComponent (type CFC58CC1) with the VFX and the
            # bone it hangs from. Added after every offset-based edit above,
            # before the remap, which re-reads the group (simgroup_inject).
            if label in skins and skins[label].get("ghost_distortion") and asset == entity["asset"] and archive == entity["archive"]:
                from simgroup_inject import add_component
                gd = skins[label]["ghost_distortion"]
                payload = bytearray(add_component(bytes(payload), entity["guid"], (0xCFC58CC1, 1),
                                                  zlib.crc32(("ddc" + entity["guid"]).encode()) & 0xFFFFFFFF,
                                                  [(1425693326, 12, gd["vfx_guid"]), (3895446034, 10, int(gd["bone_hash"], 16))]))
                print("%-16s distortion component: %s on bone %s" % (name, gd["vfx"], gd["bone"]))
            raw_group = bytes(payload)
            group_guids = [group[i]["guid"] for i in sorted(group)]
            payload, stats = remap(bytes(payload), mapping)
            if label in skins and asset == entity["asset"] and archive == entity["archive"]:
                if struct.pack("<4I", *(int(w, 16) for w in skins[label]["source_model"].split("-"))) in payload:
                    raise SystemExit("%s: CMD_SetModel still names the base model" % name)
            header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
            struct.pack_into("<I", header, 32, len(payload))
            asset_name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2variant/" + name + "|" + archive + "|" + asset)).upper()
            path = "variant_donors/%s/%s.simgroup.bin" % (name, hashlib.sha256((archive + "|" + asset).encode()).hexdigest()[:12])
            body = bytearray(header)
            body.extend((asset_name + "\0" + path + "\0SimGroup\0").encode("latin1"))
            data.extend(chunk(bytes(body)))
            for start in range(0, len(payload), MAX_CHUNK - 12):
                data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
            appended.append({"variant": name, "archive": archive, "asset": asset, "path": path, "stream": "pool",
                             "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                             "identities": stats["identities"], "references": stats["references"]})
            if model_stream and archive == model_stream["archive"]:
                # The package template stays in the pool under the variant
                # identities: the record's references to it (the dismemberment
                # prop the detached parts spawn from) resolve when the pool
                # registers, and a record that only exists in a later stream
                # fell back to the pool exploder's prop (attach_model log,
                # 13.09.). The model stream still needs a root, so a second
                # copy under root identities becomes that root.
                root_mapping = {node["guid"]: variant_guid(node["guid"], name + ".root") for node in group.values()}
                for old_guid, new_guid in mapping.items():
                    if old_guid not in root_mapping:
                        root_mapping[old_guid] = new_guid
                for target_guid in root_mapping.values():
                    if target_guid in used and target_guid not in mapping.values():
                        raise SystemExit("%s: root identity %s already taken" % (name, target_guid))
                used.update(root_mapping.values())
                root_payload, root_stats = remap(bytes(raw_group), root_mapping)
                root_header = bytearray(header); struct.pack_into("<I", root_header, 32, len(root_payload))
                root_body = bytearray(root_header)
                if merged_stream["roots"] == 0:
                    root_name, root_path = uuid_name(merged_stream["guid"]), "levels\\ds2_randomizer\\%s.simgroup.bin" % merged_stream["name"]
                else:
                    root_name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2variant/root/" + label)).upper()
                    root_path = "levels\\ds2_randomizer\\models\\%s.simgroup.bin" % label.lower()
                root_body.extend((root_name + "\0" + root_path + "\0SimGroup\0").encode("latin1"))
                merged_stream["chunks"].extend(chunk(bytes(root_body)))
                for start in range(0, len(root_payload), MAX_CHUNK - 12):
                    merged_stream["chunks"].extend(chunk(b"TADS" + root_payload[start:start + MAX_CHUNK - 12]))
                merged_stream["content_size"] += len(root_payload)
                merged_stream["roots"] += 1
                merged_stream["root_records"] += root_stats["identities"]
                merged_stream["root_owners"].extend(sorted(root_mapping[g] for g in group_guids))
                model_stream["root_records"] = root_stats["identities"]
                model_stream["root_owners"] = sorted(root_mapping[g] for g in group_guids)
                model_stream["root_asset"] = root_path
                skins[label]["model_stream"]["root_asset"] = root_path
                appended.append({"variant": name, "archive": archive, "asset": asset, "path": "root", "stream": model_stream["name"],
                                 "bytes": len(root_payload), "sha256": hashlib.sha256(root_payload).hexdigest(),
                                 "identities": root_stats["identities"], "references": root_stats["references"]})
        if model_stream:
            if "root_records" not in model_stream:
                raise SystemExit("%s: the package template is not in the closure, no model stream root" % name)
            print("%-16s model stream %s (shared) guid=%s root_records=%d" % (
                name, model_stream["name"], model_stream["guid"], model_stream["root_records"]))
        if len(changed) != len(edits):
            raise SystemExit("%s: %d of %d fields written" % (name, len(changed), len(edits)))
        print("%-16s %s <- %s  records=%d fields=%d" % (name, mapping[source], source, len(nodes), len(changed)))
        for key, old, new, flags in changed:
            if flags == 0:
                fo = struct.unpack("<f", struct.pack("<I", old))[0]; fn = struct.unpack("<f", struct.pack("<I", new))[0]
                print("     %-52s %10.3g -> %-10.3g" % (key, fo, fn))
            else:
                print("     %-52s %10d -> %-10d" % (key, old, new))
        rows.append({"guid": mapping[source], "source": source, "family": variant["family"],
                     "name": label, "chance": int(variant.get("chance", 100)), "base": variant.get("base", ""),
                     "speed": int(round(float(variant.get("speed", 1.0)) * 100)),
                     "scale": int(round(float(variant.get("scale", 1.0)) * 100))})
        if label in skins:
            # every record of the closure is a skin owner (the record, the
            # package template, the dismemberment prop the detached parts spawn
            # from): Entity::AttachModel is corrected for all of them, or the
            # parts fall back to the base look (13.09.)
            skins[label]["owners"] = sorted(set(skins[label].get("owners", [])) | set(mapping[node["guid"]] for node in nodes.values()) | set(model_stream.get("root_owners", []) if model_stream else []))

    data = bytearray(frame_stream(bytes(data)))
    print("appended %d groups for %d variants, pool now %d bytes" % (len(appended), len(rows), len(data)))
    if not args.write:
        print("dry run; pass --write to emit")
        return
    args.pool.write_bytes(bytes(data))
    manifest_path = args.pool.with_suffix(".toc.json")
    manifest = json.loads(manifest_path.read_text())
    from build_research_toc import build as build_pool_toc
    forced = [tuple(e) for e in manifest["forced"] if not e[1].startswith("ds2_randomizer_")]
    written = []
    if merged_stream["roots"]:
        model_streams.append(merged_stream)
    for old_file in args.pool.parent.glob("ds2_randomizer_*"):
        # earlier per-skin streams would be picked up by the packer's glob
        if old_file.suffix in (".str", ".toc") and old_file.stem not in ("ds2_randomizer_pool", merged_stream["name"]) and "base" not in old_file.name:
            old_file.unlink()
    for ms in model_streams:
        stream_bytes = frame_stream(bytes(HEADER) + bytes(ms["chunks"]))
        stream_path = args.pool.parent / (ms["name"] + ".str")
        stream_path.write_bytes(stream_bytes)
        toc = build_toc(STREAM_FOLDER + ms["name"], ms["identity"], ms["content_size"], [tuple(e) for e in manifest["shared"]], [])
        stream_path.with_suffix(".toc").write_bytes(toc)
        # not a TOC dependency: the engine resolves those against loaded TOCs
        # only and crashed on the null (dump 45636); the plugin requests the
        # stream by name, like the pool itself
        written.append({"name": ms["name"], "guid": ms["guid"], "identity": "%08X" % ms["identity"], "sources": ms["sources"],
                        "bytes": len(stream_bytes), "stream_sha256": hashlib.sha256(stream_bytes).hexdigest(),
                        "toc_sha256": hashlib.sha256(toc).hexdigest(), "content_size": ms["content_size"], "assets": ms["assets"],
                        "roots": ms["roots"], "root_records": ms["root_records"]})
        print("wrote %s (%d bytes) and its TOC" % (stream_path.name, len(stream_bytes)))
    toc = build_pool_toc(STREAM_FOLDER + manifest["root_name"], manifest["content_size"], [tuple(e) for e in manifest["shared"]], forced)
    args.pool.with_suffix(".toc").write_bytes(toc)
    manifest["forced"] = [list(e) for e in forced]
    manifest["toc_bytes"] = len(toc)
    manifest["toc_sha256"] = hashlib.sha256(toc).hexdigest()
    manifest["variants"] = {"rows": rows, "appended": appended, "skins": skins, "model_streams": written}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    refresh_manifest(args.pool)

    def words(text):
        return "{" + ",".join("0x%su" % w for w in text.split("-")) + "}"

    lines = ["// Generated by tools/build_variants.py from variants/ds2_variants.json.", "#pragma once",
             "namespace ds2 {",
             "// A stat variant the pool carries: a copy of `source` with some authored",
             "// numbers changed, everything else the base enemy's. `chance` is the",
             "// percentage of spawns it competes in, decided per spawn by the seed.",
             "// `speed` and `scale` are percentages applied at runtime: the animation",
             "// playback factor (movement follows it) and the visual size.",
             "// `skinModel` is the skin's MetaModel (zero without a skin): the walking",
             "// figure takes its model from Entity::AttachModel, and the plugin swaps the",
             "// base model for this one there (runtime_variants.inc).",
             "struct VariantDonorRow { Guid guid, source; const char* family; const char* name; unsigned chance, speed, scale; Guid skinModel; };",
             "inline constexpr VariantDonorRow kVariantDonors[] = {"]
    for row in sorted(rows, key=lambda r: r["guid"]):
        skin_model = skins.get(row["name"], {}).get("model", "00000000-00000000-00000000-00000000")
        lines.append("    {%s,%s,%s,%s,%d,%d,%d,%s},  // %s" % (words(row["guid"]), words(row["source"]), json.dumps(row["family"]),
                                                              json.dumps(row["name"]), row["chance"], row["speed"], row["scale"], words(skin_model), row["base"]))
    if not rows:
        # a build without variants: one all-zero row keeps the array non-empty,
        # appendVariantDonors and the tests skip it
        lines.append('    {{0u,0u,0u,0u},{0u,0u,0u,0u},"","",0,100,100,{0u,0u,0u,0u}},  // no variants in this build')
    lines += ["};",
              "// Every record of a skinned variant's closure, each with the skin's MetaModel:",
              "// the detached parts and the template are entities of their own and take",
              "// their model through Entity::AttachModel like the walking figure.",
              "struct SkinOwnerRow { Guid guid, skinModel; };",
              "inline constexpr SkinOwnerRow kSkinOwners[] = {"]
    owner_count = 0
    for name in sorted(skins):
        for owner in skins[name].get("owners", []):
            lines.append("    {%s,%s},  // %s" % (words(owner), words(skins[name]["model"]), name)); owner_count += 1
    if not owner_count:
        lines.append("    {{0u,0u,0u,0u},{0u,0u,0u,0u}},")
    # The sound events a skin renames: the plugin swaps the name where the
    # Lua audio bridge (PlayEntityEmx) receives it, keyed by the skin model.
    lines += ["};", "",
              "// Sound events a skinned variant plays under another name (PlayEntityEmx hook).",
              "struct SkinSoundRow { Guid skinModel; const char* from; const char* to; };",
              "inline constexpr SkinSoundRow kSkinSounds[] = {"]
    sound_count = 0
    for name in sorted(skins):
        for old_event, info in sorted(skins[name].get("character", {}).get("sounds", {}).items()):
            lines.append('    {%s,"%s","%s"},  // %s' % (words(skins[name]["model"]), old_event, info["to"], name)); sound_count += 1
    if not sound_count:
        lines.append('    {{0u,0u,0u,0u},"",""},')
    lines += ["};", "}", ""]
    (ROOT / "src/generated_variant_donors.h").write_text("\n".join(lines), encoding="utf-8")
    print("wrote src/generated_variant_donors.h with %d variants; now run tools/embed_stream_requirements.py" % len(rows))


if __name__ == "__main__":
    main()
