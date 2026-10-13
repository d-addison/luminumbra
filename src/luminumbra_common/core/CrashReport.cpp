#include "core/CrashReport.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/ucontext.h>
#define LUMINUMBRA_CRASH_HAS_CONTEXT_REGS 1
#endif
#endif

namespace luminumbra::crash {

namespace {

constexpr std::string_view kReportHeader = "luminumbra-crash-report v1";
constexpr std::size_t kSinkCapacity = 1024;

#if defined(_WIN32)
using Target = HANDLE;
#else
using Target = int;
#endif

// Only installers access these slots, under install_mutex. Handlers read only the
// published atomic target. Never close a published target: a handler may have loaded
// it already. Reuse unchanged files and bound all other retention to eight targets.
constexpr std::size_t kMaxReportTargets = 8;
struct ReportTarget {
    Target target{};
    std::filesystem::path directory;
    std::filesystem::path report;
};

bool IsSameReportFile(const ReportTarget& slot) {
#if defined(_WIN32)
    const HANDLE probe = CreateFileW(slot.report.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION original{};
    BY_HANDLE_FILE_INFORMATION existing{};
    const bool same = GetFileInformationByHandle(slot.target, &original) != 0 &&
                      GetFileInformationByHandle(probe, &existing) != 0 &&
                      original.dwVolumeSerialNumber == existing.dwVolumeSerialNumber &&
                      original.nFileIndexHigh == existing.nFileIndexHigh &&
                      original.nFileIndexLow == existing.nFileIndexLow;
    CloseHandle(probe);
    return same;
#else
    struct stat original {};
    struct stat existing {};
    return ::fstat(slot.target, &original) == 0 && ::stat(slot.report.c_str(), &existing) == 0 &&
           original.st_dev == existing.st_dev && original.st_ino == existing.st_ino;
#endif
}

Target OpenFreshReport(const std::filesystem::path& directory,
                       std::uint64_t pid,
                       std::filesystem::path& report) {
    const std::string stem = "crash-" + std::to_string(pid);
    // Exclusive creation preserves pre-existing evidence and cannot open a hard link
    // to a live old target. Keep the usual name when free; collisions get a suffix.
    for (unsigned int suffix = 0; suffix < 1024; ++suffix) {
        report = directory / (stem + (suffix == 0 ? "" : "-" + std::to_string(suffix)) + ".txt");
#if defined(_WIN32)
        const HANDLE file = CreateFileW(report.c_str(), GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || (GetLastError() != ERROR_FILE_EXISTS &&
                                             GetLastError() != ERROR_ALREADY_EXISTS)) {
            return file;
        }
#else
        const int fd = ::open(report.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
#endif
    }
    std::fputs("Crash capture: report filename collision limit reached\n", stderr);
#if defined(_WIN32)
    return INVALID_HANDLE_VALUE;
#else
    return -1;
#endif
}

// Fixed-size buffer that formats text and integers without allocating. Flush()
// returns false once any write has failed; later output is dropped.
class CrashSink {
public:
    explicit CrashSink(Target target)
        : target_(target) {}

    void Text(std::string_view text) {
        Append(text.data(), text.size());
    }

    void Hex(std::uint64_t value) {
        char digits[16];
        std::size_t count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 0xFu];
            value >>= 4;
        } while (value != 0);
        Text("0x");
        while (count > 0) {
            const char digit = digits[--count];
            Append(&digit, 1);
        }
    }

    void Dec(std::uint64_t value) {
        char digits[20];
        std::size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        } while (value != 0);
        while (count > 0) {
            const char digit = digits[--count];
            Append(&digit, 1);
        }
    }

    void Signed(std::int64_t value) {
        if (value < 0) {
            Text("-");
            Dec(0u - static_cast<std::uint64_t>(value));
        } else {
            Dec(static_cast<std::uint64_t>(value));
        }
    }

    bool Flush() {
        if (size_ > 0 && !failed_) {
            failed_ = !WriteBytes(buffer_, size_);
        }
        size_ = 0;
        return !failed_;
    }

private:
    void Append(const char* data, std::size_t size) {
        if (size > kSinkCapacity - size_) {
            Flush();
        }
        if (size > kSinkCapacity) {
            if (!failed_) {
                failed_ = !WriteBytes(data, size);
            }
            return;
        }
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[size_++] = data[i];
        }
    }

