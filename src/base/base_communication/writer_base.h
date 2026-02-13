#ifndef WRITER_BASE_H
#define WRITER_BASE_H

#include <iostream>
#include <common/namespace_macros.h>

PUPPET_MASTER_BASE_NS_BEGIN

class WriterBase
{
public:
    WriterBase(const WriterBase&) = delete;
    WriterBase& operator=(const WriterBase&) = delete;
    
    virtual int Write(void* data, size_t len){ return -1;}

protected:
    WriterBase() = default;
    ~WriterBase() = default;

    WriterBase(WriterBase&&) = default;
    WriterBase& operator=(WriterBase&&) = default;
};

PUPPET_MASTER_BASE_NS_END

#endif  //  WRITER_BASE_H