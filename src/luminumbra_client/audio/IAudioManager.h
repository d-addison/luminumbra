#pragma once
#include "glm/glm.hpp"
#include "luminumbra_common/components/audio/AudioTypes.h"
#include <cstdint>
#include <memory>
#include <string>

using Luminumbra::Common::AudioEventHandle;
using Luminumbra::Common::AudioEventID;
using Luminumbra::Common::AudioParamID;

namespace Luminumbra::Client {

// the client mix-bus tree. Render/audio-side only
// (never sim). Hierarchy:
//   master (engine endpoint volume)
//     +-- music                       (existing per-track music path)
//     +-- sfx                         (everything that is not music)
//           +-- ambient               (looping beds: forest/birds/wind/rain/zones)
//           +-- events                (stings/thunder/discovery -- sidechain-ducks ambient)
//           +-- ui                    (button clicks/hovers)
// Sounds routed to a SUB-bus (ambient/events/ui) are also scaled by the parent
// sfx bus, so the persisted user.audio_sfx setting scales ALL non-music audio.
enum class BusId : uint8_t {
    Master = 0,
    Music,
    Sfx,
    Ambient,
    Events,
    Ui,
};

class IAudioManager {
public:
    virtual ~IAudioManager() = default;

    virtual bool Init() = 0;
    virtual void Update() = 0;
    virtual void Shutdown() = 0;

    virtual bool LoadBank(const std::string& bankPath) = 0;
    virtual void UnloadBank(const std::string& bankPath) = 0;

    virtual void SetListenerTransform(const glm::vec3& position,
                                      const glm::vec3& forward,
                                      const glm::vec3& up) = 0;

    virtual bool PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) = 0;
    // One-shot playback with explicit bus routing. The default,
    // BusId::Sfx, preserves the pre-bus behaviour for every existing call site
    // (all bus volumes boot at 1.0, so audio is unchanged until a volume moves).
    // Route by semantics: ambient beds -> Ambient, stings/thunder/discovery ->
    // Events (these sidechain-duck the ambient bus), UI clicks -> Ui.
    virtual bool
    PlayOneShot(const AudioEventID& eventID, const glm::vec3& position, BusId bus = BusId::Sfx) = 0;
    virtual bool PlayOneShot2D(const AudioEventID& eventID, BusId bus = BusId::Sfx) = 0;

    // NEW
    virtual void PlayMusic(const AudioEventID& musicEventID) = 0;
    virtual void StopMusic() = 0;

    // Looping ambient bed (streamed, 3D). Re-calling with the same id restarts it; a huge
    // radius makes it an effectively constant world ambience. Render-only.
    virtual void
    PlayAmbientLoop(const AudioEventID& eventID, const glm::vec3& position, float radius) = 0;
    virtual void StopAmbientLoop(const AudioEventID& eventID) = 0;
    // Live volume scale for an active ambient loop (re-applies the bank base * scale * env).
    // No-op if that loop isn't currently playing. Lets a bed swell/fade at runtime — e.g. the
    // wind bed rising with the wind-field strength. Render-only.
    virtual void SetAmbientVolume(const AudioEventID& eventID, float scale) = 0;

    // Master output gain [0,1] (user.audio.master). Render-only player setting.
    virtual void SetMasterVolume(float volume) = 0;
    // Music-bus gain [0,1] (user.audio.music): scales the music bed independently of master/SFX,
    // applied to the currently-playing track and all future PlayMusic. Render-only.
    virtual void SetMusicVolume(float volume) = 0;
    // SFX-bus gain [0,1] (user.audio_sfx): scales EVERY non-music sound (one-shots,
    // spatial events, ambient beds, UI) independently of master/music, applied live
    // to playing sounds and all future playback. Render-only.
    virtual void SetSfxVolume(float volume) = 0;
    // Generic bus gain [0,1]. Master/Music/Sfx forward to the dedicated
    // setters above; Ambient/Events/Ui scale their sub-bus underneath sfx. All
    // buses boot at 1.0 (audio identical until a volume moves). Render-only.
    virtual void SetBusVolume(BusId bus, float volume) = 0;

    virtual bool StopEvent(AudioEventHandle handle, bool immediate = true) = 0;
    virtual bool SetEventPosition(AudioEventHandle handle, const glm::vec3& position) = 0;
    virtual bool SetEventVolume(AudioEventHandle handle, float volume) = 0;
    virtual bool
    SetEventParameter(AudioEventHandle handle, const AudioParamID& paramID, float value) = 0;
};

// Factory function to create the concrete instance based on config/build flags
std::unique_ptr<IAudioManager> CreateAudioManager();

} // namespace Luminumbra::Client
