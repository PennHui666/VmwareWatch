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
 *   MSVC (Developer Command Prompt) —— 必须带 manifestdependency 声明 comctl32 v6：
 *     cl /O2 /MT /DUNICODE /D_UNICODE vmware_watch.c /link ^
 *       /manifestdependency:"type='win32' name='Microsoft.Windows.Common-Controls' ^
 *       version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'" ^
 *       comctl32.lib advapi32.lib shell32.lib user32.lib shlwapi.lib
 *
 *   MinGW-w64 注意：除链接 -lcomctl32 外，还需用 .rc 资源把 comctl32 v6 清单
 *   嵌入 exe，否则 TaskDialogIndirect 入口不可用。
 *
 * 说明：日志写到 %TEMP%\VmwareWatch.log
 * ==========================================================================*/

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
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

/* ---------------------------- 日志 ---------------------------- */

static void LogW(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, MAX_PATH, L"VmwareWatch.log");

    SYSTEMTIME st;
    GetLocalTime(&st);

    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;

    fwprintf(f, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
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

/* ---------------------------- 确认框（TaskDialog） ---------------------------- */

typedef struct {
    DWORD started;
    DWORD limit;
} DlgData;

static HRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    DlgData* d = (DlgData*)ref;

    if (msg == TDN_TIMER) {
        DWORD elapsed = (DWORD)lp;                 /* 对话框打开至今的毫秒数 */
        DWORD remain  = (elapsed < d->limit) ? d->limit - elapsed : 0;
        wchar_t buf[128];
        wsprintfW(buf, L"剩余 %lu 秒，超时自动选择【否】",
                  (remain + 999) / 1000);
        SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, (LPARAM)buf);

        if (elapsed >= d->limit)
            SendMessageW(h, TDM_CLICK_BUTTON, IDNO, 0);
        return S_OK;
    }
    return S_FALSE;
}

/* 返回 IDYES / IDNO */
static int AskExit(void)
{
    DlgData d;
    d.started = GetTickCount();
    d.limit   = kDialogTimeoutMs;

    TASKDIALOGCONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize           = sizeof(cfg);
    cfg.pszWindowTitle   = L"VMware 托管启动器";
    cfg.pszMainInstruction = L"VMware 主窗口已关闭，要完全退出吗？";
    cfg.pszContent       = L"【是】 结束 VMware 全部进程（含运行中的虚拟机），并停止 4 个后台服务。\n"
                           L"【否】 保持现状：托盘图标与后台服务继续运行，与原生行为一致。";
    cfg.pszFooter        = L"剩余 20 秒，超时自动选择【否】";
    cfg.dwCommonButtons  = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.nDefaultButton   = IDNO;
    cfg.pfCallback       = DlgProc;
    cfg.lpCallbackData   = (LONG_PTR)&d;
    cfg.dwFlags          = TDF_CALLBACK_TIMER |
                           TDF_ALLOW_DIALOG_CANCELLATION |
                           TDF_SIZE_TO_CONTENT;

    int btn = 0;
    TaskDialogIndirect(&cfg, &btn, NULL, NULL);
    return btn;
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

    /* 2. 单实例 */
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"VmwareWatch_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogW(L"已有实例在运行，退出");
        return 0;
    }

    LogW(L"========== 启动器启动 ==========");

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
