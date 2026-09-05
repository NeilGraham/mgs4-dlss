// The rail down the left of the Setup and Settings tabs: a search box, room for a chip or two under it, then one
// line per card on the page, and a foot with "Collapse all / Expand all".
//
// It exists because neither page fits a window. Settings is eight groups of forty-odd rows and Setup is seven
// sections of files, and a standard window showed one and a half of either - so knowing what else was on the
// page meant scrolling to find out. The rail is the page's table of contents: every card named, each with its
// status as a dot in the card's own colour, the one under the reader's eye lit, and a click gliding the page to
// the card it names (SmoothScroll.Glide, so it reads as the page moving rather than being swapped).
//
// The owner builds the cards and tells the rail about them (Add), in page order, after clearing it (Clear) when
// the cards are rebuilt. The search box is the owner's to wire: what a row is made of differs between the two
// tabs, so the rail only holds the box and says when it changes.
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace Mgs4Launcher
{
    class Rail
    {
        public readonly TextBox Search;
        public readonly WrapPanel Chips;        // under the search: Setup puts its "Only problems" chip here
        public Action ExpandAll, CollapseAll;   // what the foot's two links do; set by the owner

        readonly ScrollViewer _sv;
        readonly Panel _host;
        readonly StackPanel _items = new StackPanel();
        readonly TextBlock _hint;
        readonly List<Entry> _entries = new List<Entry>();
        Entry _current;

        class Entry
        {
            public Border Item, Dot;
            public TextBlock Title;
            public FrameworkElement Card;
        }

        const string Lit = "#20263A", Hot = "#19191C";

        public Rail(Grid slot, ScrollViewer sv, Panel host, string hint)
        {
            _sv = sv;
            _host = host;
            slot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            slot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            slot.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            slot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            // The search, with its hint drawn over the empty box the way the scene list's is.
            var box = new Grid();
            Search = new TextBox { ToolTip = "Ctrl+F from anywhere on this tab; Escape clears it" };
            _hint = new TextBlock
            {
                Text = hint, FontSize = 12, Foreground = Widgets.Brush("#6E6E77"),
                IsHitTestVisible = false, VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(10, 0, 0, 0), TextTrimming = TextTrimming.CharacterEllipsis,
            };
            box.Children.Add(Search);
            box.Children.Add(_hint);
            Search.TextChanged += (s, e) =>
                _hint.Visibility = Search.Text.Length > 0 ? Visibility.Collapsed : Visibility.Visible;
            slot.Children.Add(box);

            Chips = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
            Grid.SetRow(Chips, 1);
            slot.Children.Add(Chips);

            // The list of cards. Its own scroller, bar hidden: nine rows fit any window this app allows, and if a
            // build ever adds a tenth the rail should still not push its foot off the bottom.
            var list = new ScrollViewer
            {
                VerticalScrollBarVisibility = ScrollBarVisibility.Hidden,
                Margin = new Thickness(0, 8, 0, 0),
                Background = Brushes.Transparent,
                Content = _items,
            };
            Grid.SetRow(list, 2);
            slot.Children.Add(list);

            var foot = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(10, 8, 0, 0) };
            foot.Children.Add(FootLink("Collapse all", () => { if (CollapseAll != null) CollapseAll(); }));
            foot.Children.Add(Widgets.Text("  ·  ", 11, "#4A4A52"));
            foot.Children.Add(FootLink("Expand all", () => { if (ExpandAll != null) ExpandAll(); }));
            Grid.SetRow(foot, 3);
            slot.Children.Add(foot);

            _sv.ScrollChanged += (s, e) => Sync();
        }

        static TextBlock FootLink(string text, Action act)
        {
            TextBlock t = Widgets.Text(text, 11, "#8FA6EE");
            t.Cursor = Cursors.Hand;
            t.MouseLeftButtonUp += (s, e) => act();
            t.MouseEnter += (s, e) => t.Foreground = Widgets.Brush("#B7C6FF");
            t.MouseLeave += (s, e) => t.Foreground = Widgets.Brush("#8FA6EE");
            return t;
        }

        public void Clear()
        {
            _items.Children.Clear();
            _entries.Clear();
            _current = null;
        }

        /// <summary>One line for a card: its title, and a dot in the colour of its status (null for none).</summary>
        public void Add(string title, string dot, FrameworkElement card, string tip = null)
        {
            var e = new Entry { Card = card };
            e.Item = new Border
            {
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(10, 6, 8, 6),
                Margin = new Thickness(0, 0, 0, 1),
                Background = Brushes.Transparent,
                Cursor = Cursors.Hand,
                ToolTip = tip,
            };
            Grid g = Widgets.Columns("*", "Auto");
            e.Title = Widgets.Text(title, 12, "#B8B8C2");
            e.Title.TextWrapping = TextWrapping.NoWrap;
            e.Title.TextTrimming = TextTrimming.CharacterEllipsis;
            e.Title.VerticalAlignment = VerticalAlignment.Center;
            g.Children.Add(e.Title);
            e.Dot = new Border
            {
                Width = 7, Height = 7, CornerRadius = new CornerRadius(4),
                Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center,
                Visibility = dot == null ? Visibility.Collapsed : Visibility.Visible,
                Background = dot == null ? Brushes.Transparent : Widgets.Brush(dot),
            };
            Grid.SetColumn(e.Dot, 1);
            g.Children.Add(e.Dot);
            e.Item.Child = g;

            e.Item.MouseLeftButtonUp += (s, ev) => Jump(e);
            e.Item.MouseEnter += (s, ev) => { if (e != _current) e.Item.Background = Widgets.Brush(Hot); };
            e.Item.MouseLeave += (s, ev) => { if (e != _current) e.Item.Background = Brushes.Transparent; };
            _items.Children.Add(e.Item);
            _entries.Add(e);
        }

        /// <summary>Recolour a card's dot after the fact - a settings group that now has an unsaved edit.</summary>
        public void Dot(FrameworkElement card, string color)
        {
            foreach (Entry e in _entries)
            {
                if (e.Card != card) continue;
                e.Dot.Visibility = color == null ? Visibility.Collapsed : Visibility.Visible;
                if (color != null) e.Dot.Background = Widgets.Brush(color);
            }
        }

        /// <summary>A card the page is not showing (filtered out) is still named, at a quarter strength, so the
        /// rail keeps saying what the page has even while a search is narrowing it.</summary>
        public void Shown(FrameworkElement card, bool shown)
        {
            foreach (Entry e in _entries)
            {
                if (e.Card != card) continue;
                e.Item.Opacity = shown ? 1 : 0.35;
                e.Item.IsHitTestVisible = shown;
            }
        }

        void Jump(Entry e)
        {
            if (e.Card.Visibility != Visibility.Visible) return;
            double top;
            try { top = e.Card.TransformToAncestor(_host).Transform(new Point(0, 0)).Y; }
            catch { return; }
            SmoothScroll.Glide(_sv, top);
            Light(e);
        }

        // The card under the reader's eye: the last one whose top is above a line a little way down the viewport,
        // or the last card there is once the page is scrolled to its foot - the short last card can never reach
        // the top, and the rail should still be able to say it is the one on screen.
        void Sync()
        {
            if (_entries.Count == 0) return;
            double offset = _sv.VerticalOffset;
            bool atFoot = _sv.ScrollableHeight > 0 && offset >= _sv.ScrollableHeight - 1;
            Entry hit = null;
            foreach (Entry e in _entries)
            {
                if (e.Card.Visibility != Visibility.Visible) continue;
                if (atFoot) { hit = e; continue; }
                double top;
                try { top = e.Card.TransformToAncestor(_host).Transform(new Point(0, 0)).Y; }
                catch { continue; }
                if (top <= offset + 48) hit = e;
                else break;
            }
            if (hit == null) foreach (Entry e in _entries) if (e.Card.Visibility == Visibility.Visible) { hit = e; break; }
            if (hit != null) Light(hit);
        }

        void Light(Entry hit)
        {
            if (hit == _current) return;
            if (_current != null)
            {
                _current.Item.Background = Brushes.Transparent;
                _current.Title.Foreground = Widgets.Brush("#B8B8C2");
                _current.Title.FontWeight = FontWeights.Normal;
            }
            _current = hit;
            hit.Item.Background = Widgets.Brush(Lit);
            hit.Title.Foreground = Widgets.Brush("#ECECEE");
            hit.Title.FontWeight = FontWeights.SemiBold;
        }
    }
}
