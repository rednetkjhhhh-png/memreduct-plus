#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_TITLE L"Mem Reduct Plus - PC Optimizer"
#define APP_REG L"Software\\MemReductPlus\\Optimizer"
#define STARTUP_BACKUP L"Software\\MemReductPlus\\Optimizer\\StartupBackups"
#define WM_REFRESH (WM_APP + 1)
#define TIMER_AUTO_MEMORY 101
#define AUTO_INTERVAL_MS (15 * 60 * 1000)

enum {
    ID_STATUS = 1001,
    ID_DIAG = 1002,
    ID_MEMORY = 1003,
    ID_AUTO = 1004,
    ID_STARTUP = 1005,
    ID_PROCESS = 1006,
    ID_DISK = 1007,
    ID_PERF = 1008,
    ID_COMPARE = 1009,
    ID_RESTORE = 1010,
    ID_REFRESH = 1011,
    ID_LOG = 1012
};

typedef struct METRICS {
    DWORD memoryLoad;
    ULONGLONG availPhys;
    ULONGLONG totalPhys;
    ULONGLONG diskFree;
    ULONGLONG diskTotal;
    DWORD processCount;
    DWORD startupCount;
} METRICS;

static HWND g_hwnd;
static HWND g_status;
static HWND g_diag;
static BOOL g_autoMemory = FALSE;
static BOOL g_haveBefore = FALSE;
static METRICS g_before;

static void SetTextFmt(HWND hwnd, const wchar_t *fmt, ...)
{
    wchar_t buffer[8192];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(buffer, ARRAYSIZE(buffer), fmt, ap);
    va_end(ap);
    SetWindowTextW(hwnd, buffer);
}

static void AppendText(HWND hwnd, const wchar_t *text)
{
    int len = GetWindowTextLengthW(hwnd);
    SendMessageW(hwnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hwnd, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static void GetLogPath(wchar_t *path, size_t cch)
{
    wchar_t base[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base));
    if (!n || n >= ARRAYSIZE(base)) GetTempPathW(ARRAYSIZE(base), base);
    StringCchPrintfW(path, cch, L"%s\\MemReductPlus", base);
    CreateDirectoryW(path, NULL);
    StringCchCatW(path, cch, L"\\changes.log");
}

static void LogChange(const wchar_t *action, const wchar_t *detail)
{
    wchar_t path[MAX_PATH];
    wchar_t line[2048];
    SYSTEMTIME st;
    HANDLE h;
    DWORD written;
    char utf8[4096];
    int bytes;

    GetLogPath(path, ARRAYSIZE(path));
    GetLocalTime(&st);
    StringCchPrintfW(line, ARRAYSIZE(line), L"%04u-%02u-%02u %02u:%02u:%02u\t%s\t%s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        action ? action : L"", detail ? detail : L"");
    bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (bytes <= 1) return;
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, utf8, (DWORD)(bytes - 1), &written, NULL);
    CloseHandle(h);
}

static BOOL ReadMetrics(METRICS *m)
{
    MEMORYSTATUSEX ms;
    ULARGE_INTEGER freeBytes, totalBytes;
    DWORD pids[4096];
    DWORD needed = 0;
    HKEY k;
    DWORD values = 0;

    ZeroMemory(m, sizeof(*m));
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return FALSE;
    m->memoryLoad = ms.dwMemoryLoad;
    m->availPhys = ms.ullAvailPhys;
    m->totalPhys = ms.ullTotalPhys;

    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, NULL)) {
        m->diskFree = freeBytes.QuadPart;
        m->diskTotal = totalBytes.QuadPart;
    }

    if (EnumProcesses(pids, sizeof(pids), &needed)) m->processCount = needed / sizeof(DWORD);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
        m->startupCount += values;
        RegCloseKey(k);
    }
    values = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
        m->startupCount += values;
        RegCloseKey(k);
    }
    return TRUE;
}

