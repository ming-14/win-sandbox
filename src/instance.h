// instance.h — in-process sandbox instance and per-process handle.
//
// The nanobind-facing core: SandboxInstance materializes the workspace/temp
// capability grants, builds the WRITE_RESTRICTED token, and spawns confined
// processes under a Job with optional resource limits. SandboxedProcess owns
// one confined run's handles (process/token/job/private temp) and answers
// wait/terminate/signal/process-tree queries in-process — no IPC, no protocol
// lines. Stdio comes from one of three shapes: the launcher's own std handles,
// three pipes created here (their parent ends come back on the process object),
// or an external HPCON. An external HPCON is usable directly because the pseudo
// console is created by the host process (the same process that calls
// CreateProcessAsUserW).
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
 * One confined run: the spawned primary process plus its job, token, private
 * temp directory, and settled exit facts. The temp directory is always
 * self-managed: dispose() revokes its capability grant and removes it.
 *
 * Ownership: SandboxInstance keeps a shared_ptr per process; PyProcess holds
 * another. The object survives shutdown (disposed) so Python wrappers never
 * dangle.
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

  /** The parent's end of the pipes: what to write the child's stdin with and
   *  what to read its stdout/stderr from. Nullptr unless the run was spawned
   *  with pipes — and the caller owns them from that moment on. */
  HANDLE stdInWrite() const { return child_.stdInWrite; }
  HANDLE stdOutRead() const { return child_.stdOutRead; }
  HANDLE stdErrRead() const { return child_.stdErrRead; }

  /** Release handles, revoke the temp grant, remove the temp dir. The pipe
   *  ends above belong to the caller and are left untouched.
   *  Idempotent; after this the object answers no live queries. */
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
 * One sandbox: owns the per-workspace standing grants, the live processes,
 * and their private-temp grants. startProcess materializes the capabilities,
 * builds the restricted token, and spawns; shutdown/dispose terminates every
 * live process, revokes the temp grants, and removes the private temp
 * directories (workspace ACEs stand — the reuse cache).
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
   * @param workspaceWrite - true = the workspace is writable too; false =
   *                         read-only workspace. Either way the run gets its
   *                         own writable private temp directory.
   * @param limits - resource limits (zeros = unlimited).
   * @param hpcon - external pseudo console handle, or nullptr to use one of the
   *                two handle shapes below.
   * @param envOverrides - child environment entries; an entry whose name is
   *                      already present replaces it (may be null).
   * @param pipeStdio - with hpcon null: hand the child three anonymous pipes
   *                    instead of the launcher's own std handles. The parent's
   *                    ends come back on the process object.
   * @returns the process object (shared ownership: instance + PyProcess).
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
