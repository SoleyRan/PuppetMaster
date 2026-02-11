#ifndef WRITER_BASE_H
#define WRITER_BASE_H

#include <iostream>

namespace puppet_master
{
namespace base
{

class WriterBase
{
public:
    virtual int write(void* data, size_t len){ return -1;}
};

}   //base
}   //puppet_master

#endif  //  WRITER_BASE_H