# vis-treesitter

tree-sitter syntax highlighting for [vis](https://github.com/martanne/vis). loads compiled `.so` parsers and `highlights.scm` queries, and falls back to vis's lpeg lexer for any language without them

### install

needs vis `0.9` or later, a C compiler, `libtree-sitter` and the Lua headers matching your vis build

```sh
git clone https://github.com/w0x7y/vis-treesitter ~/.config/vis/plugins/vis-treesitter
make -C ~/.config/vis/plugins/vis-treesitter
```

then in `visrc.lua`:

```lua
require("plugins/vis-treesitter")
```

### parsers and queries

parsers (`<lang>.so` or `libtree-sitter-<lang>.so`) and queries (`<lang>/highlights.scm`) are looked up in:

~ `$XDG_DATA_HOME/vis/treesitter/` and `$XDG_CONFIG_HOME/vis/treesitter/`\
~ `$XDG_DATA_HOME/nvim/site/` and `$XDG_DATA_HOME/nvim/lazy/nvim-treesitter/` (reuses neovim's installed parsers and queries automatically)\
~ `/usr/lib/tree-sitter` and `/usr/share/tree-sitter/queries`

`; inherits: ...` headers and neovim query predicates (`#eq?`, `#any-of?`, `#lua-match?`, `#match?`, `#has-ancestor?`, `#has-parent?`, `#set! priority`) are supported

### config

```lua
local ts = require("plugins/vis-treesitter")
ts.max_size = 512 * 1024 -- fall back to lpeg on bigger files
table.insert(ts.parser_dirs, 1, "/custom/parsers")
table.insert(ts.query_dirs, 1, "/custom/queries")
ts.syntax_map["my_syntax"] = "c"
ts.styles["type.builtin"] = "fore:cyan"
```

every capture falls back to `vis.lexers.STYLE_*` so any existing vis theme works out of the box. themes that also define finer tags (`STYLE_TYPE_BUILTIN`, `STYLE_KEYWORD_CONDITIONAL`, `STYLE_KEYWORD_REPEAT`, `STYLE_KEYWORD_RETURN`, `STYLE_PUNCTUATION`, etc.) are picked up automatically

### layout

```text
ts.c       minimal lua C binding for libtree-sitter (lua 5.1 - 5.4)
Makefile   builds ts.so
init.lua   vis plugin and query loader
LICENSE    mit
```

_license: [mit](./LICENSE)_
