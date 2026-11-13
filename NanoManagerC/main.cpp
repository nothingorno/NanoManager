#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <format>
#include <vector>
#include <unordered_map>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

#ifndef I_GROUPID_NONE
#define I_GROUPID_NONE -1
#endif

// Custom Message for Thread-Safe UI Sync
constexpr UINT WM_TELEMETRY_UPDATED = WM_APP + 1;

// Layout Constants
constexpr int TITLEBAR_HEIGHT = 36;
constexpr int MENUBAR_HEIGHT = 32;
constexpr int TABBAR_HEIGHT = 42;
constexpr int BUTTON_WIDTH = 46;
constexpr int RESIZE_BORDER = 6;

// Dark Gray Color Palette
constexpr COLORREF COL_BG_MAIN = RGB(30, 30, 35);
constexpr COLORREF COL_BG_TITLEBAR = RGB(22, 22, 26);
constexpr COLORREF COL_BG_MENUBAR = RGB(26, 26, 30);
constexpr COLORREF COL_BG_TABBAR = RGB(34, 34, 40);
constexpr COLORREF COL_BG_CARD = RGB(38, 38, 45);
constexpr COLORREF COL_BORDER = RGB(48, 48, 56);
constexpr COLORREF COL_ACCENT = RGB(0, 120, 212);
constexpr COLORREF COL_TEXT_PRIMARY = RGB(235, 235, 242);
constexpr COLORREF COL_TEXT_MUTED = RGB(145, 145, 158);
constexpr COLORREF COL_BTN_HOVER = RGB(45, 45, 52);
constexpr COLORREF COL_CLOSE_HOVER = RGB(232, 17, 35);

// Native Segoe Fluent / MDL2 Glyphs
constexpr wchar_t GLYPH_MINIMIZE = L'\uE921';
constexpr wchar_t GLYPH_MAXIMIZE = L'\uE922';
constexpr wchar_t GLYPH_RESTORE = L'\uE923';
constexpr wchar_t GLYPH_CLOSE = L'\uE8BB';
constexpr wchar_t GLYPH_PERF = L'\uEECB'; // Performance / Chart Icon
constexpr wchar_t GLYPH_TASKS = L'\uE8FD'; // TaskView / List Icon

// Application Tabs
enum class AppTab { Perf, Tasks };
AppTab g_currentTab = AppTab::Perf;

// Telemetry & Process State
std::mutex g_telemetryMutex;
float g_cpuUsage = 0.0f;
unsigned long long g_totalRamBytes = 0;
unsigned long long g_availRamBytes = 0;
DWORD g_memoryLoad = 0;

// Animation smoothing state
float g_animCpu = 0.0f;
float g_animRam = 0.0f;

struct ProcessEntry {
    std::wstring exeName;
    std::wstring displayName;
    DWORD pid;
    float cpuPercent;
    unsigned long long ramBytes;
    int iconIndex;
};
std::vector<ProcessEntry> g_processList;
bool g_running = true;

// Caches
std::unordered_map<DWORD, ULONGLONG> g_prevProcTimes;
std::unordered_map<std::wstring, std::wstring> g_displayNameCache;
std::unordered_map<std::wstring, int> g_iconIndexCache;

// UI State
enum class HoveredButton { None, Minimize, Maximize, Close };
HoveredButton g_hoveredBtn = HoveredButton::None;
bool g_trackingMouse = false;
bool g_isWin11 = false;
std::wstring g_iconFontName = L"Segoe MDL2 Assets";

bool g_helpMenuOpen = false;
HWND g_hwndAbout = nullptr;
HWND g_hwndListView = nullptr;
HFONT g_hListFont = nullptr;

// OS Version Helper
bool DetectWindows11() {
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        auto fxPtr = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(hMod, "RtlGetVersion"));
        if (fxPtr) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fxPtr(&rovi) == 0) return rovi.dwBuildNumber >= 22000;
        }
    }
    return false;
}

