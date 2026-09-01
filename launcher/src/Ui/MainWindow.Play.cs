// The Play tab: every launchable scene, grouped by act, with the automation the tests use and the command line
// that does the same thing. Rows are act headers and scenes in one list - the item template shows whichever half
// the row says it is, which keeps the grouping without a DataTemplateSelector.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;

namespace Mgs4Launcher
{
    // What a scene is, as a badge: a short word and a colour of its own, so the kind reads before the name does.
    // The families are the ones the rest of the window already uses - green for what plays, violet for what is
    // watched, amber for a briefing, blue for a plain stage boot, sky for the two entries that start the game.
    class Badge
    {
        public string Text, Back, Edge, Ink;
        public Badge(string text, string back, string edge, string ink)
        { Text = text; Back = back; Edge = edge; Ink = ink; }

        public static Badge For(string kind)
        {
            switch (kind)
            {
                case "gameplay":    return new Badge("Gameplay", "#12261A", "#1F6F43", "#5BD98A");
                case "cutscene":    return new Badge("Cutscene", "#211A33", "#4B3E7A", "#B79CFF");
                case "briefing":    return new Badge("Briefing", "#2A2312", "#7A6220", "#F2C14E");
                case "start":       return new Badge("Start", "#10222B", "#2A5A73", "#7DD3FC");
                case "stage-entry": return new Badge("Stage", "#161B2A", "#33436E", "#8FA6E8");
                default:            return new Badge(string.IsNullOrEmpty(kind) ? "Scene" : kind,
                                                     "#161B2A", "#33436E", "#8FA6E8");
            }
        }
    }

    // One row of the scene list: either an act header or a scene, never both.
    public class SceneRow
    {
        public bool IsHeader { get; set; }
        public string ActKey { get; set; }
        public string HeaderVis { get { return IsHeader ? "Visible" : "Collapsed"; } }
        public string SceneVis { get { return IsHeader ? "Collapsed" : "Visible"; } }
        public string Chevron { get; set; }
        public string HeadText { get; set; }
        public string HeadCount { get; set; }
        public string Id { get; set; }
        public string Title { get; set; }
        public string Sub { get; set; }
        public Scene Entry { get; set; }
        public List<string> Cats { get; set; }
        public string Hay { get; set; }
        public bool Hidden { get; set; }

        public string BadgeText { get; set; }
        public Brush BadgeBack { get; set; }
        public Brush BadgeEdge { get; set; }
        public Brush BadgeInk { get; set; }

        // A filled star for a favourite, an outline for the rest. Both are one character wide, so the column does
        // not shift as rows are starred.
        public bool Favourite { get; set; }
        public string Star { get { return Favourite ? "\u2605" : "\u2606"; } }
        public Brush StarInk { get { return Widgets.Brush(Favourite ? "#F2C14E" : "#4A5163"); } }
        public string StarTip { get { return Favourite ? "In Favourites - click to remove" : "Add to Favourites"; } }
    }

    partial class MainWindow
    {
        // Favourites first, because it is the one a person curates. "Start the game" is not a chip: the two entries
        // it covered are an act of their own at the top of the list, so a filter for them only ever hid the rest.
        public const string FavouritesCat = "Favourites";
        static readonly string[] CatNames =
        {
            FavouritesCat, "Cutscenes", "Mission briefings", "Named scenes", "Gameplay", "Stage entries", "Known broken"
        };

        List<SceneRow> _allRows;

        // Which of the filter chips a scene answers to. Worked out once, so filtering is a set test rather than a
        // predicate run over four hundred rows on every keystroke.
        List<string> CatsOf(Scene e)
        {
            var c = new List<string>();
            if (_favourites.Contains(e.Id)) c.Add(FavouritesCat);
            if (e.Kind == "cutscene" || e.Kind == "briefing") c.Add("Cutscenes");
            if (e.Kind == "briefing") c.Add("Mission briefings");
            if (!string.IsNullOrEmpty(e.Name)) c.Add("Named scenes");
            if (e.Kind == "gameplay") c.Add("Gameplay");
            if (e.Kind == "stage-entry") c.Add("Stage entries");
            if (e.Hidden) c.Add("Known broken");
            return c;
        }

