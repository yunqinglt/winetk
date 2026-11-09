#define _POSIX_C_SOURCE 200809L
#include "winetk/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", \
                __FILE__, __LINE__, #expression, errno); \
        exit(1); \
    } \
} while (0)

struct sink {
    char *data;
    size_t size;
    size_t capacity;
    size_t max_write;
    size_t fail_after;
    unsigned calls;
    unsigned interrupt_call;
    unsigned fail_call;
    int zero_write;
    int oversized_write;
};

static ssize_t sink_write(void *opaque, const void *data, size_t size)
{
    struct sink *sink = opaque;
    size_t wanted;
    ++sink->calls;
    if (sink->interrupt_call == sink->calls) {
        errno = EINTR;
        return -1;
    }
    if (sink->fail_call == sink->calls) {
        errno = ENOSPC;
        return -1;
    }
    if (sink->zero_write)
        return 0;
    if (sink->oversized_write)
        return (ssize_t)(size + 1);
    if (sink->fail_after && sink->size >= sink->fail_after) {
        errno = ENOSPC;
        return -1;
    }
    if (sink->max_write && size > sink->max_write)
        size = sink->max_write;
    if (sink->fail_after && size > sink->fail_after - sink->size)
        size = sink->fail_after - sink->size;
    wanted = sink->size + size + 1;
    if (wanted > sink->capacity) {
        size_t capacity = sink->capacity ? sink->capacity : 256;
        char *allocation;
        while (capacity < wanted)
            capacity *= 2;
        allocation = realloc(sink->data, capacity);
        CHECK(allocation);
        sink->data = allocation;
        sink->capacity = capacity;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    sink->data[sink->size] = 0;
    return (ssize_t)size;
}

static void sink_reset(struct sink *sink)
{
    free(sink->data);
    memset(sink, 0, sizeof(*sink));
}

static unsigned decode_digit(char value)
{
    if (value >= 'A' && value <= 'Z')
        return (unsigned)(value - 'A');
    if (value >= 'a' && value <= 'z')
        return (unsigned)(value - 'a') + 26;
    if (value >= '0' && value <= '9')
        return (unsigned)(value - '0') + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    CHECK(0);
    return 0;
}

/* Independent decoder deliberately verifies chunk boundaries and padding. */
static size_t decode_base64(const char *data, size_t size,
                            unsigned char *output)
{
    size_t offset, used = 0;
    CHECK(size && size <= 4096 && size % 4 == 0);
    for (offset = 0; offset < size; offset += 4) {
        unsigned a = decode_digit(data[offset]);
        unsigned b = decode_digit(data[offset + 1]);
        unsigned c = data[offset + 2] == '=' ? 0 : decode_digit(data[offset + 2]);
        unsigned d = data[offset + 3] == '=' ? 0 : decode_digit(data[offset + 3]);
        unsigned value = (a << 18) | (b << 12) | (c << 6) | d;
        output[used++] = (unsigned char)(value >> 16);
        if (data[offset + 2] != '=')
            output[used++] = (unsigned char)(value >> 8);
        if (data[offset + 3] != '=')
            output[used++] = (unsigned char)value;
        if (data[offset + 2] == '=') {
            CHECK(data[offset + 3] == '=' && (b & 15) == 0);
            CHECK(offset + 4 == size);
        } else if (data[offset + 3] == '=') {
            CHECK((c & 3) == 0 && offset + 4 == size);
        }
    }
    return used;
}

static size_t decode_frame_payload(const struct sink *sink, uint32_t width,
                                  uint32_t height, unsigned char *decoded,
                                  unsigned *chunks, int compressed)
{
    const char *cursor = sink->data;
    size_t used = 0;
    int more = 1;
    *chunks = 0;
    while (more) {
        const char *separator, *end;
        char expected[160];
        size_t command_size, payload_size;
        CHECK(cursor && strncmp(cursor, "\033_G", 3) == 0);
        separator = strchr(cursor, ';');
        end = strstr(cursor, "\033\\");
        CHECK(separator && end && separator < end);
        command_size = (size_t)(separator - cursor) + 1;
        payload_size = (size_t)(end - separator - 1);
        more = strstr(cursor, "m=1;") == separator - 3 ||
               strncmp(cursor, "\033_Gm=1,q=2;", 11) == 0;
        if (!*chunks) {
            snprintf(expected, sizeof(expected),
                     "\033_Ga=t,t=d,f=32,s=%u,v=%u,i=73,q=2%s,m=%d;",
                     width, height, compressed ? ",o=z" : "", more);
        } else {
            snprintf(expected, sizeof(expected), "\033_Gm=%d,q=2;", more);
        }
        CHECK(strlen(expected) == command_size);
        CHECK(memcmp(cursor, expected, command_size) == 0);
        if (more)
            CHECK(payload_size == 4096 && !memchr(separator + 1, '=', payload_size));
        used += decode_base64(separator + 1, payload_size, decoded + used);
        cursor = end + 2;
        ++*chunks;
    }
    CHECK(strcmp(cursor,
        "\033[3;4H\033_Ga=p,i=73,p=19,X=2,Y=5,C=1,q=2\033\\") == 0);
    return used;
}

static size_t decode_frame(const struct sink *sink, uint32_t width,
                          uint32_t height, unsigned char *decoded,
                          unsigned *chunks)
{
    return decode_frame_payload(sink, width, height, decoded, chunks, 0);
}

static uint32_t random_bits(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void test_compressed_padded_multichunk(void)
{
    const uint32_t width = 257, height = 129;
    const size_t row_size = (size_t)width * 4, stride = row_size + 13;
    const size_t raw_size = row_size * height, allocation_size = stride * height + 7;
    unsigned char *pixels = malloc(allocation_size);
    unsigned char *expected = malloc(raw_size);
    unsigned char *payload = malloc(raw_size);
    unsigned char *decoded = malloc(raw_size);
    struct winetk_frame frame = {pixels ? pixels + 7 : NULL,
                                 allocation_size - 7, width, height, stride};
    const struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    unsigned chunks;
    uint32_t random_state = 0x82438231;
    size_t compressed_size;
    uLongf decoded_size = (uLongf)raw_size;
    CHECK(pixels && expected && payload && decoded);
    memset(pixels, 0xa5, allocation_size);
    /* Varying noise islands make the frame compressible while keeping its
     * compressed payload larger than one protocol chunk. Non-pixel stride
     * padding and a cropped origin must never enter the decoded image. */
    for (uint32_t row = 0; row < height; ++row) {
        for (size_t column = 0; column < row_size; ++column) {
            unsigned char value = column % 64 < 16
                ? (unsigned char)random_bits(&random_state)
                : (unsigned char)(0x31 + column / 64 % 4);
            pixels[7 + (size_t)row * stride + column] = value;
            expected[(size_t)row * row_size + column] = value;
        }
    }
    sink.max_write = 37;
    sink.interrupt_call = 7;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_compress(&backend, 1) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    compressed_size = decode_frame_payload(&sink, width, height, payload, &chunks, 1);
    CHECK(compressed_size > 3072 && compressed_size < raw_size && chunks > 1);
    CHECK(uncompress(decoded, &decoded_size, payload, (uLong)compressed_size) == Z_OK);
    CHECK(decoded_size == raw_size && memcmp(decoded, expected, raw_size) == 0);

    /* The same backend can opt back out: retained compression storage must
     * not change the raw transfer or its frame replacement semantics. */
    sink_reset(&sink);
    CHECK(winetk_kitty_compress(&backend, 0) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(decode_frame(&sink, width, height, decoded, &chunks) == raw_size);
    CHECK(memcmp(decoded, expected, raw_size) == 0);
    CHECK(winetk_backend_close(&backend) == 0);
    sink_reset(&sink);

    /* Incompressible pixels must fall back to ordinary raw RGBA even when
     * compression is enabled; they must not be sent with an o=z header. */
    for (size_t index = 0; index < raw_size; ++index)
        expected[index] = (unsigned char)random_bits(&random_state);
    frame.pixels = expected;
    frame.size = raw_size;
    frame.stride = row_size;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_compress(&backend, 1) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(decode_frame(&sink, width, height, decoded, &chunks) == raw_size);
    CHECK(memcmp(decoded, expected, raw_size) == 0);
    CHECK(winetk_backend_close(&backend) == 0);
    CHECK(winetk_kitty_compress(&backend, 1) == -1 && errno == EINVAL);
    sink_reset(&sink);
    free(decoded);
    free(payload);
    free(expected);
    free(pixels);
}

static void test_probe_cleanup(void)
{
    static const char expected[] =
        "\033_Ga=q,t=d,f=32,s=1,v=1,i=73;AAAA/w==\033\\\033[c";
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_backend_probe(&backend) == 0);
    CHECK(sink.size == sizeof(expected) - 1);
    CHECK(memcmp(sink.data, expected, sink.size) == 0);
    CHECK(winetk_backend_close(&backend) == 0);
    /* a=q never stores or replaces an image, so there is nothing to delete. */
    CHECK(sink.size == sizeof(expected) - 1);
    CHECK(winetk_backend_close(&backend) == 0);
    sink_reset(&sink);
}

static void test_stride_crop_and_padding(void)
{
    unsigned char pixels[96], expected[24], decoded[24];
    struct winetk_frame frame = {pixels + 28, sizeof(pixels) - 28, 3, 2, 24};
    struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    unsigned chunks;
    size_t i;
    for (i = 0; i < sizeof(pixels); ++i)
        pixels[i] = (unsigned char)(i * 43 + 9);
    memcpy(expected, pixels + 28, 12);
    memcpy(expected + 12, pixels + 52, 12);
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(decode_frame(&sink, 3, 2, decoded, &chunks) == sizeof(decoded));
    CHECK(chunks == 1 && memcmp(decoded, expected, sizeof(decoded)) == 0);
    {
        size_t old_size = sink.size;
        static const char cleanup[] = "\033_Ga=d,d=I,i=73,q=2\033\\";
        CHECK(winetk_backend_close(&backend) == 0);
        CHECK(sink.size == old_size + sizeof(cleanup) - 1);
        CHECK(memcmp(sink.data + old_size, cleanup, sizeof(cleanup) - 1) == 0);
        CHECK(!strstr(sink.data, "d=A") && !strstr(sink.data, "d=a"));
    }
    sink_reset(&sink);

    /* Four, eight, and twelve raw bytes exercise every base64 tail case. */
    for (i = 1; i <= 3; ++i) {
        struct winetk_frame small = {pixels, sizeof(pixels), 1, (uint32_t)i, 4};
        CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
        CHECK(winetk_backend_present(&backend, &small, &viewport) == 0);
        CHECK(decode_frame(&sink, 1, (uint32_t)i, decoded, &chunks) == i * 4);
        CHECK(memcmp(decoded, pixels, i * 4) == 0);
        CHECK(winetk_backend_close(&backend) == 0);
        sink_reset(&sink);
    }
}

static void test_chunks_short_writes_and_replacement(void)
{
    unsigned char pixels[1036 * 7], expected[1028 * 7], decoded[1028 * 7];
    struct winetk_frame frame = {pixels, sizeof(pixels), 257, 7, 1036};
    struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink normal = {0}, partial = {0};
    size_t i, normal_size;
    unsigned chunks;
    for (i = 0; i < sizeof(pixels); ++i)
        pixels[i] = (unsigned char)(i * 13);
    for (i = 0; i < 7; ++i)
        memcpy(expected + i * 1028, pixels + i * 1036, 1028);
    CHECK(winetk_kitty_init(&backend, sink_write, &normal, 73, 19) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(decode_frame(&normal, 257, 7, decoded, &chunks) == sizeof(decoded));
    CHECK(chunks == 3 && memcmp(decoded, expected, sizeof(decoded)) == 0);
    normal_size = normal.size;
    /* A replacement repeats precisely the same ID and placement, with no
     * global deletion or other commands inside the image chunk stream. */
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(normal.size == normal_size * 2);
    CHECK(memcmp(normal.data, normal.data + normal_size, normal_size) == 0);
    CHECK(winetk_backend_close(&backend) == 0);

    partial.max_write = 7;
    partial.interrupt_call = 4;
    CHECK(winetk_kitty_init(&backend, sink_write, &partial, 73, 19) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(partial.size == normal_size);
    CHECK(memcmp(partial.data, normal.data, normal_size) == 0);
    CHECK(winetk_backend_close(&backend) == 0);
    sink_reset(&normal);
    sink_reset(&partial);
}

static void test_invalid_frames(void)
{
    const unsigned char pixels[8] = {0};
    struct winetk_frame frame = {pixels, sizeof(pixels), 1, 2, 4};
    struct winetk_frame invalid;
    struct winetk_viewport viewport = {0};
    struct winetk_viewport invalid_viewport;
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
#define INVALID_FRAME(member, value, error) do { \
    invalid = frame; invalid.member = value; errno = 0; \
    CHECK(winetk_backend_present(&backend, &invalid, &viewport) == -1); \
    CHECK(errno == error && sink.size == 0); \
} while (0)
    INVALID_FRAME(pixels, NULL, EINVAL);
    INVALID_FRAME(width, 0, EINVAL);
    INVALID_FRAME(height, 0, EINVAL);
    INVALID_FRAME(stride, 3, EINVAL);
    INVALID_FRAME(size, 7, EINVAL);
    INVALID_FRAME(stride, SIZE_MAX, EOVERFLOW);
#undef INVALID_FRAME
    invalid = frame;
    invalid.width = UINT32_MAX;
    invalid.height = UINT32_MAX;
    invalid.stride = SIZE_MAX;
    errno = 0;
    CHECK(winetk_backend_present(&backend, &invalid, &viewport) == -1);
    CHECK(errno == EOVERFLOW && sink.size == 0);
    invalid_viewport = viewport;
    invalid_viewport.row = UINT32_MAX;
    errno = 0;
    CHECK(winetk_backend_present(&backend, &frame, &invalid_viewport) == -1);
    CHECK(errno == EOVERFLOW && sink.size == 0);
    CHECK(winetk_backend_present(&backend, NULL, &viewport) == -1 && errno == EINVAL);
    CHECK(winetk_backend_present(&backend, &frame, NULL) == -1 && errno == EINVAL);
    CHECK(winetk_backend_close(&backend) == 0 && sink.size == 0);
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 0, 19) == -1 && errno == EINVAL);
    CHECK(winetk_kitty_init(&backend, NULL, &sink, 73, 19) == -1 && errno == EINVAL);
    CHECK(winetk_backend_probe(&backend) == -1 && errno == EINVAL);
    sink_reset(&sink);
}

static void test_write_failures(void)
{
    const unsigned char pixels[8] = {0};
    const struct winetk_frame frame = {pixels, sizeof(pixels), 1, 2, 4};
    const struct winetk_viewport viewport = {0};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    unsigned calls;
    size_t failed_size;
    sink.fail_after = 45; /* Inside a transmission, not a frame validation. */
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == -1);
    CHECK(errno == ENOSPC);
    calls = sink.calls;
    failed_size = sink.size;
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == -1 && errno == ENOSPC);
    CHECK(winetk_backend_probe(&backend) == -1 && errno == ENOSPC);
    CHECK(sink.calls == calls && sink.size == failed_size);
    sink.fail_after = 0; /* Simulate a recovered sink for best-effort cleanup. */
    CHECK(winetk_backend_close(&backend) == -1 && errno == ENOSPC);
    CHECK(strcmp(sink.data + failed_size,
          "\033\\\033_Gm=0,q=2;\033\\\033_Ga=d,d=I,i=73,q=2\033\\") == 0);
    CHECK(!backend.ops && !backend.state);
    CHECK(winetk_backend_close(&backend) == 0);
    sink_reset(&sink);

    sink.zero_write = 1;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_backend_probe(&backend) == -1 && errno == EIO);
    CHECK(winetk_backend_close(&backend) == -1 && errno == EIO);
    sink_reset(&sink);
    sink.oversized_write = 1;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_backend_probe(&backend) == -1 && errno == EIO);
    CHECK(winetk_backend_close(&backend) == -1 && errno == EIO);
    sink_reset(&sink);
}

