#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "winetk/backend.h"
#include "winetk/input.h"
#include "winetk/scene.h"
#include "winetk/terminal.h"
#include "winetk/wine.h"
#include "winetk/launcher.h"
#include "winetk/wine_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <png.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

struct options {
    uint32_t width, height, fps, record_frames, probe_ms;
    double duration;
    const char *log, *record_dir, *wine, *prefix, *wine_log, *wine_cwd, *title;
    bool kitty_direct, zoom_in, fit_resolution;
    char **command;
};

struct application {
    struct options options;
    struct wt_terminal terminal;
    struct winetk_backend backend;
    struct wt_input_parser parser;
    struct wt_viewport viewport;
    struct wt_scene scene;
    struct wt_wine_server *wine;
    struct wt_launcher child;
    struct wt_launch_geometry launch_geometry;
    char runtime_dir[PATH_MAX], socket_path[PATH_MAX], title[256];
    FILE *log;
    uint32_t image_id, columns, rows, pixel_width, pixel_height, cell_width, cell_height;
    uint32_t reported_cell_width, reported_cell_height;
    bool graphics, keyboard, pixel_mouse, synchronized, running, active, resized;
    bool dirty, geometry_valid, title_dirty, wine_canvas_logged;
    bool shared_probe, shared_reply, shared_memory;
    int32_t drag_x, drag_y;
    uint32_t pressed[128];
    size_t pressed_count;
    uint32_t wine_modifiers;
    uint32_t suppress_text, mouse_buttons;
    int32_t mouse_x, mouse_y;
    uint64_t start, frames, inputs, frame_bytes, max_present_ms, last_input;
    uint8_t *logical, *scaled;
    size_t scaled_capacity;
    char error[256];
};

static void log_event(struct application *app, const char *event, const char *format, ...)
{
    va_list arguments;
    if (!app->log) return;
    fprintf(app->log,"{\"event\":\"%s\",\"time_ms\":%llu",event,
            (unsigned long long)(wt_now_ms()-app->start));
    if (format && *format) {
        fputc(',',app->log);
        va_start(arguments,format);
        vfprintf(app->log,format,arguments);
        va_end(arguments);
    }
    fputs("}\n",app->log);
    if (ferror(app->log)) {
        if (!*app->error) snprintf(app->error,sizeof(app->error),"cannot write session log");
        app->running=false;
    }
}

static void set_error(struct application *app, const char *message)
{
    if (!*app->error) snprintf(app->error,sizeof(app->error),"%s",message);
    app->running = false;
}

static void executable_title(char *out, size_t capacity, const char *name)
{
    const char *base=name;
    mbstate_t state={0};
    size_t used=0;
    for (const char *p=name;*p;++p) if (*p=='/' || *p=='\\') base=p+1;
    while (*base && used+1<capacity) {
        wchar_t character;
        size_t count=mbrtowc(&character,base,strlen(base),&state);
        if (count==(size_t)-1 || count==(size_t)-2 || !count || wcwidth(character)<0) {
            out[used++]='?'; ++base; memset(&state,0,sizeof(state));
        } else {
            if (count>=capacity-used) break;
            memcpy(out+used,base,count); used+=count; base+=count;
        }
    }
    out[used]=0;
}

static size_t title_prefix(const char *title, uint32_t columns)
{
    mbstate_t state={0};
    size_t bytes=0;
    uint32_t cells=0;
    while (title[bytes]) {
        wchar_t character;
        size_t count=mbrtowc(&character,title+bytes,strlen(title+bytes),&state);
        int width;
        if (count==(size_t)-1 || count==(size_t)-2 || !count) break;
        width=wcwidth(character);
        if (width<0 || (uint32_t)width>columns-cells) break;
        cells+=(uint32_t)width; bytes+=count;
    }
    return bytes;
}

static void constrain_box(struct wt_scene *scene)
{
    if (scene->box_x < 0) scene->box_x = 0;
    if (scene->box_y < 80) scene->box_y = 80;
    if (scene->box_x > (int32_t)scene->width-112) scene->box_x = (int32_t)scene->width-112;
    if (scene->box_y > (int32_t)scene->height-126) scene->box_y = (int32_t)scene->height-126;
}

static void update_pressed(struct application *app, uint32_t key, enum wt_key_action action)
{
    size_t index;
    for (index=0; index<app->pressed_count && app->pressed[index]!=key; ++index) {}
    if (action == WT_KEY_RELEASE && index < app->pressed_count)
        app->pressed[index] = app->pressed[--app->pressed_count];
    else if (action != WT_KEY_RELEASE && index == app->pressed_count && app->pressed_count < 128)
        app->pressed[app->pressed_count++] = key;
}

