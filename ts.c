/*
 * vis-treesitter: minimal Lua C binding for libtree-sitter syntax highlighting.
 * Supports Lua 5.1 - 5.4.
 */

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <tree_sitter/api.h>

#define MT_LANG   "vis_ts.language"
#define MT_PARSER "vis_ts.parser"
#define MT_TREE   "vis_ts.tree"
#define MT_QUERY  "vis_ts.query"

#if LUA_VERSION_NUM < 502
static void set_funcs(lua_State *L, const luaL_Reg *l) {
	luaL_register(L, NULL, l);
}
#else
static void set_funcs(lua_State *L, const luaL_Reg *l) {
	luaL_setfuncs(L, l, 0);
}
#endif

typedef const TSLanguage *(*ts_lang_fn)(void);

typedef struct {
	const TSLanguage *lang;
	void *handle;
} LangUD;

typedef struct {
	TSParser *parser;
} ParserUD;

typedef struct {
	TSTree *tree;
} TreeUD;

typedef struct {
	TSQuery *query;
	TSQueryCursor *cursor;
} QueryUD;

typedef struct {
	uint32_t start;
	uint32_t end;
	uint32_t capture_id;
	uint32_t pattern_index;
	int priority;
} CaptureSpan;

/* ---- Language ---- */

static int l_load_language(lua_State *L) {
	const char *path = luaL_checkstring(L, 1);
	const char *symbol = luaL_checkstring(L, 2);

	void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
	if (!handle) {
		lua_pushnil(L);
		lua_pushstring(L, dlerror());
		return 2;
	}

	dlerror();
	ts_lang_fn fn = (ts_lang_fn)(uintptr_t)dlsym(handle, symbol);
	const char *err = dlerror();
	if (err || !fn) {
		dlclose(handle);
		lua_pushnil(L);
		lua_pushstring(L, err ? err : "symbol not found");
		return 2;
	}

	const TSLanguage *lang = fn();
	if (!lang) {
		dlclose(handle);
		lua_pushnil(L);
		lua_pushliteral(L, "language function returned NULL");
		return 2;
	}

	LangUD *ud = (LangUD *)lua_newuserdata(L, sizeof(LangUD));
	ud->lang = lang;
	ud->handle = handle;
	luaL_getmetatable(L, MT_LANG);
	lua_setmetatable(L, -2);
	return 1;
}

/* ---- Parser ---- */

static int l_new_parser(lua_State *L) {
	LangUD *lang_ud = (LangUD *)luaL_checkudata(L, 1, MT_LANG);
	TSParser *parser = ts_parser_new();
	if (!parser) {
		lua_pushnil(L);
		lua_pushliteral(L, "ts_parser_new failed");
		return 2;
	}
	if (!ts_parser_set_language(parser, lang_ud->lang)) {
		ts_parser_delete(parser);
		lua_pushnil(L);
		lua_pushliteral(L, "ts_parser_set_language failed (ABI version mismatch)");
		return 2;
	}
	ParserUD *ud = (ParserUD *)lua_newuserdata(L, sizeof(ParserUD));
	ud->parser = parser;
	luaL_getmetatable(L, MT_PARSER);
	lua_setmetatable(L, -2);
	return 1;
}

static int parser_parse(lua_State *L) {
	ParserUD *ud = (ParserUD *)luaL_checkudata(L, 1, MT_PARSER);
	size_t len = 0;
	const char *src = luaL_checklstring(L, 2, &len);
	TSTree *tree = ts_parser_parse_string(ud->parser, NULL, src, (uint32_t)len);
	if (!tree) {
		lua_pushnil(L);
		return 1;
	}
	TreeUD *tud = (TreeUD *)lua_newuserdata(L, sizeof(TreeUD));
	tud->tree = tree;
	luaL_getmetatable(L, MT_TREE);
	lua_setmetatable(L, -2);
	return 1;
}

static int parser_gc(lua_State *L) {
	ParserUD *ud = (ParserUD *)luaL_checkudata(L, 1, MT_PARSER);
	if (ud->parser) {
		ts_parser_delete(ud->parser);
		ud->parser = NULL;
	}
	return 0;
}

/* ---- Tree ---- */

static int tree_gc(lua_State *L) {
	TreeUD *ud = (TreeUD *)luaL_checkudata(L, 1, MT_TREE);
	if (ud->tree) {
		ts_tree_delete(ud->tree);
		ud->tree = NULL;
	}
	return 0;
}

