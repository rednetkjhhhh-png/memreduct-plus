// Mem Reduct Plus
// Startup application manager added in 2026.
// This file is distributed under the GNU GPL v3 with the parent project.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winver.h>
#include <wchar.h>
#include <wctype.h>

#include "routine.h"
#include "app.h"
#include "rapp.h"

#include "resource.h"
#include "startup_manager.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

#define STARTUP_MAX_ITEMS 512
#define STARTUP_NAME_CCH 260
#define STARTUP_COMMAND_CCH 2048
#define STARTUP_PATH_CCH 1024
#define STARTUP_PUBLISHER_CCH 256
#define STARTUP_LOCATION_CCH 96
#define STARTUP_APPROVED_PATH_CCH 260

#define STARTUP_RUN_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_APPROVED_RUN_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define STARTUP_APPROVED_RUN32_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32"
#define STARTUP_APPROVED_FOLDER_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder"
#define STARTUP_BACKUP_PATH L"Software\\MemReductPlus\\StartupBackups"

typedef enum _STARTUP_SOURCE
{
	StartupSourceCurrentUserRun,
	StartupSourceCurrentUserRun32,
	StartupSourceLocalMachineRun,
	StartupSourceLocalMachineRun32,
	StartupSourceCurrentUserFolder,
	StartupSourceCommonFolder
} STARTUP_SOURCE;

typedef enum _STARTUP_RECOMMENDATION
{
	StartupRecommendationKeep,
	StartupRecommendationReview,
	StartupRecommendationDisable
} STARTUP_RECOMMENDATION;

typedef struct _STARTUP_ITEM
{
	STARTUP_SOURCE source;
	STARTUP_RECOMMENDATION recommendation;
	BOOL enabled;
	DWORD approved_root;
	REGSAM approved_view;
	WCHAR name[STARTUP_NAME_CCH];
	WCHAR command[STARTUP_COMMAND_CCH];
	WCHAR executable_path[STARTUP_PATH_CCH];
	WCHAR publisher[STARTUP_PUBLISHER_CCH];
	WCHAR location[STARTUP_LOCATION_CCH];
	WCHAR approved_path[STARTUP_APPROVED_PATH_CCH];
} STARTUP_ITEM;

typedef struct _STARTUP_CONTEXT
{
	STARTUP_ITEM *items;
	DWORD item_count;
} STARTUP_CONTEXT;

static LPCWSTR _startup_text (
	_In_ ULONG string_id
)
{
	LPCWSTR text = _r_locale_getstring (string_id);

	return text ? text : L"";
}

static BOOL _startup_contains_i (
	_In_opt_ LPCWSTR text,
	_In_ LPCWSTR token
)
{
	SIZE_T text_length;
	SIZE_T token_length;

	if (!text || !token)
		return FALSE;

	text_length = wcslen (text);
	token_length = wcslen (token);

	if (!token_length || token_length > text_length)
		return FALSE;

	for (SIZE_T i = 0; i <= text_length - token_length; i++)
	{
		if (CompareStringOrdinal (text + i, (INT)token_length, token, (INT)token_length, TRUE) == CSTR_EQUAL)
			return TRUE;
	}

	return FALSE;
}

static LPCWSTR _startup_filename (
	_In_ LPCWSTR path
)
{
	LPCWSTR slash;
	LPCWSTR slash_alt;

	slash = wcsrchr (path, L'\\');
	slash_alt = wcsrchr (path, L'/');

	if (slash_alt && (!slash || slash_alt > slash))
		slash = slash_alt;

	return slash ? slash + 1 : path;
}

static VOID _startup_extract_executable (
	_In_ LPCWSTR command,
	_Out_writes_ (buffer_count) LPWSTR buffer,
	_In_ SIZE_T buffer_count
)
{
	WCHAR expanded[STARTUP_COMMAND_CCH];
	LPCWSTR begin;
	LPCWSTR end;
	SIZE_T length;

	buffer[0] = UNICODE_NULL;

	if (!command || !command[0])
		return;

	if (!ExpandEnvironmentStringsW (command, expanded, ARRAYSIZE (expanded)))
		wcsncpy_s (expanded, ARRAYSIZE (expanded), command, _TRUNCATE);

	begin = expanded;

	while (*begin == L' ' || *begin == L'\t')
		begin++;

	if (*begin == L'\"')
	{
		begin++;
		end = wcschr (begin, L'\"');
	}
	else
	{
		end = begin;

		while (*end && *end != L' ' && *end != L'\t' && *end != L',')
			end++;
	}

	if (!end)
		end = begin + wcslen (begin);

	length = (SIZE_T)(end - begin);

	if (length >= buffer_count)
		length = buffer_count - 1;

	wmemcpy (buffer, begin, length);
	buffer[length] = UNICODE_NULL;
}

