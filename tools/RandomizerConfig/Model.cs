// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;

namespace DS2CoopConfig
{
    enum Kind { Bool, Int, OptionalInt, Text }

    // One INI key. Values are kept as canonical strings: "1"/"0" for switches,
    // decimal for numbers, "" for an optional seed that follows the main seed.
    sealed class Option
    {
        // Value of a generated key the file does not have: the plugin's own
        // default applies, and this window does not know it.
        public const string NotSet = "\u0001";

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
        public Func<Model, string> HintFunc;  // shown in place of NoteText when set
        public Func<long, long> AdjustFunc;   // the game's own correction of a value
        public Func<string, string> ValidateFunc;
        public string CommentText;    // comment line when the key has to be added
        public IList<string> CommentLines;    // template comment lines, used instead of CommentText
        public string Heading;        // sub-heading shown above this option's card
        public bool KeyCode;          // a Windows virtual-key code
        public bool Generated;        // Expert page entry made from the file and the templates

        public string Id { get { return Section + "." + Key; } }

        public Option Note(string text) { NoteText = text; return this; }
        public Option Warn(Func<Model, string> f) { WarnFunc = f; return this; }
        public Option Hint(Func<Model, string> f) { HintFunc = f; return this; }
        public Option Raw() { RawRead = true; return this; }
        public Option From(string id) { DefaultFrom = id; return this; }
        public Option Keep() { KeepOnResetAll = true; return this; }
        public Option Adjust(Func<long, long> f) { AdjustFunc = f; return this; }
        public Option Validate(Func<string, string> f) { ValidateFunc = f; return this; }
        public Option Comment(string text) { CommentText = text; return this; }
        public Option Under(string heading) { Heading = heading; return this; }

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
            if (value == null) return "none";
            if (value == NotSet) return "not in file";
            if (Kind == Kind.Bool) return value == "1" ? "On" : "Off";
            if (Kind == Kind.OptionalInt && value == "") return "same as main seed";
            if (Kind == Kind.Text && value == "") return "(empty)";
            long code;
            if (KeyCode && long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out code))
                return value + " (" + VirtualKeys.Name(code) + ")";
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
            if (value == NotSet) return null;
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
        readonly IniDocument recommendedDoc;

        public Model(List<Option> options, IniDocument recommended)
        {
            Options = options;
            recommendedDoc = recommended;
            foreach (var o in options) ById[o.Id] = o;
            var rec = Evaluate(recommended);
            foreach (var pair in rec) Recommended[pair.Key] = pair.Value;
        }

        public IEnumerable<Option> Generated { get { foreach (var o in Options) if (o.Generated) yield return o; } }

        // Replaces the Expert entries. Their recommendation is the template
        // value, or null when no template has the key. Call LoadFrom afterwards.
        public void SetGenerated(List<Option> generated)
        {
            foreach (var o in Options)
                if (o.Generated)
                {
                    ById.Remove(o.Id);
                    Recommended.Remove(o.Id);
                    Invalid.Remove(o.Id);
                }
            Options.RemoveAll(o => o.Generated);
            foreach (var o in generated)
            {
                Options.Add(o);
                ById[o.Id] = o;
                string raw = recommendedDoc != null ? recommendedDoc.GetRaw(o.Section, o.Key) : null;
                Recommended[o.Id] = raw == null ? null : o.Effective(raw, o.CodeDefault);
            }
        }

        // What "Reset all" sets. Seeds and keys the file does not have stay as they are.
        public Dictionary<string, string> ResetAllValues()
        {
            var values = new Dictionary<string, string>();
            foreach (var o in Options)
            {
                if (o.KeepOnResetAll) continue;
                string rec = Recommended[o.Id];
                if (rec == null || Current[o.Id] == Option.NotSet) continue;
                values[o.Id] = rec;
            }
            return values;
        }

