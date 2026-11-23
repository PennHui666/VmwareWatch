/* ============================================================================
 * VMware Workstation Pro 托管启动器  ——  Win32 原生程序（C 版最终形态）
 *
 * 单文件 / 单实例 / 常驻看门狗。三段式：
 *   0. 探测：偏好文件 -> 注册表 InstallPath -> 服务 ImagePath -> 默认路径
 *      并检测 4 个服务的启动类型 / 托盘自启项，按用户偏好静默自愈
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
 *       /MANIFEST:NO advapi32.lib comctl32.lib comdlg32.lib shell32.lib user32.lib gdi32.lib shlwapi.lib
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
#include <commdlg.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

/* ---------------------------- 配置 ---------------------------- */

/* 安装目录与主程序路径：运行时探测（不再写死盘符，兼容装在 D:\只装 C:\ 等情形） */
static wchar_t gVmwDir[MAX_PATH]      = { 0 };
static wchar_t gVmwExe[MAX_PATH]      = { 0 };
static wchar_t gVmwProcName[MAX_PATH] = L"vmware.exe";   /* 进程判据用 exe 名 */

/* 偏好 / 备份文件：默认与 exe 同目录，不可写时回退 %APPDATA%\VmwareWatch */
static wchar_t gStateDir[MAX_PATH]   = { 0 };
static wchar_t gPrefPath[MAX_PATH]   = { 0 };
static wchar_t gBackupPath[MAX_PATH] = { 0 };

/* 偏好：用户对「服务改手动 + 去自启」的态度 */
#define PREF_ASK     0    /* 还没决定过，启动时问一次 */
#define PREF_MANUAL  1    /* 已授权：每次启动静默自愈 */
#define PREF_KEEP    2    /* 明确不要动 */
#define SERVICE_COUNT 4   /* 4 个常驻后台服务 */
static int     gPrefState = PREF_ASK;
static wchar_t gPrefDir[MAX_PATH] = { 0 };   /* 用户手工指定过的安装目录 */
static int     gDisabledCount = 0;           /* 本轮扫描到的 Disabled 服务数 */
static DWORD   gSvcType[SERVICE_COUNT];      /* 每个服务当前启动类型（供确认框展示） */

/* 启动顺序（停止时自动逆序） */
static const wchar_t* kServices[] = {
    L"VMAuthdService",
    L"VMUSBArbService",
    L"VMware NAT Service",
    L"VMnetDHCP"
};

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
    sei.nShow        = SW_SHOWNORMAL;   /* 不用 SW_HIDE：首个窗口会被这套启动标志左右显示状态 */
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

/* ---------------------------- 路径工具 ---------------------------- */

static void GetExeDir(wchar_t* out, size_t cap)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy_s(out, cap, exe);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

static BOOL FileExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void TrimTrailingSlash(wchar_t* s)
{
    size_t len = wcslen(s);
    while (len > 3 && (s[len - 1] == L'\\' || s[len - 1] == L'/')) {
        s[--len] = L'\0';
    }
}

/* ---------------------------- 偏好 / 备份文件 ---------------------------- */

/* 定出 state 目录（优先 exe 同级，不可写则 %APPDATA%\VmwareWatch） */
static void StateInit(void)
{
    wchar_t exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);

    wchar_t probe[MAX_PATH];
    swprintf_s(probe, MAX_PATH, L"%s\\.write-probe.tmp", exeDir);
    FILE* f = _wfopen(probe, L"w");
    if (f) {
        fclose(f);
        DeleteFileW(probe);
        wcscpy_s(gStateDir, MAX_PATH, exeDir);
    } else {
        wchar_t app[MAX_PATH] = { 0 };
        GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
        if (app[0]) {
            swprintf_s(gStateDir, MAX_PATH, L"%s\\VmwareWatch", app);
            CreateDirectoryW(gStateDir, NULL);
        } else {
            wcscpy_s(gStateDir, MAX_PATH, exeDir);   /* 兜底：仍然用 exe 目录 */
        }
    }

    swprintf_s(gPrefPath, MAX_PATH, L"%s\\VmwareWatch.prefs.ini", gStateDir);
    swprintf_s(gBackupPath, MAX_PATH, L"%s\\service-backup.txt", gStateDir);
    LogW(L"状态目录: %s", gStateDir);
}

