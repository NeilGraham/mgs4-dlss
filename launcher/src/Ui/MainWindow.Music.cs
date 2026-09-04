// Playing the menu music. Music.cs finds and decodes it; this is only when it starts, when it stops and how loud.
//
// Two rules the rest of the window relies on: the game gets the speakers to itself - the music stops the moment a
// run is started and comes back when the game is gone - and nothing here ever blocks the window. Finding a track
// means running a decoder, so that happens on a pool thread and only the playing comes back to the UI thread.
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Mgs4Launcher
{
    partial class MainWindow
    {
        MediaPlayer _music;
        string _musicPlaying;        // the track on the deck now, so a poll does not restart it every 1.5s
        bool _musicBusy;             // a decode is in flight; a second one would fight it for the same file

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
            bool silent = string.Equals(want, Music.Off, StringComparison.OrdinalIgnoreCase)
                          || _gameUp                                  // the game owns the speakers while it runs
                          || (_runProc != null && !_runProc.HasExited);

            if (silent) { StopMusic(); return; }
            if (_music != null) _music.Volume = MusicVolume();          // a volume change alone need not restart it
            if (_musicPlaying != null || _musicBusy) return;            // already playing, or on its way

            // Random is resolved once per run rather than per call, or a poll would pick a new song every tick.
            string track = string.Equals(want, Music.Random, StringComparison.OrdinalIgnoreCase)
                           ? Music.Pick(_gameDir) : want;
            if (string.IsNullOrEmpty(track)) return;

            _musicBusy = true;
            string dir = _gameDir;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string wav = null;
                try { wav = Music.Wav(dir, track); } catch { }
                Win.Dispatcher.BeginInvoke(new Action(delegate { PlayDecoded(track, wav); }));
            });
        }

        void PlayDecoded(string track, string wav)
        {
            _musicBusy = false;
            if (wav == null)
            {
                // Nothing to play, and nothing to shout about: the Settings row already says whether there is a
                // decoder, and a missing bank is the person's own install.
                Say(Music.HaveDecoder()
                    ? "could not decode " + Music.Pretty(track)
                    : "music needs a decoder - Settings, Launcher, Menu music says how");
                return;
            }
            // The setting may have been turned off, or the game started, while the decode was running.
            if (string.Equals(MusicSetting(), Music.Off, StringComparison.OrdinalIgnoreCase) || _gameUp) return;

            if (_music == null)
            {
                _music = new MediaPlayer();
                // MGS4's tracks run three and a half minutes and a menu can outlast them, so it goes round again.
                _music.MediaEnded += (s, e) =>
                {
                    try { _music.Position = TimeSpan.Zero; _music.Play(); } catch { }
                };
            }
            try
            {
                _music.Open(new Uri(wav));
                _music.Volume = MusicVolume();
                _music.Play();
                _musicPlaying = track;
                Say("music: " + Music.Pretty(track));
            }
            catch { _musicPlaying = null; }
        }

        void StopMusic()
        {
            if (_music == null) return;
            try { _music.Stop(); _music.Close(); } catch { }
            _musicPlaying = null;
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
