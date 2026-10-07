"""Check a skin in the built pool the way the game will resolve it, offline.

For every skin listed in the pool manifest: the variant records name the new
MetaModel, the MetaModel's references (meshes, physics, effects) are in the
pool, every MeshVolatile carries the inverse of its mesh's identity, every
mesh's material key finds a material in the pool, and every texture identity a
copied material holds finds a tg4h/tg4d pair in the pool (the pool loads before
the base package, so nothing may be left to the base package). Exit code 1 on
the first failure.
"""
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ds2_paths import MODDING_TOOLS  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))

import ds2str  # noqa: E402
import skin_assets  # noqa: E402
from build_variants import uuid_name  # noqa: E402


def main():
    pool = ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str"
    manifest = json.loads(pool.with_suffix(".toc.json").read_text())
    skins = manifest.get("variants", {}).get("skins", {})
    if not skins:
        print("no skins in the manifest"); return 0
    items = skin_assets.load_package(ds2str.StreamSet(str(pool)))
    streams = {ms["name"]: ms for ms in manifest["variants"].get("model_streams", [])}
    for ms in streams.values():
        items += skin_assets.load_package(ds2str.StreamSet(str(pool.parent / (ms["name"] + ".str"))))
    if len(streams) > 1:
        print("FAIL %d model streams, one is the limit (32 TOC slots in the stream manager)" % len(streams))
        return 1
    words = lambda it: struct.unpack("<9I", it["header"])
    by_guid = {}
    textures = {}
    materials = {}
    for it in items:
        w = words(it)
        if it["meta"]["type"] in ("tg4h", "tg4d"):
            textures.setdefault(w[7], {})[it["meta"]["type"]] = it
        elif it["meta"]["resource_id"]:
            by_guid[skin_assets.guid16(w[4], w[7])] = it
        if it["meta"]["type"] == "Material":
            materials[struct.unpack_from("<I", it["payload"], 8)[0]] = it
    failures = 0

    def fail(text):
        nonlocal failures
        failures += 1
        print("FAIL " + text)

    # A bank's identity at +8 must be unique: a duplicate registers the same
    # animation keys twice and the engine's unregister loop never ends
    # (checkpoint switch hang, 13.09.). Every copy must differ from its source
    # and from every other bank in the streams.
    bank_ids = {}
    for it in items:
        if it["meta"]["type"] != "BNK" or len(it["payload"]) < 12:
            continue
        ident = struct.unpack_from("<I", it["payload"], 8)[0]
        if ident in bank_ids:
            fail("bank %s shares identity %08X with %s" % (it["meta"]["name"], ident, bank_ids[ident]))
        bank_ids[ident] = it["meta"]["name"]
    for name, skin in skins.items():
        for bank_name, ids in skin.get("character", {}).get("bank_ids", {}).items():
            if ids["from"] == ids["to"]:
                fail("%s: bank copy %s kept the source identity %s" % (name, bank_name, ids["from"]))
            if int(ids["from"], 16) in bank_ids:
                fail("%s: source identity %s of %s is still carried by %s" % (name, ids["from"], bank_name, bank_ids[int(ids["from"], 16)]))
        if skin.get("character") and "bank_ids" not in skin["character"]:
            fail("%s: skin built before bank identities were renamed; rebuild" % name)
    print("bank identities: %d banks, all distinct" % len(bank_ids))

    for name, skin in skins.items():
        model = struct.pack("<4I", *(int(x, 16) for x in skin["model"].split("-")))
        base = struct.pack("<4I", *(int(x, 16) for x in skin["source_model"].split("-")))
        ms = skin.get("model_stream")
        groups = [it for it in items if it["meta"]["type"] == "SimGroup" and (("variant_donors/%s" % name) in it["meta"]["path"] or (ms and ms["name"] in it["meta"]["path"]))]
        naming = sum(1 for g in groups if model in g["payload"])
        stale = sum(1 for g in groups if base in g["payload"])
        if naming < 1 or stale:
            fail("%s: %d variant groups name the skin model, %d still name the base" % (name, naming, stale))
        if ms:
            # one shared stream: its own root carries the stream's uuid name,
            # every skin's template copy is a root SimGroup of its own in it
            stream_root = [it for it in items if it["meta"]["type"] == "SimGroup" and it["meta"]["name"] == uuid_name(ms["guid"])]
            root = [it for it in items if it["meta"]["type"] == "SimGroup" and it["meta"]["path"] == ms.get("root_asset")]
            new_stream = struct.pack("<4I", *(int(x, 16) for x in ms["guid"].split("-")))
            old_stream = struct.pack("<4I", *(int(x, 16) for x in ms["source"].split("-")))
            pointing = sum(1 for g in groups if new_stream in g["payload"])
            stale_stream = sum(1 for g in groups if old_stream in g["payload"])
            if len(stream_root) != 1:
                fail("%s: model stream root SimGroup %s not found once (%d)" % (name, uuid_name(ms["guid"]), len(stream_root)))
            if len(root) != 1:
                fail("%s: the skin's root SimGroup %s not found once (%d)" % (name, ms.get("root_asset"), len(root)))
            elif model not in root[0]["payload"]:
                fail("%s: the skin's root SimGroup does not name the skin model" % name)
            if pointing < 1 or stale_stream:
                fail("%s: %d groups name the model stream, %d still name the base stream" % (name, pointing, stale_stream))
            identity = ms["identity"] if isinstance(ms["identity"], int) else int(ms["identity"], 16)
            toc = (pool.parent / (ms["name"] + ".toc")).read_bytes()
            if struct.unpack_from("<I", toc, 36)[0] != identity:
                fail("%s: model stream TOC identity mismatch" % name)
            if any(e[1] == ms["name"] for e in manifest["forced"]):
                fail("%s: pool TOC must not force the model stream (unresolvable dependency, dump 45636)" % name)
            print("%s: model stream %s root ok, %d groups point at it" % (name, ms["name"], pointing))
        for old_guid, new_guid in skin.get("character_map", {}).items():
            old_bytes = struct.pack("<4I", *(int(x, 16) for x in old_guid.split("-")))
            new_bytes = struct.pack("<4I", *(int(x, 16) for x in new_guid.split("-")))
            copy = by_guid.get(new_bytes)
            if copy is None:
                fail("%s: character copy %s missing" % (name, new_guid)); continue
            if any(old_bytes in g["payload"] for g in groups):
                fail("%s: a variant group still names the base %s %s" % (name, copy["meta"]["type"], old_guid))
            if not any(new_bytes in g["payload"] for g in groups):
                # a character's banks may be bound by name hash alone (the
                # slasher family has no CMD_SetBNK); only the RCB must be named
                if copy["meta"]["type"] == "BNK":
                    print("%s: bank copy %s bound by character hash, not by a record" % (name, copy["meta"]["name"]))
                else:
                    fail("%s: no variant group names the %s copy" % (name, copy["meta"]["type"]))
        if skin.get("character"):
            ch = skin["character"]
            rcb = [it for it in items if it["meta"]["type"] == "RCB" and it["meta"]["name"].startswith(ch["name"])]
            # the name string is optional (superslasher, slasherhospital keep none); the hash at +8 is what binds
            if len(rcb) != 1 or struct.unpack_from("<I", rcb[0]["payload"], 8)[0] != int(ch["hash"], 16):
                fail("%s: RCB copy does not carry character %s / hash %s" % (name, ch["name"], ch["hash"]))
            else:
                print("%s: character %s (hash %s), RCB and BNK copies named by the records" % (name, ch["name"], ch["hash"]))
        if skin.get("character", {}).get("sounds"):
            bnk = [it for it in items if it["meta"]["type"] == "BNK" and it["meta"]["name"].startswith(skin["character"]["name"])]
            if len(bnk) != 1:
                fail("%s: BNK copy not found once" % name)
            else:
                bank = bnk[0]["payload"]; main_size = struct.unpack_from("<I", bank, 0)[0]
                dwords = struct.unpack_from("<%dI" % (main_size // 4), bank, 0)
                for old_event, info in skin["character"]["sounds"].items():
                    pos = bank.find(info["to"].encode() + bytes([0]))
                    refs = sum(1 for k in range(1, len(dwords)) if dwords[k] == pos and dwords[k - 1] == 2)
                    if pos < 0 or pos >= main_size:
                        fail("%s: swapped sound %s lies outside the bank's main block" % (name, info["to"]))
                    elif refs != info["references"]:
                        fail("%s: %s has %d references, expected %d" % (name, info["to"], refs, info["references"]))
                print("%s: %d sound events swapped inside the bank (main block %d bytes)" % (name, len(skin["character"]["sounds"]), main_size))
        if skin.get("character", {}).get("sound_banks") is not None:
            # every event the bank copy names must hash into a carried SBK
            # (the sound is looked up by name hash across resident banks only)
            ch = skin["character"]
            banks = {it["meta"]["name"]: it for it in items if it["meta"]["type"] == "SBK"}
            missing_banks = sorted(set(ch["sound_banks"]) - set(banks))
            if missing_banks:
                fail("%s: sound banks not in the streams: %s" % (name, ", ".join(missing_banks)))
            bnk = [it for it in items if it["meta"]["type"] == "BNK" and it["meta"]["name"].startswith(ch["name"])]
            if len(bnk) == 1 and not missing_banks:
                bank = bnk[0]["payload"]; main_size = struct.unpack_from("<I", bank, 0)[0]
                # a plain Lua handler name (exploder_muttering_short) plays as name + ".emx"
                events = set(info["to"] if info["to"].endswith(".emx") else info["to"] + ".emx" for info in ch.get("sounds", {}).values())
                # the same window the build resolves: the audio table at the
                # tail of the main block (the puker names two impact events
                # of other packages outside it, retail behaviour, 14.09.)
                table = bank.find(b"___audio: play")
                for m in re.finditer(rb"[a-z0-9_]+\.emx", bank[table:main_size] if table >= 0 else b""):
                    events.add(m.group().decode().lstrip("_"))   # the table's underscore padding
                unresolved = []
                for event in sorted(events):
                    key = struct.pack("<I", skin_assets.name_hash(event))
                    held = False
                    for bank_name in ch["sound_banks"]:
                        head = banks[bank_name]["payload"]
                        if head[:struct.unpack_from("<I", head, 8)[0]].find(key) >= 0:
                            held = True
                    if not held:
                        unresolved.append(event)
                if unresolved:
                    fail("%s: events without a carried sound bank: %s" % (name, ", ".join(unresolved)))
                else:
                    print("%s: %d sound events resolve in %d carried banks (%s)" % (name, len(events), len(ch["sound_banks"]), ", ".join(sorted(ch["sound_banks"]))))
        mm = by_guid.get(model)
        if mm is None or mm["meta"]["type"] != "MetaModel":
            fail("%s: MetaModel %s missing" % (name, skin["model"])); continue
        base_id = struct.pack("<I", int(skin["source_model"].split("-")[0], 16))
        stale_ids = sum(1 for at in range(0, len(mm["payload"]) - 3, 4) if mm["payload"][at:at + 4] == base_id)
        if stale_ids:
            fail("%s: MetaModel copy still carries the base id %d times" % (name, stale_ids))
        refs = [g for g in by_guid if g != model and g in mm["payload"]]
        kinds = {}
        for g in refs:
            kinds[by_guid[g]["meta"]["type"]] = kinds.get(by_guid[g]["meta"]["type"], 0) + 1
        meshes = [by_guid[g] for g in refs if by_guid[g]["meta"]["type"] == "Mesh"]
        if len(meshes) != skin["meshes"]:
            fail("%s: MetaModel resolves %d meshes in the pool, the skin copied %d" % (name, len(meshes), skin["meshes"]))
        for mesh in meshes:
            w = words(mesh)
            vol = by_guid.get(skin_assets.guid16(~w[4] & 0xFFFFFFFF, ~w[7] & 0xFFFFFFFF))
            if vol is not None and vol["meta"]["type"] != "MeshVolatile":
                fail("%s: inverse identity of %s is not a MeshVolatile" % (name, mesh["meta"]["name"]))
            keys = [struct.unpack_from("<I", mesh["payload"], at)[0] for at in range(0, len(mesh["payload"]) - 3, 4)]
            found = [k for k in keys if k in materials]
            if not found:
                fail("%s: mesh %s holds no material key known to the pool" % (name, mesh["meta"]["name"]))
        checked = 0
        ghost = skin.get("ghost", {})
        for mat in {id(m): m for m in materials.values()}.values():
            if name not in mat["meta"]["name"]:
                continue
            body = mat["payload"]
            if mat["meta"]["name"] in ghost:
                # a ghost material keeps the template's path strings; what
                # binds are the identity dwords, which must all find a texture
                held_ids = [struct.unpack_from("<I", body, at)[0] for at in range(0, len(body) - 3, 4)]
                wanted = [ghost[mat["meta"]["name"]]["colour"], ghost[mat["meta"]["name"]]["normal"]] + ghost[mat["meta"]["name"]]["extra"]
                for stem in wanted:
                    pair = next((textures[c] for c in textures if "tg4h" in textures[c] and textures[c]["tg4h"]["meta"]["name"] == stem), None)
                    ident = struct.unpack_from("<I", pair["tg4h"]["payload"], 0x14)[0] if pair else None
                    if pair is None or ident not in held_ids or "tg4d" not in pair:
                        fail("%s: ghost material %s needs texture %s, not bound or not in the pool" % (name, mat["meta"]["name"], stem))
                    else:
                        checked += 1
                print("%s: ghost material %s on %s, %d maps bound" % (name, mat["meta"]["name"], ghost[mat["meta"]["name"]]["template"], len(wanted)))
                continue
            for m in skin_assets.TEXTURE_PATH.finditer(body):
                stem = m.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode()
                group = m.group()[len(b"/assets/"):].rsplit(b"/", 1)[0].replace(b"/", b"_").decode()
                crc = zlib.crc32((group + stem).encode()) & 0xFFFFFFFF
                held = any(struct.unpack_from("<I", body, at)[0] == crc for at in range(0, len(body) - 3, 4))
                pair = textures.get(crc, {})
                if not held:
                    fail("%s: material %s names %s but holds no identity dword for it" % (name, mat["meta"]["name"], stem))
                elif "tg4h" not in pair or "tg4d" not in pair:
                    fail("%s: material %s needs texture %s (%08X), not in the pool" % (name, mat["meta"]["name"], stem, crc))
                else:
                    head = pair["tg4h"]["payload"]
                    if struct.unpack_from("<I", head, 0x14)[0] != crc or stem.encode() + b"\0" not in head:
                        fail("%s: tg4h of %s does not carry identity/name" % (name, stem))
                    checked += 1
        print("%s: model ok, refs %s, %d meshes with materials, %d texture bindings resolved in the pool" % (
            name, kinds, len(meshes), checked))
    print("verify_skin_pool:", "FAILED (%d)" % failures if failures else "ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
