// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppParameterMayBeConstPtrOrRef
// ReSharper disable CppTooWideScopeInitStatement

/**
 * TrayHotkeyCalc
 * - Живёт в системном трее (без видимого окна)
 * - Категории (Ctrl+1 / Ctrl+2 / Ctrl+3) через GetAsyncKeyState (без RegisterHotKey)
 * - По комбо: Ctrl+A -> Ctrl+C -> читаем clipboard -> считаем -> округляем вверх до 10 -> пишем clipboard -> Ctrl+V
 * - Настройки коэффициентов через окно "Настройки" из меню трея, сохраняются в settings.ini (в каталоге запуска)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <string>
#include <cwchar>
#include <cmath>
#include <cstring>
#include <iterator>

#include "SettingsWindow.h"
#include "TrayIcon.h"
#include "HotkeyCalc.h"

static constexpr UINT  kTrayCallbackMsg = WM_APP + 1;

// IDs меню
static constexpr UINT  kMenuSettings = 1002;
static constexpr UINT  kMenuExit     = 1001;

// ID иконки из ресурсов (main.rc: `1 ICON ...`)
static constexpr int   kAppIconId    = 1;

// Коэффициенты по умолчанию (потом подхватятся из ini)
static double gCoeffs[3] = { 1.75, 1.6, 2.0 };

static HICON gIconBig   = nullptr;
static HICON gIconSmall = nullptr;

static UINT  gTaskbarCreatedMsg = 0;

static HWND  gMainHwnd = nullptr;

//=====================================================================//
// Single instance (без семафора, чтобы не “залипало” после TerminateProcess)
//=====================================================================//

static HANDLE gSingleInstanceMutex = nullptr;

static bool EnsureSingleInstance()
{
	gSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"TrayHotkeyCalc_SingleInstance_Mutex");
	if (!gSingleInstanceMutex) return true;

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(gSingleInstanceMutex);
		gSingleInstanceMutex = nullptr;
		return false;
	}

	return true;
}

static void ReleaseSingleInstance()
{
	if (gSingleInstanceMutex) {
		CloseHandle(gSingleInstanceMutex);
		gSingleInstanceMutex = nullptr;
	}
}

//=====================================================================//
// Debug stop helper (CLion Stop)
//=====================================================================//

static BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
	switch (type)
	{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_SHUTDOWN_EVENT:
		case CTRL_LOGOFF_EVENT:
		{
			if (gMainHwnd)
			{
				PostMessageW(gMainHwnd, WM_CLOSE, 0, 0);
				return TRUE;
			}
			break;
		}
		default: /* nothing */;
	}

	return FALSE;
}

static void SetupDebugStopHelper()
{
#if !defined(NDEBUG)
	// Для Debug: позволяем "мягкому" Stop закрывать приложение.
	// Сначала пробуем прицепиться к консоли родителя, если нет - создаём свою.
	if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
		AllocConsole();
	}

	SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
#endif
}

//=====================================================================//
// Icons
//=====================================================================//

static HICON LoadAppIcon(HINSTANCE hInst, int cx, int cy)
{
	// LR_SHARED => DestroyIcon не нужен
	return static_cast<HICON>(LoadImageW(
		hInst,
		MAKEINTRESOURCEW(kAppIconId),
		IMAGE_ICON,
		cx, cy,
		LR_DEFAULTCOLOR | LR_SHARED
	));
}

static void LoadAppIcons(HINSTANCE hInst)
{
	const int cxBig   = GetSystemMetrics(SM_CXICON);
	const int cyBig   = GetSystemMetrics(SM_CYICON);
	const int cxSmall = GetSystemMetrics(SM_CXSMICON);
	const int cySmall = GetSystemMetrics(SM_CYSMICON);

	gIconBig = LoadAppIcon(hInst, cxBig, cyBig);
	gIconSmall = LoadAppIcon(hInst, cxSmall, cySmall);

	if (!gIconBig) {
		gIconBig = LoadIconW(nullptr, IDI_APPLICATION);
	}

	if (!gIconSmall) {
		gIconSmall = LoadIconW(nullptr, IDI_APPLICATION);
	}
}

//=====================================================================//
// INI helpers (settings.ini в каталоге запуска)
//=====================================================================//

static std::wstring BuildIniPath()
{
	DWORD need = GetCurrentDirectoryW(0, nullptr);
	if (need == 0) {
		return L"settings.ini";
	}

	std::wstring dir;
	dir.resize(need);

	DWORD got = GetCurrentDirectoryW(need, &dir[0]);
	if (got == 0) {
		return L"settings.ini";
	}

	dir.resize(got);

	if (!dir.empty())
	{
		const wchar_t last = dir.back();
		if (last != L'\\' && last != L'/') {
			dir += L"\\";
		}
	}

	dir += L"settings.ini";
	return dir;
}

static bool IniFileExists(const std::wstring &path)
{
	const DWORD a = GetFileAttributesW(path.c_str());
	if (a == INVALID_FILE_ATTRIBUTES) {
		return false;
	}

	if ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		return false;
	}

	return true;
}

static bool TryParseDoubleText(const std::wstring &s, double &v)
{
	v = 0.0;
	if (s.empty()) return false;

	wchar_t *end = nullptr;
	const double tmp = wcstod(s.c_str(), &end);

	if (!end || end == s.c_str()) return false;

	v = tmp;
	return true;
}

