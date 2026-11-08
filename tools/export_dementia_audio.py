"""Export the Dementia mutator's hallucination sounds as 16 bit mono WAV.

    python -I tools/export_dementia_audio.py

Reads the extracted retail data (work/ds2dat1/audiostreams, the sound bank
streams named in analysis/catalog/assets.jsonl and the English subtitle table)
and writes variants/dementia/audio/*.wav plus manifest.json. Needs
ears_cli.exe next to this file. Nothing in the game folder is written.

ears_cli decodes mono and stereo EALayer3 v2 but gives up after the first
frame when a stream has more channels. Those streams are 1-2 channel
substreams whose frames alternate inside every block (A0 B0 A1 B1 ...), so
each substream is copied into its own SNU, decoded alone and the channels are
put back together before the downmix.

Subtitles: a .tox (TOB1) lists (text key, start ms, duration ms); the key is
looked up in deadspace2_global.en.lh2. Nicole's lines share one key range.
"""
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
from ds2_paths import GAME_DIR, MODDING_TOOLS, WORK  # noqa: E402

EARS_CLI = TOOLS / "ears_cli.exe"
OUT = ROOT / "variants" / "dementia" / "audio"
STREAMS = WORK / "ds2dat1" / "audiostreams"
TEXT_STR = WORK / "ds2dat1" / "text_assets" / "text" / "f40cc683.str"
SUBTITLE_STR = WORK / "ds2dat1" / "text_assets" / "text_assets_global.str"
CODECS = {4: "XAS1", 7: "EALayer3 v2 spike"}

# Observed: every Nicole line of the campaign has a key in this range; these
# three keys inside it are the Ishimura doctors.
NICOLE_KEYS = (0x0CF10000, 0x0D810000)
NOT_NICOLE = {0x0CFE4C1C, 0x0CFE4C1E, 0x0D795AE2}

FRONT = [0, 1]   # quad cutscene and monitor mixes: the voice sits in the front pair, the rear pair is a low bed


def stream(stem, path, speaker, use, tox=None, **extra):
    """extra: avoid (reason), note, split="nicole" (one file per Nicole subtitle
    line, with per line avoid reasons in line_avoid), channels (subset to downmix)."""
    return dict(stem=stem, path=path, speaker=speaker, use=use, tox=tox, **extra)


