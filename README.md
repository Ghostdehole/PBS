# PowerBrightnessSync

⚡ **Extreme Lightweight Windows Screen Brightness Sync Tool**

**PowerBrightnessSync** is a background utility designed specifically for Windows. It monitors system **Screen Brightness Changes**, **Display Power States**, and **Power Source (AC/DC) Switches**, automatically synchronizing the current brightness value to *all* power schemes (both AC and DC settings).

It effectively solves the visual annoyance where Windows brightness suddenly "jumps" when plugging/unplugging the power cable, switching power modes, or waking the display.

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![License](https://img.shields.io/github/license/Ghostdehole/PBS?color=green)
![Release](https://img.shields.io/github/v/release/Ghostdehole/PBS?color=brightgreen)

---

## ✨ Core Features

### 🔒 Full Scenario Brightness Lock
Whether you are on battery or plugged in, once you adjust the brightness, this tool syncs the value to the "Plugged In" and "On Battery" settings of **all existing power schemes**. This ensures the brightness remains constant when you plug or unplug the charger.

### 🛡️ Modern C++ & Robust Resource Management
Built with **C++17** and native Win32 API.
- **RAII & Smart Pointers**: Uses `Microsoft::WRL::ComPtr` and `std::unique_ptr` with custom deleters to ensure zero memory leaks and safe handle management.
- **COM Integration**: Interacts directly with the Windows Task Scheduler via COM interfaces for reliable auto-start management.
- **Tiny Footprint**: Statically compiled into a single executable with no external dependencies.

### 💤 Event-Driven & Zero Idle Load
Based on the `WM_POWERBROADCAST` event mechanism. The thread remains suspended and consumes **0% CPU** until a brightness change, display toggle, or power source switch occurs.

### ⏱️ Smart Debounce (600ms)
Includes a built-in **600ms debounce timer**. When you slide the brightness bar, the tool waits for the operation to settle before writing to the registry/power config. This prevents spamming the system with write operations and protects your SSD.

### 👻 Ghost Mode
Runs completely silently: No window, no tray icon, no console output. It uses a hidden `Message-Only Window` to process system events.

### 🛑 Single Instance Protection
Uses a named mutex (`Local\PowerBrightnessSync_Mutex`) to ensure only one instance is running at a time.

---

## 🚀 Quick Start

### System Requirements
*   **OS**: Windows 10 / 11 (Recommended), Windows 7 SP1+
*   **Privileges**: **Administrator rights are mandatory.**

> **⚠️ Why Administrator?**
> Windows restricts modifying power schemes other than the "active" one by default. To prevent brightness jumps effectively, this tool must traverse and modify the AC/DC curves of *all* power schemes (including hidden ones), which requires elevated privileges.

### How to Run
1.  **Download** the latest `PowerBrightnessSync.exe` from Releases.
2.  **Right-click** and select **"Run as Administrator"**.
3.  The program will perform an immediate sync and then enter background mode.
    *   *Note: You will not see any window. Check Task Manager for `PowerBrightnessSync.exe` to verify it is running.*

### How to Exit
Since there is no UI, use one of the following methods:
*   **Task Manager**: Details tab -> End `PowerBrightnessSync.exe`.
*   **Command Line (Admin)**:
    ```cmd
    taskkill /f /im PowerBrightnessSync.exe
    ```

---

## ⚙️ Auto-Start (Task Scheduler)

The program has built-in logic to manage Windows Task Scheduler. It creates a task with `TASK_RUNLEVEL_HIGHEST` to ensure it starts silently as Admin on login.

| Argument | Description |
| :--- | :--- |
| `--onar` | **ON** Auto Run: Registers the scheduled task. |
| `--ofar` | **OFF** Auto Run: Removes the scheduled task. |

**Setup Example:**
Open CMD or PowerShell as Administrator:

```cmd
:: Enable auto-start (A message box will confirm success)
PowerBrightnessSync.exe --onar

:: Disable auto-start
PowerBrightnessSync.exe --ofar
```

---

## 🛠️ Build Guide

If you wish to compile the source yourself:

**Requirements:**
*   Visual Studio 2019 or later (MSVC)
*   Windows SDK
*   **C++17** Standard support (Required for `std::clamp` and others)

**Compilation Command:**
Open the **x64 Native Tools Command Prompt for VS** and run:

```cmd
cl /nologo /W4 /EHsc /std:c++17 /O2 /MT /GL /DNDEBUG ^
/DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 ^
PowerBrightnessSync.cpp ^
/link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
/MANIFEST:EMBED ^
/MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
/OUT:PowerBrightnessSync.exe
```

**Build Notes:**
*   `/std:c++17`: Strictly required.
*   `/MT`: Statically links the CRT, so the EXE runs on machines without VC++ Redistributables.
*   `#pragma comment`: The source code automatically links `PowrProf`, `User32`, `Advapi32`, `Kernel32`, `Shell32`, `taskschd`, and `comsupp`, so you don't need to list `.lib` files manually.

---

## 🔍 How It Works

1.  **Initialization**:
    *   Checks for Admin rights and Single Instance Mutex.
    *   Creates a hidden window (`HWND_MESSAGE`).
    *   Registers specifically for `GUID_VIDEO_BRIGHTNESS` and `GUID_CONSOLE_DISPLAY_STATE` notifications.

2.  **Event Loop**:
    *   Upon receiving `WM_POWERBROADCAST`, it resets a **600ms timer**.
    *   Once the timer expires (user stopped sliding brightness), the `PerformSync()` function is called.

3.  **Synchronization Logic (`PerformSync`)**:
    *   Gets the active power scheme.
    *   Reads the *current* effective brightness.
    *   Iterates through **every** available power scheme on the system.
    *   Writes the current brightness value to both the **AC (Plugged In)** and **DC (Battery)** indices for the Video Subgroup.

---

## 📝 Disclaimer

This tool modifies system power configuration (specifically brightness curves). While the code implements safety measures (RAII, bounds checking, debounce), behavior may vary depending on specific hardware drivers or OEM power management software.

**Authors are not responsible for any system anomalies caused by the use of this tool.**

If you encounter bugs, please submit an Issue.