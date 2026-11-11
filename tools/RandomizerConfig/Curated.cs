// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace DS2CoopConfig
{
    // The co-op, bot, survival and versus switches a player is likely to change.
    // Defaults and clamps follow the plugin source (src/runtime_coop_*.inc,
    // src/survival.h); the recommended values are the repository templates.
    static class Curated
    {
        public const string Coop = "Co-op";
        public const string Bots = "Bots";
        public const string Survival = "Survival";
        public const string Versus = "Versus";

        static readonly Func<long, long> AsIs = v => v;

        static Option Make(string group, string section, string key, Kind kind, long def, long min, long max, string title, string desc)
        {
            return new Option
            {
                Group = group, Section = section, Key = key, Kind = kind, Min = min, Max = max,
                CodeDefault = def.ToString(CultureInfo.InvariantCulture), Title = title, Description = desc,
                CommentLines = Templates.CommentLines(section, key)
            };
        }

        // switches read as GetPrivateProfileInt(...) != 0
        static Option B(string group, string section, string key, int def, string title, string desc)
        {
            return Make(group, section, key, Kind.Bool, def, 0, 1, title, desc).Raw();
        }

        // numbers the plugin uses as they are, unless an Adjust says otherwise
        static Option N(string group, string section, string key, long def, long min, long max, string title, string desc)
        {
            return Make(group, section, key, Kind.Int, def, min, max, title, desc).Adjust(AsIs);
        }

        // numbers the plugin clamps to [min, max]
        static Option C(string group, string section, string key, long def, long min, long max, string title, string desc)
        {
            return Make(group, section, key, Kind.Int, def, min, max, title, desc);
        }

        static Option Key(string group, string section, string key, long def, string title, string desc, Func<long, long> adjust)
        {
            var o = Make(group, section, key, Kind.Int, def, 0, 255, title, desc).Adjust(adjust ?? AsIs);
            o.KeyCode = true;
            return o;
        }

        static Option T(string group, string section, string key, string def, string title, string desc, Func<string, string> validate)
        {
            var o = Make(group, section, key, Kind.Text, 0, 0, 0, title, desc);
            o.CodeDefault = def;
            return o.Validate(validate);
        }

        static Func<Model, string> Needs(string id, string text) { return m => m.On(id) ? null : text; }

        static string Address(string v)
        {
            if (v.Length == 0) return null;
            if (v.IndexOf(' ') >= 0) return "No spaces: an address and port like 100.101.102.103:27015.";
            return Validators.PlainText(v, 127);
        }

        static readonly string[] Modes = { "survival", "mercenaries", "objective", "hold the zone", "versus" };

        // src/runtime_coop_rigcolor.inc kChoices
        static readonly string[] SuitColors = { "stock", "red", "orange", "yellow", "lime", "green", "blue", "purple", "pink", "white", "gray" };

        static readonly string[] HudColors = { "stock", "red", "orange", "yellow", "lime", "green", "blue", "purple", "pink", "white", "gray" };

        // src/runtime_coop_hud.inc ParseColor
        static string HudColor(string v, bool emptyOk)
        {
            string t = v.Trim();
            if (t.Length == 0) return emptyOk ? null : "Enter stock, a colour name or a hex value like FF8A2A.";
            if (HudColors.Contains(t.ToLowerInvariant())) return null;
            string hex = t.StartsWith("#") ? t.Substring(1) : t;
            if (hex.Length == 6 && hex.All(Uri.IsHexDigit)) return null;
            return "Use one of " + string.Join(", ", HudColors) + " or a hex value like FF8A2A.";
        }

        static string SuitColor(string v)
        {
            return SuitColors.Contains(v.Trim().ToLowerInvariant()) ? null : "Use one of: " + string.Join(", ", SuitColors) + ".";
        }

        // Several warnings of one row, one per line.
        static string Lines(params string[] parts)
        {
            var kept = parts.Where(p => p != null).ToArray();
            return kept.Length == 0 ? null : string.Join(Environment.NewLine, kept);
        }

        // ---- the survival director

        // What [Survival] Map can match, ignoring case: the picker's maps
        // (src/coop_mpmap.h kMapTable) and the campaign arenas (generated_arenas.h kArenas).
        public static readonly string[] SurvivalMaps = {
            "mp_bombRun", "mp_detonate", "mp_escape", "mp_gov", "mp_solar", "pdlc_mp_mall", "pdlc_mp_school", "arena_ch01", "arena_ch03", "arena_ch03b", "arena_ch04", "arena_ch05", "arena_ch08", "arena_ch09", "arena_ch10", "arena_ch11" };

        static bool IsAny(string map) { return string.Equals(map.Trim(), "any", StringComparison.OrdinalIgnoreCase); }

        public static bool KnownMap(string map)
        {
            string name = map.Trim();
            return IsAny(name) || SurvivalMaps.Any(n => n.Equals(name, StringComparison.OrdinalIgnoreCase));
        }

        static string MapList { get { return "any, " + string.Join(", ", SurvivalMaps); } }

        // An empty Map= keeps the plugin's default (survival.h ParseConfig).
        static string SurvivalMapOf(Model m)
        {
            string map = m.Get("Survival.Map").Trim();
            return map.Length > 0 ? map : m.ById["Survival.Map"].CodeDefault;
        }

        static string SurvivalMap(string v)
        {
            if (v.Length == 0) return "Enter any or a map name.";
            return Validators.PlainText(v, 63) ?? (KnownMap(v) ? null : "No map is called that. Use one of: " + MapList + ".");
        }

        // Survival waves and the enemy pool are what every wave mode, the Versus AI
        // slashers and the arenas need. With the randomizer's master switch off the
        // director still runs on its own (survival-only mode), so that switch is not checked.
        static string DirectorOff(Model m)
        {
            bool waves = m.On("Survival.Enabled"), pool = m.On("DS2Randomizer.GlobalTemplates");
            if (waves && pool) return null;
            if (!waves && !pool) return "Survival waves (Survival page) and the cross-level enemy pool (Loading page) are off. Turn both on.";
            return (waves ? "the cross-level enemy pool (Loading page)" : "Survival waves (Survival page)") + " is off. Turn it on.";
        }

        // ---- the co-op half (runtime_coop.inc Install)

        const string CoopHalfGone = "the whole co-op half stays off, with no main menu entries, multiplayer maps, bots or shop";

        static bool HostNpcsBlocked(Model m)
        {
            return m.Flag("Coop.HostNPCs", false) && (!m.Flag("Coop.MPUniformStreams", false) || m.Flag("Coop.SyncAllObjects", false));
        }

        // Why the co-op half does not install, with the fix, or null.
        public static string CoopHalfOff(Model m)
        {
            if (!m.Flag("Coop.Enabled", false)) return "[Coop] Enabled is 0 on the Expert page. Set it to 1.";
            if (HostNpcsBlocked(m)) return "[Coop] HostNPCs=1 needs MPUniformStreams=1 and SyncAllObjects=0 (Expert page). Set those, or HostNPCs=0.";
            if (!PeerListOk(m.Value("Coop.Peer"))) return "[Coop] Peer on the Expert page is not ip:port. Correct it or leave it empty.";
            return null;
        }

        static Func<Model, string> WhenCoopHalfOff(string id, string effect)
        {
            return m =>
            {
                if (id != null && !m.On(id)) return null;
                string cause = CoopHalfOff(m);
                return cause == null ? null : effect + ": " + CoopHalfGone + ". " + cause;
            };
        }

        // As ParsePeer: dotted IPv4, a colon, then a port of 1 to 65535 read by strtoul.
        public static bool PeerOk(string text)
        {
            int colon = text.IndexOf(':');
            if (colon < 0) return false;
            string[] parts = text.Substring(0, colon).Split('.');
            if (parts.Length != 4) return false;
            foreach (var p in parts)
            {
                if (p.Length == 0 || p.Length > 3 || (p.Length > 1 && p[0] == '0') || p.Any(c => c < '0' || c > '9')) return false;
                if (int.Parse(p, CultureInfo.InvariantCulture) > 255) return false;
            }
            int i = colon + 1;
            while (i < text.Length && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r'))) i++;
            if (i < text.Length && text[i] == '-') return false;
            if (i < text.Length && text[i] == '+') i++;
            long port = 0;
            int digits = 0;
            for (; i < text.Length && text[i] >= '0' && text[i] <= '9'; i++, digits++)
                if (port <= 65535) port = port * 10 + (text[i] - '0');
            return digits > 0 && port >= 1 && port <= 65535;
        }

        // [Coop] Peer: up to eight partners separated by semicolons, read into 64 bytes.
        public static bool PeerListOk(string list)
        {
            if (list == null) return true;
            if (list.Length > 63) list = list.Substring(0, 63);
            int dialled = 0;
            foreach (var one in list.Split(';'))
            {
                if (one.Length == 0 || dialled >= 8) continue;
                if (!PeerOk(one)) return false;
                dialled++;
            }
            return true;
        }

        const string HostNpcsText = "HostNPCs=1 needs MPUniformStreams=1 and SyncAllObjects=0. As it is, " + CoopHalfGone +
            ". Set MPUniformStreams=1 and SyncAllObjects=0, or HostNPCs=0.";

        // Warnings for Expert page rows, which come from the file and the templates.
        public static Func<Model, string> ExpertWarning(string section, string key)
        {
            switch ((section + "." + key).ToLowerInvariant())
            {
                case "coop.enabled":
                    return m => m.Flag("Coop.Enabled", false) ? null
                        : "Off: the whole co-op half stays off, with no main menu entries, multiplayer maps, bots, shop, HUD or notices. Set it to 1. Only the co-op launchers that set DS2COOP start it anyway.";
                case "coop.hostnpcs":
                case "coop.syncallobjects":
                    return m => HostNpcsBlocked(m) ? HostNpcsText : null;
                case "coop.mpuniformstreams":
                    return m => Lines(HostNpcsBlocked(m) ? HostNpcsText : null,
                        m.On("Coop.SuitSync") && !m.Flag("Coop.MPUniformStreams", false) ? "Share suits (Co-op page) is forced off without it. Set it to 1." : null);
                case "coop.peer":
                    return m => PeerListOk(m.Value("Coop.Peer")) ? null
                        : "Not ip:port: " + CoopHalfGone + ". Enter addresses like 100.101.102.103:27015, several separated by semicolons, or leave it empty.";
                case "coop.deathsync":
                    return m => m.On("Coop.MirrorDeath") && !m.Flag("Coop.DeathSync", false) ? "Off: Die together (Co-op page) is forced off. Set it to 1." : null;
                default:
                    return null;
            }
        }

        static string MapWarning(Model m)
        {
            string map = SurvivalMapOf(m);
            if (!KnownMap(map)) return "No map is called that, so no map ever gets waves. Use one of: " + MapList + ".";
            if (IsAny(map)) return null;
            if (m.On("Menu.Enabled"))
                return "Only " + map + " gets waves: every other map and arena in the main menu's map picker has no enemies (Versus aside). Set it to any.";
            string visit = m.Get("MpMap.Map").Trim();
            if (m.On("MpMap.Enabled") && visit.Length > 0 && !visit.Equals(map, StringComparison.OrdinalIgnoreCase))
                return "The visit key loads " + visit + " (Versus page), which gets no waves. Set it to any or " + visit + ".";
            return null;
        }

        static string MenuWarning(Model m)
        {
            if (!m.On("Menu.Enabled")) return null;
            string coop = CoopHalfOff(m);
            if (coop != null) return "The main menu stays as it is: " + CoopHalfGone + ". " + coop;
            if (!m.On("MpMap.Enabled")) return "Needs the multiplayer map visit (Versus page); without it the main menu stays as it is. Turn it on.";
            string director = DirectorOff(m);
            return director == null ? null : "No map in the map picker gets waves, and Versus no AI slashers, while " + director;
        }

        // Bonuses that never shrink (fade 0 or a 100 % floor) and no run limit: a good
        // team never runs out of time (survival.h FadedBonusMs, MaxRunSeconds).
        static Func<Model, string> Endless(string section)
        {
            return m =>
            {
                bool fades = m.Int(section + ".BonusFadeSeconds") > 0 && m.Int(section + ".BonusFloorPercent") < 100;
                return fades || m.Int(section + ".MaxRunSeconds") > 0 ? null
                    : "Time bonuses never shrink and the run has no time limit, so a fast run can go on for ever. Set the bonus fade above 0 (with a floor below 100 %) or the longest run above 0.";
            };
        }

        // [Mercenaries] / [Zone] MaxRunSeconds: 0 = never, 1 to 119 are raised to 120.
        static Option MaxRun(string section, long def, string title)
        {
            string id = section + ".MaxRunSeconds";
            var endless = Endless(section);
            return C(Survival, section, "MaxRunSeconds", def, 0, 7200, title,
                    "The run ends after this long whatever the clock says. 0 = never; values below 120 are raised to 120.")
                .Adjust(v => v < 0 ? 0 : v > 7200 ? 7200 : v > 0 && v < 120 ? 120 : v)
                .Warn(m =>
                {
                    long v = m.Int(id);
                    return Lines(v > 0 && v < 120 ? "The plugin raises this to 120." : null, endless(m));
                });
        }

        static string ArenaWarning(Model m)
        {
            if (!m.On("Arena.Arenas")) return null;
            if (!m.On("Menu.Enabled")) return "Does nothing while Survival in the main menu is off: the arenas are picked there.";
            string director = DirectorOff(m);
            if (director != null) return "An arena locks its doors and gets no waves while " + director;
            string map = SurvivalMapOf(m);
            if (IsAny(map)) return null;
            bool arena = KnownMap(map) && map.StartsWith("arena_", StringComparison.OrdinalIgnoreCase);
            return "Survival map (above) is " + map + ", so " + (arena ? "every other arena" : "every arena") +
                " locks its doors and gets no waves. Set Survival map to any.";
        }

        public static void Add(List<Option> list)
        {
            Action<Option> add = list.Add;

            // ---- Co-op
            add(T(Coop, "Coop", "JoinAddress", "", "Join address",
                "The host's address and game port, for example a Tailscale address like 100.101.102.103:27015. The host leaves it empty. The game uses UDP on that port, port + 1000 and port + 2000.", Address)
                .Under("Connection")
                .Warn(m => m.Get("Coop.JoinAddress").Length == 0 || PeerOk(m.Get("Coop.JoinAddress")) ? null
                    : "Join co-op refuses this address: it takes an IPv4 address and the host's port, like 100.101.102.103:27015, not a host name."));
            add(Key(Coop, "Coop", "CatchUpKey", 0, "Catch-up key",
                "Puts your Isaac next to a partner when the level cut you off, for example behind a barrier built for one player. With more players each press goes to the next partner.",
                v => v < 0 || v > 255 ? 0 : v));
            add(Key(Coop, "Menu", "HelpKey", 112, "Key list",
                "Shows a page with every key of the mod, in menus and levels. The same key hides it.",
                v => v < 0 || v > 255 ? 0 : v));
            add(B(Coop, "Coop", "PauseKeepsRunning", 0, "Pause keeps the world running",
                "While a partner is connected, the pause menu does not stop the game, as in a multiplayer match.")
                .Under("Shared world"));
            add(B(Coop, "Coop", "SuitSync", 0, "Share suits",
                "Partners see the suit you wear, and your suit changes reach them. Needs [Coop] MPUniformStreams on (Expert page).")
                .Warn(m => m.On("Coop.SuitSync") && !m.Flag("Coop.MPUniformStreams", false)
                    ? "Forced off while [Coop] MPUniformStreams is off (Expert page). Set MPUniformStreams=1." : null));
            add(B(Coop, "Coop", "LoadoutSync", 0, "Share weapons",
                "A partner's Isaac carries the weapons its player carries."));
            add(B(Coop, "Coop", "HelmetSync", 1, "Share the helmet",
                "A partner's helmet folds open or closed the way its player's does."));
            add(B(Coop, "Coop", "MirrorDeath", 0, "Die together",
                "When a partner dies, everybody goes through the death and restarts at the same checkpoint, so nobody drifts apart. Needs [Coop] DeathSync on (Expert page).")
                .Warn(m => m.On("Coop.MirrorDeath") && !m.Flag("Coop.DeathSync", false)
                    ? "Forced off while [Coop] DeathSync is off (Expert page). Set DeathSync=1." : null));
            add(B(Coop, "Coop", "ShareTormentorArm", 0, "Share the Tormentor's arm",
                "In the Tormentor fight every hit on its arm counts on all machines, so the grab ends for everybody at once. Every player needs it on."));
            add(B(Coop, "Coop", "LiftWait", 0, "Lifts wait for everybody",
                "A lift button waits until every partner stands in the cabin, a minute at most, then the lift goes anyway."));
            add(B(Coop, "Menu", "CampaignCoop", 1, "Co-op campaign in the main menu",
                "Single player becomes co-op: whoever starts or loads a game hosts, and \"previously on Dead Space\" becomes join co-op. A joiner arrives at the host's next checkpoint restart; his own saves stay untouched.")
                .Under("Levels, doors and loading")
                .Warn(m => m.On("Menu.CampaignCoop") && (!m.On("Menu.Enabled") || !m.On("MpMap.Enabled"))
                    ? "Does nothing while Survival in the main menu (Survival page) or the multiplayer map visit (Versus page) is off." : null));
            add(B(Coop, "Level", "FollowHost", 1, "Follow the host's level",
                "A client loads the map the host plays. After joining only a new map is followed, so crossing a checkpoint reloads nobody."));
            add(B(Coop, "Checkpoint", "FollowRestart", 1, "Restart with the host",
                "When the host restarts from a checkpoint (pause menu or death), the clients restart too."));
            add(B(Coop, "Loading", "SyncWithPartners", 0, "Start levels together",
                "A machine that finished loading keeps its loading screen up until every partner has loaded the same map, after level changes and checkpoint restarts alike."));
            add(N(Coop, "Loading", "SyncTimeoutMs", 30000, 0, 600000, "Loading wait limit (ms)",
                "After this long the level starts anyway, for example when a partner never connects."));
            add(B(Coop, "Streams", "PinPartnerStreams", 0, "Keep partners' rooms loaded",
                "When your area switches, the rooms a partner still stands in stay loaded instead of being dropped. Rooms may stay loaded a little longer."));
            add(B(Coop, "Streams", "DoorCullsFollowUs", 0, "Doors never hide your room",
                "A door a partner shut from the other side no longer hides the room you stand in."));
            add(B(Coop, "Gate", "Enabled", 1, "Nobody rides alone",
                "Progress that would take a player somewhere (a lift, a one-way door, a checkpoint) waits until every player got there, with an on-screen notice. A timeout lets it through, so progress is delayed, never lost."));
            add(B(Coop, "Mirror", "DoorsWaitForPartners", 1, "Doors wait for partners",
                "A door opens for everybody at once when every partner has reached it, as in Dead Space 3. After a long wait it opens anyway."));
            add(B(Coop, "Cutscene", "HidePartners", 1, "Hide partners in cutscenes",
                "While your Isaac plays a scripted scene, partner bodies are hidden, so each screen shows one Isaac.")
                .Under("Names and scenes"));
            add(B(Coop, "Nameplate", "Enabled", 1, "Nameplates",
                "Partners' and bots' names over their heads on the multiplayer maps. The campaign has none."));
            add(B(Coop, "Marks", "Enabled", 1, "Waypoints on screen",
                "The Hold the Zone zone and the Mercenaries time capsules as markers on the screen with their distance, at the edge with an arrow when out of view."));
            add(T(Coop, "Nameplate", "Name", "", "Your name",
                "The name partners see over your head, printable ASCII, 20 characters at most. Empty: PLAYER 1, PLAYER 2 and so on by seat.",
                v => Validators.PlainText(v, 20)));
            add(T(Coop, "Visuals", "RigColor", "stock", "RIG colour",
                "The health bar on the back of your suit, any suit: stock, red, orange, yellow, lime, green, blue, purple, pink, white or gray. Only on your screen. Read at the start of the game.",
                SuitColor));
            add(B(Coop, "CameraTools", "Enabled", 0, "Trailer camera",
                "A free camera flown with a gamepad while you keep playing Isaac with keyboard and mouse, slow motion and a camera path recorder. F6 or both sticks: free camera; F5 or A: slow motion; F4 or Back: stop time; F2 or X: record a path; F3 or Y: play it. Keys and speeds on the Expert page."));
            add(T(Coop, "Visuals", "StasisColor", "stock", "Stasis colour",
                "The stasis shot, its blast and the frozen glow, also the stasis canisters. Same colours as the RIG. Read at the start of the game.",
                SuitColor));
            add(T(Coop, "Visuals", "KinesisColor", "stock", "Kinesis colour",
                "The kinesis beam and its flashes. Same colours as the RIG.",
                SuitColor));
            add(B(Coop, "Visuals", "KinesisThrowTrail", 0, "Kinesis colour on thrown objects",
                "Also colours the trail of objects you throw. Enemy blades share that trail and take the colour too."));
            add(T(Coop, "Visuals", "LaserColor", "stock", "Aiming laser colour",
                "The aiming laser of every gun. The red laser of an empty clip stays.",
                SuitColor));
            add(T(Coop, "Visuals", "WeaponColor", "stock", "Weapon effect colour",
                "Muzzle flashes, shots, beams and impacts of every gun. One key per gun on the Expert page (PlasmaCutterColor and so on) overrides it.",
                SuitColor));
            add(T(Coop, "Visuals", "BloodColor", "stock", "Blood colour",
                "Blood sprays, drips, gore chunks and decals. Green acid and orange bile stay. Blood painted into the levels and on cut limbs stays red.",
                SuitColor));
            add(T(Coop, "Visuals", "VisorColor", "stock", "Visor colour",
                "The glow of your helmet visor, with the same colours. It also tints the lens flares the visor shares with some lamps, as the TexMod colour packs do. Read at the start of the game.",
                SuitColor));
            add(B(Coop, "Coop", "BackgroundFullSpeed", 0, "Full speed in the background",
                "Windows may not slow the game down while its window is in the background. Needed when two copies run on one PC.")
                .Under("Two copies on one PC, timeouts and crashes"));
            add(B(Coop, "Coop", "FocusInputOnly", 0, "Input only in the focused window",
                "While another window has the focus, this copy ignores keyboard, pad and mouse, so two copies on one PC do not both act on every key. Without its own line it follows Full speed in the background.")
                .From("Coop.BackgroundFullSpeed"));
            add(C(Coop, "Coop", "PeerTimeoutMs", 30000, 10000, 120000, "Partner timeout (ms)",
                "A partner who sends nothing this long loses his seat and is removed. Loading screens do not count.")
                .Warn(m => m.Flag("Coop.PeerTimeout", false) ? null
                    : "Does nothing while [Coop] PeerTimeout is off (Expert page). Set PeerTimeout=1."));
            add(B(Coop, "Coop", "CrashDumps", 0, "Crash dumps",
                "A crash writes a dump and a short report, into C:\\DS2CrashDumps when that folder exists, else into the game folder. Useful for bug reports."));
            add(B(Coop, "Coop", "ConfigCheck", 1, "Compare settings with partners",
                "On joining, each machine compares this file and its plugin build with the others and says on screen whether they match; the log lists every setting that differs."));
            add(B(Coop, "Coop", "Network", 0, "Start a network session at launch (manual setup)",
                "Not needed to play together: host co-op and join co-op in the main menu start the network for that game by themselves. On: the game opens a session as soon as it starts and dials the partners listed under Peer (Expert page), the old setup for test launchers.")
                .Under("Manual setup")
                .Warn(WhenCoopHalfOff(null, "Nothing on this page works")));

            // ---- Bots
            add(B(Bots, "Bot", "Enabled", 0, "Companion bots",
                "Bots join a few seconds after the level stands, solo or as the host. They follow you, fight, pick up items and heal, and they cannot die.")
                .Under("Squad")
                .Warn(WhenCoopHalfOff("Bot.Enabled", "No bot joins")));
            add(C(Bots, "Bot", "Count", 1, 1, 3, "Number of bots",
                "How many bots at once. Online, every human partner takes one bot's seat (four players at most)."));
            add(B(Bots, "Bot", "Profiles", 0, "Personalities",
                "Each bot gets its own name, reaction time, aim, courage and pace from the [Bot1] to [Bot3] sections (Expert page). Off: every bot is the same and reacts at once."));
            add(B(Bots, "Bot", "AnticsEnabled", 1, "Antics",
                "Lets the bots do the playful things their profiles allow while nothing is going on. Off: none for any bot."));
            add(B(Bots, "Bot", "HideInScenes", 0, "Hide in cutscenes",
                "Bots disappear while you play a scripted sequence and come back after it. Which kinds of scenes is set on the Expert page."));
            add(Key(Bots, "Bot", "Key", 0, "Send away and call back",
                "Sends the bots away and calls them back.", null));
            add(Key(Bots, "Bot", "CallKey", 0, "Call key",
                "Brings the bots to you at once. It also ends Hold and Go there.", null));
            add(Key(Bots, "Bot", "FollowHoldKey", 97, "Follow / hold key",
                "The bots hold the spot they stand on; pressed again they follow you.", null));
            add(Key(Bots, "Bot", "GoThereKey", 98, "Go there key",
                "The bots walk to the point under your crosshair (up to 40 m) and hold there until you press follow.", null));
            add(Key(Bots, "Bot", "AttackTargetKey", 99, "Attack target key",
                "The necromorph under your crosshair is every bot's target for 20 s or until it dies.", null));
            add(Key(Bots, "Bot", "FireHoldKey", 100, "Hold fire key",
                "Holding fire, the bots only fight what attacks them or you. Pressed again: fire at will.", null));
            add(Key(Bots, "Bot", "VanguardKey", 101, "Attack formation key",
                "Switches between cover (bots behind you) and attack (bots go ahead and close in).", null));
            add(B(Bots, "Bot", "Kinesis", 0, "Kinesis throws",
                "In a fight a bot now and then grabs a loose object with kinesis and throws it at its target. Experimental.")
                .Under("Fighting"));
            add(B(Bots, "Bot", "InfiniteAmmo", 0, "Infinite ammo",
                "Every gun a bot carries keeps three clips in reserve. It still has to reload."));
            add(B(Bots, "Bot", "HumanAim", 1, "Human aim",
                "A short pause before the first shot at a new target, an aim that settles and a little wander while tracking. Off: the aim goes straight to the target."));
            add(B(Bots, "Bot", "HumanSenses", 1, "Human senses",
                "A bot sees enemies only inside its view cone with a clear line, and hears them within a few metres. Off: it sees every enemy in its line of sight at once."));
            add(N(Bots, "Bot", "HealBelowPercent", 60, 0, 100, "Heal below (%)",
                "A bot uses a med pack below this much health. A bot's profile can set its own value."));
            add(B(Bots, "Bot", "NoBotFriendlyFire", 1, "No friendly fire between bots",
                "A bot's shots, swings and stomps never hurt another bot. You can still be hit by a bot."));
            add(B(Bots, "Bot", "PlayerCollision", 1, "Bots collide with you",
                "On: you and the bots bump into each other as usual. Off: Isaacs walk through one another."));
            add(B(Bots, "Bot", "Suit", 0, "Own suits",
                "Each bot wears a suit you own (worn or in your safe), its favourites first and none that you or another bot wears. Off: the bots wear your suit.")
                .Under("Gear and credits"));
            add(B(Bots, "Bot", "Bench", 0, "Upgrade at the bench",
                "Power nodes a bot picks up are its own; when you leave a bench it spends them on its guns. The upgrade trees are shared, so your guns of that kind gain too."));
            add(B(Bots, "Bot", "Store", 0, "Shop at the store",
                "Credits a bot picks up are its own; when you leave a store it buys med packs and ammo for its guns."));
            add(B(Bots, "Bot", "UseKiosks", 0, "Walk to the kiosks",
                "After you leave a bench or a store, the bots walk up and use it one at a time with the real animation. Off: they buy straight away."));
            add(B(Bots, "Bot", "BuyGuns", 0, "Buy guns",
                "At a store visit each bot buys one gun the story has unlocked, with its own credits and its favourites first."));
            add(C(Bots, "Bot", "CreditShare", 0, 0, 200, "Credit share (%)",
                "Each bot gets this percent of every credit find of yours, and you get this percent of every find of a bot. 0 = off."));
            add(B(Bots, "Bot", "PerSaveInventory", 1, "Keep bot gear per save",
                "Each campaign save keeps the bots' credits, power nodes, guns and suit in DS2BotSaves next to the plugins, and a death restart rolls them back like you. Off: they keep it for the session only."));
            add(B(Bots, "Bot", "AutoTeleport", 0, "Catch up by teleport",
                "Bots far from you, left outside your lift or behind after a scene are put next to you.")
                .Under("Following and looting"));
            add(B(Bots, "Bot", "StompCorpses", 1, "Stomp corpses for loot",
                "After a fight the bots stomp fresh corpses near them and you until the loot drops, and pick it up. A corpse you stand at is yours."));
            add(B(Bots, "Bot", "LootContainers", 1, "Open crates and lockers",
                "When calm, the bots open supply crates and lockers near you and pick up what drops. Locked ones and those the level needs stay shut."));

            // ---- Survival
            add(B(Survival, "Survival", "Enabled", 0, "Survival waves",
                "The wave director behind every wave mode, the Versus AI slashers and the campaign arenas. It runs only on the survival map below and needs the cross-level enemy pool (Loading page).")
                .Under("Waves")
                .Warn(m =>
                {
                    if (m.On("Survival.Enabled"))
                        return m.On("DS2Randomizer.GlobalTemplates") ? null
                            : "Every survival map loads empty while the cross-level enemy pool (Loading page) is off. Turn it on.";
                    return m.On("Menu.Enabled")
                        ? "Off: no map in the main menu's map picker gets waves, and Versus no AI slashers. Turn it on, or turn Survival in the main menu off." : null;
                }));
            add(T(Survival, "Survival", "Map", "mp_bombRun", "Survival map",
                "Where the waves run: any for every multiplayer map, the Outbreak maps and the campaign arenas, or one of mp_bombRun, mp_detonate, mp_escape, mp_gov, mp_solar, pdlc_mp_mall (Concourse), pdlc_mp_school (Academy), arena_ch01 (tripod room), arena_ch03 (church crypt), arena_ch03b (church sanctuary), arena_ch04, arena_ch05 (Sprawl hub), arena_ch08 (Ishimura), arena_ch09 (mining entry hall), arena_ch10 and arena_ch11 (Marker floor). Versus ignores it. The best wave is kept per map.",
                SurvivalMap)
                .Warn(MapWarning));
            add(C(Survival, "Survival", "StartDelaySeconds", 20, 0, 600, "First wave after (s)",
                "The first wave starts this long after you arrive. 0: only with the start key."));
            add(Key(Survival, "Survival", "StartKey", 0, "Start key",
                "Starts the first wave at once or skips a break.", v => v < 0 ? 0 : v > 255 ? 255 : v));
            add(C(Survival, "Survival", "MaxAlive", 15, 1, 30, "Enemies alive at once",
                "For one player; every further player adds more (Escalation, Expert page). Capped at 10 while the larger pathfinding memory is off.")
                .Warn(m => m.Int("Survival.MaxAlive") > 10 && m.Int("DS2Randomizer.NavPowerPoolKiB") <= 576
                    ? "Capped at 10: Pathfinding memory (Loading page) is at the retail 576 KiB or less. Set it to 4608." : null));
            add(C(Survival, "Survival", "PauseSeconds", 20, 0, 600, "Break between waves (s)",
                "The pause between two waves."));
            add(B(Survival, "Survival", "VentSpawns", 1, "Enemies from vents",
                "Enemies may also crawl out of the map's vents.")
                .Warn(m => m.On("Survival.VentSpawns") && !m.On("DS2Randomizer.VentRebinding")
                    ? "Without Vent rebinding (Enemies & Families page) every survival enemy comes on foot. Turn it on." : null));
            add(C(Survival, "Escalation", "FromWave", 11, 1, 1000, "Escalation from wave",
                "From this wave on every wave gets harder: tougher, faster and more enhanced enemies, shorter breaks and smaller rewards. Wave 10 is the boss wave."));
            add(B(Survival, "Menu", "Enabled", 0, "Survival in the main menu",
                "Multiplayer in the main menu becomes survival: it opens the map picker without the EA login, and host co-op and join co-op play it online.")
                .Under("Main menu and modes")
                .Warn(MenuWarning));
            add(B(Survival, "Menu", "Modes", 1, "Game modes in the map picker",
                "Up and down in the map picker switch between survival, mercenaries, objective, hold the zone and versus. Off: every visit plays survival.")
                .Warn(m =>
                {
                    string director = m.On("Menu.Modes") && m.On("Menu.Enabled") ? DirectorOff(m) : null;
                    return director == null ? null : "Survival, mercenaries, objective and hold the zone have no enemies, and versus no AI slashers, while " + director;
                }));
            add(C(Survival, "Menu", "Mode", 0, 0, 4, "Starting mode",
                "The mode the map picker starts with: 0 survival, 1 mercenaries, 2 objective, 3 hold the zone, 4 versus.")
                .Adjust(v => v < 0 || v > 4 ? 0 : v)
                .Hint(m =>
                {
                    if (!m.On("Menu.Enabled") || !m.On("Menu.Modes"))
                        return "Ignored while " + (m.On("Menu.Enabled") ? "Game modes in the map picker" : "Survival in the main menu") + " is off: every visit plays survival.";
                    long v = m.Int("Menu.Mode");
                    return v >= 0 && v < Modes.Length ? "Starts with " + Modes[v] + "." : null;
                })
                .Warn(m => m.On("Menu.Enabled") && m.On("Menu.Modes") && m.Int("Menu.Mode") == 4 && !m.On("Versus.Enabled")
                    ? "Starts with versus, but Versus (Versus page) is off, so that pick loads a map with no round. Turn Versus on, or start with another mode." : null));
            add(B(Survival, "Menu", "ExtraMaps", 1, "Outbreak maps",
                "The map picker also lists Concourse and Academy when DS2DAT12.DAT is in the game folder. Co-op partners need the same file."));
            add(B(Survival, "Arena", "Arenas", 1, "Campaign arenas",
                "The recorded campaign arenas in the survival map picker. An arena keeps its rooms loaded and locks the doors out of it.")
                .Warn(ArenaWarning));
            add(B(Survival, "Arena", "Modes", 1, "Arenas in every mode",
                "An arena plays Mercenaries or Hold the Zone when picked (Objective and Versus play survival there), a death respawns inside the arena and co-op partners join the arena with the host. Off: survival only, a death reloads the arena."));
            add(C(Survival, "Mercenaries", "StartSeconds", 150, 30, 900, "Mercenaries: starting clock (s)",
                "The time you start with. Kills and time capsules add to it; when it reaches zero the run is over.")
                .Under("Mercenaries and Hold the Zone"));
            add(C(Survival, "Mercenaries", "KillTarget", 150, 0, 10000, "Mercenaries: kills to clear",
                "This many kills end the run early as a clear, and every second left scores a bonus. 0 = only the clock."));
            add(C(Survival, "Mercenaries", "Capsules", 2, 0, 20, "Mercenaries: capsules at once",
                "How many time capsules stand on the map at once. A capsule adds time when picked up and moves elsewhere when nobody takes it. 0 = none."));
            add(C(Survival, "Mercenaries", "CapsuleTotal", 8, 0, 200, "Mercenaries: capsules per run",
                "How many capsules a run gets in all. 0 = no limit."));
            add(C(Survival, "Mercenaries", "CapsuleMinMeters", 20, 0, 150, "Mercenaries: capsule distance (m)",
                "Capsules appear at least this far from every player, at the farther spots."));
            add(C(Survival, "Mercenaries", "BonusFadeSeconds", 480, 0, 3600, "Mercenaries: bonus fade (s)",
                "Every time bonus (kills, combos, capsules) falls evenly from full at the start to the bonus floor below after this long, and stays there. 0 = bonuses never shrink.")
                .Warn(Endless("Mercenaries")));
            add(C(Survival, "Mercenaries", "BonusFloorPercent", 20, 0, 100, "Mercenaries: bonus floor (%)",
                "What is left of each time bonus once the fade is over. 100 = no fade."));
            add(MaxRun("Mercenaries", 900, "Mercenaries: longest run (s)"));
            add(C(Survival, "Zone", "CaptureSeconds", 30, 3, 300, "Hold the Zone: capture time (s)",
                "Seconds to fill a zone alone; every further player inside, bots too, makes it faster."));
            add(C(Survival, "Zone", "TargetZones", 0, 0, 1000, "Hold the Zone: zones to win",
                "0 plays until the clock runs out or the longest run below is reached."));
            add(C(Survival, "Zone", "BonusFadeSeconds", 600, 0, 3600, "Hold the Zone: bonus fade (s)",
                "The time a held zone adds falls evenly from full at the start to the bonus floor below after this long, and stays there, so the clock wins in the end. 0 = it never shrinks.")
                .Warn(Endless("Zone")));
            add(C(Survival, "Zone", "BonusFloorPercent", 20, 0, 100, "Hold the Zone: bonus floor (%)",
                "What is left of the zone bonus once the fade is over. 100 = no fade."));
            add(MaxRun("Zone", 720, "Hold the Zone: longest run (s)"));
            add(B(Survival, "Zone", "OpenObjectives", 1, "Hold the Zone: open the MP maps",
                "On the multiplayer maps every objective is completed on arrival, so all doors open; the last one never ends the round. Set it alike on all machines."));
            add(B(Survival, "Shop", "Enabled", 0, "Supply terminal",
                "A shop drawn over the game wherever survival runs, paid with your own credits: supplies, guns, upgrades and suits. Experimental.")
                .Under("Shop, respawn, music and status line"));
            add(Key(Survival, "Shop", "Key", 86, "Terminal key",
                "Opens and closes the terminal.", v => v <= 0 || v > 255 ? 86 : v));
            add(B(Survival, "Shop", "DuringWaves", 0, "Shop during waves",
                "Off: the terminal opens only before the first wave and between waves."));
            add(B(Survival, "Respawn", "Enabled", 0, "Respawn on multiplayer maps",
                "Isaac comes back at a spawn point instead of the level restarting. When every player is down, the run starts over from wave 1. Experimental."));
            add(C(Survival, "Respawn", "Lives", 1, 0, 99, "Lives per wave",
                "Respawns per wave when you play solo; every cleared wave fills them up again.")
                .Adjust(v => v < 0 || v > 99 ? 1 : v));
            add(B(Survival, "SurvivalMusic", "Enabled", 0, "Wave music",
                "Music while a wave, a Mercenaries, Objective or Zone run or a Versus round is on: the original multiplayer tracks when the map has them, else the campaign's action music, which grows with the danger. A calm piece plays between the waves."));
            add(B(Survival, "Cleanup", "PickupCap", 1, "Item limit",
                "Above the limit the oldest drops on the map fade away, never ones near a player or bot and never capsules. Hundreds of items cost frame rate."));
            add(C(Survival, "Cleanup", "MaxPickups", 40, 5, 400, "Items on the map",
                "How many drops may lie around before the oldest fade away.")
                .Adjust(v => v < 5 || v > 400 ? 40 : v));
            add(B(Survival, "Hud", "Enabled", 1, "Status line",
                "Wave, enemies left and the break's countdown as one small line at the top of the screen, instead of RIG notices."));
            add(T(Survival, "Hud", "Color", "stock", "Status line colour",
                "stock, red, orange, yellow, lime, green, blue, purple, pink, white, gray or a hex value like FF8A2A.",
                v => HudColor(v, false)));
            add(T(Survival, "Hud", "FlashColor", "", "Event line colour",
                "The short event line under the status line, with the same choices. Empty: the status line's colour.",
                v => HudColor(v, true)));

            // ---- Versus
            add(B(Versus, "Versus", "Enabled", 1, "Versus",
                "Humans against necromorph players, picked in the map picker like the other modes. The host splits the teams; necromorph players start each round as the ghost and pick a vent. Experimental.")
                .Under("Match")
                .Warn(m =>
                {
                    if (m.On("Versus.Enabled"))
                        return m.On("Necro.Enabled") ? null : "Necromorph players (below) is off, so the necro team spawns as Isaac. Turn it on.";
                    return m.On("Menu.Enabled") && m.On("Menu.Modes")
                        ? "Off: the map picker still offers versus, and picking it loads a map with no round and no AI slashers. Turn it on unless nobody picks versus." : null;
                }));
            add(B(Versus, "Necro", "Enabled", 0, "Necromorph players",
                "The necro team plays necromorphs: each necro player picks a type and comes out of a vent. Off: the necro team spawns as Isaac.")
                .Warn(m => !m.On("Necro.Enabled") && m.On("Versus.Enabled")
                    ? "Off: in Versus the necro team spawns as Isaac, so every round is Isaac against Isaac. Turn it on." : null));
            add(C(Versus, "Versus", "Humans", 0, 0, 8, "Humans per round",
                "0 = half the players, rounded up.").Adjust(v => v > 8 || v < 0 ? 0 : v));
            add(B(Versus, "Versus", "Rounds", 1, "Two rounds",
                "Two rounds with the sides swapped; the host ends each round."));
            add(N(Versus, "Versus", "SwapSeconds", 12, 0, 600, "Side swap after (s)",
                "Seconds from the end of a round to the side swap. Values below 3 are raised to 3.")
                .Adjust(v => v >= 0 && v < 3 ? 3 : v)
                .Warn(m => m.Int("Versus.SwapSeconds") < 3 ? "The plugin raises this to 3." : null));
            add(B(Versus, "Versus", "Inventory", 0, "RIG inventory",
                "On keeps the inventory usable. Off is the multiplayer rule: no inventory, Tab is the scoreboard, and guns, heal and stasis stay on their quick keys."));
            add(Key(Versus, "Versus", "ScoreKey", 9, "Scoreboard key",
                "Hold it for the scoreboard.", null));
            add(B(Versus, "Versus", "CombatText", 1, "Combat text",
                "The points over each scoring event."));
            add(B(Versus, "Versus", "MpLoot", 1, "Multiplayer loot",
                "The AI slashers drop what the maps' own slashers do: universal ammo, health kits and stasis batteries, no credits."));
            add(B(Versus, "Bot", "VersusDeaths", 1, "Bots can die in Versus",
                "In Versus the bots can die and come back after a short delay at a spawn point near their team. Everywhere else they never die."));
            add(N(Versus, "VersusPoints", "HumanKill", 100, -100000, 100000, "Points per kill (human)",
                "Points a human player scores for a kill.").Under("Points"));
            add(N(Versus, "VersusPoints", "NecroKill", 100, -100000, 100000, "Points per kill (necromorph)",
                "Points a necromorph player scores for a kill."));
            add(N(Versus, "VersusPoints", "Objective", 200, -100000, 100000, "Points per objective",
                "Points for completing an objective."));
            add(N(Versus, "VersusPoints", "TeamKill", -100, -100000, 100000, "Points for a team kill",
                "Normally negative: the penalty for killing a teammate."));
            add(B(Versus, "VersusSlashers", "Enabled", 1, "AI slashers",
                "The host keeps AI slashers on the map in place of the map's own, with the map's skins. More super slashers come while the humans dominate.")
                .Under("AI slashers")
                .Warn(m =>
                {
                    string director = m.On("VersusSlashers.Enabled") && m.On("Versus.Enabled") ? DirectorOff(m) : null;
                    if (director == null) return null;
                    return "Versus rounds get no AI slashers" + (m.Flag("MpMap.BlockMpSlashers", true) ? " (the maps' own stay blocked)" : "") + " while " + director;
                }));
            add(C(Versus, "VersusSlashers", "Alive", 5, 0, 12, "AI slashers alive",
                "How many at once. Single maps can have their own count (AlivePerMap, Expert page)."));
            add(B(Versus, "NativeLobby", "Enabled", 0, "Pre-game lobby",
                "The game's own lobby screen for host co-op, with every player's level and signal; in Versus the humans fill the top rows. Experimental.")
                .Under("Lobby and character setup")
                .Warn(m => m.On("NativeLobby.Enabled") && !m.On("Menu.Enabled") ? "Needs Survival in the main menu (Survival page)." : null));
            add(B(Versus, "NativeLobby", "LaunchButton", 1, "Host LAUNCH button",
                "The host starts the match with LAUNCH on the lobby's help bar (click) or select (A, Enter). Off: select only."));
            add(C(Versus, "NativeLobby", "LaunchSeconds", 3, 1, 10, "Launch countdown (s)",
                "Seconds from the host's launch to the start."));
            add(C(Versus, "NativeLobby", "AutoStartSeconds", 0, 0, 5999, "Start by itself after (s)",
                "0: the lobby waits until the host launches. Above 0: the countdown also starts by itself once enough players are in."));
            add(C(Versus, "NativeLobby", "MinPlayers", 2, 1, 8, "Players to start",
                "With a start by itself: the countdown starts once this many players are in."));
            add(B(Versus, "Loadout", "Enabled", 0, "Character setup",
                "The original multiplayer weapon and suit select for Versus and Mercenaries, from the Multiplayer screen and the lobby. The pick is kept in DS2Profile.ini. Experimental.")
                .Warn(m => m.On("Loadout.Enabled") && !m.On("Menu.Enabled") ? "Needs Survival in the main menu (Survival page)." : null));
            add(B(Versus, "Loadout", "Unlocks", 1, "Everything unlocked",
                "On: level 60 with every item. Off: the locks follow your own XP, which Versus matches add."));
            add(B(Versus, "Loadout", "SpecialSuits", 1, "Special suits",
                "Unlocks the collector's edition, Dr Pepper, viral and developer suits."));
            add(B(Versus, "Loadout", "RivetGun", 1, "Rivet Gun",
                "Unlocks the Rivet Gun."));
            add(B(Versus, "MpMap", "Enabled", 0, "Multiplayer map visit",
                "Plays a multiplayer map in single player: press the key below twice within 3 seconds in the campaign, and again on the map to go back to your checkpoint. Survival, the main menu entries and Versus build on it. Back up your save first.")
                .Under("Multiplayer map visit")
                .Warn(WhenCoopHalfOff("MpMap.Enabled", "The visit key does nothing")));
            add(Key(Versus, "MpMap", "Key", 110, "Visit key",
                "Press it twice. Numpad keys need NumLock on.", v => v < 0 || v > 255 ? 0 : v));
            add(T(Versus, "MpMap", "Map", "mp_bombRun", "Visit map",
                "The map the key loads: mp_bombRun, mp_detonate, mp_escape, mp_gov or mp_solar.",
                v => v.Length == 0 ? "Enter a map name." : Validators.PlainText(v, 63)));
        }
    }
}
