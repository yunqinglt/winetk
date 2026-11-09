#include "winetk/input.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

struct collector {
    struct wt_input_event events[256];
    size_t count;
};

static void collect(void *userdata, const struct wt_input_event *event)
{
    struct collector *collector = userdata;
    CHECK(collector->count < sizeof(collector->events) / sizeof(collector->events[0]));
    collector->events[collector->count++] = *event;
}

static void feed(struct wt_input_parser *parser, const char *bytes)
{
    wt_input_parser_feed(parser, bytes, strlen(bytes));
}

static bool same_event(const struct wt_input_event *a, const struct wt_input_event *b)
{
    if (a->type != b->type) return false;
    switch (a->type)
    {
    case WT_INPUT_KEY:
        return a->key.codepoint == b->key.codepoint && a->key.modifiers == b->key.modifiers &&
               a->key.action == b->key.action && a->key.legacy == b->key.legacy &&
               a->key.text_count == b->key.text_count &&
               a->key.shifted_key == b->key.shifted_key && a->key.base_key == b->key.base_key;
    case WT_INPUT_TEXT:
        return a->text.codepoint == b->text.codepoint && a->text.paste == b->text.paste &&
               a->text.length == b->text.length &&
               !memcmp(a->text.utf8, b->text.utf8, a->text.length + 1u);
    case WT_INPUT_MOUSE:
        return a->mouse.action == b->mouse.action && a->mouse.button == b->mouse.button &&
               a->mouse.x == b->mouse.x && a->mouse.y == b->mouse.y &&
               a->mouse.modifiers == b->mouse.modifiers &&
               a->mouse.wheel_x == b->mouse.wheel_x && a->mouse.wheel_y == b->mouse.wheel_y;
    case WT_INPUT_FOCUS: return a->focus.focused == b->focus.focused;
    case WT_INPUT_PASTE: return a->paste.begin == b->paste.begin;
    case WT_INPUT_GRAPHICS_REPLY:
        return a->graphics.image_id == b->graphics.image_id &&
               a->graphics.placement_id == b->graphics.placement_id &&
               a->graphics.has_image_id == b->graphics.has_image_id &&
               a->graphics.has_placement_id == b->graphics.has_placement_id &&
               a->graphics.ok == b->graphics.ok && a->graphics.truncated == b->graphics.truncated &&
               !strcmp(a->graphics.message, b->graphics.message);
    case WT_INPUT_DEVICE_ATTRIBUTES:
        return a->device.prefix == b->device.prefix && a->device.count == b->device.count &&
               !memcmp(a->device.values, b->device.values, a->device.count * sizeof(uint32_t));
    case WT_INPUT_KEYBOARD_FLAGS: return a->keyboard.flags == b->keyboard.flags;
    case WT_INPUT_WINDOW_SIZE:
        return a->window.kind == b->window.kind && a->window.width == b->window.width &&
               a->window.height == b->window.height;
    case WT_INPUT_MODE_REPORT: return a->mode.mode == b->mode.mode && a->mode.value == b->mode.value;
    }
    return false;
}

/* Every split position and one-byte reads must give the exact same event
 * sequence. This catches reliance on terminal read() boundaries. */
static void fragmentation(const char *bytes, const struct collector *expected)
{
    size_t length = strlen(bytes), split_at, i;
    struct wt_input_parser parser;
    struct collector actual;
    for (split_at = 0; split_at <= length; ++split_at)
    {
        memset(&actual, 0, sizeof(actual));
        wt_input_parser_init(&parser, collect, &actual);
        wt_input_parser_feed(&parser, bytes, split_at);
        wt_input_parser_feed(&parser, bytes + split_at, length - split_at);
        CHECK(actual.count == expected->count);
        CHECK(parser.malformed_sequences == 0);
        CHECK(parser.discarded_sequences == 0);
        for (i = 0; i < actual.count; ++i) CHECK(same_event(&actual.events[i], &expected->events[i]));
    }
    memset(&actual, 0, sizeof(actual));
    wt_input_parser_init(&parser, collect, &actual);
    for (i = 0; i < length; ++i) wt_input_parser_feed(&parser, bytes + i, 1);
    CHECK(actual.count == expected->count);
    CHECK(parser.malformed_sequences == 0);
    CHECK(parser.discarded_sequences == 0);
    for (i = 0; i < actual.count; ++i) CHECK(same_event(&actual.events[i], &expected->events[i]));
}

