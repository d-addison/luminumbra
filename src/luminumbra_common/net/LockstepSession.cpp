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
        return ok && pos + n <= size;
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
    if (!c.GetU64(out.seed))
        return false;
    if (!c.GetU16(out.tick_rate_hz))
        return false;
    if (!c.GetU32(out.client_id))
        return false;
    if (!c.GetString(out.preset))
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
    if (!m_tx || !m_tx->open)
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
    char buf[4096];
    for (;;) {
        const int n = ::recv(static_cast<SOCKET>(m_socket), buf, sizeof(buf), 0);
        if (n > 0) {
            m_recv_buffer.insert(m_recv_buffer.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            m_peer_closed = true;
            return true;
        } // graceful close
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return true;      // no more data right now
        m_peer_closed = true; // real error -> treat as disconnect (clean end)
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
    if (m_socket < 0 || m_peer_closed)
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
    if (m_recv_buffer.size() < 4)
        return false;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[i]) << (8 * i);
    if (m_recv_buffer.size() < 4u + frame_len)
        return false; // frame not fully arrived
    out.assign(m_recv_buffer.begin() + 4, m_recv_buffer.begin() + 4 + frame_len);
    m_recv_buffer.erase(m_recv_buffer.begin(), m_recv_buffer.begin() + 4 + frame_len);
    return true;
}

bool TcpTransport::IsPeerConnected() const {
    // Connected while the socket is live and no EOF/error was seen, OR while buffered
    // bytes remain to be drained (a clean close is observed after its queued frames).
    return m_socket >= 0 && (!m_peer_closed || !m_recv_buffer.empty());
}

void TcpTransport::Close() {
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
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(static_cast<int>(m_socket), buf, sizeof(buf), 0);
        if (n > 0) {
            m_recv_buffer.insert(m_recv_buffer.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            m_peer_closed = true;
            return true;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        m_peer_closed = true;
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
    if (m_socket < 0 || m_peer_closed)
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
    if (m_recv_buffer.size() < 4)
        return false;
    std::uint32_t frame_len = 0;
    for (int i = 0; i < 4; ++i)
        frame_len |= static_cast<std::uint32_t>(m_recv_buffer[i]) << (8 * i);
    if (m_recv_buffer.size() < 4u + frame_len)
        return false;
    out.assign(m_recv_buffer.begin() + 4, m_recv_buffer.begin() + 4 + frame_len);
    m_recv_buffer.erase(m_recv_buffer.begin(), m_recv_buffer.begin() + 4 + frame_len);
    return true;
}

bool TcpTransport::IsPeerConnected() const {
    return m_socket >= 0 && (!m_peer_closed || !m_recv_buffer.empty());
}

void TcpTransport::Close() {
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
    : m_config(config)
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
    // protocol/version/seed/preset/tick-rate is rejected loudly -- a mis-paired session
    // would otherwise silently desync.
    HelloMsg mine;
    mine.protocol_version = kLockstepProtocolVersion;
    mine.seed = m_config.seed;
    mine.preset = m_config.preset;
    mine.tick_rate_hz = m_config.tick_rate_hz;
    mine.client_id = m_config.local_client_id;
    if (!m_transport->SendFrame(EncodeHello(mine))) {
        LUMINUMBRA_CORE_ERROR("lockstep[{}]: handshake send failed (peer gone)",
                              m_config.local_client_id);
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
            LUMINUMBRA_CORE_ERROR("lockstep[{}]: malformed Hello", m_config.local_client_id);
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

    m_config.peer_client_id = peer.client_id;
    m_handshaken = true;
    m_status.handshaken = true;

    // Pre-schedule the local input for ticks [1.. horizon]: delay-based lockstep sends
    // inputs `horizon` ticks ahead, so the first `horizon` ticks' local inputs are in
    // flight before the first tick runs.
    for (std::uint64_t t = 1; t <= m_horizon; ++t) {
        ScheduleLocalInput(t);
    }
    return true;
}

void LockstepSession::ScheduleLocalInput(std::uint64_t tick) {
    if (tick <= m_local_input_scheduled_through)
        return;
    std::vector<std::uint8_t> blob;
    if (m_hooks.collect_local_input) {
        blob = m_hooks.collect_local_input(tick, m_hooks.user);
    }
    // Buffer our own input locally (so the merge sees it) and send it to the peer.
    m_inputs[tick][m_config.local_client_id] = blob;
    InputMsg msg;
    msg.tick = tick;
    msg.client_id = m_config.local_client_id;
    msg.inputs = blob;
    m_transport->SendFrame(EncodeInput(msg));
    m_local_input_scheduled_through = tick;
}

bool LockstepSession::DrainPeerMessages() {
    std::vector<std::uint8_t> frame;
    while (m_transport->TryReceiveFrame(frame)) {
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
                    auto local = m_local_hashes.find(h.tick);
                    if (local != m_local_hashes.end()) {
                        if (local->second.world_hash != h.world_hash) {
                            // Localize via authoritative sub-hashes (terrain/water/entities),
                            // then fall back to world_hash (mesh / non-authoritative).
                            std::string section = "world_hash";
                            if (local->second.terrain != h.terrain)
                                section = "terrain";
                            else if (local->second.water != h.water)
                                section = "water";
                            else if (local->second.entities != h.entities)
                                section = "entities";
                            EmitDesyncDump(h.tick, section);
                            return false;
                        }
                    }
                }
                break;
            }
            case MessageType::Bye: {
                ByeMsg bye;
                DecodeBye(frame, bye);
                // A clean disconnect ENDS the session cleanly (regression review): it is NOT a
                // desync. Record the disconnect tick and stop.
                m_disconnected = true;
                m_status.peer_disconnected = true;
                m_status.disconnect_tick = bye.tick;
                return false;
            }
            case MessageType::Hello:
            default:
                // A stray Hello / unknown after handshake is ignored (forward-compat: an
                // unknown future message must not crash an older peer).
                break;
        }
    }
    return true;
}

