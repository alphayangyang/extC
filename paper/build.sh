#!/usr/bin/env bash
# Build the paper.  One source (main.tex + chapters/), three outputs:
#
#   ./build.sh              anonymous SIGPLAN submission  -> main.pdf      (26 pp, body 21 pp)
#   ./build.sh journal      anonymous journal submission  -> main-journal.pdf (ACM Small format)
#   ./build.sh full         named camera-ready / arXiv    -> main-full.pdf   (reads authors.tex)
#   ./build.sh artifact     ACM artifact appendix         -> artifact-appendix.pdf
#   ./build.sh all          the four above
#   ./build.sh clean        remove every generated file (keeps .tex/.tsv/.pdf sources)
#
# Requirements: pdflatex, bibtex (TeX Live), python3 + the python packages only for the
# figures/tables (matplotlib, see paper/figures/*.py).  Nothing else is needed to typeset.
set -euo pipefail
cd "$(dirname "$0")"

# This machine carries two TeX installations: ~/.local/bin (TeX Live 2026) is first in
# PATH but does not ship acmart.cls, /usr/bin (TeX Live 2025/Debian) does.  Pick the
# first pdflatex whose own kpsewhich can find acmart.cls, so the script works either way.
pick_tex() { # $1 = binary name, $2 = file it must be able to find
  for cand in "${!1:-}" "/usr/bin/$1" "$(command -v $1 || true)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    dir=$(dirname "$cand")
    found=$("$dir/kpsewhich" "$2" 2>/dev/null || true)
    if [ -n "$found" ]; then echo "$cand"; return 0; fi
  done
  return 1
}
LATEX=$(pick_tex pdflatex acmart.cls) || { echo "FAIL: no pdflatex that can find acmart.cls; install TeX Live with acmart" >&2; exit 1; }
BIBTEX=${BIBTEX:-$(dirname "$LATEX")/bibtex}
[ -x "$BIBTEX" ] || BIBTEX=$(command -v bibtex)
FLAGS="-interaction=nonstopmode -halt-on-error"

texonce() { # $1 = jobname ($1.tex must exist)
  "$LATEX" $FLAGS "$1.tex" > "$1.pass.log" 2>&1 || { echo "FAIL: $1 (see $1.pass.log)"; exit 1; }
}

build_job() { # $1 = jobname
  rm -f "$1.aux" "$1.bbl" "$1.blg" "$1.out"
  texonce "$1"
  if grep -q "\\citation" "$1.aux"; then "$BIBTEX" "$1" > "$1.bib.log" 2>&1 || true; fi
  texonce "$1"; texonce "$1"
  pages=$(grep -o "Output written on [^ ]* ([0-9]* pages" "$1.pass.log" | grep -o "[0-9]* pages" | head -1)
  warn=$(grep -c "^LaTeX Warning" "$1.pass.log" || true)
  err=$(grep -c "^!" "$1.pass.log" || true)
  printf "%-22s %-10s warnings=%s errors=%s\n" "$1" "${pages:-?}" "$warn" "$err"
  rm -f "$1.pass.log" "$1.bib.log"
}

mode=${1:-sigplan}

case "$mode" in
  sigplan)
    build_job main
    ;;
  journal)
    build_job main-journal
    ;;
  full)
    build_job main-full
    ;;
  artifact)
    build_job artifact-appendix
    ;;
  all)
    "$0" sigplan && "$0" artifact && "$0" journal && { [ -f authors.tex ] && "$0" full || echo "skip full: authors.tex missing"; }
    ;;
  clean)
    rm -f ./*.aux ./*.bbl ./*.blg ./*.out ./*.log ./*.pass.log ./*.bib.log
    rm -f main-journal.pdf main-full.pdf
    rm -rf figures/__pycache__
    echo "cleaned (kept main.pdf, artifact-appendix.pdf, sources, data, figures)"
    ;;
  *)
    echo "usage: $0 [sigplan|journal|full|artifact|all|clean]" >&2; exit 2
    ;;
esac