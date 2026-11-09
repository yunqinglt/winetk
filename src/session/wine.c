#define _GNU_SOURCE
#include "winetk/wine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_LIMIT 8u
#define WINDOW_LIMIT 64u
#define SURFACE_BUDGET (64u * 1024u * 1024u)
#define OUTPUT_LIMIT (64u * 1024u)
#define UPDATE_BUDGET (16u * 1024u * 1024u)
#define MESSAGE_BUDGET 128u
#define MAX_WIDTH 1920u
#define MAX_HEIGHT 1080u
#define MAX_PAYLOAD (sizeof(struct wt_wine_window) + MAX_WIDTH * MAX_HEIGHT * 4u)

struct wine_client {
    int fd;
    uint32_t pid;
    struct wt_wine_header header;
    size_t header_used, payload_used;
    unsigned char *payload;
    unsigned char output[OUTPUT_LIMIT];
    size_t output_used;
};

struct wine_window {
    bool used, gpu;
    unsigned client;
    struct wt_wine_window info;
    unsigned char *pixels;
    uint32_t pixel_width, pixel_height;
    int64_t pixel_x, pixel_y;
    size_t pixel_size;
    uint64_t sequence;
};

struct wt_wine_server {
    int fd;
    char *path;
    ino_t socket_inode;
    dev_t socket_device;
    uint32_t width, height;
    uint32_t follow_pid;
    int32_t origin_x, origin_y;
    unsigned char *pixels;
    struct wine_client clients[CLIENT_LIMIT];
    struct wine_window windows[WINDOW_LIMIT];
    size_t surface_bytes;
    uint64_t next_sequence, focused_hwnd;
    unsigned focused_client;
    bool dirty;
};

static void remove_window(struct wt_wine_server *server,
                           struct wine_window *window)
{
    if (server->focused_hwnd == window->info.hwnd &&
        server->focused_client == window->client)
        server->focused_hwnd = 0;
    server->surface_bytes -= window->pixel_size;
    free(window->pixels);
    memset(window, 0, sizeof(*window));
    server->dirty = true;
}

static void remove_client(struct wt_wine_server *server, unsigned index)
{
    struct wine_client *client = &server->clients[index];
    unsigned i;
    if (client->fd >= 0)
        close(client->fd);
    free(client->payload);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    for (i = 0; i < WINDOW_LIMIT; ++i)
        if (server->windows[i].used && server->windows[i].client == index)
            remove_window(server, &server->windows[i]);
}

static struct wine_window *find_window(struct wt_wine_server *server,
                                        unsigned client, uint64_t hwnd)
{
    unsigned i;
    for (i = 0; i < WINDOW_LIMIT; ++i)
        if (server->windows[i].used && server->windows[i].client == client &&
            server->windows[i].info.hwnd == hwnd)
            return &server->windows[i];
    return NULL;
}

static bool validate_header(const struct wt_wine_header *header)
{
    if (header->magic != WT_WINE_MAGIC || header->version != WT_WINE_VERSION)
        return false;
    switch (header->type) {
    case WT_WINE_FRAME:
        return header->size >= sizeof(struct wt_wine_window) + 4u &&
               header->size <= MAX_PAYLOAD;
    case WT_WINE_WINDOW:
        return header->size == sizeof(struct wt_wine_window);
    case WT_WINE_DESTROY:
        return header->size == sizeof(struct wt_wine_destroy);
    default:
        return false;
    }
}

