#ifndef UNBOUNDED_QUEUE_H
#define UNBOUNDED_QUEUE_H

#include "bounded_queue.h"

namespace puppet_master 
{
namespace base 
{

template <typename T>
class UnBoundedQueue : public BoundedQueue<T>
{
public:
    UnBoundedQueue() : BoundedQueue<T>(0){}
};

}
}
#endif