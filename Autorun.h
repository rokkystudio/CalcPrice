// Autorun.h
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst
// ReSharper disable CppParameterMayBeConstPtrOrRef
// ReSharper disable CppTooWideScopeInitStatement

#pragma once

/**
 * @file Autorun.h
 * Автозапуск через реестр HKLM\Software\Microsoft\Windows\CurrentVersion\Run.
 *
 * @details
 * Реализация перенесена из main.cpp (рабочая версия, проверенная через меню трея):
 * - Включение/отключение пишется в HKLM\...\Run.
 * - Если прав не хватает — запускаем текущий exe с UAC (runas) и параметрами:
 *   --autorun-enable / --autorun-disable
 * - В elevated-процессе операция выполняется “по-тихому” и процесс завершается.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <cwchar>

//=====================================================================//
// Autorun (HKLM\Software\Microsoft\Windows\CurrentVersion\Run)
//=====================================================================//

/**
 * Путь к ветке Run (относительно HKLM).
 */
static const wchar_t *kAutorunRegPath   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/**
 * Имя значения (REG_SZ) в Run-ветке.
 */
static const wchar_t *kAutorunValueName = L"PriceCalc";

/**
 * Возвращает полный путь к текущему exe (GetModuleFileNameW(nullptr, ...)).
 *
 * @return Путь к exe или пустая строка при ошибке.
 */
static std::wstring GetExePath()
{
	wchar_t buf[4096] = {};
	const DWORD got = GetModuleFileNameW(nullptr, buf, _countof(buf));
	if (got == 0 || got >= _countof(buf)) {
		return L"";
	}
	return std::wstring(buf);
}

/**
 * Оборачивает путь в двойные кавычки.
 *
 * @param path Путь к файлу.
 * @return Строка вида "C:\...\app.exe".
 */
static std::wstring QuotePath(const std::wstring &path)
{
	std::wstring out;
	out.reserve(path.size() + 2);
	out.push_back(L'"');
	out += path;
	out.push_back(L'"');
	return out;
}

/**
 * Включает автозапуск, записывая значение в HKLM\...\Run.
 *
 * @details
 * Пишется строка REG_SZ: "full_path_to_exe".
 *
 * @return Код ошибки WinAPI (ERROR_SUCCESS при успехе).
 */
static LONG Autorun_Enable_HKLM()
{
	const std::wstring exe = GetExePath();
	if (exe.empty()) return ERROR_FILE_NOT_FOUND;

	const std::wstring data = QuotePath(exe);

	HKEY hKey = nullptr;
	DWORD disp = 0;

	LONG st = RegCreateKeyExW(
		HKEY_LOCAL_MACHINE,
		kAutorunRegPath,
		0,
		nullptr,
		REG_OPTION_NON_VOLATILE,
		KEY_SET_VALUE,
		nullptr,
		&hKey,
		&disp
	);

	if (st != ERROR_SUCCESS) {
		return st;
	}

	st = RegSetValueExW(
		hKey,
		kAutorunValueName,
		0,
		REG_SZ,
		reinterpret_cast<const BYTE *>(data.c_str()),
		static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))
	);

	RegCloseKey(hKey);
	return st;
}

/**
 * Отключает автозапуск, удаляя значение из HKLM\...\Run.
 *
 * @details
 * Если значение отсутствует — возвращает ERROR_SUCCESS.
 *
 * @return Код ошибки WinAPI (ERROR_SUCCESS при успехе).
 */
static LONG Autorun_Disable_HKLM()
{
	HKEY hKey = nullptr;

	LONG st = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE,
		kAutorunRegPath,
		0,
		KEY_SET_VALUE,
		&hKey
	);

	if (st == ERROR_FILE_NOT_FOUND) {
		return ERROR_SUCCESS;
	}

	if (st != ERROR_SUCCESS) {
		return st;
	}

	st = RegDeleteValueW(hKey, kAutorunValueName);
	if (st == ERROR_FILE_NOT_FOUND) {
		st = ERROR_SUCCESS;
	}

	RegCloseKey(hKey);
	return st;
}

/**
 * Запускает текущий exe с повышением прав (UAC) и ждёт завершения.
 *
 * @param enable true => "--autorun-enable", false => "--autorun-disable".
 * @return true если elevated-процесс завершился с exit code 0, иначе false.
 */
static bool Autorun_RunElevatedAndWait(bool enable)
{
	const std::wstring exe = GetExePath();
	if (exe.empty()) return false;

	const wchar_t *params = enable ? L"--autorun-enable" : L"--autorun-disable";

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = nullptr;
	sei.lpVerb = L"runas";
	sei.lpFile = exe.c_str();
	sei.lpParameters = params;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&sei) || !sei.hProcess) {
		return false;
	}

	WaitForSingleObject(sei.hProcess, INFINITE);

	DWORD exitCode = 1;
	GetExitCodeProcess(sei.hProcess, &exitCode);

	CloseHandle(sei.hProcess);
	return exitCode == 0;
}

/**
 * Пытается включить автозапуск:
 * - если прав хватает — пишет HKLM напрямую;
 * - если ERROR_ACCESS_DENIED — запускает elevated-процесс.
 *
 * @return true если автозапуск включён, иначе false.
 */
static bool Autorun_TryEnableWithElevationIfNeeded()
{
	const LONG st = Autorun_Enable_HKLM();
	if (st == ERROR_SUCCESS) return true;

	if (st == ERROR_ACCESS_DENIED) {
		return Autorun_RunElevatedAndWait(true);
	}

	return false;
}

/**
 * Пытается отключить автозапуск:
 * - если прав хватает — удаляет HKLM напрямую;
 * - если ERROR_ACCESS_DENIED — запускает elevated-процесс.
 *
 * @return true если автозапуск отключён, иначе false.
 */
static bool Autorun_TryDisableWithElevationIfNeeded()
{
	const LONG st = Autorun_Disable_HKLM();
	if (st == ERROR_SUCCESS) return true;

	if (st == ERROR_ACCESS_DENIED) {
		return Autorun_RunElevatedAndWait(false);
	}

	return false;
}

/**
 * Обрабатывает параметр командной строки для elevated-запуска.
 *
 * @details
 * Если есть --autorun-enable/--autorun-disable:
 * - выполняет операцию;
 * - возвращает 0/1 (код процесса).
 * Если параметров нет — возвращает -1 (обычный запуск приложения).
 *
 * @return -1 если это обычный запуск, либо 0/1 если это autorun-команда.
 */
static int Autorun_TryHandleCommandLine()
{
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) {
		return -1;
	}

	bool doEnable = false;
	bool doDisable = false;

	for (int i = 1; i < argc; i++)
	{
		if (wcscmp(argv[i], L"--autorun-enable") == 0) {
			doEnable = true;
		}
		else if (wcscmp(argv[i], L"--autorun-disable") == 0) {
			doDisable = true;
		}
	}

	LocalFree(argv);

	if (!doEnable && !doDisable) {
		return -1;
	}

	const LONG st = doEnable ? Autorun_Enable_HKLM() : Autorun_Disable_HKLM();
	return (st == ERROR_SUCCESS) ? 0 : 1;
}
