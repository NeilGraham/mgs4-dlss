// The command line, parsed exactly as tools/mgs4_dlss_launcher.ps1 parses it: the same flags, the same spellings,
// the same defaults. Anything the window can do is a command line, and anything set up in the window is printed
// back as one, so the two have to agree.
using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace Mgs4Launcher
{
    class Options
    {
        public string Action = "";          // "" = launch, or ui / install / install-addon / report / list /
                                            //      settings / set / shortcut / stop / help
        public string Stage = "";           // a stage id, or one of the @-entries in the catalogue
        public string GameDir = "";
        public bool GameDirGiven;           // true only when --game-dir was passed, so previews do not echo a detected path
        public string GameDirBad = "";      // a folder that was named but holds no mgs4.exe, kept for the Setup tab
        public bool Advance = true;         // press through the boot prompts until the first 3D frame
        public bool MashX;                  // keep tapping Cross so the flashback prompts fire
        public string Keys = "";            // an explicit key sequence instead of pressing through the prompts
        public double Settle = 2.0;
        public string PressKey = "";
        public bool SceneDetect = true;
        public bool EndOnGameplay;
        public double Hold, MaxMinutes;
        public double StartTimeout = 100.0, PressEvery = 0.16, PressHold = 0.07;
        public double MinSeconds = 30.0, GameplayGrace = 9.0, StaticGrace = 25.0;
        public int HudMin = 40;
        public int Width, Height;
        public string Windowing = "";
        public bool Restart = true;
        public bool KeepRunning, Quiet;
        public string Filter = "";
        public List<string> Sets = new List<string>();
        public string ShortcutPath = "";

        static bool Is(string a, string pattern) { return Regex.IsMatch(a, pattern); }

        public static Options Parse(string[] argv)
        {
            var o = new Options();
            int i = 0;
            while (i < argv.Length)
            {
                string a = argv[i];
                string value = i + 1 < argv.Length ? argv[i + 1] : null;
                bool took = true;
                if (Is(a, "^(--help|-h|/\\?)$")) { o.Action = "help"; took = false; }
                else if (Is(a, "^--(ui|window)$")) { o.Action = "ui"; took = false; }
                else if (Is(a, "^--list$")) { o.Action = "list"; took = false; }
                else if (Is(a, "^--(show-settings|settings)$")) { o.Action = "settings"; took = false; }
                else if (Is(a, "^--(setup|install|check|check-install)$")) { o.Action = "install"; took = false; }
                else if (Is(a, "^--install-addon$")) { o.Action = "install-addon"; took = false; }
                else if (Is(a, "^--report$")) { o.Action = "report"; took = false; }
                else if (Is(a, "^--stop$")) { o.Action = "stop"; took = false; }
                else if (Is(a, "^--main$")) { o.Stage = "@main"; took = false; }
                else if (Is(a, "^--collection$")) { o.Stage = "@collection"; took = false; }
                else if (Is(a, "^--title$"))
                    throw new ArgumentException("--title is gone: mgs4.exe --stage s00title_1 crashes the game on its own (verified with the add-on idle and frame generation off). Use --main.");
                else if (Is(a, "^--advance$")) { o.Advance = true; took = false; }
                else if (Is(a, "^--no-advance$")) { o.Advance = false; took = false; }
                else if (Is(a, "^--(mash-x|mash|flashbacks)$")) { o.MashX = true; took = false; }
                else if (Is(a, "^--(end-on-gameplay|end-on-hud)$")) { o.EndOnGameplay = true; took = false; }
                else if (Is(a, "^--(no-restart|attach)$")) { o.Restart = false; took = false; }
                else if (Is(a, "^--keep-running$")) { o.KeepRunning = true; took = false; }
                else if (Is(a, "^--quiet$")) { o.Quiet = true; took = false; }
                else if (Is(a, "^--no-scene-detect$")) { o.SceneDetect = false; took = false; }
                else if (a.StartsWith("-"))
                {
                    if (value == null) throw new ArgumentException(a + " needs a value  (--help lists the options)");
                    if (Is(a, "^--?-?stage$")) o.Stage = value;
                    else if (Is(a, "^--set$")) { o.Action = "set"; o.Sets.Add(value); }
                    else if (Is(a, "^--hold$")) o.Hold = Num(value);
                    else if (Is(a, "^--max-minutes$")) o.MaxMinutes = Num(value);
                    else if (Is(a, "^--start-timeout$")) o.StartTimeout = Num(value);
                    else if (Is(a, "^--min-seconds$")) o.MinSeconds = Num(value);
                    else if (Is(a, "^--press-every$")) o.PressEvery = Num(value);
                    else if (Is(a, "^--hud-min$")) o.HudMin = (int)Num(value);
                    else if (Is(a, "^--windowing$")) o.Windowing = value;
                    else if (Is(a, "^--keys$")) o.Keys = value;
                    else if (Is(a, "^--press-key$")) o.PressKey = value;
                    else if (Is(a, "^--settle$")) o.Settle = Num(value);
                    else if (Is(a, "^--?-?game-?dir$")) { o.GameDir = value; o.GameDirGiven = true; }
                    else if (Is(a, "^--shortcut$")) { o.Action = "shortcut"; o.ShortcutPath = value; }
                    else if (Is(a, "^--res$"))
                    {
                        Match m = Regex.Match(value, "^(\\d+)\\s*[xX]\\s*(\\d+)$");
                        if (!m.Success) throw new ArgumentException("--res wants WIDTHxHEIGHT, got " + value);
                        o.Width = int.Parse(m.Groups[1].Value);
                        o.Height = int.Parse(m.Groups[2].Value);
                    }
                    else throw new ArgumentException("unknown option " + a + "  (--help lists them)");
                }
                else
                {
                    took = false;
                    if (o.Action == "list") o.Filter = a;
                    else if (o.Stage == "") o.Stage = a;
                    else throw new ArgumentException("unexpected argument " + a);
                }
                i += took ? 2 : 1;
            }
            return o;
        }

        static double Num(string s)
        {
            return double.Parse(s, System.Globalization.CultureInfo.InvariantCulture);
        }

        // The same argument list as a line someone can paste into a terminal.
        public List<string> ToCli()
        {
            var a = new List<string>();
            if (Stage == "@main") a.Add("--main");
            else if (Stage == "@collection") a.Add("--collection");
            else a.Add(Stage);
            if (!Advance) a.Add("--no-advance");
            if (MashX) a.Add("--mash-x");
            if (EndOnGameplay) a.Add("--end-on-gameplay");
            if (Hold > 0) { a.Add("--hold"); a.Add(((int)Hold).ToString()); }
            if (KeepRunning) a.Add("--keep-running");
            if (Width > 0 && Height > 0) { a.Add("--res"); a.Add(Width + "x" + Height); }
            if (GameDirGiven && !string.IsNullOrEmpty(GameDir)) { a.Add("--game-dir"); a.Add(GameDir); }
            return a;
        }

        public static string Preview(IEnumerable<string> cli)
        {
            var parts = new List<string>();
            foreach (string c in cli) parts.Add(c.Contains(" ") ? "\"" + c + "\"" : c);
            return "mgs4-dlss-launcher " + string.Join(" ", parts);
        }

        public const string HelpText =
@"MGS4 DLSS Launcher - start the game or one scene of it, set the add-on up, check the install.

  mgs4-dlss-launcher                         open the window (Play / Settings / Setup)
  mgs4-dlss-launcher <stage id>              boot that scene and exit
  mgs4-dlss-launcher --main                  MGS4's own main menu, past the Master Collection screen
  mgs4-dlss-launcher --collection            the Master Collection launcher
  mgs4-dlss-launcher --list [text]           every launchable scene (filtered by id / name / act)
  mgs4-dlss-launcher --setup                 the window, opened on Setup: the game folder and the install check
                                                 (the first run opens there anyway; later ones open on Play)
  mgs4-dlss-launcher --report                the install check as text, for pasting into an issue
  mgs4-dlss-launcher --install-addon         copy the add-on that ships here next to mgs4.exe
  mgs4-dlss-launcher <id> --shortcut <file>  save that scene, with the run options given, as a .lnk
  mgs4-dlss-launcher --settings              print mgs4_dlss.ini the way the window shows it
  mgs4-dlss-launcher --set Key=Value [...]   write those keys into mgs4_dlss.ini
  mgs4-dlss-launcher --stop                  close a running game

Run options (any of them keeps this attached until the scene is done):
  --advance / --no-advance   press through the auto-save notice and ""press any button"" until the
                             first 3D frame. On by default.
  --mash-x                   keep tapping Cross for the whole scene, so the flashback prompts in
                             cutscenes fire. Wants ViGEmBus + ViGEmClient.dll; falls back to Enter.
  --end-on-gameplay          close the game when the cutscene hands over to gameplay (HUD up).
  --hold <seconds>           close the game that many seconds into the scene.
  --max-minutes <n>          ceiling on the whole run.
  --min-seconds <n>          ignore any ""it ended"" signal before this (default 30).
  --hud-min <n>              HUD draws in one heartbeat that count as gameplay (default 40).
  --keys ""5,ENTER,4,ENTER""   send that sequence to the game instead of pressing through the prompts:
                             key names (E, SPACE, ENTER, ESC, TAB, arrows, F1..) and numbers = seconds
                             to wait. For driving menus.
  --settle <seconds>         wait that long after the window appears before --keys (default 2).
  --press-key <name>         tap that keyboard key in the advance loop instead of the pad's Cross.
  --no-scene-detect          keep pressing for the whole --start-timeout instead of stopping at the
                             first 3D frame.
  --res <W>x<H>              render resolution on the command line (--res_width / --res_height).
  --windowing <mode>         windowed | full_borderless (the port often ignores this).
  --no-restart               drive the game that is already running instead of restarting it.
  --keep-running             leave the game up when the scene ends.
  --game-dir <path>          an install the Steam library search does not find.
  --quiet                    no console output.

Escape, held anywhere, stops an attached run.";
    }
}
