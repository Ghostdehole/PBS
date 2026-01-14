# PowerBrightnessSync

> ⚡ **极致轻量化的 Windows 屏幕亮度同步工具**

PowerBrightnessSync 是一款专为 Windows 设计的后台常驻小工具。  
它会监听系统的 **屏幕亮度变化与显示状态变化事件**，并将当前亮度值**同步写入所有电源计划（Power Schemes）**。

主要用于解决 Windows 在 **插拔电源、切换电源模式或显示器唤醒时亮度突然跳变**，导致视觉不适的问题。

[![Platform](https://img.shields.io/badge/platform-Windows-blue)](https://github.com/Ghostdehole/PBS)
[![License](https://img.shields.io/github/license/Ghostdehole/PBS?color=green)](https://github.com/Ghostdehole/PBS/blob/main/LICENSE)
[![Download](https://img.shields.io/github/v/release/Ghostdehole/PBS?color=brightgreen)](https://github.com/Ghostdehole/PBS/releases)

---

## ✨ 核心特性

- **全电源计划亮度同步**  
  自动将当前活动电源计划的亮度值，同步到系统内的所有电源方案，避免切换时亮度突变。

- **纯 Win32 实现，无依赖**  
  仅使用 Windows 原生 API（PowrProf / User32），无 C++ Runtime 依赖，体积极小。

- **事件驱动，零空转 CPU**  
  基于 `WM_POWERBROADCAST` 事件，仅在亮度或显示状态变化时被唤醒，其余时间完全休眠。

- **智能防抖机制**  
  内置 400ms 防抖定时器，避免拖动亮度条时频繁写入电源配置，减少系统与 SSD 压力。

- **极低内存占用**  
  使用工作集修剪（Working Set Trimming），后台驻留时物理内存占用通常低于 **1MB**。

- **静默运行（Ghost Mode）**  
  无窗口、无托盘图标、无日志输出，完全透明运行。

- **单实例保护**  
  使用全局互斥体，确保系统中仅运行一个实例。

---

## 🚀 快速开始

### 系统要求
- Windows 7 / 8 / 10 / 11
- **必须以管理员身份运行**

> ⚠️ **为什么需要管理员权限？**  
> Windows 默认只允许修改“当前激活”的电源计划。  
> 本工具会同步修改**所有电源方案**的亮度设置，因此必须使用管理员权限。

---

### 运行方式

本程序为 **绿色单文件**，无需安装：

1. 下载最新发布的 `PowerBrightnessSync.exe`
2. 右键 → **以管理员身份运行**
3. 启动后会立即同步一次亮度，随后进入后台监听模式

> 程序启动后不会显示任何窗口或托盘图标  
> 如需确认是否运行，请在任务管理器中查找 `PowerBrightnessSync.exe`

---

### 退出程序

由于程序设计为完全静默运行，如需退出请使用以下方式之一：

- **任务管理器** → 结束进程
- 命令行：
```cmd
taskkill /f /im PowerBrightnessSync.exe
```
## ⚙️ 命令行选项（开机自启）
程序支持通过命令行参数快速配置开机自启。
自启通过 Windows 任务计划程序（schtasks） 实现，而非注册表。
- **注意：配置命令执行完毕后会自动退出，不会启动后台服务。**

 |参数|功能说明|
 |-|-|
 | `--onar`  |启用开机自启|
 | `--ofar`  |关闭开机自启|

**使用示例：**

请以**管理员身份**打开 CMD 或 PowerShell 执行以下命令：
```cmd
:: 开启开机自启
PowerBrightnessSync.exe --onar

:: 取消开机自启
PowerBrightnessSync.exe --ofar
```
## 🛠️ 构建指南 (Build)
如果您想自己编译源代码，请确保安装 Visual Studio (支持 C++17)。

推荐使用 MSVC 命令行进行极致体积优化编译：

```cmd
cl /nologo /EHsc /std:c++17 /O1 /MT /GL /DNDEBUG ^
/DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 ^
PowerBrightnessSync.cpp ^
/link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
/MANIFEST:EMBED ^
/MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
/OUT:PowerBrightnessSync.exe
```
## 🔍 工作原理简述

程序通过监听系统电源广播事件（`WM_POWERBROADCAST`）运行，主要关注以下变化：

- 屏幕亮度变化  
- 显示器开关 / 唤醒状态变化  

当相关事件触发后，程序不会立即执行同步，而是先进入**防抖计时阶段**，避免在短时间内频繁写入电源配置。

防抖计时结束后，程序将执行以下步骤：

1. 读取当前**活动电源计划**中的屏幕亮度值  
2. 将该亮度值同步写入系统内的**所有其他电源方案**  
3. 同步完成后，主动释放进程工作集，使进程重新进入低内存、休眠等待状态  

---

## 📝 免责声明

本工具会对系统电源方案中的屏幕亮度配置进行读写操作。  
尽管程序中已加入防抖、边界限制与错误检查机制，但在不同设备与系统环境下的行为可能存在差异。

在日常使用前，建议用户自行进行测试以确认其行为符合预期。  
作者不对因使用本工具而导致的任何系统异常、设置变化或数据损失承担责任。

如遇 Bug，欢迎提交 Issue 反馈。