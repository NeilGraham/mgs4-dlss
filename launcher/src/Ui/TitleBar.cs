// The title bar. There is no system one any more - WindowChrome hands the whole window to WPF so the key art can
// fill the strip the caption had - so this draws the three buttons and says what is draggable. The DWM call is
// still made: the frame Windows draws around the window, and its shadow, follow the desktop's light or dark
// setting, and it is re-applied when that setting changes.
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Shell;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Mgs4Launcher
{
    static class TitleBar
    {
        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        // 20 on Windows 10 2004 and later, including 11; 19 on the 1809-1909 builds that had it under the old
        // number. Both are tried, and a build with neither simply returns an error and keeps its light bar.
        const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        const int DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;

        // AppsUseLightTheme is the per-user setting behind Settings > Personalization > Colors > "Choose your
        // default app mode". 0 means dark. Absent (older builds, or a policy that removed it) means light.
        public static bool SystemUsesDarkMode()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(
                           "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"))
                {
                    if (k == null) return false;
                    object v = k.GetValue("AppsUseLightTheme");
                    return v is int && (int)v == 0;
                }
            }
            catch { return false; }
        }

        public static void Follow(Window win)
        {
            Action apply = () =>
            {
                IntPtr h = new WindowInteropHelper(win).Handle;
                if (h == IntPtr.Zero) return;
                int dark = SystemUsesDarkMode() ? 1 : 0;
                if (DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int)) != 0)
                    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1, ref dark, sizeof(int));
            };

            // The handle only exists from SourceInitialized on, which is before the first frame is drawn - so the
            // bar is never seen in the wrong color.
            if (new WindowInteropHelper(win).Handle != IntPtr.Zero) apply();
            else win.SourceInitialized += (s, e) => apply();

            UserPreferenceChangedEventHandler onPref = (s, e) =>
            {
                if (e.Category == UserPreferenceCategory.General || e.Category == UserPreferenceCategory.Color)
                    win.Dispatcher.BeginInvoke(apply);
            };
            SystemEvents.UserPreferenceChanged += onPref;
            win.Closed += (s, e) => SystemEvents.UserPreferenceChanged -= onPref;
        }

        // Segoe MDL2 Assets, the font Windows draws its own caption buttons from: a chevron-free minimize, the
        // empty square for maximize and the two overlapping ones for restore, and the close cross.
        const string Minimize = "", Maximize = "", Restore = "";

        // The buttons, and the drag region. The chrome's caption is only 32 tall in the markup; it is raised here
        // to the whole header, so the bar drags and double-clicks like the title bar it replaced - anything in it
        // that takes clicks of its own is marked IsHitTestVisibleInChrome in the markup.
        public static void Buttons(Window win, Button min, Button max, Button close, FrameworkElement header)
        {
            min.Click += (s, e) => win.WindowState = WindowState.Minimized;
            max.Click += (s, e) => Toggle(win);
            close.Click += (s, e) => win.Close();

            Action state = () =>
            {
                bool up = win.WindowState == WindowState.Maximized;
                max.Content = up ? Restore : Maximize;
                max.ToolTip = up ? "Restore" : "Maximize";
                // A maximized window is sized to the monitor plus its resize border, so without this the edges of
                // the content - and the close button - sit off the screen.
                Thickness pad = SystemParameters.WindowResizeBorderThickness;
                win.BorderThickness = up ? new Thickness(pad.Left, pad.Top, pad.Right, pad.Bottom) : new Thickness(0);
            };
            win.StateChanged += (s, e) => state();
            state();

            WindowChrome chrome = WindowChrome.GetWindowChrome(win);
            if (chrome == null || header == null) return;
            SizeChangedEventHandler caption = (s, e) =>
            {
                if (header.ActualHeight > 0) chrome.CaptionHeight = header.ActualHeight;
            };
            header.SizeChanged += caption;
            caption(null, null);
        }

        static void Toggle(Window win)
        {
            win.WindowState = win.WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
        }
    }
}
