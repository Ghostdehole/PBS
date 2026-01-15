#include <windows.h>
#include <powrprof.h>
#include <algorithm>
#include <string>
#include <shellapi.h>
#include <taskschd.h>
#include <comdef.h>
#include <wrl/client.h>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

// ================= 常量定义 =================
// 视频子组 GUID
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
// 屏幕亮度 GUID
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };
// 显示状态 GUID (用于检测屏幕开关)
constexpr GUID kGuidConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

#define ID_TIMER_DEBOUNCE 1
#define DEBOUNCE_DELAY_MS 600

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
    return isAdmin == TRUE;
}

// ================= 核心同步逻辑 =================
void PerformSync() {
    GUID* pActive = nullptr;
    if (PowerGetActiveScheme(nullptr, &pActive) != ERROR_SUCCESS) return;
    
    // 使用 unique_ptr 确保 pActive 即使在提前返回时也能释放
    std::unique_ptr<GUID, decltype(&LocalFree)> activeGuard(pActive, LocalFree);

    // 1. 获取当前电源状态 (AC还是DC)
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return;
    bool isAC = (sps.ACLineStatus == 1);

    // 2. 获取当前实际生效的亮度值
    DWORD currentBrightness = 50; // 默认值
    DWORD res = ERROR_SUCCESS;
    
    if (isAC) {
        res = PowerReadACValueIndex(nullptr, pActive, &kGuidSubVideo, &kGuidVideoBrightness, &currentBrightness);
    } else {
        res = PowerReadDCValueIndex(nullptr, pActive, &kGuidSubVideo, &kGuidVideoBrightness, &currentBrightness);
    }

    if (res != ERROR_SUCCESS) return;

    // 限制范围
    currentBrightness = std::clamp<DWORD>(currentBrightness, 0, 100);

    // 3. 遍历所有方案，统一将 AC 和 DC 的亮度都设置为当前亮度
    // 这样拔插电源时，亮度不会发生突变
    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize) != ERROR_SUCCESS) {
            break;
        }
        index++;

        // 读取并更新 AC 值
        DWORD tempVal = 0;
        if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &tempVal) == ERROR_SUCCESS) {
            if (tempVal != currentBrightness) {
                PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, currentBrightness);
            }
        }

        // 读取并更新 DC 值
        if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &tempVal) == ERROR_SUCCESS) {
            if (tempVal != currentBrightness) {
                PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, currentBrightness);
            }
        }
    }
    
    // 激活设置确保立即生效（通常不需要，但在某些系统上能强制刷新）
    // PowerSetActiveScheme(nullptr, pActive); 
}

