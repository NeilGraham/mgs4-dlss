// The window on a controller. Gamepad.cs says which buttons are down; this says what they do, tab by tab, and
// paints a guide to it in the bottom bar while a pad is connected.
//
// The shape is the same everywhere: a *zone* is the part of the tab the pad is in, the d-pad walks within it, A
// acts on the thing under the walk, B backs out, Start does the tab's big button (Launch, Save, Re-check), and
// the bumpers change tab. What differs is what the zones are:
//
//   Play, all scenes     List (the scenes; left stick scrolls it), Options (the run options; right stick
//                        scrolls them), Filters (the chips, reached with Y). Right and Left step between the
//                        list and the options. Select flips to the other view.
//   Play, start options  Tiles (the three ways in; Left and Right pick), Bar (the one checkbox under them).
//   Settings, Setup      Rows: every visible row on the page. Right stick scrolls; a slider or a choice is
//                        nudged with Left and Right, or the left stick.
//
// One rule the d-pad follows on every list: when the thing that was picked has been scrolled off the screen,
// Down picks the first row on screen and Up picks the last, rather than stepping from something you cannot see.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Effects;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        enum Zone { List, Options, Filters, Tiles, Bar, Rows }
        Zone _zone = Zone.List;
        int _optIndex, _chipIndex, _rowIndex;

        WrapPanel _padGuide;
        ScrollViewer _sceneScroll, _pickScroll;

        // What the pad is on right now, and how it was painted before, so it can be put back.
        FrameworkElement _glowOn;
        Border _rowOn;
        Brush _rowWas;

        static readonly Brush RowLit = new SolidColorBrush(Color.FromArgb(0xFF, 0x20, 0x26, 0x3A));
        const double ScrollSpeed = 1500;        // px/s at full stick

        void WirePad()
        {
            Gamepad.Button += OnPadButton;
            Gamepad.Tick += OnPadTick;
            Gamepad.Connection += (on, kind) => { PaintGuide(); if (on) EnterTab(); else ClearPadPaint(); };
            Win.Closed += (s, e) => Gamepad.Stop();
            Gamepad.Start(Win.Dispatcher);
        }

        // Called when a tab is shown: the zone goes to the tab's first, the stick mode follows, the guide repaints.
        void EnterTab()
        {
            ClearPadPaint();
            string tab = CurrentTab();
            Gamepad.StickIsDpad = tab == "settings";
            if (tab == "play") _zone = _simplePlay ? Zone.Tiles : Zone.List;
            else _zone = Zone.Rows;
            _rowIndex = -1;
            PaintGuide();
        }

        string CurrentTab()
        {
            if (_settingsView.Visibility == Visibility.Visible) return "settings";
            if (_installView.Visibility == Visibility.Visible) return "install";
            return "play";
        }

        // ------------------------------------------------------------------------------------------ buttons

        // Only the active window listens. A pad reports the same whoever is in front, and with the game up the
        // presses are the game's: without this, Options in a cutscene would have launched a second scene.
        bool PadListening() { return Win.IsActive || Gamepad.IgnoreFocus; }

        void OnPadButton(PadButton b, bool repeat)
        {
            if (!PadListening()) return;
            if (PlaylistOpen) { PadPlaylist(b, repeat); return; }
            if (_veil.Visibility == Visibility.Visible)
            {
                if (b == PadButton.A || b == PadButton.Start) CloseVeil(true);
                else if (b == PadButton.B) CloseVeil(false);
                return;
            }
            if (!repeat && (b == PadButton.LB || b == PadButton.LT)) { StepTab(-1); return; }
            if (!repeat && (b == PadButton.RB || b == PadButton.RT)) { StepTab(1); return; }

            string tab = CurrentTab();
            if (tab == "settings") PadSettings(b, repeat);
            else if (tab == "install") PadSetup(b, repeat);
            else if (_simplePlay) PadStart(b, repeat);
            else PadList(b, repeat);
        }

        void StepTab(int dir)
        {
            string[] order = SetupHidden() ? new[] { "play", "settings" } : new[] { "play", "settings", "install" };
            int i = Array.IndexOf(order, CurrentTab()) + dir;
            if (i < 0 || i >= order.Length) return;
            ShowTab(order[i]);
            EnterTab();
        }

        // ---- Play, all scenes

        void PadList(PadButton b, bool repeat)
        {
            if (b == PadButton.Start && !repeat) { Launch(); return; }
            if (b == PadButton.Select && !repeat) { SwitchPlayView(); return; }
            if (b == PadButton.Y && !repeat)
            {
                SetZone(_zone == Zone.Filters ? Zone.List : Zone.Filters);
                return;
            }
            switch (_zone)
            {
                case Zone.List:
                    if (b == PadButton.Up) MoveScene(-1);
                    else if (b == PadButton.Down) MoveScene(1);
                    else if ((b == PadButton.Right || b == PadButton.A) && !repeat) SetZone(Zone.Options);
                    break;
                case Zone.Options:
                    if (b == PadButton.Up) MoveOption(-1);
                    else if (b == PadButton.Down) MoveOption(1);
                    else if ((b == PadButton.Left || b == PadButton.B) && !repeat) SetZone(Zone.List);
                    else if (b == PadButton.A && !repeat) Activate(OptionControls().ElementAtOrDefault(_optIndex));
                    break;
                case Zone.Filters:
                    if (b == PadButton.Left) MoveChip(-1);
                    else if (b == PadButton.Right) MoveChip(1);
                    else if (b == PadButton.A && !repeat)
                    {
                        var chip = _filters.Children.OfType<ToggleButton>().ElementAtOrDefault(_chipIndex);
                        if (chip != null) chip.IsChecked = chip.IsChecked != true;
                    }
                    else if (b == PadButton.B && !repeat) SetZone(Zone.List);
                    break;
            }
        }

        void SetZone(Zone z)
        {
            ClearPadPaint();
            _zone = z;
            if (z == Zone.Options)
            {
                List<FrameworkElement> opts = OptionControls();
                if (opts.Count == 0) { _zone = Zone.List; }
                else { _optIndex = Math.Min(_optIndex, opts.Count - 1); Glow(opts[_optIndex]); }
            }
            else if (z == Zone.Filters)
            {
                var chips = _filters.Children.OfType<ToggleButton>().ToList();
                if (chips.Count == 0) _zone = Zone.List;
                else { _chipIndex = Math.Min(_chipIndex, chips.Count - 1); Glow(chips[_chipIndex]); }
            }
            else if (z == Zone.Bar) Glow(_startAdvance);
            PaintGuide();
        }

        // The scene the d-pad lands on. Headers are skipped; a pick that has been scrolled out of sight is not
        // stepped from - Down takes the first row on screen and Up the last.
        void MoveScene(int dir)
        {
            var rows = _sceneList.Items.OfType<SceneRow>().ToList();
            if (rows.Count == 0) return;
            int cur = _sceneList.SelectedItem is SceneRow ? rows.IndexOf((SceneRow)_sceneList.SelectedItem) : -1;

            int first = -1, last = -1;
            for (int i = 0; i < rows.Count; i++)
            {
                if (rows[i].IsHeader || !RowOnScreen(rows[i])) continue;
                if (first < 0) first = i;
                last = i;
            }
            int to;
            if (cur < 0 || !RowOnScreen(rows[cur]))
                to = dir > 0 ? first : last;
            else
            {
                to = cur;
                do { to += dir; } while (to >= 0 && to < rows.Count && rows[to].IsHeader);
                if (to < 0 || to >= rows.Count) return;
            }
            if (to < 0) return;
            _sceneList.SelectedItem = rows[to];
            _sceneList.ScrollIntoView(rows[to]);
        }

        bool RowOnScreen(SceneRow row)
        {
            var item = _sceneList.ItemContainerGenerator.ContainerFromItem(row) as FrameworkElement;
            if (item == null || !item.IsVisible) return false;
            try
            {
                Point p = item.TransformToAncestor(_sceneList).Transform(new Point(0, 0));
                return p.Y >= -2 && p.Y + item.ActualHeight <= _sceneList.ActualHeight + 2;
            }
            catch { return false; }
        }

        // The controls in the picked-scene panel, top to bottom, that the pad can land on.
        List<FrameworkElement> OptionControls()
        {
            var all = new FrameworkElement[] { _editBtn, _altPick, _optAdvance, _optMashX, _optEnd, _optHold, _holdSecs,
                                               _optRes, _resPick, _cmdCopyBtn, _shortcutBtn, _stopBtn };
            return all.Where(c => c != null && c.IsVisible && c.IsEnabled).ToList();
        }

        void MoveOption(int dir)
        {
            List<FrameworkElement> opts = OptionControls();
            if (opts.Count == 0) return;
            _optIndex = Math.Max(0, Math.Min(opts.Count - 1, _optIndex + dir));
            Glow(opts[_optIndex]);
            opts[_optIndex].BringIntoView();
        }

        void MoveChip(int dir)
        {
            var chips = _filters.Children.OfType<ToggleButton>().ToList();
            if (chips.Count == 0) return;
            _chipIndex = Math.Max(0, Math.Min(chips.Count - 1, _chipIndex + dir));
            Glow(chips[_chipIndex]);
        }

        // A press on whatever the pad is on: a box ticks, a list steps to its next entry, a button is pressed,
        // and the seconds box walks a few sensible holds.
        void Activate(FrameworkElement c)
        {
            if (c == null || !c.IsEnabled) return;
            var cb = c as CheckBox;
            if (cb != null) { cb.IsChecked = cb.IsChecked != true; return; }
            var combo = c as ComboBox;
            if (combo != null && combo.Items.Count > 0) { combo.SelectedIndex = (combo.SelectedIndex + 1) % combo.Items.Count; return; }
            var btn = c as ButtonBase;
            if (btn != null) { btn.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); return; }
            if (c == _holdSecs)
            {
                int[] steps = { 15, 30, 60, 120, 300 };
                int v; int.TryParse(_holdSecs.Text, out v);
                int next = steps.FirstOrDefault(s => s > v);
                _holdSecs.Text = (next == 0 ? steps[0] : next).ToString();
            }
        }

        // ---- Play, the start options

        void PadStart(PadButton b, bool repeat)
        {
            if (b == PadButton.Start && !repeat) { Launch(); return; }
            if (b == PadButton.Select && !repeat) { SwitchPlayView(); return; }
            if (_zone == Zone.Bar)
            {
                if ((b == PadButton.Up || b == PadButton.B) && !repeat) SetZone(Zone.Tiles);
                else if (b == PadButton.A && !repeat) Activate(_startAdvance);
                return;
            }
            var ids = _startTiles.Select(t => t.Tag as string).ToList();
            int i = ids.FindIndex(id => string.Equals(id, _pickedId, StringComparison.OrdinalIgnoreCase));
            if (b == PadButton.Left && i > 0) PickStart(ids[i - 1]);
            else if (b == PadButton.Right && i >= 0 && i < ids.Count - 1) PickStart(ids[i + 1]);
            else if (b == PadButton.Right && i < 0 && ids.Count > 0) PickStart(ids[0]);
            else if (b == PadButton.Down && !repeat && _startAdvance.IsEnabled) SetZone(Zone.Bar);
        }

        // ---- Settings and Setup

        // Every row on the page the pad can land on, in page order: rows in cards that are showing and open.
        List<Border> PadRows()
        {
            var outp = new List<Border>();
            if (CurrentTab() == "settings")
                foreach (SettingsCard c in _settingsCards)
                    foreach (KeyValuePair<Border, string> r in c.Rows)
                        if (r.Key.IsVisible && r.Key.Tag is Widgets.RowInfo) outp.Add(r.Key);
            if (CurrentTab() == "install")
                foreach (SetupCard c in _setupCards)
                    foreach (SetupRow r in c.Rows)
                        if (r.Row.IsVisible && r.Row.Tag is Widgets.RowInfo) outp.Add(r.Row);
            return outp;
        }

        void PadSettings(PadButton b, bool repeat)
        {
            if (b == PadButton.Start && !repeat) { if (_saveBtn.IsEnabled) SaveSettings(); return; }
            if (b == PadButton.Up) MoveRow(-1, _settingsScroll);
            else if (b == PadButton.Down) MoveRow(1, _settingsScroll);
            else if (b == PadButton.Left) NudgeRow(-1);
            else if (b == PadButton.Right) NudgeRow(1);
            else if (b == PadButton.A && !repeat) PressRow();
        }

        void PadSetup(PadButton b, bool repeat)
        {
            if (b == PadButton.Start && !repeat) { ShowSetup(); return; }
            if (b == PadButton.Up) MoveRow(-1, _installScroll);
            else if (b == PadButton.Down) MoveRow(1, _installScroll);
            else if (b == PadButton.A && !repeat) PressRow();
        }

        void MoveRow(int dir, ScrollViewer sv)
        {
            List<Border> rows = PadRows();
            if (rows.Count == 0) return;
            int cur = _rowOn != null ? rows.IndexOf(_rowOn) : -1;
            int first = -1, last = -1;
            for (int i = 0; i < rows.Count; i++)
            {
                if (!InView(rows[i], sv)) continue;
                if (first < 0) first = i;
                last = i;
            }
            int to;
            if (cur < 0 || !InView(rows[cur], sv)) to = dir > 0 ? first : last;
            else to = Math.Max(0, Math.Min(rows.Count - 1, cur + dir));
            if (to < 0) to = dir > 0 ? 0 : rows.Count - 1;
            LightRow(rows[to]);
            _rowIndex = to;

            // Kept on screen with a little room, gliding rather than jumping so the page reads as moving.
            try
            {
                double top = rows[to].TransformToAncestor((FrameworkElement)sv.Content).Transform(new Point(0, 0)).Y;
                double bottom = top + rows[to].ActualHeight;
                if (top < sv.VerticalOffset + 8) SmoothScroll.Glide(sv, top - 8);
                else if (bottom > sv.VerticalOffset + sv.ViewportHeight - 8) SmoothScroll.Glide(sv, bottom - sv.ViewportHeight + 8);
            }
            catch { }
        }

        static bool InView(FrameworkElement row, ScrollViewer sv)
        {
            try
            {
                Point p = row.TransformToAncestor(sv).Transform(new Point(0, 0));
                return p.Y >= -2 && p.Y + row.ActualHeight <= sv.ViewportHeight + 2;
            }
            catch { return false; }
        }

        void PressRow()
        {
            var info = _rowOn != null ? _rowOn.Tag as Widgets.RowInfo : null;
            if (info == null) return;
            if (info.Button != null) { if (info.Button.IsEnabled) info.Button.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); return; }
            if (info.Editor is CheckBox || info.Editor is ComboBox) Activate(info.Editor);
        }

        // Left and Right on a settings row: a choice steps, a slider or a number moves by its step, a box ticks.
        void NudgeRow(int dir)
        {
            var info = _rowOn != null ? _rowOn.Tag as Widgets.RowInfo : null;
            if (info == null || info.Editor == null || !info.Editor.IsEnabled) return;
            var combo = info.Editor as ComboBox;
            if (combo != null && combo.Items.Count > 0)
            {
                combo.SelectedIndex = Math.Max(0, Math.Min(combo.Items.Count - 1, combo.SelectedIndex + dir));
                return;
            }
            var slider = info.Editor as Slider;
            if (slider != null)
            {
                double step = info.Spec != null && info.Spec.Step > 0 ? info.Spec.Step : 1;
                slider.Value = Math.Max(slider.Minimum, Math.Min(slider.Maximum, slider.Value + dir * step));
                return;
            }
            var box = info.Editor as TextBox;
            if (box != null)
            {
                double v;
                if (double.TryParse(box.Text, out v)) box.Text = (v + dir).ToString();
                return;
            }
            var cb = info.Editor as CheckBox;
            if (cb != null) cb.IsChecked = dir > 0;
        }

        // ------------------------------------------------------------------------------------------ sticks

        void OnPadTick(PadState s, double dt)
        {
            if (_veil.Visibility == Visibility.Visible || !PadListening()) return;
            string tab = CurrentTab();
            if (tab == "settings") Scroll(_settingsScroll, s.RY, dt);
            else if (tab == "install") Scroll(_installScroll, s.RY, dt);
            else if (_simplePlay) Scroll(_startScroll, Math.Abs(s.LY) > Math.Abs(s.RY) ? s.LY : s.RY, dt);
            else
            {
                if (_sceneScroll == null) _sceneScroll = FindScroller(_sceneList);
                Scroll(_sceneScroll, s.LY, dt);
                Scroll(_pickScroll, s.RY, dt);
            }
        }

        static void Scroll(ScrollViewer sv, double axis, double dt)
        {
            if (sv == null || axis == 0) return;
            double v = Math.Pow(Math.Abs(axis), 1.6) * ScrollSpeed * dt;
            sv.ScrollToVerticalOffset(sv.VerticalOffset - Math.Sign(axis) * v);   // stick up scrolls up
        }

        static ScrollViewer FindScroller(DependencyObject d)
        {
            if (d == null) return null;
            var sv = d as ScrollViewer;
            if (sv != null) return sv;
            int n = VisualTreeHelper.GetChildrenCount(d);
            for (int i = 0; i < n; i++)
            {
                ScrollViewer hit = FindScroller(VisualTreeHelper.GetChild(d, i));
                if (hit != null) return hit;
            }
            return null;
        }

        // ------------------------------------------------------------------------------------------ painting

        // A soft accent glow round a control the pad is on; the row version tints the row the way the scene
        // list tints its pick.
        void Glow(FrameworkElement e)
        {
            if (_glowOn != null) _glowOn.Effect = null;
            _glowOn = e;
            if (e != null)
                e.Effect = new DropShadowEffect { Color = Color.FromRgb(0x7C, 0x9C, 0xFF), BlurRadius = 12, ShadowDepth = 0, Opacity = 0.95 };
        }

        void LightRow(Border row)
        {
            if (_rowOn != null) _rowOn.Background = _rowWas;
            _rowOn = row;
            if (row != null) { _rowWas = row.Background; row.Background = RowLit; }
        }

        void ClearPadPaint()
        {
            Glow(null);
            LightRow(null);
        }

        // ------------------------------------------------------------------------------------------ the guide

        class Hint { public string Key, What; public Hint(string k, string w) { Key = k; What = w; } }

        // The badge text for a button, in the connected pad's own words.
        static string KeyName(PadButton b)
        {
            bool sony = Gamepad.Kind == PadKind.Sony;
            switch (b)
            {
                case PadButton.A: return sony ? "✕" : "A";
                case PadButton.B: return sony ? "○" : "B";
                case PadButton.X: return sony ? "□" : "X";
                case PadButton.Y: return sony ? "△" : "Y";
                case PadButton.LB: return sony ? "L1" : "LB";
                case PadButton.RB: return sony ? "R1" : "RB";
                case PadButton.Start: return sony ? "OPTIONS" : "START";
                case PadButton.Select: return sony ? "CREATE" : "SELECT";
            }
            return b.ToString();
        }

        void PaintGuide()
        {
            if (_padGuide == null) return;
            _padGuide.Children.Clear();
            if (!Gamepad.Connected) { _padGuide.Visibility = Visibility.Collapsed; return; }
            _padGuide.Visibility = Visibility.Visible;

            var hints = new List<Hint>();
            string tab = CurrentTab();
            if (PlaylistOpen)
            {
                hints.Add(new Hint("◄ ►", "Which list"));
                hints.Add(new Hint(KeyName(PadButton.A), "Add / remove"));
                hints.Add(new Hint(KeyName(PadButton.X), "Sample"));
                hints.Add(new Hint(KeyName(PadButton.Y), "Favourite"));
                hints.Add(new Hint(KeyName(PadButton.B), "Done"));
                tab = null;
            }
            else if (tab == "settings")
            {
                hints.Add(new Hint(KeyName(PadButton.A), "Toggle"));
                hints.Add(new Hint("◄ ►", "Adjust"));
                hints.Add(new Hint(KeyName(PadButton.Start), "Save"));
            }
            else if (tab == "install")
            {
                hints.Add(new Hint(KeyName(PadButton.A), "Press"));
                hints.Add(new Hint(KeyName(PadButton.Start), "Re-check"));
            }
            else if (_simplePlay)
            {
                hints.Add(new Hint("◄ ►", "Pick"));
                hints.Add(new Hint(KeyName(PadButton.Start), "Launch"));
                hints.Add(new Hint(KeyName(PadButton.Select), "All Scenes"));
            }
            else
            {
                if (_zone == Zone.Filters)
                {
                    hints.Add(new Hint(KeyName(PadButton.A), "Toggle filter"));
                    hints.Add(new Hint(KeyName(PadButton.Y), "Back to scenes"));
                }
                else if (_zone == Zone.Options)
                {
                    hints.Add(new Hint(KeyName(PadButton.A), "Toggle"));
                    hints.Add(new Hint("◄", "Scenes"));
                }
                else
                {
                    hints.Add(new Hint("►", "Options"));
                    hints.Add(new Hint(KeyName(PadButton.Y), "Filters"));
                }
                hints.Add(new Hint(KeyName(PadButton.Start), "Launch"));
                hints.Add(new Hint(KeyName(PadButton.Select), "Start Options"));
            }
            if (tab != null) hints.Add(new Hint(KeyName(PadButton.LB) + " " + KeyName(PadButton.RB), "Tabs"));

            foreach (Hint h in hints)
            {
                var badge = new Border
                {
                    Background = Widgets.Brush("#26262C"),
                    BorderBrush = Widgets.Brush("#3F3F4A"),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(4),
                    Padding = new Thickness(6, 1, 6, 1),
                    VerticalAlignment = VerticalAlignment.Center,
                    Child = Widgets.Text(h.Key, 10, "#ECECEE", true),
                };
                TextBlock what = Widgets.Text(h.What, 11, "#97979F");
                what.VerticalAlignment = VerticalAlignment.Center;
                what.Margin = new Thickness(5, 0, 14, 0);
                _padGuide.Children.Add(badge);
                _padGuide.Children.Add(what);
            }
        }
    }
}