    bool WriteBytes(const char* data, std::size_t size) {
#if defined(_WIN32)
        while (size > 0) {
            const DWORD chunk = size > 0x10000u ? 0x10000u : static_cast<DWORD>(size);
            DWORD written = 0;
            if (!WriteFile(target_, data, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            data += written;
            size -= written;
        }
        return true;
#else
        while (size > 0) {
            const ssize_t written = ::write(target_, data, size);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (written == 0) {
                return false;
            }
            data += written;
            size -= static_cast<std::size_t>(written);
        }
        return true;
#endif
    }

    Target target_;
    char buffer_[kSinkCapacity] = {};
    std::size_t size_ = 0;
    bool failed_ = false;
};

std::filesystem::path& ReportPathStorage() {
    static std::filesystem::path path;
    return path;
}

bool WriteHeader(Target target, std::string_view program_name, std::uint64_t pid) {
    CrashSink sink(target);
    sink.Text(kReportHeader);
    sink.Text("\nprogram ");
    sink.Text(program_name);
    sink.Text("\npid ");
    sink.Dec(pid);
    sink.Text("\n");
    return sink.Flush();
}

#if !defined(_WIN32)

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> g_report_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free, "handler state must be lock-free");

// Set by the first fatal signal. Any later entry, on this thread or another, skips the
// report and terminates the process.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic_flag g_handler_entered{};

const char* SignalName(int signo) {
    switch (signo) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGBUS:
            return "SIGBUS";
        default:
            return "UNKNOWN";
    }
}

#if defined(LUMINUMBRA_CRASH_HAS_CONTEXT_REGS)
void ContextPcAndSp(const ucontext_t& context, std::uint64_t& pc, std::uint64_t& sp) {
#if defined(__x86_64__)
    pc = static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_RIP]);
    sp = static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_RSP]);
#else
    pc = static_cast<std::uint64_t>(context.uc_mcontext.pc);
    sp = static_cast<std::uint64_t>(context.uc_mcontext.sp);
#endif
}
#endif

void WriteFaultRecord(int fd,
                      int signo,
                      const siginfo_t* info,
                      [[maybe_unused]] const void* context) {
    CrashSink sink(fd);
    sink.Text("signal ");
    sink.Dec(static_cast<std::uint64_t>(signo));
    sink.Text(" ");
    sink.Text(SignalName(signo));
    sink.Text("\nsi_code ");
    sink.Signed(info != nullptr ? static_cast<std::int64_t>(info->si_code) : 0);
    const auto fault_address =
        info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0u;
    sink.Text("\nfault_address ");
    sink.Hex(static_cast<std::uint64_t>(fault_address));
#if defined(LUMINUMBRA_CRASH_HAS_CONTEXT_REGS)
    if (context != nullptr) {
        std::uint64_t pc = 0;
        std::uint64_t sp = 0;
        ContextPcAndSp(*static_cast<const ucontext_t*>(context), pc, sp);
        sink.Text("\npc ");
        sink.Hex(pc);
        sink.Text("\nsp ");
        sink.Hex(sp);
    }
#endif
    sink.Text("\n");
    sink.Flush();
}

// Restores the default action for signo, unblocks it, and raises it again, so the
// process terminates with the original signal. signal(), sigprocmask() and raise()
// are async-signal-safe.
void ReraiseWithDefaultAction(int signo) {
    std::signal(signo, SIG_DFL);
    sigset_t only{};
    sigemptyset(&only);
    sigaddset(&only, signo);
    sigprocmask(SIG_UNBLOCK, &only, nullptr);
    std::raise(signo);
}

void HandleFatalSignal(int signo, siginfo_t* info, void* context) {
    if (g_handler_entered.test_and_set()) {
        ReraiseWithDefaultAction(signo);
        return;
    }
    const int fd = g_report_fd.load();
    if (fd >= 0) {
        WriteFaultRecord(fd, signo, info, context);
    }
    ReraiseWithDefaultAction(signo);
}

bool InstallAlternateStack() {
    constexpr std::size_t kAlternateStackBytes = 64 * 1024;
    static std::unique_ptr<char[]> stack;
    static thread_local bool installed_on_this_thread = false;
    // The server installs once on its entry thread. Keep that ownership even if the
    // thread exits: a different thread must never share its alternate-stack storage.
    // InstallCrashCapture serializes access to stack; the signal handler never uses it.
    if (stack && !installed_on_this_thread) {
        std::fputs("Crash capture: cross-thread reinstallation is unsupported; "
                   "install on the original entry thread\n",
                   stderr);
        return false;
    }
    std::unique_ptr<char[]> candidate;
    if (!stack) {
        candidate.reset(new (std::nothrow) char[kAlternateStackBytes]);
        if (!candidate) {
            return false;
        }
    }
    stack_t alternate{};
    alternate.ss_sp = stack ? stack.get() : candidate.get();
    alternate.ss_size = kAlternateStackBytes;
    alternate.ss_flags = 0;
    if (::sigaltstack(&alternate, nullptr) != 0) {
        return false;
    }
    if (candidate) {
        stack = std::move(candidate);
    }
    installed_on_this_thread = true;
    return true;
}

