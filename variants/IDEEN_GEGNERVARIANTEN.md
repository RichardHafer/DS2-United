# Ideen für Gegnervarianten (Dead Space 2), Stand 13.09.2026

Quelle: die Attributklassen der Xbox-PDB (`Fable/pdb_enemy_attributes.txt`, 1006
verschiedene `CMD_Set…`-Felder) plus das, was der Baukasten heute kann.

Was der Baukasten heute kann (alles per `variants/ds2_variants.json`):
- jeden Zahlenwert eines Datensatzes ändern (Klasse.Feld, z. B. `Zombie.CMD_SetChargeDamage`)
- Tempo und Größe zur Laufzeit (AnimView-Hook)
- eigene Texturen (Skin-Kopie, DDS-Workflow), eigene Lichtfarbe eines Effekts
- eigene Charakterkopie mit Sound-Tausch (Ereignis → anderer Klang aus jeder Bank)
- bald: eigene Klänge (XAS1-Encoder liegt vor)
- Spawn-Chance je Variante, Verteilung über Seed

Noch nicht, aber klein: 16-Byte-GUID-Felder setzen (heute nur 32-Bit-Werte). Damit
gehen die "Was kommt raus"-Ideen unten (Pregnant.SpawnTarget, Infector.SpawnedNPC,
Projectile-Felder). Eine Erweiterung von `apply_value` um GUIDs, ein Nachmittag.

---

## A. Sofort baubar (nur Zahlen, Tempo, Größe, Skin, Sound)

**Slasher**
- *Berserker*: `Zombie.MaxBerserkAttackCount` hoch, `ChargeStartChance` 1.0,
  `ChargeDamage` doppelt, `StasisResistance` 0.8, Skin rot-schwarz. Rennt immer.
- *Zäher Alter*: `DismemberableHealthComponent.CMD_SetRightBackHealth/LeftBackHealth`
  hoch (Arme kaum abzuschießen), dafür `HeadHealth` klein und `KilledZones` = Kopf.
  Der Gegner, den man nur per Kopfschuss loswird.
- *Glaskanone*: Health halbiert, jeder Schaden doppelt, Tempo 1.6, Größe 0.85.
- *Totsteller Deluxe*: `PlayDeadChance` 1.0 plus `AwareAtSpawn` 0: liegt immer,
  springt erst auf, wenn man vorbei ist.
- *Ausweicher*: `ThreatenedDodgeChance` 1.0, `StandbyDodgeChance` 1.0,
  `DodgeMinLoopTime` klein: tanzt um Schüsse herum.
- *Greifer*: `GrappleChance` 1.0, `BoostGrappleChanceTargetHealthThreshold` hoch:
  jeder Kontakt wird ein Ringkampf.

**Exploder**
- (haben wir) Super Exploder: dunkel, roter Sack, Hunter-Stimme.
- *Kamikaze*: `ExplodeRange` groß, Tempo 1.8, Health winzig. Sprintet und zündet früh.
- *Zeitbombe*: `ExplodeRange` fast 0, dafür `SacHealth` sehr hoch: er muss dich
  beißen (`BiteDamage`), der Sack geht kaum kaputt.
- *Brute-Sack*: `ExplosionDamageMod` hoch: die Explosion reißt alles um ihn mit.

**Lurker (Baby)**
- *MG-Lurker*: `FiringCooldown` klein, `MinFiringHoldTime` klein, `FiringSpread` groß.
- *Scharfschütze*: `MaxFiringRange` riesig, `FiringSpread` 0, `FiringCooldown` lang.
- *Klettermaxe*: `WallwalkingSwitchTime` klein, `TurnRate` hoch, Größe 1.4.

**Leaper**
- *Frosch*: `WWJumpMinDistance/MaxDistance` groß, `WWMaxConsecJumps` hoch,
  `UninjuredJumpingDamage` hoch: springt quer durch den Raum.
- *Schwanzlos-aber-wütend*: `MissingTailDamage` hoch (wird stärker, wenn man ihn kürzt).

**Pregnant**
- *Brutsack*: `SpawnNumber` 20 (Schwarm aus einem Bauch), Health klein.
- *Leere Hülle*: `SpawnNumber` 0, dafür `OverheadStrikeDamage` dreifach, Größe 1.2.

**Puker**
- *Dauerspucker*: `StandupVomitChance` 1.0, `StandupVomitAttemptRange` groß,
  `CrawlVomitAttemptRange` groß: spuckt auch kriechend über den halben Raum.
- *Kopfloser*: `HeadHealth` 1 (Kopf fliegt beim ersten Treffer),
  `HeadlessVomitChance` 1.0: der kopflose Modus ist der Normalzustand.

