# Editing a cutscene in Blender or UE5

A cutscene leaves the game as one glTF file with every actor and the camera, gets edited in Blender or UE5, and comes back as clips in banks the mod loads (variants/anims, see its README).

## 1. Export the scene

```
python -I tools/export_scene.py igc_03_03 C:/scenes
```

The scene names are in analysis/COOP_SCENE_LIST.md. This writes `igc_03_03.glb` and `igc_03_03.ds2scene.json`; keep both together.

- **Every actor:** Isaac, the NPCs, props with a rig (doors, the gunship, the shuttle). Each is a skeleton with its real bones and a thin proxy mesh, so both programs make a skeletal mesh of it.
- **Bone names:** recovered from their hashes where possible (100 of Isaac's 132). The rest are named hash_XXXXXXXX.
- **One animation per scene part** (010, 020, ...), with every actor's clip of that part. An action's longest clip is the part itself; its other leaves are `010#3` and so on.
- **Placement as in the game.**
  - Most clips (Isaac, the NPCs, the props, the gunship) are in level space.
  - A clip that is local to its sequence is put at that sequence's level matrix (in igc_03_03: the shuttle).
  - The whole scene is moved so that Isaac stands at the origin. The moves are in the sidecar, and the export back takes them out.
- **Two cameras:**
  - `ds2_camera` hangs on Isaac's m_camera bone, which is how the game drives it.
  - `ds2_camera_world` carries the same path as a free camera, easier to edit.
  - The FOV (horizontal, in degrees) is in the sidecar.
- **Units:** Y up, metres.

There are no real meshes or textures yet. The proxies show the skeletons; the DS2Import plugin can supply the real look in UE 5.8.3.

## 2a. Blender (tested headless with Blender 5.2)

```python
import sys; sys.path.append(r"C:/.../DeadSpace2Randomizer/tools/blender")
import ds2_scene
ds2_scene.import_scene("igc_03_03")   # from variants/build/scenes (or a full .glb path); 30 fps, FOV keys on both cameras
ds2_scene.show_part("010")                             # every actor and the cameras on part 010
```

- The import shows the scene's main part, the one most actors play (010 in igc_03_03). Nothing is selected; the importer's bone spheres and the dashed parent lines are off.
- Each actor shows as a figure made of thin spikes, one from every bone to each of its children. Links longer than 2.5 m are left out (a vehicle's parts).
- The skeletons are hidden; the figures move without them. `ds2_scene.bones(True)` shows the skeletons for posing and keying, and `bones(False)` hides them again.
- Each part is an action with one slot per actor. `show_part` puts a part on every actor that has it. Actors that are not in that part are hidden: their rest pose stands where their clip space begins, hundreds of metres off.
- Every run in Blender's Text editor starts with no names defined, while Blender keeps the old module cached. Put the import lines and `importlib.reload(ds2_scene)` on top of each script. The .blend keeps the loaded scene and any partner, so a reload, a new run or a reopened file goes on where it was.
- **The second player, one block for every file state** (empty file, scene already there, partner already there, older import):
  ```python
  import sys, importlib
  sys.path.append(r"C:/.../DeadSpace2Randomizer/tools/blender")
  import ds2_scene; importlib.reload(ds2_scene)
  ds2_scene.partner_setup("igc_03_03")   # imports the scene if missing, adds the partner if missing, shows and selects it
  ```
  Pose and key the partner, then run the same header with `ds2_scene.partner_export()`, which writes `variants/anims/partner/coop_partner_igc_03_03_player_010.json`.
  - The default partner carries Isaac's motion as a key on every frame, so one new key changes only that frame.
  - For a motion of its own, delete `partner` and `partner_proxy` and run `partner_setup("igc_03_03", motion="first_frame")`. The partner then holds Isaac's first pose and moves only where it is keyed (tested: a wrist key at frame 100 holds to the end, no other bone moves).
  Tested headless: a new file, the block twice, a saved and reopened file, and an older import with the root renamed `.001` all give the same clip.
- The single steps behind it:
  ```python
  ds2_scene.add_partner()        # Isaac's skeleton and figure again, 1 m beside him, starting from his motion
  ds2_scene.bones(True)          # show the skeletons; select "partner", Pose Mode, pose and key it
  ds2_scene.export_partner()     # -> variants/anims/partner/coop_partner_igc_03_03_player_010.json
  ```
  - `add_partner(offset=(x, y, z), motion="first_frame")` starts from Isaac's first pose instead of his whole motion.
  - Moving the partner object is fine: the move goes into its root (m_root), because Isaac's top bone `character` is never animated in the game.
  - Mocap: retarget onto the "partner" armature, then export the same way.
  - Tested headless: the partner exports with Isaac's motion plus the 1 m offset, and a keyed wrist arrives in the file.
- Export an actor's action:
  ```python
  ds2_scene.export_action("player", "010", r"C:/out/coop_partner_igc_03_03_player_010.json")
  ```
  Pass `bones=[...]` to export only part of the rig.
- Export a camera edit (only m_camera and the FOV):
  - `ds2_scene.export_camera("010", r"C:/out/igc_03_03_player_010.json", camera_name="ds2_camera_world")` for the free camera.
  - The same call without `camera_name` reads the bone camera.

The importer turns bones its own way; the export undoes that from the rest pose, so the result is the game's own local pose. Measured round trip on igc_03_03 (Isaac and Daina): positions within 0.0001 rig units, rotations and FOV exact.

## 2b. UE5 (5.1 or later, UE's own glTF import; not run here)

1. Import `igc_03_03.glb` into the Content Browser. Every actor becomes a skeletal mesh with its skeleton and one AnimSequence per part.
2. Put the meshes at the origin in a Level Sequence and give each its part. For the camera, attach a CineCameraActor to the player mesh's `m_camera` bone, turned 180° about Y, or use the imported free camera if UE brings it.
3. Export an edited AnimSequence:
   ```python
   import sys
   sys.path.append(r"C:/.../DeadSpace2Randomizer/tools/ue5"); sys.path.append(r"C:/.../DeadSpace2Randomizer/tools")
   import ds2_export_anim
   ds2_export_anim.export_scene_anim("/Game/igc_03_03/player_Anim_010", r"C:/out/partner.json",
                                     r"C:/scenes/igc_03_03.ds2scene.json", "player")
   ```
   It reads UE's axis map from the skeleton's reference pose against the sidecar, whether UE converts every bone or only above the root (both tested with simulated imports in `tools/test_scene_export.py`).

## 3. Back into the game

- **A partner clip** for a scene part goes to `variants/anims/partner/coop_partner_<Isaac's clip>.json`, for example `coop_partner_igc_03_03_player_010.json`.
- **A changed clip of an actor or the camera** goes to `variants/anims/scenes/<bank>/<action>.json`, for example `scenes/igc_03_03_player/igc_03_03_player_010.json`.

Then run `python -I tools/variant_pack.py build --write` and install the pool together with `variants/build/anims/ds2coop_partner_paths.bin`, which goes to `randomizer\`.

In the game ([ScenePartner] Enabled=1):
- A scene part with its own partner clip is a puppet scene by itself.
- The partner's clip starts when Isaac's clip of that part starts.
- Where the partner stands is set by `SyncedPlacement`:
  - `path` (the default) moves it along its clip's root path.
  - Use `origin` if it walks twice as far or stands far off.

Whether a ghost keeps the clip's root motion in its pose is the open question for the first test.

Not yet possible: extra actors in a scene, and moving the scene's events in time (the gunship coming earlier). Both need edits in the level data.
