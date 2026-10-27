#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <objbase.h>
#include <tchar.h>
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

#include "identity.h"
#include "spoofer.h"
#include "cleaner.h"
#include "neuron.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DXGI_USAGE_RENDER_TARGET_VIEW
#ifdef DXGI_USAGE_RENDER_TARGET_OUTPUT
#define DXGI_USAGE_RENDER_TARGET_VIEW DXGI_USAGE_RENDER_TARGET_OUTPUT
#else
#define DXGI_USAGE_RENDER_TARGET_VIEW (0x20U)
#endif
#endif

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_VIEW;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static ID3D11ShaderResourceView* g_logo_srv = nullptr;

static bool load_logo_texture(int res_id) {
    HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(res_id), MAKEINTRESOURCEW(10));
    if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    DWORD size = SizeofResource(nullptr, hr);
    void* data = LockResource(hg);
    if (!data || !size) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;

    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;

    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w && h) {
            std::vector<BYTE> buf((size_t)w * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)buf.size(), buf.data()))) {
                D3D11_TEXTURE2D_DESC d = {};
                d.Width = w;
                d.Height = h;
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_IMMUTABLE;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA sd = {};
                sd.pSysMem = buf.data();
                sd.SysMemPitch = w * 4;
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&d, &sd, &tex))) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
                    sv.Format = d.Format;
                    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = 1;
                    ok = SUCCEEDED(g_pd3dDevice->CreateShaderResourceView(tex, &sv, &g_logo_srv));
                    tex->Release();
                }
            }
        }
    }

    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ImGuiContext* g_ctx_main = nullptr;
static ImGuiContext* g_ctx_adv  = nullptr;

static HWND                    g_hwnd_adv = nullptr;
static HWND                    g_hwnd_main = nullptr;
static IDXGISwapChain*         g_advSwapChain = nullptr;
static ID3D11RenderTargetView* g_advRTV = nullptr;

static void adv_resize(UINT w, UINT h);
static void create_advanced_window();
static void destroy_advanced_window();

static HICON load_app_icon(int size) {
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                             size, size, size > 0 ? 0 : LR_DEFAULTSIZE);
}