static uint32_t virtual_key(uint32_t key)
{
    if (key>='a' && key<='z') return key-'a'+'A';
    if ((key>='A' && key<='Z') || (key>='0' && key<='9') || key==' ') return key;
    if (key>=WT_KEY_F1 && key<=WT_KEY_F12) return 0x70+key-WT_KEY_F1;
    switch (key) {
    case WT_KEY_ESCAPE: return 0x1b;
    case WT_KEY_ENTER: return 0x0d;
    case WT_KEY_TAB: return 9;
    case WT_KEY_BACKSPACE: return 8;
    case WT_KEY_INSERT: return 0x2d;
    case WT_KEY_DELETE: return 0x2e;
    case WT_KEY_LEFT: return 0x25;
    case WT_KEY_UP: return 0x26;
    case WT_KEY_RIGHT: return 0x27;
    case WT_KEY_DOWN: return 0x28;
    case WT_KEY_PAGE_UP: return 0x21;
    case WT_KEY_PAGE_DOWN: return 0x22;
    case WT_KEY_HOME: return 0x24;
    case WT_KEY_END: return 0x23;
    case 57441: case 57447: return 0x10; /* left/right Shift */
    case 57442: case 57448: return 0x11; /* left/right Control */
    case 57443: case 57449: return 0x12; /* left/right Alt */
    case 57444: return 0x5b;
    case 57450: return 0x5c;
    case ';': case ':': return 0xba;
    case '=': case '+': return 0xbb;
    case ',': case '<': return 0xbc;
    case '-': case '_': return 0xbd;
    case '.': case '>': return 0xbe;
    case '/': case '?': return 0xbf;
    case '`': case '~': return 0xc0;
    case '[': case '{': return 0xdb;
    case '\\': case '|': return 0xdc;
    case ']': case '}': return 0xdd;
    case '\'': case '"': return 0xde;
    default: return 0;
    }
}

static void wine_send(struct application *app, struct wt_wine_input input)
{
    int result=wt_wine_server_send_input(app->wine,&input);
    log_event(app,"wine_input","\"kind\":%u,\"code\":%u,\"value\":%d,\"x\":%d,\"y\":%d,\"queued\":%d",
              input.kind,input.code,input.value,input.x,input.y,result);
    if (result<0) set_error(app,"Wine input queue failed");
}

static void wine_modifiers(struct application *app, uint32_t modifiers)
{
    static const uint32_t masks[]={WT_MOD_SHIFT,WT_MOD_CTRL,WT_MOD_ALT,WT_MOD_SUPER};
    static const uint32_t keys[]={0x10,0x11,0x12,0x5b};
    for (size_t i=0;i<4;++i) if ((app->wine_modifiers^modifiers)&masks[i])
        wine_send(app,(struct wt_wine_input){.kind=WT_WINE_KEY,.code=keys[i],.value=!!(modifiers&masks[i])});
    app->wine_modifiers=modifiers;
}

static void wine_input(struct application *app, const struct wt_input_event *event)
{
    struct wt_wine_input input={0};
    if (event->type==WT_INPUT_KEY) {
        uint32_t key=event->key.codepoint;
        if (key==WT_KEY_F12 && (event->key.modifiers&(WT_MOD_CTRL|WT_MOD_SHIFT))==(WT_MOD_CTRL|WT_MOD_SHIFT)) {
            app->running=false; return;
        }
        wine_modifiers(app,event->key.modifiers);
        input.code=virtual_key(key);
        input.kind=WT_WINE_KEY; input.value=event->key.action!=WT_KEY_RELEASE;
        app->suppress_text=input.code ? event->key.text_count : 0;
        update_pressed(app,key,event->key.action);
        if (input.code) wine_send(app,input);
        /* Kitty retains CSI A/B/C/D spellings for arrows even with event
         * reporting enabled. Their release arrives as CSI 1;1:3A etc.; a
         * legacy spelling does not imply that this session lacks releases. */
        if (input.code && event->key.legacy && input.value && !app->terminal.keyboard_pushed) {
            input.value=0; wine_send(app,input);
            update_pressed(app,key,WT_KEY_RELEASE);
        }
    } else if (event->type==WT_INPUT_TEXT) {
        if (!app->suppress_text || event->text.paste)
            wine_send(app,(struct wt_wine_input){.kind=WT_WINE_TEXT,.code=event->text.codepoint});
        if (event->text.paste) app->suppress_text=0;
        else if (app->suppress_text) --app->suppress_text;
    } else if (event->type==WT_INPUT_MOUSE) {
        bool inside=wt_viewport_map(&app->viewport,event->mouse.x,event->mouse.y,app->pixel_mouse,
                                   app->columns,app->rows,&input.x,&input.y);
        if (!inside && event->mouse.action==WT_MOUSE_PRESS) return;
        app->mouse_x=input.x; app->mouse_y=input.y;
        wine_modifiers(app,event->mouse.modifiers);
        input.kind=event->mouse.action==WT_MOUSE_WHEEL ? WT_WINE_WHEEL :
                   event->mouse.action==WT_MOUSE_MOVE ? WT_WINE_MOVE : WT_WINE_BUTTON;
        input.code=event->mouse.button;
        input.value=event->mouse.action==WT_MOUSE_WHEEL ? event->mouse.wheel_y : event->mouse.action==WT_MOUSE_PRESS;
        if (input.kind==WT_WINE_BUTTON && input.code>=1 && input.code<=3) {
            if (input.value) app->mouse_buttons|=1u<<input.code;
            else app->mouse_buttons&=~(1u<<input.code);
        }
        wine_send(app,input);
    } else if (event->type==WT_INPUT_FOCUS) {
        if (!event->focus.focused) {
            for (size_t i=0;i<app->pressed_count;++i) {
                uint32_t key=virtual_key(app->pressed[i]);
                if (key) wine_send(app,(struct wt_wine_input){.kind=WT_WINE_KEY,.code=key});
            }
            app->pressed_count=0;
            wine_modifiers(app,0);
            for (uint32_t button=1;button<=3;++button) if (app->mouse_buttons&(1u<<button))
                wine_send(app,(struct wt_wine_input){.kind=WT_WINE_BUTTON,.code=button,
                          .x=app->mouse_x,.y=app->mouse_y});
            app->mouse_buttons=0;
            app->suppress_text=0;
        }
        wine_send(app,(struct wt_wine_input){.kind=WT_WINE_FOCUS,.value=event->focus.focused});
    }
}

