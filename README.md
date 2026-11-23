# VmwareWatch

让 VMware Workstation Pro 的 4 个后台服务不再常驻：**只在 VMware 开着的时候运行，关掉 VMware 就跟着停**。

Win32 原生小程序，单文件 exe、常驻内存约 1 MB、无任何运行时依赖。

## 它做什么

1. **探测**：定位 VMware 安装目录（偏好文件 → 注册表 `InstallPath` → `VMAuthdService` 的服务路径 → 默认路径），四个来源都会用 `GetFileAttributesW` 验证 `vmware.exe` 真实存在才采用
2. **体检**：检查 4 个服务的启动类型和 `vmware-tray.exe` 开机自启项；首次运行时弹一次合并确认框，之后按记住的偏好静默处理
3. **启动**：串行拉起 4 个服务（`VMAuthdService` → `VMUSBArbService` → `VMware NAT Service` → `VMnetDHCP`），再启动 `vmware.exe`
4. **监听**：每秒检查 VMware 的主窗口、主进程、托盘进程
5. **收尾**：先逆序停服务，再结束进程，最多两轮补刀，然后自己退出

### 两个对话框

**首次运行的配置确认框**（只在还没做过决定时出现）：

- **按推荐处理** → 4 个服务 `Auto` 改 `Manual` + 移除 `vmware-tray.exe` 自启项
- **保持原样** → 本次不动任何系统配置
- 勾选「以后按这个决定自动处理，不再询问」就把决定写进 `VmwareWatch.prefs.ini`，以后启动不再打扰
- 展开「查看细节」可以看到 4 个服务各自的当前启动类型（自动 / 手动 / 禁用 / 不可访问）

**关闭窗口时的退出确认框**（每次关主窗口出现），20 秒倒计时：

- **【是】** 结束全部 VMware 进程（含运行中的虚拟机）+ 停 4 个服务 + 自身退出
- **【否】/ Esc / 标题栏 X** 保持原生行为：托盘和后台服务继续运行，程序继续监听
- **20 秒不操作** 自动按【是】执行清理并退出

最小化主窗口**不会**触发确认框。主进程和托盘都没了（例如手动退出托盘）则静默收尾，不弹框。

**安装目录找不到时的错误框**：

- **浏览…** 手动选一次 `vmware.exe`，目录记进偏好文件，之后不再问
- **退出** 直接退出，不做任何改动

### 会自我修复

VMware 升级/修复安装会把服务启动类型写回「自动」、还会把托盘自启项加回来。下次双击启动器时会自动纠正回去，界面上不出现任何东西，只在日志里留一行记录。想改回原状，照着 `service-backup.txt` 里的命令执行即可。

## 目录

```
vmware_watch.c                    源码
app.manifest                      comctl32 v6 清单（TaskDialog 必需）
app.rc                            把上面那份清单编成资源的脚本
.github/workflows/build-exe.yml   在线编译：GitHub Actions 用 MSVC 出 exe
tools/backup-before-change.txt    手工改服务前的原始状态记录（含回滚命令）
```

运行后会在 exe 同级生成（不可写时回退 `%APPDATA%\VmwareWatch`）：

```
VmwareWatch.prefs.ini             偏好：state=ask/manual/keep、vmware_dir
service-backup.txt                只在真正改动系统配置时追加，含回滚命令
log\VmwareWatch_YYYYMMDD_HHMMSS.log
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
  /MANIFEST:NO advapi32.lib comctl32.lib comdlg32.lib shell32.lib user32.lib gdi32.lib shlwapi.lib
```

### 方式三：MinGW-w64

```bat
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc vmware_watch.c app.res -O2 -mwindows -static ^
  -o VmwareWatch.exe -lcomctl32 -lcomdlg32 -ladvapi32 -lshell32 -luser32 -lgdi32 -lshlwapi
```

Manifest 必须进 exe，否则 TaskDialog 不可用（这也是 MinGW 分支唯一多出来的一步）。

## 运行

双击桌面快捷方式即可，程序会自提权重开（启停服务必须管理员）。若系统开着 UAC，会弹一次授权框。

