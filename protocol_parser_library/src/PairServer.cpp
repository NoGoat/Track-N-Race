#include "tnrp/PairServer.h"

#include "tnrp/BinaryRows.h"
#include "tnrp/PairCrypto.h"
#include "tnrp/PairDiscovery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using PairSocket = SOCKET;
static constexpr PairSocket kInvalidPairSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using PairSocket = int;
static constexpr PairSocket kInvalidPairSocket = -1;
#endif

namespace tnrp {

// External linkage is required for glaze reflection under MSVC.
// Only a digest of each reconnect token is kept, so a copy of this state
// cannot be replayed as a phone. The identity seed signs every handshake;
// hosts store this document encrypted (safeStorage, DPAPI, libsecret).
struct PairPersistedDevice {
    std::string id;
    std::string name;
    std::string tokenHash;
    int64_t pairedAt{};
    int64_t lastSeenAt{};
};

struct PairPersistedState {
    std::string serverId;
    bool enabled{};
    std::string identitySeed;
    std::vector<PairPersistedDevice> devices;
};

struct PairPublicDevice {
    std::string id;
    std::string name;
    int64_t pairedAt{};
    int64_t lastSeenAt{};
    bool connected{};
};

// A phone that proved the QR secret or matching code and waits for the
// desktop user to allow it.
struct PairPendingDevice {
    std::string id;
    std::string name;
};

struct PairPublicState {
    bool enabled{};
    std::string serverId;
    int port{};
    bool pairingOpen{};
    int64_t pairingExpiresAt{};
    std::optional<std::string> matchingCode;
    std::optional<std::string> qrPayload;
    std::optional<PairPendingDevice> pendingDevice;
    std::vector<PairPublicDevice> devices;
    std::optional<std::string> error;
};

struct PairIncomingMessage {
    std::string type;
    int pairProtocol{};
    int binaryRowsVersion{};
    // hello (plaintext): handshake mode, ephemeral key, CPace message.
    std::string mode;
    std::string clientKey;
    std::string cpace;
    // auth (encrypted): one credential per mode.
    std::string deviceId;
    std::string name;
    std::string token;
    std::string secret;
    std::string confirm;
    std::string rowType;
    uint32_t streamMask{};
    // V6DataType ids this phone needs while a V6 recording is playing.
    std::vector<int> v6Types{};
    uint64_t requestId{};
    int currentLap{};
    int comparisonLap{};
    bool sectorDelta{};
    // request_lap_data: one lap of the selected driver, as "family.field".
    int lapNum{};
    std::vector<std::string> channels{};
};

// The part of playback_lap_blocks a phone reads. The full row also carries the
// desktop Analyze catalogue for every driver (hundreds of KB for a race), which
// a phone would have to download and parse before anything else could arrive.
struct PairLapBlock {
    int   lapNum{};
    float startSessionTime{};
    float endSessionTime{};
};

struct PairLapTime {
    int lapNum{};
    int lapTimeMs{};
};

struct PairLapBlocksRow {
    std::string               type{"playback_lap_blocks"};
    std::vector<PairLapBlock> blocks;
    std::vector<PairLapTime>  laps;
    int                       fastestLapNum{};
    bool                      deltaAvailable{};
    int                       trackLengthM{};
    int                       playbackDriverIndex{-1};
};

struct PairRowsFrame {
    std::string type{"rows"};
    std::vector<std::string> rows;
};

struct PairErrorFrame {
    std::string type{"error"};
    std::string code;
};

struct PairSubscribedFrame {
    std::string type{"subscribed"};
    uint32_t streamMask{};
    uint32_t historyMask{};
    std::string backfill{"none"};
};

struct PairPongFrame {
    std::string type{"pong"};
    int64_t at{};
};

struct PairWelcomeFrame {
    std::string type{"welcome"};
    int pairProtocol{pair::kProtocolVersion};
    int binaryRowsVersion{pair::kBinaryRowsVersion};
    std::string serverId;
    // The desktop's display name; a QR does not carry it.
    std::string name;
    // Issued once, when a new phone is approved; a resume keeps its token.
    std::optional<std::string> token;
    std::string source{"desktop"};
    std::optional<int> protocolYear;
    std::optional<int> formula;
    std::optional<bool> regulations2026;
    std::vector<std::string> capabilities{
        "subscribe", "latest-state", "playback-state", "lap-delta",
        "v6-requirements", "driver-restriction", "lap-data"
    };
};

// Reply to request_latest when no V6 recording is playing: driver -1 means the
// question does not apply, which is distinct from "public".
struct PairDriverRestrictionFrame {
    std::string type{"driver_restriction"};
    int         driverIndex{-1};
    bool        restricted{};
    bool        known{};
};

struct PairProtocolPeek {
    std::optional<int> active_format;
    std::optional<int> detected_format;
    std::optional<int> formula;
    std::optional<bool> regulations_2026;
};

namespace {

// Version 3: an encrypted channel (PairCrypto.h) and desktop approval of new
// phones. Version 2 added v6Types to subscribe and V6 patch rows.
constexpr int kPairProtocolVersion = pair::kProtocolVersion;
constexpr int kBinaryRowsVersion = pair::kBinaryRowsVersion;
constexpr int64_t kPairWindowMs = 2 * 60 * 1000;
// Guesses allowed per pairing window. CPace stops offline guessing, so this
// bounds the online chance of a guessed 8-character code to 5 in 31^8.
constexpr int kMaxPairingAttempts = 5;
constexpr int64_t kFailedAttemptDelayMs = 1000;
constexpr int64_t kApprovalTimeoutMs = 60 * 1000;
constexpr const char* kTooManyAttempts =
    "Pairing closed after too many wrong codes. Start pairing again.";
constexpr size_t kMaxFrameBytes = 1024 * 1024;
constexpr size_t kMaxBufferedBytes = 8 * 1024 * 1024;
// Multi-car and session state that the game itself sends at 2 Hz. V6 playback
// projects it at sample rate (22 timing patches per sample); a phone gets each
// car's newest value at most this often.
constexpr int64_t kThrottledRowIntervalMs = 250;
// A send blocked this long means the phone stopped reading altogether. Shorter
// pauses (app start-up, a GC, a rotation) are absorbed by the outbox.
constexpr int64_t kSendStallMs = 15'000;
// How long a closing reader waits for its writer after shutdown() before it
// closes the socket to abort a send that shutdown() did not interrupt.
constexpr int64_t kWriterExitGraceMs = 2'000;
constexpr uint8_t kPlaybackStateKeyType = 0xff;
constexpr uint32_t kAndroidPageMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
    (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13);
constexpr uint32_t kParticipantsMask = 1u << 8;
// The reader ignores ids it does not know; this only bounds a phone's list.
constexpr size_t kMaxV6TypesPerClient = 32;
constexpr size_t kMaxLapDataChannels = 64;
constexpr int kMaxV6TypeId = 63;

std::vector<uint8_t> sanitizeV6Types(const std::vector<int>& requested) {
    std::set<uint8_t> unique;
    for (const int value : requested) {
        if (value <= 0 || value > kMaxV6TypeId) continue;
        if (unique.size() >= kMaxV6TypesPerClient) break;
        unique.insert(static_cast<uint8_t>(value));
    }
    return {unique.begin(), unique.end()};
}

constexpr glz::opts kPartialRead{
    .null_terminated = false,
    .error_on_unknown_keys = false,
};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void closePairSocket(PairSocket socket) {
    if (socket == kInvalidPairSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void shutdownPairSocket(PairSocket socket) {
    if (socket == kInvalidPairSocket) return;
#ifdef _WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

int pairSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string pairSocketErrorText(int error) {
#ifdef _WIN32
    return "winsock=" + std::to_string(error);
#else
    return "errno=" + std::to_string(error) + " (" + std::strerror(error) + ")";
#endif
}

// Client sockets are blocking with no timeouts: the reader blocks in recv() and
// the writer in send(), and shutdown() wakes both. A send timeout is not usable
// for flow control, because a phone that pauses reading for a moment (start-up,
// GC, rotation) would be disconnected, and on Windows a timed-out socket is left
// in an indeterminate state.
void configureClientSocket(PairSocket socket) {
    const int enabled = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    const int sendBuffer = 1024 * 1024;
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
}

void setNonBlocking(PairSocket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    ioctlsocket(socket, FIONBIO, &enabled);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

void setBlocking(PairSocket socket) {
#ifdef _WIN32
    u_long enabled = 0;
    ioctlsocket(socket, FIONBIO, &enabled);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) fcntl(socket, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

bool sendAll(PairSocket socket, const uint8_t* data, size_t length,
             size_t* bytesSent, int* socketError) {
    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const int chunk = static_cast<int>(std::min<size_t>(
            remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
        int flags = 0;
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
        flags = MSG_NOSIGNAL;
#endif
        const int sent = send(socket,
            reinterpret_cast<const char*>(data + offset), chunk, flags);
        if (sent <= 0) {
            if (bytesSent) *bytesSent = offset;
            if (socketError) *socketError = sent < 0 ? pairSocketError() : 0;
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    if (bytesSent) *bytesSent = offset;
    if (socketError) *socketError = 0;
    return true;
}

bool validToken(std::string_view value, size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == '\n' || ch == '\r') return false;
    }
    return true;
}

bool constantTimeEqual(std::string_view left, std::string_view right) {
    const size_t maximum = std::max(left.size(), right.size());
    uint32_t difference = static_cast<uint32_t>(left.size() ^ right.size());
    for (size_t index = 0; index < maximum; ++index) {
        const unsigned char a = index < left.size()
            ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char b = index < right.size()
            ? static_cast<unsigned char>(right[index]) : 0;
        difference |= static_cast<uint32_t>(a ^ b);
    }
    return difference == 0;
}

std::vector<uint8_t> randomBytes(size_t count) {
    std::random_device source;
    std::vector<uint8_t> bytes(count);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(source());
    return bytes;
}

std::string hex(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::string base64(const uint8_t* data, size_t length, bool url = false) {
    static constexpr char normal[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr char urlSafe[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* alphabet = url ? urlSafe : normal;
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (size_t index = 0; index < length; index += 3) {
        const uint32_t a = data[index];
        const uint32_t b = index + 1 < length ? data[index + 1] : 0;
        const uint32_t c = index + 2 < length ? data[index + 2] : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        if (index + 1 < length) result.push_back(alphabet[(value >> 6) & 63]);
        else if (!url) result.push_back('=');
        if (index + 2 < length) result.push_back(alphabet[value & 63]);
        else if (!url) result.push_back('=');
    }
    return result;
}

uint32_t rotateLeft(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(std::string_view input) {
    std::vector<uint8_t> message(input.begin(), input.end());
    const uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8;
    message.push_back(0x80);
    while ((message.size() % 64) != 56) message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<uint8_t>(bitLength >> shift));

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xc3d2e1f0;
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        uint32_t words[80]{};
        for (int index = 0; index < 16; ++index) {
            const size_t offset = chunk + static_cast<size_t>(index) * 4;
            words[index] = (static_cast<uint32_t>(message[offset]) << 24) |
                (static_cast<uint32_t>(message[offset + 1]) << 16) |
                (static_cast<uint32_t>(message[offset + 2]) << 8) |
                message[offset + 3];
        }
        for (int index = 16; index < 80; ++index)
            words[index] = rotateLeft(words[index - 3] ^ words[index - 8] ^
                                      words[index - 14] ^ words[index - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int index = 0; index < 80; ++index) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (index < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999; }
            else if (index < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (index < 60) {
                f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc;
            } else { f = b ^ c ^ d; k = 0xca62c1d6; }
            const uint32_t next = rotateLeft(a, 5) + f + e + k + words[index];
            e = d; d = c; c = rotateLeft(b, 30); b = a; a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<uint8_t, 20> digest{};
    const uint32_t hashes[] = {h0, h1, h2, h3, h4};
    for (size_t index = 0; index < 5; ++index) {
        digest[index * 4] = static_cast<uint8_t>(hashes[index] >> 24);
        digest[index * 4 + 1] = static_cast<uint8_t>(hashes[index] >> 16);
        digest[index * 4 + 2] = static_cast<uint8_t>(hashes[index] >> 8);
        digest[index * 4 + 3] = static_cast<uint8_t>(hashes[index]);
    }
    return digest;
}

std::string webSocketAccept(std::string_view key) {
    std::string input(key);
    input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = sha1(input);
    return base64(digest.data(), digest.size());
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string logSafe(std::string_view value) {
    std::string result;
    result.reserve(std::min<size_t>(value.size(), 256));
    for (const unsigned char ch : value.substr(0, 256)) {
        result.push_back(ch < 0x20 || ch == 0x7f ? '?' : static_cast<char>(ch));
    }
    return result;
}

std::string headerValue(const std::string& request, std::string_view wanted) {
    const size_t requestLineEnd = request.find("\r\n");
    if (requestLineEnd == std::string::npos) return {};
    size_t start = requestLineEnd + 2;
    while (start < request.size()) {
        const size_t end = request.find("\r\n", start);
        if (end == std::string::npos || end == start) break;
        const size_t colon = request.find(':', start);
        if (colon != std::string::npos && colon < end) {
            std::string name = request.substr(start, colon - start);
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (name == wanted) return trim(request.substr(colon + 1, end - colon - 1));
        }
        start = end + 2;
    }
    return {};
}

void appendFrameHeader(std::vector<uint8_t>& frame, uint8_t opcode, size_t length) {
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (length < 126) {
        frame.push_back(static_cast<uint8_t>(length));
    } else if (length <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>(length >> 8));
        frame.push_back(static_cast<uint8_t>(length));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(length) >> shift));
    }
}

std::vector<uint8_t> webSocketFrame(uint8_t opcode, const uint8_t* data, size_t length) {
    std::vector<uint8_t> frame;
    frame.reserve(length + 10);
    appendFrameHeader(frame, opcode, length);
    frame.insert(frame.end(), data, data + length);
    return frame;
}

// A binary WebSocket frame carrying one sealed channel message, encrypted
// straight into the frame buffer.
std::vector<uint8_t> sealedFrame(pair::FrameCipher& cipher, uint8_t kind,
                                 const uint8_t* data, size_t length) {
    const size_t sealed = length + pair::FrameCipher::kOverhead;
    std::vector<uint8_t> frame;
    frame.reserve(sealed + 10);
    appendFrameHeader(frame, 0x2, sealed);
    const size_t at = frame.size();
    frame.resize(at + sealed);
    cipher.sealInto(frame.data() + at, kind, data, length);
    return frame;
}

std::vector<uint8_t> webSocketFrame(uint8_t opcode, const std::string& text) {
    return webSocketFrame(opcode, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

uint8_t rowTypeOf(std::string_view json) {
    static constexpr std::pair<std::string_view, uint8_t> types[] = {
        {"telemetry", 1}, {"status", 2}, {"damage", 3}, {"lap", 4},
        {"session", 5}, {"race_event", 6}, {"timing", 7},
        {"participants", 8}, {"all_status", 9}, {"tyre_sets", 10},
        {"motion", 11}, {"motion_ex", 12}, {"positions", 13},
    };
    // Indexed recordings add session_time before type for state rows that did
    // not originally carry a timestamp (notably Participants). Inspect only
    // the first type field so wrapper rows cannot match a nested telemetry row.
    static constexpr std::string_view TYPE_KEY = "\"type\":\"";
    const size_t key = json.find(TYPE_KEY);
    if (key == std::string_view::npos) return 0;
    const std::string_view value = json.substr(key + TYPE_KEY.size());
    for (const auto& [name, type] : types)
        if (value.starts_with(name) && value.size() > name.size() &&
            value[name.size()] == '\"') return type;
    return 0;
}

bool allowedControlRow(std::string_view json) {
    return json.starts_with("{\"type\":\"protocol_status\"") ||
        json.starts_with("{\"type\":\"playback_state\"") ||
        json.starts_with("{\"type\":\"timeline_reset\"") ||
        json.starts_with("{\"type\":\"playback_loaded\"") ||
        json.starts_with("{\"type\":\"playback_lap_blocks\"") ||
        json.starts_with("{\"type\":\"driver_restriction\"") ||
        json.starts_with("{\"type\":\"playback_close\"");
}

// The integer after the first occurrence of key, e.g. "\"_v6_type\":".
std::optional<int> intFieldAfter(std::string_view json, std::string_view key) {
    const size_t at = json.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    size_t index = at + key.size();
    const bool negative = index < json.size() && json[index] == '-';
    if (negative) ++index;
    if (index >= json.size() || !std::isdigit(static_cast<unsigned char>(json[index])))
        return std::nullopt;
    int value = 0;
    while (index < json.size() && std::isdigit(static_cast<unsigned char>(json[index])))
        value = value * 10 + (json[index++] - '0');
    return negative ? -value : value;
}

// V6DataType of a single-field-group playback patch, or 0 for a complete row.
int v6TypeOf(std::string_view json) {
    return intFieldAfter(json, "\"_v6_type\":").value_or(0);
}

// Latest-state rows: a newer row with the same key fully supersedes an older
// one that has not been sent yet. V6 patches carry one complete field group for
// one car, so the key is (row type, field group, car). Returns nullopt for rows
// that must be delivered as sent: events, control rows, multi-car patches.
std::optional<uint64_t> stateKeyOf(uint8_t type, int v6Type, std::string_view json) {
    if (type == 0) {
        if (!json.starts_with("{\"type\":\"playback_state\"")) return std::nullopt;
        type = kPlaybackStateKeyType;
    } else if (type == 6) {
        return std::nullopt;  // race_event: every event matters
    }
    int car = -1;
    if (type == 10) {
        car = intFieldAfter(json, "\"car_idx\":").value_or(-1);
    } else if (v6Type > 0 && (type == 7 || type == 9)) {
        static constexpr std::string_view CAR = "{\"idx\":";
        const size_t first = json.find(CAR);
        if (first == std::string_view::npos) return std::nullopt;
        if (json.find(CAR, first + CAR.size()) != std::string_view::npos) return std::nullopt;
        car = intFieldAfter(json.substr(first), CAR).value_or(-1);
    }
    return (static_cast<uint64_t>(type) << 32) |
        (static_cast<uint64_t>(static_cast<uint16_t>(v6Type)) << 16) |
        static_cast<uint16_t>(car);
}

bool throttledRowType(uint8_t type) {
    return type == 5 || type == 7 || type == 9;
}

template <class T>
std::string writeJson(const T& value) {
    std::string json;
    (void)glz::write_json(value, json);
    return json;
}

std::string phoneLapBlocks(const std::string& json) {
    PairLapBlocksRow row;
    if (glz::read<kPartialRead>(row, std::string_view(json))) return json;
    return writeJson(row);
}

std::string writePublicState(const PairPublicState& value) {
    std::string json;
    (void)glz::write<glz::opts{.skip_null_members = false}>(value, json);
    return json;
}

} // namespace

struct PairServer::Impl {
    // Latest-state rows and binary records waiting to be sent together. While
    // it is the newest unit in the outbox, a row with the same state key
    // replaces the queued one and moves to the end, so the batch holds each
    // key's newest value in the order the values were last updated.
    struct Batch {
        std::vector<std::pair<uint64_t, std::string>> rows;
        std::array<std::vector<uint8_t>, 16> binary;
        bool hasBinary{};
        size_t bytes{};
    };

    // One unit of output, delivered in order: a frame exactly as queued, or a
    // batch. A frame closes the batch before it, so state is never reordered
    // across a control row such as timeline_reset. Frames are sealed by the
    // writer as they are sent, so the cipher counter follows send order; only
    // the HTTP upgrade (raw) and the handshake (plain) go out in clear.
    struct OutboxUnit {
        std::vector<uint8_t> payload;
        uint8_t opcode{0x1};
        bool raw{};
        bool plain{};
        // Drop the connection once this frame is on the wire (refusals).
        bool closeAfter{};
        std::unique_ptr<Batch> batch;
    };

    enum class Stage : uint8_t { Hello, Auth, Approval, Ready };

    struct Client {
        PairSocket socket{kInvalidPairSocket};
        std::atomic<bool> running{true};
        std::thread thread;
        std::thread writer;
        std::atomic<bool> writerDone{false};
        // nowMs() when the writer entered send(), 0 while it is not sending.
        std::atomic<int64_t> sendingSinceMs{0};

        std::mutex outgoingMutex;
        std::condition_variable outgoingReady;
        std::deque<OutboxUnit> outgoing;
        size_t pendingBytes{};
        // Throttled rows that arrived before their key's interval elapsed.
        std::map<uint64_t, std::string> held;
        std::map<uint64_t, int64_t> lastReleasedMs;
        uint64_t supersededRows{};
        uint64_t heldRows{};

        std::vector<uint8_t> incoming;
        bool upgraded{};
        std::atomic<Stage> stage{Stage::Hello};
        pair::Mode mode{pair::Mode::Resume};
        // Reader thread only.
        pair::FrameCipher receiver;
        // Handed to the writer under outgoingMutex; the writer owns the cipher.
        std::optional<pair::Key> pendingSendKey;
        // Code mode: the confirmation the phone must return.
        std::string expectedConfirmation;
        // Set with the server mutex held once the phone may receive telemetry.
        bool authenticated{};
        std::string deviceId;
        uint32_t streamMask{};
        std::vector<uint8_t> v6Types;
        uint64_t participantsRevision{};
        uint64_t connectionId{};
        std::string peer;
        int64_t acceptedAtMs{};
        uint64_t receivedBytes{};
        uint64_t sentBytes{};
    };

    PairServerConfig config;
    StateCallback stateCallback;
    RequirementsCallback requirementsCallback;
    LapDeltaCallback lapDeltaCallback;
    LapDataCallback lapDataCallback;
    DiagnosticCallback diagnosticCallback;
    mutable std::mutex mutex;
    PairSocket listener{kInvalidPairSocket};
    std::atomic<bool> running{false};
    std::thread acceptThread;
    std::vector<std::shared_ptr<Client>> clients;
    std::vector<PairPersistedDevice> devices;
    std::string serverId;
    pair::Identity identity;
    std::string pairingSecret;
    std::string matchingCode;
    int64_t pairingExpiresAt{};
    int pairingAttempts{};
    struct PendingApproval {
        std::weak_ptr<Client> client;
        std::string deviceId;
        std::string name;
        int64_t deadline{};
    };
    std::optional<PendingApproval> pendingApproval;
    std::string lastError;
    std::string latestProtocolStatus;
    std::string latestPlaybackLapBlocks;
    // Restricted data is an absence of rows, so a phone cannot infer it from
    // the stream. Cached for the snapshot and for request_latest re-requests
    // after a reconnect or a dropped frame.
    std::string latestDriverRestriction;
    std::array<std::string, 16> latestRows;
    // V6 playback sends one field group per row, so the newest row of a type
    // says nothing about the other groups. Each group (and car) is cached
    // under its state key so a new phone's snapshot carries all of them.
    std::map<uint64_t, std::string> latestPatches;
    std::array<std::vector<uint8_t>, 16> latestBinary;
    uint64_t participantsRevision{};
    std::optional<uint64_t> sessionUid;
    std::atomic<uint64_t> nextConnectionId{0};
    PairDiscoveryAdvertiser discovery;
#ifdef _WIN32
    bool winsockStarted{};
#endif

    ~Impl() { stop(false); }

    void diagnostic(std::string_view event,
                    const std::shared_ptr<Client>& client = {},
                    std::string_view detail = {}) const {
        if (!diagnosticCallback) return;
        std::ostringstream message;
        message << "at_ms=" << nowMs() << " event=" << event;
        if (client) {
            message << " connection=" << client->connectionId
                    << " peer=" << client->peer;
        }
        if (!detail.empty()) message << " " << detail;
        diagnosticCallback(message.str());
    }

    bool pairingOpenLocked() const {
        return pairingExpiresAt > nowMs();
    }

    struct Requirements {
        uint32_t streamMask{};
        std::vector<uint8_t> v6Types;
    };

    Requirements requirementsLocked() const {
        Requirements out;
        if (!running.load()) return out;
        // Keep the roster current even with no phone connected. Every other
        // family follows the connected clients' replaceable subscriptions.
        out.streamMask = kParticipantsMask;
        std::set<uint8_t> types;
        for (const auto& client : clients) {
            if (!client->running.load() || !client->authenticated) continue;
            out.streamMask |= client->streamMask;
            types.insert(client->v6Types.begin(), client->v6Types.end());
        }
        out.v6Types.assign(types.begin(), types.end());
        return out;
    }

    std::string persistedStateLocked() const {
        return writeJson(PairPersistedState{serverId, config.enabled,
                                            pair::base64Url(identity.seed), devices});
    }

    std::string publicStateLocked() const {
        PairPublicState state;
        state.enabled = running.load();
        state.serverId = serverId;
        state.port = config.port;
        state.pairingOpen = pairingOpenLocked();
        state.pairingExpiresAt = state.pairingOpen ? pairingExpiresAt : 0;
        if (state.pairingOpen) {
            state.matchingCode = matchingCode;
            // k pins the desktop identity: the phone refuses any other key.
            state.qrPayload = "tnrpair://v3/" + serverId + "?h=" + primaryIpv4Address() +
                "&p=" + std::to_string(config.port) + "&s=" + pairingSecret +
                "&e=" + std::to_string(pairingExpiresAt) +
                "&k=" + pair::base64Url(identity.publicKey);
        }
        if (pendingApproval)
            state.pendingDevice = PairPendingDevice{pendingApproval->deviceId,
                                                    pendingApproval->name};
        if (!lastError.empty()) state.error = lastError;
        for (const auto& device : devices) {
            const bool connected = std::any_of(clients.begin(), clients.end(),
                [&](const auto& client) {
                    return client->running.load() && client->authenticated &&
                        client->deviceId == device.id;
                });
            state.devices.push_back({device.id, device.name, device.pairedAt,
                                     device.lastSeenAt, connected});
        }
        return writePublicState(state);
    }

    void notifyState() {
        StateCallback callback;
        std::string publicJson;
        std::string privateJson;
        {
            std::lock_guard lock(mutex);
            callback = stateCallback;
            publicJson = publicStateLocked();
            privateJson = persistedStateLocked();
        }
        if (callback) callback(publicJson, privateJson);
    }

    void notifyRequirements(bool refreshSnapshot = false) {
        RequirementsCallback callback;
        Requirements requirements;
        {
            std::lock_guard lock(mutex);
            callback = requirementsCallback;
            requirements = requirementsLocked();
        }
        if (callback)
            callback(requirements.streamMask, requirements.v6Types, refreshSnapshot);
    }

    // Wakes the client's reader and writer so both threads exit. Safe to call
    // from any thread that does not hold client->outgoingMutex.
    void retire(const std::shared_ptr<Client>& client) {
        client->running.store(false);
        {
            std::lock_guard lock(mutex);
            shutdownPairSocket(client->socket);
        }
        wakeWriter(client);
    }

    static void wakeWriter(const std::shared_ptr<Client>& client) {
        { std::lock_guard lock(client->outgoingMutex); }
        client->outgoingReady.notify_all();
    }

    static Batch& openBatchLocked(Client& client) {
        if (client.outgoing.empty() || !client.outgoing.back().batch) {
            OutboxUnit unit;
            unit.batch = std::make_unique<Batch>();
            client.outgoing.push_back(std::move(unit));
        }
        return *client.outgoing.back().batch;
    }

    static void putRowLocked(Client& client, uint64_t key, std::string row) {
        Batch& batch = openBatchLocked(client);
        auto existing = std::find_if(batch.rows.begin(), batch.rows.end(),
            [&](const auto& entry) { return entry.first == key; });
        if (existing != batch.rows.end()) {
            batch.bytes -= existing->second.size();
            client.pendingBytes -= existing->second.size();
            batch.rows.erase(existing);
            ++client.supersededRows;
        }
        batch.bytes += row.size();
        client.pendingBytes += row.size();
        batch.rows.emplace_back(key, std::move(row));
    }

    // Moves held rows into the open batch: all of them before a frame, else
    // those whose interval has elapsed. Returns the next held row's due time.
    static std::optional<int64_t> releaseHeldLocked(Client& client, int64_t now, bool all) {
        std::optional<int64_t> nextDue;
        for (auto iterator = client.held.begin(); iterator != client.held.end();) {
            const int64_t due = client.lastReleasedMs[iterator->first] + kThrottledRowIntervalMs;
            if (all || due <= now) {
                client.lastReleasedMs[iterator->first] = now;
                putRowLocked(client, iterator->first, std::move(iterator->second));
                iterator = client.held.erase(iterator);
            } else {
                nextDue = nextDue ? std::min(*nextDue, due) : due;
                ++iterator;
            }
        }
        return nextDue;
    }

    bool checkOverflow(const std::shared_ptr<Client>& client, size_t pendingBytes,
                       size_t queuedUnits, size_t frameBytes) {
        if (pendingBytes <= kMaxBufferedBytes) return true;
        diagnostic("client_queue_overflow", client,
            "frame_bytes=" + std::to_string(frameBytes) +
            " pending_bytes=" + std::to_string(pendingBytes) +
            " queued_units=" + std::to_string(queuedUnits) +
            " limit_bytes=" + std::to_string(kMaxBufferedBytes));
        retire(client);
        return false;
    }

    // Queues a frame for in-order delivery.
    bool enqueueFrame(const std::shared_ptr<Client>& client, OutboxUnit unit) {
        if (!client->running.load()) return false;
        const size_t frameBytes = unit.payload.size();
        size_t pendingBytes = 0;
        size_t queuedUnits = 0;
        {
            std::lock_guard lock(client->outgoingMutex);
            // Rows held back by the throttle predate this frame.
            releaseHeldLocked(*client, nowMs(), true);
            client->pendingBytes += frameBytes;
            client->outgoing.push_back(std::move(unit));
            pendingBytes = client->pendingBytes;
            queuedUnits = client->outgoing.size();
        }
        client->outgoingReady.notify_one();
        return checkOverflow(client, pendingBytes, queuedUnits, frameBytes);
    }

    // Queues a latest-state row under its key, superseding an unsent older
    // value. A throttled row waits until its key's interval has elapsed.
    bool enqueueState(const std::shared_ptr<Client>& client, uint64_t key,
                      const std::string& row, bool throttled, bool completeRow) {
        if (!client->running.load()) return false;
        size_t pendingBytes = 0;
        size_t queuedUnits = 0;
        {
            std::lock_guard lock(client->outgoingMutex);
            const int64_t now = nowMs();
            if (throttled && completeRow) {
                // A complete row replaces every patch of its type, so patches
                // still held back are older than it and must not follow it.
                const uint64_t type = key >> 32;
                for (auto iterator = client->held.begin(); iterator != client->held.end();) {
                    if ((iterator->first >> 32) == type && iterator->first != key)
                        iterator = client->held.erase(iterator);
                    else
                        ++iterator;
                }
            }
            const auto released = client->lastReleasedMs.find(key);
            const bool early = released != client->lastReleasedMs.end() &&
                now - released->second < kThrottledRowIntervalMs;
            if (throttled && (client->held.contains(key) || early)) {
                auto& slot = client->held[key];
                if (!slot.empty()) ++client->supersededRows;
                slot = row;
                ++client->heldRows;
            } else {
                if (throttled) client->lastReleasedMs[key] = now;
                putRowLocked(*client, key, row);
            }
            pendingBytes = client->pendingBytes;
            queuedUnits = client->outgoing.size();
        }
        client->outgoingReady.notify_one();
        return checkOverflow(client, pendingBytes, queuedUnits, row.size());
    }

    // Queues binary records, keeping the newest record of each type.
    bool enqueueBinary(const std::shared_ptr<Client>& client,
                       const uint8_t* data, size_t length) {
        if (!client->running.load()) return false;
        size_t pendingBytes = 0;
        size_t queuedUnits = 0;
        {
            std::lock_guard lock(client->outgoingMutex);
            Batch& batch = openBatchLocked(*client);
            (void)bin::forEachPackedRecord(data, length,
                [&](uint8_t type, const uint8_t* record, size_t recordLength) {
                    if (type >= batch.binary.size()) return;
                    auto& slot = batch.binary[type];
                    batch.bytes -= slot.size();
                    client->pendingBytes -= slot.size();
                    slot.assign(record, record + recordLength);
                    batch.bytes += slot.size();
                    client->pendingBytes += slot.size();
                    batch.hasBinary = true;
                });
            pendingBytes = client->pendingBytes;
            queuedUnits = client->outgoing.size();
        }
        client->outgoingReady.notify_one();
        return checkOverflow(client, pendingBytes, queuedUnits, length);
    }

    static OutboxUnit frameUnit(uint8_t opcode, const uint8_t* data, size_t length) {
        OutboxUnit unit;
        unit.opcode = opcode;
        unit.payload.assign(data, data + length);
        return unit;
    }

    static OutboxUnit textUnit(const std::string& json) {
        return frameUnit(0x1, reinterpret_cast<const uint8_t*>(json.data()), json.size());
    }

    // Encrypted text frame: everything after the handshake.
    void sendText(const std::shared_ptr<Client>& client, const std::string& json) {
        enqueueFrame(client, textUnit(json));
    }

    void sendBinary(const std::shared_ptr<Client>& client, const uint8_t* data, size_t length) {
        enqueueFrame(client, frameUnit(0x2, data, length));
    }

    // Handshake frames, before the channel keys exist.
    void sendPlainText(const std::shared_ptr<Client>& client, const std::string& json,
                       bool closeAfter = false) {
        OutboxUnit unit = textUnit(json);
        unit.plain = true;
        unit.closeAfter = closeAfter;
        enqueueFrame(client, std::move(unit));
    }

    void sendRows(const std::shared_ptr<Client>& client,
                  const std::vector<std::string>& rows) {
        if (!rows.empty()) sendText(client, writeJson(PairRowsFrame{"rows", rows}));
    }

    void sendError(const std::shared_ptr<Client>& client, const std::string& code) {
        sendText(client, writeJson(PairErrorFrame{"error", code}));
    }

    // Refuses and drops the connection once the reason is delivered. Sealed
    // when the channel is up, plain during the handshake.
    void refuse(const std::shared_ptr<Client>& client, const std::string& code) {
        OutboxUnit unit = textUnit(writeJson(PairErrorFrame{"error", code}));
        unit.plain = client->stage.load() == Stage::Hello;
        unit.closeAfter = true;
        enqueueFrame(client, std::move(unit));
    }

    void sendCachedParticipants(const std::shared_ptr<Client>& client, bool force) {
        std::string row;
        {
            std::lock_guard lock(mutex);
            if ((client->streamMask & (1u << 8)) == 0 || latestRows[8].empty() ||
                (!force && client->participantsRevision == participantsRevision)) return;
            row = latestRows[8];
            client->participantsRevision = participantsRevision;
        }
        sendRows(client, {row});
    }

    void sendSnapshot(const std::shared_ptr<Client>& client) {
        std::vector<std::string> rows;
        std::vector<uint8_t> binary;
        {
            std::lock_guard lock(mutex);
            if (!latestProtocolStatus.empty()) rows.push_back(latestProtocolStatus);
            if (!latestPlaybackLapBlocks.empty()) rows.push_back(latestPlaybackLapBlocks);
            if (!latestDriverRestriction.empty()) rows.push_back(latestDriverRestriction);
            for (size_t type = 1; type < latestRows.size(); ++type) {
                if ((client->streamMask & (1u << type)) == 0 || latestRows[type].empty())
                    continue;
                if (type == 8 &&
                    client->participantsRevision == participantsRevision) continue;
                rows.push_back(latestRows[type]);
                if (type == 8) client->participantsRevision = participantsRevision;
            }
            for (const auto& [key, row] : latestPatches) {
                const uint64_t type = key >> 32;
                const auto v6Type = static_cast<uint8_t>((key >> 16) & 0xffff);
                if (type >= 32 || (client->streamMask & (1u << type)) == 0) continue;
                if (!client->v6Types.empty() &&
                    !std::binary_search(client->v6Types.begin(), client->v6Types.end(), v6Type))
                    continue;
                rows.push_back(row);
            }
            for (size_t type = 1; type < latestBinary.size(); ++type) {
                if ((client->streamMask & (1u << type)) == 0) continue;
                binary.insert(binary.end(), latestBinary[type].begin(), latestBinary[type].end());
            }
        }
        sendRows(client, rows);
        if (!binary.empty()) sendBinary(client, binary.data(), binary.size());
    }

    // One guess at the code or QR secret. False once the window is spent.
    bool countPairingAttemptLocked() {
        if (++pairingAttempts <= kMaxPairingAttempts) return true;
        clearPairingWindowLocked();
        lastError = kTooManyAttempts;
        return false;
    }

    void clearPairingWindowLocked() {
        pairingSecret.clear();
        matchingCode.clear();
        pairingExpiresAt = 0;
        if (running.load()) discovery.update({serverId, config.name, config.port, false});
    }

    void delayAfterFailure() const {
        std::this_thread::sleep_for(std::chrono::milliseconds(kFailedAttemptDelayMs));
    }

    // The phone may now receive telemetry. Server mutex held.
    PairWelcomeFrame admitLocked(const std::shared_ptr<Client>& client,
                                 const std::string& deviceId) {
        client->authenticated = true;
        client->deviceId = deviceId;
        // The client declares its visible page immediately after the
        // welcome. Keep the stream closed until that first replacement
        // subscription so no broad Android snapshot leaks through.
        client->streamMask = 0;
        client->v6Types.clear();
        PairWelcomeFrame welcome;
        welcome.serverId = serverId;
        welcome.name = config.name;
        if (!latestProtocolStatus.empty()) {
            PairProtocolPeek status;
            if (!glz::read<kPartialRead>(status, std::string_view(latestProtocolStatus))) {
                welcome.protocolYear = status.active_format
                    ? status.active_format : status.detected_format;
                welcome.formula = status.formula;
                welcome.regulations2026 = status.regulations_2026;
            }
        }
        return welcome;
    }

    void admit(const std::shared_ptr<Client>& client, const PairWelcomeFrame& welcome) {
        client->stage.store(Stage::Ready);
        sendText(client, writeJson(welcome));
        notifyRequirements();
        notifyState();
    }

    // Plaintext hello: answer with the signed server_hello and set up the
    // channel. No credential is accepted before the channel exists.
    void handleHello(const std::shared_ptr<Client>& client,
                     const PairIncomingMessage& message) {
        if (message.type != "hello" || message.pairProtocol != kPairProtocolVersion) {
            diagnostic("handshake_rejected", client,
                "reason=unsupported_pair_protocol offered=" +
                std::to_string(message.pairProtocol));
            refuse(client, "unsupported_pair_protocol");
            return;
        }
        if (message.binaryRowsVersion != kBinaryRowsVersion) {
            diagnostic("handshake_rejected", client,
                "reason=unsupported_binary_rows offered=" +
                std::to_string(message.binaryRowsVersion));
            refuse(client, "unsupported_binary_rows");
            return;
        }
        const auto mode = pair::modeFromName(message.mode);
        if (!mode) {
            diagnostic("handshake_rejected", client, "reason=invalid_mode");
            refuse(client, "invalid_handshake");
            return;
        }

        std::string code;
        std::string refusal;
        pair::Identity signer;
        std::string id;
        {
            std::lock_guard lock(mutex);
            if (*mode != pair::Mode::Resume) {
                if (!pairingOpenLocked()) refusal = "pairing_closed";
                else if (pendingApproval) refusal = "pairing_busy";
                // Every code handshake is one guess, whether or not the
                // phone goes on: checking the desktop's confirmation already
                // tells it whether the code was right.
                else if (*mode == pair::Mode::Code && !countPairingAttemptLocked())
                    refusal = "pairing_closed";
                else if (*mode == pair::Mode::Code) code = matchingCode;
            }
            signer = identity;
            id = serverId;
        }
        if (!refusal.empty()) {
            diagnostic("handshake_rejected", client, "reason=" + refusal);
            refuse(client, refusal);
            notifyState();
            return;
        }

        std::string error;
        auto handshake = pair::respond(signer, id, *mode, message.clientKey,
                                       message.cpace, code, &error);
        if (!handshake) {
            diagnostic("handshake_rejected", client, "reason=" + logSafe(error));
            refuse(client, error.empty() ? "invalid_handshake" : error);
            return;
        }
        client->mode = *mode;
        client->expectedConfirmation = std::move(handshake->expectedConfirmation);
        client->receiver.setKey(handshake->receiveKey);
        {
            std::lock_guard lock(client->outgoingMutex);
            client->pendingSendKey = handshake->sendKey;
        }
        sendPlainText(client, handshake->serverHelloJson);
        client->stage.store(Stage::Auth);
        diagnostic("handshake_accepted", client,
            "mode=" + std::string(pair::modeName(*mode)));
    }

    // First encrypted message: the credential for the handshake's mode.
    void handleAuth(const std::shared_ptr<Client>& client,
                    const PairIncomingMessage& message) {
        if (message.type != "auth" || !validToken(message.deviceId, 128)) {
            diagnostic("authentication_rejected", client, "reason=invalid_device");
            refuse(client, "invalid_device");
            return;
        }
        const std::string name = validToken(message.name, 96)
            ? message.name : std::string("Android device");

        if (client->mode == pair::Mode::Resume) {
            const std::string presented = message.token.empty()
                ? std::string{} : pair::tokenHash(message.token);
            PairWelcomeFrame welcome;
            bool accepted = false;
            {
                std::lock_guard lock(mutex);
                auto existing = std::find_if(devices.begin(), devices.end(),
                    [&](const auto& device) { return device.id == message.deviceId; });
                if (existing != devices.end() && !presented.empty() &&
                    constantTimeEqual(existing->tokenHash, presented)) {
                    existing->name = name;
                    existing->lastSeenAt = nowMs();
                    welcome = admitLocked(client, message.deviceId);
                    accepted = true;
                }
            }
            if (!accepted) {
                diagnostic("authentication_rejected", client, "reason=unknown_device");
                delayAfterFailure();
                refuse(client, "unknown_device");
                return;
            }
            diagnostic("authentication_accepted", client,
                "mode=resume device_id=" + logSafe(message.deviceId));
            admit(client, welcome);
            return;
        }

        // QR secret or code confirmation; then the desktop user decides.
        std::string refusal;
        {
            std::lock_guard lock(mutex);
            if (!pairingOpenLocked()) {
                refusal = "pairing_closed";
            } else if (pendingApproval) {
                refusal = "pairing_busy";
            } else {
                const bool proved = client->mode == pair::Mode::Qr
                    ? !pairingSecret.empty() &&
                        constantTimeEqual(pairingSecret, message.secret)
                    : !client->expectedConfirmation.empty() &&
                        constantTimeEqual(client->expectedConfirmation, message.confirm);
                if (!proved) {
                    // A code guess was already counted at hello.
                    if (client->mode == pair::Mode::Qr) countPairingAttemptLocked();
                    refusal = "invalid_pairing_code";
                } else {
                    // One success per window: the secret and code die here.
                    clearPairingWindowLocked();
                    pendingApproval = PendingApproval{
                        client, message.deviceId, name, nowMs() + kApprovalTimeoutMs};
                }
            }
        }
        if (!refusal.empty()) {
            diagnostic("authentication_rejected", client, "reason=" + refusal);
            if (refusal == "invalid_pairing_code") delayAfterFailure();
            refuse(client, refusal);
            notifyState();
            return;
        }
        client->stage.store(Stage::Approval);
        sendText(client, "{\"type\":\"approval_pending\"}");
        diagnostic("approval_requested", client,
            "device_id=" + logSafe(message.deviceId));
        notifyState();
    }

    // The desktop user's answer for the waiting phone (or the timeout's).
    void decidePending(bool approve, const std::string& reason) {
        std::shared_ptr<Client> client;
        std::string deviceId;
        std::string name;
        {
            std::lock_guard lock(mutex);
            if (!pendingApproval) return;
            client = pendingApproval->client.lock();
            deviceId = pendingApproval->deviceId;
            name = pendingApproval->name;
            pendingApproval.reset();
        }
        if (!client || !client->running.load() || client->stage.load() != Stage::Approval) {
            notifyState();
            return;
        }
        if (!approve) {
            diagnostic("approval_refused", client, "reason=" + reason);
            refuse(client, reason);
            notifyState();
            return;
        }
        const std::string token = pair::randomToken(32);
        PairWelcomeFrame welcome;
        {
            std::lock_guard lock(mutex);
            const int64_t now = nowMs();
            PairPersistedDevice device{deviceId, name, pair::tokenHash(token), now, now};
            auto existing = std::find_if(devices.begin(), devices.end(),
                [&](const auto& saved) { return saved.id == deviceId; });
            if (existing == devices.end()) devices.push_back(std::move(device));
            else *existing = std::move(device);
            welcome = admitLocked(client, deviceId);
        }
        welcome.token = token;
        diagnostic("authentication_accepted", client,
            "mode=" + std::string(pair::modeName(client->mode)) +
            " device_id=" + logSafe(deviceId));
        admit(client, welcome);
    }

    void expirePendingApproval() {
        bool expired = false;
        {
            std::lock_guard lock(mutex);
            expired = pendingApproval && pendingApproval->deadline <= nowMs();
        }
        if (expired) decidePending(false, "approval_timeout");
    }

    void handleText(const std::shared_ptr<Client>& client, std::string_view text) {
        PairIncomingMessage message;
        if (glz::read<kPartialRead>(message, text)) {
            diagnostic("message_ignored", client,
                "reason=invalid_json bytes=" + std::to_string(text.size()));
            return;
        }
        switch (client->stage.load()) {
        case Stage::Hello: handleHello(client, message); return;
        case Stage::Auth: handleAuth(client, message); return;
        case Stage::Approval:
            diagnostic("message_ignored", client,
                "reason=awaiting_approval type=" + logSafe(message.type));
            return;
        case Stage::Ready: break;
        }
        if (message.type == "subscribe") {
            {
                std::lock_guard lock(mutex);
                client->streamMask = message.streamMask & kAndroidPageMask;
                client->v6Types = sanitizeV6Types(message.v6Types);
            }
            sendText(client, writeJson(PairSubscribedFrame{
                "subscribed", client->streamMask, 0, "none"
            }));
            sendSnapshot(client);
            notifyRequirements(true);
            diagnostic("subscription_applied", client,
                "stream_mask=" + std::to_string(client->streamMask) +
                " v6_types=" + std::to_string(client->v6Types.size()));
        } else if (message.type == "request_latest" &&
                   message.rowType == "driver_restriction") {
            // Answered even when nothing is cached: "no recording is playing"
            // is the honest answer, and the phone stops asking on the reply.
            std::string row;
            {
                std::lock_guard lock(mutex);
                row = latestDriverRestriction;
            }
            if (row.empty())
                row = writeJson(PairDriverRestrictionFrame{});
            sendRows(client, {row});
        } else if (message.type == "request_latest" &&
                   message.rowType == "participants") {
            sendCachedParticipants(client, true);
        } else if (message.type == "request_lap_delta") {
            LapDeltaCallback callback;
            {
                std::lock_guard lock(mutex);
                callback = lapDeltaCallback;
            }
            std::string data;
            if (callback && message.currentLap > 0 && message.comparisonLap > 0) {
                data = callback(message.currentLap, message.comparisonLap,
                                message.sectorDelta);
            }
            std::string response = "{\"type\":\"lap_delta\",\"requestId\":" +
                std::to_string(message.requestId) + ",\"data\":" +
                (data.empty() ? "null" : data) + "}";
            sendRows(client, {response});
        } else if (message.type == "request_lap_data") {
            LapDataCallback callback;
            {
                std::lock_guard lock(mutex);
                callback = lapDataCallback;
            }
            std::string data;
            if (callback && message.lapNum > 0 && message.channels.size() <= kMaxLapDataChannels)
                data = callback(message.lapNum, message.channels);
            // Its own frame rather than a rows batch: a lap is hundreds of KB,
            // and the phone parses it straight into columns off the UI thread.
            sendText(client, "{\"type\":\"lap_data\",\"requestId\":" +
                std::to_string(message.requestId) + ",\"lapNum\":" +
                std::to_string(message.lapNum) + ",\"data\":" +
                (data.empty() ? "null" : data) + "}");
        } else if (message.type == "ping") {
            sendText(client, writeJson(PairPongFrame{"pong", nowMs()}));
        } else {
            diagnostic("message_ignored", client,
                "reason=unknown_type type=" + logSafe(message.type));
        }
    }

    bool processHandshake(const std::shared_ptr<Client>& client) {
        const std::string request(client->incoming.begin(), client->incoming.end());
        const size_t end = request.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (request.size() > 16 * 1024)
                diagnostic("handshake_rejected", client, "reason=headers_too_large");
            return request.size() <= 16 * 1024;
        }
        const std::string key = headerValue(request.substr(0, end + 4),
                                             "sec-websocket-key");
        if (key.empty()) {
            diagnostic("handshake_rejected", client, "reason=missing_websocket_key");
            return false;
        }
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + webSocketAccept(key) + "\r\n\r\n";
        OutboxUnit upgrade;
        upgrade.raw = true;
        upgrade.payload.assign(response.begin(), response.end());
        enqueueFrame(client, std::move(upgrade));
        client->incoming.erase(client->incoming.begin(),
            client->incoming.begin() + static_cast<std::ptrdiff_t>(end + 4));
        client->upgraded = true;
        diagnostic("handshake_complete", client);
        return true;
    }

    bool processFrames(const std::shared_ptr<Client>& client) {
        size_t offset = 0;
        while (client->incoming.size() - offset >= 2) {
            const uint8_t first = client->incoming[offset];
            const uint8_t second = client->incoming[offset + 1];
            const bool final = (first & 0x80) != 0;
            const uint8_t opcode = first & 0x0f;
            const bool masked = (second & 0x80) != 0;
            uint64_t length = second & 0x7f;
            size_t header = 2;
            if (length == 126) {
                if (client->incoming.size() - offset < 4) break;
                length = (static_cast<uint64_t>(client->incoming[offset + 2]) << 8) |
                    client->incoming[offset + 3];
                header = 4;
            } else if (length == 127) {
                if (client->incoming.size() - offset < 10) break;
                length = 0;
                for (size_t index = 0; index < 8; ++index)
                    length = (length << 8) | client->incoming[offset + 2 + index];
                header = 10;
            }
            if (!masked) {
                diagnostic("frame_rejected", client, "reason=unmasked");
                return false;
            }
            if (!final) {
                diagnostic("frame_rejected", client, "reason=fragmented");
                return false;
            }
            if (length > kMaxFrameBytes) {
                diagnostic("frame_rejected", client,
                    "reason=too_large bytes=" + std::to_string(length));
                return false;
            }
            if (client->incoming.size() - offset < header + 4 + length) break;
            const uint8_t* mask = client->incoming.data() + offset + header;
            const uint8_t* source = mask + 4;
            std::vector<uint8_t> payload(static_cast<size_t>(length));
            for (size_t index = 0; index < payload.size(); ++index)
                payload[index] = static_cast<uint8_t>(source[index] ^ mask[index % 4]);
            offset += header + 4 + payload.size();
            if (opcode == 0x1) {
                // The hello is the only plaintext a phone may send.
                if (client->stage.load() != Stage::Hello) {
                    diagnostic("frame_rejected", client, "reason=plaintext_after_handshake");
                    return false;
                }
                handleText(client, std::string_view(
                    reinterpret_cast<const char*>(payload.data()), payload.size()));
            } else if (opcode == 0x2) {
                uint8_t kind = 0;
                std::vector<uint8_t> message;
                if (!client->receiver.open(payload.data(), payload.size(), kind, message) ||
                    kind != pair::kFrameText) {
                    diagnostic("frame_rejected", client, "reason=undecryptable");
                    return false;
                }
                handleText(client, std::string_view(
                    reinterpret_cast<const char*>(message.data()), message.size()));
            } else if (opcode == 0x8) {
                uint16_t closeCode = 0;
                std::string closeReason;
                if (payload.size() >= 2) {
                    closeCode = static_cast<uint16_t>(payload[0] << 8 | payload[1]);
                    closeReason.assign(payload.begin() + 2, payload.end());
                }
                diagnostic("client_close_frame", client,
                    "code=" + std::to_string(closeCode) +
                    " reason=" + logSafe(closeReason));
                return false;
            } else if (opcode == 0x9) {
                OutboxUnit pong = frameUnit(0xA, payload.data(), payload.size());
                pong.plain = true;
                enqueueFrame(client, std::move(pong));
            } else if (opcode != 0xA) {
                diagnostic("frame_rejected", client,
                    "reason=unsupported_opcode opcode=" + std::to_string(opcode));
                return false;
            }
        }
        if (offset > 0) client->incoming.erase(client->incoming.begin(),
            client->incoming.begin() + static_cast<std::ptrdiff_t>(offset));
        if (client->incoming.size() > kMaxFrameBytes + 14) {
            diagnostic("frame_rejected", client,
                "reason=incoming_buffer_too_large bytes=" +
                std::to_string(client->incoming.size()));
            return false;
        }
        return true;
    }

    // Takes the next outbox unit as one or two ready-to-send frames, waiting
    // for output or for a held row to fall due. False once the client retires.
    // Sealing happens here, on the writer thread, in send order.
    bool nextOutgoing(const std::shared_ptr<Client>& client, pair::FrameCipher& cipher,
                      std::vector<uint8_t>& text, std::vector<uint8_t>& binary,
                      bool& closeAfter) {
        std::unique_lock lock(client->outgoingMutex);
        while (true) {
            if (!client->running.load()) return false;
            const auto nextDue = releaseHeldLocked(*client, nowMs(), false);
            if (!client->outgoing.empty()) break;
            if (nextDue) {
                client->outgoingReady.wait_for(lock,
                    std::chrono::milliseconds(std::max<int64_t>(1, *nextDue - nowMs())));
            } else {
                client->outgoingReady.wait(lock);
            }
        }
        if (client->pendingSendKey) {
            cipher.setKey(*client->pendingSendKey);
            client->pendingSendKey.reset();
        }
        OutboxUnit unit = std::move(client->outgoing.front());
        client->outgoing.pop_front();
        if (!unit.batch) {
            client->pendingBytes -= unit.payload.size();
            lock.unlock();
            closeAfter = unit.closeAfter;
            if (unit.raw) {
                text = std::move(unit.payload);
            } else if (unit.plain) {
                text = webSocketFrame(unit.opcode, unit.payload.data(), unit.payload.size());
            } else {
                if (!cipher.ready()) return false;
                text = sealedFrame(cipher,
                    unit.opcode == 0x2 ? pair::kFrameBinary : pair::kFrameText,
                    unit.payload.data(), unit.payload.size());
            }
            return true;
        }
        Batch& batch = *unit.batch;
        client->pendingBytes -= batch.bytes;
        lock.unlock();
        // Batches only reach admitted phones, so the channel is always up.
        if (!cipher.ready()) return false;
        if (!batch.rows.empty()) {
            PairRowsFrame frame;
            frame.rows.reserve(batch.rows.size());
            for (auto& entry : batch.rows) frame.rows.push_back(std::move(entry.second));
            const std::string json = writeJson(frame);
            text = sealedFrame(cipher, pair::kFrameText,
                reinterpret_cast<const uint8_t*>(json.data()), json.size());
        }
        if (batch.hasBinary) {
            std::vector<uint8_t> records;
            for (const auto& record : batch.binary)
                records.insert(records.end(), record.begin(), record.end());
            binary = sealedFrame(cipher, pair::kFrameBinary, records.data(), records.size());
        }
        return true;
    }

    bool transmit(const std::shared_ptr<Client>& client, PairSocket socket,
                  const std::vector<uint8_t>& frame) {
        if (frame.empty()) return true;
        size_t bytesSent = 0;
        int socketError = 0;
        client->sendingSinceMs.store(nowMs());
        const bool sent = sendAll(socket, frame.data(), frame.size(),
                                  &bytesSent, &socketError);
        client->sendingSinceMs.store(0);
        client->sentBytes += bytesSent;
        if (sent) return true;
        if (client->running.load()) {
            diagnostic("socket_send_failed", client,
                "frame_bytes=" + std::to_string(frame.size()) +
                " partial_bytes=" + std::to_string(bytesSent) +
                " " + pairSocketErrorText(socketError));
        }
        retire(client);
        return false;
    }

    // The socket is passed in because the reader may clear client->socket while
    // this thread is still blocked in send().
    void writerLoop(const std::shared_ptr<Client>& client, PairSocket socket) {
        pair::FrameCipher cipher;
        std::vector<uint8_t> text;
        std::vector<uint8_t> binary;
        bool closeAfter = false;
        while (nextOutgoing(client, cipher, text, binary, closeAfter)) {
            if (!transmit(client, socket, text) || !transmit(client, socket, binary)) break;
            text.clear();
            binary.clear();
            if (closeAfter) {
                retire(client);
                break;
            }
        }
        cipher.clear();
        client->writerDone.store(true);
    }

    // The client's reader. It owns the writer thread and the socket's close.
    void clientLoop(const std::shared_ptr<Client>& client) {
        client->writer = std::thread(
            [this, client, socket = client->socket] { writerLoop(client, socket); });
        std::array<uint8_t, 16 * 1024> buffer{};
        while (running.load() && client->running.load()) {
            const int received = recv(client->socket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0);
            if (received == 0) {
                if (client->running.load()) diagnostic("socket_receive_eof", client,
                    "meaning=peer_closed_tcp_connection");
                break;
            }
            if (received < 0) {
                const int error = pairSocketError();
                if (client->running.load()) diagnostic("socket_receive_failed", client,
                    pairSocketErrorText(error));
                break;
            }
            client->receivedBytes += static_cast<uint64_t>(received);
            client->incoming.insert(client->incoming.end(), buffer.begin(),
                buffer.begin() + received);
            if (!client->upgraded) {
                if (!processHandshake(client)) break;
            }
            if (client->upgraded && !processFrames(client)) break;
        }
        retire(client);
        // shutdown() normally ends a blocked send at once. If it did not, close
        // the socket, which aborts any blocking call still using it.
        const int64_t deadline = nowMs() + kWriterExitGraceMs;
        while (!client->writerDone.load() && nowMs() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        PairSocket socket = kInvalidPairSocket;
        {
            std::lock_guard lock(mutex);
            socket = client->socket;
            client->socket = kInvalidPairSocket;
        }
        if (!client->writerDone.load()) {
            diagnostic("writer_forced_close", client);
            closePairSocket(socket);
            socket = kInvalidPairSocket;
        }
        if (client->writer.joinable()) client->writer.join();
        closePairSocket(socket);
        uint64_t superseded = 0;
        uint64_t heldRows = 0;
        {
            std::lock_guard lock(client->outgoingMutex);
            superseded = client->supersededRows;
            heldRows = client->heldRows;
        }
        {
            std::lock_guard lock(mutex);
            if (pendingApproval) {
                const auto waiting = pendingApproval->client.lock();
                if (!waiting || waiting == client) pendingApproval.reset();
            }
        }
        diagnostic("client_loop_ended", client,
            "server_running=" + std::to_string(running.load() ? 1 : 0) +
            " upgraded=" + std::to_string(client->upgraded ? 1 : 0) +
            " authenticated=" + std::to_string(client->authenticated ? 1 : 0) +
            " lifetime_ms=" + std::to_string(nowMs() - client->acceptedAtMs) +
            " received_bytes=" + std::to_string(client->receivedBytes) +
            " sent_bytes=" + std::to_string(client->sentBytes) +
            " superseded_rows=" + std::to_string(superseded) +
            " throttled_rows=" + std::to_string(heldRows));
        notifyRequirements();
        notifyState();
    }

    // A send that has not completed in kSendStallMs means the phone stopped
    // reading; drop it rather than let its outbox sit at the overflow limit.
    void retireStalledClients() {
        std::vector<std::pair<std::shared_ptr<Client>, int64_t>> stalled;
        const int64_t now = nowMs();
        {
            std::lock_guard lock(mutex);
            for (const auto& client : clients) {
                const int64_t since = client->sendingSinceMs.load();
                if (client->running.load() && since > 0 && now - since > kSendStallMs)
                    stalled.emplace_back(client, now - since);
            }
        }
        for (const auto& [client, blockedMs] : stalled) {
            diagnostic("client_send_stalled", client,
                "blocked_ms=" + std::to_string(blockedMs));
            retire(client);
        }
    }

    void reapClients() {
        std::vector<std::shared_ptr<Client>> retired;
        {
            std::lock_guard lock(mutex);
            auto iterator = clients.begin();
            while (iterator != clients.end()) {
                if (!(*iterator)->running.load()) {
                    retired.push_back(*iterator);
                    iterator = clients.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }
        for (const auto& client : retired)
            if (client->thread.joinable()) client->thread.join();
    }

    void acceptLoop() {
        while (running.load()) {
            sockaddr_in address{};
#ifdef _WIN32
            int addressSize = sizeof(address);
#else
            socklen_t addressSize = sizeof(address);
#endif
            PairSocket socket = accept(listener,
                reinterpret_cast<sockaddr*>(&address), &addressSize);
            if (socket != kInvalidPairSocket) {
                setBlocking(socket);
                configureClientSocket(socket);
                auto client = std::make_shared<Client>();
                client->socket = socket;
                client->connectionId = ++nextConnectionId;
                client->acceptedAtMs = nowMs();
                char peerAddress[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &address.sin_addr, peerAddress,
                              sizeof(peerAddress))) {
                    client->peer = std::string(peerAddress) + ":" +
                        std::to_string(ntohs(address.sin_port));
                } else {
                    client->peer = "unknown";
                }
                {
                    std::lock_guard lock(mutex);
                    clients.push_back(client);
                }
                diagnostic("client_accepted", client);
                client->thread = std::thread([this, client] { clientLoop(client); });
                notifyState();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            retireStalledClients();
            expirePendingApproval();
            reapClients();
        }
        reapClients();
    }

    bool start(std::string* error) {
        if (running.load()) {
            diagnostic("server_start_ignored", {}, "reason=already_running");
            return true;
        }
        lastError.clear();
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            lastError = "Unable to initialize Winsock for paired mode";
            if (error) *error = lastError;
            diagnostic("server_start_failed", {}, "stage=winsock_startup");
            notifyState();
            return false;
        }
        winsockStarted = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidPairSocket) {
            const int socketError = pairSocketError();
            lastError = "Unable to create paired-mode socket";
            if (error) *error = lastError;
            diagnostic("server_start_failed", {},
                "stage=socket " + pairSocketErrorText(socketError));
            stop(false);
            notifyState();
            return false;
        }
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(config.port);
        endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
            const int socketError = pairSocketError();
            lastError = "Unable to bind paired mode on port " +
                std::to_string(config.port);
            if (error) *error = lastError;
            diagnostic("server_start_failed", {},
                "stage=bind port=" + std::to_string(config.port) + " " +
                pairSocketErrorText(socketError));
            stop(false);
            notifyState();
            return false;
        }
        if (listen(listener, 8) != 0) {
            const int socketError = pairSocketError();
            lastError = "Unable to listen for paired mode on port " +
                std::to_string(config.port);
            if (error) *error = lastError;
            diagnostic("server_start_failed", {},
                "stage=listen port=" + std::to_string(config.port) + " " +
                pairSocketErrorText(socketError));
            stop(false);
            notifyState();
            return false;
        }
        setNonBlocking(listener);
        running.store(true);
        config.enabled = true;
        std::string discoveryError;
        if (!discovery.start({serverId, config.name, config.port, false},
                             &discoveryError)) lastError = discoveryError;
        acceptThread = std::thread([this] { acceptLoop(); });
        diagnostic("server_started", {},
            "port=" + std::to_string(config.port) +
            " discovery_error=" + logSafe(discoveryError));
        notifyRequirements();
        notifyState();
        return true;
    }

    void stop(bool persistDisabled) {
        bool hasRuntimeState = false;
        bool preferenceChanged = false;
        {
            std::lock_guard lock(mutex);
            preferenceChanged = persistDisabled && config.enabled;
            if (persistDisabled) config.enabled = false;
            pairingSecret.clear();
            matchingCode.clear();
            pairingExpiresAt = 0;
            pendingApproval.reset();
            hasRuntimeState = running.load() ||
                listener != kInvalidPairSocket || acceptThread.joinable() ||
                !clients.empty();
#ifdef _WIN32
            hasRuntimeState = hasRuntimeState || winsockStarted;
#endif
        }
        // PairServer::Impl owns a defensive RAII stop in its destructor. An
        // Engine also stops the server explicitly in its destructor body while
        // Engine state is still alive. Treat the later defensive stop as a true
        // no-op: notifying requirements there would call back into an Engine
        // whose members are already being destroyed.
        if (!hasRuntimeState) {
            if (preferenceChanged) {
                notifyRequirements();
                notifyState();
            }
            return;
        }
        diagnostic("server_stop_requested", {},
            "persist_disabled=" + std::to_string(persistDisabled ? 1 : 0) +
            " was_running=" + std::to_string(running.load() ? 1 : 0));
        running.store(false);
        discovery.stop();
        // The accept thread is the only other thread that reaps and joins
        // clients. Stop it before joining the remaining workers so two threads
        // can never call join() on the same std::thread concurrently.
        if (acceptThread.joinable()) acceptThread.join();
        shutdownPairSocket(listener);
        closePairSocket(listener);
        listener = kInvalidPairSocket;
        std::vector<std::shared_ptr<Client>> current;
        {
            std::lock_guard lock(mutex);
            current = clients;
            clients.clear();
            for (const auto& client : current) {
                client->running.store(false);
                shutdownPairSocket(client->socket);
            }
        }
        for (const auto& client : current) Impl::wakeWriter(client);
        for (const auto& client : current)
            if (client->thread.joinable()) client->thread.join();
#ifdef _WIN32
        if (winsockStarted) {
            WSACleanup();
            winsockStarted = false;
        }
#endif
        notifyRequirements();
        notifyState();
        diagnostic("server_stopped");
    }
};

PairServer::PairServer() : impl_(std::make_unique<Impl>()) {}
PairServer::~PairServer() = default;

void PairServer::configure(PairServerConfig config, StateCallback stateCallback,
                           RequirementsCallback requirementsCallback,
                           LapDeltaCallback lapDeltaCallback,
                           LapDataCallback lapDataCallback,
                           DiagnosticCallback diagnosticCallback) {
    bool startEnabled = config.enabled;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->config = std::move(config);
        impl_->stateCallback = std::move(stateCallback);
        impl_->requirementsCallback = std::move(requirementsCallback);
        impl_->lapDeltaCallback = std::move(lapDeltaCallback);
        impl_->lapDataCallback = std::move(lapDataCallback);
        impl_->diagnosticCallback = std::move(diagnosticCallback);
        pair::init();
        PairPersistedState persisted;
        std::optional<pair::Key> seed;
        if (!impl_->config.persistedStateJson.empty() &&
            !glz::read<kPartialRead>(persisted,
                std::string_view(impl_->config.persistedStateJson))) {
            if (validToken(persisted.serverId, 128)) impl_->serverId = persisted.serverId;
            seed = pair::keyFromBase64Url(persisted.identitySeed);
            impl_->devices.clear();
            // Phones paired before protocol 3 stored a plain token and never
            // pinned this desktop's key; they pair again.
            if (seed) {
                for (auto& device : persisted.devices) {
                    if (validToken(device.id, 128) && validToken(device.name, 96) &&
                        validToken(device.tokenHash, 128))
                        impl_->devices.push_back(std::move(device));
                }
            }
        }
        impl_->identity = seed ? pair::identityFromSeed(*seed) : pair::newIdentity();
        if (impl_->serverId.empty()) impl_->serverId = hex(randomBytes(16));
    }
    if (startEnabled) impl_->start(nullptr);
    else {
        impl_->diagnostic("server_configured", {}, "enabled=0");
        impl_->notifyState();
    }
}

bool PairServer::start(std::string* error) { return impl_->start(error); }
void PairServer::stop(bool persistDisabled) { impl_->stop(persistDisabled); }

void PairServer::openPairingWindow() {
    if (!impl_->running.load() && !impl_->start(nullptr)) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pairingSecret = pair::randomToken(24);
        impl_->matchingCode = pair::newMatchingCode();
        impl_->pairingAttempts = 0;
        if (impl_->lastError == kTooManyAttempts) impl_->lastError.clear();
        impl_->pairingExpiresAt = nowMs() + kPairWindowMs;
        impl_->discovery.update({impl_->serverId, impl_->config.name,
                                 impl_->config.port, true});
    }
    impl_->diagnostic("pairing_window_opened", {},
        "expires_at_ms=" + std::to_string(impl_->pairingExpiresAt));
    impl_->notifyState();
}

void PairServer::closePairingWindow() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pairingSecret.clear();
        impl_->matchingCode.clear();
        impl_->pairingExpiresAt = 0;
        if (impl_->running.load()) impl_->discovery.update({
            impl_->serverId, impl_->config.name, impl_->config.port, false
        });
    }
    impl_->diagnostic("pairing_window_closed");
    impl_->notifyState();
}

void PairServer::respondToPairing(bool approve) {
    impl_->decidePending(approve, "pairing_denied");
}

void PairServer::removeDevice(const std::string& id) {
    std::vector<std::shared_ptr<Impl::Client>> revoked;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->devices.erase(std::remove_if(impl_->devices.begin(), impl_->devices.end(),
            [&](const auto& device) { return device.id == id; }), impl_->devices.end());
        for (const auto& client : impl_->clients) {
            if (client->deviceId == id) {
                impl_->diagnostic("client_revoked", client,
                    "device_id=" + logSafe(id));
                client->running.store(false);
                shutdownPairSocket(client->socket);
                revoked.push_back(client);
            }
        }
    }
    for (const auto& client : revoked) Impl::wakeWriter(client);
    impl_->notifyRequirements();
    impl_->notifyState();
}

std::string PairServer::publicStateJson() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->publicStateLocked();
}

std::string PairServer::persistedStateJson() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->persistedStateLocked();
}

void PairServer::noteSession(uint64_t sessionUid) {
    std::vector<std::shared_ptr<Impl::Client>> recipients;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->sessionUid && *impl_->sessionUid == sessionUid) return;
        const bool changed = impl_->sessionUid.has_value();
        impl_->sessionUid = sessionUid;
        if (!changed) return;

        // Participants rows intentionally omit timestamps and session identity
        // so identical five-second updates dedupe cleanly. Invalidate only that
        // cached family at a header UID boundary. The tiny reset row prevents a
        // phone from treating the previous session's roster as current if the
        // game's first Participants datagram is lost on the desktop UDP side.
        impl_->latestRows[8].clear();
        ++impl_->participantsRevision;
        if (impl_->running.load()) {
            for (const auto& client : impl_->clients) {
                if (client->running.load() && client->authenticated &&
                    (client->streamMask & kParticipantsMask) != 0)
                    recipients.push_back(client);
            }
        }
    }
    for (const auto& client : recipients)
        impl_->sendRows(client, {"{\"type\":\"participants_reset\"}"});
}

void PairServer::publishRow(const std::string& source) {
    const std::string json = source.starts_with("{\"type\":\"playback_lap_blocks\"")
        ? phoneLapBlocks(source) : source;
    const uint8_t type = rowTypeOf(json);
    const int v6Type = v6TypeOf(json);
    const std::optional<uint64_t> stateKey = stateKeyOf(type, v6Type, json);
    std::vector<std::shared_ptr<Impl::Client>> recipients;
    bool participantsChanged = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (json.starts_with("{\"type\":\"timeline_reset\"")) {
            impl_->latestRows = {};
            impl_->latestPatches.clear();
            impl_->latestBinary = {};
            ++impl_->participantsRevision;
        }
        if (json.starts_with("{\"type\":\"protocol_status\""))
            impl_->latestProtocolStatus = json;
        if (json.starts_with("{\"type\":\"playback_loaded\"") ||
            json.starts_with("{\"type\":\"playback_close\"")) {
            impl_->latestPlaybackLapBlocks.clear();
            impl_->latestDriverRestriction.clear();
        } else if (json.starts_with("{\"type\":\"playback_lap_blocks\"")) {
            impl_->latestPlaybackLapBlocks = json;
        } else if (json.starts_with("{\"type\":\"driver_restriction\"")) {
            impl_->latestDriverRestriction = json;
        }
        if (type > 0 && type < impl_->latestRows.size()) {
            if (type == 8 && impl_->latestRows[type] != json) {
                ++impl_->participantsRevision;
                participantsChanged = true;
            }
            if (v6Type > 0 && stateKey && type != 8) {
                impl_->latestPatches[*stateKey] = json;
            } else {
                impl_->latestRows[type] = json;
                // A complete row supersedes every cached patch of its type.
                if (v6Type == 0) {
                    const uint64_t first = static_cast<uint64_t>(type) << 32;
                    impl_->latestPatches.erase(impl_->latestPatches.lower_bound(first),
                        impl_->latestPatches.lower_bound(first + (1ull << 32)));
                }
            }
        }
        if (!impl_->running.load()) return;
        for (const auto& client : impl_->clients) {
            if (!client->running.load() || !client->authenticated) continue;
            // The engine reads the union of every consumer's V6 fields; a phone
            // gets only the field groups it asked for.
            if (v6Type > 0 && !client->v6Types.empty() &&
                !std::binary_search(client->v6Types.begin(), client->v6Types.end(),
                                    static_cast<uint8_t>(v6Type))) continue;
            if (type == 8) {
                if (!participantsChanged || (client->streamMask & (1u << 8)) == 0 ||
                    client->participantsRevision == impl_->participantsRevision) continue;
                client->participantsRevision = impl_->participantsRevision;
            } else if (type > 0) {
                if ((client->streamMask & (1u << type)) == 0) continue;
            } else if (!allowedControlRow(json)) {
                continue;
            }
            recipients.push_back(client);
        }
    }
    if (stateKey) {
        const bool throttled = throttledRowType(type);
        for (const auto& client : recipients)
            impl_->enqueueState(client, *stateKey, json, throttled, v6Type == 0);
        return;
    }
    const std::string frame = writeJson(PairRowsFrame{"rows", {json}});
    for (const auto& client : recipients) impl_->sendText(client, frame);
}

void PairServer::publishBinary(const uint8_t* data, size_t length) {
    if (!data || length == 0) return;
    std::vector<std::pair<std::shared_ptr<Impl::Client>, std::vector<uint8_t>>> payloads;
    {
        std::lock_guard lock(impl_->mutex);
        (void)bin::forEachPackedRecord(data, length,
            [&](uint8_t type, const uint8_t* record, size_t recordLength) {
                if (type < impl_->latestBinary.size())
                    impl_->latestBinary[type].assign(record, record + recordLength);
            });
        if (!impl_->running.load()) return;
        for (const auto& client : impl_->clients) {
            if (!client->running.load() || !client->authenticated) continue;
            std::vector<uint8_t> selected;
            selected.reserve(length);
            if (bin::appendFilteredBatch(selected, data, length, client->streamMask) &&
                !selected.empty()) payloads.emplace_back(client, std::move(selected));
        }
    }
    for (auto& [client, payload] : payloads)
        impl_->enqueueBinary(client, payload.data(), payload.size());
}

void PairServer::publishSeekSnapshot(const uint8_t* data, size_t length,
                                     const std::string& coldJson) {
    // A seek flush contains a history window. Paired displays only need the
    // newest value for each hot row family, matching the normal latest-state
    // snapshot rather than replaying the entire window over the socket.
    if (data && length > 0) {
        std::array<std::vector<uint8_t>, 16> latestBinary;
        (void)bin::forEachPackedRecord(data, length,
            [&](uint8_t type, const uint8_t* record, size_t recordLength) {
                if (type < latestBinary.size())
                    latestBinary[type].assign(record, record + recordLength);
            });
        std::vector<uint8_t> snapshot;
        for (const auto& record : latestBinary)
            snapshot.insert(snapshot.end(), record.begin(), record.end());
        if (!snapshot.empty()) publishBinary(snapshot.data(), snapshot.size());
    }
    size_t start = 0;
    std::array<std::string, 16> latest;
    std::vector<std::string> controls;
    while (start < coldJson.size()) {
        size_t end = coldJson.find('\n', start);
        if (end == std::string::npos) end = coldJson.size();
        if (end > start) {
            std::string row = coldJson.substr(start, end - start);
            const uint8_t type = rowTypeOf(row);
            if (type > 0) latest[type] = std::move(row);
            else if (allowedControlRow(row)) controls.push_back(std::move(row));
        }
        start = end + 1;
    }
    for (const auto& row : latest) if (!row.empty()) publishRow(row);
    for (const auto& row : controls) publishRow(row);
}

} // namespace tnrp