static void input_event(void *context, const struct wt_input_event *event)
{
    struct application *app = context;
    switch (event->type) {
    case WT_INPUT_GRAPHICS_REPLY:
        if (event->graphics.has_image_id && event->graphics.image_id == app->image_id) {
            if (event->graphics.ok) app->graphics = true;
            else set_error(app,"terminal rejected the Kitty graphics probe");
        } else if (event->graphics.has_image_id && app->shared_probe &&
                   event->graphics.image_id == app->image_id+1) {
            app->shared_reply=true;
            app->shared_memory=event->graphics.ok;
        }
        return;
    case WT_INPUT_KEYBOARD_FLAGS: app->keyboard=true; return;
    case WT_INPUT_MODE_REPORT:
        /* Optional replies may arrive after activation; keep the decoder in
         * the same coordinate mode that was actually enabled on the TTY. */
        if (app->active) return;
        if (event->mode.value==1 || event->mode.value==2) {
            if (event->mode.mode==1016) app->pixel_mouse=true;
            if (event->mode.mode==2026) app->synchronized=true;
        }
        return;
    case WT_INPUT_WINDOW_SIZE:
        if (!event->window.width || !event->window.height || event->window.width > 65536 || event->window.height > 65536) return;
        if (event->window.kind==4) {
            app->pixel_width=event->window.width; app->pixel_height=event->window.height;
        } else if (event->window.kind==6) {
            app->reported_cell_width=event->window.width; app->reported_cell_height=event->window.height;
        } else if (event->window.kind==8) {
            app->columns=event->window.width; app->rows=event->window.height;
        }
        app->resized=true;
        return;
    case WT_INPUT_DEVICE_ATTRIBUTES: return;
    default: break;
    }
    if (!app->active) return;
    ++app->inputs;
    app->dirty=true;
    if (app->wine) { wine_input(app,event); return; }
    switch (event->type) {
    case WT_INPUT_KEY:
        update_pressed(app,event->key.codepoint,event->key.action);
        log_event(app,"key","\"codepoint\":%u,\"action\":%u,\"modifiers\":%u,\"pressed\":%zu",
                  event->key.codepoint,(unsigned)event->key.action,event->key.modifiers,app->pressed_count);
        snprintf(app->scene.last_event,sizeof(app->scene.last_event),"KEY U+%04X / ACTION %u / MOD %u",
                 event->key.codepoint,(unsigned)event->key.action,event->key.modifiers);
        if (event->key.action != WT_KEY_RELEASE) {
            uint32_t key=event->key.codepoint;
            if (key==WT_KEY_ESCAPE || ((key=='c'||key=='C') && (event->key.modifiers&WT_MOD_CTRL))) app->running=false;
            if (key==WT_KEY_LEFT) app->scene.box_x-=8;
            if (key==WT_KEY_RIGHT) app->scene.box_x+=8;
            if (key==WT_KEY_UP) app->scene.box_y-=8;
            if (key==WT_KEY_DOWN) app->scene.box_y+=8;
            if (key==' ' && event->key.action==WT_KEY_PRESS) app->scene.paused=!app->scene.paused;
            constrain_box(&app->scene);
        }
        break;
    case WT_INPUT_TEXT:
        ++app->scene.text_count;
        log_event(app,"text","\"codepoint\":%u,\"paste\":%s",event->text.codepoint,event->text.paste?"true":"false");
        snprintf(app->scene.last_event,sizeof(app->scene.last_event),"TEXT U+%04X / COUNT %llu",event->text.codepoint,
                 (unsigned long long)app->scene.text_count);
        break;
    case WT_INPUT_MOUSE:
        app->scene.pointer_inside=wt_viewport_map(&app->viewport,event->mouse.x,event->mouse.y,app->pixel_mouse,
                                                  app->columns,app->rows,&app->scene.pointer_x,&app->scene.pointer_y);
        log_event(app,"mouse","\"action\":%u,\"button\":%u,\"raw_x\":%d,\"raw_y\":%d,\"x\":%d,\"y\":%d,\"inside\":%s,\"wheel_x\":%d,\"wheel_y\":%d",
                  (unsigned)event->mouse.action,event->mouse.button,event->mouse.x,event->mouse.y,
                  app->scene.pointer_x,app->scene.pointer_y,app->scene.pointer_inside?"true":"false",event->mouse.wheel_x,event->mouse.wheel_y);
        snprintf(app->scene.last_event,sizeof(app->scene.last_event),"MOUSE %d / %d / ACTION %u / BUTTON %u",
                 app->scene.pointer_x,app->scene.pointer_y,(unsigned)event->mouse.action,event->mouse.button);
        if (event->mouse.action==WT_MOUSE_PRESS && event->mouse.button==1 && app->scene.pointer_inside &&
            app->scene.pointer_x>=app->scene.box_x && app->scene.pointer_x<app->scene.box_x+112 &&
            app->scene.pointer_y>=app->scene.box_y && app->scene.pointer_y<app->scene.box_y+64) {
            app->scene.dragging=true;
            app->drag_x=app->scene.pointer_x-app->scene.box_x;
            app->drag_y=app->scene.pointer_y-app->scene.box_y;
        }
        if (event->mouse.action==WT_MOUSE_RELEASE) app->scene.dragging=false;
        if (event->mouse.action==WT_MOUSE_MOVE && app->scene.dragging) {
            app->scene.box_x=app->scene.pointer_x-app->drag_x;
            app->scene.box_y=app->scene.pointer_y-app->drag_y;
        }
        if (event->mouse.action==WT_MOUSE_WHEEL) app->scene.box_y-=event->mouse.wheel_y*8;
        constrain_box(&app->scene);
        break;
    case WT_INPUT_FOCUS:
        app->scene.focused=event->focus.focused;
        if (!event->focus.focused) { app->pressed_count=0; app->scene.dragging=false; }
        log_event(app,"focus","\"focused\":%s,\"pressed\":%zu",event->focus.focused?"true":"false",app->pressed_count);
        snprintf(app->scene.last_event,sizeof(app->scene.last_event),"FOCUS %s",event->focus.focused?"ON":"OFF / INPUT STATE RELEASED");
        break;
    case WT_INPUT_PASTE:
        log_event(app,"paste","\"begin\":%s",event->paste.begin?"true":"false");
        break;
    default: break;
    }
}

