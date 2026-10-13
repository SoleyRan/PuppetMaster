#pragma once

#include <string>

#include <puppet_master/export.h>
#include <puppet_master/observability/observability.h>

namespace puppet_master::observability {

class PUPPET_MASTER_API PrometheusExporter final {
public:
    // Render a snapshot using Prometheus text exposition format.
    static std::string Render(const MetricsSnapshot& snapshot);
};

}  // namespace puppet_master::observability
