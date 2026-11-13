// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace DS2CoopConfig
{
    // Expert page entries: every key of the file and the templates that no
    // other page shows. The kind comes from the values, the note from the template.
    static class ExpertKeys
    {
        // 0/1 values of these are counts, seconds or points, not switches
        static readonly Regex NumberName = new Regex(@"(Seconds|Ms|Meters|Percent|Count|Key|Slot|Distance|Size|Burst|Level|Lives|Double|Triple|Quad|Side|Antics|Packs|Points)$|[a-z]M$");
        static readonly HashSet<string> NumberKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { "RewardStasis", "Type", "PairedDamage" };
        static readonly HashSet<string> NumberSections = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { "Weights", "MoreEnemies", "WatchdogTimers" };

        static bool MaySwitch(string section, string key)
        {
            if (NumberSections.Contains(section) || NumberKeys.Contains(key)) return false;
            if (key.Equals("HarmfulAntics", StringComparison.OrdinalIgnoreCase)) return true;
            if (key.StartsWith("AmmoSlot", StringComparison.OrdinalIgnoreCase) || key.StartsWith("Stages", StringComparison.OrdinalIgnoreCase)) return false;
            return !NumberName.IsMatch(key);
        }

        static bool IsInt(string v)
        {
            int n;
            return WinInt.IsPlainNumber(v) && int.TryParse(v.Trim(), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out n);
        }

        static readonly Dictionary<string, string> Words = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "guid", "GUID" }, { "vfx", "VFX" }, { "fx", "FX" }, { "hud", "HUD" }, { "npc", "NPC" }, { "npcs", "NPCs" },
            { "mp", "MP" }, { "ai", "AI" }, { "fps", "FPS" }, { "tk", "TK" }, { "igc", "IGC" }, { "pcm", "PCM" }, { "kib", "KiB" },
        };

        // "PeerTimeoutMs" -> "Peer timeout (ms)", "HostNPCs" -> "Host NPCs".
        public static string Title(string key)
        {
            if (key.IndexOf('_') >= 0 || key.IndexOf('.') >= 0 || key.IndexOf(' ') >= 0) return key;
            var words = Regex.Split(key, @"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z]{2,})|(?<=[A-Za-z])(?=[0-9])|(?<=[0-9])(?=[A-Za-z])")
                .Where(w => w.Length > 0).ToList();
            string unit = null;
            if (words.Count > 1)
            {
                switch (words[words.Count - 1])
                {
                    case "Ms": unit = "(ms)"; break;
                    case "Seconds": unit = "(s)"; break;
                    case "Meters": case "M": unit = "(m)"; break;
                    case "Percent": unit = "(%)"; break;
                    case "KiB": unit = "(KiB)"; break;
                }
                if (unit != null) words.RemoveAt(words.Count - 1);
            }
            for (int i = 0; i < words.Count; i++)
            {
                string w = words[i], mapped;
                if (Words.TryGetValue(w, out mapped)) { words[i] = mapped; continue; }
                if (w.Count(char.IsUpper) >= 2) continue;   // an acronym such as SS or NP
                words[i] = i == 0 ? char.ToUpperInvariant(w[0]) + w.Substring(1).ToLowerInvariant() : w.ToLowerInvariant();
            }
            if (unit != null) words.Add(unit);
            return string.Join(" ", words.ToArray());
        }

        // primary order, with what only `extra` has placed after its predecessor there
        static List<string> MergeOrder(List<string> primary, List<string> extra)
        {
            var result = new List<string>(primary);
            var have = new HashSet<string>(primary, StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < extra.Count; i++)
            {
                if (have.Contains(extra[i])) continue;
                int at = 0;
                for (int j = i - 1; j >= 0; j--)
                {
                    int k = result.FindIndex(s => string.Equals(s, extra[j], StringComparison.OrdinalIgnoreCase));
                    if (k >= 0) { at = k + 1; break; }
                }
                result.Insert(at, extra[i]);
                have.Add(extra[i]);
            }
            return result;
        }

        public const string UnknownNote = "Not in any template: this window does not know the key and keeps it as it is.";

        public static List<Option> Build(IniDocument file, ICollection<string> taken)
        {
            var tdoc = Templates.Doc;
            var list = new List<Option>();
            var sections = MergeOrder(tdoc.SectionNames(), file.SectionNames());
            foreach (var section in sections)
            {
                var keys = MergeOrder(file.KeyNames(section), tdoc.KeyNames(section));
                string fileSection = file.FindSection(section) >= 0 ? IniDocument.SectionName(file.Lines[file.FindSection(section)].Text) : section;
                foreach (var key in keys)
                {
                    if (taken.Contains(fileSection + "." + key) || Schema.Hidden(section, key)) continue;
                    string fileRaw = file.GetRaw(section, key), templateRaw = tdoc.GetRaw(section, key);
                    var values = new List<string>();
                    if (fileRaw != null) values.Add(fileRaw.Trim());
                    if (templateRaw != null) values.Add(templateRaw.Trim());
                    Kind kind = values.All(v => v == "0" || v == "1") && MaySwitch(section, key) ? Kind.Bool
                        : values.All(IsInt) ? Kind.Int : Kind.Text;
                    string desc = Templates.Description(section, key);
                    if (desc == null && !Templates.Knows(section, key)) desc = UnknownNote;
                    var o = new Option
                    {
                        Group = Schema.Expert, Section = fileSection, Key = key, Kind = kind, Generated = true,
                        Title = Title(key), Description = desc ?? "", CodeDefault = Option.NotSet,
                        CommentLines = Templates.CommentLines(section, key) ?? new List<string>(),
                        Min = int.MinValue, Max = int.MaxValue,
                    };
                    if (kind == Kind.Bool) o.RawRead = true;
                    if (kind == Kind.Int)
                    {
                        o.AdjustFunc = v => v;
                        o.KeyCode = key.EndsWith("Key", StringComparison.OrdinalIgnoreCase) && values.All(v => { long n; return long.TryParse(v, out n) && n >= 0 && n <= 255; });
                    }
                    if (kind == Kind.Text) o.ValidateFunc = Validators.Line;
                    o.WarnFunc = Curated.ExpertWarning(section, key);
                    list.Add(o);
                }
            }
            return list;
        }

        public static bool SameSet(IEnumerable<Option> a, IEnumerable<Option> b)
        {
            var x = a.Select(o => o.Id + "|" + o.Kind).ToList();
            var y = b.Select(o => o.Id + "|" + o.Kind).ToList();
            return x.SequenceEqual(y);
        }
    }

    // The Expert page: hundreds of keys, so rows are painted, not built from
    // controls. One text box is laid over the value being edited.
    sealed class ExpertGrid : ScrollableControl
    {
        enum ItemKind { Title, Intro, Section, Row }
        enum Part { None, Switch, Well, Reset, Add, Description }

        sealed class Item
        {
            public ItemKind Kind;
            public Option Opt;
            public string Section, Info, Warning;
            public int Top, Height, CardBottom, DescHeight, ValueTop, WarnTop, WarnHeight;
            public bool DescCut, Stacked;
        }

        readonly Model model;
        readonly string title, intro;
        List<Option> options = new List<Option>();
        readonly List<Item> items = new List<Item>();
        readonly List<Item> sectionItems = new List<Item>();
        readonly Dictionary<string, string> search = new Dictionary<string, string>();
        readonly Dictionary<string, string> typed = new Dictionary<string, string>();
        // description heights at measuredWidth, so a search does not measure again
        readonly Dictionary<string, int> measured = new Dictionary<string, int>();
        int measuredWidth = -1;
        readonly TextBox box;
        string filter = "";
        int side = 28, column = 900, matches, contentHeight;
        bool laidOut;
        Option editing;
        Item hotItem;
        Part hotPart;

        public Action<Option, string, string> Edited;   // option, new value (null while invalid), error
        public Action<Option> ResetClicked;

        public ExpertGrid(Model model, string title, string intro)
        {
            this.model = model;
            this.title = title;
            this.intro = intro;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
            AutoScroll = true;
            BackColor = Look.Back;
            TabStop = true;
            box = new TextBox { BorderStyle = BorderStyle.None, BackColor = Look.InputBack, ForeColor = Look.Text, Font = Look.Base, Visible = false };
            box.KeyDown += (s, e) =>
            {
                if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; EndEdit(true); Focus(); }
                else if (e.KeyCode == Keys.Escape) { e.SuppressKeyPress = true; EndEdit(false); Focus(); }
            };
            box.Leave += delegate { EndEdit(true); };
            box.Validating += delegate { EndEdit(true); };
            Controls.Add(box);
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.DarkScrollBars(this);
        }

        public int MatchCount { get { return matches; } }
        public int ContentHeight { get { return contentHeight; } }

        public void SetOptions(List<Option> list)
        {
            EndEdit(false);
            options = list;
            typed.Clear();
            search.Clear();
            measured.Clear();
            foreach (var o in list)
                search[o.Id] = (o.Title + " " + o.Key + " [" + o.Section + "] " + o.Section + " " + o.Description).ToLowerInvariant();
            Relayout();
        }

        public void SetColumn(int sideMargin, int width)
        {
            if (laidOut && side == sideMargin && column == width) return;
            side = sideMargin;
            column = width;
            Relayout();
        }

        public int SetFilter(string text)
        {
            filter = text;
            Relayout();
            ScrollTo(0);
            return matches;
        }

        public bool Matches(Option o) { return filter.Length == 0 || search[o.Id].Contains(filter); }

        public void ClearTyped(Option o) { typed.Remove(o.Id); Invalidate(); }
        public void ClearTyped() { EndEdit(false); typed.Clear(); Invalidate(); }

        public void ScrollTo(int y)
        {
            int max = Math.Max(0, contentHeight - ClientSize.Height);
            AutoScrollPosition = new Point(0, Math.Max(0, Math.Min(max, y)));
            Invalidate();
        }

        public int ScrollY { get { return -AutoScrollPosition.Y; } }

        // ---- layout

        static int S(int px) { return Look.S(px); }
        int Pad { get { return S(18); } }
        int ValueWidth { get { return S(300); } }
        bool StackedLayout { get { return column - 2 * Pad - ValueWidth - S(24) < S(280); } }
        int TextWidth { get { return StackedLayout ? column - 2 * Pad : column - 2 * Pad - ValueWidth - S(24); } }
        int SmallLine { get { return TextRenderer.MeasureText("Ag", Look.Small).Height; } }

        void Relayout()
        {
            EndEdit(true);
            laidOut = true;
            items.Clear();
            sectionItems.Clear();
            matches = 0;
            int y = S(24);
            int th = Look.LineHeight(Look.PageTitle);
            items.Add(new Item { Kind = ItemKind.Title, Top = y, Height = th });
            y += th + S(6);
            int ih = Look.Measure(intro, Look.Base, column);
            items.Add(new Item { Kind = ItemKind.Intro, Top = y, Height = ih });
            y += ih + S(18);
            int textWidth = TextWidth, small = SmallLine, bold = Look.LineHeight(Look.Bold);
            bool stacked = StackedLayout;
            if (textWidth != measuredWidth)
            {
                measured.Clear();
                measuredWidth = textWidth;
            }
            Item header = null;
            int count = 0;
            foreach (var o in options)
            {
                if (!Matches(o)) continue;
                matches++;
                if (header == null || !string.Equals(header.Section, o.Section, StringComparison.OrdinalIgnoreCase))
                {
                    if (header != null) { Close(header, count, y); y += S(12); }
                    header = new Item { Kind = ItemKind.Section, Section = o.Section, Top = y, Height = S(48) };
                    count = 0;
                    items.Add(header);
                    sectionItems.Add(header);
                    y += header.Height;
                }
                count++;
                var row = new Item { Kind = ItemKind.Row, Opt = o, Section = o.Section, Top = y, Stacked = stacked };
                int full;
                if (!measured.TryGetValue(o.Id, out full))
                    measured[o.Id] = full = o.Description.Length == 0 ? 0 : Look.Measure(o.Description, Look.Small, textWidth);
                row.DescHeight = Math.Min(full, 3 * small + 2);
                row.DescCut = full > row.DescHeight;
                int textBottom = y + S(12) + bold + (row.DescHeight > 0 ? S(2) + row.DescHeight : 0);
                row.ValueTop = stacked ? textBottom + S(8) : y + S(10);
                int bottom = row.ValueTop + S(30) + S(4) + small;
                if (!stacked) bottom = Math.Max(textBottom, bottom);
                row.Warning = WarningOf(o);
                if (row.Warning != null)
                {
                    row.WarnTop = bottom + S(8) - y;
                    row.WarnHeight = Look.Measure(row.Warning, Look.Small, column - 2 * Pad - S(48)) + S(13);
                    bottom += S(8) + row.WarnHeight;
                }
                row.Height = bottom + S(12) - y;
                items.Add(row);
                y += row.Height;
            }
            if (header != null) Close(header, count, y);
            y += S(24);
            contentHeight = y;
            AutoScrollMinSize = new Size(0, y);
            Invalidate();
        }

        string WarningOf(Option o)
        {
            // the options are swapped in before the file's values are loaded
            return o.WarnFunc != null && model.Current.ContainsKey(o.Id) ? o.WarnFunc(model) : null;
        }

        // Warnings follow values on every page; a row grows or shrinks with its warning.
        public void RefreshWarnings()
        {
            foreach (var it in items)
                if (it.Kind == ItemKind.Row && it.Opt.WarnFunc != null && it.Warning != WarningOf(it.Opt))
                {
                    Relayout();
                    return;
                }
            Invalidate();
        }

        static void Close(Item header, int count, int bottom)
        {
            header.CardBottom = bottom;
            string files = Templates.FilesOf(header.Section);
            header.Info = count + " key" + (count == 1 ? "" : "s") + (files != null ? "   from " + files : "   not in any template");
        }

        // ---- geometry, in content coordinates

        Rectangle WellRect(Item it)
        {
            int x = it.Stacked ? side + Pad : side + column - Pad - ValueWidth;
            int w = it.Opt.Kind == Kind.Int ? S(150) : S(226);
            return new Rectangle(x, it.ValueTop, w, S(30));
        }

        Rectangle SwitchRect(Item it)
        {
            var w = WellRect(it);
            return new Rectangle(w.X, w.Y + S(3), Switch.TrackWidth + S(10) + Look.OneLineWidth("Off", Look.Base), S(24));
        }

        Rectangle ButtonRect(Item it)
        {
            return new Rectangle(side + column - Pad - S(62), it.ValueTop + S(2), S(62), S(26));
        }

        Rectangle UnderRect(Item it)
        {
            var w = WellRect(it);
            return new Rectangle(w.X, w.Bottom + S(4), side + column - Pad - w.X, SmallLine);
        }

        Rectangle DescRect(Item it)
        {
            return new Rectangle(side + Pad, it.Top + S(12) + Look.LineHeight(Look.Bold) + S(2), TextWidth, it.DescHeight);
        }

        bool NotSet(Option o) { return model.Current.ContainsKey(o.Id) && model.Current[o.Id] == Option.NotSet; }

        bool CanReset(Option o)
        {
            string rec = model.Recommended[o.Id];
            if (rec == null || NotSet(o)) return false;
            return model.Current[o.Id] != rec || model.Invalid.ContainsKey(o.Id) || typed.ContainsKey(o.Id);
        }

        Item RowAt(int y)
        {
            int lo = 0, hi = items.Count - 1;
            while (lo <= hi)
            {
                int mid = (lo + hi) / 2;
                var it = items[mid];
                if (y < it.Top) hi = mid - 1;
                else if (y >= it.Top + it.Height) lo = mid + 1;
                else return it.Kind == ItemKind.Row ? it : null;
            }
            return null;
        }

        Part PartAt(Item it, Point p)
        {
            if (it == null) return Part.None;
            var o = it.Opt;
            if (NotSet(o))
            {
                if (model.Recommended[o.Id] != null && ButtonRect(it).Contains(p)) return Part.Add;
            }
            else
            {
                if (o.Kind == Kind.Bool && SwitchRect(it).Contains(p)) return Part.Switch;
                if (o.Kind != Kind.Bool && WellRect(it).Contains(p)) return Part.Well;
                if (ButtonRect(it).Contains(p) && CanReset(o)) return Part.Reset;
            }
            if (DescRect(it).Contains(p) && it.DescCut) return Part.Description;
            return Part.None;
        }

        // ---- painting

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Look.Back);
            int dy = AutoScrollPosition.Y;
            int top = e.ClipRectangle.Top - dy, bottom = e.ClipRectangle.Bottom - dy;
            foreach (var it in items)
            {
                if (it.Kind == ItemKind.Title && it.Top < bottom && it.Top + it.Height > top)
                    TextRenderer.DrawText(g, title, Look.PageTitle, new Point(side, it.Top + dy), Look.Text, TextFormatFlags.NoPrefix);
                else if (it.Kind == ItemKind.Intro && it.Top < bottom && it.Top + it.Height > top)
                    TextRenderer.DrawText(g, intro, Look.Base, new Rectangle(side, it.Top + dy, column, it.Height), Look.Secondary,
                        TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl | TextFormatFlags.NoPrefix);
            }
            g.SmoothingMode = SmoothingMode.AntiAlias;
            foreach (var sec in sectionItems)
            {
                if (sec.Top >= bottom || sec.CardBottom <= top) continue;
                var card = new Rectangle(side, sec.Top + dy, column - 1, sec.CardBottom - sec.Top - 1);
                using (var path = Look.Round(card, S(6)))
                {
                    using (var fill = new SolidBrush(Look.Card)) g.FillPath(fill, path);
                    var clip = g.Clip;
                    g.SetClip(path, CombineMode.Intersect);
                    PaintRows(g, sec, dy, top, bottom);
                    g.Clip = clip;
                    using (var pen = new Pen(Look.Border)) g.DrawPath(pen, path);
                }
                int hh = Look.LineHeight(Look.Heading);
                var headerText = "[" + sec.Section + "]";
                int hx = side + Pad, hy = sec.Top + dy + (sec.Height - hh) / 2;
                TextRenderer.DrawText(g, headerText, Look.Heading, new Point(hx, hy), Look.Text, TextFormatFlags.NoPrefix);
                int hw = TextRenderer.MeasureText(headerText, Look.Heading).Width;
                int sh = Look.LineHeight(Look.Small);
                TextRenderer.DrawText(g, sec.Info, Look.Small, new Point(hx + hw + S(8), hy + hh - sh - S(1)), Look.Muted, TextFormatFlags.NoPrefix);
            }
        }

        void PaintRows(Graphics g, Item sec, int dy, int top, int bottom)
        {
            int start = items.IndexOf(sec) + 1;
            using (var line = new Pen(Look.Blend(Look.Card, Look.Border, 0.8f)))
                for (int i = start; i < items.Count && items[i].Kind == ItemKind.Row; i++)
                {
                    var it = items[i];
                    if (it.Top >= bottom) break;
                    if (it.Top + it.Height <= top) continue;
                    g.DrawLine(line, side + S(1), it.Top + dy, side + column - S(2), it.Top + dy);
                    PaintRow(g, it, dy);
                }
        }

        void PaintRow(Graphics g, Item it, int dy)
        {
            var o = it.Opt;
            bool notSet = NotSet(o);
            string current = model.Current.ContainsKey(o.Id) ? model.Current[o.Id] : Option.NotSet;
            bool changed = model.Loaded.ContainsKey(o.Id) && model.IsChanged(o);
            string error;
            model.Invalid.TryGetValue(o.Id, out error);
            int y0 = it.Top + dy;
            if (changed || error != null)
                using (var bar = new SolidBrush(error != null ? Look.Error : Look.Changed)) g.FillRectangle(bar, side, y0, S(3), it.Height);

            // title, key and note
            int x = side + Pad, tw = TextWidth;
            string name = changed ? o.Title + "  (unsaved)" : o.Title;
            int bold = Look.LineHeight(Look.Bold);
            var flags = TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix;
            TextRenderer.DrawText(g, name, Look.Bold, new Rectangle(x, y0 + S(12), tw, bold), changed ? Look.Changed : Look.Text, flags);
            int nameWidth = TextRenderer.MeasureText(name, Look.Bold).Width;
            int keyWidth = Look.OneLineWidth(o.Key, Look.Small);
            if (nameWidth + S(10) + keyWidth <= tw)
            {
                int sh = Look.LineHeight(Look.Small);
                TextRenderer.DrawText(g, o.Key, Look.Small, new Rectangle(x + nameWidth + S(10), y0 + S(12) + bold - sh - S(1), keyWidth, sh), Look.Muted, flags);
            }
            if (it.DescHeight > 0)
            {
                var d = DescRect(it);
                d.Offset(0, dy);
                TextRenderer.DrawText(g, o.Description, Look.Small, d, Look.Secondary,
                    TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
            }

            // value
            var well = WellRect(it);
            well.Offset(0, dy);
            bool hot = hotItem == it;
            if (notSet)
            {
                var r = new Rectangle(well.X, well.Y, well.Width - 1, well.Height - 1);
                Look.DrawRound(g, Look.Border, r, S(3));
                TextRenderer.DrawText(g, "not in file", Look.Base, new Rectangle(well.X + S(8), well.Y, well.Width - S(16), well.Height), Look.Muted,
                    TextFormatFlags.VerticalCenter | flags);
                if (model.Recommended[o.Id] != null) DrawButton(g, it, dy, "Add", true, hot && hotPart == Part.Add);
            }
            else if (o.Kind == Kind.Bool)
            {
                var sw = SwitchRect(it);
                sw.Offset(0, dy);
                DrawSwitch(g, sw, current == "1", hot && hotPart == Part.Switch);
            }
            else
            {
                bool active = editing == o;
                var r = new Rectangle(well.X, well.Y, well.Width - 1, well.Height - 1);
                Look.FillRound(g, Look.InputBack, r, S(3));
                Color border = active ? Look.Accent : error != null ? Look.Error : hot && hotPart == Part.Well ? Look.Muted : Look.InputBorder;
                Look.DrawRound(g, border, r, S(3));
                if (!active)
                {
                    string shown;
                    if (!typed.TryGetValue(o.Id, out shown)) shown = current;
                    TextRenderer.DrawText(g, shown, Look.Base, new Rectangle(well.X + S(8), well.Y, well.Width - S(16), well.Height),
                        error != null ? Look.Error : Look.Text, TextFormatFlags.VerticalCenter | flags);
                }
            }
            if (!notSet) DrawButton(g, it, dy, "Reset", CanReset(o), hot && hotPart == Part.Reset);

            var under = UnderRect(it);
            under.Offset(0, dy);
            string rec = model.Recommended[o.Id];
            string text = error ?? (rec != null ? "Recommended: " + o.Display(rec) : o.Description == ExpertKeys.UnknownNote ? "No recommendation" : "No template value");
            TextRenderer.DrawText(g, text, Look.Small, under, error != null ? Look.Error : Look.Muted, flags);

            // the same callout as the cards on the other pages
            if (it.Warning != null)
            {
                var box = new Rectangle(side + Pad, y0 + it.WarnTop, column - 2 * Pad, it.WarnHeight);
                Look.FillRound(g, Look.Blend(Look.Card, Look.Warning, 0.09f), box, S(4));
                using (var path = Look.Round(box, S(4)))
                {
                    var clip = g.Clip;
                    g.SetClip(path, CombineMode.Intersect);
                    using (var bar = new SolidBrush(Look.Warning)) g.FillRectangle(bar, box.X, box.Y, S(2), box.Height);
                    g.Clip = clip;
                }
                Look.DrawIcon(g, Tone.Warning, box.X + S(11), box.Y + S(8));
                TextRenderer.DrawText(g, it.Warning, Look.Small, new Rectangle(box.X + S(36), box.Y + S(7), box.Width - S(48), box.Height - S(13)), Look.Warning,
                    TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl | TextFormatFlags.NoPrefix);
            }
        }

        void DrawButton(Graphics g, Item it, int dy, string text, bool enabled, bool hover)
        {
            var r = ButtonRect(it);
            r.Offset(0, dy);
            Color back = !enabled ? Look.Card : hover ? Look.Hex(0x2c3036) : Look.Hex(0x22252a);
            using (var b = new SolidBrush(back)) g.FillRectangle(b, r);
            using (var pen = new Pen(enabled ? Look.InputBorder : Look.Border)) g.DrawRectangle(pen, r.X, r.Y, r.Width - 1, r.Height - 1);
            TextRenderer.DrawText(g, text, Look.Base, r, enabled ? Look.Text : Look.Muted,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix);
        }

        // The same switch as Theme.cs draws for the cards.
        static void DrawSwitch(Graphics g, Rectangle r, bool on, bool hover)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            int tw = Switch.TrackWidth, th = S(20);
            var track = new Rectangle(r.X + 1, r.Y + (r.Height - th) / 2, tw - 1, th - 1);
            Color fill = on ? (hover ? Look.AccentHover : Look.Accent) : (hover ? Look.Hex(0x3a3f47) : Look.Hex(0x2e3238));
            Look.FillRound(g, fill, track, th / 2);
            if (!on) Look.DrawRound(g, Look.InputBorder, track, th / 2);
            int knob = th - S(6);
            int kx = on ? track.Right - S(3) - knob : track.X + S(3) + 1;
            using (var brush = new SolidBrush(on ? Color.White : Look.Hex(0xaab0b8)))
                g.FillEllipse(brush, kx, track.Y + (track.Height - knob) / 2f + 0.5f, knob, knob);
            TextRenderer.DrawText(g, on ? "On" : "Off", Look.Base, new Rectangle(r.X + tw + S(10), r.Y, r.Width - tw - S(10), r.Height), Look.Text,
                TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix);
        }

        // ---- input

        Point ContentPoint(Point client) { return new Point(client.X, client.Y - AutoScrollPosition.Y); }

        void InvalidateItem(Item it)
        {
            if (it == null) return;
            Invalidate(new Rectangle(0, it.Top + AutoScrollPosition.Y, Width, it.Height));
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            var p = ContentPoint(e.Location);
            var it = RowAt(p.Y);
            var part = PartAt(it, p);
            if (it == hotItem && part == hotPart) return;
            InvalidateItem(hotItem);
            hotItem = it;
            hotPart = part;
            InvalidateItem(hotItem);
            Cursor = part == Part.None || part == Part.Description ? Cursors.Default : part == Part.Well ? Cursors.IBeam : Cursors.Hand;
            string tip = "";
            if (it != null && part == Part.Description) tip = it.Opt.Description;
            else if (it != null && part == Part.Well)
            {
                string note = model.FileNote(it.Opt);
                tip = "[" + it.Opt.Section + "] " + it.Opt.Key + "=" + model.Current[it.Opt.Id] + (note != null ? Environment.NewLine + note : "");
            }
            Look.Tips.SetToolTip(this, tip);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            InvalidateItem(hotItem);
            hotItem = null;
            hotPart = Part.None;
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left) return;
            var p = ContentPoint(e.Location);
            var it = RowAt(p.Y);
            var part = PartAt(it, p);
            if (part != Part.Well) { EndEdit(true); Focus(); }
            if (it == null || Edited == null) return;
            var o = it.Opt;
            switch (part)
            {
                case Part.Switch:
                    typed.Remove(o.Id);
                    Edited(o, model.Current[o.Id] == "1" ? "0" : "1", null);
                    break;
                case Part.Well:
                    if (editing != o) { EndEdit(true); BeginEdit(it); }
                    break;
                case Part.Reset:
                    if (ResetClicked != null) ResetClicked(o);
                    break;
                case Part.Add:
                    Edited(o, model.Recommended[o.Id], null);
                    break;
            }
            InvalidateItem(it);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            EndEdit(true);
            ScrollTo(ScrollY - e.Delta * S(60) / 120);
            var handled = e as HandledMouseEventArgs;
            if (handled != null) handled.Handled = true;
        }

        protected override void OnScroll(ScrollEventArgs se)
        {
            EndEdit(true);
            base.OnScroll(se);
            Invalidate();
        }

        protected override bool IsInputKey(Keys keyData)
        {
            return keyData == Keys.Up || keyData == Keys.Down || base.IsInputKey(keyData);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            int page = Math.Max(S(40), ClientSize.Height - S(40));
            switch (e.KeyCode)
            {
                case Keys.Up: ScrollTo(ScrollY - S(40)); break;
                case Keys.Down: ScrollTo(ScrollY + S(40)); break;
                case Keys.PageUp: ScrollTo(ScrollY - page); break;
                case Keys.PageDown: ScrollTo(ScrollY + page); break;
                case Keys.Home: ScrollTo(0); break;
                case Keys.End: ScrollTo(contentHeight); break;
            }
            base.OnKeyDown(e);
        }

        protected override void OnResize(EventArgs e)
        {
            EndEdit(true);
            base.OnResize(e);
        }

        void BeginEdit(Item it)
        {
            var o = it.Opt;
            editing = o;
            var w = WellRect(it);
            box.Font = Look.Base;
            string text;
            if (!typed.TryGetValue(o.Id, out text)) text = model.Current[o.Id];
            box.Text = text;
            box.Bounds = new Rectangle(w.X + S(8), w.Y + AutoScrollPosition.Y + (w.Height - box.Height) / 2, w.Width - S(16), box.Height);
            box.Visible = true;
            box.Focus();
            box.SelectAll();
            InvalidateItem(it);
        }

        void EndEdit(bool commit)
        {
            if (editing == null) return;
            var o = editing;
            editing = null;
            string text = box.Text;
            box.Visible = false;
            if (commit) Apply(o, text);
            Invalidate();
        }

        void Apply(Option o, string text)
        {
            string value = text.Trim(), error;
            if (o.Kind == Kind.Int)
            {
                long v;
                if (long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v))
                {
                    value = v.ToString(CultureInfo.InvariantCulture);
                    error = o.Check(value);
                }
                else error = "Enter a whole number.";
            }
            else error = o.Check(value);
            if (error != null)
            {
                typed[o.Id] = text;
                if (Edited != null) Edited(o, null, error);
                return;
            }
            typed.Remove(o.Id);
            if (Edited != null && (value != model.Current[o.Id] || model.Invalid.ContainsKey(o.Id))) Edited(o, value, null);
        }

        // For --render: a value typed into a row as a player would.
        public void Type(Option o, string text)
        {
            var it = items.FirstOrDefault(i => i.Opt == o);
            if (it == null) return;
            ScrollTo(it.Top - S(120));
            BeginEdit(it);
            box.Text = text;
            EndEdit(true);
        }

        public void Toggle(Option o)
        {
            if (Edited != null) Edited(o, model.Current[o.Id] == "1" ? "0" : "1", null);
        }

        public int TopOf(Option o)
        {
            var it = items.FirstOrDefault(i => i.Opt == o);
            return it == null ? 0 : it.Top;
        }
    }
}