STORY = "story specific: "
STREAM_TABLE = [
    stream("nicole_dementia_audio_02_01a", "ntia_aud/dementia_audio_02_01a", "Nicole", "Nicole line", "dementia_audio_02_01a"),
    stream("nicole_dementia_audio_02_01b", "ntia_aud/dementia_audio_02_01b", "Nicole", "Nicole line", "dementia_audio_02_01b"),
    stream("nicole_dementia_audio_02_01c", "ntia_aud/dementia_audio_02_01c", "Nicole", "Nicole line", "dementia_audio_02_01c"),
    stream("nicole_dementia_audio_02_01d", "ntia_aud/dementia_audio_02_01d", "Nicole", "whisper", "dementia_audio_02_01d"),
    stream("nicole_dementia_audio_06_01", "ntia_aud/dementia_audio_06_01", "Nicole", "Nicole line", "dementia_audio_06_01",
           note="23 s monologue"),
    stream("nicole_dementia_audio_06_02", "ntia_aud/dementia_audio_06_02", "Nicole", "Nicole line", "dementia_audio_06_02",
           avoid=STORY + "choosing the Ishimura over Isaac"),
    stream("nicole_dementia_audio_06_03", "ntia_aud/dementia_audio_06_03", "Nicole", "Nicole line", "dementia_audio_06_03",
           avoid=STORY + "her final transmission from DS1"),
    stream("nicole_dementia_audio_08_01", "ntia_aud/dementia_audio_08_01", "Nicole", "Nicole line", "dementia_audio_08_01",
           avoid=STORY + "the plastic and tape of the ch08 Ishimura"),
    stream("nicole_dementia_audio_08_02", "ntia_aud/dementia_audio_08_02", "Nicole", "Nicole line", "dementia_audio_08_02",
           avoid=STORY + "her body in the Ishimura morgue"),
    stream("voice_dementia_02_01", "ntia_02_/dementia_02_01", "unknown", "whisper",
           note="no subtitle; ch02 tram station Nicole scene (se_dementia_02_01); band limited to 300-3400 Hz with speech "
                "rhythm, likely a filtered Nicole layer; check by ear"),
    stream("amb_dementia_audio_02_01", "ntia_aud/dementia_audio_02_01", "unknown", "ambience bed",
           note="no subtitle; bed of the ch02 tram station Nicole scene, channels uncorrelated"),
    stream("amb_dementia_audio_11_01", "ntia_aud/dementia_audio_11_01", "unknown", "ambience bed",
           note="no subtitle; played in the ch11 elevator (elevator_tran1_trans2); pitched voice-like drone, check by ear"),
    stream("amb_dementia_quad_01", "ntia_qua/dementia_quad_01", "none", "ambience bed"),
    stream("amb_dementia_quad_vo_duck", "ntia_qua/dementia_quad_vo_duck", "none", "ambience bed",
           note="same bed as amb_dementia_quad_01 with the front pair ducked for dialogue"),
    stream("amb_dementia_01", "dementia/amb_dementia_01", "none", "ambience bed", note="loop flag set; mostly below 150 Hz"),
    stream("amb_dementialand_v01", "dementia/amb_dementialand_v01", "none", "ambience bed"),
    stream("amb_creepy_empty_space_01", "creepy_e/amb_creepy_empty_space_01", "none", "ambience bed"),
    stream("sting_nicole_explodes", "icole_ex/mx_nicole_explodes", "none", "sting", note="25 s music cue, trim before use"),
    stream("nicole_video_monitor_03_03", "o_monito/video_monitor_03_03", "Nicole", "Nicole line", "video_monitor_03_03",
           channels=FRONT),
    stream("nicole_video_monitor_08_03", "o_monito/video_monitor_08_03", "Nicole", "Nicole line", "video_monitor_08_03",
           channels=FRONT, avoid=STORY + "the Ishimura medical deck, her last hours"),
    stream("nicole_video_monitor_08_06", "o_monito/video_monitor_08_06", "Nicole", "Nicole line", "video_monitor_08_06",
           channels=FRONT, split="nicole", line_avoid={1: STORY + "the ch08 tarps"}),
    stream("nicole_video_monitor_09_01", "o_monito/video_monitor_09_01", "Nicole", "Nicole line", "vm_09_01",
           avoid=STORY + "the Ishimura posting flashback", note="subtitle taken from vm_09_01.tox (no video_monitor_09_01.tox)"),
    stream("nicole_se_10_11a", "0_11a/se_10_11a", "Nicole", "Nicole line", "se_10_11a", split="nicole",
           line_avoid={1: STORY + "the ch10 eye machine", 2: STORY + "the ch10 eye machine"}),
    stream("nicole_igc_01_07", "01_07/igc_01_07", "Nicole", "Nicole line", "igc_01_07", channels=FRONT, split="nicole",
           note="cutscene mix; the line before (Isaac) is cut away"),
    stream("nicole_igc_09_01", "09_01/igc_09_01", "Nicole", "Nicole line", "igc_09_01", channels=FRONT, split="nicole",
           note="cutscene mix dominated by music and effects; check by ear",
           line_avoid={2: STORY + "the Ishimura posting", 3: STORY + "the Ishimura posting"}),
]

# (bank asset, output prefix, speaker, use); duplicates of an earlier bank's
# sound are listed as aliases of that file instead of written again.
BANK_TABLE = [
    ("creepy_ch10_dementia_whispers.sbk", "whisper_ch10", "unknown", "whisper"),
    ("creepy_ch12_dementia_whispers.sbk", "whisper_ch12", "unknown", "whisper"),
    ("creepy_ch07_whisper_wind.sbk", "whisper_ch07", "unknown", "whisper"),
    ("creepy_whispers_washboards.sbk", "whisper_washboards", "unknown", "whisper"),
    ("nicole.sbk", "nicole_bank", "none", "foley"),
    ("pack_boy_dementia.sbk", "boy_dementia", "other", "sting"),
    ("dementia_land_z01.sbk", "dland", "none", None),
]
# Notes from the spectrum and envelope of each sound; nothing in the data names them.
BANK_NOTES = {}
for i in range(12):
    BANK_NOTES[("nicole.sbk", i)] = {"note": "short unvoiced sound of Nicole's character bank, cloth or scuff like"}