static VOID _startup_get_publisher (
	_In_ LPCWSTR file_path,
	_Out_writes_ (buffer_count) LPWSTR buffer,
	_In_ SIZE_T buffer_count
)
{
	typedef struct _LANGANDCODEPAGE
	{
		WORD language;
		WORD code_page;
	} LANGANDCODEPAGE;

	LANGANDCODEPAGE *translations = NULL;
	LPWSTR value = NULL;
	PVOID version_info;
	DWORD ignored = 0;
	DWORD version_size;
	UINT translation_size = 0;
	UINT value_size = 0;
	WCHAR query[64];

	buffer[0] = UNICODE_NULL;

	if (!file_path || !file_path[0])
		return;

	version_size = GetFileVersionInfoSizeW (file_path, &ignored);

	if (!version_size)
		return;

	version_info = HeapAlloc (GetProcessHeap (), HEAP_ZERO_MEMORY, version_size);

	if (!version_info)
		return;

	if (!GetFileVersionInfoW (file_path, 0, version_size, version_info))
		goto Cleanup;

	if (VerQueryValueW (version_info, L"\\VarFileInfo\\Translation", (LPVOID *)&translations, &translation_size) && translation_size >= sizeof (LANGANDCODEPAGE))
	{
		swprintf_s (
			query,
			ARRAYSIZE (query),
			L"\\StringFileInfo\\%04x%04x\\CompanyName",
			translations[0].language,
			translations[0].code_page
		);

		if (VerQueryValueW (version_info, query, (LPVOID *)&value, &value_size) && value && value_size)
			wcsncpy_s (buffer, buffer_count, value, _TRUNCATE);
	}

	if (!buffer[0] && VerQueryValueW (version_info, L"\\StringFileInfo\\040904B0\\CompanyName", (LPVOID *)&value, &value_size) && value && value_size)
		wcsncpy_s (buffer, buffer_count, value, _TRUNCATE);

Cleanup:
	HeapFree (GetProcessHeap (), 0, version_info);
}

static STARTUP_RECOMMENDATION _startup_classify (
	_In_ const STARTUP_ITEM *item
)
{
	static const LPCWSTR disable_tokens[] = {
		L"discord", L"steam", L"spotify", L"teams", L"ms-teams", L"slack",
		L"zoom", L"skype", L"epicgameslauncher", L"gog galaxy", L"battle.net",
		L"ccxprocess", L"adobe gc invoker", L"adobegcinvoker", L"aam updater",
		L"adobe aam", L"adobe updater", L"creative cloud",
		L"phone link", L"yourphone", L"whatsapp", L"telegram", L"qbittorrent"
	};
	static const LPCWSTR keep_tokens[] = {
		L"securityhealth", L"windows security", L"defender", L"antivirus", L"antimalware",
		L"avast", L"avg antivirus", L"kaspersky", L"bitdefender", L"eset", L"norton",
		L"mcafee", L"malwarebytes", L"ahnlab", L"v3lite",
		L"realtek", L"synaptics", L"touchpad", L"elan", L"bluetooth", L"audio console",
		L"intel", L"amd", L"nvidia", L"credential", L"bitlocker", L"hotkey", L"smart gesture"
	};
	WCHAR windows_path[MAX_PATH];

	for (SIZE_T i = 0; i < ARRAYSIZE (disable_tokens); i++)
	{
		if (_startup_contains_i (item->name, disable_tokens[i]) ||
			_startup_contains_i (_startup_filename (item->executable_path), disable_tokens[i]))
		{
			return StartupRecommendationDisable;
		}
	}

	for (SIZE_T i = 0; i < ARRAYSIZE (keep_tokens); i++)
	{
		if (_startup_contains_i (item->name, keep_tokens[i]) ||
			_startup_contains_i (item->command, keep_tokens[i]) ||
			_startup_contains_i (item->publisher, keep_tokens[i]))
		{
			return StartupRecommendationKeep;
		}
	}

	if (GetWindowsDirectoryW (windows_path, ARRAYSIZE (windows_path)) &&
		item->executable_path[0] &&
		CompareStringOrdinal (item->executable_path, (INT)wcslen (windows_path), windows_path, (INT)wcslen (windows_path), TRUE) == CSTR_EQUAL)
	{
		return StartupRecommendationKeep;
	}

	if (_startup_contains_i (item->publisher, L"Microsoft Windows") ||
		_startup_contains_i (item->publisher, L"Intel") ||
		_startup_contains_i (item->publisher, L"Advanced Micro Devices") ||
		_startup_contains_i (item->publisher, L"NVIDIA") ||
		_startup_contains_i (item->publisher, L"Realtek"))
	{
		return StartupRecommendationKeep;
	}

	return StartupRecommendationReview;
}

