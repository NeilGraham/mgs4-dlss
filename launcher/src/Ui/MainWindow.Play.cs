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
    // What a scene is, as a badge: a short word and a color of its own, so the kind reads before the name does.
    // The families are the ones the rest of the window already uses - violet for what is watched, amber for a
    // briefing, red for the ids that crash, green for the entries that start the game and blue for a codec call
    // stage boot. These same words and colors are the filter chips over the list.
    //
    // Gameplay is well populated now: the sweep boots every id and most numbered sections turn out to work, so
    // the old blanket "broken" is gone and each scene wears what it was measured to be.
    //
    // Codec took the cyan that Stage used to have - Stage is gone, because what a bare stage id shows is gameplay
    // or a cutscene like anything else. Boss is orange: it has to sit beside Gameplay green without reading as
    // Broken red. Gray is left to the fallback below, which is what an unrecognized kind should look like.
    //
    // Boss is measured too, since 2026-09-03: the name on the second health bar is read by OCR
    // (tools\hud_read.py).
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
                case "start":       return new Badge("Start", "#1E2410", "#557F26", "#9BE04F");
                case "codec":       return new Badge("Codec", "#0F2328", "#22697A", "#4ED2E8");
                case "boss":        return new Badge("Boss", "#2A1A10", "#8A4A1E", "#FFA24E");
                case "video":       return new Badge("Video", "#2A1026", "#7A2A6E", "#E88ADA");
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

        // The frame the sweep grabbed of this scene, at the width the row draws it. Bound straight from the
        // template, so a scene the sweep never reached (or a build with no thumbnails in it) yields null and the
        // Image simply draws nothing - the placeholder behind it keeps the column aligned either way.
        // Decoded at the physical size they are drawn on a 4K screen at 200%: the row banner is 128 logical px
        // wide, the pane 480 - a decode at the logical width was being stretched two to one.
        public ImageSource Thumb { get { return IsHeader ? null : Thumbs.Get(Id, 256); } }
        public string ThumbVis { get { return !IsHeader && Thumbs.Has(Id) ? "Visible" : "Hidden"; } }

        // A filled star for a favorite, an outline for the rest. Both are one character wide, so the column does
        // not shift as rows are starred.
        public bool Favorite { get; set; }
        public string Star { get { return Favorite ? "\u2605" : "\u2606"; } }
        public Brush StarInk { get { return Widgets.Brush(Favorite ? "#F2C14E" : "#4A4A52"); } }
        public string StarTip { get { return Favorite ? "In Favorites - click to remove" : "Add to Favorites"; } }
    }

    partial class MainWindow
    {
        // The chips are the badges, one per kind and in the kind's own color, plus Favorites - the one category
        // that is not a property of the scene but a list someone curates. Named scenes went with the rename: any
        // scene can have a name now, so it stopped dividing anything.
        public const string FavoritesCat = "Favorites";
        const string BrokenCat = "Broken";
        static readonly string[] CatNames = { FavoritesCat, "Start", "Gameplay", "Boss", "Cutscene", "Video", "Codec", "Briefing", BrokenCat };

        // Every chip is the badge it names. Favorites names a list rather than a kind, so it has no badge of its
        // own and takes the briefing one's, which is already the star's amber.
        static Badge CatBadge(string name)
        {
            if (name == FavoritesCat) return Badge.For("briefing");
            foreach (string kind in new[] { "gameplay", "cutscene", "briefing", "start", "codec", "boss", "video", "broken" })
            {
                Badge b = Badge.For(kind);
                if (b.Text == name) return b;
            }
            return Badge.For("");
        }

        // On, the chip is its badge: the badge's outline, its ink, and its fill lifted a fifth of the way towards
        // that ink. The badge's own fill is 1.1:1 against the card both of them sit on - which is fine for a badge,
        // because a badge is identified by being there at all, and useless for a chip, which has to say on or off
        // in the same place either way. A fifth up puts the fill at 1.4-1.9:1, seen at a glance without shouting,
        // and still holds the ink at 4.5:1 on top of it.
        //
        // Off, the fill goes and what is left steps back: the same outline, and the ink walked a third of the way
        // down to the fill it would have sat on. The outline's own color as the text would be truer to the badge,
        // but Stage outlines at 1.6:1 against the card and Cutscene at 2.0:1, which at 11px is not text any more;
        // a third down holds every chip at 3:1 or better while still reading as the quiet one. Done here and not
        // in the style because the color is the category's, and a trigger cannot know which one it is drawing.
        static void PaintChip(ToggleButton chip)
        {
            Badge b = CatBadge(chip.Tag.ToString());
            bool on = chip.IsChecked == true;
            chip.BorderBrush = Widgets.Brush(b.Edge);
            chip.Background = on ? Widgets.Mix(b.Back, b.Ink, 0.20) : Brushes.Transparent;
            chip.Foreground = on ? Widgets.Brush(b.Ink) : Widgets.Mix(b.Ink, b.Back, 0.35);
        }

        List<SceneRow> _allRows;

        // Which of the filter chips a scene answers to. Worked out once, so filtering is a set test rather than a
        // predicate run over four hundred rows on every keystroke.
        List<string> CatsOf(Scene e)
        {
            var c = new List<string>();
            if (_favorites.Contains(e.Id)) c.Add(FavoritesCat);
            c.Add(Badge.For(e.Kind).Text);      // the badge it wears is the chip it answers to
            return c;
        }

        // The line under a title: where the stage is, from the game's own banner, then what the scene is.
        static string SubOf(Scene e)
        {
            string what = string.IsNullOrEmpty(e.Description) ? e.Note : e.Description;
            return string.IsNullOrEmpty(e.Location) ? what : e.Location + "  \u00b7  " + what;
        }

        static string HayOf(Scene e)
        {
            return (e.Id + " " + string.Join(" ", e.Alts) + " " + e.Name + " " + e.ActTitle + " " + e.Kind + " " +
                    e.Location + " " + e.Description).ToLowerInvariant();
        }

        void WirePlay()
        {
            _allRows = Catalog.All().Select(e =>
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
                    Sub = SubOf(e),
                    Cats = CatsOf(e),
                    Hay = HayOf(e),
                    BadgeText = badge.Text,
                    BadgeBack = Widgets.Brush(badge.Back),
                    BadgeEdge = Widgets.Brush(badge.Edge),
                    BadgeInk = Widgets.Brush(badge.Ink),
                    Favorite = _favorites.Contains(e.Id),
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

                // The star is its own click: toggling a favorite must not also pick the scene.
                if (!row.IsHeader && HitTheStar(e.OriginalSource))
                {
                    ToggleFavorite(row.Id);
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
            foreach (string key in Catalog.ActOrder) _collapsed[key] = true;
            ApplyFilter();
        }

        // A scene asked for on the command line, or the last one used, opens its act and is selected in it. Called
        // after the preferences are read, because that is where the last one comes from.
        void RestoreSelection()
        {
            Scene entry = Catalog.Find(_pickedId);
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

        void ToggleFavorite(string id)
        {
            if (!_favorites.Remove(id)) _favorites.Add(id);
            foreach (SceneRow r in _allRows)
            {
                if (r.Id != id) continue;
                r.Favorite = _favorites.Contains(id);
                r.Cats = CatsOf(r.Entry);
            }
            SavePrefs();            // a starred scene should still be starred if the window is closed on the spot
            ApplyFilter();
            Say(_favorites.Contains(id)
                ? id + " added to Favorites (" + _favorites.Count + ")"
                : id + " removed from Favorites (" + _favorites.Count + ")");
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
            foreach (string key in Catalog.ActOrder)
            {
                var inAct = list.Where(r => r.ActKey == key).ToList();
                if (inAct.Count == 0) continue;
                bool collapsed = !searching && _collapsed.ContainsKey(key) && _collapsed[key];
                outRows.Add(new SceneRow
                {
                    IsHeader = true,
                    ActKey = key,
                    Chevron = collapsed ? "▶" : "▼",
                    HeadText = Catalog.ActTitles.ContainsKey(key) ? Catalog.ActTitles[key] : key,
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
            // How many entries there are is the list's own line, and the list is the half of the tab that is not
            // showing when the simple view is on - which is also the half whose count nobody there asked for.
            if (_simplePlay)
                Say(string.IsNullOrEmpty(_gameDir)
                    ? "no MGS4 install found - the Setup tab says what was looked for"
                    : "MGS4 in " + _gameDir);
            else
                Say(list.Count + " of " + _allRows.Count + " entries" +
                    (picked.Count > 0 ? " - " + string.Join(", ", picked) : ""));
        }

        void ShowPicked(Scene scene)
        {
            if (scene == null) return;
            EndEdit();          // the boxes held the last scene's name; a new pick is not an edit of it
            _pickTitle.Text = string.IsNullOrEmpty(scene.Name) ? scene.Id : scene.Name;
            _pickSub.Text = SubOf(scene);

            // Decoded wider here than for a row: this one is drawn at a few hundred pixels, and asking for the
            // row's width would put a 128px image up at panel size.
            System.Windows.Media.ImageSource shot = Thumbs.Get(scene.Id, 960);
            _pickShot.Fill = shot == null ? null : new System.Windows.Media.ImageBrush(shot)
            {
                Stretch = System.Windows.Media.Stretch.Uniform,         // the whole frame; the box is 16:9 to fit it
            };
            _pickShotBox.Visibility = shot != null ? Visibility.Visible : Visibility.Collapsed;

            // A menu entry takes none of the run options: there are no boot prompts to press through, and a press
            // on MGS4's main menu picks an entry rather than getting past anything. The title-screen boot is a
            // start entry too and is not a menu - it is the game starting itself, prompts and all - so it keeps
            // the one option that means something there, which is what the runner will act on.
            bool menu = Catalog.IsMenuEntry(scene.Id);
            bool bootSequence = scene.Kind == "start" && !menu;
            _optAdvance.IsEnabled = !menu;
            foreach (Control c in new Control[] { _optMashX, _optEnd, _optHold, _holdSecs })
                c.IsEnabled = !menu && !bootSequence;
            _pickWarn.Text = scene.Hidden ? "This id is in the known-broken list: it crashes or comes up black." : "";
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

            // The other view shows the same pick as a highlighted card and one checkbox; both are cheap, and doing
            // them here means every route to a new pick - a card, a row, the command line, a restored preference -
            // leaves the two views agreeing.
            if (_simplePlay) { PaintStartTiles(); UpdateStartAdvance(); }
            UpdatePreview();
        }

        // ------------------------------------------------------------------------------- renaming a scene

        // The catalog's names come off scene_info.json, and a scene the sweep never named has none: what a scene
        // actually is only shows once it has been booted and watched. Whatever is typed here is kept in the
        // preferences file under the scene's id, so it outlives any rebuild of the data files.
        void BeginEdit()
        {
            Scene scene = Catalog.Find(_pickedId);
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
        // all say the same thing without rebuilding the catalog.
        void CommitEdit(bool reset)
        {
            Scene scene = Catalog.Find(_pickedId);
            if (scene == null) return;
            if (reset)
            {
                _sceneEdits.Remove(scene.Id);
                scene.Name = scene.BaseName;
                scene.Description = scene.BaseDescription;
                Say(scene.Id + " back to the name the catalog gives it");
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
                    : scene.Id + " is as the catalog has it");
            }

            foreach (SceneRow r in _allRows)
            {
                if (r.Entry != scene) continue;
                r.Title = string.IsNullOrEmpty(scene.Name) ? scene.Id : scene.Name;
                r.Sub = SubOf(scene);
                r.Hay = HayOf(scene);
            }
            SavePrefs();
            EndEdit();
            ApplyFilter();      // the rows are rebuilt from _allRows, which is what makes the new name show
            ShowPicked(scene);
        }

        Options CurrentOptions()
        {
            // The simple view carries one option and no scene machinery: a hold, a resolution or a close-on-
            // gameplay left ticked in the other view has no business riding along with "start the game".
            if (_simplePlay)
                return new Options
                {
                    Stage = _pickedId,
                    Advance = _startAdvance.IsChecked == true,
                    KeepRunning = false,
                    GameDir = _gameDir,
                    GameDirGiven = _opt.GameDirGiven,
                };

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
                StopMusic();        // the game is about to have the speakers; do not wait for the next poll
                Say("launching: " + Options.Preview(CurrentOptions().ToCli()));
            }
            catch (Exception e) { Say("could not launch: " + e.Message); }
            RefreshState();
        }

        void MakeShortcut()
        {
            Scene scene = Catalog.Find(_pickedId);
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
