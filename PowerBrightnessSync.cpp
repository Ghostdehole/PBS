#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
#include <guiddef.h>
#include <versionhelpers.h>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <algorithm>

#pragma comment(lib, "PowrProf.lib")
namespace fs = std::filesystem;

// ================= GUID =================
constexpr GUID kGuidSubVideo = {0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99}};
constexpr GUID kGuidVideoBrightness = {0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb}};

// ================= 日志类 =================
class Logger {
public:
    Logger(size_t maxSize = 50*1024*1024) : maxSize(maxSize), stopFlag(false) {
        if(!InitLogDir()) {
            MessageBoxA(nullptr, "Failed to create log directory.", "Error", MB_OK | MB_ICONERROR);
            return;
        }
        logThread = std::thread([this]{ ProcessQueue(); });
    }

    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stopFlag = true;
            cv.notify_all();
        }
        if (logThread.joinable()) logThread.join();
    }

    void Log(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n < 0 || n >= (int)sizeof(buf)) strncpy(buf, "Log message truncated", sizeof(buf)-1);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[600];
        int m = snprintf(line, sizeof(line),
                         "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond, buf);
        if (m < 0 || m >= (int)sizeof(line)) return;

        std::lock_guard<std::mutex> lock(mtx);
        queue.push(std::string(line));
        cv.notify_one();
    }

private:
    std::string logDir;
    std::string logPath;
    size_t maxSize;
    std::queue<std::string> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopFlag;
    std::thread logThread;

    bool InitLogDir() {
        char localAppData[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) return false;
        logDir = std::string(localAppData) + "\\PowerBrightnessSync\\Logs";
        if(!fs::exists(logDir)) {
            if(!fs::create_directories(logDir)) return false;
        }
        UpdateCurrentLogPath();
        return true;
    }

    void UpdateCurrentLogPath() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char fname[128];
        snprintf(fname, sizeof(fname), "PowerBrightnessSync_%04d-%02d-%02d.log",
                 st.wYear, st.wMonth, st.wDay);
        logPath = logDir + "\\" + fname;
    }

    void CleanupOldLogs() {
        if (logDir.empty() || !fs::exists(logDir)) return;
        std::vector<fs::directory_entry> logs;
        size_t totalSize = 0;
        for (auto& e : fs::directory_iterator(logDir)) {
            if (e.is_regular_file()) {
                logs.push_back(e);
                totalSize += e.file_size();
            }
        }
        if (totalSize <= maxSize) return;
        std::sort(logs.begin(), logs.end(), [](const fs::directory_entry& a, const fs::directory_entry& b){
            return fs::last_write_time(a) < fs::last_write_time(b);
        });
        for (auto& f : logs) {
            if (totalSize <= maxSize) break;
            size_t sz = f.file_size();
            fs::remove(f.path());
            totalSize -= sz;
        }
    }

    void ProcessQueue() {
        while(true) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]{ return stopFlag || !queue.empty(); });
            if (stopFlag && queue.empty()) break;

            std::queue<std::string> localQueue;
            std::swap(localQueue, queue);
            lock.unlock();

            std::string currentPath = logPath;
            FILE* fp = fopen(currentPath.c_str(), "a");
            if (fp) {
                while (!localQueue.empty()) {
                    fputs(localQueue.front().c_str(), fp);
                    localQueue.pop();
                }
                fclose(fp);
            }

            UpdateCurrentLogPath();
            CleanupOldLogs();
        }
    }
} g_logger;

// ================= 管理员检测 =================
bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

// ================= 功能函数 =================
struct Brightness { DWORD ac=0, dc=0; };

bool GetActiveSchemeGUID(GUID& out) {
    GUID* pGuid = nullptr;
    if (PowerGetActiveScheme(nullptr, &pGuid) != ERROR_SUCCESS) {
        g_logger.Log("PowerGetActiveScheme failed. Error=%lu", GetLastError());
        return false;
    }
    out = *pGuid;
    LocalFree(pGuid);
    return true;
}

bool ReadBrightness(const GUID& scheme, Brightness& b) {
    if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &b.ac) != ERROR_SUCCESS) {
        g_logger.Log("PowerReadACValueIndex failed. Error=%lu", GetLastError());
        return false;
    }
    if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &b.dc) != ERROR_SUCCESS) {
        g_logger.Log("PowerReadDCValueIndex failed. Error=%lu", GetLastError());
        return false;
    }

    DWORD oldAC = b.ac, oldDC = b.dc;
    b.ac = std::clamp(b.ac, 0u, 100u);
    b.dc = std::clamp(b.dc, 0u, 100u);
    if(oldAC != b.ac || oldDC != b.dc)
        g_logger.Log("Brightness values adjusted: AC %lu->%lu, DC %lu->%lu", oldAC, b.ac, oldDC, b.dc);

    return true;
}

