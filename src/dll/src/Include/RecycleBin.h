#pragma once

// Empties the Recycle Bin for command.empty_recycle_bin (issue #9).
//
// The System > Empty Recycle Bin item used to run PowerShell's Clear-RecycleBin
// with -ErrorAction SilentlyContinue. That makes one SHEmptyRecycleBin call for
// all drives at once, and when it fails (a drive whose bin can't be read, an
// item in use) the failure was hidden and nothing told the user. Here the call
// is made directly, with Windows' own progress window, and when the all-drives
// call fails each drive is emptied on its own so one bad drive no longer stops
// the rest. Every result is reported back so the caller can log it.
//
// empty_with() takes the Windows calls as parameters so it can be tested with
// stubs; empty() passes the real ones.

#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		struct RecycleBin
		{
			struct Step
			{
				wchar_t root[4] = {};	// empty for "all drives"
				HRESULT hr = S_OK;
			};

			struct Result
			{
				bool was_empty = false;	// nothing to do, nothing was called
				bool ok = false;		// every call that ran succeeded
				std::vector<Step> steps;
			};

			// Windows returns E_UNEXPECTED from SHEmptyRecycleBin when the bin
			// (or that drive's bin) has nothing in it. That is not a failure.
			static bool succeeded(HRESULT hr) { return hr == S_OK || hr == E_UNEXPECTED; }

			// Query(root, SHQUERYRBINFO*) -> HRESULT, Empty(root, flags) -> HRESULT,
			// Drives() -> std::vector<wchar_t> of drive letters to try one by one.
			template<class Query, class Empty, class Drives>
			static Result empty_with(Query query, Empty empty, Drives drives, DWORD flags)
			{
				Result r;

				SHQUERYRBINFO info{ sizeof(SHQUERYRBINFO) };
				// Only a successful query that says "nothing there" skips the call.
				// If the query itself fails, try to empty anyway.
				if(SUCCEEDED(query(nullptr, &info)) && info.i64NumItems == 0 && info.i64Size == 0)
				{
					r.was_empty = true;
					r.ok = true;
					return r;
				}

				Step all;
				all.hr = empty(nullptr, flags);
				r.steps.push_back(all);
				if(succeeded(all.hr))
				{
					r.ok = true;
					return r;
				}

				// The all-drives call stopped part way or did nothing; go drive by drive.
				r.ok = true;
				for(auto letter : drives())
				{
					Step s;
					s.root[0] = letter;
					s.root[1] = L':';
					s.root[2] = L'\\';
					SHQUERYRBINFO di{ sizeof(SHQUERYRBINFO) };
					if(SUCCEEDED(query(s.root, &di)) && di.i64NumItems == 0 && di.i64Size == 0)
						continue;
					s.hr = empty(s.root, flags);
					if(!succeeded(s.hr))
						r.ok = false;
					r.steps.push_back(s);
				}

				// Whatever is left decides: if the bin is empty now, it worked.
				SHQUERYRBINFO after{ sizeof(SHQUERYRBINFO) };
				if(SUCCEEDED(query(nullptr, &after)) && after.i64NumItems == 0 && after.i64Size == 0)
					r.ok = true;
				return r;
			}

			// Local fixed and removable drives: the ones that can have a Recycle Bin.
			static std::vector<wchar_t> local_drives()
			{
				std::vector<wchar_t> list;
				auto mask = ::GetLogicalDrives();
				for(int i = 0; i < 26; i++)
				{
					if(!(mask & (1u << i)))
						continue;
					wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', 0 };
					auto type = ::GetDriveTypeW(root);
					if(type == DRIVE_FIXED || type == DRIVE_REMOVABLE)
						list.push_back(root[0]);
				}
				return list;
			}

			// Our own item asks first, so Windows' confirmation is turned off.
			// Its progress window and sound stay, as when Explorer empties the bin.
			static Result empty(HWND owner)
			{
				return empty_with(
					[](const wchar_t *root, SHQUERYRBINFO *info) { return ::SHQueryRecycleBinW(root, info); },
					[owner](const wchar_t *root, DWORD flags) { return ::SHEmptyRecycleBinW(owner, root, flags); },
					local_drives, SHERB_NOCONFIRMATION);
			}
		};
	}
}
