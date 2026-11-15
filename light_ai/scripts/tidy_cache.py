#!/usr/bin/env python3
"""tidy_cache.py: the exact cache key of one clang-tidy run (#42), stdlib only.

Usage: tidy_cache.py <compile_db_dir> <source> <timeout> <clang++> -- <clang-tidy argv...>
Prints the sha256 key and exits 0; prints nothing and exits 1 when no key can be made
(no DB entry, the preprocessor failed or printed nothing, an included file is unreadable):
the caller then runs clang-tidy as a plain miss and never stores the result.

The key covers every input of the run:
  - the clang-tidy argv and the timeout, `clang-tidy --version`, `<clang++> --version`;
  - the TU's command line from the (stripped) compile DB;
  - the bytes of every .clang-tidy from the TU's directory up to /, and of the file named by a
    --config-file= argument (the cognitive pass of tidy_run.sh: its argv holds only the file name);
  - the output of `<clang++> <same args> -E -CC` (an LLVM driver, not the DB's gcc: clang++-11, the
    driver of the main pass; the cognitive pass's LLVM 23 wheel ships none, so its include set is the one
    LLVM 11's predefined macros select -- a difference is possible only in system headers);
  - the raw bytes of every file named in its line markers. -E drops or normalizes what some checks
    read (comments with NOLINT, intra-line columns), so the raw files make any edit of any
    included file, system headers too, change the key; a different include resolution changes
    the file list, so it changes the key as well.
"""
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

MARKER = re.compile(rb'^# \d+ "((?:[^"\\]|\\.)*)"')
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}
DROP = {"-c", "-MD", "-MMD"}


def tool_version(argv0):
    return subprocess.run([argv0, "--version"], capture_output=True, check=True).stdout


def db_entry(db_dir, source):
    path = os.path.realpath(source)
    with open(os.path.join(db_dir, "compile_commands.json"), encoding="utf-8") as f:
        for e in json.load(f):
            if os.path.realpath(os.path.join(e["directory"], e["file"])) == path:
                return e
    return None


def preprocess_argv(entry, clangxx):
    args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    out, skip = [clangxx], False
    for a in args[1:]:
        if skip:
            skip = False
        elif a in DROP_WITH_ARG:
            skip = True
        elif a not in DROP:
            out.append(a)
    return out + ["-E", "-CC", "-o", "-"]


def make_key(db_dir, source, timeout, clangxx, tidy_argv):
    entry = db_entry(db_dir, source)
    if entry is None:
        return None
    h = hashlib.sha256()

    def part(tag, data):
        h.update(b"%s %d\n" % (tag, len(data)))
        h.update(data)

    part(b"argv", "\0".join(tidy_argv + [timeout]).encode())
    part(b"tidy", tool_version(tidy_argv[0]))
    part(b"cxx", tool_version(clangxx))
    part(b"cmd", json.dumps(entry, sort_keys=True).encode())
    for a in tidy_argv[1:]:
        for flag in ("--config-file=", "-config-file="):
            if a.startswith(flag):
                with open(a[len(flag):], "rb") as f:
                    part(b"config-file", f.read())
    d = os.path.dirname(os.path.realpath(source))
    while True:
        cfg = os.path.join(d, ".clang-tidy")
        if os.path.isfile(cfg):
            with open(cfg, "rb") as f:
                part(cfg.encode(), f.read())
        if d == "/":
            break
        d = os.path.dirname(d)
    pp = subprocess.run(preprocess_argv(entry, clangxx), cwd=entry["directory"], capture_output=True, check=False)
    if pp.returncode != 0 or not pp.stdout:
        return None
    part(b"pp", pp.stdout)
    files = set()
    for line in pp.stdout.splitlines():
        m = MARKER.match(line)
        if m and not m.group(1).startswith(b"<"):
            files.add(m.group(1).decode("unicode_escape"))
    for name in sorted(files):
        with open(os.path.join(entry["directory"], name), "rb") as f:
            part(name.encode(), f.read())
    return h.hexdigest()


def main():
    if len(sys.argv) < 7 or sys.argv[5] != "--":
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    try:
        key = make_key(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[6:])
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError):
        key = None
    if key is None:
        return 1
    print(key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
