// The mgs4-dlss-launcher.exe wrapper: runs tools\mgs4_dlss_launcher.ps1 with no console window of any kind.
//
// Built by tools\build_app_exe.ps1, which also gives it the game's icon. It is a Windows-subsystem program
// (/target:winexe), so double-clicking it never flashes a console - a .bat cannot avoid that, because cmd.exe owns
// one before it can hide anything.
//
// Run from a terminal it still behaves like a command: AttachConsole hands it the console it was started from, the
// PowerShell child inherits that, and `mgs4-dlss-launcher.exe --list` or `--report` print where you typed them.
// The exit code is the script's.
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

static class App
{
    [DllImport("kernel32.dll")] static extern bool AttachConsole(int processId);
    [DllImport("kernel32.dll")] static extern bool SetStdHandle(int which, IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int which);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec,
                                     uint disposition, uint flags, IntPtr template);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    const int ATTACH_PARENT_PROCESS = -1;
    const int STD_INPUT = -10, STD_OUTPUT = -11, STD_ERROR = -12;
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
    static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);

    // AttachConsole gives the process a console but leaves its standard handles unset, so a child would inherit
    // nothing and its output would go nowhere. Point the unset ones at the console device before starting anything
    // - only the unset ones, because a caller redirecting us ("mgs4-dlss-launcher.exe --report > out.txt") has
    // already put a file there and that has to survive.
    static void BindStandardHandles()
    {
        BindOne(STD_OUTPUT, "CONOUT$", GENERIC_READ | GENERIC_WRITE);
        BindOne(STD_ERROR, "CONOUT$", GENERIC_READ | GENERIC_WRITE);
        BindOne(STD_INPUT, "CONIN$", GENERIC_READ | GENERIC_WRITE);
    }

    static void BindOne(int which, string device, uint access)
    {
        IntPtr existing = GetStdHandle(which);
        if (existing != IntPtr.Zero && existing != INVALID_HANDLE) return;
        IntPtr h = CreateFileW(device, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h != INVALID_HANDLE) { SetStdHandle(which, h); }
    }

    // Re-quote an argument the way CommandLineToArgvW will read it back.
    static string Quote(string a)
    {
        if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
        var sb = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in a)
        {
            if (c == '\\') { slashes++; continue; }
            if (c == '"') { sb.Append('\\', slashes * 2 + 1).Append('"'); }
            else { sb.Append('\\', slashes).Append(c); }
            slashes = 0;
        }
        sb.Append('\\', slashes * 2).Append('"');
        return sb.ToString();
    }

    [STAThread]
    static int Main(string[] args)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        string script = Path.Combine(dir, "tools\\mgs4_dlss_launcher.ps1");
        if (!File.Exists(script))
        {
            MessageBoxW(IntPtr.Zero,
                "Could not find tools\\mgs4_dlss_launcher.ps1 next to this program.\n\n" +
                "Run mgs4-dlss-launcher.exe from the folder it was unzipped into.",
                "MGS4 DLSS Launcher", 0x10);
            return 1;
        }

        // A console only exists if someone started this from one; that decides whether the child gets to write.
        bool console = AttachConsole(ATTACH_PARENT_PROCESS);
        if (console) { BindStandardHandles(); }

        var sb = new StringBuilder();
        sb.Append("-NoProfile -ExecutionPolicy Bypass -File ").Append(Quote(script));
        foreach (string a in args) sb.Append(' ').Append(Quote(a));

        var psi = new ProcessStartInfo("powershell.exe", sb.ToString())
        {
            UseShellExecute = false,
            CreateNoWindow = !console,
            WorkingDirectory = dir,
        };
        try
        {
            using (Process p = Process.Start(psi))
            {
                p.WaitForExit();
                return p.ExitCode;
            }
        }
        catch (Exception e)
        {
            MessageBoxW(IntPtr.Zero, "Could not start PowerShell:\n\n" + e.Message, "MGS4 DLSS Launcher", 0x10);
            return 1;
        }
    }
}