TickResult LockstepSession::PumpTick(std::uint64_t budget_ticks) {
    TickResult result;
    if (m_status.desynced) {
        result.outcome = TickOutcome::Desync;
        result.tick = m_status.desync_tick;
        return result;
    }
    if (m_status.peer_disconnected) {
        result.outcome = TickOutcome::PeerDisconnected;
        result.tick = m_status.disconnect_tick;
        return result;
    }
    if (m_agreed_tick >= budget_ticks) {
        result.outcome = TickOutcome::Finished;
        result.tick = m_agreed_tick;
        return result;
    }

    // 1) Drain peer messages (inputs, hash compares, disconnect). A fatal state stops here.
    if (!DrainPeerMessages()) {
        if (m_status.desynced) {
            result.outcome = TickOutcome::Desync;
            result.tick = m_status.desync_tick;
        } else {
            result.outcome = TickOutcome::PeerDisconnected;
            result.tick = m_status.disconnect_tick;
        }
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

    if (!have_local) {
        // Should not happen (we pre-scheduled), but be defensive: schedule it now.
        ScheduleLocalInput(want);
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
            ScheduleLocalInput(m_agreed_tick + m_horizon);
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

    // 5) Schedule the local input that is now `horizon` ticks ahead of the new agreed tick.
    ScheduleLocalInput(m_agreed_tick + m_horizon);

    // 6) Desync oracle: at the cadence, capture the LOCAL hash, send it, and compare
    // against any already-received peer hash for this tick.
    if (m_config.hash_cadence_ticks > 0 && (want % m_config.hash_cadence_ticks) == 0) {
        HashMsg local;
        local.tick = want;
        if (m_hooks.capture_hashes) {
            m_hooks.capture_hashes(want, local, m_hooks.user);
        }
        m_local_hashes[want] = local;
        m_transport->SendFrame(EncodeHash(local));
        result.ran_hash_exchange = true;

        auto peer = m_peer_hashes.find(want);
        if (peer != m_peer_hashes.end()) {
            if (peer->second.world_hash != local.world_hash) {
                std::string section = "world_hash";
                if (local.terrain != peer->second.terrain)
                    section = "terrain";
                else if (local.water != peer->second.water)
                    section = "water";
                else if (local.entities != peer->second.entities)
                    section = "entities";
                EmitDesyncDump(want, section);
                result.outcome = TickOutcome::Desync;
                result.tick = want;
                return result;
            }
        }
    }

    result.outcome =
        (m_agreed_tick >= budget_ticks) ? TickOutcome::Finished : TickOutcome::Advanced;
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
    ByeMsg bye;
    bye.tick = m_agreed_tick;
    m_transport->SendFrame(EncodeBye(bye));
    m_transport->Close();
    m_disconnected = true;
}

} // namespace Luminumbra::Net
