#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gow3::renderer {
// Borrowed only during one synchronous planner call. The shader's persistent
// identity remains SHA-256 of header || code; this fingerprint is merely a
// cache hint and must always be followed by exact comparison of both segments.
class ShaderByteSegments {
public:
    ShaderByteSegments(std::span<const uint8_t> header,std::span<const uint8_t> code)
        :header_(header),code_(code) {
        if(code.size()>std::numeric_limits<size_t>::max()-header.size())
            throw std::runtime_error("Shader byte extent overflow");
    }
    size_t size() const {return header_.size()+code_.size();}
    template<class Hash> size_t FingerprintWith(const Hash& hash) const {
        const auto first=hash(header_);
        return first^(hash(code_)+size_t(0x9e3779b9u)+(first<<6)+(first>>2));
    }
    size_t Fingerprint() const {
        return FingerprintWith([](std::span<const uint8_t> bytes) {
            const auto view=bytes.empty()?std::string_view{}:
                std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            return std::hash<std::string_view>{}(view);
        });
    }
    bool Matches(std::span<const uint8_t> bytes) const {
        return bytes.size()==size()&&
            (header_.empty()||std::equal(header_.begin(),header_.end(),bytes.begin()))&&
            (code_.empty()||std::equal(code_.begin(),code_.end(),bytes.subspan(header_.size()).begin()));
    }
    std::shared_ptr<const std::vector<uint8_t>> Snapshot() const {
        auto bytes=std::make_shared<std::vector<uint8_t>>();
        bytes->reserve(size());
        if(!header_.empty())bytes->insert(bytes->end(),header_.begin(),header_.end());
        if(!code_.empty())bytes->insert(bytes->end(),code_.begin(),code_.end());
        return bytes;
    }
private:
    std::span<const uint8_t> header_,code_;
};
}