        void WirePlay()
        {
            _allRows = Catalogue.All().Select(e =>
            {
                Badge badge = Badge.For(e.Kind);
                return new SceneRow
                {
                    IsHeader = false,
                    Entry = e,
                    Id = e.Id,
                    ActKey = e.ActKey,
                    Hidden = e.Hidden,
                    Title = string.IsNullOrEmpty(e.Name) ? e.Id : e.Name,
                    Sub = string.IsNullOrEmpty(e.Description) ? e.Note : e.Description,
                    Cats = CatsOf(e),
                    Hay = (e.Id + " " + string.Join(" ", e.Alts) + " " + e.Name + " " + e.ActTitle + " " + e.Kind + " " + e.Description).ToLowerInvariant(),
                    BadgeText = badge.Text,
                    BadgeBack = Widgets.Brush(badge.Back),
                    BadgeEdge = Widgets.Brush(badge.Edge),
                    BadgeInk = Widgets.Brush(badge.Ink),
                    Favourite = _favourites.Contains(e.Id),
                };
            }).ToList();

            // Whether the pad *could* be used, asked without using one. This line used to be worked out by opening
            // a virtual DualShock and closing it again, every time the window opened - which really did connect a
            // controller, sound and all, to answer a question about a note under a checkbox. The two things that
            // decide it are a file on disk and a driver's state, and both can simply be looked at.
            Row vigem = Checks.VigemRow();
            _mashNote.Text = vigem.Status == "ok"
                ? "A virtual DualShock 4 taps Cross about six times a second, which is what MGS4's in-cutscene flashback prompts want. The game must stay in the foreground. It is created when a run starts and removed when it ends."
                : "Needs ViGEmBus and ViGEmClient.dll (" + vigem.Value +
                  "). Without them the launcher can only press Enter, which gets past the prompts but does not fire the flashbacks.";

            foreach (string name in CatNames)
            {
                var chip = new ToggleButton { Content = name, Tag = name, Style = Widgets.ChipStyle };
                chip.Checked += (s, e) => ApplyFilter();
                chip.Unchecked += (s, e) => ApplyFilter();
                _filters.Children.Add(chip);
            }
            _search.TextChanged += (s, e) => ApplyFilter();

            // Act headers are toggled before the ListBox gets the click, and the event is marked handled so a
            // header is never selected.
            _sceneList.PreviewMouseLeftButtonDown += (s, e) =>
            {
                DependencyObject src = e.OriginalSource as DependencyObject;
                while (src != null && !(src is ListBoxItem))
                    src = System.Windows.Media.VisualTreeHelper.GetParent(src);
                var item = src as ListBoxItem;
                if (item == null) return;
                var row = item.DataContext as SceneRow;
                if (row == null) return;

                // The star is its own click: toggling a favourite must not also pick the scene.
                if (!row.IsHeader && HitTheStar(e.OriginalSource))
                {
                    ToggleFavourite(row.Id);
                    e.Handled = true;
                    return;
                }
                if (!row.IsHeader) return;
                _collapsed[row.ActKey] = !(_collapsed.ContainsKey(row.ActKey) && _collapsed[row.ActKey]);
                e.Handled = true;
                ApplyFilter();
            };
            // Double-clicking a scene starts it, the way double-clicking a file opens it. Act headers and the
            // star never get here: the handler above marks their clicks handled, so the ListBox never sees a
            // second one to pair into a double.
            _sceneList.MouseDoubleClick += (s, e) =>
            {
                // On a row, not merely inside the list: the empty space under the last scene would otherwise
                // launch whatever was still selected.
                DependencyObject src = e.OriginalSource as DependencyObject;
                while (src != null && !(src is ListBoxItem))
                    src = System.Windows.Media.VisualTreeHelper.GetParent(src);
                var item = src as ListBoxItem;
                if (item == null) return;
                var row = item.DataContext as SceneRow;
                if (row == null || row.IsHeader) return;
                e.Handled = true;
                Launch();
            };
            _sceneList.SelectionChanged += (s, e) =>
            {
                var row = _sceneList.SelectedItem as SceneRow;
                if (row == null || row.IsHeader) return;
                _pickedId = row.Id;
                ShowPicked(row.Entry);
            };

            foreach (CheckBox c in new[] { _optAdvance, _optMashX, _optEnd, _optHold, _optRes })
            {
                c.Checked += (s, e) => UpdatePreview();
                c.Unchecked += (s, e) => UpdatePreview();
            }
            foreach (TextBox t in new[] { _holdSecs, _resW, _resH }) t.TextChanged += (s, e) => UpdatePreview();
            _altPick.SelectionChanged += (s, e) =>
            {
                string id = _altPick.SelectedItem as string;
                if (!string.IsNullOrEmpty(id)) { _pickedId = id; UpdatePreview(); }
            };

            _launchBtn.Click += (s, e) => Launch();
            _stopBtn.Click += (s, e) => { Runner.StopGame(); Say("closed mgs4.exe"); RefreshState(); };
            _shortcutBtn.Click += (s, e) => MakeShortcut();

            // Every act starts collapsed, so the window opens as a short list of acts rather than 400 rows.
            foreach (string key in Catalogue.ActOrder) _collapsed[key] = true;
            ApplyFilter();
        }

