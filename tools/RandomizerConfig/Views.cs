// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace DS2CoopConfig
{
    interface IView
    {
        Control Root { get; }
        string Group { get; }
        void Refresh(Model model);
        bool Matches(string filter);
        void SetWidth(int width);
    }

    // Rows are placed by hand: nested auto-size panels took seconds per section switch.
    static class Row
    {
        public const int Pad = 18, Band = 14, BandHeight = 30;

        public static Surface Card()
        {
            return new Surface { Rounded = true, BackColor = Look.Card, ForeColor = Look.Text, Margin = new Padding(0, 0, 0, Look.S(10)) };
        }

        public static void PlaceInput(Editor e, int x, int y)
        {
            e.Control.Location = new Point(x + (e.Input != null ? e.WellLeft : 0), y);
        }

        public static int InputRight(Editor e)
        {
            return e.Control.Right + (e.Input != null ? e.WellRight : 0);
        }
    }

    // A single option: title, control, reset button, description and messages.
    sealed class OptionView : IView
    {
        readonly Option opt;
        readonly Editor editor;
        readonly Surface panel;
        readonly Label title, keyLabel, description, recommended;
        readonly Button reset;
        readonly MessageLine fileNote, note, warning, error;
        readonly string searchText;
        int width = 900;

        public OptionView(Option o, Editor e, Action<Option> onReset)
        {
            opt = o;
            editor = e;
            panel = Row.Card();
            title = Look.MakeLabel(o.Title, Look.Bold, Look.Text);
            keyLabel = Look.MakeLabel("[" + o.Section + "] " + o.Key, Look.Small, Look.Muted);
            reset = Look.MakeButton("Reset", false, Look.S(28));
            reset.Click += delegate { onReset(opt); };
            Look.Tips.SetToolTip(reset, "Reset this option to the recommended value");
            recommended = Look.MakeLabel("", Look.Small, Look.Muted);
            description = Look.MakeLabel(o.Description, null, Look.Secondary);
            panel.Controls.AddRange(new Control[] { title, keyLabel, e.Control, reset, recommended, description });
            if (e.Input != null) panel.AddWell(e.Input, e.WellLeft, e.WellRight);
            fileNote = new MessageLine(panel, Tone.Info);
            note = new MessageLine(panel, Tone.Note);
            warning = new MessageLine(panel, Tone.Warning);
            error = new MessageLine(panel, Tone.Error);
            note.Set(o.NoteText);
            searchText = (o.Title + " " + o.Key + " " + o.Section + " " + o.Description + " " + o.NoteText).ToLowerInvariant();
            Arrange();
        }

        public Control Root { get { return panel; } }
        public string Group { get { return opt.Group; } }
        public bool Matches(string filter) { return filter.Length == 0 || searchText.Contains(filter); }

        public void SetWidth(int w)
        {
            width = w;
            Arrange();
        }

        void Arrange()
        {
            int pad = Look.S(Row.Pad), inner = width - 2 * pad;
            int band = Look.S(Row.Band), bandHeight = Look.S(Row.BandHeight);
            int column = Math.Max(Look.S(190), Math.Min(Look.S(300), width * 28 / 100));
            int lineHeight = Look.LineHeight(Look.Bold);
            var c = editor.Control;
            reset.Location = new Point(width - pad - reset.Width, band + (bandHeight - reset.Height) / 2);
            int inputWidth = c.Width + (editor.Input != null ? editor.WellLeft + editor.WellRight : 0);
            // a wide input that does not fit next to the title gets its own line
            bool stacked = pad + column + Look.S(20) + inputWidth > reset.Left - Look.S(16);
            if (stacked) column = reset.Left - Look.S(16) - pad;
            int left = Look.Place(title, pad, band + (bandHeight - lineHeight) / 2, column);
            left = Look.Place(keyLabel, pad, left, column);

            int inputX = stacked ? pad : pad + column + Look.S(20);
            int rowTop = stacked ? left + Look.S(8) : band;
            Row.PlaceInput(editor, inputX, rowTop + (bandHeight - c.Height) / 2);
            int rx = Row.InputRight(editor) + Look.S(20);
            int rwidth = (stacked ? width - pad : reset.Left - Look.S(16)) - rx;
            int right = rowTop + bandHeight;
            string oneLine = recText + (rangeText.Length > 0 ? "      " + rangeText : "");
            recommended.Text = rangeText.Length == 0 || rwidth >= Look.OneLineWidth(oneLine, recommended.Font) ? oneLine : recText + Environment.NewLine + rangeText;
            if (rwidth >= Look.S(120))
            {
                int h = Look.Measure(recommended.Text, recommended.Font, rwidth);
                right = Math.Max(right, Look.Place(recommended, rx, rowTop + Math.Max(0, (bandHeight - h) / 2), rwidth));
            }
            else
            {
                // narrow window: the recommendation moves below the input
                right = Look.Place(recommended, inputX, right + Look.S(4), width - pad - inputX);
            }
            if (stacked) left = right;

            int y = Math.Max(left, right) + Look.S(10);
            y = Look.Place(description, pad, y, inner);
            int bottom = y;
            y += Look.S(8);
            foreach (var m in new[] { fileNote, note, warning, error })
            {
                int next = m.Place(pad, y, inner);
                if (next != y) { bottom = m.Rect.Bottom; y = next; }
            }
            panel.Changed = changed;
            panel.Size = new Size(width, bottom + Look.S(16));
            panel.Invalidate();
        }

        bool changed;
        string recText = "", rangeText = "";

        public void Refresh(Model model)
        {
            bool isChanged = model.IsChanged(opt);
            bool moved = false;
            string text = isChanged ? opt.Title + "  (unsaved)" : opt.Title;
            if (title.Text != text) { title.Text = text; moved = true; }
            title.ForeColor = isChanged ? Look.Changed : Look.Text;
            if (changed != isChanged) { changed = isChanged; panel.Changed = isChanged; panel.Invalidate(); }
            string rec = "Recommended: " + opt.Display(model.Recommended[opt.Id]);
            string range = opt.Kind == Kind.Int || opt.Kind == Kind.OptionalInt ? "Range: " + opt.Min + " to " + opt.Max : "";
            if (recText != rec || rangeText != range) { recText = rec; rangeText = range; moved = true; }
            moved |= fileNote.Set(model.FileNote(opt));
            moved |= note.Set(NoteFor(model));
            string err;
            model.Invalid.TryGetValue(opt.Id, out err);
            moved |= error.Set(err);
            moved |= warning.Set(err == null && opt.WarnFunc != null ? opt.WarnFunc(model) : null);
            if (moved) Arrange();
        }

        string NoteFor(Model model)
        {
            if (opt.HintFunc != null) return opt.HintFunc(model) ?? opt.NoteText;
            if (opt.KeyCode)
            {
                long code = model.Int(opt.Id);
                return code == 0 ? "No key set." : "Key: " + VirtualKeys.Name(code) + ".";
            }
            return opt.NoteText;
        }
    }

    // A sub-heading between cards; hidden while searching.
    sealed class HeadingView : IView
    {
        readonly Label label;
        readonly string group;

        public HeadingView(string group, string text, bool first)
        {
            this.group = group;
            label = new Label
            {
                AutoSize = true, Text = text, Font = Look.Heading, ForeColor = Look.Text, UseMnemonic = false,
                Margin = new Padding(0, Look.S(first ? 0 : 16), 0, Look.S(10))
            };
        }

        public Control Root { get { return label; } }
        public string Group { get { return group; } }
        public void Refresh(Model model) { }
        public bool Matches(string filter) { return filter.Length == 0; }
        public void SetWidth(int width) { }
    }

    // A [Section] of family=value lines shown as a grid.
    sealed class TableView : IView
    {
        readonly TableInfo info;
        readonly Surface panel;
        readonly Label title, section, description;
        readonly Button reset;
        readonly MessageLine note, warning, error;
        readonly List<Editor> editors = new List<Editor>();
        readonly List<Label> cellLabels = new List<Label>();
        readonly string searchText;
        readonly int labelWidth, inputWidth;
        bool changed;
        int width = 900;

        public TableView(TableInfo t, Func<Option, Editor> create, Action<Option> onReset)
        {
            info = t;
            panel = Row.Card();
            title = Look.MakeLabel(t.Title, Look.Heading, Look.Text);
            section = Look.MakeLabel("[" + t.Section + "]", Look.Small, Look.Muted);
            reset = Look.MakeButton("Reset table", false, Look.S(28));
            reset.Click += delegate { foreach (var c in info.Cells) onReset(c); };
            Look.Tips.SetToolTip(reset, "Reset every value in this table to the recommended value");
            description = Look.MakeLabel(t.Description, null, Look.Secondary);
            panel.Controls.AddRange(new Control[] { title, section, reset, description });
            note = new MessageLine(panel, Tone.Note);
            warning = new MessageLine(panel, Tone.Warning);
            error = new MessageLine(panel, Tone.Error);
            note.Set(t.NoteText);
            // one menu per table; the clicked cell comes from SourceControl
            var menu = new ContextMenuStrip();
            var owners = new Dictionary<Control, Option>();
            menu.Items.Add("Reset to recommended").Click += delegate
            {
                Option cell;
                if (menu.SourceControl != null && owners.TryGetValue(menu.SourceControl, out cell)) onReset(cell);
            };
            foreach (var cell in t.Cells)
            {
                var editor = create(cell);
                editors.Add(editor);
                var label = Look.MakeLabel(Schema.FamilyLabel(cell.Key), null, Look.Text);
                label.AutoEllipsis = true;
                cellLabels.Add(label);
                label.ContextMenuStrip = menu;
                editor.Control.ContextMenuStrip = menu;
                owners[label] = cell;
                owners[editor.Control] = cell;
                panel.Controls.Add(label);
                panel.Controls.Add(editor.Control);
                if (editor.Input != null) panel.AddWell(editor.Input, editor.WellLeft, editor.WellRight);
                labelWidth = Math.Max(labelWidth, Look.OneLineWidth(label.Text + " *", Look.Base));
                inputWidth = Math.Max(inputWidth, editor.Control.Width + (editor.Input != null ? editor.WellLeft + editor.WellRight : 0));
            }
            labelWidth = Math.Min(labelWidth, Look.S(230));
            searchText = (t.Title + " " + t.Section + " " + t.Description + " " + t.NoteText + " " +
                string.Join(" ", t.Cells.Select(c => Schema.FamilyLabel(c.Key)).ToArray())).ToLowerInvariant();
            Arrange();
        }

        public Control Root { get { return panel; } }
        public string Group { get { return info.Group; } }
        public bool Matches(string filter) { return filter.Length == 0 || searchText.Contains(filter); }

        public void SetWidth(int w)
        {
            width = w;
            Arrange();
        }

        void Arrange()
        {
            int pad = Look.S(Row.Pad), inner = width - 2 * pad;
            int band = Look.S(Row.Band), bandHeight = Look.S(Row.BandHeight);
            reset.Location = new Point(width - pad - reset.Width, band + (bandHeight - reset.Height) / 2);
            int titleHeight = Look.LineHeight(Look.Heading);
            int titleWidth = Math.Min(Look.OneLineWidth(title.Text, Look.Heading), reset.Left - pad - Look.S(16));
            title.Bounds = new Rectangle(pad, band + (bandHeight - titleHeight) / 2, titleWidth, titleHeight);
            int smallHeight = Look.LineHeight(Look.Small);
            int sectionX = title.Right + Look.S(8);
            section.Bounds = new Rectangle(sectionX, title.Bottom - smallHeight - Look.S(1), Math.Max(0, Math.Min(Look.OneLineWidth(section.Text, Look.Small), reset.Left - sectionX - Look.S(8))), smallHeight);
            int y = band + bandHeight + Look.S(8);
            y = Look.Place(description, pad, y, inner) + Look.S(8);
            foreach (var m in new[] { note, warning, error }) y = m.Place(pad, y, inner);

            int cellWidth = labelWidth + Look.S(12) + inputWidth + Look.S(28);
            int columns = Math.Max(1, inner / cellWidth);
            cellWidth = inner / columns;
            int rowHeight = Look.S(38), lineHeight = Look.LineHeight(Look.Base);
            int rows = (editors.Count + columns - 1) / columns;
            panel.Stripes.Clear();
            for (int r = 0; r < rows; r += 2)
                panel.Stripes.Add(new Rectangle(pad - Look.S(8), y + r * rowHeight, inner + Look.S(16), rowHeight));
            var stripe = Look.Blend(Look.Card, Color.White, Surface.StripeAlpha);
            for (int i = 0; i < editors.Count; i++)
            {
                int cx = pad + (i % columns) * cellWidth, cy = y + (i / columns) * rowHeight;
                var back = (i / columns) % 2 == 0 ? stripe : Look.Card;
                cellLabels[i].Bounds = new Rectangle(cx, cy + (rowHeight - lineHeight) / 2, labelWidth, lineHeight);
                cellLabels[i].BackColor = back;
                var c = editors[i].Control;
                if (editors[i].Input == null) c.BackColor = back;
                Row.PlaceInput(editors[i], cx + labelWidth + Look.S(12), cy + (rowHeight - c.Height) / 2);
            }
            y += rows * rowHeight;
            panel.Changed = changed;
            panel.Size = new Size(width, y + Look.S(14));
            panel.Invalidate();
        }

        public void Refresh(Model model)
        {
            bool any = false, moved = false;
            var warnings = new List<KeyValuePair<string, string>>();
            var errors = new List<string>();
            for (int i = 0; i < info.Cells.Count; i++)
            {
                var cell = info.Cells[i];
                bool isChanged = model.IsChanged(cell);
                any |= isChanged;
                string err;
                model.Invalid.TryGetValue(cell.Id, out err);
                string warn = err == null && cell.WarnFunc != null ? cell.WarnFunc(model) : null;
                if (err != null) errors.Add(cell.Key + ": " + err);
                if (warn != null) warnings.Add(new KeyValuePair<string, string>(cell.Key, warn));
                var label = cellLabels[i];
                label.ForeColor = err != null ? Look.Error : warn != null ? Look.Warning : isChanged ? Look.Changed : Look.Text;
                string text = Schema.FamilyLabel(cell.Key) + (isChanged ? " *" : "");
                if (label.Text != text) label.Text = text;
            }
            string heading = any ? info.Title + "  (unsaved)" : info.Title;
            if (title.Text != heading) { title.Text = heading; moved = true; }
            title.ForeColor = any ? Look.Changed : Look.Text;
            if (changed != any) { changed = any; panel.Changed = any; panel.Invalidate(); }
            // one line per distinct message, listing the kinds it applies to
            var lines = warnings.GroupBy(w => w.Value)
                .Select(g => string.Join(", ", g.Select(w => w.Key).ToArray()) + ": " + g.Key).ToArray();
            moved |= warning.Set(string.Join(Environment.NewLine, lines));
            moved |= error.Set(string.Join(Environment.NewLine, errors.ToArray()));
            if (moved) Arrange();
        }
    }
}
