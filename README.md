# VmwareWatch

让 VMware Workstation Pro 的 4 个后台服务不再常驻：**只在 VMware 开着的时候运行，关掉 VMware 就跟着停**。

Win32 原生小程序，单文件 exe、常驻内存约 1 MB、无任何运行时依赖。

## 它做什么

1. **启动**：串行拉起 4 个服务（`VMAuthdService` → `VMUSBArbService` → `VMware NAT Service` → `VMnetDHCP`），再启动 `vmware.exe`
2. **监听**：每秒检查 VMware 的主窗口、主进程、托盘进程
3. **收尾**：先逆序停服务，再结束进程，最多两轮补刀，然后自己退出

主窗口消失时（点窗口 X）会弹确认框，20 秒倒计时：

- **【是】** 结束全部 VMware 进程（含运行中的虚拟机）+ 停 4 个服务 + 自身退出
- **【否】/ Esc / 标题栏 X** 保持原生行为：托盘和后台服务继续运行，程序继续监听
- **20 秒不操作** 自动按【是】执行清理并退出（所以中途想反悔，随时点【否】）

最小化主窗口**不会**触发确认框。

## 目录

```
vmware_watch.c                    源码
app.manifest                      comctl32 v6 清单（TaskDialog 必需）
app.rc                            把上面那份清单编成资源的脚本
.github/workflows/build-exe.yml   在线编译：GitHub Actions 用 MSVC 出 exe
```

## 编译

### 方式一：GitHub Actions（在线，推荐给没有编译环境的机器）

1. 把本仓库推到 GitHub
2. Actions 页面手动触发 `build-exe`（或打 `v*` 标签，会额外发 Release）
3. 运行结束后在 Artifacts 里下载 `VmwareWatch.exe`

工作流会先用 `rc.exe` 把 `app.manifest` 编进 exe，再检查产物里确实含
`Microsoft.Windows.Common-Controls` —— 少了它 `TaskDialogIndirect` 会在加载期报“无法定位序数 345”。

### 方式二：MSVC 本地

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
rc /nologo /fo app.res app.rc
cl /O2 /MT /utf-8 /DUNICODE /D_UNICODE vmware_watch.c app.res /Fe:VmwareWatch.exe /link ^
  /MANIFEST:NO advapi32.lib comctl32.lib shell32.lib user32.lib gdi32.lib shlwapi.lib
```

### 方式三：MinGW-w64

```bat
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc vmware_watch.c app.res -O2 -mwindows -static ^
  -o VmwareWatch.exe -lcomctl32 -ladvapi32 -lshell32 -luser32 -lgdi32 -lshlwapi
```

Manifest 必须进 exe，否则 TaskDialog 不可用（这也是 MinGW 分支唯一多出来的一步）。

## 运行

双击运行，会弹一次 UAC（启停服务必须管理员，程序会自提权重开）。

日志：exe 同级目录下的 `log\` 子目录（自动创建），文件名带启动时间戳，每次启动一个文件：

```
D:\VmwareWatch\log\VmwareWatch_20261120_161230.log
```

若 exe 所在目录不可写（例如放在 `Program Files`），自动回退到 `%TEMP%`。每行带毫秒时间戳，逐行落盘。

## 可改的常量

都在 `vmware_watch.c` 顶部：

| 常量 | 含义 | 默认 |
|---|---|---|
| `kVmwExe` / `kVmwDir` | VMware 主程序路径 | `D:\VMware Workstation` |
| `kServices` | 服务清单（此顺序即启动顺序） | 4 个 |
| `kProcs` | 收尾要结束的进程 | 7 个 |
| `kDialogTimeoutMs` | 确认框倒计时 | 20000 ms |
| `kWaitWindowSec` | 等主窗口出现上限 | 120 s |

## 尚未完成

- 4 个服务的启动类型仍是「自动」：程序只管启停，**开机自启这一层还在**。要彻底做到“平时不常驻”，还需把服务改成手动（`sc config <name> start= demand`），此步骤未实现。
- `vmware-vmx.exe`（虚拟机进程）分支无真机数据：只有在虚拟机开机时该进程才存在，测试时未开虚拟机。
