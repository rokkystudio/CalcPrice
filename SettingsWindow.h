// ReSharper disable CppDFAConstantParameter
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst

#pragma once

/**
 * SettingsWindow
 * - Окно настроек коэффициентов (header-only)
 * - Системный шрифт
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <cwchar>

/**
 * ID контролов окна настроек.
 * Используются как HMENU в CreateWindowExW() и в GetDlgItem().
 */
#define IDC_EDIT1      2001
#define IDC_EDIT2      2002
#define IDC_EDIT3      2003
#define IDC_BTN_OK     2101
#define IDC_BTN_CANCEL 2102

/**
 * Константы компоновки (пиксели).
 * Подобраны под текущие размеры окна/элементов.
 */
#define SETTINGS_WIN_W          240
#define SETTINGS_WIN_H          210

#define SETTINGS_TITLE_X        12
#define SETTINGS_TITLE_Y        10
#define SETTINGS_TITLE_W        210
#define SETTINGS_TITLE_H        18

#define SETTINGS_LABEL_X        12
#define SETTINGS_LABEL_W        130
#define SETTINGS_LABEL_H        18

#define SETTINGS_ROW1_Y         40
#define SETTINGS_ROW2_Y         70
#define SETTINGS_ROW3_Y         100

#define SETTINGS_EDIT_X         150
#define SETTINGS_EDIT_W         60
#define SETTINGS_EDIT_H         22

// чтобы EDIT был по центру строки
#define SETTINGS_EDIT_Y_OFFSET  (-2)

#define SETTINGS_BTN_W          80
#define SETTINGS_BTN_H          26
#define SETTINGS_BTN_Y          135
#define SETTINGS_BTN_OK_X       32
#define SETTINGS_BTN_CANCEL_X   122

// Для x64: ID -> HMENU корректно через INT_PTR
#define SETTINGS_ID_TO_HMENU(id) (reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)))

/**
 * Коллбек сохранения (например, запись в файл/ini).
 */
typedef void (*SettingsSaveFn)();

static HWND  gSettingsHwnd = nullptr;
static HFONT gSettingsFont = nullptr;

static double *gSettingsCoeffs = nullptr;
static SettingsSaveFn gSettingsSaveFn = nullptr;

/**
 * Центрирует окно в рабочей области (без таскбара).
 * @param hwnd окно
 * @param w    ширина окна
 * @param h    высота окна
 */
static void SettingsWindow_CenterOnScreen(HWND hwnd, const int w, const int h)
{
	RECT rc = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);

	int x = rc.left + ((rc.right - rc.left) - w) / 2;
	int y = rc.top  + ((rc.bottom - rc.top) - h) / 2;

	MoveWindow(hwnd, x, y, w, h, TRUE);
}

/**
 * Создаёт системный GUI-шрифт (lfMessageFont).
 * @return HFONT (если не удалось — DEFAULT_GUI_FONT)
 */
static HFONT SettingsWindow_CreateSystemFont()
{
	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof(ncm);

	if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
		return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}

	HFONT h = CreateFontIndirectW(&ncm.lfMessageFont);
	if (!h) {
		return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}

	return h;
}

/**
 * Применяет шрифт ко всем дочерним контролам окна.
 * @param hwnd  окно-родитель
 * @param hFont шрифт
 */
static void SettingsWindow_ApplyFontToChildren(HWND hwnd, HFONT hFont)
{
	HWND child = GetWindow(hwnd, GW_CHILD);
	while (child) {
		SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
		child = GetWindow(child, GW_HWNDNEXT);
	}
}

/**
 * Записывает double в EDIT.
 * @param hEdit EDIT
 * @param v     значение
 */
static void SettingsWindow_SetEditDouble(HWND hEdit, const double v) {
	wchar_t buf[64] = {};
	swprintf_s(buf, L"%.10g", v);
	SetWindowTextW(hEdit, buf);
}

/**
 * Пытается распарсить double из строки.
 * @param s входная строка
 * @param v [out] результат
 * @return true если распарсили
 */
static bool SettingsWindow_TryParseDoubleText(const std::wstring &s, double &v)
{
	v = 0.0;

	if (s.empty()) return false;

	wchar_t *end = nullptr;
	const double tmp = wcstod(s.c_str(), &end);

	if (!end || end == s.c_str()) return false;

	// Разрешим хвостовые пробелы/переводы строк
	while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') {
		++end;
	}

	if (*end != L'\0') return false;

	v = tmp;
	return true;
}

