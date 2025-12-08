#ifndef BASE_QUEUE_H
#define BASE_QUEUE_H

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <limits>

namespace puppet_master 
{
namespace base 
{

class BaseQueue
{
public:
    BaseQueue() = default;
    virtual ~BaseQueue() = default;
    virtual bool Enqueue(const void* element) = 0;
    virtual bool Dequeue(void* element, int64_t& time_diff) = 0;
    virtual bool Dequeue(void* element) final
    {
        int64_t time_diff = 0;
        return Dequeue(element, time_diff);
    }

};

}
}

#endif