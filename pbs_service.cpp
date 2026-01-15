/*
 * Windows Brightness Sync Service (Fixed)
 * 
 * 编译命令 (MSVC):
 * cl.exe /std:c++17 /EHsc BrightnessSync.cpp /link /out:BrightnessSync.exe
 */

#include <windows.h>
#include <powrprof.h>
#include <strsafe.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <algorithm>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "User32.lib")

// 定义 GUID 常量
const GUID GUID_VIDEO_SUBGROUP_VAL = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
const GUID GUID_BRIGHTNESS_VAL     = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };
const GUID GUID_DISPLAY_STATE_VAL  = { 0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47} };

#define SVCNAME L"PBS_Service"
#define SVC_DISPLAY_NAME L"Power Brightness Sync Service"
#define SVC_DESCRIPTION  L"Automatically synchronizes screen brightness across all power plans (AC and DC)."
#define DEBOUNCE_MS 800

// RAII Wrapper for HANDLE
struct HandleDeleter {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

// 全局变量
SERVICE_STATUS g_svcStatus{};
SERVICE_STATUS_HANDLE g_svcStatusHandle{};
UniqueHandle g_svcStopEvent;

HANDLE g_timer = nullptr;
std::mutex g_timerMutex; 

HPOWERNOTIFY g_notifyBrightness = nullptr;
HPOWERNOTIFY g_notifyDisplay = nullptr;

std::mutex g_syncMutex;
std::atomic<bool> g_isStopping{ false };

// --- 日志与状态报告 ---

void LogEvent(WORD type, LPCWSTR msg) {
    if (HANDLE h = RegisterEventSourceW(nullptr, SVCNAME)) {
        LPCWSTR strs[1] = { msg };
        ReportEventW(h, type, 0, 0, nullptr, 1, 0, strs, nullptr);
        DeregisterEventSource(h);
    }
}

void ReportStatus(DWORD state, DWORD code, DWORD hint) {
    static DWORD checkpoint = 1;
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwWin32ExitCode = code;
    g_svcStatus.dwWaitHint = hint;
    g_svcStatus.dwControlsAccepted = (state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT);
    g_svcStatus.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : checkpoint++);
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

// --- 核心业务逻辑 (已修复) ---

void SyncBrightness() {
    std::lock_guard<std::mutex> lock(g_syncMutex);
    if (g_isStopping) return;

    GUID* activePtr = nullptr;
    if (PowerGetActiveScheme(nullptr, &activePtr) != ERROR_SUCCESS) {
        return;
    }
    
    std::unique_ptr<GUID, void(*)(GUID*)> activeGuard(activePtr, [](GUID* p) { 
        if(p) LocalFree(p); 
    });

    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) {
        return;
    }

    DWORD curBrightness = 0;
    DWORD ret = ERROR_SUCCESS;
    
    // 获取当前实际生效的亮度值
    if (sps.ACLineStatus == 0) { // Battery
        ret = PowerReadDCValueIndex(nullptr, activePtr, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, &curBrightness);
    } else { // AC or Unknown
        ret = PowerReadACValueIndex(nullptr, activePtr, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, &curBrightness);
    }

    if (ret != ERROR_SUCCESS) return;
    
    curBrightness = std::clamp<DWORD>(curBrightness, 0, 100);

    // 遍历所有方案
    for (DWORD idx = 0;; ++idx) {
        if (g_isStopping) break;

        GUID scheme{};
        DWORD size = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, idx, reinterpret_cast<UCHAR*>(&scheme), &size) != ERROR_SUCCESS) {
            break; 
        }

        // [重要修改]：不要跳过当前 Active Scheme。
        // 我们必须更新当前 Scheme 的 "另一侧" 设置（例如当前是 AC，需更新 DC 设置），
        // 这样拔掉电源时才不会跳变。
        // PowerWrite... 内部检查值相同时不会触发不必要的系统广播，且我们在下面手动做了检查。

        DWORD val = 0;
        
        // 1. 同步 AC 设置
        if (PowerReadACValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, &val) == ERROR_SUCCESS) {
            if (val != curBrightness) {
                PowerWriteACValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, curBrightness);
                // 只有写入了新值，才可能触发下一次事件，但下一次读到的值将等于 curBrightness，循环终止。
            }
        }
        
        // 2. 同步 DC 设置
        if (PowerReadDCValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, &val) == ERROR_SUCCESS) {
            if (val != curBrightness) {
                PowerWriteDCValueIndex(nullptr, &scheme, &GUID_VIDEO_SUBGROUP_VAL, &GUID_BRIGHTNESS_VAL, curBrightness);
            }
        }
    }
    
    // 应用更改 (这通常对于写入当前激活方案是必要的，让设置立即生效，尽管对于"另一侧"电源设置不是必须的)
    PowerSetActiveScheme(nullptr, activePtr);
}

// --- 定时器与回调 ---

VOID CALLBACK TimerCallback(PVOID, BOOLEAN) {
    if (g_isStopping) return;
    SyncBrightness();
}

