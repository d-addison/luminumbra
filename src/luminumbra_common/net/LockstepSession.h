#pragma once

// T-I4-13: delay-based lockstep transport (engine-generic; NO client/game deps).
//
// WHY DELAY-BASED, NOT ROLLBACK (binding decision -- design-decisions.md section 8,
// research worldgen-lockstep-sdfrt.md Area 2 takeaway 1, and decision 9):
//   Rollback netcode exists to hide input latency in reaction-critical COMPETITIVE
//   play; its price is N-tick re-simulation + a full world snapshot/restore every
//   frame. Our world tick includes chunk streaming, water, and physics, so rollback
//   re-simulation would be brutal -- and Project Capture is a zen co-op photography
//   game with no competitive latency demands. Rollback is therefore REJECTED,
//   permanently, for this title. We adopt the 1500-Archers delay-based model on the
//   30 Hz SimulationClock: a client's input for tick T is scheduled AHEAD and applied
//   at tick T+horizon; the session does not advance a tick until every peer's input
//   for it has arrived. The perceived-latency mitigations that make delay-based feel
//   good (render-side camera look, adaptive input horizon, server-paced speed control)
//   live elsewhere -- this transport carries only the quantized movement/action input
//   stream and the desync-oracle hash exchange.
//
// DETERMINISM INVARIANT (the load-bearing one -- design-decisions section 6/13):
//   The ENTIRE adaptive-horizon / latency machinery is OUTSIDE what feeds the world
//   hash. Horizon adaptation is measured in TICKS (how many ticks an input arrived
//   late by), never in wall-clock; no std::chrono / RTT measurement is in the
//   tick-affecting path. The simulation a peer runs depends only on (seed, preset,
//   the ordered set of per-tick inputs) -- the same three inputs the LREC1 replay
//   reconstructs from. Two peers fed identical inputs converge to identical hashes;
//   the horizon only decides WHEN a tick is allowed to run, never WHAT it computes.
//
// DESYNC ORACLE: peers exchange world_hash + authoritative sub-hashes at the 30-tick
// checkpoint cadence (the same cadence the LREC1 replay uses). On a mismatch the
// session HALTS and emits an LREC1 stream (via ReplayStream) of the local session up
// to the divergence -- the dump's checkpoints carry the LOCAL hashes, and the divergent
// tick + section (terrain/water/entities) is recorded. This is the desync-repro
// artifact, identical in shape to the --replay path, so the existing tooling re-runs it.
//
// CLEAN DISCONNECT (critique F3): a peer Bye or a socket close ENDS the session
// cleanly -- no hang, no crash, no infinite wait. The surviving side records the
// disconnect tick and stops; a clean disconnect is NOT a desync (there is no rejoin
// in v1).
//
// The transport is engine-generic: it knows about ticks, peers, opaque per-tick input
// blobs, and hash checkpoints. It knows NOTHING about voxels, biomes, cameras, or any
// game concept. The opaque input blob is exactly the InputRecord payload of LREC1.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Luminumbra::Net {

// --- On-wire protocol (fixed-width little-endian; mirrors ReplayStream's discipline:
// explicit per-byte shifts, length-prefixed strings/blobs, NO struct padding on the
// wire, NO float bit-twiddling -- hashes travel as their hex strings). ------------

inline constexpr char kLockstepMagic[5] = {'L', 'S', 'T', 'P', '1'};
inline constexpr std::uint16_t kLockstepProtocolVersion = 1;

// Message type tags (u8 on the wire).
enum class MessageType : std::uint8_t {
    Hello = 0x01,      // handshake: protocol/version/seed/preset/tick-rate + client id
    Input = 0x02,      // per-tick input: tick, client id, opaque input blob (empty ok)
    Hash = 0x03,       // periodic desync oracle: tick + world_hash + sub-hashes
    Bye = 0x04,        // clean disconnect: the disconnect tick
};

// Handshake. Both peers send a Hello first; mismatched protocol/version/seed/preset/
// tick-rate is REJECTED LOUDLY (a mis-paired session would silently desync otherwise).
struct HelloMsg {
    std::uint16_t protocol_version = kLockstepProtocolVersion;
    std::uint64_t seed = 0;
    std::string preset;
    std::uint16_t tick_rate_hz = 30;
    std::uint32_t client_id = 0; // the sender's id (0 = host, 1 = first remote)
};

// One client's input for one tick. The blob is OPAQUE (the game's per-tick input set's
// wire form; empty is valid and compact -- the headless server has none today).
struct InputMsg {
    std::uint64_t tick = 0;
    std::uint32_t client_id = 0;
    std::vector<std::uint8_t> inputs;
};

