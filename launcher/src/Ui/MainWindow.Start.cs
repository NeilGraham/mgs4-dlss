// The Play tab as it opens: the three ways to start the game, one card each, and nothing that says what is in the
// rest of it.
//
// The scene list is the other half of this tab, and it is a spoiler. It names every cutscene in the game, in story
// order, with a frame of itself on the row - who is in it, where it happens, and roughly what happens - which is
// fine for the person who measured all of it and is the story handed over to anyone who has not played it. So the
// list is behind a setting (config.ini, MGS4_PLAY_VIEW; the Settings tab's Launcher card), it is off by default,
// and the button here that turns it on says what it is turning on before it does.
//
// What is left is what someone who wants to carry on with their save actually needs: where the game opens, and
// the one piece of automation that means anything at a boot.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        // config.ini: "simple" (this view) or "all" (the scene list). Anything else, and an unwritten file, means
        // simple - a release opens on the side that gives nothing away.
        public const string PlayViewKey = "MGS4_PLAY_VIEW";
        public const string PlayViewDefault = "simple";

        public static bool PlayViewIsSimple()
        {
            string v = Paths.Setting(PlayViewKey, PlayViewDefault);
            return !string.Equals(v, "all", StringComparison.OrdinalIgnoreCase);
        }

        bool _simplePlay = true;
        // Whether the spoiler warning has been answered once, ever. Kept in launcher.json rather than config.ini:
        // it is a thing this person has been told, not a thing the app is set to.
        bool _spoilerSeen;
        readonly List<Border> _startTiles = new List<Border>();
        Border _hoverTile;
        Action _veilAction;

        static bool IsStartId(string id)
        {
            Scene s = Catalog.Find(id);
            return s != null && s.Kind == "start";
        }

        void WireStart()
        {
            // The cards keep their own width: three across on a wide window, and fewer rather than narrower as it
            // closes in. 260 logical px is where a card stops holding its title and its two lines of description.
            _startHost.SizeChanged += (s, e) =>
            {
                double w = _startHost.ActualWidth;
                if (w <= 0) return;
                int cols = Math.Max(1, Math.Min(3, (int)(w / 260)));
                if (_startHost.Columns != cols) _startHost.Columns = cols;
            };
            _startAdvance.Checked += (s, e) => { UpdatePreview(); SavePrefs(); };
            _startAdvance.Unchecked += (s, e) => { UpdatePreview(); SavePrefs(); };

            _startLaunchBtn.Click += (s, e) => Launch();
            _startStopBtn.Click += (s, e) => { Runner.StopGame(); Say("closed mgs4.exe"); RefreshState(); };
            _startShortcutBtn.Click += (s, e) => MakeShortcut();

            _viewSwitchBtn.Click += (s, e) => SwitchPlayView();

            _veilCancelBtn.Click += (s, e) => CloseVeil(false);
            _veilOkBtn.Click += (s, e) => CloseVeil(true);
        }

        // ------------------------------------------------------------------------------------- which view is on

        /// <summary>Read MGS4_PLAY_VIEW and make the Play tab agree with it. Called on the way up, and again
        /// whenever the setting is written - from the Settings tab, or from this view's own button.</summary>
        void ApplyPlayView(bool retab)
        {
            bool simple = PlayViewIsSimple();
            bool changed = simple != _simplePlay;
            _simplePlay = simple;
            LabelViewSwitch();
            if (simple)
            {
                if (_startHost.Children.Count == 0) BuildStartTiles();
                // A scene picked in the other view must not stay picked here: Launch would start it, and its name
                // is sitting in the command-line preview either way.
                if (!IsStartId(_pickedId)) _pickedId = "@main";
                ShowPicked(Catalog.Find(_pickedId));
            }
            if (retab && changed && _navPlay.IsChecked == true) ShowTab("play");
        }

        // The bottom bar's button, in both views. Going towards the scene list asks first - once, ever: the
        // warning is worth reading the first time and is nagging by the third, and someone who has said yes to it
        // has already seen what is behind it. Coming back the other way asks nothing.
        void SwitchPlayView()
        {
            if (!_simplePlay) { SetPlayView(true); return; }
            if (_spoilerSeen) { SetPlayView(false); return; }
            Confirm(
                "Show every scene in the game?",
                // The count the list itself shows: what the Broken chip reveals is not part of the offer.
                "The scene list has all " + _allRows.Count(r => !r.Hidden) + " entries in it, in the order the story tells them, "
                + "each one named for what it is and showing a frame of itself. Who is in a cutscene, where it "
                + "happens and how it ends are all on the row.\n\n"
                + "If you have not finished MGS4, this is the game spoiled. It is asked once: the button in the "
                + "bottom bar swaps the two views from here on, and Settings > Launcher holds the same switch.",
                "Show all scenes",
                () =>
                {
                    _spoilerSeen = true;
                    SavePrefs();
                    SetPlayView(false);
                });
        }

        // The label says where the button goes, not where you are.
        void LabelViewSwitch()
        {
            if (_viewSwitchBtn == null) return;
            _viewSwitchBtn.Content = _simplePlay ? "Show all scenes" : "Just the start options";
            _viewSwitchBtn.ToolTip = _simplePlay
                ? "Every scene in the game, named and with a frame of itself - the story, spoiled"
                : "Back to the three ways to start the game";
        }

        // The button and the Settings row write the same key, so the two always say the same thing.
        void SetPlayView(bool simple)
        {
            var values = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>(PlayViewKey, simple ? "simple" : "all"),
            };
            try
            {
                Checks.SetIni(Paths.EnsureConfig(), values, null);
                Paths.ForgetConfig();
            }
            catch (Exception e) { Say("could not write " + Paths.ConfigPath + ": " + e.Message); return; }
            ApplyPlayView(true);
            ApplyFilter();       // rebuilds the list's rows, and says the line that belongs to whichever view is on
        }

        // ------------------------------------------------------------------------------------------- the cards

        void BuildStartTiles()
        {
            _startHost.Children.Clear();
            _startTiles.Clear();
            foreach (Scene scene in Catalog.All().Where(e => e.Kind == "start"))
            {
                Border tile = StartTile(scene);
                _startHost.Children.Add(tile);
                _startTiles.Add(tile);
            }
            PaintStartTiles();
        }

        // How long each route makes you wait before you are in MGS4, as one word on the picture. Green, amber, red,
        // in the order the cards are in - the same three status families the rest of the window uses, so the tag
        // reads as a rating rather than as decoration. The tooltip says what the word is counting.
        //
        // @main is the menu straight away. The title-screen boot plays the logos and the title over the cemetery
        // first. The Master Collection launcher is a second program to get through before MGS4 starts at all, and
        // it is a Unity front-end that takes its own time coming up.
        static Badge SpeedOf(string id)
        {
            if (id == "@main") return new Badge("Fast", "#142117", "#2E6B45", "#62C98A");
            if (id == "@collection") return new Badge("Slow", "#2A1315", "#7A2A2F", "#FF6B66");
            return new Badge("Normal", "#2A2312", "#7A6220", "#F2C14E");
        }

        static string SpeedWhy(string id)
        {
            if (id == "@main") return "Fastest way in - the menu, with nothing to sit through first";
            if (id == "@collection") return "Slowest - the Master Collection front-end comes up first, and MGS4 starts from it";
            return "The Kojima Productions logo and the title screen play before the menu";
        }

        static Border SpeedTag(Badge b)
        {
            return new Border
            {
                Background = Widgets.Brush(b.Back),
                BorderBrush = Widgets.Brush(b.Edge),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(9, 3, 9, 3),
                // Top left, not top right. All three of these frames are busy in the right corner and quiet in the
                // left: the menu shot has the game's own OK / Back glyphs there, and the box art has the spine and
                // the logo. It is also the first thing read on the card, which is what a classification should be.
                Margin = new Thickness(10, 10, 0, 0),
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Top,
                Child = Widgets.Text(b.Text, 10, b.Ink, true),
                // A badge in a settings card sits on a flat panel; this one sits on a photograph, and one of the
                // three lands on the game's own OK / Back glyphs. A soft shadow under it is what separates a label
                // from the picture it is labelling - there are three of them, so it costs nothing.
                Effect = new System.Windows.Media.Effects.DropShadowEffect
                {
                    BlurRadius = 9,
                    ShadowDepth = 0,
                    Opacity = 0.8,
                    Color = System.Windows.Media.Colors.Black,
                },
            };
        }

        Border StartTile(Scene scene)
        {
            var card = new Border
            {
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(10),
                Margin = new Thickness(7, 0, 7, 14),
                Cursor = Cursors.Hand,
                Tag = scene.Id,
                ToolTip = scene.Id,
            };
            var stack = new StackPanel();

            // The frame across the head of the card, with the speed tag sitting on it. A Grid rather than a Border
            // so the tag can lie over the picture: Stretch.Uniform on a 16:9 source still means the height follows
            // the width on its own, whatever the column count is doing - no SizeChanged arithmetic to keep in step.
            var shot = new Grid { Background = Widgets.Brush("#111114"), ClipToBounds = true };
            ImageSource img = Thumbs.Get(scene.Id, 800);
            if (img != null)
                shot.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform });
            else shot.Height = 120;             // a build with no thumbnails in it keeps the shape of the card
            Border tag = SpeedTag(SpeedOf(scene.Id));
            tag.ToolTip = SpeedWhy(scene.Id);
            shot.Children.Add(tag);
            RenderOptions.SetBitmapScalingMode(shot, BitmapScalingMode.HighQuality);
            shot.SizeChanged += (s, e) => RoundClip((FrameworkElement)s, 10, true);
            stack.Children.Add(shot);

            var text = new StackPanel { Margin = new Thickness(16, 13, 16, 15) };
            text.Children.Add(Widgets.Text(string.IsNullOrEmpty(scene.Name) ? scene.Id : scene.Name, 14, "#ECECEE", true));
            TextBlock sub = Widgets.Text(string.IsNullOrEmpty(scene.Description) ? scene.Note : scene.Description,
                                         11, "#97979F");
            sub.Margin = new Thickness(0, 5, 0, 0);
            sub.LineHeight = 17;
            text.Children.Add(sub);
            stack.Children.Add(text);
            card.Child = stack;

            // Picked on a click, started on a double - the same two gestures the scene list takes, so the tab
            // behaves the same whichever half of it is showing.
            card.MouseLeftButtonUp += (s, e) => PickStart(((Border)s).Tag as string);
            card.MouseLeftButtonDown += (s, e) =>
            {
                if (e.ClickCount < 2) return;
                PickStart(((Border)s).Tag as string);
                Launch();
            };
            card.MouseEnter += (s, e) => { _hoverTile = (Border)s; PaintStartTiles(); };
            card.MouseLeave += (s, e) => { if (_hoverTile == s) _hoverTile = null; PaintStartTiles(); };
            return card;
        }

        // Picked is the accent and a lifted card; hovered is halfway there. Done here rather than in a style
        // because "picked" is _pickedId, which the other view and the command line can both move.
        void PaintStartTiles()
        {
            foreach (Border c in _startTiles)
            {
                bool on = string.Equals(c.Tag as string, _pickedId, StringComparison.OrdinalIgnoreCase);
                bool hot = c == _hoverTile;
                c.BorderBrush = Widgets.Brush(on ? "#7C9CFF" : hot ? "#3A3A44" : "#26262A");
                c.Background = Widgets.Brush(on ? "#191A21" : hot ? "#191919" : "#151517");
            }
        }

        void PickStart(string id)
        {
            if (string.IsNullOrEmpty(id)) return;
            _pickedId = id;
            ShowPicked(Catalog.Find(id));
            SavePrefs();
        }

        // "Skip the boot prompts" is the one automation this view offers, and it means something on exactly one of
        // the three: the title-screen boot, which is the game starting itself and has a "press any button" in the
        // way. The other two open a menu, where a press picks an entry rather than getting past a prompt - the
        // runner refuses it there for the same reason (Runner.Run).
        void UpdateStartAdvance()
        {
            bool prompts = !Catalog.IsMenuEntry(_pickedId);
            _startAdvance.IsEnabled = prompts;
            _startAdvanceNote.Text = prompts
                ? "Holds the game's hand through the logos and the \"press any button\" screen, up to the title. Off - which is how it comes - the launcher starts the game and leaves it alone."
                : "Nothing to skip here: this one opens on a menu, where a button press picks an entry rather than getting past a prompt.";
        }

        // ------------------------------------------------------------------------------------------- the veil

        void Confirm(string title, string body, string okLabel, Action onOk)
        {
            _veilTitle.Text = title;
            _veilBody.Text = body;
            _veilOkBtn.Content = okLabel;
            _veilAction = onOk;
            _veil.Visibility = Visibility.Visible;
            _veilOkBtn.Focus();         // Enter answers it and Escape backs out (WireKeys); the ring says which
        }

        void CloseVeil(bool run)
        {
            _veil.Visibility = Visibility.Collapsed;
            Action then = _veilAction;
            _veilAction = null;
            if (run && then != null) then();
        }
    }
}
