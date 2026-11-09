#ifndef WINETK_INPUT_H
#define WINETK_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Numeric key identities match the Kitty keyboard protocol. */
enum wt_key_code {
    WT_KEY_ESCAPE = 27,
    WT_KEY_ENTER = 13,
    WT_KEY_TAB = 9,
    WT_KEY_BACKSPACE = 127,
    WT_KEY_INSERT = 57348,
    WT_KEY_DELETE,
    WT_KEY_LEFT,
    WT_KEY_RIGHT,
    WT_KEY_UP,
    WT_KEY_DOWN,
    WT_KEY_PAGE_UP,
    WT_KEY_PAGE_DOWN,
    WT_KEY_HOME,
    WT_KEY_END,
    WT_KEY_F1 = 57364,
    WT_KEY_F2,
    WT_KEY_F3,
    WT_KEY_F4,
    WT_KEY_F5,
    WT_KEY_F6,
    WT_KEY_F7,
    WT_KEY_F8,
    WT_KEY_F9,
    WT_KEY_F10,
    WT_KEY_F11,
    WT_KEY_F12,
    WT_KEY_MENU = 57363
};

enum wt_modifier {
    WT_MOD_SHIFT = 1u << 0,
    WT_MOD_ALT = 1u << 1,
    WT_MOD_CTRL = 1u << 2,
    WT_MOD_SUPER = 1u << 3,
    WT_MOD_HYPER = 1u << 4,
    WT_MOD_META = 1u << 5,
    WT_MOD_CAPS_LOCK = 1u << 6,
    WT_MOD_NUM_LOCK = 1u << 7
};

enum wt_key_action {
    WT_KEY_PRESS = 1,
    WT_KEY_REPEAT = 2,
    WT_KEY_RELEASE = 3
};

enum wt_mouse_action {
    WT_MOUSE_PRESS,
    WT_MOUSE_RELEASE,
    WT_MOUSE_MOVE,
    WT_MOUSE_WHEEL
};

enum wt_input_event_type {
    WT_INPUT_KEY,
    WT_INPUT_TEXT,
    WT_INPUT_MOUSE,
    WT_INPUT_FOCUS,
    WT_INPUT_PASTE,
    WT_INPUT_GRAPHICS_REPLY,
    WT_INPUT_DEVICE_ATTRIBUTES,
    WT_INPUT_KEYBOARD_FLAGS,
    WT_INPUT_WINDOW_SIZE,
    WT_INPUT_MODE_REPORT
};

#define WT_INPUT_SEQUENCE_MAX 2048u
#define WT_INPUT_GRAPHICS_MESSAGE_MAX 256u
#define WT_INPUT_DEVICE_VALUES_MAX 16u

struct wt_input_event {
    enum wt_input_event_type type;
    union {
        struct {
            uint32_t codepoint, shifted_key, base_key, modifiers;
            enum wt_key_action action;
            /* Legacy spelling without an explicit event type. In a negotiated
             * Kitty session, a bare arrow press still has a later release. */
            bool legacy;
            uint32_t text_count; /* Number of associated TEXT events that follow. */
        } key;
        struct {
            uint32_t codepoint;
            char utf8[5]; /* Valid UTF-8, followed by NUL; length may include NUL. */
            unsigned length;
            bool paste;
        } text;
        struct {
            enum wt_mouse_action action;
            uint32_t button; /* 0 none/unspecified; 1 left, 2 middle, 3 right. */
            int32_t x, y; /* Raw, 1-based SGR coordinates, pixels or cells. */
            int32_t wheel_x, wheel_y; /* Positive means right/up. */
            uint32_t modifiers;
        } mouse;
        struct { bool focused; } focus;
        struct { bool begin; } paste;
        struct {
            uint32_t image_id, placement_id;
            bool has_image_id, has_placement_id, ok, truncated;
            char message[WT_INPUT_GRAPHICS_MESSAGE_MAX];
        } graphics;
        struct {
            char prefix; /* '?', '>', '=', or 0. */
            uint32_t values[WT_INPUT_DEVICE_VALUES_MAX];
            size_t count;
        } device;
        struct { uint32_t flags; } keyboard;
        struct {
            uint32_t kind; /* CSI t reply: 4 window pixels, 6 cell pixels, 8 cells. */
            uint32_t width, height;
        } window;
        struct { uint32_t mode, value; } mode; /* DECRQM response. */
    };
};

/* Callback is synchronous. Copy the event if it must outlive the callback.
 * Plain UTF-8 produces TEXT only; it cannot identify physical key releases.
 * Enhanced keyboard reports produce KEY then any associated TEXT, except that
 * releases never produce TEXT. KEY text_count gives the exact number of
 * associated TEXT events that immediately follow it; legacy keys, releases and
 * enhanced reports without text leave it zero.
 * Bracketed paste produces PASTE begin/end and
 * TEXT with paste=true; its bytes never become key shortcuts or protocol replies.
 * Text may contain controls (including pasted ESC); escape it before logging
 * or displaying it on a terminal. This parser never performs I/O or allocates.
 */
typedef void (*wt_input_callback)(void *userdata, const struct wt_input_event *event);

struct wt_input_parser {
    wt_input_callback callback;
    void *userdata;
    uint64_t malformed_sequences, discarded_sequences;
    /* Internal state; initialize with wt_input_parser_init(). */
    char sequence[WT_INPUT_SEQUENCE_MAX];
    size_t sequence_length;
    uint32_t utf8_codepoint, utf8_minimum, utf8_modifiers;
    unsigned state, utf8_remaining, paste_match;
    bool paste_active, string_escape, string_discard, string_osc;
};

void wt_input_parser_init(struct wt_input_parser *parser,
                          wt_input_callback callback, void *userdata);
void wt_input_parser_feed(struct wt_input_parser *parser,
                          const void *bytes, size_t length);
bool wt_input_parser_pending_escape(const struct wt_input_parser *parser);
/* Call after the caller's ESC ambiguity timeout (~30 ms). Only a lone ESC is
 * resolved; incomplete CSI, control strings and UTF-8 are retained unchanged. */
void wt_input_parser_flush(struct wt_input_parser *parser);

struct wt_viewport {
    uint32_t logical_width, logical_height;
    uint32_t terminal_width, terminal_height;
    uint32_t x, y, width, height; /* Zero-based pixel rectangle in the terminal. */
};

/* Fit a fixed logical desktop into terminal pixels, preserving aspect ratio to
 * integer pixel rounding. All dimensions must be positive and <= INT32_MAX. */
bool wt_viewport_fit(struct wt_viewport *viewport,
                     uint32_t logical_width, uint32_t logical_height,
                     uint32_t terminal_width, uint32_t terminal_height);
/* Convert 1-based SGR pixels, or cell centers when pixels=false. Returns true
 * only inside the canvas; valid outside coordinates still return clamped logical
 * positions, suitable for mouse capture. Invalid inputs return false and zero.
 * The caller must use the same rectangle for image placement and hit testing. */
bool wt_viewport_map(const struct wt_viewport *viewport,
                     int32_t terminal_x, int32_t terminal_y, bool pixels,
                     uint32_t columns, uint32_t rows,
                     int32_t *logical_x, int32_t *logical_y);

#endif
