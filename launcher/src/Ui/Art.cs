// The game's own artwork, from Steam's cache on this machine, and its icon out of mgs4.exe. None of those files is
// in this repo: they are Konami's, and they are already on the machine of anyone who owns the game. (The banner
// behind the header and the scene thumbnails, Thumbs.cs, are the pictures of the game that do ship, and they are
// screenshots taken while playing.) Every piece falls back to plain text when it is not there.
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

        // The banner behind the header: the title screen's Snake, cut at 4K from the sweep's recording of it by
        // tools\make_banner.py into tools\art\banner.jpg and built into the exe. It is a screenshot taken while
        // playing - with the scene thumbnails, the only pictures of the game that ship - and Steam's key art
        // (1920x620, the same face at a third of the size) is the fallback for a build without it. The logo
        // stays Steam's. The file wins over the built-in copy, the way tools\ data does everywhere here.
        static BitmapImage LoadBanner()
        {
            string path = Path.Combine(Paths.Root, "tools\\art\\banner.jpg");
            if (Paths.Exists(path))
            {
                BitmapImage fromFile = Load(path);
                if (fromFile != null) return fromFile;
            }
            try
            {
                using (Stream s = Paths.DataStream("banner.jpg"))
                {
                    if (s == null) return null;
                    var ms = new MemoryStream();
                    s.CopyTo(ms);
                    ms.Position = 0;
                    var bmp = new BitmapImage();
                    bmp.BeginInit();
                    bmp.CacheOption = BitmapCacheOption.OnLoad;
                    bmp.StreamSource = ms;
                    bmp.EndInit();
                    bmp.Freeze();
                    return bmp;
                }
            }
            catch { return null; }
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
        // Where Snake sits, as fractions of the band's height: how much taller than the band the art is drawn,
        // how far up it is lifted (the top of the bandana leaves, the mouth arrives), and the gap from the left.
        const double ArtScale = 1.12;
        const double ArtLift = 0.20;
        const double ArtInset = 0.14;

        public static void ApplyHeader(Window win, Image logoArt, TextBlock titleText, Panel navTabs,
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

            BitmapImage hero = LoadBanner() ?? Load(heroPath);
            double aspect = 0;
            if (hero != null && hero.PixelHeight > 0)
            {
                var brush = new ImageBrush(hero)
                {
                    Stretch = Stretch.Uniform,
                    AlignmentX = AlignmentX.Left,
                    AlignmentY = AlignmentY.Top,
                };
                brush.Freeze();
                heroArt.Fill = brush;
                heroArt.Visibility = Visibility.Visible;
                heroArt.HorizontalAlignment = HorizontalAlignment.Left;
                heroArt.VerticalAlignment = VerticalAlignment.Top;
                artBand.ClipToBounds = true;             // the art is drawn taller than the band, on purpose
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
                // The art is placed rather than fitted: a little taller than the band, lifted so the top of the
                // bandana goes out of the band and the mouth comes up into the part that is not fading, and set
                // in from the left edge. The banner is cut bandana to chin with black below (make_banner.py), so
                // what falls out at the foot is the shoulder, already faded in the file; the band's own fade and
                // its clip take the rest. Steam's key art, the fallback, is framed the same way and comes out the
                // same. The face is a shade wider than it is tall, so its clearance for the logo rides on the
                // band's height like everything else here, and the logo lands on the hair's fade.
                double inset = band * ArtInset;
                heroArt.Height = band * ArtScale;
                heroArt.Width = band * ArtScale * aspect;
                heroArt.Margin = new Thickness(inset, -band * ArtLift, 0, 0);
                double push = inset + band * 1.15;           // clear of the face, over the shoulder
                double lift = (h - Caption) * 0.12;          // a little above the center line of the bar's content
                logoArt.Margin = new Thickness(push, 0, 0, lift);
                titleText.Margin = new Thickness(push, 0, 0, lift);
                // The tabs are centered down the same row, so they need the same lift or they sit low against
                // the logo across from them. Only the bottom is ours; the rest is the XAML's and stays as set.
                navTabs.Margin = new Thickness(navTabs.Margin.Left, 0, navTabs.Margin.Right, lift);
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

        // The window used to be given mgs4.exe's icon here, which is what the taskbar button shows - so the
        // launcher sat next to the running game wearing the game's own face, and the two buttons could not be
        // told apart. Worse, it overrode the icon the build had embedded, including the one a release draws for
        // itself: that artwork was never once seen in a taskbar.
        //
        // Nothing replaces it. A WPF window with no Icon of its own falls back to the win32 icon of the exe it
        // came from, at whatever size Windows is asking for, which is better than anything set here could be -
        // one icon, chosen at build time, in every size the .ico carries. Both builds badge that icon
        // (tools\launcher_badge.ps1), so it is a tile with a play mark rather than the game's own art.
        //
        // What does need saying is who this process is. Windows groups taskbar buttons by AppUserModelID and a
        // process that never sets one inherits its host's - so a launcher started from a terminal could share a
        // button with it, under the host's icon, whatever this exe wears. That is the thing the old comment here
        // described and the old code did not do.
        [System.Runtime.InteropServices.DllImport("shell32.dll")]
        static extern int SetCurrentProcessExplicitAppUserModelID(
            [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string id);

        public static void SetTaskbarIdentity()
        {
            try { SetCurrentProcessExplicitAppUserModelID("NeilGraham.Mgs4DlssLauncher"); }
            catch { }      // pre-Win7 or a locked-down shell: the button groups as it always did
        }
    }
}
