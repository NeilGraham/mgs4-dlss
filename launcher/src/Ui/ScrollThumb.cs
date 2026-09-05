// A floor under the scrollbar thumb's length.
//
// Track sizes the thumb by the viewport-to-content ratio and nothing else: a 560px window over the scene list's
// four hundred rows put a thumb on screen a few pixels tall, which is not a thing a pointer can take hold of.
// The obvious fix - a MinHeight on the Thumb - is wrong in a way that is worse than the problem: Track still
// arranges the thumb at the length it computed and drags by that length, so the thumb was *drawn* forty pixels
// tall while Track believed it was six. It overhung the bottom of the track by the difference, and a drag moved
// the page by the wrong ratio.
//
// So the number Track is given is changed instead of the drawing. The thumb's length is
//
//     track * viewport / (scrollable + viewport)
//
// and Track reads the viewport off its own ViewportSize property, which its template binds to the ScrollBar's.
// That binding is only put in place when the property has no value of its own (Track.BindToTemplatedParent),
// so a binding of ours on the Track wins, and the ScrollBar's own ViewportSize - which the ScrollViewer reads
// for paging - is never touched. Ours hands Track the real viewport until the thumb would drop under Floor, and
// from there the viewport that makes the thumb exactly Floor long. Everything downstream of that number - where
// the thumb sits, how far a drag moves the page - is worked out from the same length, so it all agrees.
//
// Every vertical ScrollBar in the window gets this through a class handler on Loaded: the list, the settings
// page, the Setup cards, the options column, the combo popups - none of them are named here.
using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;

namespace Mgs4Launcher
{
    class ScrollThumb : IMultiValueConverter
    {
        public const double Floor = 44;     // the thumb never draws shorter than this, in logical pixels
        static bool _registered;

        public static void Attach()
        {
            if (_registered) return;
            _registered = true;
            EventManager.RegisterClassHandler(typeof(ScrollBar), FrameworkElement.LoadedEvent,
                                              new RoutedEventHandler(OnLoaded));
        }

        static void OnLoaded(object sender, RoutedEventArgs e)
        {
            var bar = sender as ScrollBar;
            if (bar == null || bar.Orientation != Orientation.Vertical || bar.Template == null) return;
            var track = bar.Template.FindName("PART_Track", bar) as Track;
            if (track == null) return;
            // Loaded fires again whenever the bar is re-parented (a recycled row, a tab shown again); once is enough.
            if (BindingOperations.GetMultiBindingExpression(track, Track.ViewportSizeProperty) != null) return;

            var mb = new MultiBinding { Converter = new ScrollThumb() };
            mb.Bindings.Add(new Binding("ViewportSize") { Source = bar });
            mb.Bindings.Add(new Binding("Maximum") { Source = bar });
            mb.Bindings.Add(new Binding("ActualHeight") { Source = track });
            track.SetBinding(Track.ViewportSizeProperty, mb);
        }

        public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
        {
            double viewport = Num(values, 0), scrollable = Num(values, 1), track = Num(values, 2);
            if (viewport <= 0 || scrollable <= 0 || track <= Floor) return viewport;
            double natural = track * viewport / (scrollable + viewport);
            if (natural >= Floor) return viewport;
            // The viewport that makes the thumb come out exactly Floor long on this track.
            return Floor * scrollable / (track - Floor);
        }

        static double Num(object[] values, int i)
        {
            if (values == null || i >= values.Length || !(values[i] is double)) return 0;
            double d = (double)values[i];
            return double.IsNaN(d) || double.IsInfinity(d) ? 0 : d;
        }

        public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture)
        {
            throw new NotSupportedException();
        }
    }
}