// ================= 自启逻辑 =================
int ManageAutoRun(bool enable) {
    const std::wstring kTaskName = L"PowerBrightnessSync";

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    struct CoUninitializer { ~CoUninitializer() { CoUninitialize(); } } autoCoUninit;

    // 增加异常捕获，防止 _bstr_t 抛出异常
    try {
        hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) return 0;

        ComPtr<ITaskService> pService;
        if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pService)))) return 0;
        if (FAILED(pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) return 0;

        ComPtr<ITaskFolder> pRootFolder;
        if (FAILED(pService->GetFolder(_bstr_t(L"\\"), &pRootFolder))) return 0;

        pRootFolder->DeleteTask(_bstr_t(kTaskName.c_str()), 0);

        if (!enable) return 1;

        ComPtr<ITaskDefinition> pTask;
        if (FAILED(pService->NewTask(0, &pTask))) return 0;

        ComPtr<IPrincipal> pPrincipal;
        if (FAILED(pTask->get_Principal(&pPrincipal))) return 0;
        pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);

        ComPtr<ITaskSettings> pSettings;
        if (FAILED(pTask->get_Settings(&pSettings))) return 0;
        pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
        pSettings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        pSettings->put_Priority(7); // 正常优先级

        ComPtr<ITriggerCollection> pTriggerCollection;
        if (FAILED(pTask->get_Triggers(&pTriggerCollection))) return 0;
        ComPtr<ITrigger> pTrigger;
        if (FAILED(pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger))) return 0;

        wchar_t exePath[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return 0;

        ComPtr<IActionCollection> pActionCollection;
        if (FAILED(pTask->get_Actions(&pActionCollection))) return 0;
        ComPtr<IAction> pAction;
        if (FAILED(pActionCollection->Create(TASK_ACTION_EXEC, &pAction))) return 0;
        ComPtr<IExecAction> pExecAction;
        if (FAILED(pAction->QueryInterface(IID_PPV_ARGS(&pExecAction)))) return 0;
        pExecAction->put_Path(_bstr_t(exePath));

        ComPtr<IRegisteredTask> pRegisteredTask;
        hr = pRootFolder->RegisterTaskDefinition(
            _bstr_t(kTaskName.c_str()), pTask.Get(), TASK_CREATE_OR_UPDATE,
            _variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &pRegisteredTask);

        return SUCCEEDED(hr) ? 1 : 0;
    }
    catch (...) {
        return 0;
    }
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
        // 增加对电源源改变的监听（防止拔插电源瞬间未触发亮度事件导致同步延迟）
        else if (wp == PBT_APMPOWERSTATUSCHANGE) {
            SetTimer(hwnd, ID_TIMER_DEBOUNCE, DEBOUNCE_DELAY_MS, nullptr);
        }
        return TRUE;

    case WM_TIMER:
        if (wp == ID_TIMER_DEBOUNCE) {
            KillTimer(hwnd, ID_TIMER_DEBOUNCE);
            PerformSync();
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
    // 1. 命令行处理
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        bool isSetup = true;
        if (lstrcmpiW(argv[1], L"--onar") == 0) {
            if (IsAdministrator()) {
                ManageAutoRun(true) ? MessageBoxW(0, L"自启动已开启", L"成功", 64) 
                                    : MessageBoxW(0, L"设置失败", L"错误", 16);
            } else {
                MessageBoxW(0, L"需要管理员权限", L"错误", 16);
            }
        } else if (lstrcmpiW(argv[1], L"--ofar") == 0) {
            if (IsAdministrator()) {
                ManageAutoRun(false);
                MessageBoxW(0, L"自启动已关闭", L"成功", 64);
            } else {
                MessageBoxW(0, L"需要管理员权限", L"错误", 16);
            }
        } else {
            isSetup = false;
        }
        if (argv) LocalFree(argv);
        if (isSetup) return 0;
    } else {
        if (argv) LocalFree(argv);
    }

    // 2. 单例检查 (RAII 管理句柄)
    HANDLE hMutexRaw = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance_v3");
    DWORD lastErr = GetLastError();
    
    // 如果互斥体已存在(ERROR_ALREADY_EXISTS) 或 创建失败(NULL)，则退出
    // 注意：如果是我们刚刚创建的，hMutexRaw 不为空且 lastErr 为 0
    if (hMutexRaw == nullptr || lastErr == ERROR_ALREADY_EXISTS) {
        if (hMutexRaw) CloseHandle(hMutexRaw);
        return 0;
    }

    // 使用 unique_ptr 接管，确保 WinMain 退出时自动关闭句柄
    std::unique_ptr<void, decltype(&CloseHandle)> mutexGuard(hMutexRaw, CloseHandle);

    // 3. 运行时检查
    if (!IsAdministrator()) return 0; // 静默退出，因为这是后台进程

    // 4. 初始同步
    PerformSync();

    // 5. 窗口创建
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Host_Class";
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // 6. 注册通知
    HPOWERNOTIFY hN1 = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    HPOWERNOTIFY hN2 = RegisterPowerSettingNotification(hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    // 7. 内存优化
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // 8. 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hN1) UnregisterPowerSettingNotification(hN1);
    if (hN2) UnregisterPowerSettingNotification(hN2);

    return (int)msg.wParam;
}