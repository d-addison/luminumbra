// delay-based lockstep transport implementation. See LockstepSession.h for
// the binding rationale (rollback was rejected after evaluating its ordering
// model; the adaptive horizon is measured in ticks and is hash-neutral). All on-wire
// integers are little-endian via explicit per-byte shifts and strings/blobs are
// length-prefixed, exactly mirroring ReplayStream's serialization discipline so the
// bytes are identical on every platform/compiler (no struct padding, no float wire form).

// winsock2.h MUST be included before <windows.h> (which spdlog/Log.h drags in),
// or its declarations conflict with winsock.h pulled by windows.h. Including it FIRST in
// this TU, guarded by _WIN32, satisfies that ordering for the TcpTransport impl below.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net/LockstepSession.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/EngineVersion.h"
#include "core/Log.h"
#include "replay/ReplayStream.h"

namespace Luminumbra::Net {
namespace {

namespace fs = std::filesystem;

bool HasValidHorizons(const HelloMsg& hello) {
    return hello.horizon_min > 0 && hello.horizon_min <= hello.horizon_start &&
           hello.horizon_start <= hello.horizon_max;
}

// --- Little-endian fixed-width append helpers (the only on-wire encoders). Identical
// discipline to ReplayStream.cpp's encoders; kept local so net has no dependency on the
// replay internals beyond the public ReplayWriter used for the dump. ----------------
void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutString(std::vector<std::uint8_t>& out, const std::string& s) {
    PutU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void PutBlob(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& b) {
    PutU32(out, static_cast<std::uint32_t>(b.size()));
    out.insert(out.end(), b.begin(), b.end());
}

// Sequential cursor reader; every Get* returns false on a short read so a garbled/
// truncated frame is detected rather than read as garbage (LREC1 Cursor discipline).
struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool ok = true;

    bool Remaining(std::size_t n) const {
        return ok && pos <= size && n <= size - pos;
    }
    bool GetU8(std::uint8_t& v) {
        if (!Remaining(1)) {
            ok = false;
            return false;
        }
        v = data[pos++];
        return true;
    }
    bool GetU16(std::uint16_t& v) {
        if (!Remaining(2)) {
            ok = false;
            return false;
        }
        v = static_cast<std::uint16_t>(data[pos]) |
            (static_cast<std::uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return true;
    }
    bool GetU32(std::uint32_t& v) {
        if (!Remaining(4)) {
            ok = false;
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
        pos += 4;
        return true;
    }
    bool GetU64(std::uint64_t& v) {
        if (!Remaining(8)) {
            ok = false;
            return false;
        }
        v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i);
        pos += 8;
        return true;
    }
    bool GetString(std::string& s) {
        std::uint32_t len = 0;
        if (!GetU32(len))
            return false;
        if (!Remaining(len)) {
            ok = false;
            return false;
        }
        s.assign(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return true;
    }
    bool GetBlob(std::vector<std::uint8_t>& b) {
        std::uint32_t len = 0;
        if (!GetU32(len))
            return false;
        if (!Remaining(len)) {
            ok = false;
            return false;
        }
        b.assign(data + pos, data + pos + len);
        pos += len;
        return true;
    }
};

// Wraps a payload in a frame: [u8 type][u32 payload-len][payload]. The explicit length
// lets the receiver detect a truncated frame and lets PeekMessageType read the tag alone.
std::vector<std::uint8_t> Frame(MessageType type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    PutU8(out, static_cast<std::uint8_t>(type));
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Opens a frame: validates the magic-free [type][len][payload] shape and returns a
// cursor over the payload. Returns false if the frame is too short or the length lies.
bool OpenFrame(const std::vector<std::uint8_t>& frame, MessageType expected, Cursor& payload) {
    Cursor c{frame.data(), frame.size(), 0, true};
    std::uint8_t type = 0;
    std::uint32_t len = 0;
    if (!c.GetU8(type) || !c.GetU32(len))
        return false;
    if (static_cast<MessageType>(type) != expected)
        return false;
    if (!c.Remaining(len))
        return false;
    payload = Cursor{frame.data() + c.pos, len, 0, true};
    return true;
}

constexpr std::size_t kInputFrameOverhead = 1 + 4 + 8 + 4 + 4;
constexpr std::size_t kHashFrameOverhead = 1 + 4 + 8 + 4 * 4;

// Inspect untrusted lengths and routing fields without allocating decoded blobs or
// strings. Session admission requires exactly one complete frame, with no trailing data.
bool InspectInput(const std::vector<std::uint8_t>& frame,
                  std::uint64_t& tick,
                  std::uint32_t& client_id) {
    Cursor c;
    std::uint32_t bytes = 0;
    return OpenFrame(frame, MessageType::Input, c) && c.size == frame.size() - 5 &&
           c.GetU64(tick) && c.GetU32(client_id) && c.GetU32(bytes) && c.Remaining(bytes) &&
           bytes == c.size - c.pos;
}

bool InspectHash(const std::vector<std::uint8_t>& frame, std::uint64_t& tick) {
    Cursor c;
    if (!OpenFrame(frame, MessageType::Hash, c) || c.size != frame.size() - 5 || !c.GetU64(tick))
        return false;
    for (int part = 0; part < 4; ++part) {
        std::uint32_t bytes = 0;
        if (!c.GetU32(bytes) || !c.Remaining(bytes))
            return false;
        c.pos += bytes;
    }
    return c.pos == c.size;
}

std::size_t HashFrameBytes(const HashMsg& hash) {
    // Only called for peer hashes whose complete encoded size was checked before decode.
    return kHashFrameOverhead + hash.world_hash.size() + hash.terrain.size() + hash.water.size() +
           hash.entities.size();
}

} // namespace

// --- Message encode/decode ---------------------------------------------------------

std::vector<std::uint8_t> EncodeHello(const HelloMsg& m) {
    std::vector<std::uint8_t> p;
    // A Hello payload leads with the magic so a stray/mis-protocol peer is caught at
    // handshake before any tick runs.
    p.insert(p.end(), kLockstepMagic, kLockstepMagic + sizeof(kLockstepMagic));
    PutU16(p, m.protocol_version);
    PutU64(p, m.seed);
    PutU16(p, m.tick_rate_hz);
    PutU32(p, m.client_id);
    PutString(p, m.preset);
    PutString(p, m.system_config_identity);
    PutString(p, m.preset_content_hash);
    PutU32(p, m.horizon_start);
    PutU32(p, m.horizon_min);
    PutU32(p, m.horizon_max);
    PutU64(p, m.hash_cadence_ticks);
    PutU64(p, m.max_frame_bytes);
    PutU64(p, m.retained_byte_limit);
    PutU64(p, m.retained_message_limit);
    return Frame(MessageType::Hello, p);
}

std::vector<std::uint8_t> EncodeInput(const InputMsg& m) {
    std::vector<std::uint8_t> p;
    PutU64(p, m.tick);
    PutU32(p, m.client_id);
    PutBlob(p, m.inputs);
    return Frame(MessageType::Input, p);
}

std::vector<std::uint8_t> EncodeHash(const HashMsg& m) {
    std::vector<std::uint8_t> p;
    PutU64(p, m.tick);
    PutString(p, m.world_hash);
    PutString(p, m.terrain);
    PutString(p, m.water);
    PutString(p, m.entities);
    return Frame(MessageType::Hash, p);
}

std::vector<std::uint8_t> EncodeBye(const ByeMsg& m) {
    std::vector<std::uint8_t> p;
    PutU64(p, m.tick);
    return Frame(MessageType::Bye, p);
}

bool PeekMessageType(const std::vector<std::uint8_t>& frame, MessageType& out_type) {
    if (frame.empty())
        return false;
    out_type = static_cast<MessageType>(frame[0]);
    return true;
}

bool DecodeHello(const std::vector<std::uint8_t>& frame, HelloMsg& out) {
    Cursor c;
    if (!OpenFrame(frame, MessageType::Hello, c))
        return false;
    char magic[sizeof(kLockstepMagic)] = {0};
    for (char& ch : magic) {
        std::uint8_t b = 0;
        if (!c.GetU8(b))
            return false;
        ch = static_cast<char>(b);
    }
    if (std::memcmp(magic, kLockstepMagic, sizeof(kLockstepMagic)) != 0)
        return false;
    if (!c.GetU16(out.protocol_version))
        return false;
    // Older Hellos lack the complete identity/admission contract. In particular,
    // v3 has preset identity but no negotiated horizon or retention settings.
    if (out.protocol_version != kLockstepProtocolVersion)
        return false;
    if (!c.GetU64(out.seed))
        return false;
    if (!c.GetU16(out.tick_rate_hz))
        return false;
    if (!c.GetU32(out.client_id))
        return false;
    if (!c.GetString(out.preset))
        return false;
    if (!c.GetString(out.system_config_identity))
        return false;
    if (!c.GetString(out.preset_content_hash))
        return false;
    // The shared decoder permits trailing bytes. Admission validates the exact
    // frame separately, without changing this decoder's compatibility contract.
    return c.GetU32(out.horizon_start) && c.GetU32(out.horizon_min) && c.GetU32(out.horizon_max) &&
           c.GetU64(out.hash_cadence_ticks) && c.GetU64(out.max_frame_bytes) &&
           c.GetU64(out.retained_byte_limit) && c.GetU64(out.retained_message_limit);
}

bool DecodeInput(const std::vector<std::uint8_t>& frame, InputMsg& out) {
    Cursor c;
    if (!OpenFrame(frame, MessageType::Input, c))
        return false;
    if (!c.GetU64(out.tick))
        return false;
    if (!c.GetU32(out.client_id))
        return false;
    if (!c.GetBlob(out.inputs))
        return false;
    return c.ok;
}

bool DecodeHash(const std::vector<std::uint8_t>& frame, HashMsg& out) {
    Cursor c;
    if (!OpenFrame(frame, MessageType::Hash, c))
        return false;
    if (!c.GetU64(out.tick))
        return false;
    if (!c.GetString(out.world_hash))
        return false;
    if (!c.GetString(out.terrain))
        return false;
    if (!c.GetString(out.water))
        return false;
    if (!c.GetString(out.entities))
        return false;
    return c.ok;
}

bool DecodeBye(const std::vector<std::uint8_t>& frame, ByeMsg& out) {
    Cursor c;
    if (!OpenFrame(frame, MessageType::Bye, c))
        return false;
    if (!c.GetU64(out.tick))
        return false;
    return c.ok;
}

// --- LoopbackTransport -------------------------------------------------------------

LoopbackTransport::LoopbackTransport(std::shared_ptr<Channel> tx, std::shared_ptr<Channel> rx)
    : m_tx(std::move(tx))
    , m_rx(std::move(rx)) {}

bool LoopbackTransport::SendFrame(const std::vector<std::uint8_t>& frame,
                                  FrameDelivery /*delivery*/) {
    if (frame.size() > kMaxFramePayloadBytes || !m_tx || !m_tx->open)
        return false;
    m_tx->queue.push_back(frame);
    return true;
}

bool LoopbackTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    if (!m_rx || m_rx->queue.empty())
        return false;
    out = std::move(m_rx->queue.front());
    m_rx->queue.pop_front();
    return true;
}

bool LoopbackTransport::IsPeerConnected() const {
    // The peer is connected while its SEND channel (our rx) is open OR still has buffered
    // messages we have not drained (a clean Bye/close should be observed AFTER its queued
    // frames, never before them).
    if (!m_rx)
        return false;
    return m_rx->open || !m_rx->queue.empty();
}

void LoopbackTransport::Close() {
    if (m_tx)
        m_tx->open = false; // our send channel closes -> the peer's rx sees it closed
}

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
MakeLoopbackPair() {
    auto a_to_b = std::make_shared<LoopbackTransport::Channel>();
    auto b_to_a = std::make_shared<LoopbackTransport::Channel>();
    // a sends into a_to_b, receives from b_to_a; b is the mirror.
    auto a = std::make_unique<LoopbackTransport>(a_to_b, b_to_a);
    auto b = std::make_unique<LoopbackTransport>(b_to_a, a_to_b);
    return {std::move(a), std::move(b)};
}

// --- TcpTransport (Winsock on Windows, POSIX sockets elsewhere) --------------------
// Wire framing: every frame is preceded by its u32 LE length, so the receiver can
// reassemble exactly one complete frame from a partially-arrived byte stream (TCP is a
// stream, not message-oriented). TryReceiveFrame is non-blocking: it pumps whatever bytes
// are available into m_recv_buffer and only returns a frame once a full one is present.

namespace {
constexpr std::size_t kTcpReadChunkBytes = 64u * 1024u;
// Read-ahead shares the frame buffer's budget rather than extending its maximum.
constexpr std::size_t kTcpReceiveBufferBytes = 4 + kMaxFramePayloadBytes;
} // namespace

std::size_t TcpTransport::ReceiveBytesNeeded() {
    const std::size_t buffered = m_recv_buffer.size() - m_recv_offset;
    if (buffered < 4)
        return 4 - buffered;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[m_recv_offset + i]) << (8 * i);
    if (frame_len > kMaxFramePayloadBytes) {
        Close();
        return 0;
    }
    const std::size_t buffered_payload = buffered - 4;
    // Read-ahead may contain later frames. Inspect only the front prefix so valid
    // frames are delivered before an oversized prefix or EOF behind them.
    if (buffered_payload >= frame_len)
        return 0;
    return static_cast<std::size_t>(frame_len) - buffered_payload;
}

void TcpTransport::CompactReceiveBuffer() {
    if (m_recv_offset == 0)
        return;
    // Complete buffered frames drain by advancing the cursor. Only move the trailing
    // incomplete frame when another socket read is needed, at most once per read batch.
    m_recv_buffer.erase(m_recv_buffer.begin(), m_recv_buffer.begin() + m_recv_offset);
    m_recv_offset = 0;
}

bool TcpTransport::IsPeerConnected() const {
    if (m_socket < 0)
        return false;
    if (!m_peer_closed)
        return true;
    // A send failure must not hide complete frames already received. A partial
    // tail or an oversized prefix cannot keep a closed peer connected.
    const std::size_t buffered = m_recv_buffer.size() - m_recv_offset;
    if (buffered < 4)
        return false;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[m_recv_offset + i]) << (8 * i);
    return frame_len <= kMaxFramePayloadBytes && frame_len <= buffered - 4;
}

#ifdef _WIN32

namespace {
// winsock2 must precede windows.h; spdlog already pulled windows.h in via Log.h, but the
// winsock symbols we need are declared by including winsock2.h here (include guards make
// the duplicate windows.h include from Log.h harmless because winsock2.h is first in THIS
// TU's net-specific section). We include them at file scope below instead.
} // namespace

bool TcpTransport::PumpRecv() {
    if (m_socket < 0)
        return false;
    // Opportunistically push any queued outbound bytes (non-blocking; no spin) whenever
    // we pump -- an earlier would-block may have left a remainder that can now go out.
    if (!m_send_q.Empty() && FlushSendNonBlocking() < 0) {
        m_peer_closed = true;
    }
    char buf[kTcpReadChunkBytes];
    for (;;) {
        if (ReceiveBytesNeeded() == 0)
            return m_socket >= 0;
        CompactReceiveBuffer();
        // Read beyond the current frame, bounded even for a maximum-size payload.
        // Stop pumping once the front frame is complete; drain buffered frames first.
        const std::size_t want =
            std::min(sizeof(buf), kTcpReceiveBufferBytes - m_recv_buffer.size());
        const int n = ::recv(static_cast<SOCKET>(m_socket), buf, static_cast<int>(want), 0);
        if (n > 0) {
            if (m_recv_buffer.size() + n > m_recv_buffer.capacity())
                m_recv_buffer.reserve(
                    std::min(kTcpReceiveBufferBytes,
                             std::max(m_recv_buffer.size() + n, m_recv_buffer.capacity() * 2)));
            m_recv_buffer.insert(m_recv_buffer.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            // recv runs only while the front frame is incomplete (the loop returns as
            // soon as it is complete), so EOF can discard only a truncated tail.
            Close();
            return true;
        } // graceful close
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return true; // no more data right now
        Close();         // real error -> treat as disconnect (clean end)
        return true;
    }
}

TcpTransport::TcpTransport() {
    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
}

TcpTransport::~TcpTransport() {
    Close();
    ::WSACleanup();
}

bool TcpTransport::Listen(std::uint16_t port, int timeout_ms) {
    m_listen_socket = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (m_listen_socket < 0)
        return false;
    BOOL reuse = TRUE;
    ::setsockopt(static_cast<SOCKET>(m_listen_socket),
                 SOL_SOCKET,
                 SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);
    if (::bind(static_cast<SOCKET>(m_listen_socket),
               reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        Close();
        return false;
    }
    if (::listen(static_cast<SOCKET>(m_listen_socket), 1) != 0) {
        Close();
        return false;
    }

    // Accept ONE client, bounded by timeout_ms via select (no hang -- regression review hygiene).
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<SOCKET>(m_listen_socket), &rfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(0, &rfds, nullptr, nullptr, &tv) <= 0) {
        Close();
        return false;
    }
    m_socket = static_cast<std::intptr_t>(
        ::accept(static_cast<SOCKET>(m_listen_socket), nullptr, nullptr));
    if (m_socket < 0) {
        Close();
        return false;
    }
    u_long nonblock = 1;
    ::ioctlsocket(static_cast<SOCKET>(m_socket), FIONBIO, &nonblock);
    return true;
}

bool TcpTransport::Connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    m_socket = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (m_socket < 0)
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        Close();
        return false;
    }
    (void)timeout_ms; // blocking connect for v1 (loopback/LAN); timeout via OS default
    if (::connect(
            static_cast<SOCKET>(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Close();
        return false;
    }
    u_long nonblock = 1;
    ::ioctlsocket(static_cast<SOCKET>(m_socket), FIONBIO, &nonblock);
    return true;
}

int TcpTransport::FlushSendNonBlocking() {
    if (m_socket < 0)
        return -1;
    // Push as much of the queued outbound bytes as the kernel send buffer will take right
    // now. DrainOnce advances while::send makes progress and STOPS the instant it
    // would-blocks (returns 0) -- it never spins on a zero-progress send. The unsent remainder
    // stays queued for the next flush; nothing is dropped.
    return m_send_q.DrainOnce([this](const std::uint8_t* p, std::size_t len) -> int {
        const int want = static_cast<int>(std::min<std::size_t>(len, 1u << 20));
        const int n =
            ::send(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(p), want, 0);
        if (n > 0)
            return n;
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0; // would-block -> stop draining (no busy-spin)
        return -1;    // real error -> fatal/disconnect
    });
}

bool TcpTransport::SendFrame(const std::vector<std::uint8_t>& frame, FrameDelivery /*delivery*/) {
    if (frame.size() > kMaxFramePayloadBytes || m_socket < 0 || m_peer_closed)
        return false;
    // Frame = [u32 LE len][payload]; APPEND it whole to the bounded outbound queue. The
    // queue never drops a message.
    std::vector<std::uint8_t> out;
    PutU32(out, static_cast<std::uint32_t>(frame.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    m_send_q.Append(out.data(), out.size());

    // Opportunistic non-blocking flush; a would-block leaves the remainder queued.
    if (FlushSendNonBlocking() < 0) {
        m_peer_closed = true;
        return false;
    }

    // High-water backpressure: if the kernel send buffer is saturated and the queue has
    // grown past the high-water mark, BLOCK on socket writability via select and keep
    // draining -- a BOUNDED blocking wait, NEVER a busy-spin -- until we are back under
    // the mark. The loop makes guaranteed progress each pass (drain-below-mark and exit,
    // or select-timeout and bail), so it cannot iterate unboundedly. A peer that stops
    // draining (select timeout) is treated as gone: the queued bytes are retained and the
    // failure is surfaced to the caller rather than dropped.
    while (m_send_q.OverHighWater()) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(static_cast<SOCKET>(m_socket), &wfds);
        timeval tv{5, 0}; // 5s ceiling per writability wait
        const int sel = ::select(0, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            m_peer_closed = true;
            return false;
        } // peer not draining
        if (FlushSendNonBlocking() < 0) {
            m_peer_closed = true;
            return false;
        }
    }
    return true;
}

bool TcpTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    PumpRecv();
    // Need at least the u32 length prefix.
    const std::size_t buffered = m_recv_buffer.size() - m_recv_offset;
    if (buffered < 4)
        return false;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[m_recv_offset + i]) << (8 * i);
    if (buffered - 4 < static_cast<std::size_t>(frame_len))
        return false; // frame not fully arrived
    const auto payload = m_recv_buffer.begin() + m_recv_offset + 4;
    out.assign(payload, payload + frame_len);
    m_recv_offset += 4 + frame_len;
    if (m_recv_offset == m_recv_buffer.size()) {
        m_recv_buffer.clear();
        m_recv_offset = 0;
    }
    return true;
}

void TcpTransport::Close() {
    std::vector<std::uint8_t>().swap(m_recv_buffer);
    m_recv_offset = 0;
    if (m_socket >= 0) {
        ::shutdown(static_cast<SOCKET>(m_socket), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(m_socket));
        m_socket = -1;
    }
    if (m_listen_socket >= 0) {
        ::closesocket(static_cast<SOCKET>(m_listen_socket));
        m_listen_socket = -1;
    }
}

// ---: single-listen-socket fan-out (TcpTransport::FromAcceptedSocket + TcpListener)
std::unique_ptr<TcpTransport> TcpTransport::FromAcceptedSocket(std::intptr_t sock) {
    if (sock < 0)
        return nullptr;
    auto t = std::make_unique<TcpTransport>();
    t->m_socket = sock;
    // Windows does not reliably inherit non-blocking from the listen socket; set it here
    // so the accepted transport behaves exactly like a Connect/Listen-produced one.
    u_long nonblock = 1;
    ::ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &nonblock);
    return t;
}