        // A scene asked for on the command line, or the last one used, opens its act and is selected in it. Called
        // after the preferences are read, because that is where the last one comes from.
        void RestoreSelection()
        {
            Scene entry = Catalogue.Find(_pickedId);
            if (entry != null)
            {
                _collapsed[entry.ActKey] = false;
                _pickedId = entry.Id;
                foreach (var chip in _filters.Children.OfType<ToggleButton>()) chip.IsChecked = false;   // so the scene is in view
            }
            ApplyFilter();
            if (entry != null)
            {
                SceneRow hit = (_sceneList.ItemsSource as IEnumerable<SceneRow>)
                    .FirstOrDefault(r => !r.IsHeader && r.Id == _pickedId);
                if (hit != null) { _sceneList.SelectedItem = hit; _sceneList.ScrollIntoView(hit); }
                ShowPicked(entry);
            }
            else UpdatePreview();
        }

        // The star carries Tag="star" in the row template; nothing else in a row does.
        static bool HitTheStar(object source)
        {
            var d = source as DependencyObject;
            while (d != null)
            {
                var fe = d as FrameworkElement;
                if (fe != null && (fe.Tag as string) == "star") return true;
                if (d is ListBoxItem) return false;
                d = System.Windows.Media.VisualTreeHelper.GetParent(d);
            }
            return false;
        }

        void ToggleFavourite(string id)
        {
            if (!_favourites.Remove(id)) _favourites.Add(id);
            foreach (SceneRow r in _allRows)
            {
                if (r.Id != id) continue;
                r.Favourite = _favourites.Contains(id);
                r.Cats = CatsOf(r.Entry);
            }
            SavePrefs();            // a starred scene should still be starred if the window is closed on the spot
            ApplyFilter();
            Say(_favourites.Contains(id)
                ? id + " added to Favourites (" + _favourites.Count + ")"
                : id + " removed from Favourites (" + _favourites.Count + ")");
        }

        void ApplyFilter()
        {
            string q = _search.Text.Trim().ToLowerInvariant();
            _searchHint.Visibility = _search.Text.Length > 0 ? Visibility.Collapsed : Visibility.Visible;

            // No chip ticked means everything (bar the broken ids); ticking chips shows the union of what they cover.
            var picked = _filters.Children.OfType<ToggleButton>()
                                 .Where(c => c.IsChecked == true).Select(c => c.Tag.ToString()).ToList();
            IEnumerable<SceneRow> rows = _allRows;
            if (picked.Count > 0) rows = rows.Where(r => r.Cats.Any(picked.Contains));
            if (!picked.Contains("Known broken")) rows = rows.Where(r => !r.Hidden);
            if (q.Length > 0) rows = rows.Where(r => r.Hay.Contains(q));
            var list = rows.ToList();

            // A search is a request to see what matched, so it overrides the collapsed groups.
            bool searching = q.Length > 0;
            var outRows = new List<SceneRow>();
            foreach (string key in Catalogue.ActOrder)
            {
                var inAct = list.Where(r => r.ActKey == key).ToList();
                if (inAct.Count == 0) continue;
                bool collapsed = !searching && _collapsed.ContainsKey(key) && _collapsed[key];
                outRows.Add(new SceneRow
                {
                    IsHeader = true,
                    ActKey = key,
                    Chevron = collapsed ? "▶" : "▼",
                    HeadText = Catalogue.ActTitles.ContainsKey(key) ? Catalogue.ActTitles[key] : key,
                    HeadCount = inAct.Count.ToString(),
                });
                if (!collapsed) outRows.AddRange(inAct);
            }
            _sceneList.ItemsSource = outRows;
            if (!string.IsNullOrEmpty(_pickedId))
            {
                SceneRow hit = outRows.FirstOrDefault(r => !r.IsHeader && r.Id == _pickedId);
                if (hit != null) _sceneList.SelectedItem = hit;
            }
            Say(list.Count + " of " + _allRows.Count + " entries" +
                (picked.Count > 0 ? " - " + string.Join(", ", picked) : ""));
        }

