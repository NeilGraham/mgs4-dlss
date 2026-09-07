// The install check, as a library: which required and optional files are in place, what the settings say, and what
// the add-on reported on its last run. A port of tools/install_checks.ps1, reading the same
// tools/install_manifest.json - the file list, the verified versions and the download links live there, not here.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    class Row
    {
        public string Status, Name, Detail, Value, Url;
        public Row(string status, string name, string detail, string value, string url)
        { Status = status; Name = name; Detail = detail; Value = value; Url = url; }
    }

    class FileSpec
    {
        public string Path, Detail, Verified, Url;
        // Another name the same file has gone by - RenoDX's add-on was renodx-dlss5.addon64 before it was
        // renodx-dlss.addon64 - so either satisfies the check, and both at once is called out.
        public string Alt;
        public bool Optional, Required, OptionalIfDriverOverride;
        // The builds this add-on was verified with, by SHA-256 of the file, each with a label for the row. Only for
        // files whose version resource says nothing (RenoDX's add-on reports 0.0.0.0): any other file is an
        // untested build and is said so.
        public List<KeyValuePair<string, string>> Builds = new List<KeyValuePair<string, string>>();
    }

    class Section
    {
        public string Id, Title, Blurb, Url, UrlLabel, Guide, SavedSettings;
        public bool Required, Bundled;
        public List<string> Accepts = new List<string>();
        public List<string> Drop = new List<string>();
        public List<FileSpec> Files = new List<FileSpec>();
        public List<Row> Rows = new List<Row>();
        public List<string> WrongKeys = new List<string>();
    }

    class Verdict { public string Text, Kind, Note; }

    // What the add-on reported the last time the game ran. File presence cannot see any of this.
    class LastRun
    {
        public string LogPath, Addon, Ngx, DlssDll, NvngxProxy, Nr, Insertion, Fg, FgTarget, Reflex,
                      Sl, DlssG, FgSupported, Driver, DriverMin, Evaluations, Feature, NrRuntime, NrFeature;
        public bool FgNoStreamline, NrCreated, HasReShadeAddonSupport, ReShadeAddonSupport;
        // The current RenoDX build says where it ran NR: on this add-on's DLAA output (the contract), or on the
        // whole backbuffer inside frame generation or at present - which on a wide display takes in the
        // pillarbox, and reads the image as linear colour.
        public bool NrOnBackbuffer;
        public string NrSourceSize;
        // RenoDX's Hook Method as ReShade.ini has it, and the swapchain and DLSS output sizes of the run: NR on the
        // backbuffer is the same image as the DLAA output when the two are one size, and the pillarbox too when
        // the swapchain is wider.
        public string NrHook;
        public int SwapW, SwapH, OutW, OutH;
        // The current RenoDX build binds its NR runtime lazily. With Streamline in the process it has been seen
        // never getting there on its own - this many "BindDevice rejected unavailable runtime state" lines - until
        // a setting in its tab was changed.
        public int NrRejections;
        // Whether ReShade's init_device reached RenoDX at all - it never did for the game's own device in any run
        // here, so this add-on raises it once more with a WARP device (NrKick), and RenoDX attaches its runtime then.
        public bool NrInitDevice, NrKicked;
        public TimeSpan LogAge;
        public List<string> ReShadeAddons = new List<string>();
    }

    static class Checks
    {
        // The file list is embedded in the exe as well as living in tools\install_manifest.json, and the file wins
        // when it is there, so editing it in a checkout works the way it always has. A copy of the exe on its own -
        // dropped on a desktop, or put beside the game - has no tools folder next to it, and reading that path
        // blind is what took the whole window down the moment Setup was opened.
        public static string ManifestPath { get { return Path.Combine(Paths.Root, "tools\\install_manifest.json"); } }
        const string ManifestResource = "install_manifest.json";

        public static string ManifestSource = "";    // what was actually read, for the footer and the report
        public static string ManifestError = "";     // empty unless there is no file list at all

        static string ManifestText()
        {
            string text = Paths.DataText(ManifestResource, out ManifestSource);
            if (text == null)
                ManifestError = "no file list to check against: neither " + ManifestPath +
                                " nor a copy inside the launcher could be read.";
            return text;
        }

        // ------------------------------------------------------------------------------------------- helpers

        public static string PeVersion(string path)
        {
            if (!Paths.Exists(path)) return null;
            try
            {
                FileVersionInfo vi = FileVersionInfo.GetVersionInfo(path);
                string s = vi.ProductVersion;
                if (string.IsNullOrEmpty(s)) s = vi.FileVersion;
                return string.IsNullOrEmpty(s) ? null : s.Replace(',', '.').Trim();
            }
            catch { return null; }
        }

        // "310,8,0,0" and "310.8.0" both normalize to 310.8.0, so the manifest can be written the readable way.
        public static string NormalVersion(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            var parts = s.Replace(',', '.').Split('.').Where(p => Regex.IsMatch(p, "^\\d+$")).ToList();
            if (parts.Count == 0) return "";
            while (parts.Count < 3) parts.Add("0");
            return string.Join(".", parts.Take(3));
        }

        // 310.8.0.0 -> 310.8.0, but 1.0.0.1 and 0.2026.0827.2036 keep every group that says something.
        public static string FormatVersion(string s)
        {
            var p = (s ?? "").Split('.').ToList();
            while (p.Count > 3 && p[p.Count - 1] == "0") p.RemoveAt(p.Count - 1);
            return string.Join(".", p);
        }

        public static string Sha256(string path)
        {
            try
            {
                using (var h = System.Security.Cryptography.SHA256.Create())
                using (var s = File.OpenRead(path))
                    return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
            }
            catch { return null; }
        }

        public static string IniValue(string path, string key)
        {
            if (!Paths.Exists(path)) return null;
            try
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    Match m = Regex.Match(line, "^\\s*" + Regex.Escape(key) + "\\s*=\\s*(.*?)\\s*$");
                    if (m.Success) return m.Groups[1].Value;
                }
            }
            catch { }
            return null;
        }

        // In place, keeping the order and the comments. The add-on owns mgs4_dlss.ini while the game runs - its
        // writes go through the Windows profile API, whose cache will happily undo an outside edit - so every
        // caller checks that first.
        public static void SetIni(string path, IEnumerable<KeyValuePair<string, string>> values)
        {
            SetIni(path, values, null);
        }

        // With a section, only keys inside that [Section] are touched, and one is created at the end if the file
        // has none - ReShade.ini is a long file full of other people's sections, and a key like NRStyle must not be
        // matched wherever it happens to appear.
        public static void SetIni(string path, IEnumerable<KeyValuePair<string, string>> values, string section)
        {
            if (!Paths.Exists(path)) throw new IOException("no ini at " + path);
            var lines = File.ReadAllLines(path).ToList();
            var left = values.ToDictionary(v => v.Key, v => v.Value, StringComparer.OrdinalIgnoreCase);
            string current = null;
            int lastInSection = -1;
            for (int i = 0; i < lines.Count; i++)
            {
                Match head = Regex.Match(lines[i], @"^\s*\[(.+?)\]\s*$");
                if (head.Success) { current = head.Groups[1].Value; continue; }
                if (section != null && !string.Equals(current, section, StringComparison.OrdinalIgnoreCase)) continue;
                Match m = Regex.Match(lines[i], @"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=");
                if (!m.Success) continue;
                lastInSection = i;
                string k = m.Groups[1].Value;
                if (!left.ContainsKey(k)) continue;
                string comment = "";
                Match c = Regex.Match(lines[i], @"(\s+;.*)$");
                if (c.Success) comment = c.Groups[1].Value;
                lines[i] = k + "=" + left[k] + comment;
                left.Remove(k);
            }
            if (left.Count > 0 && section != null && lastInSection < 0)
            {
                lines.Add("[" + section + "]");
                lastInSection = lines.Count - 1;
            }
            foreach (var kv in left)
            {
                string line = kv.Key + "=" + kv.Value;
                if (section == null) lines.Add(line);
                else lines.Insert(++lastInSection, line);       // stay inside the section it belongs to
            }
            File.WriteAllLines(path, lines.ToArray(), new UTF8Encoding(false));
        }

        // What the game's own options have to say for the add-on to work.
        public static readonly List<KeyValuePair<string, string>> WantedGameSettings = new List<KeyValuePair<string, string>>
        {
            new KeyValuePair<string, string>("api", "dx12"),
            new KeyValuePair<string, string>("vsync", "false"),
            new KeyValuePair<string, string>("fpsLimiter", "60"),
            new KeyValuePair<string, string>("enableFXAA", "false"),
        };

        public static string SetGameSettings(string savedSettings)
        {
            if (!Paths.Exists(savedSettings)) throw new IOException("no mgs4.savedsettings to write to");
            SetIni(savedSettings, WantedGameSettings);
            return savedSettings;
        }

        public static string SavedSettingsPath(string game)
        {
            string root = Path.GetDirectoryName(game);                 // ...\METAL GEAR SOLID 4
            string saves = Paths.Join(root, "mgs4_savedata_win");
            if (!Paths.Exists(saves)) return null;
            try
            {
                string[] hits = Directory.GetFiles(saves, "mgs4.savedsettings", SearchOption.AllDirectories);
                return hits.Length > 0 ? hits[0] : null;
            }
            catch { return null; }
        }

        public static int RefreshRate()
        {
            try
            {
                using (var s = new System.Management.ManagementObjectSearcher(
                           "SELECT CurrentRefreshRate FROM Win32_VideoController"))
                    foreach (System.Management.ManagementObject o in s.Get())
                    {
                        object v = o["CurrentRefreshRate"];
                        if (v != null) return Convert.ToInt32(v);
                    }
            }
            catch { }
            return 0;
        }

        public static bool GameRunning()
        {
            try { return Process.GetProcessesByName("mgs4").Length > 0; } catch { return false; }
        }

        // ------------------------------------------------------------------------------------------- manifest

        static List<Section> _manifest;
        public static List<Section> Manifest()
        {
            if (_manifest != null) return _manifest;
            // An empty list is cached even when nothing could be read, so a missing file list is one message on the
            // Setup tab rather than the same exception thrown again by every caller.
            _manifest = new List<Section>();
            string json = ManifestText();
            if (json == null) return _manifest;

            var ser = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
            var root = (Dictionary<string, object>)ser.DeserializeObject(json);
            var list = new List<Section>();
            foreach (object secObj in (object[])root["sections"])
            {
                var d = (Dictionary<string, object>)secObj;
                var sec = new Section
                {
                    Id = Str(d, "id"),
                    Title = Str(d, "title"),
                    Blurb = Str(d, "blurb"),
                    Url = Str(d, "url"),
                    UrlLabel = Str(d, "urlLabel"),
                    Guide = Str(d, "guide"),
                    Required = Bool(d, "required"),
                    Bundled = Bool(d, "bundled"),
                };
                foreach (string a in StrList(d, "accepts")) sec.Accepts.Add(a);
                foreach (string a in StrList(d, "drop")) sec.Drop.Add(a);
                if (d.ContainsKey("files"))
                    foreach (object fObj in (object[])d["files"])
                    {
                        var f = (Dictionary<string, object>)fObj;
                        var fs = new FileSpec
                        {
                            Path = Str(f, "path"),
                            Alt = Str(f, "alt"),
                            Detail = Str(f, "detail"),
                            Verified = Str(f, "verified"),
                            Url = Str(f, "url"),
                            Optional = Bool(f, "optional"),
                            Required = Bool(f, "required"),
                            OptionalIfDriverOverride = Bool(f, "optionalIfDriverOverride"),
                        };
                        if (f.ContainsKey("builds"))
                            foreach (object bObj in (object[])f["builds"])
                            {
                                var b = (Dictionary<string, object>)bObj;
                                fs.Builds.Add(new KeyValuePair<string, string>(Str(b, "sha256").ToLowerInvariant(), Str(b, "label")));
                            }
                        sec.Files.Add(fs);
                    }
                list.Add(sec);
            }
            _manifest = list;
            return _manifest;
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v;
            return d.TryGetValue(k, out v) && v != null ? v.ToString() : null;
        }
        static bool Bool(Dictionary<string, object> d, string k)
        {
            object v;
            return d.TryGetValue(k, out v) && v is bool && (bool)v;
        }
        static List<string> StrList(Dictionary<string, object> d, string k)
        {
            var outp = new List<string>();
            object v;
            if (d.TryGetValue(k, out v) && v is object[])
                foreach (object o in (object[])v) if (o != null) outp.Add(o.ToString());
            return outp;
        }

        // ------------------------------------------------------------------------------------------- last run

        public static LastRun ReadLastRun(string game)
        {
            var r = new LastRun();
            string log = Paths.Join(game, "logs\\mgs4_dlss.log");
            if (!Paths.Exists(log)) return r;
            r.LogPath = log;
            r.LogAge = DateTime.Now - new FileInfo(log).LastWriteTime;
            r.NrHook = IniValue(Paths.Join(game, "ReShade.ini"), "DirectNeuralRenderingHookPoint");
            string[] text;
            try { text = File.ReadAllLines(log); } catch { return r; }
            foreach (string line in text)
            {
                Match m;
                if ((m = Regex.Match(line, "mgs4_dlss v(\\S+) registered")).Success) r.Addon = m.Groups[1].Value;
                if ((m = Regex.Match(line, "NGX D3D12 Init_with_ProjectID.*-> (\\S+)")).Success) r.Ngx = m.Groups[1].Value;
                if ((m = Regex.Match(line, "nvngx_dlss\\.dll (loaded|not loaded), _nvngx (\\S+), DLSS5 add-on (loaded|absent)")).Success)
                { r.DlssDll = m.Groups[1].Value; r.NvngxProxy = m.Groups[2].Value; r.Nr = m.Groups[3].Value; }
                if ((m = Regex.Match(line, "insertion: (.+?) \\(PrePost=")).Success) r.Insertion = m.Groups[1].Value;
                if (line.Contains("NrKick: WARP D3D12 device created")) r.NrKicked = true;
                if ((m = Regex.Match(line, "swapchain (?:created|resized): (\\d+)x(\\d+)")).Success) { r.SwapW = int.Parse(m.Groups[1].Value); r.SwapH = int.Parse(m.Groups[2].Value); }
                if ((m = Regex.Match(line, "injecting: .*-> output (\\d+)x(\\d+)")).Success) { r.OutW = int.Parse(m.Groups[1].Value); r.OutH = int.Parse(m.Groups[2].Value); }
                if ((m = Regex.Match(line, "frame generation: (.+?), target fps (\\d+), Reflex (\\d)")).Success)
                { r.Fg = m.Groups[1].Value; r.FgTarget = m.Groups[2].Value; r.Reflex = m.Groups[3].Value; }
                if (line.Contains("frame generation off at startup: Streamline not loaded")) r.FgNoStreamline = true;
                if ((m = Regex.Match(line, "Streamline initialized \\(SL ([\\d.]+) / DLSS-G ([\\d.]+)\\); DLSS-G supported: (\\w+)")).Success)
                { r.Sl = m.Groups[1].Value; r.DlssG = m.Groups[2].Value; r.FgSupported = m.Groups[3].Value; }
                if ((m = Regex.Match(line, "driver ([\\d.]+) detected / ([\\d.]+) required")).Success)
                { r.Driver = m.Groups[1].Value; r.DriverMin = m.Groups[2].Value; }
                if ((m = Regex.Match(line, "NGX EvaluateFeature ok \\(#(\\d+)\\)")).Success) r.Evaluations = m.Groups[1].Value;
                if ((m = Regex.Match(line, "NGX CreateFeature DLSS \\((.+?)\\)")).Success) r.Feature = m.Groups[1].Value;
            }
            string rl = Paths.Join(game, "ReShade.log");
            if (Paths.Exists(rl))
            {
                string[] rtext;
                try { rtext = File.ReadAllLines(rl); } catch { rtext = new string[0]; }
                r.HasReShadeAddonSupport = true;
                r.ReShadeAddonSupport = rtext.Any(l => l.Contains("Searching for add-ons"));
                foreach (string line in rtext)
                {
                    Match m = Regex.Match(line, "Registered add-on \"(.+?)\" v(\\S+)");
                    if (m.Success) r.ReShadeAddons.Add(m.Groups[1].Value);
                    // Whether Neural Rendering actually ran. RenoDX does NR through its own NGX hook (feature 18),
                    // not through Streamline, so this - not anything sl.dlss_nr says - is the signal.
                    m = Regex.Match(line, "signed DLSSNR ([\\d.]+) D3D12 runtime initialized");
                    if (m.Success) r.NrRuntime = m.Groups[1].Value;
                    m = Regex.Match(line, "feature 18 created .* for NR input (\\S+) -> output (\\S+)");
                    if (m.Success) r.NrFeature = m.Groups[1].Value;
                    if (line.Contains("NGX feature create intercepted: feature=18")) r.NrCreated = true;
                    // The current build (renodx-dlss.addon64, "DLSS-NR direct"): its own NGX feature, and a line
                    // per source it evaluated. A DLSS-G evaluate it stepped into is the backbuffer path.
                    m = Regex.Match(line, "CreateFeature\\(Reserved18\\) succeeded: handle=\\S+ size=(\\d+x\\d+)");
                    if (m.Success) { r.NrCreated = true; r.NrFeature = m.Groups[1].Value; }
                    if (Regex.IsMatch(line, "DLSS-NR direct: attached snippet .*nvngx_dlssnr")) { if (string.IsNullOrEmpty(r.NrRuntime)) r.NrRuntime = "direct"; }
                    m = Regex.Match(line, "source evaluation completed: source=\\d+ .*size=(\\d+x\\d+)");
                    if (m.Success) r.NrSourceSize = m.Groups[1].Value;
                    if (line.Contains("ngx_evaluate.dlssg_observed") || line.Contains("ngx_evaluate.dlssg_copyback") || line.Contains("source-role=Backbuffer"))
                        r.NrOnBackbuffer = true;
                    if (line.Contains("BindDevice rejected unavailable runtime state")) r.NrRejections++;
                    if (line.Contains("RenoDX DLSS init_device begin")) r.NrInitDevice = true;
                }
            }
            return r;
        }

        // ------------------------------------------------------------------------------------------- the checks

        public static List<Section> Run(string game)
        {
            var sections = new List<Section>();
            LastRun run = ReadLastRun(game);

            foreach (Section spec in Manifest())
            {
                var sec = new Section
                {
                    Id = spec.Id, Title = spec.Title, Blurb = spec.Blurb, Url = spec.Url, UrlLabel = spec.UrlLabel,
                    Guide = spec.Guide, Required = spec.Required, Bundled = spec.Bundled,
                    Accepts = spec.Accepts, Drop = spec.Drop, Files = spec.Files
                };
                foreach (FileSpec f in spec.Files)
                {
                    string full = Paths.Join(game, f.Path);
                    bool present = Paths.Exists(full);
                    string name = f.Path;
                    bool altPresent = !string.IsNullOrEmpty(f.Alt) && Paths.Exists(Paths.Join(game, f.Alt));
                    if (!present && altPresent) { full = Paths.Join(game, f.Alt); present = true; name = f.Alt; }
                    string detail = f.Detail, value = "not found", status;
                    if (present)
                    {
                        status = "ok";
                        string ver = PeVersion(full);
                        if (!string.IsNullOrEmpty(ver))
                        {
                            value = "v" + FormatVersion(ver);
                            string want = NormalVersion(f.Verified), have = NormalVersion(ver);
                            if (want.Length > 0 && have.Length > 0 && want != have)
                                value = "v" + FormatVersion(ver) + " (verified with " + f.Verified + ")";
                        }
                        else
                        {
                            long bytes = new FileInfo(full).Length;
                            value = bytes >= 1024
                                ? string.Format(CultureInfo.InvariantCulture, "{0:n0} KB", Math.Round(bytes / 1024.0))
                                : bytes + " bytes";
                        }
                        // A file whose version says nothing is known by its hash: one of the verified builds, or not.
                        if (f.Builds.Count > 0)
                        {
                            string sha = Sha256(full);
                            string label = sha == null ? null : f.Builds.Where(b => b.Key == sha).Select(b => b.Value).FirstOrDefault();
                            if (label != null) value = label;
                            else
                            {
                                status = "warn";
                                value = "untested build";
                                detail = "not one of the builds this add-on was verified with (" + string.Join("; ", f.Builds.Select(b => b.Value)) +
                                    "). RenoDX changes where it applies NR and how it starts between builds, so the last-run rows below say what this one did" +
                                    (sha != null ? ". SHA-256 " + sha.Substring(0, 12) : "") + ", " +
                                    string.Format(CultureInfo.InvariantCulture, "{0:n0} KB", Math.Round(new FileInfo(full).Length / 1024.0));
                            }
                        }
                    }
                    else
                    {
                        // A group says what its files are worth; a file can override that either way.
                        bool mustHave = f.Required || (spec.Required && !f.Optional);
                        status = mustHave ? "bad" : "warn";
                        // DLSS itself can come from the driver instead of a file in the game folder.
                        if (f.OptionalIfDriverOverride && !string.IsNullOrEmpty(run.NvngxProxy) &&
                            run.NvngxProxy != "0000000000000000")
                        {
                            status = "ok";
                            value = "supplied by the driver";
                            detail = "no local " + f.Path + "; the NVIDIA app's DLSS override provided it on the last run";
                        }
                    }
                    // ReShade without add-on support loads, finds nothing, and looks fine on disk.
                    if (present && f.Path == "dxgi.dll" && run.HasReShadeAddonSupport && !run.ReShadeAddonSupport)
                    {
                        status = "warn";
                        detail = "ReShade loaded but never searched for add-ons - this looks like the build WITHOUT add-on support";
                    }
                    string ownUrl = (!string.IsNullOrEmpty(f.Url) && f.Url != spec.Url) ? f.Url : null;
                    sec.Rows.Add(new Row(status, name, detail, value, ownUrl));
                    if (present && altPresent && name == f.Path)
                        sec.Rows.Add(new Row("warn", f.Alt, "an older build of the same add-on next to the current one: two versions of RenoDX's DLSS add-on hooking the same NGX calls - ReShade loads both. Keep " + f.Path + " and remove this one", "also present", null));
                }
                sections.Add(sec);
            }

            sections.Add(SettingsSection(game));
            sections.Add(LastRunSection(run));
            return sections;
        }

        static Section SettingsSection(string game)
        {
            var sec = new Section
            {
                Id = "settings",
                Title = "Settings",
                Blurb = "The ones the Settings tab does not cover - the game's own options, ReShade's, and anything that reads as wrong."
            };
            string ss = SavedSettingsPath(game);
            sec.SavedSettings = ss;
            var why = new Dictionary<string, string>
            {
                { "api", "DLSS needs the D3D12 backend" },
                { "vsync", "off, the frame limiter paces instead" },
                { "fpsLimiter", "the port is built around 60" },
                { "enableFXAA", "DLAA replaces it; both together smears" },
            };
            if (ss != null)
            {
                foreach (var want in WantedGameSettings)
                {
                    string have = IniValue(ss, want.Key);
                    if (have != want.Value) sec.WrongKeys.Add(want.Key);
                    if (have == null)
                        sec.Rows.Add(new Row("warn", "Game: " + want.Key, "not written yet - set it once in the in-game options", "unset", null));
                    else if (have == want.Value)
                        sec.Rows.Add(new Row("ok", "Game: " + want.Key, why[want.Key], have, null));
                    else
                        sec.Rows.Add(new Row("warn", "Game: " + want.Key, why[want.Key] + " - expected " + want.Value, have, null));
                }
            }
            else
            {
                sec.Rows.Add(new Row("info", "Game settings", "mgs4.savedsettings not found; run the game once", "unknown", null));
            }

            // The add-on's own keys are the Settings tab's job; only the ones that are actually wrong belong here.
            string ini = Paths.Join(game, "mgs4_dlss.ini");
            if (Paths.Exists(ini))
            {
                string en = IniValue(ini, "Enabled");
                if (en != "1")
                    sec.Rows.Add(new Row("bad", "Add-on: Enabled",
                        "the add-on loads but does nothing while this is 0 - Settings tab", en, null));

                // FrameGen against the hardware, which is the one thing the Settings tab cannot tell you.
                string fgm = IniValue(ini, "FrameGen"), fgt = IniValue(ini, "FGTargetFps");
                int hz = RefreshRate();
                if (!string.IsNullOrEmpty(fgm) && fgm != "0" && hz > 0)
                {
                    int target;
                    if (hz <= 61)
                        sec.Rows.Add(new Row("warn", "Add-on: FrameGen",
                            "the display reports " + hz + " Hz - generated frames cannot be shown at 60 Hz, set FrameGen=0",
                            fgm + ", target " + fgt, null));
                    else if (int.TryParse(fgt, out target) && target > hz)
                        sec.Rows.Add(new Row("warn", "Add-on: FrameGen",
                            "target " + fgt + " fps is above the display's " + hz + " Hz", fgm + ", target " + fgt, null));
                }

                var diags = new List<string>();
                foreach (string diag in new[] { "DebugMode", "Probe", "TraceFrames", "TraceFreeze", "DumpShaders" })
                {
                    string v = IniValue(ini, diag);
                    if (!string.IsNullOrEmpty(v) && v != "0") diags.Add(diag + "=" + v);
                }
                if (diags.Count > 0)
                    sec.Rows.Add(new Row("warn", "Add-on: diagnostics on",
                        "these cost frames; set them to 0 for normal play - Settings tab", string.Join(", ", diags), null));
            }

            string rini = Paths.Join(game, "ReShade.ini");
            if (Paths.Exists(rini))
            {
                string dis = IniValue(rini, "DisabledAddons");
                if (!string.IsNullOrEmpty(dis) && dis.Contains("mgs4_dlss"))
                    sec.Rows.Add(new Row("bad", "ReShade: add-on disabled", "mgs4_dlss is in ReShade's DisabledAddons list", dis, null));
                string up = IniValue(rini, "NREnableUpscaling");
                if (up != null && up != "0")
                    sec.Rows.Add(new Row("warn", "RenoDX: NREnableUpscaling",
                        "on, on top of this add-on's own DLAA - two upscalers in a row", up, null));
                // The current RenoDX build chooses where it applies NR. Left to Auto it has been seen taking the
                // frame-generation path - the whole backbuffer, pillarbox and all on a wide display, read as
                // linear colour. Upscaled is this add-on's DLAA output, the contract the two were built on.
                if (Paths.Exists(Paths.Join(game, "renodx-dlss.addon64")))
                {
                    // RenoDX's list starts with Off: 0 Off, 1 Auto (its default), 2 Upscaled, 3 FrameGen, 4 Present.
                    string hook = IniValue(rini, "DirectNeuralRenderingHookPoint") ?? "1";
                    string[] hookNames = { "Off", "Auto", "Upscaled", "FrameGen", "Present" };
                    int hi; string hookName = int.TryParse(hook.Trim(), out hi) && hi >= 0 && hi < hookNames.Length ? hookNames[hi] : hook;
                    if (hook.Trim() == "0")
                        sec.Rows.Add(new Row("warn", "RenoDX: Hook Method", "Off - RenoDX applies no NR. Upscaled takes this add-on's DLAA output. Settings, Neural Rendering", "Off", null));
                    else if (hook.Trim() != "2")
                        sec.Rows.Add(new Row("warn", "RenoDX: Hook Method",
                            "NR is applied where RenoDX chooses: with frame generation on, the whole backbuffer inside frame generation - the pillarbox too on a wide display - with its status left at Waiting, and off again when the window loses focus. Upscaled takes this add-on's DLAA output. Settings, Neural Rendering",
                            hookName, null));
                    else
                        sec.Rows.Add(new Row("ok", "RenoDX: Hook Method", "NR is applied to this add-on's DLAA output", "Upscaled", null));
                    string req = IniValue(rini, "DirectNeuralRenderingRequireDlss");
                    if (req != null && req.Trim() == "0")
                        sec.Rows.Add(new Row("warn", "RenoDX: Require DLSS",
                            "off lets RenoDX fall back to the presentation path with dummy temporal inputs - on, and it waits for this add-on's DLSS inputs", "off", null));
                    string enc = IniValue(rini, "DirectNeuralRenderingEncoding") ?? "0";
                    if (enc.Trim() == "0")
                        sec.Rows.Add(new Row("info", "RenoDX: Encoding",
                            "Auto reads a DLSS output as linear colour; this add-on's DLAA output is sRGB, so if the image looks grey pick sRGB in RenoDX's own tab", "Auto", null));
                }
            }

            // A frame limiter is not part of this install - it fights a port whose physics are tied to 60 fps -
            // but one left over from an earlier setup is exactly the kind of thing a file list cannot explain
            // on its own, so it is still checked.
            string limiter = Paths.Join(game, "scripts\\MGSFPSUnlock.ini");
            if (Paths.Exists(limiter))
            {
                string target = (IniValue(limiter, "TargetFrameRate") ?? "").Trim();
                int fps;
                if (int.TryParse(target, out fps) && fps > 60)
                    sec.Rows.Add(new Row("warn", "Frame limiter: MGSFPSUnlock",
                        "a separate mod, not part of this install, and above 60 it works against the port - the physics are tied to 60 fps. Frame generation is how this add-on puts more frames on screen: the game keeps running at 60 and the generated ones come on top.",
                        target + " fps", null));
            }

            return sec;
        }

        static Section LastRunSection(LastRun run)
        {
            var sec = new Section
            {
                Id = "lastrun",
                Title = "Last run",
                Blurb = "What the add-on reported the last time the game started. This is the part a file list cannot tell you."
            };
            if (string.IsNullOrEmpty(run.LogPath))
            {
                sec.Rows.Add(new Row("info", "No log yet",
                    "logs\\mgs4_dlss.log appears the first time the game runs with the add-on", "-", null));
                return sec;
            }
            string when = string.Format(CultureInfo.InvariantCulture, "{0:0} min ago", run.LogAge.TotalMinutes);
            if (run.LogAge.TotalHours >= 24) when = string.Format(CultureInfo.InvariantCulture, "{0:0} days ago", run.LogAge.TotalDays);
            else if (run.LogAge.TotalHours >= 1) when = string.Format(CultureInfo.InvariantCulture, "{0:0} h ago", run.LogAge.TotalHours);
            sec.Rows.Add(new Row("info", "Last run", "logs\\mgs4_dlss.log", when, null));

            if (!string.IsNullOrEmpty(run.Addon))
                sec.Rows.Add(new Row("ok", "Add-on registered", "the ReShade add-on loaded and registered", "v" + run.Addon, null));
            if (run.HasReShadeAddonSupport)
            {
                if (run.ReShadeAddonSupport)
                    sec.Rows.Add(new Row("ok", "ReShade add-on support",
                        "registered: " + string.Join(", ", run.ReShadeAddons), "yes", null));
                else
                    sec.Rows.Add(new Row("bad", "ReShade add-on support",
                        "ReShade never searched for add-ons - install the add-on build", "no", null));
            }
            if (!string.IsNullOrEmpty(run.Ngx))
            {
                if (run.Ngx == "0x00000001")
                    sec.Rows.Add(new Row("ok", "NGX init", "NVIDIA NGX initialized for D3D12", "success", null));
                else
                    sec.Rows.Add(new Row("bad", "NGX init", "NGX did not initialize - DLSS cannot be created", run.Ngx, null));
            }
            if (!string.IsNullOrEmpty(run.DlssDll))
            {
                if (run.DlssDll == "loaded")
                    sec.Rows.Add(new Row("ok", "DLSS runtime", "nvngx_dlss.dll from the game folder", "loaded", null));
                else
                    sec.Rows.Add(new Row("ok", "DLSS runtime",
                        "the local file was not used; the driver's _nvngx provided DLSS", "driver override", null));
            }
            // "it loaded" is only worth its own row when it did not go on to do anything.
            bool nrRan = !string.IsNullOrEmpty(run.NrRuntime) || run.NrCreated;
            if (run.Nr == "loaded" && !nrRan && run.NrRejections > 0)
                sec.Rows.Add(new Row("warn", "Neural Rendering add-on",
                    "RenoDX loaded but never attached its NR runtime - " + run.NrRejections + " rejected passes. This build attaches it from ReShade's init_device, which never reached it here, " +
                    "or from a change in its own tab; this add-on raises init_device once more for it (NrKick=1 in the ini)" + (run.NrKicked ? ", which this run did without RenoDX attaching" : ", which this run did not get to") +
                    ". Until then: open the ReShade overlay, RenoDX DLSS, flip one setting and back",
                    "never attached", null));
            else if (run.Nr == "loaded" && !nrRan)
                sec.Rows.Add(new Row("warn", "Neural Rendering add-on", "RenoDX loaded but no NR pass followed", "loaded", null));
            else if (run.Nr == "absent")
                sec.Rows.Add(new Row("info", "Neural Rendering add-on", "not present - DLAA runs before the HUD instead", "absent", null));
            if (nrRan && run.NrOnBackbuffer && !string.IsNullOrEmpty(run.NrSourceSize))
                // Both: the DLAA output as asked, and then the backbuffer again inside DLSS-G - RenoDX's own
                // experimental frame-generation path, which ignores the game-image rectangle this add-on tags.
                sec.Rows.Add(new Row("warn", "Neural Rendering",
                    "RenoDX ran NR on the " + run.NrSourceSize + " DLAA output, as set - and then again on the whole backbuffer inside frame generation " +
                    "(its own experimental path, which ignores the game-image rectangle this add-on tags): that second pass is the noise in the " +
                    "pillarbox on a wide display, and a second NR over the image. RenoDX's to fix; frame generation off sidesteps it",
                    "DLAA output + backbuffer", null));
            else if (nrRan && run.NrOnBackbuffer)
            {
                // Only the backbuffer path ran. Whether that matters is the swapchain's shape: one size with the
                // DLAA output and it is the same image; wider, and it is the pillarbox too. Whether it can be
                // steered is Hook Method: on Upscaled RenoDX went this way anyway (its own choice, seen when its
                // runtime attached from inside the DLSS-G evaluate), on Auto it is what Auto picks here.
                bool sameSize = run.SwapW > 0 && run.OutW > 0 && run.SwapW == run.OutW && run.SwapH == run.OutH;
                bool upscaledSet = (run.NrHook ?? "").Trim() == "2";
                string where = "RenoDX ran NR on the backbuffer inside frame generation" + (upscaledSet ? ", although its Hook Method is Upscaled" : "") + ". ";
                if (sameSize)
                    sec.Rows.Add(new Row("info", "Neural Rendering",
                        where + "The swapchain is " + run.SwapW + "x" + run.SwapH + ", one size with this add-on's DLAA output, so it is the same image and looks the same" +
                        (upscaledSet ? "; its status reads Waiting for the path it did not take" : "") + ".",
                        "backbuffer, same image", null));
                else
                    sec.Rows.Add(new Row("warn", "Neural Rendering",
                        where + "The swapchain is " + (run.SwapW > 0 ? run.SwapW + "x" + run.SwapH : "wider than the image") +
                        " against a " + (run.OutW > 0 ? run.OutW + "x" + run.OutH : "16:9") + " image, so that pass takes in the pillarbox and reads the whole thing as linear colour. " +
                        (upscaledSet ? "RenoDX's own path choice; frame generation off keeps it to the DLAA output" : "Set RenoDX's Hook Method to Upscaled - Settings, Neural Rendering"),
                        "backbuffer", null));
            }
            else if (nrRan)
            {
                string d = "RenoDX created its NR feature (NGX feature 18) after the DLAA one";
                string size = !string.IsNullOrEmpty(run.NrSourceSize) ? run.NrSourceSize : run.NrFeature;
                if (!string.IsNullOrEmpty(size)) d = "NR ran on the " + size + " DLAA output (NGX feature 18)";
                sec.Rows.Add(new Row("ok", "Neural Rendering", d,
                    !string.IsNullOrEmpty(run.NrRuntime) ? (run.NrRuntime == "direct" ? "nvngx_dlssnr, direct" : "nvngx_dlssnr " + run.NrRuntime) : "active", null));
            }
            else if (run.Nr == "loaded")
                sec.Rows.Add(new Row("warn", "Neural Rendering",
                    "the add-on loaded but no NR feature was created - check nvngx_dlssnr.dll and the driver", "no NR pass", null));
            if (!string.IsNullOrEmpty(run.Insertion))
                sec.Rows.Add(new Row("ok", "Insertion point", "where DLAA runs in the frame", run.Insertion, null));
            if (!string.IsNullOrEmpty(run.Sl))
                sec.Rows.Add(new Row(run.FgSupported == "yes" ? "ok" : "warn", "Streamline",
                    "DLSS-G " + run.DlssG + ", supported: " + run.FgSupported, "SL " + run.Sl, null));
            else if (run.FgNoStreamline)
                sec.Rows.Add(new Row("info", "Streamline", "not loaded - frame generation was off for this run", "absent", null));
            if (!string.IsNullOrEmpty(run.Driver))
            {
                string st = "ok", d = "minimum for DLSS-G is " + run.DriverMin;
                try
                {
                    if (new Version(run.Driver) < new Version(run.DriverMin))
                    { st = "bad"; d = "below the " + run.DriverMin + " DLSS-G needs"; }
                }
                catch { }
                sec.Rows.Add(new Row(st, "NVIDIA driver", d, run.Driver, null));
            }
            if (!string.IsNullOrEmpty(run.Fg))
                sec.Rows.Add(new Row("ok", "Frame generation", "Reflex " + run.Reflex,
                    run.Fg + ", target " + run.FgTarget + " fps", null));
            if (!string.IsNullOrEmpty(run.Evaluations))
                sec.Rows.Add(new Row("ok", "DLSS evaluations", "frames DLSS actually processed in that session", run.Evaluations, null));
            return sec;
        }

        public static Verdict GetVerdict(List<Section> sections)
        {
            if (!string.IsNullOrEmpty(ManifestError))
                return new Verdict { Text = "Cannot check", Kind = "bad", Note = ManifestError };
            int bad = sections.SelectMany(s => s.Rows).Count(r => r.Status == "bad");
            int warn = sections.SelectMany(s => s.Rows).Count(r => r.Status == "warn");
            if (bad > 0) return new Verdict { Text = "Not ready", Kind = "bad", Note = bad + " required item(s) missing" };
            if (warn > 0) return new Verdict { Text = "Ready", Kind = "warn", Note = warn + " thing(s) worth a look" };
            return new Verdict { Text = "Ready", Kind = "ok", Note = "everything checked out" };
        }

        public static string TextReport(string game, List<Section> sections)
        {
            var sb = new StringBuilder();
            Verdict v = GetVerdict(sections);
            sb.AppendLine("MGS4 DLSS - install check");
            sb.AppendLine("game    : " + game);
            sb.AppendLine("checked : " + DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
            sb.AppendLine("verdict : " + v.Text + " - " + v.Note);
            if (!string.IsNullOrEmpty(ManifestSource)) sb.AppendLine("list    : " + ManifestSource);
            foreach (Section sec in sections)
            {
                sb.AppendLine();
                sb.AppendLine("[" + sec.Title + "]");
                if (!string.IsNullOrEmpty(sec.Url)) sb.AppendLine("  " + sec.Url);
                foreach (Row r in sec.Rows)
                {
                    string mark = r.Status == "ok" ? "ok  " : r.Status == "warn" ? "warn" : r.Status == "bad" ? "MISS" : "    ";
                    sb.AppendLine(string.Format("  {0}  {1,-28} {2}", mark, r.Name, r.Value));
                    if (r.Status != "ok" && !string.IsNullOrEmpty(r.Detail)) sb.AppendLine("        " + r.Detail);
                    if (r.Status != "ok" && !string.IsNullOrEmpty(r.Url)) sb.AppendLine("        " + r.Url);
                }
            }
            return sb.ToString();
        }
    }
}
