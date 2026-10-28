"""Download the port's third-party libraries into third_party/, pinned by commit.

Each library is fetched from its official repository at a fixed commit, and the
checkout is verified against that commit. third_party/ is ignored by git.

Usage: python tools/fetch_deps.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"

# name: (repository, commit, note[, folders]): with folders, only those are checked out
# (a sparse, blobless fetch), for repositories much larger than the part used.
DEPS = {
    "SDL": ("https://github.com/libsdl-org/SDL.git",
            "829a65d769d935c4852f8159e964312c0957260a", "tag release-3.4.18, zlib license"),
    "Musashi": ("https://github.com/kstenerud/Musashi.git",
                "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd", "68000 core, MIT license"),
    "ymfm": ("https://github.com/aaronsgiles/ymfm.git",
             "81aec25ccbb98f4873a255f7551ac4dadac59b4a", "Yamaha FM cores, BSD-3-Clause license"),
    "z80": ("https://github.com/superzazu/z80.git",
            "d64fe10a2274e5e40019b1086bf7d8990cbc5f23", "Z80 core, MIT license"),
    "imgui": ("https://github.com/ocornut/imgui.git",
              "c1aaac65e896a45f81f47a4d68f9959e0f86c859", "Dear ImGui, tag v1.93.0, MIT license"),
    "glsl-shaders": ("https://github.com/libretro/glsl-shaders.git",
                     "7b9df1a8abd68140ddb28c81c607f5ffab319c25",
                     "libretro shaders, only hqx/ (LGPL 2.1 or later)", ["hqx"]),
}


def git(args, cwd):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def fetch(name, url, commit, folders=None):
    target = THIRD_PARTY / name
    if (target / ".git").exists():
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=target,
                              capture_output=True, text=True).stdout.strip()
        if head == commit:
            print("%s: already at %s" % (name, commit[:12]))
            return
        print("%s: at %s, expected %s; fetching" % (name, head[:12], commit[:12]))
    else:
        target.mkdir(parents=True, exist_ok=True)
        git(["init", "-q"], target)
        git(["remote", "add", "origin", url], target)
    if folders:
        git(["sparse-checkout", "set", "--no-cone", *("/%s/" % f for f in folders)], target)
        git(["fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit], target)
    else:
        git(["fetch", "-q", "--depth", "1", "origin", commit], target)
    git(["checkout", "-q", "--detach", commit], target)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=target,
                          capture_output=True, text=True).stdout.strip()
    if head != commit:
        raise SystemExit("%s: checked out %s, expected %s" % (name, head, commit))
    print("%s: %s" % (name, commit[:12]))


def main():
    for name, (url, commit, note, *folders) in DEPS.items():
        print("== %s (%s)" % (name, note))
        fetch(name, url, commit, *folders)
    return 0


if __name__ == "__main__":
    sys.exit(main())
