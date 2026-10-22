#pragma once

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>

namespace puppet_master::runtime {

// Named scalar settings for a component or transport, as written in its
// configuration. Values are kept as text and converted on access, so a
// configuration file can be loaded without knowing what each component expects.
using ParameterMap = std::map<std::string, std::string>;

namespace detail {

template <typename T>
core::Result<T> ConvertParameter(std::string_view key, const std::string& text)
{
    const auto invalid = [&key, &text](const char* expected) {
        return core::Result<T>::FromStatus(core::Status::InvalidArgument(
            "parameter " + std::string(key) + " must be " + expected + ", got \"" + text + "\""));
    };

    if constexpr (std::is_same_v<T, std::string>) {
        return core::Result<T>(text);
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "true") {
            return core::Result<T>(true);
        }
        if (text == "false") {
            return core::Result<T>(false);
        }
        return invalid("true or false");
    } else if constexpr (std::is_integral_v<T>) {
        if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            return invalid("an integer");
        }
        char* end = nullptr;
        errno = 0;
        if constexpr (std::is_signed_v<T>) {
            const long long value = std::strtoll(text.c_str(), &end, 10);
            if (end != text.c_str() + text.size() || errno == ERANGE
                || value < static_cast<long long>(std::numeric_limits<T>::min())
                || value > static_cast<long long>(std::numeric_limits<T>::max())) {
                return invalid("an integer in range");
            }
            return core::Result<T>(static_cast<T>(value));
        } else {
            if (text.front() == '-') {
                return invalid("a non-negative integer");
            }
            const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
            if (end != text.c_str() + text.size() || errno == ERANGE
                || value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
                return invalid("a non-negative integer in range");
            }
            return core::Result<T>(static_cast<T>(value));
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        // Parse in the classic locale, so "0.5" means the same everywhere.
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        T value {};
        stream >> std::noskipws >> value;
        if (text.empty() || stream.fail() || stream.peek() != std::char_traits<char>::eof()) {
            return invalid("a number");
        }
        return core::Result<T>(value);
    } else {
        static_assert(std::is_same_v<T, void>, "parameters convert to std::string, bool, integers, or floating point");
    }
}

}  // namespace detail

// Returns the parameter `key` converted to T, or `fallback` when it is not
// set. A value that does not convert is InvalidArgument naming the key.
template <typename T>
core::Result<T> GetParameter(const ParameterMap& parameters, std::string_view key, T fallback)
{
    const auto found = parameters.find(std::string(key));
    if (found == parameters.end()) {
        return core::Result<T>(std::move(fallback));
    }
    return detail::ConvertParameter<T>(key, found->second);
}

// const char* fallbacks read as strings.
inline core::Result<std::string> GetParameter(const ParameterMap& parameters, std::string_view key, const char* fallback)
{
    return GetParameter<std::string>(parameters, key, std::string(fallback));
}

// Like GetParameter, but a missing parameter is NotFound.
template <typename T>
core::Result<T> GetRequiredParameter(const ParameterMap& parameters, std::string_view key)
{
    const auto found = parameters.find(std::string(key));
    if (found == parameters.end()) {
        return core::Result<T>::FromStatus(core::Status::NotFound("required parameter is not set: " + std::string(key)));
    }
    return detail::ConvertParameter<T>(key, found->second);
}

}  // namespace puppet_master::runtime
