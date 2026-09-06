// Steam itself: whether it is up, where it is, and getting it up before the game is started.
//
// Why this matters. mgs4.exe is a Steam build: on startup it asks the Steam client for a session, and when there is
// no client running it hands itself back to Steam and exits - Steam comes up, asks whether the game may run with
// its custom arguments, and then launches the app the way its own Play button does, which is the Master Collection
// front-end with no arguments at all. Every --stage and --skip-to-main-menu is lost on the way, and the person
// who picked a scene lands on the collection menu. steam_appid.txt keeps a *running* Steam from relaunching the
// exe; it does nothing about a Steam that is not running yet.
//
// So a launch that finds no Steam starts it first and waits for it to be signed in, and only then starts the game.
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using Microsoft.Win32;

namespace Mgs4Launcher
{
    static class Steam
    {
        public static bool Running()
        {
            try { return Process.GetProcessesByName("steam").Length > 0; } catch { return false; }
        }

        // Steam writes who is signed in under ActiveProcess as it comes up: pid first, then ActiveUser once the
        // account is in. A client that is running but still on its login screen has ActiveUser 0, and a game
        // started then is handed straight back to it.
        public static bool SignedIn()
        {
            if (!Running()) return false;
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey("Software\\Valve\\Steam\\ActiveProcess"))
                {
                    if (k == null) return false;
                    object user = k.GetValue("ActiveUser");
                    return user != null && Convert.ToInt64(user) != 0;
                }
            }
            catch { return false; }
        }

        /// <summary>steam.exe, from the registry - the same key the library search reads its install from.</summary>
        public static string ExePath()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey("Software\\Valve\\Steam"))
                {
                    if (k != null)
                    {
                        string exe = k.GetValue("SteamExe") as string;
                        if (!string.IsNullOrEmpty(exe) && Paths.Exists(exe)) return Paths.Format(exe);
                        string dir = k.GetValue("SteamPath") as string;
                        if (!string.IsNullOrEmpty(dir))
                        {
                            string p = Paths.Join(Paths.Format(dir), "steam.exe");
                            if (Paths.Exists(p)) return p;
                        }
                    }
                }
            }
            catch { }
            foreach (string lib in Paths.SteamLibraries())
            {
                string p = Paths.Join(lib, "steam.exe");
                if (Paths.Exists(p)) return p;
            }
            return null;
        }

        /// <summary>Make sure Steam is up and signed in before the game is started. Blocking, with a ceiling: a
        /// Steam that takes longer than this is left to come up on its own and the launch goes ahead anyway.
        /// Returns true when Steam is known to be ready.</summary>
        public static bool EnsureRunning(Action<string> say, int timeoutSeconds = 90)
        {
            if (SignedIn()) return true;
            if (!Running())
            {
                string exe = ExePath();
                if (exe == null)
                {
                    if (say != null) say("Steam is not running and steam.exe was not found - launching anyway");
                    return false;
                }
                if (say != null) say("Steam is not running - starting it first, so the scene arguments survive");
                try
                {
                    // -silent: the client comes up in the tray rather than putting its window over the game.
                    Process.Start(new ProcessStartInfo(exe, "-silent")
                    { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) });
                }
                catch (Exception e)
                {
                    if (say != null) say("could not start Steam: " + e.Message + " - launching anyway");
                    return false;
                }
            }
            else if (say != null) say("Steam is up but not signed in yet - waiting for it");

            DateTime t0 = DateTime.Now;
            while ((DateTime.Now - t0).TotalSeconds < timeoutSeconds)
            {
                if (SignedIn())
                {
                    // Signed in is not quite ready: the client still loads its app list for a few seconds, and a
                    // game started in that window is sometimes bounced. A short settle covers it.
                    Thread.Sleep(4000);
                    if (say != null) say("Steam is ready after " + (int)(DateTime.Now - t0).TotalSeconds + "s");
                    return true;
                }
                Thread.Sleep(500);
            }
            if (say != null) say("Steam did not sign in within " + timeoutSeconds + "s - launching anyway");
            return false;
        }
    }
}
