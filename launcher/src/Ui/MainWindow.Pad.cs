// The window on a controller. Gamepad.cs says which buttons are down; this says what they do, tab by tab, and
// puts each button's name beside the thing it does while a pad is connected.
//
// The shape is the same everywhere: a *zone* is the part of the tab the pad is in, the d-pad walks within it, A
// acts on the thing under the walk, B backs out, Start does the tab's big button (Launch, Save, Re-check), and
// the bumpers change tab. What differs is what the zones are:
//
//   Play, all scenes     List (the scenes and their act headers; left stick scrolls it), Options (the run options;
//                        right stick scrolls them), Filters (the chips, reached with Y). Right and Left step
//                        between the list and the options; on an act header, Right or A opens or shuts the act.
//                        Select flips to the other view.
//   Play, start options  Tiles (the three ways in; Left and Right pick), Bar (the checkbox and the three buttons
//                        under them; Left and Right walk, A presses - Launch among them).
//   Settings, Setup      Rows: every visible row on the page. Right stick scrolls; a slider or a choice is
//                        nudged with Left and Right, or the left stick.
//
// One rule the d-pad follows on every list: when the thing that was picked has been scrolled off the screen,
// Down picks the first row on screen and Up picks the last, rather than stepping from something you cannot see.
// "Off the screen" is judged against where the page is *going* when it is still gliding there - a held direction
// steps faster than the glide lands, and judged against the drawn offset the row it had just moved to read as
// gone, and every second press snapped back to the top.
//
// The hints. There is no legend: the d-pad, A and B are what everyone expects, and the rest are written where they
// act - ☰ on Launch and Save, the bumpers' names either side of the tabs, the view button's key on the view
// button, △ at the head of the filter chips, and ✕ / □ / △ / ○ on the playlist editor's own buttons. They show
// while a pad is connected, in the pad's own names, and go when it does.
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

        ScrollViewer _sceneScroll, _pickScroll;

        // What the pad is on right now, and how it was painted before, so it can be put back.
        FrameworkElement _glowOn;
        Border _rowOn;
        Brush _rowWas;
        // The scene row the pad last landed on. Right after a step its container may not be realized yet (the
        // list virtualizes), which would read as off screen; the one the pad itself just went to is stepped from
        // regardless. Cleared when anything else scrolls the list.
        SceneRow _padLandedRow;

        static readonly Brush RowLit = new SolidColorBrush(Color.FromArgb(0xFF, 0x20, 0x26, 0x3A));
        const double ScrollSpeed = 1500;        // px/s at full stick

        void WirePad()
        {
            WirePadHints();
            Gamepad.Button += OnPadButton;
            Gamepad.Tick += OnPadTick;
            Gamepad.Connection += (on, kind) => { PaintPadHints(); if (on) EnterTab(); else ClearPadPaint(); };
            Win.Closed += (s, e) => Gamepad.Stop();
            _padWired = true;
            Gamepad.Start(Win.Dispatcher);
        }

        // Called when a tab is shown: the zone goes to the tab's first, the stick mode follows, the hints repaint.
        void EnterTab()
        {
            ClearPadPaint();
            string tab = CurrentTab();
            Gamepad.StickIsDpad = tab == "settings";
            if (tab == "play") _zone = _simplePlay ? Zone.Tiles : Zone.List;
            else _zone = Zone.Rows;
            _rowIndex = -1;
            PaintPadHints();
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

        string[] TabOrder()
        {
            return SetupHidden() ? new[] { "play", "settings" } : new[] { "play", "settings", "install" };
        }

        void StepTab(int dir)
        {
            string[] order = TabOrder();
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
                    else if ((b == PadButton.Right || b == PadButton.A) && !repeat)
                    {
                        // On an act's header the press is the header's: it opens or shuts the act, the way a
                        // click on it does. On a scene it crosses into the options.
                        var row = _sceneList.SelectedItem as SceneRow;
                        if (row != null && row.IsHeader) ToggleAct(row.ActKey, true);
                        else SetZone(Zone.Options);
                    }
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
            else if (z == Zone.Bar)
            {
                List<FrameworkElement> bar = BarControls();
                if (bar.Count == 0) _zone = Zone.Tiles;
                else { _barIndex = Math.Min(_barIndex, bar.Count - 1); Glow(bar[_barIndex]); }
            }
            PaintPadHints();
        }

        // The row the d-pad lands on: a scene or an act header, whichever is next. A pick that has been scrolled
        // out of sight is not stepped from - Down takes the first row on screen and Up the last - unless it is
        // the row the pad itself just went to, whose container the list may not have drawn yet.
        void MoveScene(int dir)
        {
            var rows = _sceneList.Items.OfType<SceneRow>().ToList();
            if (rows.Count == 0) return;
            int cur = _sceneList.SelectedItem is SceneRow ? rows.IndexOf((SceneRow)_sceneList.SelectedItem) : -1;

            int first = -1, last = -1;
            for (int i = 0; i < rows.Count; i++)
            {
                if (!RowOnScreen(rows[i])) continue;
                if (first < 0) first = i;
                last = i;
            }
            int to;
            bool stepFrom = cur >= 0 && (RowOnScreen(rows[cur]) || rows[cur] == _padLandedRow);
            if (!stepFrom) to = dir > 0 ? first : last;
            else
            {
                to = cur + dir;
                if (to < 0 || to >= rows.Count) return;
            }
            if (to < 0) return;
            _sceneList.SelectedItem = rows[to];
            _sceneList.ScrollIntoView(rows[to]);
            _padLandedRow = rows[to];
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
                                               _optRes, _resPick, _cmdCopyBtn, _launchBtn, _stopBtn, _shortcutBtn };
            return all.Where(c => c != null && c.IsVisible && c.IsEnabled).ToList();
        }

        // The row under the start cards, left to right: the checkbox and the three buttons. Launch is among them
        // in both views, so A on it is the other way to launch.
        List<FrameworkElement> BarControls()
        {
            var all = new FrameworkElement[] { _startAdvance, _startShortcutBtn, _startStopBtn, _startLaunchBtn };
            return all.Where(c => c != null && c.IsVisible && c.IsEnabled).ToList();
        }
        int _barIndex;

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
                // The row under the cards: Left and Right walk the checkbox and the buttons, A presses the one
                // the glow is on - Launch among them - and Up or B goes back to the cards.
                List<FrameworkElement> bar = BarControls();
                if ((b == PadButton.Up || b == PadButton.B) && !repeat) SetZone(Zone.Tiles);
                else if (b == PadButton.Left || b == PadButton.Right)
                {
                    if (bar.Count == 0) return;
                    _barIndex = Math.Max(0, Math.Min(bar.Count - 1, _barIndex + (b == PadButton.Right ? 1 : -1)));
                    Glow(bar[_barIndex]);
                }
                else if (b == PadButton.A && !repeat) Activate(bar.ElementAtOrDefault(_barIndex));
                return;
            }
            var ids = _startTiles.Select(t => t.Tag as string).ToList();
            int i = ids.FindIndex(id => string.Equals(id, _pickedId, StringComparison.OrdinalIgnoreCase));
            if (b == PadButton.Left && i > 0) PickStart(ids[i - 1]);
            else if (b == PadButton.Right && i >= 0 && i < ids.Count - 1) PickStart(ids[i + 1]);
            else if (b == PadButton.Right && i < 0 && ids.Count > 0) PickStart(ids[0]);
            else if (b == PadButton.Down && !repeat && BarControls().Count > 0) SetZone(Zone.Bar);
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

            // Where the page is, or where it is on its way to: a held direction steps every 75 ms and the glide
            // to the last row takes longer than that, so the drawn offset still has that row half off the bottom.
            double offset = ScrollOffsetSettling(sv);

            int first = -1, last = -1;
            for (int i = 0; i < rows.Count; i++)
            {
                if (!InViewAt(rows[i], sv, offset)) continue;
                if (first < 0) first = i;
                last = i;
            }
            int to;
            if (cur < 0 || !InViewAt(rows[cur], sv, offset)) to = dir > 0 ? first : last;
            else to = Math.Max(0, Math.Min(rows.Count - 1, cur + dir));
            if (to < 0) to = dir > 0 ? 0 : rows.Count - 1;
            LightRow(rows[to]);
            _rowIndex = to;

            // Kept on screen with a little room, gliding rather than jumping so the page reads as moving. Aimed
            // from the settling offset too, so successive steps chain rather than each starting from where the
            // previous glide happened to be.
            try
            {
                double top = rows[to].TransformToAncestor((FrameworkElement)sv.Content).Transform(new Point(0, 0)).Y;
                double bottom = top + rows[to].ActualHeight;
                if (top < offset + 8) SmoothScroll.Glide(sv, top - 8);
                else if (bottom > offset + sv.ViewportHeight - 8) SmoothScroll.Glide(sv, bottom - sv.ViewportHeight + 8);
            }
            catch { }
        }

        // The offset a scroller is heading for when a glide is in flight, else the one it is at.
        static double ScrollOffsetSettling(ScrollViewer sv)
        {
            double target;
            return SmoothScroll.Pending(sv, out target) ? target : sv.VerticalOffset;
        }

        // Whether a row would be wholly within the viewport with the page at the given offset.
        static bool InViewAt(FrameworkElement row, ScrollViewer sv, double offset)
        {
            try
            {
                var content = sv.Content as FrameworkElement;
                if (content == null) return false;
                double top = row.TransformToAncestor(content).Transform(new Point(0, 0)).Y - offset;
                return top >= -2 && top + row.ActualHeight <= sv.ViewportHeight + 2;
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
                if (s.LY != 0) _padLandedRow = null;    // the stick moved the list: the pick may really be gone
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

        // ------------------------------------------------------------------------------------------ the hints

        // The badge text for a button, in the connected pad's own words. Start is the three-line glyph both
        // families draw on the button itself; Select is the one that has to be a word, since neither pad's mark
        // for it is a character.
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
                case PadButton.Start: return "☰";
                case PadButton.Select: return sony ? "CREATE" : "VIEW";
            }
            return b.ToString();
        }

        // A button that carries a hint: its text, moved into a block of its own so it can still be changed, and
        // the badge beside it.
        class Hinted { public TextBlock Label; public Border Badge; public PadButton Key; }
        readonly Dictionary<ContentControl, Hinted> _hinted = new Dictionary<ContentControl, Hinted>();
        Border _filterHint;

        static Border MakeBadge(PadButton key)
        {
            return new Border
            {
                Background = Widgets.Brush("#26262C"),
                BorderBrush = Widgets.Brush("#3F3F4A"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(6, 1, 6, 1),
                VerticalAlignment = VerticalAlignment.Center,
                Visibility = Visibility.Collapsed,
                Tag = key,
                Child = Widgets.Text(KeyName(key), 10, "#ECECEE", true),
            };
        }

        /// <summary>Put a pad button's badge beside a button's label. The label keeps the button's own font -
        /// it inherits it - and SetHintedLabel changes it from then on.</summary>
        void Hint(ContentControl c, PadButton key)
        {
            if (c == null || _hinted.ContainsKey(c)) return;
            var label = new TextBlock { Text = c.Content as string ?? "", VerticalAlignment = VerticalAlignment.Center };
            Border badge = MakeBadge(key);
            badge.Margin = new Thickness(9, 0, -2, 0);
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            panel.Children.Add(label);
            panel.Children.Add(badge);
            c.Content = panel;
            _hinted[c] = new Hinted { Label = label, Badge = badge, Key = key };
        }

        void SetHintedLabel(ContentControl c, string text)
        {
            Hinted h;
            if (_hinted.TryGetValue(c, out h)) h.Label.Text = text;
            else c.Content = text;
        }

        // Which buttons say which key. Done once, before the pad is polled, and painted whenever the pad comes
        // or goes or the page changes shape.
        void WirePadHints()
        {
            Hint(_launchBtn, PadButton.Start);
            Hint(_startLaunchBtn, PadButton.Start);
            Hint(_saveBtn, PadButton.Start);
            Hint(_recheckBtn, PadButton.Start);
            Hint(_viewSwitchBtn, PadButton.Select);
            Hint(_veilOkBtn, PadButton.A);
            Hint(_veilCancelBtn, PadButton.B);
            Hint(_plAddBtn, PadButton.A);
            Hint(_plRemoveBtn, PadButton.A);
            Hint(_plSampleBtn, PadButton.X);
            Hint(_plFavBtn, PadButton.Y);
            Hint(_plDoneBtn, PadButton.B);

            // The filter chips are reached with Y, so Y sits at the head of their row.
            _filterHint = MakeBadge(PadButton.Y);
            _filterHint.Margin = new Thickness(0, 0, 8, 6);
            _filterHint.ToolTip = "The filters - on a controller, this button reaches them";
            _filters.Children.Insert(0, _filterHint);
        }

        void PaintPadHints()
        {
            bool on = Gamepad.Connected;
            foreach (KeyValuePair<ContentControl, Hinted> kv in _hinted)
            {
                ((TextBlock)kv.Value.Badge.Child).Text = KeyName(kv.Value.Key);
                kv.Value.Badge.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            }
            // Add and Remove share A: whichever list the pad is in is the one A acts on, and only that one
            // wears the badge.
            if (on && _hinted.ContainsKey(_plAddBtn) && _hinted.ContainsKey(_plRemoveBtn))
            {
                _hinted[_plAddBtn].Badge.Visibility = _playlistOnRight ? Visibility.Collapsed : Visibility.Visible;
                _hinted[_plRemoveBtn].Badge.Visibility = _playlistOnRight ? Visibility.Visible : Visibility.Collapsed;
            }
            if (_filterHint != null) _filterHint.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            if (_filterHint != null) ((TextBlock)_filterHint.Child).Text = KeyName(PadButton.Y);

            // The bumpers, either side of the tabs - each shown only while there is a tab in that direction.
            if (_navPrevHint != null && _navNextHint != null)
            {
                string[] order = TabOrder();
                int i = Array.IndexOf(order, CurrentTab());
                ((TextBlock)_navPrevHint.Child).Text = KeyName(PadButton.LB);
                ((TextBlock)_navNextHint.Child).Text = KeyName(PadButton.RB);
                _navPrevHint.Visibility = on && i > 0 ? Visibility.Visible : Visibility.Collapsed;
                _navNextHint.Visibility = on && i >= 0 && i < order.Length - 1 ? Visibility.Visible : Visibility.Collapsed;
            }
        }
    }
}
