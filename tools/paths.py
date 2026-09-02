"""Where everything lives on this machine - the one place the tools ask for a path.

Nothing in this repo depends on where the checkout sits or where the game is installed. Each value comes from,
in order: an environment variable, `config.ini` in the repo root (copy `config.example.ini`), then auto-detection.

  python tools\\paths.py          - print the resolved paths (and where each one came from)

    GAME_DIR   the folder with mgs4.exe          MGS4_DIR      Steam libraries are searched for app 2492670
    OUT_DIR    recordings / analysis output      MGS4_OUT      <repo>\\work
    GOLD       the gold set                      -             <OUT_DIR>\\gold
    FFMPEG     ffmpeg.exe                        MGS4_FFMPEG   PATH
    FFPROBE    ffprobe.exe                       MGS4_FFPROBE  PATH
    PYTHON     python.exe                        MGS4_PYTHON   this interpreter
"""
import os, pathlib, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CONFIG = os.path.join(REPO, "config.ini")
STEAM_APPID = "2492670"          # METAL GEAR SOLID 4: Guns of the Patriots - Master Collection Version
INSTALL_DIR = "METAL GEAR SOLID 4"

_origin = {}                     # key -> where the value came from, for the report below


def _file_values():
    vals = {}
    if os.path.exists(CONFIG):
        with open(CONFIG, encoding="utf-8-sig") as f:
            for line in f:
                line = line.strip()
                if not line or line[0] in ";#[" or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                v = v.strip().strip('"')
                if v:
                    vals[k.strip().upper()] = v
    return vals


_FILE = _file_values()


def setting(key, detect=None, default=None):
    """The configured value of `key` (env > config.ini > detect() > default), or None."""
    v = os.environ.get(key)
    src = "env " + key
    if not v:
        v, src = _FILE.get(key.upper()), "config.ini"
    if not v and detect:
        v, src = detect(), "detected"
    if not v:
        v, src = default, "default"
    _origin[key] = src if v else "not found"
    return v


def _real(path):
    """Absolute, with the on-disk capitalization when the path exists."""
    path = os.path.normpath(os.path.expandvars(os.path.expanduser(path)))
    try:
        return str(pathlib.Path(path).resolve())
    except OSError:
        return path


def _steam_libraries():
    """Every Steam library folder on this machine, from the registry / the usual install spots."""
    roots = []
    try:
        import winreg
        for hive, path, name in ((winreg.HKEY_CURRENT_USER, r"SOFTWARE\Valve\Steam", "SteamPath"),
                                 (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, path) as k:
                    roots.append(winreg.QueryValueEx(k, name)[0])
            except OSError:
                pass
    except ImportError:
        pass
    roots += [r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"]

    # Steam's registry entry and libraryfolders.vdf cover the normal case; a library on a drive Steam has forgotten
    # is still worth finding, so every drive is tried in letter order for the handful of places a library sits.
    for letter in "CDEFGHIJKLMNOPQRSTUVWXYZAB":
        drive = letter + ":"
        if not os.path.isdir(drive + os.sep):
            continue
        for sub in ("", "SteamLibrary", "Steam", os.path.join("Games", "Steam"),
                    os.path.join("Program Files (x86)", "Steam"), os.path.join("Program Files", "Steam")):
            roots.append(os.path.join(drive + os.sep, sub) if sub else drive + os.sep)

    libs = []
    for root in roots:
        root = os.path.normpath(root)
        if root not in libs and os.path.isdir(root):
            libs.append(root)
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if not os.path.exists(vdf):
            continue
        try:
            text = open(vdf, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for line in text.splitlines():
            parts = line.split('"')
            if len(parts) >= 5 and parts[1] == "path":
                lib = os.path.normpath(parts[3].replace("\\\\", "\\"))
                if lib not in libs and os.path.isdir(lib):
                    libs.append(lib)
    return libs


def _detect_game():
    """The Steam install of app 2492670, from whichever library folder holds it."""
    for lib in _steam_libraries():
        apps = os.path.join(lib, "steamapps")
        names = [INSTALL_DIR]
        manifest = os.path.join(apps, "appmanifest_%s.acf" % STEAM_APPID)
        if os.path.exists(manifest):
            for line in open(manifest, encoding="utf-8", errors="ignore"):
                parts = line.split('"')
                if len(parts) >= 5 and parts[1] == "installdir":
                    names.insert(0, parts[3])
        for name in names:
            cand = os.path.join(apps, "common", name, "MGS4")
            if os.path.exists(os.path.join(cand, "mgs4.exe")):
                return cand
    return None


def _normalize_game(path):
    """Accept either the install root or the MGS4 folder inside it; return the folder holding mgs4.exe."""
    if not path:
        return None
    path = _real(path)
    if os.path.exists(os.path.join(path, "MGS4", "mgs4.exe")):
        return os.path.join(path, "MGS4")
    return path


GAME_DIR = _normalize_game(setting("MGS4_DIR", _detect_game))
OUT_DIR = _real(setting("MGS4_OUT", default=os.path.join(REPO, "work")))
GOLD = os.path.join(OUT_DIR, "gold")
FFMPEG = setting("MGS4_FFMPEG", lambda: shutil.which("ffmpeg"), "ffmpeg")
FFPROBE = setting("MGS4_FFPROBE", lambda: shutil.which("ffprobe"), "ffprobe")
PYTHON = setting("MGS4_PYTHON", lambda: sys.executable, "python")

GAME_EXE = os.path.join(GAME_DIR, "mgs4.exe") if GAME_DIR else None
ADDON_LOG = os.path.join(GAME_DIR, "logs", "mgs4_dlss.log") if GAME_DIR else None
ADDON_INI = os.path.join(GAME_DIR, "mgs4_dlss.ini") if GAME_DIR else None


def require_game():
    """GAME_DIR, or an explanation of how to set it."""
    if GAME_DIR and os.path.exists(os.path.join(GAME_DIR, "mgs4.exe")):
        return GAME_DIR
    raise SystemExit("mgs4.exe not found (%s).\nSet MGS4_DIR in %s (copy %s) or in the environment."
                     % (GAME_DIR or "no install found in the Steam libraries", CONFIG,
                        os.path.join(REPO, "config.example.ini")))


def out(*parts):
    """A path under OUT_DIR (which is created if it is not there yet)."""
    os.makedirs(OUT_DIR, exist_ok=True)
    return os.path.join(OUT_DIR, *parts)


def free_gb(path=None):
    """Free space on the drive holding `path` (OUT_DIR by default)."""
    return shutil.disk_usage(path or OUT_DIR).free / (1024 ** 3)


if __name__ == "__main__":
    print("repo      ", REPO)
    print("config    ", CONFIG if os.path.exists(CONFIG) else CONFIG + "  (absent - defaults / detection)")
    for name, value, key in (("GAME_DIR", GAME_DIR, "MGS4_DIR"), ("OUT_DIR", OUT_DIR, "MGS4_OUT"),
                             ("GOLD", GOLD, None), ("FFMPEG", FFMPEG, "MGS4_FFMPEG"),
                             ("FFPROBE", FFPROBE, "MGS4_FFPROBE"), ("PYTHON", PYTHON, "MGS4_PYTHON")):
        src = _origin.get(key, "") if key else ""
        exists = "" if value and os.path.exists(value) else "   <- missing"
        print("%-10s %-70s %s%s" % (name, value, src, exists))
