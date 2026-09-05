// Menu music, played out of the game's own soundtrack.
//
// Where it comes from. MGS4 keeps its music in <GameDir>\common\bank\default as FMOD Studio banks, one track to a
// file and named for the track: At_Dawn.bank, Sea_Breeze.bank, Beyond_The_Bounds.bank. Inside each is a RIFF/FEV
// wrapper around an FSB5 block, and inside that the audio is Vorbis - but with FMOD's setup headers stripped out
// and replaced by a CRC naming which set of codebooks the stream was built against (0xc4c30a29 here, the same for
// every track). Without those codebooks not one byte decodes, which is why ffmpeg cannot open these: its `fsb`
// demuxer is FSB3/FSB4 and says "version 5 is not implemented".
//
// So the decoding is vgmstream's, which carries the codebook table. It is not shipped here - it is fetched from
// vgmstream's own GitHub release, once, on request, into %LOCALAPPDATA%, and the release stays the single exe it
// has always been.
//
// **Nothing of Konami's is copied into this repo or into a release.** The banks are read from the install the
// person already owns, decoded to a cache under their own %LOCALAPPDATA%, and played there - the same rule the
// window already follows for the key art and the game's icon. There is no path here that puts the game's audio
// anywhere it could be handed on.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    // A track as a list shows it. Top level and public, because WPF binds only to public members of public
    // types, and Music is neither.
    public class TrackItem
    {
        public string File { get; private set; }
        public string Label { get; private set; }
        public bool Favorite { get; private set; }
        // The heart at the row's end: filled for a favourite, an outline for the rest, one character wide either
        // way so the column holds still. Tag="heart" in the template is what a click on it is told apart by.
        public string Heart { get { return Favorite ? "♥" : "♡"; } }
        public string HeartInk { get { return Favorite ? "#F27E9A" : "#4A4A52"; } }
        public TrackItem(string file)
        {
            File = file;
            Favorite = Music.Favorites.Contains(file);
            Music.Known k = Music.Lookup(file);
            string name = k != null ? "★ " + k.Title + "  ·  " + k.From : Music.Pretty(file);
            Label = (Favorite ? "♥ " : "") + name;
        }
    }

    static class Music
    {
        // "random" is what the setting was called before it was Shuffle; a config.ini still saying so means the same.
        public const string Off = "off", Shuffle = "shuffle", OldShuffle = "random";

        // Playlist plays the list in config.ini in order; playlist-shuffle deals that list the way Shuffle deals
        // the whole iPod.
        public const string Playlist = "playlist", PlaylistShuffle = "playlist-shuffle";

        public static bool IsShuffle(string setting)
        {
            return string.Equals(setting, Shuffle, StringComparison.OrdinalIgnoreCase)
                || string.Equals(setting, OldShuffle, StringComparison.OrdinalIgnoreCase);
        }

        public static bool IsPlaylist(string setting)
        {
            return string.Equals(setting, Playlist, StringComparison.OrdinalIgnoreCase) || IsPlaylistShuffle(setting);
        }

        public static bool IsPlaylistShuffle(string setting)
        {
            return string.Equals(setting, PlaylistShuffle, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>A mode that walks a queue - anything but Off and a single named track.</summary>
        public static bool IsQueued(string setting) { return IsShuffle(setting) || IsPlaylist(setting); }

        // ------------------------------------------------------------------------------------ the playlist

        public const string PlaylistKey = "MGS4_PLAYLIST";

        /// <summary>The playlist as config.ini holds it - file names, comma-separated, in play order - kept to
        /// the tracks this install actually has.</summary>
        public static List<string> ReadPlaylist(string gameDir)
        {
            var outp = new List<string>();
            string raw = Paths.Setting(PlaylistKey, "");
            if (string.IsNullOrEmpty(raw)) return outp;
            var have = new HashSet<string>(Tracks(gameDir), StringComparer.OrdinalIgnoreCase);
            foreach (string part in raw.Split(','))
            {
                string t = part.Trim();
                if (t.Length > 0 && have.Contains(t)) outp.Add(t);
            }
            return outp;
        }

        public static void WritePlaylist(IEnumerable<string> files)
        {
            var values = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>(PlaylistKey, string.Join(",", files)),
            };
            Checks.SetIni(Paths.EnsureConfig(), values, null);
            Paths.ForgetConfig();
        }

        /// <summary>The tracks dealt into a random order, every one once. The first is never notThis - the track
        /// that just finished - so a fresh deal never plays the same song twice running.</summary>
        public static List<string> Shuffled(List<string> pool, string notThis)
        {
            var deck = new List<string>(pool);
            lock (_dice)
            {
                for (int i = deck.Count - 1; i > 0; i--)
                {
                    int j = _dice.Next(i + 1);
                    string t = deck[i]; deck[i] = deck[j]; deck[j] = t;
                }
                if (deck.Count > 1 && notThis != null && string.Equals(deck[0], notThis, StringComparison.OrdinalIgnoreCase))
                {
                    int j = 1 + _dice.Next(deck.Count - 1);
                    string t = deck[0]; deck[0] = deck[j]; deck[j] = t;
                }
            }
            return deck;
        }

        // The tracks someone has hearted, kept in launcher.json by MainWindow the way the scene favourites are.
        public static readonly HashSet<string> Favorites = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        /// <summary>The iPod in the order every list shows it: the favourites first, then the memorable tracks,
        /// then the rest - each group keeping the order it had.</summary>
        public static List<string> Ordered(List<string> tracks)
        {
            var rest = new List<string>(tracks);
            var ranked = new List<string>();
            foreach (Known k in Memorable)
            {
                int i = rest.FindIndex(t => string.Equals(t, k.File, StringComparison.OrdinalIgnoreCase));
                if (i < 0) continue;
                ranked.Add(rest[i]);
                rest.RemoveAt(i);
            }
            ranked.AddRange(rest);
            var outp = new List<string>();
            foreach (string t in ranked) if (Favorites.Contains(t)) outp.Add(t);
            foreach (string t in ranked) if (!Favorites.Contains(t)) outp.Add(t);
            return outp;
        }

        // vgmstream's own releases. The API is asked for the current one so this does not rot; the pinned URL is
        // what it falls back to when GitHub cannot be reached or answers with something unexpected.
        const string ReleaseApi = "https://api.github.com/repos/vgmstream/vgmstream/releases/latest";
        const string AssetName = "vgmstream-win64.zip";
        const string PinnedUrl = "https://github.com/vgmstream/vgmstream/releases/download/r2117/vgmstream-win64.zip";

        public static string ToolsDir { get { return Path.Combine(Paths.AppDataDir, "tools\\vgmstream"); } }
        public static string CacheDir { get { return Path.Combine(Paths.AppDataDir, "music"); } }

        // How many decoded tracks to keep. A decode is three tenths of a second and a WAV is 40 MB, so the cache is
        // a convenience rather than a saving - it is capped low on purpose.
        const int KeepTracks = 4;

        // ------------------------------------------------------------------------------------ the track list

        public static string BankDir(string gameDir)
        {
            return string.IsNullOrEmpty(gameDir) ? null : Paths.Join(gameDir, "common\\bank\\default");
        }

        // The named tracks, which is what a person would pick from. The same folder also holds the combat and
        // alert cues (bgm_*), the per-stage ambience (env_*) and FMOD's own project banks (Master*), none of which
        // is music you would sit a menu on.
        static bool IsMusic(string name)
        {
            if (Regex.IsMatch(name, "^(bgm_|sbgm_|env_|se_|vo_|sfx_)", RegexOptions.IgnoreCase)) return false;
            if (name.StartsWith("Master", StringComparison.OrdinalIgnoreCase)) return false;
            return true;
        }

        // The game's own score is not in the iPod's folder under any name a person would pick: it is filed as
        // bgm_* cues, and the cutscene music under ww\bank\default. Two of those are worth having on the deck -
        // the title screen's theme, which is what the main menu plays, and the full Love Theme from an Act 5
        // cutscene - so they are named here and looked for in both folders.
        static readonly string[] Extras = { "bgm_title_01", "E_bgm_hv_24demo_lovetheme" };
        static readonly string[] BankDirs = { "common\\bank\\default", "ww\\bank\\default" };

        public static List<string> Tracks(string gameDir)
        {
            var outp = new List<string>();
            string dir = BankDir(gameDir);
            if (string.IsNullOrEmpty(dir) || !Paths.Exists(dir)) return outp;
            try
            {
                foreach (string f in Directory.GetFiles(dir, "*.bank"))
                {
                    string name = Path.GetFileNameWithoutExtension(f);
                    if (IsMusic(name)) outp.Add(name);
                }
            }
            catch { }
            outp.Sort(StringComparer.OrdinalIgnoreCase);
            foreach (string x in Extras)
                if (BankOf(gameDir, x) != null) outp.Add(x);
            return outp;
        }

        /// <summary>"Beyond_The_Bounds" as "Beyond The Bounds" - the file's own name, only readable.</summary>
        public static string Pretty(string track)
        {
            return string.IsNullOrEmpty(track) ? "" : track.Replace('_', ' ');
        }

        // The banks are the iPod's playlist, named the way the files are. These are the ones worth putting at
        // the head of the list, in this order: the game's own themes first, then the series' - each with the
        // title it is actually known by, and a word on where it is from. Anything not here is listed after
        // them under its file name, made readable.
        public class Known { public string File, Title, From; }
        public static readonly Known[] Memorable =
        {
            new Known { File = "bgm_title_01",               Title = "Title screen",                      From = "MGS4's title and main menu" },
            new Known { File = "E_bgm_hv_24demo_lovetheme",  Title = "Love Theme",                        From = "MGS4, in full, from an Act 5 cutscene" },
            new Known { File = "Love_Theme_hum_ver",         Title = "Love Theme (hum ver.)",             From = "MGS4's main theme, hummed" },
            new Known { File = "MGS4_Thema_Of_Love_SmaXvr",  Title = "Theme of Love (Smash Bros. ver.)",  From = "MGS4, Brawl's arrangement" },
            new Known { File = "Sea_Breeze",                 Title = "Sea Breeze",                        From = "MGS4" },
            new Known { File = "Everything_Begins",          Title = "Everything Begins",                 From = "MGS4" },
            new Known { File = "Father_and_Son",             Title = "Father and Son",                    From = "MGS4" },
            new Known { File = "Flowing_Destiny",            Title = "Flowing Destiny",                   From = "MGS4" },
            new Known { File = "Inori",                      Title = "Inori",                             From = "MGS4" },
            new Known { File = "At_Dawn",                    Title = "At Dawn",                           From = "MGS4" },
            new Known { File = "War_Has_Changed",            Title = "War Has Changed",                   From = "MGS4" },
            new Known { File = "Calling_To_The_Night",       Title = "Calling to the Night",              From = "Portable Ops" },
            new Known { File = "Snake_Eater",                Title = "Snake Eater",                       From = "MGS3" },
            new Known { File = "THE_BEST_IS_YET_TO_CO",      Title = "The Best Is Yet to Come",           From = "MGS1" },
            new Known { File = "MGSTheme_DocumentRemix",     Title = "Metal Gear Solid Main Theme (Document remix)", From = "MGS" },
            new Known { File = "THEME_OF_SOLID_SNAKE",       Title = "Theme of Solid Snake",              From = "Metal Gear 2" },
            new Known { File = "ZanzibarBreeze",             Title = "Zanzibar Breeze",                   From = "Metal Gear 2" },
            new Known { File = "Yell_dead_cell",             Title = "Yell \"Dead Cell\"",                From = "MGS2" },
            new Known { File = "MGS1_HIND_D",                Title = "Hind D",                            From = "MGS1" },
            new Known { File = "Beyond_The_Bounds",          Title = "Beyond the Bounds",                 From = "Zone of the Enders 2" },
            new Known { File = "One_Night_in_NEOKOBECITY",   Title = "One Night in Neo Kobe City",        From = "Snatcher" },
            new Known { File = "OPENING_TITLE_OLD_L",        Title = "Opening / Old L.A. 2040",           From = "Snatcher" },
            new Known { File = "POLICENAUTS_END_TITLE",      Title = "End Title",                         From = "Policenauts" },
        };

        public static Known Lookup(string track)
        {
            foreach (Known k in Memorable)
                if (string.Equals(k.File, track, StringComparison.OrdinalIgnoreCase)) return k;
            return null;
        }

        /// <summary>The title a track is known by, or its file name made readable.</summary>
        public static string Title(string track)
        {
            Known k = Lookup(track);
            return k != null ? k.Title : Pretty(track);
        }

        public static string BankOf(string gameDir, string track)
        {
            if (string.IsNullOrEmpty(gameDir) || string.IsNullOrEmpty(track)) return null;
            foreach (string d in BankDirs)
            {
                string p = Paths.Join(gameDir, d + "\\" + track + ".bank");
                if (Paths.Exists(p)) return p;
            }
            return null;
        }

        // What Shuffle means: one of the named tracks, never the one that just played. Seeded from the clock, so
        // two launches in the same minute do not sit on the same song.
        static readonly System.Random _dice = new System.Random();
        public static string Pick(string gameDir, string notThis = null)
        {
            List<string> all = Tracks(gameDir);
            if (all.Count == 0) return null;
            if (all.Count > 1 && notThis != null) all.RemoveAll(t => string.Equals(t, notThis, StringComparison.OrdinalIgnoreCase));
            lock (_dice) return all[_dice.Next(all.Count)];
        }

        // ------------------------------------------------------------------------------------ the decoder

        /// <summary>vgmstream-cli.exe: named in config.ini, downloaded into %LOCALAPPDATA%, or on PATH. Null when
        /// there is none, which is what makes the music setting say so rather than fail quietly.</summary>
        public static string Decoder()
        {
            string given = Paths.Setting("MGS4_VGMSTREAM", null);
            if (!string.IsNullOrEmpty(given) && Paths.Exists(given)) return given;

            string mine = Path.Combine(ToolsDir, "vgmstream-cli.exe");
            if (Paths.Exists(mine)) return mine;

            string path = Environment.GetEnvironmentVariable("PATH") ?? "";
            foreach (string dir in path.Split(';'))
            {
                if (dir.Trim().Length == 0) continue;
                try
                {
                    string p = Path.Combine(dir.Trim(), "vgmstream-cli.exe");
                    if (Paths.Exists(p)) return p;
                }
                catch { }               // a malformed PATH entry is not a reason to stop looking
            }
            return null;
        }

        public static bool HaveDecoder() { return Decoder() != null; }

        static string AssetUrl()
        {
            try
            {
                ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
                var wc = new WebClient();
                wc.Headers.Add("User-Agent", "mgs4-dlss-launcher");     // GitHub's API refuses a request without one
                var rel = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 }
                          .DeserializeObject(wc.DownloadString(ReleaseApi)) as Dictionary<string, object>;
                object assets;
                if (rel != null && rel.TryGetValue("assets", out assets) && assets is object[])
                    foreach (object a in (object[])assets)
                    {
                        var d = a as Dictionary<string, object>;
                        if (d == null) continue;
                        object n, u;
                        if (d.TryGetValue("name", out n) && d.TryGetValue("browser_download_url", out u) &&
                            string.Equals(n.ToString(), AssetName, StringComparison.OrdinalIgnoreCase))
                            return u.ToString();
                    }
            }
            catch { }
            return PinnedUrl;
        }

        /// <summary>Fetch vgmstream into %LOCALAPPDATA% and unpack it. Blocking - callers run it off the window's
        /// thread. Returns the decoder's path, or throws with something worth reading.</summary>
        public static string Fetch(Action<string> say)
        {
            string existing = Decoder();
            if (existing != null) return existing;

            string url = AssetUrl();
            if (say != null) say("downloading " + AssetName + "...");
            string zip = Path.Combine(Path.GetTempPath(), "vgmstream-win64.zip");
            try
            {
                ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
                using (var wc = new WebClient())
                {
                    wc.Headers.Add("User-Agent", "mgs4-dlss-launcher");
                    wc.DownloadFile(url, zip);
                }
                if (say != null) say("unpacking...");
                if (Directory.Exists(ToolsDir)) Directory.Delete(ToolsDir, true);
                Directory.CreateDirectory(ToolsDir);
                // Flattened: the zip has no folder of its own, and the exe wants its codec dlls beside it.
                using (ZipArchive z = ZipFile.OpenRead(zip))
                    foreach (ZipArchiveEntry e in z.Entries)
                    {
                        if (e.Name.Length == 0) continue;               // a directory entry
                        e.ExtractToFile(Path.Combine(ToolsDir, e.Name), true);
                    }
            }
            finally { try { File.Delete(zip); } catch { } }

            string got = Decoder();
            if (got == null) throw new IOException("downloaded, but no vgmstream-cli.exe came out of it");
            if (say != null) say("vgmstream is in " + ToolsDir);
            return got;
        }

        // ------------------------------------------------------------------------------------ decoding

        /// <summary>The track as a .wav under %LOCALAPPDATA%, decoding it first if it is not there. Null when
        /// there is no decoder, no such bank, or the decode failed - the caller simply stays quiet.</summary>
        public static string Wav(string gameDir, string track)
        {
            string bank = BankOf(gameDir, track), exe = Decoder();
            if (bank == null || exe == null) return null;

            string wav = Path.Combine(CacheDir, track + ".wav");
            try
            {
                if (Paths.Exists(wav) && new FileInfo(wav).Length > 1024) { Touch(wav); return wav; }
                Directory.CreateDirectory(CacheDir);

                // -i decodes one clean pass and ignores the bank's own loop-and-fade; the window loops it itself,
                // which is what a menu wants rather than vgmstream's two-times-then-fade default.
                var psi = new ProcessStartInfo(exe, "-i -o \"" + wav + "\" \"" + bank + "\"")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                using (Process p = Process.Start(psi))
                {
                    p.StandardOutput.ReadToEnd();
                    p.StandardError.ReadToEnd();
                    if (!p.WaitForExit(60000)) { try { p.Kill(); } catch { } return null; }
                    if (p.ExitCode != 0) return null;
                }
                if (!Paths.Exists(wav) || new FileInfo(wav).Length <= 1024) return null;
                Prune();
                return wav;
            }
            catch { return null; }
        }

        static void Touch(string file)
        {
            try { File.SetLastAccessTimeUtc(file, DateTime.UtcNow); } catch { }
        }

        // Oldest first out of the cache. A decode costs a third of a second, so keeping every track someone ever
        // tried - forty megabytes each, seventy-six of them - would be paying gigabytes to save nothing.
        static void Prune()
        {
            try
            {
                var files = new DirectoryInfo(CacheDir).GetFiles("*.wav")
                            .OrderByDescending(f => f.LastAccessTimeUtc).ToList();
                for (int i = KeepTracks; i < files.Count; i++)
                    try { files[i].Delete(); } catch { }
            }
            catch { }
        }
    }
}
