#include "Clipboard.h"
#include "Database.h"

#include <shellapi.h>
#include <psapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "stb_image.h"

#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

    constexpr UINT WM_TRAYICON             = WM_APP + 1;
    constexpr UINT WM_SHOW_EXISTING        = WM_APP + 2;

    constexpr int  HOTKEY_ID_TOGGLE        = 1;

    constexpr UINT TRAY_ICON_ID            = 1001;
    constexpr UINT IDI_MAIN_ICON           = 101;
    constexpr UINT IDM_TRAY_SHOW           = 2001;
    constexpr UINT IDM_TRAY_CLEAR          = 2002;
    constexpr UINT IDM_TRAY_STARTUP        = 2003;
    constexpr UINT IDM_TRAY_EXIT           = 2004;

    constexpr int  WINDOW_WIDTH            = 440;
    constexpr int  WINDOW_HEIGHT           = 560;

    constexpr const wchar_t* RUN_REG_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* RUN_APP_NAME  = L"Clipped";

    // Paleta de colores exacta de Windows 95 / 98
    constexpr ImU32 COL_WIN98_FACE         = IM_COL32(192, 192, 192, 255); // #C0C0C0
    constexpr ImU32 COL_WIN98_HIGHLIGHT    = IM_COL32(255, 255, 255, 255); // #FFFFFF
    constexpr ImU32 COL_WIN98_LIGHT        = IM_COL32(223, 223, 223, 255); // #DFDFDF
    constexpr ImU32 COL_WIN98_SHADOW       = IM_COL32(128, 128, 128, 255); // #808080
    constexpr ImU32 COL_WIN98_DARKSHADOW   = IM_COL32(0,   0,   0,   255); // #000000
    constexpr ImU32 COL_WIN98_NAVY         = IM_COL32(0,   0,   128, 255); // #000080
    constexpr ImU32 COL_WIN98_GRADIENT     = IM_COL32(16,  132, 208, 255); // #1084D0
    constexpr ImU32 COL_WIN98_WINDOW_BG    = IM_COL32(255, 255, 255, 255); // #FFFFFF
    constexpr ImU32 COL_WIN98_TEXT_BLACK   = IM_COL32(0,   0,   0,   255);
    constexpr ImU32 COL_WIN98_TEXT_WHITE   = IM_COL32(255, 255, 255, 255);

    // Estado global de datos y portapapeles
    Clipped::Database g_Database;
    std::string       g_LastText = "";
    uint64_t          g_LastImgHash = 0;

    // Estado de la ventana y caché temporal (solo vive mientras g_IsWindowVisible == true)
    bool              g_IsWindowVisible = false;
    bool              g_FocusSearchOnOpen = false;
    bool              g_NeedsReload = true;
    bool              g_ScrollToSelected = false;
    int               g_SelectedIndex = 0;
    int64_t           g_OpenedAtMs = 0;
    HWND              g_LastForegroundHwnd = nullptr;
    char              g_SearchBuffer[256] = {};
    std::vector<Clipped::ClipRecord> g_CachedClips;

    // Recursos de DirectX 11
    ID3D11Device*           g_pd3dDevice = nullptr;
    ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
    IDXGISwapChain*         g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

    struct ThumbnailTexture {
        ID3D11ShaderResourceView* srv = nullptr;
        int width = 0;
        int height = 0;
    };

    std::unordered_map<int64_t, ThumbnailTexture> g_ThumbnailCache;

    int64_t NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string WideToUTF8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        if (bytesNeeded <= 0) return "";
        std::string result(static_cast<size_t>(bytesNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), bytesNeeded, nullptr, nullptr);
        return result;
    }

    std::string GetDatabasePathUTF8() {
        wchar_t localAppData[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring dir = std::wstring(localAppData) + L"\\Clipped";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring dbFile = dir + L"\\clipped.db";
            return WideToUTF8(dbFile);
        }
        return "clipped.db";
    }

    bool IsStartupEnabled() {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t value[MAX_PATH + 16] = {};
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LSTATUS status = RegQueryValueExW(hKey, RUN_APP_NAME, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(hKey);
        return (status == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0');
    }

    void SetStartupEnabled(bool enable) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
            return;
        }
        if (enable) {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring quotedPath = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(
                hKey,
                RUN_APP_NAME,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(quotedPath.c_str()),
                static_cast<DWORD>((quotedPath.size() + 1) * sizeof(wchar_t))
            );
        } else {
            RegDeleteValueW(hKey, RUN_APP_NAME);
        }
        RegCloseKey(hKey);
    }

    std::string FormatRelativeTime(int64_t createdAtMs) {
        const int64_t diffSec = std::max<int64_t>(0, (NowMs() - createdAtMs) / 1000);

        char buf[32] = {};
        if (diffSec < 60) {
            return "ahora";
        } else if (diffSec < 3600) {
            std::snprintf(buf, sizeof(buf), "hace %lld min", static_cast<long long>(diffSec / 60));
        } else if (diffSec < 86400) {
            std::snprintf(buf, sizeof(buf), "hace %lld h", static_cast<long long>(diffSec / 3600));
        } else {
            std::snprintf(buf, sizeof(buf), "hace %lld d", static_cast<long long>(diffSec / 86400));
        }
        return std::string(buf);
    }

    // Dibuja un borde biselado 3D elevado estilo Windows 95/98 (Outset)
    void DrawBevelRaised(ImDrawList* dl, const ImVec2& pMin, const ImVec2& pMax) {
        // Exterior superior e izquierdo (Blanco brillante)
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMax.x - 1.0f, pMin.y), COL_WIN98_HIGHLIGHT, 1.0f);
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMin.x, pMax.y - 1.0f), COL_WIN98_HIGHLIGHT, 1.0f);
        // Interior superior e izquierdo (Gris claro)
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMin.y + 1.0f), ImVec2(pMax.x - 2.0f, pMin.y + 1.0f), COL_WIN98_LIGHT, 1.0f);
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMin.y + 1.0f), ImVec2(pMin.x + 1.0f, pMax.y - 2.0f), COL_WIN98_LIGHT, 1.0f);
        // Interior inferior y derecho (Gris sombra #808080)
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMax.y - 2.0f), ImVec2(pMax.x - 2.0f, pMax.y - 2.0f), COL_WIN98_SHADOW, 1.0f);
        dl->AddLine(ImVec2(pMax.x - 2.0f, pMin.y + 1.0f), ImVec2(pMax.x - 2.0f, pMax.y - 2.0f), COL_WIN98_SHADOW, 1.0f);
        // Exterior inferior y derecho (Negro #000000)
        dl->AddLine(ImVec2(pMin.x, pMax.y - 1.0f), ImVec2(pMax.x - 1.0f, pMax.y - 1.0f), COL_WIN98_DARKSHADOW, 1.0f);
        dl->AddLine(ImVec2(pMax.x - 1.0f, pMin.y), ImVec2(pMax.x - 1.0f, pMax.y), COL_WIN98_DARKSHADOW, 1.0f);
    }

    // Dibuja un borde biselado 3D hundido estilo Windows 95/98 (Inset / Sunken)
    void DrawBevelSunken(ImDrawList* dl, const ImVec2& pMin, const ImVec2& pMax) {
        // Exterior superior e izquierdo (Gris sombra #808080)
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMax.x - 1.0f, pMin.y), COL_WIN98_SHADOW, 1.0f);
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMin.x, pMax.y - 1.0f), COL_WIN98_SHADOW, 1.0f);
        // Interior superior e izquierdo (Negro #000000)
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMin.y + 1.0f), ImVec2(pMax.x - 2.0f, pMin.y + 1.0f), COL_WIN98_DARKSHADOW, 1.0f);
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMin.y + 1.0f), ImVec2(pMin.x + 1.0f, pMax.y - 2.0f), COL_WIN98_DARKSHADOW, 1.0f);
        // Interior inferior y derecho (Gris claro #DFDFDF)
        dl->AddLine(ImVec2(pMin.x + 1.0f, pMax.y - 2.0f), ImVec2(pMax.x - 2.0f, pMax.y - 2.0f), COL_WIN98_LIGHT, 1.0f);
        dl->AddLine(ImVec2(pMax.x - 2.0f, pMin.y + 1.0f), ImVec2(pMax.x - 2.0f, pMax.y - 2.0f), COL_WIN98_LIGHT, 1.0f);
        // Exterior inferior y derecho (Blanco #FFFFFF)
        dl->AddLine(ImVec2(pMin.x, pMax.y - 1.0f), ImVec2(pMax.x - 1.0f, pMax.y - 1.0f), COL_WIN98_HIGHLIGHT, 1.0f);
        dl->AddLine(ImVec2(pMax.x - 1.0f, pMin.y), ImVec2(pMax.x - 1.0f, pMax.y), COL_WIN98_HIGHLIGHT, 1.0f);
    }

    // Dibuja un panel hundido simple de 1px (usado en la barra de estado inferior de Win95)
    void DrawStatusPanelSunken(ImDrawList* dl, const ImVec2& pMin, const ImVec2& pMax) {
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMax.x - 1.0f, pMin.y), COL_WIN98_SHADOW, 1.0f);
        dl->AddLine(ImVec2(pMin.x, pMin.y), ImVec2(pMin.x, pMax.y - 1.0f), COL_WIN98_SHADOW, 1.0f);
        dl->AddLine(ImVec2(pMin.x, pMax.y - 1.0f), ImVec2(pMax.x - 1.0f, pMax.y - 1.0f), COL_WIN98_HIGHLIGHT, 1.0f);
        dl->AddLine(ImVec2(pMax.x - 1.0f, pMin.y), ImVec2(pMax.x - 1.0f, pMax.y), COL_WIN98_HIGHLIGHT, 1.0f);
    }

    // Botón con bisel 3D interactivo de Windows 95/98
    bool Win98Button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.753f, 0.753f, 0.753f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.800f, 0.800f, 0.800f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.700f, 0.700f, 0.700f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.0f,   0.0f,   0.0f,   1.0f));

        const bool clicked = ImGui::Button(label, size);
        const bool active  = ImGui::IsItemActive();
        const ImVec2 pMin  = ImGui::GetItemRectMin();
        const ImVec2 pMax  = ImGui::GetItemRectMax();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (active) {
            DrawBevelSunken(dl, pMin, pMax);
        } else {
            DrawBevelRaised(dl, pMin, pMax);
        }

        ImGui::PopStyleColor(4);
        return clicked;
    }

    void ReleaseAllThumbnails() {
        for (auto& [id, tex] : g_ThumbnailCache) {
            if (tex.srv) {
                tex.srv->Release();
                tex.srv = nullptr;
            }
        }
        g_ThumbnailCache.clear();
    }

    void TrimProcessMemory() {
        if (g_pd3dDeviceContext) {
            g_pd3dDeviceContext->ClearState();
            g_pd3dDeviceContext->Flush();
        }
        g_Database.ReleaseMemory();
        K32EmptyWorkingSet(GetCurrentProcess());
    }

    void ReloadClipsFromDB() {
        g_CachedClips = g_Database.GetClips(g_SearchBuffer, 100);
        if (g_CachedClips.empty()) {
            g_SelectedIndex = 0;
        } else {
            g_SelectedIndex = std::clamp(g_SelectedIndex, 0, static_cast<int>(g_CachedClips.size()) - 1);
        }
        g_NeedsReload = false;
    }

    void ShowPopupWindow(HWND hwnd) {
        HWND currentFg = GetForegroundWindow();
        if (currentFg != nullptr && currentFg != hwnd) {
            g_LastForegroundHwnd = currentFg;
        }

        POINT pt = {};
        GetCursorPos(&pt);

        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        int x = pt.x - (WINDOW_WIDTH / 2);
        int y = pt.y - 30;

        if (GetMonitorInfoW(hMon, &mi)) {
            const RECT& work = mi.rcWork;
            x = std::clamp(x, static_cast<int>(work.left + 8), static_cast<int>(work.right - WINDOW_WIDTH - 8));
            y = std::clamp(y, static_cast<int>(work.top + 8), static_cast<int>(work.bottom - WINDOW_HEIGHT - 8));
        }

        g_OpenedAtMs = NowMs();
        g_SelectedIndex = 0;
        g_NeedsReload = true;
        g_FocusSearchOnOpen = true;

        SetWindowPos(hwnd, HWND_TOPMOST, x, y, WINDOW_WIDTH, WINDOW_HEIGHT, SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        g_IsWindowVisible = true;
    }

    void HidePopupWindow(HWND hwnd) {
        if (!g_IsWindowVisible) return;

        g_IsWindowVisible = false;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearInputKeys();
            io.ClearEventsQueue();
            io.AddFocusEvent(false);
        }
        ShowWindow(hwnd, SW_HIDE);

        ReleaseAllThumbnails();
        g_CachedClips.clear();
        g_CachedClips.shrink_to_fit();
        TrimProcessMemory();
    }

    void TogglePopupWindow(HWND hwnd) {
        if (g_IsWindowVisible) {
            HidePopupWindow(hwnd);
        } else {
            ShowPopupWindow(hwnd);
        }
    }

    void SimulateCtrlV() {
        int waitedMs = 0;
        while (((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) && waitedMs < 250) {
            Sleep(10);
            waitedMs += 10;
        }

        INPUT inputs[6] = {};
        int idx = 0;

        auto addKey = [&](WORD vk, DWORD flags) {
            inputs[idx].type = INPUT_KEYBOARD;
            inputs[idx].ki.wVk = vk;
            inputs[idx].ki.dwFlags = flags;
            ++idx;
        };

        if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
            addKey(VK_LMENU, KEYEVENTF_KEYUP);
            addKey(VK_RMENU, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY);
        }

        addKey(VK_CONTROL, 0);
        addKey('V',        0);
        addKey('V',        KEYEVENTF_KEYUP);
        addKey(VK_CONTROL, KEYEVENTF_KEYUP);

        SendInput(static_cast<UINT>(idx), inputs, sizeof(INPUT));
    }

    void RestoreClipAndAutoPaste(HWND hwnd, const Clipped::ClipRecord& clip) {
        if (clip.type == Clipped::ClipType::Text) {
            g_LastText = clip.textContent;
            g_LastImgHash = 0;
            Clipped::Clipboard::SetTextUTF8(hwnd, clip.textContent);
        } else {
            g_LastImgHash = clip.hash;
            g_LastText.clear();
            Clipped::Clipboard::SetImageFromPNG(hwnd, clip.imageBlob);
        }

        g_Database.TouchClip(clip.id);
        HWND targetWnd = g_LastForegroundHwnd;

        if (targetWnd != nullptr && targetWnd != hwnd && IsWindow(targetWnd)) {
            SetForegroundWindow(targetWnd);
        }

        HidePopupWindow(hwnd);

        if (targetWnd != nullptr && targetWnd != hwnd && IsWindow(targetWnd)) {
            Sleep(35);
            SimulateCtrlV();
        }
    }

    ThumbnailTexture GetOrCreateThumbnail(const Clipped::ClipRecord& clip) {
        auto it = g_ThumbnailCache.find(clip.id);
        if (it != g_ThumbnailCache.end()) {
            return it->second;
        }

        ThumbnailTexture result = {};
        if (clip.imageBlob.empty() || !g_pd3dDevice) {
            g_ThumbnailCache[clip.id] = result;
            return result;
        }

        int srcW = 0, srcH = 0, channels = 0;
        uint8_t* rgba = stbi_load_from_memory(
            clip.imageBlob.data(),
            static_cast<int>(clip.imageBlob.size()),
            &srcW, &srcH, &channels, 4
        );
        if (!rgba || srcW <= 0 || srcH <= 0) {
            if (rgba) stbi_image_free(rgba);
            g_ThumbnailCache[clip.id] = result;
            return result;
        }

        constexpr int MAX_THUMB_W = 320;
        constexpr int MAX_THUMB_H = 150;
        float scale = 1.0f;
        if (srcW > MAX_THUMB_W) scale = static_cast<float>(MAX_THUMB_W) / static_cast<float>(srcW);
        if (srcH * scale > MAX_THUMB_H) scale = static_cast<float>(MAX_THUMB_H) / static_cast<float>(srcH);

        int dstW = std::max(1, static_cast<int>(srcW * scale));
        int dstH = std::max(1, static_cast<int>(srcH * scale));

        std::vector<uint8_t> scaledPixels;
        const uint8_t* uploadPixels = rgba;

        if (dstW < srcW || dstH < srcH) {
            scaledPixels.resize(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4);
            for (int y = 0; y < dstH; ++y) {
                int sy = (y * srcH) / dstH;
                const uint8_t* srcRow = rgba + (static_cast<size_t>(sy) * srcW * 4);
                uint8_t* dstRow = scaledPixels.data() + (static_cast<size_t>(y) * dstW * 4);
                for (int x = 0; x < dstW; ++x) {
                    int sx = (x * srcW) / dstW;
                    dstRow[x * 4 + 0] = srcRow[sx * 4 + 0];
                    dstRow[x * 4 + 1] = srcRow[sx * 4 + 1];
                    dstRow[x * 4 + 2] = srcRow[sx * 4 + 2];
                    dstRow[x * 4 + 3] = srcRow[sx * 4 + 3];
                }
            }
            uploadPixels = scaledPixels.data();
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(dstW);
        desc.Height = static_cast<UINT>(dstH);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subResource = {};
        subResource.pSysMem = uploadPixels;
        subResource.SysMemPitch = static_cast<UINT>(dstW * 4);

        ID3D11Texture2D* pTexture = nullptr;
        if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture)) && pTexture) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &result.srv);
            pTexture->Release();
            result.width = dstW;
            result.height = dstH;
        }

        stbi_image_free(rgba);
        g_ThumbnailCache[clip.id] = result;
        return result;
    }

    void CreateRenderTarget() {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }

    void CleanupRenderTarget() {
        if (g_mainRenderTargetView) {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hWnd) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = WINDOW_WIDTH;
        sd.BufferDesc.Height = WINDOW_HEIGHT;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
        if (res != S_OK) return false;

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D() {
        ReleaseAllThumbnails();
        CleanupRenderTarget();
        if (g_pSwapChain)        { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
        if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
        if (g_pd3dDevice)        { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    }

    void AddTrayIcon(HWND hwnd, bool showWelcomeBalloon) {
        HICON hCustomIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MAIN_ICON));
        if (!hCustomIcon) {
            hCustomIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
        }

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = hCustomIcon;
        wcscpy_s(nid.szTip, L"Clipped - Portapapeles Retro (Ctrl + Alt + V)");

        if (showWelcomeBalloon) {
            nid.uFlags |= NIF_INFO;
            nid.dwInfoFlags = NIIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"Clipped está activo en segundo plano");
            wcscpy_s(nid.szInfo, L"Presiona Ctrl + Alt + V en cualquier momento para abrir tu historial de portapapeles.");
        }

        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void RemoveTrayIcon(HWND hwnd) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void ShowTrayContextMenu(HWND hwnd) {
        POINT pt = {};
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
            const UINT startupFlags = MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"Mostrar Historial (Ctrl + Alt + V)");
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CLEAR, L"Limpiar historial no fijado");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, startupFlags, IDM_TRAY_STARTUP, L"Iniciar con Windows");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Salir de Clipped");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
    }

    std::string BuildTextPreview(const std::string& text, size_t maxChars = 220, int maxLines = 4) {
        std::string preview;
        preview.reserve(std::min<size_t>(text.size(), maxChars + 8));
        int lines = 0;
        for (char c : text) {
            if (c == '\r') continue;
            if (c == '\n') {
                if (++lines >= maxLines) {
                    preview += " ...";
                    break;
                }
            }
            preview.push_back(c);
            if (preview.size() >= maxChars) {
                preview += "...";
                break;
            }
        }
        return preview;
    }

    void ApplyWin98RetroStyle() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 0.0f;
        style.FrameRounding     = 0.0f;
        style.ChildRounding     = 0.0f;
        style.PopupRounding     = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding      = 0.0f;
        style.WindowBorderSize  = 0.0f;
        style.FrameBorderSize   = 0.0f;
        style.ChildBorderSize   = 0.0f;
        style.ScrollbarSize     = 16.0f;
        style.ItemSpacing       = ImVec2(6.0f, 6.0f);
        style.WindowPadding     = ImVec2(6.0f, 6.0f);
        style.FramePadding      = ImVec2(5.0f, 3.0f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]             = ImVec4(0.753f, 0.753f, 0.753f, 1.00f); // #C0C0C0
        colors[ImGuiCol_ChildBg]              = ImVec4(1.000f, 1.000f, 1.000f, 1.00f); // #FFFFFF (ListView clásico)
        colors[ImGuiCol_PopupBg]              = ImVec4(1.000f, 1.000f, 0.882f, 1.00f); // #FFFFE1 (Tooltip amarillo clásico de Win98)
        colors[ImGuiCol_Border]               = ImVec4(0.000f, 0.000f, 0.000f, 1.00f);
        colors[ImGuiCol_Text]                 = ImVec4(0.000f, 0.000f, 0.000f, 1.00f);
        colors[ImGuiCol_TextDisabled]         = ImVec4(0.420f, 0.420f, 0.420f, 1.00f);
        colors[ImGuiCol_FrameBg]              = ImVec4(1.000f, 1.000f, 1.000f, 1.00f); // Caja de texto blanca
        colors[ImGuiCol_FrameBgHovered]       = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
        colors[ImGuiCol_FrameBgActive]        = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.000f, 0.000f, 0.502f, 0.35f);
        colors[ImGuiCol_Button]               = ImVec4(0.753f, 0.753f, 0.753f, 1.00f);
        colors[ImGuiCol_ButtonHovered]        = ImVec4(0.800f, 0.800f, 0.800f, 1.00f);
        colors[ImGuiCol_ButtonActive]         = ImVec4(0.700f, 0.700f, 0.700f, 1.00f);
        colors[ImGuiCol_Header]               = ImVec4(0.000f, 0.000f, 0.502f, 1.00f); // Azul marino #000080
        colors[ImGuiCol_HeaderHovered]        = ImVec4(0.040f, 0.160f, 0.620f, 1.00f);
        colors[ImGuiCol_HeaderActive]         = ImVec4(0.000f, 0.000f, 0.502f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.875f, 0.875f, 0.875f, 1.00f); // Trama clara de barra de scroll
        colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.753f, 0.753f, 0.753f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.780f, 0.780f, 0.780f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.680f, 0.680f, 0.680f, 1.00f);
    }

    void DrawWin98ClipboardIcon(ImDrawList* dl, const ImVec2& pos) {
        // Pequeño ícono pixel-art procedural de portapapeles de 14x14 px en la barra de título
        dl->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 2.0f), ImVec2(pos.x + 13.0f, pos.y + 15.0f), IM_COL32(160, 110, 50, 255));
        dl->AddRect(ImVec2(pos.x + 1.0f, pos.y + 2.0f), ImVec2(pos.x + 13.0f, pos.y + 15.0f), IM_COL32(0, 0, 0, 255));
        // Hoja de papel blanca
        dl->AddRectFilled(ImVec2(pos.x + 3.0f, pos.y + 4.0f), ImVec2(pos.x + 11.0f, pos.y + 13.0f), IM_COL32(255, 255, 255, 255));
        // Pinza metálica superior
        dl->AddRectFilled(ImVec2(pos.x + 4.0f, pos.y + 1.0f), ImVec2(pos.x + 10.0f, pos.y + 4.0f), IM_COL32(192, 192, 192, 255));
        dl->AddRect(ImVec2(pos.x + 4.0f, pos.y + 1.0f), ImVec2(pos.x + 10.0f, pos.y + 4.0f), IM_COL32(0, 0, 0, 255));
        // Líneas de texto dentro de la hoja
        dl->AddLine(ImVec2(pos.x + 5.0f, pos.y + 7.0f),  ImVec2(pos.x + 9.0f, pos.y + 7.0f),  COL_WIN98_NAVY, 1.0f);
        dl->AddLine(ImVec2(pos.x + 5.0f, pos.y + 9.0f),  ImVec2(pos.x + 9.0f, pos.y + 9.0f),  COL_WIN98_SHADOW, 1.0f);
        dl->AddLine(ImVec2(pos.x + 5.0f, pos.y + 11.0f), ImVec2(pos.x + 8.0f, pos.y + 11.0f), COL_WIN98_SHADOW, 1.0f);
    }

    void RenderUI(HWND hwnd) {
        // Cerrar si el usuario hace clic con el ratón fuera del rectángulo de la ventana de Clipped
        if ((NowMs() - g_OpenedAtMs) > 250) {
            const bool mouseDown = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                                   ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            if (mouseDown) {
                POINT pt = {};
                RECT rc = {};
                if (GetCursorPos(&pt) && GetWindowRect(hwnd, &rc) && !PtInRect(&rc, pt)) {
                    HidePopupWindow(hwnd);
                    return;
                }
            }
        }

        if (g_NeedsReload) {
            ReloadClipsFromDB();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::EndFrame();
            HidePopupWindow(hwnd);
            return;
        }

        // Navegación rápida por teclado (Flechas Arriba/Abajo + Enter)
        if (!g_CachedClips.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                g_SelectedIndex = std::min(static_cast<int>(g_CachedClips.size()) - 1, g_SelectedIndex + 1);
                g_ScrollToSelected = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                g_SelectedIndex = std::max(0, g_SelectedIndex - 1);
                g_ScrollToSelected = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                const Clipped::ClipRecord selectedClip = g_CachedClips[g_SelectedIndex];
                ImGui::EndFrame();
                RestoreClipAndAutoPaste(hwnd, selectedClip);
                return;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WINDOW_WIDTH), static_cast<float>(WINDOW_HEIGHT)));

        constexpr ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings;

        int clickedIndex = -1;

        if (ImGui::Begin("##ClippedWin98Window", nullptr, windowFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // 1. Barra de título clásica de Windows 98 con gradiente Azul Marino (#000080 -> #1084D0)
            const ImVec2 titleMin(4.0f, 4.0f);
            const ImVec2 titleMax(static_cast<float>(WINDOW_WIDTH) - 4.0f, 28.0f);
            dl->AddRectFilledMultiColor(
                titleMin, titleMax,
                COL_WIN98_NAVY, COL_WIN98_GRADIENT,
                COL_WIN98_GRADIENT, COL_WIN98_NAVY
            );

            DrawWin98ClipboardIcon(dl, ImVec2(8.0f, 8.0f));
            dl->AddText(ImVec2(26.0f, 8.0f), COL_WIN98_TEXT_WHITE, "Clipped - Visor del Portapapeles");

            // Botones 3D en la barra de título ([Limpiar] y [X])
            ImGui::SetCursorPos(ImVec2(static_cast<float>(WINDOW_WIDTH) - 92.0f, 6.0f));
            if (Win98Button("Limpiar", ImVec2(62.0f, 20.0f))) {
                ReleaseAllThumbnails();
                g_Database.ClearUnpinned();
                g_NeedsReload = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Eliminar todos los elementos no fijados");
            }

            ImGui::SetCursorPos(ImVec2(static_cast<float>(WINDOW_WIDTH) - 26.0f, 6.0f));
            if (Win98Button("X", ImVec2(20.0f, 20.0f))) {
                ImGui::End();
                ImGui::EndFrame();
                HidePopupWindow(hwnd);
                return;
            }

            // 2. Fila de Búsqueda con campo hundido (Sunken 3D Edit Control)
            ImGui::SetCursorPos(ImVec2(8.0f, 36.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Buscar:");
            ImGui::SameLine(62.0f);

            if (g_FocusSearchOnOpen) {
                ImGui::SetKeyboardFocusHere();
                g_FocusSearchOnOpen = false;
            }

            ImGui::SetNextItemWidth(static_cast<float>(WINDOW_WIDTH) - 70.0f);
            if (ImGui::InputTextWithHint("##SearchInput", "Escriba para filtrar textos...", g_SearchBuffer, sizeof(g_SearchBuffer))) {
                g_SelectedIndex = 0;
                g_NeedsReload = true;
            }
            DrawBevelSunken(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // 3. Pozo de Lista Principal Hundido (Sunken ListView Well #FFFFFF)
            const ImVec2 listPos(8.0f, 66.0f);
            const ImVec2 listSize(static_cast<float>(WINDOW_WIDTH) - 16.0f, static_cast<float>(WINDOW_HEIGHT) - 98.0f);
            ImGui::SetCursorPos(listPos);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
            if (ImGui::BeginChild("##ClipsScrollRegion", listSize, false)) {
                ImDrawList* childDl = ImGui::GetWindowDrawList();

                if (g_CachedClips.empty()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("  (La carpeta del portapapeles está vacía)");
                }

                for (size_t i = 0; i < g_CachedClips.size(); ++i) {
                    const auto& clip = g_CachedClips[i];
                    const bool isSelected = (static_cast<int>(i) == g_SelectedIndex);

                    ImGui::PushID(static_cast<int>(clip.id));

                    const ImVec2 cardStart = ImGui::GetCursorScreenPos();
                    const float availWidth = ImGui::GetContentRegionAvail().x;

                    ImGui::BeginGroup();

                    // Encabezado de la fila: Tipo + Tamaño + Tiempo relativo + Botones 3D
                    if (clip.isPinned) {
                        ImGui::TextColored(ImVec4(0.55f, 0.0f, 0.0f, 1.0f), "[FIJADO]");
                        ImGui::SameLine();
                    }

                    std::string relTime = FormatRelativeTime(clip.createdAt);
                    if (clip.type == Clipped::ClipType::Text) {
                        ImGui::TextDisabled("TEXTO (%zu B) - %s", clip.textContent.size(), relTime.c_str());
                    } else {
                        ImGui::TextDisabled("IMAGEN (%dx%d | %.1f KB) - %s",
                            clip.width, clip.height, clip.imageBlob.size() / 1024.0f, relTime.c_str());
                    }

                    float rightOffset = ImGui::GetWindowContentRegionMax().x - 96.0f;
                    if (rightOffset > ImGui::GetCursorPosX()) {
                        ImGui::SameLine(rightOffset);
                    } else {
                        ImGui::SameLine();
                    }

                    if (Win98Button(clip.isPinned ? "Desfijar" : " Fijar ", ImVec2(64.0f, 20.0f))) {
                        g_Database.SetPinned(clip.id, !clip.isPinned);
                        g_NeedsReload = true;
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                    if (Win98Button("X", ImVec2(22.0f, 20.0f))) {
                        if (clip.type == Clipped::ClipType::Image) {
                            auto it = g_ThumbnailCache.find(clip.id);
                            if (it != g_ThumbnailCache.end()) {
                                if (it->second.srv) it->second.srv->Release();
                                g_ThumbnailCache.erase(it);
                            }
                        }
                        g_Database.DeleteClip(clip.id);
                        g_NeedsReload = true;
                    }

                    // Contenido seleccionable estilo ListView de Windows 98 (#000080 con texto blanco al seleccionar)
                    if (clip.type == Clipped::ClipType::Text) {
                        std::string preview = BuildTextPreview(clip.textContent);
                        if (isSelected) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }
                        if (ImGui::Selectable(preview.c_str(), isSelected, 0, ImVec2(availWidth, 0.0f))) {
                            clickedIndex = static_cast<int>(i);
                        }
                        if (isSelected) {
                            ImGui::PopStyleColor();
                            // Borde interior punteado/clásico de selección
                            childDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 0, 200));
                        }

                        if (ImGui::IsItemHovered() && clip.textContent.size() > 140) {
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                            if (ImGui::BeginItemTooltip()) {
                                ImGui::PushTextWrapPos(360.0f);
                                std::string fullPreview = BuildTextPreview(clip.textContent, 900, 18);
                                ImGui::TextUnformatted(fullPreview.c_str());
                                ImGui::PopTextWrapPos();
                                ImGui::EndTooltip();
                            }
                            ImGui::PopStyleVar();
                        }
                    } else {
                        ThumbnailTexture thumb = GetOrCreateThumbnail(clip);
                        if (thumb.srv && thumb.width > 0 && thumb.height > 0) {
                            float displayW = static_cast<float>(thumb.width);
                            float displayH = static_cast<float>(thumb.height);
                            if (displayW > availWidth - 16.0f) {
                                float ratio = (availWidth - 16.0f) / displayW;
                                displayW *= ratio;
                                displayH *= ratio;
                            }
                            if (isSelected) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.502f, 1.0f));
                            }
                            if (ImGui::ImageButton("##ThumbBtn", (ImTextureID)(intptr_t)thumb.srv, ImVec2(displayW, displayH))) {
                                clickedIndex = static_cast<int>(i);
                            }
                            DrawBevelSunken(childDl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                            if (isSelected) {
                                ImGui::PopStyleColor();
                            }
                        } else {
                            if (isSelected) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                            }
                            if (ImGui::Selectable("[Mapa de bits DIB - Clic para pegar]", isSelected, 0, ImVec2(availWidth, 28.0f))) {
                                clickedIndex = static_cast<int>(i);
                            }
                            if (isSelected) {
                                ImGui::PopStyleColor();
                            }
                        }
                    }

                    if (isSelected && g_ScrollToSelected) {
                        ImGui::SetScrollHereY(0.5f);
                        g_ScrollToSelected = false;
                    }

                    ImGui::EndGroup();

                    // Línea divisoria estilo punteado/gris de Windows 98 entre elementos
                    const float sepY = ImGui::GetCursorScreenPos().y + 2.0f;
                    childDl->AddLine(ImVec2(cardStart.x, sepY), ImVec2(cardStart.x + availWidth, sepY), COL_WIN98_LIGHT, 1.0f);
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));

                    ImGui::PopID();

                    if (clickedIndex >= 0) {
                        break;
                    }
                }

                ImGui::EndChild();
            }
            ImGui::PopStyleVar();

            // Dibujar el bisel hundido 3D alrededor del pozo blanco de la lista
            DrawBevelSunken(dl, listPos, ImVec2(listPos.x + listSize.x, listPos.y + listSize.y));

            // 4. Barra de Estado Inferior estilo Windows 95 (2 celdas hundidas)
            const float statusY = static_cast<float>(WINDOW_HEIGHT) - 26.0f;
            const ImVec2 panel1Min(8.0f, statusY);
            const ImVec2 panel1Max(static_cast<float>(WINDOW_WIDTH) - 106.0f, static_cast<float>(WINDOW_HEIGHT) - 6.0f);
            const ImVec2 panel2Min(static_cast<float>(WINDOW_WIDTH) - 102.0f, statusY);
            const ImVec2 panel2Max(static_cast<float>(WINDOW_WIDTH) - 8.0f, static_cast<float>(WINDOW_HEIGHT) - 6.0f);

            DrawStatusPanelSunken(dl, panel1Min, panel1Max);
            DrawStatusPanelSunken(dl, panel2Min, panel2Max);

            char statusText[96] = {};
            std::snprintf(statusText, sizeof(statusText), "%zu objeto(s)  |  Flechas + Enter para pegar", g_CachedClips.size());
            dl->AddText(ImVec2(panel1Min.x + 6.0f, panel1Min.y + 2.0f), COL_WIN98_TEXT_BLACK, statusText);
            dl->AddText(ImVec2(panel2Min.x + 10.0f, panel2Min.y + 2.0f), COL_WIN98_TEXT_BLACK, "Ctrl+Alt+V");

            // 5. Bisel 3D Exterior de toda la ventana de Windows 98
            DrawBevelRaised(dl, ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(WINDOW_WIDTH), static_cast<float>(WINDOW_HEIGHT)));
        }
        ImGui::End();

        if (clickedIndex >= 0 && clickedIndex < static_cast<int>(g_CachedClips.size())) {
            const Clipped::ClipRecord chosen = g_CachedClips[clickedIndex];
            ImGui::EndFrame();
            RestoreClipAndAutoPaste(hwnd, chosen);
            return;
        }

        ImGui::Render();
        const float clearColor[4] = { 0.753f, 0.753f, 0.753f, 1.00f }; // #C0C0C0
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync activado (0% desperdicio de GPU)
    }

} // namespace

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam)) {
        return true;
    }

    switch (uMsg) {
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (wParam == VK_MENU || wParam == VK_SPACE) {
            return 0;
        }
        break;

    case WM_CLIPBOARDUPDATE: {
        if (GetClipboardOwner() == hwnd) {
            return 0;
        }

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            std::string text;
            for (int i = 0; i < 3; ++i) {
                text = Clipped::Clipboard::GetTextUTF8(hwnd);
                if (!text.empty()) break;
                Sleep(5);
            }
            if (!text.empty() && text != g_LastText) {
                g_LastText = text;
                g_LastImgHash = 0;
                g_Database.SaveText(text);
                g_NeedsReload = true;
                if (!g_IsWindowVisible) {
                    g_Database.ReleaseMemory();
                }
            }
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
            Clipped::ImageItem img;
            bool ok = false;
            for (int i = 0; i < 3; ++i) {
                if (Clipped::Clipboard::GetImageAsPNG(hwnd, img, g_LastImgHash)) { ok = true; break; }
                Sleep(5);
            }
            if (ok && img.hash != g_LastImgHash) {
                g_LastImgHash = img.hash;
                g_LastText.clear();
                g_Database.SaveImage(img);
                g_NeedsReload = true;
                if (!g_IsWindowVisible) {
                    img.pngBytes.clear();
                    img.pngBytes.shrink_to_fit();
                    TrimProcessMemory();
                }
            }
        }
        return 0;
    }

    case WM_HOTKEY:
        if (wParam == HOTKEY_ID_TOGGLE) {
            TogglePopupWindow(hwnd);
        }
        return 0;

    case WM_SHOW_EXISTING:
        ShowPopupWindow(hwnd);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            TogglePopupWindow(hwnd);
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayContextMenu(hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TRAY_SHOW:
            ShowPopupWindow(hwnd);
            return 0;
        case IDM_TRAY_CLEAR:
            ReleaseAllThumbnails();
            g_Database.ClearUnpinned();
            g_NeedsReload = true;
            if (!g_IsWindowVisible) {
                TrimProcessMemory();
            }
            return 0;
        case IDM_TRAY_STARTUP:
            SetStartupEnabled(!IsStartupEnabled());
            return 0;
        case IDM_TRAY_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && g_IsWindowVisible) {
            if ((NowMs() - g_OpenedAtMs) > 250) {
                HidePopupWindow(hwnd);
            }
        }
        return 0;

    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int main(int argc, char* argv[]) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ClippedSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingHwnd = FindWindowW(L"ClippedPopupClass", nullptr);
        if (existingHwnd) {
            PostMessageW(existingHwnd, WM_SHOW_EXISTING, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    bool isFirstRun = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--first-run") {
            isFirstRun = true;
        }
    }

    const std::string dbPath = GetDatabasePathUTF8();
    if (GetFileAttributesA(dbPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        isFirstRun = true;
    }

    if (!g_Database.Init(dbPath, 300)) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HICON hAppIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    if (!hAppIcon) {
        hAppIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInst;
    wc.hIcon         = hAppIcon;
    wc.hIconSm       = hAppIcon;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"ClippedPopupClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"Clipped",
        WS_POPUP,
        100, 100, WINDOW_WIDTH, WINDOW_HEIGHT,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    // Esquinas rectas a 90 grados estilo Windows 95 / 98 en Windows 11
    const DWORD cornerPref = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_Database.Close();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // Cargar fuente clásica Tahoma con PixelSnapH (aspecto nítido de Windows 98)
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH  = true;

    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\tahoma.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 15.0f, &fontCfg, io.Fonts->GetGlyphRangesDefault());
    } else if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f, &fontCfg, io.Fonts->GetGlyphRangesDefault());
    } else {
        io.Fonts->AddFontDefault();
    }

    ApplyWin98RetroStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImGui_ImplDX11_CreateDeviceObjects();
    io.Fonts->ClearTexData();

    AddClipboardFormatListener(hwnd);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nullptr, 0);
    RegisterHotKey(hwnd, HOTKEY_ID_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');
    AddTrayIcon(hwnd, isFirstRun);

    TrimProcessMemory();

    bool running = true;
    while (running) {
        if (!g_IsWindowVisible) {
            WaitMessage();
        }

        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;

        if (g_IsWindowVisible) {
            RenderUI(hwnd);
        }
    }

    RemoveTrayIcon(hwnd);
    UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE);
    RemoveClipboardFormatListener(hwnd);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_Database.Close();
    if (hMutex) CloseHandle(hMutex);

    return 0;
}
