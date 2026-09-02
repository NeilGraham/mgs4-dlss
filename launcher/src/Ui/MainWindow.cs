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
        List<Section> _sections;        // the last install check, kept so returning to Setup paints at once
        string _setupDir;               // the folder it ran against; a different one makes it worthless
        DateTime _setupAt;
        bool _setupBusy;
        bool _setupCacheTried;   // the disk is read for a check once a run, not once a visit

        // Is mgs4.exe up? Asking costs eight milliseconds - it walks the whole process table - and the answer is
        // wanted constantly: every poll, every rebuild of a card, and every keystroke in a settings box, because
        // Save is only offered when the game is closed. So it is asked off the window's thread and read from here.
        // Anything about to write a file asks Checks.GameRunning itself: a cache is not a lock, and there the
        // file is what matters, not a label.
        bool _gameUp;
        bool _pollBusy;
        string _pickedId = "";
        // Scene ids the user has starred. Kept in the preferences file next to everything else the window
        // remembers, and written the moment a star is clicked rather than only when the window closes.
        readonly HashSet<string> _favorites = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, bool> _collapsed = new Dictionary<string, bool>();
        // Names and descriptions typed over the catalog's own. The catalog reads the same file for itself when
        // it builds the list; this copy is what the window edits and writes back.
        readonly Dictionary<string, Prefs.SceneEdit> _sceneEdits = Prefs.SceneEdits();
        bool _hadSavedActs;     // false on a first run, when nothing has been left in any particular state yet
        System.Diagnostics.Process _runProc;

        // Named controls from the XAML, by the names the PowerShell app used.
        Border _headerBar, _lockBanner;
        TextBlock _titleText, _status, _lockText, _pickTitle, _pickSub, _pickWarn,
                  _mashNote, _searchHint;
        Image _logoArt;
        Panel _navTabs;
        System.Windows.Shapes.Rectangle _heroArt;
        RadioButton _navPlay, _navSettings, _navInstall;
        Grid _playView, _artBand;
        ScrollViewer _installView;
        Grid _settingsView;
        StackPanel _installHost, _settingsHost;
        WrapPanel _filters;              // the filter chips wrap onto a second line when the window is narrow
        ListBox _sceneList;
        TextBox _search, _holdSecs, _cmdPreview;
        ComboBox _resPick;
        CheckBox _optAdvance, _optMashX, _optEnd, _optHold, _optRes;
        Button _launchBtn, _stopBtn, _shortcutBtn, _copyBtn, _cmdCopyBtn, _recheckBtn, _reloadBtn, _saveBtn;
        Button _minBtn, _maxBtn, _closeBtn;
        ComboBox _altPick;
        FrameworkElement _altRow;
        TextBox _editName, _editDesc;
        Button _editBtn, _editSaveBtn, _editCancelBtn, _editResetBtn;
        StackPanel _editPanel;
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
            Art.SetTaskbarIdentity();
            Art.ApplyHeader(Win, _logoArt, _titleText, _navTabs, _heroArt, _headerBar, _artBand);
            TitleBar.Buttons(Win, _minBtn, _maxBtn, _closeBtn, _headerBar);
            SmoothScroll.Attach(Win);

            _sceneList.ItemTemplate = (DataTemplate)XamlReader.Parse(Resource("SceneRow.xaml"));

            WireNav();
            WireKeys();
            LoadFavorites();       // before the rows are built: each one is created knowing whether it is starred
            WirePlay();
            WireSettings();
            WireSetup();

            RestorePrefs();
            RestoreSelection();
            ShowTab(startTab);
            StartStatePolling();

            // Two things that used to be paid for on the way up, moved to the gap after it. ApplicationIdle is
            // below input, so a click that arrives first is still served first; these fill the pause instead.
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                                       new Action(PrimeSetupIcon));
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                                       new Action(WarmSettings));
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
            _navTabs = (Panel)f("NavTabs");
            _heroArt = (System.Windows.Shapes.Rectangle)f("HeroArt");
            _navPlay = (RadioButton)f("NavPlay");
            _navSettings = (RadioButton)f("NavSettings");
            _navInstall = (RadioButton)f("NavInstall");
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
            _editBtn = (Button)f("EditBtn");
            _editPanel = (StackPanel)f("EditPanel");
            _editName = (TextBox)f("EditName");
            _editDesc = (TextBox)f("EditDesc");
            _editSaveBtn = (Button)f("EditSaveBtn");
            _editCancelBtn = (Button)f("EditCancelBtn");
            _editResetBtn = (Button)f("EditResetBtn");
            _optAdvance = (CheckBox)f("OptAdvance");
            _optMashX = (CheckBox)f("OptMashX");
            _mashNote = (TextBlock)f("MashNote");
            _optEnd = (CheckBox)f("OptEnd");
            _optHold = (CheckBox)f("OptHold");
            _holdSecs = (TextBox)f("HoldSecs");
            _optRes = (CheckBox)f("OptRes");
            _resPick = (ComboBox)f("ResPick");
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
        // picked - except where a button has the focus, or the rename boxes are open, which both keys belong to.
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
                else if (e.Key == Key.Escape && _editPanel.Visibility == Visibility.Visible &&
                         _playView.Visibility == Visibility.Visible)
                {
                    EndEdit();
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape && _search.Text.Length > 0 && _playView.Visibility == Visibility.Visible)
                {
                    _search.Clear();
                    e.Handled = true;
                }
                else if (e.Key == Key.Enter && _playView.Visibility == Visibility.Visible &&
                         _editPanel.Visibility != Visibility.Visible &&
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
            else if (tab == "settings") ShowSettings();
            RefreshState();
        }

        // Everything that depends on whether the game is up: what the window cannot infer, it polls for. Driven by
        // a timer rather than by tab switches alone - the game can start or stop while the window sits there, and
        // it did, which left Close the game pressable with nothing to close. The header used to carry a
        // idle/running/driving badge as well; it said what Close the game and the Settings banner already say.
        void RefreshState()
        {
            bool running = _gameUp;
            bool busy = _runProc != null && !_runProc.HasExited;

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
            timer.Tick += (s, e) => PollState();
            timer.Start();
            Win.Closed += (s, e) => timer.Stop();
            PollState();        // seed it now rather than showing an idle window for a second and a half
        }

        // The process table off the window's thread; only the answer comes back to it. The window opens believing
        // the game is not running, which is right on all but the run where it is, and wrong there for as long as
        // one process listing takes.
        void PollState()
        {
            if (_pollBusy) return;
            _pollBusy = true;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                bool up;
                try { up = Checks.GameRunning(); } catch { up = false; }
                Win.Dispatcher.BeginInvoke(new Action(delegate
                {
                    _pollBusy = false;
                    _gameUp = up;
                    RefreshState();
                }));
            });
        }

        public void Say(string text)
        {
            _status.Text = text;
        }

        // ------------------------------------------------------------------------------------------- prefs

        // The key was "Favourites" before the spelling was settled, and a saved file still holds it, so it is read
        // under the old name when the new one is absent. The next save writes the new one, and the old key goes.
        void LoadFavorites()
        {
            Dictionary<string, object> p = Prefs.Read();
            object list;
            if (p == null) return;
            if (!p.TryGetValue("Favorites", out list) && !p.TryGetValue("Favourites", out list)) return;
            if (!(list is object[])) return;
            foreach (object o in (object[])list)
                if (o != null) _favorites.Add(o.ToString());
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
            // the saved resolution ("ResPick"; older files carried ResW / ResH as two numbers) - a size the list does not have keeps the default
            string savedRes = str("ResPick") ?? (str("ResW") != null && str("ResH") != null ? str("ResW") + "x" + str("ResH") : null);
            if (savedRes != null) SelectRes(savedRes);
            string stage = str("Stage");
            if (!string.IsNullOrEmpty(_opt.Stage)) stage = _opt.Stage;
            if (!string.IsNullOrEmpty(stage)) _pickedId = stage;
            object filters;
            if (p.TryGetValue("Filters", out filters) && filters is object[])
            {
                var want = new HashSet<string>();
                foreach (object o in (object[])filters) want.Add(MigrateFilter(o.ToString()));
                foreach (object child in _filters.Children)
                {
                    var chip = child as System.Windows.Controls.Primitives.ToggleButton;
                    if (chip != null && chip.Tag != null) chip.IsChecked = want.Contains(chip.Tag.ToString());
                }
            }

            // Which acts were left closed. Stored as the closed ones rather than the open ones, so an act added to
            // the catalog later starts closed like every other act does on a first run.
            object acts;
            if (p.TryGetValue("Collapsed", out acts) && acts is object[])
            {
                _hadSavedActs = true;
                var shut = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (object o in (object[])acts) if (o != null) shut.Add(o.ToString());
                foreach (string key in Catalog.ActOrder) _collapsed[key] = shut.Contains(key);
            }
        }

        // The chips used to be named for the lists they showed rather than for the badge a scene wears. A saved
        // file still names those, so it is read as the chips that replaced them; "Named scenes" has no successor -
        // every scene can be named now - and simply goes unticked. Favourites is the same chip under its old
        // spelling: the tag is what gets saved, so a file written before the sweep still names it that way.
        static string MigrateFilter(string name)
        {
            switch (name)
            {
                case "Cutscenes": return "Cutscene";
                case "Mission briefings": return "Briefing";
                case "Stage entries": return "Stage";
                case "Known broken": return "Broken";
                case "Favourites": return FavoritesCat;
                default: return name;
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
                { "Favorites", new List<string>(_favorites) },
                { "SceneEdits", SceneEditsForSaving() },
                { "Collapsed", _collapsed.Where(kv => kv.Value).Select(kv => kv.Key).ToList() },
                { "Advance", _optAdvance.IsChecked == true },
                { "MashX", _optMashX.IsChecked == true },
                { "EndOnGameplay", _optEnd.IsChecked == true },
                { "Hold", _optHold.IsChecked == true },
                { "HoldSecs", _holdSecs.Text },
                { "Res", _optRes.IsChecked == true },
                { "ResPick", PickedRes() },
                { "Seen", true },
            });
        }
    
        // Only the halves actually typed are written, so a scene given a description but not a name comes back
        // wearing the catalog's name and the typed description.
        Dictionary<string, object> SceneEditsForSaving()
        {
            var outp = new Dictionary<string, object>();
            foreach (var kv in _sceneEdits)
            {
                var d = new Dictionary<string, object>();
                if (kv.Value.Name != null) d["name"] = kv.Value.Name;
                if (kv.Value.Description != null) d["description"] = kv.Value.Description;
                if (d.Count > 0) outp[kv.Key] = d;
            }
            return outp;
        }

        // The Setup tab's icon is its verdict: a checklist until the checks have run, then the tick, the warning
        // or the cross the Setup card itself shows, in the same color. Set as a local value, so it wins over the
        // style's checked-tab accent - what the install is doing matters more than which tab is open.
        void SetSetupIcon(Verdict v)
        {
            string glyph = "", color = "#97979F", tip = "Setup";
            if (v != null)
            {
                tip = "Setup - " + v.Text.ToLowerInvariant() + (string.IsNullOrEmpty(v.Note) ? "" : ", " + v.Note);
                if (v.Kind == "ok") { glyph = ""; color = "#62C98A"; }
                else if (v.Kind == "warn") { glyph = ""; color = "#F2C14E"; }
                else if (v.Kind == "bad") { glyph = ""; color = "#FF6B66"; }
            }
            _navInstall.Content = glyph;
            _navInstall.Foreground = Widgets.Brush(color);
            _navInstall.ToolTip = tip;
        }

        // The verdict is worth having before the Setup tab is ever opened, because it is what the tab's own icon
        // says. Same trick as ShowSetup: let the window paint first, then spend the third of a second on files.
        // The tab badge, for a window that opened on Play. This used to run the whole install check on the way up
        // to colour one icon: thirty-five milliseconds of file reads and a driver lookup, on the window's thread.
        // Now it takes the check the last run left on disk, and when there is none it starts the same background
        // refresh the Setup tab uses and lets that set the icon when it lands.
        void PrimeSetupIcon()
        {
            if (_installView.Visibility == Visibility.Visible) return;   // ShowSetup is about to do it properly
            if (string.IsNullOrEmpty(_gameDir))
            {
                SetSetupIcon(new Verdict { Text = "No game folder", Kind = "bad", Note = "nothing to check against yet" });
                return;
            }
            if (_sections == null && !_setupCacheTried)
            {
                _setupCacheTried = true;
                DateTime taken;
                List<Section> kept = SetupCache.Read(_gameDir, out taken);
                if (kept != null) { _sections = kept; _setupDir = _gameDir; _setupAt = taken; }
            }
            if (_sections != null)
            {
                try { SetSetupIcon(Checks.GetVerdict(_sections)); } catch { SetSetupIcon(null); }
            }
            StartSetupRefresh();
        }

        // The Settings form built before it is asked for. It is forty-three rows of controls - the reading behind
        // them is four milliseconds, the building is the rest - so the first visit used to stall exactly as every
        // visit did before the form was kept. Built here into a collapsed panel, it is already there.
        void WarmSettings()
        {
            if (_settingsView.Visibility == Visibility.Visible) return;  // ShowSettings has already done it
            if (_settingReaders.Count > 0) return;
            try { BuildSettings(); } catch { }
        }

        // The Play tab's resolution list: the game's 16:9 sizes as "WxH" tags. The port takes --res_width / --res_height
        // only from this set, so a free width and height box was never a real choice.
        string PickedRes()
        {
            ComboBoxItem it = _resPick.SelectedItem as ComboBoxItem;
            return it != null && it.Tag != null ? it.Tag.ToString() : "3840x2160";
        }
        void SelectRes(string wxh)
        {
            for (int i = 0; i < _resPick.Items.Count; i++)
            {
                ComboBoxItem it = _resPick.Items[i] as ComboBoxItem;
                if (it != null && it.Tag != null && it.Tag.ToString() == wxh) { _resPick.SelectedIndex = i; return; }
            }
        }
}
}
