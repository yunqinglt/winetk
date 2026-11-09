#include "winetk/input.h"

#include <limits.h>
#include <string.h>

enum parser_state {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_DISCARD_CSI,
    STATE_SS3,
    STATE_STRING,
    STATE_PASTE
};

struct slice { const char *data; size_t length; };

static void emit(struct wt_input_parser *parser, const struct wt_input_event *event)
{
    if (parser->callback) parser->callback(parser->userdata, event);
}

static bool scalar(uint32_t value)
{
    return value <= 0x10ffffu && !(value >= 0xd800u && value <= 0xdfffu);
}

static bool number(struct slice text, uint32_t *value)
{
    uint32_t result = 0;
    size_t i;
    if (!text.length) return false;
    for (i = 0; i < text.length; ++i)
    {
        unsigned digit = (unsigned char)text.data[i] - (unsigned)'0';
        if (digit > 9 || result > (UINT32_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

static bool split(struct slice text, char separator, struct slice *parts,
                  size_t capacity, size_t *count)
{
    size_t start = 0, i, used = 0;
    for (i = 0; i <= text.length; ++i)
    {
        if (i != text.length && text.data[i] != separator) continue;
        if (used == capacity) return false;
        parts[used].data = text.data + start;
        parts[used++].length = i - start;
        start = i + 1;
    }
    *count = used;
    return true;
}

static void key(struct wt_input_parser *parser, uint32_t codepoint,
                uint32_t modifiers, enum wt_key_action action, bool legacy,
                uint32_t shifted, uint32_t base, uint32_t text_count)
{
    struct wt_input_event event = {0};
    event.type = WT_INPUT_KEY;
    event.key.codepoint = codepoint;
    event.key.modifiers = modifiers;
    event.key.action = action;
    event.key.legacy = legacy;
    event.key.text_count = text_count;
    event.key.shifted_key = shifted;
    event.key.base_key = base;
    emit(parser, &event);
}

static void text(struct wt_input_parser *parser, uint32_t codepoint)
{
    struct wt_input_event event = {0};
    event.type = WT_INPUT_TEXT;
    event.text.codepoint = codepoint;
    event.text.paste = parser->paste_active;
    if (codepoint < 0x80)
    {
        event.text.utf8[0] = (char)codepoint;
        event.text.length = 1;
    }
    else if (codepoint < 0x800)
    {
        event.text.utf8[0] = (char)(0xc0 | (codepoint >> 6));
        event.text.utf8[1] = (char)(0x80 | (codepoint & 0x3f));
        event.text.length = 2;
    }
    else if (codepoint < 0x10000)
    {
        event.text.utf8[0] = (char)(0xe0 | (codepoint >> 12));
        event.text.utf8[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        event.text.utf8[2] = (char)(0x80 | (codepoint & 0x3f));
        event.text.length = 3;
    }
    else
    {
        event.text.utf8[0] = (char)(0xf0 | (codepoint >> 18));
        event.text.utf8[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        event.text.utf8[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        event.text.utf8[3] = (char)(0x80 | (codepoint & 0x3f));
        event.text.length = 4;
    }
    emit(parser, &event);
}

static void character(struct wt_input_parser *parser, uint32_t codepoint,
                      uint32_t modifiers)
{
    if (parser->paste_active) text(parser, codepoint);
    else if (modifiers)
        key(parser, codepoint, modifiers, WT_KEY_PRESS, true, 0, 0, 0);
    else text(parser, codepoint);
}

/* UTF-8 is decoded independently of escape-sequence framing. Invalid scalars
 * produce U+FFFD and cannot inject CSI/APC events through overlong encodings. */
static void utf8_byte(struct wt_input_parser *parser, unsigned char byte,
                     uint32_t modifiers)
{
    if (parser->utf8_remaining)
    {
        if ((byte & 0xc0) == 0x80)
        {
            parser->utf8_codepoint = (parser->utf8_codepoint << 6) | (byte & 0x3f);
            if (!--parser->utf8_remaining)
            {
                uint32_t value = parser->utf8_codepoint;
                if (!scalar(value) || value < parser->utf8_minimum)
                {
                    ++parser->malformed_sequences;
                    value = 0xfffd;
                }
                character(parser, value, parser->utf8_modifiers);
            }
            return;
        }
        parser->utf8_remaining = 0;
        ++parser->malformed_sequences;
        character(parser, 0xfffd, parser->utf8_modifiers);
    }
    if (byte < 0x80) character(parser, byte, modifiers);
    else if (byte >= 0xc2 && byte <= 0xf4)
    {
        parser->utf8_modifiers = modifiers;
        if (byte < 0xe0)
        {
            parser->utf8_codepoint = byte & 0x1f;
            parser->utf8_minimum = 0x80;
            parser->utf8_remaining = 1;
        }
        else if (byte < 0xf0)
        {
            parser->utf8_codepoint = byte & 0x0f;
            parser->utf8_minimum = 0x800;
            parser->utf8_remaining = 2;
        }
        else
        {
            parser->utf8_codepoint = byte & 7;
            parser->utf8_minimum = 0x10000;
            parser->utf8_remaining = 3;
        }
    }
    else
    {
        ++parser->malformed_sequences;
        character(parser, 0xfffd, modifiers);
    }
}

static void interrupt_utf8(struct wt_input_parser *parser)
{
    if (!parser->utf8_remaining) return;
    parser->utf8_remaining = 0;
    ++parser->malformed_sequences;
    character(parser, 0xfffd, parser->utf8_modifiers);
}

static void legacy_byte(struct wt_input_parser *parser, unsigned char byte,
                        uint32_t modifiers)
{
    uint32_t codepoint = byte;
    if (parser->utf8_remaining && (byte & 0xc0) != 0x80) interrupt_utf8(parser);
    if (parser->utf8_remaining || byte >= 0x80)
    {
        utf8_byte(parser, byte, modifiers);
        return;
    }
    if (byte < 0x20 || byte == 0x7f)
    {
        if (byte == 0) { codepoint = ' '; modifiers |= WT_MOD_CTRL; }
        else if (byte == 9) codepoint = WT_KEY_TAB;
        else if (byte == 13) codepoint = WT_KEY_ENTER;
        else if (byte == 8) { codepoint = WT_KEY_BACKSPACE; modifiers |= WT_MOD_CTRL; }
        else if (byte == 27) codepoint = WT_KEY_ESCAPE;
        else if (byte == 0x7f) codepoint = WT_KEY_BACKSPACE;
        else
        {
            codepoint = byte < 27 ? (uint32_t)('a' + byte - 1)
                                 : (uint32_t)('\\' + byte - 28);
            modifiers |= WT_MOD_CTRL;
        }
        key(parser, codepoint, modifiers, WT_KEY_PRESS, true, 0, 0, 0);
    }
    else utf8_byte(parser, byte, modifiers);
}

static bool modifiers(struct slice field, uint32_t *mods,
                      enum wt_key_action *action)
{
    struct slice parts[2];
    size_t count;
    uint32_t encoded = 1, type = WT_KEY_PRESS;
    if (!split(field, ':', parts, 2, &count)) return false;
    if (parts[0].length && !number(parts[0], &encoded)) return false;
    if (!encoded) return false;
    if (count == 2 && (!number(parts[1], &type) || type < 1 || type > 3)) return false;
    *mods = encoded - 1;
    *action = (enum wt_key_action)type;
    return true;
}

static bool associated_text(struct wt_input_parser *parser, struct slice field,
                            bool deliver, uint32_t *text_count)
{
    size_t start = 0, i;
    uint32_t count = 0;
    for (i = 0; i <= field.length; ++i)
    {
        uint32_t codepoint;
        struct slice item;
        if (i != field.length && field.data[i] != ':') continue;
        item.data = field.data + start;
        item.length = i - start;
        if (!number(item, &codepoint) || !scalar(codepoint) || codepoint < 0x20 ||
            (codepoint >= 0x7f && codepoint <= 0x9f)) return false;
        if (deliver) text(parser, codepoint);
        ++count;
        start = i + 1;
    }
    if (text_count) *text_count = count;
    return true;
}

static bool enhanced_key(struct wt_input_parser *parser, struct slice body)
{
    struct slice fields[3], codes[3];
    size_t count, code_count;
    uint32_t codepoint, shifted = 0, base = 0, mods = 0, text_count = 0;
    enum wt_key_action action = WT_KEY_PRESS;
    if (!split(body, ';', fields, 3, &count) ||
        !split(fields[0], ':', codes, 3, &code_count) ||
        !number(codes[0], &codepoint) || !scalar(codepoint)) return false;
    if (code_count > 1 && codes[1].length &&
        (!number(codes[1], &shifted) || !scalar(shifted))) return false;
    if (code_count > 2 && codes[2].length &&
        (!number(codes[2], &base) || !scalar(base))) return false;
    if (count > 1 && !modifiers(fields[1], &mods, &action)) return false;
    if (count > 2 && !associated_text(parser, fields[2], false, &text_count)) return false;
    if (codepoint)
        key(parser, codepoint, mods, action, false, shifted, base,
            action != WT_KEY_RELEASE ? text_count : 0);
    if (count > 2 && action != WT_KEY_RELEASE)
        (void)associated_text(parser, fields[2], true, NULL);
    return true;
}

static uint32_t legacy_key_code(unsigned char final, uint32_t first)
{
    if (final == '~')
    {
        switch (first)
        {
        case 1: case 7: return WT_KEY_HOME;
        case 2: return WT_KEY_INSERT;
        case 3: return WT_KEY_DELETE;
        case 4: case 8: return WT_KEY_END;
        case 5: return WT_KEY_PAGE_UP;
        case 6: return WT_KEY_PAGE_DOWN;
        case 11: return WT_KEY_F1;
        case 12: return WT_KEY_F2;
        case 13: return WT_KEY_F3;
        case 14: return WT_KEY_F4;
        case 15: return WT_KEY_F5;
        case 17: return WT_KEY_F6;
        case 18: return WT_KEY_F7;
        case 19: return WT_KEY_F8;
        case 20: return WT_KEY_F9;
        case 21: return WT_KEY_F10;
        case 23: return WT_KEY_F11;
        case 24: return WT_KEY_F12;
        case 29: return WT_KEY_MENU;
        default: return 0;
        }
    }
    if (first != 1) return 0;
    switch (final)
    {
    case 'A': return WT_KEY_UP;
    case 'B': return WT_KEY_DOWN;
    case 'C': return WT_KEY_RIGHT;
    case 'D': return WT_KEY_LEFT;
    case 'H': return WT_KEY_HOME;
    case 'F': return WT_KEY_END;
    case 'P': return WT_KEY_F1;
    case 'Q': return WT_KEY_F2;
    case 'R': return WT_KEY_F3;
    case 'S': return WT_KEY_F4;
    case 'Z': return WT_KEY_TAB;
    default: return 0;
    }
}

static bool mouse(struct wt_input_parser *parser, struct slice body,
                  unsigned char final)
{
    struct slice fields[3];
    size_t count;
    uint32_t button, x, y, base;
    struct wt_input_event event = {0};
    ++body.data;
    --body.length;
    if (!split(body, ';', fields, 3, &count) || count != 3 ||
        !number(fields[0], &button) || button > 255 ||
        !number(fields[1], &x) || !x || x > INT32_MAX ||
        !number(fields[2], &y) || !y || y > INT32_MAX) return false;
    event.type = WT_INPUT_MOUSE;
    event.mouse.x = (int32_t)x;
    event.mouse.y = (int32_t)y;
    event.mouse.modifiers = ((button & 4) ? WT_MOD_SHIFT : 0) |
                            ((button & 8) ? WT_MOD_ALT : 0) |
                            ((button & 16) ? WT_MOD_CTRL : 0);
    base = button & 3;
    if (button & 64)
    {
        event.mouse.action = WT_MOUSE_WHEEL;
        if (base < 2) event.mouse.wheel_y = base ? -1 : 1;
        else event.mouse.wheel_x = base == 2 ? -1 : 1;
    }
    else
    {
        event.mouse.button = (button & 128) ? 8 + base : (base == 3 ? 0 : base + 1);
        if (final == 'm' || (!(button & 32) && base == 3 && !(button & 128)))
            event.mouse.action = WT_MOUSE_RELEASE;
        else if (button & 32) event.mouse.action = WT_MOUSE_MOVE;
        else event.mouse.action = WT_MOUSE_PRESS;
    }
    emit(parser, &event);
    return true;
}

static bool numeric_reply(struct wt_input_parser *parser, struct slice body,
                          unsigned char final)
{
    struct slice fields[WT_INPUT_DEVICE_VALUES_MAX];
    size_t count, i;
    struct wt_input_event event = {0};
    if (final == 'u')
    {
        ++body.data;
        --body.length;
        if (!number(body, &event.keyboard.flags)) return false;
        event.type = WT_INPUT_KEYBOARD_FLAGS;
    }
    else if (final == 'y')
    {
        if (body.length < 2 || body.data[0] != '?' ||
            body.data[body.length - 1] != '$') return false;
        ++body.data;
        body.length -= 2;
        if (!split(body, ';', fields, 2, &count) || count != 2 ||
            !number(fields[0], &event.mode.mode) ||
            !number(fields[1], &event.mode.value) || event.mode.value > 4) return false;
        event.type = WT_INPUT_MODE_REPORT;
    }
    else if (final == 't')
    {
        if (!split(body, ';', fields, 3, &count) || count != 3 ||
            !number(fields[0], &event.window.kind) ||
            !number(fields[1], &event.window.height) ||
            !number(fields[2], &event.window.width) ||
            (event.window.kind != 4 && event.window.kind != 6 && event.window.kind != 8) ||
            !event.window.width || !event.window.height ||
            event.window.width > INT32_MAX || event.window.height > INT32_MAX) return false;
        event.type = WT_INPUT_WINDOW_SIZE;
    }
    else
    {
        event.type = WT_INPUT_DEVICE_ATTRIBUTES;
        if (body.length && (body.data[0] == '?' || body.data[0] == '>' || body.data[0] == '='))
        {
            event.device.prefix = body.data[0];
            ++body.data;
            --body.length;
        }
        if (!split(body, ';', fields, WT_INPUT_DEVICE_VALUES_MAX, &count)) return false;
        for (i = 0; i < count; ++i)
            if (!number(fields[i], &event.device.values[i])) return false;
        event.device.count = count;
    }
    emit(parser, &event);
    return true;
}

static void csi(struct wt_input_parser *parser, unsigned char final)
{
    struct slice body = { parser->sequence, parser->sequence_length };
    struct slice fields[2];
    size_t count;
    uint32_t first = 1, mods = 0, codepoint;
    enum wt_key_action action = WT_KEY_PRESS;
    struct wt_input_event event = {0};
    bool valid = false;
    if ((final == 'M' || final == 'm') && body.length && body.data[0] == '<')
        valid = mouse(parser, body, final);
    else if ((final == 'I' || final == 'O') && !body.length)
    {
        event.type = WT_INPUT_FOCUS;
        event.focus.focused = final == 'I';
        emit(parser, &event);
        valid = true;
    }
    else if ((final == 'u' && body.length && body.data[0] == '?') ||
             final == 'c' || final == 't' || final == 'y')
        valid = numeric_reply(parser, body, final);
    else if (final == 'u') valid = enhanced_key(parser, body);
    else if (strchr("~ABCDFHPQRSZ", final))
    {
        if (!split(body, ';', fields, 2, &count) ||
            (fields[0].length && !number(fields[0], &first)) ||
            (count > 1 && !modifiers(fields[1], &mods, &action))) goto malformed;
        if (final == '~' && first == 200 && count == 1)
        {
            event.type = WT_INPUT_PASTE;
            event.paste.begin = true;
            parser->paste_active = true;
            parser->paste_match = 0;
            parser->state = STATE_PASTE;
            emit(parser, &event);
            valid = true;
        }
        else if ((codepoint = legacy_key_code(final, first)))
        {
            bool has_event_type = count > 1 && memchr(fields[1].data, ':', fields[1].length);
            if (final == 'Z') mods |= WT_MOD_SHIFT;
            key(parser, codepoint, mods, action, !has_event_type, 0, 0, 0);
            valid = true;
        }
        else
        {
            ++parser->discarded_sequences;
            return;
        }
    }
    else
    {
        ++parser->discarded_sequences;
        return;
    }
    if (valid) return;
malformed:
    ++parser->malformed_sequences;
}

static void graphics_reply(struct wt_input_parser *parser)
{
    struct wt_input_event event = {0};
    const char *data = parser->sequence;
    size_t length = parser->sequence_length, semi = 0, start, i;
    if (!length || data[0] != 'G')
    {
        ++parser->discarded_sequences;
        return;
    }
    while (semi < length && data[semi] != ';') ++semi;
    if (semi == length) goto malformed;
    event.type = WT_INPUT_GRAPHICS_REPLY;
    start = 1;
    for (i = 1; i <= semi; ++i)
    {
        struct slice value;
        uint32_t parsed;
        if (i != semi && data[i] != ',') continue;
        if (i - start < 3 || data[start + 1] != '=' ||
            !((data[start] >= 'A' && data[start] <= 'Z') ||
              (data[start] >= 'a' && data[start] <= 'z'))) goto malformed;
        value.data = data + start + 2;
        value.length = i - start - 2;
        if (!number(value, &parsed)) goto malformed;
        if (data[start] == 'i')
        {
            if (event.graphics.has_image_id) goto malformed;
            event.graphics.image_id = parsed;
            event.graphics.has_image_id = true;
        }
        else if (data[start] == 'p')
        {
            if (event.graphics.has_placement_id) goto malformed;
            event.graphics.placement_id = parsed;
            event.graphics.has_placement_id = true;
        }
        start = i + 1;
    }
    length -= semi + 1;
    data += semi + 1;
    if (!length) goto malformed;
    for (i = 0; i < length; ++i)
        if ((unsigned char)data[i] < 32 || (unsigned char)data[i] > 126) goto malformed;
    if (length >= sizeof(event.graphics.message))
    {
        event.graphics.truncated = true;
        length = sizeof(event.graphics.message) - 1;
    }
    memcpy(event.graphics.message, data, length);
    event.graphics.ok = length == 2 && !memcmp(data, "OK", 2);
    emit(parser, &event);
    return;
malformed:
    ++parser->malformed_sequences;
}

static void append_string(struct wt_input_parser *parser, unsigned char byte)
{
    if (parser->string_discard) return;
    if (parser->sequence_length == sizeof(parser->sequence))
    {
        parser->string_discard = true;
        ++parser->discarded_sequences;
        return;
    }
    parser->sequence[parser->sequence_length++] = (char)byte;
}

static void paste_byte(struct wt_input_parser *parser, unsigned char byte)
{
    static const unsigned char end[] = "\033[201~";
    unsigned i;
    if (byte == end[parser->paste_match])
    {
        if (++parser->paste_match == sizeof(end) - 1)
        {
            struct wt_input_event event = {0};
            interrupt_utf8(parser);
            parser->paste_match = 0;
            parser->paste_active = false;
            parser->state = STATE_GROUND;
            event.type = WT_INPUT_PASTE;
            event.paste.begin = false;
            emit(parser, &event);
        }
        return;
    }
    for (i = 0; i < parser->paste_match; ++i) utf8_byte(parser, end[i], 0);
    parser->paste_match = 0;
    if (byte == end[0]) parser->paste_match = 1;
    else utf8_byte(parser, byte, 0);
}

void wt_input_parser_init(struct wt_input_parser *parser,
                          wt_input_callback callback, void *userdata)
{
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->userdata = userdata;
}

void wt_input_parser_feed(struct wt_input_parser *parser, const void *bytes,
                          size_t length)
{
    const unsigned char *data = bytes;
    size_t i;
    for (i = 0; i < length; ++i)
    {
        unsigned char byte = data[i];
        switch (parser->state)
        {
        case STATE_GROUND:
            if (byte == 27)
            {
                interrupt_utf8(parser);
                parser->state = STATE_ESCAPE;
            }
            else legacy_byte(parser, byte, 0);
            break;
        case STATE_ESCAPE:
            parser->sequence_length = 0;
            if (byte == '[') parser->state = STATE_CSI;
            else if (byte == 'O') parser->state = STATE_SS3;
            else if (byte == '_' || byte == ']' || byte == 'P' || byte == '^' || byte == 'X')
            {
                parser->state = STATE_STRING;
                parser->string_escape = false;
                parser->string_osc = byte == ']';
                /* Only APC is meaningful to the prototype. Other strings are
                 * consumed to their terminator without becoming keyboard input. */
                parser->string_discard = byte != '_';
                if (parser->string_discard) ++parser->discarded_sequences;
            }
            else
            {
                parser->state = STATE_GROUND;
                legacy_byte(parser, byte, WT_MOD_ALT);
            }
            break;
        case STATE_CSI:
        case STATE_DISCARD_CSI:
            if (byte == 27)
            {
                ++parser->malformed_sequences;
                parser->state = STATE_ESCAPE;
            }
            else if (byte >= 0x40 && byte <= 0x7e)
            {
                bool discard = parser->state == STATE_DISCARD_CSI;
                parser->state = STATE_GROUND;
                if (!discard) csi(parser, byte);
            }
            else if (parser->state == STATE_CSI)
            {
                if (byte < 0x20 || byte > 0x3f)
                {
                    ++parser->malformed_sequences;
                    parser->state = STATE_DISCARD_CSI;
                }
                else if (parser->sequence_length == sizeof(parser->sequence))
                {
                    ++parser->discarded_sequences;
                    parser->state = STATE_DISCARD_CSI;
                }
                else parser->sequence[parser->sequence_length++] = (char)byte;
            }
            break;
        case STATE_SS3:
        {
            uint32_t codepoint = legacy_key_code(byte, 1);
            parser->state = STATE_GROUND;
            if (codepoint) key(parser, codepoint, 0, WT_KEY_PRESS, true, 0, 0, 0);
            else if (byte == 27) parser->state = STATE_ESCAPE;
            else ++parser->discarded_sequences;
            break;
        }
        case STATE_STRING:
            if ((parser->string_escape && byte == '\\') ||
                (parser->string_osc && byte == 7))
            {
                parser->state = STATE_GROUND;
                if (!parser->string_discard) graphics_reply(parser);
                parser->string_escape = false;
            }
            else
            {
                if (parser->string_escape) append_string(parser, 27);
                parser->string_escape = byte == 27;
                if (!parser->string_escape) append_string(parser, byte);
            }
            break;
        case STATE_PASTE:
            paste_byte(parser, byte);
            break;
        default:
            parser->state = STATE_GROUND;
            break;
        }
    }
}

bool wt_input_parser_pending_escape(const struct wt_input_parser *parser)
{
    return parser->state == STATE_ESCAPE;
}

void wt_input_parser_flush(struct wt_input_parser *parser)
{
    if (!wt_input_parser_pending_escape(parser)) return;
    parser->state = STATE_GROUND;
    key(parser, WT_KEY_ESCAPE, 0, WT_KEY_PRESS, true, 0, 0, 0);
}

bool wt_viewport_fit(struct wt_viewport *viewport,
                     uint32_t logical_width, uint32_t logical_height,
                     uint32_t terminal_width, uint32_t terminal_height)
{
    struct wt_viewport fitted = {0};
    if (!logical_width || !logical_height || !terminal_width || !terminal_height ||
        logical_width > INT32_MAX || logical_height > INT32_MAX ||
        terminal_width > INT32_MAX || terminal_height > INT32_MAX)
    {
        *viewport = fitted;
        return false;
    }
    fitted.logical_width = logical_width;
    fitted.logical_height = logical_height;
    fitted.terminal_width = terminal_width;
    fitted.terminal_height = terminal_height;
    if ((uint64_t)terminal_width * logical_height <= (uint64_t)terminal_height * logical_width)
    {
        fitted.width = terminal_width;
        fitted.height = (uint32_t)((uint64_t)terminal_width * logical_height / logical_width);
        if (!fitted.height) fitted.height = 1;
    }
    else
    {
        fitted.height = terminal_height;
        fitted.width = (uint32_t)((uint64_t)terminal_height * logical_width / logical_height);
        if (!fitted.width) fitted.width = 1;
    }
    fitted.x = (terminal_width - fitted.width) / 2;
    fitted.y = (terminal_height - fitted.height) / 2;
    *viewport = fitted;
    return true;
}

bool wt_viewport_map(const struct wt_viewport *viewport,
                     int32_t terminal_x, int32_t terminal_y, bool pixels,
                     uint32_t columns, uint32_t rows,
                     int32_t *logical_x, int32_t *logical_y)
{
    uint64_t px, py, dx, dy;
    bool inside;
    *logical_x = *logical_y = 0;
    if (!viewport->logical_width || !viewport->logical_height ||
        viewport->logical_width > INT32_MAX || viewport->logical_height > INT32_MAX ||
        !viewport->terminal_width || !viewport->terminal_height ||
        viewport->terminal_width > INT32_MAX || viewport->terminal_height > INT32_MAX ||
        !viewport->width || !viewport->height ||
        (uint64_t)viewport->x + viewport->width > viewport->terminal_width ||
        (uint64_t)viewport->y + viewport->height > viewport->terminal_height ||
        terminal_x <= 0 || terminal_y <= 0) return false;
    if (pixels)
    {
        px = (uint32_t)terminal_x - 1u;
        py = (uint32_t)terminal_y - 1u;
    }
    else
    {
        if (!columns || !rows || columns > INT32_MAX || rows > INT32_MAX) return false;
        px = ((uint64_t)(uint32_t)terminal_x * 2 - 1) * viewport->terminal_width /
             ((uint64_t)columns * 2);
        py = ((uint64_t)(uint32_t)terminal_y * 2 - 1) * viewport->terminal_height /
             ((uint64_t)rows * 2);
    }
    inside = px >= viewport->x && px < (uint64_t)viewport->x + viewport->width &&
             py >= viewport->y && py < (uint64_t)viewport->y + viewport->height;
    dx = px < viewport->x ? 0 : px - viewport->x;
    dy = py < viewport->y ? 0 : py - viewport->y;
    if (dx >= viewport->width) dx = viewport->width - 1;
    if (dy >= viewport->height) dy = viewport->height - 1;
    *logical_x = px >= (uint64_t)viewport->x + viewport->width
                 ? (int32_t)viewport->logical_width - 1
                 : (int32_t)(dx * viewport->logical_width / viewport->width);
    *logical_y = py >= (uint64_t)viewport->y + viewport->height
                 ? (int32_t)viewport->logical_height - 1
                 : (int32_t)(dy * viewport->logical_height / viewport->height);
    return inside;
}
