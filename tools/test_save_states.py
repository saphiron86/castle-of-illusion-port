#!/usr/bin/env python3
"""Save states: a headless run saved at game frame N and loaded again must go on exactly as the
run that never stopped.

For each case, three runs of the port (muted):
  A  the whole run, with --state-hash and --wav;
  B  the same with --save-state-at N (saving must not change the run itself);
  C  --load-state from B's state, for the rest of the frames.
C's machine hashes must match A's from frame N + 1 on, and C's sound must be the end of A's.

Usage: python tools/test_save_states.py [path/to/coi.exe]   (default build/port/Release/coi.exe)
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "port", "Release", "coi.exe")

# Stage 1 along the user's first drawn route (C:/Castle_of_Illusion_stages/routes/stage01_route1.sh):
# running right with jumps over the vines, so the widescreen spawn bookkeeping is used.
JUMPS = [538, 753, 850, 916, 1061, 1376, 1605, 1664, 1839, 1881, 1965]
WALK = ["--go-to-stage", "60:1", "--cheat", "invincibility", "--easy-bounce", "--smooth-swing",
        "--press", "right@158-2925"] + sum((["--press", f"c@{f}-{f + 27}"] for f in JUMPS), [])
# Co-op: Player 2 joins in level play and follows.
COOP = ["--join2", "300", "--press2", "right@330-2400"] + sum(
    (["--press2", f"c@{f + 20}-{f + 47}"] for f in JUMPS), [])

CASES = [
    ("native, widescreen, MD+ off", ["--no-mdplus"], 1100, 1800),
    ("native, widescreen, MD+ on", ["--mdplus"], 1100, 1800),
    ("interpreter (--no-native), widescreen", ["--no-native", "--no-mdplus"], 1100, 1800),
    ("native, 4:3", ["--no-widescreen", "--no-mdplus"], 1100, 1800),
    ("native, co-op", COOP, 1100, 1800),
    ("interpreter, co-op", ["--no-native"] + COOP, 1100, 1800),
    ("native, saved before Player 2 joins", COOP, 250, 1200),
]


def run(args, cwd=ROOT):
    env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    r = subprocess.run([EXE] + args, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"coi failed ({r.returncode}): {' '.join(args)}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return r.stdout


def hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            frame, _picture, h = line.split()
            out[int(frame)] = h
    return out


def samples(path):
    with open(path, "rb") as f:
        data = f.read()
    return data[44:]                                # (the WAV header written by write_wav)


def main():
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, extra, save_at, frames in CASES:
            p = lambda s: os.path.join(tmp, s)
            base = WALK + extra
            run(base + ["--frames", str(frames), "--state-hash", p("a.txt"), "--wav", p("a.wav")])
            run(base + ["--frames", str(frames), "--state-hash", p("b.txt"), "--save-state-at", str(save_at),
                        p("s.state")])
            run(base + ["--frames", str(frames - save_at - 1), "--load-state", p("s.state"), "--state-hash", p("c.txt"),
                        "--wav", p("c.wav")])
            a, b, c = hashes(p("a.txt")), hashes(p("b.txt")), hashes(p("c.txt"))
            problems = []
            if a != b:
                first = min(f for f in a if a.get(f) != b.get(f))
                problems.append(f"saving changed the run (from frame {first})")
            expected = {f: h for f, h in a.items() if f > save_at}
            if not c or min(c) != save_at + 1:
                problems.append(f"the loaded run starts at frame {min(c) if c else None}, not {save_at + 1}")
            diff = [f for f in sorted(expected) if c.get(f) != expected[f]]
            if diff:
                problems.append(f"{len(diff)} frames differ after loading, first {diff[0]}")
            wa, wc = samples(p("a.wav")), samples(p("c.wav"))
            if not wc or wa[len(wa) - len(wc):] != wc:
                problems.append("the sound after loading differs")
            print(f"{'FAIL' if problems else 'ok  '}  {name}: saved at {save_at}, {len(expected)} frames compared"
                  + ("".join(f"\n      {x}" for x in problems)))
            failed += bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
