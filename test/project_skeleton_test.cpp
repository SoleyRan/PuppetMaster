#include <cassert>
#include <cstring>

#include <puppet_master/puppet_master.h>

int main()
{
    assert(std::strcmp(puppet_master::ProjectName(), "PuppetMaster") == 0);
    assert(puppet_master::Version()[0] != '\0');
    assert(puppet_master::kVersionMajor == 0);

    puppet_master::core::Status ok;
    assert(ok.ok());

    const auto invalid =
        puppet_master::core::Status::InvalidArgument("missing topic name");
    assert(!invalid.ok());
    assert(invalid.code() == puppet_master::core::StatusCode::kInvalidArgument);
    assert(!invalid.message().empty());

    return 0;
}
