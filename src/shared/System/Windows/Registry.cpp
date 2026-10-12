
#include <windows.h>
#include <iostream>

#include "System.h"

/*
#include "..\Text\string.h"
#include "..\Environment.h"
#include "Registry.h"
*/

namespace Nilesoft
{
	using namespace Text;
	/*
CString Registry::ExpandStrings(const CString& text) {
	WCHAR buffer[1024];
	buffer[0] = 0;
	::ExpandEnvironmentStrings(text, buffer, _countof(buffer));
	return buffer;
}
CString Registry::QueryStringValue(CRegKey& key, PCWSTR name) {
	ULONG len = 0;
	key.QueryStringValue(name, nullptr, &len);
	auto value = std::make_unique<WCHAR[]>(len);
	key.QueryStringValue(name, value.get(), &len);
	return value.get();
}
CString Registry::GetDataAsString(CRegKey& key, const RegistryItem& item) {
	ULONG realsize = item.Size;
	ULONG size = (realsize > (1 << 10) ? (1 << 10) : realsize) / sizeof(WCHAR);
	LSTATUS status;
	CString text;
	DWORD type;

	switch (item.Type) {
		case REG_SZ:
		case REG_EXPAND_SZ:
			status = key.QueryStringValue(item.Name, text.GetBufferSetLength(size), &size);
			break;

		case REG_LINK:
			//text = GetLinkPath();
			break;

		case REG_MULTI_SZ:
			size *= 2;
			status = ::RegQueryValueEx(key, item.Name, nullptr, &type, (PBYTE)text.GetBufferSetLength(size / 2), &size);
			if (status == ERROR_SUCCESS) {
				auto p = text.GetBuffer();
				while (*p) {
					p += ::wcslen(p);
					*p = L' ';
					p++;
				}
			}
			break;

		case REG_DWORD:
		{
			DWORD value;
			if (ERROR_SUCCESS == key.QueryDWORDValue(item.Name, value)) {
				text.Format(L"0x%08X (%u)", value, value);
			}
			break;
		}

		case REG_QWORD:
		{
			ULONGLONG value;
			if (ERROR_SUCCESS == key.QueryQWORDValue(item.Name, value)) {
				auto fmt = value < (1LL << 32) ? L"0x%08llX (%llu)" : L"0x%016llX (%llu)";
				text.Format(fmt, value, value);
			}
			break;
		}

		case REG_BINARY:
			CString digit;
			auto buffer = std::make_unique<BYTE[]>(item.Size);
			if (buffer == nullptr)
				break;
			ULONG bytes = item.Size;
			auto status = key.QueryBinaryValue(item.Name, buffer.get(), &bytes);
			if (status == ERROR_SUCCESS) {
				for (DWORD i = 0; i < std::min<ULONG>(bytes, 64); i++) {
					digit.Format(L"%02X ", buffer[i]);
					text += digit;
				}
			}
			break;
	}

	return text.GetLength() < 1024 ? text : text.Mid(0, 1024);
}
	*/
	RegistryKey RegistryKey::FromHandle(HKEY handle, uint32_t view)
	{
		if(!handle)
			return RegistryKey(handle, true, view);
		return nullptr;
	}

	RegistryKey RegistryKey::OpenBaseKey(HKEY hkey, bool writable, uint32_t view)
	{
		return RegistryKey(hkey, writable, view, true, false);
	}

	RegistryKey::~RegistryKey()
	{
		if(m_autoclose)
		{
			Close();
		}
	}

	RegistryKey::RegistryKey(const nullptr_t&) { }

	//RegKey(const RegKey&)=delete;
	RegistryKey::RegistryKey(const RegistryKey & key)
	{
		m_hkey = key.m_hkey;
		m_systemkey = key.m_systemkey;
		m_view = key.m_view;
		m_writable = key.m_writable;
		m_autoclose = false;
		m_ref++;
	}

