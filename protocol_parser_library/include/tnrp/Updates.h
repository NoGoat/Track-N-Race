#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Self-update support shared by every frontend.
//
// The update service (update_worker/) returns the newest release's signed
// manifest: {"manifest": "<manifest.json text>", "signature": "<base64>"}.
// publish-release.mjs writes and signs that manifest with the release key; the
// matching public key is built in here. evaluateUpdate() checks the signature,
// then picks the file for this install. verifyUpdateFile() checks a downloaded
// file against the size and SHA-512 the signed manifest gave for it, so a file
// is only ever run when it is exactly what the release key signed.

namespace tnrp {

// Compares two versions ("1.6.2", "v1.7.0-beta.1", build metadata ignored) by
// semantic-versioning rules: 1 when left is newer, -1 when older, 0 when equal.
// nullopt when either one is not a version.
std::optional<int> compareVersions(std::string_view left, std::string_view right);

// "v1.6.2" -> "1.6.2"; trims whitespace.
std::string normalizedVersion(std::string_view value);

// The update service base URL. TNR_UPDATE_ENDPOINT overrides it (testing).
std::string updateServiceUrl();
// True when TNR_UPDATE_ENDPOINT is set; frontends then skip the daily limit.
bool updateServiceOverridden();

// The query path for one install, appended to updateServiceUrl():
// "/v1/check?product=…&os=…&arch=…&format=…&v=…".
std::string updateCheckPath(std::string_view product, std::string_view os, std::string_view arch,
                            std::string_view format, std::string_view currentVersion);

struct UpdateOffer {
    // "available": a newer signed release has a file for this install.
    // "none": up to date, or the newest release has no file for this install.
    // "invalid": the response failed the signature or could not be read.
    std::string status = "invalid";
    std::string error;
    std::string version;
    std::string notesUrl;
    std::string publishedAt;
    std::string fileName;  // safe as a file name: [A-Za-z0-9._-] only
    std::string url;
    std::string sha512;    // lower-case hex
    uint64_t size = 0;
};

// Checks a service response against the built-in release key and picks the
// file whose product/os/arch/format match. `response` is the service's JSON
// body ({"manifest","signature"}).
UpdateOffer evaluateUpdate(std::string_view response, std::string_view product, std::string_view os,
                           std::string_view arch, std::string_view format, std::string_view currentVersion);
// evaluateUpdate() as JSON: {"status","error","version","notesUrl",
// "publishedAt","fileName","url","sha512","size"}.
std::string evaluateUpdateJson(std::string_view response, std::string_view product, std::string_view os,
                               std::string_view arch, std::string_view format, std::string_view currentVersion);

// Same as evaluateUpdate() with an explicit public key (32 bytes, base64).
// For tests; frontends use the built-in key.
UpdateOffer evaluateUpdateWithKey(std::string_view response, std::string_view publicKeyBase64,
                                  std::string_view product, std::string_view os, std::string_view arch,
                                  std::string_view format, std::string_view currentVersion);

// "" when the file at `path` (UTF-8) has exactly `expectedSize` bytes and the
// SHA-512 `expectedSha512` (hex); otherwise what is wrong. Reads the whole file,
// so call it off the UI thread.
std::string verifyUpdateFile(const std::string& path, std::string_view expectedSha512, uint64_t expectedSize);

}  // namespace tnrp
