# Randomizer-Ideen jenseits der Gegner (Dead Space 2), 14.09.2026

Quelle: alle 348 Nicht-Gegner-Attributklassen der Xbox-PDB (`Fable/pdb_world_attributes.txt`,
frisch gezogen) plus das, was der Baukasten heute kann. Der Pool aendert heute nur
Gegner-Datensaetze; Weltobjekte leben in den Level-Streams und brauchen entweder den
Datensatz-Weg (Kopie in den Pool, Verweis umbiegen) oder einen Laufzeit-Hook am Leser.

## 1. Beute und Wirtschaft (PickupItem, ItemDropList, PowerUp, StandardContainer, BreakableContainer)
- **Mengen-Wuerfel:** `PickupItem.CMD_SetUseRandomAmount/RandomAmountMin/Max` sind authored
  Felder: jede Munitions- und Credit-Aufnahme bekommt eine Seed-Spanne (mal 2, mal 40). Rein Zahlen.
- **Kisten-Roulette:** `StandardContainer.CMD_SetDropItemGUID` / `CMD_SetItemDropListGUID` und
  `BreakableContainer.CMD_SetDropItemGUID`: jede Kiste zieht aus einer anderen Liste. Mit der
  GUID-Erweiterung (heute nur 32-Bit-Werte) ein Nachmittag; die DS1-Kisten haben wir schon so gemacht.
- **Semiconductor-Lotterie:** `PowerUp.CMD_SetBonusCurrency/MinBonusCurrency/BonusUpgradeNodes/
  BonusHealth`: ein Halbleiter, der statt Credits einen Power Node gibt, oder Gesundheit. Rein Zahlen.
- **Mystery-Schaltplan:** `Schematic.CMD_SetPickupItemGUID` auf einen anderen Plan = die Werkstatt
  bekommt jedes Mal etwas anderes (Munitionsplan wird zum Anzugplan). GUID-Erweiterung.
- **Laden mit Laune:** `ShipStore` hat 86 Felder, davon aber vor allem Kameras und VFX; die Preise
  liegen in `GlobalItemStoreList`. Preis-Seed (jeder Artikel 50..200 %) = ein Feld pro Artikel.
- **Muni-Gattungstausch:** `SpareAmmo.CMD_SetAmmoType` + `MultiAmmo.*Quant*`: das Munitionspaket
  vor dir ist fuer eine zufaellige Waffe (die du vielleicht nicht hast). Zahlen + Verweis.

## 2. Spieler und Anzug (PlayerSettings 240 Felder, Uniform, PlayerAir, Gun)
- **Schwierigkeit pro Kapitel:** `PlayerSettings.CMD_Set*DamageTaken/DamageDealt/AmmoDifficultyScale`
  sind je Schwierigkeitsgrad authored: der Seed mischt sie pro Kapitel neu (Kapitel 5 "Impossible",
  Kapitel 6 "Easy"). Wir hooken die Schwierigkeit schon (`difficulty_changed`).
- **Unverwundbarkeits-Fenster:** `CMD_SetInvulnerableTimeAfterDamage`: 0 = alles trifft sofort
  (Hardcore-Plus), 3 s = Dark-Souls-Iframes. Zahl.
- **Anzug-Roulette:** `Uniform.CMD_SetUniformInventorySize/UniformDamageCoeff`: der Anzug, den du
  kaufst, hat zufaellig 10..25 Faecher und 0.6..1.2 Schutz. Zahlen. Suit-Changer haben wir fuer DS1.
- **Luft-Seed:** `PlayerAir.CMD_SetMaxAir/AirRefillTime`: jeder Vakuumabschnitt hat andere Luft.
- **Waffen-Chaos:** `Gun.CMD_SetSpread/Recoil*/AmmoUsage/NumAttacksPerShot/FireProjectile`:
  ein Plasma Cutter, der zwei Strahlen pro Schuss feuert oder Javelin-Projektile (GUID). Die
  Werkbank-Werte sind Lua, die Waffen-Basis ist ein Datensatz: beides mischbar.
- **Kinesis-Seed:** `TKData` (nicht gedumpt) und `TKReceptacle`: Wurfkraft, Reichweite. Pruefen.

