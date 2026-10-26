// ble_test_server: native command-line GATT server for real-BLE tests with nRF Connect.
// Layout: docs/GATT_TEST_PROFILE.md. Procedure: docs/TESTING.md.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "TestServerApp.h"

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop = true; }  // only sets a flag: async-signal-safe

void PrintUsage(const char* argv0) {
    std::printf(
        "Usage: %s [--name NAME] [--heartbeat-ms N] [--verbose] [--run-seconds N]\n"
        "  --name NAME         advertised name (default \"%s\")\n"
        "  --heartbeat-ms N    heartbeat notify interval while a central is connected (default %d, min 50)\n"
        "  --verbose           log every heartbeat and platform diagnostics\n"
        "  --run-seconds N     quit by itself after N seconds (for scripted runs)\n"
        "Commands on stdin:  n = notify now, l = send large message, s = stop/start advertising,\n"
        "                    h = help, q = quit.  Ctrl-C also shuts down cleanly.\n",
        argv0, TestProfile::kDefaultName, TestProfile::kDefaultHeartbeatMs);
}

}  // namespace

int main(int argc, char** argv) {
    TestServerApp::Options options;
    int heartbeatMs = TestProfile::kDefaultHeartbeatMs;
    int runSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto needValue = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--name") options.name = needValue("--name");
        else if (a == "--heartbeat-ms") heartbeatMs = std::atoi(needValue("--heartbeat-ms"));
        else if (a == "--run-seconds") runSeconds = std::atoi(needValue("--run-seconds"));
        else if (a == "--verbose") options.verbose = true;
        else if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); PrintUsage(argv[0]); return 2; }
    }
    if (options.name.empty() || heartbeatMs < 50) {
        std::fprintf(stderr, "--name must not be empty and --heartbeat-ms must be >= 50\n");
        return 2;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::mutex outMutex;  // Log() already serializes; this keeps main's own lines intact too
    TestServerApp::Server server(options, [&](const std::string& line) {
        std::lock_guard<std::mutex> l(outMutex);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    });

    if (!server.Start()) {
        std::fprintf(stderr, "ble_test_server: could not start (see the error above)\n");
        return 1;
    }
    std::printf("Service %s\nCommands: n = notify, l = large message, s = stop/start advertising, h = help, q = quit; Ctrl-C quits.\n",
                TestProfile::kServiceUuid);
    std::fflush(stdout);

    // stdin reader: lines go to a queue handled on the main thread. On EOF (no terminal, e.g. under a
    // script) it just stops reading; the server keeps running until a signal, --run-seconds or q.
    std::mutex cmdMutex;
    std::deque<std::string> commands;
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> l(cmdMutex);
            commands.push_back(line);
        }
    });
    reader.detach();  // blocked in getline at exit; the process ends right after cleanup

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto nextHeartbeat = start + std::chrono::milliseconds(heartbeatMs);
    auto nextRetransmit = start + std::chrono::seconds(1);

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto now = clock::now();

        std::string cmd;
        {
            std::lock_guard<std::mutex> l(cmdMutex);
            if (!commands.empty()) { cmd = commands.front(); commands.pop_front(); }
        }
        if (!cmd.empty()) {
            const char c = cmd[0];
            if (c == 'n') server.SendManualNotify();
            else if (c == 'l') server.SendLargeMessage();
            else if (c == 's') server.ToggleAdvertising();
            else if (c == 'q') g_stop = true;
            else if (c == 'h' || c == '?') PrintUsage(argv[0]);
            else std::printf("unknown command \"%s\" (n, l, s, h, q)\n", cmd.c_str());
            std::fflush(stdout);
        }
        if (now >= nextHeartbeat) {
            nextHeartbeat = now + std::chrono::milliseconds(heartbeatMs);
            server.SendHeartbeat();
        }
        if (now >= nextRetransmit) {
            nextRetransmit = now + std::chrono::seconds(1);
            server.RetransmitTick();
        }
        if (runSeconds > 0 && now - start >= std::chrono::seconds(runSeconds)) {
            std::printf("--run-seconds %d reached, shutting down\n", runSeconds);
            g_stop = true;
        }
    }

    std::printf("Shutting down...\n");
    std::fflush(stdout);
    server.Stop();
    std::printf("Bye.\n");
    return 0;
}