	RegistryKey::RegistryKey(HKEY hkeybase, bool writable)
		: RegistryKey(hkeybase, writable, 0, true, false)
	{
	}

	RegistryKey::RegistryKey(HKEY hkeybase, bool writable, uint32_t view)
		: RegistryKey(hkeybase, writable, view, true, false)
	{
	}

	RegistryKey::RegistryKey(HKEY hkeybase, bool writable, uint32_t view, bool systemkey, bool autoclose)
		: m_hkey{ hkeybase },
		m_writable{ writable }, m_view{ view },
		m_systemkey{ systemkey },
		m_autoclose{ autoclose }
	{
	}

	RegistryKey::operator HKEY() const { return m_hkey; }

	bool RegistryKey::operator ==(const nullptr_t&) { return m_hkey == nullptr; }
	bool RegistryKey::operator !=(const nullptr_t&) { return m_hkey != nullptr; }
	RegistryKey& RegistryKey::operator=(const nullptr_t&) { Close(); return *this; }

	bool RegistryKey::operator ==(const RegistryKey & key) { return m_hkey == key.m_hkey; }
	bool RegistryKey::operator !=(const RegistryKey & key) { return m_hkey != key.m_hkey; }

	RegistryKey RegistryKey::CreateSubKey(const wchar_t* subkey, bool autoclose, uint32_t options) const
	{
		if(is_valid(m_hkey, subkey))
		{
			HKEY hkey = nullptr;
			auto ret = ::RegCreateKeyExW(m_hkey, subkey, 0, nullptr, options,
										get_key_access(m_writable) | m_view, nullptr, &hkey, nullptr);
			if(ret == ERROR_SUCCESS && is_valid(hkey))
				return RegistryKey(hkey, m_writable, m_view, false, autoclose);
		}
		return nullptr;
	}

	RegistryKey RegistryKey::OpenSubKey(const wchar_t* subkey, bool autoclose, bool writable, uint32_t view) const
	{
		auto v = view == 0xffffffff ? m_view : view;
		if(is_valid(m_hkey, subkey))
		{
			HKEY hkey = nullptr;
			auto ret = ::RegOpenKeyExW(m_hkey, subkey, 0, get_key_access(writable) | v, &hkey);
			if(ret == ERROR_SUCCESS && is_valid(hkey))
			{
				return RegistryKey(hkey, writable, v, false, autoclose);
			}
		}
		return nullptr;
	}

	bool RegistryKey::OpenSubKey(HKEY root_key, const wchar_t *subkey, bool autoclose, bool writable, uint32_t view)
	{
		auto v = view == 0xffffffff ? m_view : view;
		if(is_valid(root_key, subkey))
		{
			auto ret = ::RegOpenKeyExW(root_key, subkey, 0, get_key_access(writable) | v, &m_hkey);
			if(ret == ERROR_SUCCESS && is_valid(m_hkey))
			{
				m_writable = writable;
				m_view = view;
				m_systemkey = false;
				m_autoclose = autoclose;
				return true;
			}
		}
		return false;
	}

	/*wchar_t* RegistryKey::GetString(const wchar_t* name, bool expand) const
	{
		wchar_t* value = nullptr;
		GetString(name, &value, expand);
		return value;
	}*/

	uint32_t RegistryKey::GetString(const wchar_t* name, wchar_t* * value, bool expand) const
	{
		uint32_t size = 0;
		uint32_t type = expand ? REG_EXPAND_SZ : REG_SZ;
		if(GetValue(name, type, nullptr, &size))
		{
			uint32_t len = (size / sizeof(wchar_t)) + 1;
			if(len > 1)
			{
				if(value)
				{
					*value = new wchar_t[len];
					GetValue(name, type, (LPBYTE)* value, &size);
					(*value)[len - 1] = 0;
				}
				return len;
			}
		}
		return 0;
	}

