#ifndef UNBOUNDED_QUEUE_H
#define UNBOUNDED_QUEUE_H

#include "bounded_queue.h"
#include <common/namespace_macros.h>

PUPPET_MASTER_UTILS_NS_BEGIN

template <typename T>
class UnBoundedQueue : public BoundedQueue<T>
{
public:
    UnBoundedQueue() : BoundedQueue<T>(0){}
};

PUPPET_MASTER_UTILS_NS_END

#endif