        void ShowPicked(Scene scene)
        {
            if (scene == null) return;
            _pickTitle.Text = string.IsNullOrEmpty(scene.Name) ? scene.Id : scene.Name;
            _pickSub.Text = string.IsNullOrEmpty(scene.Description) ? scene.Note : scene.Description;

            // The game-start entries take none of the run options: a menu has no boot prompts to press through.
            bool isStart = Catalogue.IsStartEntry(scene.Id);
            foreach (Control c in new Control[] { _optAdvance, _optMashX, _optEnd, _optHold, _holdSecs })
                c.IsEnabled = !isStart;
            _pickWarn.Text = scene.Hidden && scene.Id == "@main"
                ? "Known to crash on most launches with frame generation on - 'Start the game' is the honest way in."
                : scene.Hidden ? "This id is in the known-broken list: it crashes or comes up black." : "";
            _pickWarn.Visibility = string.IsNullOrEmpty(_pickWarn.Text) ? Visibility.Collapsed : Visibility.Visible;

            _altPick.Items.Clear();
            if (scene.Alts.Count > 0)
            {
                _altPick.Items.Add(scene.Id);
                foreach (string a in scene.Alts) _altPick.Items.Add(a);
                _altPick.SelectedItem = _pickedId;
                _altRow.Visibility = Visibility.Visible;
            }
            else _altRow.Visibility = Visibility.Collapsed;
            UpdatePreview();
        }

        Options CurrentOptions()
        {
            var o = new Options
            {
                Stage = _pickedId,
                Advance = _optAdvance.IsChecked == true,
                MashX = _optMashX.IsChecked == true,
                EndOnGameplay = _optEnd.IsChecked == true,
                KeepRunning = false,
                GameDir = _gameDir,
                GameDirGiven = _opt.GameDirGiven,
            };
            double hold;
            if (_optHold.IsChecked == true && double.TryParse(_holdSecs.Text, out hold)) o.Hold = hold;
            int w, h;
            if (_optRes.IsChecked == true && int.TryParse(_resW.Text, out w) && int.TryParse(_resH.Text, out h))
            { o.Width = w; o.Height = h; }
            return o;
        }

        void UpdatePreview()
        {
            if (string.IsNullOrEmpty(_pickedId)) { _cmdPreview.Text = "mgs4-dlss-launcher --list"; return; }
            _cmdPreview.Text = Options.Preview(CurrentOptions().ToCli());
        }

        // The window never runs a scene on its own thread - it starts this same program again with the equivalent
        // command line, so the UI stays responsive and every route into the game is one code path.
        void Launch()
        {
            if (string.IsNullOrEmpty(_pickedId)) { Say("pick a scene first"); return; }
            var cli = new List<string>();
            foreach (string a in CurrentOptions().ToCli()) cli.Add(a.Contains(" ") ? "\"" + a + "\"" : a);
            try
            {
                _runProc = Process.Start(new ProcessStartInfo(
                    System.Reflection.Assembly.GetExecutingAssembly().Location, string.Join(" ", cli))
                { UseShellExecute = false, CreateNoWindow = true });
                Say("launching: " + Options.Preview(CurrentOptions().ToCli()));
            }
            catch (Exception e) { Say("could not launch: " + e.Message); }
            RefreshState();
        }

        void MakeShortcut()
        {
            Scene scene = Catalogue.Find(_pickedId);
            if (scene == null) { Say("pick a scene first"); return; }
            var dlg = new Microsoft.Win32.SaveFileDialog
            {
                Title = "Save this scene as a shortcut",
                Filter = "Shortcut|*.lnk",
                FileName = Shortcut.NameFor(scene),
                InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            };
            if (dlg.ShowDialog() != true) return;
            try { Say("shortcut written: " + Shortcut.Write(CurrentOptions(), dlg.FileName)); }
            catch (Exception e) { Say("could not write the shortcut: " + e.Message); }
        }
    }
}
