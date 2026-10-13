#pragma once

// Whether the popup being tracked right now started from the taskbar.
//
// This was a namespace-scope `bool is_in_taskbar` in Main.cpp, written by
// ShowTaskbarContextMenu on one thread and read by the TrackPopupMenu hook on
// whichever thread happened to be tracking a popup. Explorer raises menus from
// more than one UI thread - the taskbar has its own - so that is a data race in
// the plain C++ sense, and worse than that it is semantically wrong: "this
// popup came from the taskbar" is a property of one popup on one thread, not of
// the process.
//
// It also leaked. The flag was set in ShowTaskbarContextMenu and cleared in the
// __finally of NtUserTrackPopupMenu, but one of the two branches invokes the
// saved native target directly and never enters the hook, so the flag stayed
// true until the next tracked popup - which then behaved as though it had come
// from the taskbar when it had not.
//
// Thread-local plus a scope that restores what it found: nesting works, the
// branch that bypasses the hook cannot leak, and no thread can see another
// thread's answer.

namespace Nilesoft
{
	namespace Shell
	{
		namespace detail
		{
			inline thread_local bool taskbar_origin = false;
		}

		// True while this thread is inside a popup that started at the taskbar.
		inline bool in_taskbar() noexcept
		{
			return detail::taskbar_origin;
		}

		class ScopedTaskbarOrigin
		{
		public:
			explicit ScopedTaskbarOrigin(bool value) noexcept
				: _previous(detail::taskbar_origin)
			{
				detail::taskbar_origin = value;
			}

			~ScopedTaskbarOrigin() noexcept
			{
				detail::taskbar_origin = _previous;
			}

			ScopedTaskbarOrigin(const ScopedTaskbarOrigin &) = delete;
			ScopedTaskbarOrigin &operator=(const ScopedTaskbarOrigin &) = delete;

		private:
			bool _previous;
		};
	}
}
