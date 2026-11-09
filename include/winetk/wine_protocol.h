#ifndef WINETK_WINE_PROTOCOL_H
#define WINETK_WINE_PROTOCOL_H

#include <stdint.h>

/* Local, little-endian Unix stream protocol. size is payload bytes, excluding
 * this header. No pointers, native handles, or architecture-dependent fields. */
#define WT_WINE_MAGIC 0x4b544957u
#define WT_WINE_VERSION 2u
#define WT_WINE_MAX_WIDTH 1920u
#define WT_WINE_MAX_HEIGHT 1080u
/* WINDOW metadata may include non-client extents and padded GDI storage. */
#define WT_WINE_MAX_WINDOW_WIDTH 2048u
#define WT_WINE_MAX_WINDOW_HEIGHT 1152u
#define WT_WINE_MAX_PAYLOAD (48u + WT_WINE_MAX_WIDTH * WT_WINE_MAX_HEIGHT * 4u)
#define WT_WINE_FRAME 1u
#define WT_WINE_DESTROY 2u
#define WT_WINE_WINDOW 3u
#define WT_WINE_INPUT 4u
#define WT_WINE_VISIBLE 1u
#define WT_WINE_FOREGROUND 2u
/* FRAME producer is a GPU drawable; retained across WINDOW geometry updates. */
#define WT_WINE_GPU 4u
#define WT_WINE_KEY 1u
#define WT_WINE_TEXT 2u
#define WT_WINE_MOVE 3u
#define WT_WINE_BUTTON 4u
#define WT_WINE_WHEEL 5u
#define WT_WINE_FOCUS 6u
#define WT_WINE_CLOSE 7u

struct wt_wine_header
{
    uint32_t magic, version, type, size;
};

/* FRAME: this structure followed by tightly packed top-down RGBA8 pixels.
 * WINDOW: this structure only. Coordinates are virtual desktop pixels.
 * FRAME pixels cover only the client area (excluding Wine non-client decoration).
 * client_* always describes the full client rectangle in virtual desktop pixels;
 * x/y/width/height is the transmitted pixel region for FRAME, outer visible rect
 * for WINDOW. The client origin keeps terminal-to-desktop input mapping exact.
 * pid is the host getpid(), hwnd is the process's Wine window handle. */
struct wt_wine_window
{
    uint64_t hwnd;
    uint32_t pid;
    int32_t x, y;
    uint32_t width, height, flags;
    int32_t client_x, client_y;
    uint32_t client_width, client_height;
};

struct wt_wine_destroy
{
    uint64_t hwnd;
    uint32_t pid, reserved;
};

/* KEY: code = Windows virtual key, value = 1 down / 0 up.
 * TEXT: code = Unicode scalar (synthesized UTF-16 key down/up).
 * MOVE/BUTTON/WHEEL: x,y = desktop pixels; BUTTON code 1/2/3 = left/middle/right,
 * value = 1 down / 0 up; WHEEL value = signed vertical wheel steps.
 * FOCUS with value=1 activates hwnd; value=0 is a focus-loss notification; CLOSE posts WM_CLOSE. modifiers is reserved, zero.
 * The connection identifies the target process; hwnd must belong to it. */
struct wt_wine_input
{
    uint64_t hwnd;
    uint32_t kind, code;
    int32_t x, y, value;
    uint32_t modifiers;
};

#endif
