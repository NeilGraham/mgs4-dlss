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
            public string Original;
        }
        readonly List<Binding> _settingReaders = new List<Binding>();

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
            bool running = Checks.GameRunning();
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

        void BuildSettings()
        {
            _settingsHost.Children.Clear();
            _settingReaders.Clear();

            bool running = Checks.GameRunning();
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
                bool missing = false;
                foreach (IniKey spec in keys)
                {
                    string label = IniForm.SourceLabel(spec.Source);
                    if (!files.Contains(label)) files.Add(label);
                    string file = IniForm.PathFor(spec.Source, _gameDir);
                    if (spec.Source != IniSource.Launcher && (string.IsNullOrEmpty(file) || !Paths.Exists(file)))
                        missing = true;
                }
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
                BorderBrush = Widgets.Brush("#20242E"),
                BorderThickness = new Thickness(0, 1, 0, 0),
            };
            Grid g = Widgets.Columns("*", "Auto");
            var left = new StackPanel();
            left.Children.Add(Widgets.Text(spec.Label, 12, "#E7EAF0"));
            var sub = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 2, 12, 0) };
            sub.Children.Add(Widgets.Text(spec.Key, 11, "#7C9CFF", false, true));
            TextBlock help = Widgets.Text("  " + spec.Help, 11, "#858D9E");
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
                    Remember(spec, () => cb.IsChecked == true ? spec.TrueWord : spec.FalseWord);
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
                    Remember(spec, () => combo.SelectedIndex >= 0 ? spec.Choices[combo.SelectedIndex] : value);
                    combo.SelectionChanged += (s2, e2) => UpdateSaveButton();
                    editor = combo;
                    break;
                }
                case "res":
                {
                    string[] parts = (value ?? "").Split('x', 'X');
                    var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
                    var w = new TextBox { Text = parts.Length == 2 ? parts[0].Trim() : "", MinWidth = 62 };
                    var h = new TextBox { Text = parts.Length == 2 ? parts[1].Trim() : "", MinWidth = 62 };
                    TextBlock by = Widgets.Text("x", 12, "#858D9E");
                    by.Margin = new Thickness(7, 0, 7, 0);
                    by.VerticalAlignment = VerticalAlignment.Center;
                    row.Children.Add(w);
                    row.Children.Add(by);
                    row.Children.Add(h);
                    // Both boxes or neither: half a resolution is not one, and clearing them is how it is unset.
                    Remember(spec, () =>
                    {
                        string across = w.Text.Trim(), down = h.Text.Trim();
                        return across.Length > 0 && down.Length > 0 ? across + "x" + down : "";
                    });
                    w.TextChanged += (s2, e2) => UpdateSaveButton();
                    h.TextChanged += (s2, e2) => UpdateSaveButton();
                    editor = row;
                    break;
                }
                case "readonly":
                {
                    TextBlock t = Widgets.Text(string.IsNullOrEmpty(value) ? "(not written yet)" : value, 12, "#858D9E", false, true);
                    t.VerticalAlignment = VerticalAlignment.Center;
                    editor = t;
                    break;
                }
                default:
                {
                    var tb = new TextBox { Text = value ?? "", MinWidth = 90, VerticalAlignment = VerticalAlignment.Center };
                    Remember(spec, () => tb.Text.Trim());
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
        void Remember(IniKey spec, Func<string> read)
        {
            _settingReaders.Add(new Binding { Spec = spec, Read = read, Original = read() });
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
                if (source == IniSource.Launcher && !Paths.Exists(file))
                    System.IO.File.WriteAllText(file,
                        "; Machine-local paths for this checkout (git-ignored). See config.example.ini for every key." +
                        Environment.NewLine);
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
            ShowSetup();      // the Setup tab's view of the game's settings just changed
        }
    }
}