for i in range(12, 17):
    BANK_NOTES[("nicole.sbk", i)] = {"note": "short low thump, footstep like"}
for i in range(17, 23):
    BANK_NOTES[("nicole.sbk", i)] = {"use": "sting", "note": "impact or boom"}
for i in range(102):
    BANK_NOTES[("pack_boy_dementia.sbk", i)] = {"note": "child vocal set of the dementia Pack boy, not Nicole"}
for i in range(8):
    BANK_NOTES[("dementia_land_z01.sbk", i)] = {"use": "whisper", "note": "one of 8 stereo 11 s variants, speech rhythm without pitch"}
for i in range(8, 12):
    BANK_NOTES[("dementia_land_z01.sbk", i)] = {"note": "pitched voice-like layer, check by ear"}


def log(*args):
    print(*args, flush=True)


# ---------------------------------------------------------------- decoding

def snr(data, at):
    h1, h2 = struct.unpack_from(">II", data, at)
    return {"codec": (h1 >> 24) & 15, "channels": ((h1 >> 18) & 63) + 1, "rate": h1 & 0x3FFFF,
            "loop": bool((h2 >> 29) & 1), "samples": h2 & 0x1FFFFFFF, "h1": h1}


def bank_snu(sound):
    """A bank sound entry (SNR header + blocks) as a standalone SNU file."""
    info = snr(sound, 0)
    head = struct.pack("<4sIII", bytes([3, 0, 0, info["channels"]]), info["samples"] * 1000 // max(info["rate"], 1), 0x20, 0)
    return head + sound[:8] + bytes(8) + sound[8:]


def substreams(data):
    """The 1-2 channel substreams of a multichannel EALayer3 SNU, each as its own SNU."""
    body = struct.unpack_from("<I", data, 8)[0]
    info = snr(data, 16)
    layout, outs, pos = None, None, body
    while pos + 8 <= len(data):
        head, samples = struct.unpack_from(">II", data, pos)
        size, flag = head & 0xFFFFFF, head >> 24
        if size < 8:
            raise ValueError("block at %#x has size %d" % (pos, size))
        frames, p, end = [], pos + 8, pos + size
        while p + 2 <= end:
            word = struct.unpack_from(">H", data, p)[0]
            n = word & 0xFFF
            if n == 0:   # zero fill after the last frame
                break
            frames.append((data[p:p + n], 2 if word & 0x4000 else 1))
            p += n
        if layout is None:
            layout, have = [], 0
            for _, ch in frames:
                if have >= info["channels"]:
                    break
                layout.append(ch)
                have += ch
            if have != info["channels"]:
                raise ValueError("first block frames give %d of %d channels" % (have, info["channels"]))
            outs = [bytearray() for _ in layout]
        if len(frames) % len(layout) or any(ch != layout[i % len(layout)] for i, (_, ch) in enumerate(frames)):
            raise ValueError("block at %#x does not follow the substream layout %s" % (pos, layout))
        for s in range(len(layout)):
            part = b"".join(f for f, _ in frames[s::len(layout)])
            outs[s] += struct.pack(">II", (flag << 24) | (len(part) + 8), samples) + part
        pos = end
        if flag & 0x80:
            break
    files = []
    for s, ch in enumerate(layout):
        head = bytearray(data[:body])
        head[3] = ch
        struct.pack_into(">I", head, 16, (info["h1"] & ~(63 << 18)) | ((ch - 1) << 18))
        files.append(bytes(head) + bytes(outs[s]))
    return files, layout


def ears_decode(snu, folder):
    src, dst = Path(folder) / "in.exa.snu", Path(folder) / "out.wav"
    src.write_bytes(snu)
    if dst.exists():
        dst.unlink()
    result = subprocess.run([str(EARS_CLI), "decode", str(src), str(dst)], capture_output=True, text=True)
    if result.returncode != 0 or not dst.exists():
        raise ValueError("ears_cli failed (%d): %s %s" % (result.returncode, result.stdout.strip(), result.stderr.strip()))
    with wave.open(str(dst)) as w:
        if w.getsampwidth() != 2:
            raise ValueError("ears_cli wrote %d bit samples" % (8 * w.getsampwidth()))
        pcm = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels())
    return pcm, w.getframerate()


