#pragma once

#include "ModeIncludes.h"

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
    std::future<bool> accept_result;
};

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options);
std::uint32_t ExpectedNetworkClients(const ServerCliOptions& options);
std::uint32_t LocalNetworkPlayerId(const ServerCliOptions& options);
bool ResolveNetworkClientPort(std::uint16_t base_port,
                              std::uint32_t client_id,
                              std::uint16_t& out_port);
void StartRuntimeTcpAccept(RuntimeTcpClientSlot& slot);
bool PollRuntimeTcpAccept(RuntimeTcpClientSlot& slot);
void CloseRuntimeTcpSlot(RuntimeTcpClientSlot& slot);