static int HealthScore(const METRICS *m)
{
    int score = 100;
    if (m->memoryLoad >= 90) score -= 35;
    else if (m->memoryLoad >= 80) score -= 25;
    else if (m->memoryLoad >= 70) score -= 15;
    if (m->diskTotal) {
        int freePct = (int)(m->diskFree * 100 / m->diskTotal);
        if (freePct < 5) score -= 30;
        else if (freePct < 10) score -= 20;
        else if (freePct < 20) score -= 10;
    }
    if (m->startupCount > 25) score -= 15;
    else if (m->startupCount > 15) score -= 8;
    if (m->processCount > 300) score -= 10;
    else if (m->processCount > 220) score -= 5;
    if (score < 0) score = 0;
    return score;
}

static void RefreshDashboard(void)
{
    METRICS m;
    SYSTEM_INFO si;
    wchar_t computer[256] = {0};
    DWORD cch = ARRAYSIZE(computer);
    ULONGLONG up = GetTickCount64() / 1000;
    int score;
    int diskPct = 0;

    if (!ReadMetrics(&m)) return;
    score = HealthScore(&m);
    if (m.diskTotal) diskPct = (int)(m.diskFree * 100 / m.diskTotal);
    GetSystemInfo(&si);
    GetComputerNameW(computer, &cch);

    SetTextFmt(g_status,
        L"PC 상태 점수: %d/100\r\n메모리 사용률: %lu%%   사용 가능: %.1f GB / %.1f GB\r\n"
        L"C: 여유 공간: %.1f GB / %.1f GB (%d%%)\r\n프로세스: %lu개   시작 프로그램(Run): %lu개",
        score, m.memoryLoad,
        (double)m.availPhys / 1073741824.0, (double)m.totalPhys / 1073741824.0,
        (double)m.diskFree / 1073741824.0, (double)m.diskTotal / 1073741824.0, diskPct,
        m.processCount, m.startupCount);

    SetTextFmt(g_diag,
        L"시스템 진단\r\n"
        L"컴퓨터: %s\r\nCPU 논리 프로세서: %lu\r\n업타임: %llu시간 %llu분\r\n"
        L"자동 메모리 정리: %s (15분 간격)\r\n\r\n"
        L"안전 정책\r\n- Windows Update, Defender, 서비스, 드라이버는 변경하지 않습니다.\r\n"
        L"- 디스크 정리는 임시 폴더의 오래된 파일만 대상으로 합니다.\r\n"
        L"- 시작 프로그램 변경과 성능 설정은 백업 후 적용됩니다.\r\n"
        L"- '전체 원상복구'에서 프로그램이 변경한 설정을 되돌릴 수 있습니다.",
        computer, si.dwNumberOfProcessors, up / 3600, (up % 3600) / 60,
        g_autoMemory ? L"켜짐" : L"꺼짐");
}

static SIZE_T CleanMemory(void)
{
    DWORD pids[4096];
    DWORD needed = 0;
    SIZE_T reclaimedEstimate = 0;
    DWORD self = GetCurrentProcessId();

    if (!EnumProcesses(pids, sizeof(pids), &needed)) return 0;
    for (DWORD i = 0; i < needed / sizeof(DWORD); ++i) {
        HANDLE p;
        PROCESS_MEMORY_COUNTERS_EX before, after;
        if (!pids[i] || pids[i] == self) continue;
        p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, pids[i]);
        if (!p) continue;
        ZeroMemory(&before, sizeof(before));
        ZeroMemory(&after, sizeof(after));
        before.cb = sizeof(before);
        after.cb = sizeof(after);
        if (GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&before, sizeof(before))) {
            EmptyWorkingSet(p);
            if (GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&after, sizeof(after)) && before.WorkingSetSize > after.WorkingSetSize)
                reclaimedEstimate += before.WorkingSetSize - after.WorkingSetSize;
        }
        CloseHandle(p);
    }
    EmptyWorkingSet(GetCurrentProcess());
    LogChange(L"MEMORY_CLEAN", L"Trimmed accessible process working sets");
    RefreshDashboard();
    return reclaimedEstimate;
}

static BOOL ContainsI(const wchar_t *s, const wchar_t *token)
{
    size_t ls, lt;
    if (!s || !token) return FALSE;
    ls = wcslen(s); lt = wcslen(token);
    if (!lt || lt > ls) return FALSE;
    for (size_t i = 0; i + lt <= ls; ++i)
        if (CompareStringOrdinal(s + i, (int)lt, token, (int)lt, TRUE) == CSTR_EQUAL) return TRUE;
    return FALSE;
}