	uint32_t RegistryKey::GetString(const wchar_t* name, wchar_t* value, uint32_t max_length, bool expand) const
	{
		if(!value || max_length == 0)
			return 0;

		// One unit is held back so an unterminated value can still be terminated
		// on the way out - RegQueryValueEx does not promise one. See the comment
		// on read_string_value below.
		uint32_t size = (max_length - 1) * sizeof(wchar_t);
		uint32_t type = expand ? REG_EXPAND_SZ : REG_SZ;
		if(GetValue(name, type, (LPBYTE)value, &size) && size)
		{
			auto count = size / sizeof(wchar_t);
			if(count > 0 && value[count - 1] == L'\0')
				count--;
			value[count] = L'\0';
			return static_cast<uint32_t>(count);
		}
		return 0;
	}

	/*
		Read one REG_SZ / REG_EXPAND_SZ value into `value`, and report its type.

		RegQueryValueEx carries an explicit warning about string values:

			"the value returned is NOT guaranteed to be null-terminated ... even
			if the function returns ERROR_SUCCESS, the application should ensure
			that the string is properly terminated before using it"

		https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexw

		So the length comes from the byte count the API reports back, and the
		terminator is treated as optional rather than assumed. Deriving it as
		`datasize/2 - 1` instead had two consequences: an empty value underflowed
		to SIZE_MAX characters, which string::terminate then clamped to a
		15-character run of NULs; and a value stored without its terminator lost
		its last character. The second one is not hypothetical - see the comment
		in exe/src/Main.cpp about the TreatAs redirect that Unregister could never
		recognise.
	*/
	static bool read_string_value(HKEY hkey, const wchar_t *name,
								  uint32_t &type, string &value)
	{
		type = REG_NONE;

		uint32_t bytes = 0;
		if(ERROR_SUCCESS != ::RegQueryValueExW(hkey, name, nullptr,
											   reinterpret_cast<DWORD *>(&type),
											   nullptr, reinterpret_cast<DWORD *>(&bytes)))
			return false;

		if(type != REG_SZ && type != REG_EXPAND_SZ)
			return false;

		// One spare unit, so a value stored without a terminator can be given one.
		auto count = bytes / sizeof(wchar_t);
		auto data = value.buffer(static_cast<uint32_t>(count + 1));
		if(!data)
			return false;

		if(bytes != 0 &&
		   ERROR_SUCCESS != ::RegQueryValueExW(hkey, name, nullptr,
											   reinterpret_cast<DWORD *>(&type),
											   reinterpret_cast<LPBYTE>(data),
											   reinterpret_cast<DWORD *>(&bytes)))
		{
			value.release(0u);
			return false;
		}

		// What the second call reports is what was actually written.
		count = bytes / sizeof(wchar_t);
		if(count > 0 && data[count - 1] == L'\0')
			count--;

		value.release(static_cast<uint32_t>(count));
		return true;
	}

	string RegistryKey::ReadString(const wchar_t *name) const
	{
		if(is_valid(m_hkey))
		{
			uint32_t type = REG_NONE;
			string ret;
			if(read_string_value(m_hkey, name, type, ret))
				return (type == REG_SZ) ? ret.move() : Environment::Expand(ret).move();
		}
		return nullptr;
	}

	bool RegistryKey::ReadString(const wchar_t *name, string &value) const
	{
		if(is_valid(m_hkey))
		{
			uint32_t type = REG_NONE;
			if(read_string_value(m_hkey, name, type, value))
			{
				// Environment::Expand returns the expansion, it does not modify
				// its argument - the result used to be computed and thrown away,
				// so REG_EXPAND_SZ values came back with the %VARIABLES% intact.
				if(type == REG_EXPAND_SZ)
					value = Environment::Expand(value).move();
				return true;
			}
			value.release(0u);
		}
		return false;
	}

	string RegistryKey::GetString(const wchar_t* name, bool expand) const
	{
		if(is_valid(m_hkey))
		{
			uint32_t type = REG_NONE;
			string ret;
			// Same terminator rules as ReadString. The old form also ignored
			// whether the second read succeeded, and returned a string over the
			// zero-filled buffer when it had not.
			if(read_string_value(m_hkey, name, type, ret))
			{
				if(expand || type == REG_EXPAND_SZ)
					return Environment::Expand(ret).move();
				return ret.move();
			}
		}
		return nullptr;
	}

