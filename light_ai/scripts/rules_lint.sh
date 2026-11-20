#!/bin/bash
# rules_lint.sh: the project rules that clang-tidy cannot see (AST matchers have no #include), part of the tidy
# gate (scripts/tidy_run.sh runs it after its self-test; #93). Stdlib python, the whole tree, well under 1 s.
# Usage: bash scripts/rules_lint.sh [root]   (default: the project root; the self-test passes tests/tidy_probe/lint)
# Scans <root>/{src,include,tests}/**/*.{cpp,hpp,h}, never <root>/tests/tidy_probe/ (the self-test fixtures).
# Rules (CONTRIBUTING.md, Coding Rules):
#   line-length      a line longer than 140 columns (.clang-format ColumnLimit; UTF-8 counts characters)
#   header-ext       a .h header: new headers use .hpp (the nine legacy names are in the allowlist)
#   include-order    a <...> include after the first "..." include (system and SDK headers first)
#   banned-include   GStreamer or OpenCV (<gst/...>, <gstreamer...>, <opencv...>)
#   core-include     include/core/ and src/core/ include only the C++17 standard library headers (the C++ names:
#                    <cstdint>, not <stdint.h>), core/*, pod_list.hpp and pod_ring.hpp (SDK-free core: host tests)
#   nolint           a NOLINT / NOLINTNEXTLINE whose check list switches a project rule off: an entry matching a
#                    custom-* name of scripts/tidy_rules.yaml or misc-const-correctness (#94; it must be enabled in
#                    the Checks of that file) as clang-tidy globs it ('*' the only wildcard: '*', 'cust*',
#                    '*-catch-all', 'misc-*'); every NOLINT of a line counts. Each one must be in the allowlist
#                    (CONTRIBUTING lists the sites)
#   nolint-reason    such a NOLINT(...) without ": <reason>" after it
#   nolint-range     a NOLINTBEGIN / NOLINTEND whose check list names a project rule (the same globs), or a bare
#                    NOLINTBEGIN (every check, detail '*'; its bare NOLINTEND is not reported again): never
#                    allowed, an allowlist line (no line number) must not cover a whole range; an allowlist line
#                    for it fails as well
#   nolint-blanket   a NOLINT without a check list, prose included (clang-tidy honours one anywhere in a line):
#                    it would switch the custom checks off too; never allowed, like nolint-range
# Allowlist: <root>/scripts/rules_allow.txt, one line per allowed finding: "<path> <rule> [<detail>]" (no line
# numbers: an edit above a site does not break it). A finding not on it fails, and so does an entry that matches
# no finding (stale).
# Output: one "path:line: rule: detail -- message" line per failing finding, sorted, then the summary line
# "rules-lint: PASS|FAIL ...". Exit 0 clean, 1 findings (or a policed built-in check not enabled in
# scripts/tidy_rules.yaml), 2 usage (no root, no scripts/tidy_rules.yaml).
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)"
ROOT=${1:-"$HERE/.."}
if [ ! -d "$ROOT" ]; then
  echo "rules-lint: no such root: $ROOT"
  exit 2
fi
ROOT="$(cd "$ROOT" && pwd -P)"
exec python3 -I - "$ROOT" "$HERE/tidy_rules.yaml" <<'PYEOF'
import os, re, sys

root = sys.argv[1]
if not os.path.exists(sys.argv[2]):
    print("rules-lint: no such config: " + sys.argv[2] + " (the custom check names come from it)")
    sys.exit(2)
with open(sys.argv[2], encoding="utf-8") as f:
    CFG = f.read()
CUSTOM = ["custom-" + m for m in re.findall(r'^\s*-\s*Name:\s*(\S+)', CFG, re.M)]  # as clang-tidy names them
if not CUSTOM:
    print("rules-lint: FAIL -- no custom check names in " + sys.argv[2])
    sys.exit(1)
# The built-in checks of the rules pass that enforce a project rule (#94): their NOLINTs are policed like the
# custom ones. Each must be enabled by name in the Checks of the config (the last entry naming it, with or without
# a leading '-', is the one that counts, as in clang-tidy), or this list has drifted from it.
BUILTIN = ("misc-const-correctness",)
CHECKS = re.search(r"^Checks:\s*'([^']*)'", CFG, re.M)
ENTRIES = [c.strip() for c in CHECKS.group(1).split(",")] if CHECKS else []
for name in BUILTIN:
    last = [c for c in ENTRIES if c.lstrip("-") == name][-1:]
    if last != [name]:
        print("rules-lint: FAIL -- " + name + " is policed here but not enabled in the Checks of " + sys.argv[2])
        sys.exit(1)
POLICED = CUSTOM + list(BUILTIN)
MAX_COLS = 140
STD = set("""algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits
clocale cmath codecvt complex condition_variable csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring
ctime cuchar cwchar cwctype deque exception execution filesystem forward_list fstream functional future
initializer_list iomanip ios iosfwd iostream istream iterator limits list locale map memory memory_resource mutex
new numeric optional ostream queue random ratio regex scoped_allocator set shared_mutex sstream stack stdexcept
streambuf string string_view system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set
utility valarray variant vector""".split())
CORE_LOCAL = ("pod_list.hpp", "pod_ring.hpp")
INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
NOLINT = re.compile(r'NOLINT(NEXTLINE|BEGIN|END)?')
PAREN = re.compile(r'\(([^)]*)\)')
BANNED = ("gst/", "gstreamer", "opencv")

