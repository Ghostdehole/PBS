#include <windows.h>
#include <powrprof.h>
#include <algorithm>
#include <memory>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Kernel32.lib")

constexpr GUID GUID_VIDEO_SUBGROUP = 
    { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };

constexpr GUID GUID_BRIGHTNESS = 
    { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };

constexpr GUID GUID_DISPLAY_STATE = 
    { 0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47} };

#define TIMER_ID 1
#define DEBOUNCE_MS 600

void SyncBrightness() {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS) return;
    
    std::unique_ptr<GUID, decltype(&LocalFree)> guard(active, LocalFree);

    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return;

    DWORD currentBrightness = 50;
    DWORD ret = (sps.ACLineStatus == 1)
        ? PowerReadACValueIndex(nullptr, active, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, &currentBrightness)
        : PowerReadDCValueIndex(nullptr, active, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, &currentBrightness);

    if (ret != ERROR_SUCCESS) return;

    currentBrightness = std::clamp<DWORD>(currentBrightness, 0, 100);

    DWORD idx = 0;
    while (true) {
        GUID scheme;
        DWORD size = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, idx++, (UCHAR*)&scheme, &size) != ERROR_SUCCESS)
            break;

        DWORD val = 0;
        if (PowerReadACValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, &val) == ERROR_SUCCESS) {
            if (val != currentBrightness)
                PowerWriteACValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, currentBrightness);
        }

        if (PowerReadDCValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, &val) == ERROR_SUCCESS) {
            if (val != currentBrightness)
                PowerWriteDCValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP, &GUID_BRIGHTNESS, currentBrightness);
        }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_POWERBROADCAST) {
        bool shouldTrigger = false;
        
        if (w == PBT_APMPOWERSTATUSCHANGE) {
            shouldTrigger = true;
        }
        else if (w == PBT_POWERSETTINGCHANGE && l) {
            auto pbs = (POWERBROADCAST_SETTING*)l;
            if (IsEqualGUID(pbs->PowerSetting, GUID_BRIGHTNESS) || 
                IsEqualGUID(pbs->PowerSetting, GUID_DISPLAY_STATE)) {
                shouldTrigger = true;
            }
        }

        if (shouldTrigger) {
            SetTimer(h, TIMER_ID, DEBOUNCE_MS, nullptr);
        }
        return TRUE;
    }
    
    if (m == WM_TIMER && w == TIMER_ID) {
        KillTimer(h, TIMER_ID);
        SyncBrightness();
        return 0;
    }

    return DefWindowProcW(h, m, w, l);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PBS_Minimal_Lock");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> mtxGuard(hMutex, CloseHandle);

    SyncBrightness();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Core";
    
    if (!RegisterClassW(&wc)) return 0;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    HPOWERNOTIFY hNot1 = RegisterPowerSettingNotification(hwnd, &GUID_BRIGHTNESS, DEVICE_NOTIFY_WINDOW_HANDLE);
    HPOWERNOTIFY hNot2 = RegisterPowerSettingNotification(hwnd, &GUID_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        DispatchMessageW(&msg);
    }

    if (hNot1) UnregisterPowerSettingNotification(hNot1);
    if (hNot2) UnregisterPowerSettingNotification(hNot2);

    return 0;
}