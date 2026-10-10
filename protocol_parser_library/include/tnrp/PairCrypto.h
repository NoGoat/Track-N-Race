#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Paired-display channel security, shared by the desktop PairServer and the
// Android client so both ends run the same code.
//
// Handshake (WebSocket text frames, public values only):
//   phone   -> hello        X25519 ephemeral key; in code mode a CPace message
//   desktop -> server_hello X25519 ephemeral key, Ed25519 identity key and a
//                           signature over the transcript; in code mode the
//                           CPace reply and a key-confirmation MAC
// The phone accepts the identity key only if it matches the pinned key (resume),
// the key in the scanned QR (qr), or the CPace confirmation proves the desktop
// knows the matching code (code). Both directions are then ChaCha20-Poly1305
// with keys from the ephemeral exchange and an implicit per-direction counter,
// so frames cannot be read, altered, replayed or reordered. Credentials (resume
// token, QR secret, code confirmation) only travel inside that channel.
namespace tnrp::pair {

constexpr int kProtocolVersion = 3;
// Layout version of the packed hot-row records (BinaryRows.h).
constexpr int kBinaryRowsVersion = 2;
constexpr size_t kKeyBytes = 32;
constexpr uint8_t kFrameText = 1;
constexpr uint8_t kFrameBinary = 2;

using Key = std::array<uint8_t, kKeyBytes>;

// Initialises libsodium. Safe to call repeatedly from any thread.
bool init();

std::string base64Url(const uint8_t* data, size_t length);
inline std::string base64Url(const Key& key) { return base64Url(key.data(), key.size()); }
std::optional<std::vector<uint8_t>> fromBase64Url(std::string_view text);
std::optional<Key> keyFromBase64Url(std::string_view text);

std::string randomToken(size_t bytes);
// BLAKE2b digest of a reconnect token; the desktop stores only this.
std::string tokenHash(std::string_view token);
bool constantTimeEqual(std::string_view left, std::string_view right);

// Eight characters from an alphabet without 0/O, 1/I/L.
std::string newMatchingCode();
// Uppercases and drops spaces and dashes; empty if a character is invalid.
std::string normalizeMatchingCode(std::string_view code);

enum class Mode { Resume, Qr, Code };
std::string_view modeName(Mode mode);
std::optional<Mode> modeFromName(std::string_view name);

// Desktop signing identity. Only the seed is persisted.
struct Identity {
    Key seed{};
    Key publicKey{};
    std::array<uint8_t, 64> secretKey{};
};
Identity newIdentity();
Identity identityFromSeed(const Key& seed);

// One direction of the encrypted channel. Wire form: kind byte, then the
// ciphertext and tag; the kind is authenticated as associated data.
class FrameCipher {
public:
    static constexpr size_t kOverhead = 1 + 16;

    void setKey(const Key& key);
    bool ready() const { return ready_; }
    // Writes kOverhead + length bytes to out.
    void sealInto(uint8_t* out, uint8_t kind, const uint8_t* data, size_t length);
    std::vector<uint8_t> seal(uint8_t kind, const uint8_t* data, size_t length);
    // False if the frame is forged, replayed or out of order.
    bool open(const uint8_t* data, size_t length, uint8_t& kind, std::vector<uint8_t>& out);
    void clear();

private:
    Key key_{};
    uint64_t counter_{};
    bool ready_{};
};

// Desktop side of the handshake for one connection.
struct ServerHandshake {
    std::string serverHelloJson;
    Key receiveKey{};
    Key sendKey{};
    // Code mode: the confirmation the phone must return inside the channel.
    std::string expectedConfirmation;
};

std::optional<ServerHandshake> respond(const Identity& identity, std::string_view serverId,
                                       Mode mode, std::string_view clientKey,
                                       std::string_view cpaceMessage,
                                       std::string_view matchingCode,
                                       std::string* error);

// Phone side of the handshake for one connection.
class ClientHandshake {
public:
    ClientHandshake();
    ~ClientHandshake();
    ClientHandshake(const ClientHandshake&) = delete;
    ClientHandshake& operator=(const ClientHandshake&) = delete;

    // pinnedIdentityKey is required for Resume and Qr; serverId and code for
    // Code (the id from discovery is bound into the PAKE).
    bool begin(Mode mode, std::string serverId, std::string pinnedIdentityKey,
               std::string matchingCode, std::string* error);
    const std::string& helloJson() const { return hello_; }
    bool accept(std::string_view serverHelloJson, std::string* error);

    const std::string& identityKey() const { return identityKey_; }
    const std::string& serverId() const { return serverId_; }
    // Code mode: the MAC proving this phone knew the code. Empty otherwise.
    const std::string& confirmation() const { return confirmation_; }
    FrameCipher& sender() { return send_; }
    FrameCipher& receiver() { return receive_; }

private:
    struct State;
    Mode mode_{Mode::Resume};
    std::string serverId_;
    std::string identityKey_;
    std::string matchingCode_;
    std::string hello_;
    std::string confirmation_;
    Key ephemeralPublic_{};
    Key ephemeralSecret_{};
    std::vector<uint8_t> cpaceMessage_;
    State* cpace_{};
    FrameCipher send_;
    FrameCipher receive_;
};

} // namespace tnrp::pair
