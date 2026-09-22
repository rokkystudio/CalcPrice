// HotkeyCalc.h
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppDFAUnreachableFunctionCall
// ReSharper disable CppParameterMayBeConst

#pragma once

/**
 * @file HotkeyCalc.h
 * Горячие клавиши и обработка буфера обмена для PriceCalc.
 *
 * @details
 * Header-only модуль, который:
 * - опрашивает состояние клавиш через GetAsyncKeyState (без RegisterHotKey);
 * - ловит комбинации Ctrl+1 / Ctrl+2 / Ctrl+3 “по фронту” (down && !prevDown);
 * - выполняет цепочку действий в активном окне/ячейке:
 *   1) Alt+Enter
 *   2) Ctrl+A
 *   3) Ctrl+C
 *   4) clipboard * coeff -> clipboard
 *   5) Right
 *   6) Ctrl+V
 *   7) Enter
 * - при успешной вставке показывает balloon у трея:
 *   категория + коэффициент, закупочная цена (взятая), цена продажи (вставленная).
 *
 * @note
 * Чтобы надёжно понять, что Ctrl+C действительно сработал, перед Ctrl+C
 * выставляем в буфер обмена уникальный маркер (если в буфере уже есть текст),
 * а затем ждём, что буфер изменится (и что в буфере уже не маркер).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <cwctype>
#include <cwchar>
#include <cmath>
#include <cstring>

#include "TrayIcon.h"

#define HC_TIMER_ID                 1
#define HC_POLL_MS                  25

#define HC_SLEEP_AFTER_RELEASE_MS   8

#define HC_SLEEP_AFTER_ALTENTER_MS  30
#define HC_SLEEP_AFTER_SELECT_MS    30
#define HC_SLEEP_AFTER_MOVE_MS      10
#define HC_SLEEP_AFTER_WRITE_MS     10

#define HC_CLIPBOARD_TIMEOUT_MS     800
#define HC_CLIPBOARD_POLL_SLEEP_MS  10

#define HC_INTBUF_CHARS             64
#define HC_INT_EPS                  0.0000001

//=====================================================================//
// State
//=====================================================================//

/**
 * Состояние клавиш для детектора “нажатия по фронту”.
 *
 * @details
 * Храним предыдущее состояние для Ctrl и для клавиш категорий (1/2/3).
 * Это позволяет не выполнять операцию повторно при удержании клавиш.
 */
struct HotkeyCalc {
	bool ctrlDown = false;
	bool key1Down = false;
	bool key2Down = false;
	bool key3Down = false;
};

/**
 * Сбрасывает состояние HotkeyCalc.
 *
 * @param h Структура состояния.
 */
static void HotkeyCalc_Init(HotkeyCalc &h) {
	memset(&h, 0, sizeof(h));
}

/**
 * Запускает таймер опроса горячих клавиш.
 *
 * @param hwnd Окно, которое будет получать WM_TIMER.
 */
static void HotkeyCalc_Start(HWND hwnd) {
	SetTimer(hwnd, HC_TIMER_ID, HC_POLL_MS, nullptr);
}

/**
 * Останавливает таймер опроса горячих клавиш.
 *
 * @param hwnd Окно, в котором был запущен таймер.
 */
static void HotkeyCalc_Stop(HWND hwnd) {
	KillTimer(hwnd, HC_TIMER_ID);
}

//=====================================================================//
// Input helpers
//=====================================================================//

/**
 * Проверяет, является ли клавиша “extended” (нужно KEYEVENTF_EXTENDEDKEY).
 *
 * @param vk Виртуальный код клавиши.
 * @return true если extended, иначе false.
 */
static bool HotkeyCalc_IsExtendedKey(const WORD vk)
{
	switch (vk)
	{
		case VK_RCONTROL:
		case VK_RMENU:
		case VK_INSERT:
		case VK_DELETE:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
		case VK_DIVIDE:
		case VK_NUMLOCK:
			return true;

		default: /* nothing */;
	}

	return false;
}

/**
 * Заполняет INPUT для клавиатуры.
 *
 * @param out  (out) структура INPUT.
 * @param vk   виртуальный код.
 * @param down true = down, false = up.
 */
