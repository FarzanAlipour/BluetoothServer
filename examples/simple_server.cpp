// simple_server.cpp - Simple BLE GATT server example
// Demonstrates basic usage of the BleServer library

#include "IBleServer.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace BleServer;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

int main() {
    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "=== BluetoothServer Simple Example ===" << std::endl;
    std::cout << "This example creates a BLE server with Device Information Service" << std::endl << std::endl;

    // Create the BLE server instance
    auto server = IBleServer::Create();
    if (!server) {
        std::cerr << "ERROR: Failed to create BLE server" << std::endl;
        return 1;
    }

    // Configure the server
    ServerConfig serverConfig;
    serverConfig.deviceName = "SimpleGATT";
    serverConfig.requiresPairing = false;
    serverConfig.maxConnections = 5;

    std::cout << "Initializing BLE server as '" << serverConfig.deviceName << "'..." << std::endl;

    // Initialize the server
    BleError error = server->Initialize(serverConfig);
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to initialize server (error code: " << static_cast<int>(error) << ")" << std::endl;
        return 1;
    }

    std::cout << "✓ Server initialized successfully" << std::endl;

    // Add Device Information Service (0x180A)
    ServiceConfig deviceInfoService;
    deviceInfoService.uuid = "0000180a-0000-1000-8000-00805f9b34fb";
    deviceInfoService.isPrimary = true;

    std::cout << "Adding Device Information Service..." << std::endl;

    error = server->AddService(deviceInfoService);
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to add service (error code: " << static_cast<int>(error) << ")" << std::endl;
        server->Shutdown();
        return 1;
    }

    std::cout << "✓ Service added successfully" << std::endl;

    // Add Manufacturer Name characteristic (0x2A29)
    CharacteristicConfig manufacturerChar;
    manufacturerChar.uuid = "00002a29-0000-1000-8000-00805f9b34fb";
    manufacturerChar.properties = static_cast<uint32_t>(CharacteristicProperty::Read);
    manufacturerChar.permissions = static_cast<uint32_t>(CharacteristicPermission::Read);
    manufacturerChar.initialValue = {'E', 'x', 'a', 'm', 'p', 'l', 'e', ' ', 'C', 'o', '.'};

    std::cout << "Adding Manufacturer Name characteristic..." << std::endl;

    error = server->AddCharacteristic(deviceInfoService.uuid, manufacturerChar);
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to add characteristic (error code: " << static_cast<int>(error) << ")" << std::endl;
        server->Shutdown();
        return 1;
    }

    std::cout << "✓ Manufacturer Name characteristic added" << std::endl;

    // Add Model Number characteristic (0x2A24) with read callback
    CharacteristicConfig modelChar;
    modelChar.uuid = "00002a24-0000-1000-8000-00805f9b34fb";
    modelChar.properties = static_cast<uint32_t>(CharacteristicProperty::Read);
    modelChar.permissions = static_cast<uint32_t>(CharacteristicPermission::Read);
    modelChar.initialValue = {'v', '1', '.', '0'};

    std::cout << "Adding Model Number characteristic with callback..." << std::endl;

    error = server->AddCharacteristic(deviceInfoService.uuid, modelChar);
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to add model characteristic (error code: " << static_cast<int>(error) << ")" << std::endl;
        server->Shutdown();
        return 1;
    }

    // Set read callback for Model Number
    server->SetCharacteristicReadCallback(
        deviceInfoService.uuid,
        modelChar.uuid,
        [](const std::string& deviceId) {
            std::cout << "  → Read request from device: " << deviceId << std::endl;
            return std::vector<uint8_t>{'M', 'o', 'd', 'e', 'l', '-', 'X', '1'};
        }
    );

    std::cout << "✓ Model Number characteristic added with callback" << std::endl;

    // Add a writable characteristic for testing
    CharacteristicConfig writableChar;
    writableChar.uuid = "00002a00-0000-1000-8000-00805f9b34fb"; // Device Name
    writableChar.properties = static_cast<uint32_t>(CharacteristicProperty::Read) |
                             static_cast<uint32_t>(CharacteristicProperty::Write);
    writableChar.permissions = static_cast<uint32_t>(CharacteristicPermission::Read) |
                               static_cast<uint32_t>(CharacteristicPermission::Write);
    writableChar.initialValue = {'T', 'e', 's', 't'};

    std::cout << "Adding writable Device Name characteristic..." << std::endl;

    error = server->AddCharacteristic(deviceInfoService.uuid, writableChar);
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to add writable characteristic (error code: " << static_cast<int>(error) << ")" << std::endl;
        server->Shutdown();
        return 1;
    }

    // Set write callback
    server->SetCharacteristicWriteCallback(
        deviceInfoService.uuid,
        writableChar.uuid,
        [](const std::string& deviceId, const std::vector<uint8_t>& data) {
            std::cout << "  → Write request from device: " << deviceId << std::endl;
            std::cout << "  → Data (" << data.size() << " bytes): ";
            for (auto byte : data) {
                if (byte >= 32 && byte < 127) {
                    std::cout << static_cast<char>(byte);
                } else {
                    std::cout << "\\x" << std::hex << static_cast<int>(byte) << std::dec;
                }
            }
            std::cout << std::endl;
        }
    );

    std::cout << "✓ Writable characteristic added with callback" << std::endl;

    // Set connection callback
    server->SetConnectionCallback([](const std::string& deviceId, bool connected) {
        if (connected) {
            std::cout << "\n[CONNECTION] Device connected: " << deviceId << std::endl;
        } else {
            std::cout << "\n[CONNECTION] Device disconnected: " << deviceId << std::endl;
        }
    });

    // Start advertising
    std::cout << "\nStarting advertisement..." << std::endl;

    error = server->StartAdvertising();
    if (error != BleError::Success) {
        std::cerr << "ERROR: Failed to start advertising (error code: " << static_cast<int>(error) << ")" << std::endl;
        server->Shutdown();
        return 1;
    }

    std::cout << "✓ Advertisement started" << std::endl;
    std::cout << "\n=== Server is now running ===" << std::endl;
    std::cout << "Device Name: " << serverConfig.deviceName << std::endl;
    std::cout << "Service UUID: " << deviceInfoService.uuid << std::endl;
    std::cout << "\nConnect using:" << std::endl;
    std::cout << "  - nRF Connect mobile app" << std::endl;
    std::cout << "  - bluetoothctl (Linux)" << std::endl;
    std::cout << "  - Windows Bluetooth Settings" << std::endl;
    std::cout << "\nPress Ctrl+C to stop..." << std::endl << std::endl;

    // Main loop - keep server running
    int seconds = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        seconds++;

        // Show status every 10 seconds
        if (seconds % 10 == 0) {
            auto devices = server->GetConnectedDevices();
            std::cout << "[STATUS] Running for " << seconds << "s, "
                      << devices.size() << " device(s) connected, "
                      << "Advertising: " << (server->IsAdvertising() ? "Yes" : "No") << std::endl;
        }
    }

    // Cleanup
    std::cout << "\nStopping advertisement..." << std::endl;
    server->StopAdvertising();

    std::cout << "Shutting down server..." << std::endl;
    server->Shutdown();

    std::cout << "Goodbye!" << std::endl;

    return 0;
}
