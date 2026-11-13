// Dead Space 2 United by Richard Hafer.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("Dead Space 2 United Config")]
[assembly: System.Reflection.AssemblyProduct("Dead Space 2 United")]
[assembly: System.Reflection.AssemblyVersion("0.1.0.0")]

namespace DS2CoopConfig
{
    static class Program
    {
        public const string IniName = Merger.FileName;
        public const string Caption = "Dead Space 2 United";
        // the release the zip carries; shown on every start of this window
        public const string ReleaseVersion = "0.1";
        public const string ExperimentalWarning =
            "Dead Space 2 United " + ReleaseVersion + " is a highly experimental release.\n\n" +
            "Expect crashes and bugs. Back up your save games before you play, and keep a copy of plugins\\DS2United.ini " +
            "before you change a lot here.\n\n" +
            "When something goes wrong, the logs are in the plugins folder (the Open log button). " +
            "Send them with a short note of where you were to the Dead Space Modding Discord: https://discord.com/invite/sWMRkYJ5JV";

        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int processId);

        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length >= 1 && args[0] == "--selftest") return SelfTest.Run(args);
            if (args.Length >= 1 && args[0] == "--migrate") return MigrateCommand(args);
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
            string startNote = null;
            string path = Open(args, ref startNote);
            if (path == null) return 1;
            MessageBox.Show(ExperimentalWarning, Caption + " " + ReleaseVersion, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            Application.Run(new MainForm(path, startNote));
            return 0;
        }

        // --migrate <folder>: writes DS2United.ini there from the old files and prints what happened.
        static int MigrateCommand(string[] args)
        {
            AttachConsole(-1);
            if (args.Length < 2 || !Directory.Exists(args[1]))
            {
                Console.WriteLine("usage: DS2UnitedConfig.exe --migrate <folder with DS2Randomizer.ini, DS2Coop.ini, ...>");
                return 2;
            }
            string folder = Path.GetFullPath(args[1]);
            try
            {
                var before = Merger.LegacyFilesIn(folder).ToDictionary(n => n, n => File.ReadAllBytes(Path.Combine(folder, n)));
                var r = Merger.Migrate(folder);
                Console.WriteLine("wrote " + r.Path + " (" + r.Keys + " keys, " + r.Doc.Lines.Count + " lines, " + r.Doc.EncodingName + ")");
                foreach (var s in r.Sources) Console.WriteLine("  source  " + s);
                foreach (var s in r.Merged) Console.WriteLine("  merged  " + s);
                foreach (var s in r.Duplicates) Console.WriteLine("  dup     " + s);
                foreach (var s in r.Notes) Console.WriteLine("  note    " + s);
                bool untouched = before.All(p => IniFile.SameBytes(p.Value, File.ReadAllBytes(Merger.SourcePathAfter(folder, p.Key))));
                Console.WriteLine("  old files " + (untouched ? "unchanged" : "CHANGED"));
                return untouched ? 0 : 1;
            }
            catch (Exception ex)
            {
                Console.WriteLine("not migrated: " + ex.Message);
                return 1;
            }
        }

        // Layout check without showing a window: one PNG per page, nothing saved.
        static int Render(string ini, string outDir, int width, int height)
        {
            var clock = Stopwatch.StartNew();
            Directory.CreateDirectory(outDir);
            using (var form = new MainForm(Path.GetFullPath(ini), null))
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
                form.RenderSearch(outDir, "vent", "20_search.png");
                form.RenderSearch(outDir, "stasis mine", "21_search_expert.png");
                form.RenderEdited(outDir);
                form.RenderExpertEdited(outDir);
                form.DiscardChanges();
                form.Hide();
            }
            return 0;
        }

        static string Open(string[] args, ref string startNote)
        {
            if (args.Length >= 1 && File.Exists(args[0]))
            {
                string full = Path.GetFullPath(args[0]);
                if (!Merger.IsLegacyName(Path.GetFileName(full))) return full;
                return ForFolder(Path.GetDirectoryName(full), ref startNote);
            }
            if (args.Length >= 1 && Directory.Exists(args[0])) return ForFolder(Path.GetFullPath(args[0]), ref startNote);
            return Locate(ref startNote);
        }

        static string ForFolder(string folder, ref string startNote)
        {
            string merged = Merger.MergedIn(folder);
            if (merged != null) return merged;
            if (Merger.LegacyFilesIn(folder).Count > 0) return Offer(folder, ref startNote);
            return null;
        }

        static string Locate(ref string startNote)
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            var folders = new[] { Path.Combine(dir, "plugins"), dir };
            foreach (var folder in folders)
                if (Merger.MergedIn(folder) != null) return Merger.MergedIn(folder);
            foreach (var folder in folders)
                if (Merger.LegacyFilesIn(folder).Count > 0) return Offer(folder, ref startNote);
            MessageBox.Show("Could not find plugins\\" + IniName + " next to this program, nor an older DS2Coop.ini or DS2Randomizer.ini.\n\n" +
                "Put this program into the Dead Space 2 folder (next to deadspace2.exe), or pick the file now.",
                Caption, MessageBoxButtons.OK, MessageBoxIcon.Information);
            using (var dialog = new OpenFileDialog())
            {
                dialog.Title = "Find " + IniName;
                dialog.Filter = "Dead Space 2 United settings|" + IniName + ";" + Merger.OldName + ";" + string.Join(";", Merger.Legacy) + "|INI files (*.ini)|*.ini";
                dialog.FileName = IniName;
                if (dialog.ShowDialog() != DialogResult.OK) return null;
                if (!Merger.IsLegacyName(Path.GetFileName(dialog.FileName))) return dialog.FileName;
                return ForFolder(Path.GetDirectoryName(dialog.FileName), ref startNote);
            }
        }

        // Asks before DS2United.ini is made from the old files of a folder.
        static string Offer(string folder, ref string startNote)
        {
            var files = Merger.LegacyFilesIn(folder);
            string text = "Dead Space 2 United keeps every setting in one file, " + IniName + ". This folder does not have it yet:\n" + folder + "\n\n" +
                "Create it now from the settings you already have?\n\n" +
                string.Join("\n", files.Select(f => "      " + f).ToArray()) + "\n\n" +
                "Every section, value and comment is copied over. The old files stay where they are and are not changed, " +
                "but once " + IniName + " exists the plugins read only " + IniName + ", so the old files are no longer used.\n\n" +
                "Yes creates " + IniName + " and opens it. No closes this program and changes nothing.";
            if (MessageBox.Show(text, Caption, MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button1) != DialogResult.Yes)
                return null;
            try
            {
                var r = Merger.Migrate(folder);
                startNote = "Created " + IniName + " from " + string.Join(", ", files.ToArray()) + " (" + r.Keys + " keys). The old files are no longer read.";
                if (r.Duplicates.Count > 0)
                    MessageBox.Show("Some keys were in two files. The first value is kept and the second line is commented out:\n\n" +
                        string.Join("\n", r.Duplicates.ToArray()), Caption, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return r.Path;
            }
            catch (Exception ex)
            {
                MessageBox.Show(IniName + " was not created, nothing was changed:\n\n" + ex.Message, Caption, MessageBoxButtons.OK, MessageBoxIcon.Error);
                return null;
            }
        }

        public static Stream Resource(string name)
        {
            return typeof(Program).Assembly.GetManifestResourceStream("DS2CoopConfig." + name);
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
        static readonly string[] Logs = { "DS2Randomizer.log", "DS2United.log", "DS2Bots.log", "DS2Versus.log" };

        readonly string iniPath;
        readonly Model model;
        byte[] loadedBytes;
        readonly List<IView> views = new List<IView>();
        readonly Dictionary<string, Editor> editors = new Dictionary<string, Editor>();
        readonly Dictionary<string, Control> pages = new Dictionary<string, Control>();
        readonly Dictionary<string, Page> flows = new Dictionary<string, Page>();
        readonly Dictionary<string, Label> intros = new Dictionary<string, Label>();
        readonly HashSet<string> taken = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly ExpertGrid grid;
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
            foreach (var flow in flows.Values) flow.SuspendLayout();
            foreach (var view in rows) view.Root.SuspendLayout();
            try { action(); }
            finally
            {
                foreach (var view in rows) view.Root.ResumeLayout(false);
                foreach (var view in views) view.Root.PerformLayout();
                foreach (var flow in flows.Values) flow.ResumeLayout(true);
                ResumeLayout(true);
            }
        }

        public MainForm(string path, string startNote)
        {
            iniPath = path;
            Curated.IniFolder = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path));
            Text = Program.Caption;
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
            foreach (var o in model.Options) taken.Add(o.Id);

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
            nav = new NavList(Schema.Groups, Schema.GroupBreaks);
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
                int room = sidebar.Height - top - pad;
                // entries shrink a little before the hint gives way, and further when the window is very short
                int entry = Look.S(38);
                while (entry > Look.S(32) && nav.HeightFor(entry) + Look.S(8) + h > room) entry--;
                hint.Visible = nav.HeightFor(entry) + Look.S(8) + h <= room;
                if (!hint.Visible)
                {
                    entry = Look.S(38);
                    while (entry > Look.S(24) && nav.HeightFor(entry) > room) entry--;
                }
                nav.ItemHeight = entry;
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
            Button log = null;
            log = MakeButton("Open log", false, delegate { OpenLog(log); }, "The plugins' logs in the plugins folder");
            var about = MakeButton("About", false, delegate { ShowAbout(); }, "Version, credits and links  F1");
            status = new StatusLine { BackColor = Look.Panel, Font = Look.Base };
            bar.Controls.AddRange(new Control[] { folder, log, about, status, resetAll, reload, save });
            bar.Resize += delegate
            {
                int pad = Look.S(16), gap = Look.S(8), y = (bar.Height - buttonHeight) / 2;
                folder.Location = new Point(pad, y);
                log.Location = new Point(folder.Right + gap, y);
                about.Location = new Point(log.Right + gap, y);
                save.Location = new Point(bar.Width - pad - save.Width, y);
                reload.Location = new Point(save.Left - gap - reload.Width, y);
                resetAll.Location = new Point(reload.Left - gap - resetAll.Width, y);
                int x0 = about.Right + Look.S(24), x1 = resetAll.Left - Look.S(24);
                status.Visible = x1 - x0 > Look.S(80);
                status.Bounds = new Rectangle(x0, Look.S(6), Math.Max(0, x1 - x0), bar.Height - Look.S(12));
            };

            content = new Panel { Dock = DockStyle.Fill, BackColor = Look.Back };
            foreach (var group in Schema.Groups)
            {
                if (group == Schema.Expert) continue;
                var flow = new Page();
                var heading = new Label { AutoSize = true, Text = group, Font = Look.PageTitle, ForeColor = Look.Text, UseMnemonic = false, Margin = new Padding(0, 0, 0, Look.S(6)) };
                var intro = new Label { AutoSize = true, Text = Schema.GroupIntro[group], ForeColor = Look.Secondary, UseMnemonic = false, Margin = new Padding(0, 0, 0, Look.S(18)) };
                flow.Controls.Add(heading);
                flow.Controls.Add(intro);
                intros[group] = intro;
                content.Controls.Add(flow);
                pages[group] = flow;
                flows[group] = flow;
            }
            grid = new ExpertGrid(model, Schema.Expert, Schema.GroupIntro[Schema.Expert]) { Dock = DockStyle.Fill, Visible = false };
            grid.Edited = OnGridEdit;
            grid.ResetClicked = ResetOne;
            content.Controls.Add(grid);
            pages[Schema.Expert] = grid;
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
            if (startNote != null) status.Set(startNote, Tone.Good);
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
            else if (e.KeyCode == Keys.F1) { e.SuppressKeyPress = true; ShowAbout(); }
        }

        const string Author = "Richard Hafer";
        const string RepoUrl = "https://github.com/RichardHafer/DS2-United";
        const string DiscordUrl = "https://discord.com/invite/sWMRkYJ5JV";

        // Version, author, links, credits and the licence.
        void ShowAbout()
        {
            using (var dialog = new Form { Text = "About", StartPosition = FormStartPosition.CenterParent, BackColor = Look.Back,
                ForeColor = Look.Text, Font = Look.Base, ShowIcon = false, ShowInTaskbar = false, MinimizeBox = false,
                MaximizeBox = false, FormBorderStyle = FormBorderStyle.FixedDialog, ClientSize = new Size(Look.S(560), Look.S(400)) })
            {
                dialog.Controls.Add(new Panel { Dock = DockStyle.Top, Height = Look.S(3), BackColor = Look.Accent });
                int y = Look.S(20), left = Look.S(24), width = Look.S(512);
                Action<string, Font, Color> line = delegate(string text, Font font, Color color)
                {
                    var label = Look.MakeLabel(text, font, color);
                    label.AutoSize = false;
                    int h = TextRenderer.MeasureText(text, font, new Size(width, 0), TextFormatFlags.WordBreak).Height + Look.S(4);
                    label.SetBounds(left, y, width, h);
                    dialog.Controls.Add(label);
                    y += h;
                };
                Action<string, string> link = delegate(string text, string url)
                {
                    var label = new LinkLabel { Text = text, Font = Look.Base, BackColor = Color.Transparent, LinkColor = Look.Changed,
                        ActiveLinkColor = Look.Accent, VisitedLinkColor = Look.Changed, UseMnemonic = false, AutoSize = false };
                    label.SetBounds(left, y, width, Look.S(24));
                    label.LinkClicked += delegate { try { Process.Start(url); } catch (Exception) { } };
                    dialog.Controls.Add(label);
                    y += Look.S(26);
                };
                line("DEAD SPACE 2", Look.Small, Look.Accent);
                line("United", Look.PageTitle, Look.Text);
                line("Version " + Program.ReleaseVersion + ", highly experimental: story co-op, survival modes, bots, the enemy randomizer and more enemies.", Look.Base, Look.Secondary);
                y += Look.S(8);
                line("Created by " + Author, Look.Heading, Look.Text);
                y += Look.S(6);
                link("github.com/RichardHafer/DS2-United", RepoUrl);
                link("Dead Space Modding on Discord", DiscordUrl);
                y += Look.S(10);
                line("Thanks to", Look.Small, Look.Muted);
                line("The Dead Space Modding community for research, tests and feedback", Look.Base, Look.Secondary);
                line("safetyhook and Zydis for hooking", Look.Base, Look.Secondary);
                line("Ultimate ASI Loader by ThirteenAG for loading the plugin", Look.Base, Look.Secondary);
                y += Look.S(10);
                line("MIT licence. Copyright (c) 2026 " + Author + ".", Look.Small, Look.Muted);
                line("Not affiliated with EA or Visceral Games. Dead Space is a trademark of EA.", Look.Small, Look.Muted);
                var close = Look.MakeButton("Close", true, Look.S(32));
                close.DialogResult = DialogResult.OK;
                close.SetBounds(dialog.ClientSize.Width - Look.S(24) - Look.S(96), y + Look.S(14), Look.S(96), Look.S(32));
                dialog.Controls.Add(close);
                dialog.AcceptButton = close;
                dialog.CancelButton = close;
                dialog.ClientSize = new Size(dialog.ClientSize.Width, close.Bottom + Look.S(18));
                dialog.ShowDialog(this);
            }
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
            var headed = new HashSet<string>();
            foreach (var o in model.Options)
            {
                if (o.Generated) continue;
                IView view;
                if (o.Table != null)
                {
                    if (!tablesDone.Add(o.Table)) continue;
                    var info = Schema.Tables.First(t => t.Section == o.Table);
                    view = new TableView(info, create, ResetOne);
                }
                else
                {
                    if (o.Heading != null) views.Add(new HeadingView(o.Group, o.Heading, headed.Add(o.Group)));
                    view = new OptionView(o, create(o), ResetOne);
                    roots[o.Id] = view.Root;
                }
                views.Add(view);
            }
            // switches first, tables after them within each section, unless a table stays in place
            foreach (var view in views.Where(v => !(v is TableView) || ((TableView)v).InPlace)) flows[view.Group].Controls.Add(view.Root);
            foreach (var view in views.Where(v => v is TableView && !((TableView)v).InPlace)) flows[view.Group].Controls.Add(view.Root);
            // the Watchdog section ends with the advanced switches, after its tables
            var flow = flows[Schema.Watchdog];
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
            foreach (var page in flows.Values)
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
            foreach (var pair in flows)
            {
                pair.Value.Padding = new Padding(side, Look.S(24), side, Look.S(24));
                intros[pair.Key].MaximumSize = new Size(width, 0);
            }
            foreach (var view in views) view.SetWidth(width);
            grid.SetColumn(side, width);
            gridSide = side;
            gridWidth = width;
        }

        int gridSide, gridWidth;

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

        void OnGridEdit(Option o, string value, string error)
        {
            if (error != null) model.Invalid[o.Id] = error;
            else
            {
                model.Invalid.Remove(o.Id);
                model.Set(o, value);
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

        int MatchesIn(string group)
        {
            if (group == Schema.Expert) return grid.MatchCount;
            return views.Count(v => v.Group == group && !(v is HeadingView) && !hidden.Contains(v));
        }

        void RefreshAll()
        {
            foreach (var view in views) view.Refresh(model);
            grid.RefreshWarnings();
            int changes = model.ChangeCount;
            string text = changes == 0 ? "No unsaved changes" : changes + " unsaved change" + (changes == 1 ? "" : "s");
            var tone = changes == 0 ? Tone.Neutral : Tone.Pending;
            if (model.Invalid.Count > 0)
            {
                text += ". " + model.Invalid.Count + " invalid value" + (model.Invalid.Count == 1 ? "" : "s") + ": fix before saving.";
                tone = Tone.Error;
            }
            var changedGroups = new HashSet<string>(model.Options.Where(o => model.IsChanged(o) || model.Invalid.ContainsKey(o.Id)).Select(o => o.Group));
            for (int i = 0; i < Schema.Groups.Length; i++) nav.Changed[i] = changedGroups.Contains(Schema.Groups[i]);
            if (search.Text.Trim().Length > 0)
            {
                nav.Matches = Schema.Groups.Select(MatchesIn).ToArray();
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
                foreach (var o in model.Options)
                {
                    Editor editor;
                    if (editors.TryGetValue(o.Id, out editor)) editor.Show(model.Current[o.Id]);
                }
                refreshing = false;
                grid.ClearTyped();
                RefreshAll();
            });
        }

        void ApplyFilter()
        {
            string filter = search.Text.Trim().ToLowerInvariant();
            hidden.Clear();
            foreach (var view in views) if (!view.Matches(filter)) hidden.Add(view);
            // hiding the pages first keeps each toggle from repainting them
            foreach (var flow in flows.Values)
            {
                flow.Visible = false;
                flow.AutoScrollPosition = Point.Empty;
            }
            grid.Visible = false;
            grid.SetFilter(filter);
            Batch(() =>
            {
                foreach (var view in views) view.Root.Visible = !hidden.Contains(view);
                RefreshAll();
            });
            if (filter.Length > 0 && MatchesIn(Schema.Groups[nav.SelectedIndex]) == 0)
            {
                int first = Array.FindIndex(Schema.Groups, g => MatchesIn(g) > 0);
                if (first >= 0) nav.SelectedIndex = first;
            }
            ShowPage(nav.SelectedIndex);
        }

        void ResetOne(Option o)
        {
            string rec = model.Recommended[o.Id];
            if (rec == null) return;
            model.Invalid.Remove(o.Id);
            model.Set(o, rec);
            Editor editor;
            if (editors.TryGetValue(o.Id, out editor))
            {
                refreshing = true;
                editor.Show(model.Current[o.Id]);
                refreshing = false;
            }
            else grid.ClearTyped(o);
            ShowFollowers(o);
            RefreshAll();
        }

        void ResetAll()
        {
            int differ = model.ResetAllValues().Count(p => model.Current[p.Key] != p.Value);
            if (MessageBox.Show(this, "Set every option to the recommended release value? " + differ + " value" + (differ == 1 ? "" : "s") +
                " would change, the Expert page included.\n\n" +
                "The seeds are kept as they are, so a running playthrough is not affected, and keys your file does not have stay out of it. " +
                "Nothing is written until you click Save.",
                "Reset all", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            model.ResetAll();
            ShowAll();
        }

        // The Expert entries follow the file: keys added or removed outside change them.
        void LoadDocument(IniDocument doc)
        {
            var generated = ExpertKeys.Build(doc, taken);
            if (!ExpertKeys.SameSet(model.Generated, generated))
            {
                model.SetGenerated(generated);
                grid.SetOptions(generated);
            }
            model.LoadFrom(doc);
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
                LoadDocument(doc);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not read the file:\n" + ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                // nothing was ever loaded: fill the model so the window does not
                // throw on the first edit or on closing
                if (model.Current.Count == 0)
                {
                    LoadDocument(new IniDocument());
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
                if (Process.GetProcessesByName("deadspace2").Length > 0 || Process.GetProcessesByName("deadspace2_4gb").Length > 0)
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

        void OpenLog(Control anchor)
        {
            string dir = Path.GetDirectoryName(iniPath);
            var menu = new ContextMenuStrip();
            foreach (var name in Logs)
            {
                string log = Path.Combine(dir, name);
                bool exists = File.Exists(log);
                var item = menu.Items.Add(exists ? name : name + "  (not written yet)");
                item.Enabled = exists;
                item.Click += delegate
                {
                    try { Process.Start("notepad.exe", "\"" + log + "\""); }
                    catch (Exception ex) { MessageBox.Show(this, ex.Message, "Open log"); }
                };
            }
            menu.Closed += delegate { BeginInvoke((Action)menu.Dispose); };
            menu.Show(anchor, new Point(0, -menu.PreferredSize.Height));
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

        static string FileName(int index)
        {
            return (index + 1).ToString("D2", CultureInfo.InvariantCulture) + "_" + Schema.Groups[index].Replace(" & ", "_").Replace(" ", "_").Replace("-", "");
        }

        // Typed input as a player would leave it: one out of range, one risky.
        public void RenderEdited(string outDir)
        {
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.General);
            flows[Schema.General].AutoScrollPosition = Point.Empty;
            Application.DoEvents();
            editors["DS2Randomizer.Chance"].Control.Text = "150";
            editors["DS2Randomizer.Seed"].Control.Text = "70";
            editors["DS2Randomizer.CrossFamily"].Show("0");
            OnEdited(editors["DS2Randomizer.CrossFamily"]);
            Application.DoEvents();
            Shot(outDir, "22_edited.png");
            editors["DS2Randomizer.PlanDump"].Control.Text = "not a guid";
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.Diagnostics);
            Application.DoEvents();
            ScrollTo(Schema.Diagnostics, editors["DS2Randomizer.PlanDump"].Control);
            Application.DoEvents();
            Shot(outDir, "23_error.png");
            editors["VentFamilies.Lurker"].Show(model.Current["VentFamilies.Lurker"] == "1" ? "0" : "1");
            OnEdited(editors["VentFamilies.Lurker"]);
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.Enemies);
            Application.DoEvents();
            ScrollTo(Schema.Enemies, editors["VentFamilies.Lurker"].Control);
            Application.DoEvents();
            Shot(outDir, "24_table_changed.png");
            // a curated key with a warning and a key code note
            editors["Survival.Enabled"].Show("1");
            OnEdited(editors["Survival.Enabled"]);
            editors["DS2Randomizer.GlobalTemplates"].Show("0");
            OnEdited(editors["DS2Randomizer.GlobalTemplates"]);
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Curated.Survival);
            flows[Curated.Survival].AutoScrollPosition = Point.Empty;
            Application.DoEvents();
            Shot(outDir, "25_survival_warning.png");
            File.AppendAllText(Path.Combine(outDir, "timing.txt"), "invalid=" + string.Join(";", model.Invalid.Select(p => p.Key + ":" + p.Value).ToArray()) +
                " changes=" + model.ChangeCount + Environment.NewLine);
        }

        // Expert rows after a toggle, a valid number, an invalid number and an added key.
        public void RenderExpertEdited(string outDir)
        {
            var gen = model.Generated.ToList();
            var toggle = gen.FirstOrDefault(o => o.Kind == Kind.Bool && model.Current[o.Id] != Option.NotSet && o.Section == "Coop");
            var number = gen.FirstOrDefault(o => o.Kind == Kind.Int && model.Current[o.Id] != Option.NotSet && o.Section == "Coop");
            var bad = gen.Skip(gen.IndexOf(number) + 1).FirstOrDefault(o => o.Kind == Kind.Int && model.Current[o.Id] != Option.NotSet && o.Section == "Coop");
            nav.SelectedIndex = Array.IndexOf(Schema.Groups, Schema.Expert);
            Application.DoEvents();
            if (toggle != null) grid.Toggle(toggle);
            if (number != null) grid.Type(number, "12345");
            if (bad != null) grid.Type(bad, "12x");
            if (toggle != null) grid.ScrollTo(grid.TopOf(toggle) - Look.S(80));
            Application.DoEvents();
            Shot(outDir, "26_expert_edited.png");
            var absent = gen.FirstOrDefault(o => model.Current[o.Id] == Option.NotSet);
            if (absent != null)
            {
                grid.ScrollTo(grid.TopOf(absent) - Look.S(80));
                Application.DoEvents();
                Shot(outDir, "27_expert_not_in_file.png");
                OnGridEdit(absent, model.Recommended[absent.Id], null);
                Application.DoEvents();
                Shot(outDir, "28_expert_added.png");
            }
            File.AppendAllText(Path.Combine(outDir, "timing.txt"), "expert invalid=" + string.Join(";", model.Invalid.Select(p => p.Key + ":" + p.Value).ToArray()) +
                " changes=" + model.ChangeCount + " expert rows=" + gen.Count + " not in file=" + gen.Count(o => model.Current[o.Id] == Option.NotSet) + Environment.NewLine);
        }

        void ScrollTo(string group, Control c)
        {
            var flow = flows[group];
            int y = c.Top + c.Parent.Top - flow.AutoScrollPosition.Y;
            flow.AutoScrollPosition = new Point(0, Math.Max(0, y - flow.ClientSize.Height / 2));
        }

        public void RenderSearch(string outDir, string text, string file)
        {
            search.Text = text;
            ApplyFilter();
            Application.DoEvents();
            Shot(outDir, file);
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
            clock.Restart();
            lastWidth = -1;
            grid.SetColumn(0, 0);
            UpdateWidths();
            long relayout = clock.ElapsedMilliseconds;
            clock.Restart();
            grid.SetColumn(gridSide, gridWidth - 1);
            grid.SetColumn(gridSide, gridWidth);
            long gridLayout = clock.ElapsedMilliseconds / 2;
            File.WriteAllText(Path.Combine(outDir, "timing.txt"), "built_ms=" + built + " shown_ms=" + shown +
                " refresh_ms=" + refresh + " edit_ms=" + edit + " filter_ms=" + filter + " showall_ms=" + showAll + " widths_ms=" + relayout +
                " expert_layout_ms=" + gridLayout +
                " options=" + model.Options.Count + " tabs_ms=" + string.Join(",", pageTimes.ToArray()) + Environment.NewLine);
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
                string group = Schema.Groups[i];
                Page flow;
                bool isFlow = flows.TryGetValue(group, out flow);
                int view = isFlow ? flow.ClientSize.Height : grid.ClientSize.Height;
                int total = isFlow ? flow.DisplayRectangle.Height : grid.ContentHeight;
                for (int part = 0, y = 0; y < total && part < 8; part++, y += view - Look.S(40))
                {
                    if (isFlow) flow.AutoScrollPosition = new Point(0, y);
                    else grid.ScrollTo(y);
                    Application.DoEvents();
                    Shot(outDir, FileName(i) + "_" + part + ".png");
                }
                if (!isFlow)
                {
                    // the end of the Expert page as well
                    grid.ScrollTo(total);
                    Application.DoEvents();
                    Shot(outDir, FileName(i) + "_end.png");
                    grid.ScrollTo(0);
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
