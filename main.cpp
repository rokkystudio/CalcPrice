// main.cpp
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppParameterMayBeConstPtrOrRef
// ReSharper disable CppTooWideScopeInitStatement

/**
 * @file main.cpp
 * Главный модуль PriceCalc (WinAPI, без видимого окна).
 *
 * @details
 * PriceCalc:
 * - Живёт в системном трее (без видимого окна)
 * - Категории (Ctrl+1 / Ctrl+2 / Ctrl+3) через GetAsyncKeyState (без RegisterHotKey)
 * - По комбо: Ctrl+A -> Ctrl+C -> читаем clipboard -> считаем -> округляем вверх до 10 -> пишем clipboard -> Ctrl+V
 * - Настройки коэффициентов через окно "Настройки" из меню трея, сохраняются в settings.ini (в каталоге запуска)
 * - Автозапуск через реестр (HKLM\...\Run) с запросом повышения прав только по нажатию пунктов меню
 *
 * @note
 * Автозапуск реализован в Autorun.h (перенесена “рабочая” версия из main.cpp).
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
#include "Autorun.h"

/**
 * Callback-сообщение от трея (WM_APP+N).
 */
#define TRAY_CALLBACK_MSG      (WM_APP + 1)

// IDs меню
#define MENU_EXIT              1001
#define MENU_SETTINGS          1002
#define MENU_AUTORUN_ENABLE    1003
#define MENU_AUTORUN_DISABLE   1004

// ID иконки из ресурсов (main.rc: `1 ICON ...`)
#define APP_ICON_ID            1

/**
 * Коэффициенты категорий по умолчанию (потом подхватятся из ini).
 */
static double gCoeffs[3] = { 1.75, 1.6, 2.0 };

static HICON gIconBig   = nullptr;
static HICON gIconSmall = nullptr;

/**
 * Зарегистрированное сообщение "TaskbarCreated" (для восстановления иконки после рестарта Explorer).
 */
static UINT  gTaskbarCreatedMsg = 0;

/**
 * HWND скрытого главного окна (нужен, в т.ч. для корректного Stop/WM_CLOSE).
 */
static HWND  gMainHwnd = nullptr;

//=====================================================================//
// Autorun (HKLM\Software\Microsoft\Windows\CurrentVersion\Run)
// Перенесено в Autorun.h (рабочая версия, используемая меню трея).
//=====================================================================//

//=====================================================================//
// Single instance (без семафора, чтобы не “залипало” после TerminateProcess)
//=====================================================================//

/**
 * Мьютекс single-instance.
 */
static HANDLE gSingleInstanceMutex = nullptr;

/**
 * Гарантирует запуск только одного экземпляра приложения.
 *
 * @details
 * Использует CreateMutexW + проверку GetLastError()==ERROR_ALREADY_EXISTS.
 * Специально без семафора, чтобы не “залипало” после TerminateProcess.
 *
 * @return true если можно продолжать запуск, false если экземпляр уже существует.
 */
static bool EnsureSingleInstance()
{
	gSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"PriceCalc_SingleInstance_Mutex");
	if (!gSingleInstanceMutex) return true;

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(gSingleInstanceMutex);
		gSingleInstanceMutex = nullptr;
		return false;
	}

	return true;
}

/**
 * Освобождает ресурсы single-instance (закрывает мьютекс).
 */
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

/**
 * Обработчик консольных Ctrl-событий (для более мягкого Stop в IDE).
 *
 * @details
 * Если главное окно уже создано — шлём WM_CLOSE, чтобы корректно удалить иконку трея.
 *
 * @param type Тип события консоли (CTRL_*).
 * @return TRUE если событие обработано, иначе FALSE.
 */
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

			// Если окна ещё нет (очень ранний Stop) — просто скажем “обработали”.
			// Дальше CLion обычно добьёт процесс сам, но главное — не зависать.
			return TRUE;
		}
		default: /* nothing */;
	}

	return FALSE;
}

/**
 * Ставит ConsoleCtrlHandler, если у процесса есть консоль.
 *
 * @details
 * Важно: НЕ AllocConsole(), иначе CLion Stop часто не попадает в наш процесс.
 * 1) Если консоль уже есть — просто ставим handler.
 * 2) Если нет — пробуем AttachConsole к родителю (если запуск был из консоли/IDE-терминала).
 * 3) Если не получилось — просто ничего (в GUI-режиме Ctrl-событий всё равно не будет).
 */
static void SetupDebugStopHelper()
{
	bool hasConsole = (GetConsoleWindow() != nullptr);

	if (!hasConsole)
	{
		if (AttachConsole(ATTACH_PARENT_PROCESS)) {
			hasConsole = true;
		} else {
			// ERROR_ACCESS_DENIED бывает, если консоль уже есть (AttachConsole не нужен).
			if (GetLastError() == ERROR_ACCESS_DENIED) {
				hasConsole = true;
			}
		}
	}

	if (hasConsole) {
		SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
	}
}

//=====================================================================//
// Icons
//=====================================================================//

/**
 * Загружает иконку приложения заданного размера из ресурсов.
 *
 * @details
 * Используется LoadImageW с LR_SHARED (DestroyIcon не нужен).
 *
 * @param hInst HINSTANCE модуля.
 * @param cx    Ширина.
 * @param cy    Высота.
 * @return HICON или nullptr при ошибке.
 */
static HICON LoadAppIcon(HINSTANCE hInst, int cx, int cy)
{
	// LR_SHARED => DestroyIcon не нужен
	return static_cast<HICON>(LoadImageW(
		hInst,
		MAKEINTRESOURCEW(APP_ICON_ID),
		IMAGE_ICON,
		cx, cy,
		LR_DEFAULTCOLOR | LR_SHARED
	));
}

