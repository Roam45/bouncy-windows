#define UNICODE
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
#include <map>
#include <cmath>

const float GRAVITY = 1.0f;
const float DAMPING = 0.7f;
const float SIDE_DAMPING = 0.99f;
const int FRAME_DELAY_MS = 16;
const int THROW_THRESHOLD = 5;

struct Vec2 {
    float x = 0, y = 0;
};

struct WindowPhysics {
    Vec2 velocity = { 0.0f, 0.0f };
    Vec2 lastPos = { 0, 0 };
    bool dragging = false;
};

std::map<HWND, WindowPhysics> windowStates;

bool IsDragging(HWND hwnd, const RECT& r) {
    WINDOWINFO info = { sizeof(info) };
    if (!GetWindowInfo(hwnd, &info)) return false;

    POINT cursor;
    GetCursorPos(&cursor);

    // Check if mouse is over window and button held
    if (cursor.x >= r.left && cursor.x <= r.right &&
        cursor.y >= r.top && cursor.y <= r.bottom &&
        GetAsyncKeyState(VK_LBUTTON)) {
        return true;
    }
    return false;
}

std::vector<HWND> GetTopLevelWindows() {
    std::vector<HWND> windows;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) {
                ((std::vector<HWND>*)lParam)->push_back(hwnd);
            }
        }
        return TRUE;
    }, (LPARAM)&windows);
    return windows;
}

void ApplyGravityAndPhysics() {
    RECT screenRect;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);

    while (true) {
        auto windows = GetTopLevelWindows();

        for (HWND hwnd : windows) {
            RECT rect;
            if (!GetWindowRect(hwnd, &rect)) continue;

            int winWidth = rect.right - rect.left;
            int winHeight = rect.bottom - rect.top;

            auto& phys = windowStates[hwnd];

            Vec2 curPos = { (float)rect.left, (float)rect.top };

            // Detect manual dragging
            bool dragging = IsDragging(hwnd, rect);
            phys.dragging = dragging;

            if (dragging) {
                // Calculate throw velocity if fast mouse move
                float dx = curPos.x - phys.lastPos.x;
                float dy = curPos.y - phys.lastPos.y;
                if (fabs(dx) > THROW_THRESHOLD || fabs(dy) > THROW_THRESHOLD) {
                    phys.velocity.x = dx;
                    phys.velocity.y = dy;
                } else {
                    phys.velocity = { 0, 0 };
                }
                phys.lastPos = curPos;
                continue;
            }

            // Gravity + inertia
            phys.velocity.y += GRAVITY;
            phys.velocity.x *= SIDE_DAMPING;

            // Apply position
            Vec2 newPos = {
                curPos.x + phys.velocity.x,
                curPos.y + phys.velocity.y
            };

            // Bounce on bottom
            if (newPos.y + winHeight >= screenRect.bottom) {
                newPos.y = screenRect.bottom - winHeight;
                phys.velocity.y *= -DAMPING;
                if (fabs(phys.velocity.y) < 1.0f) phys.velocity.y = 0;
            }

            // Bounce left/right
            if (newPos.x <= screenRect.left) {
                newPos.x = screenRect.left;
                phys.velocity.x *= -DAMPING;
            }
            if (newPos.x + winWidth >= screenRect.right) {
                newPos.x = screenRect.right - winWidth;
                phys.velocity.x *= -DAMPING;
            }

            SetWindowPos(hwnd, nullptr, (int)newPos.x, (int)newPos.y, 0, 0,
                         SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);

            phys.lastPos = newPos;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_DELAY_MS));
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {
    std::thread physicsThread(ApplyGravityAndPhysics);
    physicsThread.join();
    return 0;
}
