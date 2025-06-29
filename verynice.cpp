#define UNICODE
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>

#pragma comment(lib, "Comctl32.lib")

const int FRAME_DELAY_MS = 16;
const int THROW_THRESHOLD = 5;

// Default physics constants, adjustable in UI
float g_Friction = 0.99f;      // horizontal velocity damping
float g_BounceDamping = 0.7f;  // velocity damping on bounce

struct Vec2 { float x = 0, y = 0; };

struct WindowPhysics {
    Vec2 velocity = { 0.0f, 0.0f };
    Vec2 lastPos = { 0.0f, 0.0f };
    bool dragging = false;
    float weight = 1.0f;
};

std::map<HWND, WindowPhysics> windowStates;
bool g_Running = true;
bool g_CollisionsEnabled = true;
float g_Gravity = 1.0f;
float g_WeightScale = 1.0f;

HWND g_MainWnd = nullptr;
HWND g_GravitySlider = nullptr;
HWND g_CollisionCheckbox = nullptr;
HWND g_WeightSlider = nullptr;
HWND g_FrictionSlider = nullptr;
HWND g_BounceSlider = nullptr;
HWND g_WeightLabel = nullptr;
HWND g_FrictionLabel = nullptr;
HWND g_BounceLabel = nullptr;
HWND g_RecCenterButton = nullptr;
HWND g_QuitButton = nullptr;
HWND g_Tooltip = nullptr;

HINSTANCE g_hInstance;
RECT g_WorkArea = {};

#define IDC_GRAVITY_SLIDER 101
#define IDC_COLLISION_CHECKBOX 102
#define IDC_WEIGHT_SLIDER 103
#define IDC_RECENTER_BUTTON 104
#define IDC_QUIT_BUTTON 105
#define IDC_FRICTION_SLIDER 106
#define IDC_BOUNCE_SLIDER 107

// -- Config handling --

void SaveConfig() {
    std::ofstream out("config.ini");
    out << "collisions=" << (g_CollisionsEnabled ? "1" : "0") << "\n";
    out << "gravity=" << g_Gravity << "\n";
    out << "weightscale=" << g_WeightScale << "\n";
    out << "friction=" << g_Friction << "\n";
    out << "bounce=" << g_BounceDamping << "\n";
}

void SaveDefaultConfig() {
    std::ofstream out("config.ini");
    out << "collisions=1\n";
    out << "gravity=1.0\n";
    out << "weightscale=1.0\n";
    out << "friction=0.99\n";
    out << "bounce=0.7\n";
}

void LoadConfig() {
    std::ifstream in("config.ini");
    if (!in) {
        SaveDefaultConfig();
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("collisions=") == 0)
            g_CollisionsEnabled = (line.substr(11) == "1");
        else if (line.find("gravity=") == 0)
            g_Gravity = std::stof(line.substr(8));
        else if (line.find("weightscale=") == 0)
            g_WeightScale = std::stof(line.substr(12));
        else if (line.find("friction=") == 0)
            g_Friction = std::stof(line.substr(9));
        else if (line.find("bounce=") == 0)
            g_BounceDamping = std::stof(line.substr(7));
    }
}

// -- Tooltip helper --

void AddToolTip(HWND hwndParent, HWND hwndCtrl, const wchar_t* text) {
    if (!g_Tooltip) {
        g_Tooltip = CreateWindowEx(0, TOOLTIPS_CLASS, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwndParent, nullptr, g_hInstance, nullptr);
        SetWindowPos(g_Tooltip, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    TOOLINFO ti = { sizeof(ti) };
    ti.hwnd = hwndParent;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.uId = (UINT_PTR)hwndCtrl;
    ti.lpszText = (LPWSTR)text;
    SendMessage(g_Tooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
}

// -- Window helpers --

std::vector<HWND> GetWindows() {
    std::vector<HWND> out;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == g_MainWnd) return TRUE;
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId())
            ((std::vector<HWND>*)param)->push_back(hwnd);
        return TRUE;
    }, (LPARAM)&out);
    return out;
}

bool IsDragging(HWND hwnd, const RECT& r) {
    POINT pt;
    GetCursorPos(&pt);
    return GetAsyncKeyState(VK_LBUTTON) < 0 &&
        pt.x >= r.left && pt.x <= r.right &&
        pt.y >= r.top && pt.y <= r.bottom;
}

void CenterWindows() {
    auto wins = GetWindows();
    int midX = (g_WorkArea.left + g_WorkArea.right) / 2;
    int midY = (g_WorkArea.top + g_WorkArea.bottom) / 2;
    for (HWND hwnd : wins) {
        RECT r;
        if (!GetWindowRect(hwnd, &r)) continue;
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, midX - w / 2, midY - h / 2, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        auto& phys = windowStates[hwnd];
        phys.velocity = { 0, 0 };
        phys.lastPos = { (float)(midX - w / 2), (float)(midY - h / 2) };
    }
}