TcpListener::TcpListener() {
    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
}

TcpListener::~TcpListener() {
    Close();
    ::WSACleanup();
}

bool TcpListener::Listen(std::uint16_t port, int backlog) {
    m_listen_socket = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (m_listen_socket < 0)
        return false;
    BOOL reuse = TRUE;
    ::setsockopt(static_cast<SOCKET>(m_listen_socket),
                 SOL_SOCKET,
                 SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);
    if (::bind(static_cast<SOCKET>(m_listen_socket),
               reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        Close();
        return false;
    }
    if (::listen(static_cast<SOCKET>(m_listen_socket), backlog) != 0) {
        Close();
        return false;
    }
    // Read back the actual bound port (Listen(0) => OS-assigned ephemeral) in HOST order.
    sockaddr_in bound{};
    int bound_len = static_cast<int>(sizeof(bound));
    if (::getsockname(static_cast<SOCKET>(m_listen_socket),
                      reinterpret_cast<sockaddr*>(&bound),
                      &bound_len) == 0) {
        m_port = ::ntohs(bound.sin_port);
    } else {
        m_port = port;
    }
    // Non-blocking listen socket so AcceptOne never blocks the server tick.
    u_long nonblock = 1;
    ::ioctlsocket(static_cast<SOCKET>(m_listen_socket), FIONBIO, &nonblock);
    return true;
}

std::unique_ptr<ILockstepTransport> TcpListener::AcceptOne() {
    if (m_listen_socket < 0)
        return nullptr;
    const std::intptr_t sock = static_cast<std::intptr_t>(
        ::accept(static_cast<SOCKET>(m_listen_socket), nullptr, nullptr));
    if (sock < 0)
        return nullptr; // WSAEWOULDBLOCK (nothing pending) or error -> none now
    return TcpTransport::FromAcceptedSocket(sock);
}

std::unique_ptr<TcpTransport> TcpListener::AcceptOneBlocking(int timeout_ms) {
    if (m_listen_socket < 0)
        return nullptr;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<SOCKET>(m_listen_socket), &rfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(0, &rfds, nullptr, nullptr, &tv) <= 0)
        return nullptr; // timeout/error
    const std::intptr_t sock = static_cast<std::intptr_t>(
        ::accept(static_cast<SOCKET>(m_listen_socket), nullptr, nullptr));
    if (sock < 0)
        return nullptr;
    return TcpTransport::FromAcceptedSocket(sock);
}

