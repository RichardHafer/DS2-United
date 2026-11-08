using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;

namespace DS2CoopConfig
{
    // One input control bound to one option.
    abstract class Editor
    {
        public Option Opt;
        public Control Control;
        // the part that gets a dark input field drawn around it by the row, or null
        public Control Input;
        public int WellLeft, WellRight;
        public Action<Editor> Changed;
        public string Error;
        protected bool updating;

        public abstract void Show(string value);
        // The canonical value, or null while the input is invalid.
        public abstract string Read();

        protected void Raise()
        {
            if (!updating && Changed != null) Changed(this);
        }

        public static Editor Create(Option o)
        {
            switch (o.Kind)
            {
                case Kind.Bool: return new BoolEditor(o);
                case Kind.Int: return new IntEditor(o);
                case Kind.OptionalInt: return new OptionalIntEditor(o);
                default: return new TextEditor(o);
            }
        }
    }

    sealed class BoolEditor : Editor
    {
        readonly Switch box;

        public BoolEditor(Option o)
        {
            Opt = o;
            box = new Switch { Text = o.Table != null ? "" : "On" };
            box.FitText(o.Table != null ? null : "Off");
            box.CheckedChanged += delegate { box.Text = Opt.Table != null ? "" : (box.Checked ? "On" : "Off"); Raise(); };
            Control = box;
        }

        public override void Show(string value)
        {
            updating = true;
            box.Checked = value == "1";
            box.Text = Opt.Table != null ? "" : (box.Checked ? "On" : "Off");
            updating = false;
        }

        public override string Read() { Error = null; return box.Checked ? "1" : "0"; }
    }

    sealed class IntEditor : Editor
    {
        readonly SpinBox number;
        // a file value outside the box range and the clamped text shown for it;
        // Read returns the file value until the text is edited
        string kept, keptText;

        public IntEditor(Option o)
        {
            Opt = o;
            number = new SpinBox
            {
                Minimum = o.Min, Maximum = o.Max, DecimalPlaces = 0, ThousandsSeparator = false,
                Width = Look.S(o.Table != null ? 72 : (o.Max > 99999 ? 124 : 96)), TextAlign = HorizontalAlignment.Right
            };
            number.ValueChanged += delegate { Raise(); };
            number.TextChanged += delegate { Raise(); };
            Control = number;
            Input = number;
            WellLeft = Look.S(8);
            WellRight = Look.S(2);
        }

        public override void Show(string value)
        {
            updating = true;
            kept = null;
            decimal v;
            if (decimal.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v))
            {
                number.Value = Math.Max(number.Minimum, Math.Min(number.Maximum, v));
                if (number.Value != v) kept = value;
            }
            number.Text = number.Value.ToString(CultureInfo.InvariantCulture);
            keptText = number.Text.Trim();
            updating = false;
        }

        public override string Read()
        {
            string text = number.Text.Trim();
            if (kept != null)
            {
                if (text == keptText) { Error = null; return kept; }
                kept = null;
            }
            long v;
            if (!long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.CurrentCulture, out v))
            {
                Error = "Enter a whole number.";
                return null;
            }
            string value = v.ToString(CultureInfo.InvariantCulture);
            Error = Opt.Check(value);
            return Error == null ? value : null;
        }
    }

    sealed class OptionalIntEditor : Editor
    {
        readonly Switch follow;
        readonly SpinBox number;

        public OptionalIntEditor(Option o)
        {
            Opt = o;
            var panel = new Surface();
            follow = new Switch { Text = "Same as main seed" };
            follow.FitText(follow.Text);
            number = new SpinBox { Minimum = o.Min, Maximum = o.Max, Width = Look.S(124), TextAlign = HorizontalAlignment.Right };
            int h = Look.S(Surface.WellHeight);
            follow.Location = new Point(0, (h - follow.Height) / 2);
            number.Location = new Point(follow.Right + Look.S(18), (h - number.Height) / 2);
            panel.AddWell(number, Look.S(8), Look.S(2));
            panel.Size = new Size(number.Right + Look.S(3), h);
            follow.CheckedChanged += delegate { number.Enabled = !follow.Checked; panel.Invalidate(); Raise(); };
            number.ValueChanged += delegate { Raise(); };
            number.TextChanged += delegate { Raise(); };
            panel.Controls.Add(follow);
            panel.Controls.Add(number);
            Control = panel;
        }

        public override void Show(string value)
        {
            updating = true;
            follow.Checked = value == "";
            number.Enabled = !follow.Checked;
            decimal v;
            if (value != "" && decimal.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out v))
                number.Value = Math.Max(number.Minimum, Math.Min(number.Maximum, v));
            number.Text = number.Value.ToString(CultureInfo.InvariantCulture);
            updating = false;
        }

        public override string Read()
        {
            if (follow.Checked) { Error = null; return ""; }
            long v;
            if (!long.TryParse(number.Text.Trim(), NumberStyles.None, CultureInfo.CurrentCulture, out v))
            {
                Error = "Enter a whole number.";
                return null;
            }
            string value = v.ToString(CultureInfo.InvariantCulture);
            Error = Opt.Check(value);
            return Error == null ? value : null;
        }
    }

    sealed class TextEditor : Editor
    {
        readonly TextBox box;

        public TextEditor(Option o)
        {
            Opt = o;
            box = new TextBox { Width = Look.S(260), BorderStyle = BorderStyle.None, Font = Look.Base, BackColor = Look.InputBack, ForeColor = Look.Text };
            box.TextChanged += delegate { Raise(); };
            Control = box;
            Input = box;
            WellLeft = Look.S(8);
            WellRight = Look.S(8);
        }

        public override void Show(string value)
        {
            updating = true;
            box.Text = value;
            updating = false;
        }

        public override string Read()
        {
            Error = Opt.Check(box.Text);
            return Error == null ? box.Text : null;
        }
    }
}
