#include "tnrp/TimeUtils.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace tnrp {

static std::string stamp(const char* fmt, char msSep) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmInfo{};
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tmInfo);
    char result[80];
    std::snprintf(result, sizeof(result), "%s%c%03dZ", buf, msSep, (int)ms.count());
    return std::string(result);
}

std::string isoTimestamp() {
    return stamp("%Y-%m-%dT%H:%M:%S", '.');
}

std::string filenameTimestamp() {
    return stamp("%Y-%m-%dT%H-%M-%S", '-');
}

std::string sanitizeName(const std::string& name) {
    std::string r;
    for (unsigned char c : name)
        r += std::isalnum(c) ? (char)std::tolower(c) : '_';
    return r;
}

} // namespace tnrp
