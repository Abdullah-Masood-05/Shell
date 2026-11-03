#pragma once

// Runs a job on the thread that owns a window.
//
// Commands run on a worker thread, but Explorer's folder views and shell browsers
// belong to the thread of their window: the IShellBrowser Shell keeps is a raw
// pointer read on that thread, never marshalled, so a call through it from the
// worker runs Explorer's view code on the wrong thread (issue #8: io.dir.create
// starting a rename from the worker made Explorer hang and restart).
//
// A WH_GETMESSAGE hook on the window's thread picks up a message posted to the
// window and runs the job there. The window's thread never waits for the caller,
// so neither function can stall it.
//
//   run()  waits for the job, at most `timeout` ms; when it times out the job is
//          dropped, never run late.
//   post() returns at once; a short-lived helper thread owns the hook and the job
//          (a hook goes away with the thread that set it, so the command worker,
//          which may end right after the command, can't own it).

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <thread>

namespace Nilesoft
{
	namespace Shell
	{
		namespace on_window_thread
		{
			struct job_t
			{
				std::function<void()> fn;
				HANDLE done{};
				// 0 queued, 1 taken by the window's thread, 2 dropped by the caller.
				std::atomic<long> state{ 0 };
			};

			inline UINT message()
			{
				static UINT msg = ::RegisterWindowMessageW(L"NilesoftShell.RunOnWindowThread");
				return msg;
			}

			// The jobs that are posted and not yet freed. The message can be posted by any
			// program, so the hook only takes a pointer it finds here.
			inline std::mutex &jobs_lock()
			{
				static std::mutex lock;
				return lock;
			}

			inline std::unordered_set<job_t *> &jobs()
			{
				static std::unordered_set<job_t *> set;
				return set;
			}

			inline bool is_job(job_t *job)
			{
				std::lock_guard<std::mutex> guard(jobs_lock());
				return jobs().count(job) != 0;
			}

			inline void free_job(job_t *job)
			{
				{
					std::lock_guard<std::mutex> guard(jobs_lock());
					jobs().erase(job);
				}
				if(job->done)
					::CloseHandle(job->done);
				delete job;
			}

			inline void call(std::function<void()> &fn)
			{
				try { fn(); }
				catch(...) {}
			}

			// The job runs inside Explorer's message loop: a fault in it must not take
			// the window down (on the command thread it was caught by ContextMenu::Invoke).
			inline void call_guarded(std::function<void()> &fn)
			{
				__try { call(fn); }
				__except(EXCEPTION_EXECUTE_HANDLER) {}
			}

			inline LRESULT CALLBACK hook(int code, WPARAM wParam, LPARAM lParam)
			{
				auto msg = reinterpret_cast<MSG *>(lParam);
				if(code == HC_ACTION && wParam == PM_REMOVE && msg && msg->message == message() && msg->lParam)
				{
					auto job = reinterpret_cast<job_t *>(msg->lParam);
					if(!is_job(job))
						return ::CallNextHookEx(nullptr, code, wParam, lParam);
					msg->message = WM_NULL;
					msg->lParam = 0;
					long queued = 0;
					if(job->state.compare_exchange_strong(queued, 1))
					{
						call_guarded(job->fn);
						::SetEvent(job->done);
					}
					else
					{
						// The caller gave up on it: the message owns it now.
						free_job(job);
					}
				}
				return ::CallNextHookEx(nullptr, code, wParam, lParam);
			}

			inline bool run(HWND hwnd, std::function<void()> fn, DWORD timeout = 5000)
			{
				auto tid = hwnd ? ::GetWindowThreadProcessId(hwnd, nullptr) : 0;
				if(!tid)
					return false;
				if(tid == ::GetCurrentThreadId())
				{
					call_guarded(fn);
					return true;
				}

				auto job = new job_t;
				job->fn = std::move(fn);
				job->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if(!job->done)
				{
					free_job(job);
					return false;
				}

				try
				{
					std::lock_guard<std::mutex> guard(jobs_lock());
					jobs().insert(job);
				}
				catch(...)
				{
					free_job(job);
					return false;
				}

				auto hk = ::SetWindowsHookExW(WH_GETMESSAGE, hook, nullptr, tid);
				if(!hk)
				{
					free_job(job);
					return false;
				}

				if(!::PostMessageW(hwnd, message(), 0, reinterpret_cast<LPARAM>(job)))
				{
					::UnhookWindowsHookEx(hk);
					free_job(job);
					return false;
				}

				bool ok = ::WaitForSingleObject(job->done, timeout) == WAIT_OBJECT_0;
				bool owned = true;
				if(!ok)
				{
					long queued = 0;
					if(job->state.compare_exchange_strong(queued, 2))
						owned = false; // still queued: dropped, freed by whoever takes the message
					else
						ok = ::WaitForSingleObject(job->done, INFINITE) == WAIT_OBJECT_0; // running now
				}

				::UnhookWindowsHookEx(hk);
				if(owned)
					free_job(job);
				return ok;
			}

			inline bool post(HWND hwnd, std::function<void()> fn, DWORD timeout = 5000)
			{
				auto tid = hwnd ? ::GetWindowThreadProcessId(hwnd, nullptr) : 0;
				if(!tid)
					return false;
				try
				{
					std::thread([hwnd, timeout, fn = std::move(fn)]() mutable
					{
						run(hwnd, std::move(fn), timeout);
					}).detach();
					return true;
				}
				catch(...)
				{
					return false;
				}
			}
		}
	}
}
