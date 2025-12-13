// HotkeyCalc.h
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppDFAUnreachableFunctionCall

#pragma once

/**
 * HotkeyCalc
 * - Категории через GetAsyncKeyState (Ctrl+1 / Ctrl+2 / Ctrl+3)
 * - По команде:
 *   1) Alt+Enter
 *   2) Ctrl+A
 *   3) Ctrl+C
 *   4) clipboard * coeff -> clipboard
 *   5) Right
 *   6) Ctrl+V
 *   7) Enter
 * - Header-only (в стиле проекта)
 *
 * ЗАМЕТКИ:
 * 1) Тут нет RegisterHotKey: комбинации отслеживаются опросом (таймером) через GetAsyncKeyState.
 * 2) Чтобы не было повторного срабатывания при удержании клавиш, ловим “нажатие” по фронту (down && !prevDown).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <cwctype>
#include <cwchar>
#include <cmath>
#include <cstring>

#include "TrayIcon.h"

#define HOTKEYCALC_TIMER_ID                 1
#define HOTKEYCALC_POLL_MS                  25

#define HOTKEYCALC_SLEEP_AFTER_ALTENTER_MS  30
#define HOTKEYCALC_SLEEP_AFTER_SELECT_MS    30
#define HOTKEYCALC_SLEEP_AFTER_MOVE_MS      10
#define HOTKEYCALC_SLEEP_AFTER_WRITE_MS     10

#define HOTKEYCALC_CLIPBOARD_TIMEOUT_MS     800
#define HOTKEYCALC_CLIPBOARD_POLL_SLEEP_MS  10

#define HOTKEYCALC_INTBUF_CHARS             64
#define HOTKEYCALC_INT_EPS                  0.0000001

//=====================================================================//
// State
//=====================================================================//

struct HotkeyCalc {
	bool ctrlDown = false;
	bool key1Down = false;
	bool key2Down = false;
	bool key3Down = false;
};

static void HotkeyCalc_Init(HotkeyCalc &h) {
	memset(&h, 0, sizeof(h));
}

static void HotkeyCalc_Start(HWND hwnd) {
	SetTimer(hwnd, HOTKEYCALC_TIMER_ID, HOTKEYCALC_POLL_MS, nullptr);
}

static void HotkeyCalc_Stop(HWND hwnd) {
	KillTimer(hwnd, HOTKEYCALC_TIMER_ID);
}

//=====================================================================//
// Input helpers
//=====================================================================//

static void HotkeyCalc_SendCtrlKey(const WORD vk, const bool down) {
	INPUT in = {};
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	SendInput(1, &in, sizeof(in));
}

static void HotkeyCalc_SendChordCtrl(const WORD vk)
{
	HotkeyCalc_SendCtrlKey(VK_CONTROL, true);

	INPUT keyDown = {};
	keyDown.type = INPUT_KEYBOARD;
	keyDown.ki.wVk = vk;

	INPUT keyUp = keyDown;
	keyUp.ki.dwFlags = KEYEVENTF_KEYUP;

	INPUT seq[2] = { keyDown, keyUp };
	SendInput(2, seq, sizeof(INPUT));

	HotkeyCalc_SendCtrlKey(VK_CONTROL, false);
}

static void HotkeyCalc_SendAltKey(const bool down) {
	INPUT in = {};
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = VK_MENU;
	in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	SendInput(1, &in, sizeof(in));
}

static void HotkeyCalc_SendChordAlt(const WORD vk)
{
	HotkeyCalc_SendAltKey(true);

	INPUT keyDown = {};
	keyDown.type = INPUT_KEYBOARD;
	keyDown.ki.wVk = vk;

	INPUT keyUp = keyDown;
	keyUp.ki.dwFlags = KEYEVENTF_KEYUP;

	INPUT seq[2] = { keyDown, keyUp };
	SendInput(2, seq, sizeof(INPUT));

	HotkeyCalc_SendAltKey(false);
}

static void HotkeyCalc_SendKeyTap(const WORD vk)
{
	INPUT keyDown = {};
	keyDown.type = INPUT_KEYBOARD;
	keyDown.ki.wVk = vk;

	INPUT keyUp = keyDown;
	keyUp.ki.dwFlags = KEYEVENTF_KEYUP;

	INPUT seq[2] = { keyDown, keyUp };
	SendInput(2, seq, sizeof(INPUT));
}

//=====================================================================//
// Clipboard helpers
//=====================================================================//

static bool HotkeyCalc_WaitClipboardChange(const DWORD oldSeq, const DWORD timeoutMs)
{
	const DWORD start = GetTickCount();

	while (true) {
		// ReSharper disable once CppTooWideScopeInitStatement
		const DWORD nowSeq = GetClipboardSequenceNumber();
		if (nowSeq != oldSeq) return true;

		if (GetTickCount() - start >= timeoutMs) return false;

		Sleep(HOTKEYCALC_CLIPBOARD_POLL_SLEEP_MS);
	}
}

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

//=====================================================================//
// Parse / math helpers
//=====================================================================//

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
			continue;
		}
	}

	return out;
}

static double HotkeyCalc_RoundUpTo10(const double v) {
	// Округление до 10 в большую сторону
	return std::ceil(v / 10.0) * 10.0;
}

static std::wstring HotkeyCalc_FormatAsInt(const double v)
{
	// После округления до 10 обычно хотим целое
	const double vv = v + HOTKEYCALC_INT_EPS;
	const long long iv = static_cast<long long>(vv);

	wchar_t buf[HOTKEYCALC_INTBUF_CHARS] = {};
	swprintf_s(buf, L"%lld", iv);
	return buf;
}

//=====================================================================//
// Workflow
//=====================================================================//

static bool HotkeyCalc_DoClipboardTransform(TrayIcon &tray, const double coeff)
{
	// 1) Alt+Enter
	HotkeyCalc_SendChordAlt(VK_RETURN);
	Sleep(HOTKEYCALC_SLEEP_AFTER_ALTENTER_MS);

	// 2) Ctrl+A (выделить в ячейке)
	HotkeyCalc_SendChordCtrl('A');
	Sleep(HOTKEYCALC_SLEEP_AFTER_SELECT_MS);

	// 3) Ctrl+C
	const DWORD seq = GetClipboardSequenceNumber();
	HotkeyCalc_SendChordCtrl('C');

	// 4) Ждём изменения буфера
	if (!HotkeyCalc_WaitClipboardChange(seq, HOTKEYCALC_CLIPBOARD_TIMEOUT_MS)) {
		TrayIcon_ShowBalloon(tray, L"TrayHotkeyCalc", L"Буфер обмена не изменился после Ctrl+C.");
		return false;
	}

	// 5) Читаем текст
	std::wstring text;
	if (!HotkeyCalc_ReadClipboardText(text)) {
		TrayIcon_ShowBalloon(tray, L"TrayHotkeyCalc", L"Не удалось прочитать текст из буфера обмена (CF_UNICODETEXT).");
		return false;
	}

	// 6) Парсим число
	const std::wstring norm = HotkeyCalc_NormalizeNumberText(text);

	double value = 0.0;
	if (!HotkeyCalc_TryParseDoubleText(norm, value)) {
		TrayIcon_ShowBalloon(tray, L"TrayHotkeyCalc", L"Не удалось распознать число в буфере обмена.");
		return false;
	}

	// 7) Считаем
	double result = value * coeff;
	result = HotkeyCalc_RoundUpTo10(result);

	// 8) Пишем результат в буфер
	const std::wstring out = HotkeyCalc_FormatAsInt(result);

	if (!HotkeyCalc_WriteClipboardText(out)) {
		TrayIcon_ShowBalloon(tray, L"TrayHotkeyCalc", L"Не удалось записать результат в буфер обмена.");
		return false;
	}

	// 9) Right
	HotkeyCalc_SendKeyTap(VK_RIGHT);
	Sleep(HOTKEYCALC_SLEEP_AFTER_MOVE_MS);

	// 10) Ctrl+V
	HotkeyCalc_SendChordCtrl('V');
	Sleep(HOTKEYCALC_SLEEP_AFTER_WRITE_MS);

	// 11) Enter
	HotkeyCalc_SendKeyTap(VK_RETURN);

	return true;
}

static void HotkeyCalc_Poll(HotkeyCalc &h, TrayIcon &tray, const double coeffs[3])
{
	if (!coeffs) return;

	const bool ctrlDownNow = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	const bool key1DownNow = (GetAsyncKeyState('1') & 0x8000) != 0;
	const bool key2DownNow = (GetAsyncKeyState('2') & 0x8000) != 0;
	const bool key3DownNow = (GetAsyncKeyState('3') & 0x8000) != 0;

	if (ctrlDownNow && key1DownNow && !h.key1Down) {
		HotkeyCalc_DoClipboardTransform(tray, coeffs[0]);
	}

	if (ctrlDownNow && key2DownNow && !h.key2Down) {
		HotkeyCalc_DoClipboardTransform(tray, coeffs[1]);
	}

	if (ctrlDownNow && key3DownNow && !h.key3Down) {
		HotkeyCalc_DoClipboardTransform(tray, coeffs[2]);
	}

	h.ctrlDown = ctrlDownNow;
	h.key1Down = key1DownNow;
	h.key2Down = key2DownNow;
	h.key3Down = key3DownNow;
}

static bool HotkeyCalc_OnTimer(HotkeyCalc &h, TrayIcon &tray, WPARAM timerId, const double coeffs[3])
{
	// Таймер в приложении один, timerId оставлен для совместимости сигнатуры.
	UNREFERENCED_PARAMETER(timerId);

	HotkeyCalc_Poll(h, tray, coeffs);
	return true;
}
