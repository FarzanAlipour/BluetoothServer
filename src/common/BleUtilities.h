// BleUtilities.h - Common utility functions for BLE operations
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <optional>

namespace BleServer {
namespace Utils {

    /**
     * Validates UUID format (supports both 16-bit and 128-bit UUIDs)
     * Valid formats:
     * - 16-bit: "XXXX" (4 hex digits)
     * - 128-bit: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (standard UUID format)
     */
    inline bool IsValidUuid(const std::string& uuid) {
        if (uuid.empty()) {
            return false;
        }

        // Check for 16-bit UUID (4 hex digits)
        if (uuid.length() == 4) {
            return std::all_of(uuid.begin(), uuid.end(), ::isxdigit);
        }

        // Check for 128-bit UUID (standard format with dashes)
        if (uuid.length() == 36) {
            // Format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
            if (uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-') {
                return false;
            }

            // Check all other characters are hex digits
            for (size_t i = 0; i < uuid.length(); ++i) {
                if (i == 8 || i == 13 || i == 18 || i == 23) {
                    continue; // Skip dash positions
                }
                if (!std::isxdigit(static_cast<unsigned char>(uuid[i]))) {
                    return false;
                }
            }
            return true;
        }

        // Also accept 128-bit UUID without dashes (32 hex digits)
        if (uuid.length() == 32) {
            return std::all_of(uuid.begin(), uuid.end(),
                [](unsigned char c) { return std::isxdigit(c); });
        }

        return false;
    }

    /**
     * Normalizes UUID to uppercase standard format
     */
    inline std::string NormalizeUuid(const std::string& uuid) {
        std::string normalized = uuid;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);

        // Add dashes if 32-char format without dashes
        if (normalized.length() == 32) {
            return normalized.substr(0, 8) + "-" +
                   normalized.substr(8, 4) + "-" +
                   normalized.substr(12, 4) + "-" +
                   normalized.substr(16, 4) + "-" +
                   normalized.substr(20, 12);
        }

        return normalized;
    }

    /**
     * Validates characteristic value size against max length
     */
    inline bool IsValidValueSize(size_t valueSize, size_t maxLength) {
        return valueSize > 0 && valueSize <= maxLength;
    }

    /**
     * Validates characteristic properties flags
     */
    inline bool IsValidProperties(uint32_t properties) {
        // Check that at least one property is set and no invalid flags
        const uint32_t validFlags = 0x02 | 0x04 | 0x08 | 0x10 | 0x20; // Read, WriteNoResp, Write, Notify, Indicate
        return properties != 0 && (properties & ~validFlags) == 0;
    }

    /**
     * Validates characteristic permissions flags
     */
    inline bool IsValidPermissions(uint32_t permissions) {
        // Check no invalid flags
        const uint32_t validFlags = 0x01 | 0x02 | 0x04 | 0x08; // Read, Write, ReadEncrypted, WriteEncrypted
        return (permissions & ~validFlags) == 0;
    }

    /**
     * Generates a unique device ID from raw data
     */
    inline std::string GenerateDeviceId(const std::string& rawId) {
        // Create a more stable hash-based ID
        std::hash<std::string> hasher;
        size_t hash = hasher(rawId);

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "DEV-%016zX", hash);
        return std::string(buffer);
    }

    /**
     * Validates device name
     */
    inline bool IsValidDeviceName(const std::string& name) {
        // BLE device names should be 1-248 characters (Bluetooth spec limit)
        return !name.empty() && name.length() <= 248;
    }

    /**
     * Validates max connections value
     */
    inline bool IsValidMaxConnections(int maxConnections) {
        // Reasonable range: 1-10 concurrent connections
        return maxConnections >= 1 && maxConnections <= 10;
    }

} // namespace Utils
} // namespace BleServer
