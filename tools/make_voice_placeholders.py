"""Four placeholder voice packs (test_a .. test_d) spoken by the Windows speech voices,
so the voice system can be heard before the real recordings exist.

Writes build/voices/<pack>/pack.ini and its WAV files (22.05 kHz, 16 bit, mono).
Needs the System.Speech voices that come with Windows (Zira, Hazel, ...).

usage: python -I tools/make_voice_placeholders.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "voices"

LINES = {
    "death": ["Aaargh!", "No... not like this."],
    "pain": ["Ugh!", "Argh!", "Damn it!", "Ow!"],
    "grab": ["Get off me!", "Let go!"],
    "grab_slasher": ["Get this thing off me!"],
    "grab_pack": ["They're all over me!"],
    "grab_leaper": ["Off! Get off!"],
    "reload": ["Reloading!", "Changing mag!"],
    "stomp": ["Hah!", "Stay down!"],
    "melee": ["Back off!", "Hyah!"],
    "test": ["Voice check. Can you hear me?"],
}
# folder, shown name, voice, pitch, rate
PACKS = [
    ("test_a", "Test A", "Microsoft Zira Desktop", "+0%", "0%"),
    ("test_b", "Test B", "Microsoft Hazel Desktop", "-25%", "-10%"),
    ("test_c", "Test C", "Microsoft Zira Desktop", "+30%", "+10%"),
    ("test_d", "Test D", "Microsoft Hazel Desktop", "+10%", "+5%"),
]


def ps_quote(text):
    return "'" + text.replace("'", "''") + "'"


def main():
    script = [
        "Add-Type -AssemblyName System.Speech",
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer",
        "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(22050, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, "
        "[System.Speech.AudioFormat.AudioChannel]::Mono)",
    ]
    for folder, name, voice, pitch, rate in PACKS:
        pack = OUT / folder
        pack.mkdir(parents=True, exist_ok=True)
        ini = ["; Placeholder pack made by tools/make_voice_placeholders.py (Windows speech voice).", "", "[Pack]", "Name=" + name]
        for event, texts in LINES.items():
            ini += ["", "[" + event + "]"]
            for i, text in enumerate(texts, 1):
                wav = "%s_%02d.wav" % (event, i)
                ini.append("%s|%s" % (wav, text))
                ssml = ("<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><voice name='%s'>"
                        "<prosody pitch='%s' rate='%s'>%s</prosody></voice></speak>") % (voice, pitch, rate, text.replace("&", "and"))
                script.append("$s.SetOutputToWaveFile(%s, $f)" % ps_quote(str(pack / wav)))
                script.append("$s.SpeakSsml(%s)" % ps_quote(ssml))
        (pack / "pack.ini").write_bytes(("\r\n".join(ini) + "\r\n").encode("utf-8"))
    script.append("$s.SetOutputToNull()")
    script.append("$s.Dispose()")
    ps1 = OUT / "make_placeholders.ps1"
    ps1.write_bytes(("\r\n".join(script) + "\r\n").encode("utf-8-sig"))
    result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit("speech synthesis failed (%d)" % result.returncode)
    count = sum(1 for _ in OUT.glob("*/*.wav"))
    print("%d placeholder lines in %d packs: %s" % (count, len(PACKS), OUT))


if __name__ == "__main__":
    main()