static int read_events(struct application *app, int timeout)
{
    struct pollfd fd={.fd=app->terminal.fd,.events=POLLIN};
    int result=poll(&fd,1,timeout);
    if (result<0) return errno==EINTR ? 0 : -1;
    if (fd.revents & (POLLHUP|POLLERR|POLLNVAL)) { errno=EIO; return -1; }
    if (fd.revents & POLLIN) {
        unsigned char bytes[4096];
        /* Bound each pass so a continuous paste cannot starve rendering/signals. */
        for (int pass=0; pass<16; ++pass) {
            ssize_t n=read(fd.fd,bytes,sizeof(bytes));
            if (n<0) return (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR) ? 0 : -1;
            if (!n) break;
            app->last_input=wt_now_ms();
            wt_input_parser_feed(&app->parser,bytes,(size_t)n);
        }
    }
    if (wt_input_parser_pending_escape(&app->parser) && wt_now_ms()-app->last_input>=35)
        wt_input_parser_flush(&app->parser);
    return 0;
}

static void measure_terminal(struct application *app)
{
    struct winsize size;
    bool ioctl_pixels=false;
    if (ioctl(app->terminal.fd,TIOCGWINSZ,&size)==0) {
        if (size.ws_col) app->columns=size.ws_col;
        if (size.ws_row) app->rows=size.ws_row;
        if (size.ws_xpixel && size.ws_ypixel) {
            app->pixel_width=size.ws_xpixel; app->pixel_height=size.ws_ypixel;
            ioctl_pixels=true;
        }
    }
    if (!ioctl_pixels && app->reported_cell_width && app->reported_cell_height) {
        uint64_t width=(uint64_t)app->reported_cell_width*app->columns;
        uint64_t height=(uint64_t)app->reported_cell_height*app->rows;
        if (width && height && width<=65536 && height<=65536) {
            app->pixel_width=(uint32_t)width; app->pixel_height=(uint32_t)height;
        }
    }
}

static int resize_canvas(struct application *app)
{
    measure_terminal(app);
    if (!app->columns || app->rows<2 || !app->pixel_width || !app->pixel_height) return -1;
    /* Measure cell geometry consistently with CUP positioning. */
    app->cell_width=app->pixel_width/app->columns;
    app->cell_height=app->pixel_height/app->rows;
    if (!app->cell_width || !app->cell_height) return -1;
    if (!wt_viewport_fit(&app->viewport,app->options.width,app->options.height,
                         app->cell_width*app->columns,app->cell_height*(app->rows-1))) return -1;
    /* Keep native pixels by default; optional enlargement uses the same fit and input mapping. */
    if (!app->options.zoom_in &&
        (app->viewport.width>app->options.width || app->viewport.height>app->options.height)) {
        app->viewport.width=app->options.width; app->viewport.height=app->options.height;
        app->viewport.x=(app->viewport.terminal_width-app->viewport.width)/2;
        app->viewport.y=(app->viewport.terminal_height-app->viewport.height)/2;
    }
    /* Reserve native terminal row zero, but retain total height for mouse mapping. */
    app->viewport.y+=app->cell_height;
    app->viewport.terminal_height=app->cell_height*app->rows;
    app->title_dirty=true;
    size_t capacity=(size_t)app->viewport.width*app->viewport.height*4;
    if (capacity>app->scaled_capacity) {
        uint8_t *pixels=realloc(app->scaled,capacity);
        if (!pixels) return -1;
        app->scaled=pixels; app->scaled_capacity=capacity;
    }
    app->geometry_valid=true; app->dirty=true; app->resized=false;
    log_event(app,"resize","\"columns\":%u,\"rows\":%u,\"pixel_width\":%u,\"pixel_height\":%u,\"x\":%u,\"y\":%u,\"width\":%u,\"height\":%u,\"source_width\":%u,\"source_height\":%u,\"zoom_in\":%s",
              app->columns,app->rows,app->pixel_width,app->pixel_height,app->viewport.x,app->viewport.y,app->viewport.width,app->viewport.height,
              app->options.width,app->options.height,app->options.zoom_in?"true":"false");
    return 0;
}

