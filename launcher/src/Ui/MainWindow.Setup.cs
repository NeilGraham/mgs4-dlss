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
                _dropZone.Stroke = Widgets.Brush("#43434C");
                _dropZone.Fill = Widgets.Brush("#0E0E10");
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
        // Opening the tab paints the last check straight away and runs the next one behind it. What the check
        // looks at - which files are installed, what the add-on wrote on its last run - changes when somebody
        // installs something, which is not something that happens between two clicks on a tab. So the answer on
        // screen is almost always already the right one, and when it is not, the fresh one replaces it a moment
        // later. Only a real difference redraws: an identical result leaves the cards exactly where they were.
        void ShowSetup()
        {
            if (_installView.Visibility != Visibility.Visible) return;

            // Nothing checked yet this run: the last run left one on disk, and it is almost certainly still true -
            // nothing installs itself between closing the window and opening it again. Tried once, so a folder
            // with no cache does not go back to the file on every visit.
            if (_sections == null && !_setupCacheTried)
            {
                _setupCacheTried = true;
                DateTime taken;
                List<Section> kept = SetupCache.Read(_gameDir, out taken);
                if (kept != null) { _sections = kept; _setupDir = _gameDir; _setupAt = taken; }
            }

            bool cached = _sections != null
                       && string.Equals(_setupDir ?? "", _gameDir ?? "", StringComparison.OrdinalIgnoreCase);
            if (cached)
            {
                PaintSetup();
                Say("checked at " + _setupAt.ToString("HH:mm:ss") + "  -  rechecking...");
            }
            else if (!string.IsNullOrEmpty(_gameDir))
            {
                _installHost.Children.Clear();
                StackPanel checking;
                _installHost.Children.Add(Widgets.Card("Setup", "Reading the files, the settings and the last run...",
                                                       "info", "checking", out checking));
                Say("checking...");
            }
            StartSetupRefresh();
        }

        // Checks.Run is file reads, PE version stamps and a driver lookup - thirty-five milliseconds with the disk
        // warm and a good deal more without. None of it touches a control, so it runs off the window's thread and
        // hands the result back to be compared where controls may be touched.
        void StartSetupRefresh()
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                _sections = null; _setupDir = null;
                if (_installView.Visibility == Visibility.Visible) PaintSetup();
                return;
            }
            if (_setupBusy) return;
            _setupBusy = true;
            string dir = _gameDir;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                List<Section> fresh = null;
                try { fresh = Checks.Run(dir); }
                catch { fresh = null; }
                Win.Dispatcher.BeginInvoke(new Action(delegate { EndSetupRefresh(dir, fresh); }));
            });
        }

        void EndSetupRefresh(string dir, List<Section> fresh)
        {
            _setupBusy = false;
            if (fresh == null) return;
            if (!string.Equals(dir ?? "", _gameDir ?? "", StringComparison.OrdinalIgnoreCase)) return;

            bool same = _sections != null && SetupSignature(_sections) == SetupSignature(fresh);
            _sections = fresh;
            _setupDir = dir;
            _setupAt = DateTime.Now;
            if (!same) SetupCache.Write(dir, fresh);   // only when it moved; an identical check is already on disk

            // The tab's own badge, whichever tab is showing: a window that opened on Play primes it from the
            // cache and this is what corrects it, so it cannot sit on a verdict the check has moved past.
            try { SetSetupIcon(Checks.GetVerdict(fresh)); } catch { }

            if (_installView.Visibility != Visibility.Visible) return;
            if (!same) PaintSetup();
            Say("checked at " + _setupAt.ToString("HH:mm:ss") + "  -  file list: " + Checks.ManifestSource);
        }

        // Everything a card is drawn from, in one string: the rows a section ended up with, and the two things a
        // section carries besides them. Two runs that read the same are the same check, and nothing has to move.
        static string SetupSignature(List<Section> sections)
        {
            var sb = new System.Text.StringBuilder();
            foreach (Section sec in sections)
            {
                sb.Append(sec.Id).Append('|').Append(sec.SavedSettings).Append('|');
                foreach (string k in sec.WrongKeys) sb.Append(k).Append(',');
                sb.Append('\n');
                foreach (Row r in sec.Rows)
                    sb.Append(r.Status).Append('\t').Append(r.Name).Append('\t')
                      .Append(r.Detail).Append('\t').Append(r.Value).Append('\n');
            }
            return sb.ToString();
        }

        void PaintSetup()
        {
            // A repaint from a refresh happens under the reader's eyes, so the page stays where they left it.
            double keep = _installView.VerticalOffset;
            _installHost.Children.Clear();

            if (string.IsNullOrEmpty(_gameDir))
            {
                StackPanel none;
                var nothing = new Verdict { Text = "No game folder", Kind = "bad", Note = "nothing to check against yet" };
                _installHost.Children.Add(StatusCard(nothing));
                SetSetupIcon(nothing);
                // Which libraries were searched is the whole answer on a machine with more than one drive: Steam's
                // own install on C: is what names a library on D:.
                string why = "The Steam libraries were searched for app 2492670 and no mgs4.exe turned up.";
                List<string> libs = Paths.SteamLibraryList();
                if (libs.Count > 0) why += " Looked in: " + string.Join(", ", libs) + ".";
                else why += " No Steam library was found at all - Steam's own install could not be located.";
                if (!string.IsNullOrEmpty(_opt.GameDirBad)) why = "There is no mgs4.exe in " + _opt.GameDirBad + ".";
                _installHost.Children.Add(Widgets.Card("Nothing to check yet",
                    why + " Point the app at the folder holding mgs4.exe with Change above, and everything below fills in.",
                    "bad", "no game folder", out none));
                Say("no game folder set");
                return;
            }

            if (_sections == null) return;
            Verdict verdict = Checks.GetVerdict(_sections);
            _installHost.Children.Add(StatusCard(verdict));
            SetSetupIcon(verdict);

            if (!string.IsNullOrEmpty(Checks.ManifestError))
            {
                StackPanel lost;
                _installHost.Children.Add(Widgets.Card("No file list",
                    Checks.ManifestError + " The list of what an install needs is normally read from the tools folder " +
                    "beside the launcher, with a copy built into the launcher itself as a fallback; without either " +
                    "there is nothing to check the game folder against. Everything else in the window still works.",
                    "bad", "cannot check", out lost));
                Say("no file list - nothing to check against");
                return;
            }

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
            if (keep > 0)
                Win.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded,
                                           new Action(delegate { _installView.ScrollToVerticalOffset(keep); }));
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
            TextBlock label = Widgets.Text("Game folder", 12, "#97979F");
            label.VerticalAlignment = VerticalAlignment.Center;
            label.Margin = new Thickness(0, 0, 14, 0);
            g.Children.Add(label);

            TextBlock path = Widgets.Text(ok ? _gameDir : "no mgs4.exe found - pick the folder that holds it",
                                          12, ok ? "#7C9CFF" : "#FF6B66", false, true);
            path.VerticalAlignment = VerticalAlignment.Center;
            path.ToolTip = ok ? Paths.GameDirSource() + " - click to open it in Explorer" : null;
            if (ok)
            {
                path.Cursor = System.Windows.Input.Cursors.Hand;
                path.MouseLeftButtonUp += (s, e) => Widgets.OpenFolder(_gameDir);
            }
            Grid.SetColumn(path, 1);
            g.Children.Add(path);

            // Two jobs, two labels that cannot be read as each other: open the folder that is set, or pick a
            // different one. Browse read as both at once. There is no button back to the lookup because the
            // lookup is what runs whenever MGS4_DIR is not set - clearing it is a config.ini edit, not a step
            // anyone takes from here.
            var buttons = new StackPanel { Orientation = Orientation.Horizontal };
            var open = new Button
            {
                Content = "Open folder",
                Style = Widgets.FlatStyle,
                Margin = new Thickness(0, 0, 8, 0),
                IsEnabled = ok,
                ToolTip = ok ? "Opens " + _gameDir + " in Explorer" : "Set the game folder first",
            };
            open.Click += (s, e) => Widgets.OpenFolder(_gameDir);
            var browse = new Button
            {
                Content = "Change...",
                Style = Widgets.FlatStyle,
                ToolTip = "Pick mgs4.exe yourself and write its folder to config.ini",
            };
            browse.Click += (s, e) => BrowseForGame();
            buttons.Children.Add(open);
            buttons.Children.Add(browse);

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
                Stroke = Widgets.Brush("#43434C"),
                Fill = Widgets.Brush("#0E0E10"),
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
            TextBlock title = Widgets.Text("Drop files here", 12, "#ECECEE", true);
            title.HorizontalAlignment = HorizontalAlignment.Center;
            stack.Children.Add(title);

            var names = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) };
            List<string> drops = Install.DropNames();
            for (int i = 0; i < drops.Count; i++)
            {
                TextBlock t = Widgets.Text(drops[i], 11, "#9FB6FF", false, true);
                t.Margin = new Thickness(0, 0, 10, 0);
                names.Children.Add(t);
                if (i == drops.Count - 2) names.Children.Add(Widgets.Text("or ", 11, "#6E6E77"));
            }
            stack.Children.Add(names);
            TextBlock note = Widgets.Text("Zips are unpacked, everything else is copied into place. Anything else is left alone.",
                                          11, "#6E6E77");
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
            // The icon used to be re-read from the new folder's mgs4.exe here. It is the exe's own now, chosen
            // when this was built, so pointing at a different install does not change what the window wears.
            Say("game folder set to " + dir + " (written to config.ini)");
            ShowSetup();
        }

        // The game's own options, which the add-on needs set a particular way.
        Border GameSettingsRow(Section sec)
        {
            bool running = _gameUp;
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

        // The add-on installs itself: the app ships with mgs4_dlss.addon64 and mgs4_dlss.ini - built into the
        // release exe, or in build\ and dlss-addon\ of a checkout - so the one group of files this whole thing
        // exists for is a button rather than a download.
        Border AddonRow()
        {
            string addon, ini;
            Install.FindBundled(out addon, out ini);
            if (addon == null) return new Border();     // an unbuilt checkout has none; the drop area still works

            bool running = _gameUp;
            bool have = Paths.Exists(Paths.Join(_gameDir, "mgs4_dlss.addon64"));
            string text = have
                ? "The add-on is in place. Installing again replaces mgs4_dlss.addon64 with the copy that ships here and keeps the mgs4_dlss.ini you have."
                : "The add-on ships with this app - nothing to download. This copies mgs4_dlss.addon64, and an mgs4_dlss.ini if the game folder has none, next to mgs4.exe.";
            if (running) text += " The game is running - close it first, ReShade holds the add-on open.";
            return Widgets.ActionRow(text, have ? "Reinstall the add-on" : "Install the add-on",
                (addon == Install.BuiltIn ? "Copies the add-on built into this launcher" : "Copies " + addon) + " into " + _gameDir,
                !running, !have,
                (s, e) => { Say(Install.BundledAddon(_gameDir).ToString()); ShowSetup(); });
        }
    }
}
