#pragma once

/**
 * @file Autorun.h
 * Автозапуск через реестр Windows (HKLM\...\Run).
 *
 * @details
 * Пытаемся включить/отключить “по-тихому” (если процесс уже elevated).
 * Если прав не хватает — создаём временный .reg в %TEMP% и импортируем его через regedit.exe с UAC (runas).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <cwchar>

#define AUTORUN_RUN_KEY_W  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_TEMP_ENABLE_W  L"TrayHotkeyCalc-autorun-enable.reg"
#define AUTORUN_TEMP_DISABLE_W L"TrayHotkeyCalc-autorun-disable.reg"

static std::wstring Autorun_GetExePath()
{
	wchar_t buf[4096] = {};
	const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(_countof(buf)));
	if (n == 0 || n >= _countof(buf)) {
		return L"";
	}

	return std::wstring(buf);
}

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

static bool Autorun_WriteUtf16File(const std::wstring &path, const std::wstring &content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	// UTF-16 LE BOM
	const WORD bom = 0xFEFF;
	DWORD wrote = 0;

	if (!WriteFile(h, &bom, sizeof(bom), &wrote, nullptr)) {
		CloseHandle(h);
		return false;
	}

	const DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t));
	if (!WriteFile(h, content.c_str(), bytes, &wrote, nullptr)) {
		CloseHandle(h);
		return false;
	}

	CloseHandle(h);
	return true;
}

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

	if (!ShellExecuteExW(&sei))
	{
		exitCode = static_cast<DWORD>(GetLastError());
		return false;
	}

	if (!sei.hProcess)
	{
		// Запустилось, но хэндла нет — считаем “ок”
		return true;
	}

	WaitForSingleObject(sei.hProcess, INFINITE);

	DWORD code = 0;
	if (GetExitCodeProcess(sei.hProcess, &code)) {
		exitCode = code;
	}

	CloseHandle(sei.hProcess);
	return (exitCode == 0);
}

static bool Autorun_IsEnabledHKLM(const wchar_t *valueName)
{
	if (!valueName || !*valueName) return false;

	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AUTORUN_RUN_KEY_W, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
		return false;
	}

	DWORD type = 0;
	wchar_t buf[4096] = {};
	DWORD cb = sizeof(buf);

	const LONG r = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE *>(buf), &cb);
	RegCloseKey(hKey);

	if (r != ERROR_SUCCESS) return false;
	if (type != REG_SZ && type != REG_EXPAND_SZ) return false;

	return true;
}

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
			static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));

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
		if (!Autorun_RunRegeditElevated(regPath, exitCode))
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
		if (!Autorun_RunRegeditElevated(regPath, exitCode))
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