**Pack (Kinder)**
- *Rudel-Sprinter*: `PackRunJumpAttackChance` 1.0, `PackAttackDelay` 0,
  Tempo 1.5, Größe 0.8. Sie warten nicht mehr ab.
- *Riesenkind*: Größe 1.6, `PackJumpAttackDamage` hoch, `GrappleChance` 1.0.

**Stalker**
- *Schattenläufer*: Tempo 1.7, Skin fast schwarz, `AwarenessComponent.PeripheralRange`
  groß (sieht dich früher), `ThreatDecayRate` klein (vergisst dich nie).

**Brute**
- *Sturmbock*: `ChargingDamage` hoch, `MaxChargingTime` lang, `MinStandoffTimeToCharge`
  0: lädt sofort und immer. `ChargeStasisThreshold` hoch: Stasis bremst ihn kaum.
- *Werfer*: `FiringChance` 1.0, `FiringArc` weit, `MinProjectileSpeed` hoch: wirft
  ständig, kommt selten nah. Plus `SecondStageHealth` hoch (langer Endkampf).
- *Papier-Brute*: `PairedKillArmHealthThreshold` und Armgesundheit klein, Tempo 1.4,
  Größe 0.8: ein flinker, zerbrechlicher Brute.

**Divider**
- *Eile-Teile*: `DividerPart.RoamMinDuration/MaxDuration` klein, `TackleDamage` hoch,
  `FleeMinDuration` 0: die Teile rennen sofort an.
- *Zungenfreund*: `DividerCore.TongueAttemptChance` 1.0, `TongueGrappleDistance` groß.

**Schwarm**
- *Sauger*: `SwarmEnemy.DamageAmount` hoch, `DamageFrequency` klein, `DieOnDetach` 0
  (überlebt das Abschütteln), `MaxJumpLinearSpeed` hoch.

**Infector**
- *Seuchenherd*: `MaxInfections` hoch, `InfectionResetTime` klein, `Suicidal` 0:
  er bleibt und macht Leichen laufend zu Slashern.

**Tripod**
- *Artillerie*: `ProjectilesPerBurst` hoch, `TimeBetweenProjectileFire` klein,
  `FireProjectileChance` 1.0, `MinAttackDistance` groß.
- *Rammbock (Schwerelos)*: `ZGMidAirChargeChance` 1.0, `ZGChargeSpeed` hoch,
  `ZGMidAirChargeDamage` hoch.

**Alle**
- *Unsichtbarer Schmerz*: `PainThreshold` sehr hoch, alle `…PainInterruptTimeDelay`
  0: kein Zucken mehr bei Treffern, nur Abtrennen stoppt ihn.
- *Stasis-immun*: `StasisResistance` 1.0 (Stasis prallt ab), dafür Tempo 0.9.
- *Schnellschacht*: `DSNPCVentComponent.VentMovementSpeed` hoch, `AIVentReuseLatency`
  klein: nutzt ständig die Schächte und kommt von überall.
- *Weichling*: `WeakSpotDamageZone` + `WeakSpotDamageMultiplier` hoch: ein Körperteil
  ist die Achillesferse (Kopf, Sack, Schwanz), der Rest hält viel aus.

## B. Mit der kleinen GUID-Erweiterung (Referenzfelder setzen)

- *Pregnant mit Exploder-Füllung*: `Pregnant.SpawnTarget` auf einen Exploder-
  (oder Lurker-, Pack-) Datensatz, `SpawnNumber` 3. Aufschneiden = drei Exploder.
- *Infector, der Pukers macht*: `Infector.SpawnedNPC` auf einen anderen Datensatz.
  Alles, was auch aus Leichen aufsteht, sollte gehen (Slasher-Familie sicher, andere
  Familien: Animation "Aufstehen" muss existieren, sonst T-Pose oder Absturz).
- *Slasher mit Fernangriff*: `CadaverousNPC.RangedAttackChance` > 0 plus
  `RangedAttackProjectile` (die Spitter-Slasher haben genau diese Felder befüllt,
  ein Slasher-Datensatz mit Spitter-Werten = Spitter in jedem Skin).
- *Lurker mit Brute-Geschoss*: `Lurker.Projectile` auf das Brute-Projektil.
- *Brute-Kinder*: `Brute.Projectile` auf etwas Verrücktes (Guardian-Pod-Geschoss?).
- *Divider mit Fremdteilen*: `DividerCore.HeadDividerPartRef` usw. auf andere
  Teile (fünf Köpfe statt Hände). Ob der Zusammenbau das mitmacht: Test.
- *Todesnachricht*: `HealthComponent.OnDeathMessage` ist frei, nur die Skript-
  Zählung braucht die alte (siehe Memory).