/**
 * Загружает большую/малую иконки приложения (SM_CXICON/SM_CXSMICON).
 *
 * @details
 * Если из ресурсов не получилось — fallback на IDI_APPLICATION.
 *
 * @param hInst HINSTANCE модуля.
 */
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

/**
 * Строит путь к settings.ini в текущем каталоге запуска.
 *
 * @return Полный путь к ini.
 */
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

/**
 * Проверяет, существует ли ini-файл и не является ли он каталогом.
 *
 * @param path Полный путь.
 * @return true если файл существует и это файл, иначе false.
 */
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

/**
 * Быстрый парсинг double из текста (wcstod), без “жёсткой” проверки хвоста.
 *
 * @param s Входная строка.
 * @param v [out] Значение.
 * @return true если удалось распарсить, иначе false.
 */
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

/**
 * Сохраняет коэффициенты в settings.ini (секция [Coeffs], ключи Cat1..Cat3).
 */
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

/**
 * Загружает коэффициенты из settings.ini.
 *
 * @details
 * Если ini отсутствует — создаёт его при старте с текущими значениями gCoeffs[].
 */
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

/**
 * Открывает окно настроек (SettingsWindow).
 *
 * @param hwnd Окно-владелец (скрытое окно приложения).
 * @param hInst HINSTANCE.
 */
static void OpenSettings(HWND hwnd, HINSTANCE hInst) {
	// SaveSettings вызывается из SettingsWindow при нажатии OK, т.е. изменения пишутся в ini сразу.
	SettingsWindow_Show(hInst, hwnd, gIconBig, gIconSmall, gCoeffs, &SaveSettings);
}

/**
 * Финальная уборка перед выходом:
 * - закрыть окно настроек (если висит);
 * - убрать иконку из трея;
 * - освободить single-instance.
 *
 * @param hwnd Окно приложения (сейчас не используется).
 * @param tray Состояние трея.
 */
static void CleanupBeforeExit(HWND hwnd, TrayIcon &tray)
{
	(void) hwnd;

	if (gSettingsHwnd) {
		DestroyWindow(gSettingsHwnd);
	}

	TrayIcon_Remove(tray);

	ReleaseSingleInstance();
}

/**
 * Главный WndProc скрытого окна.
 *
 * @details
 * Обрабатывает:
 * - WM_CREATE: init иконок, ini, таймера hotkey, трея;
 * - WM_TIMER: опрос хоткеев и выполнение HotkeyCalc;
 * - WM_COMMAND: меню трея (Настройки, Автозапуск, Выход);
 * - TRAY_CALLBACK_MSG: события трея (dblclick/menu);
 * - WM_ENDSESSION/WM_DESTROY: корректная остановка.
 *
 * @param hwnd Окно.
 * @param msg  Сообщение.
 * @param wParam WPARAM.
 * @param lParam LPARAM.
 * @return LRESULT.
 */
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

			HotkeyCalc_Init(hotkey);
			HotkeyCalc_Start(hwnd);

			TrayIcon_Init(tray, hwnd, TRAY_CALLBACK_MSG, gIconSmall,
				L"PriceCalc (Ctrl+1/2/3)",
				MENU_SETTINGS,
				MENU_AUTORUN_ENABLE, MENU_AUTORUN_DISABLE,
				MENU_EXIT
			);

			TrayIcon_Add(tray);

			return 0;
		}

		case WM_TIMER: {
			HotkeyCalc_OnTimer(hotkey, tray, wParam, gCoeffs);
			return 0;
		}

		case WM_COMMAND:
		{
			const UINT id = LOWORD(wParam);

			if (id == MENU_EXIT) {
				DestroyWindow(hwnd);
				return 0;
			}

			if (id == MENU_SETTINGS) {
				OpenSettings(hwnd, hInst);
				return 0;
			}

			if (id == MENU_AUTORUN_ENABLE)
			{
				if (Autorun_TryEnableWithElevationIfNeeded()) {
					TrayIcon_ShowBalloon(tray, L"Автозапуск", L"Включено.");
				} else {
					TrayIcon_ShowBalloon(tray, L"Автозапуск", L"Не удалось включить автозапуск.");
				}
				return 0;
			}

			if (id == MENU_AUTORUN_DISABLE)
			{
				if (Autorun_TryDisableWithElevationIfNeeded()) {
					TrayIcon_ShowBalloon(tray, L"Автозапуск", L"Отключено.");
				} else {
					TrayIcon_ShowBalloon(tray, L"Автозапуск", L"Не удалось отключить автозапуск.");
				}
				return 0;
			}

			break;
		}

		case TRAY_CALLBACK_MSG:
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

/**
 * Точка входа Unicode (GUI, без консоли).
 *
 * @details
 * 1) Если это elevated-запуск для автозапуска — выполняем Autorun_TryHandleCommandLine() и выходим.
 * 2) Single instance.
 * 3) Common controls init.
 * 4) Регистрируем класс и создаём скрытое окно для message loop / tray.
 *
 * @param hInstance HINSTANCE.
 * @return Код завершения процесса.
 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	const int autorunRc = Autorun_TryHandleCommandLine();
	if (autorunRc >= 0) {
		return autorunRc;
	}

	if (!EnsureSingleInstance()) return 0;

	SetupDebugStopHelper();

	// Визуальные стили даёт manifest, а это — инициализация common controls
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);

	gTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

	const wchar_t *clsName = L"PriceCalcHiddenWindow";

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
		L"PriceCalc",
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