void TriggerDebounce() {
    std::lock_guard<std::mutex> lock(g_timerMutex); 
    if (g_isStopping) return;

    if (!g_timer) {
        if (!CreateTimerQueueTimer(&g_timer, nullptr, TimerCallback, nullptr, DEBOUNCE_MS, 0, WT_EXECUTEDEFAULT)) {
            LogEvent(EVENTLOG_ERROR_TYPE, L"CreateTimerQueueTimer failed");
        }
    } else {
        ChangeTimerQueueTimer(nullptr, g_timer, DEBOUNCE_MS, 0);
    }
}

// --- 服务控制处理 ---

DWORD WINAPI SvcCtrl(DWORD ctrl, DWORD ev, LPVOID data, LPVOID) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, 0, 2000);
        g_isStopping = true;
        if (g_svcStopEvent) SetEvent(g_svcStopEvent.get());
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        if (g_isStopping) return NO_ERROR;
        
        if (ev == PBT_APMPOWERSTATUSCHANGE) {
             TriggerDebounce();
        }
        else if (ev == PBT_POWERSETTINGCHANGE && data) {
            POWERBROADCAST_SETTING* pbs = reinterpret_cast<POWERBROADCAST_SETTING*>(data);
            if (IsEqualGUID(pbs->PowerSetting, GUID_BRIGHTNESS_VAL) ||
                IsEqualGUID(pbs->PowerSetting, GUID_DISPLAY_STATE_VAL)) {
                TriggerDebounce();
            }
        }
        return NO_ERROR;
    default: // 必须处理其他控制码
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// --- 主入口 (大部分保持不变) ---
void WINAPI SvcMain(DWORD, LPWSTR*) {
    g_svcStatusHandle = RegisterServiceCtrlHandlerExW(SVCNAME, SvcCtrl, nullptr);
    if (!g_svcStatusHandle) return;

    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ReportStatus(SERVICE_START_PENDING, 0, 3000);

    g_svcStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    g_notifyBrightness = RegisterPowerSettingNotification(g_svcStatusHandle, &GUID_BRIGHTNESS_VAL, DEVICE_NOTIFY_SERVICE_HANDLE);
    g_notifyDisplay = RegisterPowerSettingNotification(g_svcStatusHandle, &GUID_DISPLAY_STATE_VAL, DEVICE_NOTIFY_SERVICE_HANDLE);

    ReportStatus(SERVICE_RUNNING, 0, 0);
    LogEvent(EVENTLOG_INFORMATION_TYPE, L"PBS Service Started");

    SyncBrightness(); 

    WaitForSingleObject(g_svcStopEvent.get(), INFINITE);

    g_isStopping = true;
    ReportStatus(SERVICE_STOP_PENDING, 0, 1000);

    {
        std::lock_guard<std::mutex> lock(g_timerMutex);
        if (g_timer) {
            DeleteTimerQueueTimer(nullptr, g_timer, INVALID_HANDLE_VALUE);
            g_timer = nullptr;
        }
    }

    if (g_notifyBrightness) UnregisterPowerSettingNotification(g_notifyBrightness);
    if (g_notifyDisplay) UnregisterPowerSettingNotification(g_notifyDisplay);

    ReportStatus(SERVICE_STOPPED, 0, 0);
}

// 安装部分代码保持不变...
void InstallService(bool install) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, install ? SC_MANAGER_CREATE_SERVICE : SC_MANAGER_CONNECT);
    if (!scm) {
        wprintf(L"Run as Administrator.\n");
        return;
    }
    // ... (其余安装代码同原版)
    if (install) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quotedPath = L"\""; quotedPath += path; quotedPath += L"\"";
        
        SC_HANDLE svc = CreateServiceW(scm, SVCNAME, SVC_DISPLAY_NAME, SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            quotedPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        
        if(svc) {
            SERVICE_DESCRIPTIONW sd = { (LPWSTR)SVC_DESCRIPTION };
            ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);
            wprintf(L"Installed. Run 'sc start PBS_Service' to start.\n");
            CloseServiceHandle(svc);
        } else {
             wprintf(L"CreateService failed: %d\n", GetLastError());
        }
    } else {
        SC_HANDLE svc = OpenServiceW(scm, SVCNAME, SERVICE_STOP | DELETE);
        if(svc) {
            SERVICE_STATUS ss; ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            DeleteService(svc);
            wprintf(L"Removed.\n");
            CloseServiceHandle(svc);
        }
    }
    CloseServiceHandle(scm);
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        if (_wcsicmp(argv[1], L"--install") == 0 || _wcsicmp(argv[1], L"-i") == 0) {
            InstallService(true); return 0;
        }
        if (_wcsicmp(argv[1], L"--remove") == 0 || _wcsicmp(argv[1], L"-u") == 0) {
            InstallService(false); return 0;
        }
    }
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVCNAME, SvcMain }, { nullptr, nullptr } };
    StartServiceCtrlDispatcherW(table);
    return 0;
}