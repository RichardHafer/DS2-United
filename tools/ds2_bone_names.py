"""Readable names for DS2 rig bones, whose rigs keep only sdbm hashes.

The names are recovered by hashing: the conventional DS2 names (prefix l_/r_/m_,
a body part, an optional suffix and number) and a few literal names seen in the
game's strings. A bone no name matches keeps hash_XXXXXXXX.
"""
import itertools

import ds2_bnk as bnk

LITERAL = ["character", "eye_l", "eye_r", "neckuv", "l_weapon", "r_weapon", "m_camera", "m_grabber", "m_root", "m_headtracker"]
STEMS = """hip knee ankle ball toe toes foot heel shoulder elbow wrist hand clav clavicle arm forearm upperarm leg thigh calf shin
thumb index middle ring pinky finger palm head jaw chin tongue teeth lip brow eyebrow lid eye cheek nose ear forehead face neck
spine lumbar thoracic pelvis root camera grabber headtracker weapon exeospine exeoshoulderblade shoulderblade shoulderflap
flapslider hiptwist kneetwist shouldertwist elbowtwist wristtwist forearmtwist twist null pelvisnull necknull tube pack tank
helmet visor light collar strap tail tentacle blade chest belly breast hair skirt coat cloth""".split()
PREFIXES = ["", "l_", "r_", "m_", "c_"]
SUFFIXES = ["", "null", "twist", "end", "tip", "upper", "lower", "inner", "outer", "front", "back", "top", "bottom", "a", "b", "c"]
DIGITS = ["", "1", "2", "3", "4", "5", "6", "01", "02", "03", "04", "05", "06"]


def names_for(hashes) -> dict:
    """hash -> name for every hash a candidate matches."""
    wanted, found = set(hashes), {}
    for name in LITERAL:
        if bnk.sdbm(name) in wanted:
            found.setdefault(bnk.sdbm(name), name)
    for p, s, x, d in itertools.product(PREFIXES, STEMS, SUFFIXES, DIGITS):
        for name in (p + s + x + d, p + s + d + x):
            h = bnk.sdbm(name)
            if h in wanted and h not in found:
                found[h] = name
    return found


def bone_name(h: int, names: dict) -> str:
    return names.get(h, "hash_%08X" % h)