static void theme_titlebar(HWND h) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
    COLORREF caption = RGB(6, 12, 6);
    COLORREF border  = RGB(46, 74, 22);
    COLORREF text    = RGB(173, 255, 47);
    DwmSetWindowAttribute(h, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(h, 34, &border, sizeof(border));
    DwmSetWindowAttribute(h, 36, &text, sizeof(text));
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (hWnd == g_hwnd_adv) {
        if (g_ctx_adv) {
            ImGui::SetCurrentContext(g_ctx_adv);
            bool handled = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            LRESULT r = 0;
            if (!handled) {
                switch (msg) {
                case WM_SIZE:
                    if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
                        adv_resize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
                    break;
                case WM_SYSCOMMAND:
                    if ((wParam & 0xfff0) != SC_KEYMENU)
                        r = ::DefWindowProcW(hWnd, msg, wParam, lParam);
                    break;
                case WM_CLOSE:
                    destroy_advanced_window();
                    break;
                case WM_DESTROY:
                    break;
                default:
                    r = ::DefWindowProcW(hWnd, msg, wParam, lParam);
                    break;
                }
            }
            if (g_ctx_main) ImGui::SetCurrentContext(g_ctx_main);
            return r;
        }
        if (msg == WM_CLOSE) { destroy_advanced_window(); return 0; }
        if (msg == WM_DESTROY) return 0;
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    if (hWnd == g_hwnd_main) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;
        switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DPICHANGED: {
            const RECT* rect = (const RECT*)lParam;
            SetWindowPos(hWnd, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

namespace app {

enum class Page { Dashboard, Spoofer, Cleaner };

struct IdentityRow {
    std::string label;
    std::string value;
};

struct ConsoleLine {
    int level;
    std::string text;
    std::string stamp;
};

struct State {
    Page page = Page::Dashboard;

    spoofer::Options opts;
    identity::Profile target;
    bool has_target = false;

    std::vector<IdentityRow> display_rows;
    bool display_dirty = true;
    std::string status_text;
    int status_level = -1;

    std::vector<ConsoleLine> console;
    bool scroll_console = false;

    std::atomic<bool> busy{ false };
    std::mutex mtx;
    std::vector<ConsoleLine> pending;

    std::vector<cleaner::CleanTarget> scan_temp;
    std::vector<cleaner::CleanTarget> scan_own;
    std::vector<cleaner::CleanTarget> scan_game;
    bool scanned = false;

    char profiles_dir[512] = "profiles";
    bool ask_wipe = false;
};

State g;

std::string now_string() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[24];
    snprintf(b, sizeof(b), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return b;
}

void log_line(int level, const std::string& text) {
    ConsoleLine l;
    l.level = level;
    l.text = text;
    l.stamp = now_string();
    std::lock_guard<std::mutex> lock(app::g.mtx);
    app::g.pending.push_back(l);
}

void drain_pending() {
    std::lock_guard<std::mutex> lock(app::g.mtx);
    for (auto& l : app::g.pending) {
        app::g.console.push_back(l);
        app::g.status_text = l.text;
        app::g.status_level = l.level;
        if (app::g.console.size() > 500) app::g.console.erase(app::g.console.begin());
    }
    if (!app::g.pending.empty()) app::g.scroll_console = true;
    app::g.pending.clear();
}

std::string profiles_path() {
    std::string dir = app::g.profiles_dir;
    if (dir.empty()) dir = "profiles";
    if (dir.back() == '\\' || dir.back() == '/') dir.pop_back();
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::vector<IdentityRow> identity_rows() {
    std::vector<IdentityRow> rows;
    auto push = [&](const char* label, const std::string& v) {
        rows.push_back({ label, v.empty() ? "—" : v });
    };
    if (app::g.has_target) {
        auto& p = app::g.target;
        push("Nombre", p.name);
        push("MachineGuid", p.machine_guid);
        push("HwProfileGuid", p.hw_profile_guid);
        push("ProductId", p.product_id);
        push("InstallDate", p.install_date);
        push("MAC", p.network_address);
        push("Volume serial", p.volume_serial);
    } else {
        auto adapters = identity::read_adapters();
        std::string mac;
        for (auto& a : adapters) if (a.is_primary) mac = a.mac;
        push("Nombre", "SISTEMA ACTUAL");
        push("MachineGuid", identity::read_machine_guid());
        push("HwProfileGuid", identity::read_hw_profile_guid());
        push("ProductId", identity::read_product_id());
        push("InstallDate", identity::read_install_date());
        push("MAC", mac);
        push("Volume serial", identity::read_volume_serial('C'));
    }
    return rows;
}

void start_apply() {
    if (app::g.busy) return;
    app::g.busy = true;
    spoofer::Options opt = app::g.opts;
    identity::Profile target = app::g.target;
    std::string pdir = app::profiles_path();
    std::thread([opt, target, pdir]() {
        spoofer::apply(opt, target, pdir, [](int lvl, const std::string& msg) { app::log_line(lvl, msg); });
        if (opt.launch_after_spoof) {
            if (spoofer::game_running()) {
                app::log_line(2, "BloodStrike ya está abierto: no se relanza.");
            } else {
                std::string exe = spoofer::find_game_exe();
                if (exe.empty())
                    app::log_line(2, "No se encontró BloodStrike.exe para lanzarlo.");
                else
                    spoofer::launch_game(exe, [](int lvl, const std::string& msg) { app::log_line(lvl, msg); });
            }
        }
        app::g.busy = false;
    }).detach();
}

void restore_latest() {
    if (app::g.busy) return;
    std::string dir = app::profiles_path();
    std::string best;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\backup_*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                if (best.empty() || strcmp(fd.cFileName, best.c_str()) > 0) best = fd.cFileName;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (best.empty()) {
        app::log_line(2, "No hay backup.");
        return;
    }
    app::g.busy = true;
    std::string path = dir + "\\" + best;
    std::thread([path]() {
        identity::Profile p;
        if (identity::load_profile(path, p))
            spoofer::restore(p, [](int lvl, const std::string& msg) { app::log_line(lvl, msg); });
        else
            app::log_line(3, "Backup corrupto.");
        app::g.busy = false;
    }).detach();
}

}

static void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(14, 14);
    s.WindowRounding = 10.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.ChildRounding = 10.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.ItemSpacing = ImVec2(8, 7);

    ImVec4* c = s.Colors;
    const ImVec4 superficie = ImVec4(0.055f, 0.075f, 0.043f, 0.94f);
    const ImVec4 borde      = ImVec4(0.180f, 0.290f, 0.086f, 1.0f);
    const ImVec4 lima       = ImVec4(0.678f, 1.000f, 0.184f, 1.0f);
    const ImVec4 lima_osc   = ImVec4(0.290f, 0.420f, 0.086f, 1.0f);
    const ImVec4 texto      = ImVec4(0.910f, 0.960f, 0.878f, 1.0f);
    const ImVec4 dim        = ImVec4(0.478f, 0.564f, 0.400f, 1.0f);
    const ImVec4 frame      = ImVec4(0.070f, 0.098f, 0.055f, 1.0f);

    c[ImGuiCol_WindowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ChildBg]             = superficie;
    c[ImGuiCol_PopupBg]             = ImVec4(0.035f, 0.047f, 0.027f, 0.98f);
    c[ImGuiCol_Border]              = borde;
    c[ImGuiCol_FrameBg]             = frame;
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.100f, 0.145f, 0.070f, 1.0f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.130f, 0.190f, 0.086f, 1.0f);
    c[ImGuiCol_TitleBg]             = superficie;
    c[ImGuiCol_TitleBgActive]       = superficie;
    c[ImGuiCol_MenuBarBg]           = superficie;
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.024f, 0.031f, 0.016f, 1.0f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.220f, 0.330f, 0.110f, 1.0f);
    c[ImGuiCol_CheckMark]           = lima;
    c[ImGuiCol_SliderGrab]          = lima;
    c[ImGuiCol_SliderGrabActive]    = lima;
    c[ImGuiCol_Button]              = ImVec4(0.075f, 0.105f, 0.059f, 1.0f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.110f, 0.160f, 0.075f, 1.0f);
    c[ImGuiCol_ButtonActive]        = lima_osc;
    c[ImGuiCol_Header]              = ImVec4(0.075f, 0.105f, 0.059f, 1.0f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.110f, 0.160f, 0.075f, 1.0f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.140f, 0.210f, 0.090f, 1.0f);
    c[ImGuiCol_Separator]           = ImVec4(0.130f, 0.200f, 0.075f, 0.7f);
    c[ImGuiCol_Tab]                 = frame;
    c[ImGuiCol_TabHovered]          = ImVec4(0.110f, 0.160f, 0.075f, 1.0f);
    c[ImGuiCol_TabSelected]         = ImVec4(0.145f, 0.220f, 0.086f, 1.0f);
    c[ImGuiCol_TabDimmed]           = frame;
    c[ImGuiCol_TabDimmedSelected]   = ImVec4(0.110f, 0.160f, 0.075f, 1.0f);
    c[ImGuiCol_Text]                = texto;
    c[ImGuiCol_TextDisabled]        = dim;
    c[ImGuiCol_TextSelectedBg]      = ImVec4(0.290f, 0.420f, 0.086f, 0.60f);
    c[ImGuiCol_NavHighlight]        = lima;
    c[ImGuiCol_TableRowBg]          = ImVec4(0.043f, 0.063f, 0.031f, 0.55f);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(0.024f, 0.035f, 0.020f, 0.55f);
    c[ImGuiCol_TableHeaderBg]       = ImVec4(0.075f, 0.105f, 0.059f, 1.0f);
    c[ImGuiCol_TableBorderStrong]   = ImVec4(0.180f, 0.290f, 0.086f, 0.90f);
    c[ImGuiCol_TableBorderLight]    = ImVec4(0.100f, 0.160f, 0.063f, 0.50f);
}

static ImVec4 level_color_vec(int lvl) {
    switch (lvl) {
        case 1: return ImVec4(0.678f, 1.000f, 0.184f, 1.0f);
        case 2: return ImVec4(0.945f, 0.800f, 0.180f, 1.0f);
        case 3: return ImVec4(1.000f, 0.380f, 0.380f, 1.0f);
        default: return ImVec4(0.780f, 0.860f, 0.740f, 1.0f);
    }
}

static ImFont* g_mono = nullptr;

static bool lime_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.678f, 1.000f, 0.184f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.760f, 1.000f, 0.320f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.560f, 0.840f, 0.120f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.035f, 0.070f, 0.020f, 1.0f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

static bool accent_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.14f, 0.14f, 1.0f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

static bool discord_button(const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.345f, 0.396f, 0.949f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.443f, 0.486f, 0.980f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.267f, 0.318f, 0.855f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.000f, 1.000f, 1.000f, 1.0f));
    bool clicked = ImGui::Button("   DISCORD", size);
    ImGui::PopStyleColor(4);

    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = mn.x + 24.0f;
    const float cy = (mn.y + mx.y) * 0.5f;
    const ImU32 white = IM_COL32(255, 255, 255, 240);
    const ImU32 blue  = IM_COL32(88, 101, 242, 255);

    dl->AddCircleFilled(ImVec2(cx, cy - 1.0f), 8.5f, white, 28);
    dl->AddTriangleFilled(ImVec2(cx - 7.0f, cy + 5.0f), ImVec2(cx - 2.0f, cy + 5.0f), ImVec2(cx - 7.0f, cy + 10.0f), white);
    dl->AddCircleFilled(ImVec2(cx - 3.2f, cy - 1.0f), 1.7f, blue, 12);
    dl->AddCircleFilled(ImVec2(cx + 3.2f, cy - 1.0f), 1.7f, blue, 12);

    return clicked;
}

