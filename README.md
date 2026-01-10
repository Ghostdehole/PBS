# PowerBrightnessSync

> ⚡ **极致轻量化的 Windows 亮度同步工具**

PowerBrightnessSync 是一款专为 Windows 设计的微型实用工具。它驻留在后台，自动检测当前的屏幕亮度变化，并将该亮度值同步应用到系统中的**所有电源计划**（Power Schemes）。

主要解决 Windows 在插拔电源适配器或切换电源模式时，屏幕亮度突然跳变导致视觉不适的问题。

[![Platform](https://img.shields.io/badge/platform-Windows-blue)](https://github.com/Ghostdehole/PBS)
[![License](https://img.shields.io/github/license/Ghostdehole/PBS?color=green)](https://github.com/Ghostdehole/PBS/blob/main/LICENSE)
[![Download](https://img.shields.io/github/v/release/Ghostdehole/PBS?color=brightgreen)](https://github.com/Ghostdehole/PBS/releases)

## ✨ 核心特性

- **极致省电**：采用纯 Win32 API 编写，无多余依赖，专为笔记本续航优化。
- **零 CPU 占用**：完全基于事件驱动（Event-Driven）。只有在您拖动亮度条时才会唤醒，其余时间 CPU 占用率为 **0%**。
- **内存微缩**：利用工作集修剪技术，后台挂起时物理内存占用仅 **100KB - 800KB**。
- **智能防抖**：内置防抖算法，避免在滑动亮度条时频繁写入注册表，保护 SSD 寿命。
- **静默运行**：无窗口、无托盘图标，完全透明运行（Ghost Mode）。

## 🚀 快速开始

### 系统要求
- Windows 7 / 8 / 10 / 11
- **必须以管理员身份运行** (需要权限修改非当前激活的电源方案)

### 安装与运行
本程序为绿色单文件，无需安装：
1. 下载最新发布的 `.exe` 文件。
2. 右键 -> **以管理员身份运行**。
3. 程序启动后会立即同步一次亮度，随即进入后台静默监听模式。

> ⚠️ **注意**：程序运行后没有界面和托盘图标。如需确认是否运行，请在任务管理器中查找 `PowerBrightnessSync.exe`。

### 停止程序
由于程序设计为极致隐形，若需关闭程序，请使用 **任务管理器** 结束进程，或在命令行执行：
```cmd
taskkill /f /im PowerBrightnessSync.exe
```
## ⚙️ 命令行选项
程序支持通过命令行参数快速配置开机自启。
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
cl /nologo /EHsc /std:c++17 /O1 /MT /GL /DNDEBUG /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 ^
PowerBrightnessSync.cpp ^
/link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
/MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
/OUT:PowerBrightnessSync.exe
```
## 📝 免责声明
本工具涉及系统电源方案注册表的读写操作。虽然代码中已包含防抖和安全检查逻辑，但作者不对因使用本工具导致的任何系统异常承担责任。建议在日常使用前进行简单测试。本程序目前未进行实机测试。

如遇 Bug，欢迎提交 Issue 反馈。