void TcpListener::Close() {
    if (m_listen_socket >= 0) {
        ::closesocket(static_cast<SOCKET>(m_listen_socket));
        m_listen_socket = -1;
    }
}

#else // !_WIN32 -- POSIX sockets

namespace {

bool SetNonBlocking(std::intptr_t socket) {
    const int fd = static_cast<int>(socket);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void CloseSocket(std::intptr_t& socket) {
    if (socket < 0)
        return;
    ::close(static_cast<int>(socket));
    socket = -1;
}

} // namespace

TcpTransport::TcpTransport() = default;
TcpTransport::~TcpTransport() {
    Close();
}

bool TcpTransport::PumpRecv() {
    if (m_socket < 0)
        return false;
    if (!m_send_q.Empty() && FlushSendNonBlocking() < 0)
        m_peer_closed = true;
    char buf[kTcpReadChunkBytes];
    for (;;) {
        if (ReceiveBytesNeeded() == 0)
            return m_socket >= 0;
        CompactReceiveBuffer();
        const std::size_t want =
            std::min(sizeof(buf), kTcpReceiveBufferBytes - m_recv_buffer.size());
        const ssize_t n = ::recv(static_cast<int>(m_socket), buf, want, 0);
        if (n > 0) {
            if (m_recv_buffer.size() + n > m_recv_buffer.capacity())
                m_recv_buffer.reserve(
                    std::min(kTcpReceiveBufferBytes,
                             std::max(m_recv_buffer.size() + n, m_recv_buffer.capacity() * 2)));
            m_recv_buffer.insert(m_recv_buffer.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            // recv runs only while the front frame is incomplete (the loop returns as
            // soon as it is complete), so EOF can discard only a truncated tail.
            Close();
            return true;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        Close();
        return true;
    }
}

bool TcpTransport::Listen(std::uint16_t port, int timeout_ms) {
    m_listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_socket < 0)
        return false;
    int reuse = 1;
    ::setsockopt(
        static_cast<int>(m_listen_socket), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(static_cast<int>(m_listen_socket),
               reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(static_cast<int>(m_listen_socket), 1) != 0) {
        Close();
        return false;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<int>(m_listen_socket), &rfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(static_cast<int>(m_listen_socket) + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
        Close();
        return false;
    }
    m_socket = ::accept(static_cast<int>(m_listen_socket), nullptr, nullptr);
    if (m_socket < 0 || !SetNonBlocking(m_socket)) {
        Close();
        return false;
    }
    return true;
}

bool TcpTransport::Connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket < 0 || !SetNonBlocking(m_socket)) {
        Close();
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        Close();
        return false;
    }
    const int rc =
        ::connect(static_cast<int>(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        Close();
        return false;
    }
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(static_cast<int>(m_socket), &wfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(static_cast<int>(m_socket) + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
            Close();
            return false;
        }
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (::getsockopt(
                static_cast<int>(m_socket), SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
            socket_error != 0) {
            Close();
            return false;
        }
    }
    return true;
}

int TcpTransport::FlushSendNonBlocking() {
    if (m_socket < 0)
        return -1;
    return m_send_q.DrainOnce([this](const std::uint8_t* p, std::size_t len) -> int {
        const std::size_t want = std::min<std::size_t>(len, 1u << 20);
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(static_cast<int>(m_socket), p, want, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(static_cast<int>(m_socket), p, want, 0);
#endif
        if (n > 0)
            return static_cast<int>(n);
        if (n < 0 && errno == EINTR)
            return 0;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    });
}

bool TcpTransport::SendFrame(const std::vector<std::uint8_t>& frame, FrameDelivery /*delivery*/) {
    if (frame.size() > kMaxFramePayloadBytes || m_socket < 0 || m_peer_closed)
        return false;
    std::vector<std::uint8_t> out;
    PutU32(out, static_cast<std::uint32_t>(frame.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    m_send_q.Append(out.data(), out.size());
    if (FlushSendNonBlocking() < 0) {
        m_peer_closed = true;
        return false;
    }
    while (m_send_q.OverHighWater()) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(static_cast<int>(m_socket), &wfds);
        timeval tv{5, 0};
        if (::select(static_cast<int>(m_socket) + 1, nullptr, &wfds, nullptr, &tv) <= 0 ||
            FlushSendNonBlocking() < 0) {
            m_peer_closed = true;
            return false;
        }
    }
    return true;
}

bool TcpTransport::TryReceiveFrame(std::vector<std::uint8_t>& out) {
    PumpRecv();
    const std::size_t buffered = m_recv_buffer.size() - m_recv_offset;
    if (buffered < 4)
        return false;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[m_recv_offset + i]) << (8 * i);
    if (buffered - 4 < static_cast<std::size_t>(frame_len))
        return false;
    const auto payload = m_recv_buffer.begin() + m_recv_offset + 4;
    out.assign(payload, payload + frame_len);
    m_recv_offset += 4 + frame_len;
    if (m_recv_offset == m_recv_buffer.size()) {
        m_recv_buffer.clear();
        m_recv_offset = 0;
    }
    return true;
}

void TcpTransport::Close() {
    std::vector<std::uint8_t>().swap(m_recv_buffer);
    m_recv_offset = 0;
    if (m_socket >= 0)
        ::shutdown(static_cast<int>(m_socket), SHUT_RDWR);
    CloseSocket(m_socket);
    CloseSocket(m_listen_socket);
}

std::unique_ptr<TcpTransport> TcpTransport::FromAcceptedSocket(std::intptr_t socket) {
    if (socket < 0 || !SetNonBlocking(socket)) {
        if (socket >= 0)
            ::close(static_cast<int>(socket));
        return nullptr;
    }
    auto transport = std::make_unique<TcpTransport>();
    transport->m_socket = socket;
    return transport;
}

TcpListener::TcpListener() = default;
TcpListener::~TcpListener() {
    Close();
}

bool TcpListener::Listen(std::uint16_t port, int backlog) {
    m_listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_socket < 0)
        return false;
    int reuse = 1;
    ::setsockopt(
        static_cast<int>(m_listen_socket), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(static_cast<int>(m_listen_socket),
               reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(static_cast<int>(m_listen_socket), backlog) != 0) {
        Close();
        return false;
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(static_cast<int>(m_listen_socket),
                      reinterpret_cast<sockaddr*>(&bound),
                      &bound_len) == 0) {
        m_port = ntohs(bound.sin_port);
    } else {
        m_port = port;
    }
    if (!SetNonBlocking(m_listen_socket)) {
        Close();
        return false;
    }
    return true;
}

std::unique_ptr<ILockstepTransport> TcpListener::AcceptOne() {
    if (m_listen_socket < 0)
        return nullptr;
    const std::intptr_t socket = ::accept(static_cast<int>(m_listen_socket), nullptr, nullptr);
    if (socket < 0)
        return nullptr;
    return TcpTransport::FromAcceptedSocket(socket);
}

std::unique_ptr<TcpTransport> TcpListener::AcceptOneBlocking(int timeout_ms) {
    if (m_listen_socket < 0)
        return nullptr;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<int>(m_listen_socket), &rfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(static_cast<int>(m_listen_socket) + 1, &rfds, nullptr, nullptr, &tv) <= 0)
        return nullptr;
    const std::intptr_t socket = ::accept(static_cast<int>(m_listen_socket), nullptr, nullptr);
    if (socket < 0)
        return nullptr;
    return TcpTransport::FromAcceptedSocket(socket);
}

void TcpListener::Close() {
    CloseSocket(m_listen_socket);
    m_port = 0;
}

#endif // _WIN32

// --- LockstepSession ---------------------------------------------------------------

HelloMsg MakeLockstepHello(const LockstepConfig& config, std::size_t transport_frame_limit) {
    HelloMsg mine;
    mine.protocol_version = kLockstepProtocolVersion;
    mine.system_config_identity = config.system_config_identity;
    mine.preset_content_hash = config.preset_content_hash;
    mine.seed = config.seed;
    mine.preset = config.preset;
    mine.tick_rate_hz = config.tick_rate_hz;
    mine.client_id = config.local_client_id;
    mine.horizon_start = config.horizon_start;
    mine.horizon_min = config.horizon_min;
    mine.horizon_max = config.horizon_max;
    mine.hash_cadence_ticks = config.hash_cadence_ticks;
    mine.max_frame_bytes =
        std::min({kMaxFramePayloadBytes, transport_frame_limit, config.max_frame_payload_bytes});
    mine.retained_byte_limit = config.peer_retained_byte_limit;
    mine.retained_message_limit = config.peer_retained_message_limit;
    return mine;
}

LockstepSession::LockstepSession(LockstepConfig config,
                                 ILockstepTransport* transport,
                                 LockstepHooks hooks)
    : m_config(std::move(config))
    , m_transport(transport)
    , m_hooks(hooks)
    , m_replay(std::make_unique<Replay::QueuedReplayRecorder>()) {
    m_horizon = std::max(m_config.horizon_min, m_config.horizon_start);
    m_status.horizon = m_horizon;
    m_status.max_horizon_reached = m_horizon;
    if (m_dump_path.empty()) {
        m_dump_path = (fs::temp_directory_path() /
                       ("lockstep-desync-" + std::to_string(m_config.local_client_id) + ".lrec1"))
                          .string();
    }
}

LockstepSession::~LockstepSession() = default;

bool LockstepSession::FlushReplayRecorder() {
    return m_replay->Flush() && m_replay_open;
}

LockstepAdmissionStatus LockstepSession::AdmissionStatus() const {
    return {.input_ticks = m_inputs.size(),
            .peer_inputs = m_peer_input_count,
            .peer_hashes = m_peer_hashes.size(),
            .retained_bytes = m_retained_peer_bytes,
            .confirmed_hash_tick = m_confirmed_peer_hash_tick,
            .frame_limit = m_frame_limit,
            .message_limit = m_peer_message_limit,
            .byte_limit = m_peer_byte_limit,
            .applied_inputs = m_applied_inputs.size(),
            .applied_input_bytes = m_applied_input_bytes,
            .local_hashes = m_local_hashes.size(),
            .checkpoint_window = m_checkpoint_window,
            .replay_bytes = m_replay->RetainedBytes(),
            .replay_byte_limit = m_replay->ByteLimit(),
            .replay_segments = m_replay->SegmentCount()};
}

bool LockstepSession::Handshake() {
    if (m_disconnect_pending || m_status.peer_disconnected || m_status.desynced)
        return false;
    if (m_handshaken)
        return true;
    m_receive_work = {};
    const auto refuse = [this] {
        TransportDisconnected();
        return false;
    };
    // Send our Hello, then drain until the peer's Hello arrives (loopback: the peer's
    // Hello must already be queued by the single-process driver). A mismatched
    // protocol/version/seed/preset/tick-rate/config identity is rejected loudly.
    // A mis-paired session would otherwise silently desync.
    const auto mine = MakeLockstepHello(m_config, m_transport->MaxFramePayloadBytes());
    if (!HasValidHorizons(mine)) {
        m_status.handshake_error =
            "invalid local lockstep horizons: require 0 < min <= start <= max";
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: {}", m_config.local_client_id, m_status.handshake_error);
        return refuse();
    }
    if (!m_hello_sent && !m_transport->SendFrame(EncodeHello(mine))) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: handshake send refused", m_config.local_client_id);
        // A connected backend can refuse transiently without closing. No
        // Hello has been sent or peer frame consumed, so a later call may retry.
        return m_transport->IsPeerConnected() ? false : refuse();
    }
    m_hello_sent = true;

    // Drain frames looking for the peer Hello. Non-Hello frames before the handshake are
    // a protocol error.
    std::vector<std::uint8_t> frame;
    bool got_peer = false;
    HelloMsg peer;
    // Exactly one Hello poll, charged to the same budget as scheduling refusals.
    --m_receive_work.polls_left;
    if (m_transport->TryReceiveFrame(frame)) {
        m_receive_work.payload_bytes += frame.size();
        MessageType type;
        if (!PeekMessageType(frame, type) || type != MessageType::Hello) {
            LUMINUMBRA_CORE_ERROR("lockstep[{}]: expected Hello first, got type {}",
                                  m_config.local_client_id,
                                  frame.empty() ? -1 : static_cast<int>(frame[0]));
            return refuse();
        }
        if (!DecodeHello(frame, peer)) {
            LUMINUMBRA_CORE_ERROR("lockstep[{}]: malformed or unsupported Hello version (requires "
                                  "Hello v4 identity and admission limits)",
                                  m_config.local_client_id);
            return refuse();
        }
        got_peer = true;
    }
    if (!got_peer) {
        // No frame yet is a pending handshake, not an identity refusal.
        return m_transport->IsPeerConnected() ? false : refuse();
    }

    if (peer.protocol_version != mine.protocol_version) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: protocol version mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.protocol_version,
                              peer.protocol_version);
        return refuse();
    }
    if (peer.seed != mine.seed) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: seed mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.seed,
                              peer.seed);
        return refuse();
    }
    if (peer.preset != mine.preset) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: preset mismatch (local '{}' != peer '{}')",
                              m_config.local_client_id,
                              mine.preset,
                              peer.preset);
        return refuse();
    }
    if (peer.tick_rate_hz != mine.tick_rate_hz) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: tick-rate mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.tick_rate_hz,
                              peer.tick_rate_hz);
        return refuse();
    }

    if (mine.preset_content_hash.empty() || peer.preset_content_hash.empty() ||
        mine.preset_content_hash != peer.preset_content_hash) {
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: resolved preset/species content hash mismatch or missing identity",
            m_config.local_client_id);
        return refuse();
    }
    if (!HelloMatches(mine, peer)) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: simulation configuration identity mismatch",
                              m_config.local_client_id);
        return refuse();
    }

    // This is a direct two-peer protocol, with no input relay role. Bind Hello to
    // the caller's expected remote slot rather than letting the sender select it.
    if (peer.client_id != m_config.peer_client_id || peer.client_id == m_config.local_client_id) {
        ++m_status.rejected_peer_messages;
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: unexpected peer client id {}", m_config.local_client_id, peer.client_id);
        return refuse();
    }
    if (!ConfigureRetention(mine, peer)) {
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: {}", m_config.local_client_id, m_status.handshake_error);
        return refuse();
    }
    // Disk setup and worker startup belong to handshake, never the tick path.
    // Recorder failure affects diagnostics only, not handshake or simulation.
    OpenReplayRecorder();
    // Pre-schedule the local input for ticks [1.. horizon]: delay-based lockstep sends
    // inputs `horizon` ticks ahead, so the first `horizon` ticks' local inputs are in
    // flight before the first tick runs.
    for (std::uint64_t t = 1; t <= m_horizon; ++t) {
        if (!ScheduleLocalInput(t))
            return refuse();
    }
    m_handshaken = true;
    m_status.handshaken = true;
    return true;
}

