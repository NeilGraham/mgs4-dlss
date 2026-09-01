// The window: one shell, three tabs, and the same things as a command line. The chrome is the XAML lifted from the
// PowerShell app (Window.xaml, embedded); everything with data in it is built in code, tab by tab, in the partial
// classes beside this one.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
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
        readonly Dictionary<string, bool> _collapsed = new Dictionary<string, bool>();
        System.Diagnostics.Process _runProc;

        // Named controls from the XAML, by the names the PowerShell app used.
        Border _headerBar, _pill, _lockBanner;
        TextBlock _titleText, _pillText, _pillNote, _status, _lockText, _pickTitle, _pickSub, _pickWarn,
                  _cmdPreview, _mashNote, _searchHint;
        Image _logoArt;
        System.Windows.Shapes.Rectangle _heroArt;
        RadioButton _navPlay, _navSettings, _navInstall;
        Grid _playView;
        ScrollViewer _installView;
        Grid _settingsView;
        StackPanel _installHost, _settingsHost;
        WrapPanel _filters;              // the filter chips wrap onto a second line when the window is narrow
        ListBox _sceneList;
        TextBox _search, _holdSecs, _resW, _resH;
        CheckBox _optAdvance, _optMashX, _optEnd, _optHold, _optRes;
        Button _launchBtn, _stopBtn, _shortcutBtn, _copyBtn, _recheckBtn, _reloadBtn, _saveBtn;
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

            Bind();
            Art.SetWindowIcon(Win, _gameDir);
            Art.ApplyHeader(Win, _logoArt, _titleText, _heroArt, _headerBar);
            SmoothScroll.Attach(Win);

            _sceneList.ItemTemplate = (DataTemplate)XamlReader.Parse(Resource("SceneRow.xaml"));

            WireNav();
            WirePlay();
            WireSettings();
            WireSetup();

            RestorePrefs();
            RestoreSelection();
            ShowTab(startTab);
            Win.Closing += (s, e) => SavePrefs();
        }

        void Bind()
        {
            Func<string, object> f = n => Win.FindName(n);
            _headerBar = (Border)f("HeaderBar");
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
            _cmdPreview = (TextBlock)f("CmdPreview");
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
            UpdatePill();
        }

        // The pill in the header: what the game is doing, which is the one thing the window cannot infer.
        void UpdatePill()
        {
            bool running = Checks.GameRunning();
            _pillText.Text = running ? "running" : "idle";
            _pillNote.Text = running ? "mgs4.exe is up" : "nothing is running";
            StatusStyle st = Widgets.Status[running ? "ok" : "info"];
            _pill.Background = Widgets.Brush(st.Bg);
            _pill.BorderBrush = Widgets.Brush(st.Br);
            _pillText.Foreground = Widgets.Brush(st.Fg);
        }

        public void Say(string text)
        {
            _status.Text = text;
        }

        // ------------------------------------------------------------------------------------------- prefs

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