日志：exe 同级目录下的 `log\` 子目录（自动创建），文件名带启动时间戳，每次启动一个文件：

```
D:\VmwareWatch\log\VmwareWatch_20261120_161230.log
```

每行带毫秒时间戳，逐行落盘。

## 可改的常量

都在 `vmware_watch.c` 顶部：

| 常量 | 含义 | 默认 |
|---|---|---|
| `kServices` | 服务清单（此顺序即启动顺序） | 4 个 |
| `kProcs` | 收尾要结束的进程 | 7 个 |
| `kDialogTimeoutMs` | 退出确认框倒计时 | 20000 ms |
| `kWaitWindowSec` | 等主窗口出现上限 | 120 s |
| `PREF_MANUAL` / `PREF_KEEP` | 偏好三态 | ask / manual / keep |

VMware 主程序路径**不再写死**，由运行时探测；服务名保持写死（从 Workstation 6 到 17.6 未变过，且服务名决定启动顺序，动态枚举反而会丢顺序）。

## 关键实现细节（踩过的坑）

- `TaskDialogIndirect` 依赖 comctl32 v6，exe 必须内嵌清单；`/manifestdependency` 在 Actions 的 cmd 里会静默失效，只有 `app.rc` + `rc.exe` + `/MANIFEST:NO` 才确定性生效
- `TDF_CALLBACK_TIMER` / `TDN_TIMER` 在部分机器上完全不触发，所以退出框的 20 秒倒计时不靠回调：由后台线程找对话框窗口（`#32770`）后发 `TDM_CLICK_BUTTON`
- 配置确认框**不设倒计时**：配置问题必须等用户明确作答
- `pszExpandedControlText` 对应展开后的链接文字，`pszCollapsedControlText` 对应收起时，写反了会出现「收起状态显示收起细节」
- 收尾必须**先逆序停服务再杀进程**，否则 SCM 恢复策略会把 `VMware NAT Service` 和 `vmnat.exe` 复活
- 退出判据是**主进程 + 托盘都没了**，只看主进程会让【否】被白选
- 服务打开权限掩码要含 `SERVICE_QUERY_CONFIG` / `SERVICE_CHANGE_CONFIG`（`0x0037`）才能读写启动类型
- **快捷方式若设成「运行方式：最小化」**，Explorer 会用 `SW_SHOWMINIMIZED` 启动进程；Windows 规定进程**第一个窗口**首次 `ShowWindow` 强制套用该值，本程序平时没有可见窗口，于是 TaskDialog 被塞到 `-32000,-32000`、只剩 `160x28`——表现就是「弹窗跑到任务栏」。解决：弹框前起线程找到 `#32770`，`SW_RESTORE` 后按显示器工作区居中；`RelaunchElevated` 的 `nShow` 也从 `SW_HIDE` 改成 `SW_SHOWNORMAL`

## 已知限制

- 服务处于 `Disabled`（禁用）时**不会**擅自启用，只记日志并在确认框里提示；需要时自己改回「手动」
- `vmnat.exe` / `vmnetdhcp.exe` 装在 `SysWOW64`，不在安装目录下，所以收尾仍按进程名清理；`vmware.exe` 等按名字精确匹配
- `vmware-vmx.exe`（虚拟机进程）分支无真机数据：只有虚拟机开机时该进程才存在，测试时未开虚拟机
- 从开始菜单或双击 `.vmx` 直接进 VMware，会因服务未启动而报「未能启动 VMware Authorization Service」，右键以管理员身份运行即可；日常走桌面快捷方式不受影响

## 版本

| 版本 | 内容 |
|---|---|
| v0.1.0 ~ v0.1.1 | Win32 移植、Actions 编译链路、日志按启动时间切文件 |
| v0.1.2 | 去掉 comctl32 依赖，自绘弹窗 |
| v0.1.3 | 恢复原生 TaskDialog 外观（对齐 AHK 原型），内嵌清单 |
| v0.1.4 | 超时语义改为自动执行【是】，倒计时不再依赖回调 |
| v0.1.5 | 安装目录探测、服务启动类型自愈、首启确认框 |
| v0.1.6 | 确认框逐项列出服务启动类型，日志措辞修正 |
| v0.1.7 | 修正展开/收起链接文字写反的问题 |
| v0.1.8 | 修正弹窗被「最小化启动」带到任务栏的问题，并统一居中显示 |