static void test_keyboard(void)
{
    static const char input[] =
        "\033[97:65:97;6:1;65u" /* Ctrl+Shift+A plus associated text. */
        "\033[57350;5:2u"       /* Ctrl+Left repeat. */
        "\033[97;1:3;97u"       /* Release must not type a second 'a'. */
        "\033[0;;20320:22909u"  /* Text-only IME commit. */
        "\033[1;3:3D"           /* Alt+Left release in functional encoding. */
        "\033[24;8:2~"          /* Ctrl+Alt+Shift+F12 repeat. */
        "\303\251\344\270\255\360\237\231\202"; /* é, 中, 🙂 */
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == 11);
    CHECK(result.events[0].type == WT_INPUT_KEY);
    CHECK(result.events[0].key.codepoint == 'a');
    CHECK(result.events[0].key.shifted_key == 'A' && result.events[0].key.base_key == 'a');
    CHECK(result.events[0].key.modifiers == (WT_MOD_CTRL | WT_MOD_SHIFT));
    CHECK(result.events[0].key.action == WT_KEY_PRESS && !result.events[0].key.legacy);
    CHECK(result.events[0].key.text_count == 1);
    CHECK(result.events[1].type == WT_INPUT_TEXT && result.events[1].text.codepoint == 'A');
    CHECK(result.events[2].key.codepoint == WT_KEY_LEFT);
    CHECK(result.events[2].key.action == WT_KEY_REPEAT && result.events[2].key.modifiers == WT_MOD_CTRL);
    CHECK(result.events[2].key.text_count == 0);
    CHECK(result.events[3].type == WT_INPUT_KEY && result.events[3].key.action == WT_KEY_RELEASE);
    CHECK(result.events[3].key.text_count == 0);
    CHECK(result.events[4].type == WT_INPUT_TEXT && result.events[4].text.codepoint == 20320);
    CHECK(result.events[5].type == WT_INPUT_TEXT && result.events[5].text.codepoint == 22909);
    CHECK(result.events[6].key.codepoint == WT_KEY_LEFT && result.events[6].key.modifiers == WT_MOD_ALT);
    CHECK(result.events[6].key.action == WT_KEY_RELEASE);
    CHECK(!result.events[6].key.legacy);
    CHECK(result.events[6].key.text_count == 0);
    CHECK(result.events[7].key.codepoint == WT_KEY_F12 && result.events[7].key.modifiers == 7);
    CHECK(result.events[7].key.action == WT_KEY_REPEAT);
    CHECK(!result.events[7].key.legacy);
    CHECK(result.events[7].key.text_count == 0);
    CHECK(result.events[8].text.codepoint == 0xe9 && result.events[8].text.length == 2);
    CHECK(result.events[9].text.codepoint == 0x4e2d && result.events[9].text.length == 3);
    CHECK(result.events[10].text.codepoint == 0x1f642 && result.events[10].text.length == 4);
    fragmentation(input, &result);
}

