#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "app/CrashHandler.h"

#include "app/RuntimeStateRecorder.h"
#include "core/Log.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#if defined(_WIN32)
// Windows.h must precede DbgHelp.h (DbgHelp declares against windef types and
// does not include them itself). The blank line keeps clang-format from
// re-sorting the two into alphabetical — and broken — order.
#include <Windows.h>

#include <DbgHelp.h>
#endif

using namespace Luminumbra::Client::ScenarioHarness;

namespace Luminumbra::Client::App {

namespace {

// The recorder the installed unhandled-exception filter reports through.
// Set once by InstallRuntimeCrashHandler and retained for the callback lifetime.
RuntimeStateRecorder*& RuntimeStateRecorderSlot() {
    static RuntimeStateRecorder* recorder = nullptr;
    return recorder;
}

} // namespace

#if defined(_WIN32)

namespace {

bool WriteMiniDump(EXCEPTION_POINTERS* exception_info,
                   const std::filesystem::path& crash_dir,
                   const char* stem = "luminumbra") {
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    const std::filesystem::path dump_path =
        crash_dir / (std::string(stem) + "-" + TimestampForFile() + ".dmp");

    HANDLE file = CreateFileW(dump_path.wstring().c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_information{};
    exception_information.ThreadId = GetCurrentThreadId();
    exception_information.ExceptionPointers = exception_info;
    exception_information.ClientPointers = FALSE;

    const BOOL wrote_dump = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        exception_info ? MiniDumpNormal
                       : static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                    MiniDumpWithIndirectlyReferencedMemory),
        exception_info ? &exception_information : nullptr,
        nullptr,
        nullptr);
    CloseHandle(file);
    return wrote_dump == TRUE;
}

// Walk and symbolize `thread` from `ctx` (mutated as it unwinds), emitting one
// line per frame. Shared by the crash filter and the hang report.
void WalkStackFrames(HANDLE proc,
                     HANDLE thread,
                     CONTEXT& ctx,
                     const std::function<void(const std::string&)>& emit) {
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                         proc,
                         thread,
                         &frame,
                         &ctx,
                         nullptr,
                         SymFunctionTableAccess64,
                         SymGetModuleBase64,
                         nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0)
            break;

        const DWORD64 mod_base = SymGetModuleBase64(proc, pc);
        char mod_name[MAX_PATH] = "?";
        if (mod_base) {
            GetModuleFileNameA(reinterpret_cast<HMODULE>(mod_base), mod_name, MAX_PATH);
        }
        const DWORD64 rva = mod_base ? (pc - mod_base) : 0;

        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp = 0;
        const char* fn = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "??";

        const char* slash = std::strrchr(mod_name, '\\');
        const char* mod_short = slash ? slash + 1 : mod_name;

        char b[1100];
        std::snprintf(b,
                      sizeof b,
                      "#%-2d 0x%016llX  %s+0x%llX  %s",
                      i,
                      static_cast<unsigned long long>(pc),
                      mod_short,
                      static_cast<unsigned long long>(rva),
                      fn);
        emit(b);
    }
}