static void SavePrefs(void)
{
    FILE* f = _wfopen(gPrefPath, L"w, ccs=UTF-8");
    if (!f) { LogW(L"偏好写入失败: %s", gPrefPath); return; }

    const wchar_t* st = (gPrefState == PREF_MANUAL) ? L"manual"
                      : (gPrefState == PREF_KEEP)   ? L"keep" : L"ask";
    fwprintf(f, L"state=%s\n", st);
    if (gPrefDir[0]) fwprintf(f, L"vmware_dir=%s\n", gPrefDir);
    fclose(f);
    LogW(L"偏好已保存: state=%s", st);
}

static void LoadPrefs(void)
{
    gPrefState = PREF_ASK;
    gPrefDir[0] = L'\0';

    FILE* f = _wfopen(gPrefPath, L"r, ccs=UTF-8");
    if (!f) { LogW(L"首选启动：无偏好文件，按「未决定」处理"); return; }

    wchar_t line[600];
    while (fgetws(line, 600, f)) {
        size_t len = wcslen(line);
        while (len && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) line[--len] = L'\0';

        wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') p++;

        if (wcsncmp(p, L"state=", 6) == 0) {
            wchar_t* v = p + 6;
            if (_wcsicmp(v, L"manual") == 0)      gPrefState = PREF_MANUAL;
            else if (_wcsicmp(v, L"keep") == 0)   gPrefState = PREF_KEEP;
            else                                  gPrefState = PREF_ASK;
        } else if (wcsncmp(p, L"vmware_dir=", 11) == 0) {
            wcscpy_s(gPrefDir, MAX_PATH, p + 11);
            TrimTrailingSlash(gPrefDir);
        }
    }
    fclose(f);
    LogW(L"偏好已载入: state=%s, vmware_dir=%s",
         gPrefState == PREF_MANUAL ? L"manual" : (gPrefState == PREF_KEEP ? L"keep" : L"ask"),
         gPrefDir[0] ? gPrefDir : L"(未指定)");
}

