#ifndef TIME_STAMP_HPP
#define TIME_STAMP_HPP

#include <chrono>  
#include <common/namespace_macros.h>

PUPPET_MASTER_UTILS_NS_BEGIN

class TimeStamp
{
public:

    static uint64_t NanosecondsSinceEpoch()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    }

    static uint64_t MicrosecondsSinceEpoch()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    }

    static uint64_t MillisecondsSinceEpoch()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    }

    static uint64_t SecondsSinceEpoch()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    }
};

PUPPET_MASTER_UTILS_NS_END

#endif