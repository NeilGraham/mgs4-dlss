// The Settings tab: MGS4\mgs4_dlss.ini as a form, each row naming its key and what it does. Saving is blocked
// while the game is running, because the add-on owns that file then - its writes go through the Windows profile
// API, whose cache will quietly undo an outside edit.
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        // One entry per row: the key it belongs to (which knows its file) and how to read the control back.
        readonly List<KeyValuePair<IniKey, Func<string>>> _settingReaders = new List<KeyValuePair<IniKey, Func<string>>>();

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
            if (!Paths.Exists(addonIni))
            {
                StackPanel body;
                _settingsHost.Children.Add(Widgets.Card("No mgs4_dlss.ini",
                    "There is no mgs4_dlss.ini next to mgs4.exe. The Setup tab installs the add-on and its ini together.",
                    "warn", "not there", out body));
            }

            string group = null;
            StackPanel current = null;
            foreach (IniKey spec in IniForm.Spec)
            {
                bool missing = spec.Source == IniSource.Addon ? !Paths.Exists(addonIni) : string.IsNullOrEmpty(gameIni);
                if (spec.Source == IniSource.Addon && missing) continue;   // the card above already says so
                if (spec.Group != group)
                {
                    group = spec.Group;
                    StackPanel body;
                    // Each card says which file it writes, because they are two files with two owners.
                    string blurb = IniForm.SourceLabel(spec.Source);
                    if (missing) blurb += " - not there yet; run the game once and it writes them";
                    _settingsHost.Children.Add(Widgets.Card(group, blurb, missing ? "warn" : "info",
                                                            missing ? "not there" : null, out body));
                    current = body;
                }
                if (missing) continue;
                current.Children.Add(SettingRow(spec, IniForm.Read(spec, addonIni, gameIni)));
            }
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
                    editor = combo;
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
                    editor = tb;
                    break;
                }
            }
            Grid.SetColumn(editor, 1);
            g.Children.Add(editor);
            b.Child = g;
            return b;
        }

        void Remember(IniKey spec, Func<string> read)
        {
            _settingReaders.Add(new KeyValuePair<IniKey, Func<string>>(spec, read));
        }

        // Both files at once, each row going to the one its key lives in. The game owns mgs4.savedsettings while it
        // runs and the add-on owns mgs4_dlss.ini, so neither is written until it is closed.
        void SaveSettings()
        {
            if (Checks.GameRunning()) { Say("close the game first - it owns both of these while it runs"); return; }
            var written = new List<string>();
            foreach (IniSource source in new[] { IniSource.Addon, IniSource.Game })
            {
                var values = new List<KeyValuePair<string, string>>();
                foreach (var row in _settingReaders)
                {
                    if (row.Key.Source != source) continue;
                    string v = row.Value();
                    if (v != null) values.Add(new KeyValuePair<string, string>(row.Key.Key, v));
                }
                if (values.Count == 0) continue;
                string file = IniForm.PathFor(source, _gameDir);
                if (string.IsNullOrEmpty(file) || !Paths.Exists(file)) continue;
                try
                {
                    Checks.SetIni(file, values);
                    written.Add(values.Count + " to " + IniForm.SourceLabel(source));
                }
                catch (Exception e) { Say("could not write " + IniForm.SourceLabel(source) + ": " + e.Message); return; }
            }
            Say(written.Count > 0 ? "written: " + string.Join(", ", written) : "nothing to write");
            ShowSetup();      // the Setup tab's view of the game's settings just changed
        }
    }
}
