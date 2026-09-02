// Putting files where they belong: the add-on this app ships with, steam_appid.txt, and whatever is dropped on
// the Setup tab. A port of the drag-and-drop half of tools/install_checks.ps1.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;

namespace Mgs4Launcher
{
    class Outcome
    {
        public bool Ok;
        public List<string> Lines = new List<string>();
        public Outcome(bool ok) { Ok = ok; }
        public Outcome Say(string line) { Lines.Add(line); return this; }
        public override string ToString() { return string.Join("   |   ", Lines); }
    }

    static class Install
    {
        // ------------------------------------------------------------------------------------- the bundled add-on

        // The add-on's own two files, as they ship with this app. A release is one exe with both built into it as
        // resources (launcher\build.ps1); a source checkout has the built addon64 in build\ and the sample ini in
        // dlss-addon\, and a pair dropped next to the exe wins over everything. Either way they are already on the
        // machine, so the one group of files this app exists for is the one nobody should have to fetch.
        //
        // The file on disk is preferred over the built-in copy for the same reason Paths.DataText prefers it:
        // rebuilding the add-on in a checkout should change what "Install the add-on" installs without rebuilding
        // the launcher too.
        public const string BuiltIn = "the copy built into the launcher";

        static readonly string[] AddonPlaces = { "mgs4_dlss.addon64", "build\\mgs4_dlss.addon64" };
        static readonly string[] IniPlaces = { "mgs4_dlss.ini", "dlss-addon\\mgs4_dlss.ini" };

        // Where a bundled file would be read from - a path, BuiltIn, or null when this copy of the app has none.
        static string Locate(string name, string[] places)
        {
            foreach (string rel in places)
            {
                string p = Paths.Join(Paths.Root, rel);
                if (Paths.Exists(p)) return p;
            }
            return Paths.HasResource(name) ? BuiltIn : null;
        }

        static Stream Open(string name, string where)
        {
            if (where == null) return null;
            if (where == BuiltIn) return Paths.DataStream(name);
            return new FileStream(where, FileMode.Open, FileAccess.Read, FileShare.Read);
        }

        // What the Setup tab and the report say about each: a path, or that it is built in. Null for missing.
        public static void FindBundled(out string addon, out string ini)
        {
            addon = Locate("mgs4_dlss.addon64", AddonPlaces);
            ini = Locate("mgs4_dlss.ini", IniPlaces);
        }

        // One file, from wherever FindBundled said, into the game folder. Overwrites: the caller decides whether
        // the destination is one to keep.
        static void Put(string name, string from, string dest)
        {
            using (Stream src = Open(name, from))
            {
                if (src == null) throw new FileNotFoundException("no " + name + " to copy");
                string tmp = dest + ".tmp";
                using (FileStream dst = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                    src.CopyTo(dst);
                // Into place in one move, so a copy that fails half way never leaves a truncated add-on for
                // ReShade to load.
                if (File.Exists(dest)) File.Delete(dest);
                File.Move(tmp, dest);
            }
        }

        // Copies those two next to mgs4.exe: the addon64 every time, the ini only when the game folder has none -
        // the rule dlss-addon\install.sh has always used, so an ini that has been tuned (or that the add-on wrote
        // InternalRes into) is never overwritten.
        //
        // The game must be closed: ReShade holds mgs4_dlss.addon64 open for as long as it is loaded, so the copy
        // would fail on a sharing violation half way through rather than not at all.
        public static Outcome BundledAddon(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir))
                return new Outcome(false).Say("no game folder set - pick one above first");
            if (Checks.GameRunning())
                return new Outcome(false).Say("the game is running - close it first, ReShade holds mgs4_dlss.addon64 open");

            string addon, ini;
            FindBundled(out addon, out ini);
            if (addon == null)
                return new Outcome(false).Say(
                    "no mgs4_dlss.addon64 ships with this copy of the app - build it (dlss-addon\\build.bat) or take one from the releases");