// The desync oracle exchange: a peer's authoritative hashes at a checkpoint tick. Mesh
// is excluded (derived render artifact); authoritative = terrain/water/entities, with
// the combined world_hash for the top-level compare.
struct HashMsg {
    std::uint64_t tick = 0;
    std::string world_hash;
    std::string terrain;
    std::string water;
    std::string entities;
};

// Clean disconnect marker carrying the tick the sender stopped at.
struct ByeMsg {
    std::uint64_t tick = 0;
};

// Encode/decode helpers -- a message is [u8 type][u32 payload-len][payload]. Decoders
// return false on a short/garbled frame (truncation-robust, like the LREC1 cursor).
std::vector<std::uint8_t> EncodeHello(const HelloMsg& m);
std::vector<std::uint8_t> EncodeInput(const InputMsg& m);
std::vector<std::uint8_t> EncodeHash(const HashMsg& m);
std::vector<std::uint8_t> EncodeBye(const ByeMsg& m);

// Peeks the type of a framed message (false if the frame is too short to hold a tag).
bool PeekMessageType(const std::vector<std::uint8_t>& frame, MessageType& out_type);
bool DecodeHello(const std::vector<std::uint8_t>& frame, HelloMsg& out);
bool DecodeInput(const std::vector<std::uint8_t>& frame, InputMsg& out);
bool DecodeHash(const std::vector<std::uint8_t>& frame, HashMsg& out);
bool DecodeBye(const std::vector<std::uint8_t>& frame, ByeMsg& out);

// --- Transport seam ----------------------------------------------------------------
// A thin byte-stream abstraction over framed messages. Two impls: an in-process
// LoopbackTransport (gates/tests -- no real networking, no ports, no firewall flakiness)
// and a winsock2 TcpTransport (loopback + LAN). Framing is the transport's concern:
// SendMessage frames a complete message; TryReceiveMessage returns ONE complete framed
// message or false if none is currently available (non-blocking -- the session pumps it
// every tick rather than blocking the loop).
// T-I6 P3 (Steam-readiness, research mp-steam-networking.md): a per-frame delivery
// selector. State replication (Usercmd/Snapshot/Ack) sends UNRELIABLE (most-recent-
// wins; a dropped datagram is superseded); events/handshake/world-edits/chat send
// RELIABLE. Steam's ISteamNetworkingSockets carries BOTH on one connection
// (k_nSteamNetworkingSend_Unreliable / _Reliable). The Loopback/Tcp transports are
// always-reliable (a safe superset), so they ignore the flag; only a future
// SteamNetworkingTransport / GameNetworkingSockets impl honours it.
enum class FrameDelivery { Unreliable, Reliable };

class ILockstepTransport {
public:
    virtual ~ILockstepTransport() = default;

    // Sends one complete message frame. Returns false if the peer is gone.
    // NOTE: named SendFrame (not SendMessage) deliberately -- <windows.h> #defines
    // SendMessage to SendMessageA/W, which would rename this virtual under the macro.
    // `delivery` defaults to Reliable so existing callers (lockstep) are unchanged.
    virtual bool SendFrame(const std::vector<std::uint8_t>& frame,
                           FrameDelivery delivery = FrameDelivery::Reliable) = 0;

    // Non-blocking: pops ONE complete framed message into `out` and returns true, or
    // returns false if none is available right now. A clean peer close is reported via
    // IsPeerConnected()==false (NOT via a thrown error / hang).
    virtual bool TryReceiveFrame(std::vector<std::uint8_t>& out) = 0;

    // True while the peer end is still attached. A clean disconnect flips this false.
    [[nodiscard]] virtual bool IsPeerConnected() const = 0;

    // Marks this end disconnected (so the peer observes IsPeerConnected()==false).
    virtual void Close() = 0;
};

// In-process loopback: a pair of transports sharing two byte-queues (A->B, B->A). No
// sockets. Created via MakeLoopbackPair so the two ends share the same backing queues.
class LoopbackTransport final : public ILockstepTransport {
public:
    struct Channel {
        std::deque<std::vector<std::uint8_t>> queue; // complete framed messages
        bool open = true;
    };

    // Constructs one end: it SENDS into `tx` and RECEIVES from `rx` (the peer is the
    // mirror image). Channels are shared_ptr so both ends see the same queues/open flag.
    LoopbackTransport(std::shared_ptr<Channel> tx, std::shared_ptr<Channel> rx);

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;

private:
    std::shared_ptr<Channel> m_tx;
    std::shared_ptr<Channel> m_rx;
};

// Builds two LoopbackTransports wired back-to-back (a's tx is b's rx and vice versa).
std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
MakeLoopbackPair();

