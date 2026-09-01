// The window: one shell, three tabs, and the same things as a command line. The chrome is the XAML lifted from the
// PowerShell app (Window.xaml, embedded); everything with data in it is built in code, tab by tab, in the partial
// classes beside this one.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        // XamlReader gives back a fully formed Window; it is used as-is rather than having its content re-parented
        // into a subclass, which is what an earlier version did - and re-parenting a loaded window's content into a
        // second Window took the whole process down with an access violation before anything was shown.
        public readonly Window Win;

        readonly Options _opt;
        string _gameDir;
        List<Section> _sections;
        string _pickedId = "";
        // Scene ids the user has starred. Kept in the preferences file next to everything else the window
        // remembers, and written the moment a star is clicked rather than only when the window closes.
        readonly HashSet<string> _favourites = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, bool> _collapsed = new Dictionary<string, bool>();
        bool _hadSavedActs;     // false on a first run, when nothing has been left in any particular state yet
        System.Diagnostics.Process _runProc;

        // Named controls from the XAML, by the names the PowerShell app used.
        Border _headerBar, _pill, _lockBanner;
        TextBlock _titleText, _pillText, _pillNote, _status, _lockText, _pickTitle, _pickSub, _pickWarn,
                  _mashNote, _searchHint;
        Image _logoArt;
        System.Windows.Shapes.Rectangle _heroArt;
        RadioButton _navPlay, _navSettings, _navInstall;
        Grid _playView, _artBand;
        ScrollViewer _installView;
        Grid _settingsView;
        StackPanel _installHost, _settingsHost;
        WrapPanel _filters;              // the filter chips wrap onto a second line when the window is narrow
        ListBox _sceneList;
        TextBox _search, _holdSecs, _resW, _resH, _cmdPreview;
        CheckBox _optAdvance, _optMashX, _optEnd, _optHold, _optRes;
        Button _launchBtn, _stopBtn, _shortcutBtn, _copyBtn, _cmdCopyBtn, _recheckBtn, _reloadBtn, _saveBtn;
        Button _minBtn, _maxBtn, _closeBtn;
        ComboBox _altPick;
        FrameworkElement _altRow;
        System.Windows.Shapes.Rectangle _dropZone;

        static string Resource(string name)
        {
            Assembly asm = Assembly.GetExecutingAssembly();
            using (Stream s = asm.GetManifestResourceStream(name))
            using (var r = new StreamReader(s))
                return r.ReadToEnd();
        }

        public MainWindow(Options opt, string gameDir, string startTab)
        {
            _opt = opt;
            _gameDir = gameDir;

            Win = (Window)XamlReader.Parse(Resource("Window.xaml"));
            Win.WindowStartupLocation = WindowStartupLocation.CenterScreen;
            Win.AllowDrop = true;

            Widgets.LinkStyle = (Style)Win.FindResource("Link");
            Widgets.FlatStyle = (Style)Win.FindResource("Flat");
            Widgets.PrimaryStyle = (Style)Win.FindResource("Primary");
            Widgets.ChipStyle = (Style)Win.FindResource("Chip");

            DarkenMenus();
            Bind();
            TitleBar.Follow(Win);
            Art.SetWindowIcon(Win, _gameDir);
            Art.ApplyHeader(Win, _logoArt, _titleText, _heroArt, _headerBar, _artBand);
            TitleBar.Buttons(Win, _minBtn, _maxBtn, _closeBtn, _headerBar);
            SmoothScroll.Attach(Win);

            _sceneList.ItemTemplate = (DataTemplate)XamlReader.Parse(Resource("SceneRow.xaml"));

            WireNav();
            WireKeys();
            LoadFavourites();       // before the rows are built: each one is created knowing whether it is starred
            WirePlay();
            WireSettings();
            WireSetup();

            RestorePrefs();
            RestoreSelection();
            ShowTab(startTab);
            StartStatePolling();
            Win.Closing += (s, e) => SavePrefs();
        }

        // The menu WPF puts up when a text box is right-clicked is built by WPF itself and lives in a popup of its
        // own, outside this window, so it looks its style up in the application's resources and never sees the
        // window's - which is why it came up white over a dark window. The same styles, copied one level up.
        static readonly object[] MenuKeys = { typeof(ContextMenu), typeof(MenuItem), MenuItem.SeparatorStyleKey };

        void DarkenMenus()
        {
            Application app = Application.Current;
            if (app == null) return;      // --report and the other console routes never make one
            foreach (object key in MenuKeys)
                if (Win.Resources.Contains(key) && !app.Resources.Contains(key))
                    app.Resources[key] = Win.Resources[key];
        }

        void Bind()
        {
            Func<string, object> f = n => Win.FindName(n);
            _headerBar = (Border)f("HeaderBar");
            _artBand = (Grid)f("ArtBand");
            _minBtn = (Button)f("MinBtn");
            _maxBtn = (Button)f("MaxBtn");
            _closeBtn = (Button)f("CloseBtn");
            _titleText = (TextBlock)f("TitleText");
            _logoArt = (Image)f("LogoArt");
            _heroArt = (System.Windows.Shapes.Rectangle)f("HeroArt");
            _navPlay = (RadioButton)f("NavPlay");
            _navSettings = (RadioButton)f("NavSettings");
            _navInstall = (RadioButton)f("NavInstall");
            _pill = (Border)f("Pill");
            _pillText = (TextBlock)f("PillText");
            _pillNote = (TextBlock)f("PillNote");
            _playView = (Grid)f("PlayView");
            _settingsView = (Grid)f("SettingsView");
            _installView = (ScrollViewer)f("InstallView");
            _installHost = (StackPanel)f("InstallHost");
            _settingsHost = (StackPanel)f("SettingsHost");
            _copyBtn = (Button)f("CopyBtn");
            _recheckBtn = (Button)f("RecheckBtn");
            _reloadBtn = (Button)f("ReloadBtn");
            _saveBtn = (Button)f("SaveBtn");
            _search = (TextBox)f("Search");
            _searchHint = (TextBlock)f("SearchHint");
            _filters = (WrapPanel)f("Filters");
            _sceneList = (ListBox)f("SceneList");
            _pickTitle = (TextBlock)f("PickTitle");
            _pickSub = (TextBlock)f("PickSub");
            _pickWarn = (TextBlock)f("PickWarn");
            _altRow = (FrameworkElement)f("AltRow");
            _altPick = (ComboBox)f("AltPick");
            _optAdvance = (CheckBox)f("OptAdvance");
            _optMashX = (CheckBox)f("OptMashX");
            _mashNote = (TextBlock)f("MashNote");
            _optEnd = (CheckBox)f("OptEnd");
            _optHold = (CheckBox)f("OptHold");
            _holdSecs = (TextBox)f("HoldSecs");
            _optRes = (CheckBox)f("OptRes");
            _resW = (TextBox)f("ResW");
            _resH = (TextBox)f("ResH");
            _cmdPreview = (TextBox)f("CmdPreview");
            _cmdCopyBtn = (Button)f("CmdCopyBtn");
            _launchBtn = (Button)f("LaunchBtn");
            _stopBtn = (Button)f("StopBtn");
            _shortcutBtn = (Button)f("ShortcutBtn");
            _status = (TextBlock)f("Status");
            _lockBanner = (Border)f("LockBanner");
            _lockText = (TextBlock)f("LockText");
        }

        // ------------------------------------------------------------------------------------------- the tabs

        void WireNav()
        {
            _navPlay.Checked += (s, e) => ShowTab("play");
            _navSettings.Checked += (s, e) => ShowTab("settings");
            _navInstall.Checked += (s, e) => ShowTab("install");
        }

        // The keys the window says it takes, in one place because they cross the tabs: F5 is printed on the
        // Setup button, Ctrl+F reaches the search from anywhere, Escape empties it, and Enter launches what is
        // picked - except where a button has the focus, which Enter belongs to.
        void WireKeys()
        {
            Win.PreviewKeyDown += (s, e) =>
            {
                bool ctrl = (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control;
                if (e.Key == Key.F5)
                {
                    if (_installView.Visibility == Visibility.Visible) ShowSetup();
                    else if (_settingsView.Visibility == Visibility.Visible) BuildSettings();
                    e.Handled = true;
                }
                else if (ctrl && e.Key == Key.F)
                {
                    _navPlay.IsChecked = true;
                    _search.Focus();
                    _search.SelectAll();
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape && _search.Text.Length > 0 && _playView.Visibility == Visibility.Visible)
                {
                    _search.Clear();
                    e.Handled = true;
                }
                else if (e.Key == Key.Enter && _playView.Visibility == Visibility.Visible &&
                         !(Keyboard.FocusedElement is ButtonBase))
                {
                    Launch();
                    e.Handled = true;
                }
            };
        }

        void ShowTab(string tab)
        {
            if (_playView == null) return;
            _navPlay.IsChecked = tab == "play";
            _navSettings.IsChecked = tab == "settings";
            _navInstall.IsChecked = tab == "install";
            _playView.Visibility = tab == "play" ? Visibility.Visible : Visibility.Collapsed;
            _settingsView.Visibility = tab == "settings" ? Visibility.Visible : Visibility.Collapsed;
            _installView.Visibility = tab == "install" ? Visibility.Visible : Visibility.Collapsed;
            _copyBtn.Visibility = _recheckBtn.Visibility = tab == "install" ? Visibility.Visible : Visibility.Collapsed;
            _reloadBtn.Visibility = _saveBtn.Visibility = tab == "settings" ? Visibility.Visible : Visibility.Collapsed;
            _launchBtn.Visibility = _stopBtn.Visibility = _shortcutBtn.Visibility =
                tab == "play" ? Visibility.Visible : Visibility.Collapsed;

            if (tab == "install") ShowSetup();
            else if (tab == "settings") BuildSettings();
            RefreshState();
        }

        // The pill in the header, and everything else that depends on whether the game is up: what the window
        // cannot infer, it polls for. Driven by a timer rather than by tab switches alone - the game can start or
        // stop while the window sits there, and it did, which left the pill reading "idle" over a running game and
        // Close the game pressable with nothing to close.
        void RefreshState()
        {
            bool running = Checks.GameRunning();
            bool busy = _runProc != null && !_runProc.HasExited;
            string text, note, bg, br, fg;
            if (busy) { text = "driving"; note = "the launcher is attached"; bg = "#251E10"; br = "#7A6027"; fg = "#F2C14E"; }
            else if (running) { text = "running"; note = "mgs4.exe is up"; bg = "#152318"; br = "#2C6B45"; fg = "#5FD38D"; }
            else { text = "idle"; note = "nothing is running"; bg = "#161B2A"; br = "#33436E"; fg = "#7C9CFF"; }
            _pillText.Text = text;
            _pillNote.Text = note;
            _pill.Background = Widgets.Brush(bg);
            _pill.BorderBrush = Widgets.Brush(br);
            _pillText.Foreground = Widgets.Brush(fg);

            _stopBtn.IsEnabled = running || busy;
            UpdateSaveButton();     // enabled only while there is an edit to write, and the game is not running

            if (_settingsView.Visibility == Visibility.Visible)
            {
                if (string.IsNullOrEmpty(_gameDir))
                {
                    _lockText.Text = "No MGS4 install found, so there is no mgs4_dlss.ini to read or write. The Setup tab says what was looked for.";
                    _lockBanner.Visibility = Visibility.Visible;
                }
                else if (running)
                {
                    _lockText.Text = "The game is running. It rewrites mgs4_dlss.ini through the Windows profile API, whose cache would undo anything written from here - close the game to save. Most of these keys are read again every second by the add-on, and its own overlay (ReShade, Add-ons tab) can change them live.";
                    _lockBanner.Visibility = Visibility.Visible;
                }
                else _lockBanner.Visibility = Visibility.Collapsed;
            }
        }

        void StartStatePolling()
        {
            var timer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(1.5) };
            timer.Tick += (s, e) => RefreshState();
            timer.Start();
            Win.Closed += (s, e) => timer.Stop();
        }

        public void Say(string text)
        {
            _status.Text = text;
        }

        // ------------------------------------------------------------------------------------------- prefs

        void LoadFavourites()
        {
            Dictionary<string, object> p = Prefs.Read();
            object list;
            if (p == null || !p.TryGetValue("Favourites", out list) || !(list is object[])) return;
            foreach (object o in (object[])list)
                if (o != null) _favourites.Add(o.ToString());
        }

        void RestorePrefs()
        {
            _optAdvance.IsChecked = true;
            Dictionary<string, object> p = Prefs.Read();
            if (p == null) return;
            Func<string, bool> flag = k =>
            {
                object v;
                return p.TryGetValue(k, out v) && v != null && Convert.ToBoolean(v);
            };
            Func<string, string> str = k =>
            {
                object v;
                return p.TryGetValue(k, out v) && v != null ? v.ToString() : null;
            };
            if (p.ContainsKey("Advance")) _optAdvance.IsChecked = flag("Advance");
            _optMashX.IsChecked = flag("MashX");
            _optEnd.IsChecked = flag("EndOnGameplay");
            _optHold.IsChecked = flag("Hold");
            _optRes.IsChecked = flag("Res");
            if (str("HoldSecs") != null) _holdSecs.Text = str("HoldSecs");
            if (str("ResW") != null) _resW.Text = str("ResW");
            if (str("ResH") != null) _resH.Text = str("ResH");
            string stage = str("Stage");
            if (!string.IsNullOrEmpty(_opt.Stage)) stage = _opt.Stage;
            if (!string.IsNullOrEmpty(stage)) _pickedId = stage;
            object filters;
            if (p.TryGetValue("Filters", out filters) && filters is object[])
            {
                var want = new HashSet<string>();
                foreach (object o in (object[])filters) want.Add(o.ToString());
                foreach (object child in _filters.Children)
                {
                    var chip = child as System.Windows.Controls.Primitives.ToggleButton;
                    if (chip != null && chip.Tag != null) chip.IsChecked = want.Contains(chip.Tag.ToString());
                }
            }

            // Which acts were left closed. Stored as the closed ones rather than the open ones, so an act added to
            // the catalogue later starts closed like every other act does on a first run.
            object acts;
            if (p.TryGetValue("Collapsed", out acts) && acts is object[])
            {
                _hadSavedActs = true;
                var shut = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (object o in (object[])acts) if (o != null) shut.Add(o.ToString());
                foreach (string key in Catalogue.ActOrder) _collapsed[key] = shut.Contains(key);
            }
        }

        void SavePrefs()
        {
            var filters = new List<string>();
            foreach (object child in _filters.Children)
            {
                var chip = child as System.Windows.Controls.Primitives.ToggleButton;
                if (chip != null && chip.IsChecked == true && chip.Tag != null) filters.Add(chip.Tag.ToString());
            }
            Prefs.Save(new Dictionary<string, object>
            {
                { "Stage", _pickedId },
                { "Filters", filters },
                { "Favourites", new List<string>(_favourites) },
                { "Collapsed", _collapsed.Where(kv => kv.Value).Select(kv => kv.Key).ToList() },
                { "Advance", _optAdvance.IsChecked == true },
                { "MashX", _optMashX.IsChecked == true },
                { "EndOnGameplay", _optEnd.IsChecked == true },
                { "Hold", _optHold.IsChecked == true },
                { "HoldSecs", _holdSecs.Text },
                { "Res", _optRes.IsChecked == true },
                { "ResW", _resW.Text },
                { "ResH", _resH.Text },
                { "Seen", true },
            });
        }
    }
}
