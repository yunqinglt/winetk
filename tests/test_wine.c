#define _GNU_SOURCE
#include "winetk/wine.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", \
                __FILE__, __LINE__, #expression, errno); \
        exit(1); \
    } \
} while (0)

struct fixture {
    struct wt_wine_server *server;
    char directory[64], path[108];
};

static void fixture_open(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    strcpy(fixture->directory, "/tmp/winetk-wine-test-XXXXXX");
    CHECK(mkdtemp(fixture->directory));
    CHECK(snprintf(fixture->path, sizeof(fixture->path), "%s/socket", fixture->directory) > 0);
    fixture->server = wt_wine_server_open(fixture->path, 8, 6);
    CHECK(fixture->server);
}

static void fixture_close(struct fixture *fixture)
{
    wt_wine_server_close(fixture->server);
    CHECK(access(fixture->path, F_OK) < 0 && errno == ENOENT);
    CHECK(rmdir(fixture->directory) == 0);
}

static void pump(struct fixture *fixture)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        CHECK(wt_wine_server_update(fixture->server, 0) >= 0);
}

static int connect_client(struct fixture *fixture)
{
    struct sockaddr_un address = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, fixture->path);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    pump(fixture);
    return fd;
}

static void send_bytes(int fd, const void *bytes, size_t size)
{
    const unsigned char *data = bytes;
    while (size) {
        ssize_t count = send(fd, data, size, MSG_NOSIGNAL);
        CHECK(count > 0);
        data += count;
        size -= (size_t)count;
    }
}

static void message(int fd, uint32_t type, const void *payload, size_t size)
{
    struct wt_wine_header header = {WT_WINE_MAGIC, WT_WINE_VERSION, type, (uint32_t)size};
    send_bytes(fd, &header, sizeof(header));
    send_bytes(fd, payload, size);
}

static struct wt_wine_window window_info(uint64_t hwnd, int32_t x, int32_t y,
                                         uint32_t width, uint32_t height)
{
    struct wt_wine_window info = {hwnd, (uint32_t)getpid(), x, y, width, height,
                                  WT_WINE_VISIBLE, x, y, width, height};
    return info;
}

static void frame(int fd, struct wt_wine_window info, const unsigned char *pixels)
{
    size_t size = (size_t)info.width * info.height * 4u;
    unsigned char *payload = malloc(sizeof(info) + size);
    CHECK(payload);
    memcpy(payload, &info, sizeof(info));
    memcpy(payload + sizeof(info), pixels, size);
    message(fd, WT_WINE_FRAME, payload, sizeof(info) + size);
    free(payload);
}

static void check_pixel(struct fixture *fixture, unsigned x, unsigned y,
                         unsigned r, unsigned g, unsigned b)
{
    struct winetk_frame image;
    const unsigned char *pixel;
    wt_wine_server_frame(fixture->server, &image);
    CHECK(image.width == 8 && image.height == 6 && image.stride == 32 && image.size == 192);
    pixel = image.pixels + y * image.stride + x * 4;
    CHECK(pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == 255);
}

