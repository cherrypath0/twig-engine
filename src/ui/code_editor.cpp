#include "pch.hpp"
#include "ui/code_editor.hpp"

// Nuklear must always come through the project's config header (never the raw
// <nuklear.h>) so the compile-time flags stay identical across the codebase.
#include "ui/nuklear_config.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// ===========================================================================
//  Colour palette (a calm dark theme)
// ===========================================================================
namespace {

constexpr nk_color col(nk_byte r, nk_byte g, nk_byte b) { return nk_color{r, g, b, 255}; }

const nk_color C_BG       = col(30, 30, 34);   // editor background
const nk_color C_GUTTER   = col(24, 24, 28);   // line-number gutter background
const nk_color C_LINENO   = col(110, 110, 120);// line numbers
const nk_color C_TEXT     = col(220, 220, 225);// default / identifier text
const nk_color C_KEYWORD  = col(198, 120, 221);// keywords (if, for, return…)
const nk_color C_TYPE     = col(86, 182, 194);  // built-in types (int, float…)
const nk_color C_STRING   = col(152, 195, 121); // "strings" and 'chars'
const nk_color C_COMMENT  = col(106, 115, 125); // // and /* */
const nk_color C_NUMBER   = col(209, 154, 102); // numeric literals
const nk_color C_PREPROC  = col(229, 192, 123); // #include / #define
const nk_color C_PUNCT    = col(171, 178, 191); // punctuation / operators
const nk_color C_CURSOR   = col(235, 235, 240);
const nk_color C_CURLINE  = col(40, 42, 48);    // current-line highlight

// ---------------------------------------------------------------------------
//  Tiny token classifier. We re-lex the whole buffer each frame; source files
//  in an in-engine editor are small so this is cheap and keeps state trivial.
// ---------------------------------------------------------------------------
enum class Tok { Text, Keyword, Type, String, Comment, Number, Preproc, Punct };

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_char(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

bool keyword_lookup(const char* s, int n) {
    static const char* kw[] = {
        "alignas","alignof","and","asm","break","case","catch","class","concept",
        "const","consteval","constexpr","constinit","const_cast","continue",
        "co_await","co_return","co_yield","decltype","default","delete","do",
        "dynamic_cast","else","enum","explicit","export","extern","false","for",
        "friend","goto","if","inline","mutable","namespace","new","noexcept",
        "not","nullptr","operator","or","private","protected","public","register",
        "reinterpret_cast","requires","return","sizeof","static","static_assert",
        "static_cast","struct","switch","template","this","thread_local","throw",
        "true","try","typedef","typeid","typename","union","using","virtual",
        "volatile","while","xor",
    };
    for (const char* k : kw)
        if ((int)std::strlen(k) == n && std::strncmp(k, s, (size_t)n) == 0) return true;
    return false;
}

bool type_lookup(const char* s, int n) {
    static const char* ty[] = {
        "auto","bool","char","char8_t","char16_t","char32_t","double","float",
        "int","long","short","signed","unsigned","void","wchar_t","size_t",
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t",
        "uint64_t","string","vector","vec2","vec3","vec4","mat4","quat",
    };
    for (const char* t : ty)
        if ((int)std::strlen(t) == n && std::strncmp(t, s, (size_t)n) == 0) return true;
    return false;
}

// A coloured run of characters on a single (logical) line.
static const char* kCompletions[] = {
    "auto","bool","break","char","const","continue","double","else","enum","extern",
    "false","float","for","if","int","long","namespace","nullptr","return","short",
    "sizeof","static","struct","switch","template","this","true","typename","unsigned",
    "using","virtual","void","while",
    "Entity","Vec3","on_start","on_update","get_position","set_position",
    "get_rotation","set_rotation","log","self","dt"
};
struct FnHdr { const char* name; const char* header; };
static const FnHdr kFuncs[] = {
    {"printf","cstdio"},{"fprintf","cstdio"},{"sprintf","cstdio"},{"snprintf","cstdio"},
    {"scanf","cstdio"},{"puts","cstdio"},{"fopen","cstdio"},{"fclose","cstdio"},
    {"cout","iostream"},{"cin","iostream"},{"cerr","iostream"},{"endl","iostream"},
    {"string","string"},{"to_string","string"},{"stoi","string"},{"stof","string"},{"getline","string"},
    {"vector","vector"},{"array","array"},{"map","map"},{"unordered_map","unordered_map"},
    {"set","set"},{"pair","utility"},{"make_pair","utility"},{"move","utility"},
    {"sort","algorithm"},{"find","algorithm"},{"min","algorithm"},{"max","algorithm"},
    {"clamp","algorithm"},{"swap","algorithm"},{"count","algorithm"},{"reverse","algorithm"},
    {"sqrt","cmath"},{"sin","cmath"},{"cos","cmath"},{"tan","cmath"},{"pow","cmath"},{"floor","cmath"},{"ceil","cmath"},
    {"abs","cstdlib"},{"atof","cstdlib"},{"atoi","cstdlib"},{"malloc","cstdlib"},{"free","cstdlib"},{"rand","cstdlib"},
    {"memcpy","cstring"},{"memset","cstring"},{"strlen","cstring"},{"strcmp","cstring"},{"strcpy","cstring"},
    {"unique_ptr","memory"},{"shared_ptr","memory"},{"make_unique","memory"},{"make_shared","memory"},
    {"function","functional"},{"thread","thread"},{"mutex","mutex"},{"optional","optional"},
};
static bool header_present(const std::string& t, const char* h) {
    return t.find(std::string("<") + h + ">") != std::string::npos
        || t.find(std::string("\"") + h) != std::string::npos;
}
struct Span { int start; int len; Tok tok; };

nk_color tok_color(Tok t) {
    switch (t) {
        case Tok::Keyword: return C_KEYWORD;
        case Tok::Type:    return C_TYPE;
        case Tok::String:  return C_STRING;
        case Tok::Comment: return C_COMMENT;
        case Tok::Number:  return C_NUMBER;
        case Tok::Preproc: return C_PREPROC;
        case Tok::Punct:   return C_PUNCT;
        case Tok::Text:    default: return C_TEXT;
    }
}

// Lex one line ([begin,end) byte offsets within `s`) into coloured spans.
// `in_block` tracks whether we start inside a /* … */ comment and is updated
// to reflect the state at the end of the line.
void lex_line(const std::string& s, int begin, int end, bool& in_block,
              std::vector<Span>& out) {
    out.clear();
    int i = begin;
    while (i < end) {
        char c = s[(size_t)i];

        // Continuation of a multi-line block comment from a previous line.
        if (in_block) {
            int start = i;
            while (i < end) {
                if (s[(size_t)i] == '*' && i + 1 < end && s[(size_t)i + 1] == '/') {
                    i += 2; in_block = false; break;
                }
                ++i;
            }
            out.push_back({start, i - start, Tok::Comment});
            continue;
        }

        // Whitespace — emit as plain text so column math stays simple.
        if (c == ' ' || c == '\t') {
            int start = i;
            while (i < end && (s[(size_t)i] == ' ' || s[(size_t)i] == '\t')) ++i;
            out.push_back({start, i - start, Tok::Text});
            continue;
        }

        // Preprocessor directive: '#' as first non-space char colours the rest.
        if (c == '#') {
            bool leading = true;
            for (int j = begin; j < i; ++j)
                if (s[(size_t)j] != ' ' && s[(size_t)j] != '\t') { leading = false; break; }
            if (leading) { out.push_back({i, end - i, Tok::Preproc}); i = end; continue; }
        }

        // Line comment.
        if (c == '/' && i + 1 < end && s[(size_t)i + 1] == '/') {
            out.push_back({i, end - i, Tok::Comment}); i = end; continue;
        }
        // Block comment start.
        if (c == '/' && i + 1 < end && s[(size_t)i + 1] == '*') {
            int start = i; i += 2; in_block = true;
            while (i < end) {
                if (s[(size_t)i] == '*' && i + 1 < end && s[(size_t)i + 1] == '/') {
                    i += 2; in_block = false; break;
                }
                ++i;
            }
            out.push_back({start, i - start, Tok::Comment});
            continue;
        }

        // String / char literal (with backslash escapes).
        if (c == '"' || c == '\'') {
            char q = c; int start = i; ++i;
            while (i < end) {
                if (s[(size_t)i] == '\\' && i + 1 < end) { i += 2; continue; }
                if (s[(size_t)i] == q) { ++i; break; }
                ++i;
            }
            out.push_back({start, i - start, Tok::String});
            continue;
        }

        // Number literal.
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < end && std::isdigit((unsigned char)s[(size_t)i + 1]))) {
            int start = i;
            while (i < end) {
                char d = s[(size_t)i];
                if (std::isalnum((unsigned char)d) || d == '.' || d == '\'' ||
                    ((d == '+' || d == '-') && i > start &&
                     (s[(size_t)i - 1] == 'e' || s[(size_t)i - 1] == 'E')))
                    ++i;
                else break;
            }
            out.push_back({start, i - start, Tok::Number});
            continue;
        }

        // Identifier / keyword / type.
        if (is_ident_start(c)) {
            int start = i;
            while (i < end && is_ident_char(s[(size_t)i])) ++i;
            const char* p = s.c_str() + start;
            int n = i - start;
            Tok t = keyword_lookup(p, n) ? Tok::Keyword
                  : type_lookup(p, n)    ? Tok::Type
                                         : Tok::Text;
            out.push_back({start, n, t});
            continue;
        }

        // Anything else: a run of punctuation / operator characters.
        {
            int start = i;
            while (i < end) {
                char d = s[(size_t)i];
                if (std::isalnum((unsigned char)d) || d == '_' || d == ' ' || d == '\t' ||
                    d == '"' || d == '\'' || d == '#')
                    break;
                if (d == '/' && i + 1 < end &&
                    (s[(size_t)i + 1] == '/' || s[(size_t)i + 1] == '*')) break;
                ++i;
            }
            if (i == start) ++i; // never get stuck
            out.push_back({start, i - start, Tok::Punct});
        }
    }
}