static std::string fmt_bytes(uint64_t b) {
    char t[32];
    if (b >= (1ull << 30)) snprintf(t, sizeof(t), "%.2f GB", b / (double)(1ull << 30));
    else if (b >= (1ull << 20)) snprintf(t, sizeof(t), "%.1f MB", b / (double)(1ull << 20));
    else if (b >= (1ull << 10)) snprintf(t, sizeof(t), "%.1f KB", b / (double)(1ull << 10));
    else snprintf(t, sizeof(t), "%llu B", (unsigned long long)b);
    return t;
}

static void page_dashboard() {
    if (identity::is_elevated())
        ImGui::TextColored(ImVec4(0.678f, 1.0f, 0.184f, 1.0f), "ADMIN");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.380f, 0.380f, 1.0f), "SIN ADMIN");
    ImGui::Spacing();

    if (ImGui::BeginTable("##hw", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Vector", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Valor", ImGuiTableColumnFlags_WidthStretch);

        auto adapters = identity::read_adapters();
        std::string mac;
        for (auto& a : adapters) if (a.is_primary) mac = a.mac;

        struct Row { const char* label; std::string value; };
        Row rows[] = {
            { "MachineGuid", identity::read_machine_guid() },
            { "HwProfileGuid", identity::read_hw_profile_guid() },
            { "InstallDate", identity::read_install_date() },
            { "ProductId", identity::read_product_id() },
            { "Volume serial C:", identity::read_volume_serial('C') },
            { "MAC", mac },
            { "CPU", identity::read_cpu_id() },
            { "Disk serial", identity::read_disk_serial() },
            { "UDID", identity::compute_udid(mac) },
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.label);
            ImGui::TableNextColumn();
            if (r.value.empty()) ImGui::TextDisabled("—");
            else ImGui::TextUnformatted(r.value.c_str());
        }
        ImGui::EndTable();
    }
}

