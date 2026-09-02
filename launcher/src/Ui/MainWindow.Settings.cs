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
        }

        void WireSettings()
        {
            _reloadBtn.Click += (s, e) => { BuildSettings(); Say("reloaded from mgs4_dlss.ini"); };
            _saveBtn.Click += (s, e) => SaveSettings();
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
                _settingsHost.Children.Add(Widgets.Card(group.Key, blurb, missing ? "warn" : "info",
                                                        missing ? "not there" : null,
                                                        IniForm.BadgesFor(keys[0]), out body));
                if (missing) continue;
                foreach (IniKey spec in keys)
                    body.Children.Add(SettingRow(spec, IniForm.Read(spec, addonIni, gameIni)));
            }

            // Building the form gives one of its controls focus, and WPF brings a focused control into view - so
            // the tab opened part-way down its own first card. Start at the top, where the reading starts.
            var scroller = _settingsHost.Parent as ScrollViewer;
            if (scroller != null) scroller.ScrollToTop();
            UpdateSaveButton();
        }

        Border SettingRow(IniKey spec, string value)
        {
            var b = new Border
            {
                Padding = new Thickness(18, 11, 18, 11),
                BorderBrush = Widgets.Brush("#202023"),
                BorderThickness = new Thickness(0, 1, 0, 0),
            };
            Grid g = Widgets.Columns("*", "Auto");
            var left = new StackPanel();
            left.Children.Add(Widgets.Text(spec.Label, 12, "#ECECEE"));
            var sub = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 2, 12, 0) };
            sub.Children.Add(Widgets.Text(spec.Key, 11, "#7C9CFF", false, true));
            TextBlock help = Widgets.Text("  " + spec.Help, 11, "#97979F");
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
            UpdateSaveButton();
            Say(written.Count > 0 ? "written: " + string.Join(", ", written) : "nothing to write");
            StartSetupRefresh();      // the Setup tab's view of the game's settings just changed
        }
    }
}
