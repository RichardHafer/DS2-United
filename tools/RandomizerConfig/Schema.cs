// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Linq;

namespace DS2CoopConfig
{
    // Keys, defaults and ranges come from the plugin source (src/observer.cpp and
    // src/runtime_*.inc, ConfigInt/GetPrivateProfileInt/Setting calls).
    // Recommended values come from the embedded release INI.
    sealed class TableInfo
    {
        public string Section, Group, Title, Description, NoteText;
        public List<Option> Cells = new List<Option>();
        // shown where its first cell sits among the switches instead of after all of them
        public bool InPlace;
    }

    static class Schema
    {
        public const string General = "Randomizer";
        public const string Enemies = "Enemies & Families";
        public const string More = "More Enemies";
        public const string Loot = "Loot";
        public const string Schematics = "Schematics & Bench";
        public const string Loading = "Loading & Performance";
        public const string Watchdog = "Watchdog & Advanced";
        public const string Diagnostics = "Diagnostics";
        public const string Expert = "Expert";

        public static readonly string[] Groups = { Curated.Coop, Curated.Bots, Curated.Survival, Curated.Multiplayer, Curated.Extras, Curated.Speed,
            General, Enemies, More, Loot, Schematics, Loading, Watchdog, Diagnostics, Expert };

        // the side bar draws a divider after these entries: play, randomizer, technical
        public static readonly int[] GroupBreaks = { 5, 10, 13 };

        public static readonly Dictionary<string, string> GroupIntro = new Dictionary<string, string>
        {
            { Curated.Coop, "Online co-op between several copies of the game. Most switches have to match on every machine; the host decides levels, checkpoints and enemies." },
            { Curated.Bots, "Companion bots: up to three extra Isaacs the game itself runs. They follow you, fight, loot, shop and heal. Each bot's personality is at the end of this page." },
            { Curated.Survival, "Survival waves on the multiplayer maps and in the campaign arenas, and the clock modes Mercenaries, Objective and Hold the Zone. The waves themselves ([Wave1] to [Wave10]) are on the Expert page." },
            { Curated.Multiplayer, "Necromorph players in story co-op, the pre-game lobby, the multiplayer character setup and the multiplayer map visit. Experimental." },
            { Curated.Speed, "How fast each kind of enemy is, for the story and every game mode on its own." },
            { Curated.Extras, "How it looks and what you carry: colours, menus, the turret and gadgets, voice packs, Dementia, the difficulty question and the debug menu." },
            { Expert, "Every other key in DS2United.ini, grouped by section, with the note from its template. Most of these are tuning values, experiments and diagnostics: change one only when you know what the line does. A key marked \"not in file\" uses the plugin's built-in value until you add it." },
            { General, "The enemy randomizer's main switches: it replaces the campaign's enemies by seed. Restart the game after saving: the plugin reads this file only at startup. Back up your saves." },
            { Enemies, "Which enemies may be replaced, what may replace them, and how often each kind is picked." },
            { More, "More enemies per spawn point, and hordes. These cost performance; the defaults are tested." },
            { Loot, "Ammo and credits in pickups." },
            { Schematics, "Shuffles store schematics and bench upgrade nodes. Seeds here must never change mid-playthrough." },
            { Loading, "The enemy pool and loading screens, frame rate and engine limits. Mostly leave as is." },
            { Watchdog, "The watchdog looks after replacements that got stuck, so doors and quarantines still open. The advanced switches below it are safety fixes and experiments: leave them as they are." },
            { Diagnostics, "Only for bug reports. These make plugins\\DS2Randomizer.log bigger. The release ships with spawn logging on so a log sent in is always useful." },
        };

        // Family lists as the INI files carry them. Kinds without enemy catalog
        // records (HiddenFamilies) are left out: their lines do nothing, and
        // saving keeps them as they are.
        static readonly string[] AllFamilies = {
            "Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Brute",
            "Infector", "Stalker", "Hunter", "Guardian", "GuardianPod", "GuardianTentacle", "Nest", "NestProjectile",
            "Landmine", "DividerCore", "DividerPart", "SwarmEnemy", "Tripod", "WindowTripod", "BossTripod",
            "TripodProjectile", "Kong", "NicoleBoss", "DragTentacle" };
        public static readonly string[] HiddenFamilies = { "Polyp", "LeviathanTentacle", "SlamDragTentacle", "SlugTentacle", "StabTentacle" };
        static readonly string[] WeightFamilies = {
            "Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Infector",
            "Stalker", "SwarmEnemy", "Brute", "DividerCore", "DividerPart", "Hunter" };
        // catalog rows marked extended (generated_enemy_catalog.h)
        static readonly string[] ExperimentalFamilies = { "Infector", "Stalker", "SwarmEnemy", "DividerCore", "DividerPart", "Hunter" };
        // the ordinary kinds; the rest are held at one / refused in code
        static readonly string[] OrdinaryFamilies = {
            "Zombie", "PackEnemy", "TubeBeetle", "Leaper", "Lurker", "Puker", "Exploder", "Pregnant", "Infector",
            "Stalker", "SwarmEnemy", "Brute" };

        public static string FamilyLabel(string family)
        {
            switch (family)
            {
                case "Zombie": return "Zombie (Slasher)";
                case "TubeBeetle": return "TubeBeetle (Crawler)";
                case "Hunter": return "Hunter (Ubermorph)";
                case "SwarmEnemy": return "SwarmEnemy (Swarmer)";
                case "PackEnemy": return "PackEnemy (Pack)";
                case "DividerCore": return "DividerCore (Divider)";
                default: return family;
            }
        }

