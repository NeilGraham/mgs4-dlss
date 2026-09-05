// The Settings tab: MGS4\mgs4_dlss.ini as a form, each row naming its key and what it does. Saving is blocked
// while the game is running, because the add-on owns that file then - its writes go through the Windows profile
// API, whose cache will quietly undo an outside edit.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        // One entry per row: the key it belongs to (which knows its file), how to read the control back, and what
        // the file said when the form was built. The third is what makes "has anything changed?" answerable.
        class Binding
        {
            public IniKey Spec;
            public Func<string> Read;
            public Action<string> Write;    // put a value from the file back into the control, for a refresh
            public string Original;
        }
        readonly List<Binding> _settingReaders = new List<Binding>();

        // Rows with nothing to edit: they show a value and are never saved, so they are not bindings - but they
        // do go stale, so a refresh has to be able to reach them.
        readonly List<KeyValuePair<IniKey, TextBlock>> _settingLabels = new List<KeyValuePair<IniKey, TextBlock>>();

        string _settingsDir;        // the folder the form was built against; a different one means rebuild
        string _settingsShape;      // which groups had no file, which is what decides the cards themselves
        bool _settingsBusy;

        // The rail beside the form, and what the search over it filters: every card with its rows, each row
        // carrying the words a search can find it by.
        Rail _settingsRail;
        class SettingsCard
        {
            public Border Card;
            public string Group;
            public bool Missing;
            public List<KeyValuePair<Border, string>> Rows = new List<KeyValuePair<Border, string>>();
            public List<Binding> Bindings = new List<Binding>();
        }
        readonly List<SettingsCard> _settingsCards = new List<SettingsCard>();

        // Save is for writing changes, so it is only offered when there are any. Recomputed on every edit, and
        // reset by building the form, reloading, and saving - each of which makes the controls agree with the
        // files again.
        bool Changed()
        {
            foreach (Binding b in _settingReaders)
                if (!string.Equals(b.Read(), b.Original, StringComparison.Ordinal)) return true;
            return false;
        }

        void UpdateSaveButton()
        {
            bool changed = Changed();
            bool running = _gameUp;
            _saveBtn.IsEnabled = changed && !running && !string.IsNullOrEmpty(_gameDir);
            _saveBtn.ToolTip = string.IsNullOrEmpty(_gameDir) ? "No game folder"
                             : running ? "The game is running - close it to write these files"
                             : changed ? "Write the changed settings to their files"
                             : "Nothing has been changed yet";
            MarkEditedGroups();
        }

        // The rail's dot for a settings group says what the card's tag says - amber for a group whose file is not
        // there - and, over that, the accent for a group holding an edit not yet saved: the one thing on this
        // page that matters and that a folded card would otherwise hide.
        void MarkEditedGroups()
        {
            if (_settingsRail == null) return;
            foreach (SettingsCard c in _settingsCards)
            {
                bool edited = false;
                foreach (Binding b in c.Bindings)
                    if (!string.Equals(b.Read(), b.Original, StringComparison.Ordinal)) { edited = true; break; }
                _settingsRail.Dot(c.Card, edited ? "#7C9CFF" : c.Missing ? "#F2C14E" : null);
            }
        }

        void WireSettings()
        {
            _reloadBtn.Click += (s, e) => { BuildSettings(); Say("reloaded from mgs4_dlss.ini"); };
            _saveBtn.Click += (s, e) => SaveSettings();

            _settingsRail = new Rail(_settingsRailSlot, _settingsScroll, _settingsHost, "Find a setting");
            _settingsRail.Search.TextChanged += (s, e) => ApplySettingsFilter();
            _settingsRail.CollapseAll = () => { foreach (SettingsCard c in _settingsCards) Widgets.Fold(c.Card, true); SavePrefs(); };
            _settingsRail.ExpandAll = () => { foreach (SettingsCard c in _settingsCards) Widgets.Fold(c.Card, false); SavePrefs(); };
        }

        // The search over the form: a row stays when its label, its key or its help holds every word typed, a
        // card stays while any of its rows do, and a card with a match is opened whatever its fold says - what
        // was asked for should be on screen, not behind a header.
        void ApplySettingsFilter()
        {
            string[] words = SearchWords(_settingsRail.Search.Text);
            int shown = 0, total = 0;
            foreach (SettingsCard c in _settingsCards)
            {
                int hits = 0;
                foreach (KeyValuePair<Border, string> row in c.Rows)
                {
                    bool hit = Matches(row.Value, words);
                    row.Key.Visibility = hit ? Visibility.Visible : Visibility.Collapsed;
                    if (hit) hits++;
                    total++;
                }
                shown += hits;
                bool keep = words.Length == 0 || hits > 0 || (c.Rows.Count == 0 && Matches(c.Group, words));
                c.Card.Visibility = keep ? Visibility.Visible : Visibility.Collapsed;
                Widgets.Reveal(c.Card, words.Length > 0);
                _settingsRail.Shown(c.Card, keep);
            }
            if (words.Length > 0) Say(shown + " of " + total + " settings match");
        }

        static string[] SearchWords(string text)
        {
            return (text ?? "").ToLowerInvariant().Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
        }

        static bool Matches(string hay, string[] words)
        {
            if (words.Length == 0) return true;
            string h = (hay ?? "").ToLowerInvariant();
            foreach (string w in words) if (!h.Contains(w)) return false;
            return true;
        }

        // Opening the tab shows the form that is already there and re-reads the files behind it. The reading is
        // cheap - four milliseconds for all forty-three keys - but building the controls is not, and that is what
        // made the tab take a visible moment every time it was opened. So the controls are built once and kept,
        // and a refresh moves only the ones whose file actually says something different now.
        void ShowSettings()
        {
            bool built = _settingReaders.Count > 0
                      && string.Equals(_settingsDir ?? "", _gameDir ?? "", StringComparison.OrdinalIgnoreCase);
            if (!built) BuildSettings();
            StartSettingsRefresh();
        }

        // Which groups have no file to read, which is what decides whether a card gets rows or a "not there" note.
        // A change here is a change to the cards, and that is the one thing a refresh cannot patch in place.
        static bool GroupMissing(IEnumerable<IniKey> keys, string game)
        {
            foreach (IniKey spec in keys)
            {
                if (spec.Source == IniSource.Launcher) continue;
                string file = IniForm.PathFor(spec.Source, game);
                if (string.IsNullOrEmpty(file) || !Paths.Exists(file)) return true;
            }
            return false;
        }

        static string SettingsShape(string game)
        {
            var sb = new System.Text.StringBuilder();
            foreach (var group in IniForm.Spec.GroupBy(k => k.Group))
                sb.Append(group.Key).Append(GroupMissing(group, game) ? ":-|" : ":+|");
            return sb.ToString();
        }

        static string SpecId(IniKey k)
        {
            return ((int)k.Source) + "/" + (k.Section ?? "") + "/" + k.Key;
        }

        // The files are read off the window's thread: nothing here touches a control, and the answers are handed
        // back to be applied where controls may be touched.
        void StartSettingsRefresh()
        {
            if (_settingsBusy || _settingReaders.Count == 0 || string.IsNullOrEmpty(_gameDir)) return;
            _settingsBusy = true;
            string dir = _gameDir;
            var specs = new List<IniKey>();
            foreach (Binding b in _settingReaders) specs.Add(b.Spec);
            foreach (KeyValuePair<IniKey, TextBlock> kv in _settingLabels) specs.Add(kv.Key);

            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                Dictionary<string, string> fresh = null;
                string shape = null;
                try
                {
                    var read = new Dictionary<string, string>();
                    string addonIni = IniForm.IniPath(dir), gameIni = IniForm.PathFor(IniSource.Game, dir);
                    foreach (IniKey k in specs) read[SpecId(k)] = IniForm.Read(k, addonIni, gameIni);
                    shape = SettingsShape(dir);
                    fresh = read;
                }
                catch { fresh = null; }
                Win.Dispatcher.BeginInvoke(new Action(delegate { EndSettingsRefresh(dir, fresh, shape); }));
            });
        }

        void EndSettingsRefresh(string dir, Dictionary<string, string> fresh, string shape)
        {
            _settingsBusy = false;
            if (fresh == null) return;
            if (!string.Equals(dir ?? "", _gameDir ?? "", StringComparison.OrdinalIgnoreCase)) return;
            if (shape != _settingsShape) { BuildSettings(); return; }   // a file arrived or went; the cards change

            int moved = 0;
            foreach (Binding b in _settingReaders)
            {
                string v;
                if (!fresh.TryGetValue(SpecId(b.Spec), out v)) continue;
                if (string.Equals(v, b.Original, StringComparison.Ordinal)) continue;   // the file has not moved
                if (!string.Equals(b.Read(), b.Original, StringComparison.Ordinal)) continue;   // but the user has
                b.Original = v;                 // set first, so the control's own change event sees no edit
                if (b.Write != null) b.Write(v);
                // Then what the control reads back, not the file's spelling: the game writes True where the
                // form reads true, and with the raw value kept as the baseline every refresh from a running game
                // left Save lit over a form nobody had touched.
                b.Original = b.Read();
                moved++;
            }
            foreach (KeyValuePair<IniKey, TextBlock> kv in _settingLabels)
            {
                string v;
                if (!fresh.TryGetValue(SpecId(kv.Key), out v)) continue;
                string text = string.IsNullOrEmpty(v) ? "(not written yet)" : v;
                if (kv.Value.Text != text) { kv.Value.Text = text; moved++; }
            }
            UpdateSaveButton();
            if (moved > 0) Say(moved == 1 ? "1 setting changed on disk and was refreshed"
                                          : moved + " settings changed on disk and were refreshed");
        }

        void BuildSettings()
        {
            _settingsHost.Children.Clear();
            _settingReaders.Clear();
            _settingLabels.Clear();
            _settingsCards.Clear();
            if (_settingsRail != null) _settingsRail.Clear();
            _settingsDir = _gameDir;
            _settingsShape = SettingsShape(_gameDir);

            bool running = _gameUp;
            _saveBtn.IsEnabled = !running && !string.IsNullOrEmpty(_gameDir);
            if (string.IsNullOrEmpty(_gameDir))
            {
                _lockText.Text = "No MGS4 install found, so there is no mgs4_dlss.ini to read or write. The Setup tab says what was looked for.";
                _lockBanner.Visibility = Visibility.Visible;
                return;
            }
            if (running)
            {
                _lockText.Text = "The game is running. It rewrites mgs4_dlss.ini through the Windows profile API, whose cache would undo anything written from here - close the game to save. Most of these keys are read again every second by the add-on, and its own overlay (ReShade, Add-ons tab) can change them live.";
                _lockBanner.Visibility = Visibility.Visible;
            }
            else _lockBanner.Visibility = Visibility.Collapsed;

            string addonIni = IniForm.IniPath(_gameDir);
            string gameIni = IniForm.PathFor(IniSource.Game, _gameDir);
            IniForm.MusicChoices(_gameDir);     // the track list is this install's, so it is read before the rows

            // Grouped first, so a card can say every file its rows write - Display writes two of them.
            foreach (var group in IniForm.Spec.GroupBy(k => k.Group))
            {
                var keys = group.ToList();
                var files = new List<string>();
                foreach (IniKey spec in keys)
                {
                    string label = IniForm.SourceLabel(spec.Source);
                    if (!files.Contains(label)) files.Add(label);
                }
                bool missing = GroupMissing(keys, _gameDir);
                string blurb = string.Join(", ", files);
                if (missing)
                    blurb += group.Key == "Neural Rendering"
                        ? " - not there; the DLSS 5 add-on writes these once it has run"
                        : " - not there yet; run the game once and it writes them";

                StackPanel body;
                Border card = Widgets.Card(group.Key, blurb, missing ? "warn" : "info",
                                           missing ? "not there" : null,
                                           IniForm.BadgesFor(keys[0]), "settings:" + group.Key, out body);
                _settingsHost.Children.Add(card);
                var info = new SettingsCard { Card = card, Group = group.Key, Missing = missing };
                _settingsCards.Add(info);
                if (_settingsRail != null)
                    _settingsRail.Add(group.Key, missing ? "#F2C14E" : null, card,
                                      missing ? group.Key + " - " + blurb : string.Join(", ", files));
                if (missing) continue;
                foreach (IniKey spec in keys)
                {
                    int before = _settingReaders.Count;
                    Border row = SettingRow(spec, IniForm.Read(spec, addonIni, gameIni));
                    body.Children.Add(row);
                    info.Rows.Add(new KeyValuePair<Border, string>(row, group.Key + " " + spec.Label + " " + spec.Key + " " + spec.Help));
                    for (int i = before; i < _settingReaders.Count; i++) info.Bindings.Add(_settingReaders[i]);
                }
                if (group.Key == "Launcher")
                {
                    Border row = PlaylistRow();
                    body.Children.Add(row);
                    info.Rows.Add(new KeyValuePair<Border, string>(row, "Launcher menu music playlist tracks ipod"));
                    row = DecoderRow();
                    body.Children.Add(row);
                    info.Rows.Add(new KeyValuePair<Border, string>(row, "Launcher menu music decoder vgmstream"));
                }
            }

            // Building the form gives one of its controls focus, and WPF brings a focused control into view - so
            // the tab opened part-way down its own first card. Start at the top, where the reading starts.
            _settingsScroll.ScrollToTop();
            if (_settingsRail != null && _settingsRail.Search.Text.Length > 0) ApplySettingsFilter();
            UpdateSaveButton();
        }

        Border SettingRow(IniKey spec, string value)
        {
            var b = new Border
            {
                Padding = new Thickness(18, 8, 18, 8),
                BorderBrush = Widgets.Brush("#202023"),
                BorderThickness = new Thickness(0, 1, 0, 0),
            };
            Grid g = Widgets.Columns("*", "Auto");
            var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            left.Children.Add(Widgets.Text(spec.Label, 12, "#ECECEE"));
            // The key and its help on one line under the label. The help used to run off the right edge and be
            // cut mid-letter by the editor beside it; it is trimmed to an ellipsis now, with the whole of it in
            // the tooltip - and the row is a line shorter than wrapping it would make it.
            Grid sub = Widgets.Columns("Auto", "*");
            sub.Margin = new Thickness(0, 1, 12, 0);
            TextBlock key = Widgets.Text(spec.Key, 11, "#7C9CFF", false, true);
            key.VerticalAlignment = VerticalAlignment.Center;
            sub.Children.Add(key);
            TextBlock help = Widgets.Text(spec.Help, 11, "#97979F");
            help.Margin = new Thickness(8, 0, 0, 0);
            help.TextWrapping = TextWrapping.NoWrap;
            help.TextTrimming = TextTrimming.CharacterEllipsis;
            help.ToolTip = spec.Help;
            Grid.SetColumn(help, 1);
            sub.Children.Add(help);
            left.Children.Add(sub);
            g.Children.Add(left);

            FrameworkElement editor;
            switch (spec.Type)
            {
                case "bool":
                {
                    var cb = new CheckBox
                    {
                        IsChecked = string.Equals(value, spec.TrueWord, StringComparison.OrdinalIgnoreCase),
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    Remember(spec, () => cb.IsChecked == true ? spec.TrueWord : spec.FalseWord,
                             v => cb.IsChecked = string.Equals(v, spec.TrueWord, StringComparison.OrdinalIgnoreCase));
                    cb.Checked += (s2, e2) => UpdateSaveButton();
                    cb.Unchecked += (s2, e2) => UpdateSaveButton();
                    editor = cb;
                    break;
                }
                case "choice":
                {
                    var combo = new ComboBox { MinWidth = 190, VerticalAlignment = VerticalAlignment.Center };
                    for (int i = 0; i < spec.Choices.Length; i++)
                        combo.Items.Add(spec.ChoiceLabels != null && i < spec.ChoiceLabels.Length
                                        ? spec.ChoiceLabels[i] : spec.Choices[i]);
                    int idx = Array.IndexOf(spec.Choices, value ?? "");
                    combo.SelectedIndex = idx >= 0 ? idx : -1;
                    // What the file said, for a value that is none of the choices: the control cannot hold it, so
                    // the reader hands it back unchanged and a refresh has to move it along with the selection.
                    string held = value;
                    Remember(spec, () => combo.SelectedIndex >= 0 ? spec.Choices[combo.SelectedIndex] : held,
                             v => { held = v; combo.SelectedIndex = Array.IndexOf(spec.Choices, v ?? ""); });
                    combo.SelectionChanged += (s2, e2) => UpdateSaveButton();
                    editor = combo;
                    break;
                }
                case "readonly":
                {
                    TextBlock t = Widgets.Text(string.IsNullOrEmpty(value) ? "(not written yet)" : value, 12, "#97979F", false, true);
                    t.VerticalAlignment = VerticalAlignment.Center;
                    _settingLabels.Add(new KeyValuePair<IniKey, TextBlock>(spec, t));
                    editor = t;
                    break;
                }
                default:
                {
                    if (spec.Type == "int" && !double.IsNaN(spec.Min) && !double.IsNaN(spec.Max))
                    {
                        // A number in a known range: a slider, with the number beside it in a box that still
                        // takes typing. The two are kept level from either side, and the box is what is read.
                        var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
                        var slider = new Slider
                        {
                            Minimum = spec.Min, Maximum = spec.Max,
                            SmallChange = spec.Step, LargeChange = spec.Step * 5, TickFrequency = spec.Step,
                            IsSnapToTickEnabled = true, VerticalAlignment = VerticalAlignment.Center,
                            Margin = new Thickness(0, 0, 10, 0),
                        };
                        var box = new TextBox { Width = 58, VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right };
                        double v0;
                        if (!double.TryParse(value, out v0)) v0 = spec.Min;
                        slider.Value = Math.Max(spec.Min, Math.Min(spec.Max, v0));
                        box.Text = value ?? "";
                        bool syncing = false;
                        slider.ValueChanged += (s2, e2) =>
                        {
                            if (syncing) return;
                            syncing = true; box.Text = ((int)Math.Round(slider.Value)).ToString(); syncing = false;
                            UpdateSaveButton();
                        };
                        box.TextChanged += (s2, e2) =>
                        {
                            double v;
                            if (!syncing && double.TryParse(box.Text, out v))
                            { syncing = true; slider.Value = Math.Max(spec.Min, Math.Min(spec.Max, v)); syncing = false; }
                            UpdateSaveButton();
                        };
                        Remember(spec, () => box.Text.Trim(), v =>
                        {
                            box.Text = v ?? "";
                            double d; if (double.TryParse(v, out d)) { syncing = true; slider.Value = Math.Max(spec.Min, Math.Min(spec.Max, d)); syncing = false; }
                        });
                        panel.Children.Add(slider);
                        panel.Children.Add(box);
                        Grid.SetColumn(panel, 1);
                        g.Children.Add(panel);
                        b.Child = g;
                        b.Tag = new Widgets.RowInfo { Editor = slider, Spec = spec };
                        return b;
                    }
                    var tb = new TextBox { Text = value ?? "", MinWidth = 90, VerticalAlignment = VerticalAlignment.Center };
                    Remember(spec, () => tb.Text.Trim(), v => tb.Text = v ?? "");
                    tb.TextChanged += (s2, e2) => UpdateSaveButton();
                    editor = tb;
                    break;
                }
            }
            Grid.SetColumn(editor, 1);
            g.Children.Add(editor);
            b.Child = g;
            if (spec.Type != "readonly") b.Tag = new Widgets.RowInfo { Editor = editor, Spec = spec };
            return b;
        }

        // Called as each row is built, with the control already showing the file's value - so reading it back now
        // is exactly what the file says, and anything different later is an edit.
        void Remember(IniKey spec, Func<string> read, Action<string> write)
        {
            _settingReaders.Add(new Binding { Spec = spec, Read = read, Write = write, Original = read() });
        }

        // Both files at once, each row going to the one its key lives in. The game owns mgs4.savedsettings while it
        // runs and the add-on owns mgs4_dlss.ini, so neither is written until it is closed.
        void SaveSettings()
        {
            if (Checks.GameRunning()) { Say("close the game first - it owns both of these while it runs"); return; }
            var written = new List<string>();
            foreach (IniSource source in new[] { IniSource.Addon, IniSource.Game, IniSource.Launcher, IniSource.Renodx })
            {
                var values = new List<KeyValuePair<string, string>>();
                string section = null;
                foreach (Binding row in _settingReaders)
                {
                    if (row.Spec.Source != source) continue;
                    string v = row.Read();
                    if (v == null) continue;
                    section = row.Spec.Section;
                    values.Add(new KeyValuePair<string, string>(row.Spec.Key, v));
                }
                if (values.Count == 0) continue;
                string file = IniForm.PathFor(source, _gameDir);
                if (string.IsNullOrEmpty(file)) continue;
                if (source == IniSource.Launcher) file = Paths.EnsureConfig();
                if (!Paths.Exists(file)) continue;
                try
                {
                    Checks.SetIni(file, values, section);
                    written.Add(values.Count + " to " + IniForm.SourceLabel(source));
                }
                catch (Exception e) { Say("could not write " + IniForm.SourceLabel(source) + ": " + e.Message); return; }
            }
            foreach (Binding b in _settingReaders) b.Original = b.Read();   // what is on screen is what is on disk
            // config.ini was read once and kept, and some of what was just written lives in it - the Play tab's
            // own view among it, which has to change on the spot rather than at the next start.
            Paths.ForgetConfig();
            ApplyPlayView(true);
            ApplySetupTab();
            StopMusic();            // the track or the volume may have just changed; ApplyMusic picks the new one up
            ApplyMusic();
            UpdateSaveButton();
            Say(written.Count > 0 ? "written: " + string.Join(", ", written) : "nothing to write");
            StartSetupRefresh();      // the Setup tab's view of the game's settings just changed
        }
    }
}
