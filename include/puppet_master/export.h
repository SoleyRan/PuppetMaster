#pragma once

// Public symbol visibility for shared-library builds. Keeping this in the
// first skeleton branch avoids leaking compiler-specific annotations into
// future runtime, transport, and component APIs.
#if defined(PUPPET_MASTER_STATIC_DEFINE)
#    define PUPPET_MASTER_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#    if defined(PUPPET_MASTER_EXPORTS)
#        define PUPPET_MASTER_API __declspec(dllexport)
#    else
#        define PUPPET_MASTER_API __declspec(dllimport)
#    endif
#else
#    if defined(PUPPET_MASTER_EXPORTS)
#        define PUPPET_MASTER_API __attribute__((visibility("default")))
#    else
#        define PUPPET_MASTER_API
#    endif
#endif