	bool RegistryKey::GetInt(const wchar_t* name, int* value) const
	{
		int size = sizeof(DWORD);
		if(GetValue(name, REG_DWORD, (LPBYTE)value, (uint32_t*)& size))
		{
			return true;
		}
		return false;
	}

	int RegistryKey::GetInt(const wchar_t* name, int defualt_value) const
	{
		int value = 0, size = sizeof(int);
		if(GetValue(name, REG_DWORD, (LPBYTE)&value, (uint32_t*)& size))
		{
			return value;
		}
		return defualt_value;
	}

	uint32_t RegistryKey::GetDWord(const wchar_t* name, uint32_t defualt_value) const
	{
		uint32_t value = 0, sizeOF = sizeof(uint32_t);
		if(GetValue(name, REG_DWORD, (LPBYTE)& value, &sizeOF))
		{
			return value;
		}
		return defualt_value;
	}

	uint64_t RegistryKey::GetQWord(const wchar_t* name, uint64_t defualt_value) const
	{
		uint64_t value = defualt_value;
		uint32_t size = sizeof(DWORD64);
		if(GetValue(name, REG_QWORD, (LPBYTE)& value, &size))
		{
			return value;
		}
		return defualt_value;
	}

	bool RegistryKey::get_value(const wchar_t *name, uint32_t *type, uint32_t *cbdata, void* data) const
	{
		if(is_valid(m_hkey))
		{
			return ERROR_SUCCESS == ::RegQueryValueExW(m_hkey, name, nullptr, (DWORD*)type, (LPBYTE)data, (DWORD*)cbdata);
		}
		return false;
	}

	bool RegistryKey::GetValue(const wchar_t *name, uint32_t type, LPBYTE data, uint32_t *datasize) const
	{
		if(is_valid(m_hkey))
		{
			auto dwtype = REG_NONE;
			if(ERROR_SUCCESS == ::RegQueryValueExW(m_hkey, name, nullptr, &dwtype, data, (DWORD *)datasize))
			{
				return type == dwtype;
			}
		}
		return false;
	}

	uint32_t RegistryKey::ValueSize(const wchar_t* name) const
	{
		uint32_t size = 0;
		if(is_valid(m_hkey))
			::RegQueryValueExW(m_hkey, name, nullptr, nullptr, nullptr, (DWORD*)& size);
		return size;
	}

	uint32_t RegistryKey::ValueType(const wchar_t* name) const
	{
		uint32_t type = REG_NONE;
		if(is_valid(m_hkey))
			::RegQueryValueExW(m_hkey, name, nullptr, (DWORD*)& type, nullptr, nullptr);
		return type;
	}

	uint32_t RegistryKey::ValueCount() const
	{
		uint32_t values = 0;
		if(is_valid(m_hkey))
		{
			uint32_t junk = 0;
			::RegQueryInfoKeyW(m_hkey, nullptr, nullptr, nullptr,
				(LPDWORD)& junk,     // subkeys
				nullptr,
				nullptr,
					(LPDWORD)& values,   // values
				nullptr, nullptr, nullptr, nullptr);
		}
		return values;
	}

	bool RegistryKey::DeleteValue(const wchar_t* name) const
	{
		if(is_valid(m_hkey) && m_writable)
			return ERROR_SUCCESS == ::RegDeleteValueW(m_hkey, name);
		//RegDeleteKeyValueW
		return false;
	}


	bool RegistryKey::DeleteSubKey(const wchar_t* subKey) const
	{
		if(is_valid(m_hkey, subKey) && m_writable)
		{
			return ERROR_SUCCESS == ::RegDeleteKeyExW(m_hkey, subKey, m_view, 0);
		}
		return false;
	}

