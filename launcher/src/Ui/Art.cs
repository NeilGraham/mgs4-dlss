// The game's own artwork, from Steam's cache on this machine, and its icon out of mgs4.exe. None of it is in this
// repo: it is Konami's, and it is already on the machine of anyone who owns the game. Every piece falls back to
// plain text when it is not there.
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Mgs4Launcher
{
    static class Art
    {
        // Steam keeps library_hero.jpg and logo.png for every owned game under appcache\librarycache. Newer Steam
        // nests them in a hash folder per image, older builds name them <appid>_hero.jpg beside each other.
        public static Dictionary<string, string> Find()
        {
            var wanted = new Dictionary<string, string[]>
            {
                { "Hero", new[] { "library_hero.jpg", Paths.AppId + "_library_hero.jpg", Paths.AppId + "_hero.jpg" } },
                { "Logo", new[] { "logo.png", Paths.AppId + "_logo.png" } },
                { "Capsule", new[] { "library_capsule.jpg", Paths.AppId + "_library_600x900.jpg" } },
            };
            var art = new Dictionary<string, string>();
            foreach (string root in Paths.SteamLibraries())
            {
                string cache = Paths.Join(root, "appcache\\librarycache");
                if (!Paths.Exists(cache)) continue;
                // The per-app folder is small, so recursing it is cheap; the shared cache holds every game the
                // account owns, so that one is looked at flat.
                foreach (var pair in new[] { new { Dir = Paths.Join(cache, Paths.AppId), Deep = true },
                                             new { Dir = cache, Deep = false } })
                {
                    if (!Paths.Exists(pair.Dir)) continue;
                    foreach (var key in new List<string>(wanted.Keys))
                    {
                        if (art.ContainsKey(key)) continue;
                        foreach (string name in wanted[key])
                        {
                            string[] hits;
                            try
                            {
                                hits = Directory.GetFiles(pair.Dir, name,
                                    pair.Deep ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly);
                            }
                            catch { continue; }
                            if (hits.Length > 0) { art[key] = hits[0]; break; }
                        }
                    }
                }
                if (art.ContainsKey("Hero") && art.ContainsKey("Logo")) break;
            }
            return art;
        }

        public static BitmapImage Load(string path)
        {
            if (!Paths.Exists(path)) return null;
            try
            {
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.CacheOption = BitmapCacheOption.OnLoad;
                bmp.UriSource = new Uri(path);
                bmp.EndInit();
                bmp.Freeze();
                return bmp;
            }
            catch { return null; }
        }

        // Steam's logo.png carries a lot of transparent margin; trimming it lets the header set the logo's real
        // height rather than the file's.
        public static BitmapSource Trim(BitmapSource src)
        {
            if (src == null) return null;
            try
            {
                var conv = new FormatConvertedBitmap(src, PixelFormats.Bgra32, null, 0);
                int w = conv.PixelWidth, h = conv.PixelHeight, stride = w * 4;
                var px = new byte[h * stride];
                conv.CopyPixels(px, stride, 0);
                int minX = w, minY = h, maxX = -1, maxY = -1;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (px[y * stride + x * 4 + 3] > 12)
                        {
                            if (x < minX) minX = x;
                            if (x > maxX) maxX = x;
                            if (y < minY) minY = y;
                            if (y > maxY) maxY = y;
                        }
                if (maxX < minX || maxY < minY) return src;
                var crop = new CroppedBitmap(conv, new Int32Rect(minX, minY, maxX - minX + 1, maxY - minY + 1));
                crop.Freeze();
                return crop;
            }
            catch { return src; }
        }

        // The logo in place of the title, and the key art behind the header: on the left, the full height of the
        // band, uncropped. The band is the header plus a strip that reaches down into the content, and its bottom
        // is faded out, so the art keeps the height it always had on screen while showing all of itself and
        // dissolving into the window instead of stopping on a line.
        const double Bleed = 64;        // how far past the header the art carries on, before it has faded away
        const double Caption = 32;      // the strip at the top that used to be the system title bar

        public static void ApplyHeader(Window win, Image logoArt, TextBlock titleText,
                                       System.Windows.Shapes.Rectangle heroArt, Border headerBar, Grid artBand)
        {
            Dictionary<string, string> art;
            try { art = Find(); } catch { art = new Dictionary<string, string>(); }

            string logoPath, heroPath;
            art.TryGetValue("Logo", out logoPath);
            art.TryGetValue("Hero", out heroPath);

            BitmapImage logo = Load(logoPath);
            if (logo != null)
            {
                logoArt.Source = Trim(logo) ?? (ImageSource)logo;
                logoArt.Visibility = Visibility.Visible;
                titleText.Visibility = Visibility.Collapsed;
            }

            BitmapImage hero = Load(heroPath);
            double aspect = 0;
            if (hero != null && hero.PixelHeight > 0)
            {
                var brush = new ImageBrush(hero)
                {
                    Stretch = Stretch.Uniform,
                    AlignmentX = AlignmentX.Left,
                    AlignmentY = AlignmentY.Center,
                };
                brush.Freeze();
                heroArt.Fill = brush;
                heroArt.Visibility = Visibility.Visible;
                aspect = (double)hero.PixelWidth / hero.PixelHeight;
            }

            // The band has no height of its own - it is rectangles over a row that sizes to the header beside it -
            // so it is measured from the header and redone on resize. How far right the logo has to start to clear
            // Snake's face rides on the same number: it is a fraction of how wide the art comes out.
            SizeChangedEventHandler layout = (s, e) =>
            {
                double h = headerBar.ActualHeight;
                if (h <= 0) return;
                double band = h + Bleed;
                artBand.Height = band;
                artBand.OpacityMask = Fade(h / band);
                if (aspect <= 0) return;
                double push = band * aspect * 0.32;          // clear of the face, over the shoulder
                double lift = (h - Caption) * 0.12;          // a little above the centre line of the bar's content
                logoArt.Margin = new Thickness(push, 0, 0, lift);
                titleText.Margin = new Thickness(push, 0, 0, lift);
            };
            headerBar.SizeChanged += layout;
            layout(null, null);
        }

        // Solid down to the header's own bottom edge, give or take, then out to nothing by the foot of the band.
        static Brush Fade(double solidTo)
        {
            var g = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
            g.GradientStops.Add(new GradientStop(Colors.White, 0));
            g.GradientStops.Add(new GradientStop(Colors.White, solidTo * 0.88));
            g.GradientStops.Add(new GradientStop(Color.FromArgb(0, 255, 255, 255), 1));
            g.Freeze();
            return g;
        }

        // Windows groups taskbar buttons by AppUserModelID, and a process that never sets one inherits its host's.
        public static void SetWindowIcon(Window win, string gameDir)
        {
            string exe = Paths.Join(gameDir, "mgs4.exe");
            if (!Paths.Exists(exe)) return;
            try
            {
                using (System.Drawing.Icon ico = System.Drawing.Icon.ExtractAssociatedIcon(exe))
                {
                    if (ico == null) return;
                    ImageSource src = Imaging.CreateBitmapSourceFromHIcon(
                        ico.Handle, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
                    src.Freeze();
                    win.Icon = src;
                }
            }
            catch { }
        }
    }
}
