#pragma once

/**
 * @file TrayIcon.h
 * Утилиты для работы с иконкой приложения в системном трее (Windows Shell_NotifyIcon).
 *
 * @details
 * Header-only обёртка вокруг NOTIFYICONDATAW:
 * - добавление/удаление иконки в трее;
 * - показ balloon-уведомлений;
 * - контекстное меню (“Настройки”/“Выход”);
 * - пункт-группа “Автозапуск” -> (“Включить”/“Отключить”);
 * - разбор callback-сообщений от трея (двойной клик / правая кнопка).
 *
 * @note
 * В этом проекте клики/меню по трею стабильно работают в “классическом” режиме
 * (без NOTIFYICON_VERSION_4 / NIM_SETVERSION), обрабатывая WM_* события.
 *
 * @note
 * Чтобы убирать “висячую” иконку после жёсткого завершения процесса (TerminateProcess/CLion Stop),
 * используем NIF_GUID со стабильным GUID и перед NIM_ADD делаем NIM_DELETE по GUID.
 *
 * @note
 * В callback не делаем жёсткой проверки wParam==uID: в связке с GUID это может приводить
 * к пропуску событий в некоторых конфигурациях. Ориентируемся на lParam (evt).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/**
 * Идентификатор иконки в трее (uID).
 */
static constexpr UINT kTrayIconId = 1;

/**
 * Стабильный GUID для идентификации иконки между запусками.
 *
 * @details
 * Нужен для удаления “хвостов” (висячих иконок) при следующем запуске приложения через NIM_DELETE.
 */
static constexpr GUID kTrayIconGuid = {
	0x5b2f7df3, 0x6f0a, 0x4c6b, { 0x9f, 0x1e, 0x6a, 0x2b, 0x4f, 0xc1, 0x12, 0x90 }
};

/**
 * Действия, которые может запросить обработчик callback от трея.
 */
enum TrayAction {
	TrayAction_None = 0,      /**< Ничего не делать. */
	TrayAction_OpenSettings,  /**< Открыть окно настроек. */
	TrayAction_ShowMenu       /**< Показать контекстное меню трея. */
};

/**
 * Состояние трея (обёртка над NOTIFYICONDATAW) и вспомогательные поля.
 */
struct TrayIcon
{
	NOTIFYICONDATAW nid = {}; /**< Структура Windows Shell для работы с иконкой. */
	UINT callbackMsg = 0;     /**< Сообщение (WM_APP+N), которое будет приходить в окно от трея. */

	UINT menuSettings = 0;        /**< ID пункта меню “Настройки” (WM_COMMAND). */

	UINT menuAutorunEnable = 0;   /**< ID пункта меню “Автозапуск/Включить” (WM_COMMAND). */
	UINT menuAutorunDisable = 0;  /**< ID пункта меню “Автозапуск/Отключить” (WM_COMMAND). */

	UINT menuExit = 0;            /**< ID пункта меню “Выход” (WM_COMMAND). */

	DWORD lastClickTick = 0;      /**< Тайминг кликов для “двоеклика” по двум WM_LBUTTONUP. */
};

/**
 * Инициализирует структуру TrayIcon.
 *
 * @param t                 Состояние трея.
 * @param hwnd              Окно-получатель callbackMsg (скрытое окно приложения).
 * @param callbackMsg       Пользовательское сообщение, которое будет приходить от трея.
 * @param icon              Иконка для отображения в трее.
 * @param tip               Tooltip (может быть nullptr).
 * @param menuSettings      ID пункта меню “Настройки”.
 * @param menuAutorunEnable ID пункта меню “Автозапуск/Включить”.
 * @param menuAutorunDisable ID пункта меню “Автозапуск/Отключить”.
 * @param menuExit          ID пункта меню “Выход”.
 */
static void TrayIcon_Init(TrayIcon &t, HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t *tip,
	UINT menuSettings, UINT menuAutorunEnable, UINT menuAutorunDisable, UINT menuExit)
{
	memset(&t, 0, sizeof(t));

	t.callbackMsg = callbackMsg;
	t.menuSettings = menuSettings;

	t.menuAutorunEnable = menuAutorunEnable;
	t.menuAutorunDisable = menuAutorunDisable;

	t.menuExit = menuExit;
	t.lastClickTick = 0;

	t.nid.cbSize = sizeof(t.nid);
	t.nid.hWnd = hwnd;
	t.nid.uID = kTrayIconId;

	// NIF_GUID нужен, чтобы убирать “хвосты” при следующем запуске
	t.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
	t.nid.guidItem = kTrayIconGuid;

	t.nid.uCallbackMessage = callbackMsg;
	t.nid.hIcon = icon;

	if (tip) {
		wcsncpy_s(t.nid.szTip, tip, _TRUNCATE);
	}
}

/**
 * Добавляет иконку в трей.
 *
 * @details
 * Перед добавлением выполняется NIM_DELETE по GUID для удаления “висячей” иконки
 * после аварийной остановки процесса.
 *
 * @param t Состояние трея.
 */
static void TrayIcon_Add(TrayIcon &t)
{
	// Удаляем “старую” запись по GUID (на случай прошлой аварийной остановки)
	Shell_NotifyIconW(NIM_DELETE, &t.nid);

	// “Классический” режим: без NIM_SETVERSION/NOTIFYICON_VERSION_4
	Shell_NotifyIconW(NIM_ADD, &t.nid);
}