findings = []  # (path, line, rule, detail, message)
NEVER = ("nolint-range", "nolint-blanket")  # rules no allowlist line can allow


def add(path, line, rule, detail, message):
    findings.append((path, line, rule, detail, message))


def glob_re(entry):
    # clang-tidy's NOLINT globs: '*' is the only wildcard ('?' and '[' are literal)
    return re.compile(".*".join(re.escape(p) for p in entry.split("*")))


def check_nolint(rel, n, text):
    # every NOLINT of the line: its own (...) right after it, its reason up to the next NOLINT
    marks = list(NOLINT.finditer(text))
    for i, m in enumerate(marks):
        kind = m.group(1) or ""
        after = text[m.end():marks[i + 1].start() if i + 1 < len(marks) else len(text)]
        paren = PAREN.match(after)
        if paren is None:
            if kind == "BEGIN":  # a bare range switches every check off, the custom ones too
                add(rel, n, "nolint-range", "*",
                    "NOLINTBEGIN / NOLINTEND cannot switch a project rule off: NOLINT or NOLINTNEXTLINE at the site")
            elif kind != "END":  # a bare END only closes a bare BEGIN, reported there
                add(rel, n, "nolint-blanket", "",
                    "NOLINT without a check list (clang-tidy reads one anywhere in a line, prose too) "
                    "switches the custom checks off")
            continue
        entries = [c.strip() for c in paren.group(1).split(",") if c.strip()]
        custom = [c for c in entries if any(glob_re(c).fullmatch(name) for name in POLICED)]
        if kind in ("BEGIN", "END"):
            for c in custom:
                add(rel, n, "nolint-range", c,
                    "NOLINTBEGIN / NOLINTEND cannot switch a project rule off: NOLINT or NOLINTNEXTLINE at the site")
            continue
        for c in custom:
            add(rel, n, "nolint", c, "a project rule switched off: only at the sites of scripts/rules_allow.txt")
        if custom and not re.match(r'\s*:\s*\S', after[paren.end():]):
            add(rel, n, "nolint-reason", ",".join(custom), "a NOLINT of a project rule needs ': <reason>' after it")


def check_file(rel, path):
    if rel.endswith(".h"):
        add(rel, 1, "header-ext", "", "new headers use .hpp (CONTRIBUTING, Header extensions)")
    core = rel.startswith("include/core/") or rel.startswith("src/core/")
    seen_quote = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            text = line.rstrip("\r\n")
            if len(text) > MAX_COLS:
                add(rel, n, "line-length", "", "%d columns > %d" % (len(text), MAX_COLS))
            m = INC.match(text)
            if m:
                kind, target = m.group(1), m.group(2)
                if target.startswith(BANNED):
                    add(rel, n, "banned-include", target, "no GStreamer, no OpenCV")
                if kind == '"':
                    seen_quote = True
                elif seen_quote:
                    add(rel, n, "include-order", target, "system and SDK headers first, project headers last")
                if core and kind == "<" and target not in STD:
                    add(rel, n, "core-include", target, "core/ includes only the C++17 standard library")
                if core and kind == '"' and not (target.startswith("core/") or target in CORE_LOCAL):
                    add(rel, n, "core-include", target, "core/ includes only core/*, pod_list.hpp, pod_ring.hpp")
            if "NOLINT" in text:
                check_nolint(rel, n, text)


files = 0
for top in ("src", "include", "tests"):
    base = os.path.join(root, top)
    for dirpath, dirs, names in os.walk(base):
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir == os.path.join("tests", "tidy_probe"):
            dirs[:] = []
            continue
        dirs.sort()
        for name in sorted(names):
            if name.endswith((".cpp", ".hpp", ".h")):
                files += 1
                rel = os.path.join(rel_dir, name)
                check_file(rel, os.path.join(dirpath, name))

allow_path = os.path.join(root, "scripts", "rules_allow.txt")
allow = []  # [key, line number, used]
if os.path.exists(allow_path):
    with open(allow_path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            parts = line.split("#", 1)[0].split()
            if parts:
                allow.append([(parts[0], parts[1] if len(parts) > 1 else "", " ".join(parts[2:])), n, False])

out = []
allowed = 0
for path, line, rule, detail, message in sorted(findings, key=lambda x: (x[0], x[1], x[2], x[3])):
    for a in allow:
        if not a[2] and a[0] == (path, rule, detail) and rule not in NEVER:
            a[2] = True
            allowed += 1
            break
    else:
        out.append("%s:%d: %s%s -- %s" % (path, line, rule, ": " + detail if detail else "", message))
for key, n, used in allow:
    if key[1] in NEVER:
        out.append("scripts/rules_allow.txt:%d: not allowable: %s -- a NOLINT range or a blanket NOLINT is never allowed"
                   % (n, " ".join(k for k in key if k)))
    elif not used:
        out.append("scripts/rules_allow.txt:%d: stale: %s -- matches no finding" % (n, " ".join(k for k in key if k)))
for line in out:
    print(line)
verdict = "FAIL" if out else "PASS"
print("rules-lint: %s (%d files, %d findings, %d allowed by scripts/rules_allow.txt)" % (verdict, files, len(out), allowed))
sys.exit(1 if out else 0)
PYEOF