static void page_spoofer() {
    auto& o = app::g.opts;
    ImGui::Checkbox("L1  NEProtect", &o.layer_driver_block);
    ImGui::Checkbox("L2  Caches NetEase", &o.layer_purge_cache);
    ImGui::Checkbox("L3  GUIDs de registro", &o.layer_registry);
    ImGui::Checkbox("L4  Volume serial C:", &o.layer_volume);
    ImGui::Checkbox("L5  MAC", &o.layer_mac);
    ImGui::Spacing();
    ImGui::Checkbox("Sinkhole telemetria", &o.sinkhole_telemetry);
    ImGui::Checkbox("Limpiar rastros", &o.clean_traces);
    ImGui::Checkbox("Backup automatico", &o.make_backup);
    ImGui::Checkbox("Lanzar juego (RUNASINVOKER)", &o.launch_after_spoof);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(app::g.busy);
    if (lime_button("SPOOFEAR", ImVec2(200, 40))) {
        if (!app::g.has_target) {
            app::g.target = identity::make_synthetic();
            app::g.has_target = true;
            app::g.display_dirty = true;
        }
        app::start_apply();
    }
    ImGui::SameLine();
    if (ImGui::Button("RESTAURAR ULTIMO BACKUP", ImVec2(240, 40)))
        app::restore_latest();
    ImGui::EndDisabled();
    if (app::g.busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("trabajando...");
    }
}

