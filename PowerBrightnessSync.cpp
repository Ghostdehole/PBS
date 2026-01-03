#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
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
#include <atomic>
#include <chrono>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")

namespace fs = std::filesystem;

// ================= GUID 定义 =================
// 视频子组 GUID
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
// 显示器亮度 GUID
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };

// ================= 日志类 (优化版) =================
class Logger {
public:
    Logger(size_t maxSize = 10 * 1024 * 1024) : maxSize(maxSize), stopFlag(false) {
        if (!InitLogDir()) {
            return;
        }
        // 启动时清理一次旧日志即可，不需要每次写入都清理
        CleanupOldLogs();
        logThread = std::thread([this] { ProcessQueue(); });
    }

    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stopFlag = true;
            cv.notify_all();
        }
        if (logThread.joinable()) logThread.join();
    }

    // 支持宽字符格式化
    void Log(const wchar_t* fmt, ...) {
        wchar_t buf[1024];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t line[1200];
        _snwprintf_s(line, _countof(line), _TRUNCATE,
            L"[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, buf);

        std::lock_guard<std::mutex> lock(mtx);
        queue.push(line);
        cv.notify_one();
    }

private:
    fs::path logDir;
    fs::path logPath;
    size_t maxSize;
    std::queue<std::wstring> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopFlag;
    std::thread logThread;

    bool InitLogDir() {
        wchar_t localAppData[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) return false;
        
        logDir = fs::path(localAppData) / L"PowerBrightnessSync" / L"Logs";
        std::error_code ec;
        if (!fs::exists(logDir)) {
            if (!fs::create_directories(logDir, ec)) return false;
        }
        UpdateCurrentLogPath();
        return true;
    }

    void UpdateCurrentLogPath() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t fname[64];
        swprintf_s(fname, L"Sync_%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
        logPath = logDir / fname;
    }

    void CleanupOldLogs() {
        if (!fs::exists(logDir)) return;
        std::vector<fs::directory_entry> logs;
        size_t totalSize = 0;
        
        try {
            for (auto& e : fs::directory_iterator(logDir)) {
                if (e.is_regular_file()) {
                    logs.push_back(e);
                    totalSize += e.file_size();
                }
            }
        } catch (...) { return; }

        if (totalSize <= maxSize) return;

        std::sort(logs.begin(), logs.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return fs::last_write_time(a) < fs::last_write_time(b);
        });

        for (auto& f : logs) {
            if (totalSize <= maxSize) break;
            try {
                size_t sz = f.file_size();
                fs::remove(f.path());
                totalSize -= sz;
            } catch (...) {}
        }
    }

    void ProcessQueue() {
        while (true) {
            std::queue<std::wstring> localQueue;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this] { return stopFlag || !queue.empty(); });
                if (stopFlag && queue.empty()) break;
                std::swap(localQueue, queue);
            }

            // 检查日期变更
            UpdateCurrentLogPath();

            FILE* fp = nullptr;
            if (_wfopen_s(&fp, logPath.c_str(), L"a, ccs=UTF-8") == 0 && fp) {
                while (!localQueue.empty()) {
                    fputws(localQueue.front().c_str(), fp);
                    localQueue.pop();
                }
                fclose(fp);
            }
        }
    }
} g_logger;

// ================= 工具函数 =================
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

// 简单的 RAII 注册表 Key 包装
struct AutoHKey {
    HKEY hKey = nullptr;
    ~AutoHKey() { if (hKey) RegCloseKey(hKey); }
    operator HKEY() const { return hKey; }
    HKEY* operator&() { return &hKey; }
};

// ================= 核心逻辑 =================
struct Brightness { DWORD ac = 0, dc = 0; };

bool GetActiveSchemeGUID(GUID& out) {
    GUID* pGuid = nullptr;
    if (PowerGetActiveScheme(nullptr, &pGuid) != ERROR_SUCCESS) {
        g_logger.Log(L"PowerGetActiveScheme failed. Error=%lu", GetLastError());
        return false;
    }
    out = *pGuid;
    LocalFree(pGuid);
    return true;
}

bool ReadBrightness(const GUID& scheme, Brightness& b) {
    if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &b.ac) != ERROR_SUCCESS ||
        PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &b.dc) != ERROR_SUCCESS) {
        g_logger.Log(L"Failed to read brightness values.");
        return false;
    }
    b.ac = std::clamp<DWORD>(b.ac, 0u, 100u);
    b.dc = std::clamp<DWORD>(b.dc, 0u, 100u);
    return true;
}

