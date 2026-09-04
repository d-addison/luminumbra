#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <entt/entt.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "../ServerCliOptions.h"
#include "../ServerWorldRunner.h"
#include "core/EngineVersion.h"
#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/net/GnsTransport.h" // body #ifdef LUMINUMBRA_ENABLE_GNS
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/ReplicationProtocol.h"
#include "luminumbra_common/net/SteamNetworkingTransport.h" // body #ifdef LUMINUMBRA_ENABLE_STEAM
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/PlayerAvatar.h"
