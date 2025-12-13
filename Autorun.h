// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppLocalVariableMayBeConst

#pragma once

/**
 * @file Autorun.h
 * Автозапуск через реестр Windows (HKLM\...\Run).
 *
 * @details
 * Пытаемся включить/отключить “по-тихому” (если процесс уже elevated).
 * Если прав не хватает — создаём временный .reg в %TEMP% и импортируем его через regedit.exe с UAC (runas).
 *
 * Дополнительно:
 * - После операции можно (и нужно) подтвердить результат чтением реестра, чтобы сообщить пользователю “успешно”.
 *   Для этого есть функции Autorun_TryEnableHKLM_WithStatus() / Autorun_TryDisableHKLM_WithStatus().
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <cwchar>

#include "HotkeyCalc.h"

#define AUTORUN_RUN_KEY_W          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_TEMP_ENABLE_W      L"TrayHotkeyCalc-autorun-enable.reg"
#define AUTORUN_TEMP_DISABLE_W     L"TrayHotkeyCalc-autorun-disable.reg"

/**
 * Возвращает полный путь к текущему exe (GetModuleFileNameW(nullptr, ...)).
 * @return Путь к exe или пустая строка при ошибке.
 */
static std::wstring Autorun_GetExePath()
{
	wchar_t buf[4096] = {};
	const DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
	if (n == 0 || n >= _countof(buf)) {
		return L"";
	}

	return std::wstring(buf);
}

/**
 * Экранирует строку для использования внутри .reg файла:
 * - '\' -> '\\'
 * - '"' -> '\"'
 * @param s Входная строка.
 * @return Экранированная строка.
 */
static std::wstring Autorun_EscapeRegString(const std::wstring &s)
{
	std::wstring out;
	out.reserve(s.size() * 2);

	for (wchar_t c : s)
	{
		if (c == L'\\') {
			out.push_back(L'\\');
			out.push_back(L'\\');
			continue;
		}

		if (c == L'"') {
			out.push_back(L'\\');
			out.push_back(L'"');
			continue;
		}

		out.push_back(c);
	}

	return out;
}

/**
 * Записывает файл в UTF-16 LE с BOM (0xFEFF).
 * @param path Полный путь к файлу.
 * @param content Содержимое (wide string).
 * @return true если файл записан, иначе false.
 */
static bool Autorun_WriteUtf16File(const std::wstring &path, const std::wstring &content)
{
	HANDLE h = CreateFileW(
		path.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (h == INVALID_HANDLE_VALUE) return false;

	// UTF-16 LE BOM
    constexpr WORD bom = 0xFEFF;
	DWORD wrote = 0;

	if (!WriteFile(h, &bom, sizeof(bom), &wrote, nullptr)) {
		CloseHandle(h);
		return false;
	}

	const DWORD bytes = content.size() * sizeof(wchar_t);
	if (!WriteFile(h, content.c_str(), bytes, &wrote, nullptr)) {
		CloseHandle(h);
		return false;
	}

	CloseHandle(h);
	return true;
}

/**
 * Строит путь во временную папку (%TEMP%).
 * @param fileName Имя файла (без пути).
 * @return Полный путь.
 */
static std::wstring Autorun_BuildTempPath(const wchar_t *fileName)
{
	wchar_t dir[MAX_PATH] = {};
	const DWORD n = GetTempPathW(_countof(dir), dir);
	if (n == 0 || n >= _countof(dir)) {
		return std::wstring(fileName);
	}

	std::wstring out = dir;
	out += fileName;
	return out;
}

/**
 * Запускает regedit.exe /s "<regFile>" с повышением прав (runas) и ждёт завершения.
 * @param regFile Путь к .reg файлу.
 * @param exitCode [out] Код завершения процесса (или GetLastError() если ShellExecuteExW не запустился).
 * @return true если процесс стартовал и завершился с exitCode==0 (или если процесс стартовал без hProcess).
 */
static bool Autorun_RunRegeditElevated(const std::wstring &regFile, DWORD &exitCode)
{
	exitCode = 0;

	std::wstring params = L"/s \"";
	params += regFile;
	params += L"\"";

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = L"regedit.exe";
	sei.lpParameters = params.c_str();
	sei.nShow = SW_HIDE;

	if (!ShellExecuteExW(&sei)) {
		exitCode = GetLastError();
		return false;
	}

	// Запустилось, но хэндла нет — считаем “ок”
	if (!sei.hProcess) return true;

	WaitForSingleObject(sei.hProcess, INFINITE);

	DWORD code = 0;
	if (GetExitCodeProcess(sei.hProcess, &code)) {
		exitCode = code;
	}

	CloseHandle(sei.hProcess);
	return exitCode == 0;
}

/**
 * Проверяет наличие параметра автозапуска в HKLM\...\Run.
 * @param valueName Имя значения (например, "TrayHotkeyCalc").
 * @return true если значение существует и оно REG_SZ/REG_EXPAND_SZ.
 */
static bool Autorun_IsEnabledHKLM(const wchar_t *valueName)
{
	if (!valueName || !*valueName) return false;

	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AUTORUN_RUN_KEY_W,
		0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
		return false;
	}

	DWORD type = 0;
	wchar_t buf[4096] = {};
	DWORD cb = sizeof(buf);

	const LONG r = RegQueryValueExW(hKey, valueName, nullptr,
		&type, reinterpret_cast<BYTE *>(buf), &cb);
	RegCloseKey(hKey);

	if (r != ERROR_SUCCESS) return false;
	if (type != REG_SZ && type != REG_EXPAND_SZ) return false;

	return true;
}