def decode(snu, folder):
    """(int16 frames x channels, info, substream layout or None)."""
    info = snr(snu, 16)
    layout = None
    if info["codec"] == 7 and info["channels"] > 2:
        parts, layout = substreams(snu)
        decoded = [ears_decode(p, folder)[0] for p in parts]
        n = min(len(d) for d in decoded)
        pcm = np.hstack([d[:n] for d in decoded])
    else:
        pcm, rate = ears_decode(snu, folder)
        if rate != info["rate"]:
            raise ValueError("decoded rate %d, header says %d" % (rate, info["rate"]))
    if pcm.shape[1] != info["channels"]:
        raise ValueError("decoded %d channels, header says %d" % (pcm.shape[1], info["channels"]))
    if abs(len(pcm) - info["samples"]) > 1152:
        raise ValueError("decoded %d samples, header says %d" % (len(pcm), info["samples"]))
    return pcm, info, layout


def to_mono(pcm):
    """(mono int16, correlation of the least similar channel pair or None)."""
    if pcm.shape[1] == 1:
        return pcm[:, 0].copy(), None
    x = pcm.astype(np.float64)
    live = [c for c in range(x.shape[1]) if x[:, c].std() > 1.0]
    corr = None
    if len(live) > 1:
        m = np.corrcoef(x[:, live].T)
        corr = float(min(m[i, j] for i in range(len(live)) for j in range(i + 1, len(live))))
    mono = x.mean(axis=1)
    peak, src_peak = np.abs(mono).max(), np.abs(x).max()
    if peak > 0:
        mono *= min(src_peak / peak, 4.0)   # keep the loudest channel's peak level
    return np.clip(np.round(mono), -32768, 32767).astype("<i2"), corr


def write_wav(path, mono, rate):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(mono.astype("<i2").tobytes())


