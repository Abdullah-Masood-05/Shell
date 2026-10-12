#include "Worker.h"

#include <windows.h>
#include <objbase.h>

Worker::Worker() : thread_([this](std::stop_token st) { run(st); }) {}

Worker::~Worker()
{
    thread_.request_stop();
    cv_.notify_all();
}

void Worker::post(Task task)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void Worker::run(std::stop_token stop)
{
    // ShellExecuteEx and the shell folder APIs want an STA.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return !queue_.empty(); }))
                break; // stop requested
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // A task reports its own errors; never let one take the thread down.
        }
    }
    if (SUCCEEDED(hr))
        CoUninitialize();
}
