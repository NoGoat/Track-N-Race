#pragma once

namespace tnr::idlememory {

// Gives memory the process holds but is not using back to the system while the
// window is minimized or hidden. No data is discarded:
// - Windows: the working set is trimmed, so pages the hidden window does not
//   touch leave physical memory until they are used again (Task Manager's
//   Memory column drops; committed memory does not);
// - Linux and macOS: the allocator returns free pages (glibc malloc_trim,
//   malloc_zone_pressure_relief).
// Call on the GUI thread; it takes a few milliseconds.
void release();

}