bool InstallSignalHandlers() {
    constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
    struct sigaction action {};
    action.sa_sigaction = &HandleFatalSignal;
    sigemptyset(&action.sa_mask);
    for (const int signo : kFatalSignals) {
        sigaddset(&action.sa_mask, signo);
    }
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    bool ok = true;
    for (const int signo : kFatalSignals) {
        ok = (::sigaction(signo, &action, nullptr) == 0) && ok;
    }
    return ok;
}

#else

constexpr DWORD kMaxModulePathChars = 32767;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<HANDLE> g_report{nullptr};
static_assert(std::atomic<HANDLE>::is_always_lock_free, "handler state must be lock-free");
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
LONG g_handler_entered = 0;

// Main-module facts captured at install time. The fault path reads only these values,
// so it never calls into the loader.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::uintptr_t g_module_base = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::uintptr_t g_module_end = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
char g_module_path[kMaxModulePathChars] = {};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DWORD g_module_path_length = 0;

void CacheMainModule() {
    g_module_base = 0;
    g_module_end = 0;
    g_module_path_length = 0;
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    g_module_base = reinterpret_cast<std::uintptr_t>(module);
    g_module_end = g_module_base + nt->OptionalHeader.SizeOfImage;
    const DWORD length = GetModuleFileNameA(module, g_module_path, kMaxModulePathChars);
    if (length > 0 && length < kMaxModulePathChars) {
        g_module_path_length = length;
    }
}

#if defined(_WIN64) && defined(_AMD64_)

// GetProcAddress returns FARPROC, which is not the function type we need. Copying the
// bytes into a typed pointer avoids a function-type-punning cast that -Wcast-function-type
// rejects.
template<typename Fn>
Fn ResolveNtdllEntry(HMODULE module, const char* name) {
    static_assert(sizeof(Fn) == sizeof(FARPROC), "function pointer size mismatch");
    Fn result = nullptr;
    const FARPROC proc = GetProcAddress(module, name);
    if (proc != nullptr) {
        std::memcpy(&result, &proc, sizeof(result));
    }
    return result;
}

using RtlLookupFunctionEntryFn = decltype(&RtlLookupFunctionEntry);
using RtlVirtualUnwindFn = decltype(&RtlVirtualUnwind);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RtlLookupFunctionEntryFn g_lookup_function_entry = nullptr;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RtlVirtualUnwindFn g_virtual_unwind = nullptr;

void ResolveNtdllEntryPoints() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return;
    }
    g_lookup_function_entry =
        ResolveNtdllEntry<RtlLookupFunctionEntryFn>(ntdll, "RtlLookupFunctionEntry");
    g_virtual_unwind = ResolveNtdllEntry<RtlVirtualUnwindFn>(ntdll, "RtlVirtualUnwind");
}

void WriteFrames(CrashSink& sink, const CONTEXT& source) {
    constexpr int kMaxFrames = 64;
    sink.Text("frames\n");
    if (g_lookup_function_entry == nullptr || g_virtual_unwind == nullptr) {
        sink.Text("unavailable\n");
        return;
    }
    // Unwind a copy so the EXCEPTION_POINTERS the system passed in stays intact.
    CONTEXT context = source;
    for (int index = 0; index < kMaxFrames && context.Rip != 0; ++index) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = g_lookup_function_entry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // Leaf frame without unwind data: stop rather than trust the stack contents.
            break;
        }
        sink.Text("frame ");
        sink.Dec(static_cast<std::uint64_t>(index));
        sink.Text(" pc ");
        sink.Hex(static_cast<std::uint64_t>(context.Rip));
        sink.Text(" rva ");
        sink.Hex(static_cast<std::uint64_t>(context.Rip - image_base));
        sink.Text("\n");
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        g_virtual_unwind(UNW_FLAG_NHANDLER,
                         image_base,
                         context.Rip,
                         function,
                         &context,
                         &handler_data,
                         &establisher_frame,
                         nullptr);
    }
}

#else

void ResolveNtdllEntryPoints() {}

void WriteFrames(CrashSink& sink, const CONTEXT&) {
    sink.Text("frames\nunavailable\n");
}

#endif