static void HotkeyCalc_FillKeyInput(INPUT &out, const WORD vk, const bool down)
{
	out = {};
	out.type = INPUT_KEYBOARD;
	out.ki.wVk = vk;

	DWORD flags = 0;
	if (!down) {
		flags |= KEYEVENTF_KEYUP;
	}

	if (HotkeyCalc_IsExtendedKey(vk)) {
		flags |= KEYEVENTF_EXTENDEDKEY;
	}

	out.ki.dwFlags = flags;
}

/**
 * Принудительно “отпускает” модификаторы и хоткей-клавиши.
 *
 * @details
 * Основная проблема: триггер Ctrl+1/2/3 часто ещё физически зажат,
 * из-за чего следующий Alt+Enter превращается в Ctrl+Alt+Enter и
 * приложение не всегда попадает в режим/фокус, где Ctrl+A работает.
 * Поэтому перед цепочкой делаем best-effort KEYUP для модификаторов
 * и клавиш '1','2','3'.
 */
static void HotkeyCalc_ReleaseModifiersAndHotkeys()
{
	INPUT seq[16] = {};
	int n = 0;

	HotkeyCalc_FillKeyInput(seq[n++], VK_LCONTROL, false);
	HotkeyCalc_FillKeyInput(seq[n++], VK_RCONTROL, false);

	HotkeyCalc_FillKeyInput(seq[n++], VK_LSHIFT, false);
	HotkeyCalc_FillKeyInput(seq[n++], VK_RSHIFT, false);

	HotkeyCalc_FillKeyInput(seq[n++], VK_LMENU, false);
	HotkeyCalc_FillKeyInput(seq[n++], VK_RMENU, false);

	HotkeyCalc_FillKeyInput(seq[n++], VK_LWIN, false);
	HotkeyCalc_FillKeyInput(seq[n++], VK_RWIN, false);

	HotkeyCalc_FillKeyInput(seq[n++], '1', false);
	HotkeyCalc_FillKeyInput(seq[n++], '2', false);
	HotkeyCalc_FillKeyInput(seq[n++], '3', false);

	SendInput(static_cast<UINT>(n), seq, sizeof(INPUT));
}

/**
 * Отправляет нажатие/отпускание клавиши (Ctrl-ветка).
 *
 * @param vk   Виртуальный код клавиши (VK_* или 'A'/'1' и т.д.).
 * @param down true = key down, false = key up.
 */
static void HotkeyCalc_SendCtrlKey(const WORD vk, const bool down) {
	INPUT in = {};
	HotkeyCalc_FillKeyInput(in, vk, down);
	SendInput(1, &in, sizeof(in));
}

/**
 * Выполняет “аккорд” Ctrl+<vk> (нажать Ctrl, нажать/отпустить vk, отпустить Ctrl).
 *
 * @param vk Виртуальный код клавиши (обычно 'A', 'C', 'V').
 */
static void HotkeyCalc_SendChordCtrl(const WORD vk)
{
	INPUT seq[4] = {};
	HotkeyCalc_FillKeyInput(seq[0], VK_LCONTROL, true);

	HotkeyCalc_FillKeyInput(seq[1], vk, true);
	HotkeyCalc_FillKeyInput(seq[2], vk, false);

	HotkeyCalc_FillKeyInput(seq[3], VK_LCONTROL, false);

	SendInput(4, seq, sizeof(INPUT));
}

/**
 * Выполняет “аккорд” Ctrl+Shift+<vk>.
 *
 * @param vk Виртуальный код клавиши (например VK_END).
 */
static void HotkeyCalc_SendChordCtrlShift(const WORD vk)
{
	INPUT seq[6] = {};
	HotkeyCalc_FillKeyInput(seq[0], VK_LCONTROL, true);
	HotkeyCalc_FillKeyInput(seq[1], VK_LSHIFT, true);

	HotkeyCalc_FillKeyInput(seq[2], vk, true);
	HotkeyCalc_FillKeyInput(seq[3], vk, false);

	HotkeyCalc_FillKeyInput(seq[4], VK_LSHIFT, false);
	HotkeyCalc_FillKeyInput(seq[5], VK_LCONTROL, false);

	SendInput(6, seq, sizeof(INPUT));
}

/**
 * Отправляет нажатие/отпускание клавиши Alt (VK_MENU).
 *
 * @param down true = key down, false = key up.
 */
static void HotkeyCalc_SendAltKey(const bool down) {
	INPUT in = {};
	HotkeyCalc_FillKeyInput(in, VK_LMENU, down);
	SendInput(1, &in, sizeof(in));
}

