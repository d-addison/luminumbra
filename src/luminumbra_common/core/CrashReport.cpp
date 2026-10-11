#include "core/CrashReport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
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
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace luminumbra::crash {

namespace {

constexpr std::string_view kReportHeader = "luminumbra-crash-report v1";
constexpr std::size_t kSinkCapacity = 1024;
[[maybe_unused]] constexpr int kMaxFrames = 64;

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

void HandleFatalSignal(int signo, siginfo_t* info, void*) {
    const int fd = g_report_fd.load();
    if (fd < 0) {
        return;
    }
    {
        CrashSink sink(fd);
        sink.Text("signal ");
        sink.Dec(static_cast<std::uint64_t>(signo));
        sink.Text(" ");
        sink.Text(SignalName(signo));
        sink.Text("\nfault_address ");
        sink.Hex(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr)));
        sink.Text("\nframes\n");
        sink.Flush();
    }
    void* frames[kMaxFrames];
    const int count = backtrace(frames, kMaxFrames);
    backtrace_symbols_fd(frames, count, fd);
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
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    bool ok = true;
    for (const int signo : kFatalSignals) {
        ok = (::sigaction(signo, &action, nullptr) == 0) && ok;
    }
    return ok;
}

#else

using RtlLookupFunctionEntryFn = PRUNTIME_FUNCTION(NTAPI*)(DWORD64,
                                                           PDWORD64,
                                                           PUNWIND_HISTORY_TABLE);
using RtlVirtualUnwindFn = PEXCEPTION_ROUTINE(NTAPI*)(DWORD,
                                                      DWORD64,
                                                      DWORD64,
                                                      PRUNTIME_FUNCTION,
                                                      PCONTEXT,
                                                      PVOID*,
                                                      PDWORD64,
                                                      PKNONVOLATILE_CONTEXT_POINTERS);

std::atomic<HANDLE> g_report{nullptr};
LONG g_handler_entered = 0;
RtlLookupFunctionEntryFn g_lookup_function_entry = nullptr;
RtlVirtualUnwindFn g_virtual_unwind = nullptr;

void ResolveNtdllEntryPoints() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return;
    }
    g_lookup_function_entry =
        reinterpret_cast<RtlLookupFunctionEntryFn>(GetProcAddress(ntdll, "RtlLookupFunctionEntry"));
    g_virtual_unwind =
        reinterpret_cast<RtlVirtualUnwindFn>(GetProcAddress(ntdll, "RtlVirtualUnwind"));
}

#if defined(_WIN64) && defined(_AMD64_)
void WriteFrames(CrashSink& sink, const CONTEXT& source) {
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

    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(record->ExceptionAddress),
                           &module) &&
        module != nullptr) {
        char name[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(module, name, MAX_PATH);
        const std::string_view name_text =
            length > 0 ? std::string_view(name, length) : std::string_view("unknown");
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        sink.Text("\nmodule ");
        sink.Text(name_text);
        sink.Text(" base ");
        sink.Hex(static_cast<std::uint64_t>(base));
        sink.Text(" rva ");
        sink.Hex(static_cast<std::uint64_t>(address - base));
    } else {
        sink.Text("\nmodule unknown");
    }
    sink.Text("\nthread_id ");
    sink.Dec(static_cast<std::uint64_t>(GetCurrentThreadId()));
    sink.Text("\n");
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
    const HANDLE file = CreateFileW(report.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
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
    const HANDLE previous = g_report.exchange(file);
    if (previous != nullptr && previous != INVALID_HANDLE_VALUE) {
        CloseHandle(previous);
    }
    ResolveNtdllEntryPoints();
    SetUnhandledExceptionFilter(&HandleUnhandledException);
    ReportPathStorage() = report;
    return true;
#else
    static bool backtrace_primed = false;
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
    if (!backtrace_primed) {
        // The first backtrace call loads libgcc's unwinder; doing it here keeps that
        // work out of the signal handler.
        void* warmup[1];
        (void)backtrace(warmup, 1);
        backtrace_primed = true;
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