static void decode_shared_name(const struct sink *sink, int query, uint32_t id,
                               uint32_t width, uint32_t height, char name[96],
                               int placed)
{
    char expected[160];
    const char *separator, *end;
    unsigned char decoded[96];
    size_t length;
    CHECK(sink->data);
    separator = strchr(sink->data, ';');
    end = strstr(sink->data, "\033\\");
    CHECK(separator && end && separator < end);
    if (query)
        snprintf(expected, sizeof(expected),
                 "\033_Ga=q,t=s,f=32,s=%u,v=%u,i=%u;", width, height, id);
    else
        snprintf(expected, sizeof(expected),
                 "\033_Ga=t,t=s,f=32,s=%u,v=%u,i=%u,q=2;", width, height, id);
    CHECK((size_t)(separator - sink->data + 1) == strlen(expected));
    CHECK(!memcmp(sink->data, expected, strlen(expected)));
    CHECK((size_t)(end - separator - 1) <= 128);
    length = decode_base64(separator + 1, (size_t)(end - separator - 1), decoded);
    CHECK(length > 8 && length < 96 && !memcmp(decoded, "/winetk-", 8));
    memcpy(name, decoded, length);
    name[length] = 0;
    CHECK(!strchr(name + 1, '/'));
    if (placed)
        CHECK(!strcmp(end + 2,
              "\033[3;4H\033_Ga=p,i=73,p=19,X=2,Y=5,C=1,q=2\033\\"));
    else CHECK(!end[2]);
}

