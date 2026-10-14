# Dedicated server crash capture qualification

The dedicated server installs capture once on its process entry thread. POSIX
reinstallation from another thread is rejected with a stderr diagnostic before
the report is opened or truncated: `sigaltstack` belongs to the installing
thread, so its storage must never be shared with a worker. Same-thread
reinstallation remains supported. An unchanged report in a previously used
directory reuses its open writer, header (including the original program name),
contents, and offset. Superseded report descriptors and handles stay open until
process exit so a handler that already loaded one can finish writing. A static
array bounds retention to eight distinct targets (seven superseded plus the
current one); another new target is refused with a stderr diagnostic, leaving
the previous capture target installed. Retained targets remain reusable even
after that limit. This avoids handler-side reclamation and leaves the POSIX
handler's lock-free, async-signal-safe write path unchanged. Same-target
reinstallation consumes no additional descriptors or handles; the server's
normal startup uses one. Windows uses a temporary read-only attributes handle
to check file identity and closes it before reusing the existing writer.
Windows module and unwind entry-point facts are
cached once before publishing the first handle, so reinstall cannot change them
while an exception filter reads them.

New targets are created exclusively, never with truncation: an available name
is `crash-<pid>.txt`, while collisions use `crash-<pid>-<suffix>.txt`. Up to
1024 candidate names are tried before refusing. A removed or replaced report
is detected by file identity and gets a fresh file, so headers and offsets
cannot alias a live old target, including through a pre-existing hard link.
The text format remains `luminumbra-crash-report v1`; `CrashReportPath()` reports
the selected name, and the qualification harness collects artifacts without
parsing report filenames. Child crash tests still expect the unsuffixed name
in their initially empty directory.

Installer tests run in isolated children so gtest repetitions cannot accumulate
retained targets in the test runner. Regressions cover 64 same-thread reinstalls
with stable descriptor counts on Linux when `/proc/self/fd` is available,
preserved report contents, replaced files, existing evidence in other targets,
and refusal after eight targets with subsequent reuse still working. These C++
tests have not been run for this revision.

The POSIX child tests use defined `raise(SIGSEGV)` and `raise(SIGABRT)` triggers
and retain the signal, report header, fault address, and supported register
assertions. They find the test executable through `/proc/self/exe` on Linux,
`_NSGetExecutablePath` on macOS, and `sysctl` on FreeBSD and NetBSD. The macOS
child environment is obtained with `_NSGetEnviron`.

The Windows synthetic unit test calls the installed unhandled-exception filter
with an `EXCEPTION_RECORD` and an empty `CONTEXT`. It asserts the exception code
and address, thread ID, cached module/RVA, frame section, re-entry suppression,
and `EXCEPTION_CONTINUE_SEARCH` return value. Calls run in a normally exiting
child, preserving the parent process's once-only guard. It raises no native
exception and does not exercise a real stack unwind. Actual SEH delivery, unwind fidelity,
re-entry during a real fault, and WER LocalDumps production remain
**NEEDS-NATIVE-RUN** on MSVC and MinGW. Returning continue-search alone does not
prove a dump was produced. These C++ tests have not been run for this revision.

`tools/qualify/repeat_qualification.py` copies the executable once into a private
temporary directory under its run directory. It checks the copy against the
original's initial SHA-256, launches only that copy, and checks the copy again
before and after every child. Replacing the original after staging cannot change
the executable used by later iterations. The receipt records both the original
path and the snapshot path; the snapshot is removed on completion or a Python
exception, while logs and receipts remain. A forcibly killed harness may leave
its staging directory behind.

The child retains `--cwd`, or the caller's working directory when omitted. Use
the original package directory as the working directory for package-relative
assets. On Windows, adjacent `.dll` files are copied beside the snapshot because
the loader searches the executable directory first; the executable's filename
is retained for WER LocalDumps settings. DLLs and assets are not covered by the
executable's SHA-256 identity guarantee. On POSIX, adjacent `.so` files (including
versioned names) and `.dylib` files are also copied to retain `$ORIGIN` and
`@loader_path` library lookup; these libraries are likewise outside that guarantee.

The harness holds an OS file lock on each shared core
or WER directory from its baseline snapshot through child exit, dump settling,
and artifact copying. Concurrent invocations with different receipts serialize
that interval. The persistent `.luminumbra-qualification-dumps.lock` file is
excluded from artifact scans and must not be removed while a harness is running;
closing its handle releases ownership, including when the process exits.
On Windows, byte zero is locked before inspecting or initializing the file;
Windows byte-range locks can extend beyond EOF. The initializer writes and
verifies the byte under ownership. Contention and initialization permission
errors retry within a five-minute acquisition deadline; failed initialization
releases the lock before retrying. In-process regressions pause the first
initializer while a second contender attempts acquisition, and cover retries,
timeout, verification, and existing lock-file reuse.
Python regressions overlap two invocations for both core and WER collection and
check that each receipt contains only its own artifact. This locks cooperating
harness invocations; unrelated dump producers still need a separate directory.

A POSIX dump-preflight failure with `--allow-no-dumps` retains its diagnostics
and disables core-directory locking and collection for that run, allowing the
child checks to run. Writer-completion detection is tracked in issue #306.

Receipts retain the label `process repetitions`. A passed receipt proves its
process/gtest checks passed; it does not by itself qualify native crash-dump
capture. Dump preflight overrides and collection errors remain recorded.
