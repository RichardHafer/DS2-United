"""Build a playable Ubermorph: a NecroPlayer record out of the Puker's.

The Ubermorph is "Hunter" in the game's code. It shares the slasher's banks
(zombie_*.bnk plus zombiehunter_override.bnk), and those carry no readable
animation names, so every animation here is named by its HASH, taken straight
out of the Hunter's own code in the Xbox debug build (Hunter::GetForward-
LocomoteAnim, GetIdleAnim, GetDeathAnim, GetAttackAnimData, HunterVent-
Component::GetVentAnimation ...), and written the way the retail game writes
DecompressionReact*Anim: kind 10, the hash in the row.

The Puker is the template (chosen on 22.09.): its state machine is the
Spitter's, the only one with a sprint, and it grabs from standing. Its ranged
attack goes: the fields here, the state machine's UpdateAimMode in the plugin
(runtime_coop_necro.inc, InstallSpitterAimGuard). The Ubermorph regenerates
and does not die in the campaign; the record gives it the campaign's 100000
health.

Usage:
    python tools/necroplayer_ubermorph.py <global_mp_assets.str> --tool-dir <Modding Tools> --stream <out dir>
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_necroplayer_leaper as base  # noqa: E402

PUKER = "58CB1317-5D04C30B-3532554C-58583337"

# Resources: the campaign Hunter's own (ch10 necro_rush record 5ACB6715).
RESOURCES = {
    "CMD_SetModel": "A9337145-A9337145-A9337145-AD81D369",
    "CMD_SetRCB": "E3977F96-E3977F96-E3977F96-0F663E01",
    "CMD_SetModelStream": "32CB092B-CDEF8A88-4C4F4D53-30314E45",
    # No button prompt in his grab (23.09.: breakout off and instant
    # kill, but the mash prompt still showed): the grab's progress bar and
    # pulsing prompt are effects named here, and an empty GUID makes none.
    "CMD_SetProgressVFX": "00000000-00000000-00000000-00000000",
    "CMD_SetPulseVFX": "00000000-00000000-00000000-00000000",
}

# Animations by hash. None = not yet read out of the code; the build refuses
# to run while any is left.
HASHES = {
    # All out of the Xbox debug build (Hunter::*, HunterVentComponent::*,
    # HunterGrappleAttacherComponent::GetStageAnimations), 22.09.
    "m_idleAnim": 0x2AF7542D,                 # GetIdleAnim: both legs, not wandering
    "m_deathAnim": 0xC65EFC3B,                # GetDeathAnim
    # m_locoAnim* is the normal movement: his stalking gait
    # (GetForwardLocomoteAnim default); the sprint is his charge run.
    "m_locoAnimForwRight": 0xB4DC9022,
    "m_locoAnimForwLeft": 0xB4DC9022,
    "m_locoAnimBackRight": 0xB4DC9022,
    "m_locoAnimBackLeft": 0xB4DC9022,
    # For a video, chosen on 22.09.: he sprints with the SLASHER's charge
    # run (Zombie::GetForwardLocomoteAnim while charging). Same armature, same
    # banks (zombie_locomote.bnk carries it). His own charge is D78EC9F2.
    "m_sprintLoopAnim": 0xD8C07996,
    "m_sprintStopAnim": 0xFB367D84,           # his backup step: a one-shot, never a loop
    "m_sprintMissedGrabAnim": 0xB39DE5C9,     # the grapple's miss (GetStageAnimations stage 2)
    # M1: the overhead strikes (constructor attack table idx 0 / 1, left / right
    # spike). No recover-to-idle exists; the NPC backs up after a strike.
    "m_meleeAnimLHFromIdle": 0xE86F863E,
    "m_meleeAnimRHFromIdle": 0x0BC79345,
    "m_meleeAnimLHCombo": 0xE86F863E,
    "m_meleeAnimRHCombo": 0x0BC79345,
    "m_meleeAnimLHReadyToIdle": 0xFB367D84,
    "m_meleeAnimRHReadyToIdle": 0xFB367D84,
    "m_knockbackAnimPrimary": 0x84B8E676,     # DeadSpaceNPC knockdown, normal gravity
    # Vents: the Hunter reads the slasher's exit table (0x8303B2E8)
    "m_exitFloorVentLaunch": 0x2FA25069,
    "m_exitWallVentLaunchAnim": 0x50431582,
    "m_exitWallVentLoopAnim": 0x0748C824,
    "m_exitWallVentLandAnim": 0x0062EECB,
    # The grab is the Hunter's one paired kill: attacker 5689428B, victim
    # 7E2D9451. It plays as the grab's success and is the whole grab. The
    # first build gave struggle, death and breakout the kill as well, and the
    # Ubermorph did it a second time on the corpse (22.09.); those stages have
    # no animation now (DROP below), as the Hunter's own stages 1/3/5 have none.
    "CMD_SetPlayerAttackSuccessAnim": 0x7E2D9451,
    "CMD_SetNPCAttackSuccessAnim": 0x5689428B,
    "CMD_SetNPCAttackFailureAnim": 0xB39DE5C9,
    "m_fallingLoopAnim": 0x0748C824,          # the wall exit's airborne loop
}

# The Puker's ranged attack, and everything that names a Puker bone or effect.
DROP = [
    "CMD_SetBNK",
    "m_secondaryAnimBoneUpperBody",
    "m_projectileCastCone", "m_projectileChargeStartAnim", "m_projectileChargeLoopAnim",
    "m_projectileChargeTimes", "m_projectileFiringAnims", "m_projectileAimingIdleVFXGUID",
    "m_projectileAimingChargingVFXGUID", "m_projectileFanAngleRad", "m_projectileAimingCamGUID",
    "m_projectileConeFarRadius", "m_projectileConeDamage", "m_projectileConeDamageCap",
    "m_projectileConeFireVFX", "m_projectileConeFireVFXBone", "m_projectileBoneAimingGUID",
    "m_projectileAltFireAnim", "m_projectileAltGUID", "m_projectileAltMOD",
    # the grab's later stages: the kill already played as its success
    "CMD_SetPlayerStruggleLoopAnim", "CMD_SetPlayerDeathAnim", "CMD_SetPlayerBreakoutAnim",
    "CMD_SetNPCStruggleLoopAnim", "CMD_SetNPCDeathAnim", "CMD_SetNPCBreakoutAnim",
]

# No QTE on either side (22.09.): once grabbed, you are dead. The
# victim cannot break out (CMD_AllowBreakOut, a GrappleData field the
# campaign Hunter sets to 0 and no MP record carries, so it is made from
# scratch), and the attacker need not mash: the automatic damage kills at once.
FABRICATE = {
    "NecroPlayerGrapplerComponent": [("CMD_AllowBreakOut", 7, 0)],
}

COMPONENT_TYPES = ("RagdollComponent", "AlertVFXComponent", "DismemberableHealthComponent", "NecroPlayerGrapplerComponent", "NecroPlayer")

# Exit offsets: the slasher table the Hunter reads (0x830694E0, offsetType 2 =
# exit), the same frame as the MP offsets (wall 0.07/1.88/-1.37 against the
# Puker's 0.05/2.17/-0.9). The loop velocity stays the Puker's (0/-4/4.5):
# the Leaper fell through floors with a flat campaign hop.
VECTORS = {
    "m_exitFloorVentOffset": (0.0, -1.71, -0.70, 0.0),
    "m_exitWallVentOffset": (0.07, 1.88, -1.37, 0.0),
}

# The Puker has no ceiling exit (only the Pack does): its rows, the Hunter's
# animations; offset and fall velocity as the Pack has them (the slasher
# table's ceiling offset is the Pack's own 0/0.5/-0.93).
PACK = "58CA0CA9-DF9A22B2-3532554C-58583337"
ADD = [
    (PACK, "m_exitCeilingVentLaunchAnim", 0x4ABD5C48),
    (PACK, "m_exitCeilingVentLoopAnim", 0x2404D099),
    (PACK, "m_exitCeilingVentLandAnim", 0x6D1D8CAE),
    (PACK, "m_exitCeilingVentOffset", None),
    (PACK, "m_exitCeilingVentLoopVelocity", None),
]
COMPONENT_GUIDS = {
    "CMD_SetDismembermentEntity": "32CB0923-34B8135D-4C4F4D53-30314E45",   # the Hunter's DismemberableProp
    "m_buttonMashVFXGUID": "00000000-00000000-00000000-00000000",          # the mash prompt over the attacker
}
COMPONENT_INLINE = {
    "CMD_SetHealth": 100000.0,          # the campaign Hunter's; it does not die
    # and it loses no limbs, not while grabbing, not ever (22.09.):
    # the NecroPlayer has no regeneration to give them back
    "CMD_SetHeadHealth": 100000.0,
    "CMD_SetRightFrontHealth": 100000.0,
    "CMD_SetLeftFrontHealth": 100000.0,
    "CMD_SetRightBackHealth": 100000.0,
    "CMD_SetLeftBackHealth": 100000.0,
    "m_automaticDPS": 100000.0,         # the grab kills at once, no mashing
    # The sprint, turned up for fun (22.09.): the Puker sprints at 6.25,
    # a normal run tops out at 5. Braking and turning scale with it so he can
    # still be steered at that speed.
    "m_sprintSpeed": 15.0,
    "m_sprintDeceleration": 36.0,
    "m_sprintTurnRate": 60.0,
    "m_buttonMashVFXBone": 0xA5D40FE5,  # the Hunter's vision ray-cast bone (AwarenessComponent), a bone it has
}

STREAM_IDENTITY = 0x56588FCB            # ubermorph_cct\ubermorph, from generated_stream_requirements.h


# The NecroPlayer's animation fields take a NAME and hash it themselves (sdbm,
# lower case); a row carrying the hash directly (kind 10) is ignored there:
# the first Ubermorph stood in a T-pose, idle and loco read FFFFFFFF live
# (22.09. 23:30), although retail uses kind 10 for other hash fields. The
# Hunter's banks keep no readable names, but sdbm is easy to run backwards: a
# short made-up name with exactly the wanted hash is as good as the real one.
# "ub_" + seven characters, found by meeting in the middle (0.2 s).
_ALPHA = "abcdefghijklmnopqrstuvwxyz0123456789_"
_M, _MASK = 0x1003F, 0xFFFFFFFF
_tail = {}


def _poly(text):
    result = 0
    for char in text:
        result = (result * _M + ord(char)) & _MASK
    return result


def name_for(anim_hash, prefix="ub_"):
    import itertools
    if not _tail:
        for chars in itertools.product(_ALPHA, repeat=3):
            _tail.setdefault(_poly("".join(chars)), "".join(chars))
    shift3, shift7, head = pow(_M, 3, 1 << 32), pow(_M, 7, 1 << 32), _poly(prefix)
    for chars in itertools.product(_ALPHA, repeat=4):
        middle = "".join(chars)
        need = (anim_hash - head * shift7 - _poly(middle) * shift3) & _MASK
        if need in _tail:
            name = prefix + middle + _tail[need]
            if base.sdbm(name) != anim_hash:
                raise SystemExit("preimage check failed for %08X" % anim_hash)
            return name
    raise SystemExit("no name found for %08X" % anim_hash)


def configure():
    missing = [key for key, value in HASHES.items() if value is None]
    if missing:
        raise SystemExit("not read out of the code yet: " + ", ".join(missing))
    base.SOURCE_GUID = PUKER
    base.GUID_SEED = "ubermorph"
    base.LEAPER = dict(RESOURCES)
    base.ANIMS = {key: name_for(value) for key, value in HASHES.items()}
    base.HASHES = {}
    base.DROP = list(DROP)
    base.ADD = [(donor, key, name_for(value) if isinstance(value, int) else value) for donor, key, value in ADD]
    base.VECTORS = dict(VECTORS)
    base.COMPONENT_TYPES = COMPONENT_TYPES
    base.COMPONENT_GUIDS = dict(COMPONENT_GUIDS)
    base.COMPONENT_INLINE = dict(COMPONENT_INLINE)
    base.FABRICATE = dict(FABRICATE)
    base.LEAPER_STREAM_IDENTITY = STREAM_IDENTITY


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pool", help="global_mp_assets.str")
    parser.add_argument("--tool-dir", required=True)
    parser.add_argument("--stream", required=True, help="output folder for ds2_ubermorph.str/.toc")
    args = parser.parse_args()
    configure()
    base.build_stream(args.pool, args.tool_dir, args.stream, "ds2_ubermorph")


if __name__ == "__main__":
    main()
