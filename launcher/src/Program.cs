// MGS4 DLSS Launcher: start the game or any single scene in it, set the add-on up, and check the install - one
// window with three tabs, and the same things as a command line.
//
// This is the C# build of tools/mgs4_dlss_launcher.ps1: same flags, same window, same files read and written. It
// exists because the PowerShell host costs 1.6 s of startup and puts a scripting engine in the per-frame path of
// every animation; the logic here is a port, not a redesign.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace Mgs4Launcher
{
    static class Program
    {
        [DllImport("kernel32.dll")] static extern bool AttachConsole(int processId);
        [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int which);
        [DllImport("kernel32.dll")] static extern bool SetStdHandle(int which, IntPtr handle);
        [DllImport("kernel32.dll")] static extern uint GetFileType(IntPtr handle);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int MessageBox(IntPtr hWnd, string text, string caption, uint type);
        const int ATTACH_PARENT_PROCESS = -1;
        const int STD_OUTPUT = -11, STD_ERROR = -12;
        const uint FILE_TYPE_UNKNOWN = 0;

        static bool Usable(IntPtr h)
        {
            return h != IntPtr.Zero && h != new IntPtr(-1) && GetFileType(h) != FILE_TYPE_UNKNOWN;
        }

        // Whatever the caller gave us to write to, keep it.
        //
        // AttachConsole hands the process the parent's console AND replaces its standard handles with that
        // console's - which throws away a redirect the caller set up. Measured: `app.exe --report > out.txt` from
        // cmd arrives with stdout on the file (FILE_TYPE_DISK) and leaves AttachConsole with stdout on a console
        // (FILE_TYPE_CHAR), so the file caught nothing. cmd passes its handles to a windowed program and waits for
        // it like any other, so nothing else was needed: only this call was in the way.
        //
        // So: attach only when the caller left us nothing usable - a double-click, where there is no console and
        // no redirect and the right amount of output is none - and put the caller's own handles back afterwards.
        static void KeepCallersOutput()
        {
            IntPtr outHandle = GetStdHandle(STD_OUTPUT), errHandle = GetStdHandle(STD_ERROR);
            bool haveOut = Usable(outHandle), haveErr = Usable(errHandle);
            if (haveOut && haveErr) return;
            AttachConsole(ATTACH_PARENT_PROCESS);
            if (haveOut) SetStdHandle(STD_OUTPUT, outHandle);
            if (haveErr) SetStdHandle(STD_ERROR, errHandle);
        }

        // Just write. Attached to a console the text lands there; redirected to a pipe or a file it lands there;
        // double-clicked, with neither, .NET hands Console.Out a null stream and it goes nowhere - which is what
        // should happen, and no window flashes to say so. Deciding in advance whether a console exists got this
        // wrong in both directions: `start /b` hands the process one that AttachConsole then refuses to attach to,
        // and a redirected run has no console window at all yet still has somewhere to write.
        static void Say(string line)
        {
            try { Console.WriteLine(line); } catch { }
        }

        [STAThread]
        static int Main(string[] argv)
        {
            // Started from a terminal, this behaves like a command and prints where it was typed; redirected, it
            // writes to the redirect; double-clicked, it is a window and never flashes a console of its own.
            KeepCallersOutput();

            Options opt;
            try { opt = Options.Parse(argv); }
            catch (Exception e) { Say(e.Message); return 2; }

            if (opt.Action == "help") { Say(Options.HelpText); return 0; }

            string gameDir = opt.GameDir;
            if (string.IsNullOrEmpty(gameDir))
            {
                try { gameDir = Paths.GameDir(); } catch { gameDir = null; }
            }
            // --game-dir can name a folder that holds no mgs4.exe; a non-empty string is not an install. It can
            // also name the install root ("METAL GEAR SOLID 4") rather than the MGS4 folder inside it.
            if (!string.IsNullOrEmpty(gameDir))
            {
                string resolved = Paths.ResolveGameDir(gameDir);
                if (resolved != null) gameDir = resolved;
                else { opt.GameDirBad = gameDir; gameDir = null; }
            }
            opt.GameDir = gameDir;

            string startTab = opt.Action == "install" ? "install" : opt.Action == "settings-ui" ? "settings" : "play";
            if (opt.Action == "" && Prefs.FirstRun()) startTab = "install";
            bool wantsWindow = opt.Action == "ui" || opt.Action == "install" || opt.Action == "settings-ui" ||
                               (opt.Action == "" && string.IsNullOrEmpty(opt.Stage));
            if (gameDir == null && wantsWindow) startTab = "install";

            if (gameDir == null && !wantsWindow && opt.Action != "list")
            {
                Say("mgs4.exe was not found. Set MGS4_DIR in config.ini or pass --game-dir \"<path to MGS4>\".");
                foreach (string lib in Paths.SteamLibraryList()) Say("  looked in " + lib);
                return 1;
            }

            switch (opt.Action)
            {
                case "list":
                    foreach (string line in Catalogue.Listing(opt.Filter)) Say(line);
                    return 0;
                case "report":
                    Say(Checks.TextReport(gameDir, Checks.Run(gameDir)));
                    return 0;
                case "settings":
                    foreach (string line in IniForm.Report(gameDir)) Say(line);
                    return 0;
                case "set":
                    return IniForm.SetFromCli(gameDir, opt.Sets, Say);
                case "stop":
                    Runner.StopGame();
                    Say("closed mgs4.exe");
                    return 0;
                case "install-addon":
                {
                    Outcome done = Install.BundledAddon(gameDir);
                    foreach (string line in done.Lines) Say(line);
                    return done.Ok ? 0 : 1;
                }
                case "shortcut":
                    if (string.IsNullOrEmpty(opt.Stage))
                    {
                        Say("--shortcut needs a scene: mgs4-dlss-launcher <id> --shortcut <file>");
                        return 2;
                    }
                    Say("shortcut written: " + Shortcut.Write(opt, opt.ShortcutPath));
                    return 0;
            }

            if (wantsWindow || string.IsNullOrEmpty(opt.Stage))
            {
                try
                {
                    var app = new System.Windows.Application();
                    var ui = new MainWindow(opt, gameDir, startTab);
                    app.Run(ui.Win);
                    return 0;
                }
                catch (Exception e)
                {
                    // A window that dies on the way up leaves nothing on screen to say why, so the reason goes to
                    // a file and to a message box rather than to a console nobody is watching.
                    string crash = Path.Combine(Path.GetTempPath(), "mgs4-dlss-launcher-crash.log");
                    try { File.WriteAllText(crash, DateTime.Now + Environment.NewLine + e); } catch { }
                    Say(e.ToString());
                    MessageBox(IntPtr.Zero, "The window could not be opened:" + Environment.NewLine +
                               Environment.NewLine + e.Message + Environment.NewLine + Environment.NewLine +
                               "Details: " + crash, "MGS4 DLSS Launcher", 0x10);
                    return 1;
                }
            }

            if (Catalogue.Find(opt.Stage) == null)
                Say("unknown scene '" + opt.Stage + "' - it is not in tools\\scenes.csv. Launching it anyway; --list shows the known ones.");
            return Runner.Run(opt, Say);
        }
    }
}