static int record_frame(struct application *app, const struct winetk_frame *frame)
{
    png_image image;
    char path[PATH_MAX];
    if (!app->options.record_dir || app->frames>=app->options.record_frames) return 0;
    if (snprintf(path,sizeof(path),"%s/frame-%06llu.png",app->options.record_dir,(unsigned long long)(app->frames+1)) >= (int)sizeof(path)) {
        errno=ENAMETOOLONG; return -1;
    }
    memset(&image,0,sizeof(image));
    image.version=PNG_IMAGE_VERSION; image.width=frame->width; image.height=frame->height; image.format=PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&image,path,0,frame->pixels,(png_int_32)frame->stride,NULL)) {
        png_image_free(&image); errno=EIO; return -1;
    }
    png_image_free(&image);
    return 0;
}

static int present_frame(struct application *app)
{
    uint64_t start=wt_now_ms(), encoded, before=app->terminal.bytes;
    struct winetk_frame frame;
    struct winetk_viewport placement;
    const uint8_t *pixels=app->scaled;
    if (app->wine) {
        struct winetk_frame source;
        wt_wine_server_frame(app->wine,&source);
        if (source.width==app->viewport.width && source.height==app->viewport.height) pixels=source.pixels;
        else wt_scale_rgba(source.pixels,source.width,source.height,app->scaled,app->viewport.width,app->viewport.height);
    } else {
        wt_scene_render(&app->scene,app->logical);
        wt_scale_rgba(app->logical,app->scene.width,app->scene.height,app->scaled,app->viewport.width,app->viewport.height);
    }
    frame=(struct winetk_frame){.pixels=pixels,.width=app->viewport.width,.height=app->viewport.height,
                               .stride=(size_t)app->viewport.width*4,.size=(size_t)app->viewport.width*app->viewport.height*4};
    placement=(struct winetk_viewport){.column=app->viewport.x/app->cell_width,.row=app->viewport.y/app->cell_height,
                                     .pixel_x=app->viewport.x%app->cell_width,.pixel_y=app->viewport.y%app->cell_height};
    encoded=wt_now_ms();
    app->terminal.deadline_ms=encoded+2000;
    app->terminal.record_output=app->options.record_dir && app->frames<app->options.record_frames;
    if (app->synchronized && wt_terminal_command(&app->terminal,"\033[?2026h")<0) return -1;
    if (app->title_dirty) {
        char header[512];
        size_t limit=title_prefix(app->title,app->columns>1 ? app->columns-1 : 0);
        snprintf(header,sizeof(header),"\033[1;1H\033[0m\033[2K%.*s",(int)limit,app->title);
        if (wt_terminal_command(&app->terminal,header)<0) return -1;
        app->title_dirty=false;
    }
    if (winetk_backend_present(&app->backend,&frame,&placement)<0) return -1;
    if (app->synchronized && wt_terminal_command(&app->terminal,"\033[?2026l")<0) return -1;
    app->terminal.record_output=false;
    app->terminal.deadline_ms=0;
    {
        uint64_t end=wt_now_ms(), present_ms=end-encoded, bytes=app->terminal.bytes-before;
        if (present_ms>app->max_present_ms) app->max_present_ms=present_ms;
        app->frame_bytes+=bytes;
        log_event(app,"frame","\"number\":%llu,\"bytes\":%llu,\"render_ms\":%llu,\"present_ms\":%llu,\"width\":%u,\"height\":%u",
                  (unsigned long long)(app->frames+1),(unsigned long long)bytes,(unsigned long long)(encoded-start),
                  (unsigned long long)present_ms,frame.width,frame.height);
    }
    if (record_frame(app,&frame)<0) return -1;
    ++app->frames;
    if (!app->scene.paused) ++app->scene.frame;
    app->dirty=false;
    return 0;
}

static void usage(FILE *file)
{
    fputs("Usage: winetk-terminal-demo [options]\n"
          "  --width N --height N  Logical canvas (default 640x360, 320x240..1920x1080)\n"
          "  --fps N               Submission limit, 1..240 (demo 15, Wine 60)\n"
          "  --duration SECONDS    Stop automatically (default 0: interactive)\n"
          "  --log PATH            Append JSONL metrics/events (default winetk-demo.jsonl)\n"
          "  --record-dir PATH     Create NEW directory with PNG frames + graphics.bin\n"
          "  --record-frames N     Limit recorded frames, 1..1000 (default 3)\n"
          "  --probe-timeout MS    Capability deadline, 100..10000 (default 1500)\n"
          "  --kitty-transfer MODE auto (local shared memory) or direct (TTY payload)\n"
          "  --zoom-in             Fit to terminal, allowing proportional enlargement below title\n"
          "  --fit-resolution      Render at startup terminal size (aspect from --width/--height, max 1920x1080)\n"
          "  --wine PATH           Project Wine loader; launch program after --\n"
          "  --wine-prefix PATH    Isolated prefix configured for wineterm.drv\n"
          "  --wine-cwd PATH       Windows program working directory\n"
          "  --title NAME          Executable label above the canvas\n"
          "  --wine-log PATH       Append Wine stdout/stderr (required in Wine mode)\n"
          "  --help                Show this help\n"
          "Demo controls: arrows, mouse drag/wheel, Space pause, Esc or Ctrl+C exit.\n"
          "Wine controls: forwarded to app; Ctrl+Shift+F12 stops this session.\n",file);
}