// Real TCP transport (loopback + LAN scope, ONE remote). Length-prefixed frames over a
// blocking-but-polled stream socket: SendFrame writes [u32 LE frame-len][frame] and
// TryReceiveFrame non-blockingly reassembles one complete frame from a receive buffer.
// The winsock2 plumbing is guarded under _WIN32 and links ws2_32 (the gates/tests use
// LoopbackTransport ONLY, so no real ports/firewall are needed there -- this path is for
// the actual loopback/LAN session wiring). On a non-_WIN32 build the methods are stubs
// that report no connection, so the engine still compiles cross-platform; a POSIX-socket
// impl is a later additive change (the seam is identical).
class TcpTransport final : public ILockstepTransport {
public:
    TcpTransport();
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Host: bind+listen on `port`, accept ONE client (blocking up to timeout_ms).
    bool Listen(std::uint16_t port, int timeout_ms = 10000);
    // Client: connect to host:port (blocking up to timeout_ms).
    bool Connect(const std::string& host, std::uint16_t port, int timeout_ms = 10000);

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override;
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override;
    [[nodiscard]] bool IsPeerConnected() const override;
    void Close() override;

private:
    bool PumpRecv(); // pulls available bytes into m_recv_buffer; sets m_peer_closed on EOF

    std::intptr_t m_socket = -1; // SOCKET (winsock) / fd; -1 = none
    std::intptr_t m_listen_socket = -1;
    bool m_peer_closed = false;
    std::vector<std::uint8_t> m_recv_buffer; // accumulates partial frames
};

// --- Session configuration ---------------------------------------------------------

struct LockstepConfig {
    std::uint64_t seed = 0;
    std::string preset = "default";
    std::uint16_t tick_rate_hz = 30;
    std::uint32_t local_client_id = 0; // 0 = host, 1 = the one remote (v1 scope: <= 2)
    std::uint32_t peer_client_id = 1;

    // Adaptive input horizon (in TICKS -- never wall-clock). Input for tick T is applied
    // at T+horizon. Starts at kStart; GROWS by 1 (up to kMax) when a peer input for the
    // tick the session wants to run is not yet present (a late/slow peer), and SHRINKS by
    // 1 (down to kMin) after kSlackTicksToShrink consecutive ticks where the peer input
    // was already buffered ahead (slack returned). Beyond kMax the session PAUSES visibly
    // (WaitingForPeer) rather than desyncing -- a hard ceiling, never a guess. The horizon
    // affects only WHEN a tick runs, never WHAT it computes, so it is hash-neutral.
    std::uint32_t horizon_start = 3;
    std::uint32_t horizon_min = 3;
    std::uint32_t horizon_max = 10;
    std::uint32_t slack_ticks_to_shrink = 30; // ~1s of comfortable slack before shrinking

    // Desync-oracle hash exchange cadence (ticks). Reuses the LREC1 30-tick checkpoint
    // cadence so the dump's checkpoints line up with the recorded replay's.
    std::uint64_t hash_cadence_ticks = 30;
};

// Outcome of a tick-advance attempt.
enum class TickOutcome {
    Advanced,        // a tick ran (all peer inputs present); world advanced by one tick
    WaitingForPeer,  // peer input for the wanted tick not yet present -- caller pumps again
    Desync,          // a hash mismatch was detected -- session HALTED, dump emitted
    PeerDisconnected,// a clean Bye/close -- session ENDED cleanly (not a desync)
    Finished,        // the configured tick budget was reached
};

// The result the host/driver acts on each pump.
struct TickResult {
    TickOutcome outcome = TickOutcome::WaitingForPeer;
    std::uint64_t tick = 0;            // the tick this result concerns
    std::vector<std::uint8_t> local_inputs;  // inputs to APPLY for this tick (local + peer merged opaque set)
    bool ran_hash_exchange = false;   // a HashMsg was sent/compared at this tick
};

// Snapshot of session status (telemetry only -- none of this feeds the hash).
struct LockstepStatus {
    std::uint64_t agreed_tick = 0;     // last tick the session has run
    std::uint32_t horizon = 3;         // current adaptive horizon (ticks)
    bool handshaken = false;
    bool desynced = false;
    bool peer_disconnected = false;
    std::uint64_t disconnect_tick = 0; // tick at which the peer disconnected (if any)
    std::uint64_t desync_tick = 0;     // divergence tick (if desynced)
    std::string desync_section;        // terrain/water/entities/world_hash
    std::string dump_path;             // LREC1 dump path on desync
    std::uint64_t late_input_events = 0;   // how many times the peer input arrived late
    std::uint32_t max_horizon_reached = 3; // peak horizon over the session
};

// The host side provides per-tick local inputs and applies the agreed input set. These
// are the only two callbacks into game/sim code; the session itself is engine-generic.
// (The headless server passes empty inputs and an apply that just steps the world.)
struct LockstepHooks {
    // Returns the LOCAL client's opaque input blob for `tick` (empty allowed). Called
    // once when the session schedules the local input for that tick.
    std::vector<std::uint8_t> (*collect_local_input)(std::uint64_t tick, void* user) = nullptr;

