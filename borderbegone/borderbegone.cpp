#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <shlwapi.h>
#include <dwmapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef LOG_ENABLED
#define LOG_ENABLED 0
#endif

#if LOG_ENABLED
#define LOG(fmt, ...) \
    wprintf(L"[%hs::%u] " fmt L"\n", __FUNCTION__, (unsigned)__LINE__, __VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

struct Args {
	DWORD pid;
	WCHAR procName[MAX_PATH];
	WCHAR title[1024];
	BOOL hasPid;
	BOOL hasName;
	BOOL hasTitle;
};

struct Window {
	DWORD targetPid;
	LPCWSTR targetTitle;
	HWND found;
};

static VOID ShowHelp()
{
	wprintf(
		L"usage:\n"
		L"borderbegone.exe -pid 1111 -title \"hardware monitor\"\n"
		L"borderbegone.exe -name MSIAfterburner.exe -title \"hardware monitor\""
	);
}

static BOOL StrCompare(LPCWSTR str1, LPCWSTR str2)
{
	LOG(L"Comparing arg values");

	return lstrcmpiW(str1, str2) == 0;
}

static VOID FixQuotes(LPWSTR buf, SIZE_T size, LPCWSTR text)
{
	LOG(L"Fixing arg values");

	BOOL needQuotes = (wcspbrk(text, L" \t") != 0);

	SIZE_T curr = wcslen(buf);

	if (needQuotes && (curr + 1) < size) {
		buf[curr++] = L'\"';
	}

	for (SIZE_T i = 0; text[i] && (curr + 1) < size; ++i) {
		buf[curr++] = text[i];
	}

	if (needQuotes && (curr + 1) < size) {
		buf[curr++] = L'\"';
	}

	if (curr < size) {
		buf[curr] = 0;
	}
}

static BOOL IsElevated()
{
	LOG(L"Checking if elevated");

	HANDLE token;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return FALSE;
	}

	TOKEN_ELEVATION elevation{};
	DWORD size = 0;

	BOOL tokenInfo = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);

	CloseHandle(token);

	return tokenInfo && elevation.TokenIsElevated;
}

static INT RunElevated()
{
	LOG(L"Launching elevated");

	WCHAR path[MAX_PATH] = {};

	if (!GetModuleFileNameW(0, path, MAX_PATH)) {
		return 1;
	}

	INT argc = 0;

	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	WCHAR params[32768] = {};

	for (INT i = 1; i < argc; ++i) {

		if (i > 1) {
			wcscat_s(params, L" ");
		}

		FixQuotes(params, sizeof(params) / sizeof(params[0]), argv[i]);
	}

	if (argv) {
		LocalFree(argv);
	}

	HINSTANCE instance = ShellExecuteW(0, L"runas", path, (params[0] ? params : 0), 0, SW_SHOWNORMAL);

	if ((INT_PTR)instance <= 32) {
		wprintf(L"Failed to elevate: %p\n", instance);
		return 1;
	}

	return 0;
}

static DWORD FindPidByName(LPCWSTR name)
{
	LOG(L"Finding PID by process name");

	DWORD result = 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (snapshot == INVALID_HANDLE_VALUE) {
		return 0;
	}

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);

	if (Process32FirstW(snapshot, &entry)) {

		do {

			if (StrCompare(entry.szExeFile, name)) {
				result = entry.th32ProcessID;
				break;
			}
		}
		while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);

	return result;
}

static BOOL CALLBACK EnumWindows(HWND hwnd, LPARAM param)
{
	LOG(L"Querying open windows");

	Window* window = (Window*)param;
	DWORD pid = 0;

	GetWindowThreadProcessId(hwnd, &pid);

	if (pid != window->targetPid) {
		return TRUE;
	}

	if (!IsWindowVisible(hwnd) || GetParent(hwnd) != 0) {
		return TRUE;
	}

	WCHAR title[1024] = {};

	if (!GetWindowTextW(hwnd, title, 1024) || title[0] == 0) {
		return TRUE;
	}

	if (StrStrIW(title, window->targetTitle)) {
		window->found = hwnd;
		return FALSE;
	}

	return TRUE;
}

static HWND GetWindow(DWORD pid, LPCWSTR title)
{
	LOG(L"Getting target window");

	Window window = {};
	window.targetPid = pid;
	window.targetTitle = title;
	window.found = 0;

	EnumWindows(EnumWindows, (LPARAM)&window);

	return window.found;
}

static VOID HideBorder(HWND hwnd)
{
	LOG(L"Hiding window border");

	const DWORD none = DWMWA_COLOR_NONE;

	if (S_OK != DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &none, sizeof(none))) {
		const COLORREF border = RGB(0, 0, 0);
		DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	}
}

static VOID FixRendering(HWND hwnd)
{
	LOG(L"Fixing DWM rendering");

	DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;

	DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));

	MARGINS margins = { 0, 0, 0, 0 };

	DwmExtendFrameIntoClientArea(hwnd, &margins);
}

static VOID DisablePeek(HWND hwnd)
{
	LOG(L"Disabling window peek");

	BOOL on = TRUE;

	DwmSetWindowAttribute(hwnd, DWMWA_DISALLOW_PEEK, &on, sizeof(on));
	DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof(on));
}

