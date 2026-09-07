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
        // The wider answer from the same poll: which of the game's programs has the screen - mgs4.exe, the bundled
        // MGS1, or the Master Collection front-end - or null for none. The music and the status pill read this;
        // Save reads _gameUp, because only mgs4.exe owns the ini files.
        string _gameActivity;
        // When Launch was last pressed. Between then and the game being seen the window is "launching": the
        // music stays down and the pill says so, rather than the music coming back for the seconds Steam and the
        // boot take and being cut off again when the process appears.
        DateTime _launchedAt = DateTime.MinValue;
        const double LaunchGraceSeconds = 90;
        bool _padWired;
        // The primary way in, until the preferences file says which scene was picked last.
        string _pickedId = "@main";
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
                  _mashNote, _searchHint, _startAdvanceNote, _veilTitle, _veilBody, _gameStateText;
        Border _gameStateTag, _navPrevHint, _navNextHint;
        System.Windows.Shapes.Ellipse _gameStateDot;
        Image _logoArt;
        System.Windows.Shapes.Rectangle _pickShot;
        Border _pickShotBox;
        Panel _navTabs;
        System.Windows.Shapes.Rectangle _heroArt;
        RadioButton _navPlay, _navSettings, _navInstall;
        Grid _playView, _artBand;
        Grid _installView, _installRailSlot, _settingsRailSlot;
        ScrollViewer _installScroll, _settingsScroll;
        Grid _settingsView;
        // The Play tab's other half: the three ways to start the game, and the panel their cards live in.
        Grid _startView;
        UniformGrid _startHost;
        ScrollViewer _startScroll;
        Border _veil;
        StackPanel _installHost, _settingsHost;
        WrapPanel _filters;              // the filter chips wrap onto a second line when the window is narrow
        ListBox _sceneList;
        TextBox _search, _holdSecs, _cmdPreview;
        ComboBox _resPick;
        CheckBox _optAdvance, _optMashX, _optEnd, _optHold, _optRes, _startAdvance;
        Button _launchBtn, _stopBtn, _shortcutBtn, _copyBtn, _cmdCopyBtn, _recheckBtn, _reloadBtn, _saveBtn;
        Button _viewSwitchBtn, _veilOkBtn, _veilCancelBtn, _startLaunchBtn, _startStopBtn, _startShortcutBtn;
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
            ScrollThumb.Attach();
            Widgets.FoldChanged = SavePrefs;

            _sceneList.ItemTemplate = (DataTemplate)XamlReader.Parse(Resource("SceneRow.xaml"));

            WireNav();
            WireKeys();
            LoadFavorites();       // before the rows are built: each one is created knowing whether it is starred
            WirePlay();
            WireStart();
            WireSettings();
            WireSetup();
            WireMusic();

            RestorePrefs();
            // Which half of the Play tab is on is decided before the selection is restored: in the simple view a
            // scene picked last time is not one of the three, and is replaced rather than launched by mistake.
            ApplyPlayView(false);
            RestoreSelection();
            // With the Setup tab hidden there is no opening on it, whatever a first run or --setup would do.
            ApplySetupTab();
            if (startTab == "install" && SetupHidden()) startTab = "play";
            ShowTab(startTab);
            StartStatePolling();
            WirePad();

            // Two things that used to be paid for on the way up, moved to the gap after it. ApplicationIdle is
            // below input, so a click that arrives first is still served first; these fill the pause instead.
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                                       new Action(PrimeSetupIcon));
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                                       new Action(WarmSettings));
            // The music comes up in that same gap: it runs a decoder, and the window should be on screen before
            // anything that costs a third of a second is started for it.
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                                       new Action(ApplyMusic));
            Win.Closing += (s, e) => { StopMusic(false); SavePrefs(); };
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
            _startView = (Grid)f("StartView");
            _startHost = (UniformGrid)f("StartCards");
            _startAdvance = (CheckBox)f("StartAdvance");
            _startAdvanceNote = (TextBlock)f("StartAdvanceNote");
            _startLaunchBtn = (Button)f("StartLaunchBtn");
            _startStopBtn = (Button)f("StartStopBtn");
            _startShortcutBtn = (Button)f("StartShortcutBtn");
            _viewSwitchBtn = (Button)f("ViewSwitchBtn");
            _veil = (Border)f("Veil");
            _veilTitle = (TextBlock)f("VeilTitle");
            _veilBody = (TextBlock)f("VeilBody");
            _veilOkBtn = (Button)f("VeilOkBtn");
            _veilCancelBtn = (Button)f("VeilCancelBtn");
            _settingsView = (Grid)f("SettingsView");
            _settingsRailSlot = (Grid)f("SettingsRail");
            _settingsScroll = (ScrollViewer)f("SettingsScroll");
            _installView = (Grid)f("InstallView");
            _installRailSlot = (Grid)f("InstallRail");
            _installScroll = (ScrollViewer)f("InstallScroll");
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
            _pickShot = (System.Windows.Shapes.Rectangle)f("PickShot");
            _pickShotBox = (Border)f("PickShotBox");
            // The picture box is as wide as the panel and 16:9, so the whole frame shows; the crop belongs to
            // the row banner. Both it and the list's card are clipped to their rounded corners - a Border's
            // CornerRadius shapes its own background and stroke only, and what it holds overflows the curve.
            // ...except on a short window, where a 16:9 frame the panel's full width left no room under it for
            // the options and pushed Launch off the bottom of the card. Past a third of the tab's height the box
            // stops growing and the frame is cropped to fit it instead - evenly, top and bottom.
            //
            // Not while the tab is hidden, though. Switching to the simple view collapses this whole grid, and a
            // collapsed grid reads as 0 tall - the box was being fitted against that, clamped to its floor, and
            // came back with the scene list as a strip. The grid's own SizeChanged says nothing about any of
            // this (it fires neither on the collapse nor on the return), so the return is caught below through
            // IsVisibleChanged and the fit run again once the tab has been laid out.
            SizeChangedEventHandler fitShot = (o, e) =>
            {
                double w = _pickShotBox.ActualWidth;
                if (w <= 0 || !_playView.IsVisible || _playView.ActualHeight <= 0) return;
                double want = w * 9 / 16, most = Math.Max(90, _playView.ActualHeight * 0.34);
                bool cropped = want > most;
                if (cropped) want = most;
                if (double.IsNaN(_pickShotBox.Height) || Math.Abs(_pickShotBox.Height - want) > 0.5) _pickShotBox.Height = want;
                var brush = _pickShot.Fill as ImageBrush;
                if (brush != null) brush.Stretch = cropped ? Stretch.UniformToFill : Stretch.Uniform;
                RoundClip(_pickShotBox, 10, true);
            };
            _pickShotBox.SizeChanged += fitShot;
            _playView.SizeChanged += fitShot;
            _playView.IsVisibleChanged += (o, e) =>
            {
                if (!_playView.IsVisible) return;
                Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded,
                                           new Action(() => fitShot(null, null)));
            };
            var sceneCardBody = (FrameworkElement)f("SceneCardBody");
            sceneCardBody.SizeChanged += (o, e) => RoundClip(sceneCardBody, 10, false);
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
            _gameStateTag = (Border)f("GameStateTag");
            _gameStateDot = (System.Windows.Shapes.Ellipse)f("GameStateDot");
            _gameStateText = (TextBlock)f("GameStateText");
            _navPrevHint = (Border)f("NavPrevHint");
            _navNextHint = (Border)f("NavNextHint");
            _pickScroll = (ScrollViewer)f("PickScroll");
            _startScroll = (ScrollViewer)f("StartScroll");
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
                // The playlist editor is up: it takes its own keys, and Enter must not reach Launch.
                if (PlaylistOpen)
                {
                    if (PlaylistKey(e.Key)) e.Handled = true;
                    return;
                }
                // A question is up: it owns Enter and Escape, and nothing behind it hears anything.
                if (_veil.Visibility == Visibility.Visible)
                {
                    if (e.Key == Key.Escape) { CloseVeil(false); e.Handled = true; }
                    else if (e.Key == Key.Enter) { CloseVeil(true); e.Handled = true; }
                    return;
                }
                if (e.Key == Key.F5)
                {
                    if (_installView.Visibility == Visibility.Visible) ShowSetup();
                    else if (_settingsView.Visibility == Visibility.Visible) BuildSettings();
                    e.Handled = true;
                }
                // Ctrl+F is the search on whichever tab is up: the rail's box on Settings and Setup, the scene
                // list's on Play. Escape empties the one that is up.
                else if (ctrl && e.Key == Key.F && TabSearch() != null)
                {
                    TextBox box = TabSearch();
                    if (box == _search) _navPlay.IsChecked = true;
                    box.Focus();
                    box.SelectAll();
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape && _editPanel.Visibility == Visibility.Visible &&
                         _playView.Visibility == Visibility.Visible)
                {
                    EndEdit();
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape && TabSearch() != null && TabSearch().Text.Length > 0)
                {
                    TabSearch().Clear();
                    e.Handled = true;
                }
                else if (e.Key == Key.Enter &&
                         (_playView.Visibility == Visibility.Visible || _startView.Visibility == Visibility.Visible) &&
                         _editPanel.Visibility != Visibility.Visible &&
                         !(Keyboard.FocusedElement is ButtonBase))
                {
                    Launch();
                    e.Handled = true;
                }
            };
        }

        // The search box that belongs to the tab on screen, or null where there is none (the simple Play view).
        TextBox TabSearch()
        {
            if (_settingsView.Visibility == Visibility.Visible) return _settingsRail != null ? _settingsRail.Search : null;
            if (_installView.Visibility == Visibility.Visible) return _setupRail != null ? _setupRail.Search : null;
            if (_playView.Visibility == Visibility.Visible || (_navPlay.IsChecked == true && !_simplePlay)) return _search;
            return null;
        }

        void ShowTab(string tab)
        {
            if (_playView == null) return;
            _navPlay.IsChecked = tab == "play";
            _navSettings.IsChecked = tab == "settings";
            _navInstall.IsChecked = tab == "install";
            // Play is two views, and MGS4_PLAY_VIEW says which: the three ways to start the game, or every scene
            // in it. Only ever one of them is up.
            _playView.Visibility = tab == "play" && !_simplePlay ? Visibility.Visible : Visibility.Collapsed;
            _startView.Visibility = tab == "play" && _simplePlay ? Visibility.Visible : Visibility.Collapsed;
            _settingsView.Visibility = tab == "settings" ? Visibility.Visible : Visibility.Collapsed;
            _installView.Visibility = tab == "install" ? Visibility.Visible : Visibility.Collapsed;
            _copyBtn.Visibility = _recheckBtn.Visibility = tab == "install" ? Visibility.Visible : Visibility.Collapsed;
            _reloadBtn.Visibility = _saveBtn.Visibility = tab == "settings" ? Visibility.Visible : Visibility.Collapsed;
            _launchBtn.Visibility = _stopBtn.Visibility = _shortcutBtn.Visibility =
                tab == "play" ? Visibility.Visible : Visibility.Collapsed;
            // The one control the bottom bar carries on Play: which of the two views is on.
            _viewSwitchBtn.Visibility = tab == "play" ? Visibility.Visible : Visibility.Collapsed;
            LabelViewSwitch();

            if (tab == "install") ShowSetup();
            else if (tab == "settings") ShowSettings();
            RefreshState();
            if (_padWired) EnterTab();
        }

        // The three things the pill can say. Launching is the stretch between Launch being pressed and any of the
        // game's programs being seen - Steam coming up, the boot - and it lapses on its own if nothing appears.
        bool RunnerBusy() { return _runProc != null && !_runProc.HasExited; }

        bool Launching()
        {
            if (_gameActivity != null) return false;
            if (RunnerBusy()) return true;
            return _launchedAt != DateTime.MinValue && (DateTime.Now - _launchedAt).TotalSeconds < LaunchGraceSeconds;
        }

        /// <summary>The game has the screen, or is about to: the music stays down and the pill is not grey.</summary>
        bool GameBusy() { return _gameActivity != null || Launching(); }

        // Everything that depends on whether the game is up: what the window cannot infer, it polls for. Driven by
        // a timer rather than by tab switches alone - the game can start or stop while the window sits there, and
        // it did, which left Close the game pressable with nothing to close. The header used to carry a
        // idle/running/driving badge as well; the pill in the bottom bar is its successor, next to the status
        // line, small enough to be glanced at rather than read.
        void RefreshState()
        {
            bool running = _gameUp;
            bool busy = RunnerBusy();

            _stopBtn.IsEnabled = _startStopBtn.IsEnabled = _gameActivity != null || busy;
            UpdateSaveButton();
            PaintGameState();
            // The game gets the speakers to itself, and gets them back when it goes. Cheap either way: this
            // returns at once when what is playing is already what should be.
            ApplyMusic();

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
            string dir = _gameDir;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string activity;
                try { activity = Runner.GameActivity(dir); } catch { activity = null; }
                Win.Dispatcher.BeginInvoke(new Action(delegate
                {
                    _pollBusy = false;
                    _gameActivity = activity;
                    _gameUp = activity == "MGS4";
                    // Seen: the launch is over, whichever way it went. A runner that gave up (the exe was not
                    // there, no window came) ends it too, rather than the pill saying Launching for a minute.
                    if (activity != null) _launchedAt = DateTime.MinValue;
                    else if (_runProc != null && _runProc.HasExited && _runProc.ExitCode != 0) _launchedAt = DateTime.MinValue;
                    RefreshState();
                }));
            });
        }

        // The pill in the bottom bar. Grey, amber, green: the same three the Setup verdict and the speed tags use,
        // so it reads at the size it is drawn at.
        void PaintGameState()
        {
            if (_gameStateTag == null) return;
            string text, dot, back, edge, tip;
            if (_gameActivity != null)
            {
                text = _gameActivity == "MGS4" ? "Running" : _gameActivity + " running";
                dot = "#62C98A"; back = "#142117"; edge = "#2E6B45";
                tip = _gameActivity == "MGS4" ? "mgs4.exe is running"
                    : _gameActivity == "MGS1" ? "The bundled MGS1 (mgs1.exe) is running"
                    : "The Master Collection front-end is up; MGS4 starts from it";
            }
            else if (Launching())
            {
                text = "Launching"; dot = "#F2C14E"; back = "#2A2312"; edge = "#7A6220";
                tip = "Launch was pressed; waiting for the game to appear (Steam is started first if it is not running)";
            }
            else
            {
                text = "Not running"; dot = "#6E6E77"; back = "#1C1C20"; edge = "#3A3A44";
                tip = "mgs4.exe is not running";
            }
            if (_gameStateText.Text != text)
            {
                _gameStateText.Text = text;
                _gameStateDot.Fill = Widgets.Brush(dot);
                _gameStateTag.Background = Widgets.Brush(back);
                _gameStateTag.BorderBrush = Widgets.Brush(edge);
                _gameStateTag.ToolTip = tip;
            }
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

        // Restoring sets controls whose change handlers save, and a save half-way through a restore writes the
        // half that has not been read yet as empty - the acts all shut, the filters all off. SavePrefs is a no-op
        // until the restore is over.
        bool _restoring;

        void RestorePrefs()
        {
            _restoring = true;
            try { RestorePrefsInner(); }
            finally { _restoring = false; }
        }

        void RestorePrefsInner()
        {
            _optAdvance.IsChecked = true;
            Dictionary<string, object> p = Prefs.Read();
            // The hearts. A file with none of the key at all - or no file - is a first run, and starts with the
            // default set; a file that has the key, even an empty one, is the person's own list, taken as written.
            object hearts;
            if (p != null && p.TryGetValue("MusicFavorites", out hearts))
            {
                if (hearts is object[])
                    foreach (object o in (object[])hearts) if (o != null) Music.Favorites.Add(o.ToString());
            }
            else foreach (string t in Music.DefaultFavorites) Music.Favorites.Add(t);
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
            // The simple view's own copy of the same option, and its own default: off. Starting the game and
            // getting out of the way is what that view is for, and a scene run's habits are not its business.
            _startAdvance.IsChecked = flag("StartAdvance");
            _spoilerSeen = flag("SpoilerSeen");
            _optMashX.IsChecked = flag("MashX");
            _optEnd.IsChecked = flag("EndOnGameplay");
            _optHold.IsChecked = flag("Hold");
            _optRes.IsChecked = flag("Res");
            if (str("HoldSecs") != null) _holdSecs.Text = str("HoldSecs");
            // the saved resolution ("ResPick"; older files carried ResW / ResH as two numbers) - a size the list does not have keeps the default
            string savedRes = str("ResPick") ?? (str("ResW") != null && str("ResH") != null ? str("ResW") + "x" + str("ResH") : null);
            if (savedRes != null) SelectRes(savedRes);
            ApplyResFromConfig();       // config.ini's MGS4_RES is the setting; the box shows it
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

            // Which cards on Settings and Setup were folded shut, by key, and whether Setup was left showing only
            // the rows that want something.
            object shutCards;
            if (p.TryGetValue("CardsClosed", out shutCards) && shutCards is object[])
                foreach (object o in (object[])shutCards) if (o != null) Widgets.Closed.Add(o.ToString());
            if (_problemsChip != null) _problemsChip.IsChecked = flag("SetupProblemsOnly");

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
                case "Stage entries": return "Cutscene";   // the Stage chip is gone: a bare id is judged by what it shows
                case "Stage": return "Cutscene";
                case "Known broken": return "Broken";
                case "Favourites": return FavoritesCat;
                default: return name;
            }
        }

        void SavePrefs()
        {
            if (_restoring) return;
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
                { "MusicFavorites", new List<string>(Music.Favorites) },
                { "SceneEdits", SceneEditsForSaving() },
                { "Collapsed", _collapsed.Where(kv => kv.Value).Select(kv => kv.Key).ToList() },
                { "CardsClosed", new List<string>(Widgets.Closed) },
                { "SetupProblemsOnly", _problemsChip != null && _problemsChip.IsChecked == true },
                { "Advance", _optAdvance.IsChecked == true },
                { "StartAdvance", _startAdvance.IsChecked == true },
                { "SpoilerSeen", _spoilerSeen },
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
        // MGS4_SETUP_TAB in config.ini: "hidden" turns the window into a plain launcher - no install check, no
        // add-on, nothing about DLSS - for a machine with nothing to set up, an AMD card's among them.
        public const string SetupTabKey = "MGS4_SETUP_TAB";

        static bool SetupHidden()
        {
            return string.Equals(Paths.Setting(SetupTabKey, "shown"), "hidden", StringComparison.OrdinalIgnoreCase);
        }

        void ApplySetupTab()
        {
            bool hidden = SetupHidden();
            _navInstall.Visibility = hidden ? Visibility.Collapsed : Visibility.Visible;
            if (hidden && _installView.Visibility == Visibility.Visible) ShowTab("play");
        }

        void PrimeSetupIcon()
        {
            if (SetupHidden()) return;
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
        /// <summary>Clip an element to a rounded rectangle of its own size. topOnly rounds the top corners and
        /// leaves the bottom square (the geometry runs past the bottom edge, and ClipToBounds squares it off).</summary>
        static void RoundClip(FrameworkElement e, double radius, bool topOnly)
        {
            double w = e.ActualWidth, h = e.ActualHeight;
            if (w <= 0 || h <= 0) { e.Clip = null; return; }
            e.Clip = new System.Windows.Media.RectangleGeometry(new Rect(0, 0, w, topOnly ? h + radius : h), radius, radius);
        }

}
}