/* 只在真正改动系统配置时写一条，用于日后手工回滚 */
static void BackupAppend(const wchar_t* fmt, ...)
{
    BOOL isNew = !FileExists(gBackupPath);
    FILE* f = _wfopen(gBackupPath, L"a, ccs=UTF-8");
    if (!f) return;

    if (isNew) {
        fwprintf(f, L"# VMware 托管启动器 —— 配置改动记录（可直接照着回滚）\n");
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

/* ---------------------------- 安装目录探测 ---------------------------- */

/* 校验候选目录里真的有 vmware.exe；通过则落定 gVmwDir / gVmwExe */
static BOOL TryDir(const wchar_t* dir, const wchar_t* why)
{
    if (!dir || !*dir) return FALSE;

    wchar_t cand[MAX_PATH];
    wcscpy_s(cand, MAX_PATH, dir);
    TrimTrailingSlash(cand);

    wchar_t exe[MAX_PATH];
    swprintf_s(exe, MAX_PATH, L"%s\\vmware.exe", cand);
    if (!FileExists(exe)) {
        LogW(L"  候选无效: %s（来源: %s）", cand, why);
        return FALSE;
    }

    wcscpy_s(gVmwDir, MAX_PATH, cand);
    wcscpy_s(gVmwExe, MAX_PATH, exe);
    LogW(L"安装目录: %s（来源: %s）", cand, why);
    return TRUE;
}

/* 注册表 InstallPath（VMware 是 32 位程序，主键在 WOW6432Node 下；两个视图都试） */
static BOOL RegInstallPath(wchar_t* out, size_t cap)
{
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD sz = (DWORD)cap * sizeof(wchar_t);

    LONG r = RegGetValueW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\WOW6432Node\\VMware, Inc.\\VMware Workstation",
                          L"InstallPath", RRF_RT_REG_SZ, NULL, buf, &sz);
    if (r != ERROR_SUCCESS) {
        sz = (DWORD)cap * sizeof(wchar_t);
        r = RegGetValueW(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\VMware, Inc.\\VMware Workstation",
                         L"InstallPath", RRF_RT_REG_SZ, NULL, buf, &sz);
    }
    if (r != ERROR_SUCCESS || !buf[0]) return FALSE;

    wcscpy_s(out, cap, buf);
    TrimTrailingSlash(out);
    return TRUE;
}

/* 从 VMAuthdService 的 ImagePath 反推安装目录（SCM 记的路径最可信） */
static BOOL DirFromServiceImagePath(wchar_t* out, size_t cap)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { LogW(L"  OpenSCManager 失败 %lu", GetLastError()); return FALSE; }

    SC_HANDLE s = OpenServiceW(scm, L"VMAuthdService", SERVICE_QUERY_CONFIG);
    CloseServiceHandle(scm);
    if (!s) { LogW(L"  VMAuthdService 不可查询，无法反推目录"); return FALSE; }

    BYTE buf[8192];
    LPQUERY_SERVICE_CONFIGW q = (LPQUERY_SERVICE_CONFIGW)buf;
    DWORD need = 0;
    BOOL ok = QueryServiceConfigW(s, q, (DWORD)sizeof(buf), &need);
    DWORD err = GetLastError();
    CloseServiceHandle(s);
    if (!ok) { LogW(L"  QueryServiceConfig 失败 %lu", err); return FALSE; }

    wchar_t img[MAX_PATH];
    wcscpy_s(img, MAX_PATH, q->lpBinaryPathName);

    wchar_t* p = img;
    if (*p == L'"') {
        p++;
        wchar_t* e = wcschr(p, L'"');
        if (e) *e = L'\0';
    } else {
        wchar_t* sp = wcsstr(p, L" -");
        if (sp) *sp = L'\0';
    }

    wchar_t* slash = wcsrchr(p, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';

    wcscpy_s(out, cap, p);
    TrimTrailingSlash(out);
    LogW(L"  服务路径反推: %s", out);
    return TRUE;
}

/* 依次尝试四个来源，全部校验 vmware.exe 真实存在 */
static BOOL DetectInstallDir(void)
{
    wchar_t buf[MAX_PATH];

    if (TryDir(gPrefDir, L"偏好文件记录")) return TRUE;
    if (RegInstallPath(buf, MAX_PATH) && TryDir(buf, L"注册表 InstallPath")) return TRUE;
    if (DirFromServiceImagePath(buf, MAX_PATH) && TryDir(buf, L"服务 ImagePath")) return TRUE;
    if (TryDir(L"C:\\Program Files (x86)\\VMware\\VMware Workstation", L"默认路径")) return TRUE;
    if (TryDir(L"C:\\Program Files\\VMware\\VMware Workstation", L"默认路径")) return TRUE;

    LogW(L"安装目录探测失败（四个来源均未命中）");
    return FALSE;
}

/* 让用户手工挑一次 vmware.exe，成功后记进偏好 */
static void EnsureDialogOnScreen(void);   /* 定义在下方「对话框兜底定位」一节 */

static BOOL AskPickInstallDir(void)
{
    wchar_t file[MAX_PATH] = { 0 };

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"VMware 主程序 (vmware.exe)\0vmware.exe\0可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"请选择 VMware 安装目录下的 vmware.exe";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    EnsureDialogOnScreen();
    if (!GetOpenFileNameW(&ofn)) { LogW(L"用户在文件选择框取消"); return FALSE; }

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, file);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';

    if (!TryDir(dir, L"用户手工指定")) {
        LogW(L"用户选择的目录里没有 vmware.exe: %s", dir);
        return FALSE;
    }

    wcscpy_s(gPrefDir, MAX_PATH, gVmwDir);
    SavePrefs();
    LogW(L"用户指定目录已记入偏好: %s", gPrefDir);
    return TRUE;
}

