// spawn.cpp — spawn the confined child under the restricted token.
//
// Three stdio shapes share one spawn core (`spawnWithStdio`): the caller's own
// std handles (inherited), three anonymous pipes created here, or an external
// HPCON driving a pseudo console. Every shape builds the child's environment
// block explicitly from the host environment, redirecting TMP/TEMP to the
// granted private temp directory when the run has one (writable runs) — the
// host environment is never modified, so concurrent spawns cannot race.
#include "winacl.h"

#include <wincon.h>
#include <cwchar>
#include <map>
#include <stdexcept>
#include <vector>

namespace winacl {
namespace {

// Copy the host environment block, replacing TMP/TEMP with the private temp
// directory when one is supplied and then applying the caller's overrides. A
// later entry for a name already present (matched case-insensitively, the way
// Windows resolves environment names) replaces the earlier one in place, so an
// override really overrides. Returns a double-NUL-terminated block.
std::vector<wchar_t> buildEnvBlock(const std::wstring* tempDir,
                                   const std::map<std::wstring, std::wstring>* overrides) {
  struct EnvGuard {
    LPWCH env;
    ~EnvGuard() { FreeEnvironmentStringsW(env); }
  };
  LPWCH env = GetEnvironmentStringsW();
  if (env == nullptr) {
    throw std::runtime_error("GetEnvironmentStringsW failed (Win32 "
                             + std::to_string(GetLastError()) + ")");
  }
  EnvGuard guard{env};

  auto upper = [](const std::wstring& text) {
    std::wstring out(text.size(), L'\0');
    if (!text.empty()) {
      LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, text.c_str(),
                   static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    }
    return out;
  };

  std::vector<std::wstring> entries;
  std::map<std::wstring, size_t> position;  // upper-cased name -> index in entries
  auto upsert = [&](const std::wstring& entry) {
    // Drive-current-directory entries ("=C:=...") carry no name to match on.
    if (entry.empty() || entry[0] == L'=') {
      entries.push_back(entry);
      return;
    }
    const std::wstring name = upper(entry.substr(0, entry.find(L'=')));
    const auto existing = position.find(name);
    if (existing != position.end()) {
      entries[existing->second] = entry;  // override in place
      return;
    }
    position.emplace(name, entries.size());
    entries.push_back(entry);
  };

  for (LPWCH p = env; *p != L'\0'; p += wcslen(p) + 1) upsert(p);
  if (tempDir != nullptr) {
    upsert(L"TMP=" + *tempDir);
    upsert(L"TEMP=" + *tempDir);
  }
  if (overrides != nullptr) {
    for (const auto& [key, value] : *overrides) upsert(key + L"=" + value);
  }

  std::vector<wchar_t> out;
  for (const auto& entry : entries) {
    out.insert(out.end(), entry.begin(), entry.end());
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

// Fill a one-entry process/thread attribute list into a caller-owned buffer, so
// the buffer never moves after the list pointer is derived from it.
void buildAttributeList(std::vector<uint8_t>& buf, const void* value, SIZE_T valueSize,
                        DWORD attribute) {
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);  // expected ERROR_INSUFFICIENT_BUFFER
  buf.resize(attrSize);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &attrSize)) {
    throw std::runtime_error("InitializeProcThreadAttributeList failed (Win32 "
                             + std::to_string(GetLastError()) + ")");
  }
  if (!UpdateProcThreadAttribute(list, 0, attribute, const_cast<void*>(value), valueSize,
                                 nullptr, nullptr)) {
    const DWORD err = GetLastError();
    DeleteProcThreadAttributeList(list);
    throw std::runtime_error("UpdateProcThreadAttribute failed (Win32 " + std::to_string(err) + ")");
  }
}

// The spawn core shared by the inherited and piped shapes: HANDLE_LIST
// whitelists exactly the three std handles (no stray inheritable handle leaks
// into the confined child), CREATE_SUSPENDED keeps the job assignment free of
// an escape window, and the primary thread is resumed only after the child is
// in the kill-on-close job.
SpawnedChild spawnWithStdio(HANDLE token, HANDLE job, const std::wstring& commandLine,
                            const std::wstring& cwd, bool newProcessGroup,
                            const std::wstring* tempDir,
                            const std::map<std::wstring, std::wstring>* envOverrides,
                            HANDLE stdIn, HANDLE stdOut, HANDLE stdErr) {
  HANDLE handles[] = {stdIn, stdOut, stdErr};
  std::vector<uint8_t> attrBuf;
  buildAttributeList(attrBuf, handles, sizeof(handles), PROC_THREAD_ATTRIBUTE_HANDLE_LIST);
  LPPROC_THREAD_ATTRIBUTE_LIST attrList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  struct AttrGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~AttrGuard() { DeleteProcThreadAttributeList(list); }
  } attrGuard{attrList};

