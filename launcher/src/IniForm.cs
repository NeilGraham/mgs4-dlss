// mgs4_dlss.ini as a form: what each key is, what it does, and what it may be set to. The add-on owns this file
// while the game runs - its writes go through the Windows profile API, whose cache will quietly undo an outside
// edit - so everything here checks that first.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace Mgs4Launcher
{
    class IniKey
    {
        public string Group, Key, Type, Label, Help;
        public string[] Choices, ChoiceLabels;
        public IniKey(string group, string key, string type, string label, string help,
                      string[] choices = null, string[] choiceLabels = null)
        {
            Group = group; Key = key; Type = type; Label = label; Help = help;
            Choices = choices; ChoiceLabels = choiceLabels;
        }
    }

    static class IniForm
    {
        public static readonly List<IniKey> Spec = new List<IniKey>
        {
            new IniKey("DLSS", "Enabled", "bool", "DLSS on",
                "read again every second; 0 leaves the add-on loaded but idle"),
            new IniKey("DLSS", "Mode", "choice", "Mode",
                "DLAA renders at your resolution; the others render smaller and upscale. Restart the game to change.",
                new[] { "DLAA", "Quality", "Balanced", "Performance", "UltraPerformance" }),
            new IniKey("DLSS", "InternalRes", "readonly", "Internal resolution",
                "what the game renders at - detected and written by the add-on itself"),
            new IniKey("DLSS", "Preset", "choice", "Preset", "the DLSS model preset",
                new[] { "10", "11" }, new[] { "10 - preset J", "11 - preset K (transformer)" }),
            new IniKey("DLSS", "Sharpness", "int", "Sharpness", "0..100, applied live"),
            new IniKey("DLSS", "PrePost", "choice", "Insertion point",
                "auto = the final image when the DLSS 5 NR add-on is loaded, otherwise before post and the HUD",
                new[] { "auto", "0", "1" }, new[] { "auto", "before the post chain", "on the final image" }),

            new IniKey("Frame generation", "FrameGen", "choice", "Frame generation",
                "needs the Streamline runtime next to mgs4.exe and a display faster than 60 Hz. Restart to change.",
                new[] { "0", "1", "2", "3", "4" }, new[] { "off", "2x", "3x", "4x", "dynamic to the target fps" }),
            new IniKey("Frame generation", "FGTargetFps", "int", "Target fps",
                "dynamic mode aims here; match your refresh rate (0 = ask the monitor)"),
            new IniKey("Frame generation", "Reflex", "bool", "Reflex", "latency pacing; DLSS-G wants it on"),

            new IniKey("Image", "PostDof", "bool", "Depth of field after DLSS",
                "skip the game's DoF draws and re-apply the same DoF on the DLSS / NR output"),
            new IniKey("Image", "ObjectMV", "bool", "Per-object motion vectors",
                "stream-out of the game's vertex shaders; about 0.1 ms of GPU at 4K"),
            new IniKey("Image", "Jitter", "bool", "Camera jitter",
                "Halton jitter patched into the scene draw constants - what makes this real DLSS"),
            new IniKey("Image", "MotionVectors", "bool", "Camera motion vectors",
                "reconstructed from depth in a compute pass"),
            new IniKey("Image", "DRS", "bool", "Dynamic resolution handling",
                "the port shrinks its own scene under load; this brings it back to the full grid"),
            new IniKey("Image", "UIMask", "bool", "HUD mask", "no HUD ghosting under camera motion"),
            new IniKey("Image", "FrozenBackground", "bool", "Pause / Codec background",
                "keep the DLSS image behind the pause menu and the Codec"),
            new IniKey("Image", "WindowScene", "bool", "3D windows",
                "DLSS inside the Codec caller's own render target"),
            new IniKey("Image", "PreWarm", "bool", "Pre-warm",
                "build the DLSS / NR features on loading screens, so the stall is not in the first cutscene frames"),

            new IniKey("Diagnostics", "DebugMode", "choice", "Debug view", "costs frames; 0 for normal play",
                new[] { "0", "1", "2", "3", "4", "5", "9" },
                new[] { "off", "magenta path test", "bypass DLSS (A/B)", "trace 3 frames", "draw constants",
                        "motion-vector field", "vectors over the image" }),
            new IniKey("Diagnostics", "SceneLog", "bool", "Scene-state log",
                "the SCENE-STATE lines this launcher reads to tell a cutscene from gameplay - leave it on"),
            new IniKey("Diagnostics", "TraceFrames", "int", "Trace frames",
                "N = log every full-frame draw for the next N frames"),
            new IniKey("Diagnostics", "TraceFreeze", "bool", "Trace freezes",
                "log the draw chain around the moment the world stops rendering"),
            new IniKey("Diagnostics", "Probe", "bool", "Pipeline probe", "sample the image before and after the insertion"),
            new IniKey("Diagnostics", "DumpShaders", "bool", "Dump shaders",
                "write every pipeline's bytecode to logs\\shaders"),
        };

        public static string IniPath(string gameDir) { return Paths.Join(gameDir, "mgs4_dlss.ini"); }

        public static List<string> Report(string gameDir)
        {
            var outp = new List<string>();
            string ini = IniPath(gameDir);
            outp.Add("mgs4_dlss.ini: " + ini);
            if (!Paths.Exists(ini))
            {
                outp.Add("  (not there - copy dlss-addon\\mgs4_dlss.ini next to mgs4.exe)");
                return outp;
            }
            string group = "";
            foreach (IniKey s in Spec)
            {
                if (s.Group != group)
                {
                    group = s.Group;
                    outp.Add("");
                    outp.Add("[" + group + "]");
                }
                string v = Checks.IniValue(ini, s.Key) ?? "(unset)";
                outp.Add(string.Format("  {0,-22} {1,-12} {2}", s.Key, v, s.Label));
            }
            if (Checks.GameRunning())
            {
                outp.Add("");
                outp.Add("The game is running: it owns this file, so leave the writing to the add-on until it exits.");
            }
            return outp;
        }

        public static int SetFromCli(string gameDir, List<string> sets, Action<string> say)
        {
            string ini = IniPath(gameDir);
            if (Checks.GameRunning())
            {
                say("mgs4.exe is running - it rewrites mgs4_dlss.ini through the profile API and would undo this. Close it first.");
                return 1;
            }
            var vals = new List<KeyValuePair<string, string>>();
            foreach (string s in sets)
            {
                Match m = Regex.Match(s, "^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.*)$");
                if (!m.Success) { say("not a Key=Value: " + s); return 1; }
                vals.Add(new KeyValuePair<string, string>(m.Groups[1].Value, m.Groups[2].Value.Trim()));
            }
            try { Checks.SetIni(ini, vals); }
            catch (Exception e) { say(e.Message); return 1; }
            foreach (var kv in vals) say(kv.Key + "=" + kv.Value);
            say("written to " + ini);
            return 0;
        }
    }
}
