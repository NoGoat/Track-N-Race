#include "tnrp/PairCrypto.h"

#include <algorithm>
#include <cstring>

#include <glaze/glaze.hpp>
#include <sodium.h>

#include "crypto_cpace.h"

namespace tnrp::pair {

// External linkage is required for glaze reflection under MSVC.
struct PairHelloMessage {
    std::string type{"hello"};
    int pairProtocol{kProtocolVersion};
    int binaryRowsVersion{kBinaryRowsVersion};
    std::string mode;
    std::string clientKey;
    std::optional<std::string> cpace;
};

struct PairServerHelloMessage {
    std::string type{"server_hello"};
    int pairProtocol{kProtocolVersion};
    std::string serverId;
    std::string identityKey;
    std::string serverKey;
    std::string signature;
    std::optional<std::string> cpace;
    std::optional<std::string> confirm;
    // Present instead of the fields above when the desktop refuses.
    std::optional<std::string> code;
};

namespace {

constexpr std::string_view kTranscriptLabel = "TNRPAIR3 transcript";
constexpr std::string_view kSignatureLabel = "TNRPAIR3 server signature";
constexpr std::string_view kCpaceAssociated = "TNRPAIR3 cpace";
constexpr std::string_view kPhoneId = "tnr-phone";
constexpr std::string_view kCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr size_t kCodeLength = 8;

constexpr glz::opts kPartialRead{
    .null_terminated = false,
    .error_on_unknown_keys = false,
};

class Transcript {
public:
    Transcript() {
        crypto_generichash_init(&state_, nullptr, 0, kKeyBytes);
        add(kTranscriptLabel);
    }
    void add(const uint8_t* data, size_t length) {
        uint8_t prefix[4];
        for (int index = 0; index < 4; ++index)
            prefix[index] = static_cast<uint8_t>(length >> (8 * index));
        crypto_generichash_update(&state_, prefix, sizeof(prefix));
        if (length) crypto_generichash_update(&state_, data, length);
    }
    void add(std::string_view text) {
        add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
    void add(const Key& key) { add(key.data(), key.size()); }
    void add(const std::vector<uint8_t>& bytes) { add(bytes.data(), bytes.size()); }
    Key finish() {
        Key out{};
        crypto_generichash_final(&state_, out.data(), out.size());
        return out;
    }

private:
    crypto_generichash_state state_{};
};

// keyed BLAKE2b(label || transcript): directional keys and confirmations.
Key keyedDigest(const uint8_t* key, std::string_view label, const Key& transcript) {
    std::vector<uint8_t> message(label.begin(), label.end());
    message.insert(message.end(), transcript.begin(), transcript.end());
    Key out{};
    crypto_generichash(out.data(), out.size(), message.data(), message.size(),
                       key, kKeyBytes);
    return out;
}

std::vector<uint8_t> signedMessage(const Key& transcript) {
    std::vector<uint8_t> message(kSignatureLabel.begin(), kSignatureLabel.end());
    message.insert(message.end(), transcript.begin(), transcript.end());
    return message;
}

std::vector<uint8_t> cpaceAssociatedData(const Key& clientKey) {
    std::vector<uint8_t> data(kCpaceAssociated.begin(), kCpaceAssociated.end());
    data.insert(data.end(), clientKey.begin(), clientKey.end());
    return data;
}

Key transcriptOf(Mode mode, const Key& clientKey, const Key& serverKey,
                 const Key& identityKey, std::string_view serverId,
                 const std::vector<uint8_t>& cpaceMessage,
                 const std::vector<uint8_t>& cpaceResponse) {
    Transcript transcript;
    transcript.add(modeName(mode));
    transcript.add(clientKey);
    transcript.add(serverKey);
    transcript.add(identityKey);
    transcript.add(serverId);
    transcript.add(cpaceMessage);
    transcript.add(cpaceResponse);
    return transcript.finish();
}

template <class T>
std::string writeJson(const T& value) {
    std::string json;
    (void)glz::write_json(value, json);
    return json;
}

void fail(std::string* error, std::string_view message) {
    if (error) *error = std::string(message);
}

} // namespace

bool init() {
    static const bool ready = crypto_cpace_init() >= 0;
    return ready;
}

std::string base64Url(const uint8_t* data, size_t length) {
    const size_t size = sodium_base64_ENCODED_LEN(
        length, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string out(size, '\0');
    sodium_bin2base64(out.data(), out.size(), data, length,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::vector<uint8_t>> fromBase64Url(std::string_view text) {
    if (text.size() > 4096) return std::nullopt;
    std::vector<uint8_t> out(text.size());
    size_t length = 0;
    const char* end = nullptr;
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr,
                          &length, &end, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
        end != text.data() + text.size()) return std::nullopt;
    out.resize(length);
    return out;
}

std::optional<Key> keyFromBase64Url(std::string_view text) {
    const auto bytes = fromBase64Url(text);
    if (!bytes || bytes->size() != kKeyBytes) return std::nullopt;
    Key key{};
    std::memcpy(key.data(), bytes->data(), key.size());
    return key;
}

std::string randomToken(size_t bytes) {
    init();
    std::vector<uint8_t> buffer(bytes);
    randombytes_buf(buffer.data(), buffer.size());
    std::string token = base64Url(buffer.data(), buffer.size());
    sodium_memzero(buffer.data(), buffer.size());
    return token;
}

std::string tokenHash(std::string_view token) {
    init();
    Key digest{};
    crypto_generichash(digest.data(), digest.size(),
                       reinterpret_cast<const uint8_t*>(token.data()), token.size(),
                       nullptr, 0);
    return base64Url(digest);
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

std::string newMatchingCode() {
    init();
    std::string code;
    code.reserve(kCodeLength);
    for (size_t index = 0; index < kCodeLength; ++index)
        code.push_back(kCodeAlphabet[randombytes_uniform(
            static_cast<uint32_t>(kCodeAlphabet.size()))]);
    return code;
}

std::string normalizeMatchingCode(std::string_view code) {
    std::string out;
    for (const char raw : code) {
        if (raw == ' ' || raw == '-') continue;
        const char ch = (raw >= 'a' && raw <= 'z') ? static_cast<char>(raw - 'a' + 'A') : raw;
        if (kCodeAlphabet.find(ch) == std::string_view::npos) return {};
        out.push_back(ch);
    }
    return out.size() == kCodeLength ? out : std::string{};
}

std::string_view modeName(Mode mode) {
    switch (mode) {
    case Mode::Resume: return "resume";
    case Mode::Qr: return "qr";
    case Mode::Code: return "code";
    }
    return "resume";
}

std::optional<Mode> modeFromName(std::string_view name) {
    if (name == "resume") return Mode::Resume;
    if (name == "qr") return Mode::Qr;
    if (name == "code") return Mode::Code;
    return std::nullopt;
}

Identity newIdentity() {
    init();
    Key seed{};
    randombytes_buf(seed.data(), seed.size());
    Identity identity = identityFromSeed(seed);
    sodium_memzero(seed.data(), seed.size());
    return identity;
}

Identity identityFromSeed(const Key& seed) {
    init();
    Identity identity;
    identity.seed = seed;
    crypto_sign_seed_keypair(identity.publicKey.data(), identity.secretKey.data(), seed.data());
    return identity;
}

// ── FrameCipher ──────────────────────────────────────────────────────────

void FrameCipher::setKey(const Key& key) {
    key_ = key;
    counter_ = 0;
    ready_ = true;
}

void FrameCipher::clear() {
    sodium_memzero(key_.data(), key_.size());
    counter_ = 0;
    ready_ = false;
}

static void counterNonce(uint64_t counter,
                         uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES]) {
    std::memset(nonce, 0, crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    for (int index = 0; index < 8; ++index)
        nonce[index] = static_cast<uint8_t>(counter >> (8 * index));
}

void FrameCipher::sealInto(uint8_t* out, uint8_t kind, const uint8_t* data, size_t length) {
    uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    counterNonce(counter_++, nonce);
    out[0] = kind;
    unsigned long long written = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + 1, &written, data, length,
                                              out, 1, nullptr, nonce, key_.data());
}

std::vector<uint8_t> FrameCipher::seal(uint8_t kind, const uint8_t* data, size_t length) {
    std::vector<uint8_t> out(length + kOverhead);
    sealInto(out.data(), kind, data, length);
    return out;
}

bool FrameCipher::open(const uint8_t* data, size_t length, uint8_t& kind,
                       std::vector<uint8_t>& out) {
    if (!ready_ || length < kOverhead) return false;
    uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    counterNonce(counter_, nonce);
    out.resize(length - kOverhead);
    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out.data(), &written, nullptr,
            data + 1, length - 1, data, 1, nonce, key_.data()) != 0) return false;
    out.resize(static_cast<size_t>(written));
    kind = data[0];
    ++counter_;
    return true;
}

// ── Desktop handshake ────────────────────────────────────────────────────

std::optional<ServerHandshake> respond(const Identity& identity, std::string_view serverId,
                                       Mode mode, std::string_view clientKeyText,
                                       std::string_view cpaceText,
                                       std::string_view matchingCode,
                                       std::string* error) {
    if (!init()) { fail(error, "crypto_unavailable"); return std::nullopt; }
    const auto clientKey = keyFromBase64Url(clientKeyText);
    if (!clientKey) { fail(error, "invalid_handshake"); return std::nullopt; }

    Key serverPublic{};
    Key serverSecret{};
    crypto_kx_keypair(serverPublic.data(), serverSecret.data());

    std::vector<uint8_t> cpaceMessage;
    std::vector<uint8_t> cpaceResponse;
    crypto_cpace_shared_keys cpaceKeys{};
    if (mode == Mode::Code) {
        const auto message = fromBase64Url(cpaceText);
        const std::string code = normalizeMatchingCode(matchingCode);
        if (!message || message->size() != crypto_cpace_PUBLICDATABYTES ||
            code.empty() || serverId.size() > 255) {
            fail(error, "invalid_handshake");
            return std::nullopt;
        }
        cpaceMessage = *message;
        cpaceResponse.resize(crypto_cpace_RESPONSEBYTES);
        const auto associated = cpaceAssociatedData(*clientKey);
        if (crypto_cpace_step2(cpaceResponse.data(), cpaceMessage.data(), &cpaceKeys,
                code.data(), code.size(),
                kPhoneId.data(), static_cast<unsigned char>(kPhoneId.size()),
                serverId.data(), static_cast<unsigned char>(serverId.size()),
                associated.data(), associated.size()) != 0) {
            fail(error, "invalid_handshake");
            return std::nullopt;
        }
    }

    const Key transcript = transcriptOf(mode, *clientKey, serverPublic, identity.publicKey,
                                        serverId, cpaceMessage, cpaceResponse);
    Key receive{};
    Key send{};
    if (crypto_kx_server_session_keys(receive.data(), send.data(), serverPublic.data(),
                                      serverSecret.data(), clientKey->data()) != 0) {
        sodium_memzero(serverSecret.data(), serverSecret.size());
        fail(error, "invalid_handshake");
        return std::nullopt;
    }
    sodium_memzero(serverSecret.data(), serverSecret.size());

    const auto message = signedMessage(transcript);
    std::array<uint8_t, crypto_sign_BYTES> signature{};
    crypto_sign_detached(signature.data(), nullptr, message.data(), message.size(),
                         identity.secretKey.data());

    ServerHandshake result;
    result.receiveKey = keyedDigest(receive.data(), "c2s", transcript);
    result.sendKey = keyedDigest(send.data(), "s2c", transcript);
    sodium_memzero(receive.data(), receive.size());
    sodium_memzero(send.data(), send.size());

    PairServerHelloMessage hello;
    hello.serverId = std::string(serverId);
    hello.identityKey = base64Url(identity.publicKey);
    hello.serverKey = base64Url(serverPublic);
    hello.signature = base64Url(signature.data(), signature.size());
    if (mode == Mode::Code) {
        hello.cpace = base64Url(cpaceResponse.data(), cpaceResponse.size());
        hello.confirm = base64Url(keyedDigest(cpaceKeys.server_sk, "server confirm", transcript));
        result.expectedConfirmation =
            base64Url(keyedDigest(cpaceKeys.client_sk, "client confirm", transcript));
        sodium_memzero(&cpaceKeys, sizeof(cpaceKeys));
    }
    result.serverHelloJson = writeJson(hello);
    return result;
}

// ── Phone handshake ──────────────────────────────────────────────────────

struct ClientHandshake::State {
    crypto_cpace_state cpace{};
};

ClientHandshake::ClientHandshake() = default;

ClientHandshake::~ClientHandshake() {
    sodium_memzero(ephemeralSecret_.data(), ephemeralSecret_.size());
    if (cpace_) {
        sodium_memzero(cpace_, sizeof(State));
        delete cpace_;
    }
    send_.clear();
    receive_.clear();
}

bool ClientHandshake::begin(Mode mode, std::string serverId, std::string pinnedIdentityKey,
                            std::string matchingCode, std::string* error) {
    if (!init()) { fail(error, "Encryption is unavailable"); return false; }
    mode_ = mode;
    serverId_ = std::move(serverId);
    identityKey_ = std::move(pinnedIdentityKey);
    if (mode != Mode::Code && !keyFromBase64Url(identityKey_)) {
        fail(error, "The saved desktop identity is invalid. Pair again.");
        return false;
    }
    crypto_kx_keypair(ephemeralPublic_.data(), ephemeralSecret_.data());

    PairHelloMessage hello;
    hello.mode = std::string(modeName(mode));
    hello.clientKey = base64Url(ephemeralPublic_);
    if (mode == Mode::Code) {
        matchingCode_ = normalizeMatchingCode(matchingCode);
        if (matchingCode_.empty()) {
            fail(error, "Enter the 8-character code shown on the desktop");
            return false;
        }
        if (serverId_.empty() || serverId_.size() > 255) {
            fail(error, "Select a desktop first");
            return false;
        }
        cpace_ = new State();
        cpaceMessage_.resize(crypto_cpace_PUBLICDATABYTES);
        const auto associated = cpaceAssociatedData(ephemeralPublic_);
        if (crypto_cpace_step1(&cpace_->cpace, cpaceMessage_.data(),
                matchingCode_.data(), matchingCode_.size(),
                kPhoneId.data(), static_cast<unsigned char>(kPhoneId.size()),
                serverId_.data(), static_cast<unsigned char>(serverId_.size()),
                associated.data(), associated.size()) != 0) {
            fail(error, "Could not start pairing");
            return false;
        }
        hello.cpace = base64Url(cpaceMessage_.data(), cpaceMessage_.size());
    }
    hello_ = writeJson(hello);
    return true;
}

bool ClientHandshake::accept(std::string_view json, std::string* error) {
    PairServerHelloMessage hello;
    if (glz::read<kPartialRead>(hello, json)) {
        fail(error, "invalid_server_hello");
        return false;
    }
    if (hello.code) {
        fail(error, *hello.code);
        return false;
    }
    if (hello.type != "server_hello" || hello.pairProtocol != kProtocolVersion) {
        fail(error, "unsupported_pair_protocol");
        return false;
    }
    if (!serverId_.empty() && hello.serverId != serverId_) {
        fail(error, "desktop_identity_mismatch");
        return false;
    }
    if (mode_ != Mode::Code && !constantTimeEqual(hello.identityKey, identityKey_)) {
        fail(error, "desktop_identity_mismatch");
        return false;
    }
    const auto identity = keyFromBase64Url(hello.identityKey);
    const auto serverKey = keyFromBase64Url(hello.serverKey);
    const auto signature = fromBase64Url(hello.signature);
    if (!identity || !serverKey || !signature || signature->size() != crypto_sign_BYTES) {
        fail(error, "invalid_server_hello");
        return false;
    }

    std::vector<uint8_t> cpaceResponse;
    crypto_cpace_shared_keys cpaceKeys{};
    if (mode_ == Mode::Code) {
        const auto response = hello.cpace ? fromBase64Url(*hello.cpace) : std::nullopt;
        if (!cpace_ || !response || response->size() != crypto_cpace_RESPONSEBYTES ||
            !hello.confirm ||
            crypto_cpace_step3(&cpace_->cpace, &cpaceKeys, response->data()) != 0) {
            fail(error, "invalid_server_hello");
            return false;
        }
        cpaceResponse = *response;
    }

    const Key transcript = transcriptOf(mode_, ephemeralPublic_, *serverKey, *identity,
                                        hello.serverId, cpaceMessage_, cpaceResponse);
    if (mode_ == Mode::Code) {
        // A wrong code yields unrelated keys, so this MAC is the code check.
        const std::string expected =
            base64Url(keyedDigest(cpaceKeys.server_sk, "server confirm", transcript));
        const bool matches = constantTimeEqual(expected, *hello.confirm);
        confirmation_ = base64Url(keyedDigest(cpaceKeys.client_sk, "client confirm", transcript));
        sodium_memzero(&cpaceKeys, sizeof(cpaceKeys));
        if (!matches) {
            confirmation_.clear();
            fail(error, "invalid_pairing_code");
            return false;
        }
    }
    const auto message = signedMessage(transcript);
    if (crypto_sign_verify_detached(signature->data(), message.data(), message.size(),
                                    identity->data()) != 0) {
        fail(error, "desktop_identity_mismatch");
        return false;
    }

    Key receive{};
    Key send{};
    if (crypto_kx_client_session_keys(receive.data(), send.data(), ephemeralPublic_.data(),
                                      ephemeralSecret_.data(), serverKey->data()) != 0) {
        fail(error, "invalid_server_hello");
        return false;
    }
    sodium_memzero(ephemeralSecret_.data(), ephemeralSecret_.size());
    send_.setKey(keyedDigest(send.data(), "c2s", transcript));
    receive_.setKey(keyedDigest(receive.data(), "s2c", transcript));
    sodium_memzero(receive.data(), receive.size());
    sodium_memzero(send.data(), send.size());
    identityKey_ = hello.identityKey;
    serverId_ = hello.serverId;
    return true;
}

} // namespace tnrp::pair
