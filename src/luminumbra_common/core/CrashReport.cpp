#include "core/CrashReport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>

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

std::atomic<int> g_report_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free, "handler state must be lock-free");

// Set by the first fatal signal. Any later entry, on this thread or another, skips the
// report and terminates the process.
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
    if (!stack) {
        stack.reset(new (std::nothrow) char[kAlternateStackBytes]);
        if (!stack) {
            return false;
        }
    }
    stack_t alternate{};
    alternate.ss_sp = stack.get();
    alternate.ss_size = kAlternateStackBytes;
    alternate.ss_flags = 0;
    return ::sigaltstack(&alternate, nullptr) == 0;
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

std::atomic<HANDLE> g_report{nullptr};
LONG g_handler_entered = 0;

// Main-module facts captured at install time. The fault path reads only these values,
// so it never calls into the loader.
std::uintptr_t g_module_base = 0;
std::uintptr_t g_module_end = 0;
char g_module_path[kMaxModulePathChars] = {};
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

RtlLookupFunctionEntryFn g_lookup_function_entry = nullptr;
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
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec || !std::filesystem::is_directory(crash_dir, ec)) {
        return false;
    }

#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
    const std::filesystem::path report = crash_dir / ("crash-" + std::to_string(pid) + ".txt");
    // The previous report handle does not allow other writers, so it must be closed before
    // the same path can be recreated. Capture stays disabled if the recreate fails.
    const HANDLE previous = g_report.exchange(nullptr);
    if (previous != nullptr && previous != INVALID_HANDLE_VALUE) {
        CloseHandle(previous);
    }
    const HANDLE file = CreateFileW(report.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!WriteHeader(file, program_name, pid)) {
        CloseHandle(file);
        return false;
    }
    CacheMainModule();
    g_report.store(file);
    ResolveNtdllEntryPoints();
    SetUnhandledExceptionFilter(&HandleUnhandledException);
    ReportPathStorage() = report;
    return true;
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const std::filesystem::path report = crash_dir / ("crash-" + std::to_string(pid) + ".txt");
    const int fd = ::open(report.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    if (!WriteHeader(fd, program_name, pid)) {
        ::close(fd);
        return false;
    }
    if (!InstallAlternateStack() || !InstallSignalHandlers()) {
        ::close(fd);
        return false;
    }
    const int previous = g_report_fd.exchange(fd);
    if (previous >= 0) {
        ::close(previous);
    }
    ReportPathStorage() = report;
    return true;
#endif
}

const std::filesystem::path& CrashReportPath() {
    return ReportPathStorage();
}

} // namespace luminumbra::crash
