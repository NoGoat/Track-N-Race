#include "IdleMemory.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <malloc/malloc.h>
#elif defined(Q_OS_LINUX) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace tnr::idlememory {

void release() {
#ifdef Q_OS_WIN
    // -1, -1 asks Windows to remove as many pages as possible from the working
    // set. They stay committed and fault back in from memory when touched.
    // (HeapCompact was tried too: it left the heaps' committed size unchanged.)
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
#elif defined(Q_OS_MACOS)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(Q_OS_LINUX) && defined(__GLIBC__)
    malloc_trim(0);
#endif
}

}
