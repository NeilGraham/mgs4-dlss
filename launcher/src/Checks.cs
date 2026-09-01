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
        public bool Optional, Required, OptionalIfDriverOverride;
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
        public TimeSpan LogAge;
        public List<string> ReShadeAddons = new List<string>();
    }

    static class Checks
    {
        public static string ManifestPath { get { return Path.Combine(Paths.Root, "tools\\install_manifest.json"); } }

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

        // "310,8,0,0" and "310.8.0" both normalise to 310.8.0, so the manifest can be written the readable way.
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
            var ser = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
            var root = (Dictionary<string, object>)ser.DeserializeObject(File.ReadAllText(ManifestPath));
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
                        sec.Files.Add(new FileSpec
                        {
                            Path = Str(f, "path"),
                            Detail = Str(f, "detail"),
                            Verified = Str(f, "verified"),
                            Url = Str(f, "url"),
                            Optional = Bool(f, "optional"),
                            Required = Bool(f, "required"),
                            OptionalIfDriverOverride = Bool(f, "optionalIfDriverOverride"),
                        });
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
                if ((m = Regex.Match(line, "frame generation: (.+?), target fps (\\d+), Reflex (\\d)")).Success)
                { r.Fg = m.Groups[1].Value; r.FgTarget = m.Groups[2].Value; r.Reflex = m.Groups[3].Value; }
                if (line.Contains("frame generation off at startup: Streamline not loaded")) r.FgNoStreamline = true;
                if ((m = Regex.Match(line, "Streamline initialised \\(SL ([\\d.]+) / DLSS-G ([\\d.]+)\\); DLSS-G supported: (\\w+)")).Success)
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
                    sec.Rows.Add(new Row(status, f.Path, detail, value, ownUrl));
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

            // Not part of the add-on, but the Play tab's "Keep pressing X" needs both halves of it.
            Row vigem = VigemRow();
            sec.Rows.Add(vigem);
            return sec;
        }

        public static Row VigemRow()
        {
            string dll = null;
            foreach (string c in new[] { Environment.GetEnvironmentVariable("VIGEM_CLIENT_DLL"),
                                         Path.Combine(Paths.Root, "tools\\ViGEmClient.dll") })
                if (!string.IsNullOrEmpty(c) && Paths.Exists(c)) { dll = c; break; }
            bool driver = false;
            try
            {
                using (var s = new System.Management.ManagementObjectSearcher(
                           "SELECT State FROM Win32_SystemDriver WHERE Name='ViGEmBus'"))
                    foreach (System.Management.ManagementObject o in s.Get())
                        if ((o["State"] as string) == "Running") driver = true;
            }
            catch { }
            string url = "https://github.com/nefarius/ViGEmBus/releases";
            if (dll != null && driver)
                return new Row("ok", "Virtual controller",
                    "ViGEmBus is running and " + Path.GetFileName(dll) + " is in place - the launcher can tap Cross for the flashback prompts",
                    "ready", url);
            if (dll == null && !driver)
                return new Row("info", "Virtual controller",
                    "optional: ViGEmBus + tools\\ViGEmClient.dll let the launcher tap Cross through a cutscene; without them it can only press Enter",
                    "not installed", url);
            if (dll == null)
                return new Row("warn", "Virtual controller",
                    "the ViGEmBus driver is running, but tools\\ViGEmClient.dll is missing (it also ships inside the vgamepad package, or set VIGEM_CLIENT_DLL)",
                    "no ViGEmClient.dll", url);
            return new Row("warn", "Virtual controller",
                "ViGEmClient.dll is here but the ViGEmBus driver is not running - install it", "no driver", url);
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
                    sec.Rows.Add(new Row("ok", "NGX init", "NVIDIA NGX initialised for D3D12", "success", null));
                else
                    sec.Rows.Add(new Row("bad", "NGX init", "NGX did not initialise - DLSS cannot be created", run.Ngx, null));
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
            if (run.Nr == "loaded" && !nrRan)
                sec.Rows.Add(new Row("warn", "Neural Rendering add-on", "renodx-dlss5 loaded but no NR pass followed", "loaded", null));
            else if (run.Nr == "absent")
                sec.Rows.Add(new Row("info", "Neural Rendering add-on", "not present - DLAA runs before the HUD instead", "absent", null));
            if (nrRan)
            {
                string d = "RenoDX created its NR feature (NGX feature 18) after the DLAA one";
                if (!string.IsNullOrEmpty(run.NrFeature)) d = "NR ran on the " + run.NrFeature + " DLAA output (NGX feature 18)";
                sec.Rows.Add(new Row("ok", "Neural Rendering", d,
                    !string.IsNullOrEmpty(run.NrRuntime) ? "nvngx_dlssnr " + run.NrRuntime : "active", null));
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