static HKEY _startup_root_from_id (
	_In_ DWORD root_id
)
{
	return root_id == 1 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

static BOOL _startup_is_enabled (
	_In_ const STARTUP_ITEM *item
)
{
	HKEY key;
	BYTE data[32];
	DWORD data_size = (DWORD)sizeof (data);
	DWORD type = 0;
	LSTATUS status;

	status = RegOpenKeyExW (
		_startup_root_from_id (item->approved_root),
		item->approved_path,
		0,
		KEY_QUERY_VALUE | item->approved_view,
		&key
	);

	if (status != ERROR_SUCCESS)
		return TRUE;

	status = RegQueryValueExW (key, item->name, NULL, &type, data, &data_size);
	RegCloseKey (key);

	if (status != ERROR_SUCCESS || type != REG_BINARY || !data_size)
		return TRUE;

	return data[0] != 3 && data[0] != 7;
}

static VOID _startup_set_approval (
	_Inout_ STARTUP_ITEM *item
)
{
	if (item->source == StartupSourceCurrentUserRun || item->source == StartupSourceLocalMachineRun)
		wcsncpy_s (item->approved_path, ARRAYSIZE (item->approved_path), STARTUP_APPROVED_RUN_PATH, _TRUNCATE);
	else if (item->source == StartupSourceCurrentUserRun32 || item->source == StartupSourceLocalMachineRun32)
		wcsncpy_s (item->approved_path, ARRAYSIZE (item->approved_path), STARTUP_APPROVED_RUN32_PATH, _TRUNCATE);
	else
		wcsncpy_s (item->approved_path, ARRAYSIZE (item->approved_path), STARTUP_APPROVED_FOLDER_PATH, _TRUNCATE);

	item->approved_root =
		(item->source == StartupSourceLocalMachineRun ||
		 item->source == StartupSourceLocalMachineRun32 ||
		 item->source == StartupSourceCommonFolder) ? 1 : 0;
}

static BOOL _startup_append_item (
	_Inout_ STARTUP_CONTEXT *context,
	_In_ const STARTUP_ITEM *item
)
{
	if (context->item_count >= STARTUP_MAX_ITEMS)
		return FALSE;

	context->items[context->item_count] = *item;
	context->item_count += 1;

	return TRUE;
}

static VOID _startup_enum_registry (
	_Inout_ STARTUP_CONTEXT *context,
	_In_ HKEY root,
	_In_ STARTUP_SOURCE source,
	_In_ REGSAM view,
	_In_ LPCWSTR location
)
{
	HKEY key;
	DWORD index = 0;
	LSTATUS status;

	status = RegOpenKeyExW (root, STARTUP_RUN_PATH, 0, KEY_QUERY_VALUE | view, &key);

	if (status != ERROR_SUCCESS)
		return;

	for (;;)
	{
		STARTUP_ITEM item = {0};
		WCHAR value_name[STARTUP_NAME_CCH];
		BYTE value_data[STARTUP_COMMAND_CCH * sizeof (WCHAR)];
		DWORD value_name_length = (DWORD)ARRAYSIZE (value_name);
		DWORD value_data_size = (DWORD)(sizeof (value_data) - sizeof (WCHAR));
		DWORD value_type = 0;

		status = RegEnumValueW (
			key,
			index,
			value_name,
			&value_name_length,
			NULL,
			&value_type,
			value_data,
			&value_data_size
		);

		if (status == ERROR_NO_MORE_ITEMS)
			break;

		index += 1;

		if (status != ERROR_SUCCESS || (value_type != REG_SZ && value_type != REG_EXPAND_SZ))
			continue;

		value_data[value_data_size] = 0;
		value_data[value_data_size + 1] = 0;

		item.source = source;
		item.approved_view = view;
		wcsncpy_s (item.name, ARRAYSIZE (item.name), value_name, _TRUNCATE);
		wcsncpy_s (item.command, ARRAYSIZE (item.command), (LPCWSTR)value_data, _TRUNCATE);
		wcsncpy_s (item.location, ARRAYSIZE (item.location), location, _TRUNCATE);
		_startup_extract_executable (item.command, item.executable_path, ARRAYSIZE (item.executable_path));
		_startup_get_publisher (item.executable_path, item.publisher, ARRAYSIZE (item.publisher));
		_startup_set_approval (&item);
		item.enabled = _startup_is_enabled (&item);
		item.recommendation = _startup_classify (&item);

		_startup_append_item (context, &item);
	}

	RegCloseKey (key);
}

static VOID _startup_enum_folder (
	_Inout_ STARTUP_CONTEXT *context,
	_In_ REFKNOWNFOLDERID folder_id,
	_In_ STARTUP_SOURCE source,
	_In_ LPCWSTR location
)
{
	PWSTR folder_path = NULL;
	WIN32_FIND_DATAW find_data;
	HANDLE find_handle;
	WCHAR search_path[STARTUP_PATH_CCH];

	if (FAILED (SHGetKnownFolderPath (folder_id, KF_FLAG_DEFAULT, NULL, &folder_path)))
		return;

	swprintf_s (search_path, ARRAYSIZE (search_path), L"%s\\*", folder_path);
	find_handle = FindFirstFileW (search_path, &find_data);

	if (find_handle == INVALID_HANDLE_VALUE)
		goto Cleanup;

	do
	{
		STARTUP_ITEM item = {0};

		if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			wcscmp (find_data.cFileName, L".") == 0 ||
			wcscmp (find_data.cFileName, L"..") == 0)
		{
			continue;
		}

		item.source = source;
		item.approved_view = 0;
		wcsncpy_s (item.name, ARRAYSIZE (item.name), find_data.cFileName, _TRUNCATE);
		swprintf_s (item.command, ARRAYSIZE (item.command), L"%s\\%s", folder_path, find_data.cFileName);
		wcsncpy_s (item.executable_path, ARRAYSIZE (item.executable_path), item.command, _TRUNCATE);
		wcsncpy_s (item.location, ARRAYSIZE (item.location), location, _TRUNCATE);
		_startup_set_approval (&item);
		item.enabled = _startup_is_enabled (&item);
		item.recommendation = _startup_classify (&item);

		_startup_append_item (context, &item);
	}
	while (FindNextFileW (find_handle, &find_data));

	FindClose (find_handle);

Cleanup:
	CoTaskMemFree (folder_path);
}