/* ---- Query & predicate evaluation ---- */

static int l_new_query(lua_State *L) {
	LangUD *lang_ud = (LangUD *)luaL_checkudata(L, 1, MT_LANG);
	size_t len = 0;
	const char *src = luaL_checklstring(L, 2, &len);
	uint32_t err_offset = 0;
	TSQueryError err_type = TSQueryErrorNone;

	TSQuery *query = ts_query_new(lang_ud->lang, src, (uint32_t)len, &err_offset, &err_type);
	if (!query) {
		lua_pushnil(L);
		lua_pushfstring(L, "query error %d at byte %d", (int)err_type, (int)err_offset);
		lua_pushinteger(L, (lua_Integer)err_offset);
		return 3;
	}

	TSQueryCursor *cursor = ts_query_cursor_new();
	if (!cursor) {
		ts_query_delete(query);
		lua_pushnil(L);
		lua_pushliteral(L, "ts_query_cursor_new failed");
		return 2;
	}

	QueryUD *ud = (QueryUD *)lua_newuserdata(L, sizeof(QueryUD));
	ud->query = query;
	ud->cursor = cursor;
	luaL_getmetatable(L, MT_QUERY);
	lua_setmetatable(L, -2);
	return 1;
}

static int query_gc(lua_State *L) {
	QueryUD *ud = (QueryUD *)luaL_checkudata(L, 1, MT_QUERY);
	if (ud->cursor) {
		ts_query_cursor_delete(ud->cursor);
		ud->cursor = NULL;
	}
	if (ud->query) {
		ts_query_delete(ud->query);
		ud->query = NULL;
	}
	return 0;
}

static bool find_capture_node(const TSQueryMatch *match, uint32_t capture_id, TSNode *out) {
	for (uint16_t i = 0; i < match->capture_count; i++) {
		if (match->captures[i].index == capture_id) {
			*out = match->captures[i].node;
			return true;
		}
	}
	return false;
}

static bool node_text_eq(TSNode node, const char *src, size_t src_len, const char *val, uint32_t val_len) {
	uint32_t s = ts_node_start_byte(node);
	uint32_t e = ts_node_end_byte(node);
	if (e > src_len || e < s) return false;
	return (e - s) == val_len && memcmp(src + s, val, val_len) == 0;
}

static bool node_text_eq_node(TSNode a, TSNode b, const char *src, size_t src_len) {
	uint32_t sa = ts_node_start_byte(a), ea = ts_node_end_byte(a);
	uint32_t sb = ts_node_start_byte(b), eb = ts_node_end_byte(b);
	if (ea > src_len || eb > src_len || ea < sa || eb < sb) return false;
	return (ea - sa) == (eb - sb) && memcmp(src + sa, src + sb, ea - sa) == 0;
}

static bool lua_pattern_match(lua_State *L, int find_idx, const char *text, size_t text_len, const char *pat, size_t pat_len) {
	lua_pushvalue(L, find_idx);
	lua_pushlstring(L, text, text_len);
	lua_pushlstring(L, pat, pat_len);
	if (lua_pcall(L, 2, 1, 0) != 0) {
		lua_pop(L, 1);
		return false;
	}
	bool matched = !lua_isnil(L, -1);
	lua_pop(L, 1);
	return matched;
}

static bool posix_regex_match(const char *text, size_t text_len, const char *pat, size_t pat_len) {
	char *pat_z = (char *)malloc(pat_len + 1);
	char *txt_z = (char *)malloc(text_len + 1);
	if (!pat_z || !txt_z) {
		free(pat_z);
		free(txt_z);
		return false;
	}
	memcpy(pat_z, pat, pat_len);
	pat_z[pat_len] = '\0';
	memcpy(txt_z, text, text_len);
	txt_z[text_len] = '\0';

	regex_t re;
	bool matched = false;
	if (regcomp(&re, pat_z, REG_EXTENDED | REG_NOSUB) == 0) {
		matched = (regexec(&re, txt_z, 0, NULL, 0) == 0);
		regfree(&re);
	}
	free(pat_z);
	free(txt_z);
	return matched;
}

