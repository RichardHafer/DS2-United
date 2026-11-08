// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace DS2CoopConfig
{
    // The release INIs built into the program (Recommended.ini for the
    // randomizer, the repository's DS2Coop.ini, DS2Survival.ini and
    // DS2BotProfiles.ini), merged the way DS2Coop.ini is. They give the
    // recommended values, the comments for added keys and the Expert notes.
    static class Templates
    {
        static IniDocument doc, notes;
        static List<string> order;
        static Dictionary<string, string> sectionFile;
        static Dictionary<string, List<string>> sectionFiles;
        static Dictionary<string, string> legend;

        static readonly string[,] Resources =
        {
            { "Recommended.ini", "DS2Randomizer.ini" },
            { "Template.DS2United.ini", "DS2Coop.ini" },
            { "Template.DS2Survival.ini", "DS2Survival.ini" },
            { "Template.DS2BotProfiles.ini", "DS2BotProfiles.ini" },
        };

        public static List<string> Missing = new List<string>();

        static void Load()
        {
            if (doc != null) return;
            var sources = new List<MergeSource>();
            for (int i = 0; i < Resources.GetLength(0); i++)
            {
                byte[] bytes = Model.Resource(Resources[i, 0]);
                if (bytes == null) { Missing.Add(Resources[i, 0]); continue; }
                sources.Add(new MergeSource(Resources[i, 1], bytes));
            }
            var r = Merger.Build(sources, false);
            doc = r.Doc ?? new IniDocument();
            sectionFile = r.SectionFile;
            sectionFiles = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (var s in sources)
                foreach (var section in IniDocument.Parse(s.Bytes).SectionNames())
                {
                    List<string> files;
                    if (!sectionFiles.TryGetValue(section, out files)) sectionFiles[section] = files = new List<string>();
                    files.Add(s.Name);
                }
            order = doc.SectionNames();
            legend = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var profiles = sources.FirstOrDefault(s => s.Name == "DS2BotProfiles.ini");
            if (profiles != null) ReadLegend(IniDocument.Parse(profiles.Bytes));
            // the repository's DS2Randomizer.ini documents a few keys the release
            // INI leaves out; it is a test setup, so only its comments are used
            byte[] extra = Model.Resource("Notes.DS2Randomizer.ini");
            if (extra == null) Missing.Add("Notes.DS2Randomizer.ini");
            notes = extra != null ? IniDocument.Parse(extra) : new IniDocument();
        }

        public static IniDocument Doc { get { Load(); return doc; } }
        public static List<string> SectionOrder { get { Load(); return order; } }

        public static string FileOf(string section)
        {
            Load();
            string file;
            return sectionFile.TryGetValue(section, out file) ? file : null;
        }

        // Every template that has the section, "DS2Coop.ini, DS2Survival.ini" for [Zone].
        public static string FilesOf(string section)
        {
            Load();
            List<string> files;
            return sectionFiles.TryGetValue(section, out files) ? string.Join(", ", files.ToArray()) : null;
        }

        // Whether some template at least documents the key.
        public static bool Knows(string section, string key)
        {
            Load();
            return doc.FindKey(section, key) >= 0 || notes.FindKey(section, key) >= 0;
        }

        public static bool Has(string section, string key) { return Doc.FindKey(section, key) >= 0; }

        static bool IsComment(string text)
        {
            string t = text.TrimStart();
            return t.Length > 0 && t[0] == ';';
        }

        // The comment lines right above a key, as the template has them.
        public static List<string> CommentLines(string section, string key)
        {
            int at = Doc.FindKey(section, key);
            if (at >= 0) return Block(doc, at);
            at = notes.FindKey(section, key);
            return at >= 0 ? Block(notes, at) : null;
        }

        static List<string> Block(IniDocument d, int keyLine)
        {
            int start = keyLine;
            while (start > 0 && IsComment(d.Lines[start - 1].Text)) start--;
            if (start == keyLine) return null;
            var lines = new List<string>();
            for (int i = start; i < keyLine; i++) lines.Add(d.Lines[i].Text);
            return lines;
        }

        // A description for the Expert page: the key's own comment, the comment
        // of the key group it belongs to when that one names it, or the legend
        // at the top of DS2BotProfiles.ini.
        public static string Description(string section, string key)
        {
            Load();
            if (doc.FindKey(section, key) >= 0) return Description(doc, section, key);
            return notes.FindKey(section, key) >= 0 ? Description(notes, section, key) : null;
        }

        static string Description(IniDocument d, string section, string key)
        {
            int at = d.FindKey(section, key);
            var own = Block(d, at);
            if (own != null) return Join(own);
            int j = at - 1;
            while (j >= 0 && IniDocument.KeyName(d.Lines[j].Text) != null) j--;
            if (j >= 0 && j < at - 1 && IsComment(d.Lines[j].Text))
            {
                string anchor = IniDocument.KeyName(d.Lines[j + 1].Text);
                string text = Join(Block(d, j + 1));
                if (key.StartsWith(anchor, StringComparison.OrdinalIgnoreCase) || Mentions(text, key)) return text;
            }
            if (d == doc && FileOf(section) == "DS2BotProfiles.ini")
            {
                string text;
                if (legend.TryGetValue(key, out text)) return text;
                foreach (var pair in legend)
                    if (Mentions(pair.Value, key)) return pair.Value;
            }
            return null;
        }

        static bool Mentions(string text, string key)
        {
            return Regex.IsMatch(text, @"(?<![A-Za-z0-9])" + Regex.Escape(key) + @"(?![A-Za-z0-9])", RegexOptions.IgnoreCase);
        }

        // Comment lines to one paragraph; ruler lines are dropped, an empty
        // comment line starts a new paragraph.
        public static string Join(IList<string> lines)
        {
            if (lines == null) return null;
            var sb = new StringBuilder();
            foreach (var raw in lines)
            {
                string t = raw.TrimStart();
                if (t.StartsWith(";")) t = t.Substring(1);
                t = t.Trim();
                if (t.Length > 0 && t.Trim('=', '-', '*', '#').Length == 0) continue;
                if (t.Length == 0)
                {
                    if (sb.Length > 0 && sb[sb.Length - 1] != '\n') sb.Append('\n');
                    continue;
                }
                if (sb.Length > 0 && sb[sb.Length - 1] != '\n') sb.Append(' ');
                sb.Append(t);
            }
            return Regex.Replace(sb.ToString().Trim(), " {2,}", " ");
        }

        // "; Name   text" entries before the first section, continued by indented lines.
        // "ReactionMinMs/MaxMs" stands for ReactionMinMs and ReactionMaxMs.
        static void ReadLegend(IniDocument profiles)
        {
            var entry = new Regex(@"^;\s(\S+)\s{2,}(\S.*)$");
            var more = new Regex(@"^;\s{3,}(\S.*)$");
            string[] names = null;
            var text = new StringBuilder();
            Action flush = () =>
            {
                if (names != null)
                    foreach (var n in names) if (!legend.ContainsKey(n)) legend[n] = text.ToString();
                names = null;
                text.Clear();
            };
            foreach (var line in profiles.Lines)
            {
                if (IniDocument.SectionName(line.Text) != null) break;
                var m = entry.Match(line.Text);
                if (m.Success)
                {
                    flush();
                    var parts = m.Groups[1].Value.Split('/');
                    var list = new List<string> { parts[0] };
                    for (int i = 1; i < parts.Length; i++)
                    {
                        string p = parts[i];
                        list.Add(p.Length < parts[0].Length ? parts[0].Substring(0, parts[0].Length - p.Length) + p : p);
                    }
                    names = list.ToArray();
                    text.Append(m.Groups[2].Value.Trim());
                    continue;
                }
                var c = more.Match(line.Text);
                if (c.Success && names != null) { text.Append(' ').Append(c.Groups[1].Value.Trim()); continue; }
                flush();
            }
            flush();
        }
    }
}
