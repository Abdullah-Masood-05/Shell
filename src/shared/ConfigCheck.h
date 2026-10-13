#pragma once

/*
	shell.dll export used by `shell.exe -config:check <root.nss>`.

	HRESULT WINAPI ConfigCheck(const wchar_t *root, ConfigCheckSink sink, void *context)

	Parses `root` and its imports with the real parser into a private cache,
	reporting every error and warning through `sink`. Read-only: no registry
	writes, Application::Config is not changed, and it refuses to run inside
	Explorer or once the DLL has a live menu configuration loaded.

	Returns S_OK if the configuration loads, S_FALSE if it has errors, and a
	failure HRESULT if the check could not be run.
*/

typedef void(CALLBACK *ConfigCheckSink)(void *context, BOOL is_error, const wchar_t *file,
										 UINT line, UINT column, const wchar_t *message);
typedef HRESULT(WINAPI *ConfigCheckProc)(const wchar_t *root, ConfigCheckSink sink, void *context);

#define CONFIG_CHECK_EXPORT "ConfigCheck"

/*
	HRESULT WINAPI ConfigCheckTitles(const wchar_t *root, const wchar_t *where,
									 ConfigCheckSink sink, ConfigTitleSink titles,
									 void *context)

	ConfigCheck, then (if the configuration loads) every menu and item title
	through `titles`, in menu order, for `shell.exe -config:check -titles`.
	`where` is null or empty for every title, else "file", "folder", "back"
	(a folder's background), "desktop" or "taskbar": only the items whose type=
	lets them show there (where= and vis= are not evaluated). E_INVALIDARG for
	any other `where`.

	A title is evaluated when that needs no selection or window: a string,
	loc.<name> (whose value is a string) or title.<name>; anything else is
	CONFIG_TITLE_DYNAMIC with an empty `title`.
*/

#define CONFIG_TITLE_TEXT		0	// evaluated, not empty
#define CONFIG_TITLE_EMPTY		1	// evaluated to an empty string: a blank menu entry
#define CONFIG_TITLE_DYNAMIC	2	// depends on the selection or other runtime state

#define CONFIG_TITLE_MENU		0	// menu(...)
#define CONFIG_TITLE_ITEM		1	// item(...)
#define CONFIG_TITLE_MODIFY		2	// modify(...) of a system item

typedef struct CONFIG_TITLE
{
	UINT state;				// CONFIG_TITLE_TEXT, _EMPTY or _DYNAMIC
	UINT kind;				// CONFIG_TITLE_MENU, _ITEM or _MODIFY
	UINT depth;				// 0: top level
	const wchar_t *file;	// where the title= is
	UINT line, column;
	const wchar_t *source;	// "loc.<name>" or "title.<name>", else ""
	const wchar_t *title;	// the evaluated text
} CONFIG_TITLE;

typedef void(CALLBACK *ConfigTitleSink)(void *context, const CONFIG_TITLE *title);
typedef HRESULT(WINAPI *ConfigCheckTitlesProc)(const wchar_t *root, const wchar_t *where, ConfigCheckSink sink,
												ConfigTitleSink titles, void *context);

#define CONFIG_CHECK_TITLES_EXPORT "ConfigCheckTitles"
