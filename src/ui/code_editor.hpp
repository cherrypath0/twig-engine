#pragma once
// A real, editable, syntax-coloured C++ source editor drawn as a custom
// Nuklear widget. Unlike a plain nk_edit + a separate read-only preview, this
// single widget is BOTH editable AND syntax-highlighted: it claims a rectangle
// with nk_widget(), then paints line numbers, coloured tokens and a cursor
// straight onto the window command buffer (nk_window_get_canvas).
//
// Usage (inside an nk_begin/nk_end window):
//     static CodeEditor ed;
//     ed.set_text(source);                 // load once (e.g. when a file opens)
//     if (ed.draw(ctx, panel_height)) {    // returns true the frame text changed
//         source = ed.text;                // pull edits back out
//     }
//
// Monospace is assumed: a fixed glyph advance (width of 'M') is used for layout
// so columns line up regardless of the actual glyph widths. Selection is not
// implemented, but caret movement, editing, scrolling and clicking all are.

#include <string>
#include <vector>
#include <cstdint>

struct nk_context;

struct CodeEditor {
    // --- editable state -----------------------------------------------------
    std::string text;            // the full source buffer (UTF-8 / ASCII)
    int  cursor      = 0;        // caret position as a byte offset into `text`
    int  sel_anchor  = -1;       // selection start (-1 = none); span is [min,max] with cursor
    int  scroll_line = 0;        // index of the first visible line (vertical scroll)
    int  tab_spaces  = 4;        // how many spaces a Tab key inserts
    bool focused     = true;     // when false, ignores keyboard (caret hidden)

    // Undo / redo history (whole-buffer snapshots; Ctrl+Z / Ctrl+Y).
    std::vector<std::string> undo_stack;
    std::vector<std::string> redo_stack;

    // Blink phase accumulator (seconds); advanced internally each draw().
    float blink = 0.0f;

    // Double-click detection + autocomplete popup state.
    uint32_t last_click_ms  = 0;
    int      last_click_pos = -1;
    std::vector<std::string> suggestions;
    std::vector<std::string> sug_headers;   // parallel: header to #include (or "")
    int  sug_sel  = 0;
    bool sug_open = false;
    bool auto_include = true;                // on: suggest all libs + add #include on accept

    // Replace the whole buffer and clamp the caret. Call when loading a file.
    void set_text(const std::string& s);

    // Draw the editor filling the current widget slot, `height` pixels tall.
    // Must be called between nk_begin()/nk_end(). Returns true if `text` was
    // modified by user input during this frame.
    bool draw(nk_context* ctx, float height);
};