static int number(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    char *end;
    unsigned long result;
    errno=0; result=strtoul(text,&end,10);
    if (errno || !*text || *end || result<min || result>max) return -1;
    *value=(uint32_t)result; return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    bool width_set=false, height_set=false, fps_set=false;
    *options=(struct options){.width=640,.height=360,.fps=15,.record_frames=3,.probe_ms=1500,.log="winetk-demo.jsonl"};
    for (int i=1; i<argc; ++i) {
        const char *key=argv[i], *value;
        if (!strcmp(key,"--help")) { usage(stdout); return 1; }
        if (!strcmp(key,"--zoom-in")) { options->zoom_in=true; continue; }
        if (!strcmp(key,"--fit-resolution")) { options->fit_resolution=true; options->zoom_in=true; continue; }
        if (!strcmp(key,"--")) { options->command=&argv[i+1]; break; }
        if (++i>=argc) return -1;
        value=argv[i];
        if (!strcmp(key,"--width")) { if(number(value,320,1920,&options->width)<0) return -1; width_set=true; }
        else if (!strcmp(key,"--height")) { if(number(value,240,1080,&options->height)<0) return -1; height_set=true; }
        else if (!strcmp(key,"--fps")) { if(number(value,1,240,&options->fps)<0) return -1; fps_set=true; }
        else if (!strcmp(key,"--record-frames")) { if(number(value,1,1000,&options->record_frames)<0) return -1; }
        else if (!strcmp(key,"--probe-timeout")) { if(number(value,100,10000,&options->probe_ms)<0) return -1; }
        else if (!strcmp(key,"--wine")) options->wine=value;
        else if (!strcmp(key,"--wine-prefix")) options->prefix=value;
        else if (!strcmp(key,"--wine-log")) options->wine_log=value;
        else if (!strcmp(key,"--wine-cwd")) options->wine_cwd=value;
        else if (!strcmp(key,"--title")) options->title=value;
        else if (!strcmp(key,"--kitty-transfer")) {
            if (!strcmp(value,"direct")) options->kitty_direct=true;
            else if (!strcmp(value,"auto")) options->kitty_direct=false;
            else return -1;
        }
        else if (!strcmp(key,"--log")) options->log=value;
        else if (!strcmp(key,"--record-dir")) options->record_dir=value;
        else if (!strcmp(key,"--duration")) {
            char *end;
            errno=0; options->duration=strtod(value,&end);
            if (errno || !*value || *end || !(options->duration>=0 && options->duration<=86400)) return -1;
        } else return -1;
    }
    if (options->wine && (!options->prefix || !options->wine_log || !options->command || !*options->command)) return -1;
    if (options->wine) {
        if (!width_set) options->width=800;
        if (!height_set) options->height=600;
        if (!fps_set) options->fps=60;
    } else if (options->command) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct application app;
    int status=1, result, log_fd;
    uint64_t deadline, run_start, next_frame;
    memset(&app,0,sizeof(app)); app.terminal.fd=app.terminal.recording_fd=-1;
    (void)setlocale(LC_CTYPE,"");
    app.start=wt_now_ms(); app.running=true;
    result=parse_options(argc,argv,&app.options);
    if (result>0) return 0;
    if (result<0) { usage(stderr); return 2; }
    {
        const char *name=app.options.wine ? app.options.command[0] : argv[0];
        if (app.options.title) name=app.options.title;
        executable_title(app.title,sizeof(app.title),name);
    }
    log_fd=open(app.options.log,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (log_fd>=0) {
        struct stat info;
        if (fstat(log_fd,&info)<0 || !S_ISREG(info.st_mode)) {
            close(log_fd);
            fprintf(stderr,"Log path must name a regular file: %s\n",app.options.log);
            return 1;
        }
    }
    if (log_fd<0 || !(app.log=fdopen(log_fd,"a"))) {
        if(log_fd>=0) close(log_fd);
        fprintf(stderr,"Cannot open log %s: %s\n",app.options.log,strerror(errno)); return 1;
    }
    setvbuf(app.log,NULL,_IOLBF,0);
    if (wt_terminal_open(&app.terminal)<0) { set_error(&app,"no controlling TTY; run this program inside a Kitty terminal"); goto done; }
    if (getrandom(&app.image_id,sizeof(app.image_id),0)!=(ssize_t)sizeof(app.image_id)) {
        set_error(&app,"cannot allocate random session image ID"); goto done;
    }
    app.image_id=(app.image_id&0x7fffffffu)+1;
    wt_input_parser_init(&app.parser,input_event,&app);
    if (winetk_kitty_init(&app.backend,wt_terminal_write,&app.terminal,app.image_id,1)<0) goto io_error;
    if (app.options.wine && winetk_kitty_compress(&app.backend,1)<0) goto io_error;
    measure_terminal(&app);
    if (winetk_backend_probe(&app.backend)<0 || wt_terminal_command(&app.terminal,
        "\033[?u\033[14t\033[16t\033[18t\033[?1016$p\033[?2026$p")<0) goto io_error;
    /* Recordings contain self-contained TTY payloads rather than ephemeral
     * names. Shared memory is negotiated separately and never required. */
    app.shared_probe=app.options.wine && !app.options.kitty_direct && !app.options.record_dir;
    if (app.shared_probe && winetk_kitty_probe_shared(&app.backend,app.image_id+1)<0)
        app.shared_probe=false;
    deadline=wt_now_ms()+app.options.probe_ms;
    while (app.running && !wt_terminal_signal() && wt_now_ms()<deadline) {
        if (read_events(&app,20)<0) goto io_error;
        measure_terminal(&app);
        if (app.graphics && app.keyboard && app.pixel_width && app.pixel_height && app.columns && app.rows &&
            (!app.shared_probe || app.shared_reply)) {
            /* Drain a short bounded tail for mouse/sync/size replies. */
            uint64_t tail=wt_now_ms()+40;
            while (wt_now_ms()<tail && !wt_terminal_signal()) if (read_events(&app,10)<0) goto io_error;
            break;
        }
    }
    if (wt_terminal_signal()) goto done;
    if (!app.running) goto done;
    if (!app.graphics) { set_error(&app,"Kitty graphics probe timed out or is unsupported"); goto done; }
    if (!app.keyboard) { set_error(&app,"Kitty keyboard protocol is required for reliable key release events"); goto done; }
    if (app.shared_memory && winetk_kitty_shared_memory(&app.backend,1)<0) goto io_error;
    if (app.options.fit_resolution) {
        struct wt_viewport fitted;
        uint32_t available_width=app.columns ? app.pixel_width/app.columns*app.columns : 0;
        uint32_t available_height=app.rows>1 ? app.pixel_height/app.rows*(app.rows-1) : 0;
        if (available_width>WT_WINE_MAX_WIDTH) available_width=WT_WINE_MAX_WIDTH;
        if (available_height>WT_WINE_MAX_HEIGHT) available_height=WT_WINE_MAX_HEIGHT;
        if (!wt_viewport_fit(&fitted,app.options.width,app.options.height,available_width,available_height)) {
            set_error(&app,"cannot fit render resolution to terminal"); goto done;
        }
        app.options.width=fitted.width; app.options.height=fitted.height;
        /* Leave room for Wine's hidden non-client frame so Unity does not clamp
         * its requested client area to a smaller virtual work area. */
        app.launch_geometry=(struct wt_launch_geometry){fitted.width,fitted.height,
                                                        fitted.width+64,fitted.height+64};
        log_event(&app,"render_resolution","\"width\":%u,\"height\":%u,\"desktop_width\":%u,\"desktop_height\":%u",
                  fitted.width,fitted.height,app.launch_geometry.desktop_width,app.launch_geometry.desktop_height);
    }
    if (resize_canvas(&app)<0) { set_error(&app,"terminal did not report usable pixel and cell dimensions"); goto done; }
    log_event(&app,"capabilities","\"graphics\":true,\"keyboard\":true,\"pixel_mouse\":%s,\"synchronized\":%s,\"image_id\":%u,\"kitty_transfer\":\"%s\"",
              app.pixel_mouse?"true":"false",app.synchronized?"true":"false",app.image_id,
              app.shared_memory?"shm":"direct");
    if (!app.running) goto done;
    if (app.options.record_dir) {
        char path[PATH_MAX];
        if (mkdir(app.options.record_dir,0700)<0) { set_error(&app,"record directory must be a new path with an existing parent"); goto done; }
        if (snprintf(path,sizeof(path),"%s/graphics.bin",app.options.record_dir)>=(int)sizeof(path)) { errno=ENAMETOOLONG; goto io_error; }
        app.terminal.recording_fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if (app.terminal.recording_fd<0) goto io_error;
    }
    app.logical=malloc((size_t)app.options.width*app.options.height*4);

    if (!app.logical || !app.scaled) { set_error(&app,"cannot allocate canvas buffers"); goto done; }
    app.scene=(struct wt_scene){.width=app.options.width,.height=app.options.height,.focused=true,
                               .box_x=(int32_t)app.options.width*3/5,.box_y=(int32_t)app.options.height*3/7};
    snprintf(app.scene.last_event,sizeof(app.scene.last_event),"WAITING FOR INPUT");
    constrain_box(&app.scene);
    if (wt_terminal_activate(&app.terminal,app.pixel_mouse)<0) goto io_error;
    app.active=true;
    if (app.options.wine) {
        snprintf(app.runtime_dir,sizeof(app.runtime_dir),"/tmp/winetk-%lu-XXXXXX",(unsigned long)getuid());
        if (!mkdtemp(app.runtime_dir)) { set_error(&app,"cannot create private Wine socket directory"); goto done; }
        if (snprintf(app.socket_path,sizeof(app.socket_path),"%s/display.sock",app.runtime_dir)>=(int)sizeof(app.socket_path)) goto io_error;
        app.wine=wt_wine_server_open(app.socket_path,app.options.width,app.options.height);
        if (!app.wine) { set_error(&app,"cannot open Wine display socket"); goto done; }
        if (wt_launcher_start(&app.child,app.options.wine,app.options.prefix,app.socket_path,
                              app.options.wine_log,app.options.wine_cwd,app.options.fps,
                              app.options.fit_resolution ? &app.launch_geometry : NULL,app.options.command)<0) {
            set_error(&app,"cannot launch project Wine"); goto done;
        }
        wt_wine_server_follow_process(app.wine,(uint32_t)app.child.pid);
        log_event(&app,"wine_start","\"pid\":%ld",(long)app.child.pid);
    }
    run_start=next_frame=wt_now_ms();
    while (app.running && !wt_terminal_signal()) {
        uint64_t now=wt_now_ms();
        if (app.options.duration>0 && now-run_start>=(uint64_t)(app.options.duration*1000)) break;
        if (wt_terminal_resized()) {
            app.resized=true;
            if (wt_terminal_command(&app.terminal,"\033[14t\033[16t\033[18t")<0) goto io_error;
        }
        if (app.resized && resize_canvas(&app)<0) { set_error(&app,"terminal resize reported invalid dimensions"); goto done; }
        if (app.wine) {
            /* Wake on IPC readiness, including partial frames. Sleeping on
             * only the TTY added 4ms for every socket-buffer-sized fragment. */
            int changed=wt_wine_server_update(app.wine,4);
            if (changed<0) { set_error(&app,"Wine display receiver failed"); goto done; }
            if (changed) {
                struct winetk_frame frame;
                wt_wine_server_frame(app.wine,&frame);
                if (frame.width!=app.options.width || frame.height!=app.options.height) {
                    app.options.width=frame.width; app.options.height=frame.height;
                    if (resize_canvas(&app)<0) { set_error(&app,"cannot resize Wine client canvas"); goto done; }
                    app.wine_canvas_logged=false;
                }
                if (!app.wine_canvas_logged && wt_wine_server_window_count(app.wine)) {
                    log_event(&app,"wine_canvas","\"width\":%u,\"height\":%u",frame.width,frame.height);
                    app.wine_canvas_logged=true;
                }
                app.dirty=true;
                log_event(&app,"wine_update","\"windows\":%zu,\"clients\":%zu",
                          wt_wine_server_window_count(app.wine),wt_wine_server_client_count(app.wine));
            }
            int exited=wt_launcher_poll(&app.child);
            if (exited<0) { set_error(&app,"cannot wait for Wine child"); goto done; }
            if (exited) { log_event(&app,"wine_exit","\"wait_status\":%d",app.child.status); break; }
        }
        now=wt_now_ms();
        if (now>=next_frame && (app.wine ? app.dirty : (!app.scene.paused || app.dirty))) {
            if (present_frame(&app)<0) goto io_error;
            /* Do not queue catch-up frames after terminal backpressure. */
            next_frame=now+1000/app.options.fps;
            if (next_frame<wt_now_ms()) next_frame=wt_now_ms();
        }
        int wait_ms=app.wine ? 0 : 10;
        if (read_events(&app,wait_ms)<0) goto io_error;
    }
    status=*app.error ? 1 : (app.child.exited ? (WIFEXITED(app.child.status) ? WEXITSTATUS(app.child.status) :
                                              WIFSIGNALED(app.child.status) ? 128+WTERMSIG(app.child.status) : 1) : 0);
    goto done;
io_error:
    if (wt_terminal_signal()) goto done;
    snprintf(app.error,sizeof(app.error),"terminal/recording I/O failed: %s",strerror(errno));
done:
    if (wt_terminal_signal()) status=128+wt_terminal_signal();
    wt_launcher_close(&app.child);
    wt_wine_server_close(app.wine);
    if (*app.runtime_dir) (void)rmdir(app.runtime_dir);
    app.terminal.closing=true;
    app.terminal.record_output=false;
    app.terminal.deadline_ms=wt_now_ms()+500;
    if (app.terminal.fd>=0) (void)wt_terminal_command(&app.terminal,"\030");
    if (app.terminal.alternate_entered) (void)wt_terminal_command(&app.terminal,"\033[?2026l");
    if (app.backend.ops && winetk_backend_close(&app.backend)<0 && status==0) status=1;
    wt_terminal_close(&app.terminal);
    if (app.terminal.recording_fd>=0) close(app.terminal.recording_fd);
    log_event(&app,"summary","\"exit_status\":%d,\"frames\":%llu,\"inputs\":%llu,\"frame_bytes\":%llu,\"elapsed_ms\":%llu,\"max_present_ms\":%llu,\"malformed_sequences\":%llu,\"discarded_sequences\":%llu",
              status,(unsigned long long)app.frames,(unsigned long long)app.inputs,(unsigned long long)app.frame_bytes,
              (unsigned long long)(wt_now_ms()-app.start),(unsigned long long)app.max_present_ms,
              (unsigned long long)app.parser.malformed_sequences,(unsigned long long)app.parser.discarded_sequences);
    if (app.log) {
        bool log_failed=ferror(app.log)!=0;
        if (fclose(app.log)!=0) log_failed=true;
        if (log_failed) {
            if (!*app.error) snprintf(app.error,sizeof(app.error),"cannot write or close session log");
            if (status==0) status=1;
        }
    }
    if (*app.error) fprintf(stderr,"winetk: %s\n",app.error);
    else printf("winetk: %llu frames, %llu input events; log: %s\n",(unsigned long long)app.frames,(unsigned long long)app.inputs,app.options.log);
    free(app.logical); free(app.scaled);
    return status;
}