        const string Main = "DS2Randomizer";

        static Option Make(string group, string section, string key, Kind kind, long def, long min, long max, string title, string desc)
        {
            return new Option
            {
                Group = group, Section = section, Key = key, Kind = kind, Min = min, Max = max,
                CodeDefault = kind == Kind.OptionalInt ? "" : def.ToString(), Title = title, Description = desc
            };
        }

        static Option B(string group, string section, string key, int def, string title, string desc)
        {
            return Make(group, section, key, Kind.Bool, def, 0, 1, title, desc);
        }

        static Option I(string group, string section, string key, long def, long min, long max, string title, string desc)
        {
            return Make(group, section, key, Kind.Int, def, min, max, title, desc);
        }

        static Option T(string group, string section, string key, string title, string desc)
        {
            var o = Make(group, section, key, Kind.Text, 0, 0, 0, title, desc);
            o.CodeDefault = "";
            return o;
        }

        static Func<Model, string> WhenOff(string id, string text) { return m => m.On(id) ? null : text; }
        static Func<Model, string> WhenOn(string id, string text) { return m => m.On(id) ? text : null; }

        static string M(string key) { return Main + "." + key; }

        public static List<TableInfo> Tables = new List<TableInfo>();

        public static List<Option> Build()
        {
            Tables = new List<TableInfo>();
            var list = new List<Option>();
            Action<Option> add = list.Add;

            // ---- General
            add(B(General, Main, "Enabled", 0, "Randomizer on",
                "Master switch for the campaign. Off leaves the campaign's enemies, loot, schematics and bench as the game has them, without uninstalling anything. The frame cap and the larger pathfinding memory stay active.").Raw()
                .Warn(m => m.On(M("Enabled")) ? null : MasterOff(m)));
            add(I(General, Main, "Seed", 1, 0, 4294967295L, "Seed",
                "Decides which enemy replaces which. The same seed always gives the same game. The schematic and bench randomizers use it too unless they have their own seed.")
                .Keep()
                .Note("Never change the seed in the middle of a playthrough: bench nodes you already bought and schematics you found would give something else.")
                .Warn(m => m.IsChanged(m.ById[M("Seed")]) ? "Seed changed. Only do this when you start a new playthrough." : null));
            list.Last().Unsigned = true;
            add(I(General, Main, "Chance", 100, 0, 100, "Replacement chance (%)",
                "Percent of eligible spawns that get replaced. 100 = all, 50 = half, 0 = none.")
                .Warn(m => m.Int(M("Chance")) == 0 ? "0 means no enemy gets replaced. More enemies still works on its own." : null));
            add(B(General, Main, "CrossFamily", 0, "Mix enemy kinds",
                "On: a Slasher can become a Puker, a Leaper a Pack, and so on. Off: enemies only swap with other looks of their own kind.").Raw()
                .Note("On in the release on purpose: this is what makes the randomizer feel random.")
                .Warn(WhenOff(M("CrossFamily"), "Off makes the game much tamer: enemies only swap with other looks of their own kind.")));

            // ---- Enemies & Families
            add(B(Enemies, Main, "ScriptedArrivals", 0, "Replace scripted spawns",
                "Enemies that arrive through a scripted animation (doors, ambushes) and the play-dead ones may be replaced too. The replacement must match the original's spawn setup exactly and stays the same kind.")
                .Note("On in the release: without it many set-piece spawns keep their original enemy.")
                .Warn(WhenOff(M("ScriptedArrivals"), "Off makes it tamer: scripted ambushes keep their original enemies.")));
            add(B(Enemies, Main, "InfectedArrivals", 0, "Replace infected corpses",
                "Corpses that an Infector turns into enemies may be replaced too. Same rules as scripted spawns.")
                .Note("On in the release for the same reason as scripted spawns.")
                .Warn(WhenOff(M("InfectedArrivals"), "Off makes it tamer: infected corpses always become their original enemy.")));
            add(B(Enemies, Main, "Twitchers", 1, "Severed enemies",
                "Twitchers (fast, dodging slashers) and the Severed pregnant can show up in the campaign. The two chances below say how often."));
            add(I(Enemies, Main, "TwitcherChance", 15, 0, 100, "Twitcher chance (%)",
                "Share of campaign slashers that become a twitcher when they are not replaced otherwise.")
                .Warn(m => m.Int(M("TwitcherChance")) > 0 && !m.On(M("Twitchers")) ? "Does nothing while Severed enemies is off." : null));
            add(I(Enemies, Main, "SeveredPregnantChance", 15, 0, 100, "Severed pregnant chance (%)",
                "Share of campaign pregnants that become the Severed pregnant when they are not replaced otherwise.")
                .Warn(m => m.Int(M("SeveredPregnantChance")) > 0 && !m.On(M("Twitchers")) ? "Does nothing while Severed enemies is off." : null));
            add(B(Enemies, Main, "DirectDonors", 0, "Direct donors",
                "Takes the pool's pack boy template that never attacks out of play. Its hand-made stand-in is withdrawn in this build, so this only removes the broken template. Needs the cross-level enemy pool.")
                .Warn(m => m.On(M("DirectDonors")) && !m.On(M("GlobalTemplates")) ? "Does nothing while the cross-level enemy pool (GlobalTemplates) is off." : null));
            add(B(Enemies, Main, "VentVariants", 0, "Vent spawns: same kind",
                "Vent spawns may become other looks of the same kind."));
            add(B(Enemies, Main, "CrossFamilyVents", 0, "Vent spawns: other kinds",
                "Vent spawns may become other kinds too. Needs Mix enemy kinds and Vent spawns: same kind.")
                .Warn(m => m.On(M("CrossFamilyVents")) && (!m.On(M("CrossFamily")) || !m.On(M("VentVariants")))
                    ? "Does nothing while Mix enemy kinds (CrossFamily) or Vent spawns: same kind (VentVariants) is off." : null));
            add(B(Enemies, Main, "VentRebinding", 0, "Vent rebinding",
                "Lets a replacement use the vent the level placed, as long as it is a vent enemy itself. In the campaign, together with Vent spawns: same kind, many more vent spawns can be replaced this way. Survival needs it on its own for every enemy that comes out of a vent.")
                .Warn(m =>
                {
                    bool survivalVents = m.On("Survival.Enabled") && m.On("Survival.VentSpawns");
                    if (!m.On(M("VentRebinding")))
                        return survivalVents ? "Off: no survival enemy comes out of a vent, all of them arrive on foot. Turn it on." : null;
                    return !m.On(M("VentVariants")) && !survivalVents
                        ? "Does nothing in the campaign while Vent spawns: same kind (VentVariants) is off; turn that on for campaign vents. Survival uses it either way." : null;
                }));
            add(B(Enemies, Main, "VentMessageContract", 0, "Relaxed vent matching",
                "Lets two vent spawns match even when only their spawn-done messages differ. Only works together with Vent rebinding.")
                .Warn(WhenOn(M("VentMessageContract"), "Experimental and off in the release.")));
            add(I(Enemies, Main, "MaxDonorLookups", 8, 1, 16, "Candidates per spawn",
                "How many replacement candidates are tried for one spawn before the original enemy is kept."));
            add(B(Enemies, Main, "SpawnerTools", 0, "Spawner rules",
                "Reads plugins\\DS2SpawnerRules.tsv (per-spawner block, allow or family list) and keeps the recent-spawn list for the Spawner Editor. Extra spawns bound to a spawner need it too.")
                .Warn(WhenOff(M("SpawnerTools"), "Off: your rules in DS2SpawnerRules.tsv are ignored.")));

            var weights = Table(list, Enemies, "Weights", "Replacement weights",
                "How often a kind is picked as a replacement. Relative weights, not percentages: 100 = normal, 0 = never.",
                "Brute never takes part as a replacement in this build, so its weight does nothing. Hunter is the Ubermorph, which regenerates: keep it rare.",
                WeightFamilies, Kind.Int, 100, 0, 1000, false, "Replacement weight: 100 = normal, 0 = never.");
            foreach (var cell in weights.Cells)
            {
                string fam = cell.Key;
                // Lurker and super Lurker are in the global pool, so as a replacement they load
                // anywhere; only a Lurker arriving by vent is a problem (VentFamilies below).
                if (fam == "SwarmEnemy")
                    cell.Warn(m => m.Int("Weights.SwarmEnemy") > Rec(m, "Weights.SwarmEnemy")
                        ? "kept low on purpose: a Swarmer replacement arrives as a whole horde." : null);
                else if (fam == "TubeBeetle")
                    cell.Warn(m => m.Int("Weights.TubeBeetle") > Rec(m, "Weights.TubeBeetle")
                        ? "kept lower than most on purpose; more makes Crawlers much more common." : null);
            }

            Table(list, Enemies, "Sources", "Sources: may be replaced",
                "Spawn points of a checked kind may be replaced. This does not bypass missing resources or unsupported arrivals.",
                null, AllFamilies, Kind.Bool, 1, 0, 1, true, "1 = spawn points of this kind may be replaced.");
            Table(list, Enemies, "Targets", "Targets: may appear as a replacement",
                "A checked kind may appear as a replacement.",
                null, AllFamilies, Kind.Bool, 1, 0, 1, true, "1 = this kind may appear as a replacement.");
            var vents = Table(list, Enemies, "VentFamilies", "Vent families",
                "A checked kind may arrive through a vent as a replacement. Unchecked keeps it to ordinary walk-in spawns; it loses nothing but vents.",
                null, AllFamilies, Kind.Bool, 1, 0, 1, true, "1 = this kind may arrive through a vent as a replacement.");
            vents.Cells.First(c => c.Key == "Lurker").Warn(m => m.On("VentFamilies.Lurker")
                ? "a vent Lurker once held the hospital lobby shut. The release keeps Lurker out of vents." : null);
            var experimental = Table(list, Enemies, "ExperimentalClasses", "Experimental kinds",
                "Kinds that only take part when checked. Infector and Swarmer work; Stalker, the Divider and the Ubermorph are experiments. A Divider or Ubermorph never replaces an enemy whose death a door is waiting for.",
                null, ExperimentalFamilies, Kind.Bool, 0, 0, 1, false, "1 = this experimental kind takes part.");
            experimental.Cells.First(c => c.Key == "DividerPart").Warn(m => m.On("ExperimentalClasses.DividerPart")
                ? "off in the release." : null);

            // ---- More Enemies
            add(B(More, Main, "MoreEnemies", 0, "More enemies",
                "Spawns more than one enemy per spawn point by repeating an arrival that already worked. How many is set per kind in the table below.")
                .Note("Works on its own too: with Replacement chance 0 you still get more enemies."));
            var counts = new TableInfo
            {
                Section = "MoreEnemies", Group = More, Title = "How many of each kind", InPlace = true,
                Description = "The total per spawn point, not extra: 2 = two where the level placed one, 1 = unchanged. Default applies to every kind without its own line. Bosses and swarmers always stay at one.",
                NoteText = "Only used with More enemies on. Max per spawn point caps every value here."
            };
            var defaultCount = I(More, "MoreEnemies", "Default", 1, 1, 8, "Default", "Default total per spawn point.").Comment("Total per spawn point for kinds without their own line.");
            defaultCount.Table = "MoreEnemies";
            counts.Cells.Add(defaultCount);
            list.Add(defaultCount);
            // swarmer spawn points are never multiplied (src/more_enemies.h), so they get no line
            foreach (var fam in OrdinaryFamilies.Where(f => f != "SwarmEnemy"))
            {
                var cell = I(More, "MoreEnemies", fam, 1, 1, 8, fam, "Total per spawn point for " + fam + ".").From("MoreEnemies.Default").Comment("Total per spawn point, 1 = unchanged.");
                cell.Table = "MoreEnemies";
                counts.Cells.Add(cell);
                list.Add(cell);
            }
            foreach (var cell in counts.Cells)
            {
                string id = cell.Id;
                cell.Warn(m =>
                {
                    long v = m.Int(id), cap = m.Int(M("MoreEnemiesMaxPerTrigger"));
                    if (v > cap) return "capped at " + cap + " by Max per spawn point.";
                    if (v > 2 && (id.EndsWith(".Pregnant") || id.EndsWith(".Infector")))
                        return "each copy brings its own swarm or turns corpses into more enemies; 2 is the safe maximum.";
                    if (v > 3) return "more than 3 per spawn point costs performance and can fill the enemy pool (pathfinding memory).";
                    return null;
                });
            }
            Tables.Add(counts);
            add(I(More, Main, "MoreEnemiesMaxPerTrigger", 3, 1, 8, "Max per spawn point",
                "Hard cap on the total per spawn point, whatever the table asks for.")
                .Warn(m => m.Int(M("MoreEnemiesMaxPerTrigger")) > 4 ? "More than 4 per spawn point costs performance and fills the enemy pool (pathfinding memory) faster." : null));
            add(I(More, Main, "MoreEnemiesPerSecond", 48, 0, 112, "Extra spawn budget",
                "How many extra spawns a window of 16 arrivals may hold, all kinds together. 0 stops extras. Rule of thumb: 16 x (largest total - 1).")
                .Warn(PerSecondWarning));
            add(B(More, Main, "MoreEnemiesVents", 0, "Also for vent spawns",
                "Repeats vent arrivals too, one after the other."));
            add(I(More, Main, "MoreEnemiesVentDelayMs", 2000, 0, 30000, "Gap between vent repeats (ms)",
                "A vent holds one enemy at a time; two in the same frame block each other. 2000 is what the game uses for its own retries.")
                .Warn(m => m.Int(M("MoreEnemiesVentDelayMs")) == 0 && m.On(M("MoreEnemiesVents")) ? "0 sends repeats into the same vent at once; they can block each other." : null));
            add(I(More, Main, "MoreEnemiesVentGapMs", 4000, 0, 30000, "Quiet vent before a repeat (ms)",
                "A vent repeat also waits until nothing has come out of that vent for this long. Two slashers in one wall vent at the same time crash the game.")
                .Warn(m => m.Int(M("MoreEnemiesVentGapMs")) < 2000 && m.On(M("MoreEnemiesVents")) ? "Below 2000 two enemies can meet in one vent, which can crash the game." : null));
            add(B(More, Main, "VentQueueGuard", 1, "Vent jam guard",
                "A vent repeat waits until the enemy before it has left the vent (one held in stasis in the vent mouth can take long) and gives up after 90 s. A dead enemy stuck first in a vent's queue is taken out so the others can come.")
                .Warn(WhenOff(M("VentQueueGuard"), "Off: stasis on an enemy climbing out of a vent can jam that vent for good.")));
            add(B(More, Main, "MoreEnemiesHideCompletion", 1, "Hide copies from level scripts",
                "Hides the copies' spawn-completion message from level logic, so a level that counts spawns only counts the original. If it cannot be hidden, that spawn is not multiplied.")
                .Warn(WhenOff(M("MoreEnemiesHideCompletion"), "Off: level scripts that count spawns also count the copies. Keep it on.")));
            add(B(More, Main, "MoreEnemiesSkipScripted", 1, "Leave story scenes alone",
                "Enemies a story scene uses or waits for are never multiplied, for example the slashers Ellie kills when you meet her. Copies of them would be in no scene and roam where the level never has one.")
                .Warn(WhenOff(M("MoreEnemiesSkipScripted"), "Off: copies of story scene enemies roam the level, and scenes can wait for them.")));
            add(B(More, Main, "MoreEnemiesCrossFamily", 1, "Multiply replaced enemies",
                "When an enemy was replaced by another kind: on uses the smaller of the two kinds' counts, off does not multiply such a replacement at all. Swarmer spawn points are never multiplied."));
            add(I(More, Main, "SwarmHorde", 6, 1, 12, "Swarmer horde size",
                "A Swarmer that replaces another kind of enemy arrives as a horde of this many. 1 = no horde. Works with or without More enemies."));
            add(I(More, Main, "PackHorde", 4, 1, 12, "Pack horde size",
                "A Pack enemy that replaces another kind of enemy arrives as a group of this many. 1 = no horde. Works with or without More enemies."));
            add(B(More, Main, "ExtraSpawns", 1, "Hand-placed extra spawns",
                "Reads plugins\\DS2ExtraSpawns.tsv: extra enemies (Guardians, Tripods ...) placed with the Spawner Editor. Rows bound to a spawner also need Spawner rules on; area rows do not."));


            // ---- Loot
            add(I(Loot, "Loot", "AmmoPercent", 100, 25, 500, "Ammo in pickups (%)",
                "Ammo in every ammo pickup, in percent of what the level placed. 100 = unchanged.")
                .Note("Affects every ammo pickup in the game."));
            add(I(Loot, "Loot", "CreditsPercent", 100, 25, 500, "Credits in pickups (%)",
                "Credits in every credit pickup, in percent of what the level placed. 100 = unchanged.")
                .Note("Affects every credit pickup in the game."));

            // ---- Schematics & Bench
            add(B(Schematics, "SchematicRandomizer", "Enabled", 0, "Schematic randomizer",
                "Schematics you find unlock a different item in the store. Elite suits and other completion rewards stay out of the shuffle.").Raw());
            add(Make(Schematics, "SchematicRandomizer", "Seed", Kind.OptionalInt, 0, 0, 4294967295L, "Schematic seed",
                "Own seed for the schematic shuffle. Checked: the main seed is used.").Keep()
                .Note("Never change it mid-playthrough: the schematics would unlock different items.")
                .Warn(m => m.IsChanged(m.ById["SchematicRandomizer.Seed"]) ? "Schematic seed changed. Only do this when you start a new playthrough." : null));
            add(B(Schematics, "SchematicRandomizer", "StoreFollowsSchematics", 1, "Store follows schematics",
                "An item whose schematic you missed reaches the store in the chapter where its schematic now lies. Off: items unlock at their usual chapter.").Raw());
            add(B(Schematics, "SchematicRandomizer", "FirstIsWeapon", 1, "First schematic is a weapon",
                "The first schematic pickup always unlocks a weapon.").Raw());
            add(B(Schematics, "SchematicRandomizer", "SecondIsSuit", 1, "Second schematic is a suit",
                "The second schematic pickup always unlocks an affordable suit (Security or Steampunk).").Raw());
            add(B(Schematics, "BenchRandomizer", "StatShuffle", 0, "Shuffle bench stats",
                "Upgrade nodes trade stats. Each weapon and suit keeps the same stats and the same number of each; only where they sit on the grid changes.").Raw());
            add(B(Schematics, "BenchRandomizer", "NodeShuffle", 0, "Use connector cells",
                "Stats may also sit on cells that used to be plain connectors.").Raw());
            add(B(Schematics, "BenchRandomizer", "ProtectStartPoints", 1, "Real upgrade at the start",
                "A tree always opens on a real upgrade instead of an empty node.").Raw());
            add(Make(Schematics, "BenchRandomizer", "Seed", Kind.OptionalInt, 0, 0, 4294967295L, "Bench seed",
                "Own seed for the bench shuffle. Checked: the main seed is used.").Keep()
                .Note("Never change it mid-playthrough: nodes you already bought would give something else.")
                .Warn(m => m.IsChanged(m.ById["BenchRandomizer.Seed"]) ? "Bench seed changed. Only do this when you start a new playthrough." : null));

            // ---- Loading & Performance
            add(B(Loading, Main, "GlobalTemplates", 0, "Cross-level enemy pool",
                "Loads the shared enemy pool, so an enemy can appear in a level that does not ship it. Survival takes all its enemies from it. Needs the randomizer folder next to deadspace2.exe.")
                .Note("Needed for cross-level enemies. Loading screens are held until the pool is in, on purpose.")
                .Warn(m =>
                {
                    if (m.On(M("GlobalTemplates"))) return null;
                    const string campaign = "Off: only enemies the current level already has can be used, so far fewer replacements.";
                    return m.On("Survival.Enabled") || m.On("Menu.Enabled")
                        ? campaign + " Survival needs it too: without it every wave mode and the campaign arenas have no enemies. Turn it on." : campaign;
                }));
            add(B(Loading, Main, "GlobalStreamProbe", 0, "Load the pool every level",
                "Requests the enemy pool at the start of every level. The cross-level enemy pool switches this on by itself.")
                .Warn(m => !m.On(M("GlobalStreamProbe")) && !m.On(M("GlobalTemplates")) ? "Off together with the cross-level pool: the pool is never loaded." : null));
            add(B(Loading, Main, "LoadingWaitsForPool", 1, "Loading screen waits for the pool",
                "Holds the loading screen until the enemy pool is loaded, up to the maximum below.")
                .Warn(WhenOff(M("LoadingWaitsForPool"), "Off: the first spawns of a level can be decided before the pool is loaded and stay original.")));
            add(I(Loading, Main, "MinimumLoadingSeconds", 4, 0, 60, "Minimum loading time (s)",
                "The loading screen stays at least this long. 0 = no minimum."));
            add(I(Loading, Main, "MaximumLoadingSeconds", 30, 0, 120, "Maximum pool wait (s)",
                "Upper limit for the pool wait. Loading screens can take up to this long on purpose.")
                .Warn(m => m.Int(M("MaximumLoadingSeconds")) == 0 && m.On(M("LoadingWaitsForPool")) ? "0 cancels the wait for the pool." : null));
            add(B(Loading, Main, "ZoneUnloadHeal", 1, "Zone switch fix",
                "Lets an area switch go on when only the enemy pool still holds a package.")
                .Note("Keep it on: it fixes doors and elevators that never open at high frame rates.")
                .Warn(WhenOff(M("ZoneUnloadHeal"), "Off: elevators and doors can hang forever at high frame rates (above about 95 FPS).")));
            add(I(Loading, Main, "ZoneUnloadHealDelayMs", 250, 0, 5000, "Zone fix delay (ms)",
                "How long a package has to hold an area switch up before the fix steps in."));
            add(I(Loading, Main, "FpsCap", 60, 0, 240, "Frame rate cap",
                "0 = uncapped. Values from 1 to 29 are raised to 30. VSync on top is fine.")
                .Adjust(v => v < 0 ? 0 : v > 240 ? 240 : (v > 0 && v < 30) ? 30 : v)
                .Note("Uncapped (0) is fine now: the zone switch fix solved the elevator hangs. A cap only matters if something still stalls; then try 60.")
                .Warn(m =>
                {
                    long v = m.Int(M("FpsCap"));
                    if (v > 0 && v < 30) return "The plugin raises this to 30.";
                    if (v == 0 && !m.On(M("ZoneUnloadHeal"))) return "Uncapped without the zone switch fix can hang elevators.";
                    return null;
                }));
            add(I(Loading, Main, "NavPowerPoolKiB", 4608, 0, 65536, "Pathfinding memory (KiB)",
                "Memory for enemy pathfinding. Retail has 576 KiB, enough for about 160 enemies; one more and the game stops with \"ran out of available heap memory\". 0 = retail, 4608 = eight times retail.")
                .Warn(m =>
                {
                    long kib = m.Int(M("NavPowerPoolKiB"));
                    if (kib < 4608 && m.On(M("MoreEnemies"))) return "Below the default with More enemies on: the game may stop when many enemies are around.";
                    if (kib <= 576 && m.On("Survival.Enabled") && m.Int("Survival.MaxAlive") > 10)
                        return "At the retail size survival keeps at most 10 enemies alive, whatever Enemies alive at once (Survival page) says. Set it to 4608.";
                    return null;
                }));

            // ---- Watchdog & Advanced
            const string softLock = "Off: a stuck replacement that a door or quarantine waits for can soft-lock the game.";
            add(B(Watchdog, Main, "Watchdog", 0, "Watchdog",
                "Watches the enemies the randomizer created: repairs their state tables and deals with ones that never come alive or stay stuck in a vent.").Raw()
                .Warn(WhenOff(M("Watchdog"), softLock)));
            add(B(Watchdog, Main, "WatchdogKill", 0, "Remove stuck enemies",
                "Kills a stuck enemy through the game's own death path, so its drop and any door counter still fire. Only kinds checked in the table below; bosses are always refused.").Raw()
                .Warn(WhenOff(M("WatchdogKill"), softLock)));
            add(B(Watchdog, Main, "WatchdogSpareFights", 1, "Spare enemies in a fight",
                "An enemy whose time is up while it holds someone in a grab, is held itself or stands close to a player is not removed; the watchdog looks again 15 s later. The survival checks for stuck, frozen and far-behind enemies do the same.").Raw()
                .Warn(WhenOff(M("WatchdogSpareFights"), "Off: an enemy can be removed in the middle of a grab.")));
            add(I(Watchdog, Main, "WatchdogSpareMeters", 3, 0, 20, "Close to a player (m)",
                "Within this distance of a player an enemy counts as in a fight. 0 = only grabs count."));
            add(B(Watchdog, Main, "WatchdogRepairTables", 1, "Repair state tables",
                "Points enemies the randomizer created back at their current state tables after the game rebuilds them. This is the fix for a known crash.").Raw()
                .Warn(WhenOff(M("WatchdogRepairTables"), "Off brings back a known crash.")));
            add(B(Watchdog, Main, "WatchdogDeathProxy", 1, "Death message stand-in",
                "When a replacement could not take over the original enemy's death message, the watchdog sends it when the replacement dies, so a room that counts deaths still completes.").Raw()
                .Warn(WhenOff(M("WatchdogDeathProxy"), "Off: a room that counts deaths may never open.")));
            add(B(Watchdog, Main, "WatchdogReleaseDeathMessage", 1, "Release a stuck enemy's death",
                "When the watchdog gives up on an enemy that never came alive or is still stuck in its vent, it sends that enemy's death message so the room can complete. Never for an enemy you simply have not reached.").Raw()
                .Warn(WhenOff(M("WatchdogReleaseDeathMessage"), "Off: a room that counts deaths may never open.")));
            add(I(Watchdog, Main, "WatchdogSeconds", 90, 10, 3600, "Stuck timeout (s)",
                "Seconds before a replaced enemy counts as stuck. An enemy you simply have not reached yet is normal."));
            add(I(Watchdog, Main, "WatchdogVentSeconds", 30, 0, 600, "Timeout for rebound vents (s)",
                "A shorter timeout for enemies that came through a rebound vent, the ones that get stuck most. They get this or their usual timeout (per kind, else the stuck timeout), whichever is shorter, so it never makes the wait longer. 0 = off."));
            add(I(Watchdog, Main, "WatchdogStateProbeSeconds", 6, 0, 120, "State check after (s)",
                "The watchdog checks once what a replaced enemy is doing, after this many seconds or the vent time below, whichever is later. 0 = off, which also turns the vent check off."));
            add(I(Watchdog, Main, "WatchdogVentStuckSeconds", 8, 0, 120, "Stuck in vent after (s)",
                "An enemy still in its vent climb-out after this many seconds is logged as a suspect. If it is still there when its timeout runs out, its template goes on DS2VentDonorBlocklist.tsv and is not used for vents again. 0 = off."));

            var wdDefault = B(Watchdog, "WatchdogFamilies", "Default", 0, "Default", "Watchdog default for kinds without their own line.").Comment("1 = the watchdog may act on kinds without their own line.");
            wdDefault.Table = "WatchdogFamilies";
            var wdFamilies = new TableInfo
            {
                Section = "WatchdogFamilies", Group = Watchdog, Title = "Kinds the watchdog may act on",
                Description = "Default applies to kinds without their own line. Bosses, the Divider, Guardians, Nests, Tripods and tentacles are always refused in code.",
            };
            wdFamilies.Cells.Add(wdDefault);
            list.Add(wdDefault);
            foreach (var fam in OrdinaryFamilies)
            {
                var cell = B(Watchdog, "WatchdogFamilies", fam, 0, fam, "Watchdog may act on " + fam + ".").From("WatchdogFamilies.Default").Comment("1 = the watchdog may act on this kind.");
                cell.Table = "WatchdogFamilies";
                wdFamilies.Cells.Add(cell);
                list.Add(cell);
            }
            Tables.Add(wdFamilies);
            var timers = Table(list, Watchdog, "WatchdogTimers", "Timeout per kind (s)",
                "Seconds before the watchdog acts on that kind. 0 = use the stuck timeout above. Values below 10 are raised to 10. Lurker gets 45 because it clings to walls and gets stuck more easily.",
                null, OrdinaryFamilies, Kind.Int, 0, 0, 3600, false, "Seconds before the watchdog acts on this kind, 0 = the global timeout.");
            foreach (var cell in timers.Cells)
            {
                string id = cell.Id;
                cell.Warn(m => { long v = m.Int(id); return v > 0 && v < 10 ? "raised to 10 by the plugin." : null; });
            }

            add(B(Watchdog, Main, "PoolActorsFollowLevel", 1, "Replacements belong to the level",
                "A replaced enemy keeps the original spawn's stream handle, so it is torn down with the level. Fixes checkpoint restarts.")
                .Warn(WhenOff(M("PoolActorsFollowLevel"), "Keep it on: it fixes checkpoint restarts.")));
            add(B(Watchdog, Main, "ReleasePoolAtLevelEnd", 1, "Release pool at level end",
                "Frees the enemy pool when a level ends. Off holds it until the level teardown removes the streaming service."));
            add(B(Watchdog, Main, "PoolHoldForever", 0, "Never release the pool",
                "Debug only.").Warn(WhenOn(M("PoolHoldForever"), "Debug only. Leave it off.")));
            add(B(Watchdog, Main, "GuardServiceRelease", 1, "Teardown guard",
                "Skips handing a resource back to the streaming service while that service is already gone during level teardown, which used to crash.").Raw()
                .Warn(WhenOff(M("GuardServiceRelease"), "Off brings back a known crash in level teardown.")));
            add(B(Watchdog, Main, "PinPoolPrerequisites", 0, "Pin pool streams",
                "Experiment: holds extra references on shared character streams. Changes engine reference counts.")
                .Warn(WhenOn(M("PinPoolPrerequisites"), "Experimental and the riskiest switch. Leave it off.")));
            add(B(Watchdog, Main, "Variants", 0, "Stat variants",
                "Faster, tougher or recoloured enemy variants. This build has no variants, so it does nothing.")
                .Warn(WhenOn(M("Variants"), "This build has no variants; this does nothing.")));
            add(B(Watchdog, Main, "VariantRuntime", 0, "Variant speed and size",
                "Speed and size changes for variants. Needs Stat variants; not in this build."));
            add(I(Watchdog, Main, "InstallDelayMs", 3000, 0, 60000, "Hook install delay (ms)",
                "Waits this long after the game is ready before installing hooks, so other plugins that check game bytes (the DS2 Debug Menu) go first.")
                .Adjust(v => v < 0 ? 0 : v));
            list.Last().Max = 600000;
            add(I(Watchdog, Main, "PlacementHotkey", 121, 0, 255, "Placement readout key",
                "Windows virtual-key code of the key that writes your position and the nearest spawners to the log, for the Spawner Editor. 121 = F10.")
                .Adjust(v => v < 0 || v > 255 ? 121 : v));

            // ---- Diagnostics
            add(B(Diagnostics, Main, "ObserveSpawns", 0, "Log every spawn",
                "Writes a line per spawn to the log. Needed for useful bug reports.").Raw());
            add(I(Diagnostics, Main, "DetailedSpawnLimit", 256, 0, 4096, "Detailed spawn lines",
                "Buffer for detailed spawn lines between two log writes. 0 turns them off; the counters keep running."));
            add(B(Diagnostics, Main, "StreamRegistryDiagnostics", 0, "Stream registry snapshots",
                "Read-only snapshots every five seconds, up to 60 reports."));
            add(B(Diagnostics, Main, "StreamWatchDiagnostics", 0, "Stream watch",
                "Logs stream changes continuously (read-only)."));
            add(B(Diagnostics, Main, "MessageTrace", 0, "Message trace",
                "One log line per script message.").Warn(WhenOn(M("MessageTrace"), "Very noisy log. Only turn it on when asked for.")));
            add(B(Diagnostics, Main, "TextureProbe", 0, "Texture probe",
                "Debug only.").Warn(WhenOn(M("TextureProbe"), "Debug only. Leave it off.")));
            add(T(Diagnostics, Main, "VariantBias", "Variant bias",
                "Test aid: this variant name wins every draw it takes part in. Leave empty.")
                .Validate(v => Validators.PlainText(v, 63))
                .Warn(m => m.Get(M("VariantBias")).Length > 0 ? "Test aid. Leave it empty for normal play." : null));
            add(T(Diagnostics, Main, "PlanDump", "Plan dump",
                "Test aid: writes the donor order for one source GUID to the log. Leave empty.")
                .Validate(Validators.Guid)
                .Warn(m => m.Get(M("PlanDump")).Length > 0 ? "Test aid. Leave it empty for normal play." : null));

            Curated.Add(list);

            // ---- Enemy speed: one table for the story and one per mode (src/enemy_speed.h)
            var speedOn = B(Curated.Speed, "EnemySpeed", "Enabled", 0, "Enemy speed",
                "Sets how fast every kind of enemy is, for the story and for each game mode on its own. Scripted scenes, grabs, spawns and vents keep the game's speed. In co-op the host's tables count.");
            list.Add(speedOn);
            foreach (var mode in SpeedModes)
            {
                string section = "EnemySpeed" + mode[0];
                var table = new TableInfo
                {
                    Section = section, Group = Curated.Speed, Title = mode[1],
                    Description = "Speed in percent, 100 = the game's own, 25 to 300. All is for every kind without its own value; bosses and set pieces never follow it.",
                    NoteText = "Only used with Enemy speed on. Saving applies the values to a running game."
                };
                var all = I(Curated.Speed, section, "All", 100, 25, 300, "All", "Every kind without its own value.").Comment("Speed in percent for every kind without its own line.");
                all.Table = section;
                table.Cells.Add(all);
                list.Add(all);
                foreach (var kind in SpeedKinds)
                {
                    var cell = I(Curated.Speed, section, kind, 100, 25, 300, kind, "Speed of " + kind + " in percent.").Comment("Speed in percent, 100 = the game's own.");
                    if (Array.IndexOf(SpeedBosses, kind) < 0) cell.From(section + ".All");
                    cell.Table = section;
                    table.Cells.Add(cell);
                    list.Add(cell);
                }
                Tables.Add(table);
            }
            return list;
        }

