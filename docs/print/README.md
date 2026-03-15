# Printable design doc assets

This folder contains pre-rendered Mermaid diagrams as PNGs and the source `.mmd` files, used by `design.md` for printing.

## Generating printable output

From the **docs** directory (parent of `print/`):

```bash
cd docs
pandoc design.md -o design.html --standalone --css=print/design-print.css
```
Opens in any browser. Use **File → Print** (or Ctrl/Cmd+P) to print. Layout is controlled by `print/design-print.css`: edit the `:root` variables at the top to change print margins (`--print-margin-*`), paper size (`--print-paper`), fonts (`--font-sans`, `--font-serif`), font size (`--font-size-body`), and line height (`--line-height`). Requires only [pandoc](https://pandoc.org/).

For **Chinese (简体中文)** use `design-zh.md` instead of `design.md`; the same diagram images are reused. Requires Noto CJK fonts: `sudo apt install fonts-noto-cjk`.

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
