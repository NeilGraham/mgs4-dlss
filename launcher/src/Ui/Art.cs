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

        // The logo in place of the title, and the key art in the header band: on the left, filling the bar's
        // height, showing the middle 70% of its own. The bar is black and so is the art's right-hand half, so the
        // two meet with nothing to blend.
        public static void ApplyHeader(Window win, Image logoArt, TextBlock titleText, System.Windows.Shapes.Rectangle heroArt, Border headerBar)
        {
            Dictionary<string, string> art;
            try { art = Find(); } catch { return; }

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
            if (hero == null) return;
            const double shown = 0.70;
            var brush = new ImageBrush(hero)
            {
                Viewbox = new Rect(0, (1.0 - shown) / 2, 1, shown),
                Stretch = Stretch.Uniform,
                AlignmentX = AlignmentX.Left,
                AlignmentY = AlignmentY.Center,
            };
            brush.Freeze();
            heroArt.Fill = brush;
            heroArt.Visibility = Visibility.Visible;

            // How far right the logo has to start to clear Snake's face depends on how wide the art comes out,
            // which depends on the bar's height - so it is measured and redone on resize.
            double aspect = hero.PixelWidth / (hero.PixelHeight * shown);
            SizeChangedEventHandler layout = (s, e) =>
            {
                double h = headerBar.ActualHeight;
                if (h <= 0) return;
                double push = Math.Max(0.0, h * aspect * 0.32);   // clear of the face, over the shoulder
                double lift = h * 0.12;                           // a little above the bar's centre line
                logoArt.Margin = new Thickness(push, 0, 0, lift);
                titleText.Margin = new Thickness(push, 0, 0, lift);
            };
            headerBar.SizeChanged += layout;
            layout(null, null);
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
