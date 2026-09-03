#pragma once
#include "components/audio/AudioTypes.h"

// Use the common namespace
using namespace Luminumbra::Common;

namespace Luminumbra::Common {

struct AudioSourceComponent {
    AudioEventID eventID;
    bool isPlaying = false; // The system will set this to true to request playback

    // Internal state, managed by the AudioSystem
    AudioEventHandle eventHandle = 0;
};

// ...
} // namespace Luminumbra::Common