    // Applies the agreed merged input set and advances the simulation by exactly one
    // tick. Returns true on success. `merged` is the concatenation of all peers' opaque
    // blobs for this tick in ascending client-id order (deterministic ordering).
    bool (*apply_and_step)(std::uint64_t tick, const std::vector<std::uint8_t>& merged,
                           void* user) = nullptr;

    // Captures the LOCAL authoritative hashes at `tick` for the oracle exchange + dump.
    void (*capture_hashes)(std::uint64_t tick, HashMsg& out, void* user) = nullptr;

    void* user = nullptr;
};

// --- LockstepSession ---------------------------------------------------------------
// Drives one end of a 2-peer (<=2 clients, ONE remote) delay-based lockstep session
// over an ILockstepTransport. Construct, Handshake(), then PumpTick() in a loop until it
// returns Finished / Desync / PeerDisconnected. The host is the sim authority; both
// sides tick the same world and exchange hashes.
class LockstepSession {
public:
    LockstepSession(LockstepConfig config, ILockstepTransport* transport,
                    LockstepHooks hooks);

    // Exchanges Hello with the peer and validates protocol/version/seed/preset/tick-rate.
    // Returns false on a mismatch (rejected loudly) or a peer that never said Hello.
    // For LoopbackTransport both ends must have sent their Hello before either completes;
    // the host typically Handshakes both sides in the gate's single-process driver.
    bool Handshake();

    // Advances the session by AT MOST one tick. Drains pending peer messages first, then:
    //  - if a clean Bye/close was seen -> PeerDisconnected (clean end);
    //  - if a peer hash mismatched the local one at a cadence tick -> Desync (HALT + dump);
    //  - if the peer's input for the wanted tick is present -> Advanced (world stepped,
    //    hashes exchanged at the cadence);
    //  - else -> WaitingForPeer (horizon grows; caller pumps again after sending/recving).
    // `budget_ticks` is the total tick count to run; returns Finished once reached.
    TickResult PumpTick(std::uint64_t budget_ticks);

    // Sends a clean Bye at the current tick and closes the local transport end. Idempotent.
    void Disconnect();

    [[nodiscard]] LockstepStatus Status() const { return m_status; }
    [[nodiscard]] std::uint64_t AgreedTick() const { return m_agreed_tick; }
    [[nodiscard]] std::uint32_t Horizon() const { return m_horizon; }

    // Where a desync dump is written. Defaults to a temp path; set before PumpTick to
    // direct the gate's dump to a known artifact location.
    void SetDumpPath(const std::string& path) { m_dump_path = path; }

private:
    // Drains all currently-available peer messages into the input/hash buffers, flips
    // disconnect/desync flags. Hash compare happens here (against the local hash captured
    // at the same cadence tick). Returns false if a fatal (desync/disconnect) state was set.
    bool DrainPeerMessages();

    // Schedules + sends the local input for `tick` (collect hook), buffering it locally.
    void ScheduleLocalInput(std::uint64_t tick);

    // Emits the LREC1 desync dump of the local session up to `divergence_tick`.
    void EmitDesyncDump(std::uint64_t divergence_tick, const std::string& section);

    LockstepConfig m_config;
    ILockstepTransport* m_transport = nullptr;
    LockstepHooks m_hooks;

    std::uint32_t m_horizon = 3;
    std::uint64_t m_agreed_tick = 0;     // last tick run (0 = none yet)
    std::uint64_t m_local_input_scheduled_through = 0; // highest tick we've sent local input for
    std::uint32_t m_consecutive_slack_ticks = 0;

    // Buffered inputs keyed by tick: each tick holds the local + peer opaque blobs by
    // client id (ordered map => deterministic merge order). std::map (ORDERED) is used
    // deliberately so the merged-input concatenation order is stable -- an unordered_map
    // would be a hash-order desync hazard (the Factorio lesson).
    std::map<std::uint64_t, std::map<std::uint32_t, std::vector<std::uint8_t>>> m_inputs;

    // Local + peer hashes captured at cadence ticks, for the oracle compare and the dump.
    std::map<std::uint64_t, HashMsg> m_local_hashes;
    std::map<std::uint64_t, HashMsg> m_peer_hashes;

    // Recorded input stream for the desync dump (every applied tick's merged input).
    std::vector<InputMsg> m_applied_inputs;

    LockstepStatus m_status;
    std::string m_dump_path;
    bool m_handshaken = false;
    bool m_disconnected = false; // local end issued/observed a disconnect
};

} // namespace Luminumbra::Net
