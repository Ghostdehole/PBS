#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <powrprof.h>
#include <vector>
#include <algorithm> // for std::clamp
#include <shellapi.h>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Kernel32.lib")

// ================= 常量定义 =================
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };

#define ID_TIMER_DEBOUNCE 1
#define DEBOUNCE_DELAY_MS 400 // 防抖延迟 400ms


std::vector<GUID> g_cachedSchemes;
GUID g_activeScheme = { 0 };

// ================= 辅助函数 =================

// 获取管理员权限状态
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

// 缓存电源方案 (启动时执行一次即可)
void CachePowerSchemes() {
    g_cachedSchemes.clear();
    // 预留空间避免重新分配
    g_cachedSchemes.reserve(10); 
    
    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        LONG ret = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize);
        if (ret != ERROR_SUCCESS) break;
        g_cachedSchemes.push_back(scheme);
        index++;
    }
}

// ================= 核心同步逻辑 =================
void PerformSync() {
    GUID* pActive = nullptr;
    if (PowerGetActiveScheme(nullptr, &pActive) != ERROR_SUCCESS) return;
    g_activeScheme = *pActive;
    LocalFree(pActive);

    DWORD ac = 0, dc = 0;
    // 读取当前亮度
    if (PowerReadACValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &ac) != ERROR_SUCCESS) return;
    if (PowerReadDCValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &dc) != ERROR_SUCCESS) return;


    ac = std::clamp<DWORD>(ac, 0, 100);
    dc = std::clamp<DWORD>(dc, 0, 100);

    // 遍历缓存并写入 (Smart Write)
    for (const auto& scheme : g_cachedSchemes) {
        if (IsEqualGUID(scheme, g_activeScheme)) continue;

        DWORD currentAC = 0, currentDC = 0;
        // 只有值不同时才写入
        if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentAC) == ERROR_SUCCESS) {
            if (currentAC != ac) PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, ac);
        }
        if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentDC) == ERROR_SUCCESS) {
            if (currentDC != dc) PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, dc);
        }
    }
}

// ================= 自启逻辑 =================
void HandleAutoRun(const wchar_t* cmd) {
    if (!cmd || !*cmd) return;

    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;

    if (wcsstr(cmd, L"--onar")) {
        // 静默创建计划任务，以管理员权限运行
        wchar_t taskCmd[MAX_PATH * 3];
        wsprintfW(taskCmd,
            L"schtasks /Create /F /RL HIGHEST /SC ONLOGON /TN \"PowerBrightnessSync\" /TR \"\\\"%s\\\"\"",
            exePath);
        _wsystem(taskCmd);
    }
    else if (wcsstr(cmd, L"--ofar")) {
        // 删除计划任务
        _wsystem(L"schtasks /Delete /F /TN \"PowerBrightnessSync\"");
    }
}

// ================= 窗口过程 (单线程核心) =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            auto pbs = (PPOWERBROADCAST_SETTING)lp;
            if (IsEqualGUID(pbs->PowerSetting, kGuidVideoBrightness)) {
                // 收到亮度变化：重置计时器
                // SetTimer 会自动替换旧的 ID_TIMER_DEBOUNCE，实现防抖
                // 此时不执行任何重操作，瞬间返回
                SetTimer(hwnd, ID_TIMER_DEBOUNCE, DEBOUNCE_DELAY_MS, nullptr);
            }
        }
        return TRUE;

    case WM_TIMER:
        if (wp == ID_TIMER_DEBOUNCE) {
            // 计时器触发，说明用户停止滑动 400ms 了
            KillTimer(hwnd, ID_TIMER_DEBOUNCE);
            PerformSync();
            
            // 再次修剪内存，确保同步后的临时内存被释放
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

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    // === 1. 先处理参数，独立执行自启注册/删除 ===
    if (lpCmdLine && *lpCmdLine) {
        if (wcsstr(lpCmdLine, L"--onar") || wcsstr(lpCmdLine, L"--ofar")) {
            HandleAutoRun(lpCmdLine);
            return 0; // 参数模式执行完毕直接退出
        }
    }

    // === 2. 单例互斥，普通运行时生效 ===
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // 3. 权限检查
    if (!IsAdministrator()) {
        MessageBoxW(nullptr, L"请以管理员身份运行以启用同步。", L"PowerBrightnessSync", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 4. 初始化
    CachePowerSchemes();
    PerformSync(); // 启动时同步一次

    // 5. 创建隐藏窗口
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Lite";
    RegisterClassW(&wc);
    
    // 创建一个 message-only window
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // 6. 注册通知
    HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);

    // 7. 内存优化：主动将工作集修剪到最小
    // 内存回收，只留个空壳在 RAM 里
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // 8. 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hNotify) UnregisterPowerSettingNotification(hNotify);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}