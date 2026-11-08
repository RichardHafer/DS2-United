// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace DS2CoopConfig
{
    sealed class MergeSource
    {
        public readonly string Name;
        public readonly byte[] Bytes;

        public MergeSource(string name, byte[] bytes) { Name = name; Bytes = bytes; }
    }

    sealed class MergeResult
    {
        public IniDocument Doc;
        public string Problem;
        public string Path;
        public int Keys;
        public readonly List<string> Sources = new List<string>();     // one line per file
        public readonly List<string> Merged = new List<string>();      // sections found in two files
        public readonly List<string> Duplicates = new List<string>();  // keys found in two files
        public readonly List<string> Notes = new List<string>();       // anything else worth a look
        public readonly Dictionary<string, string> SectionFile = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    // Builds DS2United.ini from the per-plugin files. Each file's sections
    // follow in the order of Legacy, lines and comments as they were. A section
    // an earlier file already has is merged into it, because Windows reads only
    // the first section of a name; a key both files have keeps the first value.
    static class Merger
    {
        public const string FileName = "DS2United.ini";
        // the merged file's name before the rename to Dead Space 2 United (November 2026)
        public const string PreviousName = "DS2Coop.ini";
        // the merged file's name until 17.10.2026
        public const string OldName = "HaferSprawl.ini";
        // the co-op half's own DS2Coop.ini from before the merge, kept under this name by a migration
        public const string KeptCoopName = "DS2Coop.legacy.ini";
        public static readonly string[] Legacy = { "DS2Randomizer.ini", "DS2Coop.ini", "DS2Survival.ini", "DS2BotProfiles.ini" };

        static string Purpose(string file)
        {
            switch (file.ToLowerInvariant())
            {
                case "ds2randomizer.ini": return "the randomizer (DS2RandomizerObserver.asi)";
                case "ds2coop.ini": return "co-op, bots, survival maps and versus (DS2United.asi)";
                case "ds2survival.ini": return "survival waves and modes (DS2RandomizerObserver.asi)";
                case "ds2botprofiles.ini": return "companion bot personalities (DS2United.asi)";
                default: return "settings";
            }
        }

        public static bool IsLegacyName(string name)
        {
            return Legacy.Any(n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase));
        }

        // DS2Coop.ini is the merged file once it holds the randomizer's section; before the
        // merge the co-op half had its own file of that name (the plugins decide the same way).
        public static bool IsMerged(string path)
        {
            if (!File.Exists(path)) return false;
            try
            {
                return IniDocument.Parse(File.ReadAllBytes(path)).SectionNames().Any(s => string.Equals(s, "DS2Randomizer", StringComparison.OrdinalIgnoreCase));
            }
            catch (Exception) { return false; }
        }

        // The merged file of a folder: DS2United.ini, else a merged DS2Coop.ini or a HaferSprawl.ini
        // from before the renames, copied to DS2United.ini so the plugin and this program use the
        // same file from now on. The old file stays as it was. Null without any of them.
        public static string MergedIn(string folder)
        {
            if (folder == null || !Directory.Exists(folder)) return null;
            string merged = System.IO.Path.Combine(folder, FileName);
            if (IsMerged(merged)) return merged;
            string previous = System.IO.Path.Combine(folder, PreviousName);
            if (IsMerged(previous)) return Adopt(previous, merged);
            string old = System.IO.Path.Combine(folder, OldName);
            return File.Exists(old) ? Adopt(old, merged) : null;
        }

        static string Adopt(string source, string target)
        {
            try
            {
                File.Copy(source, target, false);
                return target;
            }
            catch (Exception) { return source; }
        }

        // Where a migration left an old file: the co-op half's DS2Coop.ini moves to KeptCoopName.
        public static string SourcePathAfter(string folder, string name)
        {
            string kept = System.IO.Path.Combine(folder, KeptCoopName);
            return string.Equals(name, PreviousName, StringComparison.OrdinalIgnoreCase) && File.Exists(kept) ? kept : System.IO.Path.Combine(folder, name);
        }

        public static List<string> LegacyFilesIn(string folder)
        {
            var found = new List<string>();
            if (folder == null || !Directory.Exists(folder)) return found;
            foreach (var name in Legacy)
            {
                string path = System.IO.Path.Combine(folder, name);
                if (File.Exists(path) && !(string.Equals(name, PreviousName, StringComparison.OrdinalIgnoreCase) && IsMerged(path))) found.Add(name);
            }
            return found;
        }

        public static MergeResult Build(IList<MergeSource> sources, bool banners)
        {
            var r = new MergeResult();
            var parsed = new List<IniDocument>();
            foreach (var s in sources)
            {
                var d = IniDocument.Parse(s.Bytes);
                if (d.Problem != null) { r.Problem = s.Name + ": " + d.Problem; return r; }
                parsed.Add(d);
            }
            var doc = new IniDocument();
            if (parsed.Any(d => d.Wide != null)) doc.MakeWide();
            string nl = parsed.Count > 0 ? parsed[0].NewLine : "\r\n";
            Action<string> add = t => doc.Lines.Add(new IniLine(t, nl));
            if (banners)
            {
                add(";======================================================================");
                add("; Dead Space 2 United by Hafer");
                add(";======================================================================");
                add(";");
                add("; Every setting of Dead Space 2 United plugins in one file. DS2UnitedConfig.exe");
                add("; edits it, a text editor works too. Restart the game after editing.");
                add("; While this file exists the plugins read only this one, not");
                add("; DS2Randomizer.ini, DS2Coop.ini, DS2Survival.ini or DS2BotProfiles.ini.");
                add("; Made from " + string.Join(", ", sources.Select(s => s.Name).ToArray()) + " on " +
                    DateTime.Now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".");
            }
            for (int f = 0; f < sources.Count; f++)
            {
                string file = sources[f].Name;
                var src = parsed[f];
                if (banners)
                {
                    if (doc.Lines.Count > 0 && doc.Lines[doc.Lines.Count - 1].Text.Trim().Length > 0) add("");
                    add(";======================================================================");
                    add("; From " + file + ": " + Purpose(file));
                    add(";======================================================================");
                    add("");
                }
                var own = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var mergedHere = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var blockKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                string target = null;
                int insertAt = -1, sections = 0, keys = 0;
                bool inSection = false;
                foreach (var line in src.Lines)
                {
                    string text = line.Text, eol = line.Eol.Length > 0 ? line.Eol : nl;
                    string name = IniDocument.SectionName(text);
                    if (name != null)
                    {
                        inSection = true;
                        sections++;
                        string owner;
                        if (!own.Contains(name) && !mergedHere.Contains(name) && r.SectionFile.TryGetValue(name, out owner))
                        {
                            mergedHere.Add(name);
                            target = name;
                            blockKeys.Clear();
                            insertAt = doc.ContentEnd(doc.FindSection(name));
                            var lead = new List<IniLine>();
                            if (banners)
                            {
                                lead.Add(new IniLine("", nl));
                                lead.Add(new IniLine("; From " + file + ":", nl));
                            }
                            doc.Lines.InsertRange(insertAt, lead);
                            insertAt += lead.Count;
                            doc.Touch();
                            r.Merged.Add("[" + name + "] of " + owner + " and " + file + " merged into one section");
                            continue;
                        }
                        // a second section of the same name in one file stays as it was
                        if (own.Contains(name) || mergedHere.Contains(name))
                            r.Notes.Add(file + " has [" + name + "] twice; the second one stays as it was (the plugins read only the first)");
                        target = null;
                        own.Add(name);
                        if (!r.SectionFile.ContainsKey(name)) r.SectionFile[name] = file;
                        doc.Lines.Add(new IniLine(text, eol));
                        continue;
                    }
                    string key = IniDocument.KeyName(text);
                    if (key != null && !inSection)
                    {
                        r.Notes.Add(file + ": " + key + " stands before any section, so no plugin read it; commented out");
                        text = "; " + text.TrimStart();
                        key = null;
                    }
                    if (key != null) keys++;
                    if (target != null)
                    {
                        if (key != null && !blockKeys.Contains(key) && doc.FindKey(target, key) >= 0)
                        {
                            r.Duplicates.Add("[" + target + "] " + key + " is in " + r.SectionFile[target] + " and " + file +
                                ": the first value (" + doc.GetRaw(target, key) + ") is kept, the second line is commented out");
                            text = "; " + text.TrimStart();
                        }
                        else if (key != null) blockKeys.Add(key);
                        doc.Lines.Insert(insertAt++, new IniLine(text, eol));
                        doc.Touch();
                        continue;
                    }
                    doc.Lines.Add(new IniLine(text, eol));
                }
                r.Keys += keys;
                r.Sources.Add(file + ": " + sections + " section" + (sections == 1 ? "" : "s") + ", " + keys + " key" + (keys == 1 ? "" : "s"));
            }
            doc.Touch();
            r.Doc = doc;
            return r;
        }

        // The value each key had where a plugin read it, first file first.
        static Dictionary<string, KeyValuePair<string, string>> Expected(IList<MergeSource> sources)
        {
            var expected = new Dictionary<string, KeyValuePair<string, string>>(StringComparer.OrdinalIgnoreCase);
            foreach (var s in sources)
            {
                var d = IniDocument.Parse(s.Bytes);
                foreach (var section in d.SectionNames())
                    foreach (var key in d.KeyNames(section))
                    {
                        string id = section + "\u0000" + key;
                        if (!expected.ContainsKey(id)) expected[id] = new KeyValuePair<string, string>(s.Name, d.GetRaw(section, key));
                    }
            }
            return expected;
        }

        // Every key keeps the value a plugin read before, and nothing new turns up.
        public static List<string> Verify(IList<MergeSource> sources, IniDocument merged)
        {
            var problems = new List<string>();
            var expected = Expected(sources);
            int count = 0;
            foreach (var pair in expected)
            {
                string[] parts = pair.Key.Split('\u0000');
                string now = merged.GetRaw(parts[0], parts[1]);
                count++;
                if (now != pair.Value.Value)
                    problems.Add("[" + parts[0] + "] " + parts[1] + " from " + pair.Value.Key + ": was \"" + pair.Value.Value + "\", now " + (now == null ? "missing" : "\"" + now + "\""));
            }
            foreach (var section in merged.SectionNames())
                foreach (var key in merged.KeyNames(section))
                    if (!expected.ContainsKey(section + "\u0000" + key)) problems.Add("[" + section + "] " + key + " is new in the merged file");
            return problems;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern uint GetPrivateProfileStringW(string section, string key, string def, char[] buffer, uint size, string file);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern uint GetPrivateProfileSectionNamesW(char[] buffer, uint size, string file);

        static string[] Split(char[] buffer, uint length)
        {
            return new string(buffer, 0, (int)length).Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries);
        }

        // Asks Windows itself, as the plugins do: every key of every old file must
        // read the same from the merged file, except a duplicate the merge reported.
        public static List<string> ProfileCheck(string folder, IList<string> names, string merged, out int compared, string keptCoop = null)
        {
            var problems = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var buffer = new char[1 << 16];
            var value = new char[1 << 16];
            compared = 0;
            foreach (var name in names)
            {
                string old = keptCoop != null && string.Equals(name, FileName, StringComparison.OrdinalIgnoreCase) ? keptCoop : System.IO.Path.Combine(folder, name);
                foreach (var section in Split(buffer, GetPrivateProfileSectionNamesW(buffer, (uint)buffer.Length, old)))
                {
                    var keys = Split(buffer, GetPrivateProfileStringW(section, null, "", buffer, (uint)buffer.Length, old));
                    foreach (var key in keys)
                    {
                        string a = new string(value, 0, (int)GetPrivateProfileStringW(section, key, "\u0001", value, (uint)value.Length, old));
                        string b = new string(value, 0, (int)GetPrivateProfileStringW(section, key, "\u0001", value, (uint)value.Length, merged));
                        compared++;
                        bool first = seen.Add(section + "\u0000" + key);
                        if (a != b && first) problems.Add("[" + section + "] " + key + " from " + name + ": Windows reads \"" + a + "\" there and \"" + b + "\" from " + FileName);
                    }
                }
            }
            return problems;
        }

        // Writes DS2United.ini into folder. The old files are only read.
        public static MergeResult Migrate(string folder)
        {
            string target = System.IO.Path.Combine(folder, FileName);
            string already = MergedIn(folder);
            if (already != null) throw new IOException(System.IO.Path.GetFileName(already) + " already exists in " + folder + ".");
            string kept = File.Exists(target) ? System.IO.Path.Combine(folder, KeptCoopName) : null;
            if (kept != null && File.Exists(kept)) throw new IOException(KeptCoopName + " already exists in " + folder + "; move it away first.");
            var names = LegacyFilesIn(folder);
            if (names.Count == 0) throw new FileNotFoundException("None of " + string.Join(", ", Legacy) + " is in " + folder + ".");
            var sources = names.Select(n => new MergeSource(n, File.ReadAllBytes(System.IO.Path.Combine(folder, n)))).ToList();
            var r = Build(sources, true);
            if (r.Problem != null) throw new InvalidDataException(r.Problem);
            byte[] bytes = r.Doc.ToBytes();
            var problems = Verify(sources, IniDocument.Parse(bytes));
            if (problems.Count > 0)
                throw new InvalidDataException("The merged file would not keep every value:\n" + string.Join("\n", problems.Take(8).ToArray()));
            string temp = target + IniFile.TempMarker + Guid.NewGuid().ToString("N").Substring(0, 8);
            using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush(true);
            }
            if (kept != null)
            {
                try { File.Move(target, kept); }
                catch (Exception) { try { File.Delete(temp); } catch (Exception) { } throw; }
            }
            try { File.Move(temp, target); }
            catch (Exception)
            {
                try { File.Delete(temp); } catch (Exception) { }
                if (kept != null) try { File.Move(kept, target); } catch (Exception) { }
                throw;
            }
            int compared;
            problems = ProfileCheck(folder, names, target, out compared, kept);
            if (problems.Count > 0)
            {
                // nothing may read a file that differs from what the plugins used so far
                try { File.Delete(target); } catch (Exception) { }
                if (kept != null) try { File.Move(kept, target); } catch (Exception) { }
                throw new InvalidDataException("Windows reads some values differently from the merged file, so it was removed again:\n" +
                    string.Join("\n", problems.Take(8).ToArray()));
            }
            r.Path = target;
            if (kept != null) r.Notes.Add("the co-op half's own " + FileName + " is kept as " + KeptCoopName);
            r.Notes.Add("Windows reads all " + compared + " values of the old files the same from " + FileName);
            return r;
        }
    }
}