static BOOL IsProtectedStartup(const wchar_t *name, const wchar_t *cmd)
{
    const wchar_t *tokens[] = {L"securityhealth",L"defender",L"antivirus",L"realtek",L"synaptics",L"touchpad",L"elan",L"bluetooth",L"intel",L"amd",L"nvidia",L"bitlocker",L"hotkey",L"credential",L"audio"};
    for (size_t i = 0; i < ARRAYSIZE(tokens); ++i)
        if (ContainsI(name, tokens[i]) || ContainsI(cmd, tokens[i])) return TRUE;
    return FALSE;
}

static BOOL IsRecommendedStartup(const wchar_t *name, const wchar_t *cmd)
{
    const wchar_t *tokens[] = {L"discord",L"steam",L"spotify",L"teams",L"slack",L"zoom",L"skype",L"epic",L"gog",L"battle.net",L"creative cloud",L"adobe",L"whatsapp",L"telegram",L"qbittorrent",L"phone link",L"yourphone"};
    for (size_t i = 0; i < ARRAYSIZE(tokens); ++i)
        if (ContainsI(name, tokens[i]) || ContainsI(cmd, tokens[i])) return TRUE;
    return FALSE;
}

static void BackupStartupValue(const wchar_t *name, const BYTE *data, DWORD size, BOOL existed)
{
    HKEY k;
    wchar_t safe[260];
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < ARRAYSIZE(safe); ++i) {
        wchar_t ch = name[i];
        safe[j++] = (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') ? L'_' : ch;
    }
    safe[j] = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_BACKUP, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    {
        wchar_t valueName[300];
        DWORD flag = existed ? 1 : 0;
        StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"%s.exists", safe);
        RegSetValueExW(k, valueName, 0, REG_DWORD, (BYTE*)&flag, sizeof(flag));
        StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"%s.data", safe);
        if (existed) RegSetValueExW(k, valueName, 0, REG_BINARY, data, size);
    }
    RegCloseKey(k);
}

static BOOL DisableStartupApproved(const wchar_t *name)
{
    HKEY k;
    BYTE old[64]; DWORD size = sizeof(old), type = 0;
    BOOL existed = FALSE;
    BYTE disabled[12] = {3,0,0,0};
    FILETIME ft;
    LSTATUS st;
    const wchar_t *path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

    st = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL);
    if (st != ERROR_SUCCESS) return FALSE;
    if (RegQueryValueExW(k, name, NULL, &type, old, &size) == ERROR_SUCCESS && type == REG_BINARY) existed = TRUE;
    BackupStartupValue(name, old, size, existed);
    GetSystemTimeAsFileTime(&ft);
    CopyMemory(disabled + 4, &ft, sizeof(ft));
    st = RegSetValueExW(k, name, 0, REG_BINARY, disabled, sizeof(disabled));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

