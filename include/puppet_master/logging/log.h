#pragma once

#include <puppet_master/logging/logger.h>

// These compatibility macros intentionally live in an explicit header instead
// of puppet_master.h so projects can choose whether to introduce global names.
#ifndef LOG_Trace
#define LOG_Trace() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kTrace, __FILE__, __LINE__)
#endif

#ifndef LOG_Debug
#define LOG_Debug() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kDebug, __FILE__, __LINE__)
#endif

#ifndef LOG_Info
#define LOG_Info() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kInfo, __FILE__, __LINE__)
#endif

#ifndef LOG_Warn
#define LOG_Warn() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kWarning, __FILE__, __LINE__)
#endif

#ifndef LOG_Error
#define LOG_Error() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kError, __FILE__, __LINE__)
#endif

#ifndef LOG_Fatal
#define LOG_Fatal() \
    ::puppet_master::logging::LogStream( \
        ::puppet_master::observability::LogLevel::kFatal, __FILE__, __LINE__)
#endif

#ifndef LOG_DEBUG_HEX
#define LOG_DEBUG_HEX(data, size, text) \
    LOG_Debug() << text << " Data[" \
                << ::puppet_master::logging::HexDump(data, size) << ']'
#endif
