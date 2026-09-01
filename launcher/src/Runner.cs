// Running a scene: the boot (mgs4.exe --stage), pressing through the auto-save notice and the "press any button"
// screen until the first 3D frame, tapping Cross for the in-cutscene flashback prompts, and ending a scene when
// the cutscene hands over to gameplay. A port of Invoke-SceneRun in tools/mgs4_dlss_launcher.ps1.
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

    // A virtual DualShock 4 through ViGEmBus. Cross is the button MGS4's flashback prompts want; a keyboard Enter
    // gets past the boot prompts but does not fire them. The DLL is loaded by full path first, so the DllImport
    // binds to the module already in the process whatever folder it came from.
    static class Pad
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct DS4Report { public byte lx, ly, rx, ry; public ushort buttons; public byte special, tl, tr; }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("ViGEmClient.dll")] static extern IntPtr vigem_alloc();
        [DllImport("ViGEmClient.dll")] static extern int vigem_connect(IntPtr client);
        [DllImport("ViGEmClient.dll")] static extern void vigem_disconnect(IntPtr client);
        [DllImport("ViGEmClient.dll")] static extern void vigem_free(IntPtr client);
        [DllImport("ViGEmClient.dll")] static extern IntPtr vigem_target_ds4_alloc();
        [DllImport("ViGEmClient.dll")] static extern int vigem_target_add(IntPtr client, IntPtr target);
        [DllImport("ViGEmClient.dll")] static extern int vigem_target_remove(IntPtr client, IntPtr target);
        [DllImport("ViGEmClient.dll")] static extern void vigem_target_free(IntPtr target);
        [DllImport("ViGEmClient.dll")] static extern int vigem_target_ds4_update(IntPtr client, IntPtr target, DS4Report r);

        const int VIGEM_ERROR_NONE = 0x20000000;
        const ushort DPAD_NONE = 0x8, CROSS = 1 << 5;
        static IntPtr _client = IntPtr.Zero, _pad = IntPtr.Zero;
        public static string Error = "";

        // tools\ViGEmClient.dll, VIGEM_CLIENT_DLL, or the copy the vgamepad package installs.
        public static string FindDll()
        {
            foreach (string c in new[] { Environment.GetEnvironmentVariable("VIGEM_CLIENT_DLL"),
                                         Paths.Join(Paths.Root, "tools\\ViGEmClient.dll"),
                                         Paths.Join(Paths.Root, "ViGEmClient.dll") })
                if (!string.IsNullOrEmpty(c) && Paths.Exists(c)) return c;
            return null;
        }

        public static bool Open(string dllPath)
        {
            if (_pad != IntPtr.Zero) return true;
            try
            {
                if (!string.IsNullOrEmpty(dllPath) && LoadLibraryW(dllPath) == IntPtr.Zero)
                { Error = "ViGEmClient.dll could not be loaded from " + dllPath; return false; }
                _client = vigem_alloc();
                int r = vigem_connect(_client);
                if (r != VIGEM_ERROR_NONE)
                { Error = "ViGEmBus is not running (vigem_connect 0x" + r.ToString("X8") + ")"; Close(); return false; }
                _pad = vigem_target_ds4_alloc();
                r = vigem_target_add(_client, _pad);
                if (r != VIGEM_ERROR_NONE) { Error = "vigem_target_add 0x" + r.ToString("X8"); Close(); return false; }
                Send(0);
                return true;
            }
            catch (Exception e) { Error = e.Message; Close(); return false; }
        }

        static void Send(ushort buttons)
        {
            var rep = new DS4Report();
            rep.lx = rep.ly = rep.rx = rep.ry = 128;
            rep.buttons = (ushort)(DPAD_NONE | buttons);
            vigem_target_ds4_update(_client, _pad, rep);
        }

        public static void TapCross(int holdMs)
        {
            if (_pad == IntPtr.Zero) return;
            Send(CROSS); Thread.Sleep(holdMs); Send(0);
        }

        public static void Close()
        {
            try
            {
                if (_pad != IntPtr.Zero) { vigem_target_remove(_client, _pad); vigem_target_free(_pad); }
                if (_client != IntPtr.Zero) { vigem_disconnect(_client); vigem_free(_client); }
            }
            catch { }
            _pad = IntPtr.Zero; _client = IntPtr.Zero;
        }
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

        public static void StopGame()
        {
            foreach (string name in new[] { "mgs4", "launcher" })
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
            if (!string.IsNullOrEmpty(opt.Windowing)) { cli.Add("--windowing"); cli.Add(opt.Windowing); }
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
                tail = new LogTail(addonLog);      // the add-on truncates its log at startup
                var psi = new ProcessStartInfo(exe, args)
                { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) };
                Process.Start(psi);
                say("launched " + Path.GetFileName(exe) + " " + args);
            }
            else say("attaching to the running game");

            if (opt.Stage == "@collection") return 0;

            // A menu is not a scene: no boot prompts to press through, no first 3D frame, and tapping Cross on the
            // main menu just starts a new game. Only an explicit key sequence makes sense here.
            if (Catalogue.IsStartEntry(opt.Stage) && string.IsNullOrEmpty(opt.Keys))
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

            // A virtual DualShock exists for one reason: MGS4's in-cutscene flashback prompts want Cross, and a
            // keyboard Enter does not fire them. The boot prompts are not like that - the auto-save notice and
            // "press any button" take any button at all, and Enter is one. So a run that is only pressing through
            // those stays on the keyboard and never creates a controller for the game to notice.
            bool padOk = false;
            if (opt.MashX && string.IsNullOrEmpty(opt.PressKey))
            {
                string dll = Pad.FindDll();
                if (dll == null) say("no ViGEmClient.dll - falling back to Enter, flashback prompts will not fire");
                else
                {
                    padOk = Pad.Open(dll);
                    if (padOk) say("virtual DualShock 4 on " + Path.GetFileName(dll));
                    else say("no controller: " + Pad.Error + " - falling back to Enter, flashback prompts will not fire");
                }
            }
            ushort vk = VirtualKey(string.IsNullOrEmpty(opt.PressKey) ? "ENTER" : opt.PressKey);
            bool usePad = padOk && string.IsNullOrEmpty(opt.PressKey);
            if (opt.Advance && !usePad && string.IsNullOrEmpty(opt.Keys))
                say("pressing " + (string.IsNullOrEmpty(opt.PressKey) ? "Enter" : opt.PressKey.ToUpperInvariant()) +
                    " on the keyboard" + (opt.MashX ? "" : " (no controller needed for the boot prompts)"));
            Action press = () =>
            {
                if (!Win.Focus(hwnd)) Win.Focus(hwnd);
                if (usePad) Pad.TapCross((int)(opt.PressHold * 1000));
                else { Win.Key(vk, true); Thread.Sleep((int)(opt.PressHold * 1000)); Win.Key(vk, false); }
            };

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
                    if (Win.EscapeDown()) { say("Escape - stopping"); Pad.Close(); return 130; }
                    ReadSceneSignals(tail, ref state, ref hud);
                    if (opt.SceneDetect && (state == "cutscene" || state == "gameplay")) break;
                    if (GameWindow() == IntPtr.Zero) { say("the game exited"); Pad.Close(); return 1; }
                    press(); n++;
                    Thread.Sleep((int)(opt.PressEvery * 1000));
                }
                if (state == "cutscene" || state == "gameplay") say("scene running (" + state + ") after " + n + " presses");
                else say("no 3D frame within " + opt.StartTimeout + "s (state " + state + ")");
            }

            if (!(opt.MashX || opt.EndOnGameplay || opt.Hold > 0 || opt.MaxMinutes > 0)) { Pad.Close(); return 0; }

            // Phase 2 - stay with the scene: keep Cross going for the flashbacks, and watch for the hand-over.
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

                if (opt.MashX) press();
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
            Pad.Close();
            say(string.Format("done after {0:n0}s: {1}", (DateTime.Now - tScene).TotalSeconds, reason));
            if (!opt.KeepRunning && reason != "still running" && reason != "the game exited")
                if (opt.EndOnGameplay || opt.Hold > 0 || opt.MaxMinutes > 0) { say("closing the game"); StopGame(); }
            return 0;
        }
    }
}