## C. Braucht Code im Plugin (Laufzeit-Hook), realistisch

- *Regenerator-Slasher*: `Hunter.RegenerationTimeout` gibt es nur beim Hunter. Für
  andere Familien müsste ein Hook die Zonen-Gesundheit nachfüllen. Machbar wie der
  Tempo-Hook: Zonenwerte am DismemberableHealthComponent periodisch zurücksetzen.
- *Wachsender Gegner*: Größe zur Laufzeit an die verbleibende Gesundheit koppeln
  (wird größer je mehr er einsteckt). Der Scale-Setter ist bekannt.
- *Raser bei Stasis*: Tempo-Hook invertieren, wenn Stasis anliegt (wir sitzen schon in
  jeder Tempo-Änderung, Stasis inklusive).
- *Leuchtende Augen / pulsierende Haut*: braucht ein Material mit Glow-Slot; das
  Körpermaterial hat keinen. Nur über ein eigenes Licht-VFX am Kopfknochen (VFX-Kopie
  mit anderer Farbe, wie das Sacklicht).
- *Rudel aus Varianten*: eine Variante, die beim Spawn zwei Begleiter nachzieht
  (MoreEnemies-Pfad mit fester Begleiterliste).

## D. Nicht realistisch / Sackgassen

- Neue Animationen oder neue Angriffsarten: Bänke sind gebaut, kein Editor.
- Größe mit passender Kollision (Kapsel bleibt 1.8): nur optisch.
- Necromorph-Spieler-Klasse (NecroPlayer) im Singleplayer: Multiplayer-Modul.

Vorschlag für die nächsten drei: Berserker-Slasher (A), Brutsack-Pregnant mit
Exploder-Füllung (B, nach der GUID-Erweiterung), Regenerator-Slasher (C).

---

## E. Nachschub vom 14.09.2026 (Lore-Fundgrube + Baukasten-Stand)

Gebaut seit dem 13.09.: Berserker/Juggernaut/Dodger/Glass/Grappler-Slasher, Kamikaze-Exploder,
Frosch-Leaper, Brutsack-Pregnant, Dauerspucker-Puker, Rudel-Sprinter, GhostLeaper (Dementia-Shader).
Neu im Baukasten: `"ghost"` (Material auf dem Verzerrungs-Shader des Geisterkinds), eindeutige
Bank-Kennungen, Werte folgen dem Stat. Quellen: Dead-Space-Wiki (Enhanced Necromorphs, DS3-Typen),
Rely on Horror (DS2-Gegnerliste).

**Aus der Serie geborgt (Lore-Varianten, sofort baubar)**
- *Enhanced* (DS1/DS2-Kanon): schwarze Haut, gelb leuchtende Augen, mehr Health, mehr Schaden. Wir
  haben sie schon halb (Juggernaut). Eine "Enhanced"-Familie ueber ALLE Familien mit einer Regel:
  Health x1.6, Schaden x1.4, Skin dunkel + Augen-VFX (Lichtfarbe gelb am Kopfknochen, wie das
  Sacklicht des Exploders). Als Seed-Regel "ab Kapitel 10 nur noch Enhanced" per INI.
- *Twitcher* (DS1, Militaerversion mit Stasis-Modul im Koerper): Slasher mit Tempo 2.2, ruckelnde
  Bewegung = Animationsrate hoch, Stasis-Resistenz 1.0, dafuer Health x0.7. Skin: Soldaten-Slasher
  (slashersecurity_cct passt perfekt als Modell).
