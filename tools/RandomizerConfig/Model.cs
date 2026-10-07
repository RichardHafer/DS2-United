using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;

namespace DS2RandomizerConfig
{
    enum Kind { Bool, Int, OptionalInt, Text }

    // One INI key. Values are kept as canonical strings: "1"/"0" for switches,
    // decimal for numbers, "" for an optional seed that follows the main seed.
    sealed class Option
    {
        public string Group, Section, Key, Title, Description;
        public string Table;          // family table this key belongs to, or null
        public Kind Kind;
        public long Min, Max;
        public string CodeDefault;
        public bool RawRead;          // read with a plain GetPrivateProfileInt (any non-zero is on)
        public bool Unsigned;         // seeds: read as an unsigned 32-bit value
        public bool KeepOnResetAll;
        public string DefaultFrom;    // id whose value applies while the key is absent
        public string NoteText;       // always shown
        public Func<Model, string> WarnFunc;
        public Func<long, long> AdjustFunc;   // the game's own correction of a value
        public Func<string, string> ValidateFunc;
        public string CommentText;    // comment line when the key has to be added

        public string Id { get { return Section + "." + Key; } }

        public Option Note(string text) { NoteText = text; return this; }
        public Option Warn(Func<Model, string> f) { WarnFunc = f; return this; }
        public Option Raw() { RawRead = true; return this; }
        public Option From(string id) { DefaultFrom = id; return this; }
        public Option Keep() { KeepOnResetAll = true; return this; }
        public Option Adjust(Func<long, long> f) { AdjustFunc = f; return this; }
        public Option Validate(Func<string, string> f) { ValidateFunc = f; return this; }
        public Option Comment(string text) { CommentText = text; return this; }

        public bool IsNumeric { get { return Kind != Kind.Text; } }

        public string ShortComment
        {
            get
            {
                if (CommentText != null) return CommentText;
                string d = Description ?? Title;
                int dot = d.IndexOf(". ", StringComparison.Ordinal);
                if (dot > 0) d = d.Substring(0, dot + 1);
                if (d.Length > 110) d = d.Substring(0, 107) + "...";
                return d;
            }
        }

        public string Display(string value)
        {
            if (Kind == Kind.Bool) return value == "1" ? "On" : "Off";
            if (Kind == Kind.OptionalInt && value == "") return "same as main seed";
            if (Kind == Kind.Text && value == "") return "(empty)";
            return value;
        }

        // What the plugin ends up using for a raw INI value.
        public string Effective(string raw, string fallback)
        {
            if (raw == null) return fallback;
            if (Kind == Kind.Text) return raw;
            if (raw.Trim().Length == 0) return fallback;   // GetPrivateProfileInt returns the default for an empty value
            uint parsed = WinInt.Parse(raw);
            if (Kind == Kind.Bool)
                return (RawRead ? parsed != 0 : (int)parsed >= 1) ? "1" : "0";
            if (Kind == Kind.OptionalInt) return parsed.ToString(CultureInfo.InvariantCulture);
            long v = Unsigned ? (long)parsed : (long)(int)parsed;
            v = AdjustFunc != null ? AdjustFunc(v) : Math.Max(Min, Math.Min(Max, v));
            return v.ToString(CultureInfo.InvariantCulture);
        }

