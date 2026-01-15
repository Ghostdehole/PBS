# PowerBrightnessSync

> ⚡ **极致轻量化的 Windows 屏幕亮度同步工具**

PowerBrightnessSync 是一款专为 Windows 设计的后台常驻小工具。
它会监听系统的 **屏幕亮度变化、显示器开关状态以及电源源（AC/DC）切换事件**，并将当前亮度值**同步写入所有电源计划（Power Schemes）的 AC 与 DC 设置中**。

主要用于解决 Windows 在 **插拔电源、切换电源模式或显示器唤醒时亮度突然跳变**，导致视觉不适的问题。

[![Platform](https://img.shields.io/badge/platform-Windows-blue)](https://github.com/Ghostdehole/PBS)
[![License](https://img.shields.io/github/license/Ghostdehole/PBS?color=green)](https://github.com/Ghostdehole/PBS/blob/main/LICENSE)
[![Download](https://img.shields.io/github/v/release/Ghostdehole/PBS?color=brightgreen)](https://github.com/Ghostdehole/PBS/releases)

---

## ✨ 核心特性

- **全场景亮度锁定**  
  无论你当前使用电池还是插电，一旦调节亮度，工具会自动将该值同步写入到**所有电源方案**的“接通电源”和“使用电池”设置中。这意味着**拔插电源时，亮度将不再发生改变**。

- **Modern C++ & Win32 高效实现**  
  基于 C++17 与原生 Windows API 开发，利用 COM 接口管理任务计划，结合 RAII 资源管理，代码健壮且体积极小（单文件，静态编译后无依赖）。

- **事件驱动，零空转**  
  基于 `WM_POWERBROADCAST` 事件通知机制。仅在亮度改变、显示器开关或电源插拔时唤醒，其余时间线程挂起，**CPU 占用率为 0%**。

- **智能防抖机制 (600ms)**  
  内置 600ms 防抖定时器。当你滑动亮度条时，程序会等待操作结束再执行写入，避免频繁读写注册表/电源配置，保护 SSD 并降低系统压力。

- **静默运行 (Ghost Mode)**  
  无窗口、无托盘图标、无日志输出，通过 Message-Only Window 处理消息，完全透明运行。

- **单实例保护**  
  使用全局命名互斥体，确保系统中仅运行一个实例。

---

## 🚀 快速开始

### 系统要求
- Windows 10 / 11 (推荐) / Windows 7 SP1+
- **必须以管理员身份运行**

> ⚠️ **为什么需要管理员权限？**  
> Windows 默认只允许修改“当前激活”的电源计划。
> 本工具需要遍历并修改系统内**所有电源方案**（包括隐藏方案）的 AC/DC 亮度曲线，因此必须拥有管理员权限才能生效。

---

### 运行方式

本程序为 **绿色单文件**，无需安装：

1. 下载最新发布的 `PowerBrightnessSync.exe`
2. 双击启动程序
3. 启动后程序会立即进行一次同步，随后进入后台静默监听。

> **注意**：程序启动后不会显示任何界面。
> 如需确认运行状态，请在任务管理器中查找 `PowerBrightnessSync.exe`。

---

### 退出程序

由于程序设计为完全静默运行，如需退出请使用以下方式之一：

- **任务管理器** → 详细信息 → 结束 `PowerBrightnessSync.exe` 进程
- **命令行 (管理员)**：
```cmd
taskkill /f /im PowerBrightnessSync.exe
```

---

## ⚙️ 命令行选项（开机自启）

程序内置了对 Windows 任务计划程序（Task Scheduler）的管理功能。
设置自启将创建一个**最高权限 (Highest RunLevel)** 的任务，确保开机后能以管理员权限静默运行。

| 参数 | 功能说明 |
| :--- | :--- |
| `--onar` | **开启**开机自启 (注册任务计划) |
| `--ofar` | **关闭**开机自启 (删除任务计划) |

**使用示例：**

请以**管理员身份**打开 CMD 或 PowerShell 执行：

```cmd
:: 开启开机自启 (成功会弹窗提示)
PowerBrightnessSync.exe --onar

:: 取消开机自启
PowerBrightnessSync.exe --ofar
```

---

## 🛠️ 构建指南 (Build)

如果您想自己编译源代码，环境要求如下：
- Visual Studio 2019 或更高版本
- Windows SDK
- 支持 **C++17** 标准

推荐使用 MSVC 命令行进行编译（确保已进入 `x64 Native Tools Command Prompt`）：

```cmd
cl /nologo /EHsc /std:c++17 /O2 /MT /GL /DNDEBUG ^
/DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 ^
PowerBrightnessSync.cpp ^
/link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
/MANIFEST:EMBED ^
/MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
/OUT:PowerBrightnessSync.exe
```

> **编译说明**：
> - `/std:c++17`: 必需，使用了 `std::clamp` 等特性。
> - `/MT`: 静态链接运行时，确保在没有 VC++ 运行库的机器上也能运行。
> - `/MANIFESTUAC`: 强制嵌入管理员权限请求，防止双击运行时因权限不足而无声退出。
> - 代码中已通过 `#pragma comment` 自动链接了 `taskschd.lib`, `comsupp.lib` 等库。

---

## 🔍 工作原理简述

程序创建了一个消息专用窗口（Message-Only Window），注册了 `RegisterPowerSettingNotification` 监听：

1.  **GUID_VIDEO_BRIGHTNESS**: 屏幕亮度变化。
2.  **GUID_CONSOLE_DISPLAY_STATE**: 显示器熄灭/点亮。
3.  **PBT_APMPOWERSTATUSCHANGE**: 电源线插拔。

**处理流程：**
1.  检测到上述任一事件，启动 **600ms** 计时器（如果计时器已存在则重置，实现防抖）。
2.  计时结束后，触发同步逻辑：
    - 读取当前激活方案的实际亮度值。
    - 遍历系统所有电源方案。
    - 将读取到的亮度值，强制写入到这些方案的 **AC (接通电源)** 和 **DC (使用电池)** 亮度索引中。

---

## 📝 免责声明

本工具会对系统电源方案中的屏幕亮度配置进行读写操作。
尽管程序中已加入防抖、边界限制与 RAII 资源管理机制，但在不同硬件驱动（特别是 OEM 厂商自带的电源管理软件）环境下，行为可能存在差异。

建议初次使用时观察一段时间。
作者不对因使用本工具而导致的任何系统设置异常承担责任。

如遇 Bug，欢迎提交 Issue 反馈。