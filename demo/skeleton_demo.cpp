#include <iostream>

#include <puppet_master/puppet_master.h>

int main()
{
    std::cout << puppet_master::ProjectName() << " "
              << puppet_master::Version() << '\n';
    return 0;
}
