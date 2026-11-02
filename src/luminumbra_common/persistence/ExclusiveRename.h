#pragma once

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace Luminumbra::Persistence::Detail {

#if defined(__linux__)
// Bind the libc syscall entry under a private C++ name: its public declaration
// requires GNU feature macros on some libcs. renameat2 itself is absent from
// older glibc and musl versions even when the kernel supports the operation.
extern "C" long NativeRenameSyscall(long number, ...) __asm__("syscall");
#elif defined(__APPLE__)
// Darwin hides this declaration and RENAME_EXCL in strict POSIX profiles.
extern "C" int
renameatx_np(int from, const char* source, int to, const char* destination, unsigned flags);
#endif

// Atomically move a file or directory only if the destination is vacant.
// Unsupported platforms/kernels refuse without changing either entry; a
// check followed by ordinary rename would overwrite a racing destination.
inline int RenameWithoutReplacement(int from, const char* source, int to, const char* destination) {
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1; // Linux RENAME_NOREPLACE ABI.
    return static_cast<int>(
        NativeRenameSyscall(SYS_renameat2, from, source, to, destination, kNoReplace));
#elif defined(__APPLE__)
    constexpr unsigned kExclusive = 4; // Darwin RENAME_EXCL ABI.
    return renameatx_np(from, source, to, destination, kExclusive);
#else
    (void)from;
    (void)source;
    (void)to;
    (void)destination;
    errno = ENOTSUP;
    return -1;
#endif
}

} // namespace Luminumbra::Persistence::Detail
#endif