std::wstring GetProcessDisplayName(const std::wstring& fullPath, const std::wstring& fallbackExe) {
    if (fullPath.empty()) return fallbackExe;
    auto it = g_displayNameCache.find(fullPath);
    if (it != g_displayNameCache.end()) return it->second;

    std::wstring result = fallbackExe;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(fullPath.c_str(), &dummy);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(fullPath.c_str(), 0, size, data.data())) {
            struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; } *lpTranslate;
            UINT cbTranslate = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
                wchar_t subBlock[64];
                swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
                LPVOID lpBuffer = nullptr;
                UINT dwBytes = 0;
                if (VerQueryValueW(data.data(), subBlock, &lpBuffer, &dwBytes) && dwBytes > 1) {
                    std::wstring desc(static_cast<wchar_t*>(lpBuffer));
                    if (!desc.empty()) result = desc;
                }
            }
        }
    }
    g_displayNameCache[fullPath] = result;
    return result;
}

int GetProcessIconIndex(const std::wstring& fullPath, const std::wstring& exeName) {
    std::wstring key = fullPath.empty() ? exeName : fullPath;
    auto it = g_iconIndexCache.find(key);
    if (it != g_iconIndexCache.end()) return it->second;

    SHFILEINFOW sfi = {};
    if (!fullPath.empty() && SHGetFileInfoW(fullPath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
        g_iconIndexCache[key] = sfi.iIcon;
        return sfi.iIcon;
    }

    SHGetFileInfoW(exeName.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    g_iconIndexCache[key] = sfi.iIcon;
    return sfi.iIcon;
}

// Background Telemetry & Process Polling Thread
void TelemetryThreadFunc(HWND hwnd) {
    ULARGE_INTEGER lastIdle = {}, lastKernel = {}, lastUser = {};
    FILETIME ftIdle, ftKernel, ftUser;

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        lastIdle.LowPart = ftIdle.dwLowDateTime; lastIdle.HighPart = ftIdle.dwHighDateTime;
        lastKernel.LowPart = ftKernel.dwLowDateTime; lastKernel.HighPart = ftKernel.dwHighDateTime;
        lastUser.LowPart = ftUser.dwLowDateTime; lastUser.HighPart = ftUser.dwHighDateTime;
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!g_running) break;

        float cpu = 0.0f;
        ULONGLONG totalSysDelta = 0;
        if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
            ULARGE_INTEGER idle, kernel, user;
            idle.LowPart = ftIdle.dwLowDateTime; idle.HighPart = ftIdle.dwHighDateTime;
            kernel.LowPart = ftKernel.dwLowDateTime; kernel.HighPart = ftKernel.dwHighDateTime;
            user.LowPart = ftUser.dwLowDateTime; user.HighPart = ftUser.dwHighDateTime;

            ULONGLONG idleDiff = idle.QuadPart - lastIdle.QuadPart;
            ULONGLONG kernelDiff = kernel.QuadPart - lastKernel.QuadPart;
            ULONGLONG userDiff = user.QuadPart - lastUser.QuadPart;

            lastIdle = idle; lastKernel = kernel; lastUser = user;

            ULONGLONG sysTotal = kernelDiff + userDiff;
            totalSysDelta = sysTotal;
            if (sysTotal > 0) {
                ULONGLONG sysActive = sysTotal > idleDiff ? sysTotal - idleDiff : 0;
                cpu = static_cast<float>((double)sysActive / sysTotal) * 100.0f;
                cpu = std::clamp(cpu, 0.0f, 100.0f);
            }
        }

        MEMORYSTATUSEX memInfo = { sizeof(memInfo) };
        GlobalMemoryStatusEx(&memInfo);

        std::vector<ProcessEntry> tempProcesses;
        std::unordered_map<DWORD, ULONGLONG> nextProcTimes;

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (pe.th32ProcessID == 0) continue;

                    unsigned long long workingSet = 0;
                    float procCpu = 0.0f;
                    std::wstring fullPath;

                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
                        if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                            workingSet = pmc.WorkingSetSize;
                        }

                        FILETIME ftCreate, ftExit, ftProcKernel, ftProcUser;
                        if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftProcKernel, &ftProcUser)) {
                            ULARGE_INTEGER k, u;
                            k.LowPart = ftProcKernel.dwLowDateTime; k.HighPart = ftProcKernel.dwHighDateTime;
                            u.LowPart = ftProcUser.dwLowDateTime; u.HighPart = ftProcUser.dwHighDateTime;
                            ULONGLONG currentTotal = k.QuadPart + u.QuadPart;
                            nextProcTimes[pe.th32ProcessID] = currentTotal;

                            auto prevIt = g_prevProcTimes.find(pe.th32ProcessID);
                            if (prevIt != g_prevProcTimes.end() && totalSysDelta > 0) {
                                ULONGLONG procDelta = currentTotal - prevIt->second;
                                procCpu = static_cast<float>((double)procDelta / (double)totalSysDelta) * 100.0f;
                                procCpu = std::clamp(procCpu, 0.0f, 100.0f);
                            }
                        }

                        wchar_t pathBuf[MAX_PATH] = {};
                        DWORD pathLen = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
                            fullPath = pathBuf;
                        }
                        CloseHandle(hProc);
                    }

                    std::wstring dispName = GetProcessDisplayName(fullPath, pe.szExeFile);
                    int iconIdx = GetProcessIconIndex(fullPath, pe.szExeFile);

                    tempProcesses.push_back({ pe.szExeFile, dispName, pe.th32ProcessID, procCpu, workingSet, iconIdx });
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }

        g_prevProcTimes = std::move(nextProcTimes);

        std::sort(tempProcesses.begin(), tempProcesses.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
            int cmp = _wcsicmp(a.displayName.c_str(), b.displayName.c_str());
            if (cmp != 0) return cmp < 0;
            return a.pid < b.pid;
            });

        {
            std::lock_guard<std::mutex> lock(g_telemetryMutex);
            g_cpuUsage = cpu;
            g_totalRamBytes = memInfo.ullTotalPhys;
            g_availRamBytes = memInfo.ullAvailPhys;
            g_memoryLoad = memInfo.dwMemoryLoad;
            g_processList = std::move(tempProcesses);
        }

        PostMessage(hwnd, WM_TELEMETRY_UPDATED, 0, 0);
    }
}

