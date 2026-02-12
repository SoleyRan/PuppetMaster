#ifndef READER_BASE_H
#define READER_BASE_H

#include <iostream>
#include <functional>
#include <common/namespace_macros.h>

PUPPET_MASTER_BASE_NS_BEGIN

class ReaderBase
{
public:
    virtual void set_callBack(const std::function<void()> &_func){};

    // 对dds来说，默认只取最新的，可以修改成每次只取一个，然后返回值如果是-1，说明没有数据了，是0说明有数据，大于0说明有数据并且是数据长度
    // 对zmq来说，返回的是数据长度
    virtual int read(void* data, int64_t& time_diff){return -1;}
protected:
    int64_t last_time_ {0}; // 上次读取的时间戳
};

PUPPET_MASTER_BASE_NS_END

#endif