/* ---------------------------- 对话框兜底定位 ----------------------------
 * 起因：快捷方式若被设成「运行方式：最小化」，资源管理器会以
 * STARTF_USESHOWWINDOW + SW_SHOWMINIMIZED 启动本进程；而 Windows 有个老规矩——
 * 进程**第一个**窗口的首次 ShowWindow 会被强制套用 STARTUPINFO 里的显示方式。
 * 本程序平时没有任何可见窗口，于是 TaskDialog 就成了那个"第一个窗口"，
 * 结果整块弹窗被塞到 -32000,-32000、只有 160x28 —— 表现为"最小化到任务栏"。
 *
 * 对策：弹框前起一个守护线程，等窗口出现后还原、再按显示器工作区居中。
 * 顺带也解决"弹窗不在屏幕中央"的问题。
 * ---------------------------------------------------------------------- */

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

static DWORD WINAPI FixupDialogThread(LPVOID param)
{
    (void)param;

    HWND dlg = NULL;
    for (int i = 0; i < 50 && !dlg; i++) {     /* 最多等 5 秒 */
        Sleep(100);
        dlg = FindTaskDialogWindow();
    }
    if (!dlg) return 0;

    /* 1) 先把它从"最小化/隐藏"状态拉回正常 */
    if (IsIconic(dlg))          ShowWindow(dlg, SW_RESTORE);
    if (!IsWindowVisible(dlg))  ShowWindow(dlg, SW_SHOWNORMAL);

    /* 2) 等布局完成（刚创建时拿到的是 0x0 或图标尺寸），再居中 */
    RECT rc = { 0, 0, 0, 0 };
    for (int i = 0; i < 20; i++) {
        if (!IsWindow(dlg)) return 0;
        GetWindowRect(dlg, &rc);
        if ((rc.right - rc.left) > 80 && (rc.bottom - rc.top) > 60) break;
        Sleep(50);
    }

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w > 80 && h > 60) {
        HMONITOR mon = MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            int x = mi.rcWork.left + ((mi.rcWork.right  - mi.rcWork.left) - w) / 2;
            int y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top)  - h) / 2;
            SetWindowPos(dlg, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
        }
    }

    SetForegroundWindow(dlg);
    LogW(L"对话框已还原并居中（%d x %d）", w, h);
    return 0;
}