/**
 * Выполняет “аккорд” Alt+<vk> (нажать Alt, нажать/отпустить vk, отпустить Alt).
 *
 * @param vk Виртуальный код клавиши (например VK_RETURN).
 */
static void HotkeyCalc_SendChordAlt(const WORD vk)
{
	INPUT seq[4] = {};
	HotkeyCalc_FillKeyInput(seq[0], VK_LMENU, true);

	HotkeyCalc_FillKeyInput(seq[1], vk, true);
	HotkeyCalc_FillKeyInput(seq[2], vk, false);

	HotkeyCalc_FillKeyInput(seq[3], VK_LMENU, false);

	SendInput(4, seq, sizeof(INPUT));
}

/**
 * “Тап” одной клавиши: key down + key up.
 *
 * @param vk Виртуальный код клавиши (например VK_RIGHT / VK_RETURN).
 */
static void HotkeyCalc_SendKeyTap(const WORD vk)
{
	INPUT seq[2] = {};
	HotkeyCalc_FillKeyInput(seq[0], vk, true);
	HotkeyCalc_FillKeyInput(seq[1], vk, false);
	SendInput(2, seq, sizeof(INPUT));
}

//=====================================================================//
// Clipboard helpers
//=====================================================================//

/**
 * Ждёт изменения sequence number у буфера обмена.
 *
 * @param oldSeq    Предыдущее значение GetClipboardSequenceNumber().
 * @param timeoutMs Таймаут ожидания, мс.
 * @return true если буфер обмена изменился, иначе false (по таймауту).
 */
static bool HotkeyCalc_WaitClipboardChange(const DWORD oldSeq, const DWORD timeoutMs)
{
	const DWORD start = GetTickCount();

	while (true) {
		// ReSharper disable once CppTooWideScopeInitStatement
		const DWORD nowSeq = GetClipboardSequenceNumber();
		if (nowSeq != oldSeq) return true;

		if (GetTickCount() - start >= timeoutMs) return false;

		Sleep(HC_CLIPBOARD_POLL_SLEEP_MS);
	}
}

/**
 * Читает текст из буфера обмена как CF_UNICODETEXT.
 *
 * @param out (out) Прочитанная строка. Очищается в начале.
 * @return true при успехе, иначе false.
 */
static bool HotkeyCalc_ReadClipboardText(std::wstring &out)
{
	out.clear();

	if (!OpenClipboard(nullptr)) return false;

	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (!h) {
		CloseClipboard();
		return false;
	}

	const wchar_t *p = static_cast<const wchar_t *>(GlobalLock(h));
	if (!p) {
		CloseClipboard();
		return false;
	}

	out = p;

	GlobalUnlock(h);
	CloseClipboard();
	return true;
}

/**
 * Записывает текст в буфер обмена как CF_UNICODETEXT.
 *
 * @param text Текст для записи.
 * @return true при успехе, иначе false.
 */
static bool HotkeyCalc_WriteClipboardText(const std::wstring &text)
{
	if (!OpenClipboard(nullptr)) return false;

	EmptyClipboard();

	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	const HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!hMem) {
		CloseClipboard();
		return false;
	}

	void *dst = GlobalLock(hMem);
	if (!dst) {
		GlobalFree(hMem);
		CloseClipboard();
		return false;
	}

	memcpy(dst, text.c_str(), bytes);
	GlobalUnlock(hMem);

	if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
		GlobalFree(hMem);
		CloseClipboard();
		return false;
	}

	// После SetClipboardData владельцем памяти становится система
	CloseClipboard();
	return true;
}

/**
 * Строит уникальный маркер, чтобы отличать “наше ожидание Ctrl+C” от ситуации,
 * когда буфер уже содержит то же значение и приложение не обновляет clipboard.
 *
 * @return Уникальная строка-маркер.
 */
static std::wstring HotkeyCalc_MakeClipboardMarker()
{
	std::wstring m = L"__PriceCalcMarker__";
	m += L"_";
	m += std::to_wstring(GetCurrentProcessId());
	m += L"_";
	m += std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
	return m;
}

/**
 * Best-effort восстановление текстового буфера обмена (если мы успели его изменить).
 *
 * @param oldText Предыдущее текстовое содержимое буфера обмена.
 */
static void HotkeyCalc_TryRestoreClipboardText(const std::wstring &oldText)
{
	// Если не получится — ничего критичного, но стараемся не оставлять маркер в буфере.
	(void)HotkeyCalc_WriteClipboardText(oldText);
}

