#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <chrono>
#include <cstdint>
#include <dwmapi.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

constexpr wchar_t PROCESS_NAME[] = L"apogea.exe";
constexpr wchar_t MODULE_NAME[] = L"GameAssembly.dll";
constexpr uintptr_t BASE_OFFSET = 0x02537240;
constexpr uintptr_t OFFSETS[] = { 0x98, 0x18, 0x20, 0x40, 0xB8, 0x8, 0xE8 };
constexpr int OFFSET_COUNT = 7;

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwndOverlay = nullptr;
static HANDLE g_hProcess = nullptr;

bool showMenu = true;
bool running = false;
bool paused = false;

uint32_t currentXP = 0;
uint32_t startXP = 0;
std::chrono::steady_clock::time_point startTime;
std::chrono::steady_clock::time_point pauseTime;
double pausedSeconds = 0.0;

uintptr_t GetModuleBase(DWORD pid, const wchar_t* modName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W modEntry{ sizeof(modEntry) };
    if (Module32FirstW(hSnap, &modEntry)) {
        do {
            if (!_wcsicmp(modEntry.szModule, modName)) {
                CloseHandle(hSnap);
                return (uintptr_t)modEntry.modBaseAddr;
            }
        } while (Module32NextW(hSnap, &modEntry));
    }
    CloseHandle(hSnap);
    return 0;
}

uint32_t ReadXPFromProcess() {
    if (!g_hProcess) return 0;
    PROCESSENTRY32W entry{ sizeof(entry) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do { if (!_wcsicmp(entry.szExeFile, PROCESS_NAME)) { pid = entry.th32ProcessID; break; } } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    if (!pid) return 0;

    uintptr_t base = GetModuleBase(pid, MODULE_NAME);
    if (!base) return 0;

    uintptr_t addr = 0;

    if (!ReadProcessMemory(g_hProcess, (LPCVOID)(base + BASE_OFFSET), &addr, sizeof(addr), nullptr)) return 0;


    for (int i = 0; i < OFFSET_COUNT - 1; i++) {
        addr += OFFSETS[i];
        if (!ReadProcessMemory(g_hProcess, (LPCVOID)addr, &addr, sizeof(addr), nullptr)) return 0;
    }

    uint32_t val = 0;
    ReadProcessMemory(g_hProcess, (LPCVOID)(addr + OFFSETS[OFFSET_COUNT - 1]), &val, sizeof(val), nullptr);
    return val;
}

void ApplyCustomTheme() {
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.WindowBorderSize = 1.0f;

    auto& c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.05f, 0.18f, 0.94f); 
    c[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.15f, 0.55f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.85f, 0.35f, 0.85f, 0.5f);
    c[ImGuiCol_Button] = ImVec4(0.75f, 0.25f, 0.75f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.40f, 0.95f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.15f, 0.60f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(1.00f, 0.90f, 1.00f, 1.00f);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {

    PROCESSENTRY32W pe{ sizeof(pe) };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD gamePid = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do { if (!_wcsicmp(pe.szExeFile, PROCESS_NAME)) { gamePid = pe.th32ProcessID; break; } } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    if (gamePid) g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, gamePid);

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"ApogeaOv", nullptr };
    RegisterClassEx(&wc);
    g_hwndOverlay = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"Apogea Analyzer", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, hInstance, nullptr);

    SetLayeredWindowAttributes(g_hwndOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(g_hwndOverlay, &margins);
    ShowWindow(g_hwndOverlay, SW_SHOWDEFAULT);


    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwndOverlay; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pd3dDeviceContext);

    ID3D11Texture2D* pBackBuffer; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView); pBackBuffer->Release();

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_hwndOverlay);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    ApplyCustomTheme();

    while (true) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) return 0;
        }

       
        currentXP = ReadXPFromProcess();

       
        if (GetAsyncKeyState(VK_INSERT) & 1) showMenu = !showMenu;

        SetWindowLong(g_hwndOverlay, GWL_EXSTYLE, showMenu ? (GetWindowLong(g_hwndOverlay, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT) : (GetWindowLong(g_hwndOverlay, GWL_EXSTYLE) | WS_EX_TRANSPARENT));

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();

        if (showMenu) {
            ImGui::Begin("Apogea Analyzer", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 1.0f, 1.0f), "XP Live: %u", currentXP);
            ImGui::Separator();

            if (ImGui::Button("Start Hunt")) {
                running = true; paused = false; pausedSeconds = 0;
                startXP = currentXP; startTime = std::chrono::steady_clock::now();
            }
            ImGui::SameLine();
            if (ImGui::Button(paused ? "Resume" : "Pause")) {
                if (running) {
                    paused = !paused;
                    if (paused) pauseTime = std::chrono::steady_clock::now();
                    else pausedSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseTime).count();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) { running = false; paused = false; }

            if (running) {
                double elapsed = (paused) ? std::chrono::duration<double>(pauseTime - startTime).count() - pausedSeconds
                    : std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() - pausedSeconds;
                uint32_t gained = (currentXP >= startXP) ? (currentXP - startXP) : 0;
                double xph = (elapsed > 1.5) ? (gained / elapsed) * 3600.0 : 0.0;

                ImGui::Text("Status: %s", paused ? "PAUSED" : "HUNTING...");
                ImGui::Text("Time: %.1fs", elapsed > 0 ? elapsed : 0.0);
                ImGui::Text("Gained: %u", gained);
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "XP/h: %.0f", xph);
            }
            else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Status: IDLE");
            }
            ImGui::End();
        }

        const float clear[4] = { 0, 0, 0, 0 };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }
    return 0;
}