void LockstepSession::TransportDisconnected(std::optional<std::uint64_t> stop_tick) {
    if (!m_disconnect_pending) {
        m_disconnect_pending = true;
        m_status.disconnect_tick = stop_tick.value_or(m_agreed_tick);
        // Do not open new admission slots while a local close drains in batches.
        if (!m_transport_close_admission_tick)
            m_transport_close_admission_tick = m_agreed_tick;
    }
    // Closing can discard queued frames. Compare all available hashes for executed
    // ticks first, even when the final send (including Bye) was rejected.
    if (!m_status.desynced) {
        // A peer hash may have been buffered before its local tick was executed.
        // The local cadence hash is now captured before any post-step send.
        // Earlier executed checkpoints compared on receipt or immediately after
        // their step. Only this tick can have been captured just before a send
        // failed; do not rescan the retained lookahead on every closure batch.
        const auto checkpoint = m_peer_hashes.find(m_agreed_tick);
        if (checkpoint != m_peer_hashes.end())
            ComparePeerHash(checkpoint->second);
        if (!m_status.desynced)
            DrainPeerMessages();
    }
    if (!m_status.desynced && m_receive_drain_pending)
        return;
    m_transport->Close();
    m_disconnected = true;
    m_status.peer_disconnected = true;
    m_pending_outbound.clear();
    m_pending_outbound_bytes = 0;
    ReleaseRetainedMessages();
}

