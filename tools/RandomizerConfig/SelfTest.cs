using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace DS2RandomizerConfig
{
    // DS2RandomizerConfig.exe --selftest <copy of an ini>
    // Edits the copy through the same save path as the window and checks byte
    // by byte that only the edited values changed. Backups, failed saves,
    // UTF-16 and the other file checks run on scratch copies in %TEMP%.
    // Writes <ini>.selftest.txt.
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
                Console.WriteLine("usage: DS2RandomizerConfig.exe --selftest <copy-of-DS2Randomizer.ini>");
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
            scratch = Path.Combine(Path.GetTempPath(), "DS2RandomizerConfig-selftest-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            try
            {
                Execute(path);
                Directory.CreateDirectory(scratch);
                ScratchPhases(File.ReadAllBytes(path));
            }
            catch (Exception ex) { failures++; Log("  FAIL  exception: " + ex); }
            finally
            {
                try { if (Directory.Exists(scratch)) Directory.Delete(scratch, true); }
                catch (Exception ex) { Log("  note  scratch folder not removed: " + ex.Message); }
            }
            Log(failures == 0 ? "RESULT: PASS" : "RESULT: FAIL (" + failures + ")");
            string text = report.ToString();
            File.WriteAllText(path + ".selftest.txt", text);
            Console.Write(text);
            return failures == 0 ? 0 : 1;
        }

        static Model Fresh(byte[] bytes)
        {
            var model = new Model(Schema.Build(), Model.LoadRecommended());
            model.LoadFrom(IniDocument.Parse(bytes));
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
            foreach (var o in reset.Options)
                if (!o.KeepOnResetAll) reset.Current[o.Id] = reset.Recommended[o.Id];
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
            var follower = new Model(Schema.Build(), Model.LoadRecommended());
            follower.LoadFrom(doc);
            string old = follower.Current["MoreEnemies.Default"];
            string moved = old == "2" ? "3" : "2";
            Log("phase 5: Default " + old + " -> " + moved + ", Zombie (no line of its own) set back to " + old);
            follower.Set(follower.ById["MoreEnemies.Default"], moved);
            Check(follower.Current["MoreEnemies.Zombie"] == moved, "Zombie follows Default to " + moved);
            follower.Set(follower.ById["MoreEnemies.Zombie"], old);
            follower.ApplyTo(doc);
            var reread = new Model(Schema.Build(), Model.LoadRecommended());
            reread.LoadFrom(IniDocument.Parse(doc.ToBytes()));
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
            var reread = new Model(Schema.Build(), Model.LoadRecommended());
            reread.LoadFrom(after);
            int mismatches = 0;
            foreach (var o in reread.Options)
            {
                if (reread.Loaded[o.Id] == expected[o.Id]) continue;
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
                else if (!(line.Text.Length == 0 || line.Text.StartsWith("; ") || IniDocument.SectionName(line.Text) != null)) badInsert++;
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
