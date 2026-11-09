/* A real Win32 program for the terminal Wine driver's GDI and input smoke test. */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef _WIN64
#define SMOKE_ARCH "x86_64"
#else
#define SMOKE_ARCH "i386"
#endif

static struct
{
    DWORD started, duration_ms;
    unsigned int paints, ticks, key_downs, chars, moves, left_clicks, right_clicks;
    int mouse_x, mouse_y, wheel, offset, client_width, client_height;
    WCHAR text[96];
    size_t text_length;
    const WCHAR *status_path;
    HFONT title_font, body_font;
} state;

static void write_status(const char *phase)
{
    FILE *file;
    if (!state.status_path || !(file = _wfopen(state.status_path, L"wb"))) return;
    fprintf(file,
            "phase=%s\narchitecture=%s\npaint_count=%u\ntimer_ticks=%u\n"
            "key_down_count=%u\nchar_count=%u\nmouse_move_count=%u\n"
            "left_click_count=%u\nright_click_count=%u\nmouse_x=%d\nmouse_y=%d\n"
            "wheel_delta=%d\nelapsed_ms=%lu\nlast_char=U+%04X\nclient_width=%d\nclient_height=%d\n",
            phase, SMOKE_ARCH, state.paints, state.ticks, state.key_downs, state.chars,
            state.moves, state.left_clicks, state.right_clicks, state.mouse_x, state.mouse_y,
            state.wheel, (unsigned long)(GetTickCount() - state.started),
            state.text_length ? state.text[state.text_length - 1] : 0,
            state.client_width, state.client_height);
    fclose(file);
}

static void report(const char *event)
{
    printf("WINETK_SMOKE event=%s arch=%s paints=%u ticks=%u keys=%u chars=%u "
           "moves=%u left=%u right=%u mouse=%d,%d wheel=%d elapsed_ms=%lu\n",
           event, SMOKE_ARCH, state.paints, state.ticks, state.key_downs, state.chars,
           state.moves, state.left_clicks, state.right_clicks, state.mouse_x, state.mouse_y,
           state.wheel, (unsigned long)(GetTickCount() - state.started));
    fflush(stdout);
    write_status(event);
}

