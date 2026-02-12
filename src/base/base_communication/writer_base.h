#ifndef WRITER_BASE_H
#define WRITER_BASE_H

#include <iostream>
#include <common/namespace_macros.h>

PUPPET_MASTER_BASE_NS_BEGIN

class WriterBase
{
public:
    virtual int write(void* data, size_t len){ return -1;}
};

PUPPET_MASTER_BASE_NS_END

#endif  //  WRITER_BASE_H