        static readonly string[][] SpeedModes = {
            new[] { "Story", "Story" }, new[] { "Survival", "Survival" }, new[] { "Mercenaries", "Mercenaries" }, new[] { "Objective", "Objective" },
            new[] { "Zone", "Hold the zone" }, new[] { "Versus", "Versus" } };
        static readonly string[] SpeedBosses = { "Tripod", "Guardian", "Nest", "Boss" };
        static readonly string[] SpeedKinds = {
            "Slasher", "Pack", "Swarmer", "TubeBeetle", "Leaper", "Lurker", "Exploder", "Infector", "Puker", "Pregnant", "Stalker", "Brute", "Ubermorph",
            "Divider", "DividerPart", "Tripod", "Guardian", "Nest", "Boss" };

        // Rows the window leaves out on purpose; saving keeps them as they are.
        public static bool Hidden(string section, string key)
        {
            if (!section.Equals("Sources", StringComparison.OrdinalIgnoreCase) && !section.Equals("Targets", StringComparison.OrdinalIgnoreCase) &&
                !section.Equals("VentFamilies", StringComparison.OrdinalIgnoreCase)) return false;
            return HiddenFamilies.Any(f => f.Equals(key, StringComparison.OrdinalIgnoreCase));
        }

        // With the master switch off the campaign stays as it is; survival can still
        // run on its own, but only with its own switch and the enemy pool on.
        static string MasterOff(Model m)
        {
            const string off = "Off: nothing in the campaign is randomized.";
            if (!m.On("Survival.Enabled") && !m.On("Menu.Enabled")) return off;
            return off + " Survival still works only with Survival waves (Survival page) and the cross-level enemy pool (Loading page) on. " +
                "For survival without campaign changes, the proven setup is the randomizer on with Replacement chance 0 and More enemies off.";
        }

