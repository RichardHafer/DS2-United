"""The Outbreak maps' baked entity images (DS2DAT12, extracted to <game>/work/ds2dat12).

Their simgroups carry the objects in runtime layout, without a field list
(tools/ds2_assets.py baked_images), so analysis/catalog_dlc has their classes
only. The attribute offsets are the same as on the other maps, and a message
attribute holds the name hash (h * 0x1003F + lower-case char) in its first
dword, so the values can be read straight from the image.

The blob is the packet's data area: BinaryParamImageTOC::Mount 0x0085C110 hands
each image to AttributePacket::New 0x007CEBC0, which copies it to header + 0x10.
The offsets below come from the class descriptors' attribute tables (+0x1C) in
a retail dump (tools/dump_attribute_offsets.py).
"""
import json
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402
from ds2_assets import assets, simgroup, loose_images  # noqa: E402

DLC_MAPS = ("pdlc_mp_mall", "pdlc_mp_school")
DLC_CATALOG = root / "analysis" / "catalog_dlc" / "entities.jsonl"

# VentMarker (descriptor 0x020B3D90)
VENT_TYPE = 0x134            # CMD_SetVentType: 0 floor, 1 wall, 2 ceiling
VENT_CULLING = 0x170         # m_activateLocalCulling
VENT_SELECT_ON = 0x1D4       # m_activateSelection
VENT_SELECT_OFF = 0x1E0      # m_deactivateSelection
VENT_START_INACTIVE = 0x1EC  # CMD_SetStartInactive (byte)
# FuncSpawn (0x020A00B0)
FUNCSPAWN_TARGET_NAME = 0x00
FUNCSPAWN_TARGET = 0x0C
FUNCSPAWN_MAX = 0x1C         # int, -1 when not authored
# DSNPCSpawningComponent (0x020A4078); the header the rebind reads is this - 0x10
SPAWNING_MODE = 0x00
SPAWNING_FLAGS = 0x04
SPAWNING_VENT = 0x08
SPAWNING_OPTIONS = 0x24
SPAWNING_SIZE = 0x5C
# MultiplayerSpawnPoint (0x020219B0)
SPAWNPOINT_INITIAL = 0x00    # m_validForInitialSpawn (byte)
SPAWNPOINT_RESPAWN = 0x01    # m_validForRespawn (byte)
SPAWNPOINT_ACTIVE = 0x10     # m_initiallyActive (byte)
SPAWNPOINT_ON = 0x14         # m_activateMessage
SPAWNPOINT_OFF = 0x20        # m_deactivateMessage


def name_hash(text):
    h = 0
    for c in text.lower():
        h = (h * 0x1003F + ord(c)) & 0xFFFFFFFF
    return h


def guid_at(blob, offset):
    words = struct.unpack_from("<4I", blob, offset)
    return "-".join("%08X" % w for w in words) if any(words) else ""


def u32_at(blob, offset):
    return struct.unpack_from("<I", blob, offset)[0]


def message_at(blob, offset):
    """(hash, name) of a message attribute. Its third dword is the offset of the
    name, counted from that dword; the name is kept only when it hashes back."""
    h, _, rel = struct.unpack_from("<3I", blob, offset)
    if not h or not rel or offset + 8 + rel >= len(blob):
        return h, ""
    start = offset + 8 + rel
    end = blob.find(b"\0", start)
    name = blob[start:end if end >= 0 else len(blob)].decode("latin1")
    return h, name if name_hash(name) == h else ""


def _type_classes():
    types = {}
    if DLC_CATALOG.exists():
        with open(DLC_CATALOG, encoding="utf-8") as f:
            for line in f:
                e = json.loads(line)
                for im in e["images"]:
                    if im.get("class"):
                        types[im["type_id"]] = im["class"]
    return types


def entity_classes():
    """GUID -> class of every entity in analysis/catalog_dlc."""
    classes = {}
    if DLC_CATALOG.exists():
        with open(DLC_CATALOG, encoding="utf-8") as f:
            for line in f:
                e = json.loads(line)
                classes[e["guid"].upper()] = e["class"]
    return classes


def iter_baked(classes, work=None):
    """(map, guid, class, blob) for every baked entity of the given classes on the Outbreak maps."""
    types = _type_classes()
    base_dir = Path(work) if work else WORK / "ds2dat12"
    for path in sorted(base_dir.rglob("*.str")):
        rel = path.relative_to(base_dir).parts
        if not rel or rel[0].lower() not in DLC_MAPS:
            continue
        try:
            stream = ds2str.StreamSet(path)
        except Exception:
            continue
        for meta, chunks in assets(stream):
            if meta["type"] != "SimGroup":
                continue
            data = stream.asset_payload(chunks, meta["size"])
            try:
                entities, base = simgroup(data)
                images = loose_images(data, base)
            except Exception:
                continue
            for ent in entities:
                im = images.get(ent["image_id"])
                if not im or not im.get("baked") or not im["data_offset"]:
                    continue
                cls = types.get(im["type_id"])
                if cls in classes:
                    yield rel[0].lower(), ent["guid"].upper(), cls, data[im["data_offset"]:im["data_offset"] + im["size"]]


def iter_baked_entities(classes, work=None):
    """Every baked entity of the given classes on the Outbreak maps as a dict:
    map, archive (as in the catalogs), main (the map's main stream), guid, class,
    blob, components (class -> blob) and matrix (the simgroup transform, 16
    floats, or None)."""
    types = _type_classes()
    base_dir = Path(work) if work else WORK / "ds2dat12"
    for path in sorted(base_dir.rglob("*.str")):
        rel = path.relative_to(base_dir).parts
        if not rel or rel[0].lower() not in DLC_MAPS:
            continue
        try:
            stream = ds2str.StreamSet(path)
        except Exception:
            continue
        name = rel[0].lower()
        archive = "ds2dat12\\" + "\\".join(rel)
        main = len(rel) == 2 and rel[1].lower() == name + ".str"
        for meta, chunks in assets(stream):
            if meta["type"] != "SimGroup":
                continue
            data = stream.asset_payload(chunks, meta["size"])
            try:
                entities, base = simgroup(data)
                images = loose_images(data, base)
            except Exception:
                continue
            for ent in entities:
                im = images.get(ent["image_id"])
                if not im or not im.get("baked") or not im["data_offset"]:
                    continue
                cls = types.get(im["type_id"])
                if cls not in classes:
                    continue
                components = {}
                for cid in ent["component_images"]:
                    ci = images.get(cid)
                    if ci and ci.get("baked") and ci["data_offset"]:
                        components.setdefault(types.get(ci["type_id"], "?"), data[ci["data_offset"]:ci["data_offset"] + ci["size"]])
                at = ent["transform_offset"]
                matrix = struct.unpack_from("<16f", data, at) if at and at + 64 <= len(data) else None
                yield {"map": name, "archive": archive, "main": main, "guid": ent["guid"].upper(), "class": cls,
                       "blob": data[im["data_offset"]:im["data_offset"] + im["size"]], "components": components, "matrix": matrix}