static void AnalyzeAndOptimizeStartup(HWND owner)
{
    HKEY k;
    DWORD index = 0, total = 0, recommended = 0, protectedCount = 0, changed = 0;
    wchar_t summary[1024];
    const wchar_t *run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    if (RegOpenKeyExW(HKEY_CURRENT_USER, run, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) {
        MessageBoxW(owner, L"현재 사용자 시작 프로그램을 읽을 수 없습니다.", APP_TITLE, MB_ICONWARNING);
        return;
    }
    for (;;) {
        wchar_t name[260]; BYTE data[4096]; DWORD nc = ARRAYSIZE(name), ds = sizeof(data), type = 0;
        LSTATUS st = RegEnumValueW(k, index++, name, &nc, NULL, &type, data, &ds);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        ((wchar_t*)data)[(ds / sizeof(wchar_t)) < (ARRAYSIZE(data) / sizeof(wchar_t)) ? (ds / sizeof(wchar_t)) : (ARRAYSIZE(data)/sizeof(wchar_t)-1)] = 0;
        ++total;
        if (IsProtectedStartup(name, (wchar_t*)data)) ++protectedCount;
        else if (IsRecommendedStartup(name, (wchar_t*)data)) ++recommended;
    }
    RegCloseKey(k);

    StringCchPrintfW(summary, ARRAYSIZE(summary),
        L"시작 프로그램 분석 결과\r\n\r\n전체: %lu개\r\n보호 항목: %lu개\r\n중지 권장: %lu개\r\n\r\n권장 항목만 중지하시겠습니까?\r\n변경 전 상태는 자동 백업됩니다.",
        total, protectedCount, recommended);
    if (!recommended || MessageBoxW(owner, summary, APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        MessageBoxW(owner, summary, APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, run, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        index = 0;
        for (;;) {
            wchar_t name[260]; BYTE data[4096]; DWORD nc = ARRAYSIZE(name), ds = sizeof(data), type = 0;
            LSTATUS st = RegEnumValueW(k, index++, name, &nc, NULL, &type, data, &ds);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
            ((wchar_t*)data)[(ds / sizeof(wchar_t)) < (ARRAYSIZE(data) / sizeof(wchar_t)) ? (ds / sizeof(wchar_t)) : (ARRAYSIZE(data)/sizeof(wchar_t)-1)] = 0;
            if (!IsProtectedStartup(name, (wchar_t*)data) && IsRecommendedStartup(name, (wchar_t*)data)) {
                if (DisableStartupApproved(name)) ++changed;
            }
        }
        RegCloseKey(k);
    }
    StringCchPrintfW(summary, ARRAYSIZE(summary), L"권장 시작 프로그램 %lu개를 중지했습니다.\r\n다음 로그인부터 적용됩니다.", changed);
    LogChange(L"STARTUP_OPTIMIZE", summary);
    MessageBoxW(owner, summary, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    RefreshDashboard();
}

typedef struct PROCROW { DWORD pid; SIZE_T ws; wchar_t name[MAX_PATH]; } PROCROW;
static int __cdecl ProcCmp(const void *a, const void *b)
{
    const PROCROW *pa = (const PROCROW*)a, *pb = (const PROCROW*)b;
    if (pa->ws < pb->ws) return 1;
    if (pa->ws > pb->ws) return -1;
    return 0;
}

static void ShowTopProcesses(HWND owner)
{
    DWORD pids[4096], needed = 0;
    PROCROW rows[512];
    size_t count = 0;
    wchar_t out[8192] = L"고사용량 프로세스 분석 (작업 집합 기준)\r\n\r\n";
    if (!EnumProcesses(pids, sizeof(pids), &needed)) return;
    for (DWORD i = 0; i < needed / sizeof(DWORD) && count < ARRAYSIZE(rows); ++i) {
        HANDLE p;
        PROCESS_MEMORY_COUNTERS_EX pm;
        if (!pids[i]) continue;
        p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
        if (!p) continue;
        pm.cb = sizeof(pm);
        if (GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) {
            DWORD cch = ARRAYSIZE(rows[count].name);
            rows[count].pid = pids[i]; rows[count].ws = pm.WorkingSetSize;
            if (!QueryFullProcessImageNameW(p, 0, rows[count].name, &cch)) StringCchCopyW(rows[count].name, ARRAYSIZE(rows[count].name), L"(이름 확인 불가)");
            ++count;
        }
        CloseHandle(p);
    }
    qsort(rows, count, sizeof(rows[0]), ProcCmp);
    for (size_t i = 0; i < count && i < 12; ++i) {
        wchar_t line[700];
        const wchar_t *name = wcsrchr(rows[i].name, L'\\');
        if (name) ++name; else name = rows[i].name;
        StringCchPrintfW(line, ARRAYSIZE(line), L"%2zu. %-32s  PID %-6lu  %.1f MB\r\n", i + 1, name, rows[i].pid, (double)rows[i].ws / 1048576.0);
        StringCchCatW(out, ARRAYSIZE(out), line);
    }
    StringCchCatW(out, ARRAYSIZE(out), L"\r\n참고: 사용량이 높다고 해서 자동 종료하지 않습니다. 브라우저/개발도구/게임 등은 정상적으로 메모리를 많이 사용할 수 있습니다.");
    MessageBoxW(owner, out, APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static ULONGLONG CleanFolderOldFiles(const wchar_t *folder)
{
    wchar_t mask[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    FILETIME now;
    ULARGE_INTEGER n, f;
    ULONGLONG removed = 0;
    StringCchPrintfW(mask, ARRAYSIZE(mask), L"%s\\*", folder);
    h = FindFirstFileW(mask, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    GetSystemTimeAsFileTime(&now);
    n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
    do {
        wchar_t path[MAX_PATH];
        ULARGE_INTEGER t;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (n.QuadPart > t.QuadPart && n.QuadPart - t.QuadPart > 24ULL * 60 * 60 * 10000000ULL) {
            StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", folder, fd.cFileName);
            f.LowPart = fd.nFileSizeLow; f.HighPart = fd.nFileSizeHigh;
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(path)) removed += f.QuadPart;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

static void SafeDiskCleanup(HWND owner)
{
    wchar_t temp[MAX_PATH];
    wchar_t winTemp[MAX_PATH];
    ULONGLONG bytes = 0;
    wchar_t msg[512];
    if (MessageBoxW(owner, L"사용자 TEMP 및 Windows TEMP에서 24시간 이상 된 파일만 삭제합니다.\r\n개인 문서, 다운로드, 휴지통은 건드리지 않습니다.\r\n\r\n진행하시겠습니까?", APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    if (GetTempPathW(ARRAYSIZE(temp), temp)) bytes += CleanFolderOldFiles(temp);
    if (GetWindowsDirectoryW(winTemp, ARRAYSIZE(winTemp))) {
        StringCchCatW(winTemp, ARRAYSIZE(winTemp), L"\\Temp");
        bytes += CleanFolderOldFiles(winTemp);
    }
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"안전한 디스크 정리를 완료했습니다.\r\n삭제된 임시 파일: 약 %.1f MB", (double)bytes / 1048576.0);
    LogChange(L"DISK_CLEAN", msg);
    MessageBoxW(owner, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    RefreshDashboard();
}

static void BackupPerformanceSettings(void)
{
    HKEY k;
    GUID *active = NULL;
    BOOL ui = TRUE;
    DWORD one = 1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    {
        DWORD sz = sizeof(one), type = 0;
        if (RegQueryValueExW(k, L"PerfBackupSaved", NULL, &type, (BYTE*)&one, &sz) == ERROR_SUCCESS && type == REG_DWORD && one == 1) { RegCloseKey(k); return; }
    }
    if (PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS && active) {
        RegSetValueExW(k, L"PowerGuid", 0, REG_BINARY, (BYTE*)active, sizeof(GUID));
        LocalFree(active);
    }
    SystemParametersInfoW(SPI_GETUIEFFECTS, 0, &ui, 0);
    one = ui ? 1 : 0;
    RegSetValueExW(k, L"UiEffects", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
    one = 1;
    RegSetValueExW(k, L"PerfBackupSaved", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
    RegCloseKey(k);
}

static void ApplyPerformanceMode(HWND owner)
{
    GUID high = GUID_MIN_POWER_SAVINGS;
    BackupPerformanceSettings();
    PowerSetActiveScheme(NULL, &high);
    SystemParametersInfoW(SPI_SETUIEFFECTS, 0, (PVOID)FALSE, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    LogChange(L"PERFORMANCE_MODE", L"High performance power plan + reduced UI effects");
    MessageBoxW(owner, L"성능 모드를 적용했습니다.\r\n- 전원 계획: 고성능\r\n- Windows UI 효과: 최소화\r\n\r\n'전체 원상복구'에서 이전 설정으로 되돌릴 수 있습니다.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static void RestoreStartupBackups(void)
{
    HKEY b;
    DWORD idx = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_BACKUP, 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &b) != ERROR_SUCCESS) return;
    for (;;) {
        wchar_t value[300]; DWORD vc = ARRAYSIZE(value), type = 0, size = 0;
        LSTATUS st = RegEnumValueW(b, idx++, value, &vc, NULL, &type, NULL, &size);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS || !ContainsI(value, L".exists")) continue;
        {
            wchar_t base[260]; wchar_t dataName[300]; DWORD exists = 0, es = sizeof(exists); DWORD et = 0;
            BYTE data[64]; DWORD ds = sizeof(data), dt = 0;
            HKEY a;
            size_t len = wcslen(value);
            if (len <= 7) continue;
            StringCchCopyNW(base, ARRAYSIZE(base), value, len - 7); base[len - 7] = 0;
            if (RegQueryValueExW(b, value, NULL, &et, (BYTE*)&exists, &es) != ERROR_SUCCESS) continue;
            StringCchPrintfW(dataName, ARRAYSIZE(dataName), L"%s.data", base);
            if (exists) RegQueryValueExW(b, dataName, NULL, &dt, data, &ds);
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0, NULL, 0, KEY_SET_VALUE, NULL, &a, NULL) == ERROR_SUCCESS) {
                if (exists) RegSetValueExW(a, base, 0, REG_BINARY, data, ds);
                else RegDeleteValueW(a, base);
                RegCloseKey(a);
            }
        }
    }
    RegCloseKey(b);
    RegDeleteTreeW(HKEY_CURRENT_USER, STARTUP_BACKUP);
}

static void RestoreAll(HWND owner)
{
    HKEY k;
    RestoreStartupBackups();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REG, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        DWORD saved = 0, ss = sizeof(saved), st = 0;
        if (RegQueryValueExW(k, L"PerfBackupSaved", NULL, &st, (BYTE*)&saved, &ss) == ERROR_SUCCESS && saved == 1) {
            GUID guid; DWORD gs = sizeof(guid), gt = 0;
            DWORD ui = 1, us = sizeof(ui), ut = 0;
            if (RegQueryValueExW(k, L"PowerGuid", NULL, &gt, (BYTE*)&guid, &gs) == ERROR_SUCCESS && gt == REG_BINARY && gs == sizeof(guid)) PowerSetActiveScheme(NULL, &guid);
            if (RegQueryValueExW(k, L"UiEffects", NULL, &ut, (BYTE*)&ui, &us) == ERROR_SUCCESS) SystemParametersInfoW(SPI_SETUIEFFECTS, 0, (PVOID)(ULONG_PTR)(ui ? TRUE : FALSE), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
            RegDeleteValueW(k, L"PerfBackupSaved"); RegDeleteValueW(k, L"PowerGuid"); RegDeleteValueW(k, L"UiEffects");
        }
        RegCloseKey(k);
    }
    LogChange(L"RESTORE_ALL", L"Restored optimizer-managed startup and performance settings");
    MessageBoxW(owner, L"Mem Reduct Plus가 변경했던 시작 프로그램 및 성능 설정을 가능한 범위에서 원상복구했습니다.\r\n임시 파일 삭제와 메모리 트리밍은 되돌릴 대상이 없습니다.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
    RefreshDashboard();
}

static void CompareBeforeAfter(HWND owner)
{
    METRICS now;
    wchar_t msg[1200];
    if (!ReadMetrics(&now)) return;
    if (!g_haveBefore) {
        g_before = now; g_haveBefore = TRUE;
        MessageBoxW(owner, L"현재 상태를 '최적화 전' 기준값으로 저장했습니다.\r\n최적화 작업 후 다시 이 버튼을 누르면 전후 차이를 보여드립니다.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    StringCchPrintfW(msg, ARRAYSIZE(msg),
        L"최적화 전후 비교\r\n\r\n메모리 사용률: %lu%% -> %lu%% (%+ld%%p)\r\n사용 가능 메모리: %.1f GB -> %.1f GB (%+.1f GB)\r\nC: 여유 공간: %.1f GB -> %.1f GB (%+.1f GB)\r\n프로세스 수: %lu -> %lu\r\n시작 프로그램 수: %lu -> %lu\r\n\r\n주의: Windows는 캐시를 적극 활용하므로 메모리 수치가 일시적으로 다시 증가하는 것은 정상입니다.",
        g_before.memoryLoad, now.memoryLoad, (LONG)now.memoryLoad - (LONG)g_before.memoryLoad,
        (double)g_before.availPhys/1073741824.0, (double)now.availPhys/1073741824.0, ((double)now.availPhys-(double)g_before.availPhys)/1073741824.0,
        (double)g_before.diskFree/1073741824.0, (double)now.diskFree/1073741824.0, ((double)now.diskFree-(double)g_before.diskFree)/1073741824.0,
        g_before.processCount, now.processCount, g_before.startupCount, now.startupCount);
    MessageBoxW(owner, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static void OpenLog(void)
{
    wchar_t path[MAX_PATH];
    GetLogPath(path, ARRAYSIZE(path));
    ShellExecuteW(g_hwnd, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
}

static void MakeButton(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h)
{
    CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
            20, 20, 760, 100, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(NULL), NULL);
        g_diag = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            20, 300, 760, 210, hwnd, (HMENU)ID_DIAG, GetModuleHandleW(NULL), NULL);
        MakeButton(hwnd, ID_MEMORY, L"메모리 즉시 정리", 20, 140, 180, 44);
        MakeButton(hwnd, ID_AUTO, L"자동 메모리 정리: 꺼짐", 210, 140, 180, 44);
        MakeButton(hwnd, ID_STARTUP, L"시작 프로그램 분석/중지", 400, 140, 180, 44);
        MakeButton(hwnd, ID_PROCESS, L"고사용량 프로세스 분석", 590, 140, 190, 44);
        MakeButton(hwnd, ID_DISK, L"안전한 디스크 정리", 20, 195, 180, 44);
        MakeButton(hwnd, ID_PERF, L"성능 모드 적용", 210, 195, 180, 44);
        MakeButton(hwnd, ID_COMPARE, L"최적화 전후 비교", 400, 195, 180, 44);
        MakeButton(hwnd, ID_RESTORE, L"전체 원상복구", 590, 195, 190, 44);
        MakeButton(hwnd, ID_REFRESH, L"PC 상태 새로고침", 20, 250, 180, 34);
        MakeButton(hwnd, ID_LOG, L"변경 기록 열기", 210, 250, 180, 34);
        RefreshDashboard();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MEMORY: {
            METRICS b, a; SIZE_T est; wchar_t msgb[512];
            ReadMetrics(&b); est = CleanMemory(); Sleep(250); ReadMetrics(&a);
            StringCchPrintfW(msgb, ARRAYSIZE(msgb), L"메모리 정리를 완료했습니다.\r\n접근 가능한 프로세스 작업 집합 감소량: 약 %.1f MB\r\n메모리 사용률: %lu%% -> %lu%%", (double)est/1048576.0, b.memoryLoad, a.memoryLoad);
            MessageBoxW(hwnd, msgb, APP_TITLE, MB_OK | MB_ICONINFORMATION); return 0; }
        case ID_AUTO:
            g_autoMemory = !g_autoMemory;
            if (g_autoMemory) SetTimer(hwnd, TIMER_AUTO_MEMORY, AUTO_INTERVAL_MS, NULL); else KillTimer(hwnd, TIMER_AUTO_MEMORY);
            SetWindowTextW(GetDlgItem(hwnd, ID_AUTO), g_autoMemory ? L"자동 메모리 정리: 켜짐" : L"자동 메모리 정리: 꺼짐");
            LogChange(L"AUTO_MEMORY", g_autoMemory ? L"Enabled 15 minute timer" : L"Disabled"); RefreshDashboard(); return 0;
        case ID_STARTUP: AnalyzeAndOptimizeStartup(hwnd); return 0;
        case ID_PROCESS: ShowTopProcesses(hwnd); return 0;
        case ID_DISK: SafeDiskCleanup(hwnd); return 0;
        case ID_PERF: ApplyPerformanceMode(hwnd); return 0;
        case ID_COMPARE: CompareBeforeAfter(hwnd); return 0;
        case ID_RESTORE: RestoreAll(hwnd); return 0;
        case ID_REFRESH: RefreshDashboard(); return 0;
        case ID_LOG: OpenLog(); return 0;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_AUTO_MEMORY && g_autoMemory) CleanMemory();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_AUTO_MEMORY);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    WNDCLASSEXW wc;
    MSG msg;
    UNREFERENCED_PARAMETER(hPrev); UNREFERENCED_PARAMETER(cmd);
    InitCommonControlsEx(&ic);
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"MemReductPlusOptimizerClass";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 570, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, show); UpdateWindow(g_hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