static void check_shared_pixels(const char *name, const unsigned char *expected,
                                size_t size)
{
    struct stat status;
    void *mapping;
    int fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    CHECK(!fstat(fd, &status));
    CHECK(status.st_uid == geteuid() && (status.st_mode & 0777) == 0600);
    CHECK(status.st_size >= 0 && (uintmax_t)status.st_size == size);
    mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(!memcmp(mapping, expected, size));
    CHECK(!munmap(mapping, size));
    CHECK(!close(fd));
}

static void check_shared_absent(const char *name)
{
    int fd;
    errno = 0;
    fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    CHECK(fd == -1 && errno == ENOENT);
}

static void test_shared_probe_cleanup(void)
{
    static const unsigned char pixel[] = {0, 0, 0, 255};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    char first[96], second[96];
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_probe_shared(&backend, 0) == -1 && errno == EINVAL);
    CHECK(winetk_kitty_probe_shared(&backend, 83) == 0);
    decode_shared_name(&sink, 1, 83, 1, 1, first, 0);
    check_shared_pixels(first, pixel, sizeof(pixel));
    size_t previous = sink.size;
    CHECK(winetk_kitty_probe_shared(&backend, 84) == -1 && errno == EBUSY);
    CHECK(sink.size == previous);
    CHECK(!shm_unlink(first)); /* The terminal consumes and unlinks the query. */
    sink_reset(&sink);
    CHECK(winetk_kitty_probe_shared(&backend, 84) == 0);
    decode_shared_name(&sink, 1, 84, 1, 1, second, 0);
    CHECK(strcmp(first, second));
    check_shared_pixels(second, pixel, sizeof(pixel));
    previous = sink.size;
    CHECK(winetk_backend_close(&backend) == 0 && sink.size == previous);
    check_shared_absent(first);
    check_shared_absent(second);
    CHECK(winetk_kitty_probe_shared(&backend, 83) == -1 && errno == EINVAL);
    CHECK(winetk_kitty_shared_memory(&backend, 1) == -1 && errno == EINVAL);
    sink_reset(&sink);
}

