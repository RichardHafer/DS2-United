using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("DS2 Randomizer Config")]
[assembly: System.Reflection.AssemblyProduct("Dead Space 2 Randomizer")]
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]

namespace DS2RandomizerConfig
{
    static class Program
    {
        public const string IniName = "DS2Randomizer.ini";

        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length >= 1 && args[0] == "--selftest") return SelfTest.Run(args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length >= 3 && args[0] == "--render")
            {
                // --render <ini> <outDir> [scale] [clientWidth clientHeight]
                float scale = args.Length >= 4 ? float.Parse(args[3], CultureInfo.InvariantCulture) : 0f;
                int w = args.Length >= 6 ? int.Parse(args[4], CultureInfo.InvariantCulture) : 1280;
                int h = args.Length >= 6 ? int.Parse(args[5], CultureInfo.InvariantCulture) : 800;
                Look.Init(scale);
                return Render(args[1], args[2], w, h);
            }
            Look.Init(0f);
            string path = args.Length >= 1 && File.Exists(args[0]) ? Path.GetFullPath(args[0]) : Locate();
            if (path == null) return 1;
            Application.Run(new MainForm(path));
            return 0;
        }

        // Layout check without showing a window: one PNG per section, nothing saved.
        static int Render(string ini, string outDir, int width, int height)
        {
            var clock = Stopwatch.StartNew();
            Directory.CreateDirectory(outDir);
            using (var form = new MainForm(Path.GetFullPath(ini)))
            {
                long built = clock.ElapsedMilliseconds;
                // fully transparent and far off-screen, so nobody sees it
                form.ShowInTaskbar = false;
                form.Opacity = 0;
                form.StartPosition = FormStartPosition.Manual;
                form.Location = new Point(-30000, -30000);
                form.MinimumSize = Size.Empty;
                form.MaximumSize = new Size(Look.S(width) * 2, Look.S(height) * 2);
                form.ClientSize = new Size(Look.S(width), Look.S(height));
                form.Show();
                Application.DoEvents();
                long shown = clock.ElapsedMilliseconds;
                form.TimeRefresh(outDir, built, shown);
                form.RenderTabs(outDir);
                form.RenderSearch(outDir, "vent");
                form.RenderEdited(outDir);
                form.DiscardChanges();
                form.Hide();
            }
            return 0;
        }

        static string Locate()
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            foreach (var candidate in new[] { Path.Combine(dir, "plugins", IniName), Path.Combine(dir, IniName) })
                if (File.Exists(candidate)) return candidate;
            MessageBox.Show("Could not find plugins\\" + IniName + " next to this program.\n\n" +
                "Put this program into the Dead Space 2 folder (next to deadspace2.exe), or pick the file now.",
                "DS2 Randomizer Config", MessageBoxButtons.OK, MessageBoxIcon.Information);
            using (var dialog = new OpenFileDialog())
            {
                dialog.Title = "Find " + IniName;
                dialog.Filter = IniName + "|" + IniName + "|INI files (*.ini)|*.ini";
                dialog.FileName = IniName;
                return dialog.ShowDialog() == DialogResult.OK ? dialog.FileName : null;
            }
        }

        public static Stream Resource(string name)
        {
            return typeof(Program).Assembly.GetManifestResourceStream("DS2RandomizerConfig." + name);
        }
    }

    // Scrolling page for one section.
    sealed class Page : FlowLayoutPanel
    {
        public Page()
        {
            DoubleBuffered = true;
            Dock = DockStyle.Fill;
            AutoScroll = true;
            FlowDirection = FlowDirection.TopDown;
            WrapContents = false;
            BackColor = Look.Back;
            Visible = false;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.DarkScrollBars(this);
        }
    }

    sealed class MainForm : Form
    {
        readonly string iniPath;
        readonly Model model;
        byte[] loadedBytes;
        readonly List<IView> views = new List<IView>();
        readonly Dictionary<string, Editor> editors = new Dictionary<string, Editor>();
        readonly Dictionary<string, Page> pages = new Dictionary<string, Page>();
        readonly Dictionary<string, Label> intros = new Dictionary<string, Label>();
        readonly Panel content;
        readonly NavList nav;
        readonly TextBox search;
        readonly StatusLine status;
        bool refreshing;
        readonly Timer resizeTimer, searchTimer;
        readonly HashSet<IView> hidden = new HashSet<IView>();

        // Every label or size change would otherwise lay out the whole page again.
        void Batch(Action action)
        {
            var rows = views.ToArray();
            SuspendLayout();
            foreach (var flow in pages.Values) flow.SuspendLayout();
            foreach (var view in rows) view.Root.SuspendLayout();
            try { action(); }
            finally
            {
                foreach (var view in rows) view.Root.ResumeLayout(false);
                foreach (var view in views) view.Root.PerformLayout();
                foreach (var flow in pages.Values) flow.ResumeLayout(true);
                ResumeLayout(true);
            }
        }

        public MainForm(string path)
        {
            iniPath = path;
            Text = "Dead Space 2 Randomizer - Settings";
            // the layout is scaled by hand through Look.S, so WinForms must not scale it again
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoScaleDimensions = CurrentAutoScaleDimensions;
            Font = Look.Base;
            BackColor = Look.Back;
            ForeColor = Look.Text;
            var area = Screen.PrimaryScreen.WorkingArea;
            Size = new Size(Math.Min(Look.S(1280), area.Width), Math.Min(Look.S(820), area.Height));
            MinimumSize = new Size(Math.Min(Look.S(900), area.Width), Math.Min(Look.S(600), area.Height));
            StartPosition = FormStartPosition.CenterScreen;
            KeyPreview = true;
            try
            {
                using (var stream = Program.Resource("App.ico"))
                    if (stream != null) Icon = new Icon(stream);
            }
            catch (Exception) { }

            model = new Model(Schema.Build(), Model.LoadRecommended());

            Image logo = null;
            try
            {
                using (var stream = Program.Resource("Logo.png"))
                    if (stream != null) logo = new Bitmap(stream);
            }
            catch (Exception) { }
            var banner = new Banner(logo, iniPath) { Dock = DockStyle.Top };
            if (logo != null) logo.Dispose();
            Look.Tips.SetToolTip(banner, iniPath);

            // left: search and sections
            var sidebar = new Surface { Dock = DockStyle.Left, Width = Look.S(244), BackColor = Look.Panel, Edges = AnchorStyles.Right };
            search = new TextBox { BorderStyle = BorderStyle.None, BackColor = Look.InputBack, ForeColor = Look.Text, Font = Look.Base };
            search.HandleCreated += delegate { Native.CueBanner(search, "Search options"); };
            searchTimer = new Timer { Interval = 300 };
            searchTimer.Tick += delegate { searchTimer.Stop(); ApplyFilter(); };
            search.TextChanged += delegate { searchTimer.Stop(); searchTimer.Start(); };
            search.KeyDown += (s, e) =>
            {
                if (e.KeyCode != Keys.Escape) return;
                search.Text = "";
                e.SuppressKeyPress = true;
            };
            nav = new NavList(Schema.Groups);
            nav.SelectedChanged += delegate { ShowPage(nav.SelectedIndex); };
            var hint = Look.MakeLabel("The game reads this file only at startup. Restart it after saving.", Look.Small, Look.Muted);
            sidebar.Controls.Add(search);
            sidebar.Controls.Add(nav);
            sidebar.Controls.Add(hint);
            sidebar.AddWell(search, Look.S(30), Look.S(8));
            sidebar.Paint += (s, e) => DrawMagnifier(e.Graphics, search.Left - Look.S(22), search.Top + search.Height / 2);
            sidebar.Resize += delegate
            {
                int pad = Look.S(16), well = Look.S(Surface.WellHeight);
                int inner = sidebar.Width - 1 - 2 * pad;
                search.Bounds = new Rectangle(pad + Look.S(30), pad + (well - search.Height) / 2, inner - Look.S(38), search.Height);
                int top = pad + well + Look.S(12);
                int h = Look.Measure(hint.Text, hint.Font, inner);
                int list = nav.Count * nav.ItemHeight + Look.S(4);
                // the hint gives way when the window is too short for the whole list
                hint.Visible = top + list + Look.S(8) + h + pad <= sidebar.Height;
                hint.Bounds = new Rectangle(pad, sidebar.Height - pad - h, inner, h);
                nav.Bounds = new Rectangle(0, top, sidebar.Width - 1, Math.Max(0, (hint.Visible ? hint.Top - Look.S(8) : sidebar.Height) - top));
                sidebar.Invalidate();
            };

            // bottom: actions and status
            var bar = new Surface { Dock = DockStyle.Bottom, Height = Look.S(62), BackColor = Look.Panel, Edges = AnchorStyles.Top };
            int buttonHeight = Look.S(34);
            var save = MakeButton("Save", true, delegate { Save(); }, "Write your changes (a backup is made first, the two newest are kept)  Ctrl+S");
            var reload = MakeButton("Reload", false, delegate { Reload(true); }, "Read the file again and drop unsaved changes");
            var resetAll = MakeButton("Reset all to recommended", false, delegate { ResetAll(); }, "Every option back to the release settings (seeds are kept)");
            var folder = MakeButton("Open plugins folder", false, delegate { OpenFolder(); }, null);
            var log = MakeButton("Open log", false, delegate { OpenLog(); }, "plugins\\DS2Randomizer.log");
            status = new StatusLine { BackColor = Look.Panel, Font = Look.Base };
            bar.Controls.AddRange(new Control[] { folder, log, status, resetAll, reload, save });
            bar.Resize += delegate
            {
                int pad = Look.S(16), gap = Look.S(8), y = (bar.Height - buttonHeight) / 2;
                folder.Location = new Point(pad, y);
                log.Location = new Point(folder.Right + gap, y);
                save.Location = new Point(bar.Width - pad - save.Width, y);
                reload.Location = new Point(save.Left - gap - reload.Width, y);
                resetAll.Location = new Point(reload.Left - gap - resetAll.Width, y);
                int x0 = log.Right + Look.S(24), x1 = resetAll.Left - Look.S(24);
                status.Visible = x1 - x0 > Look.S(80);
                status.Bounds = new Rectangle(x0, Look.S(6), Math.Max(0, x1 - x0), bar.Height - Look.S(12));
            };

            content = new Panel { Dock = DockStyle.Fill, BackColor = Look.Back };
            foreach (var group in Schema.Groups)
            {
                var flow = new Page();
                var heading = new Label { AutoSize = true, Text = group, Font = Look.PageTitle, ForeColor = Look.Text, UseMnemonic = false, Margin = new Padding(0, 0, 0, Look.S(6)) };
                var intro = new Label { AutoSize = true, Text = Schema.GroupIntro[group], ForeColor = Look.Secondary, UseMnemonic = false, Margin = new Padding(0, 0, 0, Look.S(18)) };
                flow.Controls.Add(heading);
                flow.Controls.Add(intro);
                intros[group] = intro;
                content.Controls.Add(flow);
                pages[group] = flow;
            }
            pages[Schema.Groups[0]].Visible = true;

            Batch(() =>
            {
                BuildViews();
                Controls.Add(content);
                Controls.Add(sidebar);
                Controls.Add(bar);
                Controls.Add(banner);
            });

            Reload(false);
            // widths follow the window once it settles, not on every resize step
            resizeTimer = new Timer { Interval = 150 };
            resizeTimer.Tick += delegate { resizeTimer.Stop(); UpdateWidths(); };
            Resize += delegate { resizeTimer.Stop(); resizeTimer.Start(); };
            Shown += delegate { UpdateWidths(); };
            FormClosing += OnClosing;
            KeyDown += OnShortcut;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.DarkTitleBar(Handle);
        }

        static Button MakeButton(string text, bool primary, EventHandler click, string tip)
        {
            var b = Look.MakeButton(text, primary, Look.S(34));
            b.Click += click;
            if (tip != null) Look.Tips.SetToolTip(b, tip);
            return b;
        }

        static void DrawMagnifier(Graphics g, int x, int y)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float r = Look.S(5);
            using (var pen = new Pen(Look.Muted, Math.Max(1.5f, Look.Scale * 1.6f)))
            {
                g.DrawEllipse(pen, x, y - r - Look.S(2), r * 2, r * 2);
                float cx = x + r, cy = y - Look.S(2);
                float d = r * 0.72f;
                g.DrawLine(pen, cx + d, cy + d, cx + d + Look.S(4), cy + d + Look.S(4));
            }
        }

        void OnShortcut(object sender, KeyEventArgs e)
        {
            if (e.Control && e.KeyCode == Keys.S) { e.SuppressKeyPress = true; Save(); }
            else if (e.Control && e.KeyCode == Keys.F) { e.SuppressKeyPress = true; search.Focus(); search.SelectAll(); }
        }

        void BuildViews()
        {
            Func<Option, Editor> create = o =>
            {
                var e = Editor.Create(o);
                e.Changed = OnEdited;
                editors[o.Id] = e;
                return e;
            };
            var tablesDone = new HashSet<string>();
            var roots = new Dictionary<string, Control>();
            foreach (var o in model.Options)
            {
                IView view;
                if (o.Table != null)
                {
                    if (!tablesDone.Add(o.Table)) continue;
                    var info = Schema.Tables.First(t => t.Section == o.Table);
                    view = new TableView(info, create, ResetOne);
                }
                else
                {
                    view = new OptionView(o, create(o), ResetOne);
                    roots[o.Id] = view.Root;
                }
                views.Add(view);
            }
            // switches first, tables after them within each section
            foreach (var view in views.Where(v => v is OptionView)) pages[view.Group].Controls.Add(view.Root);
            foreach (var view in views.Where(v => v is TableView)) pages[view.Group].Controls.Add(view.Root);
            // the Watchdog section ends with the advanced switches, after its tables
            var flow = pages[Schema.Watchdog];
            var advanced = new Label
            {
                AutoSize = true, Text = "Advanced: leave as is", Font = Look.Heading, ForeColor = Look.Warning,
                Margin = new Padding(0, Look.S(18), 0, Look.S(10))
            };
            flow.Controls.Add(advanced);
            string[] advancedKeys = { "PoolActorsFollowLevel", "ReleasePoolAtLevelEnd", "PoolHoldForever", "GuardServiceRelease",
                "PinPoolPrerequisites", "Variants", "VariantRuntime", "InstallDelayMs", "PlacementHotkey" };
            foreach (var key in advancedKeys)
            {
                Control root;
                if (roots.TryGetValue("DS2Randomizer." + key, out root)) flow.Controls.SetChildIndex(root, flow.Controls.Count - 1);
            }
            // FlowLayoutPanel ignores the bottom padding when it scrolls
            foreach (var page in pages.Values)
                page.Controls.Add(new Panel { Size = new Size(1, Look.S(14)), Margin = Padding.Empty, BackColor = Look.Back });
        }

        void ShowPage(int index)
        {
            string group = Schema.Groups[index];
            content.SuspendLayout();
            pages[group].Visible = true;
            foreach (var pair in pages)
                if (pair.Key != group) pair.Value.Visible = false;
            content.ResumeLayout(true);
        }

        void UpdateWidths() { Batch(SetWidths); }

        int lastWidth = -1;

        void SetWidths()
        {
            int hostWidth = content.ClientSize.Width;
            if (hostWidth == lastWidth) return;
            lastWidth = hostWidth;
            int scroll = SystemInformation.VerticalScrollBarWidth;
            int width = Math.Max(Look.S(520), Math.Min(Look.S(1160), hostWidth - scroll - 2 * Look.S(28)));
            // wide windows keep a readable column in the middle
            int side = Math.Max(Look.S(28), (hostWidth - scroll - width) / 2);
            foreach (var pair in pages)
            {
                pair.Value.Padding = new Padding(side, Look.S(24), side, Look.S(24));
                intros[pair.Key].MaximumSize = new Size(width, 0);
            }
            foreach (var view in views) view.SetWidth(width);
        }

        void OnEdited(Editor e)
        {
            if (refreshing) return;
            string value = e.Read();
            if (value == null)
            {
                model.Invalid[e.Opt.Id] = e.Error;
            }
            else
            {
                model.Invalid.Remove(e.Opt.Id);
                string before = model.Current[e.Opt.Id];
                model.Set(e.Opt, value);
                if (before != value) ShowFollowers(e.Opt);
            }
            RefreshAll();
        }

        void ShowFollowers(Option source)
        {
            refreshing = true;
            foreach (var o in model.Options)
                if (o.DefaultFrom == source.Id) editors[o.Id].Show(model.Current[o.Id]);
            refreshing = false;
        }

        void RefreshAll()
        {
            foreach (var view in views) view.Refresh(model);
            int changes = model.ChangeCount;
            string text = changes == 0 ? "No unsaved changes" : changes + " unsaved change" + (changes == 1 ? "" : "s");
            var tone = changes == 0 ? Tone.Neutral : Tone.Pending;
            if (model.Invalid.Count > 0)
            {
                text += ". " + model.Invalid.Count + " invalid value" + (model.Invalid.Count == 1 ? "" : "s") + ": fix before saving.";
                tone = Tone.Error;
            }
            for (int i = 0; i < Schema.Groups.Length; i++)
            {
                string group = Schema.Groups[i];
                nav.Changed[i] = model.Options.Any(o => o.Group == group && model.IsChanged(o));
            }
            if (search.Text.Trim().Length > 0)
            {
                nav.Matches = Schema.Groups.Select(g => views.Count(v => v.Group == g && !hidden.Contains(v))).ToArray();
                if (nav.Matches.All(n => n == 0)) text += "   Search: nothing found.";
            }
            else nav.Matches = null;
            nav.Invalidate();
            status.Set(text, tone);
        }

        void ShowAll()
        {
            Batch(() =>
            {
                refreshing = true;
                foreach (var o in model.Options) editors[o.Id].Show(model.Current[o.Id]);
                refreshing = false;
                RefreshAll();
            });
        }

        void ApplyFilter()
        {
            string filter = search.Text.Trim().ToLowerInvariant();
            hidden.Clear();
            foreach (var view in views) if (!view.Matches(filter)) hidden.Add(view);
            // hiding the pages first keeps each toggle from repainting them
            foreach (var flow in pages.Values)
            {
                flow.Visible = false;
                flow.AutoScrollPosition = Point.Empty;
            }
            Batch(() =>
            {
                foreach (var view in views) view.Root.Visible = !hidden.Contains(view);
                RefreshAll();
            });
            if (filter.Length > 0 && !views.Any(v => v.Group == Schema.Groups[nav.SelectedIndex] && !hidden.Contains(v)))
            {
                var first = views.FirstOrDefault(v => !hidden.Contains(v));
                if (first != null) nav.SelectedIndex = Array.IndexOf(Schema.Groups, first.Group);
            }
            ShowPage(nav.SelectedIndex);
        }

        void ResetOne(Option o)
        {
            model.Invalid.Remove(o.Id);
            model.Set(o, model.Recommended[o.Id]);
            refreshing = true;
            editors[o.Id].Show(model.Current[o.Id]);
            refreshing = false;
            ShowFollowers(o);
            RefreshAll();
        }

        void ResetAll()
        {
            if (MessageBox.Show(this, "Set every option to the recommended release value?\n\n" +
                "The seeds are kept as they are, so a running playthrough is not affected. Nothing is written until you click Save.",
                "Reset all", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            model.Invalid.Clear();
            foreach (var o in model.Options)
                if (!o.KeepOnResetAll) model.Current[o.Id] = model.Recommended[o.Id];
            ShowAll();
        }

        void Reload(bool ask)
        {
            if (ask && model.ChangeCount > 0 &&
                MessageBox.Show(this, "Drop your unsaved changes and read the file again?", "Reload",
                    MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            IniDocument doc;
            try
            {
                loadedBytes = File.ReadAllBytes(iniPath);
                doc = IniDocument.Parse(loadedBytes);
                model.LoadFrom(doc);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not read the file:\n" + ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                // nothing was ever loaded: fill the model so the window does not
                // throw on the first edit or on closing
                if (model.Current.Count == 0)
                {
                    model.LoadFrom(new IniDocument());
                    ShowAll();
                }
                return;
            }
            ShowAll();
            if (doc.Problem != null && !problemShown)
            {
                problemShown = true;
                MessageBox.Show(this, doc.Problem + "\n\nThe values shown may be wrong, and this window will not save the file.",
                    "File format", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        bool problemShown;

        bool Save()
        {
            // a value still being typed has not raised its event yet
            ValidateChildren();
            if (model.Invalid.Count > 0)
            {
                var lines = model.Invalid.Select(p => p.Key + ": " + p.Value).ToArray();
                MessageBox.Show(this, "Some values are invalid, nothing was saved:\n\n" + string.Join("\n", lines),
                    "Save", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            if (model.ChangeCount == 0)
            {
                status.Set("Nothing to save", Tone.Neutral);
                return true;
            }
            try
            {
                byte[] onDisk = File.ReadAllBytes(iniPath);
                if (!IniFile.SameBytes(onDisk, loadedBytes) &&
                    MessageBox.Show(this, "The file was changed by another program since it was loaded.\n\n" +
                        "Apply your changes on top of the new version? Other values from that version are kept.",
                        "Save", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return false;
                string backup;
                var changes = IniFile.Save(iniPath, model, out backup);
                Reload(false);
                string message = changes.Count == 0 ? "Nothing to save" :
                    "Saved " + changes.Count + " change" + (changes.Count == 1 ? "" : "s") + ", backup written: " + Path.GetFileName(backup);
                var tone = changes.Count == 0 ? Tone.Neutral : Tone.Good;
                if (Process.GetProcessesByName("deadspace2").Length > 0)
                {
                    message += ". Dead Space 2 is running: it reads this file only at startup, so restart the game.";
                    tone = Tone.Warning;
                }
                status.Set(message, tone);
                return true;
            }
            catch (SaveFailure ex)
            {
                string text = ex.FileIntact ? "Saving failed, the file was not changed:\n" + ex.Message
                    : "Saving failed and the file may be incomplete:\n" + ex.Message + "\n\nThe previous version is in\n" +
                      ex.BackupPath + "\nRename it to " + Path.GetFileName(iniPath) + " to restore it.";
                MessageBox.Show(this, text, "Save", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
            catch (Exception ex)
            {
                // only reading the file comes before the write
                MessageBox.Show(this, "Saving failed, the file was not changed:\n" + ex.Message, "Save", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        void OpenFolder()
        {
            try { Process.Start("explorer.exe", "/select,\"" + iniPath + "\""); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Open folder"); }
        }

        void OpenLog()
        {
            string log = Path.Combine(Path.GetDirectoryName(iniPath), "DS2Randomizer.log");
            if (!File.Exists(log))
            {
                MessageBox.Show(this, "There is no log yet:\n" + log + "\n\nIt is written when the game runs with the mod.", "Open log");
                return;
            }
            try { Process.Start("notepad.exe", "\"" + log + "\""); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Open log"); }
        }

        public void DiscardChanges() { Reload(false); }

        // The client area only, as the player sees it inside the window frame.
        Bitmap Snapshot()
        {
            using (var full = new Bitmap(Width, Height))
            {
                DrawToBitmap(full, new Rectangle(0, 0, Width, Height));
                var origin = PointToScreen(Point.Empty);
                var client = new Rectangle(origin.X - Left, origin.Y - Top, ClientSize.Width, ClientSize.Height);
                client.Intersect(new Rectangle(0, 0, full.Width, full.Height));
                return full.Clone(client, full.PixelFormat);
            }
        }

        void Shot(string outDir, string name)
        {
            using (var bmp = Snapshot()) bmp.Save(Path.Combine(outDir, name));
        }

        // Typed input as a player would leave it: one out of range, one risky.
        public void RenderEdited(string outDir)
        {
            nav.SelectedIndex = 0;
            pages[Schema.General].AutoScrollPosition = Point.Empty;
            Application.DoEvents();
            editors["DS2Randomizer.Chance"].Control.Text = "150";
            editors["DS2Randomizer.Seed"].Control.Text = "70";
            editors["DS2Randomizer.CrossFamily"].Show("0");
            OnEdited(editors["DS2Randomizer.CrossFamily"]);
            Application.DoEvents();
            Shot(outDir, "9_edited.png");
            editors["DS2Randomizer.PlanDump"].Control.Text = "not a guid";
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.Diagnostics);
            Application.DoEvents();
            ScrollTo(Schema.Diagnostics, editors["DS2Randomizer.PlanDump"].Control);
            Application.DoEvents();
            Shot(outDir, "11_error.png");
            editors["VentFamilies.Lurker"].Show(model.Current["VentFamilies.Lurker"] == "1" ? "0" : "1");
            OnEdited(editors["VentFamilies.Lurker"]);
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.Enemies);
            Application.DoEvents();
            ScrollTo(Schema.Enemies, editors["VentFamilies.Lurker"].Control);
            Application.DoEvents();
            Shot(outDir, "12_table_changed.png");
            File.AppendAllText(Path.Combine(outDir, "timing.txt"), "invalid=" + string.Join(";", model.Invalid.Select(p => p.Key + ":" + p.Value).ToArray()) +
                " changes=" + model.ChangeCount + Environment.NewLine);
        }

        void ScrollTo(string group, Control c)
        {
            var flow = pages[group];
            int y = c.Top + c.Parent.Top - flow.AutoScrollPosition.Y;
            flow.AutoScrollPosition = new Point(0, Math.Max(0, y - flow.ClientSize.Height / 2));
        }

        public void RenderSearch(string outDir, string text)
        {
            search.Text = text;
            ApplyFilter();
            Application.DoEvents();
            Shot(outDir, "10_search.png");
            search.Text = "";
            ApplyFilter();
            nav.SelectedIndex = 0;
            Application.DoEvents();
        }

        public void TimeRefresh(string outDir, long built, long shown)
        {
            var clock = Stopwatch.StartNew();
            for (int i = 0; i < 10; i++) RefreshAll();
            long refresh = clock.ElapsedMilliseconds / 10;
            // one edit that changes a title and a warning, and its undo
            var chance = model.ById["DS2Randomizer.Chance"];
            clock.Restart();
            editors[chance.Id].Show("0");
            OnEdited(editors[chance.Id]);
            Application.DoEvents();
            long edit = clock.ElapsedMilliseconds;
            ResetOne(chance);
            var pageTimes = new List<string>();
            for (int round = 0; round < 2; round++)
                for (int i = 0; i < Schema.Groups.Length; i++)
                {
                    clock.Restart();
                    nav.SelectedIndex = i;
                    Application.DoEvents();
                    pageTimes.Add(clock.ElapsedMilliseconds.ToString());
                }
            nav.SelectedIndex = 0;
            Application.DoEvents();
            clock.Restart();
            search.Text = "lurker";
            ApplyFilter();
            long filter = clock.ElapsedMilliseconds;
            search.Text = "";
            ApplyFilter();
            clock.Restart();
            ShowAll();
            long showAll = clock.ElapsedMilliseconds;
            File.WriteAllText(Path.Combine(outDir, "timing.txt"), "built_ms=" + built + " shown_ms=" + shown +
                " refresh_ms=" + refresh + " edit_ms=" + edit + " filter_ms=" + filter + " showall_ms=" + showAll + " tabs_ms=" + string.Join(",", pageTimes.ToArray()) + Environment.NewLine);
        }

        public void RenderTabs(string outDir)
        {
            Directory.CreateDirectory(outDir);
            UpdateWidths();
            for (int i = 0; i < Schema.Groups.Length; i++)
            {
                nav.SelectedIndex = i;
                Application.DoEvents();
                UpdateWidths();
                Application.DoEvents();
                var flow = pages[Schema.Groups[i]];
                int view = flow.ClientSize.Height, total = flow.DisplayRectangle.Height;
                for (int part = 0, y = 0; y < total && part < 8; part++, y += view - Look.S(40))
                {
                    flow.AutoScrollPosition = new Point(0, y);
                    Application.DoEvents();
                    Shot(outDir, (i + 1) + "_" + Schema.Groups[i].Replace(" & ", "_").Replace(" ", "_") + "_" + part + ".png");
                }
            }
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (model.ChangeCount == 0) return;
            var answer = MessageBox.Show(this, "Save your changes before closing?", "Unsaved changes",
                MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (answer == DialogResult.Cancel) e.Cancel = true;
            else if (answer == DialogResult.Yes && !Save()) e.Cancel = true;
        }
    }
}