void WriteBrightnessWithRetry(GUID& scheme, const Brightness& b, DWORD index) {
    const int maxRetry = 2;
    for(int attempt=0; attempt<=maxRetry; ++attempt) {
        LONG retAC = PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, b.ac);
        LONG retDC = PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, b.dc);
        if(retAC == ERROR_SUCCESS && retDC == ERROR_SUCCESS) break;

        g_logger.Log("Attempt %d: Write AC=%ld, DC=%ld failed, refreshing scheme GUID", attempt, retAC, retDC);
        GUID refreshed;
        if(GetActiveSchemeGUID(refreshed)) scheme = refreshed;
        Sleep(50);
    }
}

void SyncSchemeBrightness(const GUID& activeScheme, const Brightness& b) {
    DWORD index = 0;
    while(true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        LONG ret = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize);
        if(ret == ERROR_NO_MORE_ITEMS) break;
        else if(ret == ERROR_INSUFFICIENT_BUFFER) {
            g_logger.Log("PowerEnumerate buffer too small at index %lu, continuing", index);
            index++; continue;
        }
        else if(ret != ERROR_SUCCESS) {
            g_logger.Log("PowerEnumerate failed at index %lu, error=%ld", index, ret);
            break;
        }
        if(!IsEqualGUID(scheme, activeScheme)) {
            WriteBrightnessWithRetry(scheme, b, index);
        }
        index++;
    }
    g_logger.Log("Brightness synchronized: AC=%lu, DC=%lu", b.ac, b.dc);
}

void SyncBrightness() {
    GUID activeScheme;
    if(!GetActiveSchemeGUID(activeScheme)) return;
    Brightness b;
    if(!ReadBrightness(activeScheme, b)) return;
    SyncSchemeBrightness(activeScheme, b);
}

// ================= 开机自启 =================
void EnableAutoRun() {
    char exe[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if(len == 0 || len == MAX_PATH) { g_logger.Log("GetModuleFileNameA failed"); return; }

    std::string exeStr(exe);
    std::replace(exeStr.begin(), exeStr.end(), '\"', '\''); 
    std::string exePath = "\"" + exeStr + "\"";

    HKEY hKey;
    if(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        char existing[MAX_PATH] = {};
        DWORD size = sizeof(existing);
        if(RegQueryValueExA(hKey, "PowerBrightnessSync", nullptr, nullptr, (BYTE*)existing, &size) == ERROR_SUCCESS) {
            g_logger.Log("Auto-run already exists.");
            RegCloseKey(hKey);
            return;
        }
        RegSetValueExA(hKey, "PowerBrightnessSync", 0, REG_SZ, (BYTE*)exePath.c_str(), (DWORD)(exePath.size()+1));
        RegCloseKey(hKey);
        g_logger.Log("Auto-run enabled");
    } else g_logger.Log("Failed to open Run key for auto-run");
}

void DisableAutoRun() {
    HKEY hKey;
    if(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, "PowerBrightnessSync");
        RegCloseKey(hKey);
        g_logger.Log("Auto-run disabled");
    } else g_logger.Log("Failed to open Run key to disable auto-run");
}

// ================= 窗口消息 =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if(msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE) {
        auto pbs = (PPOWERBROADCAST_SETTING)lp;
        if(pbs && IsEqualGUID(pbs->PowerSetting, kGuidVideoBrightness) && pbs->DataLength >= sizeof(DWORD))
            SyncBrightness();
    } else if(msg == WM_CLOSE) PostQuitMessage(0);
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ================= WinMain =================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    if(!IsWindowsVistaOrGreater()) {
        MessageBoxA(nullptr, "This program requires Windows Vista or higher.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    if(!IsAdministrator()) {
        MessageBoxA(nullptr, "Administrator privileges required to sync all power schemes.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::string cmd(lpCmdLine ? lpCmdLine : "");
    if(cmd.find("--enable-autobrightness") != std::string::npos) { EnableAutoRun(); return 0; }
    if(cmd.find("--disable-autobrightness") != std::string::npos) { DisableAutoRun(); return 0; }

    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PowerBrightnessSyncHiddenWnd";

    if(!RegisterClass(&wc)) { g_logger.Log("Failed to register window class"); return 1; }

    HWND hwnd = CreateWindow(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if(!hwnd) { g_logger.Log("Failed to create hidden window"); return 1; }

    HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    if(!hNotify) g_logger.Log("Failed to register power notification");

    SyncBrightness();

    MSG msg;
    while(GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if(hNotify) UnregisterPowerSettingNotification(hNotify);
    DestroyWindow(hwnd);
    return (int)msg.wParam;
}
