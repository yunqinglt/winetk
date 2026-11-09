#include "winetk/scene.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Small original arrangement of conventional 5x7 bitmap letterforms. */
static const uint8_t letters[36][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static void pixel(const struct wt_scene *s, uint8_t *p, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height) return;
    p += ((size_t)y * s->width + (uint32_t)x) * 4;
    p[0] = (uint8_t)(color >> 16); p[1] = (uint8_t)(color >> 8);
    p[2] = (uint8_t)color; p[3] = 255;
}

static void rect(const struct wt_scene *s, uint8_t *p, int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)s->width ? (int)s->width : x + w;
    int y1 = y + h > (int)s->height ? (int)s->height : y + h;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) pixel(s, p, px, py, color);
}

static void text(const struct wt_scene *s, uint8_t *p, int x, int y, const char *value, int scale, uint32_t color)
{
    for (; *value; ++value, x += 6 * scale) {
        unsigned char c = (unsigned char)toupper((unsigned char)*value);
        const uint8_t *glyph = NULL;
        uint8_t punctuation[5] = {0};
        if (c >= '0' && c <= '9') glyph = letters[c - '0'];
        else if (c >= 'A' && c <= 'Z') glyph = letters[c - 'A' + 10];
        else {
            if (c == '-') memset(punctuation, 8, 5);
            if (c == '/') { punctuation[0]=0x60; punctuation[1]=0x18; punctuation[2]=6; punctuation[3]=1; }
            if (c == ':') punctuation[2] = 0x24;
            if (c == '.') punctuation[2] = 0x40;
            if (c == '+') { memset(punctuation,8,5); punctuation[2]=0x3e; }
            glyph = punctuation;
        }
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (glyph[col] & (1u << row)) rect(s,p,x+col*scale,y+row*scale,scale,scale,color);
    }
}

void wt_scene_render(const struct wt_scene *s, uint8_t *p)
{
    char label[128];
    for (uint32_t y = 0; y < s->height; ++y) {
        for (uint32_t x = 0; x < s->width; ++x) {
            uint32_t grid = (x % 32 == 0 || y % 32 == 0) ? 12 : 0;
            uint32_t color = ((14 + grid) << 16) | ((22 + grid + y * 16 / s->height) << 8) | (35 + grid);
            pixel(s,p,(int)x,(int)y,color);
        }
    }
    rect(s,p,0,0,(int)s->width,48,0x152235);
    text(s,p,16,12,"WINETK / KITTY CANVAS",2,0xeeeeee);
    rect(s,p,16,65,70,14,0xe45d61); rect(s,p,90,65,70,14,0x68c990); rect(s,p,164,65,70,14,0x68a9eb);
    text(s,p,250,68,s->focused ? "FOCUS ON" : "FOCUS OFF",1,0xbed1e6);
    rect(s,p,24,102,240,120,0x0a101c); rect(s,p,20,98,240,120,0x253951);
    rect(s,p,20,98,240,23,0x3d6c8b); text(s,p,30,106,"PIXEL FORWARDING PROTOTYPE",1,0xffffff);
    text(s,p,32,137,"ARROWS MOVE THE ORANGE BOX",1,0xd9e3ee);
    text(s,p,32,155,"MOUSE DRAGS / WHEEL MOVES",1,0xd9e3ee);
    text(s,p,32,173,"SPACE PAUSES THE ANIMATION",1,0xd9e3ee);
    text(s,p,32,191,"CTRL+C OR ESC TO EXIT",1,0xd9e3ee);
    rect(s,p,s->box_x+4,s->box_y+4,112,64,0x101521);
    rect(s,p,s->box_x,s->box_y,112,64,s->dragging ? 0xffc46b : 0xe88b49);
    text(s,p,s->box_x+10,s->box_y+12,"DRAG ME",2,0x211f27);
    text(s,p,s->box_x+10,s->box_y+40,"LOGICAL PIXELS",1,0x211f27);
    {
        int span = s->width > 36 ? (int)s->width - 36 : 1;
        int x = 8 + (int)((s->frame * 4) % (unsigned)span);
        rect(s,p,x,(int)s->height-80,20,8,0x6acfc1);
    }
    rect(s,p,0,(int)s->height-62,(int)s->width,62,0x152235);
    snprintf(label,sizeof(label),"FRAME %llu / TEXT %llu / %ux%u",(unsigned long long)s->frame,
             (unsigned long long)s->text_count,s->width,s->height);
    text(s,p,12,(int)s->height-50,label,1,0x9dbbd6);
    text(s,p,12,(int)s->height-32,s->last_event,1,0xe4ecf4);
    text(s,p,12,(int)s->height-14,"RGBA8 / DIRECT TRANSFER / NO WINE DRIVER YET",1,0x9dbbd6);
    if (s->pointer_inside) {
        rect(s,p,s->pointer_x-8,s->pointer_y,17,1,0xffffff);
        rect(s,p,s->pointer_x,s->pointer_y-8,1,17,0xffffff);
    }
}

/* Pixel-center sampling with edge clamping. Eight fractional bits keep the
 * interpolator integer-only; four 8-bit channels remain within uint32_t. */
static uint64_t sample_position(uint32_t index, uint32_t source, uint32_t output)
{
    uint64_t center=((uint64_t)index*2+1)*source*128/output;
    uint64_t maximum=(uint64_t)(source-1)*256;
    if (center<128) return 0;
    center-=128;
    return center>maximum ? maximum : center;
}

void wt_scale_rgba(const uint8_t *source, uint32_t sw, uint32_t sh,
                   uint8_t *destination, uint32_t width, uint32_t height)
{
    if (!sw || !sh || !width || !height) return;
    if (sw==width && sh==height) {
        if (source!=destination) memcpy(destination,source,(size_t)width*height*4);
        return;
    }
    if (width>sw || height>sh) {
        for (uint32_t y=0;y<height;++y) {
            uint64_t sy=sample_position(y,sh,height);
            uint32_t y0=sy>>8, y1=y0+1<sh ? y0+1 : y0, fy=sy&255;
            for (uint32_t x=0;x<width;++x) {
                uint64_t sx=sample_position(x,sw,width);
                uint32_t x0=sx>>8, x1=x0+1<sw ? x0+1 : x0, fx=sx&255;
                const uint8_t *a=source+((size_t)y0*sw+x0)*4;
                const uint8_t *b=source+((size_t)y0*sw+x1)*4;
                const uint8_t *c=source+((size_t)y1*sw+x0)*4;
                const uint8_t *d=source+((size_t)y1*sw+x1)*4;
                for (unsigned channel=0;channel<4;++channel) {
                    uint32_t top=a[channel]*(256-fx)+b[channel]*fx;
                    uint32_t bottom=c[channel]*(256-fx)+d[channel]*fx;
                    destination[((size_t)y*width+x)*4+channel]=
                        (uint8_t)((top*(256-fy)+bottom*fy+32768)>>16);
                }
            }
        }
        return;
    }
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            size_t sy = (uint64_t)y * sh / height, sx = (uint64_t)x * sw / width;
            memcpy(destination + ((size_t)y * width + x) * 4, source + (sy * sw + sx) * 4, 4);
        }
}
