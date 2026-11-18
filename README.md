# DeskHide —— 双击桌面空白处，隐藏桌面图标

<img src="assets/icon.png" width="88" align="right" alt="DeskHide">

常驻托盘的极小吃内存工具：在**桌面空白处双击左键**就把所有桌面图标收起来（壁纸、Windows 小部件、壁纸引擎动画都不受影响），再双击一次恢复。

- 纯 Win32，单文件 exe（~320 KB，静态链接，无运行库依赖）
- 常驻私有内存约 **1.7 MB**，工作集约 10 MB，句柄 130 左右
- 空闲时**零定时器唤醒**（用低级鼠标钩子，不是轮询）
- 支持开机自启（写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`）

## 下载与安装

从 [Releases](../../releases/latest) 下载 `DeskHide.exe`，放到一个本地固定目录（推荐 `%LOCALAPPDATA%\DeskHide\DeskHide.exe`），双击运行即可。托盘出现蓝色显示器图标后就生效了。

开机自启：**右键托盘图标 → 勾选「开机自启」**（立刻生效，无需重启）。

> 不要把 exe 放在沙箱目录、网络盘或某些同步盘里运行——Windows 可能降低进程完整性级别，导致程序无法与 Explorer 通信（见下文「权限与兼容性」）。

## 怎么用

| 操作 | 效果 |
| --- | --- |
| 桌面空白处双击左键 | 隐藏 / 恢复桌面图标（唯一需要的操作） |
| 左键单击托盘图标 | 同样切换隐藏 / 恢复 |
| 右键托盘图标 | 菜单：隐藏桌面图标 / 显示桌面图标、开机自启、退出 |

判定规则（避免误触）：

- 双击的**两下**都必须落在桌面空白处——点在图标上、任务栏上、任何窗口上都不算；
- **两次点击发生前**，桌面都不能有选中项。所以「先点图标选中它，再双击」不会触发；想隐藏时如果桌面有图标处于选中状态，先在空白处点一下（这次点击会清空选中并作为双击的第一下），紧接着再点一下就隐藏了；
- 时间间隔遵循系统「双击速度」设置（`GetDoubleClickTime()`），两下位移也必须在系统双击判定范围内。

## 命令行（可选）

```
DeskHide.exe              正常后台常驻
DeskHide.exe --toggle     切换一次图标可见性后退出（不常驻）
DeskHide.exe --hide       只隐藏
DeskHide.exe --show       只显示（--restore 同义）
DeskHide.exe --exit       让正在运行的实例退出
DeskHide.exe --autostart / --no-autostart   写 / 删开机自启
DeskHide.exe --log [--logfile <路径>]       写诊断日志（默认 %LOCALAPPDATA%\DeskHide\deskhide.log）
```

## 原理

- **判定“点在桌面上”**：`WindowFromPoint` → `GetAncestor(GA_ROOT)`，根窗口类名为 `Progman` / `WorkerW` 即视为桌面。
- **判定“没有选中图标”**：向桌面的 `SysListView32` 发 `LVM_GETNEXTITEM(-1, LVNI_SELECTED)`，返回 -1 表示无选中（跨进程只传整数，不碰对方内存）。
- **隐藏/恢复**：向桌面的 `SHELLDLL_DefView` 发 `WM_COMMAND`，命令 ID `0x7402` —— 就是资源管理器自己「显示桌面图标」菜单项的命令。好处是状态原生：会同步写注册表 `HKCU\...\Explorer\Advanced\HideIcons`，Explorer 重启后保持一致，图标窗口只是隐藏而不销毁。若消息被系统拦下，退回 `ShowWindow` 兜底。
- **监听点击**：`WH_MOUSE_LL` 低级鼠标钩子（只观察，不吞事件，返回 `CallNextHookEx`）。钩子里只做判断，切换动作 `PostMessage` 到主线程执行，避免在钩子中操作 Explorer 造成卡顿。钩子安装失败时自动退化为 10 ms 轮询。

## 已知限制

- **恢复比隐藏慢一点**：隐藏是让图标窗口停止绘制，屏幕约 20 ms 就变干净；恢复需要 Explorer 重新绘制整个图标层（每个图标的位图与文字标签），在普通机器上实测要 250–350 ms 的 Explorer CPU 时间，屏幕才会一次性刷新。这段开销在 Explorer 内部，从外部无法催快（`RedrawWindow(UPDATENOW)`、`InvalidateRect`、`SetWindowPos(FRAMECHANGED)` 都试过，无效果）。若显式用 `ShowWindow(SW_SHOW)` 绕过「显示桌面图标」命令，同样约 290 ms，可确认与命令无关。
- 桌面上有选中项时双击不会触发（这是设计，避免误触）。

## 权限与兼容性

- Windows 10 / 11 x64。程序以当前用户权限运行，**不需要管理员**。
- 它与 Explorer 之间靠窗口消息通信，受 UIPI 限制：进程的完整性级别必须不低于 explorer，否则消息会被静默丢弃。正常情况下两者同级，没问题；如果从受限位置（沙箱目录、某些网络盘）启动，Windows 可能降低进程完整性级别，此时启动会提示「无法访问桌面（权限不足）」，请把 exe 放到本地普通目录再运行。
- 若本程序隐藏过图标，在**退出、关机、注销**时会自动把图标恢复显示，避免出现“找不到图标”的尴尬。

## 卸载

1. 右键托盘图标 → 退出；
2. 删除 `%LOCALAPPDATA%\DeskHide\` 整个目录；
3. 若菜单里还勾着开机自启，退出前先取消勾选；或手动删除注册表值
   `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `DeskHide`。

## 从源码构建

需要 MinGW-w64（开发时用 g++ 14.2.0，UCRT 版）：

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

或手动：

```
windres --codepage=65001 -I src -i src\app.rc -o build_res.o
g++ -std=c++17 -municode -mwindows -O2 -s -static -static-libgcc -static-libstdc++ ^
    -fno-exceptions -fno-rtti -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    -o DeskHide.exe src\deskhide.cpp build_res.o -lshell32
```

## 文件

```
DeskHide/
├─ build.ps1           构建脚本（MinGW）
├─ README.md
├─ LICENSE
└─ src/
   ├─ deskhide.cpp     全部实现（约 570 行）
   ├─ resource.h       资源 ID
   ├─ app.rc           图标 / 清单 / 版本信息
   ├─ app.manifest     PerMonitorV2、asInvoker、Win10/11 兼容声明
   └─ icon.ico         多尺寸图标（16–256 px）
```

## 许可证

[MIT](LICENSE)