static BOOL FixWindowStyle(HWND hwnd)
{
	LOG(L"Applying new window style");

	RECT rect;

	if (!GetWindowRect(hwnd, &rect)) {
		return FALSE;
	}

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	style &= ~(WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
	style |= WS_POPUP;

	exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE | WS_EX_APPWINDOW);
	exStyle |= WS_EX_TOOLWINDOW;

	SetWindowLongPtrW(hwnd, GWL_STYLE, style);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

	FixRendering(hwnd);
	HideBorder(hwnd);
	DisablePeek(hwnd);

	if (!SetWindowPos(hwnd, 0, rect.left, rect.top, (rect.right - rect.left), (rect.bottom - rect.top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
		wprintf(L"SetWindowPos failed: %lu\n", GetLastError());
		return FALSE;
	}

	return TRUE;
}

static VOID RefocusWindow(HWND hwnd)
{
	LOG(L"Refocusing window");

	if (IsIconic(hwnd)) {
		ShowWindow(hwnd, SW_RESTORE);
	}
	else {
		ShowWindow(hwnd, SW_SHOW);
	}

	HWND fgWindow = GetForegroundWindow();

	DWORD fgThread = fgWindow ? GetWindowThreadProcessId(fgWindow, 0) : 0;

	DWORD targetThread = GetWindowThreadProcessId(hwnd, 0);

	if (fgThread && targetThread && fgThread != targetThread) {
		AttachThreadInput(targetThread, fgThread, TRUE);
	}

	SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	SetFocus(hwnd);

	if (fgThread && targetThread && fgThread != targetThread)
		AttachThreadInput(targetThread, fgThread, FALSE);
}

static VOID RefreshWindow(HWND hwnd)
{
	LOG(L"Refreshing window state");

	RefocusWindow(hwnd);
	ShowWindow(hwnd, SW_MINIMIZE);

	DWORD ticks = GetTickCount();

	while (!IsIconic(hwnd) && (GetTickCount() - ticks) < 500) {
		Sleep(10);
	}

	ShowWindow(hwnd, SW_RESTORE);

	ticks = GetTickCount();
	while (IsIconic(hwnd) && (GetTickCount() - ticks) < 500) {
		Sleep(10);
	}

	SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	SetFocus(hwnd);
}

static VOID ClearArgs(Args* args)
{
	LOG(L"Clearing arg values");

	args->pid = 0;
	args->procName[0] = 0;
	args->title[0] = 0;
	args->hasPid = FALSE;
	args->hasName = FALSE;
	args->hasTitle = FALSE;
}

static BOOL ParseArgs(Args* args)
{
	LOG(L"Parsing arg values");

	ClearArgs(args);

	INT argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (!argv || argc < 2) {

		if (argv) LocalFree(argv); {
			return FALSE;
		}
	}

	for (INT i = 1; i < argc; ++i) {
		LPCWSTR arg = argv[i];
		if (StrCompare(arg, L"-pid") && (i + 1) < argc) {
			args->pid = (DWORD)_wcstoui64(argv[++i], 0, 10);
			args->hasPid = TRUE;
		}
		else if (StrCompare(arg, L"-name") && (i + 1) < argc) {
			wcsncpy_s(args->procName, argv[++i], _TRUNCATE);
			args->hasName = TRUE;
		}
		else if (StrCompare(arg, L"-title") && (i + 1) < argc) {
			wcsncpy_s(args->title, argv[++i], _TRUNCATE);
			args->hasTitle = TRUE;
		}
		else if (StrCompare(arg, L"-h") || StrCompare(arg, L"--help") || StrCompare(arg, L"/?")) {

			if (argv) {
				LocalFree(argv);
				return FALSE;
			}
		}
	}

	if (argv) {
		LocalFree(argv);
	}

	if (!args->hasTitle) {
		return FALSE;
	}

	if (!(args->hasPid || args->hasName)) {
		return FALSE;
	}

	return TRUE;
}

INT wmain()
{
	LOG(L"Starting");

	Args args;

	if (!ParseArgs(&args)) {
		ShowHelp();
		return 2;
	}

	//need elevation to fix afterburner
	if (!IsElevated()) {
		return RunElevated();
	}

	DWORD pid = args.hasPid ? args.pid : 0;

	if (!pid && args.hasName) {
		pid = FindPidByName(args.procName);
		if (!pid) {
			wprintf(L"Process not found: %s\n", args.procName);
			return 3;
		}
	}

	HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

	if (!processHandle) {
		wprintf(L"OpenProcess failed: PID: %lu Error: %lu\n", pid, GetLastError());
		return 4;
	}

	wprintf(L"Opened process: PID: %lu\n", pid);

	HWND window = GetWindow(pid, args.title);

	if (!window) {
		wprintf(L"Window not found: PID: %lu Title: \"%s\"\n", pid, args.title);
		CloseHandle(processHandle);
		return 5;
	}

	wprintf(L"Found window: 0x%p\n", window);

	if (!FixWindowStyle(window)) {
		wprintf(L"Failed to update window styles\n");
		CloseHandle(processHandle);
		return 6;
	}

	RefocusWindow(window);
	RefreshWindow(window);

	CloseHandle(processHandle);

	LOG(L"Done");

	return 0;
}
