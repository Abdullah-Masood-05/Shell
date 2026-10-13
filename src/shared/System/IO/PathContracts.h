#pragma once

// The three buffer-sizing contracts the Win32 path functions actually use.
//
// They look alike from the call site and are not alike, which is how this tree
// ended up recording an allocation capacity as a string length in one wrapper,
// testing for a truncation signal that can never occur in another, and never
// retrying at all in five more. There is deliberately no single generic
// "dynamic string" helper here: each function below implements one documented
// return-value contract and says which, so a call site is wrong in an obvious
// way rather than a plausible one.
//
// Every helper hands back the length reported by the final successful call.
// None of them ever treats the size it asked for as the length it got.

namespace Nilesoft
{
	namespace IO
	{
		namespace Contracts
		{
			using Nilesoft::Text::string;

			// A path cannot exceed 32,767 characters even with the long-path
			// opt-in, so a retry loop that passes this is looping on something
			// other than a short buffer.
			//
			//   https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
			inline constexpr DWORD MaximumPath = 32768;

			// Contract A - preflight for the size, then fill.
			//
			// For GetFullPathNameW, GetLongPathNameW and GetShortPathNameW:
			//
			//     If the function succeeds, the return value is the length, in
			//     TCHARs, of the string copied to lpszLongPath, not including
			//     the terminating null character.
			//
			//     If the [...] buffer is too small to contain the path, the
			//     return value is the size, in TCHARs, of the buffer that is
			//     required to hold the path and the terminating null character.
			//
			//   https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew
			//   https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew
			//
			// The two returns mean different things and differ by exactly one,
			// which is why using the first as a string length is a mistake that
			// survives casual testing: the value is only ever wrong by a single
			// trailing NUL that most consumers then stop at anyway.
			//
			// `call(buffer, capacity)` invokes the API. A null buffer with zero
			// capacity is the documented sizing query for these three.
			template<typename Call>
			string preflight_then_fill(Call call)
			{
				DWORD required = call(nullptr, 0);
				if(required == 0 || required > MaximumPath)
					return {};

				// Bounded: the path can legitimately change between the sizing
				// call and the fill, but not indefinitely.
				for(int attempt = 0; attempt < 4; attempt++)
				{
					string result(static_cast<size_t>(required) + 1);

					DWORD written = call(result.buffer(required + 1), required);
					if(written == 0)
						return {};

					// written < required is success: required counted the NUL.
					if(written < required)
						return result.release(written).move();

					// Otherwise `written` is a fresh required size and the path
					// grew underneath us. Ask again with it.
					if(written > MaximumPath)
						return {};
					required = written;
				}

				return {};
			}

			// Contract B - grow while the result exactly fills the buffer.
			//
			// GetModuleFileNameW has no sizing query at all:
			//
			//     If the buffer is too small to hold the module name, the string
			//     is truncated to nSize characters including the terminating
			//     null character, the function returns nSize, and the function
			//     sets the last error to ERROR_INSUFFICIENT_BUFFER.
			//
			//   https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew
			//
			// So truncation is `returned == nSize`. A wrapper testing for
			// `returned > nSize` is testing for something the function is
			// documented never to do, and silently keeps the truncated path.
			//
			// The last-error check is deliberately not the retry condition:
			// Windows XP is documented not to set it, and a caller cannot tell
			// the two apart from the return value alone.
			template<typename Call>
			string grow_on_exact_fill(Call call, DWORD initial = MAX_PATH)
			{
				DWORD capacity = initial;
				while(capacity <= MaximumPath)
				{
					string result(static_cast<size_t>(capacity) + 1);

					DWORD written = call(result.buffer(capacity + 1), capacity);
					if(written == 0)
						return {};

					if(written < capacity)
						return result.release(written).move();

					capacity *= 2;
				}

				return {};
			}

			// Contract C - fill first, and the overflow return is a size.
			//
			// SearchPathW, GetCurrentDirectoryW and GetTempPathW answer a
			// correctly sized buffer with a length and an undersized one with a
			// capacity:
			//
			//     If the function succeeds, the value returned is the length, in
			//     TCHARs, of the string that is copied to the buffer, not
			//     including the terminating null character. If the return value
			//     is greater than nBufferLength, the value returned is the size
			//     of the buffer that is required to hold the path, including the
			//     terminating null character.
			//
			//   https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-searchpathw
			//
			// Note the boundary: here the overflow signal is strictly greater
			// than the capacity passed, not equal to it, because a result that
			// exactly fills the buffer minus its NUL is a success.
			template<typename Call>
			string fill_then_resize(Call call, DWORD initial = MAX_PATH)
			{
				DWORD capacity = initial;

				for(int attempt = 0; attempt < 4; attempt++)
				{
					string result(static_cast<size_t>(capacity) + 1);

					DWORD written = call(result.buffer(capacity + 1), capacity);
					if(written == 0)
						return {};

					if(written <= capacity)
						return result.release(written).move();

					if(written > MaximumPath)
						return {};
					capacity = written;
				}

				return {};
			}
		}
	}
}