static void test_fragments_clipping_lifecycle(void)
{
    static const unsigned char pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    struct fixture fixture;
    struct wt_wine_window info = window_info(1, -1, 1, 2, 2);
    struct wt_wine_header header = {WT_WINE_MAGIC, WT_WINE_VERSION, WT_WINE_FRAME,
                                     sizeof(info) + sizeof(pixels)};
    unsigned char packet[sizeof(header) + sizeof(info) + sizeof(pixels)];
    struct wt_wine_destroy destroy = {1, (uint32_t)getpid(), 0};
    size_t i;
    int fd;
    fixture_open(&fixture);
    check_pixel(&fixture, 0, 0, 18, 22, 30);
    fd = connect_client(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 1);
    memcpy(packet, &header, sizeof(header));
    memcpy(packet + sizeof(header), &info, sizeof(info));
    memcpy(packet + sizeof(header) + sizeof(info), pixels, sizeof(pixels));
    for (i = 0; i < sizeof(packet); ++i) {
        send_bytes(fd, packet + i, 1);
        CHECK(wt_wine_server_update(fixture.server, 0) == (i + 1 == sizeof(packet)));
    }
    CHECK(wt_wine_server_window_count(fixture.server) == 1);
    check_pixel(&fixture, 0, 1, 0, 255, 0);
    check_pixel(&fixture, 0, 2, 255, 255, 255);
    check_pixel(&fixture, 1, 1, 18, 22, 30);
    info.x = 6;
    info.y = 5;
    info.client_x=info.x; info.client_y=info.y;
    message(fd, WT_WINE_WINDOW, &info, sizeof(info));
    CHECK(wt_wine_server_update(fixture.server, 0) == 1);
    check_pixel(&fixture, 0, 1, 18, 22, 30);
    check_pixel(&fixture, 6, 5, 255, 0, 0);
    info.flags = 0;
    message(fd, WT_WINE_WINDOW, &info, sizeof(info));
    pump(&fixture);
    check_pixel(&fixture, 6, 5, 18, 22, 30);
    info.flags = WT_WINE_VISIBLE;
    info.x = INT_MIN;
    info.y = INT_MAX;
    info.client_x=info.x; info.client_y=info.y;
    message(fd, WT_WINE_WINDOW, &info, sizeof(info));
    pump(&fixture);
    check_pixel(&fixture, 6, 5, 18, 22, 30);
    message(fd, WT_WINE_DESTROY, &destroy, sizeof(destroy));
    pump(&fixture);
    CHECK(wt_wine_server_window_count(fixture.server) == 0);
    close(fd);
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    fixture_close(&fixture);
}

static void test_composition_disconnect_and_input(void)
{
    static const unsigned char red[] = {255, 0, 0, 255, 255, 0, 0, 255};
    static const unsigned char blue[] = {0, 0, 255, 255};
    static const unsigned char half_green[] = {0, 255, 0, 128};
    struct fixture fixture;
    struct wt_wine_window a = window_info(10, 0, 1, 2, 1);
    struct wt_wine_window b = window_info(20, 1, 1, 1, 1);
    struct wt_wine_input input = {0}, received;
    struct wt_wine_header header;
    int first, second;
    fixture_open(&fixture);
    first = connect_client(&fixture);
    second = connect_client(&fixture);
    frame(first, a, red);
    frame(second, b, blue);
    pump(&fixture);
    check_pixel(&fixture, 1, 1, 0, 0, 255);
    frame(first, a, red); /* Damage never implicitly raises an existing window. */
    pump(&fixture);
    check_pixel(&fixture, 1, 1, 0, 0, 255);
    frame(second, b, half_green);
    pump(&fixture);
    check_pixel(&fixture, 1, 1, 127, 128, 0);
    /* Activation can happen after WINDOW metadata: a later FRAME must select
     * the foreground application before the user clicks it. */
    b.flags |= WT_WINE_FOREGROUND;
    frame(second, b, half_green);
    pump(&fixture);
    input.kind = WT_WINE_KEY;
    input.code = 'B';
    input.value = 1;
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 1);
    CHECK(recv(second, &header, sizeof(header), MSG_WAITALL) == sizeof(header));
    CHECK(recv(second, &received, sizeof(received), MSG_WAITALL) == sizeof(received));
    CHECK(received.hwnd == 20 && received.kind == WT_WINE_KEY && received.code == 'B');
    CHECK(recv(first, &header, sizeof(header), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    input.kind = WT_WINE_BUTTON;
    input.code = 1;
    input.value = 1;
    input.x = input.y = 1;
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 1);
    CHECK(recv(second, &header, sizeof(header), MSG_WAITALL) == sizeof(header));
    CHECK(header.magic == WT_WINE_MAGIC && header.version == WT_WINE_VERSION &&
          header.type == WT_WINE_INPUT && header.size == sizeof(received));
    CHECK(recv(second, &received, sizeof(received), MSG_WAITALL) == sizeof(received));
    CHECK(received.hwnd == 20 && received.kind == WT_WINE_BUTTON && received.value == 1);
    input.kind = WT_WINE_KEY;
    input.code = 'A';
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 1);
    CHECK(recv(second, &header, sizeof(header), MSG_WAITALL) == sizeof(header));
    CHECK(recv(second, &received, sizeof(received), MSG_WAITALL) == sizeof(received));
    CHECK(received.hwnd == 20 && received.code == 'A');
    CHECK(recv(first, &header, sizeof(header), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    /* A click outside the focused window must still select a lower window. */
    input.kind = WT_WINE_BUTTON;
    input.code = 1;
    input.x = 0;
    input.y = 1;
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 1);
    CHECK(recv(first, &header, sizeof(header), MSG_WAITALL) == sizeof(header));
    CHECK(recv(first, &received, sizeof(received), MSG_WAITALL) == sizeof(received));
    CHECK(received.hwnd == 10);
    input.kind = WT_WINE_KEY;
    pump(&fixture);
    close(second);
    pump(&fixture);
    CHECK(wt_wine_server_window_count(fixture.server) == 1);
    check_pixel(&fixture, 1, 1, 255, 0, 0);
    input.hwnd = 10;
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 1);
    CHECK(recv(first, &header, sizeof(header), MSG_WAITALL) == sizeof(header));
    CHECK(recv(first, &received, sizeof(received), MSG_WAITALL) == sizeof(received));
    CHECK(received.hwnd == 10);
    close(first);
    pump(&fixture);
    check_pixel(&fixture, 1, 1, 18, 22, 30);
    CHECK(wt_wine_server_send_input(fixture.server, &input) == 0);
    fixture_close(&fixture);
}

