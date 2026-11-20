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
 *   MSVC (Developer Command Prompt)：
 *     cl /O2 /MT /utf-8 /DUNICODE /D_UNICODE /W3 vmware_watch.c /Fe:VmwareWatch.exe ^
 *       /link advapi32.lib shell32.lib user32.lib shlwapi.lib
 *
 *   MinGW-w64:
 *     x86_64-w64-mingw32-gcc vmware_watch.c -O2 -mwindows -static -o VmwareWatch.exe ^
 *       -ladvapi32 -lshell32 -luser32 -lshlwapi
 *
 *   两者都不再需要 comctl32 / manifest：确认框是纯 user32 自绘窗口。
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
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

/* WIN32_LEAN_AND_MEAN 会裁掉 shellapi.h，而 ShellExecuteExW 在那里，必须显式包含 */
#include <windows.h>
#include <shellapi.h>
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

/* ------------------- 确认框（纯 user32 自绘，不依赖 comctl32） -------------------
 * 原先用 TaskDialogIndirect：它是 comctl32 v6 的"仅序号导出"(ordinal 345)，
 * 必须先正确嵌入 manifest 才能加载。实测线上 exe 根本没带上 manifest，
 * 加载期就报"无法定位序数 345 于动态链接库 xxx.exe 上"。
 * 这里改成自己 CreateWindow + 定时器实现，零额外依赖，行为完全可控。
 * ------------------------------------------------------------------------- */

#define DLG_CX        460          /* 客户区基准尺寸（按 96 DPI 设计，运行时按屏幕 DPI 缩放） */
#define DLG_CY        196
#define DLG_ID_YES    1001
#define DLG_ID_NO     1002
#define DLG_ID_TIMER  1

static HWND  gDlgWnd       = NULL;
static HWND  gDlgCounter   = NULL;
static int   gDlgResult    = IDNO;
static DWORD gDlgStart     = 0;
static DWORD gDlgLimit     = 0;
static HFONT gDlgFontTitle = NULL;
static HFONT gDlgFontBody  = NULL;

static int ScreenDpi(void)
{
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return dpi ? dpi : 96;
}

/* 按屏幕 DPI 缩放一个基准像素值 */
static int Scaled(int v)
{
    return MulDiv(v, ScreenDpi(), 96);
}

static HFONT MakeFont(int pt, int weight)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(pt, ScreenDpi(), 72);
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Microsoft YaHei UI");
    return CreateFontIndirectW(&lf);
}

static void DlgFinish(int result)
{
    gDlgResult = result;
    if (gDlgWnd) DestroyWindow(gDlgWnd);
}

static LRESULT CALLBACK DlgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(h, GWLP_HINSTANCE);

        gDlgFontTitle = MakeFont(12, FW_SEMIBOLD);
        gDlgFontBody  = MakeFont(9,  FW_NORMAL);

        HWND t = CreateWindowExW(0, L"STATIC",
            L"VMware 主窗口已关闭，要完全退出吗？",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            Scaled(24), Scaled(26), Scaled(412), Scaled(26), h, NULL, hi, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)gDlgFontTitle, TRUE);

        HWND b = CreateWindowExW(0, L"STATIC",
            L"【是】 结束 VMware 全部进程（含运行中的虚拟机），并停止 4 个后台服务。\r\n"
            L"【否】 保持现状：托盘图标与后台服务继续运行，与原生行为一致。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            Scaled(24), Scaled(62), Scaled(412), Scaled(46), h, NULL, hi, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)gDlgFontBody, TRUE);

        gDlgCounter = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            Scaled(24), Scaled(118), Scaled(412), Scaled(20), h, NULL, hi, NULL);
        SendMessageW(gDlgCounter, WM_SETFONT, (WPARAM)gDlgFontBody, TRUE);

        HWND yes = CreateWindowExW(0, L"BUTTON", L"是",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            Scaled(244), Scaled(152), Scaled(88), Scaled(30), h, (HMENU)DLG_ID_YES, hi, NULL);
        HWND no  = CreateWindowExW(0, L"BUTTON", L"否",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            Scaled(340), Scaled(152), Scaled(88), Scaled(30), h, (HMENU)DLG_ID_NO, hi, NULL);
        SendMessageW(yes, WM_SETFONT, (WPARAM)gDlgFontBody, TRUE);
        SendMessageW(no,  WM_SETFONT, (WPARAM)gDlgFontBody, TRUE);

        SetFocus(no);   /* 默认焦点在【否】，误按回车也不会误清 */
        return 0;
    }

    case WM_TIMER: {
        DWORD elapsed = GetTickCount() - gDlgStart;
        DWORD remain  = (elapsed < gDlgLimit) ? gDlgLimit - elapsed : 0;
        wchar_t buf[128];
        swprintf_s(buf, 128, L"剩余 %lu 秒，超时自动选择【否】", (remain + 999) / 1000);
        SetWindowTextW(gDlgCounter, buf);
        if (elapsed >= gDlgLimit) DlgFinish(IDNO);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == DLG_ID_YES) { DlgFinish(IDYES); return 0; }
        if (LOWORD(wp) == DLG_ID_NO)  { DlgFinish(IDNO);  return 0; }
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DlgFinish(IDNO); return 0; }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == gDlgCounter) SetTextColor(dc, RGB(0x66, 0x66, 0x66));
        return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
    }

    case WM_CLOSE:      /* 点标题栏 X 也按【否】处理，绝不误清 */
        DlgFinish(IDNO);
        return 0;

    case WM_DESTROY:
        KillTimer(h, DLG_ID_TIMER);
        if (gDlgFontTitle) { DeleteObject(gDlgFontTitle); gDlgFontTitle = NULL; }
        if (gDlgFontBody)  { DeleteObject(gDlgFontBody);  gDlgFontBody  = NULL; }
        gDlgWnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 返回 IDYES / IDNO（任何异常都返回 IDNO：宁可不动，也不误清） */
static int AskExit(void)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DlgWndProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
        wc.lpszClassName = L"VmwareWatchDlg";
        if (!RegisterClassExW(&wc)) {
            LogW(L"注册确认框失败 %lu", GetLastError());
            return IDNO;
        }
        registered = TRUE;
    }

    gDlgResult = IDNO;
    gDlgStart  = GetTickCount();
    gDlgLimit  = kDialogTimeoutMs;

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc;
    rc.left = 0; rc.top = 0;
    rc.right  = Scaled(DLG_CX);
    rc.bottom = Scaled(DLG_CY);
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w   = rc.right - rc.left;
    int hgt = rc.bottom - rc.top;

    RECT wa;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - hgt) / 2;

    gDlgWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                              L"VmwareWatchDlg", L"VMware 托管启动器", style,
                              x, y, w, hgt,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!gDlgWnd) {
        LogW(L"创建确认框失败 %lu", GetLastError());
        return IDNO;
    }

    ShowWindow(gDlgWnd, SW_SHOW);
    SetForegroundWindow(gDlgWnd);
    SetTimer(gDlgWnd, DLG_ID_TIMER, 200, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return gDlgResult;
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
