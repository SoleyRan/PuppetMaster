#ifndef COMM_BASE_H
#define COMM_BASE_H

#include <string>
#include <common/attributes.h>
#include <memory>
#include "reader_base.h"
#include "writer_base.h"

namespace puppet_master
{
namespace base
{

class CommBase
{
public:
    CommBase() = default;
    // 如果之后有新的通信方式在创建节点时需要初始化参数，可以修改init函数
    virtual void init() = 0;

    virtual std::shared_ptr<WriterBase> create_writer(std::string topic_name, void* data, void* attribute) = 0;
    virtual std::shared_ptr<ReaderBase> create_reader(std::string topic_name, void* data, void* attribute) = 0;
};

}   //base
}   //puppet_master

#endif  //  COMM_BASE_H