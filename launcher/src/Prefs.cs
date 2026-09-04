// What the window remembers between runs: the scene last picked, the ticked options, and whether the Setup tab has
// been seen once. Small enough to be one JSON file; nothing here is needed for the app to work.
using System;
using System.Collections.Generic;
using System.IO;
using System.Web.Script.Serialization;

namespace Mgs4Launcher
{
    static class Prefs
    {
        public static string Path
        {
            get
            {
                return System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "mgs4-dlss-launcher\\launcher.json");
            }
        }

        // Before the app was renamed. Read when the new path is not there yet, so the game folder and the ticked
        // options survive the rename; the next save writes the new one.
        public static string OldPath
        {
            get
            {
                return System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "mgs4-dlss\\launcher.json");
            }
        }

        public static Dictionary<string, object> Read()
        {
            foreach (string p in new[] { Path, OldPath })
            {
                if (!Paths.Exists(p)) continue;
                try
                {
                    var ser = new JavaScriptSerializer();
                    var d = ser.DeserializeObject(File.ReadAllText(p)) as Dictionary<string, object>;
                    if (d != null) return d;
                }
                catch { }
            }
            return null;
        }

        // No preferences file yet, or one from before this marker existed, means nobody has opened the window
        // here: the install check is the first thing worth seeing. Every run after that opens on Play.
        public static bool FirstRun()
        {
            Dictionary<string, object> p = Read();
            object seen;
            return !(p != null && p.TryGetValue("Seen", out seen) && Convert.ToBoolean(seen));
        }

        // A name and a description someone has typed over the catalog's own. Either half can be unset, which is
        // why this is not just two strings in a dictionary: an edited description with the file's name left alone
        // has to survive the catalog being re-measured and re-named.
        public class SceneEdit { public string Name, Description; }

        public static Dictionary<string, SceneEdit> SceneEdits()
        {
            var outp = new Dictionary<string, SceneEdit>(StringComparer.OrdinalIgnoreCase);
            Dictionary<string, object> p = Read();
            object raw;
            if (p == null || !p.TryGetValue("SceneEdits", out raw)) return outp;
            var d = raw as Dictionary<string, object>;
            if (d == null) return outp;
            foreach (var kv in d)
            {
                var e = kv.Value as Dictionary<string, object>;
                if (e == null) continue;
                var edit = new SceneEdit();
                object v;
                if (e.TryGetValue("name", out v) && v != null) edit.Name = v.ToString();
                if (e.TryGetValue("description", out v) && v != null) edit.Description = v.ToString();
                if (edit.Name != null || edit.Description != null) outp[kv.Key] = edit;
            }
            return outp;
        }

        public static void Save(Dictionary<string, object> values)
        {
            try
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
                File.WriteAllText(Path, new JavaScriptSerializer().Serialize(values));
            }
            catch { }
        }
    }

    // One .lnk for one scene, wherever the caller wants it, carrying the run options it was made with. The
    // shortcut holds this program's absolute path, so moving the checkout breaks it - make a new one rather than
    // editing it.
    static class Shortcut
    {
        // A sensible default file name: "<id> - <act> - <name>", minus what Windows will not take.
        public static string NameFor(Scene scene)
        {
            string name = scene.Id;
            if (!string.IsNullOrEmpty(scene.ActTitle) && scene.Kind != "start") name += " - " + scene.ActTitle;
            if (!string.IsNullOrEmpty(scene.Name)) name += " - " + scene.Name;
            foreach (char c in System.IO.Path.GetInvalidFileNameChars()) name = name.Replace(c, '-');
            return name;
        }

        public static string Write(Options opt, string path)
        {
            if (string.IsNullOrEmpty(opt.Stage)) throw new ArgumentException("no scene to make a shortcut for");
            if (string.IsNullOrEmpty(path)) throw new ArgumentException("no file name for the shortcut");
            if (!path.ToLowerInvariant().EndsWith(".lnk")) path += ".lnk";
            string dir = System.IO.Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir) && !Paths.Exists(dir)) Directory.CreateDirectory(dir);

            var cli = new List<string>();
            foreach (string a in opt.ToCli()) cli.Add(a.Contains(" ") ? "\"" + a + "\"" : a);
            Scene scene = Catalog.Find(opt.Stage);

            // Late-bound WScript.Shell: no interop assembly to reference, which keeps the build to csc.exe alone.
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object lnk = shellType.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod,
                                                null, shell, new object[] { path });
            Type t = lnk.GetType();
            Action<string, object> set = (name, value) =>
                t.InvokeMember(name, System.Reflection.BindingFlags.SetProperty, null, lnk, new[] { value });

            set("TargetPath", System.Reflection.Assembly.GetExecutingAssembly().Location);
            set("Arguments", string.Join(" ", cli));
            if (!string.IsNullOrEmpty(opt.GameDir))
            {
                set("WorkingDirectory", opt.GameDir);
                set("IconLocation", Paths.Join(opt.GameDir, "mgs4.exe") + ",0");
            }
            string desc = opt.Stage;
            if (scene != null && !string.IsNullOrEmpty(scene.Description)) desc += " - " + scene.Description;
            else if (scene != null && !string.IsNullOrEmpty(scene.Note)) desc += " - " + scene.Note;
            set("Description", desc);
            t.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod, null, lnk, new object[0]);
            return path;
        }
    }
}