// Smooth ListView Update with Native Collapsible Tree Groups (3+ instances)
void UpdateTaskListView() {
    if (!g_hwndListView || !IsWindowVisible(g_hwndListView)) return;

    std::vector<ProcessEntry> procs;
    {
        std::lock_guard<std::mutex> lock(g_telemetryMutex);
        procs = g_processList;
    }

    std::unordered_map<std::wstring, int> nameCounts;
    for (const auto& p : procs) {
        nameCounts[p.displayName]++;
    }

    SendMessage(g_hwndListView, WM_SETREDRAW, FALSE, 0);

    ListView_RemoveAllGroups(g_hwndListView);
    ListView_EnableGroupView(g_hwndListView, TRUE);

    int groupIDCounter = 1;
    std::unordered_map<std::wstring, int> groupMap;

    ListView_DeleteAllItems(g_hwndListView);

    for (int i = 0; i < (int)procs.size(); ++i) {
        const auto& p = procs[i];
        int assignedGroupID = I_GROUPID_NONE;

        if (nameCounts[p.displayName] >= 3) {
            auto it = groupMap.find(p.displayName);
            if (it == groupMap.end()) {
                assignedGroupID = groupIDCounter++;
                groupMap[p.displayName] = assignedGroupID;

                LVGROUP grp = { sizeof(grp) };
                grp.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE | LVGF_ALIGN;
                grp.iGroupId = assignedGroupID;

                std::wstring groupHeaderTitle = std::format(L"{} ({})", p.displayName, nameCounts[p.displayName]);
                grp.pszHeader = const_cast<LPWSTR>(groupHeaderTitle.c_str());
                grp.state = LVGS_COLLAPSIBLE | LVGS_NORMAL;
                grp.uAlign = LVGA_HEADER_LEFT;
                SendMessage(g_hwndListView, LVM_INSERTGROUP, -1, (LPARAM)&grp);
            }
            else {
                assignedGroupID = it->second;
            }
        }

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | (assignedGroupID != I_GROUPID_NONE ? LVIF_GROUPID : 0);
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<LPWSTR>(p.displayName.c_str());
        lvi.iImage = p.iconIndex;
        lvi.iGroupId = assignedGroupID;

        int itemIndex = ListView_InsertItem(g_hwndListView, &lvi);

        std::wstring pidStr = std::to_wstring(p.pid);
        std::wstring cpuStr = std::format(L"{:.1f}%", p.cpuPercent);
        double ramMB = (double)p.ramBytes / (1024.0 * 1024.0);
        std::wstring ramStr = std::format(L"{:.1f} MB", ramMB);

        ListView_SetItemText(g_hwndListView, itemIndex, 1, const_cast<LPWSTR>(p.exeName.c_str()));
        ListView_SetItemText(g_hwndListView, itemIndex, 2, const_cast<LPWSTR>(pidStr.c_str()));
        ListView_SetItemText(g_hwndListView, itemIndex, 3, const_cast<LPWSTR>(cpuStr.c_str()));
        ListView_SetItemText(g_hwndListView, itemIndex, 4, const_cast<LPWSTR>(ramStr.c_str()));
    }

    SendMessage(g_hwndListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hwndListView, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void CreateAboutWindow(HINSTANCE hInstance, HWND parent) {
    if (g_hwndAbout && IsWindow(g_hwndAbout)) {
        BringWindowToTop(g_hwndAbout);
        SetForegroundWindow(g_hwndAbout);
        return;
    }

    const wchar_t* ABOUT_CLASS = L"NanoManagerC_AboutWindow";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COL_BG_MAIN);
    wc.lpszClassName = ABOUT_CLASS;
    RegisterClassExW(&wc);

    g_hwndAbout = CreateWindowExW(
        WS_EX_DLGMODALFRAME, ABOUT_CLASS, L"About NanoManager v3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 280,
        parent, nullptr, hInstance, nullptr
    );

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hwndAbout, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    ShowWindow(g_hwndAbout, SW_SHOW);
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    g_isWin11 = DetectWindows11();
    g_iconFontName = g_isWin11 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icex);

    const wchar_t* CLASS_NAME = L"NanoManagerC_MainFrame";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc)) return 0;

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"NanoManager v3",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 0;

    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);

    int topOffset = TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT;
    g_hwndListView = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, topOffset, 1280, 800 - topOffset,
        hwnd, (HMENU)1001, hInstance, nullptr
    );

    // Enabled LVS_EX_GROUPVIEW for collapsible task grouping support
    ListView_SetExtendedListViewStyle(g_hwndListView, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_GROUPVIEW);

    ListView_SetBkColor(g_hwndListView, COL_BG_MAIN);
    ListView_SetTextBkColor(g_hwndListView, COL_BG_MAIN);
    ListView_SetTextColor(g_hwndListView, COL_TEXT_PRIMARY);

    g_hListFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessage(g_hwndListView, WM_SETFONT, (WPARAM)g_hListFont, TRUE);

    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    lvc.pszText = const_cast<LPWSTR>(L"App / Display Name"); lvc.cx = 350;
    ListView_InsertColumn(g_hwndListView, 0, &lvc);

    lvc.pszText = const_cast<LPWSTR>(L"Process Name"); lvc.cx = 220;
    ListView_InsertColumn(g_hwndListView, 1, &lvc);

    lvc.pszText = const_cast<LPWSTR>(L"PID"); lvc.cx = 90;
    ListView_InsertColumn(g_hwndListView, 2, &lvc);

    lvc.pszText = const_cast<LPWSTR>(L"CPU"); lvc.cx = 100;
    ListView_InsertColumn(g_hwndListView, 3, &lvc);

    lvc.pszText = const_cast<LPWSTR>(L"Memory (RAM)"); lvc.cx = 160;
    ListView_InsertColumn(g_hwndListView, 4, &lvc);

    SHFILEINFOW sfiSys = {};
    HIMAGELIST hIml = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfiSys, sizeof(sfiSys), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (hIml) {
        ListView_SetImageList(g_hwndListView, hIml, LVSIL_SMALL);
    }

    std::thread telemetryThread(TelemetryThreadFunc, hwnd);
    telemetryThread.detach();

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    SetTimer(hwnd, 1, 16, nullptr);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    if (g_hListFont) DeleteObject(g_hListFont);
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        SetBkMode(hdc, TRANSPARENT);

        HFONT hFontTitle = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hFontBody = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        SelectObject(hdc, hFontTitle);
        SetTextColor(hdc, COL_TEXT_PRIMARY);
        TextOutW(hdc, 24, 24, L"NanoManager v3.0", 16);

        SelectObject(hdc, hFontBody);
        SetTextColor(hdc, COL_TEXT_MUTED);
        TextOutW(hdc, 24, 60, L"Monolithic C++ Performance Telemetry Engine", 43);
        TextOutW(hdc, 24, 85, L"Built with pure Win32 API & custom window frame.", 47);
        TextOutW(hdc, 24, 130, L"© 2026 nothingorno / NanoManager Project", 38);

        DeleteObject(hFontTitle);
        DeleteObject(hFontBody);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        g_hwndAbout = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