static void test_reject_bad_streams(void)
{
    struct fixture fixture;
    struct wt_wine_header cases[] = {
        {0, WT_WINE_VERSION, WT_WINE_FRAME, 36},
        {WT_WINE_MAGIC, 100, WT_WINE_FRAME, 36},
        {WT_WINE_MAGIC, WT_WINE_VERSION, 999, 0},
        {WT_WINE_MAGIC, WT_WINE_VERSION, WT_WINE_FRAME, UINT32_MAX},
        {WT_WINE_MAGIC, WT_WINE_VERSION, WT_WINE_WINDOW, 31},
        {WT_WINE_MAGIC, WT_WINE_VERSION, WT_WINE_DESTROY, 15},
        {WT_WINE_MAGIC, WT_WINE_VERSION, WT_WINE_INPUT, 32}
    };
    unsigned char payload[sizeof(struct wt_wine_window)+4] = {0};
    struct wt_wine_window info = window_info(1, 0, 0, 1921, 1);
    size_t i;
    int fd;
    fixture_open(&fixture);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fd = connect_client(&fixture);
        send_bytes(fd, &cases[i], sizeof(cases[i]));
        pump(&fixture);
        CHECK(wt_wine_server_client_count(fixture.server) == 0);
        close(fd);
    }
    fd = connect_client(&fixture);
    memcpy(payload, &info, sizeof(info));
    message(fd, WT_WINE_FRAME, payload, sizeof(payload));
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    CHECK(wt_wine_server_window_count(fixture.server) == 0);
    close(fd);
    fd = connect_client(&fixture);
    info.width = 2; /* Packet claims eight pixel bytes but carries four. */
    memcpy(payload, &info, sizeof(info));
    message(fd, WT_WINE_FRAME, payload, sizeof(payload));
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    close(fd);
    fd = connect_client(&fixture);
    info.width = 1;
    info.pid ^= 0x40000000u; /* The payload cannot impersonate another process. */
    memcpy(payload, &info, sizeof(info));
    message(fd, WT_WINE_FRAME, payload, sizeof(payload));
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    close(fd);
    fd = connect_client(&fixture);
    send_bytes(fd, &cases[0], 7); /* EOF in the middle of a header. */
    close(fd);
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    fixture_close(&fixture);
}

static void test_private_socket_and_bounds(void)
{
    struct fixture fixture;
    struct stat status;
    int clients[9];
    unsigned i;
    fixture_open(&fixture);
    CHECK(lstat(fixture.path, &status) == 0 && (status.st_mode & 0777) == 0600);
    CHECK(!wt_wine_server_open(fixture.path, 8, 6) && errno == EEXIST);
    CHECK(!wt_wine_server_open("/tmp/winetk-public-socket", 8, 6) && errno == EACCES);
    CHECK(!wt_wine_server_open(fixture.path, 1921, 1) && errno == EINVAL);
    for (i = 0; i < 9; ++i)
        clients[i] = connect_client(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 8);
    for (i = 0; i < 9; ++i)
        close(clients[i]);
    pump(&fixture);
    CHECK(wt_wine_server_client_count(fixture.server) == 0);
    fixture_close(&fixture);
}