bool LockstepSession::SendOrQueueFrame(std::vector<std::uint8_t> frame) {
    if (frame.size() > std::min(kMaxFramePayloadBytes, m_transport->MaxFramePayloadBytes())) {
        TransportDisconnected();
        return false;
    }
    if (m_pending_outbound.empty()) {
        if (m_transport->SendFrame(frame))
            return true;
        // A terminal backend refusal reports closure after its received frames drain.
        if (!DrainPeerMessages() || (!m_received_bye_tick && !m_transport->IsPeerConnected())) {
            TransportDisconnected();
            return false;
        }
        // Bye can arrive during the refusal drain. Its sender no longer needs
        // this frame, so do not recreate the retry queue just discarded by that drain.
        if (m_received_bye_tick)
            return true;
    }
    // Subtract before adding, so neither the capacity check nor accounting overflows.
    if (m_pending_outbound.size() >= kPendingOutboundFrameLimit ||
        frame.size() > kPendingOutboundByteLimit - m_pending_outbound_bytes) {
        TransportDisconnected();
        return false;
    }
    m_pending_outbound_bytes += frame.size();
    m_pending_outbound.push_back(std::move(frame));
    return true;
}

bool LockstepSession::FlushPendingOutbound() {
    // Bye discards obsolete retries; received inputs and hashes still drain through T.
    while (!m_pending_outbound.empty() && !m_received_bye_tick) {
        const auto& frame = m_pending_outbound.front();
        if (!m_transport->SendFrame(frame)) {
            if (!DrainPeerMessages() || (!m_received_bye_tick && !m_transport->IsPeerConnected())) {
                TransportDisconnected();
                return false;
            }
            return true;
        }
        m_pending_outbound_bytes -= frame.size();
        m_pending_outbound.pop_front();
    }
    return true;
}

bool LockstepSession::ScheduleLocalInput(std::uint64_t tick) {
    if (tick <= m_local_input_scheduled_through)
        return true;
    std::vector<std::uint8_t> blob;
    if (m_hooks.collect_local_input) {
        blob = m_hooks.collect_local_input(tick, m_hooks.user);
    }
    // Input frame: type + payload length + tick + client id + blob length + blob.
    constexpr std::size_t overhead = 1 + 4 + 8 + 4 + 4;
    const auto limit = std::min(m_frame_limit, m_transport->MaxFramePayloadBytes());
    if (limit < overhead || blob.size() > limit - overhead) {
        TransportDisconnected();
        return false;
    }
    // During closure, buffered remote inputs can outlast our scheduled horizon.
    // Collect the remaining local slots without sending into a closed session.
    const bool closing = m_handshaken && (m_received_bye_tick || m_transport_close_admission_tick ||
                                          !m_transport->IsPeerConnected());
    if (!closing) {
        const InputMsg msg{.tick = tick, .client_id = m_config.local_client_id, .inputs = blob};
        if (!SendOrQueueFrame(EncodeInput(msg)))
            return false;
    }
    // Buffer locally once accepted/retained for reliable retry, or while draining.
    m_inputs[tick][m_config.local_client_id] = std::move(blob);
    m_local_input_scheduled_through = tick;
    return true;
}

bool LockstepSession::ComparePeerHash(PeerCheckpoint& checkpoint) {
    if (checkpoint.compared)
        return true;
    const auto& peer = checkpoint.hash;
    const auto local = m_local_hashes.find(peer.tick);
    if (local == m_local_hashes.end())
        return true;
    if (local->second->world_hash == peer.world_hash) {
        // An out-of-order match needs only a tick marker until the missing prefix
        // arrives. Do not keep peer-controlled strings for already compared ticks.
        const auto payload_bytes = HashFrameBytes(peer) - kHashFrameOverhead;
        // Move assignment of an empty string may retain the destination's heap
        // allocation. Swap explicitly before discounting the payload storage.
        std::string{}.swap(checkpoint.hash.world_hash);
        std::string{}.swap(checkpoint.hash.terrain);
        std::string{}.swap(checkpoint.hash.water);
        std::string{}.swap(checkpoint.hash.entities);
        m_retained_peer_bytes -= payload_bytes;
        checkpoint.compared = true;
        return true;
    }
    const std::string section = CheckpointMismatchSection(*local->second, peer);
    EmitDesyncDump(peer.tick, section);
    return false;
}

std::optional<std::size_t> LockstepSession::PeerHashStringCapacity(std::uint64_t tick) const {
    const auto checkpoint = m_peer_hashes.find(tick);
    if (checkpoint == m_peer_hashes.end())
        return std::nullopt;
    const auto& hash = checkpoint->second.hash;
    return hash.world_hash.capacity() + hash.terrain.capacity() + hash.water.capacity() +
           hash.entities.capacity();
}

