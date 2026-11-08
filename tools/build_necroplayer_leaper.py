"""Build a playable Leaper: a sixth NecroPlayer record for the multiplayer pool.

Dead Space 2 ships four playable necromorphs. The class that drives them is
almost entirely data: a NecroPlayer record names the model, the animation
bundle, every movement speed, every animation by NAME, the melee set, the
projectile set and the vent exits, and fourteen components carry the rest. The
code knows five type names (PACK, LURKER, SPITTER, PUKER and the never-shipped
DIVIDERHEAD), and MPLevelSettings has five slots for their record GUIDs, so the
type is only ever an index into that table (NecroPlayer::Spawn: `lea edx,
[type*0x10 + attributes + 0x78]`). Nothing else about a playable necro lives in
code.

So a fifth or sixth necromorph does not need engine surgery, it needs a record.
This builds the Leaper's by copying the Lurker's (the closest relative, also
a wall-climbing jumper that grabs) and replacing everything that describes the
creature:

  * the model, the render bundle and the model stream become the Leaper's
  * every animation name becomes the Leaper's own, taken from its authored
    banks (leaper_locomote_base, leaper_scripted, leaper_attack_paired,
    leaper_jumps, leaper_hit_reacts, leaper_death)
  * the projectile set is dropped: the Leaper has no ranged attack
  * the grab keeps its shape but names the Leaper's paired animations, on both
    sides; `pc_leaper_grab_attack_*` already exists because the campaign
    Leaper grabs Isaac, so the victim's half needed no new animation either

The one thing the Leaper has no counterpart for is strafing: the Lurker was
given four directional runs for multiplayer and the Leaper only ever ran
forwards. All four names therefore point at the same run, and moving sideways
slides. That was a deliberate trade, not an oversight.

The record is appended to the pool the way tools/build_variants.py appends a
variant, and its GUID goes into the plugin's SixthTypeGuid, where
runtime_coop_necro.inc lends it to the type table for the length of one spawn.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from ds2_assets import assets, simgroup, loose_images, u32  # noqa: E402


def sdbm(text):
    result = 0
    for char in text.lower():
        result = (result * 0x1003F + ord(char)) & 0xFFFFFFFF
    return result


# The Leaper's own resources, read out of its character stream
# (ds_assets/char_str/npc/leap_cct/leaper.str). It carries no CMD_SetBNK: the
# campaign Leaper has none either, its banks arrive with the model stream.
LEAPER = {
    "CMD_SetModel": "ACB61CC3-ACB61CC3-ACB61CC3-2F24BB5B",
    "CMD_SetRCB": "48047A29-48047A29-48047A29-FBABFF51",
    "CMD_SetModelStream": "36C83B5F-95EB02FE-44455257-34303245",
}

# Animation names, all verified present in the Leaper's banks. Where the Lurker
# has something the Leaper does not, the nearest Leaper animation is named and
# the reason is given.
ANIMS = {
    # NecroPlayer image
    "m_idleAnim": "leaper_unaware_idle",
    # Names are checked against the TEXT in the Leaper's banks. A hash check
    # against the bank bytes is no test: on 22.09. it rejected run_old (which
    # plays: a proper run) and passed scurry (which does not: he walked in
    # his idle pose).
    "m_deathAnim": "leaper_die",
    # No directional animations exist: all four directions are the same
    # forward animation, so strafing slides. Accepted deliberately. Walking
    # and running are separate fields (m_locoAnim* / m_runAnim*); the first
    # test put run_old on the walk and left the run empty, and a running
    # Leaper played nothing (22.09.). A test the same evening: the two
    # are the other way round from what the names suggest. m_locoAnim* is the
    # normal fast movement, m_runAnim* the slow one. And run_old is no
    # animation at all (its hash is in none of the Leaper's banks, the name only
    # shows up in sound events), so loco played whatever the engine fell back
    # to, slow on walls. scurry is the campaign Leaper's real fast run (its
    # hash is in leaper_locomote_base.bnk, with scurry_2_idle beside it).
    "m_locoAnimForwRight": "run_old",
    "m_locoAnimForwLeft": "run_old",
    "m_locoAnimBackRight": "run_old",
    "m_locoAnimBackLeft": "run_old",
    "m_exitFloorVentLaunch": "Leaper_crawloutvent",
    # Vent exits as the campaign Leaper does them. LeaperVentComponent::
    # GetVentAnimation (Xbox debug build) hardcodes them by hash: leaving a
    # WALL vent is one animation, leaper_vent_wall_climb_out; leaving a
    # CEILING vent is leaper_vent_drop_down / _loop / _land; a floor vent it
    # cannot leave at all. The jump_up set the first builds used here is the
    # way INTO a ceiling vent, which is why he stuck in the wall (22.09.).
    # The MP exit wants launch / loop / land, so after climbing out the
    # Leaper falls in his own jump loop and lands with his own landing.
    # The wall exit is the campaign's own climb out. It fell through the floor
    # twice (27 m, 36 m), not because of the animation but because of the
    # Lurker's exit offset, which starts the necro a metre LOWER (y -1) for
    # the Lurker's jump; the climb then ends under the floor and the loop
    # never finds ground. The campaign Leaper starts it at (0.15, 0, -0.45)
    # (LeaperVentComponent::GetOffset, the table at 0x8306A058), the same frame
    # as the MP offsets (z -0.45 against the Lurker's -0.65), so that offset
    # comes with it, below. A plain jump (jump_leap_*) was tried in between and
    # never left the vent: it has no motion of its own.
    "m_exitWallVentLaunchAnim": "leaper_vent_wall_climb_out",
    "m_exitWallVentLoopAnim": "jump_forward_loop",
    "m_exitWallVentLandAnim": "jump_forward_land",
    "m_knockbackAnimPrimary": "knockdown",
    "m_knockbackAnimLoop": "knockback_wall",
    "m_knockbackAnimStand": "knockdown_getup",
    # The record's type is NOT a label. NecroPlayer::SpawnInit copies it into
    # m_type, and NecroPlayerMasterSM::CreateTypeSpecificSM indexes a table of
    # five state tables with it: Pack, Lurker, Spitter, Puker, and a zero for
    # DIVIDERHEAD, which never shipped. A Leaper typed DIVIDERHEAD would have had
    # no state machine at all. LURKER gives it the Lurker's, the only one with
    # wall-walking (NecroPlayerLurkerSM::UpdateWallwalkingState). Its tentacle
    # attack asks for Lurker animations by hash that the Leaper's banks do not
    # carry, so it plays nothing. (Xbox debug build, 22.09.)
    "CMD_NecroPlayer_SetType": "NECROPLAYERTYPE_LURKER",
    # Kept, but emptied rather than dropped. A dropped GUID is not empty: the
    # class default is guid128_t(), which Invalidate()s to BADBADBA-BEEFBEEF-...,
    # and NecroPlayer::SetBoneBoundAimingVFXActive only skips on IsClear(). With
    # the default it would create the effect on each projectile bone, bones
    # (l_/m_/r_spike) the Leaper does not have. All zeros is IsClear().
    "m_boneBoundAimingVFXGUID": "00000000-00000000-00000000-00000000",
    # NecroPlayerGrapplerComponent: the necro's half. Both halves are copied
    # from the campaign Leaper's own grappler (analysis/isolated_pool/
    # leaper.candidate.simgroup.bin), because the two sides are a PAIR and have
    # to be named as the campaign pairs them: Isaac's pc_..._pounce plays against
    # the Leaper's ..._pounce, and the breakout is meat_cello on both sides.
    # The first build guessed push_off, which exists for the Leaper but not for
    # Isaac, and shifted attempt/success by one (22.09.).
    "CMD_NecroPlayerGC_SetAttemptLoopAnim": "jump_attack_loop",
    "CMD_SetNPCAttackAttemptAnim": "leaper_grab_attack",
    "CMD_SetNPCAttackSuccessAnim": "leaper_grab_attack_pounce",
    "CMD_SetNPCAttackFailureAnim": "leaper_grab_attack_miss",
    "CMD_SetNPCStruggleLoopAnim": "leaper_grab_attack_bite_loop",
    "CMD_SetNPCDeathAnim": "leaper_grab_attack_bite_death",
    "CMD_SetNPCBreakoutAnim": "leaper_grab_attack_meat_cello",
    "m_jumpGrabAttemptAnim": "leaper_grab_attack_pounce",
    # and Isaac's half, which the campaign Leaper already needs
    "CMD_SetPlayerAttackSuccessAnim": "pc_leaper_grab_attack_pounce",
    "CMD_SetPlayerStruggleLoopAnim": "pc_leaper_grab_attack_bite_loop",
    "CMD_SetPlayerDeathAnim": "pc_leaper_grab_attack_bite_death",
    "CMD_SetPlayerBreakoutAnim": "pc_leaper_grab_attack_meat_cello",
}

# Fields of the Lurker's record that describe the Lurker and must not describe
# the Leaper. Dropping a field leaves the attribute at its class default.
DROP = [
    "m_projectileGUIDs", "m_projectileFiringAnims", "m_projectileAimingIdleVFXGUID",
    "m_projectileAmmoType", "m_projectileZeroGAmmoType", "m_projectileAimingCamGUID",
    "m_projectileConeDamage", "m_projectileConeDamageCap",
    "m_projectileChargeStartAnim", "m_projectileChargeLoopAnim", "m_projectileChargeTimes",
    "m_projectileAimingChargingVFXGUID", "m_projectileAimingChargingVFXLength",
    "m_projectileCastCone", "m_projectileMOD",
    "m_secondaryAnimBoneUpperBody",   # names a Lurker bone
    "m_wallwalkingVFX",               # the Lurker's tentacle effect
    # The Lurker names mp_lurker.bnk here, the bank its MP animations live in.
    # The Leaper's animations arrive with its own model stream, the way the
    # campaign Leaper gets them, and it authors no bank of its own, so this
    # field goes rather than pointing the Leaper at Lurker sounds.
    "CMD_SetBNK",
]

SOURCE_GUID = "58CA17A0-B16998A0-3532554C-58583337"   # the Lurker NecroPlayer record

# Fields the Lurker's record does not have at all and the Leaper needs. The
# Lurker never melees (its attack is the tentacle, which the plugin switches off
# for a Leaper) and has no run animations of its own; Puker and Spitter carry
# both. Each row is copied from a record that has it, so the field id, the
# descriptor and the kind are the engine's own and only the value changes.
# The melee set is played by NecroPlayerSM's standard melee, which every type's
# state machine has. ReadyToIdle is a one-shot transition, never a loop: a
# state waiting for a looping animation to end would wait for ever.
PUKER_GUID = "58CB1317-5D04C30B-3532554C-58583337"
PACK_GUID = "58CA0CA9-DF9A22B2-3532554C-58583337"
SPITTER_GUID = "58CAC498-725F1CF9-3532554C-58583337"
ADD = [
    (PUKER_GUID, "m_meleeAnimLHFromIdle", "leaper_attack_larm"),
    (PUKER_GUID, "m_meleeAnimRHFromIdle", "leaper_attack_rarm"),
    (PUKER_GUID, "m_meleeAnimLHCombo", "leaper_attack_larm"),
    (PUKER_GUID, "m_meleeAnimRHCombo", "leaper_attack_rarm"),
    (PUKER_GUID, "m_meleeAnimLHReadyToIdle", "scurry_2_idle"),
    (PUKER_GUID, "m_meleeAnimRHReadyToIdle", "scurry_2_idle"),
    (SPITTER_GUID, "m_runAnimForwRight", "walk"),
    (SPITTER_GUID, "m_runAnimForwLeft", "walk"),
    (SPITTER_GUID, "m_runAnimBackRight", "walk"),
    (SPITTER_GUID, "m_runAnimBackLeft", "walk"),
    (SPITTER_GUID, "m_runAnimPSS0Speed", None),   # a float, taken over as the Spitter has it (4.80)
    # The Lurker has no ceiling exit at all (only the Pack does), so a Leaper
    # sent to a ceiling vent had no animation and never came out (22.09.:
    # "spawn sound, but he did not come out"). Offset and fall velocity are the
    # Pack's; the campaign Leaper falls at the same -7 (GetExitingLoop-
    # InitialVelocity), its offsets are in another frame and not taken over.
    (PACK_GUID, "m_exitCeilingVentLaunchAnim", "leaper_vent_drop_down"),
    (PACK_GUID, "m_exitCeilingVentLoopAnim", "leaper_vent_drop_down_loop"),
    (PACK_GUID, "m_exitCeilingVentLandAnim", "leaper_vent_drop_down_land"),
    (PACK_GUID, "m_exitCeilingVentOffset", None),
    (PACK_GUID, "m_exitCeilingVentLoopVelocity", None),
]

# Vector fields replaced in place (flags 14, four floats). None at the moment.
# The wall exit once took the campaign Leaper's own fall (LeaperVentComponent::
# GetExitingLoopInitialVelocity: 0 / -1.5 / 4.0), and the Leaper fell straight
# through the floor: 27 m in 4 s out of a wall vent (vent exit report, 22.09.
# 22:2x). NecroPlayerExitVentSM looks for the ground ahead along the loop
# velocity (m_exitVentLandLookaheadTime), and a hop that flat never points at
# the floor, so the land state never came. The Lurker's -5 / 6, which the MP
# landing is tuned for, stays.
VECTORS = {
    "m_exitWallVentOffset": (0.15, 0.0, -0.45, 0.0),
}

# Two components beyond the NecroPlayer and its grappler still describe the
# Lurker and are changed too.
#  * RagdollComponent names the prop that takes over the body when it comes
#    apart. The Lurker's (58CA178F) is a DismemberableProp with the LURKER model,
#    living in the Lurker's MP stream; every one of the 54 campaign Leaper
#    records names 4BC78C36 instead, a DismemberableProp with the Leaper model in
#    leap_cct/leaper.str, the stream the plugin keeps resident anyway.
#  * AlertVFXComponent hangs the button-mash prompt on m_spine2 (sdbm E1ABD0C1),
#    a bone the Leaper does not have; m_neck (D5A991F1) is one it does.
COMPONENT_GUIDS = {
    "CMD_SetDismembermentEntity": "4BC78C36-9D56C506-544E4543-53574F4B",
}
COMPONENT_INLINE = {
    "m_buttonMashVFXBone": sdbm("m_neck"),
}

# The Leaper's character stream and the identity the engine knows it by, read
# out of ds2_randomizer_pool.toc's dependency list.
LEAPER_STREAM = r"global_assets\global_assets\ds_assets\char_str\npc\leap_cct\leaper"

# Knobs another creature's config (tools/necroplayer_<name>.py) overrides
# before calling build_stream. The defaults are the Leaper's.
GUID_SEED = "leaper"             # seeds the record GUID and the fresh image ids
HASHES = {}                      # field -> animation hash, written inline (kind 10)
COMPONENT_TYPES = ("RagdollComponent", "AlertVFXComponent")   # components whose fields COMPONENT_* may change
# Inline rows no MP record carries, made from scratch: {image type: [(field,
# kind, value)]}. Only inline kinds (bool 7, int...), no type index.
FABRICATE = {}
LEAPER_STREAM_IDENTITY = 0x002DFCF9


def load_names():
    for candidate in (ROOT / "analysis").rglob("*dictionary*.json"):
        data = json.loads(candidate.read_text(encoding="utf-8"))
        if "names" in data:
            return data["names"]
    return {}


def read_group(pool):
    sys.path.insert(0, str(Path(pool).parent))
    import ds2str  # noqa: E402
    stream = ds2str.StreamSet(pool)
    for meta, chunks in assets(stream):
        if meta["type"] != "SimGroup":
            continue
        return meta, stream.asset_payload(chunks, meta["size"])
    raise SystemExit("no SimGroup in " + pool)


def describe(pool, tools_dir):
    """Print the record as it stands and as it would be built, without writing."""
    sys.path.insert(0, tools_dir)
    names = load_names()

    def name_of(value):
        return "|".join(names.get("%08X" % value, [])) or "0x%08X" % value

    meta, data = read_group(pool)
    records, base = simgroup(data)
    images = loose_images(data, base)
    source = [r for r in records if r["guid"] == SOURCE_GUID]
    if len(source) != 1:
        raise SystemExit("source record %s not found once" % SOURCE_GUID)
    source = source[0]

    wanted = dict(LEAPER)
    wanted.update(ANIMS)
    covered, missing, dropped, kept = [], [], [], []
    for image_id in [source["image_id"]] + list(source["component_images"]):
        image = images.get(image_id)
        if not image:
            continue
        for field in image["fields"]:
            label = name_of(field["field_id"])
            if label in DROP:
                dropped.append(label)
            elif label in wanted:
                covered.append((label, wanted[label]))
            else:
                kept.append(label)
    for label in wanted:
        if label not in [c[0] for c in covered]:
            missing.append(label)

    print("source record %s, %d components" % (SOURCE_GUID, len(source["component_images"])))
    print("\nreplaced (%d):" % len(covered))
    for label, value in sorted(covered):
        print("   %-44s -> %s" % (label, value))
    print("\ndropped, the Leaper has no counterpart (%d):" % len(dropped))
    for label in sorted(set(dropped)):
        print("   %s" % label)
    print("\nkept as authored (%d fields)" % len(kept))
    if missing:
        print("\nWANTED BUT NOT IN THE SOURCE RECORD (%d) - these would need adding, not replacing:" % len(missing))
        for label in sorted(missing):
            print("   %s" % label)


# Which field rows carry a distance instead of a value. Measured over every row
# of the pool, not guessed: these flags always point into the block's data area
# and 0, 2, 5, 7, 8, 10, 11 and 22 never do. Knowing the set is enough, because
# a row that moves only needs its distance corrected by how far it moved; what
# it points at never has to be understood, or even found.
RELATIVE = frozenset((1, 3, 12, 14, 15, 16, 17))


def relative(raw):
    """Whether a field row points at data rather than holding its value.

    Kind 1 is two things, told apart by the u16 at +4: with a type index there
    it is the NAME of an enum or flag value, stored relative to the row; with
    FFFF it is a plain integer held in the row itself. The whole retail MP pool
    agrees: 220 rows of the first kind, all pointing at text; 263 of the
    second, all small numbers (4, 1, 7000...). The first builds treated every
    kind-1 row as relative, and CMD_SetGhostNameHideTime came out as the
    row's own offset (0x4A8) instead of 4000 (read live, 22.09.)."""
    if raw[6] == 1:
        return bytes(raw[4:6]) != b"\xff\xff"
    return raw[6] in RELATIVE


