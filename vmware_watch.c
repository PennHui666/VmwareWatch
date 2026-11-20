/* ============================================================================
 * VMware Workstation Pro 托管启动器  ——  Win32 原生程序（C 版最终形态）
 *
 * 单文件 / 单实例 / 常驻看门狗。三段式：
 *   1. 启动：自提权 -> 串行拉起 4 个服务 -> 启动 vmware.exe
 *   2. 监听：每秒判断 主窗口 / 主进程 / 托盘 三态
 *   3. 收尾：先逆序停服务，再结束进程，最多两轮补刀，然后自己退出
 *
 * 编译：
 *   MinGW-w64:
 *     x86_64-w64-mingw32-gcc vmware_watch.c -O2 -mwindows -static ^
 *       -o VmwareWatch.exe -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi
 *
 *   MSVC (Developer Command Prompt) —— 必须先把 manifest 编成资源再链接：
 *     rc /nologo /fo app.res app.rc
 *     cl /O2 /MT /utf-8 /DUNICODE /D_UNICODE /W3 vmware_watch.c app.res /Fe:VmwareWatch.exe /link ^
 *       /MANIFEST:NO advapi32.lib comctl32.lib shell32.lib user32.lib gdi32.lib shlwapi.lib
 *
 *   注意 manifest 不能省：TaskDialogIndirect 是 comctl32 v6 的序号导出，
 *   没有清单会在加载期报"无法定位序数 345"。
 *
 *   MinGW-w64：同样需要把 app.manifest 通过 .rc 编进 exe，否则 TaskDialog 不可用。
 *
 * 说明：日志写到 exe 同级目录的 log\ 下，文件名带启动时间戳（每次启动一个文件）；
 *       目录不可写时回退到 %TEMP%。
 * ==========================================================================*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS   /* _wfopen 的 C4996 噪音，这里是有意使用 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

/* WIN32_LEAN_AND_MEAN 会裁掉 shellapi.h，而 ShellExecuteExW 在那里，必须显式包含 */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

/* ---------------------------- 配置 ---------------------------- */

static const wchar_t* kVmwExe  = L"D:\\VMware Workstation\\vmware.exe";
static const wchar_t* kVmwDir  = L"D:\\VMware Workstation";

/* 启动顺序（停止时自动逆序） */
static const wchar_t* kServices[] = {
    L"VMAuthdService",
    L"VMUSBArbService",
    L"VMware NAT Service",
    L"VMnetDHCP"
};
#define SERVICE_COUNT 4

/* 收尾时需要结束的进程（先虚拟机，再界面，再托盘，最后服务进程） */
static const wchar_t* kProcs[] = {
    L"vmware-vmx.exe",
    L"vmware.exe",
    L"vmware-tray.exe",
    L"vmware-authd.exe",
    L"vmware-usbarbitrator64.exe",
    L"vmnat.exe",
    L"vmnetdhcp.exe"
};
#define PROC_COUNT 7

static const DWORD kDialogTimeoutMs = 20000;   /* 确认框倒计时 */
static const DWORD kWaitWindowSec   = 120;     /* 等主窗口出现上限 */

/* ---------------------------- 日志 ----------------------------
 * 位置：exe 同级目录下的 log\ 子目录（自动创建），文件按启动时间命名，
 *       形如 log\VmwareWatch_20261120_161230.log —— 每次启动一个文件。
 * 兜底：若 exe 所在目录不可写（例如放在 Program Files），自动回退到 %TEMP%。
 * ------------------------------------------------------------ */

#define LOGPATH_MAX 520

static wchar_t gLogPath[LOGPATH_MAX] = { 0 };
static FILE*   gLog = NULL;

/* 拼一个带启动时间戳的日志文件名，写到 out */
static void MakeLogName(wchar_t* out, size_t cap, const wchar_t* dir, const SYSTEMTIME* st)
{
    swprintf_s(out, cap, L"%s\\VmwareWatch_%04d%02d%02d_%02d%02d%02d.log",
               dir, st->wYear, st->wMonth, st->wDay, st->wHour, st->wMinute, st->wSecond);
}

