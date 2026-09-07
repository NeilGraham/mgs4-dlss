// Updates: the launcher's own releases on GitHub, and the file list (tools\install_manifest.json) it checks the
// game folder against. Both are looked up at most once a day when the window opens, or whenever the Setup tab's
// button is pressed. The add-on and the ini ride inside the release exe, so a new release is a new add-on too:
// after an update the Setup tab's "Install the add-on" copies the new one next to mgs4.exe. The file list can
// move on its own - a new RenoDX build verified, a ReShade version bumped - without a release: the current copy on
// the master branch is fetched and kept under %LOCALAPPDATA% when its revision is newer than the one built in.
//
// What is checked, and what it costs: one GET on api.github.com (60 an hour unauthenticated, one a day here) and
// one on raw.githubusercontent.com, eight seconds each at most, on a thread of their own; a machine off the
// network gets one failed attempt and the same message it had. The state - when it last looked, what it found -
// is a small JSON file beside the preferences, never the preferences file itself, which the window rewrites whole.
//
// Replacing a running exe: the new one is downloaded beside the state file, checked against the SHA-256 GitHub
// publishes for the asset, and swapped in by a command script that waits for this process to end, keeps the
// old exe as .old, and starts the new one. A checkout is never replaced - it is built from source.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    static class Updates
    {
        public const string Repo = "NeilGraham/mgs4-dlss";
        public const string ReleasesPage = "https://github.com/" + Repo + "/releases";
        const string ReleaseApi = "https://api.github.com/repos/" + Repo + "/releases/latest";
        const string ManifestUrl = "https://raw.githubusercontent.com/" + Repo + "/master/tools/install_manifest.json";
        const string ExeName = "mgs4-dlss-launcher.exe";
        public static readonly TimeSpan Every = TimeSpan.FromHours(24);
        const int TimeoutMs = 8000;

        public class Result
        {
            public string Current, Latest, Page, Notes, AssetUrl, AssetSha, Error;
            public long AssetSize;
            public bool Newer;                 // a release newer than this exe
            public string ManifestRevision;    // the revision on master, when it was read
            public bool ManifestNewer;         // ... and it is newer than the one in use, so it was kept
            public DateTime CheckedAt;
            public bool Ok { get { return string.IsNullOrEmpty(Error); } }

            public string Summary()
            {
                if (!Ok) return Error;
                string s = Newer ? "v" + Latest + " is out (this is v" + Current + ")" : "up to date (v" + Current + ")";
                if (ManifestNewer) s += "; file list revision " + ManifestRevision + " taken";
                return s;
            }
        }

        // ------------------------------------------------------------------------------------------- versions

        // Stamped by launcher\build.ps1 from MGS4_DLSS_VERSION in dlss-addon\src\mgs4_dlss.cpp: the add-on's version
        // is the release's version. An exe built without the stamp says 0.0.0 and every release is newer than it.
        public static string CurrentVersion
        {
            get
            {
                try
                {
                    object[] a = Assembly.GetExecutingAssembly().GetCustomAttributes(typeof(AssemblyInformationalVersionAttribute), false);
                    if (a.Length > 0)
                    {
                        string v = ((AssemblyInformationalVersionAttribute)a[0]).InformationalVersion;
                        if (!string.IsNullOrEmpty(v)) return v;
                    }
                }
                catch { }
                return "0.0.0";
            }
        }

        static Version Parse(string s)
        {
            if (string.IsNullOrEmpty(s)) return null;
            s = s.Trim();
            if (s.StartsWith("v", StringComparison.OrdinalIgnoreCase)) s = s.Substring(1);
            int cut = s.IndexOfAny(new[] { '-', '+', ' ' });
            if (cut > 0) s = s.Substring(0, cut);
            var parts = new List<string>(s.Split('.'));
            while (parts.Count < 2) parts.Add("0");
            Version v;
            return Version.TryParse(string.Join(".", parts.ToArray()), out v) ? v : null;
        }

        public static bool IsNewer(string candidate, string current)
        {
            Version a = Parse(candidate), b = Parse(current);
            return a != null && (b == null || a > b);
        }

        // ------------------------------------------------------------------------------------------- state

        static string StatePath { get { return Path.Combine(Paths.AppDataDir, "updates.json"); } }
        public static string CachedManifestPath { get { return Path.Combine(Paths.AppDataDir, "install_manifest.json"); } }
        static string DownloadDir { get { return Path.Combine(Paths.AppDataDir, "update"); } }

        static Dictionary<string, object> ReadState()
        {
            try
            {
                if (Paths.Exists(StatePath))
                {
                    var d = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(StatePath)) as Dictionary<string, object>;
                    if (d != null) return d;
                }
            }
            catch { }
            return new Dictionary<string, object>();
        }

        static void WriteState(Dictionary<string, object> d)
        {
            try
            {
                Directory.CreateDirectory(Paths.AppDataDir);
                File.WriteAllText(StatePath, new JavaScriptSerializer().Serialize(d));
            }
            catch { }
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v; return d != null && d.TryGetValue(k, out v) && v != null ? v.ToString() : "";
        }

        public static DateTime? LastChecked()
        {
            DateTime t;
            string s = Str(ReadState(), "CheckedAt");
            return DateTime.TryParse(s, null, System.Globalization.DateTimeStyles.RoundtripKind, out t) ? t.ToLocalTime() : (DateTime?)null;
        }

        // Once a day, counted from the last check that reached GitHub. A failed attempt is not counted, so a
        // machine that was offline looks again next time the window opens - that costs a few seconds on a thread
        // nobody waits for, and nothing else.
        public static bool Due()
        {
            DateTime? last = LastChecked();
            return last == null || DateTime.Now - last.Value >= Every;
        }

        // What the last successful check found, for a window that opens before the daily one is due.
        public static Result LastKnown()
        {
            Dictionary<string, object> d = ReadState();
            if (string.IsNullOrEmpty(Str(d, "CheckedAt"))) return null;
            var r = new Result
            {
                Current = CurrentVersion,
                Latest = Str(d, "Latest"),
                Page = Str(d, "Page"),
                Notes = Str(d, "Notes"),
                AssetUrl = Str(d, "AssetUrl"),
                AssetSha = Str(d, "AssetSha"),
                ManifestRevision = Str(d, "ManifestRevision"),
            };
            long size; long.TryParse(Str(d, "AssetSize"), out size); r.AssetSize = size;
            r.CheckedAt = LastChecked() ?? DateTime.MinValue;
            r.Newer = IsNewer(r.Latest, r.Current);
            return r;
        }

        // ------------------------------------------------------------------------------------------- the check

        static string Get(string url, string accept)
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.Method = "GET";
            req.UserAgent = "mgs4-dlss-launcher/" + CurrentVersion;
            req.Accept = accept;
            // While the repository is private, GitHub answers an unauthenticated request with 403. A token in the
            // environment or config.ini (MGS4_GITHUB_TOKEN) lets the check and the download through for whoever
            // has one; a public repository needs none.
            string token = Paths.Setting("MGS4_GITHUB_TOKEN", "");
            if (!string.IsNullOrEmpty(token)) req.Headers["Authorization"] = "Bearer " + token.Trim();
            req.Timeout = TimeoutMs;
            req.ReadWriteTimeout = TimeoutMs;
            using (var resp = (HttpWebResponse)req.GetResponse())
            using (var r = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                return r.ReadToEnd();
        }

        static string Explain(Exception e)
        {
            var we = e as WebException;
            var resp = we != null ? we.Response as HttpWebResponse : null;
            if (resp != null)
            {
                if (resp.StatusCode == HttpStatusCode.NotFound) return "GitHub answered 404: no release is published, or the repository is private and this request carried no token";
                if ((int)resp.StatusCode == 403) return "GitHub refused the request (rate limit, or the repository is private)";
                return "GitHub answered " + (int)resp.StatusCode;
            }
            if (we != null) return "could not reach GitHub: " + we.Status;
            return e.Message;
        }

        // The whole check: the newest release, then the file list on master. Run it on a thread; it can take
        // sixteen seconds when nothing answers.
        public static Result Check()
        {
            var r = new Result { Current = CurrentVersion, CheckedAt = DateTime.Now };
            try
            {
                var ser = new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 };
                var rel = ser.DeserializeObject(Get(ReleaseApi, "application/vnd.github+json")) as Dictionary<string, object>;
                if (rel == null) throw new Exception("GitHub's answer was not a release");
                r.Latest = Str(rel, "tag_name");
                r.Page = Str(rel, "html_url");
                r.Notes = Str(rel, "body");
                object assets;
                if (rel.TryGetValue("assets", out assets) && assets is object[])
                    foreach (object a in (object[])assets)
                    {
                        var d = a as Dictionary<string, object>;
                        if (d == null || !string.Equals(Str(d, "name"), ExeName, StringComparison.OrdinalIgnoreCase)) continue;
                        r.AssetUrl = Str(d, "browser_download_url");
                        long size; long.TryParse(Str(d, "size"), out size); r.AssetSize = size;
                        // "sha256:<hex>" since GitHub started publishing digests; older releases have none.
                        string digest = Str(d, "digest");
                        if (digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase)) r.AssetSha = digest.Substring(7).ToLowerInvariant();
                    }
                r.Newer = IsNewer(r.Latest, r.Current);
            }
            catch (Exception e) { r.Error = Explain(e); }

            // The file list is worth having even when the release lookup failed, and the other way round.
            try
            {
                string text = Get(ManifestUrl, "text/plain");
                string rev = Checks.RevisionOf(text);
                r.ManifestRevision = rev;
                if (!string.IsNullOrEmpty(rev) && Checks.Manifest() != null &&
                    string.CompareOrdinal(rev, Checks.ManifestRevision ?? "") > 0)
                {
                    Directory.CreateDirectory(Paths.AppDataDir);
                    File.WriteAllText(CachedManifestPath, text, new UTF8Encoding(false));
                    Checks.ForgetManifest();
                    r.ManifestNewer = true;
                }
            }
            catch (Exception e) { if (r.Ok) r.Error = "file list: " + Explain(e); }

            if (!string.IsNullOrEmpty(r.Latest))
            {
                Dictionary<string, object> d = ReadState();
                d["CheckedAt"] = r.CheckedAt.ToUniversalTime().ToString("o");
                d["Latest"] = r.Latest; d["Page"] = r.Page; d["Notes"] = r.Notes ?? "";
                d["AssetUrl"] = r.AssetUrl ?? ""; d["AssetSha"] = r.AssetSha ?? ""; d["AssetSize"] = r.AssetSize.ToString();
                d["ManifestRevision"] = r.ManifestRevision ?? "";
                WriteState(d);
            }
            return r;
        }

        // ------------------------------------------------------------------------------------------- applying one

        public static bool CanSelfUpdate { get { return !Paths.IsCheckout; } }

        // Fetches the release exe and checks it against GitHub's digest. Returns the path of the verified file,
        // or throws with the reason.
        public static string Download(Result r, Action<string> say)
        {
            if (r == null || string.IsNullOrEmpty(r.AssetUrl)) throw new Exception("the release carries no " + ExeName);
            Directory.CreateDirectory(DownloadDir);
            string target = Path.Combine(DownloadDir, ExeName);
            string part = target + ".part";
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            using (var wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.UserAgent] = "mgs4-dlss-launcher/" + CurrentVersion;
                string token = Paths.Setting("MGS4_GITHUB_TOKEN", "");
                if (!string.IsNullOrEmpty(token)) wc.Headers[HttpRequestHeader.Authorization] = "Bearer " + token.Trim();
                if (say != null) wc.DownloadProgressChanged += (s, e) => say("downloading v" + r.Latest + "  " + e.ProgressPercentage + "%");
                wc.DownloadFile(r.AssetUrl, part);
            }
            if (!string.IsNullOrEmpty(r.AssetSha))
            {
                string have = Checks.Sha256(part);
                if (have != r.AssetSha) { try { File.Delete(part); } catch { } throw new Exception("the download's SHA-256 does not match what GitHub published - not applied"); }
            }
            if (Paths.Exists(target)) File.Delete(target);
            File.Move(part, target);
            return target;
        }

        // Swaps the exe in place from outside the process and starts the new one. The caller closes the window
        // straight after; the script waits for this process id to be gone before it copies.
        public static void Apply(string newExe)
        {
            if (!CanSelfUpdate) throw new Exception("this launcher is a checkout, built from source - pull and rebuild instead");
            string me = Assembly.GetExecutingAssembly().Location;
            string old = me + ".old";
            string script = Path.Combine(DownloadDir, "apply-update.cmd");
            int pid = Process.GetCurrentProcess().Id;
            var sb = new StringBuilder();
            sb.AppendLine("@echo off");
            sb.AppendLine(":wait");
            sb.AppendLine("tasklist /FI \"PID eq " + pid + "\" 2>nul | find \"" + pid + "\" >nul");
            sb.AppendLine("if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait)");
            sb.AppendLine("copy /y \"" + me + "\" \"" + old + "\" >nul");
            sb.AppendLine("copy /y \"" + newExe + "\" \"" + me + "\" >nul");
            sb.AppendLine("if errorlevel 1 (echo The update could not be written over \"" + me + "\". The new file is at \"" + newExe + "\". & pause & exit /b 1)");
            sb.AppendLine("start \"\" \"" + me + "\"");
            sb.AppendLine("del \"%~f0\"");
            File.WriteAllText(script, sb.ToString(), Encoding.ASCII);
            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + script + "\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            });
        }

        // The --check-updates action: the same check, as text.
        public static IEnumerable<string> Report()
        {
            Checks.Manifest();   // so the list in use is known before it is compared with the one on master
            Result r = Check();
            yield return "this launcher : v" + r.Current + (Paths.IsCheckout ? " (checkout - updates are a git pull and a rebuild)" : "");
            if (!r.Ok) yield return "check         : " + r.Error;
            if (!string.IsNullOrEmpty(r.Latest))
            {
                yield return "newest release: " + r.Latest + (r.Newer ? "  <- newer" : "  (not newer)") + "  " + r.Page;
                if (!string.IsNullOrEmpty(r.AssetUrl))
                    yield return "asset         : " + r.AssetUrl + (r.AssetSize > 0 ? "  (" + (r.AssetSize / 1024) + " KB)" : "") +
                                 (string.IsNullOrEmpty(r.AssetSha) ? "  no digest published" : "  sha256 " + r.AssetSha.Substring(0, 12));
            }
            yield return "file list     : in use " + (Checks.ManifestRevision ?? "(no revision)") + " from " + Checks.ManifestSource +
                         (string.IsNullOrEmpty(r.ManifestRevision) ? "" : "; on master " + r.ManifestRevision + (r.ManifestNewer ? "  <- taken" : ""));
            DateTime? last = LastChecked();
            yield return "last checked  : " + (last == null ? "never" : last.Value.ToString("yyyy-MM-dd HH:mm")) + "  (again after " + Every.TotalHours + " h, or from the Setup tab)";
        }
    }
}