//=====================================================================//
// Parse / math helpers
//=====================================================================//

/**
 * Пытается распарсить число (double) из строки.
 *
 * @details
 * Использует wcstod(). Допускает хвостовые пробелы/переводы строк.
 *
 * @param s Входная строка.
 * @param v (out) Результат.
 * @return true если число распознано полностью (с учётом хвостовых пробелов), иначе false.
 */
static bool HotkeyCalc_TryParseDoubleText(const std::wstring &s, double &v)
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
 * Нормализует текст, вытаскивая “похожее на число”.
 *
 * @details
 * Разрешаем:
 * - цифры;
 * - один ведущий '-';
 * - один десятичный разделитель '.' или ',' (приводим к '.').
 *
 * @param in Исходный текст.
 * @return Нормализованная строка, пригодная для wcstod().
 */
static std::wstring HotkeyCalc_NormalizeNumberText(const std::wstring &in)
{
	// Достаём "похожее на число":
	// - цифры
	// - один ведущий '-'
	// - один десятичный разделитель '.' или ',' (приводим к '.')
	std::wstring out;
	out.reserve(in.size());

	bool hasSign = false;
	bool hasSep  = false;

	for (const wchar_t c : in)
	{
		if (!hasSign && out.empty() && c == L'-') {
			out.push_back(c);
			hasSign = true;
			continue;
		}

		if (std::iswdigit(c)) {
			out.push_back(c);
			continue;
		}

		if (!hasSep && (c == L'.' || c == L',')) {
			out.push_back(L'.');
			hasSep = true;
        }
	}

	return out;
}

/**
 * Округляет значение вверх до ближайших 10.
 *
 * @param v Входное значение.
 * @return Округлённое вверх значение (ceil(v/10)*10).
 */
static double HotkeyCalc_RoundUpTo10(const double v) {
	// Округление до 10 в большую сторону
	return std::ceil(v / 10.0) * 10.0;
}

/**
 * Форматирует число как целое (для вывода после округления до 10).
 *
 * @details
 * Добавляет маленький epsilon, чтобы компенсировать погрешности double перед static_cast<long long>.
 *
 * @param v Входное значение.
 * @return Строка с целым числом.
 */
static std::wstring HotkeyCalc_FormatAsInt(const double v)
{
	// После округления до 10 обычно хотим целое
	const double vv = v + HC_INT_EPS;
	const long long iv = static_cast<long long>(vv);

	wchar_t buf[HC_INTBUF_CHARS] = {};
	swprintf_s(buf, L"%lld", iv);
	return buf;
}

/**
 * Форматирует число для сообщения у трея (коэффициент/закупка).
 *
 * @param v Число.
 * @return Строка (%.10g).
 */
static std::wstring HotkeyCalc_FormatForBalloon(const double v)
{
	wchar_t buf[64] = {};
	swprintf_s(buf, L"%.10g", v);
	return buf;
}

/**
 * Показывает сообщение об успешном применении категории.
 *
 * @details
 * Первая строка: "Категория #N - coeff".
 *
 * @param tray     Трей (для balloon).
 * @param category Номер категории (1..3).
 * @param coeff    Коэффициент категории.
 * @param buy      Закупочная цена (значение, которое было прочитано).
 * @param sellText Цена продажи (значение, которое было записано/вставлено).
 */
static void HotkeyCalc_ShowSuccessBalloon(TrayIcon &tray, const int category, const double coeff, const double buy, const std::wstring &sellText)
{
	const std::wstring coeffText = HotkeyCalc_FormatForBalloon(coeff);
	const std::wstring buyText = HotkeyCalc_FormatForBalloon(buy);

	std::wstring msg;
	msg.reserve(160);

	msg += L"Категория #";
	msg += std::to_wstring(category);
	msg += L" - ";
	msg += coeffText;

	msg += L"\r\nЗакупка: ";
	msg += buyText;

	msg += L"\r\nПродажа: ";
	msg += sellText;

	TrayIcon_ShowBalloon(tray, L"PriceCalc", msg.c_str());
}

//=====================================================================//
// Workflow
//=====================================================================//