static void test_functional_arrow_events(void)
{
    /* Kitty keeps these traditional spellings under keyboard flags 31. A
     * bare press is ambiguous in isolation; typed repeats/releases are not. */
    static const char input[] = "\033[A\033[1;1:2A\033[1;1:3A"
                                "\033[B\033[1;1:3B\033[D\033[1;1:3D"
                                "\033[C\033[1;1:3C";
    static const uint32_t keys[] = {WT_KEY_UP,WT_KEY_UP,WT_KEY_UP,
                                   WT_KEY_DOWN,WT_KEY_DOWN,WT_KEY_LEFT,WT_KEY_LEFT,
                                   WT_KEY_RIGHT,WT_KEY_RIGHT};
    static const enum wt_key_action actions[] = {WT_KEY_PRESS,WT_KEY_REPEAT,WT_KEY_RELEASE,
                                                WT_KEY_PRESS,WT_KEY_RELEASE,WT_KEY_PRESS,WT_KEY_RELEASE,
                                                WT_KEY_PRESS,WT_KEY_RELEASE};
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == sizeof(keys)/sizeof(keys[0]));
    for (size_t i=0;i<result.count;++i) {
        CHECK(result.events[i].type == WT_INPUT_KEY);
        CHECK(result.events[i].key.codepoint == keys[i]);
        CHECK(result.events[i].key.action == actions[i]);
        CHECK(result.events[i].key.legacy == (actions[i] == WT_KEY_PRESS));
    }
    fragmentation(input, &result);
}

static void test_associated_text_count(void)
{
    static const char input[] =
        "\033[97;1:2;97:769:128578u" /* Repeat: a, combining acute, emoji. */
        "\033[57350;1u\344\270\255" /* Arrow, then unrelated plain UTF-8. */
        "\033[97;1:3;97:769u"        /* Release suppresses the entire text field. */
        "\033[A\345\245\275";      /* Legacy arrow and unrelated UTF-8. */
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == 9);
    CHECK(result.events[0].type == WT_INPUT_KEY);
    CHECK(result.events[0].key.action == WT_KEY_REPEAT);
    CHECK(result.events[0].key.text_count == 3);
    CHECK(result.events[1].type == WT_INPUT_TEXT && result.events[1].text.codepoint == 'a');
    CHECK(result.events[2].type == WT_INPUT_TEXT && result.events[2].text.codepoint == 769);
    CHECK(result.events[3].type == WT_INPUT_TEXT && result.events[3].text.codepoint == 128578);
    CHECK(result.events[4].type == WT_INPUT_KEY && !result.events[4].key.legacy);
    CHECK(result.events[4].key.codepoint == WT_KEY_LEFT && result.events[4].key.text_count == 0);
    CHECK(result.events[5].type == WT_INPUT_TEXT && result.events[5].text.codepoint == 0x4e2d);
    CHECK(result.events[6].type == WT_INPUT_KEY && result.events[6].key.action == WT_KEY_RELEASE);
    CHECK(result.events[6].key.text_count == 0);
    CHECK(result.events[7].type == WT_INPUT_KEY && result.events[7].key.legacy);
    CHECK(result.events[7].key.codepoint == WT_KEY_UP && result.events[7].key.text_count == 0);
    CHECK(result.events[8].type == WT_INPUT_TEXT && result.events[8].text.codepoint == 0x597d);
    fragmentation(input, &result);
}

static void test_protocol_interleaving(void)
{
    static const char input[] =
        "v\033_Gi=19,p=2;OK\033\\"
        "\033[<20;123;45M\033[I\033[?62;4;22c\033[?31u"
        "\033[4;720;1280t\033[?1016;1$y\033[<65;125;46M\033[O";
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == 10);
    CHECK(result.events[0].type == WT_INPUT_TEXT && result.events[0].text.codepoint == 'v');
    CHECK(result.events[1].type == WT_INPUT_GRAPHICS_REPLY && result.events[1].graphics.ok);
    CHECK(result.events[1].graphics.has_image_id && result.events[1].graphics.image_id == 19);
    CHECK(result.events[1].graphics.has_placement_id && result.events[1].graphics.placement_id == 2);
    CHECK(!strcmp(result.events[1].graphics.message, "OK"));
    CHECK(result.events[2].type == WT_INPUT_MOUSE && result.events[2].mouse.action == WT_MOUSE_PRESS);
    CHECK(result.events[2].mouse.button == 1 && result.events[2].mouse.x == 123 && result.events[2].mouse.y == 45);
    CHECK(result.events[2].mouse.modifiers == (WT_MOD_SHIFT | WT_MOD_CTRL));
    CHECK(result.events[3].type == WT_INPUT_FOCUS && result.events[3].focus.focused);
    CHECK(result.events[4].type == WT_INPUT_DEVICE_ATTRIBUTES);
    CHECK(result.events[4].device.prefix == '?' && result.events[4].device.count == 3);
    CHECK(result.events[4].device.values[0] == 62 && result.events[4].device.values[2] == 22);
    CHECK(result.events[5].type == WT_INPUT_KEYBOARD_FLAGS && result.events[5].keyboard.flags == 31);
    CHECK(result.events[6].type == WT_INPUT_WINDOW_SIZE && result.events[6].window.kind == 4);
    CHECK(result.events[6].window.width == 1280 && result.events[6].window.height == 720);
    CHECK(result.events[7].type == WT_INPUT_MODE_REPORT && result.events[7].mode.mode == 1016);
    CHECK(result.events[7].mode.value == 1);
    CHECK(result.events[8].mouse.action == WT_MOUSE_WHEEL && result.events[8].mouse.wheel_y == -1);
    CHECK(result.events[9].type == WT_INPUT_FOCUS && !result.events[9].focus.focused);
    fragmentation(input, &result);
}

