#include <windows.h>
#include <powrprof.h>
#include <vector>
#include <algorithm>
#include <string>
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Shell32.lib")

// ================= 常量定义 =================
// 视频子组 GUID
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
// 屏幕亮度 GUID
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };
// 显示状态 GUID (用于检测屏幕开关)
constexpr GUID kGuidConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

#define ID_TIMER_DEBOUNCE 1
#define DEBOUNCE_DELAY_MS 400

GUID g_activeScheme = { 0 };

// ================= 辅助函数 =================

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

// 辅助执行命令（静默模式），返回是否成功启动进程
bool ExecuteSilent(const std::wstring& parameters) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"open";
    sei.lpFile = L"schtasks.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000); // 等待最长5秒
        
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode); // 获取退出代码
        
        CloseHandle(sei.hProcess);
        return (exitCode == 0); // 只有返回 0 才代表 schtasks 执行成功
    }
    return false;
}

// ================= 核心同步逻辑 =================
void PerformSync() {
    GUID* pActive = nullptr;
    if (PowerGetActiveScheme(nullptr, &pActive) != ERROR_SUCCESS) return;
    g_activeScheme = *pActive;
    
    // 获取当前亮度
    DWORD ac = 0, dc = 0;
    DWORD resAc = PowerReadACValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &ac);
    DWORD resDc = PowerReadDCValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &dc);

    if (resAc != ERROR_SUCCESS && resDc != ERROR_SUCCESS) {
        LocalFree(pActive);
        return;
    }
    
    ac = std::clamp<DWORD>(ac, 0, 100);
    dc = std::clamp<DWORD>(dc, 0, 100);

    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize) != ERROR_SUCCESS) {
            break;
        }
        index++;

        if (IsEqualGUID(scheme, g_activeScheme)) continue;

        DWORD currentAC = 0, currentDC = 0;
        
        if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentAC) == ERROR_SUCCESS) {
            if (currentAC != ac) PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, ac);
        }
        
        if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentDC) == ERROR_SUCCESS) {
            if (currentDC != dc) PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, dc);
        }
    }
    LocalFree(pActive);
}

// ================= 自启逻辑 =================
int ManageAutoRun(bool enable) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return 0;

    wchar_t args[MAX_PATH * 2];
    
    if (enable) {
        swprintf_s(args, MAX_PATH * 2, 
            L"/Create /F /RL HIGHEST /SC ONLOGON /TN \"PowerBrightnessSync\" /TR \"\\\"%s\\\"\"", 
            exePath);
    }
    else {
        swprintf_s(args, MAX_PATH * 2, 
            L"/Delete /F /TN \"PowerBrightnessSync\"");
    }

    return ExecuteSilent(args) ? 1 : 0;
}

// ================= 窗口过程 =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            auto pbs = (PPOWERBROADCAST_SETTING)lp;
            if (IsEqualGUID(pbs->PowerSetting, kGuidVideoBrightness) || 
                IsEqualGUID(pbs->PowerSetting, kGuidConsoleDisplayState)) {
                SetTimer(hwnd, ID_TIMER_DEBOUNCE, DEBOUNCE_DELAY_MS, nullptr);
            }
        }
        return TRUE;

    case WM_TIMER:
        if (wp == ID_TIMER_DEBOUNCE) {
            KillTimer(hwnd, ID_TIMER_DEBOUNCE);
            PerformSync();
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ================= 入口点 =================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isSetupMode = false;

    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--onar") == 0) {
                isSetupMode = true;
                if (!IsAdministrator()) {
                    MessageBoxW(nullptr, L"设置自启动失败：\n需要管理员权限。", L"权限不足", MB_OK | MB_ICONERROR);
                } else {
                    int ret = ManageAutoRun(true); 
                    if (ret == 1) MessageBoxW(nullptr, L"自启动设置成功！", L"提示", MB_OK | MB_ICONINFORMATION);
                    else MessageBoxW(nullptr, L"自启动设置失败！\n请检查任务计划服务是否开启。", L"错误", MB_OK | MB_ICONERROR);
                }
                break;
            }
            else if (lstrcmpiW(argv[i], L"--ofar") == 0) {
                isSetupMode = true;
                if (!IsAdministrator()) {
                    MessageBoxW(nullptr, L"取消自启动失败：\n需要管理员权限。", L"权限不足", MB_OK | MB_ICONERROR);
                } else {
                    ManageAutoRun(false);
                    MessageBoxW(nullptr, L"已取消开机自启动。", L"提示", MB_OK | MB_ICONINFORMATION);
                }
                break;
            }
        }
    }
    if (argv) LocalFree(argv);

    if (isSetupMode) {
        return 0; 
    }
    
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    if (!IsAdministrator()) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    PerformSync();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Lite_Host";
    
    if (!RegisterClassW(&wc)) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    HPOWERNOTIFY hNotifyBrightness = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    HPOWERNOTIFY hNotifyDisplay = RegisterPowerSettingNotification(hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hNotifyBrightness) UnregisterPowerSettingNotification(hNotifyBrightness);
    if (hNotifyDisplay) UnregisterPowerSettingNotification(hNotifyDisplay);
    if (hMutex) CloseHandle(hMutex);

    return (int)msg.wParam;
}