/**
 * Включает автозапуск в HKLM\...\Run.
 *
 * @details
 * 1) Пробуем записать ключ напрямую (если уже elevated).
 * 2) Если ERROR_ACCESS_DENIED — создаём .reg во временной папке и импортируем через regedit.exe (runas).
 *
 * @param valueName Имя значения (например, "TrayHotkeyCalc").
 * @param err [out] Текст ошибки (если false).
 * @return true если операция выполнена, иначе false.
 */
static bool Autorun_EnableHKLM(const wchar_t *valueName, std::wstring &err)
{
	err.clear();

	if (!valueName || !*valueName) {
		err = L"Некорректное имя параметра автозапуска.";
		return false;
	}

	const std::wstring exePath = Autorun_GetExePath();
	if (exePath.empty()) {
		err = L"Не удалось получить путь к exe.";
		return false;
	}

	const std::wstring cmd = L"\"" + exePath + L"\"";

	// 1) Попытка без UAC (если уже elevated)
	HKEY hKey = nullptr;
	LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, AUTORUN_RUN_KEY_W, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
	if (r == ERROR_SUCCESS)
	{
		r = RegSetValueExW(hKey, valueName, 0, REG_SZ,
			reinterpret_cast<const BYTE *>(cmd.c_str()),
			(cmd.size() + 1) * sizeof(wchar_t));

		RegCloseKey(hKey);

		if (r == ERROR_SUCCESS) {
			return true;
		}
	}

	// 2) Если не хватило прав — делаем через regedit.exe (runas) по клику
	if (r == ERROR_ACCESS_DENIED)
	{
		const std::wstring regPath = Autorun_BuildTempPath(AUTORUN_TEMP_ENABLE_W);

		const std::wstring escPath = Autorun_EscapeRegString(exePath);

		std::wstring reg;
		reg += L"Windows Registry Editor Version 5.00\r\n\r\n";
		reg += L"[HKEY_LOCAL_MACHINE\\";
		reg += AUTORUN_RUN_KEY_W;
		reg += L"]\r\n\"";
		reg += valueName;
		reg += L"\"=\"\\\"";
		reg += escPath;
		reg += L"\\\"\"\r\n";

		if (!Autorun_WriteUtf16File(regPath, reg)) {
			err = L"Не удалось создать .reg файл во временной папке.";
			return false;
		}

		DWORD exitCode = 0;
		const bool ok = Autorun_RunRegeditElevated(regPath, exitCode);

		// Пытаемся прибраться (даже если не вышло)
		DeleteFileW(regPath.c_str());

		if (!ok)
		{
			if (exitCode == ERROR_CANCELLED) {
				err = L"Отменено пользователем (UAC).";
				return false;
			}

			err = L"Не удалось включить автозапуск (regedit).";
			return false;
		}

		return true;
	}

	err = L"Не удалось включить автозапуск (ошибка реестра).";
	return false;
}

/**
 * Отключает автозапуск в HKLM\...\Run.
 *
 * @details
 * 1) Пробуем удалить значение напрямую (если уже elevated).
 * 2) Если ERROR_ACCESS_DENIED — создаём .reg во временной папке и импортируем через regedit.exe (runas).
 *
 * @param valueName Имя значения (например, "TrayHotkeyCalc").
 * @param err [out] Текст ошибки (если false).
 * @return true если операция выполнена, иначе false.
 */