def quiet_point(mono, rate, lo, hi):
    """Centre (s) of the quietest 20 ms window between lo and hi seconds."""
    win = int(rate * 0.02)
    a, b = max(0, int(lo * rate)), min(len(mono) - win, int(hi * rate))
    if b <= a:
        return max(0.0, min(lo, len(mono) / rate))
    x = mono[a:b + win].astype(np.float64) ** 2
    energy = np.convolve(x, np.ones(win), "valid")[:b - a]
    return (a + int(np.argmin(energy)) + win // 2) / rate


def cut(mono, rate, start, end, fade=0.01):
    part = mono[int(start * rate):int(end * rate)].astype(np.float64)
    n = min(int(fade * rate), len(part) // 2)
    if n:
        ramp = np.linspace(0.0, 1.0, n)
        part[:n] *= ramp
        part[-n:] *= ramp[::-1]
    return np.round(part).astype("<i2")


# ---------------------------------------------------------------- retail data

def stream_assets(path):
    sys.path.insert(0, str(MODDING_TOOLS))
    import ds2str
    from ds2_assets import assets
    stream = ds2str.StreamSet(str(path))
    for meta, chunks in assets(stream):
        yield meta, (lambda c=chunks, m=meta: stream.asset_payload(c, m["size"]))


def subtitles():
    """{tox stem: [(key, start s, end s, speaker, text)]} with English text."""
    table = {}
    for meta, payload in stream_assets(TEXT_STR):
        if meta["name"] == "deadspace2_global.en.lh2":
            lh2 = payload()
            n = struct.unpack_from("<I", lh2, 0x10)[0]
            keys = struct.unpack_from("<%dI" % n, lh2, 0x20)
            offs = struct.unpack_from("<%dI" % n, lh2, 0x20 + 4 * n)
            table = {k: lh2[o:lh2.index(b"\0", o)].decode("cp1252", "replace") for k, o in zip(keys, offs)}
    if not table:
        raise SystemExit("no English text table in %s" % TEXT_STR)
    out = {}
    for meta, payload in stream_assets(SUBTITLE_STR):
        if meta["type"] != "TOB":
            continue
        data = payload()
        if data[:4] != b"TOB1":
            continue
        lines = []
        for i in range(struct.unpack_from("<I", data, 24)[0]):
            key, start, length, _ = struct.unpack_from("<4I", data, 32 + 16 * i)
            nicole = NICOLE_KEYS[0] <= key < NICOLE_KEYS[1] and key not in NOT_NICOLE
            text = table.get(key)
            if text is not None:
                text = " ".join(text.replace("\\n", " ").split())   # the table keeps literal \n
            lines.append((key, start / 1000.0, (start + length) / 1000.0, "Nicole" if nicole else "other", text))
        out[meta["name"][:-4]] = lines
    return out


def bank_payloads(names):
    archives = {}
    with (ROOT / "analysis" / "catalog" / "assets.jsonl").open(encoding="utf-8") as f:
        for line in f:
            if any(name in line for name in names):
                row = json.loads(line)
                if row["name"] in names:
                    archives.setdefault(row["name"], row["archive"])
    out = {}
    for name in names:
        if name not in archives:
            raise SystemExit("%s is in no stream of the catalog" % name)
        for meta, payload in stream_assets(WORK / archives[name]):
            if meta["name"] == name and meta["type"] == "SBK":
                out[name] = (archives[name], payload())
                break
        else:
            raise SystemExit("%s not found in %s" % (name, archives[name]))
    return out


# ---------------------------------------------------------------- export

def rel(path):
    try:
        return Path(path).relative_to(GAME_DIR).as_posix()
    except ValueError:
        return Path(path).as_posix()


def entry(file, mono, rate, info, source, layout, corr, **fields):
    row = {"file": file, "source": source, "duration_s": round(len(mono) / rate, 3),
           "source_codec": CODECS.get(info["codec"], str(info["codec"])), "source_channels": info["channels"],
           "source_rate": info["rate"], "out_channels": 1, "out_rate": rate}
    if info["loop"]:
        row["source_loop_flag"] = True
    if info["channels"] > 1:
        row["downmix"] = "mean of %d channels, peak matched" % info["channels"]
        if corr is not None:
            row["channel_correlation_min"] = round(corr, 3)
    if layout:
        row["substreams"] = layout
    row.update(fields)
    return row


def export_streams(folder, subs, manifest, failures):
    for spec in STREAM_TABLE:
        stem, tox, split = spec["stem"], spec["tox"], spec.get("split")
        path = STREAMS / (spec["path"] + ".exa.snu")
        try:
            pcm, info, layout = decode(path.read_bytes(), folder)
        except (OSError, ValueError) as error:
            failures.append({"source": rel(path), "error": str(error)})
            log("FAIL %-36s %s" % (stem, error))
            continue
        picked = spec.get("channels")
        mono, corr = to_mono(pcm[:, picked] if picked else pcm)
        rate = info["rate"]
        lines = subs.get(tox, []) if tox else []
        if tox and not lines:
            failures.append({"source": rel(path), "error": "no subtitle " + tox})
        common = {"speaker": spec["speaker"], "use": spec["use"]}
        for key in ("avoid", "note"):
            if spec.get(key):
                common[key] = spec[key]
        if tox:
            common["subtitle_file"] = tox + ".tox"
        if picked:
            common["downmix"] = "mean of channels %s of %d, peak matched" % ("+".join(map(str, picked)), info["channels"])
        if split == "nicole":
            for n, (a, b, text) in enumerate(nicole_cuts(mono, rate, lines)):
                name = "%s_%d.wav" % (stem, n + 1)
                part = cut(mono, rate, a, b)
                write_wav(OUT / name, part, rate)
                fields = dict(common, range_s=[round(a, 3), round(b, 3)], subtitle=text)
                if n + 1 in spec.get("line_avoid", {}):
                    fields["avoid"] = spec["line_avoid"][n + 1]
                manifest.append(entry(name, part, rate, info, rel(path), layout, corr, **fields))
                log("ok   %-36s %6.2f s  %s" % (name, len(part) / rate, (text or "")[:50]))
            continue
        name = stem + ".wav"
        write_wav(OUT / name, mono, rate)
        if len(lines) == 1 and lines[0][3] == spec["speaker"]:
            common["subtitle"] = lines[0][4]
        elif lines:
            common["subtitle"] = [{"start_s": round(s, 3), "end_s": round(e, 3), "speaker": who, "text": text}
                                  for _, s, e, who, text in lines]
        manifest.append(entry(name, mono, rate, info, rel(path), layout, corr, **common))
        log("ok   %-36s %6.2f s  %d ch %d Hz%s" % (name, len(mono) / rate, info["channels"], rate,
                                                   "  (substreams %s)" % layout if layout else ""))


def nicole_cuts(mono, rate, lines):
    """(start s, end s, text) per Nicole line; neighbouring lines share the
    quietest point between them as their boundary."""
    length = len(mono) / rate
    edges = []
    for (_, _, e, _, _), (_, s, _, _, _) in zip(lines, lines[1:]):
        # subtitles overlap or butt; the real gap sits a little before the later time
        edges.append(quiet_point(mono, rate, min(e, s) - 0.35, max(e, s) + 0.1))
    out = []
    for i, (_, s, e, who, text) in enumerate(lines):
        if who != "Nicole":
            continue
        if i:
            a = edges[i - 1]
        else:
            a = 0.0 if s < 0.5 else quiet_point(mono, rate, s - 0.6, s + 0.05)
        b = edges[i] if i < len(edges) else quiet_point(mono, rate, e, min(e + 1.0, length))
        out.append((a, b, text))
    return out


def default_use(seconds):
    return "ambience bed" if seconds >= 8.0 else "sting"


def export_banks(folder, manifest, failures):
    seen = {}
    banks = bank_payloads([b[0] for b in BANK_TABLE])
    import custom_sounds as cs
    for bank, prefix, speaker, use in BANK_TABLE:
        archive, payload = banks[bank]
        sounds = cs.sounds_of(payload)
        written = 0
        for i, sound in enumerate(sounds):
            source = "%s : %s #%d" % (rel(WORK / archive), bank, i)
            digest = hashlib.sha1(sound).hexdigest()
            if digest in seen:
                seen[digest].setdefault("aliases", []).append(source)
                continue
            try:
                pcm, info, layout = decode(bank_snu(sound), folder)
            except ValueError as error:
                failures.append({"source": source, "error": str(error)})
                log("FAIL %s #%d %s" % (bank, i, error))
                continue
            mono, corr = to_mono(pcm)
            name = "%s_%02d.wav" % (prefix, i)
            write_wav(OUT / name, mono, info["rate"])
            fields = {"speaker": speaker, "use": use or default_use(len(mono) / info["rate"])}
            fields.update(BANK_NOTES.get((bank, i), {}))
            row = entry(name, mono, info["rate"], info, source, layout, corr, **fields)
            manifest.append(row)
            seen[digest] = row
            written += 1
        log("bank %-36s %d sounds, %d written, %d duplicates" % (bank, len(sounds), written, len(sounds) - written))


def main():
    if not EARS_CLI.exists():
        raise SystemExit("decoder missing: %s" % EARS_CLI)
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.wav"):
        old.unlink()
    manifest, failures = [], []
    subs = subtitles()
    with tempfile.TemporaryDirectory() as folder:
        export_streams(folder, subs, manifest, failures)
        export_banks(folder, manifest, failures)
    total = sum((OUT / row["file"]).stat().st_size for row in manifest)
    doc = {"generator": "tools/export_dementia_audio.py", "game_dir": GAME_DIR.as_posix(), "files": manifest, "failures": failures}
    (OUT / "manifest.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    log("%d files, %.1f MB, %d failures -> %s" % (len(manifest), total / 1e6, len(failures), OUT))
    for f in failures:
        log("  failed: %s: %s" % (f["source"], f["error"]))


if __name__ == "__main__":
    main()