static void test_mouse_and_legacy(void)
{
    static const char input[] =
        "\033[<0;1;1M\033[<32;2;3M\033[<0;2;3m\033[<35;4;5M"
        "\033[<66;6;7M\033[<67;6;7M\033[<128;8;9M"
        "\033OA\033[Z\033[3~\033[15~\033x\033\303\251\003\t\177";
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == 16);
    CHECK(result.events[0].mouse.button == 1 && result.events[0].mouse.action == WT_MOUSE_PRESS);
    CHECK(result.events[1].mouse.button == 1 && result.events[1].mouse.action == WT_MOUSE_MOVE);
    CHECK(result.events[2].mouse.button == 1 && result.events[2].mouse.action == WT_MOUSE_RELEASE);
    CHECK(result.events[3].mouse.button == 0 && result.events[3].mouse.action == WT_MOUSE_MOVE);
    CHECK(result.events[4].mouse.wheel_x == -1 && result.events[5].mouse.wheel_x == 1);
    CHECK(result.events[6].mouse.button == 8);
    CHECK(result.events[7].key.codepoint == WT_KEY_UP);
    CHECK(result.events[8].key.codepoint == WT_KEY_TAB && result.events[8].key.modifiers == WT_MOD_SHIFT);
    CHECK(result.events[9].key.codepoint == WT_KEY_DELETE && result.events[10].key.codepoint == WT_KEY_F5);
    CHECK(result.events[11].key.codepoint == 'x' && result.events[11].key.modifiers == WT_MOD_ALT);
    CHECK(result.events[12].key.codepoint == 0xe9 && result.events[12].key.modifiers == WT_MOD_ALT);
    CHECK(result.events[13].key.codepoint == 'c' && result.events[13].key.modifiers == WT_MOD_CTRL);
    CHECK(result.events[14].key.codepoint == WT_KEY_TAB && result.events[15].key.codepoint == WT_KEY_BACKSPACE);
    for (size_t i = 7; i < result.count; ++i) CHECK(result.events[i].key.text_count == 0);
    fragmentation(input, &result);
}

static void test_paste(void)
{
    static const char input[] = "\033[200~Hi\033[97;1u\n\344\270\255\033\033[201~\033[97;1:3u";
    static const uint32_t pasted[] = {'H', 'i', 27, '[', '9', '7', ';', '1', 'u', '\n', 0x4e2d, 27};
    struct collector result = {0};
    struct wt_input_parser parser;
    size_t i;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, input);
    CHECK(result.count == 15);
    CHECK(result.events[0].type == WT_INPUT_PASTE && result.events[0].paste.begin);
    for (i = 0; i < sizeof(pasted) / sizeof(pasted[0]); ++i)
    {
        CHECK(result.events[i + 1].type == WT_INPUT_TEXT && result.events[i + 1].text.paste);
        CHECK(result.events[i + 1].text.codepoint == pasted[i]);
    }
    CHECK(result.events[13].type == WT_INPUT_PASTE && !result.events[13].paste.begin);
    CHECK(result.events[14].type == WT_INPUT_KEY && result.events[14].key.action == WT_KEY_RELEASE);
    fragmentation(input, &result);
}

