#ifndef COMMON_MACROS_H
#define COMMON_MACROS_H

#define puppet_unlikely(x) __builtin_expect(!!(x), 0)
#define puppet_likely(x) __builtin_expect(!!(x), 1)

#define DEFAULT_DOMAIN_ID 8


#endif // COMMON_MACROS_H