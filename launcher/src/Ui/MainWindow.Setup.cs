// The Setup tab: the verdict, the game folder everything is checked against, a drop area that says what it takes,
// and one card per group of the install with a row per file.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        void WireSetup()
        {
            _recheckBtn.Click += (s, e) => ShowSetup();
            _copyBtn.Click += (s, e) =>
            {
                if (_sections == null) return;
                try { Clipboard.SetText(Checks.TextReport(_gameDir, _sections)); } catch { }
                _copyBtn.Content = "Copied";
                var t = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(1.6) };
                t.Tick += (s2, e2) => { _copyBtn.Content = "Copy report"; t.Stop(); };
                t.Start();
            };

            _installView.DragOver += (s, e) =>
            {
                e.Handled = true;
                bool over = e.Data.GetDataPresent(DataFormats.FileDrop);
                e.Effects = over ? DragDropEffects.Copy : DragDropEffects.None;
                if (over && _dropZone != null)
                {
                    _dropZone.Stroke = Widgets.Brush("#7C9CFF");
                    _dropZone.Fill = Widgets.Brush("#182034");
                }
            };
            _installView.DragLeave += (s, e) =>
            {
                if (_dropZone == null) return;
                _dropZone.Stroke = Widgets.Brush("#3E4A66");
                _dropZone.Fill = Widgets.Brush("#111520");
            };
            _installView.Drop += (s, e) =>
            {
                e.Handled = true;
                if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
                var paths = (string[])e.Data.GetData(DataFormats.FileDrop);
                if (paths == null || paths.Length == 0) return;
                try { Say(string.Join("   |   ", Install.CopyDropped(_sections, _gameDir, paths))); }
                catch (Exception ex) { Say("drop failed: " + ex.Message); }
                ShowSetup();
            };
        }

        // The checks are about a third of a second of file reads and log parsing, and the cards on top of that.
        // Run synchronously they hold the click, so the tab looks like it is refusing to open: put a placeholder
        // up, let WPF paint, and do the work at Background priority once the frame is on screen.
        void ShowSetup()
        {
            if (_installView.Visibility != Visibility.Visible) return;
            _installHost.Children.Clear();
            StackPanel body;
            _installHost.Children.Add(Widgets.Card("Setup", "Reading the files, the settings and the last run...",
                                                   "info", "checking", out body));
            Say("checking...");
            Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, new Action(BuildSetup));
        }

        void BuildSetup()
        {
            if (_installView.Visibility != Visibility.Visible) return;
            _installHost.Children.Clear();

            if (string.IsNullOrEmpty(_gameDir))
            {
                StackPanel none;
                _installHost.Children.Add(StatusCard(new Verdict
                { Text = "No game folder", Kind = "bad", Note = "nothing to check against yet" }));
                // Which libraries were searched is the whole answer on a machine with more than one drive: Steam's
                // own install on C: is what names a library on D:.
                string why = "The Steam libraries were searched for app 2492670 and no mgs4.exe turned up.";
                List<string> libs = Paths.SteamLibraryList();
                if (libs.Count > 0) why += " Looked in: " + string.Join(", ", libs) + ".";
                else why += " No Steam library was found at all - Steam's own install could not be located.";
                if (!string.IsNullOrEmpty(_opt.GameDirBad)) why = "There is no mgs4.exe in " + _opt.GameDirBad + ".";
                _installHost.Children.Add(Widgets.Card("Nothing to check yet",
                    why + " Point the app at the folder holding mgs4.exe with Browse above, and everything below fills in.",
                    "bad", "no game folder", out none));
                Say("no game folder set");
                return;
            }

            _sections = Checks.Run(_gameDir);
            _installHost.Children.Add(StatusCard(Checks.GetVerdict(_sections)));

            foreach (Section sec in _sections)
            {
                int bad = sec.Rows.Count(r => r.Status == "bad");
                int warn = sec.Rows.Count(r => r.Status == "warn");
                string kind = "ok", label = "all good";
                if (warn > 0) { kind = "warn"; label = warn + " to look at"; }
                if (bad > 0) { kind = "bad"; label = bad + " missing"; }
                if (sec.Rows.Count == 0) { kind = "info"; label = "nothing to check"; }

                StackPanel body;
                Border card = Widgets.Card(sec.Title, sec.Blurb, kind, label, out body);
                if (!string.IsNullOrEmpty(sec.Guide) || !string.IsNullOrEmpty(sec.Url))
                    body.Children.Add(Widgets.GuideRow(sec));

                if (sec.Id == "settings" && !string.IsNullOrEmpty(sec.SavedSettings)) body.Children.Add(GameSettingsRow(sec));
                if (sec.Id == "game" && !Paths.Exists(Paths.Join(_gameDir, "steam_appid.txt"))) body.Children.Add(AppIdRow());
                if (sec.Bundled) body.Children.Add(AddonRow());

                bool first = true;
                foreach (Row row in sec.Rows) { body.Children.Add(Widgets.CheckRow(row, first)); first = false; }
                _installHost.Children.Add(card);
            }
            Say("checked at " + DateTime.Now.ToString("HH:mm:ss") + "  -  file list: tools\\install_manifest.json");
        }

        // The first card, and the only one that is not a step: how the check came out, the folder it ran against,
        // and the fact that files can be dropped here.
        Border StatusCard(Verdict verdict)
        {
            StackPanel body;
            Border card = Widgets.Card("Install check: " + verdict.Text, verdict.Note, verdict.Kind, verdict.Text, out body);

            bool ok = !string.IsNullOrEmpty(_gameDir);
            var row = new Border { Padding = new Thickness(18, 13, 18, 13) };
            Grid g = Widgets.Columns("Auto", "*", "Auto");
            TextBlock label = Widgets.Text("Game folder", 12, "#858D9E");
            label.VerticalAlignment = VerticalAlignment.Center;
            label.Margin = new Thickness(0, 0, 14, 0);
            g.Children.Add(label);

            TextBlock path = Widgets.Text(ok ? _gameDir : "no mgs4.exe found - pick the folder that holds it",
                                          12, ok ? "#7C9CFF" : "#FF7B72", false, true);
            path.VerticalAlignment = VerticalAlignment.Center;
            path.ToolTip = ok ? Paths.GameDirSource() : null;
            Grid.SetColumn(path, 1);
            g.Children.Add(path);

            var buttons = new StackPanel { Orientation = Orientation.Horizontal };
            var browse = new Button { Content = "Browse...", Style = Widgets.FlatStyle, Margin = new Thickness(0, 0, 8, 0) };
            browse.Click += (s, e) => BrowseForGame();
            var detect = new Button { Content = "Detect", Style = Widgets.FlatStyle };
            detect.Click += (s, e) =>
            {
                Paths.SetConfiguredGameDir(null);
                _gameDir = Paths.GameDir();
                Say(_gameDir == null ? "no mgs4.exe found in the Steam libraries" : "detected " + _gameDir);
                ShowSetup();
            };
            buttons.Children.Add(browse);
            buttons.Children.Add(detect);
            Grid.SetColumn(buttons, 2);
            g.Children.Add(buttons);
            row.Child = g;
            body.Children.Add(row);
            body.Children.Add(DropArea());
            return card;
        }

        // What can be dropped, named from the manifest so the two cannot drift.
        Border DropArea()
        {
            var wrap = new Border { Padding = new Thickness(18, 0, 18, 16) };
            var grid = new Grid();
            _dropZone = new System.Windows.Shapes.Rectangle
            {
                RadiusX = 10, RadiusY = 10,
                Stroke = Widgets.Brush("#3E4A66"),
                Fill = Widgets.Brush("#111520"),
                StrokeThickness = 1,
                StrokeDashArray = new DoubleCollection(new[] { 4.0, 3.0 }),
                MinHeight = 92,
            };
            grid.Children.Add(_dropZone);

            var stack = new StackPanel
            {
                VerticalAlignment = VerticalAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(18, 14, 18, 14),
            };
            TextBlock title = Widgets.Text("Drop files here", 12, "#E7EAF0", true);
            title.HorizontalAlignment = HorizontalAlignment.Center;
            stack.Children.Add(title);

            var names = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) };
            List<string> drops = Install.DropNames();
            for (int i = 0; i < drops.Count; i++)
            {
                TextBlock t = Widgets.Text(drops[i], 11, "#9FB6FF", false, true);
                t.Margin = new Thickness(0, 0, 10, 0);
                names.Children.Add(t);
                if (i == drops.Count - 2) names.Children.Add(Widgets.Text("or ", 11, "#5C6478"));
            }
            stack.Children.Add(names);
            TextBlock note = Widgets.Text("Zips are unpacked, everything else is copied into place. Anything else is left alone.",
                                          11, "#5C6478");
            note.HorizontalAlignment = HorizontalAlignment.Center;
            note.Margin = new Thickness(0, 6, 0, 0);
            stack.Children.Add(note);
            grid.Children.Add(stack);
            wrap.Child = grid;
            return wrap;
        }

        void BrowseForGame()
        {
            var dlg = new Microsoft.Win32.OpenFileDialog
            {
                Title = "Pick mgs4.exe",
                Filter = "mgs4.exe|mgs4.exe|Every file|*.*",
                CheckFileExists = true,
            };
            if (dlg.ShowDialog() != true) return;
            string dir = Paths.ResolveGameDir(Path.GetDirectoryName(dlg.FileName));
            if (dir == null) { Say("no mgs4.exe in that folder"); return; }
            _gameDir = dir;
            Paths.SetConfiguredGameDir(dir);
            Art.SetWindowIcon(Win, _gameDir);
            Say("game folder set to " + dir + " (written to config.ini)");
            ShowSetup();
        }

        // The game's own options, which the add-on needs set a particular way.
        Border GameSettingsRow(Section sec)
        {
            bool running = Checks.GameRunning();
            string text = sec.WrongKeys.Count > 0
                ? "The game is set to " + string.Join(", ", sec.WrongKeys) + " differently from what the add-on needs."
                : "DirectX 12, vsync off, FXAA off and the 60 fps limiter are all set as the add-on wants them.";
            if (running) text += " The game is running - close it before writing to its settings.";
            return Widgets.ActionRow(text, "Set them for me",
                "Writes api=dx12, vsync=false, enableFXAA=false and fpsLimiter=60 into " + sec.SavedSettings,
                sec.WrongKeys.Count > 0 && !running, sec.WrongKeys.Count > 0,
                (s, e) =>
                {
                    if (Checks.GameRunning()) { Say("close the game first - it owns mgs4.savedsettings"); return; }
                    try
                    {
                        Checks.SetGameSettings(sec.SavedSettings);
                        Say("set api=dx12, vsync=false, enableFXAA=false, fpsLimiter=60 in " + sec.SavedSettings);
                    }
                    catch (Exception ex) { Say("could not write the game settings: " + ex.Message); }
                    ShowSetup();
                });
        }

        // One line the install has no other way of getting: see Install.SteamAppId.
        Border AppIdRow()
        {
            return Widgets.ActionRow(
                "Neither Steam nor the game ever writes steam_appid.txt - it is one line holding the app id, and " +
                "without it a scene boot is handed back to Steam and relaunched without its --stage argument. " +
                "Launching a scene writes it anyway; this is the same thing, now.",
                "Write steam_appid.txt", "Writes " + Paths.Join(_gameDir, "steam_appid.txt"), true, false,
                (s, e) => { Say(Install.SteamAppId(_gameDir).ToString()); ShowSetup(); });
        }

        // The add-on installs itself: the app ships with mgs4_dlss.addon64 and mgs4_dlss.ini, so the one group of
        // files this whole thing exists for is a button rather than a download.
        Border AddonRow()
        {
            string addon, ini;
            Install.FindBundled(out addon, out ini);
            if (addon == null) return new Border();     // an unbuilt checkout has none; the drop area still works

            bool running = Checks.GameRunning();
            bool have = Paths.Exists(Paths.Join(_gameDir, "mgs4_dlss.addon64"));
            string text = have
                ? "The add-on is in place. Installing again replaces mgs4_dlss.addon64 with the copy that ships here and keeps the mgs4_dlss.ini you have."
                : "The add-on ships with this app - nothing to download. This copies mgs4_dlss.addon64, and an mgs4_dlss.ini if the game folder has none, next to mgs4.exe.";
            if (running) text += " The game is running - close it first, ReShade holds the add-on open.";
            return Widgets.ActionRow(text, have ? "Reinstall the add-on" : "Install the add-on",
                "Copies " + addon + " into " + _gameDir, !running, !have,
                (s, e) => { Say(Install.BundledAddon(_gameDir).ToString()); ShowSetup(); });
        }
    }
}
