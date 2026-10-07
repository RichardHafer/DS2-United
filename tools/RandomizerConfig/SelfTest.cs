using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace DS2RandomizerConfig
{
    // DS2RandomizerConfig.exe --selftest <copy of an ini>
    // Edits the copy through the same save path as the window and checks byte
    // by byte that only the edited values changed. Writes <ini>.selftest.txt.
    static class SelfTest
    {
        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int processId);

        static StringWriter report;
        static int failures;

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
            try { Execute(path); }
            catch (Exception ex) { failures++; Log("  FAIL  exception: " + ex); }
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
            Log("  bom=" + before.HasBom + " lines=" + before.Lines.Count + " newline=" + Escape(before.NewLine) +
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
            byte[] written;
            var none = IniFile.Save(path, again, out backup, out written);
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

        static void SaveAndVerify(string path, Model model, byte[] original)
        {
            var expected = new Dictionary<string, string>(model.Current);
            string backup;
            byte[] written;
            var changes = IniFile.Save(path, model, out backup, out written);
            foreach (var c in changes) Log("    wrote " + c);
            Check(changes.Count > 0, changes.Count + " changes written");
            Check(backup != null && File.Exists(backup) && IniFile.SameBytes(File.ReadAllBytes(backup), original),
                "backup " + (backup == null ? "(none)" : Path.GetFileName(backup)) + " equals the file before saving");
            byte[] now = File.ReadAllBytes(path);
            var before = IniDocument.Parse(original);
            var after = IniDocument.Parse(now);
            Check(before.HasBom == after.HasBom, "BOM " + (before.HasBom ? "kept" : "still absent"));
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