static VOID _startup_enumerate (
	_Inout_ STARTUP_CONTEXT *context
)
{
	context->item_count = 0;

	_startup_enum_registry (context, HKEY_CURRENT_USER, StartupSourceCurrentUserRun, KEY_WOW64_64KEY, L"Current user (64-bit)");
	_startup_enum_registry (context, HKEY_CURRENT_USER, StartupSourceCurrentUserRun32, KEY_WOW64_32KEY, L"Current user (32-bit)");
	_startup_enum_registry (context, HKEY_LOCAL_MACHINE, StartupSourceLocalMachineRun, KEY_WOW64_64KEY, L"All users (64-bit)");
	_startup_enum_registry (context, HKEY_LOCAL_MACHINE, StartupSourceLocalMachineRun32, KEY_WOW64_32KEY, L"All users (32-bit)");
	_startup_enum_folder (context, &FOLDERID_Startup, StartupSourceCurrentUserFolder, L"Current user folder");
	_startup_enum_folder (context, &FOLDERID_CommonStartup, StartupSourceCommonFolder, L"All users folder");
}

static ULONGLONG _startup_hash_item (
	_In_ const STARTUP_ITEM *item
)
{
	const ULONGLONG offset_basis = 1469598103934665603ULL;
	const ULONGLONG prime = 1099511628211ULL;
	ULONGLONG hash = offset_basis;
	const WCHAR *parts[] = {item->approved_path, item->name};

	hash ^= (ULONGLONG)item->approved_root;
	hash *= prime;
	hash ^= (ULONGLONG)item->approved_view;
	hash *= prime;

	for (SIZE_T p = 0; p < ARRAYSIZE (parts); p++)
	{
		for (const WCHAR *cursor = parts[p]; *cursor; cursor++)
		{
			hash ^= (ULONGLONG)towlower (*cursor);
			hash *= prime;
		}
	}

	return hash;
}

static VOID _startup_backup_key_name (
	_In_ const STARTUP_ITEM *item,
	_Out_writes_ (buffer_count) LPWSTR buffer,
	_In_ SIZE_T buffer_count
)
{
	swprintf_s (buffer, buffer_count, L"%016llX", _startup_hash_item (item));
}

