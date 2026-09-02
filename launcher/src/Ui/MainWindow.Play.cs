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
    // The families are the ones the rest of the window already uses - violet for what is watched, amber for a
    // briefing, red for the ids that crash, and two weights of grey: dim for a plain stage boot, bright for the
    // two entries that start the game. These same words and colours are the filter chips over the list.
    //
    // Gameplay is still here, and nothing wears it: every numbered section in the stage table crashes, so the
    // catalogue calls them "broken" instead. It stays because the kind can still arrive from scene_info.json,
    // and a section that turns out to boot should look like what it is.
    class Badge
    {
        public string Text, Back, Edge, Ink;
        public Badge(string text, string back, string edge, string ink)
        { Text = text; Back = back; Edge = edge; Ink = ink; }

        public static Badge For(string kind)
        {
            switch (kind)
            {
                case "gameplay":    return new Badge("Gameplay", "#142117", "#2E6B45", "#62C98A");
                case "cutscene":    return new Badge("Cutscene", "#211A33", "#4B3E7A", "#B79CFF");
                case "briefing":    return new Badge("Briefing", "#2A2312", "#7A6220", "#F2C14E");
                case "start":       return new Badge("Start", "#1D1D21", "#4A4A55", "#D2D2DA");
                case "stage-entry": return new Badge("Stage", "#1C1C20", "#3A3A44", "#8A8A94");
                case "broken":      return new Badge("Broken", "#2A1315", "#7A2A2F", "#FF6B66");
                default:            return new Badge(string.IsNullOrEmpty(kind) ? "Scene" : kind,
                                                     "#1C1C20", "#3A3A44", "#8A8A94");
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
        public Brush StarInk { get { return Widgets.Brush(Favourite ? "#F2C14E" : "#4A4A52"); } }
        public string StarTip { get { return Favourite ? "In Favourites - click to remove" : "Add to Favourites"; } }
    }

    partial class MainWindow
    {
        // The chips are the badges, one per kind and in the kind's own colour, plus Favourites - the one category
        // that is not a property of the scene but a list someone curates. Named scenes went with the rename: any
        // scene can have a name now, so it stopped dividing anything.
        public const string FavouritesCat = "Favourites";
        const string BrokenCat = "Broken";
        static readonly string[] CatNames = { FavouritesCat, "Start", "Stage", "Cutscene", "Briefing", BrokenCat };

        // The star's own amber for Favourites; every other chip takes its colour from the badge it names.
        static string CatInk(string name)
        {
            if (name == FavouritesCat) return "#F2C14E";
            foreach (string kind in new[] { "gameplay", "cutscene", "briefing", "start", "stage-entry", "broken" })
            {
                Badge b = Badge.For(kind);
                if (b.Text == name) return b.Ink;
            }
            return "#97979F";
        }

        // Outline while the filter is off, filled while it is on. Done here rather than in the chip's style
        // because the colour is the category's, and a trigger cannot know which category it is drawing.
        static void PaintChip(ToggleButton chip)
        {
            Brush ink = Widgets.Brush(CatInk(chip.Tag.ToString()));
            bool on = chip.IsChecked == true;
            chip.BorderBrush = ink;
            chip.Background = on ? ink : Brushes.Transparent;
            chip.Foreground = on ? Widgets.Brush("#0B0B0C") : ink;
        }

        List<SceneRow> _allRows;

        // Which of the filter chips a scene answers to. Worked out once, so filtering is a set test rather than a
        // predicate run over four hundred rows on every keystroke.
        List<string> CatsOf(Scene e)
        {
            var c = new List<string>();
            if (_favourites.Contains(e.Id)) c.Add(FavouritesCat);
            c.Add(Badge.For(e.Kind).Text);      // the badge it wears is the chip it answers to
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
                chip.Checked += (s, e) => { PaintChip((ToggleButton)s); ApplyFilter(); SavePrefs(); };
                chip.Unchecked += (s, e) => { PaintChip((ToggleButton)s); ApplyFilter(); SavePrefs(); };
                PaintChip(chip);
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
                SavePrefs();
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
            _holdSecs.TextChanged += (s, e) => UpdatePreview();
            _resPick.SelectionChanged += (s, e) => UpdatePreview();
            _altPick.SelectionChanged += (s, e) =>
            {
                string id = _altPick.SelectedItem as string;
                if (!string.IsNullOrEmpty(id)) { _pickedId = id; UpdatePreview(); }
            };

            // The preview is selectable text, so a click in it should not have to fight the caret; the button is
            // there for the common case of wanting the whole line.
            _cmdCopyBtn.Click += (s, e) =>
            {
                try { Clipboard.SetText(_cmdPreview.Text); } catch { }
                _cmdCopyBtn.Content = "Copied";
                var t = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(1.6) };
                t.Tick += (s2, e2) => { _cmdCopyBtn.Content = "Copy"; t.Stop(); };
                t.Start();
            };

            _editBtn.Click += (s, e) => BeginEdit();
            _editCancelBtn.Click += (s, e) => EndEdit();
            _editSaveBtn.Click += (s, e) => CommitEdit(false);
            _editResetBtn.Click += (s, e) => CommitEdit(true);
            // Enter in the name box saves, rather than reaching the window's own Enter and launching the game.
            _editName.KeyDown += (s, e) =>
            {
                if (e.Key != Key.Enter) return;
                e.Handled = true;
                CommitEdit(false);
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
                _pickedId = entry.Id;
                // On a first run the scene's act is opened so it can be seen. After that the tab comes back as it
                // was left - acts however they were left, chips still ticked - because a filter someone chose is
                // theirs to keep, and clearing it to show one row is not a trade the window gets to make.
                if (!_hadSavedActs) _collapsed[entry.ActKey] = false;
            }
            ApplyFilter();
            if (entry != null)
            {
                // The panel shows it either way; the list can only select it when the filters and the act it is in
                // leave it on screen.
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
            if (!picked.Contains(BrokenCat)) rows = rows.Where(r => !r.Hidden);
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
            EndEdit();          // the boxes held the last scene's name; a new pick is not an edit of it
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

        // ------------------------------------------------------------------------------- renaming a scene

        // The catalogue's names come off the stage table and labels.json, and most scenes have none: what a scene
        // actually is only shows once it has been booted and watched. Whatever is typed here is kept in the
        // preferences file under the scene's id, so it outlives any rebuild of the data files.
        void BeginEdit()
        {
            Scene scene = Catalogue.Find(_pickedId);
            if (scene == null) { Say("pick a scene first"); return; }
            _editName.Text = scene.Name ?? "";
            _editDesc.Text = scene.Description ?? "";
            _editPanel.Visibility = Visibility.Visible;
            _editBtn.Visibility = Visibility.Collapsed;
            _editName.Focus();
            _editName.SelectAll();
        }

        void EndEdit()
        {
            _editPanel.Visibility = Visibility.Collapsed;
            _editBtn.Visibility = Visibility.Visible;
        }

        // Save writes what is in the boxes; Reset drops the entry and lets the data files speak again. Either way
        // the Scene object is updated in place, so the list row, the details panel and a shortcut made afterwards
        // all say the same thing without rebuilding the catalogue.
        void CommitEdit(bool reset)
        {
            Scene scene = Catalogue.Find(_pickedId);
            if (scene == null) return;
            if (reset)
            {
                _sceneEdits.Remove(scene.Id);
                scene.Name = scene.BaseName;
                scene.Description = scene.BaseDescription;
                Say(scene.Id + " back to the name the catalogue gives it");
            }
            else
            {
                string name = _editName.Text.Trim(), desc = _editDesc.Text.Trim();
                var edit = new Prefs.SceneEdit
                {
                    Name = name == (scene.BaseName ?? "") ? null : name,
                    Description = desc == (scene.BaseDescription ?? "") ? null : desc,
                };
                if (edit.Name == null && edit.Description == null) _sceneEdits.Remove(scene.Id);
                else _sceneEdits[scene.Id] = edit;
                scene.Name = name;
                scene.Description = desc;
                Say(_sceneEdits.ContainsKey(scene.Id)
                    ? scene.Id + " renamed - kept in " + Prefs.Path
                    : scene.Id + " is as the catalogue has it");
            }

            foreach (SceneRow r in _allRows)
            {
                if (r.Entry != scene) continue;
                r.Title = string.IsNullOrEmpty(scene.Name) ? scene.Id : scene.Name;
                r.Sub = string.IsNullOrEmpty(scene.Description) ? scene.Note : scene.Description;
                r.Hay = (scene.Id + " " + string.Join(" ", scene.Alts) + " " + scene.Name + " " +
                         scene.ActTitle + " " + scene.Kind + " " + scene.Description).ToLowerInvariant();
            }
            SavePrefs();
            EndEdit();
            ApplyFilter();      // the rows are rebuilt from _allRows, which is what makes the new name show
            ShowPicked(scene);
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
            if (_optRes.IsChecked == true)
            {
                string[] wh = PickedRes().Split('x');
                int w, h;
                if (wh.Length == 2 && int.TryParse(wh[0], out w) && int.TryParse(wh[1], out h)) { o.Width = w; o.Height = h; }
            }
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