/**
 * Выполняет операцию “взять число из ячейки/текста, умножить, округлить, вставить дальше”.
 *
 * @details
 * Последовательность:
 * 1) Alt+Enter
 * 2) Ctrl+A
 * 3) (опционально) выставляем маркер в clipboard, чтобы Ctrl+C точно “менял буфер”
 * 4) Ctrl+C
 * 5) Ждём изменения буфера обмена
 * 6) Читаем CF_UNICODETEXT
 * 7) Нормализуем и парсим число
 * 8) result = RoundUpTo10(value * coeff)
 * 9) Записываем результат в буфер обмена
 * 10) Right
 * 11) Ctrl+V
 * 12) Enter
 * 13) Balloon: категория + coeff, закупка, продажа
 *
 * @param tray     Трей (для вывода сообщений).
 * @param category Номер категории (1..3).
 * @param coeff    Коэффициент умножения.
 * @return true если вся цепочка выполнена успешно, иначе false.
 */
static bool HotkeyCalc_DoClipboardTransform(TrayIcon &tray, const int category, const double coeff)
{
	// Важно: “снимаем” Ctrl/модификаторы от хоткея, чтобы Alt+Enter не превращался в Ctrl+Alt+Enter
	HotkeyCalc_ReleaseModifiersAndHotkeys();
	Sleep(HC_SLEEP_AFTER_RELEASE_MS);

	// 1) Alt+Enter
	HotkeyCalc_SendChordAlt(VK_RETURN);
	Sleep(HC_SLEEP_AFTER_ALTENTER_MS);

	// 2) Ctrl+A (выделить в ячейке)
	HotkeyCalc_SendChordCtrl('A');
	Sleep(HC_SLEEP_AFTER_SELECT_MS);

	// 3) Подготовка: если в буфере есть текст — ставим уникальный маркер, чтобы Ctrl+C гарантированно изменил буфер
	std::wstring oldClipboardText;
	const bool oldClipboardTextOk = HotkeyCalc_ReadClipboardText(oldClipboardText);

	std::wstring marker;
	bool markerUsed = false;

	if (oldClipboardTextOk)
	{
		marker = HotkeyCalc_MakeClipboardMarker();

		if (!HotkeyCalc_WriteClipboardText(marker)) {
			// Не смогли подготовить маркер — лучше не рисковать “вставкой не того”.
			TrayIcon_ShowBalloon(tray, L"PriceCalc", L"Не удалось получить доступ к буферу обмена (занят). Повторите.");
			return false;
		}

		markerUsed = true;
	}

	// 4) Ctrl+C (с 1 повтором: иногда Ctrl+A/фокус “глючит”)
	bool copied = false;

	for (int attempt = 0; attempt < 2; attempt++)
	{
		const DWORD seq = GetClipboardSequenceNumber();
		HotkeyCalc_SendChordCtrl('C');

		if (HotkeyCalc_WaitClipboardChange(seq, HC_CLIPBOARD_TIMEOUT_MS)) {
			copied = true;
			break;
		}

		if (attempt == 0)
		{
			// Повторим выделение и копирование: иногда первая попытка не попадает в нужное поле
			HotkeyCalc_SendChordCtrl('A');
			Sleep(HC_SLEEP_AFTER_SELECT_MS);

			// Fallback: иногда Ctrl+A “теряется” в конкретном контроле — добиваем выделение через Ctrl+Home / Ctrl+Shift+End
			HotkeyCalc_SendChordCtrl(VK_HOME);
			HotkeyCalc_SendChordCtrlShift(VK_END);
			Sleep(HC_SLEEP_AFTER_SELECT_MS);

			if (markerUsed) {
				// Вернём маркер (если его кто-то успел изменить), чтобы вторая попытка тоже была надёжной
				(void)HotkeyCalc_WriteClipboardText(marker);
			}

			continue;
		}
	}

	if (!copied)
	{
		if (markerUsed && oldClipboardTextOk) {
			HotkeyCalc_TryRestoreClipboardText(oldClipboardText);
		}

		TrayIcon_ShowBalloon(tray, L"PriceCalc",
			L"Не удалось скопировать значение (Ctrl+C не изменил буфер обмена).\r\n"
			L"Проверьте, что фокус в ячейке и Ctrl+A действительно выделяет значение.");
		return false;
	}

	// 5) Читаем текст
	std::wstring text;
	if (!HotkeyCalc_ReadClipboardText(text))
	{
		if (markerUsed && oldClipboardTextOk) {
			HotkeyCalc_TryRestoreClipboardText(oldClipboardText);
		}

		TrayIcon_ShowBalloon(tray, L"PriceCalc", L"Не удалось прочитать текст из буфера обмена (CF_UNICODETEXT).");
		return false;
	}

	// Если в буфере так и остался маркер — значит копирование не сработало (выделение/фокус/приложение)
	if (markerUsed && text == marker)
	{
		if (oldClipboardTextOk) {
			HotkeyCalc_TryRestoreClipboardText(oldClipboardText);
		}

		TrayIcon_ShowBalloon(tray, L"PriceCalc",
			L"Не удалось скопировать значение (выделение/копирование не сработало).\r\n"
			L"Проверьте фокус ячейки и попробуйте ещё раз.");
		return false;
	}

	// 6) Парсим число
	const std::wstring norm = HotkeyCalc_NormalizeNumberText(text);

	double value = 0.0;
	if (!HotkeyCalc_TryParseDoubleText(norm, value))
	{
		if (markerUsed && oldClipboardTextOk) {
			HotkeyCalc_TryRestoreClipboardText(oldClipboardText);
		}

		TrayIcon_ShowBalloon(tray, L"PriceCalc", L"Не удалось распознать число в буфере обмена.");
		return false;
	}

	// 7) Считаем
	double result = value * coeff;
	result = HotkeyCalc_RoundUpTo10(result);

	// 8) Пишем результат в буфер
	const std::wstring out = HotkeyCalc_FormatAsInt(result);

	if (!HotkeyCalc_WriteClipboardText(out))
	{
		if (markerUsed && oldClipboardTextOk) {
			HotkeyCalc_TryRestoreClipboardText(oldClipboardText);
		}

		TrayIcon_ShowBalloon(tray, L"PriceCalc", L"Не удалось записать результат в буфер обмена.");
		return false;
	}

	// 9) Right
	HotkeyCalc_SendKeyTap(VK_RIGHT);
	Sleep(HC_SLEEP_AFTER_MOVE_MS);

	// 10) Ctrl+V
	HotkeyCalc_SendChordCtrl('V');
	Sleep(HC_SLEEP_AFTER_WRITE_MS);

	// 11) Enter
	HotkeyCalc_SendKeyTap(VK_RETURN);

	// 12) Успех: показываем, что применили
	HotkeyCalc_ShowSuccessBalloon(tray, category, coeff, value, out);

	return true;
}

