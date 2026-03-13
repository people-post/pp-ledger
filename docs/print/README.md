# Printable design doc assets

This folder contains pre-rendered Mermaid diagrams as PNGs and the source `.mmd` files, used by `design-printable.md` for printing.

## Generating PDF or printable output

From the **docs** directory (parent of `print/`):

**Option A — PDF (investor-friendly layout and fonts)**  
```bash
cd docs
pandoc design-printable.md -o design-printable.pdf \
  --pdf-engine=xelatex \
  --include-in-header=print/latex-header.tex \
  -V mainfont="DejaVu Serif" \
  -V sansfont="DejaVu Sans" \
  -V fontsize=11pt \
  -V papersize=a4
```
Uses serif body text and sans-serif headings, generous margins, 1.25 line spacing, and a clean title page. Requires [pandoc](https://pandoc.org/) and `texlive-xetex` plus `fonts-dejavu` (e.g. `sudo apt install texlive-xetex fonts-dejavu`).

**Chinese (简体中文) PDF**  
```bash
cd docs
pandoc design-printable-zh.md -o design-printable-zh.pdf \
  --pdf-engine=xelatex \
  --include-in-header=print/latex-header.tex \
  --include-in-header=print/latex-header-zh.tex \
  -V mainfont="Noto Serif CJK SC" \
  -V sansfont="Noto Sans CJK SC" \
  -V fontsize=11pt \
  -V papersize=a4
```
Same layout as the English PDF; reuses the same diagram images. Requires Noto CJK fonts: `sudo apt install fonts-noto-cjk`.

**Chinese (简体中文) PDF with pdfLaTeX + ctex**  
```bash
cd docs
pandoc design-printable-zh.md -o design-printable-zh-pdflatex.pdf \
  --pdf-engine=pdflatex \
  --include-in-header=print/latex-header.tex \
  --include-in-header=print/latex-header-zh-ctex.tex
```
This uses the `ctex` package to manage Chinese fonts and line breaking under pdfLaTeX. Requires `texlive-lang-chinese` (or an equivalent TeX Live Chinese language collection).

**Option B — HTML then print to PDF (no extra deps)**  
```bash
cd docs
pandoc design-printable.md -o design-printable.html --standalone --metadata title="Time Chain Design"
```
Open `docs/design-printable.html` in a browser and use **Print → Save as PDF** to get a PDF with all diagrams.

To re-render the diagram images (requires Node and `@mermaid-js/mermaid-cli`):

```bash
cd docs/print
for f in slots-and-rounds block-contents beacon-relays-miners checkpoints reserved-accounts reserved-accounts-zh user-account; do
  npx -y @mermaid-js/mermaid-cli -i "$f.mmd" -o "$f.png" -b transparent
done
```

To regenerate a **single** diagram (for example `reserved-accounts.png`):

```bash
cd docs/print
npx -y @mermaid-js/mermaid-cli -i reserved-accounts.mmd -o reserved-accounts.png -b transparent
# Chinese version
npx -y @mermaid-js/mermaid-cli -i reserved-accounts-zh.mmd -o reserved-accounts-zh.png -b transparent
```
