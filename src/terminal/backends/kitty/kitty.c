#define _POSIX_C_SOURCE 200809L
#include "winetk/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* 3072 raw bytes encode to the protocol's maximum 4096-byte payload.
 * Keeping a multiple of 3 makes padding necessary only on the final chunk. */
#define RAW_CHUNK_SIZE 3072u
#define ENCODED_CHUNK_SIZE 4096u
#define SHARED_FRAME_LIMIT 4u
#define SHARED_NAME_SIZE 96u

struct kitty_state {
    winetk_write_fn write_fn;
    void *write_context;
    uint32_t image_id;
    uint32_t placement_id;
    int output_error;
    bool image_touched;
    bool transferring;
    bool compression;
    bool shared_memory;
    bool shared_nonce_ready;
    uint64_t shared_nonce, shared_sequence;
    char shared_probe[SHARED_NAME_SIZE];
    char shared_frames[SHARED_FRAME_LIMIT][SHARED_NAME_SIZE];
    unsigned char *compressed;
    size_t compressed_capacity;
};

static int write_all(struct kitty_state *state, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size) {
        ssize_t written = state->write_fn(state->write_context, bytes, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (!errno)
                errno = EIO;
            return -1;
        }
        if (!written || (size_t)written > size) {
            errno = EIO;
            return -1;
        }
        bytes += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int output_failed(struct kitty_state *state)
{
    state->output_error = errno ? errno : EIO;
    errno = state->output_error;
    return -1;
}

static int check_output(const struct kitty_state *state)
{
    if (state->output_error) {
        errno = state->output_error;
        return -1;
    }
    return 0;
}

static size_t encode_base64(const unsigned char *input, size_t size, char *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in = 0, out = 0;
    while (in < size) {
        size_t remaining = size - in;
        uint32_t value = (uint32_t)input[in] << 16;
        if (remaining > 1)
            value |= (uint32_t)input[in + 1] << 8;
        if (remaining > 2)
            value |= input[in + 2];
        output[out++] = alphabet[(value >> 18) & 63];
        output[out++] = alphabet[(value >> 12) & 63];
        output[out++] = remaining > 1 ? alphabet[(value >> 6) & 63] : '=';
        output[out++] = remaining > 2 ? alphabet[value & 63] : '=';
        in += remaining > 3 ? 3 : remaining;
    }
    return out;
}

/* Shared memory is an optional local fast path. Names stay live until the
 * terminal has opened/read/unlinked them; never recycle a still-live object.
 * All pending names, including an unanswered query, remain bounded. */
static void reclaim_shared_name(char name[SHARED_NAME_SIZE])
{
    int fd;
    if (!name[0]) return;
    fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    if (fd >= 0) close(fd);
    else if (errno == ENOENT) name[0] = 0;
}

static void remove_shared_name(char name[SHARED_NAME_SIZE])
{
    if (!name[0]) return;
    (void)shm_unlink(name);
    name[0] = 0;
}

static int create_shared_frame(struct kitty_state *state, char name[SHARED_NAME_SIZE],
                               const struct winetk_frame *frame, size_t row_size,
                               size_t total_size)
{
    off_t extent = (off_t)total_size;
    void *mapping;
    int fd = -1, error;
    if (extent < 0 || (uintmax_t)extent != (uintmax_t)total_size) {
        errno = EOVERFLOW;
        return -1;
    }
    if (!state->shared_nonce_ready) {
        ssize_t got;
        do got = getrandom(&state->shared_nonce, sizeof(state->shared_nonce), GRND_NONBLOCK);
        while (got < 0 && errno == EINTR);
        if (got != (ssize_t)sizeof(state->shared_nonce)) {
            if (got >= 0) errno = EIO;
            return -1;
        }
        state->shared_nonce_ready = true;
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        int size = snprintf(name, SHARED_NAME_SIZE,
                            "/winetk-%" PRIx64 "-%jd-%" PRIx64,
                            state->shared_nonce, (intmax_t)getpid(), ++state->shared_sequence);
        if (size < 0 || (size_t)size >= SHARED_NAME_SIZE) { errno = EOVERFLOW; break; }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { name[0] = 0; return -1; }
    if (fchmod(fd, 0600) < 0 || ftruncate(fd, extent) < 0) goto failed;
    /* ftruncate alone can succeed on a full tmpfs and make memcpy SIGBUS.
     * Reserve backing storage before mapping; lack of space is a fallback. */
    error = posix_fallocate(fd, 0, extent);
    if (error) { errno = error; goto failed; }
    mapping = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) goto failed;
    for (uint32_t row = 0; row < frame->height; ++row)
        memcpy((unsigned char *)mapping + (size_t)row * row_size,
               frame->pixels + (size_t)row * frame->stride, row_size);
    error = munmap(mapping, total_size);
    if (error < 0) goto failed;
    close(fd);
    return 0;
failed:
    error = errno;
    close(fd);
    remove_shared_name(name);
    errno = error;
    return -1;
}

static int send_shared_frame(struct kitty_state *state, const struct winetk_frame *frame,
                             size_t row_size, size_t total_size)
{
    char encoded[SHARED_NAME_SIZE * 2], command[SHARED_NAME_SIZE * 2 + 160];
    size_t encoded_size;
    int size;
    unsigned slot;
    if (!state->shared_memory) return 0;
    reclaim_shared_name(state->shared_probe);
    for (slot = 0; slot < SHARED_FRAME_LIMIT; ++slot) {
        reclaim_shared_name(state->shared_frames[slot]);
        if (!state->shared_frames[slot][0]) break;
    }
    if (slot == SHARED_FRAME_LIMIT) return 0;
    if (create_shared_frame(state, state->shared_frames[slot], frame, row_size, total_size))
        return 0; /* Allocation/syscall failure retains the direct transfer. */
    encoded_size = encode_base64((const unsigned char *)state->shared_frames[slot],
                                 strlen(state->shared_frames[slot]), encoded);
    size = snprintf(command, sizeof(command),
                    "\033_Ga=t,t=s,f=32,s=%" PRIu32 ",v=%" PRIu32 ",i=%" PRIu32 ",q=2;",
                    frame->width, frame->height, state->image_id);
    if (size < 0 || (size_t)size + encoded_size + 2 > sizeof(command)) {
        remove_shared_name(state->shared_frames[slot]);
        return 0;
    }
    memcpy(command + size, encoded, encoded_size);
    memcpy(command + size + encoded_size, "\033\\", 2);
    state->image_touched = true;
    state->transferring = true;
    if (write_all(state, command, (size_t)size + encoded_size + 2)) return output_failed(state);
    state->transferring = false;
    return 1;
}

static int validate_frame(const struct winetk_frame *frame,
                          const struct winetk_viewport *viewport,
                          size_t *row_size, size_t *total_size)
{
    size_t preceding_rows;
    if (!frame || !viewport || !frame->pixels || !frame->width ||
        !frame->height) {
        errno = EINVAL;
        return -1;
    }
    if ((uintmax_t)frame->width * 4 > SIZE_MAX ||
        viewport->column == UINT32_MAX || viewport->row == UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *row_size = (size_t)frame->width * 4;
    if (frame->stride < *row_size) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)(frame->height - 1) >
        (SIZE_MAX - *row_size) / frame->stride ||
        (size_t)frame->height > SIZE_MAX / *row_size) {
        errno = EOVERFLOW;
        return -1;
    }
    preceding_rows = (size_t)(frame->height - 1) * frame->stride;
    if (frame->size < preceding_rows + *row_size) {
        errno = EINVAL;
        return -1;
    }
    *total_size = *row_size * (size_t)frame->height;
    return 0;
}