/**
 * Опрос комбинаций Ctrl+1/2/3 и запуск действия “по фронту”.
 *
 * @param h      Состояние (предыдущее состояние клавиш).
 * @param tray   Трей (для сообщений).
 * @param coeffs Массив из 3 коэффициентов (для категорий 1/2/3).
 */
static void HotkeyCalc_Poll(HotkeyCalc &h, TrayIcon &tray, const double coeffs[3])
{
	if (!coeffs) return;

	const bool ctrlDownNow = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	const bool key1DownNow = (GetAsyncKeyState('1') & 0x8000) != 0;
	const bool key2DownNow = (GetAsyncKeyState('2') & 0x8000) != 0;
	const bool key3DownNow = (GetAsyncKeyState('3') & 0x8000) != 0;

	if (ctrlDownNow && key1DownNow && !h.key1Down) {
		HotkeyCalc_DoClipboardTransform(tray, 1, coeffs[0]);
	}

	if (ctrlDownNow && key2DownNow && !h.key2Down) {
		HotkeyCalc_DoClipboardTransform(tray, 2, coeffs[1]);
	}

	if (ctrlDownNow && key3DownNow && !h.key3Down) {
		HotkeyCalc_DoClipboardTransform(tray, 3, coeffs[2]);
	}

	h.ctrlDown = ctrlDownNow;
	h.key1Down = key1DownNow;
	h.key2Down = key2DownNow;
	h.key3Down = key3DownNow;
}

/**
 * Обработчик WM_TIMER для HotkeyCalc.
 *
 * @param h       Состояние HotkeyCalc.
 * @param tray    Трей (для сообщений).
 * @param timerId Идентификатор таймера (для совместимости сигнатуры; не используется).
 * @param coeffs  Массив коэффициентов (3 значения).
 */
static void HotkeyCalc_OnTimer(HotkeyCalc &h, TrayIcon &tray, WPARAM timerId, const double coeffs[3])
{
	// Таймер в приложении один, timerId оставлен для совместимости сигнатуры.
	UNREFERENCED_PARAMETER(timerId);

	HotkeyCalc_Poll(h, tray, coeffs);
}