void AssignWeights() {
    auto wins = GetWindows();
    for (HWND hwnd : wins) {
        RECT r;
        if (!GetWindowRect(hwnd, &r)) continue;
        int area = (r.right - r.left) * (r.bottom - r.top);
        float baseWeight = 1.0f + (float)area / 200000.0f; // scaled
        windowStates[hwnd].weight = baseWeight * g_WeightScale;
    }
}

// -- Physics loop --

void PhysicsLoop() {
    SystemParametersInfo(SPI_GETWORKAREA, 0, &g_WorkArea, 0);
    AssignWeights();

    while (g_Running) {
        auto wins = GetWindows();

        for (HWND hwnd : wins) {
            RECT r;
            if (!GetWindowRect(hwnd, &r)) continue;

            int w = r.right - r.left;
            int h = r.bottom - r.top;
            Vec2 curPos = { (float)r.left, (float)r.top };
            auto& state = windowStates[hwnd];

            if (IsDragging(hwnd, r)) {
                Vec2 delta = { curPos.x - state.lastPos.x, curPos.y - state.lastPos.y };
                if (fabs(delta.x) > THROW_THRESHOLD || fabs(delta.y) > THROW_THRESHOLD)
                    state.velocity = delta;
                else state.velocity = { 0, 0 };
                state.lastPos = curPos;
                continue;
            }

            float invWeight = 1.0f / state.weight;

            // Gravity & inertia scaled by inverse weight
            state.velocity.y += g_Gravity * invWeight;
            state.velocity.x *= g_Friction; // Use friction slider
            state.velocity.y *= g_Friction;

            Vec2 newPos = { curPos.x + state.velocity.x * invWeight, curPos.y + state.velocity.y * invWeight };

            // Bounce on edges (no top boundary)
            if (newPos.x < g_WorkArea.left) {
                newPos.x = (float)g_WorkArea.left;
                state.velocity.x *= -g_BounceDamping; // Use bounce slider
            }
            if (newPos.x + w > g_WorkArea.right) {
                newPos.x = (float)(g_WorkArea.right - w);
                state.velocity.x *= -g_BounceDamping;
            }
            if (newPos.y + h > g_WorkArea.bottom) {
                newPos.y = (float)(g_WorkArea.bottom - h);
                state.velocity.y *= -g_BounceDamping;
                if (fabs(state.velocity.y) < 1.0f) state.velocity.y = 0;
            }

            // Collisions between windows
            if (g_CollisionsEnabled) {
                for (HWND other : wins) {
                    if (other == hwnd) continue;
                    RECT or_;
                    if (!GetWindowRect(other, &or_)) continue;

                    RECT thisRect = {
                        (LONG)newPos.x,
                        (LONG)newPos.y,
                        (LONG)(newPos.x + w),
                        (LONG)(newPos.y + h)
                    };
                    RECT intersect;
                    if (IntersectRect(&intersect, &thisRect, &or_)) {
                        // Basic collision response: invert velocity with damping
                        state.velocity.x *= -g_BounceDamping;
                        state.velocity.y *= -g_BounceDamping;
                    }
                }
            }

            SetWindowPos(hwnd, nullptr, (int)newPos.x, (int)newPos.y, 0, 0,
                SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);

            state.lastPos = newPos;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_DELAY_MS));
    }
}

