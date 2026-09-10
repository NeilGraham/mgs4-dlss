"""Assembles the release assets into release\\ (git-ignored):

    python tools\\package_release.py --version 1.3.0

    release\\mgs4-dlss-launcher.exe    the launcher, one file, with the add-on and its ini built in - the only download
                                      most people need (launcher\\build.ps1 -Release)
    release\\mgs4_dlss.addon64         the built add-on (build\\mgs4_dlss.addon64 - run dlss-addon\\build.bat first)
    release\\mgs4_dlss.ini             the shipped configuration (dlss-addon\\mgs4_dlss.ini)
    release\\notes-<version>.md        the release body for `gh release create --notes-file`: docs\\release-install.md
                                      (the install steps every release page carries) and then that version's
                                      section of docs\\releases.md

The exe is built here with -Release, so it wears the launcher's own icon (tools\\launcher_icon.ps1) rather than the
one a local build reads out of mgs4.exe - that artwork is Konami's and does not ship. The loose add-on and ini are
for a manual install, or for updating an add-on that is already in place without touching the launcher.
"""
import argparse, os, re, shutil, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE_NAME = "mgs4-dlss-launcher.exe"


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
    # every release page opens with how to install - with the launcher, and by hand - then what changed
    install = open(os.path.join(REPO, "docs", "release-install.md"), encoding="utf-8-sig").read().strip() + "\n"
    return install + "\n## What changed in v%s\n\n" % version + body


def build_launcher(out):
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", os.path.join(REPO, "launcher", "build.ps1"), "-Release", "-Out", out]
    r = subprocess.run(cmd, cwd=REPO)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit("launcher build failed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="e.g. 1.3.0 (must have a section in docs/releases.md)")
    ap.add_argument("--out", default=os.path.join(REPO, "release"))
    a = ap.parse_args()

    addon = os.path.join(REPO, "build", "mgs4_dlss.addon64")
    ini = os.path.join(REPO, "dlss-addon", "mgs4_dlss.ini")
    for p in (addon, ini):
        if not os.path.exists(p):
            sys.exit("missing %s%s" % (p, " - run dlss-addon\\build.bat first" if p == addon else ""))
    notes = release_notes(a.version)         # before anything is built: a missing section fails fast
    os.makedirs(a.out, exist_ok=True)

    shutil.copy2(addon, os.path.join(a.out, "mgs4_dlss.addon64"))
    shutil.copy2(ini, os.path.join(a.out, "mgs4_dlss.ini"))
    with open(os.path.join(a.out, "notes-%s.md" % a.version), "w", encoding="utf-8", newline="\n") as f:
        f.write(notes)

    exe = os.path.join(a.out, EXE_NAME)
    build_launcher(exe)
    # the exe must carry the add-on: a release build refuses without it, but check the size too
    if os.path.getsize(exe) < os.path.getsize(addon):
        sys.exit("%s is smaller than the add-on it should contain" % exe)

    print("release %s assembled in %s:" % (a.version, a.out))
    for n in (EXE_NAME, "mgs4_dlss.addon64", "mgs4_dlss.ini", "notes-%s.md" % a.version):
        print("  %-28s %9d bytes" % (n, os.path.getsize(os.path.join(a.out, n))))
    print("publish with:")
    print("  gh release create v%s --title v%s --notes-file release\\notes-%s.md release\\%s release\\mgs4_dlss.addon64 release\\mgs4_dlss.ini"
          % (a.version, a.version, a.version, EXE_NAME))


if __name__ == "__main__":
    main()