- *Wheezer*/*Gasser* (DS1): ein Puker, der nicht spuckt, sondern beim Tod eine Giftwolke laesst.
  Umsetzung: `HeadlessVomitChance` 0, `StandupVomitChance` 0, Tod = Explosion-Variante ohne Kraft
  (`ExplosionDamageMod` klein, Radius gross). Braucht Exploder-Sack-Logik am Puker: Hook-Idee C.
- *Feeder* (DS3): blinde, schnelle, kleine Schwarm-Slasher, die nur auf Geraeusche reagieren.
  `AwarenessComponent` Sichtkegel 0, Hoerradius gross, Groesse 0.8, Tempo 1.6, Health x0.4,
  immer im Rudel (MoreEnemies-Pfad mit fester Begleiterzahl 3). Skin sehr hell/ausgeblichen.
- *Spitter* (DS3): Slasher mit Fernangriff. `CadaverousNPC.RangedAttackChance` + Projektil-GUID
  (Idee B), die Spitter-Slasher tragen die Werte schon. Slasher in jedem Skin, der spuckt.
- *Waster* (DS3): Slasher, dem nach dem Abtrennen der Beine Tentakel wachsen = zweite Phase.
  Bei uns: `MissingLeg*`-Schaden hoch, `Crawl*` Tempo hoch, Kriechphase gefaehrlicher als die erste.
- *Snow Beast light*: Brute-Aussehen mit Regeneration; braucht Hook C (Zonen-Health nachfuellen).
- *Tormentor-Rudiment*: gibt es nur als Skript-Boss, kein Datensatz zum Kopieren. Sackgasse.

**Geisterwelt (nach dem GhostLeaper)**
- *Geister-Familie*: derselbe Shader fuer Slasher, Puker, Pack, Exploder; dazu per Hook das Aufloesen
  beim Tod (Dementia-Todeseffekte `dl_pack_death_*` am Koerper starten, Leiche nach der Sterbe-
  animation entfernen). Gliedmassen-Health hoch, damit nichts abfaellt und herumliegt.
- *Phantom*: Geist-Shader, der nur beim Angriff sichtbar wird: Noise-Intensitaet im Material hoch
  (fast unsichtbar), `AwareAtSpawn` 0, `PlayDead` 0. Sichtbarkeit zur Laufzeit an Distanz koppeln
  (Materialparameter schreiben, Hook-Idee wie Tempo).
- *Marker-Fluesterer*: Geist-Pack-Boy mit Sounds der Nicole-Halluzinationen (SBK-Spender aus dem
  Nicole-Paket, Ereignis-Tausch wie beim Super Exploder).

**Verhaltens-Umkehrungen (nur Zahlen)**
- *Feigling*: `FearComponent` hoch, flieht bei Beschuss, kommt zurueck wenn man nachlaedt.
  Ein Slasher, der Ecken nutzt. (Fear-Felder stehen im PDB, Namen pruefen.)
- *Sniper-Puker*: `StandupVomitMinAttemptRange` gross = spuckt NUR aus der Ferne, Nahkampf 0.
- *Kamikaze-Pack*: Pack-Kinder mit `GrappleChance` 1.0 und `ForcedPairedKillPlayerHealthThreshold`
  hoch: jedes Kind versucht den Paired-Kill. Gruselig, kurz, fair mit Tempo 0.9.
- *Schlafender Exploder*: `AwareAtSpawn` 0 + `PlayDead` 1.0 fuer Exploder: liegt zwischen Leichen,
  Sack leuchtet aber (VFX Licht an) = der Spieler kann ihn finden, wenn er hinschaut.
- *Ping-Pong-Leaper*: `WWMaxConsecJumps` hoch, `PauseMax` 0, Sprungschaden 0.5: nervt, toetet kaum.
- *Panzer-Pregnant*: Bauch-Health sehr hoch, Gliedmassen normal: man MUSS sie zerlegen, das
  Aufschneiden ist keine Option mehr.

**Ausstattung / Zusammensetzung (kleine Code-Schritte)**
- *Beutetraeger*: `ItemDropListGUID` auf eine Liste mit Power Node / Schaltplan (GUID-Idee B):
  eine seltene "goldene" Variante (Skin hell, Chance 3) die immer Beute faellt. Wie Diablo.
- *Grosse Bruder*: Pregnant mit `SpawnTarget` = Exploder (GUID-Idee B), SpawnNumber 3.
- *Infector-Alptraum*: Infector mit `SpawnedNPC` = Berserker-Variante (Infizieren erzeugt Varianten).
- *Rudelfuehrer*: eine Variante, die beim Spawn 2-3 normale Gegner ihrer Familie nachzieht
  (MoreEnemies-Pfad, feste Begleiterliste), selbst Enhanced.

**Laufzeit (Hook-Ideen, geordnet nach Aufwand)**
1. Aufloesen beim Tod (Geister-Familie): Tod-Hook vorhanden, Entity entfernen nach N ms, VFX starten.
2. Regeneration: Zonen-Health am DismemberableHealthComponent alle 2 s um X nachfuellen, nur wenn
   nicht in Stasis; Enhanced-Regenerator als seltene Variante.
3. Wachsen/Schrumpfen: Groesse an Rest-Health, Setter bekannt.
4. Phantom-Sichtbarkeit: Materialparameter (Noise-Intensitaet) an Distanz.
5. Raser bei Stasis: Tempo-Hook invertieren.

**Regeln statt Gegner (INI-Seiten der gleichen Muenze)**
- Kapitel-Kurve: Varianten-Chance steigt mit dem Kapitel (Kapitel ist im Stream-Namen bekannt).
- Boss-Rush-Seed: jede Familie ausschliesslich in ihrer staerksten Variante, Chance 100.
- Themen-Seeds: "Geisterwelt" (alle Ghost), "Enhanced Run", "Schnellrunde" (alle Tempo 1.4, Health 0.7).
