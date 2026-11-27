-- vis-treesitter: tree-sitter syntax highlighting for vis.
-- Reuses compiled tree-sitter .so parsers and highlights.scm queries (including
-- Neovim's if installed) and falls back to vis's LPeg lexer for any language
-- without a parser or query.

local source_dir = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"

-- Load the compiled ts.so next to this file without touching package.cpath
local loader = package.loadlib(source_dir .. "ts.so", "luaopen_ts")
local ts = loader and loader()
if not ts then
  return { enabled = false, error = "vis-treesitter: ts.so not built (run make in " .. source_dir .. ")" }
end

local home = os.getenv("HOME") or ""
local xdg_data = os.getenv("XDG_DATA_HOME") or (home .. "/.local/share")
local xdg_config = os.getenv("XDG_CONFIG_HOME") or (home .. "/.config")

local M = {
  enabled = true,
  max_size = 512 * 1024, -- fall back to LPeg on very large files

  -- Directories searched for <lang>.so (or libtree-sitter-<lang>.so)
  parser_dirs = {
    xdg_data .. "/vis/treesitter/parser",
    xdg_config .. "/vis/treesitter/parser",
    xdg_data .. "/nvim/site/parser",
    xdg_data .. "/nvim/lazy/nvim-treesitter/parser",
    "/usr/lib/tree-sitter",
    "/usr/local/lib/tree-sitter",
  },

  -- Directories searched for <lang>/highlights.scm
  query_dirs = {
    xdg_data .. "/vis/treesitter/queries",
    xdg_config .. "/vis/treesitter/queries",
    xdg_data .. "/nvim/site/queries",
    xdg_config .. "/nvim/queries",
    xdg_data .. "/nvim/lazy/nvim-treesitter/runtime/queries",
    "/usr/share/nvim/runtime/queries",
    "/usr/share/tree-sitter/queries",
    "/usr/local/share/tree-sitter/queries",
  },

  -- Map vis syntax names to tree-sitter language names
  syntax_map = {
    ansi_c = "c",
    csharp = "c_sharp",
    ["git-commit"] = "gitcommit",
    shell = "bash",
  },

  -- Optional per-extension overrides (e.g. vis uses "typescript" for .tsx)
  ext_map = {
    tsx = "tsx",
    jsx = "javascript",
  },

  -- Optional capture -> vis style string overrides (e.g. M.styles["type.builtin"] = "fore:cyan")
  -- Checked before vis.lexers.STYLE_* so themes or visrc.lua can customize any capture.
  styles = {},
}

-- Self-contained style compatibility (vis 0.9 vs later vis.ui)
local ui = vis.ui
local STYLE_ID = ui.style_push and ui:style_push() or 55

local function define_style(win, id, spec)
  if ui.style_define then
    return ui:style_define(id, spec)
  end
  return win:style_define(id, spec)
end

-- Standard vis.lexers STYLE_* fallbacks for tree-sitter captures so any vis theme works
local CAPTURE_FALLBACKS = {
  ["comment"] = "STYLE_COMMENT",
  ["string"] = "STYLE_STRING",
  ["string.regexp"] = "STYLE_REGEX",
  ["string.escape"] = "STYLE_EMBEDDED",
  ["string.special"] = "STYLE_EMBEDDED",
  ["string.special.symbol"] = "STYLE_STRING_SYMBOL",
  ["character"] = "STYLE_STRING",
  ["character.special"] = "STYLE_EMBEDDED",
  ["number"] = "STYLE_NUMBER",
  ["boolean"] = "STYLE_CONSTANT",
  ["constant"] = "STYLE_CONSTANT",
  ["constant.builtin"] = "STYLE_CONSTANT_BUILTIN",
  ["variable"] = "STYLE_VARIABLE",
  ["variable.builtin"] = "STYLE_VARIABLE_BUILTIN",
  ["property"] = "STYLE_PROPERTY",
  ["function"] = "STYLE_FUNCTION",
  ["function.builtin"] = "STYLE_FUNCTION_BUILTIN",
  ["function.macro"] = "STYLE_FUNCTION_BUILTIN",
  ["constructor"] = "STYLE_TYPE",
  ["type"] = "STYLE_TYPE",
  ["type.builtin"] = "STYLE_TYPE",
  ["module"] = "STYLE_TYPE",
  ["attribute"] = "STYLE_ANNOTATION",
  ["label"] = "STYLE_LABEL",
  ["lifetime"] = "STYLE_CONSTANT",
  ["keyword"] = "STYLE_KEYWORD",
  ["keyword.directive"] = "STYLE_PREPROCESSOR",
  ["operator"] = "STYLE_OPERATOR",
  ["punctuation"] = "STYLE_OPERATOR",
  ["punctuation.special"] = "STYLE_EMBEDDED",
  ["tag"] = "STYLE_TAG",
  ["tag.attribute"] = "STYLE_ATTRIBUTE",
  ["tag.delimiter"] = "STYLE_TAG_CHARS",
  ["markup.heading"] = "STYLE_HEADING",
  ["markup.strong"] = "STYLE_BOLD",
  ["markup.italic"] = "STYLE_ITALIC",
  ["markup.raw"] = "STYLE_CODE",
  ["markup.link.url"] = "STYLE_LINK",
  ["markup.link.label"] = "STYLE_REFERENCE",
  ["markup.list"] = "STYLE_LIST",
  ["diff.plus"] = "STYLE_ADDITION",
  ["diff.minus"] = "STYLE_DELETION",
  ["diff.delta"] = "STYLE_CHANGE",
}

local function file_readable(path)
  local f = io.open(path, "r")
  if f then
    f:close()
    return true
  end
  return false
end

local function read_file(path)
  local f = io.open(path, "r")
  if not f then
    return nil
  end
  local data = f:read("a")
  f:close()
  return data
end

local function find_parser(lang)
  local names = { lang .. ".so", "libtree-sitter-" .. lang .. ".so", lang .. ".dylib" }
  for _, dir in ipairs(M.parser_dirs) do
    for _, name in ipairs(names) do
      local path = dir .. "/" .. name
      if file_readable(path) then
        return path
      end
    end
  end
end

local function find_query_file(lang)
  for _, dir in ipairs(M.query_dirs) do
    local path = dir .. "/" .. lang .. "/highlights.scm"
    if file_readable(path) then
      return path
    end
  end
end

-- Recursively resolves `; inherits: a,b` headers at the top of highlights.scm
local function load_query_source(lang, seen)
  seen = seen or {}
  if seen[lang] then
    return ""
  end
  seen[lang] = true

  local path = find_query_file(lang)
  if not path then
    return nil
  end
  local src = read_file(path)
  if not src then
    return nil
  end

  local inherits = src:match("^;%s*inherits:%s*([^\n]+)")
  if not inherits then
    return src
  end

  local parts = {}
  for parent in inherits:gmatch("[^,%s]+") do
    local optional = parent:match("^%((.+)%)$")
    local name = optional or parent
    local parent_src = load_query_source(name, seen)
    if parent_src then
      parts[#parts + 1] = parent_src
    elseif not optional then
      return nil
    end
  end
  parts[#parts + 1] = src
  return table.concat(parts, "\n")
end

-- Cache of lang -> { parser, query } or false (tried and unavailable)
local langs = {}

local function get_lang(lang_name)
  local entry = langs[lang_name]
  if entry ~= nil then
    return entry or nil
  end

  local so = find_parser(lang_name)
  local qsrc = so and load_query_source(lang_name)
  if not so or not qsrc then
    langs[lang_name] = false
    return nil
  end

  local sym = "tree_sitter_" .. lang_name:gsub("-", "_")
  local lang = ts.load_language(so, sym)
  if not lang then
    langs[lang_name] = false
    return nil
  end

  local parser = ts.new_parser(lang)
  local query = parser and ts.new_query(lang, qsrc)
  if not parser or not query then
    langs[lang_name] = false
    return nil
  end

  entry = { parser = parser, query = query }
  langs[lang_name] = entry
  return entry
end

local function resolve_lang_name(win)
  local path = win.file and win.file.path
  if path then
    local ext = path:match("%.([^./]+)$")
    if ext and M.ext_map[ext] then
      return M.ext_map[ext]
    end
  end
  local syntax = win.syntax
  if not syntax then
    return nil
  end
  return M.syntax_map[syntax] or syntax
end

-- Resolve a tree-sitter capture name (e.g. "function.method.call") to a vis style string
local function resolve_style(capture)
  if capture == "none" then
    return vis.lexers.STYLE_DEFAULT or ""
  end

  -- 1. Explicit overrides in M.styles (most specific prefix first)
  local name = capture
  while name do
    local s = M.styles[name]
    if s ~= nil then
      return s or nil
    end
    name = name:match("^(.+)%.[^.]+$")
  end

  -- 2. Direct theme tag vis.lexers.STYLE_<CAPTURE> (e.g. STYLE_TYPE_BUILTIN, STYLE_KEYWORD_RETURN)
  -- Check rawget first so a theme's __index parent-fallback metatable doesn't shadow our fallback table
  local lexers = vis.lexers
  if type(lexers) == "table" then
    name = capture
    while name do
      local key = "STYLE_" .. name:upper():gsub("%.", "_")
      local s = rawget(lexers, key)
      if type(s) == "string" and s ~= "" then
        return s
      end
      name = name:match("^(.+)%.[^.]+$")
    end

    -- 3. Standard vis STYLE_* fallback table
    name = capture
    while name do
      local fallback_key = CAPTURE_FALLBACKS[name]
      if fallback_key then
        local s = lexers[fallback_key]
        if type(s) == "string" and s ~= "" then
          return s
        end
      end
      name = name:match("^(.+)%.[^.]+$")
    end

    -- 4. Last resort: allow theme's __index metatable
    local key = "STYLE_" .. capture:upper():gsub("%.", "_")
    local s = lexers[key]
    if type(s) == "string" and s ~= "" then
      return s
    end
  end

  return nil
end

-- Per-file parse tree cache: file -> { lang, content, tree }
local trees = setmetatable({}, { __mode = "k" })

local function get_tree(file, lang_name, entry)
  local content = file:content(0, file.size)
  local cached = trees[file]
  if cached and cached.lang == lang_name and cached.content == content then
    return cached.tree, content
  end
  local tree = entry.parser:parse(content)
  if not tree then
    return nil, content
  end
  trees[file] = { lang = lang_name, content = content, tree = tree }
  return tree, content
end

function M.clear_cache()
  langs = {}
  trees = setmetatable({}, { __mode = "k" })
end

function M.has_language(win)
  if not M.enabled then
    return false
  end
  local name = resolve_lang_name(win)
  return name ~= nil and get_lang(name) ~= nil
end

-- Skip vis's built-in LPeg WIN_HIGHLIGHT pass when tree-sitter handles this window,
-- without breaking win:set_syntax or plugins/textobject-lexer.lua
if vis.lexers and type(vis.lexers.load) == "function" then
  local orig_load = vis.lexers.load
  vis.lexers.load = function(name, alt_name, cache)
    if cache and M.enabled and name then
      local caller = debug.getinfo(2, "S")
      if caller and caller.short_src and caller.short_src:match("vis%-std%.lua$") then
        local win = vis.win
        if not win or not win.file or win.file.size <= M.max_size then
          local ts_name = (win and win.syntax == name) and resolve_lang_name(win) or (M.syntax_map[name] or name)
          if ts_name and get_lang(ts_name) then
            return nil
          end
        end
      end
    end
    return orig_load(name, alt_name, cache)
  end
end

vis.events.subscribe(vis.events.FILE_CLOSE, function(file)
  trees[file] = nil
end)

vis.events.subscribe(vis.events.WIN_HIGHLIGHT, function(win)
  if not M.enabled or not win.file or not win.viewport or not win.viewport.bytes then
    return
  end
  if win.file.size == 0 or win.file.size > M.max_size then
    return
  end

  local lang_name = resolve_lang_name(win)
  if not lang_name then
    return
  end
  local entry = get_lang(lang_name)
  if not entry then
    return
  end

  local tree, content = get_tree(win.file, lang_name, entry)
  if not tree then
    return
  end

  local view = win.viewport.bytes
  local vstart, vend = view.start, view.finish
  if vend <= vstart then
    return
  end

  -- Reset viewport to default style so stale styles from LPeg (if any) never bleed through
  win:style(ui.style_ids and ui.style_ids.DEFAULT or win.STYLE_DEFAULT, vstart, vend - 1)

  local captures = entry.query:captures(tree, content, vstart, vend)
  local style_cache = {}
  local last_spec = nil

  for i = 1, #captures, 3 do
    local cname = captures[i]
    local s = captures[i + 1]
    local e = captures[i + 2]

    local spec = style_cache[cname]
    if spec == nil then
      spec = resolve_style(cname) or false
      style_cache[cname] = spec
    end

    if spec then
      if spec ~= last_spec then
        define_style(win, STYLE_ID, spec)
        last_spec = spec
      end
      local a = s < vstart and vstart or s
      local b = (e > vend and vend or e) - 1
      if b >= a then
        win:style(STYLE_ID, a, b)
      end
    end
  end
end)

return M