static bool eval_predicates(
	lua_State *L,
	int find_idx,
	const TSQuery *query,
	const TSQueryMatch *match,
	const char *src,
	size_t src_len,
	int *priority_out
) {
	uint32_t step_count = 0;
	const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(query, match->pattern_index, &step_count);
	if (!steps || step_count == 0) return true;

	uint32_t i = 0;
	while (i < step_count) {
		uint32_t start = i;
		while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) {
			i++;
		}
		uint32_t count = i - start;
		if (i < step_count) i++; /* skip Done */

		if (count == 0 || steps[start].type != TSQueryPredicateStepTypeString) continue;

		uint32_t op_len = 0;
		const char *op = ts_query_string_value_for_id(query, steps[start].value_id, &op_len);

		/* #set! priority N */
		if (op_len == 4 && memcmp(op, "set!", 4) == 0) {
			if (count >= 3 &&
			    steps[start + 1].type == TSQueryPredicateStepTypeString &&
			    steps[start + 2].type == TSQueryPredicateStepTypeString) {
				uint32_t klen = 0, vlen = 0;
				const char *key = ts_query_string_value_for_id(query, steps[start + 1].value_id, &klen);
				const char *val = ts_query_string_value_for_id(query, steps[start + 2].value_id, &vlen);
				if (klen == 8 && memcmp(key, "priority", 8) == 0 && vlen > 0) {
					*priority_out = atoi(val);
				}
			}
			continue;
		}

		bool negated = false;
		const char *base = op;
		uint32_t base_len = op_len;
		if (base_len > 4 && memcmp(base, "not-", 4) == 0) {
			negated = true;
			base += 4;
			base_len -= 4;
		}
		bool any_mode = false;
		if (base_len > 4 && memcmp(base, "any-", 4) == 0) {
			any_mode = true;
			base += 4;
			base_len -= 4;
		}

		/* #eq? / #not-eq? / #any-eq? */
		if (base_len == 3 && memcmp(base, "eq?", 3) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture) {
				uint32_t cid = steps[start + 1].value_id;
				if (steps[start + 2].type == TSQueryPredicateStepTypeString) {
					uint32_t vlen = 0;
					const char *val = ts_query_string_value_for_id(query, steps[start + 2].value_id, &vlen);
					bool saw = false, all_ok = true;
					for (uint16_t c = 0; c < match->capture_count; c++) {
						if (match->captures[c].index == cid) {
							saw = true;
							bool m = node_text_eq(match->captures[c].node, src, src_len, val, vlen);
							if (m) ok = true;
							else all_ok = false;
						}
					}
					if (!any_mode) ok = saw && all_ok;
				} else if (steps[start + 2].type == TSQueryPredicateStepTypeCapture) {
					TSNode a, b;
					if (find_capture_node(match, cid, &a) &&
					    find_capture_node(match, steps[start + 2].value_id, &b)) {
						ok = node_text_eq_node(a, b, src, src_len);
					}
				}
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* #any-of? / #not-any-of? (note: "any-" was stripped above -> base == "of?") */
		if (any_mode && base_len == 3 && memcmp(base, "of?", 3) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture) {
				TSNode node;
				if (find_capture_node(match, steps[start + 1].value_id, &node)) {
					for (uint32_t a = 2; a < count; a++) {
						if (steps[start + a].type == TSQueryPredicateStepTypeString) {
							uint32_t vlen = 0;
							const char *val = ts_query_string_value_for_id(query, steps[start + a].value_id, &vlen);
							if (node_text_eq(node, src, src_len, val, vlen)) {
								ok = true;
								break;
							}
						}
					}
				}
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* #lua-match? / #not-lua-match? / #any-lua-match? */
		if (base_len == 10 && memcmp(base, "lua-match?", 10) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture &&
			    steps[start + 2].type == TSQueryPredicateStepTypeString) {
				uint32_t cid = steps[start + 1].value_id;
				uint32_t plen = 0;
				const char *pat = ts_query_string_value_for_id(query, steps[start + 2].value_id, &plen);
				bool saw = false, all_ok = true;
				for (uint16_t c = 0; c < match->capture_count; c++) {
					if (match->captures[c].index == cid) {
						saw = true;
						uint32_t s = ts_node_start_byte(match->captures[c].node);
						uint32_t e = ts_node_end_byte(match->captures[c].node);
						bool m = (e <= src_len && e >= s) &&
						         lua_pattern_match(L, find_idx, src + s, e - s, pat, plen);
						if (m) ok = true;
						else all_ok = false;
					}
				}
				if (!any_mode) ok = saw && all_ok;
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* #match? / #not-match? */
		if (base_len == 6 && memcmp(base, "match?", 6) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture &&
			    steps[start + 2].type == TSQueryPredicateStepTypeString) {
				TSNode node;
				if (find_capture_node(match, steps[start + 1].value_id, &node)) {
					uint32_t s = ts_node_start_byte(node);
					uint32_t e = ts_node_end_byte(node);
					uint32_t plen = 0;
					const char *pat = ts_query_string_value_for_id(query, steps[start + 2].value_id, &plen);
					if (e <= src_len && e >= s) {
						ok = posix_regex_match(src + s, e - s, pat, plen);
					}
				}
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* #has-ancestor? / #not-has-ancestor? */
		if (base_len == 13 && memcmp(base, "has-ancestor?", 13) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture) {
				TSNode node;
				if (find_capture_node(match, steps[start + 1].value_id, &node)) {
					TSNode p = ts_node_parent(node);
					while (!ts_node_is_null(p) && !ok) {
						const char *ptype = ts_node_type(p);
						size_t ptlen = strlen(ptype);
						for (uint32_t a = 2; a < count; a++) {
							if (steps[start + a].type == TSQueryPredicateStepTypeString) {
								uint32_t vlen = 0;
								const char *val = ts_query_string_value_for_id(query, steps[start + a].value_id, &vlen);
								if (ptlen == vlen && memcmp(ptype, val, vlen) == 0) {
									ok = true;
									break;
								}
							}
						}
						p = ts_node_parent(p);
					}
				}
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* #has-parent? / #not-has-parent? */
		if (base_len == 11 && memcmp(base, "has-parent?", 11) == 0 && count >= 3) {
			bool ok = false;
			if (steps[start + 1].type == TSQueryPredicateStepTypeCapture) {
				TSNode node;
				if (find_capture_node(match, steps[start + 1].value_id, &node)) {
					TSNode p = ts_node_parent(node);
					if (!ts_node_is_null(p)) {
						const char *ptype = ts_node_type(p);
						size_t ptlen = strlen(ptype);
						for (uint32_t a = 2; a < count; a++) {
							if (steps[start + a].type == TSQueryPredicateStepTypeString) {
								uint32_t vlen = 0;
								const char *val = ts_query_string_value_for_id(query, steps[start + a].value_id, &vlen);
								if (ptlen == vlen && memcmp(ptype, val, vlen) == 0) {
									ok = true;
									break;
								}
							}
						}
					}
				}
			}
			if (negated ? ok : !ok) return false;
			continue;
		}

		/* Unknown directives (#offset!, #gsub!, #is-not?, etc.): pass through */
	}

	return true;
}

static int span_cmp(const void *pa, const void *pb) {
	const CaptureSpan *a = (const CaptureSpan *)pa;
	const CaptureSpan *b = (const CaptureSpan *)pb;
	if (a->priority != b->priority) return (a->priority < b->priority) ? -1 : 1;
	uint32_t len_a = a->end - a->start;
	uint32_t len_b = b->end - b->start;
	if (len_a != len_b) return (len_a > len_b) ? -1 : 1; /* outer spans first */
	if (a->pattern_index != b->pattern_index) return (a->pattern_index < b->pattern_index) ? -1 : 1;
	if (a->start != b->start) return (a->start < b->start) ? -1 : 1;
	return 0;
}

static bool skip_capture_name(const char *name, uint32_t len) {
	if (len == 0 || name[0] == '_') return true;
	if (len == 5 && memcmp(name, "spell", 5) == 0) return true;
	if (len == 7 && memcmp(name, "nospell", 7) == 0) return true;
	if (len == 7 && memcmp(name, "conceal", 7) == 0) return true;
	return false;
}

static int query_captures(lua_State *L) {
	QueryUD *qud = (QueryUD *)luaL_checkudata(L, 1, MT_QUERY);
	TreeUD *tud = (TreeUD *)luaL_checkudata(L, 2, MT_TREE);
	size_t src_len = 0;
	const char *src = luaL_checklstring(L, 3, &src_len);
	uint32_t start_byte = (uint32_t)luaL_optinteger(L, 4, 0);
	uint32_t end_byte = (uint32_t)luaL_optinteger(L, 5, (lua_Integer)src_len);
	if (end_byte > src_len) end_byte = (uint32_t)src_len;

	/* Cache string.find on the stack for #lua-match? */
	lua_getglobal(L, "string");
	lua_getfield(L, -1, "find");
	int find_idx = lua_gettop(L);

	ts_query_cursor_set_byte_range(qud->cursor, start_byte, end_byte);
	ts_query_cursor_exec(qud->cursor, qud->query, ts_tree_root_node(tud->tree));

	size_t cap = 256, len = 0;
	CaptureSpan *spans = (CaptureSpan *)malloc(cap * sizeof(CaptureSpan));
	if (!spans) {
		lua_pop(L, 2);
		return luaL_error(L, "out of memory");
	}

	TSQueryMatch match;
	while (ts_query_cursor_next_match(qud->cursor, &match)) {
		int priority = 100;
		if (!eval_predicates(L, find_idx, qud->query, &match, src, src_len, &priority)) {
			continue;
		}
		for (uint16_t i = 0; i < match.capture_count; i++) {
			TSNode node = match.captures[i].node;
			uint32_t s = ts_node_start_byte(node);
			uint32_t e = ts_node_end_byte(node);
			if (e <= s || e <= start_byte || s >= end_byte) continue;

			uint32_t cid = match.captures[i].index;
			uint32_t nlen = 0;
			const char *cname = ts_query_capture_name_for_id(qud->query, cid, &nlen);
			if (skip_capture_name(cname, nlen)) continue;

			if (len == cap) {
				cap *= 2;
				CaptureSpan *tmp = (CaptureSpan *)realloc(spans, cap * sizeof(CaptureSpan));
				if (!tmp) {
					free(spans);
					lua_pop(L, 2);
					return luaL_error(L, "out of memory");
				}
				spans = tmp;
			}
			spans[len].start = s;
			spans[len].end = e;
			spans[len].capture_id = cid;
			spans[len].pattern_index = match.pattern_index;
			spans[len].priority = priority;
			len++;
		}
	}

	if (len > 1) {
		qsort(spans, len, sizeof(CaptureSpan), span_cmp);
	}

	/* Build capture name lookup table (1-based) to avoid re-interning strings */
	uint32_t cap_count = ts_query_capture_count(qud->query);
	lua_createtable(L, (int)cap_count, 0);
	int names_idx = lua_gettop(L);
	for (uint32_t cid = 0; cid < cap_count; cid++) {
		uint32_t nlen = 0;
		const char *cname = ts_query_capture_name_for_id(qud->query, cid, &nlen);
		lua_pushlstring(L, cname, nlen);
		lua_rawseti(L, names_idx, (int)(cid + 1));
	}

	/* Flat result: { name1, start1, end1, ... } */
	lua_createtable(L, (int)(len * 3), 0);
	int out_idx = lua_gettop(L);
	for (size_t i = 0; i < len; i++) {
		int base = (int)(i * 3);
		lua_rawgeti(L, names_idx, (int)(spans[i].capture_id + 1));
		lua_rawseti(L, out_idx, base + 1);
		lua_pushinteger(L, (lua_Integer)spans[i].start);
		lua_rawseti(L, out_idx, base + 2);
		lua_pushinteger(L, (lua_Integer)spans[i].end);
		lua_rawseti(L, out_idx, base + 3);
	}

	free(spans);
	lua_remove(L, names_idx);
	lua_remove(L, find_idx);
	lua_remove(L, find_idx - 1); /* string table */
	return 1;
}

/* ---- Module registration ---- */

static const luaL_Reg parser_methods[] = {
	{"parse", parser_parse},
	{"__gc", parser_gc},
	{NULL, NULL},
};

static const luaL_Reg tree_methods[] = {
	{"__gc", tree_gc},
	{NULL, NULL},
};

static const luaL_Reg query_methods[] = {
	{"captures", query_captures},
	{"__gc", query_gc},
	{NULL, NULL},
};

static const luaL_Reg module_funcs[] = {
	{"load_language", l_load_language},
	{"new_parser", l_new_parser},
	{"new_query", l_new_query},
	{NULL, NULL},
};

static void register_mt(lua_State *L, const char *name, const luaL_Reg *methods) {
	luaL_newmetatable(L, name);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	set_funcs(L, methods);
	lua_pop(L, 1);
}

int luaopen_ts(lua_State *L) {
	luaL_newmetatable(L, MT_LANG);
	lua_pop(L, 1);
	register_mt(L, MT_PARSER, parser_methods);
	register_mt(L, MT_TREE, tree_methods);
	register_mt(L, MT_QUERY, query_methods);

	lua_newtable(L);
	set_funcs(L, module_funcs);
	return 1;
}
