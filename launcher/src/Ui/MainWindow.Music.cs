// Playing the menu music. Music.cs finds and decodes it; this is when it starts, when it stops, how loud, what
// comes next, and the deck in the bottom bar that shows all of that.
//
// Two rules the rest of the window relies on: the game gets the speakers to itself - the music stops the moment a
// run is started and comes back when the game is gone - and nothing here ever blocks the window. Finding a track
// means running a decoder, so that happens on a pool thread and only the playing comes back to the UI thread.
//
// What plays is a *queue*: the order the tracks will go in, worked out ahead of time and walked with Next and
// Back. Shuffle's queue is the whole iPod dealt once - every track is heard before any comes round again, and a
// fresh deal is only cut when the last card has been played. Playlist's queue is the list as it was written,
// and goes round. A single picked track has no queue at all: it loops, and its deck is pause alone.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        MediaPlayer _music;
        string _musicPlaying;        // the track on the deck now, so a poll does not restart it every 1.5s
        bool _musicBusy;             // a decode is in flight; a second one would fight it for the same file
        bool _musicPaused;

        // The queue, the mode it was built for, and where in it the deck is. Rebuilt when the mode changes or the
        // playlist is edited; dealt again when Shuffle runs off the end.
        List<string> _queue;
        string _queueMode;
        int _queueAt = -1;

        StackPanel _transport;
        Button _musicBackBtn, _musicPauseBtn, _musicNextBtn;
        TextBlock _musicLabel, _musicAt, _musicLen;
        Slider _scrub;
        bool _scrubbing;             // the slider is being moved by the timer, not the hand, so its change is not a seek
        DispatcherTimer _deckTimer;
        const double BackRestarts = 3.0;

        // ------------------------------------------------------------------------------------------ the deck

        // The deck: the track's title, the three buttons under it the way iTunes draws them - bare glyphs, the
        // play one larger - and under those the scrubber with the time either side of it.
        void WireMusic()
        {
            _transport = (StackPanel)Win.FindName("Transport");
            var deck = (Style)Win.FindResource("Deck");
            _musicBackBtn = DeckButton("", 12, "Back to the start of this track - or, within three seconds of it starting, to the one before", (s, e) => MusicBack(), deck);
            _musicPauseBtn = DeckButton("", 17, "Pause", (s, e) => MusicPause(), deck);
            _musicNextBtn = DeckButton("", 12, "Next track", (s, e) => MusicNext(), deck);

            _musicLabel = Widgets.Text("", 11, "#ECECEE");
            _musicLabel.HorizontalAlignment = HorizontalAlignment.Center;
            _musicLabel.TextAlignment = TextAlignment.Center;
            _musicLabel.TextWrapping = TextWrapping.NoWrap;
            _musicLabel.TextTrimming = TextTrimming.CharacterEllipsis;
            _musicLabel.MaxWidth = 300;

            var row = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 1, 0, 1) };
            row.Children.Add(_musicBackBtn);
            row.Children.Add(_musicPauseBtn);
            row.Children.Add(_musicNextBtn);

            // The scrubber. The timer moves it as the track plays; a hand on it seeks. The two are told apart by
            // the flag the timer raises round its own writes.
            _scrub = new Slider { Style = (Style)Win.FindResource("Scrub"), Minimum = 0, Maximum = 1, VerticalAlignment = VerticalAlignment.Center };
            _scrub.ValueChanged += (s, e) => { if (!_scrubbing) Seek(_scrub.Value); };
            _musicAt = Widgets.Text("0:00", 10, "#8A8A96", false, true);
            _musicLen = Widgets.Text("0:00", 10, "#8A8A96", false, true);
            _musicAt.VerticalAlignment = _musicLen.VerticalAlignment = VerticalAlignment.Center;
            _musicAt.Margin = new Thickness(0, 0, 8, 0);
            _musicLen.Margin = new Thickness(8, 0, 0, 0);
            var line = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center };
            line.Children.Add(_musicAt);
            line.Children.Add(_scrub);
            line.Children.Add(_musicLen);

            _transport.Children.Add(_musicLabel);
            _transport.Children.Add(row);
            _transport.Children.Add(line);

            _deckTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
            _deckTimer.Tick += (s, e) => TickDeck();

            WirePlaylistEditor();
        }

        static Button DeckButton(string glyph, double size, string tip, RoutedEventHandler onClick, Style style)
        {
            var b = new Button { Content = glyph, Style = style, FontSize = size, ToolTip = tip, VerticalAlignment = VerticalAlignment.Center };
            b.Click += onClick;
            return b;
        }

        void PaintTransport()
        {
            if (_transport == null) return;
            bool show = _musicPlaying != null;
            _transport.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            if (!show) { _deckTimer.Stop(); return; }
            bool queued = _queue != null && Music.IsQueued(MusicSetting());
            _musicBackBtn.Visibility = _musicNextBtn.Visibility = queued ? Visibility.Visible : Visibility.Collapsed;
            _musicPauseBtn.Content = _musicPaused ? "" : "";
            _musicPauseBtn.ToolTip = _musicPaused ? "Play" : "Pause";
            _musicLabel.Text = Music.Title(_musicPlaying);
            _musicNextBtn.IsEnabled = !_musicBusy;
            // Back has somewhere to go when the track is past its first seconds (to its start) or when there is
            // a track before it in the queue. At the head of the queue, freshly started, it has nothing - and
            // says so by going grey.
            _musicBackBtn.IsEnabled = !_musicBusy && (Position() > BackRestarts || _queueAt > 0);
            if (!_deckTimer.IsEnabled) _deckTimer.Start();
            TickDeck();
        }

        double Position()
        {
            try { return _music != null ? _music.Position.TotalSeconds : 0; } catch { return 0; }
        }

        double Length()
        {
            try { return _music != null && _music.NaturalDuration.HasTimeSpan ? _music.NaturalDuration.TimeSpan.TotalSeconds : 0; }
            catch { return 0; }
        }

        static string Clock(double s)
        {
            if (s < 0 || double.IsNaN(s)) s = 0;
            int m = (int)(s / 60), r = (int)(s % 60);
            return m + ":" + r.ToString("00");
        }

        void TickDeck()
        {
            if (_music == null || _musicPlaying == null) return;
            double at = Position(), len = Length();
            _musicAt.Text = Clock(at);
            _musicLen.Text = Clock(len);
            _scrubbing = true;
            try
            {
                _scrub.Maximum = len > 0 ? len : 1;
                if (!_scrub.IsMouseCaptureWithin) _scrub.Value = Math.Min(at, _scrub.Maximum);
            }
            finally { _scrubbing = false; }
            bool canBack = !_musicBusy && (at > BackRestarts || _queueAt > 0);
            if (_musicBackBtn.IsEnabled != canBack) _musicBackBtn.IsEnabled = canBack;
        }

        void Seek(double seconds)
        {
            if (_music == null || _musicPlaying == null) return;
            try { _music.Position = TimeSpan.FromSeconds(seconds); } catch { }
            _musicAt.Text = Clock(seconds);
        }

        // ------------------------------------------------------------------------------------------ the queue

        void MusicPause()
        {
            if (_music == null || _musicPlaying == null) return;
            try
            {
                if (_musicPaused) _music.Play(); else _music.Pause();
                _musicPaused = !_musicPaused;
            }
            catch { }
            PaintTransport();
        }

        // The queue for a mode, or null for a mode that has none (a single track, or nothing at all).
        List<string> BuildQueue(string mode, string avoidFirst)
        {
            if (Music.IsShuffle(mode)) return Music.Shuffled(Music.Tracks(_gameDir), avoidFirst);
            if (Music.IsPlaylist(mode))
            {
                List<string> list = Music.ReadPlaylist(_gameDir);
                if (list.Count == 0) return null;
                return Music.IsPlaylistShuffle(mode) ? Music.Shuffled(list, avoidFirst) : list;
            }
            return null;
        }

        void MusicNext()
        {
            if (_musicBusy) return;
            string mode = MusicSetting();
            if (!Music.IsQueued(mode)) return;
            if (_queue == null || _queueMode != mode)
            {
                _queue = BuildQueue(mode, _musicPlaying);
                _queueMode = mode;
                _queueAt = -1;
            }
            if (_queue == null || _queue.Count == 0)
            {
                Say("the playlist is empty - Settings, Launcher, Playlist");
                return;
            }
            if (_queueAt + 1 >= _queue.Count)
            {
                // Off the end. Shuffle deals again, with the first card of the new deal never the one just
                // heard; a playlist simply goes round.
                if (Music.IsShuffle(mode) || Music.IsPlaylistShuffle(mode)) _queue = BuildQueue(mode, _musicPlaying);
                _queueAt = -1;
            }
            _queueAt++;
            StartTrack(_queue[_queueAt]);
        }

        void MusicBack()
        {
            if (_musicBusy || _music == null) return;
            if (Position() > BackRestarts || _queue == null || _queueAt <= 0)
            {
                try { _music.Position = TimeSpan.Zero; if (_musicPaused) MusicPause(); } catch { }
                PaintTransport();
                return;
            }
            _queueAt--;
            StartTrack(_queue[_queueAt]);
        }

        // The playlist was edited: the queue is stale. Rebuilt on the spot when the deck is on it, so what plays
        // next is what the list now says - kept in step with the track playing where it is still in the list.
        void PlaylistChanged()
        {
            string mode = MusicSetting();
            if (!Music.IsPlaylist(mode)) { _queue = null; return; }
            _queue = BuildQueue(mode, null);
            _queueMode = mode;
            _queueAt = _queue != null && _musicPlaying != null ? _queue.IndexOf(_musicPlaying) : -1;
            PaintTransport();
        }

        // Decode off the window's thread, play when it lands. Whatever was playing keeps going until then.
        void StartTrack(string track)
        {
            if (string.IsNullOrEmpty(track)) return;
            _musicBusy = true;
            PaintTransport();
            string dir = _gameDir;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string wav = null;
                try { wav = Music.Wav(dir, track); } catch { }
                Win.Dispatcher.BeginInvoke(new Action(delegate { PlayDecoded(track, wav); }));
            });
        }

        static string MusicSetting() { return Paths.Setting("MGS4_MUSIC", Music.Off) ?? Music.Off; }

        static double MusicVolume()
        {
            double v;
            if (!double.TryParse(Paths.Setting("MGS4_MUSIC_VOLUME", "35"), out v)) v = 35;
            return Math.Max(0, Math.Min(100, v)) / 100.0;
        }

        /// <summary>Read the setting and make the deck agree with it. Called on the way up, whenever Settings is
        /// saved, and whenever the game starts or stops.</summary>
        void ApplyMusic()
        {
            string want = MusicSetting();
            bool off = string.Equals(want, Music.Off, StringComparison.OrdinalIgnoreCase);
            // The playlist editor samples tracks whatever the setting says; the game still wins the speakers.
            bool silent = (off && !PlaylistOpen)
                          || _gameUp                                  // the game owns the speakers while it runs
                          || (_runProc != null && !_runProc.HasExited);

            if (silent) { StopMusic(); return; }
            if (_music != null) _music.Volume = MusicVolume();          // a volume change alone need not restart it
            if (_musicPlaying != null || _musicBusy) return;            // already playing, or on its way
            if (off) return;                                            // the editor is open with nothing sampled yet

            // A queued mode draws once per silence rather than per call, or a poll would pick a new song every tick.
            if (Music.IsQueued(want)) { MusicNext(); return; }
            StartTrack(want);
        }

        void PlayDecoded(string track, string wav)
        {
            _musicBusy = false;
            PaintTransport();
            if (wav == null)
            {
                // Nothing to play, and nothing to shout about: the Settings row already says whether there is a
                // decoder, and a missing bank is the person's own install.
                Say(Music.HaveDecoder()
                    ? "could not decode " + Music.Title(track)
                    : "music needs a decoder - Settings, Launcher, Menu music says how");
                return;
            }
            // The setting may have been turned off, or the game started, while the decode was running.
            if ((string.Equals(MusicSetting(), Music.Off, StringComparison.OrdinalIgnoreCase) && !PlaylistOpen) || _gameUp) return;

            if (_music == null)
            {
                _music = new MediaPlayer();
                // MGS4's tracks run three and a half minutes and a menu can outlast them: a picked track goes
                // round again, and a queue moves on to what is next in it.
                _music.MediaEnded += (s, e) =>
                {
                    if (Music.IsQueued(MusicSetting())) { MusicNext(); return; }
                    try { _music.Position = TimeSpan.Zero; _music.Play(); } catch { }
                };
                _music.MediaOpened += (s, e) => TickDeck();
            }
            try
            {
                _music.Open(new Uri(wav));
                _music.Volume = MusicVolume();
                _music.Play();
                _musicPlaying = track;
                _musicPaused = false;
                Say("music: " + Music.Title(track));
            }
            catch { _musicPlaying = null; }
            PaintTransport();
        }

        void StopMusic()
        {
            if (_music == null) return;
            try { _music.Stop(); _music.Close(); } catch { }
            _musicPlaying = null;
            _musicPaused = false;
            PaintTransport();
        }

        // ------------------------------------------------------------------------------------ the playlist editor

        Border _playlistVeil, _allTracksBox, _playlistBox;
        ListBox _allTracks, _playlistTracks;
        TextBlock _allTracksHead, _playlistHead, _playlistSummary;
        Button _plAddBtn, _plSampleBtn, _plRemoveBtn, _plClearBtn, _plDoneBtn, _plFavBtn;
        ObservableCollection<TrackItem> _playlist = new ObservableCollection<TrackItem>();
        bool _playlistOnRight;       // which list the keyboard and the pad are in

        public bool PlaylistOpen { get { return _playlistVeil != null && _playlistVeil.Visibility == Visibility.Visible; } }

        void WirePlaylistEditor()
        {
            Func<string, object> f = n => Win.FindName(n);
            _playlistVeil = (Border)f("PlaylistVeil");
            _allTracksBox = (Border)f("AllTracksBox");
            _playlistBox = (Border)f("PlaylistBox");
            _allTracks = (ListBox)f("AllTracks");
            _playlistTracks = (ListBox)f("PlaylistTracks");
            _allTracksHead = (TextBlock)f("AllTracksHead");
            _playlistHead = (TextBlock)f("PlaylistHead");
            _plAddBtn = (Button)f("PlAddBtn");
            _plSampleBtn = (Button)f("PlSampleBtn");
            _plRemoveBtn = (Button)f("PlRemoveBtn");
            _plClearBtn = (Button)f("PlClearBtn");
            _plDoneBtn = (Button)f("PlDoneBtn");
            _plFavBtn = (Button)f("PlFavBtn");
            _playlistTracks.ItemsSource = _playlist;
            _plFavBtn.Click += (s, e) => PlaylistFavorite();
            // The heart is its own click: hearting a track must not also pick it.
            _allTracks.PreviewMouseLeftButtonDown += (s, e) =>
            {
                if (!HitTheHeart(e.OriginalSource)) return;
                DependencyObject d = e.OriginalSource as DependencyObject;
                while (d != null && !(d is ListBoxItem)) d = VisualTreeHelper.GetParent(d);
                var item = d as ListBoxItem;
                var t = item != null ? item.DataContext as TrackItem : null;
                if (t != null) { ToggleMusicFavorite(t.File); e.Handled = true; }
            };

            _plAddBtn.Click += (s, e) => PlaylistAdd();
            _plRemoveBtn.Click += (s, e) => PlaylistRemove();
            _plSampleBtn.Click += (s, e) => PlaylistSample();
            _plClearBtn.Click += (s, e) => { _playlist.Clear(); PaintPlaylistHeads(); };
            _plDoneBtn.Click += (s, e) => ClosePlaylist();
            _allTracks.MouseDoubleClick += (s, e) => PlaylistAdd();
            _playlistTracks.MouseDoubleClick += (s, e) => PlaylistRemove();
            _allTracks.GotFocus += (s, e) => SetPlaylistSide(false);
            _playlistTracks.GotFocus += (s, e) => SetPlaylistSide(true);
            _allTracks.PreviewMouseLeftButtonDown += (s, e) => SetPlaylistSide(false);
            _playlistTracks.PreviewMouseLeftButtonDown += (s, e) => SetPlaylistSide(true);
        }

        void OpenPlaylist()
        {
            FillIpod(null);
            _playlist.Clear();
            foreach (string t in Music.ReadPlaylist(_gameDir)) _playlist.Add(new TrackItem(t));
            PaintPlaylistHeads();
            _playlistVeil.Visibility = Visibility.Visible;
            SetPlaylistSide(false);
            if (_allTracks.Items.Count > 0 && _allTracks.SelectedIndex < 0) _allTracks.SelectedIndex = 0;
            _allTracks.Focus();
            PaintGuide();
        }

        // Done: the list is written to config.ini on the spot - it is a thing of its own rather than a row on the
        // form, so it does not wait on Save - and the deck is told.
        void ClosePlaylist()
        {
            _playlistVeil.Visibility = Visibility.Collapsed;
            var files = new List<string>();
            foreach (TrackItem t in _playlist) files.Add(t.File);
            try
            {
                Music.WritePlaylist(files);
                Say(files.Count == 0 ? "playlist cleared" : "playlist kept: " + files.Count + " track" + (files.Count == 1 ? "" : "s"));
            }
            catch (Exception e) { Say("could not write the playlist: " + e.Message); }
            if (_playlistSummary != null) _playlistSummary.Text = PlaylistSummary();
            if (!Changed()) BuildSettings();     // the Menu music list is built with the form; hearts may have moved
            PlaylistChanged();
            ApplyMusic();           // a sample playing under an Off setting stops here
            PaintGuide();
        }

        // The iPod list, favourites first. Rebuilt whenever a heart changes, keeping the pick where it was.
        void FillIpod(string keep)
        {
            var all = new List<TrackItem>();
            foreach (string t in Music.Ordered(Music.Tracks(_gameDir))) all.Add(new TrackItem(t));
            _allTracks.ItemsSource = all;
            if (keep != null)
            {
                TrackItem hit = all.Find(t => string.Equals(t.File, keep, StringComparison.OrdinalIgnoreCase));
                if (hit != null) { _allTracks.SelectedItem = hit; _allTracks.ScrollIntoView(hit); }
            }
        }

        static bool HitTheHeart(object source)
        {
            var d = source as DependencyObject;
            while (d != null)
            {
                var fe = d as FrameworkElement;
                if (fe != null && (fe.Tag as string) == "heart") return true;
                if (d is ListBoxItem) return false;
                d = VisualTreeHelper.GetParent(d);
            }
            return false;
        }

        // A heart on or off a track: remembered at once, and every list that shows the iPod re-sorted, the
        // favourites at its head. The playlist keeps its own order - hearts only change how its rows read.
        void ToggleMusicFavorite(string file)
        {
            if (string.IsNullOrEmpty(file)) return;
            if (!Music.Favorites.Remove(file)) Music.Favorites.Add(file);
            SavePrefs();
            FillIpod(file);
            for (int i = 0; i < _playlist.Count; i++)
                if (string.Equals(_playlist[i].File, file, StringComparison.OrdinalIgnoreCase)) _playlist[i] = new TrackItem(file);
            Say(Music.Favorites.Contains(file)
                ? Music.Title(file) + " hearted (" + Music.Favorites.Count + ")"
                : Music.Title(file) + " un-hearted (" + Music.Favorites.Count + ")");
        }

        void PlaylistFavorite()
        {
            var t = (_playlistOnRight ? _playlistTracks.SelectedItem : _allTracks.SelectedItem) as TrackItem;
            if (t != null) ToggleMusicFavorite(t.File);
        }

        void SetPlaylistSide(bool right)
        {
            _playlistOnRight = right;
            _allTracksBox.BorderBrush = Widgets.Brush(right ? "#26262A" : "#7C9CFF");
            _playlistBox.BorderBrush = Widgets.Brush(right ? "#7C9CFF" : "#26262A");
        }

        void PaintPlaylistHeads()
        {
            int n = _allTracks.Items.Count;
            _allTracksHead.Text = "The iPod  ·  " + n + " track" + (n == 1 ? "" : "s");
            _playlistHead.Text = _playlist.Count == 0 ? "Playlist  ·  empty"
                               : "Playlist  ·  " + _playlist.Count + " track" + (_playlist.Count == 1 ? "" : "s") + ", in this order";
        }

        void PlaylistAdd()
        {
            var t = _allTracks.SelectedItem as TrackItem;
            if (t == null) return;
            _playlist.Add(new TrackItem(t.File));
            _playlistTracks.ScrollIntoView(_playlist[_playlist.Count - 1]);
            PaintPlaylistHeads();
            // The pick walks on, so adding a run of tracks is a run of presses.
            if (_allTracks.SelectedIndex < _allTracks.Items.Count - 1) _allTracks.SelectedIndex++;
        }

        void PlaylistRemove()
        {
            int i = _playlistTracks.SelectedIndex;
            if (i < 0) return;
            _playlist.RemoveAt(i);
            if (_playlist.Count > 0) _playlistTracks.SelectedIndex = Math.Min(i, _playlist.Count - 1);
            PaintPlaylistHeads();
        }

        // Play the picked track on the deck now. Whatever mode is set carries on from it when it ends.
        void PlaylistSample()
        {
            var t = (_playlistOnRight ? _playlistTracks.SelectedItem : _allTracks.SelectedItem) as TrackItem;
            if (t == null) return;
            if (_gameUp) { Say("the game has the speakers"); return; }
            StartTrack(t.File);
        }

        // The keyboard in the editor: arrows walk, Tab and Left/Right cross, Enter adds, Delete removes, Space
        // samples, Escape is Done. Returns true when the key was the editor's.
        bool PlaylistKey(Key key)
        {
            if (!PlaylistOpen) return false;
            ListBox on = _playlistOnRight ? _playlistTracks : _allTracks;
            switch (key)
            {
                case Key.Escape: ClosePlaylist(); return true;
                case Key.Enter: if (_playlistOnRight) PlaylistRemove(); else PlaylistAdd(); return true;
                case Key.Delete: PlaylistRemove(); return true;
                case Key.Space: PlaylistSample(); return true;
                case Key.F: PlaylistFavorite(); return true;
                case Key.Left: SetPlaylistSide(false); _allTracks.Focus(); return true;
                case Key.Right: SetPlaylistSide(true); _playlistTracks.Focus(); return true;
                case Key.Up: StepList(on, -1); return true;
                case Key.Down: StepList(on, 1); return true;
            }
            return false;
        }

        static void StepList(ListBox list, int dir)
        {
            if (list.Items.Count == 0) return;
            int i = Math.Max(0, Math.Min(list.Items.Count - 1, list.SelectedIndex + dir));
            list.SelectedIndex = i;
            list.ScrollIntoView(list.Items[i]);
        }

        // A controller in the editor: the same moves on the d-pad, A adds or removes, X samples, B or Start is Done.
        void PadPlaylist(PadButton b, bool repeat)
        {
            ListBox on = _playlistOnRight ? _playlistTracks : _allTracks;
            if (b == PadButton.Up) StepList(on, -1);
            else if (b == PadButton.Down) StepList(on, 1);
            else if (repeat) return;
            else if (b == PadButton.Left) SetPlaylistSide(false);
            else if (b == PadButton.Right) SetPlaylistSide(true);
            else if (b == PadButton.A) { if (_playlistOnRight) PlaylistRemove(); else PlaylistAdd(); }
            else if (b == PadButton.X) PlaylistSample();
            else if (b == PadButton.Y) PlaylistFavorite();
            else if (b == PadButton.B || b == PadButton.Start) ClosePlaylist();
        }

        // The row in the Launcher card: what the playlist holds, and the button that opens the editor.
        string PlaylistSummary()
        {
            List<string> list = Music.ReadPlaylist(_gameDir);
            if (list.Count == 0) return "No playlist yet. Pick tracks from the game's iPod; they play in the order you add them, when Menu music is set to Playlist.";
            var names = new List<string>();
            foreach (string t in list) { names.Add(Music.Title(t)); if (names.Count == 4) break; }
            string more = list.Count > names.Count ? " and " + (list.Count - names.Count) + " more" : "";
            return list.Count + " track" + (list.Count == 1 ? "" : "s") + ": " + string.Join(", ", names) + more + ".";
        }

        Border PlaylistRow()
        {
            Border row = Widgets.ActionRow(PlaylistSummary(), "Edit playlist...", "Pick and order the tracks Playlist plays",
                                           Music.Tracks(_gameDir).Count > 0, false, (s, e) => OpenPlaylist(), out _playlistSummary);
            return row;
        }

        // ------------------------------------------------------------------------------------ the decoder row

        // The row under the music settings, in the Launcher card: what is decoding this, or the offer to go and
        // get it. The download is a deliberate press rather than something the app does on its own the first time
        // it runs - it is this program reaching out to the internet, and that is the person's call to make.
        Border DecoderRow()
        {
            string exe = Music.Decoder();
            if (exe != null)
                return Widgets.ActionRow(
                    "Decoding with vgmstream at " + exe + ". Tracks are decoded out of your own install into " +
                    Music.CacheDir + " as they are picked; the four most recent are kept.",
                    "Open folder", Music.CacheDir, true, false,
                    (s, e) => Widgets.OpenFolder(Music.CacheDir));

            return Widgets.ActionRow(
                "MGS4 keeps its soundtrack as FMOD banks, and the audio inside them is Vorbis with the setup " +
                "headers stripped - ffmpeg cannot read it and nor can Windows. vgmstream can: about 4 MB, from " +
                "its own GitHub release, into " + Music.ToolsDir + ". None of the game's audio is copied anywhere " +
                "but your own machine.",
                "Download vgmstream", "https://github.com/vgmstream/vgmstream", !_decoderBusy, true,
                (s, e) => FetchDecoder());
        }

        bool _decoderBusy;

        void FetchDecoder()
        {
            if (_decoderBusy) return;
            _decoderBusy = true;
            Say("fetching vgmstream...");
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string err = null;
                try { Music.Fetch(m => Win.Dispatcher.BeginInvoke(new Action(delegate { Say(m); }))); }
                catch (Exception e) { err = e.Message; }
                Win.Dispatcher.BeginInvoke(new Action(delegate
                {
                    _decoderBusy = false;
                    if (err != null) { Say("could not fetch vgmstream: " + err); }
                    else Say("vgmstream is ready - pick a track in Menu music");
                    BuildSettings();        // the row is a different row now
                    ApplyMusic();
                }));
            });
        }
    }
}