static void test_escape_timeout(void)
{
    struct collector result = {0};
    struct wt_input_parser parser;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, "\033");
    CHECK(wt_input_parser_pending_escape(&parser) && !result.count);
    wt_input_parser_flush(&parser);
    CHECK(!wt_input_parser_pending_escape(&parser) && result.count == 1);
    CHECK(result.events[0].key.codepoint == WT_KEY_ESCAPE && !result.events[0].key.modifiers);
    feed(&parser, "\033[97;");
    CHECK(!wt_input_parser_pending_escape(&parser));
    wt_input_parser_flush(&parser);
    CHECK(result.count == 1);
    feed(&parser, "1u\344\270");
    wt_input_parser_flush(&parser);
    CHECK(result.count == 2 && result.events[1].key.codepoint == 'a');
    feed(&parser, "\255");
    CHECK(result.count == 3 && result.events[2].text.codepoint == 0x4e2d);
    feed(&parser, "\033\033");
    CHECK(result.count == 4 && result.events[3].key.codepoint == WT_KEY_ESCAPE);
    CHECK(result.events[3].key.modifiers == WT_MOD_ALT);
}

static void test_malformed_and_bounds(void)
{
    struct collector result = {0};
    struct wt_input_parser parser;
    char big[WT_INPUT_SEQUENCE_MAX + 512];
    static const char bad_utf8[] = "\300\257\355\240\200\364\220\200\200\342\003";
    size_t i;
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, "\033[999999999999999999999u\033[97;0u\033[97;1:9u"
                  "\033[97;1;65:55296u\033[<0;0;2M\033[<0;1;2147483648M\033[?1016;9$y");
    CHECK(result.count == 0 && parser.malformed_sequences == 7);
    feed(&parser, "\033[123\033[?3u");
    CHECK(result.count == 1 && result.events[0].keyboard.flags == 3);
    feed(&parser, "\033[12\1773u!");
    CHECK(result.count == 2 && result.events[1].text.codepoint == '!');
    CHECK(parser.malformed_sequences == 9);
    memset(big, '9', sizeof(big));
    feed(&parser, "\033[");
    wt_input_parser_feed(&parser, big, sizeof(big));
    feed(&parser, "uQ");
    CHECK(result.count == 3 && result.events[2].text.codepoint == 'Q');
    CHECK(parser.discarded_sequences == 1);
    feed(&parser, "\033_G");
    wt_input_parser_feed(&parser, big, sizeof(big));
    feed(&parser, "\033\\R");
    CHECK(result.count == 4 && result.events[3].text.codepoint == 'R');
    CHECK(parser.discarded_sequences == 2);
    /* Unsolicited control strings cannot masquerade as keyboard input. */
    feed(&parser, "\033]0;bad\033[97u\007\033Pignored\033\\Z");
    CHECK(result.count == 5 && result.events[4].text.codepoint == 'Z');
    CHECK(parser.discarded_sequences == 4);
    wt_input_parser_feed(&parser, bad_utf8, sizeof(bad_utf8) - 1);
    CHECK(result.count == 11);
    for (i = 5; i < 10; ++i)
        CHECK(result.events[i].type == WT_INPUT_TEXT && result.events[i].text.codepoint == 0xfffd);
    CHECK(result.events[10].type == WT_INPUT_KEY && result.events[10].key.codepoint == 'c');
    CHECK(result.events[10].key.modifiers == WT_MOD_CTRL);
    CHECK(parser.malformed_sequences == 14);
    /* No callback is also valid, including text containing NUL in paste. */
    wt_input_parser_init(&parser, NULL, NULL);
    feed(&parser, "\033[200~");
    wt_input_parser_feed(&parser, "\0", 1);
    feed(&parser, "\033[201~");
    CHECK(!parser.paste_active);
}