def new_guid(text):
    """A GUID in our own namespace: the last two words spell LEAP and NECR so
    the record is recognisable in a catalog dump, the first two come from the
    name so two records never collide."""
    first = sdbm(text)
    return (first, (first * 0x1003F + 0x9E3779B9) & 0xFFFFFFFF, 0x5041454C, 0x5243454E)


def build(pool, tools_dir, out_path):
    sys.path.insert(0, tools_dir)
    import ds2str  # noqa: E402
    names = load_names()

    def name_of(value):
        return "|".join(names.get("%08X" % value, [])) or "0x%08X" % value

    stream = ds2str.StreamSet(pool)
    found = None
    for meta, chunks in assets(stream):
        if meta["type"] == "SimGroup":
            found = bytes(stream.asset_payload(chunks, meta["size"]))
            break
    if found is None:
        raise SystemExit("no SimGroup in " + pool)
    data = found

    records, base = simgroup(data)
    images = loose_images(data, base)
    source = [r for r in records if r["guid"] == SOURCE_GUID]
    if len(source) != 1:
        raise SystemExit("source record %s not found once" % SOURCE_GUID)
    source = source[0]

    size = u32(data, 4)
    refs, instances, lists = struct.unpack_from("<III", data, 8)
    nrefs, ninstances, nlists = struct.unpack_from("<HHH", data, 0x14)
    psize = u32(data, base + 4)
    images_off = u32(data, base + 16)
    nimages = u32(data, base + 20)
    fields_off = u32(data, base + 24)
    nfields = u32(data, base + 28)

    wanted = dict(LEAPER)
    wanted.update(ANIMS)

    grappler_type = sdbm("NecroPlayerGrapplerComponent")
    grappler_old = None
    for image_id in source["component_images"]:
        image = images.get(image_id)
        if image and image["type_id"] == grappler_type:
            grappler_old = image_id
    if grappler_old is None:
        raise SystemExit("the source record has no grappler component")
    own = [source["image_id"], grappler_old]

    taken = {u32(data, base + images_off + i * 12) for i in range(nimages)}
    fresh = {}
    for old_id in own:
        candidate = sdbm("leaper_necroplayer_%08X" % old_id)
        while candidate in taken:
            candidate = (candidate + 1) & 0xFFFFFFFF
        fresh[old_id] = candidate
        taken.add(candidate)

    # --- part one: references, the component list, the instance table --------
    part = bytearray(data[:size])
    while len(part) % 4:
        part.append(0)
    new_refs = len(part)
    part.extend(data[refs:refs + nrefs * 4])
    part.extend(struct.pack("<II", fresh[own[0]], fresh[own[1]]))
    delta = new_refs - refs
    struct.pack_into("<I", part, 8, new_refs)
    struct.pack_into("<H", part, 0x14, nrefs + 2)
    for i in range(nlists):
        list_block = u32(data, lists + i * 4)
        count = u32(data, list_block + 4)
        for k in range(count):
            at = list_block + 8 + k * 4
            struct.pack_into("<I", part, at, u32(data, at) + delta)
    for record in records:
        struct.pack_into("<I", part, record["offset"] + 8, u32(data, record["offset"] + 8) + delta)

    main_entry = new_refs + nrefs * 4
    grappler_entry = main_entry + 4

    # The new record's component list: the source's entries with the grappler
    # swapped for ours. The other twelve components are shared, which is what
    # the pool does between its own records anyway.
    old_block = u32(data, source["offset"] + 0x20)
    count = u32(data, old_block + 4)
    while len(part) % 4:
        part.append(0)
    new_block = len(part)
    part.extend(struct.pack("<II", u32(data, old_block), count))
    for k in range(count):
        entry = u32(data, old_block + 8 + k * 4) + delta
        part.extend(struct.pack("<I", grappler_entry if u32(part, entry) == grappler_old else entry))

    while len(part) % 4:
        part.append(0)
    new_instances = len(part)
    # from `part`, not from `data`: every instance's reference offset was just
    # moved along with the table, and copying the original rows would undo it
    part.extend(part[instances:instances + ninstances * 68])
    row = bytearray(part[source["offset"]:source["offset"] + 68])
    guid = new_guid(GUID_SEED + "_necroplayer")
    guid_text = "-".join("%08X" % w for w in guid)
    struct.pack_into("<I", row, 8, main_entry)
    struct.pack_into("<4I", row, 12, *guid)
    # +0x1C is guid_hash of the GUID, NOT sdbm of its text. All 311 retail
    # records of the MP pool follow this and none use sdbm; the spawn lookup
    # finds a record by exactly this hash. With sdbm here the record existed
    # but was filed under a number nobody asks for ("has no instance data",
    # every time, for an entire evening, 22.09.).
    from remap_pool_guids import guid_hash  # noqa: E402
    struct.pack_into("<I", row, 0x1C, guid_hash(guid_text))
    struct.pack_into("<I", row, 0x20, new_block)
    struct.pack_into("<I", row, 0x2C, 0)   # never auto-spawned: the spawn asks for it by GUID
    struct.pack_into("<I", row, 0x30, 0)   # and it has no transform of its own
    part.extend(row)
    struct.pack_into("<I", part, 0xC, new_instances)
    struct.pack_into("<H", part, 0x16, ninstances + 1)
    struct.pack_into("<I", part, 4, len(part))
    while len(part) % 16:
        part.append(0)

    # --- part two: the parameter block --------------------------------------
    block = bytearray(data[base:base + psize])
    while len(block) % 4:
        block.append(0)
    new_fields_at = len(block)
    shift = new_fields_at - fields_off
    block.extend(block[fields_off:fields_off + nfields * 12])
    for i in range(nfields):
        at = new_fields_at + i * 12
        if block[at + 6] in RELATIVE:
            struct.pack_into("<i", block, at + 8, struct.unpack_from("<i", block, at + 8)[0] - shift)

    pending = []
    index_arrays = {}
    total_rows = nfields
    replaced = dropped = 0
    for old_id in own:
        indices = []
        for field in images[old_id]["fields"]:
            key = name_of(field["field_id"])
            if key in DROP:
                dropped += 1
                continue
            at = len(block)
            raw = bytearray(data[field["offset"]:field["offset"] + 12])
            replacement = wanted.get(key)
            if replacement is None:
                if raw[6] in RELATIVE:
                    moved = at - (field["offset"] - base)
                    struct.pack_into("<i", raw, 8, struct.unpack_from("<i", raw, 8)[0] - moved)
            else:
                replaced += 1
            block.extend(raw)
            if replacement is not None:
                pending.append((at, replacement, raw[6]))
            indices.append(total_rows)
            total_rows += 1
        index_arrays[old_id] = indices

    for at, replacement, flags in pending:
        while len(block) % 4:
            block.append(0)
        where = len(block)
        if flags == 12:
            block.extend(struct.pack("<4I", *(int(w, 16) for w in replacement.split("-"))))
        else:
            block.extend(replacement.encode("latin1") + b"\0")
        struct.pack_into("<i", block, at + 8, where - (at + 8))

    array_at = {}
    for old_id in own:
        while len(block) % 2:
            block.append(0)
        array_at[old_id] = len(block)
        for value in index_arrays[old_id]:
            block.extend(struct.pack("<H", value))

    while len(block) % 4:
        block.append(0)
    new_images_at = len(block)
    block.extend(block[images_off:images_off + nimages * 12])
    for old_id in own:
        type_index = None
        for i in range(nimages):
            entry = images_off + i * 12
            if u32(block, entry) == old_id:
                type_index = struct.unpack_from("<H", block, entry + 4)[0]
        if type_index is None:
            raise SystemExit("source image %08X not in the image table" % old_id)
        block.extend(struct.pack("<IHHI", fresh[old_id], type_index, len(index_arrays[old_id]), array_at[old_id]))

    struct.pack_into("<I", block, 16, new_images_at)
    struct.pack_into("<I", block, 20, nimages + 2)
    struct.pack_into("<I", block, 24, new_fields_at)
    struct.pack_into("<I", block, 28, total_rows)
    struct.pack_into("<I", block, 4, len(block))

    out = bytearray(part)
    out.extend(block)
    Path(out_path).write_bytes(bytes(out))
    print("wrote %s" % out_path)
    print("   %d bytes, was %d" % (len(out), len(data)))
    print("   record %s, %d fields replaced, %d dropped" % (guid_text, replaced, dropped))
    return guid_text


