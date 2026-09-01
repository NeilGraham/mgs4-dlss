// The window's own title bar is drawn by Windows, not by WPF, so it ignores everything the XAML says and comes up
// light unless the app asks otherwise. DwmSetWindowAttribute is the ask; the desktop's own setting is what it is
// asked for, and it is re-applied when that setting changes, so switching Windows to light or dark while the
// window is open moves the bar with it.
using System;
using System.Windows;
using System.Windows.Interop;
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

        // AppsUseLightTheme is the per-user setting behind Settings > Personalisation > Colours > "Choose your
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
            // bar is never seen in the wrong colour.
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
    }
}
