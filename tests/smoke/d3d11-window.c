#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

static int running = 1;
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { running = 0; PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(void)
{
    static const char shader[] =
        "struct V{float4 p:SV_Position;float4 c:COLOR;};"
        "V vs(uint i:SV_VertexID){V o;float2 p[3]={float2(0,.7),float2(.7,-.7),float2(-.7,-.7)};"
        "o.p=float4(p[i],0,1);o.c=float4(.8,.6,.4,1);return o;}"
        "float4 ps(V i):SV_Target{return i.c;}";
    WNDCLASSW wc = {0};
    RECT rect = {0,0,640,480};
    HWND hwnd = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11RenderTargetView *target = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3DBlob *code = NULL, *errors = NULL;
    D3D_FEATURE_LEVEL level = 0;
    DXGI_SWAP_CHAIN_DESC desc = {0};
    HRESULT hr = E_FAIL;
    unsigned frames = 0;
    DWORD start = 0;
    int result = 1;
    wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"WineTKD3D11";
    if (!RegisterClassW(&wc)) goto done;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(wc.lpszClassName, L"WineTK D3D11", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         0,0,rect.right-rect.left,rect.bottom-rect.top,NULL,NULL,wc.hInstance,NULL);
    if (!hwnd) goto done;
    desc.BufferDesc.Width = 640; desc.BufferDesc.Height = 480;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1; desc.OutputWindow = hwnd; desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,
                                     D3D11_SDK_VERSION,&desc,&swapchain,&device,&level,&context);
    printf("D3D11CreateDeviceAndSwapChain=0x%08lx feature_level=%#x\n", (unsigned long)hr,level);
    fflush(stdout);
    if (FAILED(hr)) goto done;
    hr = D3DCompile(shader,sizeof(shader)-1,NULL,NULL,NULL,"vs","vs_4_0",0,0,&code,&errors);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&vs);
    ID3D10Blob_Release(code); code = NULL;
    if (FAILED(hr)) goto done;
    hr = D3DCompile(shader,sizeof(shader)-1,NULL,NULL,NULL,"ps","ps_4_0",0,0,&code,&errors);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&ps);
    ID3D10Blob_Release(code); code = NULL;
    if (FAILED(hr)) goto done;
    ID3D11DeviceContext_VSSetShader(context,vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(context,ps,NULL,0);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    start = GetTickCount();
    while (running && GetTickCount()-start < 8000)
    {
        MSG msg;
        unsigned w = frames < 240 ? 640 : 800, h = frames < 240 ? 480 : 600;
        const float background[] = {12.f/255,24.f/255,48.f/255,1};
        D3D11_VIEWPORT viewport = {0,0,(float)w,(float)h,0,1};
        while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (frames == 240)
        {
            ID3D11DeviceContext_OMSetRenderTargets(context,0,NULL,NULL);
            ID3D11RenderTargetView_Release(target); target = NULL;
            rect.left=rect.top=0; rect.right=800; rect.bottom=600;
            AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
            SetWindowPos(hwnd,NULL,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER);
            hr = IDXGISwapChain_ResizeBuffers(swapchain,1,w,h,DXGI_FORMAT_UNKNOWN,0);
            if (FAILED(hr)) goto done;
        }
        if (!target)
        {
            hr = IDXGISwapChain_GetBuffer(swapchain,0,&IID_ID3D11Texture2D,(void **)&texture);
            if (FAILED(hr)) goto done;
            hr = ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)texture,NULL,&target);
            ID3D11Texture2D_Release(texture); texture = NULL;
            if (FAILED(hr)) goto done;
        }
        ID3D11DeviceContext_OMSetRenderTargets(context,1,&target,NULL);
        ID3D11DeviceContext_RSSetViewports(context,1,&viewport);
        ID3D11DeviceContext_ClearRenderTargetView(context,target,background);
        ID3D11DeviceContext_Draw(context,3,0);
        hr = IDXGISwapChain_Present(swapchain,0,0);
        if (FAILED(hr)) goto done;
        ++frames;
        if (frames*1000/60 > GetTickCount()-start) Sleep(frames*1000/60-(GetTickCount()-start));
    }
    result = frames >= 300 ? 0 : 1;
done:
    printf("D3D11_SMOKE frames=%u elapsed_ms=%lu hr=%#lx exit=%d\n",frames,
           (unsigned long)(start ? GetTickCount()-start : 0),(unsigned long)hr,result);
    if (errors) { fprintf(stderr,"%s\n",(char *)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
    if (code) ID3D10Blob_Release(code);
    if (context) ID3D11DeviceContext_ClearState(context);
    if (target) ID3D11RenderTargetView_Release(target);
    if (texture) ID3D11Texture2D_Release(texture);
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    if (swapchain) IDXGISwapChain_Release(swapchain);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    return result;
}