        static long Rec(Model m, string id)
        {
            long v;
            return long.TryParse(m.Recommended[id], out v) ? v : 0;
        }

        static string PerSecondWarning(Model m)
        {
            if (!m.On(M("MoreEnemies"))) return null;
            long budget = m.Int(M("MoreEnemiesPerSecond"));
            if (budget == 0) return "0 stops all extra spawns.";
            long cap = m.Int(M("MoreEnemiesMaxPerTrigger"));
            long largest = 1;
            foreach (var o in m.Options)
                if (o.Section == "MoreEnemies") largest = Math.Max(largest, Math.Min(cap, m.Int(o.Id)));
            long wanted = Math.Min(112, 16 * (largest - 1));
            if (budget < wanted) return "Too low for your counts: only some spawns get extras. " + wanted + " covers them.";
            return null;
        }

        static TableInfo Table(List<Option> list, string group, string section, string title, string desc, string note,
            string[] families, Kind kind, long def, long min, long max, bool raw, string comment)
        {
            var table = new TableInfo { Section = section, Group = group, Title = title, Description = desc, NoteText = note };
            foreach (var fam in families)
            {
                var o = Make(group, section, fam, kind, def, min, max, fam, desc);
                o.Table = section;
                o.RawRead = raw;
                o.CommentText = comment;
                table.Cells.Add(o);
                list.Add(o);
            }
            Tables.Add(table);
            return table;
        }
    }
}
