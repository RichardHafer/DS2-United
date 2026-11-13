// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace DS2CoopConfig
{
    // DS2UnitedConfig.exe --selftest <copy of DS2Coop.ini>
    // Edits the copy through the same save path as the window and checks byte
    // by byte that only the edited values changed. Backups, failed saves,
    // UTF-16, the migration and the other file checks run on scratch copies
    // in %TEMP%. Writes <ini>.selftest.txt.
    static class SelfTest
    {
        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int processId);

        static StringWriter report;
        static int failures;
        static string scratch;

        static void Log(string text) { report.WriteLine(text); }

        static void Check(bool ok, string what)
        {
            if (!ok) failures++;
            Log((ok ? "  ok    " : "  FAIL  ") + what);
        }

        public static int Run(string[] args)
        {
            AttachConsole(-1);
            report = new StringWriter();
            failures = 0;
            if (args.Length < 2 || !File.Exists(args[1]))
            {
                Console.WriteLine("usage: DS2UnitedConfig.exe --selftest <copy-of-DS2Coop.ini>");
                return 2;
            }
            string path = Path.GetFullPath(args[1]);
            // never on a live install: the copy must not sit in a game folder
            string dir = Path.GetDirectoryName(path);
            string parent = Path.GetDirectoryName(dir);
            if (File.Exists(Path.Combine(dir, "deadspace2.exe")) || (parent != null && File.Exists(Path.Combine(parent, "deadspace2.exe"))))
            {
                Console.WriteLine("refused: " + path + " is inside a Dead Space 2 folder. Run the self-test on a copy.");
                return 2;
            }
            scratch = Path.Combine(Path.GetTempPath(), "DS2CoopConfig-selftest-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            // The phases save, back up and reset the file they get, so they get a copy: run on the
            // repository's template they once left seeds, appended keys and reset values in it.
            byte[] given = File.ReadAllBytes(path);
            try
            {
                string work = Path.Combine(scratch, "given");
                Directory.CreateDirectory(work);
                string copy = Path.Combine(work, Path.GetFileName(path));
                File.WriteAllBytes(copy, given);
                Execute(copy);
                ScratchPhases(File.ReadAllBytes(copy));
                MergedFilePhases(given);
            }
            catch (Exception ex) { failures++; Log("  FAIL  exception: " + ex); }
            finally
            {
                try { if (Directory.Exists(scratch)) Directory.Delete(scratch, true); }
                catch (Exception ex) { Log("  note  scratch folder not removed: " + ex.Message); }
            }
            Check(IniFile.SameBytes(given, File.ReadAllBytes(path)), "the given file is untouched");
            Log(failures == 0 ? "RESULT: PASS" : "RESULT: FAIL (" + failures + ")");
            string text = report.ToString();
            File.WriteAllText(Path.Combine(Path.GetTempPath(), "DS2UnitedConfig-selftest.txt"), text);
            Console.Write(text);
            return failures == 0 ? 0 : 1;
        }

        static Model Fresh(byte[] bytes) { return FromDoc(IniDocument.Parse(bytes)); }

        // As the window builds it: the fixed pages plus the Expert entries of this file.
        static Model FromDoc(IniDocument doc)
        {
            var model = new Model(Schema.Build(), Model.LoadRecommended());
            var taken = new HashSet<string>(model.Options.Select(o => o.Id), StringComparer.OrdinalIgnoreCase);
            model.SetGenerated(ExpertKeys.Build(doc, taken));
            model.LoadFrom(doc);
            return model;
        }

        static string Toggle(string v) { return v == "1" ? "0" : "1"; }

        static void Execute(string path)
        {
            byte[] original = File.ReadAllBytes(path);
            var before = IniDocument.Parse(original);
            Log("self-test " + path);
            Log("  encoding=" + before.EncodingName + " lines=" + before.Lines.Count + " newline=" + Escape(before.NewLine) +
                " options=" + Fresh(original).Options.Count);

            // Phase 1: a handful of edits, existing and missing keys, one missing section at most
            var model = Fresh(original);
            var edits = new Dictionary<string, string>();
            Func<string, string> cur = id => model.Loaded[id];
            edits["DS2Randomizer.Seed"] = (long.Parse(cur("DS2Randomizer.Seed")) + 1).ToString();
            edits["DS2Randomizer.Chance"] = cur("DS2Randomizer.Chance") == "50" ? "100" : "50";
            edits["DS2Randomizer.CrossFamily"] = Toggle(cur("DS2Randomizer.CrossFamily"));
            edits["DS2Randomizer.NavPowerPoolKiB"] = cur("DS2Randomizer.NavPowerPoolKiB") == "8192" ? "4096" : "8192";
            edits["DS2Randomizer.VariantBias"] = "selftest_variant";
            edits["Weights.SwarmEnemy"] = cur("Weights.SwarmEnemy") == "55" ? "56" : "55";
            edits["Loot.AmmoPercent"] = cur("Loot.AmmoPercent") == "150" ? "151" : "150";
            edits["VentFamilies.Lurker"] = Toggle(cur("VentFamilies.Lurker"));
            edits["MoreEnemies.Zombie"] = cur("MoreEnemies.Zombie") == "2" ? "3" : "2";
            edits["BenchRandomizer.Seed"] = "1234";
            edits["SchematicRandomizer.FirstIsWeapon"] = Toggle(cur("SchematicRandomizer.FirstIsWeapon"));
            foreach (var pair in edits)
            {
                Check(model.ById[pair.Key].Check(pair.Value) == null, "edit is valid: " + pair.Key + "=" + pair.Value);
                model.Set(model.ById[pair.Key], pair.Value);
            }
            Log("phase 1: " + edits.Count + " edits");
            SaveAndVerify(path, model, original);

            // Phase 2: saving again without edits writes nothing
            byte[] afterOne = File.ReadAllBytes(path);
            var again = Fresh(afterOne);
            string backup;
            var none = IniFile.Save(path, again, out backup);
            Log("phase 2: save without edits");
            Check(none.Count == 0 && backup == null, "no changes reported, no backup made");
            Check(IniFile.SameBytes(afterOne, File.ReadAllBytes(path)), "file bytes untouched");

            // Phase 3: reset all to recommended (seeds kept), as the button does
            var reset = Fresh(afterOne);
            reset.ResetAll();
            Log("phase 3: reset all to recommended (" + reset.ChangeCount + " values differ)");
            SaveAndVerify(path, reset, afterOne);

            // Phase 4: an optional seed back to "same as main seed" comments its line out
            byte[] afterThree = File.ReadAllBytes(path);
            var follow = Fresh(afterThree);
            follow.Set(follow.ById["BenchRandomizer.Seed"], "");
            Log("phase 4: bench seed follows the main seed again");
            SaveAndVerify(path, follow, afterThree);
            var lines = IniDocument.Parse(File.ReadAllBytes(path)).Lines;
            Check(lines.Any(l => l.Text == "; Seed=1234"), "line reads \"; Seed=1234\"");

            // Phase 5, in memory only: a kind without its own line follows Default.
            // Default moves away and the kind is set back to the old value; the
            // kind has to be written, or the game reads the new Default for it.
            var doc = IniDocument.Parse(File.ReadAllBytes(path));
            doc.CommentOut("MoreEnemies", "Zombie");
            var follower = FromDoc(doc);
            string old = follower.Current["MoreEnemies.Default"];
            string moved = old == "2" ? "3" : "2";
            Log("phase 5: Default " + old + " -> " + moved + ", Zombie (no line of its own) set back to " + old);
            follower.Set(follower.ById["MoreEnemies.Default"], moved);
            Check(follower.Current["MoreEnemies.Zombie"] == moved, "Zombie follows Default to " + moved);
            follower.Set(follower.ById["MoreEnemies.Zombie"], old);
            follower.ApplyTo(doc);
            var reread = Fresh(doc.ToBytes());
            Check(reread.Loaded["MoreEnemies.Default"] == moved && reread.Loaded["MoreEnemies.Zombie"] == old,
                "after saving, Default reads " + reread.Loaded["MoreEnemies.Default"] + " and Zombie " + reread.Loaded["MoreEnemies.Zombie"]);
        }

        static string Scratch(string name, byte[] bytes)
        {
            string file = Path.Combine(scratch, name);
            File.WriteAllBytes(file, bytes);
            return file;
        }

        static bool SamePath(string a, string b) { return string.Equals(a, b, StringComparison.OrdinalIgnoreCase); }

        static string[] TempFiles() { return Directory.GetFiles(scratch, "*" + IniFile.TempMarker + "*"); }

        // A model on the file with the main seed one higher.
        static Model SeedBumped(string file)
        {
            var m = Fresh(File.ReadAllBytes(file));
            string seed = m.Loaded["DS2Randomizer.Seed"];
            m.Set(m.ById["DS2Randomizer.Seed"], seed == "4294967295" ? "0" : (long.Parse(seed) + 1).ToString());
            return m;
        }

        static void ScratchPhases(byte[] original)
        {
            Log("scratch folder " + scratch);

            // Phase 6: at most two backups, the newest ones, and only this INI's
            Log("phase 6: backups are pruned to " + IniFile.BackupsKept);
            string ini = Scratch(Program.IniName, original);
            string[] old = { ".bak-20200101-000000", ".bak-20200101-000000-2", ".bak-20200101-000000-10", ".bak-20200102-000000" };
            foreach (var suffix in old) File.WriteAllBytes(ini + suffix, original);
            string other = Scratch("Other.ini.bak-20200101-000000", original);
            string notes = Scratch(Program.IniName + ".bak-notes", original);
            var order = IniFile.Backups(ini).Select(f => Path.GetFileName(f)).ToArray();
            string wanted = string.Join(",", new[] { old[3], old[2], old[1], old[0] }.Select(s => Program.IniName + s).ToArray());
            Check(string.Join(",", order) == wanted, "backups sort newest first: " + string.Join(", ", order));
            var made = new List<string>();
            string seedBefore = Fresh(original).Loaded["DS2Randomizer.Seed"];
            for (int i = 0; i < 4; i++)
            {
                byte[] before = File.ReadAllBytes(ini);
                string backup;
                var changes = IniFile.Save(ini, SeedBumped(ini), out backup);
                made.Add(backup);
                Check(changes.Count == 1 && backup != null && IniFile.SameBytes(File.ReadAllBytes(backup), before),
                    "save " + (i + 1) + ": backup " + Path.GetFileName(backup) + " holds the file before it");
            }
            var left = IniFile.Backups(ini);
            Check(left.Count == 2 && SamePath(left[0], made[3]) && SamePath(left[1], made[2]),
                "the two newest backups remain: " + string.Join(", ", left.Select(f => Path.GetFileName(f)).ToArray()));
            Check(File.Exists(other) && File.Exists(notes), "backups of other files and look-alike names are untouched");
            // a pruned gap in this second must not give the next backup a lower number
            string planted = ini + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture) + "-5";
            File.WriteAllBytes(planted, original);
            string next = IniFile.BackupPath(ini);
            File.WriteAllBytes(next, original);
            Check(SamePath(IniFile.Backups(ini)[0], next), "the next backup name sorts newest: " + Path.GetFileName(next));
            File.Delete(planted);
            File.Delete(next);

            // Phase 7: the save goes through a temp file, a failed save leaves the file as it was
            Log("phase 7: atomic save");
            Check(TempFiles().Length == 0, "no temp file left after saving");
            long expectedSeed = (long.Parse(seedBefore) + 4) % 4294967296L;
            string seedNow = Fresh(File.ReadAllBytes(ini)).Loaded["DS2Randomizer.Seed"];
            Check(seedNow == expectedSeed.ToString(), "the file holds the last saved seed (" + seedNow + ")");
            byte[] beforeFail = File.ReadAllBytes(ini);
            int backupsBefore = IniFile.Backups(ini).Count;
            SaveFailure failure = null;
            // a reader that does not share delete or write access blocks the replace
            using (new FileStream(ini, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                try { string b; IniFile.Save(ini, SeedBumped(ini), out b); }
                catch (SaveFailure f) { failure = f; }
            }
            Check(failure != null, "saving onto a locked file fails" + (failure != null ? ": " + failure.Message.Trim() : ""));
            Check(failure != null && failure.FileIntact && failure.BackupPath == null, "the failure says the file was not changed");
            Check(IniFile.SameBytes(beforeFail, File.ReadAllBytes(ini)), "the file is unchanged, byte for byte");
            Check(TempFiles().Length == 0, "the temp file was removed");
            Check(IniFile.Backups(ini).Count == backupsBefore, "no extra backup left behind");

            // Phase 8: rows the window does not show survive a save
            Log("phase 8: hidden family rows are kept");
            var hiddenDoc = IniDocument.Parse(original);
            var shown = Fresh(original);
            int listed = 0;
            foreach (var section in new[] { "Sources", "Targets", "VentFamilies" })
                foreach (var fam in Schema.HiddenFamilies)
                {
                    if (shown.ById.ContainsKey(section + "." + fam)) listed++;
                    if (hiddenDoc.GetRaw(section, fam) == null) hiddenDoc.SetValue(section, fam, "0", true, null);
                }
            Check(listed == 0, "no option for " + string.Join(", ", Schema.HiddenFamilies) + " (" + listed + " found)");
            string hiddenIni = Scratch("hidden.ini", hiddenDoc.ToBytes());
            var hiddenBefore = new Dictionary<string, string>();
            foreach (var section in new[] { "Sources", "Targets", "VentFamilies" })
                foreach (var fam in Schema.HiddenFamilies) hiddenBefore[section + "." + fam] = hiddenDoc.GetRaw(section, fam);
            var resetHidden = Fresh(File.ReadAllBytes(hiddenIni));
            foreach (var o in resetHidden.Options)
                if (o.Table == "Sources" || o.Table == "Targets" || o.Table == "VentFamilies") resetHidden.Set(o, resetHidden.Current[o.Id] == "1" ? "0" : "1");
            string hb;
            IniFile.Save(hiddenIni, resetHidden, out hb);
            var hiddenAfter = IniDocument.Parse(File.ReadAllBytes(hiddenIni));
            int lost = hiddenBefore.Count(p => hiddenAfter.GetRaw(p.Key.Split('.')[0], p.Key.Split('.')[1]) != p.Value);
            Check(lost == 0, hiddenBefore.Count + " hidden rows unchanged after flipping every shown row");

            // Phase 9: a value above the box range is shown clamped, noted and kept
            Log("phase 9: InstallDelayMs above the box maximum");
            var delayDoc = IniDocument.Parse(original);
            delayDoc.SetValue("DS2Randomizer", "InstallDelayMs", "900000", true, null);
            string delayIni = Scratch("delay.ini", delayDoc.ToBytes());
            var delay = Fresh(File.ReadAllBytes(delayIni));
            var delayOpt = delay.ById["DS2Randomizer.InstallDelayMs"];
            string note = delay.FileNote(delayOpt);
            Check(delay.Loaded[delayOpt.Id] == "900000", "the model reads 900000");
            Check(note != null && note.Contains("900000") && note.Contains(delayOpt.Max.ToString()), "file note: " + note);
            Look.Init(0f);
            var editor = Editor.Create(delayOpt);
            editor.Show(delay.Current[delayOpt.Id]);
            Check(editor.Control.Text.Trim() == delayOpt.Max.ToString() && editor.Read() == "900000",
                "the box shows " + editor.Control.Text.Trim() + " and still reads 900000 until edited");
            var delayEdit = SeedBumped(delayIni);
            string db;
            IniFile.Save(delayIni, delayEdit, out db);
            string rawAfter = IniDocument.Parse(File.ReadAllBytes(delayIni)).GetRaw("DS2Randomizer", "InstallDelayMs");
            Check(rawAfter == "900000", "saving another change keeps InstallDelayMs=" + rawAfter);

            // Phase 10: UTF-16 files read and save like the byte file, encoding and BOM kept
            string text = Encoding.UTF8.GetString(original, IniDocument.Parse(original).HasBom ? 3 : 0, original.Length - (IniDocument.Parse(original).HasBom ? 3 : 0));
            foreach (bool bigEndian in new[] { false, true })
            {
                string label = bigEndian ? "UTF-16 BE" : "UTF-16 LE";
                Log("phase 10: " + label + " copy");
                var enc = new UnicodeEncoding(bigEndian, true);
                byte[] wide = enc.GetPreamble().Concat(enc.GetBytes(text)).ToArray();
                // the UTF-8 twin gets the same edits, the saved texts must match
                var narrowBytes = Encoding.UTF8.GetBytes(text);
                string narrow = Scratch("narrow-" + (bigEndian ? "be" : "le") + ".ini", narrowBytes);
                string wideIni = Scratch("wide-" + (bigEndian ? "be" : "le") + ".ini", wide);
                var doc = IniDocument.Parse(wide);
                Check(doc.EncodingName == label && doc.Problem == null, "read as " + doc.EncodingName);
                var a = Fresh(narrowBytes);
                var b = Fresh(wide);
                int differ = a.Options.Count(o => a.Loaded[o.Id] != b.Loaded[o.Id]);
                Check(differ == 0, "all " + a.Options.Count + " options read the same as from UTF-8 (" + differ + " differ)");
                foreach (var m in new[] { a, b })
                {
                    m.Set(m.ById["DS2Randomizer.Chance"], m.Loaded["DS2Randomizer.Chance"] == "50" ? "100" : "50");
                    m.Set(m.ById["DS2Randomizer.VariantBias"], "selftest_wide");
                    m.Set(m.ById["Loot.CreditsPercent"], m.Loaded["Loot.CreditsPercent"] == "150" ? "151" : "150");
                }
                string nb;
                IniFile.Save(narrow, a, out nb);
                SaveAndVerify(wideIni, b, wide);
                byte[] saved = File.ReadAllBytes(wideIni);
                byte[] bom = enc.GetPreamble();
                Check(saved.Length >= 2 && saved[0] == bom[0] && saved[1] == bom[1], label + " BOM kept");
                string wideText = enc.GetString(saved, 2, saved.Length - 2);
                string narrowText = Encoding.UTF8.GetString(File.ReadAllBytes(narrow));
                Check(wideText == narrowText, "saved text equals the UTF-8 save");
            }

            // Phase 11: broken UTF-16 is refused before anything is written
            Log("phase 11: broken UTF-16 is refused");
            var leEnc = new UnicodeEncoding(false, true);
            byte[] broken = leEnc.GetPreamble().Concat(leEnc.GetBytes(text)).Concat(new byte[] { 0x41 }).ToArray();
            string brokenIni = Scratch("broken.ini", broken);
            Check(IniDocument.Parse(broken).Problem != null, "odd byte count is reported as a problem");
            SaveFailure refused = null;
            try { string rb; IniFile.Save(brokenIni, SeedBumped(brokenIni), out rb); }
            catch (SaveFailure f) { refused = f; }
            Check(refused != null && refused.FileIntact, "saving is refused" + (refused != null ? ": " + refused.Message : ""));
            Check(IniFile.SameBytes(broken, File.ReadAllBytes(brokenIni)) && IniFile.Backups(brokenIni).Count == 0, "file untouched, no backup made");
        }

        static void MergedFilePhases(byte[] given)
        {
            // Phase 12: the templates are built in and every curated key is in one of them
            Log("phase 12: templates and curated pages");
            Check(Templates.Missing.Count == 0, "all templates are built in" + (Templates.Missing.Count > 0 ? ", missing: " + string.Join(", ", Templates.Missing.ToArray()) : ""));
            var schema = Schema.Build();
            foreach (var group in new[] { Curated.Coop, Curated.Bots, Curated.Survival, Curated.Multiplayer })
            {
                var keys = schema.Where(o => o.Group == group).ToList();
                var absent = keys.Where(o => !Templates.Has(o.Section, o.Key)).Select(o => o.Id).ToList();
                Check(absent.Count == 0, group + ": " + keys.Count + " keys, all in the templates" + (absent.Count > 0 ? "; missing: " + string.Join(", ", absent.ToArray()) : ""));
                Log("        " + string.Join(", ", keys.Select(o => o.Id).ToArray()));
                var bare = keys.Where(o => string.IsNullOrEmpty(o.Description)).Select(o => o.Id).ToList();
                Check(bare.Count == 0, group + ": every key has a description" + (bare.Count > 0 ? "; not: " + string.Join(", ", bare.ToArray()) : ""));
            }
            var ids = schema.Select(o => o.Id).ToList();
            Check(ids.Distinct(StringComparer.OrdinalIgnoreCase).Count() == ids.Count, "no key is on two fixed pages");
            var model = new Model(schema, Model.LoadRecommended());
            var differ = schema.Where(o => o.Group != Schema.Expert && Array.IndexOf(new[] { Curated.Coop, Curated.Bots, Curated.Survival, Curated.Multiplayer }, o.Group) >= 0 &&
                model.Recommended[o.Id] != o.Effective(Templates.Doc.GetRaw(o.Section, o.Key), o.CodeDefault)).Select(o => o.Id).ToList();
            Check(differ.Count == 0, "curated recommendations are the template values");
            var codeDiffers = schema.Where(o => Array.IndexOf(new[] { Curated.Coop, Curated.Bots, Curated.Survival, Curated.Multiplayer }, o.Group) >= 0 &&
                o.DefaultFrom == null && model.Recommended[o.Id] != o.CodeDefault).Select(o => o.Id + "=" + model.Recommended[o.Id] + " (code " + o.CodeDefault + ")").ToList();
            Log("  note  template differs from the plugin default for " + codeDiffers.Count + " curated keys: " + string.Join(", ", codeDiffers.ToArray()));

            MigrationPhases();

            // Phase 14: every key of the file is on exactly one page
            Log("phase 14: Expert page covers the file");
            var clock = System.Diagnostics.Stopwatch.StartNew();
            var full = Fresh(given);
            long buildMs = clock.ElapsedMilliseconds;
            var doc = IniDocument.Parse(given);
            int inFile = 0, hiddenRows = 0;
            var uncovered = new List<string>();
            foreach (var section in doc.SectionNames())
                foreach (var key in doc.KeyNames(section))
                {
                    inFile++;
                    if (Schema.Hidden(section, key)) hiddenRows++;
                    else if (!full.ById.ContainsKey(section + "." + key)) uncovered.Add("[" + section + "] " + key);
                }
            Check(uncovered.Count == 0, inFile + " keys in the file, " + hiddenRows + " hidden family rows, the rest on a page" +
                (uncovered.Count > 0 ? "; not shown: " + string.Join(", ", uncovered.Take(10).ToArray()) : ""));
            var gen = full.Generated.ToList();
            var genIds = gen.Select(o => o.Id).ToList();
            Check(genIds.Distinct(StringComparer.OrdinalIgnoreCase).Count() == genIds.Count, gen.Count + " Expert rows, none twice");
            var overlap = genIds.Where(id => ids.Contains(id, StringComparer.OrdinalIgnoreCase)).ToList();
            Check(overlap.Count == 0, "no Expert row repeats a key of another page" + (overlap.Count > 0 ? ": " + string.Join(", ", overlap.ToArray()) : ""));
            int notSet = gen.Count(o => full.Current[o.Id] == Option.NotSet);
            int unknown = gen.Count(o => o.Description == ExpertKeys.UnknownNote);
            int noted = gen.Count(o => o.Description.Length > 0 && o.Description != ExpertKeys.UnknownNote);
            Log("  note  " + notSet + " template keys not in the file, " + unknown + " keys in no template, " + noted + " with a template note; built in " + buildMs + " ms");
            var kinds = new Dictionary<string, Kind>
            {
                { "Coop.Port", Kind.Int }, { "Mirror.Classes", Kind.Text }, { "Coop.AudioTrace", Kind.Bool }, { "Necro.GhostSpeed", Kind.Int },
                { "Bot1.Antics", Kind.Int }, { "Bot1.HarmfulAntics", Kind.Bool }, { "Survival.RewardMedPackSize", Kind.Int }, { "Bot1.Name", Kind.Text },
                { "Gadgets.StasisMineEnabled", Kind.Bool }, { "Bot1.AimSway", Kind.Text }, { "Wave1.Enemies", Kind.Text }, { "Escalation.PauseStepSeconds", Kind.Text },
            };
            foreach (var pair in kinds)
            {
                Option o;
                if (!full.ById.TryGetValue(pair.Key, out o)) { Log("  skip  " + pair.Key + " is not on the Expert page of this file"); continue; }
                Check(o.Kind == pair.Value, pair.Key + " is a " + o.Kind + (o.Kind == pair.Value ? "" : ", expected " + pair.Value));
            }
            Option sample;
            if (full.ById.TryGetValue("Bot1.ReactionMaxMs", out sample))
                Check(sample.Description.Length > 0, "[Bot1] ReactionMaxMs is described from the profile legend: " + Short(sample.Description));
            if (full.ById.TryGetValue("Coop.PeerTimeoutStallGraceMs", out sample))
                Check(sample.Description.Length > 0, "[Coop] PeerTimeoutStallGraceMs takes its group's note: " + Short(sample.Description));
            if (full.ById.TryGetValue("Coop.TickMode", out sample))
                Check(sample.Description.Length == 0, "[Coop] TickMode does not borrow StartKey's note");

            RoundTrip(given);
            WarningPhases(given);
        }

        static string Short(string text) { return text.Length > 70 ? text.Substring(0, 67) + "..." : text; }

        static string WarningOf(Model m, string id)
        {
            Option o;
            return m.ById.TryGetValue(id, out o) && o.WarnFunc != null ? o.WarnFunc(m) : null;
        }

        // The templates as a fresh install's file, with "Section.Key=value" edits;
        // "!Section.Key" drops that line first, so the plugin's default applies.
        static Model Setup(string edits)
        {
            var doc = IniDocument.Parse(Templates.Doc.ToBytes());
            var sets = new List<string[]>();
            foreach (var edit in edits.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
            {
                if (edit[0] == '!')
                {
                    string[] id = edit.Substring(1).Split('.');
                    doc.CommentOut(id[0], id[1]);
                }
                else sets.Add(edit.Split(new[] { '=' }, 2));
            }
            var m = FromDoc(doc);
            foreach (var s in sets) m.Set(m.ById[s[0]], s[1]);
            return m;
        }

        // needle null: the row must stay quiet
        static void Expect(string edits, string id, string needle)
        {
            string w = WarningOf(Setup(edits), id);
            bool ok = needle == null ? w == null : w != null && w.Contains(needle);
            Check(ok, (edits.Length == 0 ? "templates" : edits) + " -> " + id + ": " + (w == null ? "no warning" : Short(w.Replace(Environment.NewLine, " / "))));
        }

        // Phase 16: a setting that breaks a mode says what breaks and how to fix it.
        static void WarningPhases(byte[] given)
        {
            Log("phase 16: dependency warnings");
            Check(!Schema.Build().Any(o => o.Key.Equals("FollowIntoTubes", StringComparison.OrdinalIgnoreCase)), "no page row for the removed [Bot] FollowIntoTubes");
            string[] quiet = {
                "DS2Randomizer.Enabled", "DS2Randomizer.GlobalTemplates", "DS2Randomizer.VentRebinding", "Survival.Enabled", "Survival.Map",
                "Survival.MaxAlive", "Menu.Enabled", "Menu.Modes", "Menu.Mode", "Arena.Arenas", "Necro.Enabled", "Necro.Invasion",
                "MpMap.Enabled", "Bot.Enabled", "Coop.Network", "Coop.JoinAddress", "Coop.SuitSync",
                "Coop.MirrorDeath", "Coop.PeerTimeoutMs", "Coop.Enabled", "Coop.HostNPCs", "Coop.MPUniformStreams", "Coop.SyncAllObjects",
                "Coop.Peer", "Coop.DeathSync" };
            foreach (var id in quiet) Expect("", id, null);

            // the survival director: Survival waves and the pool, not the master switch
            Expect("Survival.Enabled=0", "Menu.Enabled", "Survival waves (Survival page) is off");
            Expect("DS2Randomizer.GlobalTemplates=0", "Menu.Enabled", "cross-level enemy pool (Loading page) is off");
            Expect("Survival.Enabled=0 DS2Randomizer.GlobalTemplates=0", "Menu.Modes", "Turn both on");
            Expect("DS2Randomizer.GlobalTemplates=0", "Arena.Arenas", "locks its doors");
            Expect("Survival.Enabled=0", "Survival.Enabled", "Turn it on");
            Expect("DS2Randomizer.GlobalTemplates=0", "Survival.Enabled", "enemy pool");
            Expect("DS2Randomizer.GlobalTemplates=0", "DS2Randomizer.GlobalTemplates", "Survival needs it too");
            Expect("DS2Randomizer.Enabled=0", "DS2Randomizer.Enabled", "Replacement chance 0 and More enemies off");
            Expect("DS2Randomizer.Enabled=0", "Menu.Enabled", null);
            Expect("DS2Randomizer.Enabled=0 Survival.Enabled=0 Menu.Enabled=0", "DS2Randomizer.Enabled", "nothing in the campaign");

            // survival map
            Expect("Survival.Map=mp_gov", "Survival.Map", "Set it to any");
            Expect("Survival.Map=mp_gov", "Arena.Arenas", "every arena");
            Expect("Survival.Map=arena_ch04", "Arena.Arenas", "every other arena");
            Expect("Survival.Map=mp_typo", "Survival.Map", "No map is called that");
            Expect("!Survival.Map", "Survival.Map", "Only mp_bombRun");
            Expect("Survival.Map=", "Survival.Map", "Only mp_bombRun");
            Expect("Menu.Enabled=0 Survival.Map=mp_gov MpMap.Map=mp_solar", "Survival.Map", "visit key loads mp_solar");
            Expect("Menu.Enabled=0 Survival.Map=MP_SOLAR MpMap.Map=mp_solar", "Survival.Map", null);
            var map = Setup("").ById["Survival.Map"];
            Check(map.Check("ANY") == null && map.Check("arena_ch10") == null && map.Check("pdlc_mp_school") == null && map.Check("mp_typo") != null,
                "Survival map accepts any, the picker's maps and the arenas, and refuses mp_typo");

            // necromorph players and the modes; Versus is not in the release
            Check(!Schema.Build().Any(o => o.Section.StartsWith("Versus", StringComparison.OrdinalIgnoreCase) || o.Key.StartsWith("Versus", StringComparison.OrdinalIgnoreCase)),
                "no page row for Versus");
            Expect("Necro.Enabled=1 Necro.Invasion=0", "Necro.Invasion", "does not follow the humans");
            Expect("Necro.Enabled=0 Necro.Invasion=0", "Necro.Invasion", null);
            Expect("Menu.Mode=4", "Menu.Mode", null);

            // Mercenaries and Hold the Zone: shrinking bonuses or a run limit end every run
            foreach (var id in new[] { "Mercenaries.BonusFadeSeconds", "Mercenaries.MaxRunSeconds", "Zone.BonusFadeSeconds", "Zone.MaxRunSeconds" })
                Expect("", id, null);
            Expect("Mercenaries.BonusFadeSeconds=0 Mercenaries.MaxRunSeconds=0", "Mercenaries.MaxRunSeconds", "go on for ever");
            Expect("Mercenaries.BonusFadeSeconds=0 Mercenaries.MaxRunSeconds=0", "Mercenaries.BonusFadeSeconds", "go on for ever");
            Expect("Zone.BonusFloorPercent=100 Zone.MaxRunSeconds=0", "Zone.BonusFadeSeconds", "go on for ever");
            Expect("Zone.BonusFadeSeconds=0", "Zone.BonusFadeSeconds", null);
            Expect("Mercenaries.MaxRunSeconds=60", "Mercenaries.MaxRunSeconds", "raises this to 120");
            var maxRun = Setup("").ById["Zone.MaxRunSeconds"];
            Check(maxRun.Effective("60", maxRun.CodeDefault) == "120" && maxRun.Effective("0", maxRun.CodeDefault) == "0" && maxRun.CodeDefault == "720",
                "[Zone] MaxRunSeconds reads 60 as 120 and 0 as never, default 720");

            // the co-op half
            Expect("Coop.Enabled=0", "Coop.Enabled", "Set it to 1");
            Expect("Coop.Enabled=0", "Coop.Network", "[Coop] Enabled is 0");
            Expect("!Coop.Enabled", "Menu.Enabled", "whole co-op half");
            Expect("Coop.Enabled=0", "Bot.Enabled", "No bot joins");
            Expect("Coop.Enabled=0", "MpMap.Enabled", "visit key does nothing");
            Expect("Coop.MPUniformStreams=0", "Coop.HostNPCs", "MPUniformStreams=1");
            Expect("Coop.MPUniformStreams=0", "Coop.MPUniformStreams", "Share suits");
            Expect("Coop.MPUniformStreams=0", "Coop.SuitSync", "MPUniformStreams=1");
            Expect("Coop.SyncAllObjects=1", "Coop.SyncAllObjects", "SyncAllObjects=0");
            Expect("Coop.SyncAllObjects=1", "Menu.Enabled", "HostNPCs=1 needs");
            Expect("Coop.Peer=1.2.3.4", "Coop.Peer", "Not ip:port");
            Expect("Coop.Peer=host:27015", "Coop.Network", "Peer");
            Expect("Coop.Peer=1.2.3.4:27015;5.6.7.8:27016", "Coop.Peer", null);
            Expect("Coop.PeerTimeout=0", "Coop.PeerTimeoutMs", "PeerTimeout=1");
            Expect("!Coop.PeerTimeout", "Coop.PeerTimeoutMs", "PeerTimeout=1");
            Expect("Coop.DeathSync=0", "Coop.MirrorDeath", "DeathSync=1");
            Expect("Coop.DeathSync=0", "Coop.DeathSync", "Die together");
            Expect("Coop.JoinAddress=example.com:27015", "Coop.JoinAddress", "refuses");

            // vents and pathfinding memory
            Expect("DS2Randomizer.VentRebinding=0", "DS2Randomizer.VentRebinding", "on foot");
            Expect("DS2Randomizer.VentVariants=0", "DS2Randomizer.VentRebinding", null);
            Expect("DS2Randomizer.VentVariants=0 Survival.Enabled=0", "DS2Randomizer.VentRebinding", "Survival uses it either way");
            Expect("DS2Randomizer.NavPowerPoolKiB=0 Survival.MaxAlive=15", "Survival.MaxAlive", "Capped at 10");

            // every warning and hint copes with a file, the templates, an empty file and every switch flipped
            var flipped = Setup("");
            foreach (var o in flipped.Options.ToList())
                if (o.Kind == Kind.Bool && flipped.Current[o.Id] != Option.NotSet) flipped.Set(o, flipped.Current[o.Id] == "1" ? "0" : "1");
            int evaluated = 0;
            var thrown = new List<string>();
            foreach (var m in new[] { Fresh(given), Setup(""), FromDoc(new IniDocument()), flipped })
                foreach (var o in m.Options)
                    foreach (var f in new[] { o.WarnFunc, o.HintFunc })
                    {
                        if (f == null) continue;
                        try { f(m); evaluated++; }
                        catch (Exception ex) { thrown.Add(o.Id + ": " + ex.GetType().Name); }
                    }
            Check(thrown.Count == 0, evaluated + " warnings and hints evaluated on four files" + (thrown.Count > 0 ? "; threw: " + string.Join(", ", thrown.Take(10).ToArray()) : ""));
        }

        static string WithLine(byte[] bytes, string section, string line)
        {
            var doc = IniDocument.Parse(bytes);
            int h = doc.FindSection(section);
            doc.Lines.Insert(h + 1, new IniLine(line, doc.NewLine));
            doc.Touch();
            return Encoding.GetEncoding(28591).GetString(doc.ToBytes());
        }

        // Phase 13: the old files merged into DS2Coop.ini, on the templates
        // with a duplicate, an unknown section and a stray key added.
        static void MigrationPhases()
        {
            var latin = Encoding.GetEncoding(28591);
            var sources = new Dictionary<string, byte[]>
            {
                { "DS2Randomizer.ini", Model.Resource("Recommended.ini") },
                { "DS2Coop.ini", latin.GetBytes(latin.GetString(Model.Resource("Template.DS2United.ini")) + "\r\n[SelftestCoopOnly]\r\nAlpha=1\r\n") },
                { "DS2Survival.ini", latin.GetBytes(WithLine(Model.Resource("Template.DS2Survival.ini"), "Zone", "Ring=0")) },
                { "DS2BotProfiles.ini", latin.GetBytes("Orphan=1\r\n" + latin.GetString(Model.Resource("Template.DS2BotProfiles.ini"))) },
            };
            foreach (bool wide in new[] { false, true })
            {
                string folder = Path.Combine(scratch, wide ? "migrate-utf16" : "migrate");
                Directory.CreateDirectory(folder);
                Log("phase 13: migration" + (wide ? " with DS2Survival.ini as UTF-16" : ""));
                foreach (var pair in sources)
                {
                    byte[] bytes = pair.Value;
                    if (wide && pair.Key == "DS2Survival.ini")
                    {
                        var enc = new UnicodeEncoding(false, true);
                        bytes = enc.GetPreamble().Concat(enc.GetBytes(latin.GetString(bytes))).ToArray();
                    }
                    File.WriteAllBytes(Path.Combine(folder, pair.Key), bytes);
                }
                var before = Merger.Legacy.ToDictionary(n => n, n => File.ReadAllBytes(Path.Combine(folder, n)));
                MergeResult r;
                try { r = Merger.Migrate(folder); }
                catch (Exception ex) { Check(false, "migration failed: " + ex.Message); continue; }
                foreach (var s in r.Sources) Log("    source " + s);
                foreach (var s in r.Merged) Log("    merged " + s);
                foreach (var s in r.Duplicates) Log("    dup    " + s);
                foreach (var s in r.Notes) Log("    note   " + s);
                Check(File.Exists(r.Path), Path.GetFileName(r.Path) + " written");
                Check(before.All(p => IniFile.SameBytes(p.Value, File.ReadAllBytes(Merger.SourcePathAfter(folder, p.Key)))), "the old files are unchanged");
                Check(Merger.IsMerged(r.Path) && Path.GetFileName(r.Path) == Merger.FileName && File.Exists(Path.Combine(folder, Merger.PreviousName)),
                    "the merged file is " + Merger.FileName + ", the co-op half's own " + Merger.PreviousName + " stays where it was");
                var merged = IniDocument.Parse(File.ReadAllBytes(r.Path));
                Check(merged.EncodingName == (wide ? "UTF-16 LE" : "ANSI/UTF-8"), "merged file is " + merged.EncodingName);
                var parsed = Merger.Legacy.Select(n => new MergeSource(n, before[n])).ToList();
                var problems = Merger.Verify(parsed, merged);
                Check(problems.Count == 0, "every key keeps the value a plugin read" + (problems.Count > 0 ? ": " + string.Join("; ", problems.Take(5).ToArray()) : ""));
                int sourceKeys = 0;
                foreach (var s in parsed)
                {
                    var d = IniDocument.Parse(s.Bytes);
                    foreach (var section in d.SectionNames()) sourceKeys += d.KeyNames(section).Count;
                }
                int mergedKeys = merged.SectionNames().Sum(s => merged.KeyNames(s).Count);
                Check(mergedKeys == sourceKeys - 1, mergedKeys + " keys merged from " + sourceKeys + " (one duplicate)");
                int zoneHeaders = merged.Lines.Count(l => string.Equals(IniDocument.SectionName(l.Text), "Zone", StringComparison.OrdinalIgnoreCase));
                Check(zoneHeaders == 1, "one [Zone] section (" + zoneHeaders + " found)");
                var coopZone = IniDocument.Parse(sources["DS2Coop.ini"]);
                var survZone = IniDocument.Parse(sources["DS2Survival.ini"]);
                int zoneKeys = coopZone.KeyNames("Zone").Count + survZone.KeyNames("Zone").Count - 1;
                Check(merged.KeyNames("Zone").Count == zoneKeys, "[Zone] holds " + merged.KeyNames("Zone").Count + " keys from both files, " + zoneKeys + " expected");
                Check(merged.GetRaw("Zone", "Ring") == coopZone.GetRaw("Zone", "Ring") && merged.GetRaw("Zone", "CaptureSeconds") == survZone.GetRaw("Zone", "CaptureSeconds"),
                    "[Zone] Ring keeps DS2Coop.ini's value, CaptureSeconds DS2Survival.ini's");
                Check(r.Duplicates.Count == 1 && r.Duplicates[0].Contains("Ring") && merged.Lines.Any(l => l.Text == "; Ring=0"), "the duplicate Ring=0 is reported and commented out");
                Check(r.Notes.Any(n => n.Contains("Orphan")) && merged.Lines.Any(l => l.Text == "; Orphan=1"), "the key before any section is reported and commented out");
                Check(merged.GetRaw("SelftestCoopOnly", "Alpha") == "1", "an unknown section is copied as it is");
                foreach (var name in Merger.Legacy)
                    Check(merged.Lines.Any(l => l.Text.StartsWith("; From " + name + ": ")), "header comment for " + name);
                Check(r.Notes.Any(n => n.StartsWith("Windows reads all")), "Windows reads the merged file as the old ones: " + r.Notes.Last());
                bool refused = false;
                try { Merger.Migrate(folder); }
                catch (IOException) { refused = true; }
                Check(refused, "a second migration refuses to overwrite " + Merger.FileName);
            }
        }

        // Phase 15: unknown keys survive, missing keys come back with their template comment.
        static void RoundTrip(byte[] given)
        {
            Log("phase 15: save round trip with unknown and missing keys");
            var doc = IniDocument.Parse(given);
            doc.SetValue("Coop", "SelftestUnknownKey", "keep me", false, (IList<string>)new[] { "; a key no template knows" }, null);
            doc.SetValue("SelftestSection", "Alpha", "1", true, (IList<string>)null, null);
            doc.SetValue("SelftestSection", "Beta", "some text", false, (IList<string>)null, null);
            int line = doc.FindKey("Bot", "HumanAim");
            if (line >= 0) { doc.Lines.RemoveAt(line); doc.Touch(); }
            int turret = doc.FindSection("Turret");
            if (turret >= 0) { doc.Lines.RemoveRange(turret, doc.SectionEnd(turret) - turret); doc.Touch(); }
            string ini = Scratch("roundtrip.ini", doc.ToBytes());
            byte[] before = File.ReadAllBytes(ini);
            var m = Fresh(before);
            Option unknown = null, alpha = null, beta = null, aim = null, range = null;
            Check(m.ById.TryGetValue("Coop.SelftestUnknownKey", out unknown) && unknown.Generated && unknown.Kind == Kind.Text &&
                unknown.Description == ExpertKeys.UnknownNote, "the unknown key is an editable Expert row");
            Check(m.ById.TryGetValue("SelftestSection.Alpha", out alpha) && alpha.Kind == Kind.Bool &&
                m.ById.TryGetValue("SelftestSection.Beta", out beta) && beta.Kind == Kind.Text, "the unknown section's keys are Expert rows");
            Check(m.ById.TryGetValue("Bot.HumanAim", out aim) && m.RawInFile[aim.Id] == null && m.Current[aim.Id] == aim.CodeDefault,
                "[Bot] HumanAim is missing and shows the plugin default " + (aim != null ? aim.CodeDefault : "?"));
            Check(m.ById.TryGetValue("Turret.Range", out range) && m.Current[range.Id] == Option.NotSet, "[Turret] Range is a template row marked not in file");
            if (unknown == null || beta == null || aim == null || range == null) return;
            m.Set(aim, "0");
            m.Set(range, "25");
            m.Set(beta, "changed text");
            m.Set(m.ById["Coop.PauseKeepsRunning"], m.Current["Coop.PauseKeepsRunning"] == "1" ? "0" : "1");
            Option tick;
            if (m.ById.TryGetValue("Coop.NetSendPeriodMs", out tick) && m.Current[tick.Id] != Option.NotSet)
                m.Set(tick, (m.Int(tick.Id) + 1).ToString());
            SaveAndVerify(ini, m, before);
            var after = IniDocument.Parse(File.ReadAllBytes(ini));
            Check(after.GetRaw("Coop", "SelftestUnknownKey") == "keep me" && after.GetRaw("SelftestSection", "Alpha") == "1" &&
                after.GetRaw("SelftestSection", "Beta") == "changed text", "unknown keys kept, the edited one written");
            var comment = Templates.CommentLines("Bot", "HumanAim");
            int at = after.FindKey("Bot", "HumanAim");
            bool commented = comment != null && at >= comment.Count && comment.Select((c, i) => after.Lines[at - comment.Count + i].Text == c).All(b => b);
            Check(commented, "[Bot] HumanAim was added with its " + (comment != null ? comment.Count : 0) + " template comment lines");
            int headers = after.Lines.Count(l => string.Equals(IniDocument.SectionName(l.Text), "Turret", StringComparison.OrdinalIgnoreCase));
            var order = after.SectionNames();
            int pos = order.FindIndex(s => s.Equals("Turret", StringComparison.OrdinalIgnoreCase));
            var tmpl = Templates.SectionOrder;
            int ti = tmpl.FindIndex(s => s.Equals("Turret", StringComparison.OrdinalIgnoreCase));
            string expectedBefore = null;
            for (int i = ti - 1; i >= 0 && expectedBefore == null; i--)
                if (order.Any(s => s.Equals(tmpl[i], StringComparison.OrdinalIgnoreCase))) expectedBefore = tmpl[i];
            Check(headers == 1 && pos > 0 && string.Equals(order[pos - 1], expectedBefore, StringComparison.OrdinalIgnoreCase),
                "[Turret] was created once, right after [" + expectedBefore + "]" + (pos > 0 ? " (after [" + order[pos - 1] + "])" : ""));
            Check(after.GetRaw("Turret", "Range") == "25", "[Turret] Range=25 written");
        }

        static void SaveAndVerify(string path, Model model, byte[] original)
        {
            var expected = new Dictionary<string, string>(model.Current);
            string backup;
            var changes = IniFile.Save(path, model, out backup);
            foreach (var c in changes) Log("    wrote " + c);
            Check(changes.Count > 0, changes.Count + " changes written");
            Check(backup != null && File.Exists(backup) && IniFile.SameBytes(File.ReadAllBytes(backup), original),
                "backup " + (backup == null ? "(none)" : Path.GetFileName(backup)) + " equals the file before saving");
            byte[] now = File.ReadAllBytes(path);
            var before = IniDocument.Parse(original);
            var after = IniDocument.Parse(now);
            Check(before.EncodingName == after.EncodingName, "encoding " + before.EncodingName + " kept");
            Check(IniFile.Backups(path).Count <= IniFile.BackupsKept, IniFile.Backups(path).Count + " backups next to the file, at most " + IniFile.BackupsKept);
            CompareLines(before, after, changes);

            // every option reads back as intended; untouched ones as before
            var reread = FromDoc(after);
            int mismatches = 0;
            foreach (var o in reread.Options)
            {
                string want;
                if (!expected.TryGetValue(o.Id, out want))
                {
                    mismatches++;
                    Log("    new option after saving: " + o.Id);
                    continue;
                }
                if (reread.Loaded[o.Id] == want) continue;
                // an absent key that follows its default reads as that default
                if (reread.RawInFile[o.Id] == null && o.DefaultFrom != null && reread.Loaded[o.Id] == reread.Loaded[o.DefaultFrom]) continue;
                mismatches++;
                Log("    mismatch " + o.Id + ": wanted " + expected[o.Id] + ", reads " + reread.Loaded[o.Id]);
            }
            Check(mismatches == 0, "all " + reread.Options.Count + " options read back as intended");
        }

        // Walks both files line by line. Old lines must all survive in order;
        // a changed line may differ only after its '='; new lines may only be
        // comments, blanks, section headers or added keys.
        static void CompareLines(IniDocument before, IniDocument after, List<string> changes)
        {
            var a = before.Lines;
            var b = after.Lines;
            string nl = before.NewLine;
            int i = 0, valueChanges = 0, inserted = 0, commented = 0, badInsert = 0, badEol = 0, badValue = 0;
            var addedKeys = new List<string>();
            for (int j = 0; j < b.Count; j++)
            {
                var line = b[j];
                if (i < a.Count)
                {
                    var old = a[i];
                    bool eolOk = line.Eol == old.Eol || (old.Eol == "" && i == a.Count - 1);
                    if (line.Text == old.Text && eolOk) { i++; continue; }
                    string oldKey = IniDocument.KeyName(old.Text), newKey = IniDocument.KeyName(line.Text);
                    if (oldKey != null && newKey != null && string.Equals(oldKey, newKey, StringComparison.OrdinalIgnoreCase))
                    {
                        int eq = old.Text.IndexOf('=');
                        int prefix = 0;
                        while (prefix < old.Text.Length && prefix < line.Text.Length && old.Text[prefix] == line.Text[prefix]) prefix++;
                        int suffix = 0;
                        while (suffix < old.Text.Length - prefix && suffix < line.Text.Length - prefix &&
                               old.Text[old.Text.Length - 1 - suffix] == line.Text[line.Text.Length - 1 - suffix]) suffix++;
                        if (prefix <= eq) badValue++;
                        if (!eolOk) badEol++;
                        valueChanges++;
                        i++;
                        continue;
                    }
                    if (oldKey != null && line.Text == "; " + old.Text.TrimStart())
                    {
                        if (!eolOk) badEol++;
                        commented++;
                        i++;
                        continue;
                    }
                }
                inserted++;
                bool lastLine = j == b.Count - 1;
                if (line.Eol != nl && !(lastLine && line.Eol == "")) badEol++;
                string key = IniDocument.KeyName(line.Text);
                if (key != null) addedKeys.Add(key);
                else if (!(line.Text.Length == 0 || line.Text.StartsWith(";") || IniDocument.SectionName(line.Text) != null)) badInsert++;
            }
            Check(i == a.Count, "every original line is still there, in order (" + i + "/" + a.Count + ")");
            Check(badValue == 0, valueChanges + " lines changed only after their '='");
            Check(badEol == 0, "line endings unchanged; added lines use " + Escape(nl));
            Check(badInsert == 0, inserted + " added lines are comments, blanks, headers or keys");
            int expectedAdded = changes.Count(c => c.EndsWith("(added)"));
            int expectedCommented = changes.Count(c => c.Contains("commented out"));
            Check(addedKeys.Count == expectedAdded, addedKeys.Count + " keys added, " + expectedAdded + " expected");
            Check(commented == expectedCommented, commented + " lines commented out, " + expectedCommented + " expected");
            Check(valueChanges == changes.Count - expectedAdded - expectedCommented,
                valueChanges + " values rewritten, " + (changes.Count - expectedAdded - expectedCommented) + " expected");
        }

        static string Escape(string nl) { return nl.Replace("\r", "CR").Replace("\n", "LF"); }
    }
}
