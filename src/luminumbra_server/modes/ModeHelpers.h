#pragma once

#include "ModeIncludes.h"

#include <functional>

// Runtime TCP peers must send Hello within 150 host simulation ticks (about five
// seconds at 30 Hz), starting when TCP acceptance is observed by the host poll.
inline constexpr std::uint64_t kRuntimeTcpHelloTimeoutTicks = 150;

struct RuntimeTcpClientSlot {
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
    std::uint64_t hello_started_tick = 0;
    std::future<bool> accept_result;
};

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options);
std::uint32_t ExpectedNetworkClients(const ServerCliOptions& options);
std::uint32_t LocalNetworkPlayerId(const ServerCliOptions& options);
bool ResolveNetworkClientPort(std::uint16_t base_port,
                              std::uint32_t client_id,
                              std::uint16_t& out_port);
// Optional readiness callback runs on the async accept worker before its accept wait.
void StartRuntimeTcpAccept(RuntimeTcpClientSlot& slot, std::function<void()> on_listening = {});
// Poll once per host tick. Hello is refused at elapsed >= kRuntimeTcpHelloTimeoutTicks;
// the transport is closed and the slot can listen again on the next poll.
bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot,
                          const Luminumbra::Net::HelloMsg& expected,
                          std::uint64_t host_tick,
                          std::function<void()> on_listening = {});
Luminumbra::Net::HelloMsg RuntimeHelloFrom(const ServerCliOptions& options);
void CloseRuntimeTcpSlot(RuntimeTcpClientSlot& slot);
