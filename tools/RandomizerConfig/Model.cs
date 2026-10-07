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
            if (WinInt.IsPlainNumber(raw) && raw.Trim().TrimStart('+') == Loaded[o.Id]) return null;
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

    static class IniFile
    {
        public static bool SameBytes(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }

        public static string BackupPath(string path)
        {
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            string backup = path + ".bak-" + stamp;
            for (int n = 2; File.Exists(backup); n++) backup = path + ".bak-" + stamp + "-" + n;
            return backup;
        }

        // Applies the model's changes to the file as it is on disk now.
        // Returns the changes; backupPath is null when nothing was written.
        public static List<string> Save(string path, Model model, out string backupPath, out byte[] written)
        {
            backupPath = null;
            written = null;
            byte[] current = File.ReadAllBytes(path);
            var doc = IniDocument.Parse(current);
            var changes = model.ApplyTo(doc);
            if (changes.Count == 0) return changes;
            byte[] output = doc.ToBytes();
            backupPath = BackupPath(path);
            File.WriteAllBytes(backupPath, current);
            File.WriteAllBytes(path, output);
            written = output;
            return changes;
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
