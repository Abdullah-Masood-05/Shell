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
