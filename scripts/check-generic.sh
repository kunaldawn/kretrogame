#!/usr/bin/env bash
# Fails if any file git tracks, or would track, names something from the local
# game collection.
#
# The repository holds the generic tool only (see CLAUDE.md). What counts as
# "the collection" is read from the local, gitignored data on this machine, so
# the script itself names nothing:
#
#   games/*.toml   manifest ids, names, disc references, setup and executable
#                  names, install subdirectories, launch argument words
#   db/discs.txt   every field: fingerprints, sizes, volume labels, set ids, names
#   iso/           image, archive and installer file names, and their words
#   tests/local/   the first column of every listing (archive names)
#
# Names are also split into their words, and words common enough to be
# harmless on their own are dropped (see STOP and DICT below). Files are searched
# case-insensitively (see the matching rule at the end). Upstream licence texts under
# licenses/ and vendored code under third_party/ are skipped; licenses/SOURCES.in
# is ours and is searched.
#
# With no local data there is nothing to look for, and the check passes with a
# note. `make check-generic` runs this; CI does not, having no local data.
#
#   scripts/check-generic.sh          hits, one per line, exit 1 if any
#   scripts/check-generic.sh --terms  print the term list and exit
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"

TERMS="$(python3 - <<'PY'
import os, re, sys, tomllib

# Words too common to mean anything alone. A term made of several words is
# kept whole even when its parts are here. Single words split out of a longer
# name are kept only when they are not ordinary English (the system word list,
# when there is one), so a title's distinctive words are caught and its
# everyday words are not.
STOP = set("""
setup install installer autorun disc disk discs cd dvd iso zip exe bin cue
data core game games edition usa europe asia alt uk english eng
gold platinum classic deluxe complete collection trilogy rerelease release
version patch update demo full retail original special anthology pack
windows win31 win95 win98 winxp win7 window pc dos audio music video
cinematics movies bonus extra extras manual readme docs
""".split())

DICT = set()
for d in ("/usr/share/dict/words", "/usr/dict/words"):
    if os.path.isfile(d):
        DICT = {w.strip().lower() for w in open(d, encoding="utf-8", errors="replace")}
        break

terms = set()

def stem(t):
    return re.sub(r"\.[A-Za-z0-9]{1,4}$", "", t)

def add_stem(t):
    """A name without its extension, unless that leaves an everyday word."""
    s = stem(t)
    if s != t and (len(s) < 4 or s.lower() in DICT):
        return
    add(s)

def add(t):
    t = t.strip()
    if (len(t) >= 3 and not t.isdigit() and t.lower() not in STOP
            and stem(t).lower() not in STOP):
        terms.add(t)

def words(s):
    """The distinctive words of a name: CamelCase and punctuation split."""
    for w in re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+", s):
        if len(w) >= 4 and w.lower() not in DICT:
            add(w)

def name(s):
    """A file or disc name: whole, without its extension, and its words."""
    s = s.strip()
    if not s:
        return
    add(s)
    add_stem(s)
    words(stem(s))

def path(s):
    for part in re.split(r"[/\\]", s):
        add(part)
        add_stem(part)

def value(v):
    if isinstance(v, str):
        yield v
    elif isinstance(v, list):
        for x in v:
            yield from value(x)
    elif isinstance(v, dict):
        for x in v.values():
            yield from value(x)

# Manifests.
if os.path.isdir("games"):
    for f in sorted(os.listdir("games")):
        if not f.endswith(".toml"):
            continue
        add(f[:-5])
        try:
            with open(os.path.join("games", f), "rb") as fh:
                m = tomllib.load(fh)
        except Exception as e:
            print(f"check-generic: cannot read games/{f}: {e}", file=sys.stderr)
            continue
        if "id" in m:
            add(m["id"])
            words(m["id"].replace("-", " "))
        if "name" in m:
            add(m["name"])
            for part in re.split(r"\s*[:]\s*|\s+-\s+", m["name"]):
                add(part)
            words(m["name"])
        for key, v in m.get("source", {}).items():
            if key == "method":
                continue
            for s in value(v):
                file, _, label = s.partition("#")
                name(os.path.basename(file)) if "/" not in file else path(file)
                add(label)
        run = m.get("run", {})
        for s in value(run.get("exe", "")):
            path(s)
        for tok in str(run.get("args", "")).split():
            if not tok.startswith(("-", "+")) and not tok.isdigit():
                add(tok)

# Disc fingerprints: every field of every line.
if os.path.isfile("db/discs.txt"):
    for line in open("db/discs.txt", encoding="utf-8", errors="replace"):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        for i, field in enumerate(f):
            if field.isdigit():
                if len(field) >= 7:
                    terms.add(field)
            elif i == len(f) - 1:
                name(field)
            else:
                add(field)

# Images and archives.
if os.path.isdir("iso"):
    for f in os.listdir("iso"):
        name(f)

# Local test expectations: the first column of each listing.
if os.path.isdir("tests/local"):
    for dp, _, fs in os.walk("tests/local"):
        for f in fs:
            try:
                for line in open(os.path.join(dp, f), encoding="utf-8", errors="replace"):
                    if line.startswith("#") or not line.strip():
                        continue
                    name(line.split("\t")[0].rstrip("\n"))
            except OSError:
                pass

for t in sorted(terms, key=str.lower):
    print(t)
PY
)" || exit 2

if [ "${1:-}" = "--terms" ]; then printf '%s\n' "$TERMS"; exit 0; fi
if [ -z "$TERMS" ]; then
  echo "check-generic: no local collection data (games/, db/, iso/, tests/local/); nothing to check"
  exit 0
fi

# Everything git tracks or would track, minus upstream licence texts and
# vendored code.
mapfile -d '' FILES < <(git ls-files -z --cached --others --exclude-standard |
  while IFS= read -r -d '' f; do
    case "$f" in
      third_party/*) continue ;;
      licenses/SOURCES.in) ;;
      licenses/*) continue ;;
    esac
    [ -f "$f" ] && printf '%s\0' "$f"
  done)

# A term matches as a whole word, case-insensitively; one of six characters or
# more also matches inside a longer word, so a name run together with another
# ("NameGold.zip") is found too. A C escape before a term ("\tNAME") counts as
# a word boundary.
HITS="$(printf '%s\0' "${FILES[@]}" | TERMS="$TERMS" python3 -c '
import os, re, sys
terms = [t for t in os.environ["TERMS"].split("\n") if t]
short = [re.escape(t) for t in terms if len(t) < 6]
long_ = [re.escape(t) for t in terms if len(t) >= 6]
parts = []
if long_:
    parts.append("(?:" + "|".join(sorted(long_, key=len, reverse=True)) + ")")
if short:
    parts.append(r"(?:(?<![A-Za-z0-9_])|(?<=\\[ntr]))(?:" + "|".join(sorted(short, key=len, reverse=True)) + r")(?![A-Za-z0-9_])")
rx = re.compile("|".join(parts), re.I)
for f in sys.stdin.read().split("\0"):
    if not f:
        continue
    try:
        data = open(f, "rb").read()
    except OSError:
        continue
    if b"\0" in data:
        continue
    for n, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
        m = rx.search(line)
        if m:
            print(f"{f}:{n}: [{m.group(0)}] {line.strip()[:160]}")
')"
if [ -n "$HITS" ]; then
  printf '%s\n' "$HITS"
  printf 'check-generic: %d line(s) name the local collection\n' "$(printf '%s\n' "$HITS" | wc -l)"
  exit 1
fi
printf 'check-generic: %d files clean against %d local terms\n' "${#FILES[@]}" "$(printf '%s\n' "$TERMS" | wc -l)"
