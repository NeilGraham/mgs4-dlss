// Everything that can be launched: tools/scenes.csv (the stage table) with names from tools/labels.json and the
// corrections in tools/scene_info.json. The stage table says a scene exists; only booting it says what it is, so
// scene_info.json is where the truth about each one lives - this only assembles it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    public class Scene
    {
        public string Id, Kind, ActKey, Name, Description, SortAs, ActTitle, Note;
        public int Rank;
        public bool Hidden;
        public List<string> Alts = new List<string>();
        public string SortPrefix;
        public int SortCat, SortNum;
    }

    static class Catalogue
    {
        static string ScenesCsv { get { return Path.Combine(Paths.Root, "tools\\scenes.csv"); } }
        static string LabelsJson { get { return Path.Combine(Paths.Root, "tools\\labels.json"); } }
        static string SceneInfoJson { get { return Path.Combine(Paths.Root, "tools\\scene_info.json"); } }

        static List<Scene> _all;
        public static List<string> ActOrder = new List<string>();
        public static Dictionary<string, string> ActTitles = new Dictionary<string, string>();

        // "act2-naomi-in-the-lab" -> "Naomi in the Lab"; the act is already a column of its own.
        public static string FormatSceneName(string slug)
        {
            if (string.IsNullOrEmpty(slug)) return "";
            var parts = slug.Split('-').Where(p => p.Length > 0).ToList();
            if (parts.Count > 1 && Regex.IsMatch(parts[0], "^(act\\d|prologue|epilogue|interlude|briefing)$"))
                parts.RemoveAt(0);
            var small = new HashSet<string> { "the", "a", "an", "and", "of", "in", "at", "on", "with", "to", "by", "versus", "vs" };
            var fixedWords = new Dictionary<string, string>
            {
                { "pmc", "PMC" }, { "pmcs", "PMCs" }, { "mk2", "Mk. II" }, { "otc", "OTC" }, { "rex", "REX" },
                { "ray", "RAY" }, { "hud", "HUD" }, { "ai", "AI" }, { "vs", "vs" }
            };
            var outp = new List<string>();
            for (int i = 0; i < parts.Count; i++)
            {
                string w = parts[i];
                if (fixedWords.ContainsKey(w)) outp.Add(fixedWords[w]);
                else if (i > 0 && small.Contains(w)) outp.Add(w);
                else outp.Add(char.ToUpper(w[0]) + w.Substring(1));
            }
            return string.Join(" ", outp);
        }

        // Act 1..5 first, then the epilogue, is story order for someone picking a scene; the stage ids do not say
        // that (s00 is the Big Boss material at the very end, s10/s20/s30 are the briefings between acts).
        static string ActKeyFor(string id)
        {
            if (Regex.IsMatch(id, "^s0[1-5]")) return "act" + id.Substring(2, 1);
            if (id.StartsWith("s00")) return "epilogue";
            return "other";
        }

        public static List<Scene> All()
        {
            if (_all != null) return _all;
            var ser = new JavaScriptSerializer { MaxJsonLength = 32 * 1024 * 1024 };

            var labels = new Dictionary<string, string>();
            if (Paths.Exists(LabelsJson))
            {
                var raw = ser.DeserializeObject(File.ReadAllText(LabelsJson)) as Dictionary<string, object>;
                if (raw != null)
                    foreach (var kv in raw)
                        if (kv.Value != null) labels[kv.Key] = kv.Value.ToString();
            }

            var info = new Dictionary<string, Dictionary<string, object>>();
            if (Paths.Exists(SceneInfoJson))
            {
                var raw = ser.DeserializeObject(File.ReadAllText(SceneInfoJson)) as Dictionary<string, object>;
                if (raw != null)
                {
                    object acts;
                    if (raw.TryGetValue("acts", out acts) && acts is object[])
                        foreach (object a in (object[])acts)
                        {
                            var d = (Dictionary<string, object>)a;
                            string key = d["key"].ToString();
                            ActOrder.Add(key);
                            ActTitles[key] = d["title"].ToString();
                        }
                    object scenes;
                    if (raw.TryGetValue("scenes", out scenes) && scenes is Dictionary<string, object>)
                        foreach (var kv in (Dictionary<string, object>)scenes)
                            if (kv.Value is Dictionary<string, object>)
                                info[kv.Key] = (Dictionary<string, object>)kv.Value;
                }
            }

            var list = new List<Scene>();
            // No "@title" here on purpose: mgs4.exe --stage s00title_1 access-violates within seconds every time,
            // with the add-on idle and frame generation off as well. Verified 2026-08-31; do not add it untested.
            list.Add(new Scene
            {
                Id = "@main", Kind = "start", ActKey = "start", Rank = 10,
                Name = "Main menu, skipping the intro",
                Description = "Drops straight onto the menu selection, past the pre-menu credits and PRESS START. Quick for testing, but it crashes on most launches with frame generation on - use 'Start the game' to play.",
                Hidden = true, SortAs = ""
            });
            list.Add(new Scene
            {
                Id = "@collection", Kind = "start", ActKey = "start", Rank = 20,
                Name = "Master Collection launcher",
                Description = "The Unity front-end, where the display settings live.",
                Hidden = false, SortAs = ""
            });

            // Ids that crash or come up black: every one whose id ends in a single digit. The two-digit sections
            // ("_00", "_11") work, and "_D2" is a cutscene, not a section.
            const string brokenRe = "_\\d$";
            var aliasOf = new Dictionary<string, string>();

            if (Paths.Exists(ScenesCsv))
            {
                string[] lines = File.ReadAllLines(ScenesCsv);
                var head = lines[0].Split(',').ToList();
                int iEntry = head.IndexOf("stage_entry"), iKind = head.IndexOf("kind");
                for (int i = 1; i < lines.Length; i++)
                {
                    if (lines[i].Trim().Length == 0) continue;
                    string[] cells = lines[i].Split(',');
                    if (cells.Length <= iEntry) continue;
                    string id = cells[iEntry];
                    Dictionary<string, object> o;
                    info.TryGetValue(id, out o);
                    if (o != null && o.ContainsKey("sameAs")) { aliasOf[id] = o["sameAs"].ToString(); continue; }

                    var s = new Scene
                    {
                        Id = id,
                        Kind = iKind >= 0 && cells.Length > iKind ? cells[iKind] : "",
                        Name = FormatSceneName(labels.ContainsKey(id) ? labels[id] : null),
                        ActKey = ActKeyFor(id),
                        Rank = 0,
                        Description = "",
                        Hidden = Regex.IsMatch(id, brokenRe),
                        SortAs = ""
                    };
                    if (o != null)
                    {
                        if (o.ContainsKey("kind")) s.Kind = o["kind"].ToString();
                        if (o.ContainsKey("name")) s.Name = o["name"].ToString();
                        if (o.ContainsKey("act")) s.ActKey = o["act"].ToString();
                        if (o.ContainsKey("rank")) s.Rank = Convert.ToInt32(o["rank"]);
                        if (o.ContainsKey("description")) s.Description = o["description"].ToString();
                        if (o.ContainsKey("hidden")) s.Hidden = Convert.ToBoolean(o["hidden"]);
                        if (o.ContainsKey("sortAs")) s.SortAs = o["sortAs"].ToString();
                    }
                    list.Add(s);
                }
            }

            // Fold the "same scene, other id" entries into the one they duplicate, so the list has one row and the
            // panel offers the choice. If the primary is missing, the alias stands on its own.
            foreach (var kv in aliasOf)
            {
                Scene primary = list.FirstOrDefault(e => e.Id == kv.Value);
                if (primary != null) primary.Alts.Add(kv.Key);
                else list.Add(new Scene
                {
                    Id = kv.Key, Kind = "cutscene", ActKey = ActKeyFor(kv.Key), Rank = 0,
                    Name = "", Description = "", Hidden = false, SortAs = ""
                });
            }

            foreach (Scene e in list)
            {
                e.ActTitle = ActTitles.ContainsKey(e.ActKey) ? ActTitles[e.ActKey] : "Uncategorised";
                switch (e.Kind)
                {
                    case "cutscene": e.Note = "in-engine cutscene"; break;
                    case "briefing": e.Note = "mission briefing"; break;
                    case "gameplay": e.Note = "playable section"; break;
                    case "start": e.Note = "starts the game"; break;
                    default: e.Note = "boots the stage at its start"; break;
                }
                // Story order inside an act: the stage, then its cutscenes, then its numbered sections. Sorting on
                // the id alone puts "_00" before "_D1" because a digit sorts before a letter, which is backwards.
                e.SortPrefix = e.Id; e.SortCat = 0; e.SortNum = 0;
                Match m = Regex.Match(e.Id, "^([^_]+)_(.*)$");
                if (m.Success)
                {
                    e.SortPrefix = m.Groups[1].Value;
                    string suffix = m.Groups[2].Value;
                    Match d = Regex.Match(suffix, "^D(\\d*)$");
                    if (d.Success) { e.SortCat = 1; e.SortNum = d.Groups[1].Value.Length > 0 ? int.Parse(d.Groups[1].Value) : -1; }
                    else e.SortCat = 2;
                }
                if (!string.IsNullOrEmpty(e.SortAs)) { e.SortPrefix = e.SortAs; e.SortCat = 0; e.SortNum = 0; }
            }

            var order = new Dictionary<string, int>();
            for (int i = 0; i < ActOrder.Count; i++) order[ActOrder[i]] = i;
            _all = list
                .OrderBy(e => order.ContainsKey(e.ActKey) ? order[e.ActKey] : int.MaxValue)
                .ThenBy(e => e.Rank)
                .ThenBy(e => e.SortPrefix, StringComparer.OrdinalIgnoreCase)
                .ThenBy(e => e.SortCat)
                .ThenBy(e => e.SortNum)
                .ThenBy(e => e.Id, StringComparer.OrdinalIgnoreCase)
                .ToList();
            return _all;
        }

        // By id, including the alternate ids folded into an entry.
        public static Scene Find(string id)
        {
            if (string.IsNullOrEmpty(id)) return null;
            foreach (Scene s in All())
            {
                if (string.Equals(s.Id, id, StringComparison.OrdinalIgnoreCase)) return s;
                foreach (string alt in s.Alts)
                    if (string.Equals(alt, id, StringComparison.OrdinalIgnoreCase)) return s;
            }
            return null;
        }

        // Entries that start the game rather than a scene: the run options do not apply to them.
        public static bool IsStartEntry(string id)
        {
            if (string.IsNullOrEmpty(id) || id.StartsWith("@")) return true;
            Scene s = Find(id);
            return s != null && s.Kind == "start";
        }

        public static List<string> Listing(string filter)
        {
            var rows = All().Where(r => !r.Hidden).ToList();
            if (!string.IsNullOrEmpty(filter))
                rows = All().Where(r =>
                    (r.Id + " " + string.Join(" ", r.Alts) + " " + r.Name + " " + r.ActTitle + " " + r.Kind + " " + r.Description)
                        .IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0).ToList();

            var outp = new List<string>();
            string act = null;
            foreach (Scene r in rows)
            {
                if (r.ActTitle != act)
                {
                    act = r.ActTitle;
                    outp.Add("");
                    outp.Add("[" + act + "]");
                }
                string ids = r.Id;
                if (r.Alts.Count > 0) ids += " (= " + string.Join(", ", r.Alts) + ")";
                string what = string.IsNullOrEmpty(r.Description) ? r.Note : r.Description;
                string name = string.IsNullOrEmpty(r.Name) ? what : r.Name + " - " + what;
                outp.Add(string.Format("  {0,-30} {1,-10} {2}", ids, r.Kind, name));
            }
            outp.Add("");
            outp.Add(rows.Count + (rows.Count == 1 ? " entry." : " entries.") + "  mgs4-dlss-launcher <id>  boots one.");
            return outp;
        }
    }
}
