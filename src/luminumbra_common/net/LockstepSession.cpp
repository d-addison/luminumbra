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
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>

#include "core/EngineVersion.h"
#include "core/Log.h"
#include "replay/ReplayStream.h"

namespace Luminumbra::Net {
namespace {

namespace fs = std::filesystem;

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
    bool GetString(std::string& s,
                   std::size_t maximum_size = std::numeric_limits<std::size_t>::max()) {
        std::uint32_t len = 0;
        if (!GetU32(len))
            return false;
        if (len > maximum_size || !Remaining(len)) {
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

} // namespace

// --- Message encode/decode ---------------------------------------------------------

std::vector<std::uint8_t> EncodeHello(const HelloMsg& m) {
    if (m.simulation_content_identity.size() > kMaxSimulationContentIdentityBytes)
        return {};
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
    PutString(p, m.simulation_content_identity);
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
    // Older Hellos do not bind captured simulation content and cannot be admitted safely.
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
    if (!c.GetString(out.simulation_content_identity, kMaxSimulationContentIdentityBytes))
        return false;
    return c.ok;
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

LockstepSession::LockstepSession(LockstepConfig config,
                                 ILockstepTransport* transport,
                                 LockstepHooks hooks)
    : m_config(std::move(config))
    , m_transport(transport)
    , m_hooks(hooks) {
    m_horizon = std::max(m_config.horizon_min, m_config.horizon_start);
    m_status.horizon = m_horizon;
    m_status.max_horizon_reached = m_horizon;
    if (m_dump_path.empty()) {
        m_dump_path = (fs::temp_directory_path() /
                       ("lockstep-desync-" + std::to_string(m_config.local_client_id) + ".lrec1"))
                          .string();
    }
}

bool LockstepSession::Handshake() {
    // Send our Hello, then drain until the peer's Hello arrives (loopback: the peer's
    // Hello must already be queued by the single-process driver). A mismatched
    // protocol/version/seed/preset/tick-rate/config identity is rejected loudly.
    // A mis-paired session would otherwise silently desync.
    HelloMsg mine;
    mine.protocol_version = kLockstepProtocolVersion;
    mine.system_config_identity = m_config.system_config_identity;
    mine.preset_content_hash = m_config.preset_content_hash;
    mine.simulation_content_identity = m_config.simulation_content_identity;
    mine.seed = m_config.seed;
    mine.preset = m_config.preset;
    mine.tick_rate_hz = m_config.tick_rate_hz;
    mine.client_id = m_config.local_client_id;
    if (!m_transport->SendFrame(EncodeHello(mine))) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: handshake send failed (peer gone)",
                              m_config.local_client_id);
        TransportDisconnected();
        return false;
    }

    // Drain frames looking for the peer Hello. Non-Hello frames before the handshake are
    // a protocol error.
    std::vector<std::uint8_t> frame;
    bool got_peer = false;
    HelloMsg peer;
    // Bounded drain: pull whatever is currently available.
    while (m_transport->TryReceiveFrame(frame)) {
        MessageType type;
        if (!PeekMessageType(frame, type) || type != MessageType::Hello) {
            LUMINUMBRA_CORE_ERROR("lockstep[{}]: expected Hello first, got type {}",
                                  m_config.local_client_id,
                                  frame.empty() ? -1 : static_cast<int>(frame[0]));
            return false;
        }
        if (!DecodeHello(frame, peer)) {
            LUMINUMBRA_CORE_ERROR("lockstep[{}]: malformed or unsupported Hello version (requires "
                                  "captured simulation content identity)",
                                  m_config.local_client_id);
            return false;
        }
        got_peer = true;
        break;
    }
    if (!got_peer) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: peer never sent Hello", m_config.local_client_id);
        return false;
    }

    if (peer.protocol_version != mine.protocol_version) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: protocol version mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.protocol_version,
                              peer.protocol_version);
        return false;
    }
    if (peer.seed != mine.seed) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: seed mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.seed,
                              peer.seed);
        return false;
    }
    if (peer.preset != mine.preset) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: preset mismatch (local '{}' != peer '{}')",
                              m_config.local_client_id,
                              mine.preset,
                              peer.preset);
        return false;
    }
    if (peer.tick_rate_hz != mine.tick_rate_hz) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: tick-rate mismatch (local {} != peer {})",
                              m_config.local_client_id,
                              mine.tick_rate_hz,
                              peer.tick_rate_hz);
        return false;
    }

    if (mine.preset_content_hash.empty() || peer.preset_content_hash.empty() ||
        mine.preset_content_hash != peer.preset_content_hash) {
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: resolved preset/species content hash mismatch or missing identity",
            m_config.local_client_id);
        return false;
    }
    if (!HelloMatches(mine, peer)) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: simulation configuration or content identity mismatch",
                              m_config.local_client_id);
        return false;
    }

    m_config.peer_client_id = peer.client_id;
    // Pre-schedule the local input for ticks [1.. horizon]: delay-based lockstep sends
    // inputs `horizon` ticks ahead, so the first `horizon` ticks' local inputs are in
    // flight before the first tick runs.
    for (std::uint64_t t = 1; t <= m_horizon; ++t) {
        if (!ScheduleLocalInput(t))
            return false;
    }
    m_handshaken = true;
    m_status.handshaken = true;
    return true;
}

