// instance.h — in-process sandbox instance and per-process handle: the
// nanobind-facing core that materializes the capability grants, builds the
// restricted token, and spawns under a Job. Everything is in-process — no IPC,
// no protocol lines — which is also why an external HPCON works directly: the
// pseudo console belongs to the same process that calls CreateProcessAsUserW.
#pragma once

#include "winacl.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace winacl {

/** Why a confined run ended (string form for the Python layer). */
inline const char* limitKindString(LimitKind kind) {
  switch (kind) {
    case LimitKind::None: return "normal";
    case LimitKind::Cpu: return "cpu_limit";
    case LimitKind::Memory: return "memory_limit";
    case LimitKind::ProcessCount: return "process_count_limit";
    case LimitKind::Timeout: return "timeout";
    case LimitKind::User: return "user";
  }
  return "unknown";
}

/**
 * One confined run: process, job, token, private temp directory (writable runs
 * only), settled exit facts. A temp directory, when present, is self-managed.
 *
 * Ownership: SandboxInstance keeps a shared_ptr per process and PyProcess holds
 * another; the object survives shutdown (disposed) so wrappers never dangle.
 */
class SandboxedProcess {
public:
  SandboxedProcess(SpawnedChild child, std::unique_ptr<Job> job, HANDLE token,
                   std::wstring tempDir, uint64_t wallClockMs);
  ~SandboxedProcess();
  SandboxedProcess(const SandboxedProcess&) = delete;
  SandboxedProcess& operator=(const SandboxedProcess&) = delete;

  DWORD pid() const { return child_.pid; }
  bool disposed() const { return disposed_.load(); }

  /** Block until the primary process exits; returns (exit code, reason). */
  std::pair<uint32_t, std::string> wait();
  /** Non-blocking probe: settled (exit code, reason) or nullopt while running. */
  std::optional<std::pair<uint32_t, std::string>> pollExit();
  /** Terminate the whole job (reason = user). */
  void terminate(uint32_t exitCode = 1);
  /** Live process ids in the job. */
  std::vector<DWORD> queryProcessList();

  /** The parent's end of the pipes, nullptr unless spawned with pipes. The
   *  caller owns them from that moment on. */
  HANDLE stdInWrite() const { return child_.stdInWrite; }
  HANDLE stdOutRead() const { return child_.stdOutRead; }
  HANDLE stdErrRead() const { return child_.stdErrRead; }

  /** Release handles, revoke the temp grant, remove the temp dir; the pipe ends
   *  above are left untouched. Idempotent; afterwards no live queries. */
  void dispose();

private:
  /** Read the settled exit code and classify why the run ended. */
  std::pair<uint32_t, std::string> settleExit();

  std::atomic<bool> disposed_{false};
  bool exited_ = false;
  uint32_t exitCode_ = 0;
  std::string exitReason_ = "normal";
  volatile LONG userTerminated_ = 0;
  volatile LONG timedOut_ = 0;
  HANDLE wallClockEvent_ = nullptr;
  std::thread wallClockThread_;
  SpawnedChild child_;
  std::unique_ptr<Job> job_;
  HANDLE token_ = nullptr;
  std::wstring tempDir_;
};

/**
 * One sandbox: the per-workspace standing grants (the reuse cache) and the live
 * processes. shutdown/dispose terminates every live process, revokes the temp
 * grants and removes the private temp directories; workspace ACEs stand.
 */
class SandboxInstance {
public:
  SandboxInstance() = default;
  ~SandboxInstance() { shutdown(); }
  SandboxInstance(const SandboxInstance&) = delete;
  SandboxInstance& operator=(const SandboxInstance&) = delete;

  /**
   * Spawn one confined process.
   * @param commandLine - full command line (CreateProcessAsUserW splits it).
   * @param workingDir - workspace root (must exist; becomes the workspace).
   * @param workspaceWrite - true = workspace writable, plus a private writable
   *                         temp directory; false = strictly read-only: no
   *                         workspace grant, no private temp, TMP/TEMP left at
   *                         the host values, empty write whitelist.
   * @param limits - resource limits (zeros = unlimited).
   * @param hpcon - external pseudo console handle, or nullptr for one of the
   *                handle shapes below.
   * @param envOverrides - entries whose name is already present replace it.
   * @param pipeStdio - with hpcon null: three anonymous pipes instead of the
   *                    launcher's own std handles; the parent's ends come back
   *                    on the process object.
   * @returns the process object (shared with PyProcess).
   */
  std::shared_ptr<SandboxedProcess> startProcess(
      const std::wstring& commandLine, const std::wstring& workingDir, bool workspaceWrite,
      const ResourceLimits& limits, HPCON hpcon = nullptr,
      const std::map<std::wstring, std::wstring>* envOverrides = nullptr,
      bool pipeStdio = false);
  /** Dispose all processes, revoke temp grants, remove temp dirs. */
  void shutdown();

private:
  std::mutex mutex_;  // guards both members; startProcess runs without the GIL
  std::set<std::wstring> grantedWorkspaces_;
  std::vector<std::shared_ptr<SandboxedProcess>> processes_;
};

}  // namespace winacl