// Walk + symbolize the faulting thread's stack at crash time and write it to a
// readable text file + the (now file-backed) log. The minidump path has been
// producing EMPTY.dmp files, and logs were stdout-only, so a normal-launch crash
// left nothing to diagnose. DbgHelp resolves public/export names; the per-frame
// `module+0xRVA` lets addr2line recover exact file:line from the binary's DWARF
// (run tools/gates/symbolize-crash.ps1 on the crash file). A crash handler must
// not itself throw — everything here is guarded and bounded.
void WriteCrashStackTrace(EXCEPTION_POINTERS* xp, const std::filesystem::path& crash_dir) {
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    const std::filesystem::path path = crash_dir / ("crash-" + TimestampForFile() + ".txt");
    std::ofstream out(path);

    const HANDLE proc = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    auto emit = [&](const std::string& s) {
        if (out) {
            out << s << "\n";
            out.flush();
        } // flush per line: the process may be torn down any moment
        LUMINUMBRA_CORE_CRITICAL("{}", s);
    };

    const uint32_t code = (xp && xp->ExceptionRecord)
                              ? static_cast<uint32_t>(xp->ExceptionRecord->ExceptionCode)
                              : 0u;
    void* faddr = (xp && xp->ExceptionRecord) ? xp->ExceptionRecord->ExceptionAddress : nullptr;
    {
        char b[160];
        std::snprintf(b,
                      sizeof b,
                      "=== LUMINUMBRA CRASH  exception=0x%08X  faulting_addr=%p ===",
                      code,
                      faddr);
        emit(b);
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);

    if (!xp || !xp->ContextRecord) {
        emit("(no thread context captured — cannot walk the stack)");
        SymCleanup(proc);
        return;
    }

    CONTEXT ctx = *xp->ContextRecord; // StackWalk64 mutates the context as it unwinds.
    {
        char b[256];
        std::snprintf(b,
                      sizeof b,
                      "regs: rip=0x%llX rsp=0x%llX rbp=0x%llX",
                      (unsigned long long)ctx.Rip,
                      (unsigned long long)ctx.Rsp,
                      (unsigned long long)ctx.Rbp);
        emit(b);
    }
    // NULL function-pointer call (rip==0, faulting_addr==0): the CALL already pushed the
    // return address, so [rsp] holds the CALLER's address. Seed the walk from there so the
    // first symbolized frame names WHO called null (StackWalk64 can't start from pc==0).
    if (ctx.Rip == 0 && ctx.Rsp != 0) {
        const DWORD64 ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        char b[160];
        std::snprintf(b,
                      sizeof b,
                      "(rip==0: null call — caller return addr [rsp]=0x%llX)",
                      (unsigned long long)ret);
        emit(b);
        ctx.Rip = ret; // pretend we are in the caller
        ctx.Rsp += 8;  // pop the pushed return address
    }
    WalkStackFrames(proc, thread, ctx, emit);
    emit("=== end stack ===  (resolve file:line with tools/gates/symbolize-crash.ps1 <crash.txt>)");
    SymCleanup(proc);
}

LONG WINAPI RuntimeUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    // One-shot: a parallel null-pointer fault hits on EVERY worker thread at once. The
    // first thread in writes the (single, clean) crash report; the rest just terminate
    // so the log isn't spammed and the report isn't interleaved.
    static std::atomic<bool> s_handling{false};
    bool expected = false;
    if (!s_handling.compare_exchange_strong(expected, true)) {
        // A sibling thread (the same parallel fault hits every worker at once) is already
        // writing the report. Do NOT return — that terminates the process and kills the
        // writer mid-flush (which left an empty crash file). PARK here; the writer's
        // return tears the whole process (and us) down once the report is on disk.
        for (;;) {
            Sleep(1000);
        }
    }

    const uint32_t exception_code =
        exception_info && exception_info->ExceptionRecord
            ? static_cast<uint32_t>(exception_info->ExceptionRecord->ExceptionCode)
            : 0;
    // Flush buffered breadcrumbs to the file sink FIRST, so logs/luminumbra.log holds
    // the pre-crash context even if a later step here itself faults.
    if (auto& lg = Log::GetCoreLogger())
        lg->flush();

    RuntimeStateRecorder* recorder = RuntimeStateRecorderSlot();
    const std::filesystem::path crash_dir =
        recorder ? recorder->crash_dir() : std::filesystem::path("crashes");
    WriteCrashStackTrace(exception_info, crash_dir);

    if (recorder) {
        recorder->mark_unhandled_exception(exception_code);
        WriteMiniDump(exception_info, recorder->crash_dir());
    }
    if (auto& lg = Log::GetCoreLogger())
        lg->flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    RuntimeStateRecorderSlot() = &recorder;
    SetUnhandledExceptionFilter(RuntimeUnhandledExceptionFilter);
}