static bool consume_message(struct wt_wine_server *server, unsigned index)
{
    struct wine_client *client = &server->clients[index];
    struct wine_window *window;
    struct wt_wine_window info;
    size_t pixel_size = 0;
    unsigned i;
    if (client->header.type == WT_WINE_DESTROY) {
        struct wt_wine_destroy destroy;
        memcpy(&destroy, client->payload, sizeof(destroy));
        if (!destroy.hwnd || destroy.reserved || destroy.pid != client->pid)
            return false;
        window = find_window(server, index, destroy.hwnd);
        if (window)
            remove_window(server, window);
        return true;
    }
    memcpy(&info, client->payload, sizeof(info));
    if (!info.hwnd || info.pid != client->pid ||
        info.width > (client->header.type==WT_WINE_WINDOW ? 2048u : MAX_WIDTH) ||
        info.height > (client->header.type==WT_WINE_WINDOW ? 1152u : MAX_HEIGHT) ||
        info.client_width > MAX_WIDTH || info.client_height > MAX_HEIGHT ||
        (info.flags & ~(WT_WINE_VISIBLE | WT_WINE_FOREGROUND | WT_WINE_GPU)))
        return false;
    if (client->header.type == WT_WINE_FRAME) {
        if (!info.width || !info.height)
            return false;
        pixel_size = (size_t)info.width * info.height * 4u;
        if (client->header.size != sizeof(info) + pixel_size)
            return false;
    }
    window = find_window(server, index, info.hwnd);
    if (!window) {
        for (i = 0; i < WINDOW_LIMIT && server->windows[i].used; ++i) {}
        if (i == WINDOW_LIMIT)
            return false;
        window = &server->windows[i];
        window->used = true;
        window->client = index;
        window->sequence = ++server->next_sequence;
    }
    if (pixel_size) {
        unsigned char *pixels;
        if (pixel_size > SURFACE_BUDGET - (server->surface_bytes - window->pixel_size))
            return false;
        pixels = malloc(pixel_size);
        if (!pixels)
            return false;
        memcpy(pixels, client->payload + sizeof(info), pixel_size);
        server->surface_bytes -= window->pixel_size;
        free(window->pixels);
        window->pixels = pixels;
        window->pixel_size = pixel_size;
        window->pixel_width = info.width;
        window->pixel_height = info.height;
        window->pixel_x = info.x;
        window->pixel_y = info.y;
        window->gpu = !!(info.flags & WT_WINE_GPU);
        server->surface_bytes += pixel_size;
    }
    if (client->header.type == WT_WINE_WINDOW) {
        if (window->pixels) {
            window->pixel_x+=(int64_t)info.client_x-window->info.client_x;
            window->pixel_y+=(int64_t)info.client_y-window->info.client_y;
        }
        window->sequence = ++server->next_sequence;
    }
    window->info = info;
    if ((info.flags & WT_WINE_VISIBLE) &&
        ((info.flags & WT_WINE_FOREGROUND) || !server->focused_hwnd)) {
        server->focused_client = index;
        server->focused_hwnd = info.hwnd;
    }
    server->dirty = true;
    return true;
}

static int read_client(struct wt_wine_server *server, unsigned index,
                        size_t *budget, unsigned *messages)
{
    struct wine_client *client = &server->clients[index];
    while (*budget && *messages) {
        unsigned char *destination;
        size_t size;
        ssize_t count;
        if (client->header_used < sizeof(client->header)) {
            destination = (unsigned char *)&client->header + client->header_used;
            size = sizeof(client->header) - client->header_used;
        } else {
            destination = client->payload + client->payload_used;
            size = client->header.size - client->payload_used;
        }
        if (size > *budget)
            size = *budget;
        count = recv(client->fd, destination, size, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!count)
            return -1;
        *budget -= (size_t)count;
        if (client->header_used < sizeof(client->header)) {
            client->header_used += (size_t)count;
            if (client->header_used != sizeof(client->header))
                continue;
            if (!validate_header(&client->header))
                return -1;
            client->payload = malloc(client->header.size);
            if (!client->payload)
                return -1;
        } else {
            client->payload_used += (size_t)count;
            if (client->payload_used != client->header.size)
                continue;
            if (!consume_message(server, index))
                return -1;
            free(client->payload);
            client->payload = NULL;
            client->payload_used = client->header_used = 0;
            --*messages;
        }
    }
    return 0;
}

static int flush_client(struct wine_client *client)
{
    while (client->output_used) {
        ssize_t count = send(client->fd, client->output, client->output_used, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!count) {
            errno = EPIPE;
            return -1;
        }
        client->output_used -= (size_t)count;
        memmove(client->output, client->output + count, client->output_used);
    }
    return 0;
}

static int compare_windows(const void *left, const void *right)
{
    const struct wine_window *a = *(const struct wine_window *const *)left;
    const struct wine_window *b = *(const struct wine_window *const *)right;
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence;
}

