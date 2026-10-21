// A JSON-configured application in one main(): components only implement
// their logic, and the launcher owns runtime, scheduler, and lifecycle.
//
//     puppet_master_launcher_demo demo/config/speed_pipeline.json
//     puppet_master_launcher_demo --validate demo/config/speed_pipeline.json
//     puppet_master_launcher_demo --describe demo/config/speed_pipeline.json
//
// Runs until Ctrl+C. With PUPPETMASTER_DEMO_MAX_SAMPLES=<n> in the
// environment, it stops by itself after n samples (used by the test suite).

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/configuration/json_launcher.h>
#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/typed.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

struct VehicleSpeed {
    double meters_per_second;
    std::uint64_t sample;
};

template <>
struct puppet_master::transport::MessageTypeName<VehicleSpeed> {
    static constexpr const char* value = "demo.VehicleSpeed";
};

namespace {

class SpeedSensor final : public runtime::Component {
public:
    explicit SpeedSensor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto writer = transport::MakeTypedWriter<VehicleSpeed>(context.CreateWriter(spec_.writers.front()));
        if (!writer.ok()) {
            return writer.status();
        }
        writer_ = std::make_unique<transport::TypedWriter<VehicleSpeed>>(std::move(writer).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++sample_;
        return writer_->Write(VehicleSpeed {10.0 + static_cast<double>(sample_ % 5), sample_});
    }

private:
    runtime::ComponentSpec spec_;
    std::unique_ptr<transport::TypedWriter<VehicleSpeed>> writer_;
    std::uint64_t sample_ {0};
};

class SpeedMonitor final : public runtime::Component {
public:
    SpeedMonitor(runtime::ComponentSpec spec, std::uint64_t max_samples)
        : spec_(std::move(spec)), max_samples_(max_samples)
    {
    }

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto reader = transport::MakeTypedReader<VehicleSpeed>(context.CreateReader(spec_.readers.front()));
        if (!reader.ok()) {
            return reader.status();
        }
        reader_ = std::make_unique<transport::TypedReader<VehicleSpeed>>(std::move(reader).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        // Drain everything queued since the last trigger.
        while (true) {
            auto message = reader_->Read();
            if (!message.ok()) {
                break;
            }
            const auto& speed = message.value().value;
            std::cout << "sample " << speed.sample << ": " << speed.meters_per_second << " m/s\n";
            if (max_samples_ != 0 && ++seen_ == max_samples_) {
                std::raise(SIGINT);  // the launcher turns this into a clean shutdown
            }
        }
        return core::Status::Ok();
    }

private:
    runtime::ComponentSpec spec_;
    std::unique_ptr<transport::TypedReader<VehicleSpeed>> reader_;
    std::uint64_t max_samples_;
    std::uint64_t seen_ {0};
};

}  // namespace

std::uint64_t MaxSamplesFromEnvironment()
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "PUPPETMASTER_DEMO_MAX_SAMPLES") != 0 || value == nullptr) {
        return 0;
    }
    const auto limit = std::strtoull(value, nullptr, 10);
    std::free(value);
    return limit;
#else
    const char* value = std::getenv("PUPPETMASTER_DEMO_MAX_SAMPLES");
    return value == nullptr ? 0 : std::strtoull(value, nullptr, 10);
#endif
}

int main(int argc, char** argv)
{
    const std::uint64_t max_samples = MaxSamplesFromEnvironment();

    config::ComponentFactoryRegistry factories;
    factories.Register("speed_sensor", [](const runtime::ComponentSpec& spec) {
        return core::Result<runtime::ComponentPtr>(runtime::ComponentPtr(std::make_shared<SpeedSensor>(spec)));
    });
    factories.Register("speed_monitor", [max_samples](const runtime::ComponentSpec& spec) {
        return core::Result<runtime::ComponentPtr>(
            runtime::ComponentPtr(std::make_shared<SpeedMonitor>(spec, max_samples)));
    });
    return config::RunJsonApplication(argc, argv, factories);
}