static bool Autorun_DisableHKLM(const wchar_t *valueName, std::wstring &err)
{
	err.clear();

	if (!valueName || !*valueName) {
		err = L"Некорректное имя параметра автозапуска.";
		return false;
	}

	// 1) Попытка без UAC (если уже elevated)
	HKEY hKey = nullptr;
	LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, AUTORUN_RUN_KEY_W, 0, KEY_SET_VALUE, &hKey);
	if (r == ERROR_SUCCESS)
	{
		r = RegDeleteValueW(hKey, valueName);
		RegCloseKey(hKey);

		if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) {
			return true;
		}
	}

	// 2) Если не хватило прав — делаем через regedit.exe (runas) по клику
	if (r == ERROR_ACCESS_DENIED)
	{
		const std::wstring regPath = Autorun_BuildTempPath(AUTORUN_TEMP_DISABLE_W);

		std::wstring reg;
		reg += L"Windows Registry Editor Version 5.00\r\n\r\n";
		reg += L"[HKEY_LOCAL_MACHINE\\";
		reg += AUTORUN_RUN_KEY_W;
		reg += L"]\r\n\"";
		reg += valueName;
		reg += L"\"=-\r\n";

		if (!Autorun_WriteUtf16File(regPath, reg)) {
			err = L"Не удалось создать .reg файл во временной папке.";
			return false;
		}

		DWORD exitCode = 0;
		const bool ok = Autorun_RunRegeditElevated(regPath, exitCode);

		// Пытаемся прибраться (даже если не вышло)
		DeleteFileW(regPath.c_str());

		if (!ok)
		{
			if (exitCode == ERROR_CANCELLED) {
				err = L"Отменено пользователем (UAC).";
				return false;
			}

			err = L"Не удалось отключить автозапуск (regedit).";
			return false;
		}

		return true;
	}

	err = L"Не удалось отключить автозапуск (ошибка реестра).";
	return false;
}

/**
 * Включает автозапуск и формирует “человеческий” статус (для balloon/логов).
 *
 * @details
 * - Если уже включено: вернёт true и status="Автозапуск уже включен."
 * - Если включили: вернёт true и status="Автозапуск включен."
 * - Если операция “вернулась ok”, но реестр не подтвердился: вернёт false и err="..."
 *
 * @param valueName Имя значения (например, "TrayHotkeyCalc").
 * @param status [out] Сообщение успеха (если true).
 * @param err [out] Сообщение ошибки (если false).
 * @return true если включено и подтверждено чтением реестра.
 */
static bool Autorun_TryEnableHKLM_WithStatus(const wchar_t *valueName, std::wstring &status, std::wstring &err)
{
	status.clear();
	err.clear();

	if (Autorun_IsEnabledHKLM(valueName)) {
		status = L"Автозапуск уже включен.";
		return true;
	}

	if (!Autorun_EnableHKLM(valueName, err)) {
		return false;
	}

	if (!Autorun_IsEnabledHKLM(valueName)) {
		err = L"Операция выполнена, но не удалось подтвердить включение автозапуска.";
		return false;
	}

	status = L"Автозапуск включен.";
	return true;
}

/**
 * Отключает автозапуск и формирует “человеческий” статус (для balloon/логов).
 *
 * @details
 * - Если уже отключено: вернёт true и status="Автозапуск уже отключен."
 * - Если отключили: вернёт true и status="Автозапуск отключен."
 * - Если операция “вернулась ok”, но реестр не подтвердился: вернёт false и err="..."
 *
 * @param valueName Имя значения (например, "TrayHotkeyCalc").
 * @param status [out] Сообщение успеха (если true).
 * @param err [out] Сообщение ошибки (если false).
 * @return true если отключено и подтверждено чтением реестра.
 */
static bool Autorun_TryDisableHKLM_WithStatus(const wchar_t *valueName, std::wstring &status, std::wstring &err)
{
	status.clear();
	err.clear();

	if (!Autorun_IsEnabledHKLM(valueName)) {
		status = L"Автозапуск уже отключен.";
		return true;
	}

	if (!Autorun_DisableHKLM(valueName, err)) {
		return false;
	}

	if (Autorun_IsEnabledHKLM(valueName)) {
		err = L"Операция выполнена, но не удалось подтвердить отключение автозапуска.";
		return false;
	}

	status = L"Автозапуск отключен.";
	return true;
}