static void test_shared_stride_reuse_cleanup(void)
{
    unsigned char pixels[96], expected[24];
    struct winetk_frame frame = {pixels + 28, sizeof(pixels) - 28, 3, 2, 24};
    const struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    char first[96], second[96];
    for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = (unsigned char)(i * 43 + 9);
    memcpy(expected, pixels + 28, 12);
    memcpy(expected + 12, pixels + 52, 12);
    sink.max_write = 7;
    sink.interrupt_call = 4;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_shared_memory(&backend, 1) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    decode_shared_name(&sink, 0, 73, 3, 2, first, 1);
    check_shared_pixels(first, expected, sizeof(expected));
    CHECK(!shm_unlink(first));
    sink_reset(&sink);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    decode_shared_name(&sink, 0, 73, 3, 2, second, 1);
    CHECK(strcmp(first, second));
    check_shared_pixels(second, expected, sizeof(expected));
    CHECK(winetk_backend_close(&backend) == 0);
    check_shared_absent(first);
    check_shared_absent(second);
    sink_reset(&sink);
}

static void test_shared_bounded_fallback(void)
{
    enum { WIDTH = 129, HEIGHT = 129, SIZE = WIDTH * HEIGHT * 4 };
    unsigned char *pixels = malloc(SIZE), *payload = malloc(SIZE), *decoded = malloc(SIZE);
    struct winetk_frame frame = {pixels, SIZE, WIDTH, HEIGHT, WIDTH * 4};
    const struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    char names[5][96];
    unsigned chunks;
    uLongf length = SIZE;
    CHECK(pixels && payload && decoded);
    memset(pixels, 0x5a, SIZE);
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_shared_memory(&backend, 1) == 0);
    CHECK(winetk_kitty_compress(&backend, 1) == 0);
    for (unsigned slot = 0; slot < 4; ++slot) {
        CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
        decode_shared_name(&sink, 0, 73, WIDTH, HEIGHT, names[slot], 1);
        check_shared_pixels(names[slot], pixels, SIZE);
        for (unsigned earlier = 0; earlier < slot; ++earlier)
            CHECK(strcmp(names[slot], names[earlier]));
        sink_reset(&sink);
    }
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    size_t compressed = decode_frame_payload(&sink, WIDTH, HEIGHT, payload, &chunks, 1);
    CHECK(uncompress(decoded, &length, payload, (uLong)compressed) == Z_OK);
    CHECK(length == SIZE && !memcmp(decoded, pixels, SIZE));
    for (unsigned slot = 0; slot < 4; ++slot) check_shared_pixels(names[slot], pixels, SIZE);
    CHECK(!shm_unlink(names[1]));
    sink_reset(&sink);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    decode_shared_name(&sink, 0, 73, WIDTH, HEIGHT, names[4], 1);
    for (unsigned slot = 0; slot < 4; ++slot) CHECK(strcmp(names[4], names[slot]));
    check_shared_pixels(names[4], pixels, SIZE);
    CHECK(winetk_backend_close(&backend) == 0);
    for (unsigned slot = 0; slot < 5; ++slot) check_shared_absent(names[slot]);
    sink_reset(&sink);
    free(decoded);
    free(payload);
    free(pixels);
}