  STARTUPINFOEXW siex{};
  siex.StartupInfo.cb = sizeof(siex);
  siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  siex.StartupInfo.hStdInput = stdIn;
  siex.StartupInfo.hStdOutput = stdOut;
  siex.StartupInfo.hStdError = stdErr;
  siex.lpAttributeList = attrList;

  const std::vector<wchar_t> envBlock = buildEnvBlock(tempDir, envOverrides);

  PROCESS_INFORMATION pi{};
  // CREATE_NEW_PROCESS_GROUP: makes the child the head of its own process
  // group (a confined child shares the host console).
  const DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT
                              | EXTENDED_STARTUPINFO_PRESENT
                              | (newProcessGroup ? CREATE_NEW_PROCESS_GROUP : 0);
  const BOOL created = CreateProcessAsUserW(
      token, nullptr, const_cast<LPWSTR>(commandLine.c_str()), nullptr, nullptr, TRUE,
      creationFlags, const_cast<wchar_t*>(envBlock.data()), cwd.empty() ? nullptr : cwd.c_str(),
      &siex.StartupInfo, &pi);

  if (!created) {
    throw std::runtime_error("CreateProcessAsUserW failed (Win32 " + std::to_string(GetLastError())
                             + ") for command: " + wideToUtf8(commandLine));
  }

  // The child is suspended and NOT yet in the kill-on-close job: any failure
  // from here must terminate it so it cannot hang or run unconfined.
  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    const DWORD err = GetLastError();
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw std::runtime_error("AssignProcessToJobObject failed (Win32 " + std::to_string(err) + ")");
  }
  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
    const DWORD err = GetLastError();
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw std::runtime_error("ResumeThread failed (Win32 " + std::to_string(err) + ")");
  }
  CloseHandle(pi.hThread);
  return SpawnedChild{pi.hProcess, pi.dwProcessId};
}

}  // namespace

