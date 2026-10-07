// winacl.h — shared definitions for the in-process Windows confinement core: a
// WRITE_RESTRICTED token whose restricting SIDs carry the write capabilities,
// inside a kill-on-close job, with the caller's stdio passed straight through.
// Fail-closed: every Win32 failure throws before the child is spawned.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace winacl {

// SHA-256 of a byte range (BCrypt streaming API; implemented in sid.cpp).
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t size);

// The sandbox grant = "Modify" (0x110156): FILE_GENERIC_WRITE minus READ_CONTROL
// plus DELETE and FILE_DELETE_CHILD. WRITE_DAC/WRITE_OWNER are deliberately out,
// so a confined child can never take ownership or rewrite DACLs to escape.
constexpr DWORD kGrantMask = (FILE_GENERIC_WRITE | DELETE | FILE_DELETE_CHILD) & ~STANDARD_RIGHTS_WRITE;

// Restricting-token flags: strip max privilege, synthesize the limited-user
// (filtered admin) effect, and intersect writes with restricting-SID grants.
constexpr DWORD kRestrictFlags = DISABLE_MAX_PRIVILEGE | LUA_TOKEN | WRITE_RESTRICTED;
constexpr DWORD kLuaFlags = DISABLE_MAX_PRIVILEGE | LUA_TOKEN;

// --- sid.cpp ----------------------------------------------------------------

// Deterministic S-1-4-x-y capability SID for the canonical workspace path. The
// derivation is frozen so ACEs from earlier sessions stay reusable.
std::wstring workspaceWriteSid(const std::wstring& workspaceRoot);

// Deterministic S-1-4-x-y-1 capability SID derived from a private temp path
// (same frozen derivation, third subauthority fixed at 1).
std::wstring tempWriteSid(const std::wstring& tempDir);

// Convert an SDDL SID string to a freshly LocalAlloc'd SID (caller frees).
PSID parseSid(const std::wstring& sddl);

// --- token.cpp --------------------------------------------------------------

// Build the write-restricted token. `writeSids` is the whole write whitelist:
// empty for a read-only run, else the workspace + private temp capability SIDs.
// `outLogonSid`/`outWorldSid` come back caller-owned (LocalFree).
HANDLE createRestrictedToken(const std::vector<PSID>& writeSids,
                             PSID& outLogonSid, PSID& outWorldSid);

// Merge a full-access ACE for `sid` into the token's default DACL, so objects
// the confined child creates (pipes, events) pass the restricting-SID write
// check. `sid` must be a restricting SID — the per-run temp SID, or the logon
// SID for read-only runs.
void setTokenDefaultDaclGrant(HANDLE token, PSID sid);

// --- acl.cpp ----------------------------------------------------------------

// Grant kGrantMask (OI|CI) to `sid` on `dir`, under a per-path lock. The apply is
// skipped when the exact ACE already stands: SetNamedSecurityInfoW would
// otherwise re-propagate it across the whole tree.
void grantWrite(const std::wstring& dir, PSID sid);

// Remove every ACE for `sid` from `dir`'s DACL (other entries preserved).
void revokeWrite(const std::wstring& dir, PSID sid);

// Deny the sandbox's restricting SIDs (logon SID + Everyone) write-class process
// rights on the host — PROCESS_TERMINATE, VM write, thread creation, handle
// duplication, etc. — so the child's restricting pass-2 check fails for them.
// Idempotent; fails closed before the spawn.
void hardenHostProcessDacl(PSID logonSid, PSID worldSid);

// --- job.cpp ----------------------------------------------------------------