        public int ResetAll()
        {
            int n = 0;
            Invalid.Clear();
            foreach (var pair in ResetAllValues())
            {
                if (Current[pair.Key] != pair.Value) n++;
                Current[pair.Key] = pair.Value;
            }
            return n;
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

        // The value of any key, Expert rows included, or null when the plugin
        // falls back to its own default (key absent, or on no page at all).
        public string Value(string id)
        {
            Option o;
            string v;
            if (!ById.TryGetValue(id, out o) || !Current.TryGetValue(o.Id, out v) || v == Option.NotSet) return null;
            return v;
        }

        // A switch as GetPrivateProfileInt(...) != 0 reads it.
        public bool Flag(string id, bool fallback)
        {
            string v = Value(id);
            if (v == null || v.Trim().Length == 0) return fallback;
            return WinInt.Parse(v) != 0;
        }

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
                if (value == Option.NotSet) continue;
                bool existed = doc.FindKey(o.Section, o.Key) >= 0;
                var comment = o.CommentLines ?? (IList<string>)new[] { "; " + o.ShortComment };
                doc.SetValue(o.Section, o.Key, value, o.IsNumeric, comment, Templates.SectionOrder);
                changes.Add("[" + o.Section + "] " + o.Key + "=" + value + (existed ? "" : " (added)"));
            }
            return changes;
        }

        // The release values: Recommended.ini for the randomizer and the
        // repository templates for everything else, merged like DS2United.ini.
        public static IniDocument LoadRecommended() { return Templates.Doc; }

        public static byte[] Resource(string name)
        {
            var asm = Assembly.GetExecutingAssembly();
            using (var stream = asm.GetManifestResourceStream("DS2CoopConfig." + name))
            {
                if (stream == null) return null;
                var ms = new MemoryStream();
                stream.CopyTo(ms);
                return ms.ToArray();
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

        // Any single line the file can hold.
        public static string Line(string value)
        {
            foreach (char c in value)
                if (c < 0x20 || c > 0xFF) return "Use one line of plain text (Latin-1 characters).";
            if (value.Trim() != value) return "No leading or trailing spaces.";
            return null;
        }

        public static string Decimal(string value)
        {
            double d;
            if (!double.TryParse(value, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out d))
                return "Enter a number like 2.5 (with a point, not a comma).";
            return null;
        }
    }

    static class VirtualKeys
    {
        public static string Name(long code)
        {
            if (code == 0) return "none";
            if (code >= 0x70 && code <= 0x87) return "F" + (code - 0x6F);
            if (code >= 0x60 && code <= 0x69) return "Numpad " + (code - 0x60);
            if ((code >= 0x30 && code <= 0x39) || (code >= 0x41 && code <= 0x5A)) return ((char)code).ToString();
            switch (code)
            {
                case 0x08: return "Backspace";
                case 0x09: return "Tab";
                case 0x0D: return "Enter";
                case 0x10: return "Shift";
                case 0x11: return "Ctrl";
                case 0x12: return "Alt";
                case 0x13: return "Pause";
                case 0x14: return "Caps Lock";
                case 0x1B: return "Escape";
                case 0x20: return "Space";
                case 0x21: return "Page Up";
                case 0x22: return "Page Down";
                case 0x23: return "End";
                case 0x24: return "Home";
                case 0x25: return "Left";
                case 0x26: return "Up";
                case 0x27: return "Right";
                case 0x28: return "Down";
                case 0x2D: return "Insert";
                case 0x2E: return "Delete";
                case 0x6A: return "Numpad *";
                case 0x6B: return "Numpad +";
                case 0x6D: return "Numpad -";
                case 0x6E: return "Numpad decimal point";
                case 0x6F: return "Numpad /";
                case 0x90: return "Num Lock";
                case 0x91: return "Scroll Lock";
                case 0xC0: return "`";
                case 0xDC: return "\\";
                default: return "key " + code;
            }
        }
    }
}