static void page_cleaner() {
    if (!app::g.scanned) {
        app::g.scan_temp = cleaner::scan_temp();
        app::g.scan_own  = cleaner::scan_own_traces();
        app::g.scan_game = cleaner::scan_game_traces();
        app::g.scanned = true;
    }

    auto group = [](const char* name, std::vector<cleaner::CleanTarget>& t) {
        uint64_t bytes = 0, files = 0;
        for (auto& c : t) { bytes += c.bytes; files += c.files; }
        ImGui::TextUnformatted(name);
        ImGui::SameLine(150);
        ImGui::TextDisabled("%llu archivos  ·  %s", (unsigned long long)files, fmt_bytes(bytes).c_str());
    };
    group("Sistema", app::g.scan_temp);
    group("Panel", app::g.scan_own);
    group("Juego", app::g.scan_game);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("RE-ESCANEAR", ImVec2(150, 34)))
        app::g.scanned = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(app::g.busy);
    if (accent_button("LIMPIAR TODO", ImVec2(200, 34)))
        app::g.ask_wipe = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("basura y rastros");
}

static void confirm_wipe() {
    if (app::g.ask_wipe) {
        ImGui::OpenPopup("Limpiar");
        app::g.ask_wipe = false;
    }
    if (ImGui::BeginPopupModal("Limpiar", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Se borra la basura y los rastros. No se puede deshacer.");
        ImGui::Separator();
        if (accent_button("BORRAR", ImVec2(140, 0))) {
            std::vector<cleaner::CleanTarget> all;
            for (auto& t : app::g.scan_temp) all.push_back(t);
            for (auto& t : app::g.scan_own)  all.push_back(t);
            for (auto& t : app::g.scan_game) all.push_back(t);
            cleaner::wipe(all, [](int lvl, const std::string& m) { app::log_line(lvl, m); });
            app::g.scanned = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static NeuronField g_neurons;

static void main_window() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float adv_w = 38.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - adv_w);
    if (ImGui::Button("≡", ImVec2(adv_w, 30))) {
        if (g_hwnd_adv) destroy_advanced_window();
        else create_advanced_window();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Avanzado");

    static bool was_busy = false;
    if (app::g.busy) was_busy = true;
    else if (was_busy) { was_busy = false; app::g.display_dirty = true; }
    if (app::g.display_dirty) {
        app::g.display_rows = app::identity_rows();
        app::g.display_dirty = false;
    }

    const float card_w = 620.0f;
    const float card_h = 545.0f;
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float ox = origin.x + (avail.x - card_w) * 0.5f; if (ox < origin.x) ox = origin.x;
    float oy = origin.y + (avail.y - card_h) * 0.5f; if (oy < origin.y) oy = origin.y;
    ImGui::SetCursorScreenPos(ImVec2(ox, oy));
    ImGui::BeginChild("##card", ImVec2(card_w, card_h), true);

    const float pad = 24.0f;
    const float inner_w = card_w - pad * 2.0f;

    if (g_logo_srv) {
        const float logo = 104.0f;
        ImGui::SetCursorPosX(pad + (inner_w - logo) * 0.5f);
        ImGui::Image((ImTextureID)(intptr_t)g_logo_srv, ImVec2(logo, logo));
        ImGui::Dummy(ImVec2(0, 8));
    }

    {
        const char* t = "IDENTIDAD HARDWARE";
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::SetCursorPosX(pad + (inner_w - ts.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.678f, 1.0f, 0.184f, 1.0f));
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 5));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 5));
    }

    ImGui::SetCursorPosX(pad);
    if (ImGui::BeginTable("##ident", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_SizingStretchProp,
                          ImVec2(inner_w, 0))) {
        ImGui::TableSetupColumn("Vector", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Valor",  ImGuiTableColumnFlags_WidthStretch);
        for (auto& r : app::g.display_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.478f, 0.564f, 0.400f, 1.0f));
            ImGui::TextUnformatted(r.label.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            bool none = (r.value == "—");
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  none ? ImVec4(0.360f, 0.420f, 0.320f, 1.0f)
                                       : ImVec4(0.910f, 0.960f, 0.878f, 1.0f));
            if (g_mono) ImGui::PushFont(g_mono);
            ImGui::TextUnformatted(r.value.c_str());
            if (g_mono) ImGui::PopFont();
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 16));
    const float b1w = 280.0f, b2w = 200.0f, gap = 14.0f;
    const float total = b1w + b2w + gap;
    ImGui::SetCursorPosX(pad + (inner_w - total) * 0.5f);
    ImGui::BeginDisabled(app::g.busy);
    if (ImGui::Button("GENERAR OTRO PERFIL SINTÉTICO", ImVec2(b1w, 44))) {
        app::g.target = identity::make_synthetic();
        app::g.has_target = true;
        app::g.display_dirty = true;
        app::g.status_text.clear();
        app::g.status_level = -1;
    }
    ImGui::SameLine();
    if (lime_button(app::g.busy ? "SPOOFEANDO..." : "SPOOFEAR", ImVec2(b2w, 44))) {
        if (!app::g.has_target) {
            app::g.target = identity::make_synthetic();
            app::g.has_target = true;
            app::g.display_dirty = true;
        }
        app::start_apply();
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 12));
    const float dc_w = 190.0f, dc_h = 36.0f;
    ImGui::SetCursorPosX(pad + (inner_w - dc_w) * 0.5f);
    if (discord_button(ImVec2(dc_w, dc_h)))
        ShellExecuteW(nullptr, L"open", L"https://discord.gg/CE8xPnpV6a", nullptr, nullptr, SW_SHOWNORMAL);

    if (!app::g.busy && app::g.status_level >= 0 && !app::g.status_text.empty()) {
        ImGui::Dummy(ImVec2(0, 10));
        ImVec2 ts = ImGui::CalcTextSize(app::g.status_text.c_str(), nullptr, false, inner_w);
        ImGui::SetCursorPosX(ts.x >= inner_w ? pad : pad + (inner_w - ts.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, level_color_vec(app::g.status_level));
        ImGui::TextWrapped("%s", app::g.status_text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::End();
}

static void adv_resize(UINT w, UINT h) {
    if (!g_advSwapChain) return;
    if (g_advRTV) { g_advRTV->Release(); g_advRTV = nullptr; }
    g_advSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = nullptr;
    g_advSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_advRTV);
        bb->Release();
    }
}