/** Resource limits for one confined run; 0/absent fields mean "unlimited". */
struct ResourceLimits {
  uint64_t memoryMb = 0;          // per-process commit limit
  uint64_t jobMemoryMb = 0;       // whole-job commit limit
  uint64_t cpuMs = 0;             // per-job CPU time (JOB_TIME)
  uint32_t cpuRatePercent = 0;    // CPU rate hard cap (Win8+; 0 = off)
  uint32_t maxProcesses = 0;      // active-process limit
  uint64_t wallClockMs = 0;       // wall-clock timeout (launcher-enforced)
  bool noUi = false;              // Job UI restrictions (handles/system/display/atoms)
  bool crashSilent = false;       // DIE_ON_UNHANDLED_EXCEPTION
  bool breakawayOk = false;       // allow children to break away
};

/** Why a confined run ended, for the exit protocol. */
enum class LimitKind { None, Cpu, Memory, ProcessCount, Timeout, User };

/**
 * One confined run's job, plus the IOCP thread that terminates it on a hard-limit
 * notification (END_OF_JOB_TIME / PROCESS_MEMORY_LIMIT / JOB_MEMORY_LIMIT) —
 * Windows does not auto-kill on those.
 */
class Job {
public:
  explicit Job(const ResourceLimits& limits);
  ~Job();
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  HANDLE handle() const { return job_; }
  /** Terminate every process in the job. */
  void terminateAll(uint32_t exitCode);
  /** The limit that killed the job (thread-safe read). */
  LimitKind limitKind() const;
  /** Stop the IOCP thread (join). */
  void stop();

private:
  static DWORD WINAPI iocpThread(LPVOID param);
  void iocpLoop();
  void handleMessage(DWORD message, DWORD pid);

  HANDLE job_ = nullptr;
  HANDLE iocp_ = nullptr;
  HANDLE iocpThread_ = nullptr;
  volatile LONG limitKind_ = static_cast<LONG>(LimitKind::None);  // LimitKind
  volatile LONG stopRequested_ = 0;
};

// --- spawn.cpp --------------------------------------------------------------

// One spawned child: the process handle (owned) and pid, plus the parent's end of
// each pipe for the piped shape (nullptr otherwise). Those three handles belong
// to the caller from the moment the spawn returns; the sandbox process object
// never closes them.
struct SpawnedChild {
  HANDLE process;
  DWORD pid;
  HANDLE stdInWrite = nullptr;   // piped shape only
  HANDLE stdOutRead = nullptr;   // piped shape only
  HANDLE stdErrRead = nullptr;   // piped shape only
};

// Spawn with the caller's std handles passed straight through: the child
// inherits the three handles the launcher itself has.
SpawnedChild spawnSandboxedInherited(HANDLE token, HANDLE job, const std::wstring& commandLine,
                                     const std::wstring& cwd, bool newProcessGroup,
                                     const std::wstring* tempDir = nullptr,
                                     const std::map<std::wstring, std::wstring>* envOverrides = nullptr);

// Spawn with three anonymous pipes created here: the child gets stdin's read end
// and stdout/stderr's write ends; the caller gets the opposite ends.
SpawnedChild spawnSandboxedPiped(HANDLE token, HANDLE job, const std::wstring& commandLine,
                                 const std::wstring& cwd, bool newProcessGroup,
                                 const std::wstring* tempDir = nullptr,
                                 const std::map<std::wstring, std::wstring>* envOverrides = nullptr);

// Spawn under an external ConPTY handle: the pseudo console drives the child's
// stdio. bInheritHandles=FALSE + EXTENDED_STARTUPINFO_PRESENT + the
// PSEUDOCONSOLE thread attribute, and no CREATE_SUSPENDED — a suspended primary
// thread fails DLL initialization under a pseudo console.
SpawnedChild spawnSandboxedConPTY(HANDLE token, HANDLE job, HPCON hpcon,
                                  const std::wstring& commandLine,
                                  const std::wstring& cwd,
                                  const std::wstring* tempDir = nullptr,
                                  const std::map<std::wstring, std::wstring>* envOverrides = nullptr);

// --- util -------------------------------------------------------------------

// UTF-8 <-> UTF-16 conversion for the binding boundary (implemented in sid.cpp).
std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

}  // namespace winacl