bool LockstepSession::IsWithinPeerLookahead(std::uint64_t tick) const {
    // The remote can have consumed all inputs we scheduled (one maximum horizon
    // ahead), then scheduled its own next horizon. Handshake validates min/start/max.
    const auto lookahead = std::uint64_t{2} * m_config.horizon_max;
    // Closure can execute buffered inputs, but must not open additional hash
    // slots. Late oracle messages use the window at the first observed closure.
    const auto anchor = m_received_bye_tick
                            ? m_bye_admission_tick
                            : m_transport_close_admission_tick.value_or(m_agreed_tick);
    return tick <= anchor || tick - anchor <= lookahead;
}

bool LockstepSession::IsRetainedPeerCheckpoint(std::uint64_t tick) const {
    const auto cadence = m_config.hash_cadence_ticks;
    if (cadence == 0 || tick == 0 || tick % cadence != 0 || tick <= m_confirmed_peer_hash_tick)
        return false;
    // Retain only the unconfirmed checkpoint window plus both horizons of
    // remote lead. Divide before subtracting: neither tick arithmetic nor the
    // window calculation can overflow, even at the last representable tick.
    const auto lead = std::uint64_t{2} * m_config.horizon_max;
    const auto lead_checkpoints = lead / cadence + (lead % cadence != 0);
    if (tick / cadence - m_confirmed_peer_hash_tick / cadence >
        m_checkpoint_window + lead_checkpoints)
        return false;
    // Executed ticks must still have their local oracle. Never retain a hash
    // that cannot be compared, even if a missing prefix prevents confirmation.
    // Local cadence entries form a contiguous suffix; checking its oldest tick
    // keeps this admission check constant time.
    return tick > m_agreed_tick ||
           (!m_local_hashes.empty() && tick >= m_local_hashes.begin()->first);
}

bool LockstepSession::CanRetainPeerFrame(std::size_t bytes) const {
    // Shared Input/Hash budget, checked before decoding any peer-owned payload.
    // Subtraction also refuses inconsistent accounting without overflowing.
    const LockstepAdmissionStatus usage{.peer_inputs = m_peer_input_count,
                                        .peer_hashes = m_peer_hashes.size(),
                                        .retained_bytes = m_retained_peer_bytes,
                                        .message_limit = m_peer_message_limit,
                                        .byte_limit = m_peer_byte_limit};
    return HasPeerRetentionCapacity(usage, bytes);
}

bool LockstepSession::ConfigureRetention(const HelloMsg& mine, const HelloMsg& peer) {
    if (!HasValidHorizons(peer)) {
        m_status.handshake_error =
            "invalid peer lockstep horizons: require 0 < min <= start <= max";
        return false;
    }
    if (mine.horizon_start != peer.horizon_start || mine.horizon_min != peer.horizon_min ||
        mine.horizon_max != peer.horizon_max ||
        mine.hash_cadence_ticks != peer.hash_cadence_ticks ||
        mine.retained_byte_limit != peer.retained_byte_limit ||
        mine.retained_message_limit != peer.retained_message_limit) {
        m_status.handshake_error = "incompatible lockstep horizons, cadence or retention limits";
        return false;
    }
    const auto horizon = mine.horizon_max;
    const auto frame_limit = std::min(mine.max_frame_bytes, peer.max_frame_bytes);
    const auto required_overhead =
        mine.hash_cadence_ticks == 0 ? kInputFrameOverhead : kHashFrameOverhead;
    if (frame_limit < required_overhead || peer.max_frame_bytes > kMaxFramePayloadBytes) {
        m_status.handshake_error = "invalid lockstep horizon or maximum frame size";
        return false;
    }
    m_frame_limit = static_cast<std::size_t>(frame_limit);
    const auto lead = std::uint64_t{2} * horizon;
    // A receive queue of delayed checkpoints, both input horizons, and the next
    // required checkpoint. Sender pacing bounds unconfirmed local history; the
    // extra remote lead slots make admission lossless even with asymmetric delay.
    const auto cadence = mine.hash_cadence_ticks;
    const auto lead_checkpoints = cadence == 0 ? 0 : lead / cadence + (lead % cadence != 0);
    m_checkpoint_window = cadence == 0 ? 0 : kReceiveQueueFrameLimit + lead_checkpoints + 1;
    const auto hash_slots = m_checkpoint_window + lead_checkpoints;
    const auto messages = lead + hash_slots;
    const auto size_max = (std::numeric_limits<std::size_t>::max)();
    if (messages > size_max / m_frame_limit) {
        m_status.handshake_error = "lockstep retention size overflows this platform";
        return false;
    }
    m_peer_message_limit = static_cast<std::size_t>(messages);
    m_peer_byte_limit = m_peer_message_limit * m_frame_limit;
    if ((mine.retained_message_limit != 0 && mine.retained_message_limit < messages) ||
        (mine.retained_byte_limit != 0 && mine.retained_byte_limit < m_peer_byte_limit)) {
        m_status.handshake_error = "lockstep retention budget cannot fit negotiated traffic";
        return false;
    }
    // Include the last partial cadence interval and buffered closure inputs;
    // pacing is disabled only while draining the already bounded peer lead.
    const auto tick_max = (std::numeric_limits<std::uint64_t>::max)();
    if (cadence != 0 && m_checkpoint_window > (tick_max - lead - cadence) / cadence) {
        m_status.handshake_error = "lockstep replay retention size overflows this platform";
        return false;
    }
    const auto unconfirmed_ticks =
        cadence == 0 ? 0 : m_checkpoint_window * cadence + lead + cadence;
    if (m_frame_limit > (size_max - 46) / 3 ||
        !m_replay->Configure(3 * m_frame_limit + 46, unconfirmed_ticks)) {
        m_status.handshake_error = "lockstep replay retention size overflows this platform";
        return false;
    }
    return true;
}

void LockstepSession::PruneDiagnosticHistory() {
    while (!m_applied_inputs.empty() && (m_applied_inputs.size() > kReceiveQueueFrameLimit ||
                                         m_applied_input_bytes > kMaxFramePayloadBytes)) {
        m_applied_input_bytes -= m_applied_inputs.front().inputs.size();
        m_applied_inputs.pop_front();
    }
    // Never discard an unconfirmed oracle. Confirmed hashes need only the same
    // bounded tick suffix as the input diagnostics (each entry is erased once).
    const auto oldest =
        m_agreed_tick > kReceiveQueueFrameLimit ? m_agreed_tick - kReceiveQueueFrameLimit : 0;
    while (!m_local_hashes.empty() && m_local_hashes.begin()->first <= oldest &&
           m_local_hashes.begin()->first <= m_confirmed_peer_hash_tick) {
        m_local_hashes.erase(m_local_hashes.begin());
    }
}

void LockstepSession::PruneConsumedInputs() {
    // Each entry is removed once; no scan of the retained lookahead on each tick.
    while (!m_inputs.empty() && m_inputs.begin()->first <= m_agreed_tick) {
        const auto it = m_inputs.begin();
        const auto peer = it->second.find(m_config.peer_client_id);
        if (peer != it->second.end()) {
            m_retained_peer_bytes -= kInputFrameOverhead + peer->second.size();
            --m_peer_input_count;
        }
        m_inputs.erase(it);
    }
}

void LockstepSession::PruneConfirmedPeerHashes() {
    // Confirm only a contiguous prefix: a reordered later match must never make
    // an earlier, still in-flight mismatch stale. Each retained entry is erased once.
    const auto cadence = m_config.hash_cadence_ticks;
    while (cadence != 0 && !m_peer_hashes.empty()) {
        const auto it = m_peer_hashes.begin();
        if (it->first > m_agreed_tick || it->first <= m_confirmed_peer_hash_tick ||
            it->first - m_confirmed_peer_hash_tick != cadence)
            break;
        if (!ComparePeerHash(it->second))
            return;
        m_confirmed_peer_hash_tick = it->first;
        m_retained_peer_bytes -= HashFrameBytes(it->second.hash);
        m_peer_hashes.erase(it);
    }
}

void LockstepSession::ReleaseRetainedMessages() {
    // Called only after the receive drain and sticky closure reporting. Local
    // diagnostics and any finalized disconnect-time desync artifact remain available.
    m_inputs.clear();
    m_peer_hashes.clear();
    m_peer_input_count = 0;
    m_retained_peer_bytes = 0;
    m_replay->Close();
    if (*m_replay->FailureReason() != '\0')
        m_status.replay_error = m_replay->FailureReason();
}

