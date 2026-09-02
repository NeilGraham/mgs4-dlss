// A recording of one scroll, for telling a tab that feels smooth from one that does not.
//
// It answers the questions a feel cannot: what the wheel actually sent (how many events and how far apart), how
// much distance that asked for against how much the page actually covered, and what the window managed to draw
// while catching up (the gap between composited frames, and how many of those were long enough to see). All of
// it per burst - one push of the wheel or one drag of a finger - because a burst is the unit a hand notices.
//
// asked and landed are the pair worth reading. They should match, and the same push delivered fast and delivered
// slowly should land the same number either way; when they do not match, the page ran out of room.
//
// Off unless MGS4_SCROLL_LOG is set, and it writes one line per burst to %TEMP%\mgs4-scroll.log:
//
//   set MGS4_SCROLL_LOG=1
//   mgs4-dlss-launcher.exe
//
// Nothing here changes how the window scrolls; it only watches. Delete this file and the four Trace calls in
// SmoothScroll.cs once the question is settled.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Mgs4Launcher
{
    static class ScrollTrace
    {
        public static readonly bool On =
            !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MGS4_SCROLL_LOG"));

        static readonly string Path = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(), "mgs4-scroll.log");

        static readonly Stopwatch Clock = Stopwatch.StartNew();
        static readonly List<double> Gaps = new List<double>();     // ms between wheel events
        static readonly List<double> Frames = new List<double>();   // ms between composited frames
        static readonly int[] Branch = new int[3];                  // sub-notch finger / unused / whole notch
        static readonly List<double> Steps = new List<double>();    // px each event was worth
        static readonly List<double> Taus = new List<double>();     // the time constant each event chose
        static string Where = "?";
        static ScrollViewer Host;
        static double Extent, Viewport, Travel, Start;
        static bool Started;

        public static void Wheel(ScrollViewer sv, int delta, double gap, int branch, double px, double tau)
        {
            if (!On) return;
            if (!Started) { Started = true; Gaps.Clear(); Frames.Clear(); Steps.Clear(); Taus.Clear();
                            Array.Clear(Branch, 0, 3); Travel = 0; Start = sv.VerticalOffset; }
            Host = sv;
            Steps.Add(Math.Abs(px));
            Taus.Add(tau);
            Where = Name(sv);
            Extent = sv.ExtentHeight;
            Viewport = sv.ViewportHeight;
            if (Gaps.Count > 0 || gap < 1000) Gaps.Add(gap);
            if (branch >= 0 && branch < 3) Branch[branch]++;
            Travel += Math.Abs(px);
        }

        public static void Frame(double dt)
        {
            if (!On || !Started) return;
            Frames.Add(dt);
        }

        // The burst is over - the easing has landed and nothing is moving. Write what it took.
        public static void End()
        {
            if (!On || !Started) return;
            Started = false;
            if (Frames.Count == 0) return;
            try
            {
                File.AppendAllText(Path, string.Format(
                    "{0,8:0.0}s  {1,-12} extent={2,-8:0} view={3,-6:0} asked={4,-6:0}px landed={15,-6:0}px  " +
                    "wheel: n={5,-4} gap {6}  events finger/notch={7}/{9}\r\n" +
                    "          step   {10}\r\n" +
                    "          tau    {11}\r\n" +
                    "          frames n={12,-4} {13}  slow(>25ms)={14}\r\n",
                    Clock.Elapsed.TotalSeconds, Where, Extent, Viewport, Travel,
                    Gaps.Count, Spread(Gaps), Branch[0], Branch[1], Branch[2],
                    Spread(Steps), Spread(Taus),
                    Frames.Count, Spread(Frames), Over(Frames, 25),
                    Host != null ? Math.Abs(Host.VerticalOffset - Start) : 0.0) );
            }
            catch { }
        }

        static string Spread(List<double> xs)
        {
            if (xs.Count == 0) return "(none)";
            var s = new List<double>(xs);
            s.Sort();
            double sum = 0;
            foreach (double x in s) sum += x;
            return string.Format("mean={0:0.0} med={1:0.0} p95={2:0.0} max={3:0.0}",
                                 sum / s.Count, s[s.Count / 2], s[(int)((s.Count - 1) * 0.95)], s[s.Count - 1]);
        }

        static int Over(List<double> xs, double ms)
        {
            int n = 0;
            foreach (double x in xs) if (x > ms) n++;
            return n;
        }

        // Which tab this ScrollViewer belongs to, read off the nearest named ancestor rather than guessed.
        static string Name(ScrollViewer sv)
        {
            DependencyObject d = sv;
            while (d != null)
            {
                var fe = d as FrameworkElement;
                if (fe != null && !string.IsNullOrEmpty(fe.Name)) return fe.Name;
                d = VisualTreeHelper.GetParent(d);
            }
            return sv.GetType().Name;
        }
    }
}