static int follow_window(struct wt_wine_server *server)
{
    const struct wine_window *target=NULL;
    uint64_t area=0;
    unsigned best=0;
    if (!server->follow_pid) return 0;
    for (unsigned i=0;i<WINDOW_LIMIT;++i) {
        const struct wine_window *window=&server->windows[i];
        uint64_t candidate=(uint64_t)window->info.client_width*window->info.client_height;
        unsigned priority=window->info.pid==server->follow_pid ? 2 : window->gpu ? 1 : 0;
        /* A launcher may have no window; follow its GPU child instead of the
         * bootstrap explorer desktop. Directly launched apps retain priority. */
        if (window->used && window->pixels && priority &&
            (window->info.flags&WT_WINE_VISIBLE) &&
            (priority>best || (priority==best && candidate>area))) {
            target=window; area=candidate; best=priority;
        }
    }
    if (!target) return 0;
    uint32_t width=target->info.client_width, height=target->info.client_height;
    if (width!=server->width || height!=server->height) {
        unsigned char *pixels=realloc(server->pixels,(size_t)width*height*4);
        if (!pixels) return -1;
        server->pixels=pixels; server->width=width; server->height=height;
    }
    server->origin_x=target->info.client_x;
    server->origin_y=target->info.client_y;
    return 0;
}

static void compose(struct wt_wine_server *server)
{
    const struct wine_window *ordered[WINDOW_LIMIT];
    size_t count = 0, offset;
    unsigned i;
    for (offset = 0; offset < (size_t)server->width * server->height * 4u; offset += 4) {
        server->pixels[offset] = 18;
        server->pixels[offset + 1] = 22;
        server->pixels[offset + 2] = 30;
        server->pixels[offset + 3] = 255;
    }
    for (i = 0; i < WINDOW_LIMIT; ++i)
        if (server->windows[i].used && server->windows[i].pixels &&
            (server->windows[i].info.flags & WT_WINE_VISIBLE))
            ordered[count++] = &server->windows[i];
    qsort(ordered, count, sizeof(ordered[0]), compare_windows);
    for (offset = 0; offset < count; ++offset) {
        const struct wine_window *window = ordered[offset];
        int64_t window_x=(int64_t)window->pixel_x-server->origin_x;
        int64_t window_y=(int64_t)window->pixel_y-server->origin_y;
        int64_t left = window_x, top = window_y;
        int64_t right = left + (window->pixel_width < window->info.width ?
                                 window->pixel_width : window->info.width);
        int64_t bottom = top + (window->pixel_height < window->info.height ?
                                  window->pixel_height : window->info.height);
        int64_t x, y;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > server->width) right = server->width;
        if (bottom > server->height) bottom = server->height;
        if (left >= right || top >= bottom)
            continue;
        for (y = top; y < bottom; ++y) {
            const unsigned char *source = window->pixels +
                ((size_t)(y - window_y) * window->pixel_width +
                 (size_t)(left - window_x)) * 4u;
            unsigned char *destination = server->pixels +
                ((size_t)y * server->width + (size_t)left) * 4u;
            for (x = left; x < right; ++x, source += 4, destination += 4) {
                unsigned alpha = source[3], component;
                for (component = 0; component < 3; ++component)
                    destination[component] = (unsigned char)(
                        (source[component] * alpha + destination[component] *
                         (255u - alpha) + 127u) / 255u);
            }
        }
    }
}

