"""The Dementia mutator's sound folder: picks from variants/dementia/audio (made by
tools/export_dementia_audio.py) into variants/build/dementia, named the way
runtime_coop_dementia.inc sorts them (whisper_*, nicole_*, amb_*), with a .txt
subtitle next to each Nicole line. Install the folder as <game>\\plugins\\dementia.

    python -I tools/build_dementia_pack.py
"""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "variants" / "dementia" / "audio"
OUT = ROOT / "variants" / "build" / "dementia"

# Whispers: the creepy banks, four of the dementia land whisper loops and Nicole's "Isaac?" (no subtitle).
WHISPERS = ["whisper_ch10_%02d.wav" % i for i in range(7)] + ["whisper_ch12_%02d.wav" % i for i in range(5)] + \
           ["whisper_washboards_%02d.wav" % i for i in range(4)] + ["dland_%02d.wav" % i for i in range(4)]
WHISPER_RENAMED = {"nicole_dementia_audio_02_01d.wav": "whisper_nicole_isaac.wav"}
# Nicole's lines that work outside their scene (the story-bound ones and those under music stay out).
NICOLE = ["nicole_dementia_audio_02_01a.wav", "nicole_dementia_audio_02_01b.wav", "nicole_dementia_audio_02_01c.wav",
          "nicole_dementia_audio_06_01.wav", "nicole_igc_01_07_1.wav", "nicole_igc_01_07_2.wav", "nicole_se_10_11a_3.wav",
          "nicole_se_10_11a_4.wav", "nicole_video_monitor_03_03.wav", "nicole_video_monitor_08_06_2.wav"]
AMBIENCE = ["amb_dementia_01.wav", "amb_dementialand_v01.wav", "amb_creepy_empty_space_01.wav"]


def ascii_text(text):
    """The overlay draws ASCII only (the text table holds some UTF-8 read as cp1252)."""
    for a, b in (("â€¦", "..."), ("…", "..."), ("’", "'"), ("‘", "'"), ("“", '"'), ("”", '"'), ("—", " - "), ("–", "-")):
        text = text.replace(a, b)
    return text.encode("ascii", "replace").decode("ascii")


def timed_parts(text, duration_s, limit=90):
    """The subtitle in parts of at most `limit` characters (a row holds 99), cut after a
    sentence where possible, each shown from its share of the line's length: 'ms|text'."""
    words, parts, current = text.split(), [], ""
    for word in words:
        if current and len(current) + 1 + len(word) > limit:
            parts.append(current)
            current = word
        else:
            current = (current + " " + word).strip()
            if current.endswith((".", "?", "!")) and len(current) > limit * 0.6:
                parts.append(current)
                current = ""
    if current:
        parts.append(current)
    total, done, out = sum(len(p) for p in parts), 0, []
    for p in parts:
        out.append("%d|%s" % (int(duration_s * 1000.0 * done / max(total, 1)), p))
        done += len(p)
    return out


def main():
    manifest = {f["file"]: f for f in json.loads((SOURCE / "manifest.json").read_text(encoding="utf-8"))["files"]}
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    picked, size, missing = [], 0, []
    for name in WHISPERS + list(WHISPER_RENAMED) + NICOLE + AMBIENCE:
        entry = manifest.get(name)
        if not entry or not (SOURCE / name).exists():
            missing.append(name)
            continue
        if entry.get("avoid"):
            raise SystemExit("%s is marked avoid in the manifest" % name)
        target = WHISPER_RENAMED.get(name, name if not name.startswith("dland_") else "whisper_" + name)
        shutil.copyfile(SOURCE / name, OUT / target)
        size += (OUT / target).stat().st_size
        subtitle = entry.get("subtitle") if name in NICOLE else None
        if isinstance(subtitle, str) and subtitle:
            lines = timed_parts(ascii_text(subtitle), float(entry.get("duration_s") or 0.0))
            (OUT / target).with_suffix(".txt").write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
        picked.append(target)
    if missing:
        raise SystemExit("missing in %s: %s (run tools/export_dementia_audio.py)" % (SOURCE, ", ".join(missing)))
    kinds = {k: sum(1 for p in picked if p.startswith(k)) for k in ("whisper", "nicole", "amb")}
    print("dementia pack: %d files (%s), %.1f MB -> %s" % (len(picked), ", ".join("%s %d" % kv for kv in kinds.items()), size / 1048576.0, OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
