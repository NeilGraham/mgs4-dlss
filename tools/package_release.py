"""Assembles the release assets into release\\ (git-ignored):

    python tools\\package_release.py --version 1.2.0

    release\\mgs4_dlss.addon64         the built add-on (build\\mgs4_dlss.addon64 - run dlss-addon\\build.bat first)
    release\\mgs4_dlss.ini             the shipped configuration (dlss-addon\\mgs4_dlss.ini)
    release\\mgs4_dlss_launcher.zip    the launcher (mgs4-dlss-launcher.bat, launcher\\, the tools\\ data it reads), the
                                      docs, config.example.ini, and the same add-on + ini at the zip's root - which is
                                      where the launcher's "Install the add-on" looks for them
    release\\notes-<version>.md        that version's section of docs\\releases.md, for `gh release create --notes-file`

The launcher exe is deliberately not in the zip: it is built on the first run of the .bat, because the icon it wears is
read from the mgs4.exe on the user's machine and that artwork is Konami's.
"""
import argparse, io, os, re, sys, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# what the launcher zip carries, relative to the repo root
ZIP_FILES = [
    "mgs4-dlss-launcher.bat",
    "config.example.ini",
    "README.md",
    "launcher/build.ps1",
    "tools/paths.ps1", "tools/paths.py", "tools/paths.sh", "tools/game_icon.ps1",
    "tools/install_manifest.json", "tools/scenes.csv", "tools/labels.json", "tools/scene_info.json",
    "tools/stages.md", "tools/launch_stage.ps1",
]
ZIP_DIRS = ["launcher/src", "docs"]
ZIP_NAME = "mgs4_dlss_launcher.zip"


def release_notes(version):
    text = open(os.path.join(REPO, "docs", "releases.md"), encoding="utf-8").read()
    m = re.search(r"^## v" + re.escape(version) + r"\b.*?$", text, re.M)
    if not m:
        sys.exit("docs/releases.md has no '## v%s' section" % version)
    start = m.end() + 1
    nxt = re.search(r"^## ", text[start:], re.M)
    body = text[start:start + nxt.start()] if nxt else text[start:]
    # release bodies are rendered on GitHub: promote the section's sub-headings one level
    body = re.sub(r"^### ", "## ", body.strip() + "\n", flags=re.M)
    return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="e.g. 1.2.0 (must have a section in docs/releases.md)")
    ap.add_argument("--out", default=os.path.join(REPO, "release"))
    a = ap.parse_args()

    addon = os.path.join(REPO, "build", "mgs4_dlss.addon64")
    ini = os.path.join(REPO, "dlss-addon", "mgs4_dlss.ini")
    for p in (addon, ini):
        if not os.path.exists(p):
            sys.exit("missing %s%s" % (p, " - run dlss-addon\\build.bat first" if p == addon else ""))
    os.makedirs(a.out, exist_ok=True)

    import shutil
    shutil.copy2(addon, os.path.join(a.out, "mgs4_dlss.addon64"))
    shutil.copy2(ini, os.path.join(a.out, "mgs4_dlss.ini"))

    with open(os.path.join(a.out, "notes-%s.md" % a.version), "w", encoding="utf-8", newline="\n") as f:
        f.write(release_notes(a.version))

    files = list(ZIP_FILES)
    for d in ZIP_DIRS:
        for root, _dirs, names in os.walk(os.path.join(REPO, d)):
            for n in sorted(names):
                if n.endswith((".log", ".pyc")):
                    continue
                files.append(os.path.relpath(os.path.join(root, n), REPO).replace("\\", "/"))
    for rel in files:
        if not os.path.exists(os.path.join(REPO, rel)):
            sys.exit("zip content missing: " + rel)

    zpath = os.path.join(a.out, ZIP_NAME)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in files:
            z.write(os.path.join(REPO, rel), rel)
        z.write(addon, "mgs4_dlss.addon64")
        z.write(ini, "mgs4_dlss.ini")
    print("release %s assembled in %s:" % (a.version, a.out))
    for n in ("mgs4_dlss.addon64", "mgs4_dlss.ini", ZIP_NAME, "notes-%s.md" % a.version):
        print("  %-28s %9d bytes" % (n, os.path.getsize(os.path.join(a.out, n))))
    print("  zip: %d files" % (len(files) + 2))


if __name__ == "__main__":
    main()