	bool RegistryKey::DeleteSubKeyTree(const wchar_t* subKey) const
	{
		if(is_valid(m_hkey, subKey) && m_writable)
			return ERROR_SUCCESS == ::RegDeleteTreeW(m_hkey, subKey);
		return false;
	}

	uint32_t RegistryKey::SubKeyCount() const
	{
		uint32_t subkeys = 0;
		uint32_t junk = 0;
		if(is_valid(m_hkey))
		{
			::RegQueryInfoKeyW(m_hkey,
								nullptr, nullptr, nullptr,
									(LPDWORD)& subkeys,  // subkeys
								nullptr, nullptr,
									(LPDWORD)& junk,     // values
								nullptr, nullptr, nullptr, nullptr);
		}
		return subkeys;
	}

	bool RegistryKey::SetString(const wchar_t* name, const wchar_t* value, size_t length, bool expand) const
	{
		if(value && *value && length > 0)
		{
			length++;
			length *= sizeof(wchar_t);
		}

		uint32_t type = expand ? REG_EXPAND_SZ : REG_SZ;
		return SetValue(name, type, (LPBYTE)value, length);
	}

	bool RegistryKey::SetString(const wchar_t* name, const wchar_t* value, bool expand) const
	{
		auto length = string::Length(value);
		return SetString(name, value, length, expand);
	}

	bool RegistryKey::SetString(const wchar_t* subkey, const wchar_t* name, const wchar_t* value, bool expand) const
	{
		return Registry::SetKeyValue(m_hkey, subkey, name, value, expand);
	}

	bool RegistryKey::SetString(const wchar_t* subkey, const wchar_t* name, const wchar_t* value, size_t length, bool expand) const
	{
		return Registry::SetKeyValue(m_hkey, subkey, name, value, length, expand);
	}

	bool RegistryKey::SetInt(const wchar_t* name, uint32_t value) const
	{
		return SetValue(name, REG_DWORD, (LPBYTE)& value, sizeof(value));
	}

	bool RegistryKey::SetInt(const wchar_t* subkey, const wchar_t* name, uint32_t value) const
	{
		return Registry::SetKeyValue(m_hkey, subkey, name, value);
	}

	bool RegistryKey::SetInt64(const wchar_t* name, uint64_t value) const
	{
		return SetValue(name, REG_QWORD, (LPBYTE)& value, sizeof(value));
	}

	bool RegistryKey::SetInt64(const wchar_t* subkey, const wchar_t* name, uint64_t value) const
	{
		return Registry::SetKeyValue(m_hkey, subkey, name, value);
	}

	bool RegistryKey::SetValue(const wchar_t* name, uint32_t type, LPBYTE data, size_t size) const
	{
		auto ret = false;
		if(is_valid(m_hkey) && m_writable)
		{
			ret= ERROR_SUCCESS == ::RegSetValueExW(m_hkey, name, 0, type, data, (DWORD)size);
			//::RegFlushKey(m_hkey);
		}
		return ret;
	}

	const HKEY RegistryKey::Handle()
	{
		return m_hkey;
	}

	void RegistryKey::Flush() const
	{
		if(is_valid(m_hkey))
			::RegFlushKey(m_hkey);
	}

	void RegistryKey::Close()
	{
		if(is_valid(m_hkey))
		{
			if(!is_system_key(m_hkey))
			{
				if(m_ref == 0)
					::RegCloseKey(m_hkey);
			}
			m_hkey = nullptr;
		}
	}

	bool RegistryKey::IsWritable() const
	{
		return m_writable;
	}

	uint32_t RegistryKey::View() const
	{
		return m_view;
	}

	bool RegistryKey::ExistsValue(const wchar_t* name) const
	{
		if(is_valid(m_hkey, name))
			return ERROR_SUCCESS == ::RegQueryValueExW(m_hkey, name, nullptr, nullptr, nullptr, nullptr);
		return false;
	}

