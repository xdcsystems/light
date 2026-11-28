#!/usr/bin/env python3
"""tidy_cache.py: the exact cache key of one clang-tidy run (#42), stdlib only.

Usage: tidy_cache.py <compile_db_dir> <source> <timeout> <clang++> -- <clang-tidy argv...>
Prints the sha256 key and exits 0; prints nothing and exits 1 when no key can be made
(no DB entry, the preprocessor failed or printed nothing, an included file is unreadable):
the caller then runs clang-tidy as a plain miss and never stores the result.

The key covers every input of the run:
  - the clang-tidy argv and the timeout, `clang-tidy --version`, `<clang++> --version` (the raw bytes of
    the files named by TIDY_BIN_VER_FILE / TIDY_CXX_VER_FILE when set: tidy_run.sh asks once per run);
  - the TU's command line from the (stripped) compile DB;
  - the bytes of every .clang-tidy from the TU's directory up to /, and of the file named by a
    --config-file= argument (the rules pass of tidy_run.sh: its argv holds only the file name);
  - the preprocessor digest (pp_digest), the sha256 of the next two items:
  - the output of `<clang++> <same args> -E -CC` (an LLVM driver, not the DB's gcc: clang++-11, the
    driver of the main pass; the rules pass's LLVM 23 wheel ships none, so its include set is the one
    LLVM 11's predefined macros select -- a difference is possible only in system headers);
  - the raw bytes of every file named in its line markers. -E drops or normalizes what some checks
    read (comments with NOLINT, intra-line columns), so the raw files make any edit of any
    included file, system headers too, change the key; a different include resolution changes
    the file list, so it changes the key as well.

With TIDY_PP_CACHE_DIR set (tidy_run.sh: emptied at the start of every run, under its run lock) the digest
of a TU is computed once and reused by the second pass. It is stored with the stat stamps of the files it
read; a changed stamp (a file edited between the passes) computes it again, so a stale digest never keys the
result of the new content.
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


def tool_version(argv0, saved_env):
    saved = os.environ.get(saved_env)
    if not saved:
        return subprocess.run([argv0, "--version"], capture_output=True, check=True).stdout
    with open(saved, "rb") as f:
        data = f.read()
    if not data:
        raise ValueError("empty version file: " + saved)
    return data


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


def add_part(h, tag, data):
    h.update(b"%s %d\n" % (tag, len(data)))
    h.update(data)


def file_stamp(path):
    s = os.stat(path)
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


def compute_pp_digest(entry, clangxx):
    """(digest, {path: stamp}) of the TU's preprocessed form and included files; None if the preprocessor fails."""
    pp = subprocess.run(preprocess_argv(entry, clangxx), cwd=entry["directory"], capture_output=True, check=False)
    if pp.returncode != 0 or not pp.stdout:
        return None
    h = hashlib.sha256()
    add_part(h, b"pp", pp.stdout)
    files = set()
    for line in pp.stdout.splitlines():
        m = MARKER.match(line)
        if m and not m.group(1).startswith(b"<"):
            files.add(m.group(1).decode("unicode_escape"))
    stamps = {}
    for name in sorted(files):
        path = os.path.join(entry["directory"], name)
        stamps[path] = file_stamp(path)  # before the read: an edit during it changes the stamp
        with open(path, "rb") as f:
            add_part(h, name.encode(), f.read())
    return h.hexdigest(), stamps


def memo_lookup(memo):
    """The stored digest, or None: no memo, an unreadable one, or one of its files changed."""
    try:
        with open(memo, encoding="utf-8") as f:
            saved = json.load(f)
        if all(file_stamp(path) == stamp for path, stamp in saved["stamps"].items()):
            return saved["digest"]
    except (OSError, ValueError, KeyError):
        pass
    return None


def memo_store(memo, digest, stamps):
    """Atomic; a failed write only costs the next pass the preprocessing."""
    tmp = "%s.%d" % (memo, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"digest": digest, "stamps": stamps}, f)
        os.replace(tmp, memo)
    except OSError:
        pass


def pp_digest(db_dir, source, entry, clangxx):
    memo_dir = os.environ.get("TIDY_PP_CACHE_DIR")
    memo = None
    if memo_dir:
        name = hashlib.sha256("\0".join((db_dir, source, clangxx)).encode()).hexdigest()
        memo = os.path.join(memo_dir, name + ".pp")
        digest = memo_lookup(memo)
        if digest is not None:
            return digest
    computed = compute_pp_digest(entry, clangxx)
    if computed is None:
        return None
    if memo:
        memo_store(memo, *computed)
    return computed[0]


def make_key(db_dir, source, timeout, clangxx, tidy_argv):
    entry = db_entry(db_dir, source)
    if entry is None:
        return None
    h = hashlib.sha256()

    def part(tag, data):
        add_part(h, tag, data)

    part(b"argv", "\0".join(tidy_argv + [timeout]).encode())
    part(b"tidy", tool_version(tidy_argv[0], "TIDY_BIN_VER_FILE"))
    part(b"cxx", tool_version(clangxx, "TIDY_CXX_VER_FILE"))
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
    digest = pp_digest(db_dir, source, entry, clangxx)
    if digest is None:
        return None
    part(b"pp_digest", digest.encode())
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