static void test_follow_client_resolution_and_input_origin(void)
{
    struct fixture fixture;
    struct winetk_frame image;
    struct wt_wine_window main=window_info(100,40,60,3,2), popup=window_info(200,41,60,1,1);
    struct wt_wine_input input={.kind=WT_WINE_BUTTON,.code=1,.value=1,.x=1,.y=0}, received;
    struct wt_wine_header header;
    unsigned char red[48], green[]={0,255,0,255};
    int fd;
    for (size_t i=0;i<sizeof(red);i+=4) { red[i]=255; red[i+1]=red[i+2]=0; red[i+3]=255; }
    fixture_open(&fixture);
    wt_wine_server_follow_process(fixture.server,(uint32_t)getpid());
    fd=connect_client(&fixture);
    frame(fd,main,red); pump(&fixture);
    wt_wine_server_frame(fixture.server,&image);
    CHECK(image.width==3 && image.height==2 && image.size==24 && image.pixels[0]==255);
    popup.flags|=WT_WINE_FOREGROUND;
    frame(fd,popup,green); pump(&fixture);
    wt_wine_server_frame(fixture.server,&image);
    CHECK(image.width==3 && image.height==2 && image.pixels[4]==0 && image.pixels[5]==255);
    CHECK(wt_wine_server_send_input(fixture.server,&input)==1);
    CHECK(recv(fd,&header,sizeof(header),MSG_WAITALL)==sizeof(header));
    CHECK(recv(fd,&received,sizeof(received),MSG_WAITALL)==sizeof(received));
    CHECK(received.hwnd==200 && received.x==41 && received.y==60);
    /* Geometry-only movement retains the last pixels at the new client origin. */
    main.x=main.client_x=-100; main.y=main.client_y=200;
    message(fd,WT_WINE_WINDOW,&main,sizeof(main)); pump(&fixture);
    wt_wine_server_frame(fixture.server,&image);
    CHECK(image.width==3 && image.height==2 && image.pixels[0]==255 && image.pixels[4]==255);
    main.width=main.client_width=4; main.height=main.client_height=3;
    frame(fd,main,red); pump(&fixture);
    wt_wine_server_frame(fixture.server,&image);
    CHECK(image.width==4 && image.height==3 && image.size==48 && image.pixels[44]==255);
    /* A Steam host is the launch process but the reparented renderer owns the
     * drawable. GPU frames remain eligible after geometry-only notifications. */
    wt_wine_server_follow_process(fixture.server,(uint32_t)getpid()+1);
    main.flags|=WT_WINE_GPU;
    main.width=main.client_width=3; main.height=main.client_height=2;
    frame(fd,main,red); pump(&fixture);
    wt_wine_server_frame(fixture.server,&image);
    CHECK(image.width==3 && image.height==2);
    main.flags&=~WT_WINE_GPU;
    main.x=main.client_x=70;
    message(fd,WT_WINE_WINDOW,&main,sizeof(main)); pump(&fixture);
    input.x=0;
    CHECK(wt_wine_server_send_input(fixture.server,&input)==1);
    CHECK(recv(fd,&header,sizeof(header),MSG_WAITALL)==sizeof(header));
    CHECK(recv(fd,&received,sizeof(received),MSG_WAITALL)==sizeof(received));
    CHECK(received.hwnd==100 && received.x==70 && received.y==200);
    close(fd); fixture_close(&fixture);
}

int main(void)
{
    CHECK(sizeof(struct wt_wine_header) == 16);
    CHECK(sizeof(struct wt_wine_window) == 48);
    CHECK(sizeof(struct wt_wine_input) == 32);
    test_fragments_clipping_lifecycle();
    test_composition_disconnect_and_input();
    test_reject_bad_streams();
    test_private_socket_and_bounds();
    test_follow_client_resolution_and_input_origin();
    puts("Wine IPC/composition tests passed");
    return 0;
}