BLOB_LEN = {12: 16, 14: 16}          # GUID and vector; strings are NUL terminated
STRING_FLAGS = frozenset((1, 3, 16, 17))


def blob_of(data, field):
    """The bytes a relative row names, and how many of them. The record uses no
    flags 15, the one kind whose length is not known, so every blob here can be
    copied rather than pointed at."""
    signed = struct.unpack("<i", struct.pack("<I", field["value"]))[0]
    off = field["offset"] + 8 + signed
    if field["flags"] in BLOB_LEN:
        return data[off:off + BLOB_LEN[field["flags"]]]
    end = data.find(b"\0", off)
    if end < 0:
        raise SystemExit("unterminated string at %d" % off)
    return data[off:end + 1]


def build_group(pool, tools_dir, out_path):
    """A SimGroup of its own carrying nothing but the Leaper.

    Sharing the pool's component images was fine while the record lived inside
    the pool's own group, but a stream of our own cannot reach them: image ids
    are local to a group. So all fifteen images are copied, which the record
    can afford: it uses no field kind whose length is a mystery."""
    sys.path.insert(0, tools_dir)
    import ds2str  # noqa: E402
    names = load_names()

    def name_of(value):
        return "|".join(names.get("%08X" % value, [])) or "0x%08X" % value

    stream = ds2str.StreamSet(pool)
    data = None
    for meta, chunks in assets(stream):
        if meta["type"] == "SimGroup":
            data = bytes(stream.asset_payload(chunks, meta["size"]))
            break
    if data is None:
        raise SystemExit("no SimGroup in " + pool)

    records, base = simgroup(data)
    images = loose_images(data, base)
    source = [r for r in records if r["guid"] == SOURCE_GUID][0]
    order = [source["image_id"]] + list(source["component_images"])

    wanted = dict(LEAPER)
    wanted.update(ANIMS)
    grappler_type = sdbm("NecroPlayerGrapplerComponent")
    necro_type = sdbm("NecroPlayer")

    # --- part one -----------------------------------------------------------
    part = bytearray(data[:0x3C])
    # +0x2C names the character stream the records in this group need, by the
    # identity the TOCs use. Proof: the randomizer pool's copied enemy groups
    # carry BD951E79 and A4132B69 there, which are exactly the identities its
    # own TOC lists for exploder and infector. Copying the header from the MP
    # pool left ours pointing at the MP pool's stream, a dependency the Leaper
    # has nothing to do with.
    struct.pack_into("<I", part, 0x2C, LEAPER_STREAM_IDENTITY)
    refs_at = 0x3C
    part.extend(b"\0" * 4 * len(order))
    while len(part) % 4:
        part.append(0)
    list_at = len(part)
    # A component list opens with a hash of the component TYPES it holds, in
    # list order: h = h*0x1003F + byte over every type id's little-endian
    # bytes. Proven 8/8 on the retail MP pool, where no list of 120 carries
    # zero. Ours did, the fourth thing a hand-built group got wrong (22.09.).
    list_hash = 0
    for image_id in order[1:]:
        for byte in struct.pack("<I", images[image_id]["type_id"]):
            list_hash = (list_hash * 0x1003F + byte) & 0xFFFFFFFF
    source_list = struct.unpack_from("<I", data, source["offset"] + 0x20)[0]
    if list_hash != struct.unpack_from("<I", data, source_list)[0]:
        raise SystemExit("component list hash %08X differs from the Lurker's" % list_hash)
    part.extend(struct.pack("<II", list_hash, len(order) - 1))
    for i in range(1, len(order)):
        part.extend(struct.pack("<I", refs_at + i * 4))
    while len(part) % 4:
        part.append(0)
    lists_at = len(part)
    part.extend(struct.pack("<I", list_at))
    while len(part) % 4:
        part.append(0)
    instances_at = len(part)
    row = bytearray(data[source["offset"]:source["offset"] + 68])
    guid = new_guid(GUID_SEED + "_necroplayer")
    guid_text = "-".join("%08X" % w for w in guid)
    struct.pack_into("<I", row, 8, refs_at)
    struct.pack_into("<4I", row, 12, *guid)
    # +0x1C is guid_hash of the GUID, NOT sdbm of its text. All 311 retail
    # records of the MP pool follow this and none use sdbm; the spawn lookup
    # finds a record by exactly this hash. With sdbm here the record existed
    # but was filed under a number nobody asks for ("has no instance data",
    # every time, for an entire evening, 22.09.).
    from remap_pool_guids import guid_hash  # noqa: E402
    struct.pack_into("<I", row, 0x1C, guid_hash(guid_text))
    struct.pack_into("<I", row, 0x20, list_at)
    struct.pack_into("<I", row, 0x2C, 0)
    struct.pack_into("<I", row, 0x30, 0)
    part.extend(row)
    struct.pack_into("<III", part, 8, refs_at, instances_at, lists_at)
    struct.pack_into("<HHH", part, 0x14, len(order), 1, 1)
    struct.pack_into("<I", part, 4, len(part))
    while len(part) % 16:
        part.append(0)
    group_base = len(part)

    # --- part two -----------------------------------------------------------
    fresh_ids = {}
    for old_id in order:
        candidate = sdbm(GUID_SEED + "_image_%08X" % old_id)
        while candidate in fresh_ids.values():
            candidate = (candidate + 1) & 0xFFFFFFFF
        fresh_ids[old_id] = candidate
    for i, old_id in enumerate(order):
        struct.pack_into("<I", part, refs_at + i * 4, fresh_ids[old_id])

    # the type table is looked up by bisection, so it has to stay sorted
    types = sorted({images[i]["type_id"] for i in order})
    type_index = {t: i for i, t in enumerate(types)}

    rows, index_arrays, blobs = [], {}, []
    replaced = dropped = 0
    for old_id in order:
        image = images[old_id]
        mine = image["type_id"] in (necro_type, grappler_type)
        component = image["type_id"] in {sdbm(name) for name in COMPONENT_TYPES}
        indices = []
        for field in image["fields"]:
            key = name_of(field["field_id"])
            if mine and key in DROP:
                dropped += 1
                continue
            raw = bytearray(data[field["offset"]:field["offset"] + 12])
            replacement = wanted.get(key) if mine else COMPONENT_GUIDS.get(key) if component else None
            if mine and key in HASHES:
                # An animation named by its hash rather than its name: kind 10,
                # no type index, the hash in the row. The retail game stores
                # DecompressionReact*Anim and CMD_SetTargetAnim this way.
                raw[4:6] = b"\xff\xff"
                raw[6] = 10
                struct.pack_into("<I", raw, 8, HASHES[key])
                replaced += 1
                indices.append(len(rows))
                rows.append([raw, None])
                continue
            if mine and key in VECTORS:
                if raw[6] != 14:
                    raise SystemExit("%s is not a vector field" % key)
                replaced += 1
                indices.append(len(rows))
                rows.append([raw, struct.pack("<4f", *VECTORS[key])])
                continue
            if component and key in COMPONENT_INLINE:
                if relative(raw):
                    raise SystemExit("%s is not an inline field" % key)
                value = COMPONENT_INLINE[key]
                if isinstance(value, float):
                    struct.pack_into("<f", raw, 8, value)
                else:
                    struct.pack_into("<I", raw, 8, value)
                replaced += 1
                payload = None
            elif replacement is not None:
                if component and not mine and raw[6] != 12:
                    raise SystemExit("%s is not a GUID field" % key)
                replaced += 1
                payload = (struct.pack("<4I", *(int(w, 16) for w in replacement.split("-")))
                           if raw[6] == 12 else replacement.encode("latin1") + b"\0")
            elif relative(raw):
                payload = blob_of(data, field)
            else:
                payload = None
            indices.append(len(rows))
            rows.append([raw, payload])
        if image["type_id"] == necro_type:
            present = {name_of(f["field_id"]) for f in image["fields"]}
            for donor_guid, key, value in ADD:
                if key in present:
                    raise SystemExit("%s already in the source record; replace it instead" % key)
                donor = [r for r in records if r["guid"] == donor_guid][0]
                field = [f for f in images[donor["image_id"]]["fields"] if name_of(f["field_id"]) == key]
                if len(field) != 1:
                    raise SystemExit("%s not found once in %s" % (key, donor_guid))
                field = field[0]
                raw = bytearray(data[field["offset"]:field["offset"] + 12])
                if value is None:
                    payload = blob_of(data, field) if relative(raw) else None
                elif isinstance(value, int):
                    raw[4:6] = b"\xff\xff"
                    raw[6] = 10
                    struct.pack_into("<I", raw, 8, value)
                    payload = None
                else:
                    if not relative(raw):
                        raise SystemExit("%s is inline; give None to copy it" % key)
                    payload = value.encode("latin1") + b"\0"
                indices.append(len(rows))
                rows.append([raw, payload])
                replaced += 1
        for type_name, fields in FABRICATE.items():
            if image["type_id"] != sdbm(type_name):
                continue
            present = {name_of(f["field_id"]) for f in image["fields"]}
            for key, kind, value in fields:
                if key in present:
                    raise SystemExit("%s already in the record; replace it instead" % key)
                if kind in RELATIVE:
                    raise SystemExit("%s: only inline kinds can be made from scratch" % key)
                indices.append(len(rows))
                rows.append([bytearray(struct.pack("<IHBBI", sdbm(key), 0xFFFF, kind, 0xFF, value)), None])
                replaced += 1
        index_arrays[old_id] = indices

    # The u16 at +4 of an enum or flag row is an index into THIS group's type
    # table, and that table holds the enum types too: in the MP pool index
    # 0x1D is NecroPlayerType, 0x53 AnimatedFlags, 0x0A meansOfDeath_t. The
    # first builds kept only the fifteen component classes, so every such
    # index pointed past the table, every enum parsed as 0, and
    # "NECROPLAYERTYPE_LURKER" became 0, PACK: the Leaper ran the Pack's state
    # machine and could never climb (read live, 22.09.: attributes+0x194 = 0
    # where the Lurker has 1). So the enum types the kept rows name come along,
    # and every index is rewritten to its place in the new, sorted table.
    source_block = (struct.unpack_from("<I", data, 4)[0] + 15) & ~15
    source_types_at, source_type_count = struct.unpack_from("<II", data, source_block + 8)
    source_types = [struct.unpack_from("<II", data, source_block + source_types_at + 8 * i)[0]
                    for i in range(source_type_count)]
    named = set()
    for raw, _payload in rows:
        index = struct.unpack_from("<H", raw, 4)[0]
        if index != 0xFFFF:
            if index >= len(source_types):
                raise SystemExit("type index %d past the source table" % index)
            named.add(source_types[index])
    types = sorted({images[i]["type_id"] for i in order} | named)
    type_index = {t: i for i, t in enumerate(types)}
    for raw, _payload in rows:
        index = struct.unpack_from("<H", raw, 4)[0]
        if index != 0xFFFF:
            struct.pack_into("<H", raw, 4, type_index[source_types[index]])

    header = 32
    types_at = header
    images_at = types_at + 8 * len(types)
    fields_at = images_at + 12 * len(order)
    tail = fields_at + 12 * len(rows)
    block = bytearray(tail)
    struct.pack_into("<I", block, 0, 0x91C8773C)
    struct.pack_into("<I", block, 8, types_at)
    struct.pack_into("<H", block, 12, len(types))
    struct.pack_into("<I", block, 16, images_at)
    struct.pack_into("<I", block, 20, len(order))
    struct.pack_into("<I", block, 24, fields_at)
    struct.pack_into("<I", block, 28, len(rows))
    # Each type row carries a second word that is constant per type across all
    # 275 types of the retail MP pool and the randomizer pool (NecroPlayer 40,
    # most components 1 or 2): a class version, and never zero. Ours were all
    # zero until 22.09.; they are copied from the source group now.
    version = dict(struct.unpack_from("<II", data, source_block + source_types_at + 8 * i)
                   for i in range(source_type_count))
    for i, type_id in enumerate(types):
        if not version.get(type_id):
            raise SystemExit("no version for type %08X in the source group" % type_id)
        struct.pack_into("<II", block, types_at + i * 8, type_id, version[type_id])

    # the blobs go after the tables; every relative row is pointed at its own
    # copy, measured from the row as the engine measures it
    for i, (raw, payload) in enumerate(rows):
        at = fields_at + i * 12
        if payload is None:
            block[at:at + 12] = raw
            continue
        while len(block) % 4:
            block.append(0)
        where = len(block)
        block.extend(payload)
        struct.pack_into("<i", raw, 8, where - (at + 8))
        block[at:at + 12] = raw

    for old_id in order:
        while len(block) % 2:
            block.append(0)
        index_arrays[old_id] = (len(block), index_arrays[old_id])
        for value in index_arrays[old_id][1]:
            block.extend(struct.pack("<H", value))
    for i, old_id in enumerate(order):
        at, indices = index_arrays[old_id]
        struct.pack_into("<IHHI", block, images_at + i * 12,
                         fresh_ids[old_id], type_index[images[old_id]["type_id"]], len(indices), at)
    struct.pack_into("<I", block, 4, len(block))

    out = bytearray(part)
    out.extend(block)
    Path(out_path).write_bytes(bytes(out))
    print("wrote %s" % out_path)
    print("   %d bytes, one record, %d images, %d field rows" % (len(out), len(order), len(rows)))
    print("   %d fields replaced, %d dropped" % (replaced, dropped))
    print("   record %s" % guid_text)
    return guid_text


