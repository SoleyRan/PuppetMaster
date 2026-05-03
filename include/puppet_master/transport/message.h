#pragma once

#include <cstddef>
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

struct MessageDescriptor {
    std::string type_name;
    std::string encoding {"application/octet-stream"};

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