void WriteFaultReport(HANDLE target, const EXCEPTION_POINTERS& pointers) {
    CrashSink sink(target);
    const EXCEPTION_RECORD* record = pointers.ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    sink.Text("exception_code ");
    sink.Hex(static_cast<std::uint64_t>(record->ExceptionCode));
    sink.Text("\nexception_address ");
    sink.Hex(static_cast<std::uint64_t>(address));
    sink.Text("\nthread_id ");
    sink.Dec(static_cast<std::uint64_t>(GetCurrentThreadId()));

    if (g_module_base != 0 && address >= g_module_base && address < g_module_end) {
        const std::string_view name = g_module_path_length > 0
                                          ? std::string_view(g_module_path, g_module_path_length)
                                          : std::string_view("unknown");
        sink.Text("\nmodule ");
        sink.Text(name);
        sink.Text(" base ");
        sink.Hex(static_cast<std::uint64_t>(g_module_base));
        sink.Text(" rva ");
        sink.Hex(static_cast<std::uint64_t>(address - g_module_base));
    } else {
        sink.Text("\nmodule unknown");
    }
    sink.Text("\n");
    // The basic record is on disk before any unwinding starts.
    sink.Flush();

    if (pointers.ContextRecord != nullptr) {
        WriteFrames(sink, *pointers.ContextRecord);
    }
    sink.Flush();
}

LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* pointers) {
    if (InterlockedCompareExchange(&g_handler_entered, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const HANDLE target = g_report.load();
    if (target != nullptr && target != INVALID_HANDLE_VALUE && pointers != nullptr &&
        pointers->ExceptionRecord != nullptr) {
        WriteFaultReport(target, *pointers);
    }
    // Let WER and LocalDumps see the exception so a full dump can still be produced.
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

} // namespace

bool InstallCrashCapture(const std::filesystem::path& crash_dir, std::string_view program_name) {
    static std::mutex install_mutex;
    static std::array<ReportTarget, kMaxReportTargets> targets;
    static std::size_t target_count = 0;
    const std::lock_guard<std::mutex> install_lock(install_mutex);
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec || !std::filesystem::is_directory(crash_dir, ec)) {
        return false;
    }
    const std::filesystem::path directory = std::filesystem::canonical(crash_dir, ec);
    if (ec) {
        return false;
    }
#if !defined(_WIN32)
    if (!InstallAlternateStack()) {
        return false;
    }
#endif
    for (std::size_t index = 0; index < target_count; ++index) {
        const ReportTarget& slot = targets[index];
        if (slot.directory == directory && IsSameReportFile(slot)) {
            // Preserve the header and offset as well as the resource. A handler can
            // still be writing this file, including when returning to an old target.
            std::filesystem::path report = crash_dir / slot.report.filename();
#if defined(_WIN32)
            g_report.store(slot.target);
            SetUnhandledExceptionFilter(&HandleUnhandledException);
#else
            if (!InstallSignalHandlers()) {
                return false;
            }
            g_report_fd.store(slot.target);
#endif
            ReportPathStorage() = std::move(report);
            return true;
        }
    }
    if (target_count == targets.size()) {
        std::fputs("Crash capture: eight retained report targets exhausted; "
                   "previous target remains installed\n", stderr);
        return false;
    }

#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
    ReportTarget candidate;
    candidate.directory = directory;
    candidate.target = OpenFreshReport(directory, pid, candidate.report);
#if defined(_WIN32)
    if (candidate.target == INVALID_HANDLE_VALUE) {
        return false;
    }
#else
    if (candidate.target < 0) {
        return false;
    }
#endif
    if (!WriteHeader(candidate.target, program_name, pid)) {
#if defined(_WIN32)
        CloseHandle(candidate.target);
#else
        ::close(candidate.target);
#endif
        return false;
    }
    std::filesystem::path report = crash_dir / candidate.report.filename();
#if defined(_WIN32)
    // These facts are immutable after first publication, even during reinstall.
    static std::once_flag module_initialized;
    std::call_once(module_initialized, [] {
        CacheMainModule();
        ResolveNtdllEntryPoints();
    });
    g_report.store(candidate.target);
    SetUnhandledExceptionFilter(&HandleUnhandledException);
#else
    if (!InstallSignalHandlers()) {
        ::close(candidate.target);
        return false;
    }
    g_report_fd.store(candidate.target);
#endif
    targets[target_count++] = std::move(candidate);
    ReportPathStorage() = std::move(report);
    return true;
}

const std::filesystem::path& CrashReportPath() {
    return ReportPathStorage();
}

} // namespace luminumbra::crash
