#!/usr/bin/env python3
"""Pin shutdown ordering and supply the production destructor to the native probe."""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/luminumbra_client/audio/AudioPropagationSystem.cpp"


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]',
                             source[start:], re.DOTALL):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:start + token.start()]
    raise AssertionError(f"Unclosed function: {signature}")


def code(text):
    return re.sub(r"\s+", "", re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL))


def check():
    source = SOURCE.read_text(encoding="utf-8")
    destructor = body(source, "AudioPropagationSystem::~AudioPropagationSystem()")
    assert code(destructor) == code("""
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            m_shutdown = true;
        }
        m_task_cv.notify_all();
        for (auto& thread : m_worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    """), "Shutdown must lock, store, unlock, notify all, then join every joinable worker"
    assert len(re.findall(r"m_shutdown\s*=", source)) == 1, "Audit every shutdown writer"
    assert source.count("m_task_cv.wait(") == 1, "Audit every task waiter"
    worker = body(source, "void AudioPropagationSystem::WorkerThreadFunction()")
    assert code(worker) == code("""
        LUMIN_PROFILE_THREAD("Audio propagation worker");
        while (!m_shutdown) {
            AsyncTask task;
            {
                LUMIN_PROFILE_LOCK(
                    std::unique_lock<std::mutex>, lock, m_task_mutex, "Audio task lock wait");
                LUMIN_PROFILE_WAIT("Audio condition wait", m_task_cv.wait(lock, [this] {
                    return !m_pending_tasks.empty() || m_shutdown;
                }));
                if (m_shutdown)
                    break;
                if (!m_pending_tasks.empty()) {
                    task = std::move(m_pending_tasks.back());
                    m_pending_tasks.pop_back();
                    m_metrics.active_calculations++;
                } else {
                    continue;
                }
            }
            LUMIN_PROFILE_ZONE_N("Audio worker job");
            AudioPropagationResult result =
                CalculatePropagation(task.source, task.listener, task.max_distance);
            if (task.callback) {
                task.callback(result);
            }
            m_metrics.active_calculations--;
        }
    """), "Keep worker predicate, queued-task, callback and profiling semantics"
    enqueue = body(source, "void AudioPropagationSystem::CalculatePropagationAsync(")
    assert code(enqueue) == code("""
        {
            LUMIN_PROFILE_LOCK(std::lock_guard<std::mutex>, lock, m_task_mutex, "Audio task lock wait");
            m_pending_tasks.push_back({source, listener, max_distance, std::move(callback)});
        }
        m_task_cv.notify_one();
    """), "Keep enqueue and notification semantics"
    cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
    for registration in (
            "add_executable(audio_shutdown_test audio/audio_shutdown_test.cpp",
            "add_test(NAME AudioShutdownTransition COMMAND audio_shutdown_test)",
            "add_test(NAME AudioShutdownSourceContract COMMAND",
            "${CMAKE_SOURCE_DIR}/test/tools/check_audio_shutdown_contract.py",
            "--emit-destructor ${audio_shutdown_body}",
            "DEPENDS ${audio_shutdown_source} ${audio_shutdown_generator}",
            "luminumbra_configure_first_party_target(audio_shutdown_test)"):
        assert registration in cmake, f"Missing native/source registration: {registration}"
    paths = (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
    assert '"test/tools/check_audio_shutdown_contract.py",' in paths
    print("Audio shutdown source contracts passed")


def emit(destination):
    # Compile the actual production body, not a hand-copied shutdown algorithm.
    # Only the class name and mutex type change; all control flow stays intact.
    destructor = body(SOURCE.read_text(encoding="utf-8"),
                      "AudioPropagationSystem::~AudioPropagationSystem()")
    destination.write_text("ShutdownFixture::~ShutdownFixture() {\n"
                           + destructor.replace("std::mutex", "TransitionMutex")
                           + "\n}\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit-destructor", type=Path)
    args = parser.parse_args()
    if args.emit_destructor is not None:
        emit(args.emit_destructor)
    else:
        check()
