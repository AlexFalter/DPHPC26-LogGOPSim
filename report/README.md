# Project report

The canonical source is [../project_report.md](../project_report.md).
Read [project_report.pdf](project_report.pdf) for the compiled version;
`project_report.tex` is generated from the Markdown.

## Build

From any working directory, run the script by its path; from the project root:

```sh
./report/build.sh
```

Requires Python 3 and TeX Live / MacTeX with `latexmk`, `pdflatex`, TikZ, Latin
Modern, `geometry`, `microtype`, `longtable`, `booktabs`, `enumitem`, `listings`,
`graphicx` and `hyperref`. No Python packages or network access are needed.

The script regenerates LaTeX, compiles it in ignored `build/`, and copies the PDF
here. Edit Markdown for content; direct changes to generated `.tex` are
replaced on the next build. Edit `plan-style.sty` and `diagrams/*.tex` for layout.

## Diagrams and conversion

TikZ produces the report's vector diagrams. The fourteen Mermaid blocks
in Markdown correspond, in order, to the TikZ figures listed in `generate.py`.
Edit both representations when changing a diagram; unused files under
`diagrams/` are not rendered.

`generate.py` is a focused, dependency-free converter for this report. It keeps
headings through subsubsections, prose, lists, tables, code and links, and
substitutes the figures in report order. Tables repeat headings across pages; source links resolve relative
to the project root. It checks the diagram count to catch a mismatch between
Markdown and the figure list. Update both representations when changing a diagram.
