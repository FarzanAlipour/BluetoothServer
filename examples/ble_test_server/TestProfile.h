#pragma once
// GATT test profile: the single C++ source of truth for the layout used by ble_test_server and
// by the fake-backend scenario test. Human-readable spec: docs/GATT_TEST_PROFILE.md.
// The Unity sample (samples/UnityBleSample/Assets/Scripts/BleSampleGatt.cs) mirrors the same UUIDs.
// Keep the three in sync; they use custom 128-bit UUIDs (not Bluetooth SIG numbers).
#include <cstddef>
#include <cstdint>

namespace TestProfile {

constexpr const char* kDefaultName = "BLE-TestServer";

constexpr const char* kServiceUuid = "7b1e0000-5a6c-4f0e-9d3b-2c1a8e4f6b01";
constexpr const char* kReadUuid    = "7b1e0001-5a6c-4f0e-9d3b-2c1a8e4f6b01";  // Read
constexpr const char* kWriteUuid   = "7b1e0002-5a6c-4f0e-9d3b-2c1a8e4f6b01";  // Write, Write w/o response
constexpr const char* kNotifyUuid  = "7b1e0003-5a6c-4f0e-9d3b-2c1a8e4f6b01";  // Read, Notify (heartbeat)
constexpr const char* kEchoUuid    = "7b1e0004-5a6c-4f0e-9d3b-2c1a8e4f6b01";  // Write, Write w/o response, Notify
// 7b1e0005 was the legacy BleMessageChunker "Large" characteristic; retired (not registered any more).
// Large messages use the BleLink service (BLELINK_SERVICE_UUID in IBleServer.h, docs/PROTOCOL.md).

// Property flags (CharacteristicProperty in IBleServer.h): Read=0x02 WriteNoResp=0x04 Write=0x08 Notify=0x10.
constexpr uint32_t kPropRead = 0x02;
constexpr uint32_t kPropWriteNoResp = 0x04;
constexpr uint32_t kPropWrite = 0x08;
constexpr uint32_t kPropNotify = 0x10;
// Permission flags (CharacteristicPermission): Read=0x01 Write=0x02.
constexpr uint32_t kPermRead = 0x01;
constexpr uint32_t kPermWrite = 0x02;

constexpr uint32_t kReadProps = kPropRead;
constexpr uint32_t kWriteProps = kPropWrite | kPropWriteNoResp;
constexpr uint32_t kNotifyProps = kPropRead | kPropNotify;
constexpr uint32_t kEchoProps = kPropWrite | kPropWriteNoResp | kPropNotify;

constexpr int kDefaultHeartbeatMs = 1000;
// BleLink messages: log PROGRESS events only at multiples of this (every event with --verbose).
constexpr uint64_t kProgressLogStep = 256u * 1024u;
// Largest blob the `b <size>` command generates (the peer's maxMessage may be lower).
constexpr size_t kMaxBlobSize = 0x7FFFFFFF;

}  // namespace TestProfile
