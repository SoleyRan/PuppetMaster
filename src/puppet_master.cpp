#include <puppet_master/puppet_master.h>

namespace puppet_master {

const char* ProjectName() noexcept
{
    return "PuppetMaster";
}

const char* Version() noexcept
{
    return kVersion;
}

}  // namespace puppet_master
