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
    static class Music
    {
        public const string Off = "off", Random = "random";

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
            return outp;
        }

        /// <summary>"Beyond_The_Bounds" as "Beyond The Bounds" - the file's own name, only readable.</summary>
        public static string Pretty(string track)
        {
            return string.IsNullOrEmpty(track) ? "" : track.Replace('_', ' ');
        }

        public static string BankOf(string gameDir, string track)
        {
            string dir = BankDir(gameDir);
            if (string.IsNullOrEmpty(dir) || string.IsNullOrEmpty(track)) return null;
            string p = Path.Combine(dir, track + ".bank");
            return Paths.Exists(p) ? p : null;
        }

        // What "Random" means: one of the named tracks, chosen per run. Seeded from the clock, so two launches in
        // the same minute do not sit on the same song.
        static readonly System.Random _dice = new System.Random();
        public static string Pick(string gameDir)
        {
            List<string> all = Tracks(gameDir);
            if (all.Count == 0) return null;
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