def simgroup_header(pool, tools_dir):
    """The SHDR header of the pool's SimGroup asset, to be reused for ours.

    ds2_assets.assets() hands back the decoded fields but not the raw thirty-six
    bytes in front of the names, and those carry flags the loader reads. Copying
    a header that the game already accepts is safer than inventing one."""
    sys.path.insert(0, tools_dir)
    import ds2str  # noqa: E402
    stream = ds2str.StreamSet(pool)
    for _offset, _tag, body in stream.chunks():
        if stream.tag(body[:4]) != b"SHDR":
            continue
        names = body[36:].split(b"\0")
        if names[2].decode("latin1") == "SimGroup":
            return bytearray(body[:36])
    raise SystemExit("no SimGroup header in " + pool)


def build_stream(pool, tools_dir, out_dir, stream_name):
    """A loose stream the plugin can ask for by name, like the randomizer pool."""
    from append_vent_donors import MAX_CHUNK, chunk  # noqa: E402
    from build_variants import build_toc, uuid_name  # noqa: E402

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    payload_path = out_dir / (stream_name + ".simgroup.bin")
    guid_text = build_group(pool, tools_dir, payload_path)
    payload = payload_path.read_bytes()
    payload_path.unlink()

    # Named the way the randomizer pool names its roots, which is the only
    # shape the loader accepts: the SimGroup is called after its own GUID in
    # UUID byte order, it lives under the stream's folder, and it carries no
    # resource id of its own. The first attempt used a made-up name and a
    # levels\ path, and the record was simply never there
    # ("has no instance data", 22.09. 19:20).
    from remap_pool_guids import guid_hash  # noqa: E402
    root = uuid_name(guid_text)
    # The identity the engine keys the stream by. ds2_randomizer_models, the
    # closest analogue (a small loose stream the plugin asks for by name),
    # carries guid_hash of its root; only the big pool has zero there, and
    # zero left ours unloadable (22.09.).
    identity = guid_hash(guid_text)
    asset_name = root
    asset_path = "randomizer\\%s\\%s.simgroup.bin" % (stream_name, hashlib.sha1(guid_text.encode()).hexdigest()[:24])
    # The three streams every loose stream of ours leans on, copied from the
    # pool's TOC: without them the loader has nothing to resolve against.
    shared = [(0x01A2021D, "global_assets\\global_assets"),
              (0x00000000, "global_assets\\global_assets_global"),
              (0x00000000, "text_assets\\text_assets_global")]

    header = simgroup_header(pool, tools_dir)
    struct.pack_into("<I", header, 16, 0)                  # no resource id, like the pool's roots
    struct.pack_into("<I", header, 32, len(payload))
    body = bytearray(header)
    body.extend((asset_name + "\0" + asset_path + "\0" + "SimGroup" + "\0").encode("latin1"))

    # A loose .str is not just its chunks. The native reader wants the 12-byte
    # stream header first and reads in 128 KiB blocks: no chunk may cross a
    # block, and the file is filled to a whole block with an LLIF chunk.
    # tools/stream_framing.py does exactly that for every randomizer stream,
    # and its validate_framing rejects anything else. The first four builds of
    # this file had neither header nor framing (4 KiB, zero-filled), and the
    # engine registered the TOC but never read a byte of the stream: QUEUED
    # forever, loading screen held, three tests on 22.09.
    from stream_framing import HEADER, frame_stream  # noqa: E402
    data = bytearray(HEADER)
    data.extend(chunk(bytes(body)))
    for start in range(0, len(payload), MAX_CHUNK - 12):
        data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
    # content_size is the sum of the asset PAYLOADS, not the length of the
    # chunks around them. ds2_randomizer_models proves it: 99823153 in its TOC
    # and 99823153 bytes of payload across 1162 assets. The first attempt wrote
    # the chunk length and the stream never reached status 3, which hung the
    # loading screen, because a stream request has no deadline.
    content_size = len(payload)
    data = bytearray(frame_stream(bytes(data)))

    str_path = out_dir / (stream_name + ".str")
    toc_path = out_dir / (stream_name + ".toc")
    str_path.write_bytes(bytes(data))
    # No forced dependency: build_toc's own round trip refuses one, and
    # ds2_randomizer_models (the loose stream that demonstrably loads) lists
    # none either. The plugin requests the Leaper's character stream by name
    # instead, which is the same thing by another road.
    toc_path.write_bytes(build_toc("randomizer\\" + stream_name, identity, content_size, shared, []))
    print("\nwrote %s (%d bytes) and %s" % (str_path, len(data), toc_path.name))
    print("   root %s, identity %08X" % (root, identity))
    print("   ask for it as: randomizer\\%s" % stream_name)
    print("   SixthTypeGuid=%s" % guid_text)
    return guid_text


