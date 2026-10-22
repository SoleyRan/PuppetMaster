#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>

namespace puppet_master::transport {

using ByteBuffer = std::vector<core::Byte>;

class ByteView {
public:
    ByteView() = default;

    ByteView(const core::Byte* data, std::size_t size)
        : data_(data), size_(size)
    {
    }

    explicit ByteView(const ByteBuffer& buffer)
        : data_(buffer.data()), size_(buffer.size())
    {
    }

    static ByteView From(const void* data, std::size_t size)
    {
        return ByteView(static_cast<const core::Byte*>(data), size);
    }

    const core::Byte* data() const noexcept
    {
        return data_;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    const core::Byte* begin() const noexcept
    {
        return data_;
    }

    const core::Byte* end() const noexcept
    {
        return data_ == nullptr ? nullptr : data_ + size_;
    }

    core::Status Validate() const
    {
        if (data_ == nullptr && size_ != 0) {
            return core::Status::InvalidArgument("byte view data is null but size is non-zero");
        }
        return core::Status::Ok();
    }

private:
    const core::Byte* data_ {nullptr};
    std::size_t size_ {0};
};

class MutableByteView {
public:
    MutableByteView() = default;

    MutableByteView(core::Byte* data, std::size_t size)
        : data_(data), size_(size)
    {
    }

    core::Byte* data() const noexcept
    {
        return data_;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    core::Status Validate() const
    {
        if (data_ == nullptr && size_ != 0) {
            return core::Status::InvalidArgument("mutable byte view data is null but size is non-zero");
        }
        return core::Status::Ok();
    }

private:
    core::Byte* data_ {nullptr};
    std::size_t size_ {0};
};

// Identifies what a topic carries. Endpoints on one topic must agree on all
// three fields; every transport rejects a mismatch when the endpoint is
// created, including across processes.
struct MessageDescriptor {
    std::string type_name;
    std::string encoding {"application/octet-stream"};
    // Wire format version of type_name. Raise it on every change that old
    // readers cannot decode, such as reordered fields or changed units, so
    // separately built processes refuse each other instead of misreading.
    // 0 means unversioned.
    std::uint32_t version {0};

    core::Status Validate() const
    {
        if (type_name.empty()) {
            return core::Status::InvalidArgument("message type name must not be empty");
        }
        if (encoding.empty()) {
            return core::Status::InvalidArgument("message encoding must not be empty");
        }
        return core::Status::Ok();
    }

    // "type_name v<version> (encoding)", with the version left out when 0.
    std::string ToString() const
    {
        std::string text = type_name;
        if (version != 0) {
            text += " v" + std::to_string(version);
        }
        return text + " (" + encoding + ")";
    }

    friend bool operator==(const MessageDescriptor& lhs, const MessageDescriptor& rhs)
    {
        return lhs.type_name == rhs.type_name && lhs.encoding == rhs.encoding && lhs.version == rhs.version;
    }

    friend bool operator!=(const MessageDescriptor& lhs, const MessageDescriptor& rhs) { return !(lhs == rhs); }
};

struct MessageMetadata {
    core::SequenceNumber sequence {0};
    core::TimePoint source_timestamp {};
    core::TimePoint reception_timestamp {};
};

struct Message {
    ByteBuffer payload;
    MessageMetadata metadata;
};

inline ByteBuffer CopyBytes(ByteView view)
{
    if (view.empty() || view.data() == nullptr) {
        return {};
    }

    return ByteBuffer(view.begin(), view.end());
}

}  // namespace puppet_master::transport