void LockstepSession::TransportDisconnected() {
    // Closing can discard queued frames. Compare all available hashes for executed
    // ticks first, even when the final send (including Bye) was rejected.
    if (!m_status.desynced) {
        // A peer hash may have been buffered before its local tick was executed.
        // The local cadence hash is now captured before any post-step send.
        for (const auto& entry : m_peer_hashes) {
            if (!ComparePeerHash(entry.second))
                break;
        }
        if (!m_status.desynced)
            DrainPeerMessages();
    }
    m_transport->Close();
    m_disconnected = true;
    if (!m_status.peer_disconnected)
        m_status.disconnect_tick = m_agreed_tick;
    m_status.peer_disconnected = true;
    m_pending_outbound.clear();
    m_pending_outbound_bytes = 0;
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
        if (!DrainPeerMessages() || !m_transport->IsPeerConnected()) {
            TransportDisconnected();
            return false;
        }
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
    while (!m_pending_outbound.empty()) {
        const auto& frame = m_pending_outbound.front();
        if (!m_transport->SendFrame(frame)) {
            if (!DrainPeerMessages() || !m_transport->IsPeerConnected()) {
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
    const auto limit = std::min(kMaxFramePayloadBytes, m_transport->MaxFramePayloadBytes());
    if (limit < overhead || blob.size() > limit - overhead) {
        TransportDisconnected();
        return false;
    }
    InputMsg msg;
    msg.tick = tick;
    msg.client_id = m_config.local_client_id;
    msg.inputs = blob;
    if (!SendOrQueueFrame(EncodeInput(msg))) {
        return false;
    }
    // Buffer locally once the frame is accepted or retained for reliable retry.
    m_inputs[tick][m_config.local_client_id] = std::move(blob);
    m_local_input_scheduled_through = tick;
    return true;
}

bool LockstepSession::ComparePeerHash(const HashMsg& peer) {
    const auto local = m_local_hashes.find(peer.tick);
    if (local == m_local_hashes.end() || local->second.world_hash == peer.world_hash)
        return true;
    // Localize via authoritative sub-hashes, then fall back to world_hash.
    std::string section = "world_hash";
    if (local->second.terrain != peer.terrain)
        section = "terrain";
    else if (local->second.water != peer.water)
        section = "water";
    else if (local->second.entities != peer.entities)
        section = "entities";
    EmitDesyncDump(peer.tick, section);
    return false;
}

bool LockstepSession::DrainPeerMessages() {
    std::vector<std::uint8_t> frame;
    bool saw_bye = false;
    std::uint64_t bye_tick = 0;
    while (!m_status.desynced && m_transport->TryReceiveFrame(frame)) {
        MessageType type;
        if (!PeekMessageType(frame, type))
            continue;
        switch (type) {
            case MessageType::Input: {
                InputMsg in;
                if (DecodeInput(frame, in)) {
                    m_inputs[in.tick][in.client_id] = in.inputs;
                }
                break;
            }
            case MessageType::Hash: {
                HashMsg h;
                if (DecodeHash(frame, h)) {
                    m_peer_hashes[h.tick] = h;
                    // Compare immediately if we already hold the local hash for that tick.
                    ComparePeerHash(h);
                }
                break;
            }
            case MessageType::Bye: {
                ByeMsg bye;
                DecodeBye(frame, bye);
                // End cleanly after draining: a queued hash for an executed tick
                // still takes precedence over this Bye.
                if (!saw_bye)
                    bye_tick = bye.tick;
                saw_bye = true;
                break;
            }
            case MessageType::Hello:
            default:
                // A stray Hello / unknown after handshake is ignored (forward-compat: an
                // unknown future message must not crash an older peer).
                break;
        }
    }
    if (saw_bye) {
        m_disconnected = true;
        m_status.peer_disconnected = true;
        m_status.disconnect_tick = bye_tick;
    }
    return !m_status.desynced && !m_status.peer_disconnected;
}

TickResult LockstepSession::PumpTick(std::uint64_t budget_ticks) {
    TickResult result;
    const auto terminal_result = [&] {
        result.outcome = m_status.desynced ? TickOutcome::Desync : TickOutcome::PeerDisconnected;
        result.tick = m_status.desynced ? m_status.desync_tick : m_status.disconnect_tick;
        return result;
    };
    if (m_status.desynced || m_status.peer_disconnected)
        return terminal_result();
    if (m_agreed_tick >= budget_ticks && m_pending_outbound.empty()) {
        result.outcome = TickOutcome::Finished;
        result.tick = m_agreed_tick;
        return result;
    }

    // 1) Drain peer messages (inputs, hash compares, disconnect). A fatal state stops here.
    if (!DrainPeerMessages())
        return terminal_result();
    if (!FlushPendingOutbound())
        return terminal_result();
    if (m_agreed_tick >= budget_ticks) {
        result.outcome =
            m_pending_outbound.empty() ? TickOutcome::Finished : TickOutcome::WaitingForPeer;
        result.tick = m_agreed_tick;
        return result;
    }

    const std::uint64_t want = m_agreed_tick + 1;
    result.tick = want;

    // 2) Do we have ALL peers' inputs for the wanted tick? (local + the one remote).
    auto it = m_inputs.find(want);
    const bool have_local =
        it != m_inputs.end() && it->second.find(m_config.local_client_id) != it->second.end();
    const bool have_peer =
        it != m_inputs.end() && it->second.find(m_config.peer_client_id) != it->second.end();

    if (!have_peer && !m_transport->IsPeerConnected()) {
        TransportDisconnected();
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
    std::vector<std::uint8_t> merged;
    for (const auto& [client_id, blob] : it->second) {
        merged.insert(merged.end(), blob.begin(), blob.end());
    }
    result.local_inputs = merged;

    bool stepped = true;
    if (m_hooks.apply_and_step) {
        stepped = m_hooks.apply_and_step(want, merged, m_hooks.user);
    }
    if (!stepped) {
        // A failed sim step is a fatal local error -- treat as a desync against ourselves
        // so the dump is emitted and the session halts rather than silently diverging.
        EmitDesyncDump(want, "apply_and_step");
        result.outcome = TickOutcome::Desync;
        result.tick = want;
        return result;
    }

    // Record the applied merged input for the desync dump's input stream.
    InputMsg applied;
    applied.tick = want;
    applied.client_id = 0; // merged set (not a single client)
    applied.inputs = merged;
    m_applied_inputs.push_back(std::move(applied));

    m_agreed_tick = want;
    m_status.agreed_tick = want;

    // Drop the consumed tick's inputs (bounded memory; we never re-run a tick -- no rollback).
    m_inputs.erase(want);

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
        m_local_hashes[want] = local;
    }

    // 6) Schedule the local input that is now `horizon` ticks ahead of the new agreed tick.
    bool send_failed =
        m_transport->IsPeerConnected() && !ScheduleLocalInput(m_agreed_tick + m_horizon);
    if (m_status.desynced)
        return terminal_result();

    // 7) Send the cadence hash and compare any already-received peer hash for this tick.
    if (hash_due) {
        const auto& local = m_local_hashes.at(want);
        if (m_transport->IsPeerConnected()) {
            // Hash frame: type + payload length + tick + four string lengths/data.
            constexpr std::size_t overhead = 1 + 4 + 8 + 4 * 4;
            const auto limit = std::min(kMaxFramePayloadBytes, m_transport->MaxFramePayloadBytes());
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
    }

    // A failed send must not hide a divergence in the tick we just executed.
    if (send_failed)
        return terminal_result();

    result.outcome = (m_agreed_tick >= budget_ticks && m_pending_outbound.empty())
                         ? TickOutcome::Finished
                         : TickOutcome::Advanced;
    return result;
}

void LockstepSession::EmitDesyncDump(std::uint64_t divergence_tick, const std::string& section) {
    m_status.desynced = true;
    m_status.desync_tick = divergence_tick;
    m_status.desync_section = section;

    // Emit an LREC1 stream of the LOCAL session up to the divergence so the existing
    // --replay path can re-run it. The dump's checkpoints carry the LOCAL hashes (this is
    // the desync-repro artifact: replaying it reconstructs exactly what this peer ran).
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
    // start_world_hash: the first cadence hash if we captured one, else empty.
    if (!m_local_hashes.empty()) {
        header.start_world_hash = m_local_hashes.begin()->second.world_hash;
    }

    Luminumbra::Replay::ReplayWriter writer;
    if (!writer.Open(m_dump_path, header)) {
        LUMINUMBRA_CORE_ERROR(
            "lockstep[{}]: failed to open desync dump '{}'", m_config.local_client_id, m_dump_path);
        return;
    }
    // Input records: every applied tick's merged opaque input set.
    for (const auto& applied : m_applied_inputs) {
        writer.RecordInput(applied.tick, applied.inputs);
    }
    // Checkpoint records: every LOCAL cadence hash captured up to (and including) the
    // divergence tick.
    for (const auto& [tick, h] : m_local_hashes) {
        if (tick > divergence_tick)
            continue;
        Luminumbra::Replay::CheckpointRecord cp;
        cp.tick = tick;
        cp.world_hash = h.world_hash;
        cp.terrain = h.terrain;
        cp.water = h.water;
        cp.entities = h.entities;
        writer.RecordCheckpoint(cp);
    }
    writer.Finalize(m_agreed_tick);
    m_status.dump_path = m_dump_path;

    LUMINUMBRA_CORE_ERROR("lockstep[{}]: DESYNC at tick {} (section={}); LREC1 dump -> {}",
                          m_config.local_client_id,
                          divergence_tick,
                          section,
                          m_dump_path);
}

void LockstepSession::Disconnect() {
    if (m_disconnected)
        return;
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
