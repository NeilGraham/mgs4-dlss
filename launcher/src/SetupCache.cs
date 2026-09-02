// The last install check, kept on disk so the Setup tab has something to draw the first time it is opened in a
// run rather than a blank card and a wait.
//
// Every section the check produced is stored, not just the ones the manifest names: Run builds two of its own -
// Settings and Last run - that no file list knows about, and a cache without them would draw a Setup tab missing
// its last two cards.
//
// What is deliberately not restored from here is the three lists that say which files a section accepts, where
// they go, and what it expects: those drive what happens when somebody drops a file on the window, and acting on
// a stale copy of them would put a file in the wrong place. They are taken from install_manifest.json, which is
// read in well under a millisecond and is the current truth. A cache that does not answer for every section the
// manifest names is refused outright, so a launcher shipping a longer file list never draws an old one.
//
// It is a separate file from launcher.json on purpose. That one is written whole on every save, so a check that
// finished between two saves would be dropped; this is written when a check finishes and read when one is wanted.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    static class SetupCache
    {
        public static string Path
        {
            get
            {
                return System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "mgs4-dlss-launcher\\setup_check.json");
            }
        }

        public static void Write(string game, List<Section> sections)
        {
            if (string.IsNullOrEmpty(game) || sections == null) return;
            try
            {
                var secs = new List<object>();
                foreach (Section sec in sections)
                {
                    var rows = new List<object>();
                    foreach (Row r in sec.Rows)
                        rows.Add(new List<object> { r.Status, r.Name, r.Detail, r.Value, r.Url });
                    secs.Add(new Dictionary<string, object>
                    {
                        { "id", sec.Id },
                        { "title", sec.Title },
                        { "blurb", sec.Blurb },
                        { "url", sec.Url },
                        { "urlLabel", sec.UrlLabel },
                        { "guide", sec.Guide },
                        { "required", sec.Required },
                        { "bundled", sec.Bundled },
                        { "saved", sec.SavedSettings },
                        { "wrong", new List<object>(sec.WrongKeys.ToArray()) },
                        { "rows", rows },
                    });
                }
                var doc = new Dictionary<string, object>
                {
                    { "dir", game },
                    { "at", DateTime.Now.ToString("o", CultureInfo.InvariantCulture) },
                    { "sections", secs },
                };
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
                File.WriteAllText(Path, new JavaScriptSerializer().Serialize(doc));
            }
            catch { }
        }

        // Null unless there is a check on disk, it was taken against this same folder, and it answers for every
        // section the manifest names. Anything less and the caller is better off waiting for the real one.
        public static List<Section> Read(string game, out DateTime taken)
        {
            taken = DateTime.MinValue;
            if (string.IsNullOrEmpty(game)) return null;
            try
            {
                if (!Paths.Exists(Path)) return null;
                var doc = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(Path)) as Dictionary<string, object>;
                if (doc == null) return null;

                object dir;
                if (!doc.TryGetValue("dir", out dir) || dir == null) return null;
                if (!string.Equals(dir.ToString(), game, StringComparison.OrdinalIgnoreCase)) return null;

                object raw;
                if (!doc.TryGetValue("sections", out raw)) return null;
                var secs = raw as object[];
                if (secs == null) return null;

                // The file list as it is now, to hand each section back the three lists a dropped file is placed by.
                var specs = new Dictionary<string, Section>(StringComparer.OrdinalIgnoreCase);
                foreach (Section spec in Checks.Manifest()) specs[spec.Id] = spec;

                var sections = new List<Section>();
                var seen = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
                foreach (object one in secs)
                {
                    var body = one as Dictionary<string, object>;
                    if (body == null) return null;
                    string id = Get(body, "id");
                    if (string.IsNullOrEmpty(id)) return null;
                    seen[id] = true;

                    var sec = new Section
                    {
                        Id = id,
                        Title = Get(body, "title"),
                        Blurb = Get(body, "blurb"),
                        Url = Get(body, "url"),
                        UrlLabel = Get(body, "urlLabel"),
                        Guide = Get(body, "guide"),
                        Required = Flag(body, "required"),
                        Bundled = Flag(body, "bundled"),
                        SavedSettings = Get(body, "saved"),
                    };

                    // Never from the cache: where a dropped file is put is decided by the manifest, now.
                    Section spec2;
                    if (specs.TryGetValue(id, out spec2))
                    {
                        sec.Accepts = spec2.Accepts;
                        sec.Drop = spec2.Drop;
                        sec.Files = spec2.Files;
                    }

                    object wrong;
                    if (body.TryGetValue("wrong", out wrong) && wrong is object[])
                        foreach (object k in (object[])wrong)
                            if (k != null) sec.WrongKeys.Add(k.ToString());

                    object rows;
                    if (body.TryGetValue("rows", out rows) && rows is object[])
                        foreach (object r in (object[])rows)
                        {
                            var cells = r as object[];
                            if (cells == null || cells.Length < 5) continue;
                            sec.Rows.Add(new Row(Str(cells[0]), Str(cells[1]), Str(cells[2]), Str(cells[3]), Str(cells[4])));
                        }

                    sections.Add(sec);
                }

                // A section the file list names and the cache has never heard of means the list grew since: the
                // card for it would simply be absent, so the whole thing is refused and the real check is waited for.
                foreach (string id in specs.Keys)
                    if (!seen.ContainsKey(id)) return null;

                object at;
                if (doc.TryGetValue("at", out at) && at != null)
                    DateTime.TryParse(at.ToString(), CultureInfo.InvariantCulture, DateTimeStyles.None, out taken);
                return sections;
            }
            catch { return null; }
        }

        static string Str(object o) { return o == null ? null : o.ToString(); }

        static string Get(Dictionary<string, object> d, string key)
        {
            object v;
            return d.TryGetValue(key, out v) && v != null ? v.ToString() : null;
        }

        static bool Flag(Dictionary<string, object> d, string key)
        {
            object v;
            if (!d.TryGetValue(key, out v) || v == null) return false;
            try { return Convert.ToBoolean(v); } catch { return false; }
        }
    }
}