            var outcome = new Outcome(true);
            try
            {
                Put("mgs4_dlss.addon64", addon, Paths.Join(gameDir, "mgs4_dlss.addon64"));
                outcome.Say("mgs4_dlss.addon64 -> the game folder" + (addon == BuiltIn ? " (from inside the launcher)" : ""));
            }
            catch (Exception e)
            {
                return new Outcome(false).Say("could not copy mgs4_dlss.addon64: " + e.Message);
            }

            string destIni = Paths.Join(gameDir, "mgs4_dlss.ini");
            if (Paths.Exists(destIni)) outcome.Say("kept the mgs4_dlss.ini already there");
            else if (ini != null)
            {
                try { Put("mgs4_dlss.ini", ini, destIni); outcome.Say("mgs4_dlss.ini -> the game folder"); }
                catch (Exception e) { outcome.Say("could not copy mgs4_dlss.ini: " + e.Message); }
            }
            else outcome.Say("no mgs4_dlss.ini to copy - the add-on will use its built-in defaults");
            return outcome;
        }

        // ------------------------------------------------------------------------------------- steam_appid.txt

        // Not something Steam or the game ever writes - it is a Steamworks convention the *caller* provides: one
        // line holding the appid, next to the exe, telling the Steam API which game this is. Without it mgs4.exe
        // hands itself back to Steam at startup and is relaunched without its arguments, which is exactly how a
        // --stage boot loses the stage.
        public static Outcome SteamAppId(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir))
                return new Outcome(false).Say("no game folder set - pick one above first");
            string path = Paths.Join(gameDir, "steam_appid.txt");
            if (Paths.Exists(path)) return new Outcome(true).Say("steam_appid.txt is already there");
            try
            {
                // No BOM and no trailing newline: the file is read as a bare number, and a BOM in front of it is not one.
                File.WriteAllText(path, Paths.AppId, new System.Text.UTF8Encoding(false));
                return new Outcome(true).Say("wrote steam_appid.txt (" + Paths.AppId + ") - scene boots keep their --stage argument now");
            }
            catch (Exception e)
            {
                return new Outcome(false).Say("could not write steam_appid.txt: " + e.Message);
            }
        }

        // ------------------------------------------------------------------------------------- drag and drop

        // Everything the install can legitimately receive, so a stray file in a zip is never written into the game
        // folder. Anything not matching one of these is reported as skipped rather than copied.
        static readonly string[] DropPatterns =
        {
            "sl.*.dll", "nvngx_*.dll", "*.addon64", "mgs4_dlss.ini", "winmm.dll", "wininet.dll",
            "*.asi", "MGSFPSUnlock.ini", "steam_appid.txt", "*license*"
        };

        static bool Like(string name, string pattern)
        {
            string rx = "^" + System.Text.RegularExpressions.Regex.Escape(pattern).Replace("\\*", ".*").Replace("\\?", ".") + "$";
            return System.Text.RegularExpressions.Regex.IsMatch(name, rx,
                System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        }

        static bool DropAllowed(string name)
        {
            return DropPatterns.Any(p => Like(name, p));
        }

        // The file names the drop area names, straight from the manifest so the two cannot drift.
        public static List<string> DropNames()
        {
            var names = new List<string>();
            foreach (Section s in Checks.Manifest()) names.AddRange(s.Drop);
            return names;
        }

        // What a dropped file is: which manifest group claims it, by the "accepts" patterns.
        static Section DropTarget(List<Section> sections, string name)
        {
            if (sections == null) return null;
            foreach (Section s in sections)
                foreach (string pat in s.Accepts)
                    if (Like(name, pat)) return s;
            return null;
        }

        // Where a dropped or unpacked file belongs: next to mgs4.exe, all of it. The scripts\ folder was only
        // ever for the ASI mods, and nothing on the allowlist above goes there any more.
        static string DropFolder(string gameDir)
        {
            return gameDir;
        }

        // Puts dropped files where the manifest says they go. Zips are unpacked flat into the game folder, which
        // is what streamline.zip wants; the ReShade setup is run rather than merely started.
        public static List<string> CopyDropped(List<Section> sections, string gameDir, IEnumerable<string> paths)
        {
            var log = new List<string>();
            if (string.IsNullOrEmpty(gameDir)) { log.Add("no game folder set - pick one above first"); return log; }

            foreach (string path in paths)
            {
                if (!Paths.Exists(path)) { log.Add("gone: " + path); continue; }
                string name = Path.GetFileName(path);

                if (Like(name, "ReShade_Setup*.exe")) { log.AddRange(RunReShadeSetup(path, gameDir)); continue; }

                if (Path.GetExtension(name).ToLowerInvariant() == ".zip")
                {
                    try
                    {
                        using (ZipArchive zip = ZipFile.OpenRead(path))
                        {
                            int took = 0, skipped = 0;
                            foreach (ZipArchiveEntry entry in zip.Entries)
                            {
                                if (string.IsNullOrEmpty(entry.Name)) continue;      // a directory entry
                                if (!DropAllowed(entry.Name)) { skipped++; continue; }
                                string into = DropFolder(gameDir);
                                if (!Paths.Exists(into)) Directory.CreateDirectory(into);
                                entry.ExtractToFile(Paths.Join(into, entry.Name), true);
                                took++;
                            }
                            log.Add(name + " -> " + took + " file(s) into the game folder" +
                                    (skipped > 0 ? ", " + skipped + " skipped" : ""));
                        }
                    }
                    catch (Exception e) { log.Add("could not read " + name + ": " + e.Message); }
                    continue;
                }

                if (!DropAllowed(name)) { log.Add("skipped " + name + " - not part of the install"); continue; }
                Section sec = DropTarget(sections, name);
                string dest = DropFolder(gameDir);
                if (!Paths.Exists(dest)) Directory.CreateDirectory(dest);
                try
                {
                    File.Copy(path, Paths.Join(dest, name), true);
                    log.Add(name + " -> " + (sec != null ? sec.Title : "the game folder"));
                }
                catch (Exception e) { log.Add("could not copy " + name + ": " + e.Message); }
            }
            return log;
        }

        // ReShade's setup is scriptable, so the one step of the install that looks un-droppable is not: the target
        // exe as the first argument, --headless to install without asking anything (no target picker, no API
        // picker, no shader packs - none are used here), and --api dxgi because dxgi.dll is the name this game
        // loads. It takes well under a second. If it comes back non-zero the setup is started the old way, with
        // its own window, rather than leaving the user with nothing.
        static List<string> RunReShadeSetup(string setup, string gameDir)
        {
            var log = new List<string>();
            string target = Paths.Join(gameDir, "mgs4.exe");
            bool done = false;
            if (Paths.Exists(target))
            {
                try
                {
                    var psi = new ProcessStartInfo(setup, "\"" + target + "\" --headless --api dxgi")
                    { UseShellExecute = false, CreateNoWindow = true };
                    using (Process p = Process.Start(psi))
                    {
                        if (p.WaitForExit(120000) && p.ExitCode == 0)
                        {
                            string dll = Paths.Join(gameDir, "dxgi.dll");
                            if (Paths.Exists(dll))
                            {
                                string ver = Checks.PeVersion(dll);
                                log.Add("installed ReShade" + (string.IsNullOrEmpty(ver) ? "" : " " + Checks.FormatVersion(ver)) + " as dxgi.dll");
                                done = true;
                            }
                        }
                    }
                }
                catch (Exception e) { log.Add("could not run " + Path.GetFileName(setup) + ": " + e.Message); }
            }
            if (!done)
            {
                try
                {
                    Process.Start(new ProcessStartInfo(setup) { UseShellExecute = true });
                    log.Add("started " + Path.GetFileName(setup) +
                            " - point it at mgs4.exe, pick Direct3D 10/11/12, and tick no shader packs");
                }
                catch (Exception e) { log.Add("could not start " + Path.GetFileName(setup) + ": " + e.Message); }
            }
            return log;
        }
    }
}
