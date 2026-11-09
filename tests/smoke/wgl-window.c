/* Native WGL smoke: two colors, timer-driven swaps, no Direct3D dependencies. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

static HDC dc;
static HGLRC context;
static DWORD started;
static unsigned int frames;
static BOOL resized;
static GLuint pack_buffer, read_framebuffer;
static void (APIENTRY *gen_buffers)(GLsizei, GLuint *);
static void (APIENTRY *bind_buffer)(GLenum, GLuint);
static void (APIENTRY *gen_framebuffers)(GLsizei, GLuint *);
static void (APIENTRY *bind_framebuffer)(GLenum, GLuint);

#define WT_PIXEL_PACK_BUFFER 0x88eb
#define WT_PIXEL_PACK_BUFFER_BINDING 0x88ed
#define WT_READ_FRAMEBUFFER 0x8ca8
#define WT_READ_FRAMEBUFFER_BINDING 0x8caa

static void check_gl(const char *phase)
{
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
    {
        fprintf(stderr, "GL error %#x at %s, frame %u\n", error, phase, frames);
        fflush(stderr);
        ExitProcess(16);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_TIMER)
    {
        RECT rect;
        GLint value;
        if (!resized && GetTickCount() - started >= 4000)
        {
            RECT outer = {0, 0, 800, 600};
            AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            resized = TRUE;
            fprintf(stdout, "resize_client=800x600\n");
            fflush(stdout);
        }
        GetClientRect(hwnd, &rect);
        glViewport(0, 0, rect.right, rect.bottom);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.1f, 0.2f, 0.8f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, rect.bottom / 2, rect.right / 2, rect.bottom / 2);
        glClearColor(0.9f, 0.1f, 0.2f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        check_gl("draw");
        /* A presenter must not inherit application's pixel pack/FBO state. */
        bind_buffer(WT_PIXEL_PACK_BUFFER, pack_buffer);
        bind_framebuffer(WT_READ_FRAMEBUFFER, read_framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 8);
        glPixelStorei(GL_PACK_ROW_LENGTH, 777);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
        glPixelStorei(GL_PACK_SKIP_ROWS, 5);
        check_gl("pack and FBO setup");
        if (!SwapBuffers(dc)) fprintf(stderr, "SwapBuffers failed %lu\n", GetLastError());
        check_gl("SwapBuffers");
        glGetIntegerv(WT_PIXEL_PACK_BUFFER_BINDING, &value);
        if ((GLuint)value != pack_buffer) ExitProcess(10);
        glGetIntegerv(WT_READ_FRAMEBUFFER_BINDING, &value);
        if ((GLuint)value != read_framebuffer) ExitProcess(11);
        glGetIntegerv(GL_PACK_ALIGNMENT, &value);
        if (value != 8) ExitProcess(12);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &value);
        if (value != 777) ExitProcess(13);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &value);
        if (value != 3) ExitProcess(14);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &value);
        if (value != 5) ExitProcess(15);
        check_gl("state query");
        ++frames;
        if (GetTickCount() - started >= 8000) DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, WCHAR *command, int show)
{
    WNDCLASSW cls = {0};
    PIXELFORMATDESCRIPTOR format = {0};
    RECT rect = {0, 0, 640, 480};
    MSG message;
    HWND hwnd;
    int index;
    GLint native_buffer, double_buffer;
    union { PROC generic; void (APIENTRY *gen)(GLsizei, GLuint *); void (APIENTRY *bind)(GLenum, GLuint); } proc;
    (void)previous;
    (void)command;
    (void)show;
    cls.style = CS_OWNDC;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.lpszClassName = L"WineTK-WGL-Smoke";
    if (!RegisterClassW(&cls)) return 1;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(cls.lpszClassName, L"WineTK surfaceless OpenGL", WS_OVERLAPPEDWINDOW,
                         40, 30, rect.right - rect.left, rect.bottom - rect.top,
                         NULL, NULL, instance, NULL);
    if (!hwnd) return 2;
    dc = GetDC(hwnd);
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    if (!(index = ChoosePixelFormat(dc, &format)) || !SetPixelFormat(dc, index, &format)) return 3;
    if (!(context = wglCreateContext(dc)) || !wglMakeCurrent(dc, context)) return 4;
    check_gl("make current");
    glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
    glGetIntegerv(GL_READ_BUFFER, &native_buffer);
    fprintf(stderr, "GL_DEFAULT_BUFFER=%#x GL_DOUBLEBUFFER=%d\n", native_buffer, double_buffer);
    proc.generic = wglGetProcAddress("glGenBuffers");
    gen_buffers = proc.gen;
    proc.generic = wglGetProcAddress("glBindBuffer");
    bind_buffer = proc.bind;
    proc.generic = wglGetProcAddress("glGenFramebuffers");
    gen_framebuffers = proc.gen;
    proc.generic = wglGetProcAddress("glBindFramebuffer");
    bind_framebuffer = proc.bind;
    if (!gen_buffers || !bind_buffer || !gen_framebuffers || !bind_framebuffer) return 5;
    gen_buffers(1, &pack_buffer);
    check_gl("buffer initialization");
    gen_framebuffers(1, &read_framebuffer);
    check_gl("object initialization");
    fprintf(stdout, "GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s\n", glGetString(GL_VENDOR),
            glGetString(GL_RENDERER), glGetString(GL_VERSION));
    fflush(stdout);
    ShowWindow(hwnd, SW_SHOW);
    started = GetTickCount();
    SetTimer(hwnd, 1, 8, NULL);
    while (GetMessageW(&message, NULL, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    fprintf(stdout, "frames=%u elapsed_ms=%lu\n", frames, GetTickCount() - started);
    return 0;
}
