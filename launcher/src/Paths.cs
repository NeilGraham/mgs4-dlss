// Where the game and this checkout live, resolved exactly as tools/paths.ps1 and tools/paths.py resolve them:
// environment variable > config.ini in the repo root > detection. Kept in step with those two on purpose - the
// PowerShell app, the Python capture scripts and this have to agree or none of them can be trusted.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Mgs4Launcher
{
    static class Paths
    {
        public const string AppId = "2492670";      // METAL GEAR SOLID 4: Guns of the Patriots - Master Collection
        public const string InstallDirName = "METAL GEAR SOLID 4";

        // The folder holding the app: next to the exe in a release, the repo root in a checkout.
        public static string Root
        {
            get { return Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location); }
        }

        public static string ConfigPath { get { return Path.Combine(Root, "config.ini"); } }

        // Exists, without throwing on a drive letter that is not mounted.
        public static bool Exists(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            try { return File.Exists(path) || Directory.Exists(path); } catch { return false; }
        }

        // Join without touching the filesystem, so a path on a missing drive does not raise.
        public static string Join(string a, string b)
        {
            if (string.IsNullOrEmpty(a)) return null;
            try { return Path.Combine(a, b); } catch { return null; }
        }

        // Absolute, with an upper-case drive letter: the registry hands Steam's path back in lower case.
        public static string Format(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;
            path = path.Replace('/', '\\').TrimEnd('\\');
            if (path.Length > 1 && path[1] == ':') path = char.ToUpper(path[0]) + path.Substring(1);
            return path;
        }

        static Dictionary<string, string> _config;
        public static Dictionary<string, string> Config
        {
            get
            {
                if (_config != null) return _config;
                _config = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                if (Exists(ConfigPath))
                {
                    foreach (string raw in File.ReadAllLines(ConfigPath))
                    {
                        string line = raw.Trim();
                        if (line.Length == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                        int eq = line.IndexOf('=');
                        if (eq <= 0) continue;
                        string value = line.Substring(eq + 1).Trim().Trim('"');
                        if (value.Length > 0) _config[line.Substring(0, eq).Trim()] = value;
                    }
                }
                return _config;
            }
        }

        public static string Setting(string key, string fallback)
        {
            string v = Environment.GetEnvironmentVariable(key);
            if (string.IsNullOrEmpty(v) && Config.ContainsKey(key)) v = Config[key];
            if (string.IsNullOrEmpty(v)) v = fallback;
            return string.IsNullOrEmpty(v) ? null : Environment.ExpandEnvironmentVariables(v);
        }

        static string RegValue(RegistryKey root, string subkey, string name)
        {
            try
            {
                using (RegistryKey k = root.OpenSubKey(subkey))
                    return k == null ? null : k.GetValue(name) as string;
            }
            catch { return null; }
        }

        // Steam's own install, then every library its steamapps\libraryfolders.vdf names - which is where an
        // install on another drive is listed, so the client on C: is what finds a game on D:. The drive sweep
        // after it is only for a library Steam has forgotten, or a copied install.
        public static List<string> SteamLibraries()
        {
            var roots = new List<string>();
            string s = RegValue(Registry.CurrentUser, "SOFTWARE\\Valve\\Steam", "SteamPath");
            if (!string.IsNullOrEmpty(s)) roots.Add(s);
            s = RegValue(Registry.LocalMachine, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
            if (!string.IsNullOrEmpty(s)) roots.Add(s);
            roots.Add("C:\\Program Files (x86)\\Steam");
            roots.Add("C:\\Program Files\\Steam");
            foreach (DriveInfo d in DriveInfo.GetDrives())
            {
                bool ready;
                try { ready = d.IsReady; } catch { continue; }
                if (!ready) continue;
                string letter = d.Name.TrimEnd('\\');
                foreach (string sub in new[] { "", "\\SteamLibrary", "\\Steam", "\\Games\\Steam",
                                               "\\Program Files (x86)\\Steam", "\\Program Files\\Steam" })
                    roots.Add(letter + sub);
            }

            var libs = new List<string>();
            Action<string> add = p =>
            {
                if (!Exists(p)) return;
                foreach (string have in libs)
                    if (string.Equals(have, p, StringComparison.OrdinalIgnoreCase)) return;
                libs.Add(p);
            };
            foreach (string raw in roots)
            {
                string root = raw.Replace('/', '\\');
                add(root);
                string vdf = Join(root, "steamapps\\libraryfolders.vdf");
                if (!Exists(vdf)) continue;
                foreach (string line in File.ReadAllLines(vdf))
                {
                    Match m = Regex.Match(line, "^\\s*\"path\"\\s+\"(.+)\"\\s*$");
                    if (m.Success) add(m.Groups[1].Value.Replace("\\\\", "\\"));
                }
            }
            return libs;
        }

        // The libraries worth naming when the game is not found: the ones holding a steamapps folder, rather than
        // every drive letter the sweep tried.
        public static List<string> SteamLibraryList()
        {
            var named = new List<string>();
            foreach (string lib in SteamLibraries())
                if (Exists(Join(lib, "steamapps"))) named.Add(Format(lib));
            return named;
        }

        // The install of app 2492670. The folder under steamapps\common is normally "METAL GEAR SOLID 4", but the
        // app manifest is what actually says, so that is tried first when it is there.
        public static string FindGameDir()
        {
            foreach (string lib in SteamLibraries())
            {
                string apps = Join(lib, "steamapps");
                var names = new List<string> { InstallDirName };
                string manifest = Join(apps, "appmanifest_" + AppId + ".acf");
                if (Exists(manifest))
                {
                    foreach (string line in File.ReadAllLines(manifest))
                    {
                        Match m = Regex.Match(line, "^\\s*\"installdir\"\\s+\"(.+)\"\\s*$");
                        if (m.Success) { names.Insert(0, m.Groups[1].Value); break; }
                    }
                }
                foreach (string name in names)
                {
                    string cand = Join(apps, Path.Combine("common", name, "MGS4"));
                    if (Exists(Join(cand, "mgs4.exe"))) return cand;
                }
            }
            return null;
        }

        // A folder is the game folder if mgs4.exe is in it, or if mgs4.exe is in an MGS4 folder inside it -
        // "METAL GEAR SOLID 4" is the install root and MGS4 is the part that matters. null when it is neither.
        public static string ResolveGameDir(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;
            path = Format(path);
            if (Exists(Join(path, "mgs4.exe"))) return path;
            string inner = Join(path, "MGS4");
            if (Exists(Join(inner, "mgs4.exe"))) return inner;
            return null;
        }

        public static string GameDir()
        {
            string game = Setting("MGS4_DIR", null);
            if (string.IsNullOrEmpty(game)) game = FindGameDir();
            string resolved = ResolveGameDir(game);
            return resolved != null ? resolved : Format(game);
        }

        public static string OutDir()
        {
            return Format(Setting("MGS4_OUT", Path.Combine(Root, "work")));
        }

        public static string GameDirSource()
        {
            if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MGS4_DIR")))
                return "from the MGS4_DIR environment variable";
            if (Config.ContainsKey("MGS4_DIR")) return "from config.ini";
            return "found in the Steam libraries";
        }

        // The one place the app writes it. config.ini is git-ignored and is what every script in the repo asks.
        public static string SetConfiguredGameDir(string dir)
        {
            var lines = new List<string>();
            if (Exists(ConfigPath)) lines.AddRange(File.ReadAllLines(ConfigPath));
            bool done = false;
            for (int i = 0; i < lines.Count && !done; i++)
            {
                Match m = Regex.Match(lines[i], "^\\s*;?\\s*MGS4_DIR\\s*=\\s*(.*)$");
                if (!m.Success) continue;
                lines[i] = string.IsNullOrEmpty(dir) ? ";MGS4_DIR=" + m.Groups[1].Value : "MGS4_DIR=" + dir;
                done = true;
            }
            if (!done && !string.IsNullOrEmpty(dir))
            {
                if (lines.Count == 0)
                    lines.Add("; Machine-local paths for this checkout (git-ignored). See config.example.ini for every key.");
                lines.Add("MGS4_DIR=" + dir);
            }
            File.WriteAllLines(ConfigPath, lines.ToArray());
            _config = null;
            return ConfigPath;
        }
    }
}
