#include "tnrp/Updates.h"

#include <sodium.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #condition);                                           \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

std::string base64(const unsigned char* data, size_t size) {
    std::string out(sodium_base64_encoded_len(size, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), data, size, sodium_base64_VARIANT_ORIGINAL);
    out.resize(out.size() - 1);  // drop the terminator
    return out;
}

std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

struct Keys {
    unsigned char publicKey[crypto_sign_PUBLICKEYBYTES];
    unsigned char secretKey[crypto_sign_SECRETKEYBYTES];
    std::string publicBase64() const { return base64(publicKey, sizeof publicKey); }
};

std::string signedResponse(const Keys& keys, const std::string& manifest) {
    unsigned char signature[crypto_sign_BYTES];
    crypto_sign_detached(signature, nullptr, reinterpret_cast<const unsigned char*>(manifest.data()),
                         manifest.size(), keys.secretKey);
    return "{\"manifest\":" + jsonString(manifest) + ",\"signature\":" +
           jsonString(base64(signature, sizeof signature)) + ",\"extra\":1}";
}

const std::string kSha(128, 'a');

std::string manifestFor(const std::string& version, const std::string& url = "https://example.com/a.exe",
                        const std::string& name = "Track-N-Race-Qt-Windows.exe",
                        const std::string& sha = kSha) {
    return "{\n  \"schema\": 1,\n  \"version\": \"" + version +
           "\",\n  \"publishedAt\": \"2026-11-14T00:00:00Z\",\n  \"notesUrl\": \"https://example.com/notes\",\n"
           "  \"files\": [\n"
           "    {\"product\":\"electron\",\"os\":\"windows\",\"arch\":\"x64\",\"format\":\"installer\","
           "\"name\":\"Track-N-Race-Electron-Windows.exe\",\"url\":\"https://example.com/e.exe\",\"size\":5,"
           "\"sha512\":\"" + kSha + "\"},\n"
           "    {\"product\":\"qt\",\"os\":\"windows\",\"arch\":\"x64\",\"format\":\"installer\",\"name\":\"" + name +
           "\",\"url\":\"" + url + "\",\"size\":42,\"sha512\":\"" + sha + "\",\"future\":true}\n"
           "  ]\n}\n";
}

void versions() {
    using tnrp::compareVersions;
    CHECK(compareVersions("1.6.2", "1.6.2") == 0);
    CHECK(compareVersions("1.6.3", "1.6.2") == 1);
    CHECK(compareVersions("1.6.2", "1.10.0") == -1);
    CHECK(compareVersions("v2.0.0", "1.99.99") == 1);
    CHECK(compareVersions(" 1.6.2 ", "1.6.2+build.7") == 0);
    CHECK(compareVersions("1.7.0-beta.1", "1.7.0") == -1);
    CHECK(compareVersions("1.7.0-beta.2", "1.7.0-beta.10") == -1);
    CHECK(compareVersions("1.7.0-beta", "1.7.0-1") == 1);
    CHECK(compareVersions("1.7.0-beta", "1.7.0-beta.1") == -1);
    CHECK(compareVersions("1.6", "1.6.0") == std::nullopt);
    CHECK(compareVersions("1.6.1[o", "1.6.1") == std::nullopt);
    CHECK(compareVersions("1.6.0.1", "1.6.0") == std::nullopt);
    CHECK(compareVersions("1.6.0-", "1.6.0") == std::nullopt);
    CHECK(compareVersions("", "1.6.0") == std::nullopt);
    CHECK(tnrp::normalizedVersion(" v1.6.2\n") == "1.6.2");
    CHECK(tnrp::normalizedVersion("version") == "version");
}

void checkPath() {
    CHECK(tnrp::updateCheckPath("qt", "windows", "x64", "installer", "v1.6.2") ==
          "/v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.2");
    CHECK(tnrp::updateCheckPath("q t", "w&x", "x64", "installer", "1.0.0+a/b") ==
          "/v1/check?product=q%20t&os=w%26x&arch=x64&format=installer&v=1.0.0%2Ba%2Fb");
    CHECK(tnrp::updateInstallPath("android", "android", "universal", "apk", "v1.6.2", " v1.7.0") ==
          "/v1/install?product=android&os=android&arch=universal&format=apk&v=1.6.2&to=1.7.0");
}