/**
 * Читает текст из EDIT и парсит double.
 * Разрешает запятую как десятичный разделитель.
 * @param hEdit EDIT
 * @param v [out] результат
 * @return true если распарсили
 */
static bool SettingsWindow_GetEditDouble(HWND hEdit, double &v)
{
	wchar_t buf[128] = {};
	GetWindowTextW(hEdit, buf, _countof(buf));

	std::wstring s = buf;
	// Разрешим запятую
	for (wchar_t & i : s) {
		if (i == L',') {
			i = L'.';
		}
	}

	return SettingsWindow_TryParseDoubleText(s, v);
}

/**
 * WndProc окна настроек.
 * Создаёт контролы, обрабатывает OK/Отмена, сохраняет коэффициенты.
 * @param hwnd   окно
 * @param msg    сообщение Windows
 * @param wParam параметр сообщения
 * @param lParam параметр сообщения
 * @return результат обработки сообщения
 */
static LRESULT CALLBACK SettingsWindow_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CREATE:
		{
			gSettingsFont = SettingsWindow_CreateSystemFont();

			CreateWindowExW(0, L"STATIC", L"Коэффициенты категорий:",
				WS_CHILD | WS_VISIBLE,
				SETTINGS_TITLE_X, SETTINGS_TITLE_Y, SETTINGS_TITLE_W, SETTINGS_TITLE_H,
				hwnd, nullptr, nullptr, nullptr);

			CreateWindowExW(0, L"STATIC", L"Категория 1 (CTRL+1):",
				WS_CHILD | WS_VISIBLE,
				SETTINGS_LABEL_X, SETTINGS_ROW1_Y, SETTINGS_LABEL_W, SETTINGS_LABEL_H,
				hwnd, nullptr, nullptr, nullptr);

			HWND e1 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
				WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				SETTINGS_EDIT_X, SETTINGS_ROW1_Y + SETTINGS_EDIT_Y_OFFSET, SETTINGS_EDIT_W, SETTINGS_EDIT_H,
				hwnd, SETTINGS_ID_TO_HMENU(IDC_EDIT1), nullptr, nullptr);

			CreateWindowExW(0, L"STATIC", L"Категория 2 (CTRL+2):",
				WS_CHILD | WS_VISIBLE,
				SETTINGS_LABEL_X, SETTINGS_ROW2_Y, SETTINGS_LABEL_W, SETTINGS_LABEL_H,
				hwnd, nullptr, nullptr, nullptr);

			HWND e2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
				WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				SETTINGS_EDIT_X, SETTINGS_ROW2_Y + SETTINGS_EDIT_Y_OFFSET, SETTINGS_EDIT_W, SETTINGS_EDIT_H,
				hwnd, SETTINGS_ID_TO_HMENU(IDC_EDIT2), nullptr, nullptr);

			CreateWindowExW(0, L"STATIC", L"Категория 3 (CTRL+3):",
				WS_CHILD | WS_VISIBLE,
				SETTINGS_LABEL_X, SETTINGS_ROW3_Y, SETTINGS_LABEL_W, SETTINGS_LABEL_H,
				hwnd, nullptr, nullptr, nullptr);

			HWND e3 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
				WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				SETTINGS_EDIT_X, SETTINGS_ROW3_Y + SETTINGS_EDIT_Y_OFFSET, SETTINGS_EDIT_W, SETTINGS_EDIT_H,
				hwnd, SETTINGS_ID_TO_HMENU(IDC_EDIT3), nullptr, nullptr);

			CreateWindowExW(0, L"BUTTON", L"OK",
				WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				SETTINGS_BTN_OK_X, SETTINGS_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H,
				hwnd, SETTINGS_ID_TO_HMENU(IDC_BTN_OK), nullptr, nullptr);

			CreateWindowExW(0, L"BUTTON", L"Отмена",
				WS_CHILD | WS_VISIBLE,
				SETTINGS_BTN_CANCEL_X, SETTINGS_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H,
				hwnd, SETTINGS_ID_TO_HMENU(IDC_BTN_CANCEL), nullptr, nullptr);

			if (gSettingsCoeffs) {
				SettingsWindow_SetEditDouble(e1, gSettingsCoeffs[0]);
				SettingsWindow_SetEditDouble(e2, gSettingsCoeffs[1]);
				SettingsWindow_SetEditDouble(e3, gSettingsCoeffs[2]);
			}

			SettingsWindow_ApplyFontToChildren(hwnd, gSettingsFont);

			SettingsWindow_CenterOnScreen(hwnd, SETTINGS_WIN_W, SETTINGS_WIN_H);
			return 0;
		}

		case WM_COMMAND:
		{
			const UINT id = LOWORD(wParam);

			if (id == IDC_BTN_CANCEL) {
				DestroyWindow(hwnd);
				return 0;
			}

			if (id == IDC_BTN_OK)
			{
				double c1 = 0.0, c2 = 0.0, c3 = 0.0;

				HWND e1 = GetDlgItem(hwnd, IDC_EDIT1);
				HWND e2 = GetDlgItem(hwnd, IDC_EDIT2);
				HWND e3 = GetDlgItem(hwnd, IDC_EDIT3);

				if (!SettingsWindow_GetEditDouble(e1, c1) || c1 <= 0.0 ||
					!SettingsWindow_GetEditDouble(e2, c2) || c2 <= 0.0 ||
					!SettingsWindow_GetEditDouble(e3, c3) || c3 <= 0.0)
				{
					MessageBoxW(hwnd, L"Проверьте коэффициенты (должны быть числа > 0).",
						L"Настройки", MB_OK | MB_ICONWARNING);
					return 0;
				}

				if (gSettingsCoeffs) {
					gSettingsCoeffs[0] = c1;
					gSettingsCoeffs[1] = c2;
					gSettingsCoeffs[2] = c3;
				}

				if (gSettingsSaveFn) {
					gSettingsSaveFn();
				}

				DestroyWindow(hwnd);
				return 0;
			}

			break;
		}

		case WM_DESTROY:
		{
			if (gSettingsFont && gSettingsFont != GetStockObject(DEFAULT_GUI_FONT)) {
				DeleteObject(gSettingsFont);
			}

			gSettingsFont   = nullptr;
			gSettingsHwnd   = nullptr;
			gSettingsCoeffs = nullptr;
			gSettingsSaveFn = nullptr;
			return 0;
		}

		default: /* nothing */;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/**
 * Показывает окно настроек (singleton).
 * Если окно уже создано — просто поднимает его.
 * @param hInst     HINSTANCE приложения
 * @param owner     окно-владелец (сейчас не используется)
 * @param hIconBig  большая иконка окна
 * @param hIconSmall малая иконка окна
 * @param coeffs    массив коэффициентов [3]
 * @param saveFn    коллбек сохранения
 */
