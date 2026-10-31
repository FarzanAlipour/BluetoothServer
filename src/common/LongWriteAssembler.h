// LongWriteAssembler.h - offset-aware assembly of GATT writes (ATT Prepare Write / Execute Write).
//
// Platform independent and header only, so the rules are unit-tested once (tests/test_long_write.cpp)
// and shared by every backend:
//   - macOS: the peripheral manager hands over a whole Execute Write as one batch of write
//            requests with offsets -> AssembleBatch().
//   - Linux / Windows: the stack may deliver fragments one call at a time, each with an offset
//            -> ApplyFragment() against the stored value.
//
// Rules (ATT semantics, see Core Spec Vol 3 Part F 3.4.6):
//   - A fragment at `offset` keeps the first `offset` bytes of the value and replaces the rest with
//     its data, so in-order fragments concatenate and offset 0 replaces the whole value.
//   - offset > current length (a gap)            -> InvalidOffset.
//   - offset + length > kMaxAttributeLength (512) -> InvalidAttributeValueLength.
//   - A batch is all-or-nothing: on any error nothing is applied.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace BleServer {
namespace LongWrite {

    // Maximum length of an attribute value (Core Spec Vol 3 Part F 3.2.9).
    constexpr std::size_t kMaxAttributeLength = 512;

    enum class Status {
        Ok,
        InvalidOffset,                 // ATT 0x07
        InvalidAttributeValueLength,   // ATT 0x0D
        AttributeNotFound              // ATT 0x0A (unknown characteristic in a batch)
    };

    inline const char* ToString(Status s) {
        switch (s) {
            case Status::Ok: return "ok";
            case Status::InvalidOffset: return "invalid offset";
            case Status::InvalidAttributeValueLength: return "invalid attribute value length";
            case Status::AttributeNotFound: return "attribute not found";
        }
        return "unknown";
    }

    // Checks one fragment against a value of length `currentLength` without changing anything.
    inline Status CheckFragment(std::size_t currentLength, std::size_t offset, std::size_t length,
                                std::size_t maxLength = kMaxAttributeLength) {
        if (offset > currentLength || offset > maxLength) {
            return Status::InvalidOffset;
        }
        if (length > maxLength - offset) {
            return Status::InvalidAttributeValueLength;
        }
        return Status::Ok;
    }

    // Applies one fragment to `value` in place. On error `value` is unchanged.
    inline Status ApplyFragment(std::vector<uint8_t>& value, std::size_t offset, const uint8_t* data,
                                std::size_t length, std::size_t maxLength = kMaxAttributeLength) {
        const Status s = CheckFragment(value.size(), offset, length, maxLength);
        if (s != Status::Ok) {
            return s;
        }
        value.resize(offset);
        if (length > 0) {
            value.insert(value.end(), data, data + length);
        }
        return Status::Ok;
    }

    inline Status ApplyFragment(std::vector<uint8_t>& value, std::size_t offset,
                                const std::vector<uint8_t>& data,
                                std::size_t maxLength = kMaxAttributeLength) {
        return ApplyFragment(value, offset, data.data(), data.size(), maxLength);
    }

    // One request of a write batch, in the order the stack delivered it.
    struct Fragment {
        std::string key;               // identifies the characteristic (UUID, path, ...)
        std::size_t offset = 0;
        std::vector<uint8_t> data;
    };

    struct AssembledValue {
        std::string key;
        std::vector<uint8_t> value;    // full value after all fragments for `key`
    };

    struct BatchResult {
        Status status = Status::Ok;
        std::size_t failedIndex = 0;   // index into the input when status != Ok
        // One entry per characteristic, in first-appearance order. Empty when status != Ok.
        std::vector<AssembledValue> values;
    };

    // Assembles a batch of fragments that may target several characteristics.
    // `base(key, out)` returns false for an unknown characteristic; otherwise it fills `out` with the
    // value fragments are applied to. Backends pass an empty value: an Execute Write delivers a new
    // message, so a batch must start at offset 0 for every characteristic.
    inline BatchResult AssembleBatch(
        const std::vector<Fragment>& fragments,
        const std::function<bool(const std::string& key, std::vector<uint8_t>& out)>& base,
        std::size_t maxLength = kMaxAttributeLength) {
        BatchResult result;
        std::vector<AssembledValue> values;
        for (std::size_t i = 0; i < fragments.size(); ++i) {
            const Fragment& f = fragments[i];
            AssembledValue* target = nullptr;
            for (auto& v : values) {
                if (v.key == f.key) { target = &v; break; }
            }
            if (!target) {
                AssembledValue fresh;
                fresh.key = f.key;
                if (!base(f.key, fresh.value)) {
                    result.status = Status::AttributeNotFound;
                    result.failedIndex = i;
                    return result;
                }
                values.push_back(std::move(fresh));
                target = &values.back();
            }
            const Status s = ApplyFragment(target->value, f.offset, f.data, maxLength);
            if (s != Status::Ok) {
                result.status = s;
                result.failedIndex = i;
                return result;
            }
        }
        result.values = std::move(values);
        return result;
    }

} // namespace LongWrite
} // namespace BleServer
