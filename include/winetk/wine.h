#ifndef WINETK_WINE_H
#define WINETK_WINE_H

#include "winetk/backend.h"
#include "winetk/wine_protocol.h"

#include <stddef.h>
#include <stdint.h>

struct wt_wine_server;

/* The caller supplies an absolute socket pathname inside a private directory
 * owned by the current user (mode 0700). Existing paths are never replaced.
 * Connections must have the same effective uid as the receiving process.
 * All calls must be serialized; no background thread or terminal I/O is used. */
struct wt_wine_server *wt_wine_server_open(const char *path,
                                          uint32_t width, uint32_t height);
/* Follow this application's largest visible client rectangle. It determines
 * the canvas resolution/origin; other windows are composited relative to it.
 * Input x/y remain canvas-local at the public API and are translated back to
 * Wine desktop pixels internally. Zero disables follow mode. */
void wt_wine_server_follow_process(struct wt_wine_server *server, uint32_t pid);
/* Poll/consume a bounded amount of pending work. 1 means the composed desktop
 * changed, 0 means unchanged; -1 reports a listener/poll/allocation error.
 * Malformed, oversized or disconnected clients lose their own windows only. */
int wt_wine_server_update(struct wt_wine_server *server, int timeout_ms);
/* Borrowed top-down RGBA storage, valid until the next update or close. */
void wt_wine_server_frame(const struct wt_wine_server *server,
                          struct winetk_frame *frame);
size_t wt_wine_server_window_count(const struct wt_wine_server *server);
size_t wt_wine_server_client_count(const struct wt_wine_server *server);
/* A zero hwnd selects the focused window, or the window under a button press.
 * Desktop coordinates are absolute. 1 means queued/sent, 0 no target, -1 error.
 * A bounded per-client queue preserves complete packets across short writes. */
int wt_wine_server_send_input(struct wt_wine_server *server,
                              const struct wt_wine_input *input);
void wt_wine_server_close(struct wt_wine_server *server);

#endif