static void test_graphics_errors(void)
{
    struct wt_input_parser parser;
    struct collector result = {0};
    char long_message[300];
    wt_input_parser_init(&parser, collect, &result);
    feed(&parser, "\033_Gi=7;ENOENT: image missing\033\\");
    CHECK(result.count == 1 && !result.events[0].graphics.ok);
    CHECK(!strcmp(result.events[0].graphics.message, "ENOENT: image missing"));
    feed(&parser, "\033_Gi=7,i=8;OK\033\\\033_Gi=1;bad\nreply\033\\");
    CHECK(result.count == 1 && parser.malformed_sequences == 2);
    memset(long_message, 'x', sizeof(long_message));
    feed(&parser, "\033_Gi=9;");
    wt_input_parser_feed(&parser, long_message, sizeof(long_message));
    feed(&parser, "\033\\");
    CHECK(result.count == 2 && result.events[1].graphics.truncated);
    CHECK(strlen(result.events[1].graphics.message) == WT_INPUT_GRAPHICS_MESSAGE_MAX - 1);
}

static void test_viewport(void)
{
    struct wt_viewport viewport;
    int32_t x, y;
    CHECK(wt_viewport_fit(&viewport, 640, 480, 1280, 720));
    CHECK(viewport.x == 160 && viewport.y == 0 && viewport.width == 960 && viewport.height == 720);
    CHECK(wt_viewport_map(&viewport, 161, 1, true, 0, 0, &x, &y) && x == 0 && y == 0);
    CHECK(wt_viewport_map(&viewport, 641, 361, true, 0, 0, &x, &y) && x == 320 && y == 240);
    CHECK(!wt_viewport_map(&viewport, 160, 1, true, 0, 0, &x, &y) && x == 0 && y == 0);
    CHECK(!wt_viewport_map(&viewport, 1121, 721, true, 0, 0, &x, &y) && x == 639 && y == 479);
    CHECK(wt_viewport_map(&viewport, 41, 13, false, 80, 24, &x, &y) && x == 325 && y == 250);
    CHECK(!wt_viewport_map(&viewport, 1, 1, false, 0, 24, &x, &y) && x == 0 && y == 0);
    CHECK(wt_viewport_fit(&viewport, 1280, 720, 640, 480));
    CHECK(viewport.x == 0 && viewport.y == 60 && viewport.width == 640 && viewport.height == 360);
    CHECK(wt_viewport_map(&viewport, 640, 420, true, 0, 0, &x, &y) && x == 1278 && y == 718);
    CHECK(!wt_viewport_map(&viewport, 641, 421, true, 0, 0, &x, &y) && x == 1279 && y == 719);
    CHECK(wt_viewport_fit(&viewport, INT32_MAX, 1, 1, INT32_MAX));
    CHECK(viewport.width == 1 && viewport.height == 1);
    CHECK(!wt_viewport_fit(&viewport, UINT32_MAX, 1, 1, 1));
    CHECK(!viewport.width && !viewport.height);
    CHECK(!wt_viewport_fit(&viewport, 640, 480, 0, 720));
    CHECK(!wt_viewport_map(&viewport, 1, 1, true, 0, 0, &x, &y));
    CHECK(wt_viewport_fit(&viewport, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX));
    CHECK(wt_viewport_map(&viewport, INT32_MAX, INT32_MAX, false,
                           INT32_MAX, INT32_MAX, &x, &y));
    CHECK(x == INT32_MAX - 1 && y == INT32_MAX - 1);
    viewport.x = UINT32_MAX;
    CHECK(!wt_viewport_map(&viewport, 1, 1, true, 1, 1, &x, &y));
}

int main(void)
{
    test_keyboard();
    test_functional_arrow_events();
    test_associated_text_count();
    test_protocol_interleaving();
    test_mouse_and_legacy();
    test_paste();
    test_escape_timeout();
    test_malformed_and_bounds();
    test_graphics_errors();
    test_viewport();
    puts("input parser and viewport tests passed");
    return 0;
}
