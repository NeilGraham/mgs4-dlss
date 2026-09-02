// The pieces every tab is built from: text, cards, rows, and the status colours. Built in code rather than XAML
// because the content is data - a card per manifest group, a row per file - and the XAML holds the chrome.
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Mgs4Launcher
{
    class StatusStyle { public string Bg, Br, Fg; }

    static class Widgets
    {
        public static readonly Dictionary<string, StatusStyle> Status = new Dictionary<string, StatusStyle>
        {
            { "ok",   new StatusStyle { Bg = "#142117", Br = "#2E6B45", Fg = "#62C98A" } },
            { "warn", new StatusStyle { Bg = "#2A2312", Br = "#7A6220", Fg = "#F2C14E" } },
            { "bad",  new StatusStyle { Bg = "#2A1315", Br = "#7A2A2F", Fg = "#FF6B66" } },
            { "info", new StatusStyle { Bg = "#1C1C20", Br = "#3A3A44", Fg = "#B8B8C2" } },
        };

        public static Style LinkStyle, FlatStyle, PrimaryStyle, ChipStyle;

        public static Brush Brush(string hex)
        {
            return new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        }

        public static TextBlock Text(string text, double size, string color, bool bold = false, bool mono = false)
        {
            var t = new TextBlock
            {
                Text = text ?? "",
                FontSize = size,
                Foreground = Brush(color),
                TextWrapping = TextWrapping.Wrap,
            };
            if (bold) t.FontWeight = FontWeights.SemiBold;
            if (mono) t.FontFamily = new FontFamily("Consolas");
            return t;
        }

        public static Grid Columns(params string[] widths)
        {
            var g = new Grid();
            foreach (string w in widths)
            {
                var cd = new ColumnDefinition();
                if (w == "*") cd.Width = new GridLength(1, GridUnitType.Star);
                else if (w == "Auto") cd.Width = GridLength.Auto;
                else cd.Width = new GridLength(double.Parse(w));
                g.ColumnDefinitions.Add(cd);
            }
            return g;
        }

        // The badges a settings group wears, left of its title: whose setting it is, and whether it is a
        // diagnostic. Their own small palette, in the families the rest of the window uses.
        static readonly Dictionary<string, StatusStyle> BadgeStyles = new Dictionary<string, StatusStyle>
        {
            { "Game",      new StatusStyle { Bg = "#1D1D21", Br = "#4A4A55", Fg = "#D2D2DA" } },
            { "MGS4 DLSS", new StatusStyle { Bg = "#161B2A", Br = "#33436E", Fg = "#9FB6FF" } },
            { "Debug",     new StatusStyle { Bg = "#2A2312", Br = "#7A6220", Fg = "#F2C14E" } },
            { "RenoDX",    new StatusStyle { Bg = "#1E1E23", Br = "#45454F", Fg = "#B3B3BE" } },
        };

        public static Border Badge(string text)
        {
            StatusStyle st = BadgeStyles.ContainsKey(text) ? BadgeStyles[text] : Status["info"];
            return new Border
            {
                Background = Brush(st.Bg),
                BorderBrush = Brush(st.Br),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(8, 2, 8, 2),
                Margin = new Thickness(0, 0, 8, 0),
                VerticalAlignment = VerticalAlignment.Center,
                Child = Text(text, 10, st.Fg, true),
            };
        }

        // A card: a titled panel with a status tag, and a body the caller fills with rows.
        public static Border Card(string title, string blurb, string tagKind, string tagLabel, out StackPanel body)
        {
            return Card(title, blurb, tagKind, tagLabel, null, out body);
        }

        public static Border Card(string title, string blurb, string tagKind, string tagLabel,
                                  IEnumerable<string> badges, out StackPanel body)
        {
            var card = new Border
            {
                Background = Brush("#151517"),
                BorderBrush = Brush("#26262A"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(10),
                Margin = new Thickness(0, 0, 0, 14),
            };
            var stack = new StackPanel();

            var hdr = new Border
            {
                Background = Brush("#17171A"),
                BorderBrush = Brush("#26262A"),
                BorderThickness = new Thickness(0, 0, 0, 1),
                CornerRadius = new CornerRadius(10, 10, 0, 0),
                Padding = new Thickness(18, 13, 18, 13),
            };
            Grid hg = Columns("*", "Auto");
            var hs = new StackPanel();
            var titleRow = new StackPanel { Orientation = Orientation.Horizontal };
            if (badges != null)
                foreach (string b in badges) titleRow.Children.Add(Badge(b));
            TextBlock titleText = Text(title, 14, "#ECECEE", true);
            titleText.VerticalAlignment = VerticalAlignment.Center;
            titleRow.Children.Add(titleText);
            hs.Children.Add(titleRow);
            if (!string.IsNullOrEmpty(blurb))
            {
                TextBlock b = Text(blurb, 11, "#97979F");
                b.Margin = new Thickness(0, 2, 12, 0);
                hs.Children.Add(b);
            }
            hg.Children.Add(hs);
            if (!string.IsNullOrEmpty(tagLabel))
            {
                StatusStyle st = Status.ContainsKey(tagKind) ? Status[tagKind] : Status["info"];
                var tag = new Border
                {
                    Background = Brush(st.Bg),
                    BorderBrush = Brush(st.Br),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(4),
                    Padding = new Thickness(12, 4, 12, 4),
                    VerticalAlignment = VerticalAlignment.Center,
                    Child = Text(tagLabel, 11, st.Fg, true),
                };
                Grid.SetColumn(tag, 1);
                hg.Children.Add(tag);
            }
            hdr.Child = hg;
            stack.Children.Add(hdr);
            card.Child = stack;
            body = stack;
            return card;
        }

        // A row of explanation with one button on the right - the shape every "do it for me" row in Setup takes.
        public static Border ActionRow(string text, string buttonLabel, string tooltip, bool enabled,
                                       bool primary, RoutedEventHandler onClick)
        {
            var b = new Border
            {
                Background = Brush("#101012"),
                BorderBrush = Brush("#202023"),
                BorderThickness = new Thickness(0, 0, 0, 1),
                Padding = new Thickness(18, 12, 18, 12),
            };
            Grid g = Columns("*", "Auto");
            TextBlock t = Text(text, 11, "#A9A9B1");
            t.VerticalAlignment = VerticalAlignment.Center;
            t.Margin = new Thickness(0, 0, 16, 0);
            g.Children.Add(t);
            var btn = new Button
            {
                Content = buttonLabel,
                Style = primary && enabled ? PrimaryStyle : FlatStyle,
                IsEnabled = enabled,
                ToolTip = tooltip,
                VerticalAlignment = VerticalAlignment.Center,
            };
            btn.Click += onClick;
            Grid.SetColumn(btn, 1);
            g.Children.Add(btn);
            b.Child = g;
            return b;
        }

        // What a group's files are and where they come from, above the files themselves.
        public static Border GuideRow(Section sec)
        {
            var b = new Border
            {
                Background = Brush("#101012"),
                BorderBrush = Brush("#202023"),
                BorderThickness = new Thickness(0, 0, 0, 1),
                Padding = new Thickness(18, 12, 18, 12),
            };
            Grid g = Columns("*", "Auto");
            TextBlock t = Text(sec.Guide, 11, "#A9A9B1");
            t.VerticalAlignment = VerticalAlignment.Center;
            t.Margin = new Thickness(0, 0, 16, 0);
            g.Children.Add(t);
            if (!string.IsNullOrEmpty(sec.Url))
            {
                var link = new Button
                {
                    Content = (string.IsNullOrEmpty(sec.UrlLabel) ? "Get the files" : sec.UrlLabel) + "  " + '→',
                    Style = LinkStyle,
                    Tag = sec.Url,
                    ToolTip = sec.Url,
                    VerticalAlignment = VerticalAlignment.Center,
                };
                link.Click += (s, e) => Open(((Button)s).Tag as string);
                Grid.SetColumn(link, 1);
                g.Children.Add(link);
            }
            b.Child = g;
            return b;
        }

        // One row of the install check: the path, what it is, the value found, and a link when that one file comes
        // from somewhere other than its group.
        public static Border CheckRow(Row row, bool first)
        {
            StatusStyle st = Status.ContainsKey(row.Status) ? Status[row.Status] : Status["info"];
            var rb = new Border { Padding = new Thickness(18, 11, 18, 11) };
            if (!first)
            {
                rb.BorderBrush = Brush("#202023");
                rb.BorderThickness = new Thickness(0, 1, 0, 0);
            }
            Grid g = Columns("28", "*", "Auto");
            string glyph = row.Status == "ok" ? "✓" : row.Status == "warn" ? "⚠" : row.Status == "bad" ? "✕" : "•";
            TextBlock mark = Text(glyph, 13, st.Fg, true);
            mark.VerticalAlignment = VerticalAlignment.Center;
            g.Children.Add(mark);

            var mid = new StackPanel();
            TextBlock name = Text(row.Name, 12, "#ECECEE", false, true);
            mid.Children.Add(name);
            if (!string.IsNullOrEmpty(row.Detail))
            {
                TextBlock d = Text(row.Detail, 11, "#97979F");
                d.Margin = new Thickness(0, 2, 12, 0);
                mid.Children.Add(d);
            }
            if (!string.IsNullOrEmpty(row.Url))
            {
                var lb = new Button { Content = row.Url, Style = LinkStyle, Tag = row.Url, HorizontalAlignment = HorizontalAlignment.Left };
                lb.Click += (s, e) => Open(((Button)s).Tag as string);
                mid.Children.Add(lb);
            }
            Grid.SetColumn(mid, 1);
            g.Children.Add(mid);

            TextBlock val = Text(row.Value, 11, st.Fg, false, true);
            val.VerticalAlignment = VerticalAlignment.Center;
            val.Margin = new Thickness(12, 0, 0, 0);
            val.TextWrapping = TextWrapping.NoWrap;
            Grid.SetColumn(val, 2);
            g.Children.Add(val);

            rb.Child = g;
            return rb;
        }

        // A folder in Explorer. Separate from Open so the caller cannot hand the shell a path that is gone.
        public static void OpenFolder(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !System.IO.Directory.Exists(dir)) return;
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(dir) { UseShellExecute = true }); }
            catch { }
        }

        public static void Open(string url)
        {
            if (string.IsNullOrEmpty(url)) return;
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true }); }
            catch { }
        }
    }
}