def verify(payload_path, tools_dir, guid_text):
    """Read the result back the way the catalog reader does."""
    sys.path.insert(0, tools_dir)
    names = load_names()

    def name_of(value):
        return "|".join(names.get("%08X" % value, [])) or "0x%08X" % value

    data = Path(payload_path).read_bytes()
    records, base = simgroup(data)
    images = loose_images(data, base)
    match = [r for r in records if r["guid"] == guid_text]
    if not match:
        raise SystemExit("the new record is not in the rebuilt payload")
    record = match[0]
    print("re-read: record %s, %d components" % (record["guid"], len(record["component_images"])))
    grappler_type = sdbm("NecroPlayerGrapplerComponent")
    for image_id in [record["image_id"]] + list(record["component_images"]):
        image = images.get(image_id)
        if not image:
            print("   image %08X MISSING" % image_id)
            continue
        if image_id != record["image_id"] and image["type_id"] != grappler_type:
            continue
        print("\n   %s, %d fields" % (name_of(image["type_id"]), len(image["fields"])))
        for field in image["fields"]:
            signed = struct.unpack("<i", struct.pack("<I", field["value"]))[0]
            off = field["offset"] + 8 + signed
            text = ""
            if field["flags"] in (1, 3, 16, 17) and 0 < off < len(data):
                end = data.find(b"\0", off, min(off + 200, len(data)))
                if end > off and all(32 <= b < 127 for b in data[off:end]):
                    text = '"%s"' % data[off:end].decode()
            elif field["flags"] == 12 and off + 16 <= len(data):
                text = "guid " + "-".join("%08X" % v for v in struct.unpack_from("<4I", data, off))
            elif field["flags"] == 0:
                text = "f %.3f" % struct.unpack("<f", struct.pack("<I", field["value"]))[0]
            print("      %-44s %s" % (name_of(field["field_id"]), text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pool", help="global_mp_assets.str")
    parser.add_argument("--tool-dir", required=True, help="the folder holding ds2str.py")
    parser.add_argument("--describe", action="store_true", help="report the plan and change nothing")
    parser.add_argument("--payload", help="write the rebuilt SimGroup payload here")
    parser.add_argument("--group", help="write a SimGroup of its own, carrying only the Leaper")
    parser.add_argument("--stream", help="write a loose stream (.str and .toc) into this folder")
    parser.add_argument("--stream-name", default="ds2_leaper")
    args = parser.parse_args()
    if args.describe:
        describe(args.pool, args.tool_dir)
        return
    if args.stream:
        build_stream(args.pool, args.tool_dir, args.stream, args.stream_name)
        return
    if args.group:
        guid_text = build_group(args.pool, args.tool_dir, args.group)
        print()
        verify(args.group, args.tool_dir, guid_text)
        return
    if not args.payload:
        raise SystemExit("give --describe, --payload or --group")
    guid_text = build(args.pool, args.tool_dir, args.payload)
    print()
    verify(args.payload, args.tool_dir, guid_text)


if __name__ == "__main__":
    main()
