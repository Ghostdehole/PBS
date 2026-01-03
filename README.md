# PowerBrightnessSync

PowerBrightnessSync 是一款 Windows 工具，用于同步所有电源计划的屏幕亮度，并支持开机自启和日志记录。
本工具目前未经过任何测试，如遇到问题请提交Issues

## 功能

- 自动同步当前电源计划亮度到所有方案
- 支持开机自启（注册表写入）
- 日志记录，保存于 `%LOCALAPPDATA%\PowerBrightnessSync\Logs`
- 管理员权限检测
- 支持 Windows Vista 及以上版本

## 使用方法

### 启动程序

直接运行可执行文件，程序在后台运行并监听亮度变化。

### 命令行选项

| 参数       | 说明                       |
|-----------|---------------------------|
| `--onar`  | 启用开机自启               |
| `--ofar`  | 关闭开机自启               |

示例：

```cmd
PowerBrightnessSync.exe --onar
PowerBrightnessSync.exe --ofar
