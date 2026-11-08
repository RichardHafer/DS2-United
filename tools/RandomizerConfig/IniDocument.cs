using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace DS2CoopConfig
{
    sealed class IniLine
    {
        public string Text;   // without the line terminator
        public string Eol;    // "\r\n", "\n", "\r" or "" on the last line

        public IniLine(string text, string eol) { Text = text; Eol = eol; }
    }

    // Lossless INI editor. ANSI and UTF-8 files map bytes 1:1 to chars (Latin-1),
    // UTF-16 files are decoded and written back in the same byte order with
    // their BOM. Anything that is not touched is written back exactly as read.
    sealed class IniDocument
    {
        static readonly Encoding Bytes1To1 = Encoding.GetEncoding(28591);
        static readonly byte[] Bom = { 0xEF, 0xBB, 0xBF };

        public bool HasBom;          // UTF-8 BOM
        public Encoding Wide;        // UTF-16 LE or BE (BOM kept), null for byte files
        public string Problem;       // why the file cannot be written back safely, or null
        public List<IniLine> Lines = new List<IniLine>();
        bool WideBigEndian;

        // First header per section and first key per name inside it, rebuilt
        // after lines were inserted or removed. Call Touch() after editing Lines directly.
        Dictionary<string, int> headers;
        Dictionary<string, Dictionary<string, int>> keys;

        public void Touch() { headers = null; keys = null; }

        public void MakeWide()
        {
            Wide = new UnicodeEncoding(false, false);
            WideBigEndian = false;
            HasBom = false;
        }

        void BuildIndex()
        {
            headers = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            keys = new Dictionary<string, Dictionary<string, int>>(StringComparer.OrdinalIgnoreCase);
            Dictionary<string, int> current = null;
            for (int i = 0; i < Lines.Count; i++)
            {
                string text = Lines[i].Text;
                string name = SectionName(text);
                if (name != null)
                {
                    // a repeated section is not read, Windows stops at the first one
                    if (headers.ContainsKey(name)) { current = null; continue; }
                    headers[name] = i;
                    current = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
                    keys[name] = current;
                    continue;
                }
                if (current == null) continue;
                string key = KeyName(text);
                if (key != null && !current.ContainsKey(key)) current[key] = i;
            }
        }

        // Sections in file order, each once.
        public List<string> SectionNames()
        {
            if (headers == null) BuildIndex();
            var list = new List<KeyValuePair<int, string>>();
            foreach (var pair in headers) list.Add(new KeyValuePair<int, string>(pair.Value, pair.Key));
            list.Sort((a, b) => a.Key.CompareTo(b.Key));
            var names = new List<string>();
            foreach (var pair in list) names.Add(SectionName(Lines[pair.Key].Text));
            return names;
        }

        // Keys of a section in file order, as Windows reads them.
        public List<string> KeyNames(string section)
        {
            if (headers == null) BuildIndex();
            var names = new List<string>();
            Dictionary<string, int> map;
            if (!keys.TryGetValue(section, out map)) return names;
            var list = new List<KeyValuePair<int, string>>();
            foreach (var pair in map) list.Add(new KeyValuePair<int, string>(pair.Value, pair.Key));
            list.Sort((a, b) => a.Key.CompareTo(b.Key));
            foreach (var pair in list) names.Add(KeyName(Lines[pair.Key].Text));
            return names;
        }

        public string EncodingName
        {
            get
            {
                if (Wide != null) return WideBigEndian ? "UTF-16 BE" : "UTF-16 LE";
                return HasBom ? "UTF-8 BOM" : "ANSI/UTF-8";
            }
        }

        public static IniDocument Parse(byte[] bytes)
        {
            var doc = new IniDocument();
            string text;
            bool le = bytes.Length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
            bool be = bytes.Length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF;
            if (le || be)
            {
                doc.WideBigEndian = be;
                doc.Wide = new UnicodeEncoding(be, false, false);
                text = doc.Wide.GetString(bytes, 2, bytes.Length - 2);
            }
            else
            {
                int start = 0;
                if (bytes.Length >= 3 && bytes[0] == Bom[0] && bytes[1] == Bom[1] && bytes[2] == Bom[2])
                {
                    doc.HasBom = true;
                    start = 3;
                }
                text = Bytes1To1.GetString(bytes, start, bytes.Length - start);
            }
            doc.Split(text);
            if (le && bytes.Length >= 4 && bytes[2] == 0 && bytes[3] == 0)
                doc.Problem = "The file is saved as UTF-32, which the game cannot read. Save it as ANSI, UTF-8 or Unicode (UTF-16) in a text editor.";
            else if (doc.Wide != null && !IniFile.SameBytes(doc.ToBytes(), bytes))
                doc.Problem = "The file is marked as Unicode (UTF-16) but is not valid UTF-16, so saving could damage it. Save it again as ANSI, UTF-8 or Unicode in a text editor.";
            return doc;
        }

        void Split(string text)
        {
            int lineStart = 0;
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (c != '\r' && c != '\n') continue;
                string eol = (c == '\r' && i + 1 < text.Length && text[i + 1] == '\n') ? "\r\n" : c.ToString();
                Lines.Add(new IniLine(text.Substring(lineStart, i - lineStart), eol));
                i += eol.Length - 1;
                lineStart = i + 1;
            }
            if (lineStart < text.Length)
                Lines.Add(new IniLine(text.Substring(lineStart), ""));
        }

        public byte[] ToBytes()
        {
            var sb = new StringBuilder();
            foreach (var line in Lines) sb.Append(line.Text).Append(line.Eol);
            byte[] prefix = Wide != null ? (WideBigEndian ? new byte[] { 0xFE, 0xFF } : new byte[] { 0xFF, 0xFE })
                : HasBom ? Bom : new byte[0];
            byte[] body = (Wide ?? Bytes1To1).GetBytes(sb.ToString());
            var all = new byte[prefix.Length + body.Length];
            Array.Copy(prefix, all, prefix.Length);
            Array.Copy(body, 0, all, prefix.Length, body.Length);
            return all;
        }

        // The terminator new lines get: whatever the file mostly uses.
        public string NewLine
        {
            get
            {
                int crlf = 0, lf = 0, cr = 0;
                foreach (var line in Lines)
                {
                    if (line.Eol == "\r\n") crlf++;
                    else if (line.Eol == "\n") lf++;
                    else if (line.Eol == "\r") cr++;
                }
                if (lf > crlf && lf >= cr) return "\n";
                if (cr > crlf && cr > lf) return "\r";
                return "\r\n";
            }
        }

        public static string SectionName(string text)
        {
            string t = text.Trim();
            if (t.Length < 2 || t[0] != '[') return null;
            int close = t.IndexOf(']');
            if (close < 0) return null;
            return t.Substring(1, close - 1).Trim();
        }

        // Key of a "key=value" line, or null for comments, headers and blanks.
        public static string KeyName(string text)
        {
            string t = text.TrimStart();
            if (t.Length == 0 || t[0] == ';' || SectionName(text) != null) return null;
            int eq = text.IndexOf('=');
            if (eq <= 0) return null;
            string key = text.Substring(0, eq).Trim();
            return key.Length == 0 ? null : key;
        }

        // [start, end) of the value: after '=' and leading blanks, before
        // trailing blanks. With numericOnly, only the leading number is
        // covered, so a trailing inline note survives an edit.
        public static void ValueSpan(string text, bool numericOnly, out int start, out int end)
        {
            int eq = text.IndexOf('=');
            start = eq + 1;
            while (start < text.Length && (text[start] == ' ' || text[start] == '\t')) start++;
            end = text.Length;
            while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
            if (!numericOnly) return;
            int len = WinInt.TokenLength(text, start, end);
            if (len > 0) end = start + len;
        }

        // Windows uses the first matching section and the first matching key.
        public int FindSection(string section)
        {
            if (headers == null) BuildIndex();
            int i;
            return headers.TryGetValue(section, out i) ? i : -1;
        }

        public int SectionEnd(int header)
        {
            for (int i = header + 1; i < Lines.Count; i++)
                if (SectionName(Lines[i].Text) != null) return i;
            return Lines.Count;
        }

        // Just after the last key (or the header) of a section: blank lines and
        // comments before the next header usually introduce that one.
        public int ContentEnd(int header)
        {
            int i = SectionEnd(header) - 1;
            while (i > header)
            {
                string t = Lines[i].Text.Trim();
                if (t.Length > 0 && t[0] != ';') break;
                i--;
            }
            return i + 1;
        }

        public int FindKey(string section, string key)
        {
            if (headers == null) BuildIndex();
            Dictionary<string, int> map;
            int i;
            return keys.TryGetValue(section, out map) && map.TryGetValue(key, out i) ? i : -1;
        }

        // The trimmed value, or null when the key is absent.
        public string GetRaw(string section, string key)
        {
            int index = FindKey(section, key);
            if (index < 0) return null;
            int start, end;
            ValueSpan(Lines[index].Text, false, out start, out end);
            string value = Lines[index].Text.Substring(start, end - start);
            if (value.Length >= 2 && (value[0] == '"' || value[0] == '\'') && value[value.Length - 1] == value[0])
                value = value.Substring(1, value.Length - 2);
            return value;
        }

        // Rewrites only the value part of an existing line, or adds the key
        // (with a one-line comment) at the end of its section.
        public void SetValue(string section, string key, string value, bool numeric, string comment)
        {
            SetValue(section, key, value, numeric, string.IsNullOrEmpty(comment) ? null : new[] { "; " + comment }, null);
        }

        // As above with comment lines taken over as they are. A missing section
        // goes after the nearest section that precedes it in sectionOrder.
        public void SetValue(string section, string key, string value, bool numeric, IList<string> comment, IList<string> sectionOrder)
        {
            int index = FindKey(section, key);
            if (index >= 0)
            {
                string text = Lines[index].Text;
                int start, end;
                ValueSpan(text, numeric, out start, out end);
                Lines[index].Text = text.Substring(0, start) + value + text.Substring(end);
                return;
            }
            string nl = NewLine;
            var added = new List<IniLine>();
            int header = FindSection(section);
            int insertAt;
            int anchor = header < 0 ? Predecessor(section, sectionOrder) : -1;
            if (header < 0 && anchor >= 0)
            {
                insertAt = ContentEnd(anchor);
                added.Add(new IniLine("", nl));
                added.Add(new IniLine("[" + section + "]", nl));
            }
            else if (header >= 0)
            {
                insertAt = header + 1;
                int end = SectionEnd(header);
                for (int i = header + 1; i < end; i++)
                    if (KeyName(Lines[i].Text) != null) insertAt = i + 1;
                // next to a commented-out template such as "; Seed=50", if there is one
                for (int i = header + 1; i < end; i++)
                {
                    string t = Lines[i].Text.TrimStart();
                    if (!t.StartsWith(";")) continue;
                    string inner = t.Substring(1).TrimStart();
                    if (KeyName(inner) != null && string.Equals(KeyName(inner), key, StringComparison.OrdinalIgnoreCase))
                        insertAt = i + 1;
                }
            }
            else
            {
                insertAt = Lines.Count;
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Text.Trim().Length > 0)
                    added.Add(new IniLine("", nl));
                added.Add(new IniLine("[" + section + "]", nl));
            }
            if (comment != null)
                foreach (var c in comment) added.Add(new IniLine(c.TrimStart().StartsWith(";") ? c : "; " + c, nl));
            added.Add(new IniLine(key + "=" + value, nl));
            // a section put between two others keeps a blank line before the next one
            if (anchor >= 0 && insertAt < Lines.Count && Lines[insertAt].Text.Trim().Length > 0)
                added.Add(new IniLine("", nl));
            if (insertAt > 0 && Lines[insertAt - 1].Eol.Length == 0)
            {
                // the previous line ended the file without a terminator; keep it that way
                Lines[insertAt - 1].Eol = nl;
                added[added.Count - 1].Eol = "";
            }
            Lines.InsertRange(insertAt, added);
            Touch();
        }

        int Predecessor(string section, IList<string> order)
        {
            if (order == null) return -1;
            int at = -1;
            for (int i = 0; i < order.Count; i++)
                if (string.Equals(order[i], section, StringComparison.OrdinalIgnoreCase)) { at = i; break; }
            for (int i = at - 1; i >= 0; i--)
            {
                int h = FindSection(order[i]);
                if (h >= 0) return h;
            }
            return -1;
        }

        // Turns "Key=value" into "; Key=value", so the default applies again.
        public bool CommentOut(string section, string key)
        {
            int index = FindKey(section, key);
            if (index < 0) return false;
            Lines[index].Text = "; " + Lines[index].Text.TrimStart();
            Touch();
            return true;
        }
    }

    // GetPrivateProfileInt semantics: leading blanks, a sign, an optional
    // 0x/0o/0b prefix, digits until the first invalid character, 32-bit wrap.
    static class WinInt
    {
        static int DigitValue(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 99;
        }

        static int Prefix(string s, int i, int end, out int radix)
        {
            radix = 10;
            if (i + 1 < end && s[i] == '0')
            {
                char p = char.ToLowerInvariant(s[i + 1]);
                if (p == 'x') { radix = 16; return 2; }
                if (p == 'o') { radix = 8; return 2; }
                if (p == 'b') { radix = 2; return 2; }
            }
            return 0;
        }

        // Length of the number token at start, 0 if there is none.
        public static int TokenLength(string s, int start, int end)
        {
            int i = start;
            if (i < end && (s[i] == '+' || s[i] == '-')) i++;
            int radix;
            int prefix = Prefix(s, i, end, out radix);
            int digitsStart = i + prefix;
            int j = digitsStart;
            while (j < end && DigitValue(s[j]) < radix) j++;
            if (j == digitsStart)
            {
                // "0x" with nothing after it still reads the 0
                if (prefix > 0) return i + 1 - start;
                return 0;
            }
            return j - start;
        }

        public static uint Parse(string s)
        {
            int i = 0, end = s.Length;
            while (i < end && s[i] <= ' ') i++;
            bool negative = false;
            if (i < end && (s[i] == '+' || s[i] == '-')) { negative = s[i] == '-'; i++; }
            int radix;
            int prefix = Prefix(s, i, end, out radix);
            if (prefix > 0 && (i + 2 >= end || DigitValue(s[i + 2]) >= radix)) return 0;
            i += prefix;
            uint value = 0;
            while (i < end)
            {
                int d = DigitValue(s[i]);
                if (d >= radix) break;
                value = unchecked(value * (uint)radix + (uint)d);
                i++;
            }
            return negative ? unchecked((uint)(-(int)value)) : value;
        }

        public static bool IsPlainNumber(string s)
        {
            s = s.Trim();
            if (s.Length == 0) return false;
            int i = s[0] == '-' ? 1 : 0;
            if (i == s.Length) return false;
            for (; i < s.Length; i++) if (s[i] < '0' || s[i] > '9') return false;
            return true;
        }
    }
}