struct wt_wine_server *wt_wine_server_open(const char *path,
                                          uint32_t width, uint32_t height)
{
    struct wt_wine_server *server;
    struct sockaddr_un address = {0};
    struct stat status;
    char *parent, *slash;
    int saved_errno;
    unsigned i;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address.sun_path) ||
        !width || !height || width > MAX_WIDTH || height > MAX_HEIGHT) {
        errno = EINVAL;
        return NULL;
    }
    parent = strdup(path);
    if (!parent)
        return NULL;
    slash = strrchr(parent, '/');
    if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    if (lstat(parent, &status) < 0) {
        saved_errno = errno;
        free(parent);
        errno = saved_errno;
        return NULL;
    }
    free(parent);
    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077)) {
        errno = EACCES;
        return NULL;
    }
    if (lstat(path, &status) == 0) {
        errno = EEXIST;
        return NULL;
    }
    if (errno != ENOENT)
        return NULL;
    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->fd = -1;
    for (i = 0; i < CLIENT_LIMIT; ++i)
        server->clients[i].fd = -1;
    server->pixels = malloc((size_t)width * height * 4u);
    server->path = strdup(path);
    server->width = width;
    server->height = height;
    if (!server->pixels || !server->path)
        goto fail;
    server->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server->fd < 0)
        goto fail;
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(server->fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto fail;
    if (lstat(path, &status) < 0)
        goto fail;
    server->socket_inode = status.st_ino;
    server->socket_device = status.st_dev;
    if (chmod(path, 0600) < 0 || listen(server->fd, CLIENT_LIMIT) < 0)
        goto fail;
    compose(server);
    return server;
fail:
    saved_errno = errno;
    wt_wine_server_close(server);
    errno = saved_errno;
    return NULL;
}

int wt_wine_server_update(struct wt_wine_server *server, int timeout_ms)
{
    struct pollfd descriptors[CLIENT_LIMIT + 1];
    size_t budget = UPDATE_BUDGET;
    unsigned messages = MESSAGE_BUDGET, i;
    int result;
    if (!server || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    descriptors[0].fd = server->fd;
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    for (i = 0; i < CLIENT_LIMIT; ++i) {
        descriptors[i + 1].fd = server->clients[i].fd;
        descriptors[i + 1].events = POLLIN |
            (server->clients[i].output_used ? POLLOUT : 0);
        descriptors[i + 1].revents = 0;
    }
    result = poll(descriptors, CLIENT_LIMIT + 1, server->dirty ? 0 : timeout_ms);
    if (result < 0 && errno != EINTR)
        return -1;
    if (result < 0)
        result = 0;
    if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;
        return -1;
    }
    if (descriptors[0].revents & POLLIN) {
        /* One bounded accept batch avoids starvation from new connections. */
        for (i = 0; i < CLIENT_LIMIT; ++i) {
            struct ucred credentials;
            socklen_t length = sizeof(credentials);
            unsigned slot;
            int fd = accept4(server->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
                break;
            }
            for (slot = 0; slot < CLIENT_LIMIT && server->clients[slot].fd >= 0; ++slot) {}
            if (slot == CLIENT_LIMIT ||
                getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
                length != sizeof(credentials) || credentials.uid != geteuid()) {
                close(fd);
                continue;
            }
            server->clients[slot].fd = fd;
            server->clients[slot].pid = (uint32_t)credentials.pid;
            int receive_buffer=4*1024*1024;
            (void)setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&receive_buffer,sizeof(receive_buffer));
            /* Newly accepted sockets were not in this poll's snapshot. */
            descriptors[slot + 1].revents = POLLIN;
        }
    }
    for (i = 0; i < CLIENT_LIMIT; ++i) {
        short events = descriptors[i + 1].revents;
        if (server->clients[i].fd < 0)
            continue;
        if (((events & POLLIN) && read_client(server, i, &budget, &messages) < 0) ||
            ((events & POLLOUT) && flush_client(&server->clients[i]) < 0) ||
            (events & (POLLERR | POLLHUP | POLLNVAL)))
            remove_client(server, i);
    }
    if (server->dirty) {
        if (follow_window(server)<0) return -1;
        compose(server);
        server->dirty = false;
        return 1;
    }
    return 0;
}

void wt_wine_server_follow_process(struct wt_wine_server *server, uint32_t pid)
{
    if (!server) return;
    server->follow_pid=pid;
    if (!pid) server->origin_x=server->origin_y=0;
    server->dirty=true;
}

void wt_wine_server_frame(const struct wt_wine_server *server,
                          struct winetk_frame *frame)
{
    if (!frame)
        return;
    memset(frame, 0, sizeof(*frame));
    if (!server)
        return;
    frame->pixels = server->pixels;
    frame->width = server->width;
    frame->height = server->height;
    frame->stride = (size_t)server->width * 4u;
    frame->size = frame->stride * server->height;
}