HoveredButton HitTestTitleBarButtons(HWND hwnd, POINT ptClient) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);

    if (ptClient.y < 0 || ptClient.y >= TITLEBAR_HEIGHT) return HoveredButton::None;

    int right = rcClient.right;
    if (ptClient.x >= right - BUTTON_WIDTH && ptClient.x < right) return HoveredButton::Close;
    if (ptClient.x >= right - BUTTON_WIDTH * 2 && ptClient.x < right - BUTTON_WIDTH) return HoveredButton::Maximize;
    if (ptClient.x >= right - BUTTON_WIDTH * 3 && ptClient.x < right - BUTTON_WIDTH * 2) return HoveredButton::Minimize;

    return HoveredButton::None;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_TIMER: {
        if (wParam == 1 && g_currentTab == AppTab::Perf) {
            float targetCpu = 0.0f;
            float targetRam = 0.0f;
            {
                std::lock_guard<std::mutex> lock(g_telemetryMutex);
                targetCpu = g_cpuUsage;
                targetRam = (float)g_memoryLoad;
            }

            g_animCpu += (targetCpu - g_animCpu) * 0.15f;
            g_animRam += (targetRam - g_animRam) * 0.15f;

            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            rcClient.top = TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT;
            InvalidateRect(hwnd, &rcClient, FALSE);
        }
        return 0;
    }

    case WM_TELEMETRY_UPDATED: {
        if (g_currentTab == AppTab::Tasks) {
            UpdateTaskListView();
        }
        return 0;
    }

    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS* pParams = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(hwnd)) {
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(hMon, &mi)) pParams->rgrc[0] = mi.rcWork;
            }
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT ptClient = ptScreen;
        ScreenToClient(hwnd, &ptClient);

        RECT rcClient;
        GetClientRect(hwnd, &rcClient);

        if (!IsZoomed(hwnd)) {
            bool left = ptClient.x < RESIZE_BORDER;
            bool right = ptClient.x >= rcClient.right - RESIZE_BORDER;
            bool top = ptClient.y < RESIZE_BORDER;
            bool bottom = ptClient.y >= rcClient.bottom - RESIZE_BORDER;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }

        if (HitTestTitleBarButtons(hwnd, ptClient) != HoveredButton::None) return HTCLIENT;
        if (ptClient.y >= TITLEBAR_HEIGHT && ptClient.y < TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT) return HTCLIENT;
        if (ptClient.y < TITLEBAR_HEIGHT) return HTCAPTION;

        return HTCLIENT;
    }

    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int topOffset = TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT;
        if (g_hwndListView) {
            SetWindowPos(g_hwndListView, nullptr, 0, topOffset, width, height - topOffset, SWP_NOZORDER);

            int col0 = static_cast<int>(width * 0.32);
            int col1 = static_cast<int>(width * 0.22);
            int col2 = 80;
            int col3 = 90;
            int col4 = width - (col0 + col1 + col2 + col3) - 20;
            if (col4 < 100) col4 = 100;

            ListView_SetColumnWidth(g_hwndListView, 0, col0);
            ListView_SetColumnWidth(g_hwndListView, 1, col1);
            ListView_SetColumnWidth(g_hwndListView, 2, col2);
            ListView_SetColumnWidth(g_hwndListView, 3, col3);
            ListView_SetColumnWidth(g_hwndListView, 4, col4);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HoveredButton newHover = HitTestTitleBarButtons(hwnd, pt);
        if (newHover != g_hoveredBtn) {
            g_hoveredBtn = newHover;
            RECT rcTitle = { 0, 0, 10000, TITLEBAR_HEIGHT };
            InvalidateRect(hwnd, &rcTitle, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_trackingMouse = false;
        if (g_hoveredBtn != HoveredButton::None) {
            g_hoveredBtn = HoveredButton::None;
            RECT rcTitle = { 0, 0, 10000, TITLEBAR_HEIGHT };
            InvalidateRect(hwnd, &rcTitle, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (pt.y >= TITLEBAR_HEIGHT && pt.y < TITLEBAR_HEIGHT + MENUBAR_HEIGHT) {
            if (pt.x >= 16 && pt.x <= 70) {
                g_helpMenuOpen = !g_helpMenuOpen;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            else if (g_helpMenuOpen) {
                if (pt.x >= 16 && pt.x <= 180 && pt.y >= TITLEBAR_HEIGHT + MENUBAR_HEIGHT && pt.y <= TITLEBAR_HEIGHT + MENUBAR_HEIGHT + 36) {
                    CreateAboutWindow(GetModuleHandle(nullptr), hwnd);
                }
                g_helpMenuOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        else if (pt.y >= TITLEBAR_HEIGHT + MENUBAR_HEIGHT && pt.y < TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT) {
            g_helpMenuOpen = false;
            if (pt.x >= 16 && pt.x < 130 && g_currentTab != AppTab::Perf) {
                g_currentTab = AppTab::Perf;
                ShowWindow(g_hwndListView, SW_HIDE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (pt.x >= 130 && pt.x < 250 && g_currentTab != AppTab::Tasks) {
                g_currentTab = AppTab::Tasks;
                ShowWindow(g_hwndListView, SW_SHOW);
                UpdateTaskListView();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else if (g_helpMenuOpen) {
            g_helpMenuOpen = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HoveredButton clicked = HitTestTitleBarButtons(hwnd, pt);

        if (clicked == HoveredButton::Close) PostMessage(hwnd, WM_CLOSE, 0, 0);
        else if (clicked == HoveredButton::Maximize) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        else if (clicked == HoveredButton::Minimize) ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hwnd, &ps);

        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int width = rcClient.right - rcClient.left;
        int height = rcClient.bottom - rcClient.top;

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, width, height);
        HGDIOBJ hOldBmp = SelectObject(hdcMem, hbmMem);

        HBRUSH bgBrush = CreateSolidBrush(COL_BG_MAIN);
        FillRect(hdcMem, &rcClient, bgBrush);
        DeleteObject(bgBrush);

        RECT rcTitle = { 0, 0, width, TITLEBAR_HEIGHT };
        HBRUSH titleBrush = CreateSolidBrush(COL_BG_TITLEBAR);
        FillRect(hdcMem, &rcTitle, titleBrush);
        DeleteObject(titleBrush);

        RECT rcMenu = { 0, TITLEBAR_HEIGHT, width, TITLEBAR_HEIGHT + MENUBAR_HEIGHT };
        HBRUSH menuBrush = CreateSolidBrush(COL_BG_MENUBAR);
        FillRect(hdcMem, &rcMenu, menuBrush);
        DeleteObject(menuBrush);

        RECT rcTab = { 0, TITLEBAR_HEIGHT + MENUBAR_HEIGHT, width, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT };
        HBRUSH tabBrush = CreateSolidBrush(COL_BG_TABBAR);
        FillRect(hdcMem, &rcTab, tabBrush);
        DeleteObject(tabBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, COL_BORDER);
        HGDIOBJ oldPen = SelectObject(hdcMem, borderPen);
        MoveToEx(hdcMem, 0, TITLEBAR_HEIGHT + MENUBAR_HEIGHT, nullptr);
        LineTo(hdcMem, width, TITLEBAR_HEIGHT + MENUBAR_HEIGHT);
        MoveToEx(hdcMem, 0, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT, nullptr);
        LineTo(hdcMem, width, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT);
        SelectObject(hdcMem, oldPen);
        DeleteObject(borderPen);

        HFONT hTitleFont = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hMenuFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hTabFont = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hCardFont = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hValueFont = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hIconFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_iconFontName.c_str());

        SetBkMode(hdcMem, TRANSPARENT);

        SelectObject(hdcMem, hTitleFont);
        SetTextColor(hdcMem, COL_TEXT_PRIMARY);
        RECT rcTitleText = { 16, 0, width - BUTTON_WIDTH * 3, TITLEBAR_HEIGHT };
        DrawTextW(hdcMem, L"NanoManager v3", -1, &rcTitleText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdcMem, hIconFont);
        auto drawGlyphButton = [&](int indexFromRight, HoveredButton btnType, wchar_t glyph, COLORREF hoverCol) {
            RECT rcBtn = { width - BUTTON_WIDTH * indexFromRight, 0, width - BUTTON_WIDTH * (indexFromRight - 1), TITLEBAR_HEIGHT };
            if (g_hoveredBtn == btnType) {
                HBRUSH hBrush = CreateSolidBrush(hoverCol);
                FillRect(hdcMem, &rcBtn, hBrush);
                DeleteObject(hBrush);
            }
            wchar_t glyphStr[2] = { glyph, L'\0' };
            DrawTextW(hdcMem, glyphStr, 1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            };
        drawGlyphButton(3, HoveredButton::Minimize, GLYPH_MINIMIZE, COL_BTN_HOVER);
        drawGlyphButton(2, HoveredButton::Maximize, IsZoomed(hwnd) ? GLYPH_RESTORE : GLYPH_MAXIMIZE, COL_BTN_HOVER);
        drawGlyphButton(1, HoveredButton::Close, GLYPH_CLOSE, COL_CLOSE_HOVER);

        SelectObject(hdcMem, hMenuFont);
        SetTextColor(hdcMem, g_helpMenuOpen ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
        RECT rcHelpBtn = { 16, TITLEBAR_HEIGHT, 70, TITLEBAR_HEIGHT + MENUBAR_HEIGHT };
        if (g_helpMenuOpen) {
            HBRUSH hSelBrush = CreateSolidBrush(COL_BTN_HOVER);
            FillRect(hdcMem, &rcHelpBtn, hSelBrush);
            DeleteObject(hSelBrush);
        }
        DrawTextW(hdcMem, L"Help", -1, &rcHelpBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Render Tabs with Segoe Fluent/MDL2 Icons + Text
        auto drawTabItem = [&](int x, wchar_t icon, const wchar_t* label, AppTab tab) {
            RECT rcT = { x, TITLEBAR_HEIGHT + MENUBAR_HEIGHT, x + 114, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT };
            bool active = (g_currentTab == tab);
            if (active) {
                HBRUSH hActiveBrush = CreateSolidBrush(COL_BG_MAIN);
                FillRect(hdcMem, &rcT, hActiveBrush);
                DeleteObject(hActiveBrush);
                SetTextColor(hdcMem, COL_TEXT_PRIMARY);
            }
            else {
                SetTextColor(hdcMem, COL_TEXT_MUTED);
            }

            // Draw Icon
            SelectObject(hdcMem, hIconFont);
            wchar_t iconStr[2] = { icon, L'\0' };
            RECT rcIcon = { rcT.left + 12, rcT.top, rcT.left + 36, rcT.bottom };
            DrawTextW(hdcMem, iconStr, 1, &rcIcon, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Draw Label
            SelectObject(hdcMem, hTabFont);
            RECT rcLabel = { rcT.left + 36, rcT.top, rcT.right, rcT.bottom };
            DrawTextW(hdcMem, label, -1, &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            };
        drawTabItem(16, GLYPH_PERF, L"Perf", AppTab::Perf);
        drawTabItem(130, GLYPH_TASKS, L"Tasks", AppTab::Tasks);

        if (g_currentTab == AppTab::Perf) {
            unsigned long long totalRam, availRam;
            {
                std::lock_guard<std::mutex> lock(g_telemetryMutex);
                totalRam = g_totalRamBytes;
                availRam = g_availRamBytes;
            }

            auto drawTelemetryCard = [&](int x, int y, int w, int h, const wchar_t* title, float percent, const std::wstring& details) {
                RECT rcCard = { x, y, x + w, y + h };
                HBRUSH cardBrush = CreateSolidBrush(COL_BG_CARD);
                FillRect(hdcMem, &rcCard, cardBrush);
                DeleteObject(cardBrush);

                HBRUSH brBorder = CreateSolidBrush(COL_BORDER);
                FrameRect(hdcMem, &rcCard, brBorder);
                DeleteObject(brBorder);

                SelectObject(hdcMem, hCardFont);
                SetTextColor(hdcMem, COL_TEXT_PRIMARY);
                RECT rcHeader = { x + 20, y + 16, x + w - 20, y + 40 };
                DrawTextW(hdcMem, title, -1, &rcHeader, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hValueFont);
                std::wstring percStr = std::format(L"{:.1f}%", percent);
                RECT rcVal = { x + 20, y + 45, x + w - 20, y + 85 };
                DrawTextW(hdcMem, percStr.c_str(), -1, &rcVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                RECT rcBarBg = { x + 20, y + 105, x + w - 20, y + 115 };
                HBRUSH bgBarBrush = CreateSolidBrush(RGB(50, 50, 60));
                FillRect(hdcMem, &rcBarBg, bgBarBrush);
                DeleteObject(bgBarBrush);

                int fillWidth = static_cast<int>((w - 40) * (percent / 100.0f));
                if (fillWidth > w - 40) fillWidth = w - 40;
                if (fillWidth < 0) fillWidth = 0;
                if (fillWidth > 0) {
                    RECT rcBarFill = { x + 20, y + 105, x + 20 + fillWidth, y + 115 };
                    HBRUSH fillBrush = CreateSolidBrush(COL_ACCENT);
                    FillRect(hdcMem, &rcBarFill, fillBrush);
                    DeleteObject(fillBrush);
                }

                SelectObject(hdcMem, hMenuFont);
                SetTextColor(hdcMem, COL_TEXT_MUTED);
                RECT rcDetails = { x + 20, y + 130, x + w - 20, y + 155 };
                DrawTextW(hdcMem, details.c_str(), -1, &rcDetails, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                };

            int cardWidth = (width - 72) / 2;
            int cardHeight = 180;
            int cardY = TITLEBAR_HEIGHT + MENUBAR_HEIGHT + TABBAR_HEIGHT + 24;

            double usedRamGB = (double)(totalRam - availRam) / (1024.0 * 1024.0 * 1024.0);
            double totalRamGB = (double)totalRam / (1024.0 * 1024.0 * 1024.0);
            std::wstring ramDetails = std::format(L"Used: {:.2f} GB / Total: {:.2f} GB", usedRamGB, totalRamGB);
            std::wstring cpuDetails = L"Monolithic Core Telemetry Polling Active";

            drawTelemetryCard(24, cardY, cardWidth, cardHeight, L"Processor (CPU)", g_animCpu, cpuDetails);
            drawTelemetryCard(cardWidth + 48, cardY, cardWidth, cardHeight, L"Memory (RAM)", g_animRam, ramDetails);
        }

        if (g_helpMenuOpen) {
            RECT rcPopup = { 16, TITLEBAR_HEIGHT + MENUBAR_HEIGHT, 180, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + 40 };
            HBRUSH popupBrush = CreateSolidBrush(COL_BG_CARD);
            FillRect(hdcMem, &rcPopup, popupBrush);
            DeleteObject(popupBrush);

            HBRUSH brPopupBorder = CreateSolidBrush(COL_BORDER);
            FrameRect(hdcMem, &rcPopup, brPopupBorder);
            DeleteObject(brPopupBorder);

            SelectObject(hdcMem, hMenuFont);
            SetTextColor(hdcMem, COL_TEXT_PRIMARY);
            RECT rcItem = { 26, TITLEBAR_HEIGHT + MENUBAR_HEIGHT, 170, TITLEBAR_HEIGHT + MENUBAR_HEIGHT + 40 };
            DrawTextW(hdcMem, L"About NanoManager", -1, &rcItem, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdcScreen, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);

        DeleteObject(hTitleFont);
        DeleteObject(hMenuFont);
        DeleteObject(hTabFont);
        DeleteObject(hCardFont);
        DeleteObject(hValueFont);
        DeleteObject(hIconFont);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}