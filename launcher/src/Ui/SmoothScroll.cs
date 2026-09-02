// Wheel notches and trackpad deltas both ease toward a target offset; only the time constant differs.
//
// WPF gives a notch three "lines" and applies it in one jump - and in a ListBox a "line" is a whole row, so the
// scene list moved three scenes at a time. The step is taken on CompositionTarget.Rendering, the frame the
// compositor is about to draw, and the distance covered depends on how long the frame took, so a dropped frame
// costs no ground. A DispatcherTimer was the obvious way to do this and the wrong one: it runs at Background
// priority, which measured 42 ticks a second with stalls to 147 ms, and every stall is a stutter.
//
// A precision touchpad reports the finger in deltas well under a notch, but it reports them in bursts rather than
// evenly, so applying each one as it arrives reads as chunky however small it is. It gets the same easing with a
// much shorter time constant: fast enough to stay under the finger, slow enough to smooth the bursts into a
// scrub.
//
// That delta is the only thing that tells the two apart, and it does not always tell the truth. A trackpad on the
// far end of a remote desktop arrives here as whatever that client synthesised - Moonlight relaying a MacBook
// sends whole notches, dozens a second, and nothing in the event says it was a finger. What gives it away is the
// clock: a hand cannot turn a notched wheel fifty times a second, so whole notches arriving faster than that were
// made by something that is not a wheel. Those take their distance from the time since the last one instead of
// from their own count, which moves the page at one speed no matter how many events the relay decides to send.
//
// A backstop under both: the target may only ever sit Ahead notches in front of the page that is drawn, so
// nothing - a misread stream, a wheel spun hard, a client nobody has seen yet - can launch the list into orbit.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace Mgs4Launcher
{
    class SmoothScroll
    {
        public static double Line = 34;         // pixels a "line" is worth, so Windows' default 3 is about a hundred
        public static double Tau = 70;          // ms to cover 63% of what is left, for a notch
        public static double TauFine = 22;      // the same for a trackpad, where the finger is still moving
        public static double Ahead = 3;         // notches the target may run in front of the drawn page, at most
        public static double StreamGap = 20;    // ms between whole notches below which no hand is turning a wheel
        public static double StreamSpeed = 1600;// px/s such a stream is given, whatever rate it arrives at

        readonly Dictionary<ScrollViewer, double> _targets = new Dictionary<ScrollViewer, double>();
        readonly Stopwatch _clock = Stopwatch.StartNew();
        readonly Stopwatch _since = Stopwatch.StartNew();   // since the last wheel event, to tell a hand from a relay
        double _tau = Tau;
        bool _running;

        Window _win;

        public static void Attach(Window win)
        {
            var s = new SmoothScroll { _win = win };
            win.PreviewMouseWheel += s.OnWheel;
        }

        // What one notch is worth here. Windows' own setting rather than a number of ours: Mouse settings calls it
        // "lines to scroll", it is 3 unless someone changed it, and -1 means they asked for a screen at a time.
        static double Notch(ScrollViewer sv)
        {
            int lines = SystemParameters.WheelScrollLines;
            if (lines <= 0) return Math.Max(sv.ViewportHeight - Line, Line);
            return lines * Line;
        }

        // The ScrollViewer a wheel event belongs to: the innermost one under the pointer that actually has
        // somewhere to go. Walking up from OriginalSource rather than trusting the sender means the scene list,
        // the settings pane, the Setup cards and the options column all work without naming any of them.
        static ScrollViewer HostFor(object source)
        {
            DependencyObject d = source as DependencyObject;
            while (d != null)
            {
                var sv = d as ScrollViewer;
                if (sv != null && sv.ScrollableHeight > 0) return sv;
                d = (d is Visual || d is System.Windows.Media.Media3D.Visual3D)
                    ? VisualTreeHelper.GetParent(d)
                    : LogicalTreeHelper.GetParent(d);
            }
            return null;
        }

        // Nothing under the pointer scrolls - the pointer is over a margin, a gap a panel does not take hit tests
        // for, or the header. Rather than hand the wheel back to WPF, which scrolls in whole rows and in jumps,
        // find the one thing on this tab that does scroll. Depth-first from the window and skipping what is not
        // on screen, so it is the visible tab's own scroller and never a collapsed one behind it.
        static ScrollViewer OnlyScroller(DependencyObject d)
        {
            if (d == null) return null;
            var fe = d as FrameworkElement;
            if (fe != null && fe.Visibility != Visibility.Visible) return null;
            var sv = d as ScrollViewer;
            if (sv != null && sv.ScrollableHeight > 0) return sv;
            int n = VisualTreeHelper.GetChildrenCount(d);
            for (int i = 0; i < n; i++)
            {
                ScrollViewer hit = OnlyScroller(VisualTreeHelper.GetChild(d, i));
                if (hit != null) return hit;
            }
            return null;
        }

        void OnWheel(object sender, MouseWheelEventArgs e)
        {
            ScrollViewer sv = HostFor(e.OriginalSource) ?? OnlyScroller(_win);
            if (sv == null) return;             // nothing here scrolls - leave the event alone
            e.Handled = true;

            double gap = _since.Elapsed.TotalMilliseconds;
            _since.Restart();

            double step = Notch(sv), px;
            if (Math.Abs(e.Delta) < 120)
            {
                // Under a full notch: a precision touchpad on this machine, saying how far the finger went.
                // Believe the distance, and catch up with it far sooner so the page stays under the finger.
                px = e.Delta / 120.0 * step;
                _tau = TauFine;
            }
            else if (gap < StreamGap)
            {
                // Whole notches faster than a hand can turn a wheel: a finger somewhere else, relayed as notches.
                // The count is the relay's invention, so the distance comes off the clock instead and the page
                // moves at StreamSpeed however many it sends. The first event of a flick still has a long gap
                // behind it and lands as a notch, which is what starts the scroll.
                px = Math.Sign(e.Delta) * StreamSpeed * Math.Min(gap, StreamGap) / 1000.0;
                _tau = TauFine;
            }
            else
            {
                px = e.Delta / 120.0 * step;
                _tau = Tau;
            }

            double from = _targets.ContainsKey(sv) ? _targets[sv] : sv.VerticalOffset;
            double to = from - px;

            // However many events arrived, the target stays within Ahead notches of what is on screen. One notch
            // is well inside that and lands whole; a flood is drained at the speed the easing can drain it.
            double now = sv.VerticalOffset, cap = Ahead * step;
            if (to > now + cap) to = now + cap;
            if (to < now - cap) to = now - cap;
            if (to < 0) to = 0;
            if (to > sv.ScrollableHeight) to = sv.ScrollableHeight;
            _targets[sv] = to;

            if (!_running)
            {
                _clock.Restart();
                CompositionTarget.Rendering += OnRender;
                _running = true;
            }
        }

        void OnRender(object sender, EventArgs e)
        {
            double dt = _clock.Elapsed.TotalMilliseconds;
            _clock.Restart();
            if (dt <= 0) return;
            if (dt > 200) dt = 200;             // after a long stall, glide the rest rather than teleporting
            double f = 1.0 - Math.Exp(-dt / _tau);

            var done = new List<ScrollViewer>();
            foreach (var kv in _targets)
            {
                ScrollViewer sv = kv.Key;
                double left = kv.Value - sv.VerticalOffset;
                // Under half a pixel from home: land exactly on it and stop, or the easing crawls forever.
                if (Math.Abs(left) < 0.5) { sv.ScrollToVerticalOffset(kv.Value); done.Add(sv); }
                else sv.ScrollToVerticalOffset(sv.VerticalOffset + left * f);
            }
            foreach (ScrollViewer sv in done) _targets.Remove(sv);

            if (_targets.Count == 0 && _running)
            {
                CompositionTarget.Rendering -= OnRender;
                _running = false;
            }
        }
    }
}