size_t wt_wine_server_window_count(const struct wt_wine_server *server)
{
    size_t count = 0;
    unsigned i;
    if (server)
        for (i = 0; i < WINDOW_LIMIT; ++i)
            count += server->windows[i].used;
    return count;
}

size_t wt_wine_server_client_count(const struct wt_wine_server *server)
{
    size_t count = 0;
    unsigned i;
    if (server)
        for (i = 0; i < CLIENT_LIMIT; ++i)
            count += server->clients[i].fd >= 0;
    return count;
}

int wt_wine_server_send_input(struct wt_wine_server *server,
                              const struct wt_wine_input *input)
{
    struct wt_wine_header header = {WT_WINE_MAGIC, WT_WINE_VERSION,
                                     WT_WINE_INPUT, sizeof(*input)};
    struct wt_wine_input packet;
    struct wine_window *target = NULL;
    struct wine_client *client;
    unsigned i;
    if (!server || !input) {
        errno = EINVAL;
        return -1;
    }
    packet = *input;
    if (input->kind==WT_WINE_MOVE || input->kind==WT_WINE_BUTTON || input->kind==WT_WINE_WHEEL) {
        int64_t x=(int64_t)packet.x+server->origin_x, y=(int64_t)packet.y+server->origin_y;
        if (x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX) { errno=EINVAL; return -1; }
        packet.x=(int32_t)x; packet.y=(int32_t)y;
    }
    /* Hit testing is a separate pass: a lower window can receive a click
     * outside the focused window even when its stacking sequence is smaller. */
    for (i = 0; i < WINDOW_LIMIT; ++i) {
        struct wine_window *candidate = &server->windows[i];
        bool match;
        if (!candidate->used || !(candidate->info.flags & WT_WINE_VISIBLE))
            continue;
        if (input->hwnd)
            match = input->hwnd == candidate->info.hwnd;
        else
            match = input->kind == WT_WINE_BUTTON && input->value &&
                packet.x >= candidate->info.client_x && packet.y >= candidate->info.client_y &&
                (int64_t)packet.x < (int64_t)candidate->info.client_x + candidate->info.client_width &&
                (int64_t)packet.y < (int64_t)candidate->info.client_y + candidate->info.client_height;
        if (match && (!target || candidate->sequence > target->sequence))
            target = candidate;
    }
    if (!target && !input->hwnd) {
        target = find_window(server, server->focused_client, server->focused_hwnd);
        if (target && !(target->info.flags & WT_WINE_VISIBLE))
            target = NULL;
        if (!target) {
            for (i = 0; i < WINDOW_LIMIT; ++i) {
                struct wine_window *candidate = &server->windows[i];
                if (candidate->used && (candidate->info.flags & WT_WINE_VISIBLE) &&
                    (!target || candidate->sequence > target->sequence))
                    target = candidate;
            }
        }
    }
    if (!target)
        return 0;
    packet.hwnd = target->info.hwnd;
    client = &server->clients[target->client];
    if (sizeof(header) + sizeof(packet) > OUTPUT_LIMIT - client->output_used) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(client->output + client->output_used, &header, sizeof(header));
    client->output_used += sizeof(header);
    memcpy(client->output + client->output_used, &packet, sizeof(packet));
    client->output_used += sizeof(packet);
    if (flush_client(client) < 0) {
        int saved_errno = errno;
        remove_client(server, target->client);
        errno = saved_errno;
        return -1;
    }
    if (input->kind == WT_WINE_BUTTON && input->value) {
        server->focused_client = target->client;
        server->focused_hwnd = target->info.hwnd;
        target->sequence = ++server->next_sequence;
        server->dirty = true;
    }
    return 1;
}

void wt_wine_server_close(struct wt_wine_server *server)
{
    unsigned i;
    struct stat status;
    if (!server)
        return;
    if (server->fd >= 0)
        close(server->fd);
    for (i = 0; i < CLIENT_LIMIT; ++i)
        remove_client(server, i);
    if (server->path && server->socket_inode && lstat(server->path, &status) == 0 &&
        status.st_ino == server->socket_inode && status.st_dev == server->socket_device)
        unlink(server->path);
    free(server->pixels);
    free(server->path);
    free(server);
}
