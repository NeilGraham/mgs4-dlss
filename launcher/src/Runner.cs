// Running a scene: the boot (mgs4.exe --stage), pressing through the auto-save notice and the "press any button"
// screen until the first 3D frame, tapping E for the in-cutscene flashback prompts, and ending a scene when the
// cutscene hands over to gameplay. A port of Invoke-SceneRun in tools/mgs4_dlss_launcher.ps1.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace Mgs4Launcher
{
    // keybd_event with a real scan code is what this port's input path reacts to; scan-code-only SendInput events
    // were ignored by it. Keys only reach the foreground window, so the game is pushed there first (Alt tap +
    // AttachThreadInput is the sequence Windows wants before it allows a foreground change).
    static class Win
    {
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
        [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
        [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
        [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
        [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
        [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

        public static void Key(ushort vk, bool down)
        {
            keybd_event((byte)vk, (byte)MapVirtualKey(vk, 0), (uint)(down ? 0 : 2), UIntPtr.Zero);
        }

        public static bool Focus(IntPtr h)
        {
            if (GetForegroundWindow() == h) return true;
            keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
            uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
            if (fg != me) AttachThreadInput(me, fg, true);
            ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
            if (fg != me) AttachThreadInput(me, fg, false);
            Thread.Sleep(250);
            return GetForegroundWindow() == h;
        }

        public static bool EscapeDown() { return (GetAsyncKeyState(0x1B) & 0x8000) != 0; }
    }

    // Only the lines the add-on wrote since we started reading. The log is replaced at every launch, so a shrink
    // resets.
    class LogTail
    {
        public string Path;
        public long Pos;
        public LogTail(string path)
        {
            Path = path;
            Pos = Paths.Exists(path) ? new FileInfo(path).Length : 0;
        }
        public string[] Read()
        {
            if (!Paths.Exists(Path)) return new string[0];
            long len = new FileInfo(Path).Length;
            if (len < Pos) Pos = 0;
            if (len == Pos) return new string[0];
            using (var fs = new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                fs.Seek(Pos, SeekOrigin.Begin);
                var buf = new byte[len - Pos];
                int read = fs.Read(buf, 0, buf.Length);
                Pos += read;
                return Encoding.UTF8.GetString(buf, 0, read).Split(new[] { "\r\n", "\n" }, StringSplitOptions.None);
            }
        }
    }

    static class Runner
    {
        public static IntPtr GameWindow()
        {
            foreach (Process p in Process.GetProcessesByName("mgs4"))
                if (p.MainWindowHandle != IntPtr.Zero) return p.MainWindowHandle;
            return IntPtr.Zero;
        }

        // What of the game is up, beyond mgs4.exe itself. The window's music and its status badge want the wider
        // answer - the Master Collection front-end that @collection opens, and the bundled MGS1 that s04a05l runs,
        // are both "the game has the screen" even though neither is mgs4.exe. Checks.GameRunning stays mgs4 alone,
        // because that is the one that owns the ini files.
        //
        // "launcher" is too common a process name to trust on its own, so a launcher.exe only counts when it is
        // the collection's: the one in the Launcher folder beside the game's, or - when its path cannot be read,
        // which an elevated process refuses - one whose window is titled for the collection.
        public static string GameActivity(string gameDir)
        {
            try
            {
                if (Process.GetProcessesByName("mgs4").Length > 0) return "MGS4";
                if (Process.GetProcessesByName("mgs1").Length > 0) return "MGS1";
                string want = string.IsNullOrEmpty(gameDir) ? null
                              : Paths.Join(Path.GetDirectoryName(gameDir), "Launcher\\launcher.exe");
                foreach (Process p in Process.GetProcessesByName("launcher"))
                {
                    string path = null;
                    try { path = p.MainModule.FileName; } catch { }
                    if (path != null)
                    {
                        if (want != null && string.Equals(Paths.Format(path), Paths.Format(want), StringComparison.OrdinalIgnoreCase))
                            return "Master Collection";
                        continue;
                    }
                    string title = "";
                    try { title = p.MainWindowTitle ?? ""; } catch { }
                    if (title.IndexOf("METAL GEAR", StringComparison.OrdinalIgnoreCase) >= 0) return "Master Collection";
                }
            }
            catch { }
            return null;
        }

        public static void StopGame()
        {
            // "mgs1" is not a typo: s04a05l starts the bundled MGS1 (a separate mgs1.exe titled "METAL GEAR
            // SOLID"), and nothing here was closing it - it outlived the run and sat on the desktop.
            foreach (string name in new[] { "mgs4", "mgs1", "launcher" })
                foreach (Process p in Process.GetProcessesByName(name))
                    try { p.Kill(); } catch { }
            Thread.Sleep(3000);
        }

        // --skip-to-main-menu is what a plain mgs4.exe used to do: without it the port stops on the Master
        // Collection screen first. --stage <id> goes past both, straight into a scene.
        public static void LaunchCommand(Options opt, string gameDir, out string exe, out string args)
        {
            if (opt.Stage == "@collection")
            {
                exe = Paths.Join(Path.GetDirectoryName(gameDir), "Launcher\\launcher.exe");
                args = "";
                return;
            }
            exe = Paths.Join(gameDir, "mgs4.exe");
            var cli = new List<string>();
            if (opt.Stage == "@main" || opt.Stage == "") cli.Add("--skip-to-main-menu");
            else { cli.Add("--stage"); cli.Add(opt.Stage); }
            // A resolution asked for on the command line wins; otherwise the default from config.ini is used, and
            // with neither the game picks for itself.
            int w = opt.Width, h = opt.Height;
            if (w <= 0 || h <= 0) IniForm.DefaultResolution(out w, out h);
            if (w > 0 && h > 0)
            {
                cli.Add("--res_width"); cli.Add(w.ToString());
                cli.Add("--res_height"); cli.Add(h.ToString());
            }
            string windowing = opt.Windowing;
            if (string.IsNullOrEmpty(windowing)) windowing = Paths.Setting("MGS4_WINDOWING", null);
            if (!string.IsNullOrEmpty(windowing)) { cli.Add("--windowing"); cli.Add(windowing); }
            args = string.Join(" ", cli.ConvertAll(c => c.Contains(" ") ? "\"" + c + "\"" : c));
        }

        // Key names as tools\launch_stage.ps1 spelled them, so its sequences still work.
        public static ushort VirtualKey(string name)
        {
            switch (name.ToUpperInvariant())
            {
                case "SPACE": return 0x20;
                case "ENTER": return 0x0D;
                case "ESC": return 0x1B;
                case "TAB": return 0x09;
                case "UP": return 0x26;
                case "DOWN": return 0x28;
                case "LEFT": return 0x25;
                case "RIGHT": return 0x27;
                case "BACK": return 0x08;
                case "DELETE": return 0x2E;
                case "PAUSE": return 0x13;
                case "SCROLL": return 0x91;
                case "INSERT": return 0x2D;
                case "HOME": return 0x24;
                case "END": return 0x23;
                case "PRINTSCREEN": case "PRTSC": return 0x2C;
            }
            Match m = Regex.Match(name, "^F(\\d+)$", RegexOptions.IgnoreCase);
            if (m.Success) return (ushort)(0x6F + int.Parse(m.Groups[1].Value));
            if (name.Length == 1) return (ushort)char.ToUpperInvariant(name[0]);
            throw new ArgumentException("unknown key " + name);
        }

        // "5,ENTER,4,ENTER": a number is a wait in seconds, anything else a key press.
        public static void SendKeys(IntPtr hwnd, string keys, Action<string> say)
        {
            foreach (string tok in keys.Split(','))
            {
                string t = tok.Trim();
                if (t.Length == 0) continue;
                if (Regex.IsMatch(t, "^\\d+(\\.\\d+)?$"))
                {
                    say("wait " + t + " s");
                    Thread.Sleep((int)(double.Parse(t, System.Globalization.CultureInfo.InvariantCulture) * 1000));
                    continue;
                }
                ushort vk = VirtualKey(t);
                bool ok = false;
                for (int a = 0; a < 5 && !ok; a++) { ok = Win.Focus(hwnd); if (!ok) Thread.Sleep(400); }
                say("press " + t + " (foreground " + ok + ")");
                Win.Key(vk, true); Thread.Sleep(300); Win.Key(vk, false);
            }
        }

        // The add-on's scene classifier, straight out of the log.
        static void ReadSceneSignals(LogTail tail, ref string state, ref int hudDraws)
        {
            hudDraws = -1;      // only a heartbeat seen in *this* read counts
            foreach (string line in tail.Read())
            {
                Match m = Regex.Match(line, "SCENE-STATE-TICK (cutscene|gameplay|no-3d) \\(frame \\d+, scene draws \\d+, HUD draws (\\d+)");
                if (m.Success) { state = m.Groups[1].Value; hudDraws = int.Parse(m.Groups[2].Value); continue; }
                m = Regex.Match(line, "SCENE-STATE (cutscene|gameplay|no-3d) ");
                if (m.Success) { state = m.Groups[1].Value; continue; }
                // Do NOT break the press loop on the add-on's FIRST-3D-FRAME line. The act title card is itself a
                // rendered 3D frame, so that fires while the game is still waiting to be pressed past the card:
                // pressing stops early and the boot stalls for seconds. SCENE-STATE's 30-frame confirmation is
                // what distinguishes a title card from a scene, and it is worth the half second it costs.
                // a build with SceneLog off still says this, and it only happens on a 3D frame
                if (line.Contains("NGX EvaluateFeature ok") && state == "unknown") state = "cutscene";
            }
        }

        public static int Run(Options opt, Action<string> consoleSay)
        {
            string gameDir = opt.GameDir;
            if (string.IsNullOrEmpty(gameDir)) gameDir = Paths.GameDir();
            string log = Paths.Join(gameDir, "logs\\launcher.log");
            Directory.CreateDirectory(Path.GetDirectoryName(log));
            Action<string> say = m =>
            {
                string line = string.Format("[{0:HH:mm:ss.fff}] {1}", DateTime.Now, m);
                if (!opt.Quiet && consoleSay != null) consoleSay(line);
                try { File.AppendAllText(log, line + Environment.NewLine, Encoding.ASCII); } catch { }
            };

            string addonLog = Paths.Join(gameDir, "logs\\mgs4_dlss.log");
            var tail = new LogTail(addonLog);
            string exe, args;
            LaunchCommand(opt, gameDir, out exe, out args);
            if (!Paths.Exists(exe)) { say("not found: " + exe); return 1; }

            // A --stage boot is handed back to Steam and relaunched without its arguments when steam_appid.txt is
            // absent, so it is written on the way past rather than left as a warning on the Setup tab.
            if (args.Contains("--stage") || args.Contains("--skip-to-main-menu"))
            {
                Outcome appid = Install.SteamAppId(gameDir);
                if (appid.Lines.Count > 0 && !appid.Lines[0].Contains("already there")) say(appid.Lines[0]);
            }

            if (opt.Restart)
            {
                StopGame();
                // Steam first. A Steam build started with no client running hands itself back to Steam, which
                // relaunches the collection's front-end without any of these arguments - see Steam.cs.
                Steam.EnsureRunning(say);
                tail = new LogTail(addonLog);      // the add-on truncates its log at startup
                var psi = new ProcessStartInfo(exe, args)
                { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) };
                Process.Start(psi);
                say("launched " + Path.GetFileName(exe) + " " + args);
            }
            else say("attaching to the running game");

            if (opt.Stage == "@collection") return 0;

            // A menu is not a scene: no boot prompts to press through, no first 3D frame, and tapping Cross on the
            // main menu just starts a new game. Only an explicit key sequence makes sense here. This used to say
            // IsStartEntry, which also caught the title-screen boot - and that one is the game starting itself,
            // with the logos and "press any button" in the way and a first 3D frame (the title over the cemetery)
            // for the press loop to stop at. It is the one start entry the run options mean anything on, and the
            // Play tab offers exactly that option there.
            if (Catalog.IsMenuEntry(opt.Stage) && string.IsNullOrEmpty(opt.Keys))
            {
                if (opt.Advance || opt.MashX || opt.EndOnGameplay)
                    say("menu entry: leaving the game alone (the run options are for scenes)");
                return 0;
            }

            bool attached = opt.Advance || opt.MashX || opt.EndOnGameplay || !string.IsNullOrEmpty(opt.Keys)
                            || opt.Hold > 0 || opt.MaxMinutes > 0;
            if (!attached) return 0;

            IntPtr hwnd = IntPtr.Zero;
            for (int i = 0; i < 120 && hwnd == IntPtr.Zero; i++)
            {
                hwnd = GameWindow();
                if (hwnd == IntPtr.Zero) Thread.Sleep(1000);
            }
            if (hwnd == IntPtr.Zero) { say("no game window appeared"); return 1; }

            // Two keys. The boot prompts - the auto-save notice and "press any button" - take any key at all, and
            // Enter is the one that has always got past them. The flashback prompts inside a cutscene are the
            // port's Cross, which on the keyboard is E; Enter does nothing to them. This used to be a virtual
            // DualShock through the ViGEmBus driver, which meant a kernel driver and a DLL to install for one
            // checkbox - E is the same press with nothing to set up. --press-key names one key for both.
            string bootKey = string.IsNullOrEmpty(opt.PressKey) ? "ENTER" : opt.PressKey;
            string sceneKey = string.IsNullOrEmpty(opt.PressKey) ? "E" : opt.PressKey;
            Func<string, Action> tapper = name =>
            {
                ushort vk = VirtualKey(name);
                return () =>
                {
                    if (!Win.Focus(hwnd)) Win.Focus(hwnd);
                    Win.Key(vk, true); Thread.Sleep((int)(opt.PressHold * 1000)); Win.Key(vk, false);
                };
            };
            Action press = tapper(bootKey), pressScene = tapper(sceneKey);
            if (opt.Advance && string.IsNullOrEmpty(opt.Keys))
                say("pressing " + bootKey.ToUpperInvariant() + " through the boot prompts" +
                    (opt.MashX ? ", then " + sceneKey.ToUpperInvariant() + " for the flashbacks" : ""));

            if (!string.IsNullOrEmpty(opt.Keys))
            {
                if (opt.Settle > 0) { say("window up, settling " + opt.Settle + "s"); Thread.Sleep((int)(opt.Settle * 1000)); }
                SendKeys(hwnd, opt.Keys, say);
            }

            string state = "unknown";
            int hud = -1;
            DateTime t0 = DateTime.Now;

            // Phase 1 - through the auto-save notice, the "press any button" screen and the load, to the first 3D frame.
            if (opt.Advance && string.IsNullOrEmpty(opt.Keys))
            {
                say("pressing through the boot prompts (up to " + opt.StartTimeout + "s)");
                int n = 0;
                while ((DateTime.Now - t0).TotalSeconds < opt.StartTimeout)
                {
                    if (Win.EscapeDown()) { say("Escape - stopping"); return 130; }
                    ReadSceneSignals(tail, ref state, ref hud);
                    if (opt.SceneDetect && (state == "cutscene" || state == "gameplay")) break;
                    if (GameWindow() == IntPtr.Zero) { say("the game exited"); return 1; }
                    press(); n++;
                    Thread.Sleep((int)(opt.PressEvery * 1000));
                }
                if (state == "cutscene" || state == "gameplay") say("scene running (" + state + ") after " + n + " presses");
                else say("no 3D frame within " + opt.StartTimeout + "s (state " + state + ")");
            }

            if (!(opt.MashX || opt.EndOnGameplay || opt.Hold > 0 || opt.MaxMinutes > 0)) return 0;

            // Phase 2 - stay with the scene: keep E going for the flashbacks, and watch for the hand-over.
            DateTime tScene = DateTime.Now;
            DateTime? gameplaySince = null, staticSince = null;
            string reason = "still running";
            while (true)
            {
                if (Win.EscapeDown()) { reason = "Escape"; break; }
                if (GameWindow() == IntPtr.Zero) { reason = "the game exited"; break; }
                double inScene = (DateTime.Now - tScene).TotalSeconds;
                if (opt.Hold > 0 && inScene >= opt.Hold) { reason = "held " + opt.Hold + "s"; break; }
                if (opt.MaxMinutes > 0 && (DateTime.Now - t0).TotalMinutes >= opt.MaxMinutes) { reason = "max-minutes"; break; }

                if (opt.MashX) pressScene();
                Thread.Sleep((int)(opt.PressEvery * 1000));

                string prev = state;
                ReadSceneSignals(tail, ref state, ref hud);
                if (state != prev) say(string.Format("state -> {0} at {1:n0}s", state, inScene));

                if (opt.EndOnGameplay && inScene > opt.MinSeconds)
                {
                    if (hud >= opt.HudMin) { reason = "HUD up (" + hud + " draws)"; break; }
                    if (state == "gameplay")
                    {
                        if (gameplaySince == null) gameplaySince = DateTime.Now;
                        else if ((DateTime.Now - gameplaySince.Value).TotalSeconds >= opt.GameplayGrace) { reason = "gameplay"; break; }
                    }
                    else gameplaySince = null;
                    if (state == "no-3d")
                    {
                        if (staticSince == null) staticSince = DateTime.Now;
                        else if ((DateTime.Now - staticSince.Value).TotalSeconds >= opt.StaticGrace)
                        { reason = "the scene ended (loading / continue screen)"; break; }
                    }
                    else staticSince = null;
                }
            }
            say(string.Format("done after {0:n0}s: {1}", (DateTime.Now - tScene).TotalSeconds, reason));
            if (!opt.KeepRunning && reason != "still running" && reason != "the game exited")
                if (opt.EndOnGameplay || opt.Hold > 0 || opt.MaxMinutes > 0) { say("closing the game"); StopGame(); }
            return 0;
        }
    }
}
