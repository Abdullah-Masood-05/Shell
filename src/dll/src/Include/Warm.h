#pragma once

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Preload and keep-alive of Windows' own context menu handlers.

			Most of the wait before a menu is Windows creating the handlers
			registered for the selection and asking them for their items. The
			first menu in an Explorer process loads every handler DLL, and
			packaged handlers (Windows Terminal's "Open in Terminal") run in a
			DllHost surrogate that exits soon after each menu closes.

			settings.keepalive keeps what a menu created: a reference on each
			in-process handler's DLL, and IClassFactory::LockServer on each
			out-of-process handler's server. Each menu thread also creates
			out-of-process handlers from the class factory it kept, so it doesn't
			ask RPCSS for the running server again, which gets slower the longer
			Explorer runs. Turning keepalive off lets all of it go at the next
			menu.

			settings.preload builds (without showing) the menus of a file, a
			folder, a folder background and the desktop shortly after Explorer
			loads Shell, then of an empty file of each type in
			settings.preload.types (unset: jpg, png, pdf, docx, mp4, mkv, html
			and obj), whose own handlers the others don't load; with keepalive
			on, what it loads stays loaded. A preload that never finished keeps
			later ones from running until the config changes, so a handler that
			crashes Explorer there doesn't do so at every start.
		*/
		namespace Warm
		{
			// The CoCreateInstance hook created a context menu handler for a menu.
			void Created(REFCLSID clsid, DWORD context, IUnknown *object) noexcept;

			// keepalive: create an out-of-process handler from the class factory
			// this thread kept for it. True: handled, hr is the result. False: the
			// caller activates it as usual.
			bool CreateFromFactory(REFCLSID clsid, DWORD context, IUnknown *outer, REFIID riid, void **ppv, HRESULT &hr) noexcept;

			// Explorer only, once per process: start the preload thread.
			void StartPreload() noexcept;

			// True on the preload thread, whose menus are never shown.
			bool Preloading() noexcept;
		}
	}
}
