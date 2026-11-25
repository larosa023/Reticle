// Reticle - frame capture test (runs in CI on a Windows runner).
//
//   frametest.exe            starts Reticle's ETW capture, launches renderers (D3D11, D3D9) as child processes
//                            and checks that frames::Snapshot reports their frame rate
//   frametest.exe --d3d11    a window presenting with DXGI (flip model) as fast as ~150 fps
//   frametest.exe --d3d9     a window presenting with D3D9
#include "../src/frames.h"
#include "../src/util.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3d9.h>
#include <cstdio>
#include <string>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND MakeWindow(const wchar_t* title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"frametest";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"frametest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    return h;
}

static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

static int RenderD3D11(int seconds) {
    HWND hwnd = MakeWindow(L"frametest d3d11");
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("d3d11: create failed 0x%08lx\n", hr); return 3; }
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    int n = 0;
    while (GetTickCount64() < end) {
        Pump();
        float c[4] = {0.1f, (n % 100) / 100.0f, 0.3f, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, c);
        sc->Present(0, 0);
        ++n;
        Sleep(5);
    }
    printf("d3d11: presented %d frames\n", n);
    return 0;
}

static int RenderD3D9(int seconds) {
    HWND hwnd = MakeWindow(L"frametest d3d9");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("d3d9: no Direct3D 9\n"); return 3; }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("d3d9: CreateDevice failed 0x%08lx\n", hr); return 3; }
    ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    int n = 0;
    while (GetTickCount64() < end) {
        Pump();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(n % 255, 40, 90), 1, 0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        ++n;
        Sleep(5);
    }
    printf("d3d9: presented %d frames\n", n);
    return 0;
}

static bool Measure(const wchar_t* arg, const char* name, bool required) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + arg;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { printf("%s: couldn't start\n", name); return !required; }
    frames::SetTarget(pi.dwProcessId);
    Sleep(4000);
    frames::Live live;
    frames::Snapshot(live, 240);
    double rate = frames::PresentRate(pi.dwProcessId);
    printf("%s: valid=%d fps=%.1f frameMs=%.2f low1=%.1f low01=%.1f api=%ls rate=%.1f graph=%zu\n", name, live.valid, live.fps, live.frameMs,
           live.low1, live.low01, live.api, rate, live.recent.size());
    // benchmark path
    frames::BenchStart();
    Sleep(1500);
    std::vector<float> b = frames::BenchStop();
    frames::Summary s = frames::Summarize(b);
    printf("%s: bench frames=%zu avg=%.1f low1=%.1f\n", name, s.frames, s.avgFps, s.low1);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    bool ok = live.valid && live.fps > 20 && rate > 20 && s.frames > 20;
    if (code == 3) { printf("%s: renderer unavailable on this machine\n", name); return !required; }
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    return ok || !required;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--d3d11")) return RenderD3D11(8);
    if (argc > 1 && !wcscmp(argv[1], L"--d3d9")) return RenderD3D9(8);
    LogInit();
    if (!frames::Start()) {
        printf("frame capture didn't start: status=%d error=%lu\n", (int)frames::GetStatus(), frames::LastError());
        return 2;
    }
    printf("frame capture running\n");
    bool ok = Measure(L"--d3d11", "d3d11", true);
    ok = Measure(L"--d3d9", "d3d9", false) && ok;
    frames::Stop();
    printf(ok ? "ALL PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