## 3. Welt und Physik (GravityField, ImpulseField, DrainField, ForceField, Mover, StandardDoor)
- **Schwerkraft-Wuerfel:** `GravityField.CMD_SetDefaultVector/DefaultState/GravTrap`: ein Raum,
  der zufaellig seitliche Schwerkraft hat, oder Schwerelosigkeit an Orten, wo keine war. Vektor =
  drei Floats; DefaultState = Schalter. Das wird verrueckt, weil Navigation und Gegner mitgehen.
- **Windkanal:** `ImpulseField.CMD_SetForceVector`: Schubfelder umdrehen = der Luftstrom im
  Solarpuzzle zieht statt zu schieben. Zahlen.
- **Tuer-Laune:** `StandardDoor.CMD_DoorStartLocked/DoorTimeScaleFactor/OneWayLocked`: Tueren
  oeffnen langsam (Faktor 0.3, Gegner kommen naeher) oder sind einseitig. Vorsicht:
  `StreamingCriticalDoor` und `KeyLockGUID` nie anfassen, sonst haengt der Stream.
- **Aufzug-Tempo:** `Mover.CMD_SetMoverOptions` + Travel-Sounds: Aufzuege doppelt so langsam mit
  Gegnern drin. Zahlen, aber der Schacht-Stream muss lange genug offen sein (bekanntes Thema).
- **Fallen-Seed:** `EnvironmentalHazard.CMD_SetDamage/Frequency/Range/FOV/Dismember` (Dampf,
  Laser, Ventilatoren): jede Falle hat andere Reichweite und ob sie zerteilt. `TriggerHurt`
  dito. `Landmine` (41 Felder: Projektil, Reichweite, Trigger-Radius): Minen, die schiessen.
- **Zerstoerbare Fenster:** `Destructible.CMD_IsDecompressionWindow`: ein Fenster, das keins war,
  wird eins. Riskant, weil der Dekompressions-Ablauf ein Skript ist; nur bei authored Fenstern.

## 4. Licht, Kamera, Stimmung (EARSLight, AnimateLight, DrunkCameraModifierInfo, ShaderTintComponent, DynamicDistortionComponent)
- **Licht-Seed:** `EARSLight.CMD_Set_t_color/intensity/radius`: Raeume in Rot, flackernd
  (`AnimateLight.CMD_IntensityJitter/Frequency/Function`). Tausende Lichter, alle Datensaetze.
  Ein "Blackout-Seed": jedes dritte Licht aus, Taschenlampe wird Pflicht.
- **Halluzinations-Kamera:** `DrunkCameraModifierInfo.CMD_SetPitch/Yaw/RollAmplitude/Frequency`:
  die Kamera schwankt, wenn ein Marker-Gegner nah ist. Laufzeit-Hook am Kamera-Modifier.
- **Farbstich pro Gegner:** `ShaderTintComponent.CMD_SetColor` gibt es als Komponente: Gegner mit
  Tint OHNE Texturarbeit, wenn der Datensatz die Komponente hat. Pruefen, welche Klassen sie tragen.
- **Verzerrung am Knochen:** `DynamicDistortionComponent.CMD_SetDistortionVFX/Bone`: Hitzeflimmern
  um einen Gegner; passt zur Geister-Familie.
- **Stasis-Werfer in der Welt:** `StasisCaster` (Reichweite, Radius, Schaden, Feuerrate): die
  Stasis-Stationen der Level koennten auf DICH schiessen (SourceEntity/TargetEntity tauschen).

## 5. Text, Ton, Meta (RASAnnouncement, TrainingTip, UIPopup, MusicEvent, VidScreen, SaveKiosk)
- **RIG-Durchsagen:** `RASAnnouncement.CMD_SetText`: die Sprawl-Ansagen bekommen eigene Texte
  ("Necromorph-Variante gesichtet"). Textfeld = Hash in die Sprachtabelle; eigene Strings brauchen
  eine Texttabelle. Machbar, weil `text_assets` ein loser Stream ist wie unser Pool.
- **Musik-Wuerfel:** `MusicEvent.CMD_SendEvent`: Kampfmusik in Ruheraeumen, Stille im Kampf.
- **Videowaende:** `VidScreen.CMD_SetVideoFile`: der Werbebildschirm zeigt ein anderes Video.
- **Speicherkiosk mit Bedingung:** `SaveKiosk.CMD_SetMinHealth`: speichern nur ueber X Gesundheit.
  Gemein, klein.