static void create_advanced_window() {
    if (g_hwnd_adv) return;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       load_app_icon(0), nullptr, nullptr, nullptr, L"SPOOFERBS_ADV",
                       load_app_icon(GetSystemMetrics(SM_CXSMICON)) };
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int ww = 780, wh = 620;
    g_hwnd_adv = CreateWindowExW(0, L"SPOOFERBS_ADV", L"AVANZADO", WS_OVERLAPPEDWINDOW,
                                 (sw - ww) / 2 + 160, (sh - wh) / 2 + 80, ww, wh,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd_adv) return;

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_VIEW;
    sd.OutputWindow = g_hwnd_adv;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) {
        if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
            if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))))
                factory->CreateSwapChain(g_pd3dDevice, &sd, &g_advSwapChain);
            adapter->Release();
        }
        dxgi_dev->Release();
    }
    if (factory) factory->Release();

    if (!g_advSwapChain) {
        DestroyWindow(g_hwnd_adv);
        g_hwnd_adv = nullptr;
        return;
    }
    adv_resize((UINT)ww, (UINT)wh);

    g_ctx_adv = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx_adv);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_theme();
    ImFont* f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (!f) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(g_hwnd_adv);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    ImGui::SetCurrentContext(g_ctx_main);

    theme_titlebar(g_hwnd_adv);
    ShowWindow(g_hwnd_adv, SW_SHOW);
    UpdateWindow(g_hwnd_adv);
}