static void fill(HDC dc, int left, int top, int right, int bottom, COLORREF color)
{
    RECT rect = {left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void text(HDC dc, int x, int y, const WCHAR *value, COLORREF color)
{
    SetTextColor(dc, color);
    TextOutW(dc, x, y, value, (int)wcslen(value));
}

static void paint(HWND hwnd)
{
    static const COLORREF colors[] = {
        RGB(233, 96, 103), RGB(95, 195, 151), RGB(94, 161, 224), RGB(239, 175, 74),
        RGB(174, 132, 211), RGB(75, 185, 188)};
    PAINTSTRUCT ps;
    RECT rect;
    WCHAR line[192];
    HDC dc = BeginPaint(hwnd, &ps);
    HGDIOBJ original_font;
    int width, height, x, i, travel;

    GetClientRect(hwnd, &rect);
    width = rect.right;
    height = rect.bottom;
    state.client_width = width;
    state.client_height = height;
    fill(dc, 0, 0, width, height, RGB(16, 29, 37));
    fill(dc, 0, 0, width, 66, RGB(24, 40, 58));
    SetBkMode(dc, TRANSPARENT);
    original_font = SelectObject(dc, state.title_font);
    text(dc, 20, 14, L"winetk / Wine GDI", RGB(236, 243, 251));
    SelectObject(dc, state.body_font);
    text(dc, 22, 43, L"Real Win32 window - no X11 / Wayland window", RGB(159, 190, 212));

    for (i = 0; i < 6; ++i)
        fill(dc, 20 + i * (width - 40) / 6, 82,
             20 + (i + 1) * (width - 40) / 6 - 3, 126, colors[i]);

    text(dc, 22, 144, L"Type text, move the mouse, click, or use arrow keys.", RGB(212, 228, 238));
    text(dc, 22, 168, L"Escape closes this Windows application.", RGB(151, 179, 197));
    swprintf(line, 192, L"Text (%u characters): %ls", state.chars,
             state.text_length ? state.text : L"[waiting for terminal input]");
    text(dc, 22, 202, line, RGB(235, 207, 138));
    swprintf(line, 192, L"Mouse %d,%d   moves %u   left %u   right %u   wheel %d",
             state.mouse_x, state.mouse_y, state.moves, state.left_clicks,
             state.right_clicks, state.wheel);
    text(dc, 22, 228, line, RGB(149, 216, 198));

    travel = width > 160 ? width - 160 : 1;
    x = ((int)(state.ticks % (unsigned int)travel) * 5 + state.offset) % travel;
    if (x < 0) x += travel;
    x += 20;
    fill(dc, x, 272, x + 112, 322, RGB(239, 144, 67));
    text(dc, x + 14, 287, L"GDI TIMER", RGB(33, 31, 27));
    if (state.mouse_x >= 0 && state.mouse_y >= 0)
    {
        fill(dc, state.mouse_x - 7, state.mouse_y - 1,
             state.mouse_x + 8, state.mouse_y + 2, RGB(250, 250, 250));
        fill(dc, state.mouse_x - 1, state.mouse_y - 7,
             state.mouse_x + 2, state.mouse_y + 8, RGB(250, 250, 250));
    }

    fill(dc, 0, height - 46, width, height, RGB(24, 40, 58));
    swprintf(line, 192, L"%ls  /  WM_PAINT %u  /  WM_TIMER %u  /  keydown %u",
#ifdef _WIN64
             L"x86_64",
#else
             L"i386",
#endif
             state.paints + 1, state.ticks, state.key_downs);
    text(dc, 22, height - 32, line, RGB(171, 199, 220));
    SelectObject(dc, original_font);
    EndPaint(hwnd, &ps);
    ++state.paints;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        if (!SetTimer(hwnd, 1, 50, NULL)) return -1;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_TIMER:
        ++state.ticks;
        InvalidateRect(hwnd, NULL, FALSE);
        if (state.duration_ms && GetTickCount() - state.started >= state.duration_ms)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (!(state.ticks % 20)) write_status("running");
        return 0;
    case WM_KEYDOWN:
        ++state.key_downs;
        if (wparam == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (wparam == VK_LEFT) state.offset -= 12;
        if (wparam == VK_RIGHT) state.offset += 12;
        report("key");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CHAR:
        ++state.chars;
        if (wparam == L'\b')
        {
            if (state.text_length) state.text[--state.text_length] = 0;
        }
        else if (wparam >= 32 && wparam != 127)
        {
            if (state.text_length == sizeof(state.text) / sizeof(state.text[0]) - 1)
            {
                MoveMemory(state.text, state.text + 1,
                           (state.text_length - 1) * sizeof(state.text[0]));
                --state.text_length;
            }
            state.text[state.text_length++] = (WCHAR)wparam;
            state.text[state.text_length] = 0;
        }
        printf("WINETK_SMOKE character=U+%04X\n", (unsigned int)wparam);
        report("char");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        state.mouse_x = GET_X_LPARAM(lparam);
        state.mouse_y = GET_Y_LPARAM(lparam);
        ++state.moves;
        if (state.moves == 1) report("mouse_move");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        state.mouse_x = GET_X_LPARAM(lparam);
        state.mouse_y = GET_Y_LPARAM(lparam);
        if (message == WM_LBUTTONDOWN) ++state.left_clicks;
        else ++state.right_clicks;
        SetFocus(hwnd);
        report(message == WM_LBUTTONDOWN ? "left_click" : "right_click");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        state.wheel += GET_WHEEL_DELTA_WPARAM(wparam);
        report("wheel");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        report("complete");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int wmain(int argc, WCHAR **argv)
{
    static const WCHAR class_name[] = L"WinetkGdiSmokeWindow";
    const DWORD style = WS_OVERLAPPEDWINDOW;
    WNDCLASSW window_class = {0};
    RECT window_rect = {0, 0, 640, 400};
    HWND hwnd;
    MSG message;
    BOOL result;
    int i;

    for (i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--duration") && i + 1 < argc)
        {
            WCHAR *end;
            unsigned long seconds = wcstoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || seconds > 86400)
            {
                fputs("--duration requires whole seconds from 0 to 86400\n", stderr);
                return 2;
            }
            state.duration_ms = (DWORD)seconds * 1000;
        }
        else if ((!wcscmp(argv[i], L"--status") || !wcscmp(argv[i], L"--log")) && i + 1 < argc)
            state.status_path = argv[++i];
        else if ((!wcscmp(argv[i], L"--width") || !wcscmp(argv[i], L"--height")) && i + 1 < argc)
        {
            int is_width = !wcscmp(argv[i], L"--width");
            WCHAR *end;
            unsigned long size = wcstoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || size < 320 || size > (is_width ? 1920u : 1080u))
            {
                fputs("--width requires 320..1920; --height requires 320..1080\n", stderr);
                return 2;
            }
            if (is_width) window_rect.right = (LONG)size;
            else window_rect.bottom = (LONG)size;
        }
        else if (!wcscmp(argv[i], L"--help"))
        {
            puts("gdi-window.exe [--duration SECONDS] [--status WINDOWS_PATH] [--width N] [--height N]\n"
                 "Duration 0 (default) runs until Escape or window close.\n"
                 "--status writes the latest counters as plain key=value text.\n"
                 "Event and final counters are always printed on standard output.");
            return 0;
        }
        else
        {
            fputs("Unknown or incomplete argument; use --help\n", stderr);
            return 2;
        }
    }

    state.mouse_x = state.mouse_y = -1;
    state.started = GetTickCount();
    state.title_font = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"DejaVu Sans");
    state.body_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"DejaVu Sans");
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class))
    {
        fprintf(stderr, "RegisterClassW failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    AdjustWindowRect(&window_rect, style, FALSE);
    hwnd = CreateWindowExW(0, class_name, L"winetk / Wine GDI", style,
                           64, 72, window_rect.right - window_rect.left,
                           window_rect.bottom - window_rect.top, NULL, NULL,
                           window_class.hInstance, NULL);
    if (!hwnd)
    {
        fprintf(stderr, "CreateWindowExW failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    report("start");
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DeleteObject(state.title_font);
    DeleteObject(state.body_font);
    return result == -1 ? 1 : (int)message.wParam;
}
