#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned keys;
static unsigned clicks;
static int running = 1;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_KEYDOWN:
        ++keys;
        if (wparam == VK_ESCAPE)
            DestroyWindow(window);
        return 0;
    case WM_LBUTTONDOWN:
        ++clicks;
        return 0;
    case WM_DESTROY:
        running = 0;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct vertex {
    float x, y, z, rhw;
    DWORD color;
};

int wmain(int argc, wchar_t **argv)
{
    unsigned duration = 8;
    const wchar_t *status_path = NULL;
    HRESULT result;
    HMODULE library = NULL;
    IDirect3D8 *(WINAPI *create_direct3d)(UINT) = NULL;
    IDirect3D8 *direct3d = NULL;
    IDirect3DDevice8 *device = NULL;
    HWND window = NULL;
    WNDCLASSW window_class = {0};
    D3DPRESENT_PARAMETERS parameters = {0};
    RECT rectangle = {0, 0, 640, 480};
    DWORD started = 0;
    unsigned frames = 0;
    int exit_code = 1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--duration") && i + 1 < argc)
            duration = (unsigned)wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--status") && i + 1 < argc)
            status_path = argv[++i];
        else {
            fprintf(stderr, "Usage: d3d8-window.exe [--duration SECONDS] [--status PATH]\n");
            return 2;
        }
    }
    if (!duration || duration > 3600)
        return 2;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"WinetkD3D8Smoke";
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&window_class))
        goto done;
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowW(window_class.lpszClassName, L"WineTK Direct3D 8 smoke",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                           rectangle.right - rectangle.left,
                           rectangle.bottom - rectangle.top,
                           NULL, NULL, window_class.hInstance, NULL);
    if (!window)
        goto done;
    SetForegroundWindow(window);
    library = LoadLibraryW(L"d3d8.dll");
    if (library) {
        FARPROC entry = GetProcAddress(library, "Direct3DCreate8");
        _Static_assert(sizeof(entry) == sizeof(create_direct3d), "function pointer size");
        memcpy(&create_direct3d, &entry, sizeof(entry));
    }
    if (!create_direct3d || !(direct3d = create_direct3d(D3D_SDK_VERSION))) {
        fprintf(stderr, "Direct3DCreate8 returned NULL\n");
        goto done;
    }
    parameters.BackBufferWidth = 640;
    parameters.BackBufferHeight = 480;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    result = IDirect3D8_CreateDevice(direct3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                    window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                    &parameters, &device);
    printf("CreateDevice=0x%08lx\n", (unsigned long)result);
    fflush(stdout);
    if (FAILED(result))
        goto done;
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    started = GetTickCount();
    while (running && GetTickCount() - started < duration * 1000) {
        MSG message;
        unsigned phase = frames % 120;
        float offset = phase < 60 ? (float)phase : (float)(120 - phase);
        struct vertex triangle[] = {
            {320.0f + offset, 80.0f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 64, 64)},
            {140.0f + offset, 380.0f, 0.5f, 1.0f, D3DCOLOR_XRGB(64, 255, 64)},
            {500.0f + offset, 380.0f, 0.5f, 1.0f, D3DCOLOR_XRGB(64, 64, 255)},
        };
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
            break;
        result = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                       D3DCOLOR_XRGB(12, 24, 48), 1.0f, 0);
        if (SUCCEEDED(result))
            result = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(result)) {
            result = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                     triangle, sizeof(triangle[0]));
            IDirect3DDevice8_EndScene(device);
        }
        if (SUCCEEDED(result))
            result = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        if (FAILED(result)) {
            fprintf(stderr, "Render/present failed=0x%08lx at frame %u\n",
                    (unsigned long)result, frames);
            goto done;
        }
        ++frames;
        {
            DWORD elapsed = GetTickCount() - started;
            DWORD deadline = frames * 1000 / 60;
            if (deadline > elapsed)
                Sleep(deadline - elapsed);
        }
    }
    exit_code = frames ? 0 : 1;
done:
    {
        DWORD elapsed_ms = started ? GetTickCount() - started : 0;
        printf("D3D8_SMOKE frames=%u elapsed_ms=%lu fps=%.2f keys=%u clicks=%u exit=%d\n",
               frames, (unsigned long)elapsed_ms,
               elapsed_ms ? frames * 1000.0 / elapsed_ms : 0.0, keys, clicks, exit_code);
        if (status_path) {
            FILE *status = _wfopen(status_path, L"w");
            if (!status)
                exit_code = 1;
            else {
                fprintf(status, "{\"frames\":%u,\"elapsed_ms\":%lu,\"keys\":%u,\"clicks\":%u,\"exit\":%d}\n",
                        frames, (unsigned long)elapsed_ms, keys, clicks, exit_code);
                fclose(status);
            }
        }
    }
    if (device)
        IDirect3DDevice8_Release(device);
    if (direct3d)
        IDirect3D8_Release(direct3d);
    if (library)
        FreeLibrary(library);
    if (window && IsWindow(window))
        DestroyWindow(window);
    return exit_code;
}
