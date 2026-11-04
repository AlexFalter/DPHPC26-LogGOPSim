#!/bin/sh
# Run from any directory. Keep generated auxiliaries separate from deliverables.
set -eu
cd "$(dirname "$0")"
if command -v latexmk >/dev/null 2>&1; then
    LATEXMK=latexmk
elif [ -x /Library/TeX/texbin/latexmk ]; then
    PATH="/Library/TeX/texbin:$PATH"
    export PATH
    LATEXMK=latexmk
else
    echo 'latexmk and pdfLaTeX are required (TeX Live / MacTeX).' >&2
    exit 1
fi
mkdir -p build
"$LATEXMK" -pdf -interaction=nonstopmode -halt-on-error -outdir=build project_report.tex
cp build/project_report.pdf project_report.pdf
