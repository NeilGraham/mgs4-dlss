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
        readonly Dictionary<string, Func<string>> _settingReaders = new Dictionary<string, Func<string>>();

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

            string ini = IniForm.IniPath(_gameDir);
            if (!Paths.Exists(ini))
            {
                StackPanel body;
                _settingsHost.Children.Add(Widgets.Card("No mgs4_dlss.ini",
                    "There is no mgs4_dlss.ini next to mgs4.exe. The Setup tab installs the add-on and its ini together.",
                    "warn", "not there", out body));
                return;
            }

            string group = null;
            StackPanel current = null;
            foreach (IniKey spec in IniForm.Spec)
            {
                if (spec.Group != group)
                {
                    group = spec.Group;
                    StackPanel body;
                    _settingsHost.Children.Add(Widgets.Card(group, null, "info", null, out body));
                    current = body;
                }
                current.Children.Add(SettingRow(spec, Checks.IniValue(ini, spec.Key)));
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
                    var cb = new CheckBox { IsChecked = value == "1", VerticalAlignment = VerticalAlignment.Center };
                    _settingReaders[spec.Key] = () => cb.IsChecked == true ? "1" : "0";
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
                    _settingReaders[spec.Key] = () => combo.SelectedIndex >= 0 ? spec.Choices[combo.SelectedIndex] : value;
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
                    _settingReaders[spec.Key] = () => tb.Text.Trim();
                    editor = tb;
                    break;
                }
            }
            Grid.SetColumn(editor, 1);
            g.Children.Add(editor);
            b.Child = g;
            return b;
        }

        void SaveSettings()
        {
            if (Checks.GameRunning()) { Say("close the game first - it owns mgs4_dlss.ini while it runs"); return; }
            string ini = IniForm.IniPath(_gameDir);
            var values = new List<KeyValuePair<string, string>>();
            foreach (var kv in _settingReaders)
            {
                string v = kv.Value();
                if (v != null) values.Add(new KeyValuePair<string, string>(kv.Key, v));
            }
            try
            {
                Checks.SetIni(ini, values);
                Say("written to " + ini);
            }
            catch (Exception e) { Say("could not write mgs4_dlss.ini: " + e.Message); }
        }
    }
}