/* 每个 TaskDialog / 文件选择框弹出前调用一次 */
static void EnsureDialogOnScreen(void)
{
    HANDLE th = CreateThread(NULL, 0, FixupDialogThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

/* ---------------------------- 安装目录错误框 ---------------------------- */

/* 返回 TRUE=用户想浏览；FALSE=退出 */
static BOOL AskInstallDir(void)
{
    TASKDIALOG_BUTTON b[] = {
        { 101, L"浏览…" },
        { 102, L"退出"   }
    };

    TASKDIALOGCONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize             = sizeof(cfg);
    cfg.hInstance          = GetModuleHandleW(NULL);
    cfg.pszWindowTitle     = L"VMware 托管启动器";
    cfg.pszMainInstruction = L"未能定位 VMware Workstation 安装目录";
    cfg.pszContent         = L"已依次尝试：偏好文件记录、注册表 InstallPath、VMAuthdService 服务路径、默认安装路径，"
                             L"均未找到 vmware.exe。\n可以手动指定一次，之后会记住这个位置。";
    cfg.pszMainIcon        = TD_ERROR_ICON;
    cfg.pButtons           = b;
    cfg.cButtons           = 2;
    cfg.nDefaultButton     = 101;
    cfg.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;

    int btn = 0;
    EnsureDialogOnScreen();
    HRESULT hr = TaskDialogIndirect(&cfg, &btn, NULL, NULL);
    if (FAILED(hr) || btn == 0 || btn == 102) return FALSE;
    return TRUE;
}

/* ---------------------------- 服务启动类型：检测 / 静默自愈 ---------------------------- */

static SC_HANDLE SvcOpenCfg(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return NULL;
    SC_HANDLE s = OpenServiceW(scm, name,
        SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);
    CloseServiceHandle(scm);
    return s;
}

/* 返回启动类型；服务不存在/打不开返回 0xFFFFFFFF */
static DWORD SvcStartType(const wchar_t* name)
{
    SC_HANDLE s = SvcOpenCfg(name);
    if (!s) return 0xFFFFFFFF;

    BYTE buf[8192];
    LPQUERY_SERVICE_CONFIGW q = (LPQUERY_SERVICE_CONFIGW)buf;
    DWORD need = 0;
    DWORD type = 0xFFFFFFFF;

    if (QueryServiceConfigW(s, q, (DWORD)sizeof(buf), &need))
        type = q->dwStartType;
    else
        LogW(L"  读启动类型失败 %s (err %lu)", name, GetLastError());

    CloseServiceHandle(s);
    return type;
}

static BOOL SvcMakeManual(const wchar_t* name)
{
    SC_HANDLE s = SvcOpenCfg(name);
    if (!s) {
        LogW(L"  改启动类型失败 %s：打不开服务 (err %lu)", name, GetLastError());
        return FALSE;
    }

    BOOL ok = ChangeServiceConfigW(s,
                                   SERVICE_NO_CHANGE,          /* 服务类型不改 */
                                   SERVICE_DEMAND_START,       /* 改为「手动」 */
                                   SERVICE_NO_CHANGE,          /* 错误控制不改 */
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    DWORD err = GetLastError();
    CloseServiceHandle(s);

    if (!ok) { LogW(L"  改启动类型失败 %s (err %lu)", name, err); return FALSE; }
    LogW(L"  启动类型 %s: Auto -> Manual", name);
    return TRUE;
}

/* 扫 4 个服务；fix=TRUE 时把 Auto 改 Manual。返回 Auto 的数量 */
static int ScanServices(BOOL fix)
{
    int autoCnt = 0;
    gDisabledCount = 0;

    for (int i = 0; i < SERVICE_COUNT; i++) {
        const wchar_t* n = kServices[i];
        DWORD t = SvcStartType(n);
        gSvcType[i] = t;

        if (t == 0xFFFFFFFF) {
            LogW(L"  服务 %s 不存在或无法访问，跳过", n);
        } else if (t == SERVICE_AUTO_START) {
            autoCnt++;
            if (fix) {
                if (SvcMakeManual(n)) {
                    gSvcType[i] = SERVICE_DEMAND_START;
                    BackupAppend(L"服务 %s：原始启动类型=Auto，已改为 Manual（回滚: sc config \"%s\" start= auto）", n, n);
                }
            } else {
                LogW(L"  服务 %s 当前为 Auto（待纠正）", n);
            }
        } else if (t == SERVICE_DISABLED) {
            gDisabledCount++;
            LogW(L"  服务 %s 已被禁用（Disabled），不擅自启用", n);
        } else {
            LogW(L"  服务 %s 启动类型已是 Manual，跳过", n);
        }
    }
    return autoCnt;
}

/* ---------------------------- 托盘开机自启项 ---------------------------- */

static const wchar_t* kRunKey    = L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kTrayValue = L"vmware-tray.exe";

/* 存在则把原值带回 out */
static BOOL TrayAutostartValue(wchar_t* out, size_t cap)
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return FALSE;

    wchar_t buf[512] = { 0 };
    DWORD sz = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(h, kTrayValue, NULL, &type, (BYTE*)buf, &sz);
    RegCloseKey(h);

    if (r != ERROR_SUCCESS) return FALSE;
    if (out && cap) wcscpy_s(out, cap, buf);
    return TRUE;
}

static BOOL RemoveTrayAutostart(void)
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        LogW(L"  移除自启项失败：打开 Run 键失败 (err %lu)", GetLastError());
        return FALSE;
    }

    LONG r = RegDeleteValueW(h, kTrayValue);
    RegCloseKey(h);

    if (r == ERROR_SUCCESS)  { LogW(L"  已移除开机自启项 %s", kTrayValue); return TRUE; }
    if (r == ERROR_FILE_NOT_FOUND) { LogW(L"  自启项本来就不存在"); return FALSE; }
    LogW(L"  移除自启项失败 (err %ld)", r);
    return FALSE;
}