static BOOL _startup_backup_value (
	_In_ const STARTUP_ITEM *item
)
{
	HKEY backup_root = NULL;
	HKEY backup_item = NULL;
	HKEY approved_key = NULL;
	BYTE *previous_data = NULL;
	DWORD previous_size = 0;
	DWORD previous_type = 0;
	DWORD previous_exists = 0;
	DWORD saved = 0;
	DWORD saved_size = (DWORD)sizeof (saved);
	DWORD root_id = item->approved_root;
	DWORD view = (DWORD)item->approved_view;
	WCHAR key_name[32];
	LSTATUS status;
	BOOL result = FALSE;

	status = RegCreateKeyExW (HKEY_CURRENT_USER, STARTUP_BACKUP_PATH, 0, NULL, 0, KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE, NULL, &backup_root, NULL);

	if (status != ERROR_SUCCESS)
		goto Cleanup;

	_startup_backup_key_name (item, key_name, ARRAYSIZE (key_name));

	status = RegCreateKeyExW (backup_root, key_name, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &backup_item, NULL);

	if (status != ERROR_SUCCESS)
		goto Cleanup;

	if (RegQueryValueExW (backup_item, L"Saved", NULL, NULL, (LPBYTE)&saved, &saved_size) == ERROR_SUCCESS && saved == 1)
	{
		result = TRUE;
		goto Cleanup;
	}

	status = RegOpenKeyExW (
		_startup_root_from_id (item->approved_root),
		item->approved_path,
		0,
		KEY_QUERY_VALUE | item->approved_view,
		&approved_key
	);

	if (status == ERROR_SUCCESS)
	{
		status = RegQueryValueExW (approved_key, item->name, NULL, &previous_type, NULL, &previous_size);

		if (status == ERROR_SUCCESS)
		{
			previous_exists = 1;

			if (previous_size)
			{
				previous_data = HeapAlloc (GetProcessHeap (), 0, previous_size);

				if (!previous_data)
					goto Cleanup;

				status = RegQueryValueExW (approved_key, item->name, NULL, &previous_type, previous_data, &previous_size);

				if (status != ERROR_SUCCESS)
					goto Cleanup;
			}
		}
		else if (status != ERROR_FILE_NOT_FOUND)
		{
			goto Cleanup;
		}
	}
	else if (status != ERROR_FILE_NOT_FOUND)
	{
		goto Cleanup;
	}

	if (RegSetValueExW (backup_item, L"ApprovedRoot", 0, REG_DWORD, (const BYTE *)&root_id, (DWORD)sizeof (root_id)) != ERROR_SUCCESS ||
		RegSetValueExW (backup_item, L"ApprovedView", 0, REG_DWORD, (const BYTE *)&view, (DWORD)sizeof (view)) != ERROR_SUCCESS ||
		RegSetValueExW (backup_item, L"ApprovedPath", 0, REG_SZ, (const BYTE *)item->approved_path, (DWORD)((wcslen (item->approved_path) + 1) * sizeof (WCHAR))) != ERROR_SUCCESS ||
		RegSetValueExW (backup_item, L"ValueName", 0, REG_SZ, (const BYTE *)item->name, (DWORD)((wcslen (item->name) + 1) * sizeof (WCHAR))) != ERROR_SUCCESS ||
		RegSetValueExW (backup_item, L"PreviousExists", 0, REG_DWORD, (const BYTE *)&previous_exists, (DWORD)sizeof (previous_exists)) != ERROR_SUCCESS ||
		RegSetValueExW (backup_item, L"PreviousType", 0, REG_DWORD, (const BYTE *)&previous_type, (DWORD)sizeof (previous_type)) != ERROR_SUCCESS)
	{
		goto Cleanup;
	}

	if (previous_exists && RegSetValueExW (backup_item, L"PreviousData", 0, REG_BINARY, previous_data, previous_size) != ERROR_SUCCESS)
		goto Cleanup;

	saved = 1;
	result = RegSetValueExW (backup_item, L"Saved", 0, REG_DWORD, (const BYTE *)&saved, (DWORD)sizeof (saved)) == ERROR_SUCCESS;

Cleanup:
	if (previous_data)
		HeapFree (GetProcessHeap (), 0, previous_data);

	if (approved_key)
		RegCloseKey (approved_key);

	if (backup_item)
		RegCloseKey (backup_item);

	if (backup_root)
		RegCloseKey (backup_root);

	return result;
}

static VOID _startup_remove_backup (
	_In_ const STARTUP_ITEM *item
)
{
	HKEY backup_root;
	WCHAR key_name[32];

	if (RegOpenKeyExW (HKEY_CURRENT_USER, STARTUP_BACKUP_PATH, 0, KEY_WRITE, &backup_root) != ERROR_SUCCESS)
		return;

	_startup_backup_key_name (item, key_name, ARRAYSIZE (key_name));
	RegDeleteTreeW (backup_root, key_name);
	RegCloseKey (backup_root);
}

static BOOL _startup_disable_item (
	_Inout_ STARTUP_ITEM *item
)
{
	HKEY approved_key;
	BYTE disabled_data[12] = {3, 0, 0, 0};
	FILETIME timestamp;
	LSTATUS status;

	if (!item->enabled)
		return TRUE;

	if (!_startup_backup_value (item))
		return FALSE;

	GetSystemTimeAsFileTime (&timestamp);
	CopyMemory (disabled_data + 4, &timestamp, sizeof (timestamp));

	status = RegCreateKeyExW (
		_startup_root_from_id (item->approved_root),
		item->approved_path,
		0,
		NULL,
		0,
		KEY_SET_VALUE | item->approved_view,
		NULL,
		&approved_key,
		NULL
	);

	if (status != ERROR_SUCCESS)
	{
		_startup_remove_backup (item);
		return FALSE;
	}

	status = RegSetValueExW (approved_key, item->name, 0, REG_BINARY, disabled_data, (DWORD)sizeof (disabled_data));
	RegCloseKey (approved_key);

	if (status != ERROR_SUCCESS)
	{
		_startup_remove_backup (item);
		return FALSE;
	}

	item->enabled = FALSE;
	return TRUE;
}

static BOOL _startup_read_dword (
	_In_ HKEY key,
	_In_ LPCWSTR value_name,
	_Out_ DWORD *value
)
{
	DWORD size = (DWORD)sizeof (*value);
	DWORD type = 0;

	return RegQueryValueExW (key, value_name, NULL, &type, (LPBYTE)value, &size) == ERROR_SUCCESS && type == REG_DWORD;
}

