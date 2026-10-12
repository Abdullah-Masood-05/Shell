#pragma once

// A single background thread that runs tasks in order. Results go back to the UI
// with slint::invoke_from_event_loop; nothing here touches Slint objects.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class Worker
{
public:
    using Task = std::function<void()>;

    Worker();
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void post(Task task);

private:
    void run(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Task> queue_;
    std::jthread thread_; // last: stopped and joined before the queue goes away
};