	bool RegistryKey::Exists(const wchar_t* subKey) const
	{
		bool result = false;
		if(is_valid(m_hkey, subKey))
		{
			HKEY hKey = nullptr;
			// Try open an existing key.
			result = ERROR_SUCCESS == ::RegOpenKeyExW(m_hkey, subKey, 0, KEY_READ, &hKey);
			if(result && hKey)
				::RegCloseKey(hKey);
		}
		return result;
	}

	void RegistryKey::auto_close(bool set_auto_close)
	{
		m_autoclose = set_auto_close;
	}

	////////////////////[ Registry ]/////////////////////////

	const RegistryKey Registry::ClassesRoot = RegistryKey(HKEY_CLASSES_ROOT, true, KEY_WOW64_64KEY);
	const RegistryKey Registry::CurrentUser = RegistryKey(HKEY_CURRENT_USER, true, KEY_WOW64_64KEY);
	const RegistryKey Registry::LocalMachine = RegistryKey(HKEY_LOCAL_MACHINE, true, KEY_WOW64_64KEY);

	bool Registry::DeleteKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name)
	{
		if(is_valid(hkey, subKey) && name)
		{
			return ERROR_SUCCESS == ::RegDeleteKeyValueW(hkey, subKey, name);
		}
		return false;
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name,
							  uint32_t type, LPBYTE value, size_t value_size)
	{
		auto ret = false;
		if(is_valid(hkey, subKey))
		{
			ret = ERROR_SUCCESS == ::RegSetKeyValueW(hkey, subKey, name, type, value, (DWORD)value_size);
			//::RegFlushKey(hkey);
		}
		return ret;
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name, int value)
	{
		return SetKeyValue(hkey, subKey, name, REG_DWORD, (LPBYTE)& value, sizeof(value));
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name, uint32_t value)
	{
		return SetKeyValue(hkey, subKey, name, REG_DWORD, (LPBYTE)& value, sizeof(value));
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name, uint64_t value)
	{
		return SetKeyValue(hkey, subKey, name, REG_QWORD, (LPBYTE)& value, sizeof(value));
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name,
							 const wchar_t* value, size_t length, bool expand)
	{
		return SetKeyValue(hkey, subKey, name,
								   expand ? REG_EXPAND_SZ : REG_SZ,
									   (LPBYTE)value, length * sizeof(wchar_t));
	}

	bool Registry::SetKeyValue(HKEY hkey, const wchar_t* subKey, const wchar_t* name, const wchar_t* value, bool expand)
	{
		return SetKeyValue(hkey, subKey, name, value, string::Length(value), expand);
	}

	bool Registry::Exists(HKEY hKeyRoot, const wchar_t* subKey, uint32_t view)
	{
		bool result = false;
		if(is_valid(hKeyRoot, subKey))
		{
			HKEY hKey{};
			// Try open an existing key.
			result = ERROR_SUCCESS == ::RegOpenKeyExW(hKeyRoot, subKey, 0, KEY_READ | view, &hKey);
			if(hKey)
				::RegCloseKey(hKey);
		}
		return result;
	}

	bool Registry::ExistsValue(HKEY hKeyRoot, const wchar_t* subKey, const wchar_t* name, uint32_t view)
	{
		bool result = false;
		if(is_valid(hKeyRoot, subKey) && name && name[0])
		{
			HKEY hKey{};
			if(ERROR_SUCCESS == ::RegOpenKeyExW(hKeyRoot, subKey, 0, KEY_QUERY_VALUE | view, &hKey))
			{
				result = ERROR_SUCCESS == ::RegQueryValueExW(hKey, name, nullptr, nullptr, nullptr, nullptr);
				if(hKey)
					::RegCloseKey(hKey);
			}
		}
		return result;
	}

	bool Registry::DeleteSubKey(HKEY hKeyRoot, const wchar_t* subKey)
	{
		return ERROR_SUCCESS == ::RegDeleteTreeW(hKeyRoot, subKey);
	}
}