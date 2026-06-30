# Third-Party Notices

This firmware includes third-party open-source components.

## papyrix-reader — Markdown tokenizer

- **Files:** `lib/Epub/Epub/markdown/md_parser.c`, `lib/Epub/Epub/markdown/md_parser.h`
- **Upstream:** https://github.com/bigbag/papyrix-reader (`lib/Markdown/src/md_parser.{c,h}`)
- **License:** MIT — Copyright (c) 2025 Dave Allie. Full text: `lib/Epub/Epub/markdown/LICENSE.papyrix-MIT`.
- **Usage:** vendored verbatim. It is a standalone, no-AST streaming Markdown tokenizer used by
  PaperBit's native Markdown reader (Route A). PaperBit's own adapter (`MarkdownToBoxParser`) maps its
  tokens into this engine's existing `ParsedText`/`Page` box model; papyrix's own `MarkdownParser`/
  render types are NOT vendored.
