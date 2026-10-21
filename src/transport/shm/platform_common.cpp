#include "platform.h"

#include <cstdio>

namespace puppet_master::transport::shm::detail {

std::uint64_t Fnv1a64(std::string_view text) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string ObjectName(core::DomainId domain, std::string_view topic, std::string_view suffix)
{
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(Fnv1a64(topic)));

#ifdef _WIN32
    std::string name = "Local\\pm.";
#else
    std::string name = "/pm.";
#endif
    name += std::to_string(domain);
    name += '.';
    name += hash;
    if (!suffix.empty()) {
        name += '.';
        name += suffix;
    }
    return name;
}

}  // namespace puppet_master::transport::shm::detail