static void test_shared_syscall_fallback(void)
{
    const unsigned char pixels[8] = {0, 1, 2, 255, 3, 4, 5, 255};
    unsigned char decoded[8];
    const struct winetk_frame frame = {pixels, sizeof(pixels), 1, 2, 4};
    const struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    struct rlimit original, blocked;
    unsigned chunks;
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_shared_memory(&backend, 1) == 0);
    CHECK(!getrlimit(RLIMIT_NOFILE, &original));
    blocked = original;
    blocked.rlim_cur = 0; /* Force shm_open to fail without filling real tmpfs. */
    CHECK(!setrlimit(RLIMIT_NOFILE, &blocked));
    CHECK(winetk_kitty_probe_shared(&backend, 83) == -1 && errno == EMFILE);
    CHECK(!sink.size);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == 0);
    CHECK(!setrlimit(RLIMIT_NOFILE, &original));
    CHECK(decode_frame(&sink, 1, 2, decoded, &chunks) == sizeof(pixels));
    CHECK(!memcmp(decoded, pixels, sizeof(pixels)));
    CHECK(winetk_backend_close(&backend) == 0);
    sink_reset(&sink);
}

static void test_shared_output_failure_cleanup(void)
{
    const unsigned char pixels[8] = {0};
    const struct winetk_frame frame = {pixels, sizeof(pixels), 1, 2, 4};
    const struct winetk_viewport viewport = {3, 2, 2, 5};
    struct winetk_backend backend = {0};
    struct sink sink = {0};
    char name[96];
    sink.fail_call = 2; /* SHM command succeeds, its placement write fails. */
    CHECK(winetk_kitty_init(&backend, sink_write, &sink, 73, 19) == 0);
    CHECK(winetk_kitty_shared_memory(&backend, 1) == 0);
    CHECK(winetk_backend_present(&backend, &frame, &viewport) == -1 && errno == ENOSPC);
    decode_shared_name(&sink, 0, 73, 1, 2, name, 0);
    check_shared_pixels(name, pixels, sizeof(pixels));
    CHECK(winetk_backend_close(&backend) == -1 && errno == ENOSPC);
    check_shared_absent(name);
    sink_reset(&sink);
}

int main(void)
{
    test_probe_cleanup();
    test_stride_crop_and_padding();
    test_chunks_short_writes_and_replacement();
    test_compressed_padded_multichunk();
    test_invalid_frames();
    test_write_failures();
    test_shared_probe_cleanup();
    test_shared_stride_reuse_cleanup();
    test_shared_bounded_fallback();
    test_shared_syscall_fallback();
    test_shared_output_failure_cleanup();
    puts("kitty backend tests passed");
    return 0;
}