static BOOL _startup_read_string (
	_In_ HKEY key,
	_In_ LPCWSTR value_name,
	_Out_writes_ (buffer_count) LPWSTR buffer,
	_In_ DWORD buffer_count
)
{
	DWORD size = (DWORD)(buffer_count * sizeof (WCHAR));
	DWORD type = 0;

	if (RegQueryValueExW (key, value_name, NULL, &type, (LPBYTE)buffer, &size) != ERROR_SUCCESS || type != REG_SZ)
		return FALSE;

	buffer[buffer_count - 1] = UNICODE_NULL;
	return TRUE;
}

static DWORD _startup_restore_all (
	_Out_ DWORD *failed_count
)
{
	HKEY backup_root;
	DWORD restored_count = 0;
	DWORD index = 0;

	*failed_count = 0;

	if (RegOpenKeyExW (HKEY_CURRENT_USER, STARTUP_BACKUP_PATH, 0, KEY_ENUMERATE_SUB_KEYS | KEY_WRITE, &backup_root) != ERROR_SUCCESS)
		return 0;

	for (;;)
	{
		HKEY backup_item = NULL;
		HKEY approved_key = NULL;
		WCHAR subkey_name[64];
		WCHAR approved_path[STARTUP_APPROVED_PATH_CCH];
		WCHAR value_name[STARTUP_NAME_CCH];
		DWORD subkey_length = (DWORD)ARRAYSIZE (subkey_name);
		DWORD approved_root = 0;
		DWORD approved_view = 0;
		DWORD previous_exists = 0;
		DWORD previous_type = 0;
		DWORD saved = 0;
		BYTE *previous_data = NULL;
		DWORD previous_size = 0;
		LSTATUS status;
		BOOL restored = FALSE;

		status = RegEnumKeyExW (backup_root, index, subkey_name, &subkey_length, NULL, NULL, NULL, NULL);

		if (status == ERROR_NO_MORE_ITEMS)
			break;

		if (status != ERROR_SUCCESS)
		{
			index += 1;
			continue;
		}

		status = RegOpenKeyExW (backup_root, subkey_name, 0, KEY_QUERY_VALUE, &backup_item);

		if (status != ERROR_SUCCESS ||
			!_startup_read_dword (backup_item, L"Saved", &saved) || saved != 1 ||
			!_startup_read_dword (backup_item, L"ApprovedRoot", &approved_root) ||
			!_startup_read_dword (backup_item, L"ApprovedView", &approved_view) ||
			!_startup_read_dword (backup_item, L"PreviousExists", &previous_exists) ||
			!_startup_read_dword (backup_item, L"PreviousType", &previous_type) ||
			!_startup_read_string (backup_item, L"ApprovedPath", approved_path, ARRAYSIZE (approved_path)) ||
			!_startup_read_string (backup_item, L"ValueName", value_name, ARRAYSIZE (value_name)))
		{
			goto ItemCleanup;
		}

		if (previous_exists)
		{
			status = RegQueryValueExW (backup_item, L"PreviousData", NULL, NULL, NULL, &previous_size);

			if (status != ERROR_SUCCESS)
				goto ItemCleanup;

			if (previous_size)
			{
				previous_data = HeapAlloc (GetProcessHeap (), 0, previous_size);

				if (!previous_data)
					goto ItemCleanup;

				status = RegQueryValueExW (backup_item, L"PreviousData", NULL, NULL, previous_data, &previous_size);

				if (status != ERROR_SUCCESS)
					goto ItemCleanup;
			}
		}

		status = RegCreateKeyExW (
			_startup_root_from_id (approved_root),
			approved_path,
			0,
			NULL,
			0,
			KEY_SET_VALUE | (REGSAM)approved_view,
			NULL,
			&approved_key,
			NULL
		);

		if (status != ERROR_SUCCESS)
			goto ItemCleanup;

		if (previous_exists)
			status = RegSetValueExW (approved_key, value_name, 0, previous_type, previous_data, previous_size);
		else
		{
			status = RegDeleteValueW (approved_key, value_name);

			if (status == ERROR_FILE_NOT_FOUND)
				status = ERROR_SUCCESS;
		}

		restored = status == ERROR_SUCCESS;

ItemCleanup:
		if (previous_data)
			HeapFree (GetProcessHeap (), 0, previous_data);

		if (approved_key)
			RegCloseKey (approved_key);

		if (backup_item)
			RegCloseKey (backup_item);

		if (restored)
		{
			RegDeleteTreeW (backup_root, subkey_name);
			restored_count += 1;
		}
		else
		{
			*failed_count += 1;
			index += 1;
		}
	}

	RegCloseKey (backup_root);

	if (!*failed_count)
		RegDeleteTreeW (HKEY_CURRENT_USER, STARTUP_BACKUP_PATH);

	return restored_count;
}

