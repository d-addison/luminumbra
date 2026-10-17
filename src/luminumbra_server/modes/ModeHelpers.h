#pragma once

#include "ModeIncludes.h"

#include <chrono>
#include <cstdint>
#include <ratio>
#include <thread>

#include "luminumbra_common/net/ReplicationAdmission.h"

// Optional transports establish the connection separately (30 s). Admission time
// is local 30 Hz steady-clock time, never callback, replication or simulation ticks.
inline bool AwaitReplicationAdmission(Luminumbra::Net::AdmittedReplicationTransport& transport,
                                      void (*run_callbacks)()) {
    const auto started = std::chrono::steady_clock::now();
    transport.Begin(0);
    while (!transport.admission().admitted() && !transport.admission().failed()) {
        run_callbacks();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 30>>>(
                elapsed)
                .count());
        transport.PumpAdmission(ticks);
        if (!transport.admission().admitted() && !transport.admission().failed())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (transport.admission().failed()) {
        LUMINUMBRA_CORE_ERROR("replication admission failed: {}", transport.admission().reason());
        return false;
    }
    return true;
}

struct RuntimeTcpClientSlot {
    static constexpr std::chrono::seconds kHelloTimeout{10};

    std::uint32_t client_id = 0;
    std::uint16_t port = 0;
    std::unique_ptr<Luminumbra::Net::TcpTransport> transport;
    bool accepted = false;
    bool connected = false;
    std::uint64_t joined_tick = 0;
    std::uint64_t left_tick = 0;
    float initial_x = 0.0f;
    bool accepting = false;
    bool awaiting_hello = false;
    // Transport-only deadline, valid while awaiting_hello; never uses simulation ticks.
    std::chrono::steady_clock::time_point hello_deadline{};
    std::future<bool> accept_result;
};

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options);
std::uint32_t ExpectedNetworkClients(const ServerCliOptions& options);
std::uint32_t LocalNetworkPlayerId(const ServerCliOptions& options);
bool ResolveNetworkClientPort(std::uint16_t base_port,
                              std::uint32_t client_id,
                              std::uint16_t& out_port);
void StartRuntimeTcpAccept(RuntimeTcpClientSlot& slot);
bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot, const Luminumbra::Net::HelloMsg& expected);
bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot,
                          const Luminumbra::Net::HelloMsg& expected,
                          std::chrono::steady_clock::time_point now);
std::string CapturedSimulationIdentityForHello(const Luminumbra::world::GameSession& session);
Luminumbra::Net::HelloMsg
RuntimeHelloFrom(const ServerCliOptions& options,
                 const Luminumbra::world::GameSession* captured_session = nullptr);
enum class RuntimeHelloAdmission {
    Pending,
    Accepted,
    Refused
};
// Consume only the first queued frame; following replication frames stay queued.
RuntimeHelloAdmission PollRuntimeHello(Luminumbra::Net::ILockstepTransport& transport,
                                       const Luminumbra::Net::HelloMsg& expected,
                                       std::uint32_t client_id);
void CloseRuntimeTcpSlot(RuntimeTcpClientSlot& slot);
