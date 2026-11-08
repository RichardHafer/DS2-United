# Voice packs

Every player and bot in co-op speaks with a voice pack: recorded lines that play when something happens to that character.

The lines play in 3D at the character's head:

- muffled behind walls;
- heard around corners along the walk between you;
- with a subtitle at the bottom of the screen.

## Where packs live

`<game>\plugins\voices\<pack>\pack.ini` plus the WAV files next to it. The folder name is the pack's id. Folders starting with `_` are ignored, so `_template` can stay.

## Who gets which pack

- **By default, join order:** the host gets the first pack of `[Voices] Defaults`, the next player who joined the second, and so on. The host's bots come after the humans. Empty `Defaults` means all packs in folder order.
- **Picking your own:** set `[Voices] Pack=<folder name>`. The other machines learn your pick within 2 s.
- **Sharing a pack:** several players may use the same pack.
- **A pick another machine doesn't have:** that machine plays your default pack instead.

## pack.ini

See `_template\pack.ini`. Each section is an event. Each line is `file.wav` or `file.wav|Subtitle`.

| Section | When | Suggested count |
| --- | --- | --- |
| `death` | the character dies | 2 to 4 |
| `pain` | the character takes damage (at most every 2.5 s) | 4 to 8 |
| `grab` | grabbed by an enemy (fallback for every kind) | 2 to 4 |
| `grab_slasher`, `grab_pack`, `grab_leaper`, ... | grabbed by that kind | 1 to 3 each, optional |
| `reload` | reloading (half the time, at most every 6 s) | 3 to 6 |
| `stomp` | stomping (every time, at most every 3 s) | 3 to 6 |
| `melee` | melee swing (every time, at most every 3 s) | 3 to 6 |
| `test` | the debug menu's "Voice test" | 1 |
| `low_health` | health falls below a quarter (again after it was above half), at most every 30 s | 2 to 4 |
| `low_ammo` | the gun in hand has one magazine or less left (60 %, at most every 45 s) | 2 to 4 |
| `low_ammo_empty` | the gun in hand is completely empty (always) | 1 to 3, optional |
| `new_area` | the first time in a room on this map (35 %, at most every 90 s) | 3 to 6 |
| `new_area_ch01` ... `new_area_ch16` | the same, in that chapter | optional |
| `big_enemy` | a big enemy comes into sight within 25 m (once per enemy, once a minute per kind) | 2 to 4 |
| `big_enemy_brute`, `big_enemy_tormentor`, ... | that kind | 1 to 3 each, optional |
| `multi_kill` | three kills within 4 s | 2 to 4 |
| `necro_spotted` | a necro player comes into sight within 20 m (story invasion) | 2 to 4 |

Grab kinds: `slasher`, `pack`, `leaper`, `lurker`, `brute`, `infector`, `stalker`, `puker`, `tripod`, `hunter`, `divider`, `swarm`, `tormentor`, `tentacle`, `nicole`. Twitchers count as slashers.

Big enemy kinds: `brute`, `stalker`, `tripod`, `hunter` (the Ubermorph), `tormentor`, `nicole`, `guardian`.

The six situation lines (`low_health` to `necro_spotted`) are judged on the speaker's own machine and reach the partners over the network. Lines about a room, a big enemy or a necro are said once for the team: a partner's line of the same kind keeps yours back for 8 s. `[Voices] Situations=0` switches all six off. A pack without these sections stays silent there.

Isaac's own voice is muted while packs are installed (`[Voices] MuteIsaac=1`). The muted sounds are pain, death, grab struggle, the stomp and melee grunts, the stomp yelling and the shock scream. The suit's breathing stays.

Events come from the game's own vocal triggers:

- **Pain:** only when a hit reaction animation plays, not on every scratch.
- **Melee and stomp:** from the swing and stomp grunts. The MP maps' hip melee uses the stomp grunt, so it plays a `stomp` line.
- **Death and reload:** these also reach the partners over the network.

The chances and gaps are set in `[Voices]` (`Pain=100,2500` means 100 % and 2500 ms).

## Example with folders

`_example_pack` keeps one folder per event (`death`, `pain`, `grab`, `grab\slasher`, `reload`, ...), each with a
`RECORD_HERE.txt` naming the files to record. Its `pack.ini` lists them with their folder (`pain\pain_01.wav`):
a file may sit in a subfolder of the pack, never outside it. Copy the folder to `plugins\voices\` under a name
without the leading `_` and record into it.

## Recording

- **Format:** WAV, ideally mono 16 bit, 44.1 or 48 kHz. Stereo, 24 bit and 32 bit float also work; stereo is mixed down to mono.
- **Editing:**
  - Cut the silence at both ends.
  - Normalise each line to about -1 dB peak so all packs sound equally loud.
  - Keep the room dry: no echo and no music.
- **Content:** The game adds no effects of its own, so grunts and breaths are part of the line. Keep reactions short: pain and melee under a second, death up to three seconds.
- **File names:** Name them as in the template (`pain_01.wav`, `pain_02.wav`, ...) and list each one in `pack.ini`.

## Testing

Use the debug menu, tab CHEATS, row "Voice test": every character says its `test` line (or a `pain` line), one after the other, at its own position.

The log (`DS2Coop.log`) lists each pack with its line count, every missing file, who speaks with which pack and every line played.

## Placeholders

`tools\make_voice_placeholders.py` builds four test packs (`test_a` to `test_d`) with the Windows speech voices into `build\voices`. They are only there to test the sound until the real recordings exist.
