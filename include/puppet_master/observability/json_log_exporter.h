#pragma once

#include <string>

#include <puppet_master/export.h>
#include <puppet_master/observability/observability.h>

namespace puppet_master::observability {

class PUPPET_MASTER_API JsonLogExporter final {
public:
    // 输出单个 JSON 对象；fields 数组保留字段顺序和重复键，不附加换行。
    static std::string Render(const LogRecord& record);
};

}  // namespace puppet_master::observability
