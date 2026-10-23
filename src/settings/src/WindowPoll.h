#pragma once

// A repeating timer for the pages' polls (status changes, the Windows theme), which
// only matter while the window is shown. While the app sits in the system tray with
// its window hidden they all stop, so the process isn't woken several times a second
// for nothing; showing the window starts them again. main.cpp says when
// (WindowPoll::setWindowShown). Same start/stop calls as slint::Timer.

#include <slint.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

class WindowPoll
{
public:
    WindowPoll() { all().push_back(this); }
    ~WindowPoll()
    {
        auto &v = all();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
    WindowPoll(const WindowPoll &) = delete;
    WindowPoll &operator=(const WindowPoll &) = delete;

    void start(slint::TimerMode, std::chrono::milliseconds interval, std::function<void()> callback)
    {
        interval_ = interval;
        callback_ = std::move(callback);
        wanted_ = true;
        if (shown())
            run();
    }
    void stop()
    {
        wanted_ = false;
        timer_.stop();
    }
    bool running() const { return wanted_; }

    // The window was shown (true) or hidden to the tray (false).
    static void setWindowShown(bool on)
    {
        shown() = on;
        for (WindowPoll *p : all()) {
            if (!p->wanted_)
                continue;
            if (on && !p->timer_.running())
                p->run();
            else if (!on)
                p->timer_.stop();
        }
    }

private:
    void run()
    {
        timer_.start(slint::TimerMode::Repeated, interval_, [this] {
            if (callback_)
                callback_();
        });
    }
    static std::vector<WindowPoll *> &all()
    {
        static std::vector<WindowPoll *> v;
        return v;
    }
    static bool &shown()
    {
        static bool on = true;
        return on;
    }

    slint::Timer timer_;
    std::chrono::milliseconds interval_{ 500 };
    std::function<void()> callback_;
    bool wanted_ = false;
};