/**
 * Удаляет иконку из трея.
 *
 * @details
 * Сначала удаляет по GUID (надёжнее), затем делает fallback удаление по (hWnd + uID).
 * Для устойчивости NIM_DELETE выполняется дважды.
 *
 * @param t Состояние трея.
 */
static void TrayIcon_Remove(TrayIcon &t)
{
	if (!t.nid.hWnd) return;

	// 1) Удаляем по GUID (если он есть)
	Shell_NotifyIconW(NIM_DELETE, &t.nid);
	Shell_NotifyIconW(NIM_DELETE, &t.nid);

	// 2) Fallback: удалить по hWnd+uID
	NOTIFYICONDATAW tmp = {};
	tmp.cbSize = sizeof(tmp);
	tmp.hWnd = t.nid.hWnd;
	tmp.uID = t.nid.uID;

	Shell_NotifyIconW(NIM_DELETE, &tmp);
	Shell_NotifyIconW(NIM_DELETE, &tmp);

	memset(&t.nid, 0, sizeof(t.nid));
	t.callbackMsg = 0;

	t.menuSettings = 0;
	t.menuAutorunEnable = 0;
	t.menuAutorunDisable = 0;
	t.menuExit = 0;

	t.lastClickTick = 0;
}

/**
 * Показывает balloon-уведомление от иконки в трее.
 *
 * @param t     Состояние трея.
 * @param title Заголовок (может быть nullptr).
 * @param text  Текст (может быть nullptr).
 */
static void TrayIcon_ShowBalloon(const TrayIcon &t, const wchar_t *title, const wchar_t *text)
{
	if (!t.nid.hWnd) return;

	NOTIFYICONDATAW tmp = {};
	tmp.cbSize = sizeof(tmp);

	// При NIF_GUID модификацию делаем по GUID (так надёжнее)
	tmp.uFlags = NIF_INFO | NIF_GUID;
	tmp.guidItem = kTrayIconGuid;

	// На всякий случай оставим и hWnd/uID
	tmp.hWnd = t.nid.hWnd;
	tmp.uID = t.nid.uID;

	if (title) {
		wcsncpy_s(tmp.szInfoTitle, title, _TRUNCATE);
	}

	if (text) {
		wcsncpy_s(tmp.szInfo, text, _TRUNCATE);
	}

	tmp.dwInfoFlags = NIIF_INFO;

	Shell_NotifyIconW(NIM_MODIFY, &tmp);
}

/**
 * Показывает контекстное меню трея в заданной точке.
 *
 * @param t    Состояние трея.
 * @param hwnd Окно-владелец меню (обычно скрытое окно приложения).
 * @param pt   Координаты курсора (screen coordinates).
 */
static void TrayIcon_ShowContextMenu(const TrayIcon &t, HWND hwnd, POINT pt)
{
	HMENU menu = CreatePopupMenu();
	if (!menu) return;

	AppendMenuW(menu, MF_STRING, t.menuSettings, L"Настройки");

	// “Группа” (submenu): Автозапуск -> (Включить / Отключить)
	HMENU autorun = CreatePopupMenu();
	if (autorun) {
		AppendMenuW(autorun, MF_STRING, t.menuAutorunEnable, L"Включить");
		AppendMenuW(autorun, MF_STRING, t.menuAutorunDisable, L"Отключить");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(autorun), L"Автозапуск");
	}

	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, t.menuExit, L"Выход");

	SetForegroundWindow(hwnd);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);

	PostMessageW(hwnd, WM_NULL, 0, 0);

	DestroyMenu(menu);
}

/**
 * Обрабатывает callback-сообщение от трея и возвращает требуемое действие.
 *
 * @details
 * Ориентируемся на lParam (evt):
 * - WM_LBUTTONDBLCLK: открыть настройки;
 * - WM_LBUTTONUP дважды в пределах GetDoubleClickTime(): открыть настройки;
 * - WM_RBUTTONUP / WM_CONTEXTMENU: показать меню.
 *
 * @param t      Состояние трея.
 * @param wParam WPARAM из сообщения callback (может игнорироваться).
 * @param lParam LPARAM из сообщения callback (содержит код события WM_*).
 * @return TrayAction Что нужно сделать в WndProc.
 */
static TrayAction TrayIcon_HandleCallback(TrayIcon &t, WPARAM wParam, LPARAM lParam)
{
	(void)wParam;

	const UINT evt = static_cast<UINT>(lParam);

	if (evt == WM_LBUTTONDBLCLK) {
		t.lastClickTick = 0;
		return TrayAction_OpenSettings;
	}

	// Иногда DBLCLK не приходит стабильно — добиваем по двум UP
	if (evt == WM_LBUTTONUP)
	{
		const DWORD now = GetTickCount();
		// ReSharper disable once CppTooWideScopeInitStatement
		const UINT dbl = GetDoubleClickTime();

		if (t.lastClickTick && (now - t.lastClickTick <= dbl)) {
			t.lastClickTick = 0;
			return TrayAction_OpenSettings;
		}

		t.lastClickTick = now;
		return TrayAction_None;
	}

	if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
		return TrayAction_ShowMenu;
	}

	return TrayAction_None;
}