/* 进程启动时调用一次；返回后 gLog 可能为 NULL（写不了日志时静默降级） */
static void LogOpen(void)
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    /* 取 exe 所在目录 */
    wchar_t exeDir[MAX_PATH];
    wcscpy_s(exeDir, MAX_PATH, exePath);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';

    wchar_t logDir[LOGPATH_MAX];
    swprintf_s(logDir, LOGPATH_MAX, L"%s\\log", exeDir);
    CreateDirectoryW(logDir, NULL);     /* 已存在则直接成功，失败也不致命 */

    SYSTEMTIME st;
    GetLocalTime(&st);

    MakeLogName(gLogPath, LOGPATH_MAX, logDir, &st);
    gLog = _wfopen(gLogPath, L"a, ccs=UTF-8");
    if (gLog) return;

    /* 回退：用户临时目录 */
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t tmpDir[MAX_PATH];
    wcscpy_s(tmpDir, MAX_PATH, tmp);
    size_t len = wcslen(tmpDir);
    if (len && tmpDir[len - 1] == L'\\') tmpDir[len - 1] = L'\0';

    MakeLogName(gLogPath, LOGPATH_MAX, tmpDir, &st);
    gLog = _wfopen(gLogPath, L"a, ccs=UTF-8");
}

static void LogW(const wchar_t* fmt, ...)
{
    if (!gLog) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(gLog, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfwprintf(gLog, fmt, ap);
    va_end(ap);
    fwprintf(gLog, L"\n");
    fflush(gLog);          /* 常驻程序，逐行落盘，异常退出也不丢关键记录 */
}

/* ---------------------------- 提权 / 单实例 ---------------------------- */

static BOOL IsElevated(void)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return FALSE;

    TOKEN_ELEVATION el;
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

static void RelaunchElevated(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";
    sei.lpFile       = exe;
    sei.lpParameters = L"";
    sei.nShow        = SW_HIDE;
    ShellExecuteExW(&sei);
}

/* ---------------------------- 服务层 ---------------------------- */

static SC_HANDLE SvcOpen(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return NULL;

    SC_HANDLE s = OpenServiceW(scm, name,
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
    CloseServiceHandle(scm);
    return s;
}

static DWORD SvcState(SC_HANDLE s)
{
    SERVICE_STATUS st;
    if (!QueryServiceStatus(s, &st)) return 0xFFFFFFFF;
    return st.dwCurrentState;   /* 1=停止 2=启动中 3=停止中 4=运行中 */
}

/* 启动服务，等待进入 RUNNING；已在运行视为成功。返回 0=成功 */
static int SvcStart(const wchar_t* name)
{
    SC_HANDLE s = SvcOpen(name);
    if (!s) { LogW(L"  打开服务失败 %s (err %lu)", name, GetLastError()); return 1; }

    if (SvcState(s) == SERVICE_RUNNING) {
        CloseServiceHandle(s);
        LogW(L"  起服务 %s -> 已在运行", name);
        return 0;
    }

    BOOL ok = StartServiceW(s, 0, NULL);
    DWORD err = GetLastError();
    if (!ok && err != ERROR_SERVICE_ALREADY_RUNNING) {
        CloseServiceHandle(s);
        LogW(L"  起服务 %s -> 启动失败 %lu", name, err);
        return 1;
    }

    for (int i = 0; i < 40; i++) {
        if (SvcState(s) == SERVICE_RUNNING) break;
        Sleep(250);
    }
    DWORD st = SvcState(s);
    CloseServiceHandle(s);
    LogW(L"  起服务 %s -> %s", name, st == SERVICE_RUNNING ? L"已启动" : L"超时");
    return st == SERVICE_RUNNING ? 0 : 1;
}

/* 停止服务，等待进入 STOPPED。返回 0=成功 */
static int SvcStop(const wchar_t* name)
{
    SC_HANDLE s = SvcOpen(name);
    if (!s) { LogW(L"  打开服务失败 %s", name); return 1; }

    DWORD state;
    for (int i = 0; i < 20; i++) {
        state = SvcState(s);
        if (state == SERVICE_STOPPED) break;
        if (state == SERVICE_RUNNING) {
            SERVICE_STATUS st;
            ControlService(s, SERVICE_CONTROL_STOP, &st);
            DWORD err = GetLastError();
            if (err && err != ERROR_SERVICE_NOT_ACTIVE) {
                CloseServiceHandle(s);
                LogW(L"  停服务 %s -> 停止失败 %lu", name, err);
                return 1;
            }
        }
        Sleep(500);
    }
    state = SvcState(s);
    CloseServiceHandle(s);
    LogW(L"  停服务 %s -> %s", name, state == SERVICE_STOPPED ? L"已停止" : L"超时");
    return state == SERVICE_STOPPED ? 0 : 1;
}

/* ---------------------------- 进程层 ---------------------------- */

static DWORD FindPid(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static BOOL KillProc(const wchar_t* name)
{
    int n = 0;
    DWORD pid;
    while ((pid = FindPid(name)) != 0 && n < 10) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        Sleep(250);
        n++;
    }
    return FindPid(name) == 0;
}

/* ---------------------------- 主窗口判定 ---------------------------- */

static DWORD gWatchPid = 0;

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == gWatchPid && IsWindowVisible(hwnd)) {
        *(HWND*)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindMainWindow(void)
{
    gWatchPid = FindPid(L"vmware.exe");
    if (!gWatchPid) return NULL;
    HWND hwnd = NULL;
    EnumWindows(EnumWinProc, (LPARAM)&hwnd);
    return hwnd;
}

/* ---------------------------- 确认框（原生 TaskDialog） ----------------------------
 * 外观与 AHK 原型一致：信息图标 + 脚注灰条 + 「是(Y)」/「否(N)」按钮。
 * 依赖 comctl32 v6，所以 exe 必须带正确嵌入的 manifest（见 app.manifest / app.rc）。
 *
 * 为什么不用 TDF_CALLBACK_TIMER：实测 TDN_TIMER 回调一次都没触发 —— 脚注不刷新、
 * 超时也不动作，对话框会一直挂着。改为开一个后台线程，到点直接给对话框发
 * TDM_CLICK_BUTTON(IDYES)，完全不依赖回调机制。
 *
 * 语义：20 秒内不操作 -> 自动执行【是】；中途可选【否】/Esc/点标题栏 X 拦下。
 * ------------------------------------------------------------------------------- */

/* 在自身进程里找当前可见的 TaskDialog 窗口（类名 #32770） */
typedef struct {
    DWORD pid;
    HWND  hwnd;
} FINDDLG;

static BOOL CALLBACK FindDlgProc(HWND hwnd, LPARAM lp)
{
    FINDDLG* f = (FINDDLG*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (_wcsicmp(cls, L"#32770") == 0) {
        f->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindTaskDialogWindow(void)
{
    FINDDLG f;
    f.pid  = GetCurrentProcessId();
    f.hwnd = NULL;
    EnumWindows(FindDlgProc, (LPARAM)&f);
    return f.hwnd;
}

/* 倒计时线程：先等对话框出现，到点给它发【是】 */
static DWORD WINAPI DlgTimerThread(LPVOID param)
{
    (void)param;

    HWND dlg = NULL;
    for (int i = 0; i < 50 && !dlg; i++) {       /* 最多 5 秒等对话框出现 */
        Sleep(100);
        dlg = FindTaskDialogWindow();
    }
    if (!dlg) return 0;

    int ticks = (int)(kDialogTimeoutMs / 100);
    for (int i = 0; i < ticks; i++) {
        Sleep(100);
        if (!IsWindow(dlg)) return 0;            /* 用户已经作答，线程自行退出 */
    }
    if (IsWindow(dlg))
        SendMessageW(dlg, TDM_CLICK_BUTTON, IDYES, 0);   /* 超时 = 按【是】执行清理 */
    return 0;
}

/* 返回 IDYES / IDNO */
static int AskExit(void)
{
    HANDLE th = CreateThread(NULL, 0, DlgTimerThread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    TASKDIALOGCONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize             = sizeof(cfg);
    cfg.hInstance          = GetModuleHandleW(NULL);
    cfg.pszWindowTitle     = L"VMware 托管启动器";
    cfg.pszMainInstruction = L"VMware 主窗口已关闭，要完全退出吗？";
    cfg.pszContent         = L"【是】 结束 VMware 全部进程（含运行中的虚拟机），并停止 4 个后台服务。\n"
                             L"【否】 保持现状：托盘图标与后台服务继续运行，与原生行为一致。";
    cfg.pszFooter          = L"20 秒内不操作，将自动选择【是】。";
    cfg.pszMainIcon        = TD_INFORMATION_ICON;
    cfg.dwCommonButtons    = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.nDefaultButton     = IDNO;           /* 焦点默认在【否】，误按回车不会误清 */
    cfg.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION |   /* Esc / 标题栏 X 按【否】 */
                             TDF_SIZE_TO_CONTENT;

    int btn = 0;
    HRESULT hr = TaskDialogIndirect(&cfg, &btn, NULL, NULL);
    if (FAILED(hr) || btn == 0)
        return IDNO;                          /* 对话框起不来就按【否】，绝不误清 */
    return (btn == IDYES) ? IDYES : IDNO;
}

/* ---------------------------- 收尾 ---------------------------- */

static void Cleanup(void)
{
    LogW(L"--- 收尾：先逆序停服务 ---");
    for (int i = SERVICE_COUNT - 1; i >= 0; i--)
        SvcStop(kServices[i]);

    LogW(L"--- 收尾：再结束进程 ---");
    for (int i = 0; i < PROC_COUNT; i++) {
        BOOL ok = KillProc(kProcs[i]);
        LogW(L"  结束 %s -> %s", kProcs[i], ok ? L"已结束" : L"仍在运行");
    }

    /* 补刀：服务/进程被恢复策略拉起时再清一轮，最多两次 */
    for (int round = 0; round < 2; round++) {
        BOOL left = FALSE;
        for (int i = 0; i < PROC_COUNT; i++)
            if (FindPid(kProcs[i]) != 0) left = TRUE;
        for (int i = SERVICE_COUNT - 1; i >= 0; i--) {
            SC_HANDLE s = SvcOpen(kServices[i]);
            if (s && SvcState(s) != SERVICE_STOPPED) { left = TRUE; SvcStop(kServices[i]); }
            if (s) CloseServiceHandle(s);
        }
        if (!left) break;
        for (int i = 0; i < PROC_COUNT; i++) KillProc(kProcs[i]);
    }
    LogW(L"--- 收尾完成 ---");
}

/* ---------------------------- 主流程 ---------------------------- */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* 1. 提权（非管理员则重启自己） */
    if (!IsElevated()) {
        RelaunchElevated();
        return 0;
    }

    /* 2. 日志 + 单实例 */
    LogOpen();
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"VmwareWatch_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogW(L"已有实例在运行，退出");
        return 0;
    }

    LogW(L"========== 启动器启动 ==========");
    LogW(L"日志文件: %s", gLogPath);

    /* 3. 启动阶段：服务 + 主程序 */
    if (FindPid(L"vmware.exe") != 0) {
        LogW(L"VMware 已在运行，跳过拉起");
    } else {
        for (int i = 0; i < SERVICE_COUNT; i++)
            SvcStart(kServices[i]);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessW(kVmwExe, NULL, NULL, NULL, FALSE, 0, NULL,
                           kVmwDir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LogW(L"已拉起 %s", kVmwExe);
        } else {
            LogW(L"启动主程序失败 %lu", GetLastError());
        }
    }

    /* 4. 等主窗口出现 */
    int waited = 0;
    while (!FindMainWindow() && waited < (int)kWaitWindowSec) {
        Sleep(1000);
        waited++;
    }
    if (!FindMainWindow()) {
        LogW(L"%lu 秒内未等到主窗口，退出（保持现状）", kWaitWindowSec);
        return 0;
    }
    LogW(L"主窗口已出现，进入监听");

    /* 5. 监听循环 */
    BOOL prompted = FALSE;
    for (;;) {
        Sleep(1000);

        /* 5.1 主进程与托盘都没了 = 真的完全退出（静默收尾） */
        if (FindPid(L"vmware.exe") == 0 && FindPid(L"vmware-tray.exe") == 0) {
            LogW(L"主进程与托盘均已退出，直接收尾");
            Cleanup();
            return 0;
        }

        /* 5.2 主窗口在（含最小化），复位“已提示”标记 */
        if (FindMainWindow()) {
            if (prompted) {
                LogW(L"主窗口已恢复，允许再次提示");
                prompted = FALSE;
            }
            continue;
        }

        /* 5.3 主窗口没了但仍有进程 = 点了 X，首次弹框 */
        if (!prompted) {
            prompted = TRUE;
            LogW(L"主窗口消失（仍有进程在跑），弹确认框");
            int r = AskExit();
            LogW(L"确认框返回: %s", r == IDYES ? L"Yes" : L"No/Timeout");
            if (r == IDYES) {
                Cleanup();
                return 0;
            }
            LogW(L"保持现状，继续监听");
        }
    }
}
