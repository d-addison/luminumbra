#pragma once

#include "../ServerCliOptions.h"

int RunSmoke(const ServerCliOptions& options);
int RunWindBench(const ServerCliOptions& options);
int RunAetherBench(const ServerCliOptions& options);
int RunWeatherBench(const ServerCliOptions& options);
int RunHeavy(const ServerCliOptions& options);
int RunRecord(const ServerCliOptions& options);
int RunReplay(const ServerCliOptions& options);
int RunMutateReplayFixture(const ServerCliOptions& options);
int RunLockstepLoopback(const ServerCliOptions& options);
int RunServer(const ServerCliOptions& options);
int RunReplicate(const ServerCliOptions& options);
int RunNetHostServerMode(const ServerCliOptions& options);
int RunNetSoak(const ServerCliOptions& options);
int RunNetSoakClient(const ServerCliOptions& options);
int RunNetHost(const ServerCliOptions& options);
int RunNetJoin(const ServerCliOptions& options);

#ifdef LUMINUMBRA_ENABLE_STEAM
int RunSteamHost(const ServerCliOptions& options);
int RunSteamJoin(const ServerCliOptions& options);
#endif // LUMINUMBRA_ENABLE_STEAM

#ifdef LUMINUMBRA_ENABLE_GNS
int RunGnsHost(const ServerCliOptions& options);
int RunGnsJoin(const ServerCliOptions& options);
#endif // LUMINUMBRA_ENABLE_GNS
