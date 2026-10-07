module;
// Keep textual SDK/standard headers in a global module fragment on every platform.
#include "MMKV/MMKV.h"
#include "MMKVMetaInfo.hpp"
#include "crc32/Checksum.h"

module Template.Storage.Mmkv:sdk;

// Private implementation partition: the public storage interface exports none of these names.
namespace storage {
using ::MMKV;
using ::MMKVConfig;
using ::MMKVLogError;
using ::MMKV_SYNC;
using ::mmkv::MMBuffer;
using ::mmkv::MMKVMetaInfo;
using ::mmkv::MMKVVersionFlag;
using ::mmkv::MMKVVersionActualSize;

// Keep SDK operators and platform-dependent CRC macros at the header boundary.
inline auto accessMode(bool readOnly) -> MMKVMode
{
    // The SDK's operator| has TU-local linkage, so it must not escape through this partition.
    auto flags = static_cast<uint32_t>(MMKV_SINGLE_PROCESS);
    if (readOnly) flags |= static_cast<uint32_t>(MMKV_READ_ONLY);
    return static_cast<MMKVMode>(flags);
}
inline auto checksum(uint32_t seed, const uint8_t *bytes, size_t size) -> uint32_t
{
    return CRC32(seed, bytes, size);
}
} // namespace storage
