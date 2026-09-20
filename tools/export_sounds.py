"""Export every sound of a variant's base package as WAV, for editing.

    python tools/export_sounds.py SuperExploder
        writes variants/sounds/<variant>/original/<bank>_<index>.wav for every
        SBK of the variant's skin package (needs ears_cli.exe next to this file)

Put the edited files, same names, 16 bit mono, into
variants/sounds/<variant>/original/edited/ and name the banks in the spec
under "custom_sounds" (see the SuperExploder entry); Rebuild-Variants.cmd
then encodes them into private bank copies.
"""
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
import ds2str  # noqa: E402
import skin_assets  # noqa: E402
import custom_sounds as cs  # noqa: E402
from export_skin_textures import CHARACTER_STREAMS  # noqa: E402


def main():
    name = sys.argv[1]
    spec = json.loads((ROOT / "variants/ds2_variants.json").read_text(encoding="utf-8"))
    variant = next(v for v in spec["variants"] if v["name"] == name)
    package = variant["skin"]["package"]
    out = ROOT / "variants/sounds" / name / "original"
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for it in skin_assets.load_package(ds2str.StreamSet(str(CHARACTER_STREAMS / package))):
        if it["meta"]["type"] != "SBK":
            continue
        stem = it["meta"]["name"][:-4]
        sounds = cs.sounds_of(it["payload"])
        for i, sound in enumerate(sounds):
            h1, h2 = struct.unpack_from(">II", sound, 0)
            snu = bytearray(cs.SNU_HEADER)
            struct.pack_into("<I", snu, 8, 0x20)
            tmp = out / "_tmp.exa.snu"
            tmp.write_bytes(bytes(snu) + sound[:8] + bytes(8) + sound[8:])
            wav = out / ("%s_%02d.wav" % (stem, i))
            result = subprocess.run([str(cs.EARS_CLI), "decode", str(tmp), str(wav)], capture_output=True, text=True)
            status = "ok" if result.returncode == 0 else (result.stdout + result.stderr).strip()
            print("%-32s %02d codec=%d %5d Hz %6.2f s  %s" % (it["meta"]["name"], i, (h1 >> 24) & 15, h1 & 0xFFFFFF,
                                                             (h2 & 0x1FFFFFFF) / max(1, h1 & 0xFFFFFF), status))
            total += result.returncode == 0
        if (out / "_tmp.exa.snu").exists():
            os.remove(out / "_tmp.exa.snu")
    print("%d sounds written to %s" % (total, out))


if __name__ == "__main__":
    main()
