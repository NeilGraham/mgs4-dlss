// One frame per scene, shown as the banner on its row and beside the description when it is picked.
//
// The set rides inside the exe as a single zip resource (tools\thumbs\*.jpg from tools\make_thumbs.py, zipped by build.ps1)
// rather than as a few hundred separate resources: csc names a resource after its file, so a directory of them
// would be a directory of /resource: switches, and the manifest lookup below would have to guess at names.
//
// Decoded lazily and cached: a full catalog is ~180 images and the list only ever shows a screenful, so building
// them all up front would cost a second of startup for pictures nobody scrolled to. DecodePixelWidth keeps each
// one at the size it is actually drawn - a BitmapImage otherwise holds the full decode regardless.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Mgs4Launcher
{
    public static class Thumbs
    {
        const string Resource = "scene_thumbs.zip";
        static readonly Dictionary<string, byte[]> _raw = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        static readonly Dictionary<string, ImageSource> _cache = new Dictionary<string, ImageSource>();
        static bool _loaded;

        static void Load()
        {
            if (_loaded) return;
            _loaded = true;
            try
            {
                Assembly asm = Assembly.GetExecutingAssembly();
                using (Stream s = asm.GetManifestResourceStream(Resource))
                {
                    if (s == null) return;          // a build with no sweep behind it: rows simply have no banner
                    // ZipArchive wants a seekable stream and a manifest resource stream is not always one.
                    using (var ms = new MemoryStream())
                    {
                        s.CopyTo(ms);
                        ms.Position = 0;
                        using (var zip = new ZipArchive(ms, ZipArchiveMode.Read))
                            foreach (ZipArchiveEntry e in zip.Entries)
                            {
                                using (Stream es = e.Open())
                                using (var b = new MemoryStream())
                                {
                                    es.CopyTo(b);
                                    string id = Path.GetFileNameWithoutExtension(e.Name);
                                    _raw[id] = b.ToArray();
                                }
                            }
                    }
                }
            }
            catch { }                                // a broken zip must not stop the window opening
        }

        public static bool Has(string id)
        {
            Load();
            return id != null && _raw.ContainsKey(id);
        }

        /// <summary>The scene's frame at the given draw width, or null if the sweep never captured one.</summary>
        public static ImageSource Get(string id, int decodeWidth)
        {
            Load();
            if (string.IsNullOrEmpty(id)) return null;
            string key = id + "@" + decodeWidth;
            ImageSource hit;
            if (_cache.TryGetValue(key, out hit)) return hit;
            byte[] bytes;
            if (!_raw.TryGetValue(id, out bytes)) return null;
            try
            {
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.StreamSource = new MemoryStream(bytes);
                bmp.DecodePixelWidth = decodeWidth;
                bmp.CacheOption = BitmapCacheOption.OnLoad;
                bmp.EndInit();
                bmp.Freeze();                        // shared across the list's item containers, so it must be frozen
                _cache[key] = bmp;
                return bmp;
            }
            catch { return null; }
        }
    }
}
