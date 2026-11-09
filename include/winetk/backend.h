#ifndef WINETK_BACKEND_H
#define WINETK_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Top-down, straight-alpha RGBA8 in byte order R, G, B, A. A cropped view
 * may adjust pixels/size/width/height while retaining its parent's stride.
 * size is the readable extent starting at pixels, including row padding.
 * The caller retains ownership until present() returns. */
struct winetk_frame {
    const uint8_t *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
    size_t stride;
};

/* Zero-based terminal cell origin and pixel offsets within that cell.
 * The caller validates offsets against the measured cell size and prepares
 * a frame at its desired output size; the backend does not scale it. */
struct winetk_viewport {
    uint32_t column;
    uint32_t row;
    uint32_t pixel_x;
    uint32_t pixel_y;
};

/* POSIX write semantics: positive counts may be short, -1 sets errno.
 * The backend retries EINTR and short writes. EAGAIN and other failures are
 * terminal for this backend; use a blocking sink or handle readiness inside
 * the callback. Returning zero is an EIO error. */
typedef ssize_t (*winetk_write_fn)(void *context, const void *data, size_t size);

struct winetk_backend_ops {
    int (*probe)(void *state);
    int (*present)(void *state, const struct winetk_frame *frame,
                   const struct winetk_viewport *viewport);
    int (*close)(void *state);
};

/* Initialize to {0}. Calls must be serialized with all other terminal output:
 * a complete frame is one transaction. No operation owns the TTY lifecycle. */
struct winetk_backend {
    const struct winetk_backend_ops *ops;
    void *state;
};

int winetk_backend_probe(struct winetk_backend *backend);
int winetk_backend_present(struct winetk_backend *backend,
                           const struct winetk_frame *frame,
                           const struct winetk_viewport *viewport);
/* Idempotent; releases the backend even when terminal cleanup fails. */
int winetk_backend_close(struct winetk_backend *backend);

/* IDs must be distinct nonzero IDs reserved by the owning terminal session.
 * Repeated presents replace this image and recreate this placement. close()
 * deletes only this session image, never other terminal graphics.
 * probe() emits a non-storing RGBA query followed by DA1. The caller parses
 * its response and must not present until support is confirmed. */
int winetk_kitty_init(struct winetk_backend *backend, winetk_write_fn write_fn,
                     void *write_context, uint32_t image_id,
                     uint32_t placement_id);
/* Opt in to zlib compression for large frames. Small/incompressible frames
 * retain direct RGBA; changing this never affects pixel semantics. */
int winetk_kitty_compress(struct winetk_backend *backend, int enabled);
/* Probe POSIX shared-memory access with a separate nonzero query ID. The caller
 * must receive a matching successful graphics response before enabling it.
 * Local allocation failure is recoverable: keep the ordinary direct backend. */
int winetk_kitty_probe_shared(struct winetk_backend *backend, uint32_t query_id);
/* Off by default. Uses at most four outstanding frames and one probe object;
 * unsupported/unavailable/full shared memory falls back to the direct path.
 * The terminal unlinks consumed objects; close() removes all leftovers. */
int winetk_kitty_shared_memory(struct winetk_backend *backend, int enabled);

#endif
