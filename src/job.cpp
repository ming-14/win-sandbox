// job.cpp — one confined run's job: resource limits + kill-on-close + an IOCP
// thread that terminates the whole job on a hard-limit hit.
//
// Windows does NOT auto-terminate on Job limit notifications — an associated
// completion port only delivers the message — so the handling side must call
// TerminateJobObject. That is this thread's whole job.
#include "winacl.h"

#include <stdexcept>

namespace winacl {
namespace {

constexpr ULONG_PTR kJobCompletionKey = 0x01;

void check(bool ok, const char* what) {
  if (!ok) {
    throw std::runtime_error(std::string(what) + " failed (Win32 " + std::to_string(GetLastError()) + ")");
  }
}

}  // namespace

Job::Job(const ResourceLimits& limits) {
  job_ = CreateJobObjectW(nullptr, nullptr);
  check(job_ != nullptr, "CreateJobObjectW");
  try {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (limits.cpuMs > 0) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
      // FILETIME 100ns units.
      ext.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
          static_cast<LONGLONG>(limits.cpuMs) * 10'000;
    }
    if (limits.memoryMb > 0) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
      ext.ProcessMemoryLimit = static_cast<SIZE_T>(limits.memoryMb) * 1024 * 1024;
    }
    if (limits.jobMemoryMb > 0) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
      ext.JobMemoryLimit = static_cast<SIZE_T>(limits.jobMemoryMb) * 1024 * 1024;
    }
    if (limits.maxProcesses > 0) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
      ext.BasicLimitInformation.ActiveProcessLimit = limits.maxProcesses;
    }
    if (limits.breakawayOk) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    }
    if (limits.crashSilent) {
      ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    }
    check(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &ext, sizeof(ext)) != 0,
          "SetInformationJobObject(ExtendedLimitInformation)");

    if (limits.cpuRatePercent > 0) {
      // 0.01% units; HARD_CAP throttles instead of notifying. Win8+; a failure
      // degrades to no rate cap (fail-open).
      JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
      cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
      cpu.CpuRate = limits.cpuRatePercent * 100;
      SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu));
    }

    if (limits.noUi) {
      JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
      ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS
          | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS;
      check(SetInformationJobObject(job_, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) != 0,
            "SetInformationJobObject(BasicUIRestrictions)");
    }

    // Associate the completion port BEFORE any process enters the job.
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    check(iocp_ != nullptr, "CreateIoCompletionPort");
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT associate{};
    associate.CompletionKey = reinterpret_cast<void*>(kJobCompletionKey);
    associate.CompletionPort = iocp_;
    check(SetInformationJobObject(job_, JobObjectAssociateCompletionPortInformation, &associate,
                                  sizeof(associate)) != 0,
          "SetInformationJobObject(AssociateCompletionPort)");

    iocpThread_ = CreateThread(nullptr, 0, &Job::iocpThread, this, 0, nullptr);
    check(iocpThread_ != nullptr, "CreateThread(IOCP)");
  } catch (...) {
    // Partial construction: stop the thread, release what exists.
    stop();
    if (job_ != nullptr) CloseHandle(job_);
    job_ = nullptr;
    throw;
  }
}

Job::~Job() {
  stop();
  if (job_ != nullptr) CloseHandle(job_);
}

void Job::terminateAll(uint32_t exitCode) {
  if (job_ != nullptr) TerminateJobObject(job_, exitCode);
}

LimitKind Job::limitKind() const {
  return static_cast<LimitKind>(InterlockedCompareExchange(
      const_cast<volatile LONG*>(&limitKind_), 0, 0));
}

void Job::stop() {
  if (InterlockedExchange(&stopRequested_, 1) == 0 && iocp_ != nullptr) {
    PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);  // wake the loop
  }
  if (iocpThread_ != nullptr) {
    WaitForSingleObject(iocpThread_, INFINITE);
    CloseHandle(iocpThread_);
    iocpThread_ = nullptr;
  }
  if (iocp_ != nullptr) {
    CloseHandle(iocp_);
    iocp_ = nullptr;
  }
}

DWORD WINAPI Job::iocpThread(LPVOID param) {
  static_cast<Job*>(param)->iocpLoop();
  return 0;
}

void Job::iocpLoop() {
  for (;;) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    if (!GetQueuedCompletionStatus(iocp_, &bytes, &key, &overlapped, INFINITE)) {
      continue;  // a failed wait is not a reason to die; the stop sentinel wakes us
    }
    if (key != kJobCompletionKey) {
      // The stop sentinel (key 0) — leave the loop when shutdown was asked.
      if (InterlockedCompareExchange(&stopRequested_, 0, 0) != 0) break;
      continue;
    }
    const DWORD message = bytes;
    const DWORD pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(overlapped));
    handleMessage(message, pid);
    if (InterlockedCompareExchange(&stopRequested_, 0, 0) != 0) break;
  }
}

void Job::handleMessage(DWORD message, DWORD pid) {
  switch (message) {
    case JOB_OBJECT_MSG_END_OF_JOB_TIME:
    case JOB_OBJECT_MSG_END_OF_PROCESS_TIME:
      if (InterlockedCompareExchange(&limitKind_, static_cast<LONG>(LimitKind::Cpu),
                                     static_cast<LONG>(LimitKind::None)) == static_cast<LONG>(LimitKind::None)) {
        terminateAll(1);
      }
      break;
    case JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT:
    case JOB_OBJECT_MSG_JOB_MEMORY_LIMIT:
      if (InterlockedCompareExchange(&limitKind_, static_cast<LONG>(LimitKind::Memory),
                                     static_cast<LONG>(LimitKind::None)) == static_cast<LONG>(LimitKind::None)) {
        terminateAll(1);
      }
      break;
    case JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT:
      // Creation-time rejection: existing processes are not at fault, so
      // nothing is terminated — only recorded for the exit report.
      InterlockedCompareExchange(&limitKind_, static_cast<LONG>(LimitKind::ProcessCount),
                                 static_cast<LONG>(LimitKind::None));
      break;
    default:
      break;
  }
}

}  // namespace winacl
