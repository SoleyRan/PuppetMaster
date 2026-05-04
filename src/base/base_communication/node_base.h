#ifndef NODE_BASE_H
#define NODE_BASE_H

#include <string>
#include <boost/shared_ptr.hpp>
#include <common/attributes.h>
#include <common/namespace_macros.h>
#include "reader_base.h"
#include "writer_base.h"

PUPPET_MASTER_BASE_NS_BEGIN

class NodeBase
{
public:
    NodeBase(const NodeBase&) = delete;
    NodeBase& operator=(const NodeBase&) = delete;

    // // 如果之后有新的通信方式在创建节点时需要初始化参数，可以修改init函数
    // virtual void init() = 0;

    virtual std::shared_ptr<WriterBase> CreateWriter(std::string topic_name, void* data, void* attribute) = 0;
    virtual std::shared_ptr<ReaderBase> CreateReader(std::string topic_name, void* data, void* attribute) = 0;

protected:
    NodeBase() = default;
    ~NodeBase() = default;

    NodeBase(NodeBase&&) = default;
    NodeBase& operator=(NodeBase&&) = default;
};

PUPPET_MASTER_BASE_NS_END

#endif  //  COMM_BASE_H