SpawnedChild spawnSandboxedInherited(HANDLE token, HANDLE job, const std::wstring& commandLine,
                                     const std::wstring& cwd, bool newProcessGroup,
                                     const std::wstring* tempDir,
                                     const std::map<std::wstring, std::wstring>* envOverrides) {
  // Pass the caller's std handles straight through via STARTF_USESTDHANDLES.
  HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE stdErr = GetStdHandle(STD_ERROR_HANDLE);
  if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
      || stdOut == INVALID_HANDLE_VALUE || stdErr == nullptr || stdErr == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("GetStdHandle returned an invalid handle");
  }

  // Re-enable inheritance on those handles for the duration of the spawn.
  struct InheritGuard {
    HANDLE in, out, err;
    ~InheritGuard() {
      SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0);
    }
  };
  if (!SetHandleInformation(stdIn, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
      || !SetHandleInformation(stdOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
      || !SetHandleInformation(stdErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    throw std::runtime_error("SetHandleInformation(inherit) failed (Win32 "
                             + std::to_string(GetLastError()) + ")");
  }
  InheritGuard guard{stdIn, stdOut, stdErr};
  return spawnWithStdio(token, job, commandLine, cwd, newProcessGroup, tempDir, envOverrides,
                        stdIn, stdOut, stdErr);
}

SpawnedChild spawnSandboxedPiped(HANDLE token, HANDLE job, const std::wstring& commandLine,
                                 const std::wstring& cwd, bool newProcessGroup,
                                 const std::wstring* tempDir,
                                 const std::map<std::wstring, std::wstring>* envOverrides) {
  // Inheritable on creation: the child's end must survive CreateProcessAsUserW.
  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, inWrite = nullptr;
  HANDLE outRead = nullptr, outWrite = nullptr;
  HANDLE errRead = nullptr, errWrite = nullptr;
  auto closeAll = [&] {
    HANDLE all[] = {inRead, inWrite, outRead, outWrite, errRead, errWrite};
    for (HANDLE handle : all) {
      if (handle != nullptr) CloseHandle(handle);
    }
  };
  auto fail = [&](const char* what) {
    const DWORD err = GetLastError();
    closeAll();
    throw std::runtime_error(std::string(what) + " (Win32 " + std::to_string(err) + ")");
  };
  if (!CreatePipe(&inRead, &inWrite, &inheritable, 0)) fail("CreatePipe(stdin) failed");
  if (!CreatePipe(&outRead, &outWrite, &inheritable, 0)) fail("CreatePipe(stdout) failed");
  if (!CreatePipe(&errRead, &errWrite, &inheritable, 0)) fail("CreatePipe(stderr) failed");

  SpawnedChild child;
  try {
    child = spawnWithStdio(token, job, commandLine, cwd, newProcessGroup, tempDir, envOverrides,
                           inRead, outWrite, errWrite);
  } catch (...) {
    closeAll();
    throw;
  }

  // The child holds its ends now. Drop the parent's copies of those — a read
  // never sees EOF while a write end is still open — and clear the inherit
  // flag CreatePipe left on the surviving ends.
  CloseHandle(inRead);
  CloseHandle(outWrite);
  CloseHandle(errWrite);
  SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
  child.stdInWrite = inWrite;
  child.stdOutRead = outRead;
  child.stdErrRead = errRead;
  return child;
}

SpawnedChild spawnSandboxedConPTY(HANDLE token, HANDLE job, HPCON hpcon,
                                  const std::wstring& commandLine,
                                  const std::wstring& cwd, const std::wstring* tempDir,
                                  const std::map<std::wstring, std::wstring>* envOverrides) {
  // The attribute list carries exactly the PSEUDOCONSOLE attribute (lpValue is
  // the HPCON value itself, not its address).
  std::vector<uint8_t> attrBuf;
  buildAttributeList(attrBuf, hpcon, sizeof(HPCON), PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE);
  LPPROC_THREAD_ATTRIBUTE_LIST attrList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  struct AttrGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~AttrGuard() { DeleteProcThreadAttributeList(list); }
  } guard{attrList};

  // STARTF_USESTDHANDLES must still be set with NULL handles or the stdio copy
  // path is not activated and the pseudo console is ignored under
  // CreateProcessAsUserW (win-sandbox's empirical finding).
  STARTUPINFOEXW siex{};
  siex.StartupInfo.cb = sizeof(siex);
  siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  siex.lpAttributeList = attrList;

  // Explicit environment block with the private temp override (see
  // buildEnvBlock); lpEnvironment=NULL is unreliable for DLL initialization
  // under the restricted token + pseudo console.
  const std::vector<wchar_t> envBlock = buildEnvBlock(tempDir, envOverrides);

  PROCESS_INFORMATION pi{};
  // CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT; no
  // CREATE_NO_WINDOW (ConPTY hosts conhost headless) and no
  // CREATE_NEW_PROCESS_GROUP (the pseudo console forwards Ctrl+C itself).
  // NO CREATE_SUSPENDED: a suspended primary thread fails DLL initialization
  // under the pseudo console (STATUS_DLL_INIT_FAILED, 0xC0000142) — the job
  // assignment follows immediately after the spawn instead (win-sandbox's
  // ConPTY branch has the same shape).
  const BOOL created = CreateProcessAsUserW(
      token, nullptr, const_cast<LPWSTR>(commandLine.c_str()), nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
      const_cast<wchar_t*>(envBlock.data()), cwd.empty() ? nullptr : cwd.c_str(), &siex.StartupInfo, &pi);
  if (!created) {
    throw std::runtime_error("CreateProcessAsUserW(ConPTY) failed (Win32 "
                             + std::to_string(GetLastError()) + ")");
  }

  // Assign immediately after the spawn — the child may briefly run unconfined
  // before the assignment (the OJ-acceptable window win-sandbox documents).
  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    const DWORD err = GetLastError();
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw std::runtime_error("AssignProcessToJobObject failed (Win32 " + std::to_string(err) + ")");
  }
  CloseHandle(pi.hThread);
  return SpawnedChild{pi.hProcess, pi.dwProcessId};
}

}  // namespace winacl