- **Kapitel-Ende-Seed:** `Checkpoint.CMD_SetDeathRestartEvent`: beim Tod woanders weitermachen.
  Nur mit Verstand: Stream-Sets muessen passen.

## 6. Verrueckt, aber im Baukasten
- **NecroPlayer:** Spieler als Necromorph gibt es als Klasse (Multiplayer). Singleplayer-Nutzung
  ist die Sackgasse von 13.09. (bleibt).
- **Nicole als Begleiter:** `NeutralNPC` + `FlockingComponent`: ein neutraler NPC, der dir folgt.
  Die Nicole-Halluzination ist ein NeutralNPC-Datensatz; mit Flocking auf den Spieler = Begleitung.
- **Tentakel-Fallen:** `DragTentacle/StabTentacle/SlamDragTentacle` sind Datensaetze mit
  Spawn-Ort; ein Drag-Tentakel-Datensatz in einen Flur kopiert (Pool-Weg wie die Gegner).
- **Guardian im Gang:** `Guardian` + `GuardianPod` als Pool-Spender an Schacht-Quellen: die
  Wand-Necromorphs tauchen dort auf, wo Slasher waren. Familienliste der Spawner beachten.
- **Solarpuzzle-Seed:** `SolarPuzzle` hat Felder; die Loesung koennte pro Seed anders liegen.
- **Hacking-Minispiel:** `HackingStationMiniGame` (71 Felder, Timings): schneller, mehr Segmente.

## Reihenfolge nach Ertrag pro Aufwand
1. Beute-Mengen (PickupItem) und Semiconductor-Lotterie: nur Zahlen, sofort sichtbar.
2. Fallen- und Licht-Seed: nur Zahlen, viel Atmosphaere.
3. GUID-Erweiterung (ein Nachmittag) -> Kisten, Schaltplaene, Pregnant-Fuellung, Spitter.
4. Schwierigkeit pro Kapitel + Unverwundbarkeits-Fenster: Hook existiert schon.
5. Schwerkraft-Wuerfel: wenig Code, viel Wahnsinn, muss getestet werden.

## 7. Nachtrag 15.09.2026: was andere Randomizer bieten, und was davon bei uns geht

Vorbild BioRand (Resident Evil): Tueren/Schluessel-Logik, Item-Positionen, Startinventar,
Gegner, NPC-Stimmen, Musik, Zwischensequenzen, Seed-Sharing. Vorbild RE4-Randomizer:
Haendler-Sortiment, Startinventar, Gegner-HP, Upgrades, Anzahl je Raum, Setting-Profile.

Machbar mit dem, was wir schon in der Hand haben (Hooks/Offsets belegt):
- **Startinventar-Seed** (GunProxy/PlayerStore beim Neustart): andere Startwaffe statt Cutter.
- **Laden-Seed** (ItemStoreList, Schematic-Freischaltung sitzt schon in unserer Hand):
  Sortiment und Preise pro Seed; "Mystery-Slot" mit Zufallsware.
- **Gegner-HP/Schaden-Seed** (PlayerSettings DamageTaken/DamageDealt, Health-Attribute):
  pro Kapitel andere Haerte, im Log ansagbar.
- **Kill-Quoten-Seed** (FuncSpawn MaxSpawn/PopThresh): Raeume spawnen mehr Wellen (wie
  MoreEnemies, aber authored-treu).
- **Todes-Neustart woanders** (Checkpoint.DeathRestartEvent): Roguelike-Modus.
- **Fallen-Seed** (TriggerHurt Damage/Knockback, EnvironmentalHazard): Dampf, Feuer,
  Strom anders verteilt.
- **Musik/Ansagen-Seed** (MusicEvent, RASAnnouncement): Stimmung tauschen.
- **Kisten-Roulette** (StandardContainer.DropItemGUID): jede Kiste anders.
- **Seed-Karte im Log** (schon fast da): welche Waffe wo, welcher Anzug wo, teilbar.
- **Setting-Profile** (INI-Presets: Casual / Chaos / Streamer) und ein Seed-Hash im Log,
  damit zwei Spieler denselben Lauf haben (wir haben es: Seed + Katalog-SHA).

Erst wenn die Varianten stabil sind: Skins/Tempo/Groesse als "Elite"-Layer obendrauf.
