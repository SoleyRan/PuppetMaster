#include "period_clock.h"

namespace puppet_master
{
namespace tools
{

void PeriodClock::CreatTime(int offset) noexcept 
{
    ::sigevent m_sigEvent;
    m_sigEvent.sigev_value.sival_ptr = &m_timer; 
    m_sigEvent.sigev_notify = SIGEV_THREAD_ID; 
    m_sigEvent.sigev_signo = SIGRTMIN;
    m_sigEvent._sigev_un._tid = syscall(SYS_gettid);
    if(timer_create(CLOCK_MONOTONIC , &m_sigEvent, &m_timer) == -1)
    {
        LOG_Warn()<< "[error] timer_create error" << strerror(errno) ;
    }
    return;
}

// 在注册time时，接收信号的线程就是注册的线程
void PeriodClock::CreatTime(int offset, int data) noexcept 
{
    ::sigevent m_sigEvent;
    m_sigEvent.sigev_notify = SIGEV_THREAD_ID; 
    m_sigEvent.sigev_signo = SIGRTMIN;
    m_sigEvent.sigev_value.sival_int = data;
    m_sigEvent._sigev_un._tid = syscall(SYS_gettid);
    if(timer_create(CLOCK_MONOTONIC , &m_sigEvent, &m_timer) == -1)
    {
        LOG_Warn()<< "[error] timer create error : " << strerror(errno) ;
    }
    return;
}

void PeriodClock::Arm(timespec period,  timespec interval) noexcept 
{
    m_its.it_value = period;
    m_its.it_interval = interval;
    if(timer_settime(m_timer, 0 , &m_its, NULL) == -1)
    {
        LOG_Warn()<< "[error] timer arm error : " << strerror(errno) ;
    }
    return;
}

void PeriodClock::DisArm() noexcept 
{
    m_its.it_value.tv_sec = 0;
    m_its.it_value.tv_nsec = 0;
    if(timer_settime(m_timer, 0 , &m_its, NULL) == -1)
    {
        LOG_Warn()<< "[error] timer disarm error" << strerror(errno) ;
    }
    return;
}

bool PeriodClock::IsActive() noexcept 
{
    if(timer_gettime(m_timer, &m_its) == -1)
    {
        LOG_Warn()<< "[error] timer get time error" << strerror(errno) ;
    }
    return m_its.it_value.tv_sec != 0 || m_its.it_value.tv_nsec != 0;
}

timespec PeriodClock::GetTimeRemained() noexcept 
{
    if(timer_gettime(m_timer, &m_its) == -1)
    {
        LOG_Warn()<< "[error] timer get time error" << strerror(errno) ;
    }
    return m_its.it_value;
}

void PeriodClock::DeleteTime() noexcept
{
    if(timer_delete(m_timer) == -1)
    {
        LOG_Warn()<< "[error] timer delete error" << strerror(errno) ;
    }
    return;
}

}
}