void SyncSchemeBrightness(const GUID& activeScheme, const Brightness& b) {
    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        LONG ret = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) {
            index++; continue;
        }

        // 仅同步到非活动方案，避免不必要的写操作
        if (!IsEqualGUID(scheme, activeScheme)) {
            // 简单重试逻辑
            bool success = false;
            for (int i = 0; i < 2; i++) {
                if (PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, b.ac) == ERROR_SUCCESS &&
                    PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, b.dc) == ERROR_SUCCESS) {
                    success = true;
                    break;
                }
                Sleep(10);
            }
            if (!success) g_logger.Log(L"Failed to write to scheme index %lu", index);
        }
        index++;
    }
    g_logger.Log(L"Synced Brightness: AC=%lu, DC=%lu", b.ac, b.dc);
}

// 核心同步函数，带简单的状态检查
void PerformSync() {
    GUID activeScheme;
    if (!GetActiveSchemeGUID(activeScheme)) return;
    
    Brightness b;
    if (!ReadBrightness(activeScheme, b)) return;
    
    // 检查是否真的需要同步（避免重复写入）
    // 这里简单直接写，因为读取其他方案成本也不低
    SyncSchemeBrightness(activeScheme, b);
}

// ================= 防抖逻辑 =================
// 避免滑动亮度条时频繁触发
std::atomic<bool> g_needsSync{ false };
std::mutex g_syncMutex;
std::condition_variable g_syncCv;

void SyncWorker() {
    while (true) {
        std::unique_lock<std::mutex> lock(g_syncMutex);
        g_syncCv.wait(lock); // 等待触发信号
        
        // 收到信号后，等待一小段时间（防抖），如果期间又有信号，则继续等待
        // 简单实现：Sleep一段，然后检查最后状态
        lock.unlock();
        
        // 防抖窗口 500ms
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // 执行同步
        PerformSync();

        // 简单的排空：如果刚才在 sleep 期间又有信号，其实本次 Sync 已经覆盖了最新的值
        // 所以不需要再次循环，直接继续等待下一次信号
    }
}

// ================= 开机自启 (Unicode) =================
void EnableAutoRun() {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;

    std::wstring pathWithQuotes = L"\"";
    pathWithQuotes += exePath;
    pathWithQuotes += L"\"";

    AutoHKey hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        
        RegSetValueExW(hKey, L"PowerBrightnessSync", 0, REG_SZ, 
            reinterpret_cast<const BYTE*>(pathWithQuotes.c_str()), 
            static_cast<DWORD>((pathWithQuotes.size() + 1) * sizeof(wchar_t)));
        g_logger.Log(L"Auto-run enabled.");
    }
}

void DisableAutoRun() {
    AutoHKey hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, L"PowerBrightnessSync");
        g_logger.Log(L"Auto-run disabled.");
    }
}

// ================= 窗口消息 =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE) {
        auto pbs = (PPOWERBROADCAST_SETTING)lp;
        // 仅处理亮度变化事件
        if (pbs && IsEqualGUID(pbs->PowerSetting, kGuidVideoBrightness)) {
            // 触发后台同步线程
            g_syncCv.notify_one();
        }
    }
    else if (msg == WM_CLOSE) {
        PostQuitMessage(0);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ================= 入口点 =================
// 使用 wWinMain 支持 Unicode 命令行
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    // 1. 单例检查
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // 已经运行
    }

    if (!IsWindowsVistaOrGreater()) return 1;

    // 2. 解析参数
    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");
    if (cmd.find(L"--onar") != std::wstring::npos) { EnableAutoRun(); return 0; }
    if (cmd.find(L"--ofar") != std::wstring::npos) { DisableAutoRun(); return 0; }

    if (!IsAdministrator()) {
        MessageBoxW(nullptr, L"Administrator privileges required to sync power schemes.", L"PowerBrightnessSync", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_logger.Log(L"Service started.");

    // 3. 启动同步线程
    std::thread syncThread(SyncWorker);
    syncThread.detach(); // 让它在后台运行

    // 4. 创建隐藏窗口监听电源事件
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PowerBrightnessSyncHiddenWnd";

    if (!RegisterClassW(&wc)) {
        g_logger.Log(L"Failed to register window class.");
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // 注册电源设置通知
    HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    
    // 启动时先同步一次
    g_syncCv.notify_one();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hNotify) UnregisterPowerSettingNotification(hNotify);
    DestroyWindow(hwnd);
    if (hMutex) CloseHandle(hMutex);
    
    return (int)msg.wParam;
}