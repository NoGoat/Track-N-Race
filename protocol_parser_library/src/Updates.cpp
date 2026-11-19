#include "tnrp/Updates.h"

#include <sodium.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace tnrp {
// Named (not anonymous) so glaze can reflect these.
namespace update_wire {

struct ServiceResponse {
    std::string manifest;
    std::string signature;
};

struct ManifestFile {
    std::string product;
    std::string os;
    std::string arch;
    std::string format;
    std::string name;
    std::string url;
    std::string sha512;
    uint64_t size = 0;
};

struct Manifest {
    int schema = 0;
    std::string version;
    std::string publishedAt;
    std::string notesUrl;
    std::vector<ManifestFile> files;
};

}  // namespace update_wire

namespace {

using update_wire::Manifest;
using update_wire::ManifestFile;
using update_wire::ServiceResponse;

// The release signing key's public half (publish-release.mjs keygen). The
// private key never enters the repo.
constexpr std::string_view kReleasePublicKey = "w6xcQVUaTnS2eKJdyLopSmSFdr8Vv+6DWe+74wGNBCg=";

// The deployed update_worker (update_worker/wrangler.toml). If it can't be
// reached, frontends fall back to the GitHub release check, which can only
// point at the download page.
constexpr std::string_view kUpdateServiceUrl = "https://api.track-n-race.com";

constexpr glz::opts kLenient{.error_on_unknown_keys = false};

struct ParsedVersion {
    std::array<uint64_t, 3> core{};
    std::vector<std::string> prerelease;
};

std::string_view trimmed(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

bool allDigits(std::string_view value) {
    if (value.empty()) return false;
    for (char c : value)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool identifierChars(std::string_view value) {
    if (value.empty()) return false;
    for (char c : value)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    return true;
}

std::optional<ParsedVersion> parseVersion(std::string_view input) {
    std::string_view value = trimmed(input);
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.remove_prefix(1);
    if (const auto plus = value.find('+'); plus != std::string_view::npos) {
        const std::string_view build = value.substr(plus + 1);
        value = value.substr(0, plus);
        size_t start = 0;
        while (start <= build.size()) {
            const size_t dot = build.find('.', start);
            const std::string_view part = build.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
            if (!identifierChars(part)) return std::nullopt;
            if (dot == std::string_view::npos) break;
            start = dot + 1;
        }
    }
    std::string_view prerelease;
    if (const auto dash = value.find('-'); dash != std::string_view::npos) {
        prerelease = value.substr(dash + 1);
        value = value.substr(0, dash);
        if (prerelease.empty()) return std::nullopt;
    }

    ParsedVersion out;
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t dot = value.find('.', start);
        if ((i < 2) == (dot == std::string_view::npos)) return std::nullopt;
        const std::string_view part = value.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!allDigits(part) || part.size() > 18) return std::nullopt;
        out.core[i] = std::stoull(std::string(part));
        start = dot + 1;
    }

    if (!prerelease.empty()) {
        size_t from = 0;
        while (true) {
            const size_t dot = prerelease.find('.', from);
            const std::string_view part =
                prerelease.substr(from, dot == std::string_view::npos ? std::string_view::npos : dot - from);
            if (!identifierChars(part)) return std::nullopt;
            out.prerelease.emplace_back(part);
            if (dot == std::string_view::npos) break;
            from = dot + 1;
        }
    }
    return out;
}

int comparePrereleasePart(const std::string& left, const std::string& right) {
    const bool leftNumber = allDigits(left);
    const bool rightNumber = allDigits(right);
    if (leftNumber && rightNumber) {
        if (left.size() != right.size()) return left.size() > right.size() ? 1 : -1;
        const int order = left.compare(right);
        return order == 0 ? 0 : (order > 0 ? 1 : -1);
    }
    if (leftNumber != rightNumber) return leftNumber ? -1 : 1;
    const int order = left.compare(right);
    return order == 0 ? 0 : (order > 0 ? 1 : -1);
}

std::string lower(std::string_view value) {
    std::string out(value);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isHex(std::string_view value) {
    for (char c : value)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool safeFileName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '.') return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

// https only, except plain http to this machine for local testing. The file's
// hash comes from the signed manifest either way.
bool allowedUrl(std::string_view url) {
    if (startsWith(url, "https://")) return url.size() > 8;
    return startsWith(url, "http://127.0.0.1:") || startsWith(url, "http://127.0.0.1/") ||
           startsWith(url, "http://localhost:") || startsWith(url, "http://localhost/");
}

std::optional<std::vector<unsigned char>> base64Decode(std::string_view text, size_t expected) {
    const std::string_view value = trimmed(text);
    std::vector<unsigned char> out(expected + 3);
    size_t length = 0;
    if (sodium_base642bin(out.data(), out.size(), value.data(), value.size(), nullptr, &length, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        length != expected)
        return std::nullopt;
    out.resize(length);
    return out;
}

void appendQuery(std::string& out, std::string_view name, std::string_view value, bool first) {
    out += first ? '?' : '&';
    out += name;
    out += '=';
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
}

UpdateOffer invalid(std::string error) {
    UpdateOffer offer;
    offer.error = std::move(error);
    return offer;
}

}  // namespace

std::optional<int> compareVersions(std::string_view leftText, std::string_view rightText) {
    const auto left = parseVersion(leftText);
    const auto right = parseVersion(rightText);
    if (!left || !right) return std::nullopt;
    for (int i = 0; i < 3; ++i)
        if (left->core[i] != right->core[i]) return left->core[i] > right->core[i] ? 1 : -1;
    if (left->prerelease.empty() != right->prerelease.empty()) return left->prerelease.empty() ? 1 : -1;
    const size_t count = std::max(left->prerelease.size(), right->prerelease.size());
    for (size_t i = 0; i < count; ++i) {
        if (i >= left->prerelease.size()) return -1;
        if (i >= right->prerelease.size()) return 1;
        if (const int order = comparePrereleasePart(left->prerelease[i], right->prerelease[i]); order != 0)
            return order;
    }
    return 0;
}

std::string normalizedVersion(std::string_view value) {
    std::string_view text = trimmed(value);
    if (text.size() > 1 && (text.front() == 'v' || text.front() == 'V') &&
        std::isdigit(static_cast<unsigned char>(text[1])))
        text.remove_prefix(1);
    return std::string(text);
}

std::string updateServiceUrl() {
    if (const char* overridden = std::getenv("TNR_UPDATE_ENDPOINT"); overridden && *overridden) {
        std::string url(overridden);
        while (!url.empty() && url.back() == '/') url.pop_back();
        return url;
    }
    return std::string(kUpdateServiceUrl);
}

bool updateServiceOverridden() {
    const char* overridden = std::getenv("TNR_UPDATE_ENDPOINT");
    return overridden && *overridden;
}

bool updateQuietPeriodOver(int64_t nowMs, int64_t sinceMs) {
    constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
    return sinceMs <= 0 || nowMs < sinceMs || nowMs - sinceMs >= kDayMs;
}

std::string updateCheckPath(std::string_view product, std::string_view os, std::string_view arch,
                            std::string_view format, std::string_view currentVersion) {
    std::string path = "/v1/check";
    appendQuery(path, "product", product, true);
    appendQuery(path, "os", os, false);
    appendQuery(path, "arch", arch, false);
    appendQuery(path, "format", format, false);
    appendQuery(path, "v", normalizedVersion(currentVersion), false);
    return path;
}

std::string updateInstallPath(std::string_view product, std::string_view os, std::string_view arch,
                              std::string_view format, std::string_view currentVersion, std::string_view version) {
    std::string path = updateCheckPath(product, os, arch, format, currentVersion);
    path.replace(0, std::string_view("/v1/check").size(), "/v1/install");
    appendQuery(path, "to", normalizedVersion(version), false);
    return path;
}

UpdateOffer evaluateUpdateWithKey(std::string_view responseText, std::string_view publicKeyBase64,
                                  std::string_view product, std::string_view os, std::string_view arch,
                                  std::string_view format, std::string_view currentVersion) {
    if (sodium_init() < 0) return invalid("libsodium failed to initialise");

    ServiceResponse response;
    const std::string responseBuffer(responseText);
    if (glz::read<kLenient>(response, responseBuffer)) return invalid("the update service response is not valid JSON");
    if (response.manifest.empty() || response.signature.empty())
        return invalid("the update service response has no signed manifest");

    const auto key = base64Decode(publicKeyBase64, crypto_sign_PUBLICKEYBYTES);
    if (!key) return invalid("the release public key is malformed");
    const auto signature = base64Decode(response.signature, crypto_sign_BYTES);
    if (!signature) return invalid("the manifest signature is malformed");
    if (crypto_sign_verify_detached(signature->data(),
                                    reinterpret_cast<const unsigned char*>(response.manifest.data()),
                                    response.manifest.size(), key->data()) != 0)
        return invalid("the manifest signature does not match the release key");

    Manifest manifest;
    if (glz::read<kLenient>(manifest, response.manifest)) return invalid("the signed manifest is not valid JSON");
    if (manifest.schema != 1) return invalid("the signed manifest uses an unsupported schema");

    const auto order = compareVersions(manifest.version, currentVersion);
    if (!order) return invalid("could not compare versions " + std::string(currentVersion) + " and " + manifest.version);

    UpdateOffer offer;
    offer.status = "none";
    offer.version = normalizedVersion(manifest.version);
    offer.notesUrl = manifest.notesUrl;
    offer.publishedAt = manifest.publishedAt;
    if (*order <= 0) return offer;

    for (const ManifestFile& file : manifest.files) {
        if (file.product != product || file.os != os || file.arch != arch || file.format != format) continue;
        if (!safeFileName(file.name)) return invalid("the signed manifest names an unsafe file: " + file.name);
        if (!allowedUrl(file.url)) return invalid("the signed manifest has a download URL that is not https");
        if (file.sha512.size() != 128 || !isHex(file.sha512))
            return invalid("the signed manifest has a malformed SHA-512 for " + file.name);
        if (file.size == 0) return invalid("the signed manifest has no size for " + file.name);
        offer.status = "available";
        offer.fileName = file.name;
        offer.url = file.url;
        offer.sha512 = lower(file.sha512);
        offer.size = file.size;
        return offer;
    }
    return offer;  // newer release, but nothing for this install
}

UpdateOffer evaluateUpdate(std::string_view response, std::string_view product, std::string_view os,
                           std::string_view arch, std::string_view format, std::string_view currentVersion) {
    return evaluateUpdateWithKey(response, kReleasePublicKey, product, os, arch, format, currentVersion);
}

std::string evaluateUpdateJson(std::string_view response, std::string_view product, std::string_view os,
                               std::string_view arch, std::string_view format, std::string_view currentVersion) {
    std::string out;
    (void)glz::write_json(evaluateUpdate(response, product, os, arch, format, currentVersion), out);
    return out;
}

std::string verifyUpdateFile(const std::string& path, std::string_view expectedSha512, uint64_t expectedSize) {
    if (sodium_init() < 0) return "libsodium failed to initialise";
    if (expectedSha512.size() != 128 || !isHex(expectedSha512)) return "the expected SHA-512 is malformed";

    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
    std::ifstream in(file, std::ios::binary);
    if (!in) return "could not open the downloaded file";

    crypto_hash_sha512_state state;
    crypto_hash_sha512_init(&state);
    std::vector<unsigned char> buffer(1 << 20);
    uint64_t total = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        total += static_cast<uint64_t>(got);
        if (total > expectedSize) return "the downloaded file is larger than expected";
        crypto_hash_sha512_update(&state, buffer.data(), static_cast<unsigned long long>(got));
    }
    if (in.bad()) return "could not read the downloaded file";
    if (total != expectedSize) return "the downloaded file is incomplete";

    unsigned char digest[crypto_hash_sha512_BYTES];
    crypto_hash_sha512_final(&state, digest);
    char hex[crypto_hash_sha512_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
    if (lower(expectedSha512) != hex) return "the downloaded file does not match the signed SHA-512";
    return {};
}

}  // namespace tnrp
