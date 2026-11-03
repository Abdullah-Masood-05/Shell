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
//          dropped and freed, never run late.
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
				DWORD tid{};    // the window's thread: the only one that may take the job
				WPARAM id{};    // posted with the pointer, so a stale message never matches a new job
				bool taken{};   // set under jobs_lock() by the hook that runs it
			};

			inline UINT message()
			{
				static UINT msg = ::RegisterWindowMessageW(L"NilesoftShell.RunOnWindowThread");
				return msg;
			}

			// The jobs that are posted and still wanted. The message can be posted by any
			// program, and a dropped job's message can still be in a queue, so the hook only
			// takes a pointer it finds here with the same id, for its own thread. Taking a
			// job and dropping it both happen under this lock, so exactly one side wins and
			// the caller always frees the job.
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

			inline WPARAM next_id()
			{
				static std::atomic<WPARAM> id{ 0 };
				return ++id;
			}

			inline void free_job(job_t *job)
			{
				if(job->done)
					::CloseHandle(job->done);
				delete job;
			}

			// Only C++ exceptions are caught: a fault inside Explorer's own view code goes to
			// Windows' crash reporting as usual, rather than leaving Explorer running with
			// a lock that code may hold.
			inline int cpp_only(unsigned long code)
			{
				return code == 0xE06D7363 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
			}

			inline void call(std::function<void()> &fn)
			{
				__try { fn(); }
				__except(cpp_only(::GetExceptionCode())) {}
			}

			inline LRESULT CALLBACK hook(int code, WPARAM wParam, LPARAM lParam)
			{
				auto msg = reinterpret_cast<MSG *>(lParam);
				if(code == HC_ACTION && wParam == PM_REMOVE && msg && msg->message == message() && msg->lParam)
				{
					auto job = reinterpret_cast<job_t *>(msg->lParam);
					bool mine = false;
					{
						std::lock_guard<std::mutex> guard(jobs_lock());
						if(jobs().count(job) && job->id == msg->wParam && job->tid == ::GetCurrentThreadId() && !job->taken)
						{
							job->taken = true;
							jobs().erase(job);
							mine = true;
						}
					}
					if(mine)
					{
						msg->message = WM_NULL;
						msg->lParam = 0;
						call(job->fn);
						::SetEvent(job->done);
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
					call(fn);
					return true;
				}

				auto job = new job_t;
				job->fn = std::move(fn);
				job->tid = tid;
				job->id = next_id();
				job->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if(!job->done)
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

				try
				{
					std::lock_guard<std::mutex> guard(jobs_lock());
					jobs().insert(job);
				}
				catch(...)
				{
					::UnhookWindowsHookEx(hk);
					free_job(job);
					return false;
				}

				bool ok = ::PostMessageW(hwnd, message(), job->id, reinterpret_cast<LPARAM>(job)) &&
					::WaitForSingleObject(job->done, timeout) == WAIT_OBJECT_0;
				if(!ok)
				{
					bool dropped;
					{
						std::lock_guard<std::mutex> guard(jobs_lock());
						dropped = !job->taken;
						jobs().erase(job);
					}
					// Not taken yet: dropped, never run late (its message, if still queued,
					// no longer matches anything). Taken: it is running now, so wait for it.
					if(!dropped)
						ok = ::WaitForSingleObject(job->done, INFINITE) == WAIT_OBJECT_0;
				}

				::UnhookWindowsHookEx(hk);
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