// Byte offset where each logical line begins (plus a trailing sentinel equal to
// text.size()), so line `k` spans [starts[k], starts[k+1]) excluding the '\n'.
void compute_line_starts(const std::string& t, std::vector<int>& starts) {
    starts.clear();
    starts.push_back(0);
    for (int i = 0; i < (int)t.size(); ++i)
        if (t[(size_t)i] == '\n') starts.push_back(i + 1);
    starts.push_back((int)t.size());
}

} // namespace

// ===========================================================================
//  CodeEditor
// ===========================================================================
void CodeEditor::set_text(const std::string& s) {
    text = s;
    if (cursor > (int)text.size()) cursor = (int)text.size();
    if (cursor < 0) cursor = 0;
    scroll_line = 0;
    undo_stack.clear();
    redo_stack.clear();
}

bool CodeEditor::draw(nk_context* ctx, float height) {
    if (!ctx) return false;

    const nk_user_font* font = ctx->style.font;
    if (!font) return false;

    // Claim a rectangle from the current layout row.
    struct nk_rect bounds;
    enum nk_widget_layout_states st = nk_widget(&bounds, ctx);
    if (st == NK_WIDGET_INVALID) return false;
    bounds.h = height;

    struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
    if (!canvas) return false;
    const struct nk_input* in = &ctx->input;

    // --- metrics ------------------------------------------------------------
    const float line_h  = font->height + 4.0f;                 // row pitch
    const float advance = font->width(font->userdata, font->height, "M", 1); // monospace cell
    const int   gutter_digits = 5;
    const float gutter_w = advance * gutter_digits + 8.0f;
    const float pad_x    = 4.0f;
    const float text_x0  = bounds.x + gutter_w + pad_x;
    const float text_y0  = bounds.y + 2.0f;
    const int   visible_rows = (int)((bounds.h - 4.0f) / line_h);

    // Precompute line boundaries.
    std::vector<int> starts;
    compute_line_starts(text, starts);
    const int line_count = (int)starts.size() - 1; // sentinel excluded

    bool changed = false;
    bool hovered = nk_input_is_mouse_hovering_rect(in, bounds);

    // ----------------------------------------------------------------------
    //  Mouse: click to place caret / focus, wheel to scroll.
    // ----------------------------------------------------------------------
    // Press places the caret + starts a selection; dragging extends it.
    if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, bounds, nk_true)) {
        focused = true;
        float mxp = in->mouse.pos.x, myp = in->mouse.pos.y;
        int row = scroll_line + (int)((myp - text_y0) / line_h);
        if (row < 0) row = 0;
        if (row > line_count - 1) row = line_count - 1;
        int ls = starts[(size_t)row];
        int le = starts[(size_t)row + 1];
        if (le > ls && text[(size_t)(le - 1)] == '\n') --le;
        int col = (int)((mxp - text_x0) / advance + 0.5f);
        if (col < 0) col = 0;
        int caret = ls + col;
        if (caret > le) caret = le;
        if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) {
            uint32_t now = SDL_GetTicks();
            int dpos = caret - last_click_pos; if (dpos < 0) dpos = -dpos;
            bool dbl = (now - last_click_ms < 400u) && dpos <= 2;
            last_click_ms = now; last_click_pos = caret;
            if (dbl) {
                int a = caret, b = caret;
                while (a > 0 && is_ident_char(text[(size_t)(a - 1)])) --a;
                while (b < (int)text.size() && is_ident_char(text[(size_t)b])) ++b;
                sel_anchor = a; caret = b;
            } else if (nk_input_is_key_down(in, NK_KEY_SHIFT)) {
                if (sel_anchor < 0) sel_anchor = cursor;
            } else {
                sel_anchor = caret;
            }
        }
        cursor = caret;
        blink = 0.0f;
    }
    // Clicking outside removes keyboard focus so global shortcuts still work.
    if (!hovered && nk_input_has_mouse_click(in, NK_BUTTON_LEFT))
        focused = false;

    if (hovered && in->mouse.scroll_delta.y != 0.0f) {
        scroll_line -= (int)in->mouse.scroll_delta.y * 3;
    }

    // Helper: caret -> (row, column) using current line boundaries.
    auto caret_row = [&](int caret) {
        int lo = 0, hi = line_count - 1, row = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (caret >= starts[(size_t)mid]) { row = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return row;
    };

    // ----------------------------------------------------------------------
    //  Keyboard editing (only when focused).
    // ----------------------------------------------------------------------
    if (focused) {
        // --- undo / redo (Ctrl+Z / Ctrl+Y) handled before normal edits ------
        bool history_used = false;
        if (nk_input_is_key_pressed(in, NK_KEY_TEXT_UNDO) && !undo_stack.empty()) {
            redo_stack.push_back(text);
            text = undo_stack.back(); undo_stack.pop_back();
            if (cursor > (int)text.size()) cursor = (int)text.size();
            changed = true; history_used = true; blink = 0;
        } else if (nk_input_is_key_pressed(in, NK_KEY_TEXT_REDO) && !redo_stack.empty()) {
            undo_stack.push_back(text);
            text = redo_stack.back(); redo_stack.pop_back();
            if (cursor > (int)text.size()) cursor = (int)text.size();
            changed = true; history_used = true; blink = 0;
        }

        const std::string before_edit = text;

        // Delete the current selection (if any); returns true if it removed text.
        auto del_sel = [&]() -> bool {
            if (sel_anchor < 0 || sel_anchor == cursor) return false;
            int lo = sel_anchor < cursor ? sel_anchor : cursor;
            int hi = sel_anchor < cursor ? cursor : sel_anchor;
            if (lo < 0) lo = 0;
            if (hi > (int)text.size()) hi = (int)text.size();
            text.erase(text.begin() + lo, text.begin() + hi);
            cursor = lo; sel_anchor = -1; changed = true; return true;
        };
        const bool had_sel = (sel_anchor >= 0 && sel_anchor != cursor);
        const int  sel_lo  = had_sel ? (sel_anchor < cursor ? sel_anchor : cursor) : 0;
        const int  sel_hi  = had_sel ? (sel_anchor < cursor ? cursor : sel_anchor) : 0;

        // Clipboard: Ctrl+C / Ctrl+X / Ctrl+V (SDL clipboard).
        if (!history_used && had_sel && nk_input_is_key_pressed(in, NK_KEY_COPY))
            SDL_SetClipboardText(text.substr((size_t)sel_lo, (size_t)(sel_hi - sel_lo)).c_str());
        if (!history_used && had_sel && nk_input_is_key_pressed(in, NK_KEY_CUT)) {
            SDL_SetClipboardText(text.substr((size_t)sel_lo, (size_t)(sel_hi - sel_lo)).c_str());
            del_sel();
        }
        if (!history_used && nk_input_is_key_pressed(in, NK_KEY_PASTE)) {
            char* clip = SDL_GetClipboardText();
            if (clip && *clip) {
                del_sel();
                std::string cs(clip);
                text.insert((size_t)cursor, cs);
                cursor += (int)cs.size();
                changed = true;
            }
            if (clip) SDL_free(clip);
        }

        // Printable text from this frame (already UTF-8 in input.keyboard.text).
        const struct nk_keyboard& kb = in->keyboard;
        if (!history_used && kb.text_len > 0) {
            del_sel();
            for (int k = 0; k < kb.text_len; ++k) {
                char ch = kb.text[k];
                if (ch == '\r' || ch == '\n') continue;     // Enter handled separately
                if ((unsigned char)ch < 0x20 && ch != '\t') continue;
                text.insert(text.begin() + cursor, ch);
                ++cursor;
                changed = true;
            }
        }

        // ---- autocomplete: keywords/types + C++ funcs (with auto-#include) ------
        bool ac_tab = false, ac_nav = false;
        {
            int ps = cursor;
            while (ps > 0 && is_ident_char(text[(size_t)(ps - 1)])) --ps;
            std::string prefix = text.substr((size_t)ps, (size_t)(cursor - ps));
            suggestions.clear(); sug_headers.clear();
            if ((int)prefix.size() >= 2) {
                for (const char* w : kCompletions)
                    if (std::strncmp(w, prefix.c_str(), prefix.size()) == 0 && prefix != w) {
                        suggestions.push_back(w); sug_headers.push_back("");
                    }
                for (const FnHdr& f : kFuncs)
                    if (std::strncmp(f.name, prefix.c_str(), prefix.size()) == 0 && prefix != f.name) {
                        // auto_include off -> only suggest funcs whose header is already in the file
                        if (auto_include || header_present(text, f.header)) {
                            suggestions.push_back(f.name); sug_headers.push_back(f.header);
                        }
                    }
            }
            sug_open = !suggestions.empty();
            if (sug_sel >= (int)suggestions.size()) sug_sel = 0;
            if (sug_open) {
                if (nk_input_is_key_pressed(in, NK_KEY_DOWN)) { sug_sel = (sug_sel + 1) % (int)suggestions.size(); ac_nav = true; }
                if (nk_input_is_key_pressed(in, NK_KEY_UP))   { sug_sel = (sug_sel + (int)suggestions.size() - 1) % (int)suggestions.size(); ac_nav = true; }
                if (nk_input_is_key_pressed(in, NK_KEY_TAB)) {
                    std::string ins = suggestions[(size_t)sug_sel].substr(prefix.size());
                    text.insert((size_t)cursor, ins);
                    cursor += (int)ins.size();
                    // auto-include: add the header if missing
                    const std::string& hdr = sug_headers[(size_t)sug_sel];
                    if (auto_include && !hdr.empty() && !header_present(text, hdr.c_str())) {
                        std::string inc = "#include <" + hdr + ">\n";
                        size_t last = std::string::npos, p = text.find("#include");
                        while (p != std::string::npos) { last = p; p = text.find("#include", p + 1); }
                        size_t at = 0;
                        if (last != std::string::npos) { size_t e = text.find('\n', last); at = (e == std::string::npos) ? text.size() : e + 1; }
                        text.insert(at, inc);
                        if ((int)at <= cursor) cursor += (int)inc.size();
                    }
                    changed = true; ac_tab = true; sug_open = false;
                }
            }
        }

        if (!history_used) {
            if (nk_input_is_key_pressed(in, NK_KEY_ENTER)) {
                del_sel();
                text.insert(text.begin() + cursor, '\n');
                ++cursor; changed = true;
            }
            if (nk_input_is_key_pressed(in, NK_KEY_TAB) && !ac_tab) {
                del_sel();
                for (int k = 0; k < tab_spaces; ++k)
                    text.insert(text.begin() + cursor + k, ' ');
                cursor += tab_spaces; changed = true;
            }
            if (nk_input_is_key_pressed(in, NK_KEY_BACKSPACE)) {
                if (!del_sel() && cursor > 0) {
                    int ls = cursor; while (ls > 0 && text[(size_t)(ls - 1)] != '\n') --ls;
                    int col = cursor - ls;
                    bool tab_del = (col >= tab_spaces) && (col % tab_spaces == 0);
                    for (int k = 1; k <= tab_spaces && tab_del; ++k)
                        if (text[(size_t)(cursor - k)] != ' ') tab_del = false;
                    if (tab_del) { text.erase((size_t)(cursor - tab_spaces), (size_t)tab_spaces); cursor -= tab_spaces; }
                    else { text.erase(text.begin() + (cursor - 1)); --cursor; }
                    changed = true;
                }
            }
            if (nk_input_is_key_pressed(in, NK_KEY_DEL)) {
                if (!del_sel() && cursor < (int)text.size()) { text.erase(text.begin() + cursor); changed = true; }
            }
        }

        // Coalesce this frame's normal edits into one undo step.
        if (changed && !history_used && before_edit != text) {
            if (undo_stack.empty() || undo_stack.back() != before_edit) {
                undo_stack.push_back(before_edit);
                if (undo_stack.size() > 400) undo_stack.erase(undo_stack.begin());
            }
            redo_stack.clear();
        }

        // Caret motion. Recompute line table after edits above so movement
        // is consistent within the same frame.
        if (changed) compute_line_starts(text, starts);
        const int lc = (int)starts.size() - 1;

        if (!ac_nav && (nk_input_is_key_pressed(in, NK_KEY_LEFT) || nk_input_is_key_pressed(in, NK_KEY_RIGHT) ||
            nk_input_is_key_pressed(in, NK_KEY_UP)   || nk_input_is_key_pressed(in, NK_KEY_DOWN)))
            sel_anchor = -1;   // caret motion collapses the selection
        if (nk_input_is_key_pressed(in, NK_KEY_LEFT)  && cursor > 0) { --cursor; blink = 0; }
        if (nk_input_is_key_pressed(in, NK_KEY_RIGHT) && cursor < (int)text.size()) { ++cursor; blink = 0; }
        if (!ac_nav && (nk_input_is_key_pressed(in, NK_KEY_UP) || nk_input_is_key_pressed(in, NK_KEY_DOWN))) {
            int row = 0, lo = 0, hi = lc - 1;
            while (lo <= hi) { int m = (lo + hi) / 2;
                if (cursor >= starts[(size_t)m]) { row = m; lo = m + 1; } else hi = m - 1; }
            int col = cursor - starts[(size_t)row];
            int target = row + (nk_input_is_key_pressed(in, NK_KEY_DOWN) ? 1 : -1);
            if (target >= 0 && target < lc) {
                int ts = starts[(size_t)target];
                int te = starts[(size_t)target + 1];
                if (te > ts && text[(size_t)(te - 1)] == '\n') --te;
                int want = ts + col;
                cursor = want > te ? te : want;
                blink = 0;
            }
        }
        if (nk_input_is_key_pressed(in, NK_KEY_TEXT_LINE_START)) { // Home
            int row = caret_row(cursor);
            cursor = starts[(size_t)row]; blink = 0;
        }
        if (nk_input_is_key_pressed(in, NK_KEY_TEXT_LINE_END)) {   // End
            int row = caret_row(cursor);
            int te = starts[(size_t)row + 1];
            if (te > starts[(size_t)row] && te <= (int)text.size() &&
                text[(size_t)(te - 1)] == '\n') --te;
            cursor = te; blink = 0;
        }
    }

    if (cursor < 0) cursor = 0;
    if (cursor > (int)text.size()) cursor = (int)text.size();

    // Recompute lines once more if anything changed, then figure caret row/col.
    if (changed) compute_line_starts(text, starts);
    const int lines_now = (int)starts.size() - 1;
    int caret_line = caret_row(cursor);
    int caret_col  = cursor - starts[(size_t)caret_line];

    // ----------------------------------------------------------------------
    //  Scrolling: clamp and follow the caret.
    // ----------------------------------------------------------------------
    int max_scroll = lines_now - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (caret_line < scroll_line) scroll_line = caret_line;
    else if (visible_rows > 0 && caret_line >= scroll_line + visible_rows)
        scroll_line = caret_line - visible_rows + 1;
    if (scroll_line > max_scroll) scroll_line = max_scroll;
    if (scroll_line < 0) scroll_line = 0;

    // ----------------------------------------------------------------------
    //  Painting.
    // ----------------------------------------------------------------------
    const bool r_sel  = (sel_anchor >= 0 && sel_anchor != cursor);
    const int  r_selL = r_sel ? (sel_anchor < cursor ? sel_anchor : cursor) : 0;
    const int  r_selH = r_sel ? (sel_anchor < cursor ? cursor : sel_anchor) : 0;

    nk_fill_rect(canvas, bounds, 0.0f, C_BG);
    struct nk_rect gutter = nk_rect(bounds.x, bounds.y, gutter_w, bounds.h);
    nk_fill_rect(canvas, gutter, 0.0f, C_GUTTER);

    // Clip everything to the widget rectangle so long lines don't bleed out.
    nk_push_scissor(canvas, bounds);

    // Determine block-comment state at the first visible line by scanning the
    // hidden prefix once (cheap for small files).
    bool in_block = false;
    {
        std::vector<Span> tmp;
        for (int r = 0; r < scroll_line && r < lines_now; ++r)
            lex_line(text, starts[(size_t)r], starts[(size_t)r + 1] -
                     ((starts[(size_t)r + 1] > starts[(size_t)r] &&
                       text[(size_t)(starts[(size_t)r + 1] - 1)] == '\n') ? 1 : 0),
                     in_block, tmp);
    }

    std::vector<Span> spans;
    char numbuf[16];
    for (int vis = 0; vis < visible_rows; ++vis) {
        int row = scroll_line + vis;
        if (row >= lines_now) break;
        float ry = text_y0 + vis * line_h;

        int ls = starts[(size_t)row];
        int le = starts[(size_t)row + 1];
        if (le > ls && text[(size_t)(le - 1)] == '\n') --le; // drop newline

        // Highlight the caret's current line (skip when there's a selection).
        if (row == caret_line && !r_sel) {
            struct nk_rect hl = nk_rect(bounds.x + gutter_w, ry - 1.0f,
                                        bounds.w - gutter_w, line_h);
            nk_fill_rect(canvas, hl, 0.0f, C_CURLINE);
        }
        // Highlight the selected span on this line.
        if (r_sel) {
            int line_end_incl = starts[(size_t)row + 1];
            int a = r_selL > ls ? r_selL : ls;
            int b = r_selH < line_end_incl ? r_selH : line_end_incl;
            if (b > a && r_selL < line_end_incl && r_selH > ls) {
                float hx0 = text_x0 + (float)(a - ls) * advance;
                float hx1 = text_x0 + (float)(b - ls) * advance;
                if (r_selH > line_end_incl) hx1 += advance * 0.4f;  // spans the newline
                nk_fill_rect(canvas, nk_rect(hx0, ry - 1.0f, hx1 - hx0, line_h),
                             0.0f, nk_rgba(80, 120, 200, 110));
            }
        }

        // Line number (right-aligned in the gutter).
        int n = std::snprintf(numbuf, sizeof(numbuf), "%d", row + 1);
        float numw = font->width(font->userdata, font->height, numbuf, n);
        struct nk_rect nr = nk_rect(bounds.x + gutter_w - numw - 6.0f, ry,
                                    numw, line_h);
        nk_draw_text(canvas, nr, numbuf, n, font, C_GUTTER, C_LINENO);

        // Coloured tokens for the line. We position every glyph on the fixed
        // monospace grid so colours never shift the layout.
        {   // indent guides (faint vertical lines every tab_spaces columns)
            int indent = 0; while (indent < (le - ls) && text[(size_t)(ls + indent)] == ' ') ++indent;
            for (int g = tab_spaces; g < indent; g += tab_spaces) {
                float gx = text_x0 + (float)g * advance;
                nk_stroke_line(canvas, gx, ry, gx, ry + line_h, 1.0f, nk_rgba(120, 122, 132, 55));
            }
        }
        lex_line(text, ls, le, in_block, spans);
        for (const Span& sp : spans) {
            nk_color c = tok_color(sp.tok);
            for (int j = 0; j < sp.len; ++j) {
                int idx = sp.start + j;
                char ch = text[(size_t)idx];
                int colx = idx - ls;
                if (ch == '\t') continue; // tabs render as blank cells
                float gx = text_x0 + colx * advance;
                if (gx > bounds.x + bounds.w) break;
                struct nk_rect gr = nk_rect(gx, ry, advance + 1.0f, line_h);
                nk_draw_text(canvas, gr, &text[(size_t)idx], 1, font, C_BG, c);
            }
        }
    }

    // ----------------------------------------------------------------------
    //  Caret (simple blink driven by wall-clock time).
    // ----------------------------------------------------------------------
    if (focused) {
        blink += 0.016f;
        bool show = std::fmod((double)SDL_GetTicks() / 1000.0, 1.06) < 0.53;
        int caret_vis_row = caret_line - scroll_line;
        if (show && caret_vis_row >= 0 && caret_vis_row < visible_rows) {
            float cx = text_x0 + caret_col * advance;
            float cy = text_y0 + caret_vis_row * line_h;
            struct nk_rect cr = nk_rect(cx, cy, 2.0f, line_h - 2.0f);
            nk_fill_rect(canvas, cr, 0.0f, C_CURSOR);
        }
    }

    // Restore Nuklear's default clip so subsequent widgets draw normally.
    nk_push_scissor(canvas, nk_rect(0, 0, 16384, 16384));

    // ---- autocomplete popup (near the caret) ----
    if (focused && sug_open && !suggestions.empty()) {
        int crow = caret_line - scroll_line;
        if (crow >= 0 && crow < visible_rows) {
            float px = text_x0 + caret_col * advance;
            float py = text_y0 + (crow + 1) * line_h + 2.0f;
            int shown = (int)suggestions.size(); if (shown > 7) shown = 7;
            float pw = 60.0f;
            for (int i = 0; i < shown; ++i) {
                float w = font->width(font->userdata, font->height,
                                      suggestions[(size_t)i].c_str(), (int)suggestions[(size_t)i].size());
                if (w + 20.0f > pw) pw = w + 20.0f;
            }
            struct nk_rect box = nk_rect(px, py, pw, shown * line_h + 4.0f);
            nk_fill_rect(canvas, box, 3.0f, nk_rgb(40, 42, 48));
            nk_stroke_rect(canvas, box, 3.0f, 1.0f, nk_rgb(95, 100, 110));
            for (int i = 0; i < shown; ++i) {
                struct nk_rect rr = nk_rect(px + 2.0f, py + 2.0f + i * line_h, pw - 4.0f, line_h);
                nk_color bg = (i == sug_sel) ? nk_rgb(70, 90, 140) : nk_rgb(40, 42, 48);
                if (i == sug_sel) nk_fill_rect(canvas, rr, 2.0f, bg);
                nk_draw_text(canvas, rr, suggestions[(size_t)i].c_str(),
                             (int)suggestions[(size_t)i].size(), font, bg, nk_rgb(225, 225, 230));
            }
        }
    }

    // A thin border around the editor.
    nk_stroke_line(canvas, bounds.x, bounds.y, bounds.x + bounds.w, bounds.y, 1.0f, C_PUNCT);
    nk_stroke_line(canvas, bounds.x + gutter_w, bounds.y,
                   bounds.x + gutter_w, bounds.y + bounds.h, 1.0f, C_GUTTER);

    return changed;
}
