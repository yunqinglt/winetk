/* Poll real DirectInput state to distinguish enhanced arrows from the numpad. */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef _WIN64
#define ARCHITECTURE "x86_64"
#else
#define ARCHITECTURE "i386"
#endif

static const struct { const char *name; unsigned scan; } keys[] = {
    {"up", DIK_UP}, {"down", DIK_DOWN}, {"left", DIK_LEFT}, {"right", DIK_RIGHT},
    {"numpad8", DIK_NUMPAD8}, {"numpad2", DIK_NUMPAD2},
    {"numpad4", DIK_NUMPAD4}, {"numpad6", DIK_NUMPAD6},
};
static IDirectInputDevice8A *keyboard;
static unsigned transitions[8], samples[8], polls, successful;
static BYTE previous[256];
static DWORD started, duration_ms = 10000;
static HRESULT last_result;
static const WCHAR *status_path;

static void write_status(const char *phase)
{
    FILE *status;
    if (!status_path || !(status = _wfopen(status_path, L"w")))
        return;
    fprintf(status, "phase=%s\narchitecture=%s\npolls=%u\nsuccessful_polls=%u\n"
            "last_hresult=0x%08lx\nelapsed_ms=%lu\n", phase, ARCHITECTURE, polls, successful,
            (unsigned long)last_result, (unsigned long)(GetTickCount() - started));
    for (unsigned i = 0; i < 8; ++i)
        fprintf(status, "%s_count=%u\n%s_samples=%u\n%s_held=%u\n", keys[i].name, transitions[i],
                keys[i].name, samples[i], keys[i].name, !!(previous[keys[i].scan] & 0x80));
    fclose(status);
}

static void poll_keyboard(void)
{
    BYTE current[256] = {0};
    if (!keyboard)
        return;
    ++polls;
    last_result = IDirectInputDevice8_GetDeviceState(keyboard, sizeof(current), current);
    if (last_result == DIERR_INPUTLOST || last_result == DIERR_NOTACQUIRED) {
        IDirectInputDevice8_Acquire(keyboard);
        last_result = IDirectInputDevice8_GetDeviceState(keyboard, sizeof(current), current);
    }
    if (SUCCEEDED(last_result)) {
        ++successful;
        for (unsigned i = 0; i < 8; ++i) {
            if (current[keys[i].scan] & 0x80) {
                ++samples[i];
                if (!(previous[keys[i].scan] & 0x80)) {
                    ++transitions[i];
                    printf("DINPUT8 key=%s scan=0x%02x count=%u\n", keys[i].name, keys[i].scan, transitions[i]);
                    fflush(stdout);
                }
            }
        }
        CopyMemory(previous, current, sizeof(previous));
    }
    write_status("running");
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        SetTimer(window, 1, 10, NULL);
        return 0;
    case WM_TIMER:
        poll_keyboard();
        if (GetTickCount() - started >= duration_ms)
            DestroyWindow(window);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        static const WCHAR text[] = L"DirectInput 8: hold each arrow; inspect status counters.";
        TextOutW(dc, 12, 12, text, (int)(sizeof(text) / sizeof(text[0]) - 1));
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        write_status("complete");
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

int wmain(int argc, WCHAR **argv)
{
    IDirectInput8A *input = NULL;
    WNDCLASSW window_class = {0};
    HWND window = NULL;
    MSG message;
    int exit_code = 1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--status") && i + 1 < argc)
            status_path = argv[++i];
        else if (!wcscmp(argv[i], L"--duration") && i + 1 < argc) {
            WCHAR *end;
            unsigned long seconds = wcstoul(argv[++i], &end, 10);
            if (!seconds || seconds > 3600 || *end)
                return 2;
            duration_ms = seconds * 1000;
        } else
            return 2;
    }
    started = GetTickCount();
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"WinetkDInput8Smoke";
    window_class.lpfnWndProc = window_proc;
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&window_class))
        goto done;
    window = CreateWindowW(window_class.lpszClassName, L"WineTK DirectInput 8",
                           WS_VISIBLE | WS_OVERLAPPEDWINDOW, 0, 0, 640, 400,
                           NULL, NULL, window_class.hInstance, NULL);
    if (!window)
        goto done;
    SetForegroundWindow(window);
    SetFocus(window);
    last_result = DirectInput8Create(window_class.hInstance, DIRECTINPUT_VERSION,
                                     &IID_IDirectInput8A, (void **)&input, NULL);
    if (FAILED(last_result))
        goto done;
    last_result = IDirectInput8_CreateDevice(input, &GUID_SysKeyboard, &keyboard, NULL);
    if (FAILED(last_result))
        goto done;
    last_result = IDirectInputDevice8_SetDataFormat(keyboard, &c_dfDIKeyboard);
    if (FAILED(last_result))
        goto done;
    last_result = IDirectInputDevice8_SetCooperativeLevel(keyboard, window, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(last_result))
        goto done;
    IDirectInputDevice8_Acquire(keyboard);
    write_status("ready");
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    exit_code = successful ? 0 : 1;
done:
    printf("DINPUT8_SUMMARY polls=%u successful=%u result=0x%08lx exit=%d\n",
           polls, successful, (unsigned long)last_result, exit_code);
    if (exit_code)
        write_status("failed");
    if (keyboard) {
        IDirectInputDevice8_Unacquire(keyboard);
        IDirectInputDevice8_Release(keyboard);
    }
    if (input)
        IDirectInput8_Release(input);
    if (window && IsWindow(window))
        DestroyWindow(window);
    return exit_code;
}
