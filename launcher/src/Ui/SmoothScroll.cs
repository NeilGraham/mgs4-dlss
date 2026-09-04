// Wheel notches and trackpad deltas both ease toward a target offset; only the time constant differs.
//
// WPF gives a notch three "lines" and applies it in one jump - and in a ListBox a "line" is a whole row, so the
// scene list moved three scenes at a time. Hence a notch measured in pixels, and an easing to carry it.
//
// A precision touchpad reports the finger in deltas well under a notch, but it reports them in bursts rather than
// evenly, so applying each one as it arrives reads as chunky however small it is. It gets the same easing with a
// much shorter time constant: fast enough to stay under the finger, slow enough to smooth the bursts into a
// scrub.
//
// A notch is worth a notch, and how fast it arrived is not part of that. Push the wheel five notches in a tenth
// of a second and five notches in half a second and the page ends in the same place both times - the fast push
// only takes a moment longer to settle there. Two things here used to break that rule and both are gone:
//
//   - the target was clamped to within three notches of the offset already drawn, so a push faster than the
//     easing could keep up with had the rest of its distance deleted. That is the one you feel: the harder you
//     push, the less of the push survives.
//   - a notch's distance was then scaled by the time since the last notch, to stop a relayed finger - Moonlight
//     sends whole notches dozens a second, and nothing in the event says it was not a wheel - from moving the
//     page at ten thousand pixels a second. It stopped that by making every quick notch worth less, which is the
//     same bug wearing a different hat.
//
// What is left is the plain reading: distance is the notch count times what Windows says a notch is worth, the
// target accumulates it, and the only thing that ever removes distance is the top and bottom of the page. The
// relay is answered where it should have been all along - not in the distance, but in the speed the page is
// allowed to travel at. MaxSpeed is a ceiling on how fast the drawn offset may move: it is there so that a big
// accumulated push glides at a speed you can still read, rather than crossing a four-hundred-row list in a blur.
// At 240Hz it works out around 37px a frame. It changes how long the journey takes and never where it ends -
// an eight-notch push measured 816px on the nose whether it was delivered in 0.03s or in 0.5s.
//
// The step is taken on CompositionTarget.Rendering, the frame the compositor is about to draw, and the distance
// covered depends on how long the frame took, so a dropped frame costs no ground. A DispatcherTimer was the
// obvious way to do this and the wrong one: it runs at Background priority, which measured 42 ticks a second with
// stalls to 147 ms, and every stall is a stutter.
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
        public static double MaxSpeed = 9000;   // px/s the drawn page may travel at, at most - see the note above

        readonly Dictionary<ScrollViewer, double> _targets = new Dictionary<ScrollViewer, double>();
        readonly Stopwatch _clock = Stopwatch.StartNew();
        readonly Stopwatch _since = Stopwatch.StartNew();   // since the last wheel event; nothing reads it but the trace
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
            else
            {
                // A whole notch, worth what Windows says a notch is worth. Nothing about the clock comes into it:
                // the same push delivered fast and delivered slowly asks for the same distance.
                px = e.Delta / 120.0 * step;
                _tau = Tau;
            }

            // Off the target rather than off the drawn offset, so notches that arrive while the page is still
            // moving add to where it was going instead of to where it happens to have got.
            double from = _targets.ContainsKey(sv) ? _targets[sv] : sv.VerticalOffset;
            double to = from - px;

            // The page is the only thing that removes distance.
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
                else
                {
                    // The easing's own step, then the ceiling on top of it. The ceiling only ever makes the
                    // journey longer - the target is untouched, so the page still ends exactly where the notches
                    // asked it to.
                    double move = left * f, most = MaxSpeed * dt / 1000.0;
                    if (Math.Abs(move) > most) move = Math.Sign(move) * most;
                    sv.ScrollToVerticalOffset(sv.VerticalOffset + move);
                }
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
