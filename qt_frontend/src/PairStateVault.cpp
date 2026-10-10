#include "PairStateVault.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#elif defined(Q_OS_LINUX)
#include <dlfcn.h>
#endif

namespace PairStateVault {
namespace {

#ifdef Q_OS_WIN
const QByteArray kPrefix = QByteArrayLiteral("dpapi:v1:");

QByteArray protect(const QByteArray& json) {
    DATA_BLOB input{static_cast<DWORD>(json.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(json.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Track N Race pairing", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return json;
    const QByteArray sealed = kPrefix + QByteArray(
        reinterpret_cast<const char*>(output.pbData), static_cast<qsizetype>(output.cbData))
        .toBase64();
    LocalFree(output.pbData);
    return sealed;
}

std::optional<QByteArray> unprotect(const QByteArray& stored) {
    const QByteArray blob = QByteArray::fromBase64(stored.mid(kPrefix.size()));
    DATA_BLOB input{static_cast<DWORD>(blob.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return std::nullopt;
    QByteArray json(reinterpret_cast<const char*>(output.pbData),
                    static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return json;
}
#elif defined(Q_OS_LINUX)
// The document lives in the keyring; QSettings keeps only this marker.
const QByteArray kPrefix = QByteArrayLiteral("secret:v1");

// libsecret is loaded at run time so the AppImage still starts without it.
// These mirror its public ABI (secret/secret-schema.h).
struct SchemaAttribute {
    const char* name;
    int type;
};
struct Schema {
    const char* name;
    int flags;
    SchemaAttribute attributes[32];
    int reserved;
    void* reserved1;
    void* reserved2;
    void* reserved3;
    void* reserved4;
    void* reserved5;
    void* reserved6;
    void* reserved7;
};
using StoreFn = int (*)(const Schema*, const char*, const char*, const char*, void*, void**, ...);
using LookupFn = char* (*)(const Schema*, void*, void**, ...);
using FreeFn = void (*)(char*);

struct Secret {
    StoreFn store{};
    LookupFn lookup{};
    FreeFn free{};
    Schema schema{};

    static const Secret* get() {
        static const Secret secret = [] {
            Secret loaded;
            void* library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!library) return loaded;
            loaded.store = reinterpret_cast<StoreFn>(dlsym(library, "secret_password_store_sync"));
            loaded.lookup = reinterpret_cast<LookupFn>(dlsym(library, "secret_password_lookup_sync"));
            loaded.free = reinterpret_cast<FreeFn>(dlsym(library, "secret_password_free"));
            loaded.schema.name = "com.tracknrace.Pairing";
            loaded.schema.attributes[0] = {"document", 0};  // SECRET_SCHEMA_ATTRIBUTE_STRING
            return loaded;
        }();
        return secret.store && secret.lookup && secret.free ? &secret : nullptr;
    }
};

QByteArray protect(const QByteArray& json) {
    const Secret* secret = Secret::get();
    if (!secret) return json;
    const QByteArray text = json + '\0';
    if (!secret->store(&secret->schema, nullptr, "Track N Race paired displays",
                       text.constData(), nullptr, nullptr, "document", "engine-state",
                       nullptr)) return json;
    return kPrefix;
}

std::optional<QByteArray> unprotect(const QByteArray&) {
    const Secret* secret = Secret::get();
    if (!secret) return std::nullopt;
    char* value = secret->lookup(&secret->schema, nullptr, nullptr,
                                 "document", "engine-state", nullptr);
    if (!value) return std::nullopt;
    QByteArray json(value);
    secret->free(value);
    return json;
}
#else
const QByteArray kPrefix;
QByteArray protect(const QByteArray& json) { return json; }
std::optional<QByteArray> unprotect(const QByteArray& stored) { return stored; }
#endif

} // namespace

QByteArray seal(const QByteArray& json) {
    if (json.isEmpty()) return json;
    return protect(json);
}

std::optional<QByteArray> open(const QByteArray& stored) {
    if (kPrefix.isEmpty() || !stored.startsWith(kPrefix)) return stored;
    return unprotect(stored);
}

} // namespace PairStateVault