// -- Window proc --

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_COLLISION_CHECKBOX:
            g_CollisionsEnabled = (SendMessage(g_CollisionCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveConfig();
            break;
        case IDC_QUIT_BUTTON:
            g_Running = false;
            PostQuitMessage(0);
            break;
        case IDC_RECENTER_BUTTON:
            CenterWindows();
            AssignWeights();
            break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lParam == g_GravitySlider) {
            int pos = SendMessage(g_GravitySlider, TBM_GETPOS, 0, 0);
            g_Gravity = pos / 10.0f;
            SaveConfig();
        }
        else if ((HWND)lParam == g_WeightSlider) {
            int pos = SendMessage(g_WeightSlider, TBM_GETPOS, 0, 0);
            g_WeightScale = pos / 100.0f;
            AssignWeights();
            SaveConfig();
        }
        else if ((HWND)lParam == g_FrictionSlider) {
            int pos = SendMessage(g_FrictionSlider, TBM_GETPOS, 0, 0);
            g_Friction = pos / 100.0f;
            SaveConfig();
        }
        else if ((HWND)lParam == g_BounceSlider) {
            int pos = SendMessage(g_BounceSlider, TBM_GETPOS, 0, 0);
            g_BounceDamping = pos / 100.0f;
            SaveConfig();
        }
        break;
    case WM_DESTROY:
        g_Running = false;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// -- UI --

void CreateUI(HWND hwnd) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Gravity Label + Slider
    CreateWindow(L"STATIC", L"Gravity (0.0 - 5.0):", WS_CHILD | WS_VISIBLE,
        20, 20, 180, 20, hwnd, nullptr, g_hInstance, nullptr);

    g_GravitySlider = CreateWindow(TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        20, 45, 300, 30, hwnd, (HMENU)IDC_GRAVITY_SLIDER, g_hInstance, nullptr);
    SendMessage(g_GravitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 50));
    SendMessage(g_GravitySlider, TBM_SETPOS, TRUE, (int)(g_Gravity * 10));
    SendMessage(g_GravitySlider, TBM_SETTICFREQ, 5, 0);
    AddToolTip(hwnd, g_GravitySlider, L"Adjust the gravity strength");

    // Collision checkbox
    g_CollisionCheckbox = CreateWindow(L"BUTTON", L"Enable Window Collisions",
        WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
        20, 90, 200, 25, hwnd, (HMENU)IDC_COLLISION_CHECKBOX, g_hInstance, nullptr);
    SendMessage(g_CollisionCheckbox, BM_SETCHECK, g_CollisionsEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    AddToolTip(hwnd, g_CollisionCheckbox, L"Toggle collision between windows");

    // Weight scale label + slider
    CreateWindow(L"STATIC", L"Weight Scale (0.1 - 2.0):", WS_CHILD | WS_VISIBLE,
        20, 125, 180, 20, hwnd, nullptr, g_hInstance, nullptr);

    g_WeightSlider = CreateWindow(TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        20, 150, 300, 30, hwnd, (HMENU)IDC_WEIGHT_SLIDER, g_hInstance, nullptr);
    SendMessage(g_WeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 200));
    SendMessage(g_WeightSlider, TBM_SETPOS, TRUE, (int)(g_WeightScale * 100));
    SendMessage(g_WeightSlider, TBM_SETTICFREQ, 10, 0);
    AddToolTip(hwnd, g_WeightSlider, L"Scale weight of windows by size");

    // Friction label + slider
    CreateWindow(L"STATIC", L"Friction (0.5 - 1.0):", WS_CHILD | WS_VISIBLE,
        20, 185, 180, 20, hwnd, nullptr, g_hInstance, nullptr);

    g_FrictionSlider = CreateWindow(TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        20, 210, 300, 30, hwnd, (HMENU)IDC_FRICTION_SLIDER, g_hInstance, nullptr);
    SendMessage(g_FrictionSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 100));
    SendMessage(g_FrictionSlider, TBM_SETPOS, TRUE, (int)(g_Friction * 100));
    SendMessage(g_FrictionSlider, TBM_SETTICFREQ, 5, 0);
    AddToolTip(hwnd, g_FrictionSlider, L"Velocity damping factor");

    // Bounce label + slider
    CreateWindow(L"STATIC", L"Bounce Damping (0.1 - 1.0):", WS_CHILD | WS_VISIBLE,
        20, 245, 180, 20, hwnd, nullptr, g_hInstance, nullptr);

    g_BounceSlider = CreateWindow(TRACKBAR_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        20, 270, 300, 30, hwnd, (HMENU)IDC_BOUNCE_SLIDER, g_hInstance, nullptr);
    SendMessage(g_BounceSlider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendMessage(g_BounceSlider, TBM_SETPOS, TRUE, (int)(g_BounceDamping * 100));
    SendMessage(g_BounceSlider, TBM_SETTICFREQ, 5, 0);
    AddToolTip(hwnd, g_BounceSlider, L"Damping factor on bounce");

    // Recenter button
    g_RecCenterButton = CreateWindow(L"BUTTON", L"Recenter Windows",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 310, 150, 30, hwnd, (HMENU)IDC_RECENTER_BUTTON, g_hInstance, nullptr);
    AddToolTip(hwnd, g_RecCenterButton, L"Move all windows to screen center");

    // Quit button
    g_QuitButton = CreateWindow(L"BUTTON", L"Quit",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        200, 310, 120, 30, hwnd, (HMENU)IDC_QUIT_BUTTON, g_hInstance, nullptr);
    AddToolTip(hwnd, g_QuitButton, L"Exit this program");

    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);
}

// -- WinMain --

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInstance = hInstance;
    LoadConfig();
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PhysicsControllerWndClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    g_MainWnd = CreateWindow(wc.lpszClassName, L"Window Physics Controller",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 360, 400, nullptr, nullptr, hInstance, nullptr);
    if (!g_MainWnd) return 0;

    CreateUI(g_MainWnd);

    ShowWindow(g_MainWnd, nCmdShow);

    // Start physics thread
    std::thread physicsThread(PhysicsLoop);
    physicsThread.detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_Running = false;
    return 0;
}