static LPCWSTR _startup_recommendation_text (
	_In_ STARTUP_RECOMMENDATION recommendation
)
{
	switch (recommendation)
	{
		case StartupRecommendationKeep:
			return _startup_text (IDS_STARTUP_KEEP);

		case StartupRecommendationDisable:
			return _startup_text (IDS_STARTUP_DISABLE_RECOMMENDED);

		default:
			return _startup_text (IDS_STARTUP_REVIEW);
	}
}

static VOID _startup_fill_list (
	_In_ HWND hwnd,
	_Inout_ STARTUP_CONTEXT *context
)
{
	HWND listview = GetDlgItem (hwnd, IDC_STARTUP_LIST);
	DWORD enabled_count = 0;
	DWORD recommended_count = 0;
	WCHAR summary[256];

	ListView_DeleteAllItems (listview);

	for (DWORD i = 0; i < context->item_count; i++)
	{
		STARTUP_ITEM *item = &context->items[i];
		LVITEMW list_item = {0};
		INT row;

		if (item->enabled)
			enabled_count += 1;

		if (item->enabled && item->recommendation == StartupRecommendationDisable)
			recommended_count += 1;

		list_item.mask = LVIF_TEXT | LVIF_PARAM;
		list_item.iItem = (INT)i;
		list_item.pszText = item->name;
		list_item.lParam = (LPARAM)i;
		row = ListView_InsertItem (listview, &list_item);

		ListView_SetItemText (listview, row, 1, (LPWSTR)(item->enabled ? _startup_text (IDS_STARTUP_ENABLED) : _startup_text (IDS_STARTUP_DISABLED)));
		ListView_SetItemText (listview, row, 2, (LPWSTR)_startup_recommendation_text (item->recommendation));
		ListView_SetItemText (listview, row, 3, item->location);
		ListView_SetItemText (listview, row, 4, item->publisher[0] ? item->publisher : item->command);

		ListView_SetCheckState (listview, row, item->enabled && item->recommendation == StartupRecommendationDisable);
	}

	swprintf_s (
		summary,
		ARRAYSIZE (summary),
		_startup_text (IDS_STARTUP_SUMMARY),
		context->item_count,
		enabled_count,
		recommended_count
	);

	SetDlgItemTextW (hwnd, IDC_STARTUP_SUMMARY, summary);
}

static VOID _startup_refresh (
	_In_ HWND hwnd,
	_Inout_ STARTUP_CONTEXT *context
)
{
	_startup_enumerate (context);
	_startup_fill_list (hwnd, context);
}