bool LockstepSession::DrainPeerMessages() {
    std::vector<std::uint8_t> frame;
    m_receive_drain_pending = true;
    // Bye limits input execution and late hash comparison to its executed tick.
    // The original admission window remains fixed while buffered inputs drain.
    while (!m_status.desynced && m_receive_work.polls_left != 0 &&
           m_receive_work.payload_bytes < kReceiveQueueByteBudget) {
        --m_receive_work.polls_left;
        if (!m_transport->TryReceiveFrame(frame)) {
            m_receive_drain_pending = false;
            break;
        }
        // Transports enforce kMaxFramePayloadBytes before allocating. The byte
        // budget is checked before each poll, so this sum cannot overflow.
        m_receive_work.payload_bytes += frame.size();
        // A refused handshake drains received bytes under the usual budget,
        // but must never admit inputs, hashes or replay data from that peer.
        if (!m_handshaken && m_disconnect_pending)
            continue;
        MessageType type;
        if (!PeekMessageType(frame, type))
            continue;
        switch (type) {
            case MessageType::Input: {
                std::uint64_t tick = 0;
                std::uint32_t client_id = 0;
                if (frame.size() > m_frame_limit || !InspectInput(frame, tick, client_id) ||
                    client_id != m_config.peer_client_id ||
                    (m_received_bye_tick && tick > *m_received_bye_tick) || tick <= m_agreed_tick ||
                    !IsWithinPeerLookahead(tick)) {
                    ++m_status.rejected_peer_messages;
                    break;
                }
                const auto stored = m_inputs.find(tick);
                if (stored != m_inputs.end()) {
                    const auto peer = stored->second.find(client_id);
                    if (peer != stored->second.end()) {
                        // Exact retransmissions are idempotent; conflicting repeats
                        // cannot replace an already admitted deterministic input.
                        if (peer->second.size() != frame.size() - kInputFrameOverhead ||
                            !std::equal(peer->second.begin(),
                                        peer->second.end(),
                                        frame.begin() + kInputFrameOverhead))
                            ++m_status.rejected_peer_messages;
                        break;
                    }
                }
                if (!CanRetainPeerFrame(frame.size())) {
                    ++m_status.rejected_peer_messages;
                    break;
                }
                InputMsg in;
                if (DecodeInput(frame, in)) {
                    m_inputs[in.tick][in.client_id] = std::move(in.inputs);
                    ++m_peer_input_count;
                    m_retained_peer_bytes += frame.size();
                }
                break;
            }
            case MessageType::Hash: {
                std::uint64_t tick = 0;
                if (frame.size() > m_frame_limit || !InspectHash(frame, tick) ||
                    (m_received_bye_tick && tick > *m_received_bye_tick) ||
                    !IsRetainedPeerCheckpoint(tick) || !IsWithinPeerLookahead(tick) ||
                    m_peer_hashes.find(tick) != m_peer_hashes.end() ||
                    !CanRetainPeerFrame(frame.size())) {
                    ++m_status.rejected_peer_messages;
                    break;
                }
                HashMsg h;
                if (DecodeHash(frame, h)) {
                    const auto inserted =
                        m_peer_hashes.emplace(tick, PeerCheckpoint{std::move(h), false});
                    m_retained_peer_bytes += frame.size();
                    // Late hashes still compare immediately, then retain only a
                    // marker if an earlier checkpoint is outstanding.
                    if (!ComparePeerHash(inserted.first->second))
                        break;
                    PruneConfirmedPeerHashes();
                }
                break;
            }
            case MessageType::Bye: {
                ByeMsg bye;
                if (!DecodeBye(frame, bye)) {
                    ++m_status.rejected_peer_messages;
                    break;
                }
                // End cleanly after draining: a queued hash for an executed tick
                // still takes precedence over this Bye.
                if (!m_received_bye_tick) {
                    m_received_bye_tick = bye.tick;
                    m_bye_admission_tick = m_transport_close_admission_tick.value_or(m_agreed_tick);
                    m_pending_outbound.clear();
                    m_pending_outbound_bytes = 0;
                }
                break;
            }
            case MessageType::Hello:
            default:
                // A stray Hello / unknown after handshake is ignored (forward-compat: an
                // unknown future message must not crash an older peer).
                break;
        }
    }
    if (!m_transport_close_admission_tick && !m_transport->IsPeerConnected()) {
        // A send-side closure can precede the last received frames. Drain the
        // bounded lead already admitted, but never move its admission anchor:
        // closure bypasses checkpoint pacing and must not open unlimited slots.
        m_transport_close_admission_tick = m_agreed_tick;
        m_pending_outbound.clear();
        m_pending_outbound_bytes = 0;
    }
    return !m_status.desynced && !m_status.peer_disconnected;
}

TickResult LockstepSession::PumpTick(std::uint64_t budget_ticks) {
    m_receive_work = {};
    TickResult result;
    const auto terminal_result = [&] {
        result.outcome = m_status.desynced            ? TickOutcome::Desync
                         : m_status.peer_disconnected ? TickOutcome::PeerDisconnected
                                                      : TickOutcome::WaitingForPeer;
        result.tick = m_status.desynced            ? m_status.desync_tick
                      : m_status.peer_disconnected ? m_status.disconnect_tick
                                                   : m_agreed_tick;
        return result;
    };
    if (m_status.desynced || m_status.peer_disconnected)
        return terminal_result();
    if (!m_handshaken) {
        if (m_disconnect_pending || !m_transport->IsPeerConnected())
            TransportDisconnected();
        return terminal_result();
    }
    // 1) Drain peer messages (inputs, hash compares, disconnect). A fatal state stops here.
    if (!DrainPeerMessages())
        return terminal_result();
    if (m_disconnect_pending) {
        TransportDisconnected();
        return terminal_result();
    }
    if (!FlushPendingOutbound())
        return terminal_result();
    // Peers pre-schedule inputs beyond their final executed tick. Bye limits
    // execution even if those later inputs are buffered. The receive drain above
    // and TransportDisconnected still compare queued hashes through the stop tick.
    if (m_received_bye_tick && m_agreed_tick >= *m_received_bye_tick) {
        TransportDisconnected(m_received_bye_tick);
        return terminal_result();
    }
    if (m_agreed_tick >= budget_ticks) {
        result.outcome = m_pending_outbound.empty() && !m_receive_drain_pending
                             ? TickOutcome::Finished
                             : TickOutcome::WaitingForPeer;
        result.tick = m_agreed_tick;
        return result;
    }

    const std::uint64_t want = m_agreed_tick + 1;
    result.tick = want;
    // Keep all unconfirmed hashes without growing with session duration. Reliable
    // receive continues while paused, so a delayed oldest checkpoint always fits
    // and releases this wait. This is pacing only; applied bytes are unchanged.
    if (!m_transport_close_admission_tick && m_transport->IsPeerConnected() &&
        !m_received_bye_tick && m_config.hash_cadence_ticks != 0 &&
        m_agreed_tick / m_config.hash_cadence_ticks -
                m_confirmed_peer_hash_tick / m_config.hash_cadence_ticks >=
            m_checkpoint_window) {
        result.outcome = TickOutcome::WaitingForPeer;
        return result;
    }

    // 2) Do we have ALL peers' inputs for the wanted tick? (local + the one remote).
    auto it = m_inputs.find(want);
    const bool have_local =
        it != m_inputs.end() && it->second.find(m_config.local_client_id) != it->second.end();
    const bool have_peer =
        it != m_inputs.end() && it->second.find(m_config.peer_client_id) != it->second.end();

    if (!have_peer && (m_received_bye_tick || m_transport_close_admission_tick ||
                       !m_transport->IsPeerConnected())) {
        // The wanted input or an executed tick's late hash may be behind this
        // batch. A quota yield must not turn a queued input into a disconnect.
        if (m_receive_drain_pending) {
            result.outcome = TickOutcome::WaitingForPeer;
            return result;
        }
        TransportDisconnected(m_received_bye_tick);
        return terminal_result();
    }

    if (!have_local) {
        // Should not happen (we pre-scheduled), but be defensive: schedule it now.
        if (!ScheduleLocalInput(want))
            return terminal_result();
        it = m_inputs.find(want);
    }

    if (!have_peer) {
        // The peer's input has not arrived -- delay-based lockstep WAITS rather than
        // guessing. ADAPT: this is a late peer; grow the horizon (in ticks, NOT wall-clock)
        // up to the documented max so future inputs are sent further ahead and absorb the
        // jitter. At the ceiling we PAUSE visibly (WaitingForPeer) rather than desync.
        if (m_horizon < m_config.horizon_max) {
            ++m_horizon;
            m_status.max_horizon_reached = std::max(m_status.max_horizon_reached, m_horizon);
            // Sending the next-horizon local input ahead is what actually absorbs jitter.
            if (!ScheduleLocalInput(m_agreed_tick + m_horizon))
                return terminal_result();
        }
        ++m_status.late_input_events;
        m_consecutive_slack_ticks = 0;
        m_status.horizon = m_horizon;
        result.outcome = TickOutcome::WaitingForPeer;
        return result;
    }

    // 3) All inputs present -> SHRINK toward min when slack has returned (the peer has been
    // comfortably ahead for slack_ticks_to_shrink consecutive ticks). Pure pacing; the
    // horizon never changes WHAT a tick computes.
    if (m_horizon > m_config.horizon_min) {
        // Peer input for `want` was already buffered when we got here => slack tick.
        ++m_consecutive_slack_ticks;
        if (m_consecutive_slack_ticks >= m_config.slack_ticks_to_shrink) {
            --m_horizon;
            m_consecutive_slack_ticks = 0;
        }
    }
    m_status.horizon = m_horizon;

    // 4) Merge the agreed inputs in ascending client-id order (deterministic: std::map is
    // ordered) and apply + step the world by exactly one tick.
    // Both slots are frame-bounded; check their exact sum before allocating.
    std::size_t merged_bytes = 0;
    for (const auto& [client_id, blob] : it->second) {
        if (blob.size() > (std::numeric_limits<std::size_t>::max)() - merged_bytes) {
            TransportDisconnected();
            return terminal_result();
        }
        merged_bytes += blob.size();
    }
    // The merged wire blob becomes immutable once assembled. Recording shares
    // this allocation with the tick rather than copying its potentially large bytes.
    std::vector<std::uint8_t> merged_inputs;
    merged_inputs.reserve(merged_bytes);
    for (const auto& [client_id, blob] : it->second) {
        merged_inputs.insert(merged_inputs.end(), blob.begin(), blob.end());
    }
    const auto recorded_inputs =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(merged_inputs));
    const auto& merged = *recorded_inputs;
    result.local_inputs = merged;

    bool stepped = true;
    if (m_hooks.apply_and_step) {
        stepped = m_hooks.apply_and_step(want, merged, m_hooks.user);
    }
    if (!stepped) {
        // A failed sim step is a fatal local error -- treat as a desync against ourselves
        // so the dump is emitted and the session halts rather than silently diverging.
        RecordReplayTick(want, recorded_inputs, nullptr);
        EmitDesyncDump(want, "apply_and_step");
        result.outcome = TickOutcome::Desync;
        result.tick = want;
        return result;
    }

    // Record the applied merged input for the bounded diagnostic suffix.
    InputMsg applied;
    applied.tick = want;
    applied.client_id = 0; // merged set (not a single client)
    applied.inputs = merged;
    m_applied_input_bytes += applied.inputs.size();
    m_applied_inputs.push_back(std::move(applied));

    m_agreed_tick = want;
    m_status.agreed_tick = want;

    // Remove the entire consumed prefix, including any past-tick entries.
    PruneConsumedInputs();

    // 5) Capture the executed tick's LOCAL hash before any send can fail and drain
    // new peer hashes. Every available hash for this tick can then be compared.
    const bool hash_due =
        m_config.hash_cadence_ticks > 0 && (want % m_config.hash_cadence_ticks) == 0;
    if (hash_due) {
        HashMsg local;
        local.tick = want;
        if (m_hooks.capture_hashes) {
            m_hooks.capture_hashes(want, local, m_hooks.user);
        }
        m_local_hashes[want] = std::make_shared<const HashMsg>(std::move(local));
    }

    {
        const auto hash = m_local_hashes.find(want);
        RecordReplayTick(
            want, recorded_inputs, hash == m_local_hashes.end() ? nullptr : hash->second);
    }
    if (m_hooks.record_replay_tick) {
        const auto hash = m_local_hashes.find(want);
        m_hooks.record_replay_tick(want,
                                   merged,
                                   hash == m_local_hashes.end() ? nullptr : hash->second.get(),
                                   m_hooks.user);
    }
    PruneDiagnosticHistory();

    // 6) Schedule the local input that is now `horizon` ticks ahead of the new agreed tick.
    bool send_failed = !m_received_bye_tick && !m_transport_close_admission_tick &&
                       m_transport->IsPeerConnected() &&
                       !ScheduleLocalInput(m_agreed_tick + m_horizon);
    if (m_status.desynced)
        return terminal_result();

    // 7) Send the cadence hash and compare any already-received peer hash for this tick.
    if (hash_due) {
        const auto& local = *m_local_hashes.at(want);
        if (!m_received_bye_tick && !m_transport_close_admission_tick &&
            m_transport->IsPeerConnected()) {
            // Hash frame: type + payload length + tick + four string lengths/data.
            constexpr std::size_t overhead = 1 + 4 + 8 + 4 * 4;
            const auto limit = std::min(m_frame_limit, m_transport->MaxFramePayloadBytes());
            bool oversized = limit < overhead;
            std::size_t remaining = oversized ? 0 : limit - overhead;
            for (const auto* part :
                 {&local.world_hash, &local.terrain, &local.water, &local.entities}) {
                if (part->size() > remaining) {
                    oversized = true;
                    break;
                }
                remaining -= part->size();
            }
            if (oversized || !SendOrQueueFrame(EncodeHash(local))) {
                if (oversized)
                    TransportDisconnected();
                send_failed = true;
                if (m_status.desynced)
                    return terminal_result();
            } else {
                result.ran_hash_exchange = true;
            }
        }

        auto peer = m_peer_hashes.find(want);
        if (peer != m_peer_hashes.end() && !ComparePeerHash(peer->second))
            return terminal_result();
        PruneConfirmedPeerHashes();
        if (m_status.desynced)
            return terminal_result();
    }

    // A failed send must not hide a divergence in the tick we just executed.
    if (send_failed)
        return terminal_result();

    result.outcome =
        (m_agreed_tick >= budget_ticks && m_pending_outbound.empty() && !m_receive_drain_pending)
            ? TickOutcome::Finished
            : TickOutcome::Advanced;
    return result;
}

