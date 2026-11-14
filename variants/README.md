# Enemy variant packs

One folder per variant under `variants/packs/<Name>/`:

| path | what |
|---|---|
| `variant.json` | the variant: base enemy, numbers, speed, size, look, sounds, model |
| `skin/` | edited DDS textures (`skin export` puts the current ones here) |
| `sounds/` | edited WAV files for the variant's own sound banks |
| `model/` | the character package rebuilt with other meshes (4Kodda's toolkit) |

Paths inside `variant.json` are relative to the pack folder. All commands run from the
DeadSpace2Randomizer folder with `python -I tools/variant_pack.py ...`.

## Adding a variant

1. `new ArmoredBrute --family Brute` makes the folder with a disabled `variant.json`. It picks
   the family's usual character package and an authored record the pool already copies.
   Families: Zombie (slasher), Leaper, Lurker, Puker, Exploder, Pregnant, PackEnemy,
   TubeBeetle (crawler), Infector, Stalker, SwarmEnemy, Brute, DividerCore, Hunter (Ubermorph).
   `--package zomb_cct/slasher_armor.str` picks another look of the family.
2. `fields ArmoredBrute` lists every number the record authors, with its current value.
   Put the ones to change into `"fields"`: `"x1.5"` multiplies, `"=0.8"` sets a float,
   `"i0"` sets a whole number (the 0/1 switches). The key is `Component.CMD_Name` as listed.
3. `speed` is the animation and movement factor (1.25 = a quarter faster), `scale` the visual
   size (the collision capsule stays, keep it modest), `chance` the percentage of the family's
   spawns the variant takes part in.
4. Look: either `"tint": {"hue": 20, "sat": 1.4, "val": 0.6}` (or `[r, g, b]`) for every
   colour texture, or hand-painted textures: `skin export ArmoredBrute`, edit the `.dds` files
   in `skin/` (not the `.original.dds`; same size and format, saved with mipmaps), then
   `skin apply ArmoredBrute`. `"tag"` is one character that keeps the copied names apart;
   two variants on one package need different tags.
5. Sounds (optional): `sounds export ArmoredBrute` writes every sound of the package as
   `sounds/original/<bank>_<nn>.wav`. Copy the ones to change into `sounds/` under the same
   name (16 bit mono WAV), then `sounds setup ArmoredBrute`. Sounds you leave out keep the
   original.
6. Model (optional): build the package with other meshes in Blender with the DS2-3 Modding
   Toolkit by 4Kodda (meshes weighted to the character's own skeleton; its Pack STR step writes
   the `.str`; do not use its installer, which writes into the game's archives). Put the file
   into `model/` and add `"model": {"package_file": "model/brute.str"}`. `model check
   ArmoredBrute` lists what differs from the retail package. Textures and sounds then come from
   that file too, so `skin export` shows the model's own textures. Changed animations inside the
   package travel along with the character copy.
7. `check ArmoredBrute`, then set `"enabled": true`.
8. `build` is a dry run on a scratch copy; `build --write` builds the pool, the shared model
   stream `ds2_randomizer_models` and `src/generated_variant_donors.h`. Then `build_ds2coop.cmd`
   (or `scripts\Rebuild-Variants.cmd` for both steps).
9. Install with the game closed: `build\DS2Coop.asi` into `plugins\`, and
   `analysis\runtime_stream\ds2_randomizer_pool.str/.toc` plus `ds2_randomizer_models.str/.toc`
   into `<game>\randomizer\`. Every machine in co-op needs the same files.
10. INI: `[DS2Randomizer] Variants=1`, and `[Variants] ArmoredBrute=<percent>` overrides the
    pack's chance (0 keeps a built variant from spawning). Stat and skin variants reach the
    randomizer only; survival skips them (it takes only the retargeted twitcher, see below).

## Another character package (retarget)

`"retarget": {"package": "zomb_cct/twitcher_security.str"}` builds the variant from another
package of the same family: the record's model, rig, model stream and dismemberment prop point at
that package, and the build forces it resident in the pool TOC. No skin with it. The Twitcher pack
uses it with `"contexts": ["direct", "vent"]` (no scripted entrances), `"require_fields"` (a source
must author play dead, so it can be switched off) and `"exclude_sources"` (Ellie's two slashers,
which track only NPCs). Retargeted rows have their own INI switch, `[DS2Randomizer] Twitchers`, and
also come into survival (`[Survival] TwitcherPercent`); run `tools/build_survival_vents.py` after a
build so their vent keys follow.

## Notes

- Only enabled packs are built. Every built pack costs memory for the whole game, whether the
  INI lets it spawn or not; a skin copies the package's textures, materials and meshes.
- `drop_list` (own loot) and `skin.ghost` (another package's material) crashed in
  ItemDropListComponent (GoldenLeaper, GhostLeaper); `check` refuses them unless the pack sets
  `"allow_unstable": true`.
- `not_authored` keeps fields an older spec named that no record of the model carries; they
  never took effect.
- The build starts from `ds2_randomizer_pool.base.str`, the pool without variants. A pool that
  carries no variant refreshes that base first, so later pool stages (boss donors, chapter
  dependencies, field patches) are never lost.
- `variants/ds2_variants.json` is the old single-file spec; the build no longer reads it.