static VOID _startup_disable_batch (
	_In_ HWND hwnd,
	_Inout_ STARTUP_CONTEXT *context,
	_In_ BOOL recommended_only
)
{
	HWND listview = GetDlgItem (hwnd, IDC_STARTUP_LIST);
	DWORD changed_count = 0;
	DWORD protected_count = 0;
	DWORD failed_count = 0;
	WCHAR result_text[256];

	if (MessageBoxW (hwnd, _startup_text (IDS_STARTUP_CONFIRM_DISABLE), APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		return;

	for (INT row = 0; row < ListView_GetItemCount (listview); row++)
	{
		LVITEMW list_item = {0};
		STARTUP_ITEM *item;

		list_item.mask = LVIF_PARAM;
		list_item.iItem = row;

		if (!ListView_GetItem (listview, &list_item) || (DWORD)list_item.lParam >= context->item_count)
			continue;

		item = &context->items[(DWORD)list_item.lParam];

		if (!item->enabled)
			continue;

		if (item->recommendation == StartupRecommendationKeep)
		{
			if (!recommended_only && ListView_GetCheckState (listview, row))
				protected_count += 1;

			continue;
		}

		if (recommended_only)
		{
			if (item->recommendation != StartupRecommendationDisable)
				continue;
		}
		else if (!ListView_GetCheckState (listview, row))
		{
			continue;
		}

		if (_startup_disable_item (item))
			changed_count += 1;
		else
			failed_count += 1;
	}

	swprintf_s (result_text, ARRAYSIZE (result_text), _startup_text (IDS_STARTUP_DISABLE_RESULT), changed_count, protected_count, failed_count);
	MessageBoxW (hwnd, result_text, APP_NAME, failed_count ? MB_OK | MB_ICONWARNING : MB_OK | MB_ICONINFORMATION);
	_startup_refresh (hwnd, context);
}

static VOID _startup_restore (
	_In_ HWND hwnd,
	_Inout_ STARTUP_CONTEXT *context
)
{
	DWORD failed_count = 0;
	DWORD restored_count;
	WCHAR result_text[256];

	if (MessageBoxW (hwnd, _startup_text (IDS_STARTUP_CONFIRM_RESTORE), APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
		return;

	restored_count = _startup_restore_all (&failed_count);
	swprintf_s (result_text, ARRAYSIZE (result_text), _startup_text (IDS_STARTUP_RESTORE_RESULT), restored_count, failed_count);
	MessageBoxW (hwnd, result_text, APP_NAME, failed_count ? MB_OK | MB_ICONWARNING : MB_OK | MB_ICONINFORMATION);
	_startup_refresh (hwnd, context);
}

static VOID _startup_initialize_dialog (
	_In_ HWND hwnd,
	_Inout_ STARTUP_CONTEXT *context
)
{
	HWND listview = GetDlgItem (hwnd, IDC_STARTUP_LIST);
	LVCOLUMNW column = {0};
	LPCWSTR column_names[] = {
		_startup_text (IDS_STARTUP_COLUMN_NAME),
		_startup_text (IDS_STARTUP_COLUMN_STATUS),
		_startup_text (IDS_STARTUP_COLUMN_RECOMMENDATION),
		_startup_text (IDS_STARTUP_COLUMN_LOCATION),
		_startup_text (IDS_STARTUP_COLUMN_DETAILS)
	};
	INT column_widths[] = {150, 72, 105, 120, 220};

	SetWindowTextW (hwnd, _startup_text (IDS_STARTUP_MANAGER));
	SetDlgItemTextW (hwnd, IDC_STARTUP_DESCRIPTION, _startup_text (IDS_STARTUP_DESCRIPTION));
	SetDlgItemTextW (hwnd, IDC_STARTUP_REFRESH, _startup_text (IDS_STARTUP_REFRESH));
	SetDlgItemTextW (hwnd, IDC_STARTUP_DISABLE_SELECTED, _startup_text (IDS_STARTUP_DISABLE_SELECTED));
	SetDlgItemTextW (hwnd, IDC_STARTUP_DISABLE_RECOMMENDED, _startup_text (IDS_STARTUP_DISABLE_RECOMMENDED_BUTTON));
	SetDlgItemTextW (hwnd, IDC_STARTUP_RESTORE, _startup_text (IDS_STARTUP_RESTORE));
	SetDlgItemTextW (hwnd, IDCANCEL, _startup_text (IDS_CLOSE));

	ListView_SetExtendedListViewStyleEx (
		listview,
		LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP,
		LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP
	);

	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
	column.fmt = LVCFMT_LEFT;

	for (INT i = 0; i < (INT)ARRAYSIZE (column_names); i++)
	{
		column.pszText = (LPWSTR)column_names[i];
		column.cx = column_widths[i];
		ListView_InsertColumn (listview, i, &column);
	}

	_startup_refresh (hwnd, context);
}

static INT_PTR CALLBACK StartupManagerProc (
	_In_ HWND hwnd,
	_In_ UINT message,
	_In_ WPARAM wparam,
	_In_ LPARAM lparam
)
{
	STARTUP_CONTEXT *context = (STARTUP_CONTEXT *)GetWindowLongPtrW (hwnd, DWLP_USER);

	switch (message)
	{
		case WM_INITDIALOG:
		{
			context = HeapAlloc (GetProcessHeap (), HEAP_ZERO_MEMORY, sizeof (*context));

			if (!context)
			{
				EndDialog (hwnd, IDCANCEL);
				return TRUE;
			}

			context->items = HeapAlloc (GetProcessHeap (), HEAP_ZERO_MEMORY, sizeof (STARTUP_ITEM) * STARTUP_MAX_ITEMS);

			if (!context->items)
			{
				HeapFree (GetProcessHeap (), 0, context);
				EndDialog (hwnd, IDCANCEL);
				return TRUE;
			}

			SetWindowLongPtrW (hwnd, DWLP_USER, (LONG_PTR)context);
			_startup_initialize_dialog (hwnd, context);
			return TRUE;
		}

		case WM_COMMAND:
		{
			if (!context)
				break;

			switch (LOWORD (wparam))
			{
				case IDC_STARTUP_REFRESH:
					_startup_refresh (hwnd, context);
					return TRUE;

				case IDC_STARTUP_DISABLE_SELECTED:
					_startup_disable_batch (hwnd, context, FALSE);
					return TRUE;

				case IDC_STARTUP_DISABLE_RECOMMENDED:
					_startup_disable_batch (hwnd, context, TRUE);
					return TRUE;

				case IDC_STARTUP_RESTORE:
					_startup_restore (hwnd, context);
					return TRUE;

				case IDOK:
				case IDCANCEL:
					EndDialog (hwnd, LOWORD (wparam));
					return TRUE;
			}

			break;
		}

		case WM_DESTROY:
		{
			if (context)
			{
				if (context->items)
					HeapFree (GetProcessHeap (), 0, context->items);

				HeapFree (GetProcessHeap (), 0, context);
				SetWindowLongPtrW (hwnd, DWLP_USER, 0);
			}

			break;
		}
	}

	UNREFERENCED_PARAMETER (lparam);
	return FALSE;
}

VOID StartupManagerShow (
	_In_opt_ HWND hwnd_parent
)
{
	DialogBoxParamW (
		GetModuleHandleW (NULL),
		MAKEINTRESOURCEW (IDD_STARTUP_MANAGER),
		hwnd_parent,
		&StartupManagerProc,
		0
	);
}