/* ---------------------------- 首次运行的可优化项确认框 ---------------------------- */

/* 返回 101=按推荐处理 / 102=保持原样；*pDontAsk 带回「不再询问」勾选状态 */
static int AskOptimize(int autoCnt, int disabledCnt, BOOL hasTray, BOOL* pDontAsk)
{
    (void)disabledCnt;   /* 禁用服务数已并入展开区的逐项列表 */

    const wchar_t* trayLine = hasTray
        ? L"检测到 vmware-tray.exe 开机自启项，建议移除，避免开机常驻托盘"
        : L"开机自启项已为空（无需处理）";

    wchar_t content[1200];
    if (autoCnt >= SERVICE_COUNT) {
        swprintf_s(content, 1200,
            L"检测到以下可优化项：\n"
            L"・4 个 VMware 后台服务当前均为「自动」启动，建议改为「手动」，只在本启动器拉起时运行\n"
            L"・%s\n\n"
            L"改为「手动」后，开机不再自动加载这些服务；日常通过桌面快捷方式启动完全不受影响。",
            trayLine);
    } else {
        swprintf_s(content, 1200,
            L"检测到以下可优化项：\n"
            L"・4 个 VMware 后台服务中有 %d 个当前为「自动」启动，建议改为「手动」，只在本启动器拉起时运行\n"
            L"・%s\n\n"
            L"改为「手动」后，开机不再自动加载这些服务；日常通过桌面快捷方式启动完全不受影响。",
            autoCnt, trayLine);
    }

    /* 展开区：逐个列出服务当前类型，让用户看得见到底改什么 */
    wchar_t expand[1100];
    swprintf_s(expand, 1100, L"各服务当前启动类型：");
    for (int i = 0; i < SERVICE_COUNT; i++) {
        const wchar_t* t = (gSvcType[i] == SERVICE_AUTO_START)   ? L"自动（将被改为手动）"
                         : (gSvcType[i] == SERVICE_DEMAND_START) ? L"手动（无需处理）"
                         : (gSvcType[i] == SERVICE_DISABLED)     ? L"禁用（不会擅自启用）"
                         :                                         L"不可访问（将跳过）";
        wchar_t item[220];
        swprintf_s(item, 220, L"\n・%s —— %s", kServices[i], t);
        wcscat_s(expand, 1100, item);
    }
    wcscat_s(expand, 1100,
        L"\n\n改动前的原始值会记录到 service-backup.txt，可照着文件里的命令回滚。"
        L"\n选择「保持原样」则本次不做任何系统改动，服务仍在开机时自动启动。");

    TASKDIALOG_BUTTON b[] = {
        { 101, L"按推荐处理" },
        { 102, L"保持原样"   }
    };

    TASKDIALOGCONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize                   = sizeof(cfg);
    cfg.hInstance                = GetModuleHandleW(NULL);
    cfg.pszWindowTitle           = L"VMware 托管启动器";
    cfg.pszMainInstruction       = L"检测到可优化的启动设置，是否按推荐方式处理？";
    cfg.pszContent               = content;
    cfg.pszMainIcon              = TD_INFORMATION_ICON;
    cfg.pButtons                 = b;
    cfg.cButtons                 = 2;
    cfg.nDefaultButton           = 101;
    cfg.pszExpandedInformation   = expand;
    cfg.pszExpandedControlText   = L"收起细节";   /* 展开后显示的链接文字 */
    cfg.pszCollapsedControlText  = L"查看细节";   /* 收起时显示的链接文字 */
    cfg.pszVerificationText      = L"以后按这个决定自动处理，不再询问";
    cfg.dwFlags                  = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;

    int btn = 0, checked = 0;
    EnsureDialogOnScreen();
    HRESULT hr = TaskDialogIndirect(&cfg, &btn, NULL, &checked);
    if (pDontAsk) *pDontAsk = (checked != 0);
    if (FAILED(hr) || btn == 0) return 102;   /* 框起不来就当作用户不要改 */
    return btn;
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
    gWatchPid = FindPid(gVmwProcName);
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
    EnsureDialogOnScreen();
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

    /* 3. 偏好 + 安装目录探测（不再写死路径） */
    StateInit();
    LoadPrefs();

    if (!DetectInstallDir()) {
        LogW(L"安装目录探测失败，请用户介入");
        BOOL got = FALSE;
        for (;;) {
            if (!AskInstallDir()) {
                LogW(L"用户选择退出（未能定位安装目录）");
                return 0;
            }
            if (AskPickInstallDir()) { got = TRUE; break; }
            LogW(L"用户未选中有效 vmware.exe，再次询问");
        }
        if (!got) return 0;
    }

    /* 4. 配置检测：服务启动类型 + 托盘自启项，按偏好自愈 */
    int autoCnt = ScanServices(FALSE);           /* 先只扫，不动手 */
    wchar_t trayVal[512] = { 0 };
    BOOL hasTray = TrayAutostartValue(trayVal, 512);

    BOOL fix = (gPrefState == PREF_MANUAL);

    if (gPrefState == PREF_ASK && (autoCnt > 0 || hasTray)) {
        LogW(L"未决定过偏好：发现 %d 个 Auto 服务 / 自启项%s，弹确认框", autoCnt, hasTray ? L"存在" : L"无");
        BOOL dontAsk = FALSE;
        int btn = AskOptimize(autoCnt, gDisabledCount, hasTray, &dontAsk);
        fix = (btn == 101);
        gPrefState = fix ? PREF_MANUAL : PREF_KEEP;
        LogW(L"确认框返回: %s, 不再询问=%s", fix ? L"按推荐处理" : L"保持原样", dontAsk ? L"是" : L"否");

        if (dontAsk) SavePrefs();
        else LogW(L"用户未勾选「不再询问」，本次按所选执行，下次仍会提示");
    } else if (gPrefState == PREF_ASK) {
        LogW(L"配置已是推荐状态（无 Auto 服务、无自启项），无需提示");
    }

    if (fix) {
        if (autoCnt > 0) ScanServices(TRUE);     /* 真正改动手 */
        if (hasTray) {
            BackupAppend(L"开机自启项 %s：原值=%s，已移除（回滚: reg add \"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run\" /v %s /t REG_SZ /d \"%s\" /f）",
                         kTrayValue, trayVal, kTrayValue, trayVal);
            RemoveTrayAutostart();
        }
    }

    /* 5. 启动阶段：服务 + 主程序 */
    if (FindPid(gVmwProcName) != 0) {
        LogW(L"VMware 已在运行，跳过拉起");
    } else {
        for (int i = 0; i < SERVICE_COUNT; i++)
            SvcStart(kServices[i]);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessW(gVmwExe, NULL, NULL, NULL, FALSE, 0, NULL,
                           gVmwDir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LogW(L"已拉起 %s", gVmwExe);
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
        if (FindPid(gVmwProcName) == 0 && FindPid(L"vmware-tray.exe") == 0) {
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