namespace {

// Fixed-size, allocation-free writer used by the hang report: the main thread may
// be blocked inside the allocator or the logger, so this path touches neither.
struct RawFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit RawFile(const wchar_t* path) {
        handle = CreateFileW(path,
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    }
    ~RawFile() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    void line(const char* text) noexcept {
        if (handle == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(handle, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        WriteFile(handle, "\r\n", 2, &written, nullptr);
        FlushFileBuffers(handle);
    }
};

} // namespace

void ReportMainThreadHang(std::uint64_t last_heartbeat, double stalled_seconds) noexcept {
    // Best-effort, Windows-only. The report is written with raw Win32 file calls and
    // the dump is taken by an external process (rundll32 comsvcs.dll MiniDump), so
    // nothing here suspends, allocates on, logs from or shares DbgHelp with the
    // possibly-hung main thread. Symbolize the dump offline with the retained PDB.
    RuntimeStateRecorder* recorder = RuntimeStateRecorderSlot();
    wchar_t dir[MAX_PATH] = L"crashes";
    if (recorder) {
        const std::wstring wide = recorder->crash_dir().wstring();
        if (wide.size() < MAX_PATH)
            std::wcsncpy(dir, wide.c_str(), MAX_PATH - 1);
    }
    CreateDirectoryW(dir, nullptr);
    SYSTEMTIME st{};
    GetSystemTime(&st);
    wchar_t stamp[32];
    swprintf(stamp,
             32,
             L"%04u%02u%02u-%02u%02u%02u",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond);
    wchar_t report_path[MAX_PATH], dump_path[MAX_PATH];
    swprintf(report_path, MAX_PATH, L"%s\\hang-%s.txt", dir, stamp);
    swprintf(dump_path, MAX_PATH, L"%s\\hang-%s.dmp", dir, stamp);

    RawFile report(report_path);
    char line[512];
    std::snprintf(line,
                  sizeof line,
                  "=== LUMINUMBRA HANG  heartbeat=%llu  stalled=%.1fs  pid=%lu ===",
                  static_cast<unsigned long long>(last_heartbeat),
                  stalled_seconds,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    report.line(line);

    wchar_t command[MAX_PATH * 2 + 96];
    swprintf(command,
             MAX_PATH * 2 + 96,
             L"rundll32.exe C:\\Windows\\System32\\comsvcs.dll,MiniDump %lu \"%s\" mini",
             static_cast<unsigned long>(GetCurrentProcessId()),
             dump_path);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(
        nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (started) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::snprintf(line,
                      sizeof line,
                      "external minidump: %s (wait=%lu exit=%lu)",
                      wait == WAIT_OBJECT_0 ? "finished" : "timed out",
                      static_cast<unsigned long>(wait),
                      static_cast<unsigned long>(code));
        report.line(line);
    } else {
        std::snprintf(line,
                      sizeof line,
                      "external minidump: CreateProcess failed (error %lu)",
                      static_cast<unsigned long>(GetLastError()));
        report.line(line);
    }
    report.line(
        "symbolize offline: cdb -z <hang-*.dmp> -c \"~*k; q\" with the PDB beside the executable");
    report.line("=== end hang report ===");
}
#else
void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    RuntimeStateRecorderSlot() = &recorder;
}

void ReportMainThreadHang(std::uint64_t last_heartbeat, double stalled_seconds) noexcept {
    // Text record only (no stack capture off Windows); raw stdio, no logger, no
    // allocation beyond the path string built before any file is touched.
    RuntimeStateRecorder* recorder = RuntimeStateRecorderSlot();
    std::string dir = recorder ? recorder->crash_dir().string() : std::string("crashes");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[128];
    std::snprintf(
        name, sizeof name, "/hang-%llu.txt", static_cast<unsigned long long>(last_heartbeat));
    dir += name;
    if (FILE* f = std::fopen(dir.c_str(), "w")) {
        std::fprintf(f,
                     "=== LUMINUMBRA HANG  heartbeat=%llu  stalled=%.1fs ===\n(stack capture is "
                     "Windows-only)\n",
                     static_cast<unsigned long long>(last_heartbeat),
                     stalled_seconds);
        std::fclose(f);
    }
}
#endif

[[noreturn]] void TriggerForcedCrash() {
#if defined(_WIN32)
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    std::raise(SIGABRT);
#endif
    std::abort();
}

} // namespace Luminumbra::Client::App