static void SettingsWindow_Show(HINSTANCE hInst, HWND owner, HICON hIconBig, HICON hIconSmall, double coeffs[3], SettingsSaveFn saveFn)
{
	UNREFERENCED_PARAMETER(owner);

	if (gSettingsHwnd) {
		ShowWindow(gSettingsHwnd, SW_SHOWNORMAL);
		SetForegroundWindow(gSettingsHwnd);
		return;
	}

	gSettingsCoeffs = coeffs;
	gSettingsSaveFn = saveFn;

    auto clsName = L"TrayHotkeyCalcSettings";

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.hInstance = hInst;
	wc.lpfnWndProc = SettingsWindow_WndProc;
	wc.lpszClassName = clsName;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = hIconBig;
	wc.hIconSm = hIconSmall;

	WNDCLASSEXW exists = {};
	exists.cbSize = sizeof(exists);

	if (!GetClassInfoExW(hInst, clsName, &exists)) {
		RegisterClassExW(&wc);
	}

    constexpr DWORD exStyle = WS_EX_APPWINDOW;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW &
        ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME);

	gSettingsHwnd = CreateWindowExW(
		exStyle, clsName, L"Настройки", style,
		CW_USEDEFAULT, CW_USEDEFAULT, SETTINGS_WIN_W, SETTINGS_WIN_H,
		nullptr, nullptr, hInst, nullptr
	);

	if (gSettingsHwnd) {
		ShowWindow(gSettingsHwnd, SW_SHOWNORMAL);
		UpdateWindow(gSettingsHwnd);
	}
}
