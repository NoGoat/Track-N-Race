#pragma once

#include <string>

namespace tnrp {

// ISO-8601 UTC timestamp with millisecond precision, e.g. 2024-06-26T15:30:45.123Z.
// Stamped onto every parsed row as "ts" (matches the Electron/native format).
std::string isoTimestamp();

// Filesystem-safe UTC timestamp for .tnrd filenames, e.g. 2024-06-26T15-30-45-123Z.
std::string filenameTimestamp();

// Lower-cases and replaces every non-alphanumeric char with '_' for use in
// .tnrd filenames (track/session names).
std::string sanitizeName(const std::string& name);

} // namespace tnrp