static int kitty_probe(void *opaque)
{
    struct kitty_state *state = opaque;
    char command[128];
    int size;
    if (check_output(state))
        return -1;
    size = snprintf(command, sizeof(command),
                    "\033_Ga=q,t=d,f=32,s=1,v=1,i=%" PRIu32
                    ";AAAA/w==\033\\\033[c", state->image_id);
    if (size < 0 || (size_t)size >= sizeof(command)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (write_all(state, command, (size_t)size))
        return output_failed(state);
    return 0;
}

static int kitty_present(void *opaque, const struct winetk_frame *frame,
                         const struct winetk_viewport *viewport)
{
    struct kitty_state *state = opaque;
    unsigned char raw[RAW_CHUNK_SIZE];
    char encoded[ENCODED_CHUNK_SIZE];
    char command[256];
    char packet[ENCODED_CHUNK_SIZE+256+2];
    size_t row_size, total_size, consumed = 0;
    uint32_t row = 0;
    size_t row_offset = 0;
    bool first = true;
    bool compressed=false;
    size_t payload_size;

    if (check_output(state) ||
        validate_frame(frame, viewport, &row_size, &total_size))
        return -1;
    int shared = send_shared_frame(state, frame, row_size, total_size);
    if (shared < 0) return -1;
    if (shared) goto place;

    payload_size=total_size;
    if (state->compression && total_size>=65536) {
        const unsigned char *source=frame->pixels;
        unsigned char *packed=NULL;
        uLongf capacity=compressBound((uLong)total_size), length=capacity;
        if (frame->stride!=row_size) {
            packed=malloc(total_size);
            if (!packed) return -1;
            for (uint32_t y=0;y<frame->height;++y)
                memcpy(packed+(size_t)y*row_size,frame->pixels+(size_t)y*frame->stride,row_size);
            source=packed;
        }
        if (capacity>state->compressed_capacity) {
            unsigned char *buffer=realloc(state->compressed,capacity);
            if (!buffer) { free(packed); return -1; }
            state->compressed=buffer; state->compressed_capacity=capacity;
        }
        int result=compress2(state->compressed,&length,source,(uLong)total_size,Z_BEST_SPEED);
        free(packed);
        if (result!=Z_OK) { errno=EIO; return -1; }
        if (length<total_size) { compressed=true; payload_size=length; }
    }

    state->image_touched = true;
    state->transferring = true;
    while (consumed < payload_size) {
        size_t chunk_size = payload_size - consumed;
        size_t filled = 0, encoded_size;
        int command_size;
        unsigned more;
        if (chunk_size > sizeof(raw))
            chunk_size = sizeof(raw);
        if (compressed) {
            memcpy(raw,state->compressed+consumed,chunk_size);
            filled=chunk_size;
        }
        while (filled < chunk_size) {
            size_t copy_size = row_size - row_offset;
            if (copy_size > chunk_size - filled)
                copy_size = chunk_size - filled;
            memcpy(raw + filled,
                   frame->pixels + (size_t)row * frame->stride + row_offset,
                   copy_size);
            filled += copy_size;
            row_offset += copy_size;
            if (row_offset == row_size) {
                ++row;
                row_offset = 0;
            }
        }
        consumed += chunk_size;
        more = consumed < payload_size;
        encoded_size = encode_base64(raw, chunk_size, encoded);
        if (first) {
            command_size = snprintf(command, sizeof(command),
                "\033_Ga=t,t=d,f=32,s=%" PRIu32 ",v=%" PRIu32
                ",i=%" PRIu32 ",q=2%s,m=%u;", frame->width,
                frame->height, state->image_id, compressed ? ",o=z" : "", more);
        } else {
            command_size = snprintf(command, sizeof(command),
                                    "\033_Gm=%u,q=2;", more);
        }
        if (command_size < 0 || (size_t)command_size >= sizeof(command)) {
            errno = EOVERFLOW;
            return output_failed(state);
        }
        memcpy(packet,command,(size_t)command_size);
        memcpy(packet+command_size,encoded,encoded_size);
        memcpy(packet+command_size+encoded_size,"\033\\",2);
        if (write_all(state,packet,(size_t)command_size+encoded_size+2))
            return output_failed(state);
        first = false;
    }
    state->transferring = false;

place:
    /* Re-transmission with the same image ID deletes its old placements.
     * Always place the complete new image, at native pixel size. C=1 avoids
     * cursor movement/scrolling after the explicit cell positioning. */
    {
        int size = snprintf(command, sizeof(command),
            "\033[%" PRIu32 ";%" PRIu32 "H"
            "\033_Ga=p,i=%" PRIu32 ",p=%" PRIu32
            ",X=%" PRIu32 ",Y=%" PRIu32 ",C=1,q=2\033\\",
            viewport->row + 1, viewport->column + 1,
            state->image_id, state->placement_id,
            viewport->pixel_x, viewport->pixel_y);
        if (size < 0 || (size_t)size >= sizeof(command)) {
            errno = EOVERFLOW;
            return output_failed(state);
        }
        if (write_all(state, command, (size_t)size))
            return output_failed(state);
    }
    return 0;
}

static int kitty_close(void *opaque)
{
    struct kitty_state *state = opaque;
    char command[96];
    int saved_error = state->output_error;
    if (saved_error) {
        /* Also terminate a possible partial query or placement command. */
        (void)write_all(state, "\033\\", 2);
    }
    if (state->image_touched) {
        /* A failed sink may have stopped inside an APC or a multi-chunk
         * image. Best-effort termination allows the scoped delete to parse.
         * No graphics command is interleaved during successful operation. */
        if (state->transferring) {
            static const char finish[] = "\033_Gm=0,q=2;\033\\";
            if (write_all(state, finish, sizeof(finish) - 1) && !saved_error)
                saved_error = errno;
        }
        {
            int size = snprintf(command, sizeof(command),
                "\033_Ga=d,d=I,i=%" PRIu32 ",q=2\033\\", state->image_id);
            if (size < 0 || (size_t)size >= sizeof(command)) {
                if (!saved_error)
                    saved_error = EOVERFLOW;
            } else if (write_all(state, command, (size_t)size) && !saved_error) {
                saved_error = errno;
            }
        }
    }
    remove_shared_name(state->shared_probe);
    for (unsigned slot = 0; slot < SHARED_FRAME_LIMIT; ++slot)
        remove_shared_name(state->shared_frames[slot]);
    free(state->compressed);
    free(state);
    if (saved_error) {
        errno = saved_error;
        return -1;
    }
    return 0;
}

static const struct winetk_backend_ops kitty_ops = {
    .probe = kitty_probe,
    .present = kitty_present,
    .close = kitty_close,
};

int winetk_kitty_init(struct winetk_backend *backend, winetk_write_fn write_fn,
                     void *write_context, uint32_t image_id,
                     uint32_t placement_id)
{
    struct kitty_state *state;
    if (!backend || backend->ops || backend->state || !write_fn ||
        !image_id || !placement_id) {
        errno = EINVAL;
        return -1;
    }
    state = calloc(1, sizeof(*state));
    if (!state)
        return -1;
    state->write_fn = write_fn;
    state->write_context = write_context;
    state->image_id = image_id;
    state->placement_id = placement_id;
    backend->state = state;
    backend->ops = &kitty_ops;
    return 0;
}

int winetk_kitty_compress(struct winetk_backend *backend, int enabled)
{
    if (!backend || backend->ops!=&kitty_ops || !backend->state) { errno=EINVAL; return -1; }
    ((struct kitty_state *)backend->state)->compression=!!enabled;
    return 0;
}

int winetk_kitty_probe_shared(struct winetk_backend *backend, uint32_t query_id)
{
    static const unsigned char pixels[] = {0, 0, 0, 255};
    const struct winetk_frame frame = {pixels, sizeof(pixels), 1, 1, 4};
    struct kitty_state *state;
    char encoded[SHARED_NAME_SIZE * 2], command[SHARED_NAME_SIZE * 2 + 128];
    size_t encoded_size;
    int size;
    if (!backend || backend->ops != &kitty_ops || !backend->state || !query_id) {
        errno = EINVAL;
        return -1;
    }
    state = backend->state;
    if (check_output(state)) return -1;
    reclaim_shared_name(state->shared_probe);
    if (state->shared_probe[0]) { errno = EBUSY; return -1; }
    if (create_shared_frame(state, state->shared_probe, &frame, 4, 4)) return -1;
    encoded_size = encode_base64((const unsigned char *)state->shared_probe,
                                 strlen(state->shared_probe), encoded);
    size = snprintf(command, sizeof(command),
                    "\033_Ga=q,t=s,f=32,s=1,v=1,i=%" PRIu32 ";", query_id);
    if (size < 0 || (size_t)size + encoded_size + 2 > sizeof(command)) {
        remove_shared_name(state->shared_probe);
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(command + size, encoded, encoded_size);
    memcpy(command + size + encoded_size, "\033\\", 2);
    if (write_all(state, command, (size_t)size + encoded_size + 2)) return output_failed(state);
    return 0;
}

int winetk_kitty_shared_memory(struct winetk_backend *backend, int enabled)
{
    if (!backend || backend->ops != &kitty_ops || !backend->state) { errno = EINVAL; return -1; }
    ((struct kitty_state *)backend->state)->shared_memory = !!enabled;
    return 0;
}

int winetk_backend_probe(struct winetk_backend *backend)
{
    if (!backend || !backend->ops || !backend->ops->probe || !backend->state) {
        errno = EINVAL;
        return -1;
    }
    return backend->ops->probe(backend->state);
}

int winetk_backend_present(struct winetk_backend *backend,
                           const struct winetk_frame *frame,
                           const struct winetk_viewport *viewport)
{
    if (!backend || !backend->ops || !backend->ops->present || !backend->state) {
        errno = EINVAL;
        return -1;
    }
    return backend->ops->present(backend->state, frame, viewport);
}

int winetk_backend_close(struct winetk_backend *backend)
{
    int result;
    if (!backend) {
        errno = EINVAL;
        return -1;
    }
    if (!backend->ops && !backend->state)
        return 0;
    if (!backend->ops || !backend->ops->close || !backend->state) {
        errno = EINVAL;
        return -1;
    }
    result = backend->ops->close(backend->state);
    backend->ops = NULL;
    backend->state = NULL;
    return result;
}