        // Error text for a canonical value, or null.
        public string Check(string value)
        {
            if (Kind == Kind.Text) return ValidateFunc != null ? ValidateFunc(value) : null;
            if (Kind == Kind.OptionalInt && value == "") return null;
            long v;
            if (!long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v))
                return "Enter a whole number.";
            if (v < Min || v > Max)
                return "Allowed range: " + Min + " to " + Max + ".";
            return null;
        }
    }

    sealed class Model
    {
        public readonly List<Option> Options;
        public readonly Dictionary<string, Option> ById = new Dictionary<string, Option>(StringComparer.OrdinalIgnoreCase);
        public readonly Dictionary<string, string> Loaded = new Dictionary<string, string>();
        public readonly Dictionary<string, string> Current = new Dictionary<string, string>();
        public readonly Dictionary<string, string> Recommended = new Dictionary<string, string>();
        public readonly Dictionary<string, string> RawInFile = new Dictionary<string, string>();
        public readonly Dictionary<string, string> Invalid = new Dictionary<string, string>();

        public Model(List<Option> options, IniDocument recommended)
        {
            Options = options;
            foreach (var o in options) ById[o.Id] = o;
            var rec = Evaluate(recommended);
            foreach (var pair in rec) Recommended[pair.Key] = pair.Value;
        }

        Dictionary<string, string> Evaluate(IniDocument doc)
        {
            var result = new Dictionary<string, string>();
            foreach (var o in Options)
            {
                string raw = doc != null ? doc.GetRaw(o.Section, o.Key) : null;
                string fallback = o.DefaultFrom != null ? result[o.DefaultFrom] : o.CodeDefault;
                result[o.Id] = o.Effective(raw, fallback);
            }
            return result;
        }

        public void LoadFrom(IniDocument doc)
        {
            var values = Evaluate(doc);
            Loaded.Clear(); Current.Clear(); RawInFile.Clear(); Invalid.Clear();
            foreach (var o in Options)
            {
                RawInFile[o.Id] = doc.GetRaw(o.Section, o.Key);
                Loaded[o.Id] = values[o.Id];
                Current[o.Id] = values[o.Id];
            }
        }

        public string Get(string id) { return Current[id]; }
        public bool On(string id) { return Current[id] == "1"; }

        public long Int(string id)
        {
            long v;
            return long.TryParse(Current[id], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v) ? v : 0;
        }

        public bool IsChanged(Option o) { return Current[o.Id] != Loaded[o.Id]; }

        public int ChangeCount
        {
            get { int n = 0; foreach (var o in Options) if (IsChanged(o)) n++; return n; }
        }

        // Sets a value and carries keys that follow it (absent from the file) along.
        public void Set(Option o, string value)
        {
            string old = Current[o.Id];
            Current[o.Id] = value;
            foreach (var other in Options)
                if (other.DefaultFrom == o.Id && RawInFile[other.Id] == null && Current[other.Id] == old)
                    Current[other.Id] = value;
        }

        // A note when the file holds something the game reads differently.
        public string FileNote(Option o)
        {
            string raw = RawInFile[o.Id];
            if (raw == null || o.Kind == Kind.Text) return null;
            if (raw.Trim().Length == 0)
                return "The file has an empty value; the game uses " + o.Display(Loaded[o.Id]) + ".";
            if (o.Kind == Kind.Bool && (raw.Trim() == "0" || raw.Trim() == "1")) return null;
            long v;
            if (o.Kind == Kind.Int && long.TryParse(Loaded[o.Id], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v) && (v < o.Min || v > o.Max))
                return "The file says " + raw.Trim() + ", outside the range this window offers, so the box shows " + (v > o.Max ? o.Max : o.Min) +
                    ". The game uses " + v + ", and saving keeps it unless you change this value.";
            if (WinInt.IsPlainNumber(raw) && raw.Trim() == Loaded[o.Id]) return null;
            return "The file says \"" + raw + "\"; the game reads it as " + o.Display(Loaded[o.Id]) + ".";
        }

        // Writes every changed value into doc. Returns one line per change.
        public List<string> ApplyTo(IniDocument doc)
        {
            var changes = new List<string>();
            foreach (var o in Options)
            {
                string value = Current[o.Id];
                bool absent = RawInFile[o.Id] == null;
                // An absent key that follows another one reads as that key's new
                // value after the save, so that is what it is compared with. Against
                // the loaded value, a kind set back to the old default was dropped
                // while the default itself moved.
                if (absent && o.DefaultFrom != null)
                {
                    if (value == Current[o.DefaultFrom]) continue;
                }
                else if (value == Loaded[o.Id]) continue;
                if (absent && o.DefaultFrom == null && value == o.CodeDefault) continue;
                if (o.Kind == Kind.OptionalInt && value == "")
                {
                    if (doc.CommentOut(o.Section, o.Key))
                        changes.Add("[" + o.Section + "] " + o.Key + " commented out (follows the main seed)");
                    continue;
                }
                bool existed = doc.FindKey(o.Section, o.Key) >= 0;
                doc.SetValue(o.Section, o.Key, value, o.IsNumeric, o.ShortComment);
                changes.Add("[" + o.Section + "] " + o.Key + "=" + value + (existed ? "" : " (added)"));
            }
            return changes;
        }

        public static IniDocument LoadRecommended()
        {
            var asm = Assembly.GetExecutingAssembly();
            using (var stream = asm.GetManifestResourceStream("DS2RandomizerConfig.Recommended.ini"))
            {
                if (stream == null) return new IniDocument();
                var ms = new MemoryStream();
                stream.CopyTo(ms);
                return IniDocument.Parse(ms.ToArray());
            }
        }
    }

    sealed class SaveFailure : Exception
    {
        public readonly bool FileIntact;
        public readonly string BackupPath;   // the previous version when the file is not intact

        public SaveFailure(string message, bool intact, string backup, Exception inner) : base(message, inner)
        {
            FileIntact = intact;
            BackupPath = backup;
        }
    }

    static class IniFile
    {
        public const int BackupsKept = 2;
        public const string TempMarker = ".saving-";

        public static bool SameBytes(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }

        public static string BackupPath(string path)
        {
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            // above every backup of the same second, even a pruned gap, so it sorts newest
            string prefix = Path.GetFileName(path) + ".bak-" + stamp;
            int next = 1;
            foreach (var file in Backups(path))
            {
                string name = Path.GetFileName(file);
                if (!name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                int n = name.Length == prefix.Length ? 1 : int.Parse(name.Substring(prefix.Length + 1), CultureInfo.InvariantCulture);
                next = Math.Max(next, n + 1);
            }
            string backup = next == 1 ? path + ".bak-" + stamp : path + ".bak-" + stamp + "-" + next;
            while (File.Exists(backup)) backup = path + ".bak-" + stamp + "-" + ++next;
            return backup;
        }

        // Applies the model's changes to the file as it is on disk now. The new
        // text goes to a temp file in the same folder that then replaces the file,
        // so a failure leaves the old file in place. Returns the changes;
        // backupPath is null when nothing was written.
        public static List<string> Save(string path, Model model, out string backupPath)
        {
            backupPath = null;
            byte[] current = File.ReadAllBytes(path);
            var doc = IniDocument.Parse(current);
            if (doc.Problem != null) throw new SaveFailure(doc.Problem, true, null, null);
            var changes = model.ApplyTo(doc);
            if (changes.Count == 0) return changes;
            byte[] output = doc.ToBytes();
            string backup = null, temp = null;
            try
            {
                backup = BackupPath(path);
                File.WriteAllBytes(backup, current);
                temp = path + TempMarker + Guid.NewGuid().ToString("N").Substring(0, 8);
                using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    stream.Write(output, 0, output.Length);
                    stream.Flush(true);
                }
                Replace(temp, path);
            }
            catch (Exception ex)
            {
                TryDelete(temp);
                bool intact = false;
                try { intact = File.Exists(path) && SameBytes(File.ReadAllBytes(path), current); }
                catch (Exception) { }
                // the backup only matters when the file itself was damaged
                if (intact) { TryDelete(backup); backup = null; }
                throw new SaveFailure(ex.Message, intact, backup, ex);
            }
            backupPath = backup;
            PruneBackups(path, backup);
            return changes;
        }

        static void Replace(string temp, string path)
        {
            try
            {
                File.Replace(temp, path, null);
                return;
            }
            // ReplaceFile is not supported on every file system
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            catch (PlatformNotSupportedException) { }
            if (File.Exists(path)) File.Delete(path);
            File.Move(temp, path);
        }

        static void TryDelete(string file)
        {
            if (file == null) return;
            try { if (File.Exists(file)) File.Delete(file); }
            catch (Exception) { }
        }

        // Backups of this INI, newest first: <name>.bak-yyyyMMdd-HHmmss[-n].
        public static List<string> Backups(string path)
        {
            string dir = Path.GetDirectoryName(path), name = Path.GetFileName(path);
            var pattern = new Regex("^" + Regex.Escape(name) + @"\.bak-(\d{8}-\d{6})(?:-(\d{1,9}))?$", RegexOptions.IgnoreCase);
            var found = new List<KeyValuePair<string, string>>();
            foreach (var file in Directory.GetFiles(dir, name + ".bak-*"))
            {
                var m = pattern.Match(Path.GetFileName(file));
                if (!m.Success) continue;
                int n = m.Groups[2].Success ? int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture) : 1;
                found.Add(new KeyValuePair<string, string>(m.Groups[1].Value + "-" + n.ToString("D9", CultureInfo.InvariantCulture), file));
            }
            found.Sort((a, b) => string.CompareOrdinal(b.Key, a.Key));
            var list = new List<string>();
            foreach (var pair in found) list.Add(pair.Value);
            return list;
        }

        // Keeps the backup just written and the newest others up to BackupsKept.
        static void PruneBackups(string path, string keep)
        {
            List<string> all;
            try { all = Backups(path); }
            catch (Exception) { return; }
            int kept = 1;
            foreach (var file in all)
            {
                if (string.Equals(file, keep, StringComparison.OrdinalIgnoreCase)) continue;
                if (kept < BackupsKept) { kept++; continue; }
                TryDelete(file);
            }
        }
    }

    static class Validators
    {
        static readonly Regex Guid4 = new Regex("^[0-9A-Fa-f]{1,8}-[0-9A-Fa-f]{1,8}-[0-9A-Fa-f]{1,8}-[0-9A-Fa-f]{1,8}$");

        public static string PlainText(string value, int maxLength)
        {
            if (value.Length > maxLength) return "At most " + maxLength + " characters.";
            foreach (char c in value)
                if (c < 0x20 || c > 0x7E) return "Use plain ASCII characters only.";
            if (value.Trim() != value) return "No leading or trailing spaces.";
            return null;
        }

        public static string Guid(string value)
        {
            if (value.Length == 0) return null;
            if (!Guid4.IsMatch(value)) return "Leave empty or enter a GUID like 30CA2DAB-7FED4260-54544950-32303153.";
            return null;
        }
    }
}
