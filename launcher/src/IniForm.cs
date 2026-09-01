// mgs4_dlss.ini as a form: what each key is, what it does, and what it may be set to. The add-on owns this file
// while the game runs - its writes go through the Windows profile API, whose cache will quietly undo an outside
// edit - so everything here checks that first.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Mgs4Launcher
{
    // Which file a key lives in. Two of them, with different owners and the same hazard: the add-on rewrites
    // mgs4_dlss.ini through the Windows profile API while the game runs, and the game rewrites mgs4.savedsettings
    // when it exits - so neither is safe to edit under a running game, and both are safe when it is closed.
    // Addon: MGS4\mgs4_dlss.ini. Game: the game's own mgs4.savedsettings. Launcher: config.ini in this checkout,
    // for the handful of things that belong to the app rather than to either of them.
    enum IniSource { Addon, Game, Launcher, Renodx }

    class IniKey
    {
        public string Group, Key, Type, Label, Help;
        public string[] Choices, ChoiceLabels;
        public IniSource Source;
        public string TrueWord, FalseWord;      // the game writes true/false where the add-on writes 1/0
        public string Section;                  // the [Section] inside its file, when the file has any
        public IniKey(string group, string key, string type, string label, string help,
                      string[] choices = null, string[] choiceLabels = null,
                      IniSource source = IniSource.Addon, string trueWord = "1", string falseWord = "0")
        {
            Group = group; Key = key; Type = type; Label = label; Help = help;
            Choices = choices; ChoiceLabels = choiceLabels;
            Source = source; TrueWord = trueWord; FalseWord = falseWord;
            Section = source == IniSource.Renodx ? "RenoDX.DLSS5" : null;
        }
    }

    static class IniForm
    {
        public static readonly List<IniKey> Spec = new List<IniKey>
        {
            // The game's own options, out of mgs4_savedata_win\<steamid>\mgs4\mgs4.savedsettings - the same file
            // its in-game menu writes. Four of these are what the add-on needs set a particular way, and the Setup
            // tab has a button for exactly those four; the rest are here because this is where settings live.
            new IniKey("Display", "api", "choice", "Renderer",
                "this add-on is a D3D12 add-on and does nothing on the D3D11 backend",
                new[] { "dx12", "dx11" }, new[] { "dx12 - DirectX 12", "dx11 - DirectX 11" },
                IniSource.Game, "true", "false"),
            new IniKey("Display", "displayIndex", "int", "Display",
                "which monitor the game opens on, counting from 0", null, null, IniSource.Game, "true", "false"),
            new IniKey("Display", "vsync", "bool", "Vsync",
                "off pairs better with frame generation - the limiter below is what paces the game",
                null, null, IniSource.Game, "true", "false"),
            new IniKey("Display", "fpsLimiter", "int", "Frame limiter",
                "60. The port's physics are tied to it; frame generation is what puts more frames on screen",
                null, null, IniSource.Game, "true", "false"),

            // This app's own, not the game's: it has no resolution or window-mode setting of its own, it takes
            // --res_width / --res_height / --windowing on the command line. They sit here because this is where a
            // person looks for them, and in config.ini because that is where the launcher's machine-local values
            // live.
            new IniKey("Display", "MGS4_RES", "res", "Resolution",
                "what a scene boot asks the game for; empty lets the game choose. The Play tab's own box overrides it for that run",
                null, null, IniSource.Launcher, "true", "false"),
            new IniKey("Display", "MGS4_WINDOWING", "choice", "Mode",
                "how the window comes up. The port has been seen ignoring this on a --stage boot; the Master Collection launcher's own display settings are the reliable place for it",
                new[] { "full_exclusive", "full_borderless", "windowed" },
                new[] { "Fullscreen", "Borderless Window", "Window" },
                IniSource.Launcher, "true", "false"),

            new IniKey("Quality", "globalGraphicsQuality", "choice", "Overall quality",
                "the preset the three below follow unless they are set apart from it",
                new[] { "0", "1", "2", "3" }, new[] { "0", "1", "2", "3 - highest" }, IniSource.Game, "true", "false"),
            new IniKey("Quality", "textureQuality", "choice", "Textures",
                "3 is what the game writes at its highest",
                new[] { "0", "1", "2", "3" }, new[] { "0", "1", "2", "3 - highest" }, IniSource.Game, "true", "false"),
            new IniKey("Quality", "shadowQuality", "choice", "Shadows",
                "3 is what the game writes at its highest",
                new[] { "0", "1", "2", "3" }, new[] { "0", "1", "2", "3 - highest" }, IniSource.Game, "true", "false"),
            new IniKey("Quality", "vfxQuality", "choice", "Effects",
                "3 is what the game writes at its highest",
                new[] { "0", "1", "2", "3" }, new[] { "0", "1", "2", "3 - highest" }, IniSource.Game, "true", "false"),
            new IniKey("Quality", "enableFXAA", "bool", "FXAA",
                "off with DLAA - it only blurs the image DLSS is given",
                null, null, IniSource.Game, "true", "false"),

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


            // RenoDX's DLSS 5 add-on, out of [RenoDX.DLSS5] in MGS4\ReShade.ini. Not this project's settings and
            // not this project's defaults - they are read from the file and written back to it, and the values
            // this add-on was verified against are in the README's "setup this was verified on".
            new IniKey("Neural Rendering", "NeuralUplift", "bool", "Neural uplift",
                "the NR pass itself", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NRIntensity", "int", "Intensity",
                "how strongly NR is applied", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NRStyle", "int", "Style",
                "which NR look RenoDX asks for", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NRLocalTone", "bool", "Local tone",
                "local tone handling in the NR pass", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NRLocalStructure", "bool", "Local structure",
                "local structure handling in the NR pass", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NRSkinStructure", "int", "Skin structure",
                "-1 is what this add-on was verified with", null, null, IniSource.Renodx),
            new IniKey("Neural Rendering", "NREnableUpscaling", "bool", "NR upscaling",
                "off: this add-on already runs DLAA on the final image, so NR only denoises and uplifts it",
                null, null, IniSource.Renodx),

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

        // The file a key is written to. The game's is found by searching the save folder, so it is resolved once
        // and handed around rather than re-searched per row.
        public static string PathFor(IniSource source, string gameDir)
        {
            if (source == IniSource.Addon) return IniPath(gameDir);
            if (source == IniSource.Launcher) return Paths.ConfigPath;
            if (source == IniSource.Renodx) return Paths.Join(gameDir, "ReShade.ini");
            return Checks.SavedSettingsPath(gameDir);
        }

        public static string SourceLabel(IniSource source)
        {
            if (source == IniSource.Addon) return "mgs4_dlss.ini";
            if (source == IniSource.Launcher) return "config.ini";
            if (source == IniSource.Renodx) return "ReShade.ini [RenoDX.DLSS5]";
            return "mgs4.savedsettings";
        }

        // What a group is, said in a badge or two on its card: whose setting this is, and whether it is one of the
        // diagnostics that cost frames. "Diagnostics" is the add-on's, so it carries both.
        public static string[] BadgesFor(IniKey spec)
        {
            if (spec.Source == IniSource.Game || spec.Source == IniSource.Launcher) return new[] { "Game" };
            if (spec.Source == IniSource.Renodx) return new[] { "RenoDX" };
            if (spec.Group == "Diagnostics") return new[] { "Debug", "MGS4 DLSS" };
            return new[] { "MGS4 DLSS" };
        }

        // The value as the file holds it, or null when the file has no such key.
        public static string Read(IniKey spec, string addonIni, string gameIni)
        {
            string file = spec.Source == IniSource.Addon ? addonIni
                        : spec.Source == IniSource.Launcher ? Paths.ConfigPath
                        : spec.Source == IniSource.Renodx ? Paths.Join(GameDirOf(addonIni), "ReShade.ini")
                        : gameIni;
            return string.IsNullOrEmpty(file) ? null : Checks.IniValue(file, spec.Key);
        }

        // The resolution a scene boot uses when nothing was asked for on the command line: MGS4_RES from the
        // environment or config.ini, as WIDTHxHEIGHT. Zero when it is unset or unreadable, which means "let the
        // game choose", the way it behaved before this existed.
        public static void DefaultResolution(out int width, out int height)
        {
            width = 0; height = 0;
            string v = Paths.Setting("MGS4_RES", null);
            if (string.IsNullOrEmpty(v)) return;
            Match m = Regex.Match(v.Trim(), "^(\\d+)\\s*[xX]\\s*(\\d+)$");
            if (!m.Success) return;
            width = int.Parse(m.Groups[1].Value);
            height = int.Parse(m.Groups[2].Value);
        }

        // The add-on's ini sits in the game folder, so it is also how the other files there are found.
        static string GameDirOf(string addonIni)
        {
            return string.IsNullOrEmpty(addonIni) ? null : System.IO.Path.GetDirectoryName(addonIni);
        }

        public static IniKey Find(string key)
        {
            foreach (IniKey spec in Spec)
                if (string.Equals(spec.Key, key, StringComparison.OrdinalIgnoreCase)) return spec;
            return null;
        }

        public static List<string> Report(string gameDir)
        {
            var outp = new List<string>();
            string addonIni = IniPath(gameDir), gameIni = PathFor(IniSource.Game, gameDir);
            foreach (var group in Spec.GroupBy(k => k.Group))
            {
                var files = new List<string>();
                foreach (IniKey spec in group)
                {
                    string file = PathFor(spec.Source, gameDir);
                    string line = SourceLabel(spec.Source) + ": " + (string.IsNullOrEmpty(file) ? "not found" : file);
                    if (!files.Contains(line)) files.Add(line);
                }
                outp.Add("");
                outp.Add("[" + group.Key + "]");
                foreach (string f in files) outp.Add("  " + f);
                foreach (IniKey spec in group)
                    outp.Add(string.Format("  {0,-22} {1,-12} {2}", spec.Key,
                                           Read(spec, addonIni, gameIni) ?? "(unset)", spec.Label));
            }
            if (Checks.GameRunning())
            {
                outp.Add("");
                outp.Add("The game is running: it owns these files, so leave the writing until it exits.");
            }
            return outp;
        }

        // Key=Value from the command line, each one written to whichever file holds that key. A key the spec does
        // not know goes to the add-on's ini, which is where every key used to go.
        public static int SetFromCli(string gameDir, List<string> sets, Action<string> say)
        {
            if (Checks.GameRunning())
            {
                say("mgs4.exe is running - it rewrites these through the profile API and would undo this. Close it first.");
                return 1;
            }
            var byFile = new Dictionary<IniSource, List<KeyValuePair<string, string>>>();
            foreach (string set in sets)
            {
                Match m = Regex.Match(set, "^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.*)$");
                if (!m.Success) { say("not a Key=Value: " + set); return 1; }
                IniKey spec = Find(m.Groups[1].Value);
                IniSource source = spec != null ? spec.Source : IniSource.Addon;
                if (!byFile.ContainsKey(source)) byFile[source] = new List<KeyValuePair<string, string>>();
                byFile[source].Add(new KeyValuePair<string, string>(m.Groups[1].Value, m.Groups[2].Value.Trim()));
            }
            foreach (var kv in byFile)
            {
                string file = PathFor(kv.Key, gameDir);
                if (kv.Key == IniSource.Launcher && !Paths.Exists(file))
                    System.IO.File.WriteAllText(file,
                        "; Machine-local paths for this checkout (git-ignored). See config.example.ini for every key." +
                        Environment.NewLine);
                if (string.IsNullOrEmpty(file) || !Paths.Exists(file))
                {
                    say("no " + SourceLabel(kv.Key) + " to write to" +
                        (kv.Key == IniSource.Game ? " - run the game once and it writes one" : ""));
                    return 1;
                }
                IniKey first = Find(kv.Value[0].Key);
                try { Checks.SetIni(file, kv.Value, first != null ? first.Section : null); }
                catch (Exception e) { say(e.Message); return 1; }
                foreach (var set in kv.Value) say(set.Key + "=" + set.Value);
                say("written to " + file);
            }
            return 0;
        }
    }
}
