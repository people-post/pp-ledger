# Printable design doc assets

Mermaid sources (`.mmd`), PNGs, and print CSS used by product design docs.
HTML exports live under [`html/`](html/).

## Generating printable output

From the **docs** directory:

```bash
cd docs
pandoc product/DESIGN.md -o print/html/design.html --standalone --css=print/design-print.css
```

Opens in any browser. Use **File → Print** (or Ctrl/Cmd+P). Layout:
`print/design-print.css` (`:root` variables for margins, paper, fonts).

For **Chinese (简体中文)** use `product/DESIGN.zh.md`. Requires Noto CJK:
`sudo apt install fonts-noto-cjk`.

### Encrypted token extension (简体中文)

```bash
cd docs
pandoc product/ENCRYPTED_TOKEN.zh.md -o print/html/encrypted-token-extension-zh.html \
  --standalone \
  --css=print/encrypted-token-extension-print-zh.css \
  -V lang=zh-CN
```

Image paths in product markdown are `../print/…` relative to `product/`.

### Re-render diagrams

```bash
cd docs/print
for f in slots-and-rounds block-contents beacon-relays-miners checkpoints reserved-accounts reserved-accounts-zh user-account; do
  npx -y @mermaid-js/mermaid-cli -i "$f.mmd" -o "$f.png" -b transparent
done
```

See also the documentation map: [../README.md](../README.md).
