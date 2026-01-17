#include <windows.h>
#include <powrprof.h>
//#include <algorithm>
#include <string>
#include <shellapi.h>
#include <taskschd.h>
#include <comdef.h>
#include <wrl/client.h>
#include <memory>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

// ================= Constants =================
// Video sub-group GUID
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
// Screen brightness GUID
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };
// Display state GUID (used to detect screen on/off)
constexpr GUID kGuidConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

#define ID_TIMER_DEBOUNCE 1
#define DEBOUNCE_DELAY_MS 600

// ================= Helper Functions =================

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

// ================= Core Sync Logic =================
void PerformSync() {
    GUID* pActive = nullptr;
    if (PowerGetActiveScheme(nullptr, &pActive) != ERROR_SUCCESS) return;
    
    // Use unique_ptr to ensure pActive is freed even on early return
    std::unique_ptr<GUID, decltype(&LocalFree)> activeGuard(pActive, LocalFree);

    // Get current power status (AC or DC)
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return;
    bool isAC = (sps.ACLineStatus == 1);

    // Get the currently effective brightness value
    DWORD currentBrightness = 50;
    DWORD res = ERROR_SUCCESS;
    
    if (isAC) {
        res = PowerReadACValueIndex(nullptr, pActive, &kGuidSubVideo, &kGuidVideoBrightness, &currentBrightness);
    } else {
        res = PowerReadDCValueIndex(nullptr, pActive, &kGuidSubVideo, &kGuidVideoBrightness, &currentBrightness);
    }

    if (res != ERROR_SUCCESS) return;

    // Limit range
    //currentBrightness = std::clamp<DWORD>(currentBrightness, 0, 100);

    // Iterate all schemes, unify AC and DC brightness to the current value
    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize) != ERROR_SUCCESS) {
            break;
        }
        index++;

        // Read and update AC value
        DWORD tempVal = 0;
        if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &tempVal) == ERROR_SUCCESS) {
            if (tempVal != currentBrightness) {
                PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, currentBrightness);
            }
        }

        // Read and update DC value
        if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &tempVal) == ERROR_SUCCESS) {
            if (tempVal != currentBrightness) {
                PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, currentBrightness);
            }
        }
    }
}

// ================= Auto-run Logic =================
int ManageAutoRun(bool enable) {
    const std::wstring kTaskName = L"PowerBrightnessSync";

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    struct CoUninitializer { ~CoUninitializer() { CoUninitialize(); } } autoCoUninit;

    // Add exception handling to prevent _bstr_t from throwing
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
        pSettings->put_Priority(7);

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

// ================= Window Procedure =================
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
        // Add listener for power source change (to prevent sync delay when plugging/unplugging power)
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

// ================= Entry Point =================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Command line handling
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        bool isSetup = true;
        if (lstrcmpiW(argv[1], L"--onar") == 0) {
            if (IsAdministrator()) {
                ManageAutoRun(true) ? MessageBoxW(0, L"Auto-start enabled", L"Success", 64) 
                                    : MessageBoxW(0, L"Setup failed", L"Error", 16);
            } else {
                MessageBoxW(0, L"Administrator privileges required", L"Error", 16);
            }
        } else if (lstrcmpiW(argv[1], L"--ofar") == 0) {
            if (IsAdministrator()) {
                ManageAutoRun(false);
                MessageBoxW(0, L"Auto-start disabled", L"Success", 64);
            } else {
                MessageBoxW(0, L"Administrator privileges required", L"Error", 16);
            }
        } else {
            isSetup = false;
        }
        if (argv) LocalFree(argv);
        if (isSetup) return 0;
    } else {
        if (argv) LocalFree(argv);
    }

    // Singleton check (RAII manages handle)
    HANDLE hMutexRaw = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Mutex");
    DWORD lastErr = GetLastError();
    
    // If the mutex already exists (ERROR_ALREADY_EXISTS) or creation fails (NULL), exit
    // Note: if we just created it, hMutexRaw is not null and lastErr is 0
    if (hMutexRaw == nullptr || lastErr == ERROR_ALREADY_EXISTS) {
        if (hMutexRaw) CloseHandle(hMutexRaw);
        return 0;
    }

    // Use unique_ptr to manage handle, ensuring WinMain exit automatically closes it
    std::unique_ptr<void, decltype(&CloseHandle)> mutexGuard(hMutexRaw, CloseHandle);

    // Runtime check
    if (!IsAdministrator()) return 0; // Silent exit because this is a background process

    // Initial sync
    PerformSync();

    // Window creation
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Host_Class";
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // Register notifications
    HPOWERNOTIFY hN1 = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    HPOWERNOTIFY hN2 = RegisterPowerSettingNotification(hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hN1) UnregisterPowerSettingNotification(hN1);
    if (hN2) UnregisterPowerSettingNotification(hN2);

    return (int)msg.wParam;
}