void offers(const Keys& keys, const Keys& otherKeys) {
    const std::string key = keys.publicBase64();
    const auto evaluate = [&](const std::string& response, const std::string& current,
                              const std::string& product = "qt", const std::string& format = "installer") {
        return tnrp::evaluateUpdateWithKey(response, key, product, "windows", "x64", format, current);
    };

    // A newer signed release with a file for this install.
    auto offer = evaluate(signedResponse(keys, manifestFor("1.7.0")), "1.6.2");
    CHECK(offer.status == "available");
    CHECK(offer.error.empty());
    CHECK(offer.version == "1.7.0");
    CHECK(offer.fileName == "Track-N-Race-Qt-Windows.exe");
    CHECK(offer.url == "https://example.com/a.exe");
    CHECK(offer.size == 42);
    CHECK(offer.sha512 == kSha);
    CHECK(offer.notesUrl == "https://example.com/notes");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0")), "1.6.2", "electron").fileName ==
          "Track-N-Race-Electron-Windows.exe");

    // Up to date, older, or nothing for this install.
    CHECK(evaluate(signedResponse(keys, manifestFor("1.6.2")), "1.6.2").status == "none");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.6.1")), "1.6.2").status == "none");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0")), "1.6.2", "qt", "zip").status == "none");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0")), "1.6.2", "android").status == "none");

    // Anything not signed by the release key is rejected.
    std::string tampered = signedResponse(keys, manifestFor("1.7.0"));
    tampered.replace(tampered.find("a.exe"), 5, "b.exe");
    CHECK(evaluate(tampered, "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(otherKeys, manifestFor("1.7.0")), "1.6.2").status == "invalid");
    CHECK(evaluate("{\"manifest\":\"{}\",\"signature\":\"bm90IGEgc2lnbmF0dXJl\"}", "1.6.2").status == "invalid");
    CHECK(evaluate("not json", "1.6.2").status == "invalid");
    CHECK(evaluate("{}", "1.6.2").status == "invalid");
    CHECK(tnrp::evaluateUpdateWithKey(signedResponse(keys, manifestFor("1.7.0")), "short", "qt", "windows", "x64",
                                      "installer", "1.6.2").status == "invalid");
    // The built-in key is not the test key.
    CHECK(tnrp::evaluateUpdate(signedResponse(keys, manifestFor("1.7.0")), "qt", "windows", "x64", "installer",
                               "1.6.2").status == "invalid");

    // Signed but unusable entries.
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "http://example.com/a.exe")), "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "file:///C:/a.exe")), "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "http://127.0.0.1:8788/a.exe")), "1.6.2").status ==
          "available");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "http://localhost.evil.com/a.exe")), "1.6.2").status ==
          "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "https://example.com/a.exe", "..\\evil.exe")),
                   "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "https://example.com/a.exe", "a/b.exe")),
                   "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "https://example.com/a.exe",
                                                    "Track-N-Race-Qt-Windows.exe", "abc")),
                   "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "https://example.com/a.exe",
                                                    "Track-N-Race-Qt-Windows.exe", std::string(128, 'g'))),
                   "1.6.2").status == "invalid");
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0", "https://example.com/a.exe",
                                                    "Track-N-Race-Qt-Windows.exe", std::string(128, 'A'))),
                   "1.6.2").sha512 == kSha);
    // A version that cannot be compared.
    CHECK(evaluate(signedResponse(keys, manifestFor("1.7.0")), "1.6.1[o").status == "invalid");
    std::string schema2 = manifestFor("1.7.0");
    schema2.replace(schema2.find("\"schema\": 1"), 11, "\"schema\": 2");
    CHECK(evaluate(signedResponse(keys, schema2), "1.6.2").status == "invalid");

    // The JSON form carries the same fields.
    const std::string json = tnrp::evaluateUpdateJson("{}", "qt", "windows", "x64", "installer", "1.6.2");
    CHECK(json.find("\"status\":\"invalid\"") != std::string::npos);
    CHECK(json.find("\"sha512\"") != std::string::npos);
}

void files() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "tnrp_updates_test_\xC3\xA9.bin";
    std::vector<unsigned char> data(3 * 1024 * 1024 + 17);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 31 + 7);
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    unsigned char digest[crypto_hash_sha512_BYTES];
    crypto_hash_sha512(digest, data.data(), data.size());
    char hex[crypto_hash_sha512_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
    std::string upper(hex);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const std::u8string u8 = path.u8string();
    const std::string utf8(u8.begin(), u8.end());
    CHECK(tnrp::verifyUpdateFile(utf8, hex, data.size()).empty());
    CHECK(tnrp::verifyUpdateFile(utf8, upper, data.size()).empty());
    CHECK(!tnrp::verifyUpdateFile(utf8, hex, data.size() + 1).empty());
    CHECK(!tnrp::verifyUpdateFile(utf8, hex, data.size() - 1).empty());
    CHECK(!tnrp::verifyUpdateFile(utf8, std::string(128, '0'), data.size()).empty());
    CHECK(!tnrp::verifyUpdateFile(utf8, "abc", data.size()).empty());
    CHECK(!tnrp::verifyUpdateFile(utf8 + ".missing", hex, data.size()).empty());
    std::filesystem::remove(path);
}

void quietPeriod() {
    constexpr int64_t day = 24LL * 60 * 60 * 1000;
    constexpr int64_t now = 1'800'000'000'000;
    CHECK(tnrp::updateQuietPeriodOver(now, 0));
    CHECK(!tnrp::updateQuietPeriodOver(now, now));
    CHECK(!tnrp::updateQuietPeriodOver(now, now - day + 1));
    CHECK(tnrp::updateQuietPeriodOver(now, now - day));
    CHECK(tnrp::updateQuietPeriodOver(now, now + 1000));  // clock moved back
}

}  // namespace

int main() {
    if (sodium_init() < 0) return 1;
    Keys keys, otherKeys;
    crypto_sign_keypair(keys.publicKey, keys.secretKey);
    crypto_sign_keypair(otherKeys.publicKey, otherKeys.secretKey);

    versions();
    checkPath();
    offers(keys, otherKeys);
    files();
    quietPeriod();

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::puts("updates: all checks passed");
    return failures ? 1 : 0;
}
