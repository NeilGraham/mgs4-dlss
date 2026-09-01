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
        public static double Step = 48;         // pixels a wheel notch asks for
        public static double Tau = 70;          // ms to cover 63% of what is left, for a notch
        public static double TauFine = 22;      // the same for a trackpad, where the finger is still moving

        readonly Dictionary<ScrollViewer, double> _targets = new Dictionary<ScrollViewer, double>();
        readonly Stopwatch _clock = Stopwatch.StartNew();
        double _tau = Tau;
        bool _running;

        public static void Attach(Window win)
        {
            var s = new SmoothScroll();
            win.PreviewMouseWheel += s.OnWheel;
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

        void OnWheel(object sender, MouseWheelEventArgs e)
        {
            ScrollViewer sv = HostFor(e.OriginalSource);
            if (sv == null) return;             // nothing here scrolls - leave the event alone
            e.Handled = true;

            // Anything under a full notch came from a precision touchpad: same distance per unit of movement,
            // but caught up with far sooner, so the page stays under the finger.
            _tau = Math.Abs(e.Delta) < 120 ? TauFine : Tau;

            double from = _targets.ContainsKey(sv) ? _targets[sv] : sv.VerticalOffset;
            double to = from - e.Delta / 120.0 * Step;
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
