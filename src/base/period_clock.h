# ifndef PERIOD_CLOCK_H
# define PERIOD_CLOCK_H

#include <time.h>
#include <iostream>
#include <chrono>
#include <sys/syscall.h>
#include <unistd.h> 
#include <signal.h>
#include <cstring>
#include <logger/log.hpp>

namespace puppet_master
{
namespace base
{

class PeriodClock
{
public:
    PeriodClock() = default;

    void CreatTime(int offset) noexcept;
    void CreatTime(int offset, int data) noexcept;

    void Arm(timespec period, timespec interval) noexcept;

    void DisArm() noexcept;
    
    bool IsActive() noexcept;

    timespec GetTimeRemained() noexcept;

    void DeleteTime() noexcept;

private:
    timer_t m_timer;
    struct itimerspec m_its;
};

}
}

# endif