static void SaveSettings()
{
	const std::wstring ini = BuildIniPath();

	for (int i = 0; i < 3; i++)
	{
		wchar_t key[16] = {};
		swprintf_s(key, L"Cat%d", i + 1);

		wchar_t val[64] = {};
		swprintf_s(val, L"%.10g", gCoeffs[i]);

		WritePrivateProfileStringW(L"Coeffs", key, val, ini.c_str());
	}
}

static void LoadSettings()
{
	const std::wstring ini = BuildIniPath();
	const bool existed = IniFileExists(ini);

	for (int i = 0; i < 3; i++)
	{
		wchar_t key[16] = {};
		swprintf_s(key, L"Cat%d", i + 1);

		wchar_t defVal[64] = {};
		swprintf_s(defVal, L"%.10g", gCoeffs[i]);

		wchar_t buf[64] = {};
		GetPrivateProfileStringW(L"Coeffs", key, defVal, buf, std::size(buf), ini.c_str());

		double v = 0.0;
		if (TryParseDoubleText(buf, v) && v > 0.0) {
			gCoeffs[i] = v;
		}
	}

	// Если файла не было — создаём при старте (с текущими значениями)
	if (!existed) {
		SaveSettings();
	}
}

//=====================================================================//
// Main window
//=====================================================================//

static void OpenSettings(HWND hwnd, HINSTANCE hInst) {
	// SaveSettings вызывается из SettingsWindow при нажатии OK, т.е. изменения пишутся в ini сразу.
	SettingsWindow_Show(hInst, hwnd, gIconBig, gIconSmall, gCoeffs, &SaveSettings);
}

static void CleanupBeforeExit(HWND hwnd, TrayIcon &tray)
{
	(void) hwnd;

	if (gSettingsHwnd) {
		DestroyWindow(gSettingsHwnd);
	}

	TrayIcon_Remove(tray);

	ReleaseSingleInstance();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static TrayIcon tray = {};
	static HINSTANCE hInst = nullptr;
	static HotkeyCalc hotkey = {};

	if (gTaskbarCreatedMsg && msg == gTaskbarCreatedMsg) {
		// Explorer перезапустился: иконку нужно добавить заново
		TrayIcon_Add(tray);
		return 0;
	}

	switch (msg)
	{
		case WM_CREATE:
		{
			hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
			gMainHwnd = hwnd;

			LoadAppIcons(hInst);

			LoadSettings();

			TrayIcon_Init(tray, hwnd, kTrayCallbackMsg, gIconSmall, L"TrayHotkeyCalc (Ctrl+1/2/3)", kMenuSettings, kMenuExit);
			TrayIcon_Add(tray);

			// HotkeyCalc: один таймер (GetAsyncKeyState)
			HotkeyCalc_Init(hotkey);
			HotkeyCalc_Start(hwnd);

			return 0;
		}

		case WM_TIMER:
		{
			if (HotkeyCalc_OnTimer(hotkey, tray, wParam, gCoeffs)) {
				return 0;
			}
			break;
		}

		case WM_COMMAND:
		{
			const UINT id = LOWORD(wParam);

			if (id == kMenuExit) {
				DestroyWindow(hwnd);
				return 0;
			}

			if (id == kMenuSettings) {
				OpenSettings(hwnd, hInst);
				return 0;
			}

			break;
		}

		case kTrayCallbackMsg:
		{
			const TrayAction a = TrayIcon_HandleCallback(tray, wParam, lParam);

			if (a == TrayAction_OpenSettings) {
				OpenSettings(hwnd, hInst);
				return 0;
			}

			if (a == TrayAction_ShowMenu) {
				POINT pt = {};
				GetCursorPos(&pt);
				TrayIcon_ShowContextMenu(tray, hwnd, pt);
				return 0;
			}

			break;
		}

		case WM_QUERYENDSESSION: {
			// ОС спрашивает “можно ли завершать сеанс”. Не тормозим и не блокируем.
			return TRUE;
		}

		case WM_ENDSESSION:
		{
			// Реальное завершение сеанса (выключение/перезагрузка/выход)
			if (wParam)
			{
				HotkeyCalc_Stop(hwnd);
				CleanupBeforeExit(hwnd, tray);
				PostQuitMessage(0);
			}
			return 0;
		}

		case WM_CLOSE: {
			DestroyWindow(hwnd);
			return 0;
		}

		case WM_DESTROY:
		{
			HotkeyCalc_Stop(hwnd);

			CleanupBeforeExit(hwnd, tray);
			gMainHwnd = nullptr;

			PostQuitMessage(0);
			return 0;
		}
		default: /* nothing */;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	if (!EnsureSingleInstance()) return 0;

	SetupDebugStopHelper();

	// Визуальные стили даёт manifest, а это — инициализация common controls
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);

	gTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

	const wchar_t *clsName = L"TrayHotkeyCalcHiddenWindow";

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.hInstance = hInstance;
	wc.lpfnWndProc = WndProc;
	wc.lpszClassName = clsName;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

	LoadAppIcons(hInstance);
	wc.hIcon = gIconBig;
	wc.hIconSm = gIconSmall;

	if (!RegisterClassExW(&wc)) {
		ReleaseSingleInstance();
		return 1;
	}

	// Скрытое окно нужно для message loop / tray callbacks
	HWND hwnd = CreateWindowExW(
		0, clsName,
		L"TrayHotkeyCalc",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 300, 200,
		nullptr, nullptr, hInstance, nullptr
	);

	if (!hwnd) {
		ReleaseSingleInstance();
		return 2;
	}

	MSG m = {};
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}

	return 0;
}