static void destroy_advanced_window() {
    if (!g_hwnd_adv) return;
    HWND h = g_hwnd_adv;
    if (g_ctx_adv) {
        ImGui::SetCurrentContext(g_ctx_adv);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ctx_adv);
        g_ctx_adv = nullptr;
        ImGui::SetCurrentContext(g_ctx_main);
    }
    if (g_advRTV) { g_advRTV->Release(); g_advRTV = nullptr; }
    if (g_advSwapChain) { g_advSwapChain->Release(); g_advSwapChain = nullptr; }
    DestroyWindow(h);
    g_hwnd_adv = nullptr;
}

static void console_draw() {
    for (auto& l : app::g.console) {
        ImGui::PushStyleColor(ImGuiCol_Text, level_color_vec(l.level));
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (app::g.scroll_console) {
        ImGui::SetScrollHereY(1.0f);
        app::g.scroll_console = false;
    }
}

static void advanced_render() {
    if (!g_hwnd_adv || !g_ctx_adv || !g_advSwapChain || !g_advRTV) return;

    ImGui::SetCurrentContext(g_ctx_adv);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##adv", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Panel")) {
            page_dashboard();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Spofear")) {
            ImGui::BeginChild("##c2", ImVec2(0, -170), false);
            page_spoofer();
            ImGui::EndChild();
            ImGui::BeginChild("##log2", ImVec2(0, 0), true);
            console_draw();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Limpieza")) {
            ImGui::BeginChild("##c3", ImVec2(0, -170), false);
            page_cleaner();
            ImGui::EndChild();
            ImGui::BeginChild("##log3", ImVec2(0, 0), true);
            console_draw();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    confirm_wipe();

    ImGui::Render();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_advRTV, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_advRTV, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_advSwapChain->Present(1, 0);

    ImGui::SetCurrentContext(g_ctx_main);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    UNREFERENCED_PARAMETER(hInstance);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool relaunched = wcsstr(GetCommandLineW(), L"-elevated") != nullptr;
    if (!identity::is_elevated() && !relaunched) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = path;
        sei.lpParameters = L"-elevated";
        sei.nShow = SW_SHOWNORMAL;
        sei.fMask = SEE_MASK_NOASYNC;
        if (ShellExecuteExW(&sei))
            return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       hInstance, load_app_icon(0), nullptr, nullptr, nullptr, L"SPOOFERBS",
                       load_app_icon(GetSystemMetrics(SM_CXSMICON)) };
    ::RegisterClassExW(&wc);

    int scr_w = GetSystemMetrics(SM_CXSCREEN);
    int scr_h = GetSystemMetrics(SM_CYSCREEN);
    const int win_w = 900, win_h = 640;
    int pos_x = (scr_w - win_w) / 2; if (pos_x < 0) pos_x = 0;
    int pos_y = (scr_h - win_h) / 2; if (pos_y < 0) pos_y = 0;
    HWND hwnd = ::CreateWindowExW(0, L"SPOOFERBS", L"SPOOFER BLOOD STRIKE",
                                  WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                  pos_x, pos_y, win_w, win_h,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    g_hwnd_main = hwnd;
    if (!hwnd || !CreateDeviceD3D(hwnd)) {
        ::MessageBoxA(nullptr, "No se pudo crear el dispositivo D3D11.", "SPOOFEAR", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        ::UnregisterClassW(L"SPOOFERBS", wc.hInstance);
        return 1;
    }

    theme_titlebar(hwnd);
    load_logo_texture(1);
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    g_ctx_main = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx_main);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    apply_theme();

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (!font)
        io.Fonts->AddFontDefault();
    g_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    if (wcsstr(GetCommandLineW(), L"-adv"))
        create_advanced_window();

    if (!identity::is_elevated())
        app::log_line(2, "Sin admin: L3/L4/hosts fallaran.");

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui::SetCurrentContext(g_ctx_main);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app::drain_pending();

        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            g_neurons.draw_now(io.DeltaTime, vp->WorkSize.x, vp->WorkSize.y);
        }

        main_window();

        ImGui::Render();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);

        advanced_render();
    }

    if (g_hwnd_adv) destroy_advanced_window();
    cleaner::wipe_self_traces(nullptr);

    ImGui::SetCurrentContext(g_ctx_main);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_ctx_main);
    g_ctx_main = nullptr;
    if (g_logo_srv) { g_logo_srv->Release(); g_logo_srv = nullptr; }
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(L"SPOOFERBS", wc.hInstance);
    CoUninitialize();
    return 0;
}