void LockstepSession::SetDumpPath(const std::string& path) {
    m_dump_path = path;
    // Preserve the caller's pre-PumpTick setup contract even after handshake has
    // started the recorder. Reopening here keeps directory I/O out of PumpTick.
    if (m_handshaken && m_agreed_tick == 0 && !m_status.desynced && !m_disconnected) {
        m_replay->Close();
        m_replay_open = false;
        m_replay_failed = false;
        m_status.replay_error.clear();
        OpenReplayRecorder();
    }
}

bool LockstepSession::OpenReplayRecorder() {
    if (m_replay_open)
        return true;
    if (m_replay_failed)
        return false;
    Luminumbra::Replay::ReplayHeader header;
    header.version = Luminumbra::Replay::kLrec1Version;
    header.system_config_identity = m_config.system_config_identity;
    header.preset_content_hash = m_config.preset_content_hash;
    header.tick_rate_hz = m_config.tick_rate_hz;
    header.seed = m_config.seed;
    header.seed_string = std::to_string(m_config.seed);
    header.preset = m_config.preset;
    header.preset_hash =
        std::strtoull(Luminumbra::Replay::Fnv1a64Hex(m_config.preset).c_str(), nullptr, 16);
    header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
    if (m_hooks.capture_hashes) {
        HashMsg boot;
        m_hooks.capture_hashes(0, boot, m_hooks.user);
        header.start_world_hash = std::move(boot.world_hash);
    }
    m_replay_open = m_replay->Open(m_dump_path, header);
    m_replay_failed = !m_replay_open;
    if (m_replay_failed) {
        m_status.replay_error = "recorder setup failed";
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: cannot open bounded replay recorder for '{}'",
                              m_config.local_client_id,
                              m_dump_path);
    }
    return m_replay_open;
}

void LockstepSession::RecordReplayTick(std::uint64_t tick,
                                       std::shared_ptr<const std::vector<std::uint8_t>> merged,
                                       std::shared_ptr<const HashMsg> hash) {
    if (!m_replay_open || m_replay_failed)
        return;
    const auto confirmed = m_config.hash_cadence_ticks == 0 ? tick : m_confirmed_peer_hash_tick;
    if (!m_replay->RecordTick(tick, std::move(merged), std::move(hash), confirmed)) {
        m_replay_failed = true;
        m_status.replay_error = m_replay->FailureReason();
        // Do not wait, log to disk, or discard accepted work on the tick path.
        // Finalize/Close drains it and reports the recorder's first failure.
    }
}

void LockstepSession::EmitDesyncDump(std::uint64_t divergence_tick, const std::string& section) {
    m_status.desynced = true;
    m_status.desync_tick = divergence_tick;
    m_status.desync_section = section;
    if (!m_replay_open || !m_replay->Finalize(m_dump_path,
                                              std::max(m_agreed_tick, divergence_tick),
                                              divergence_tick)) {
        if (*m_replay->FailureReason() != '\0')
            m_status.replay_error = m_replay->FailureReason();
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: failed to finalize desync dump '{}' at tick {}",
                              m_config.local_client_id,
                              m_dump_path,
                              divergence_tick);
        m_replay->Close();
        return;
    }
    m_status.dump_path = m_dump_path;
    m_replay->Close();

    LUMINUMBRA_CORE_ERROR("lockstep[{}]: DESYNC at tick {} (section={}); LREC1 dump -> {}",
                          m_config.local_client_id,
                          divergence_tick,
                          section,
                          m_dump_path);
}

void LockstepSession::Disconnect() {
    m_receive_work = {};
    if (m_disconnected)
        return;
    if (m_disconnect_pending) {
        TransportDisconnected();
        return;
    }
    if (!FlushPendingOutbound())
        return;
    if (!m_pending_outbound.empty()) {
        TransportDisconnected();
        return;
    }
    // A rejected Bye may itself close the transport, so drain before attempting it
    // as well as before the explicit Close in TransportDisconnected.
    if (!m_status.desynced)
        DrainPeerMessages();
    ByeMsg bye;
    bye.tick = m_agreed_tick;
    if (!m_transport->SendFrame(EncodeBye(bye))) {
        LUMINUMBRA_CORE_WARN("lockstep[{}]: disconnect send failed", m_config.local_client_id);
    }
    TransportDisconnected();
}

} // namespace Luminumbra::Net
