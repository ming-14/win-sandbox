// module.cpp — nanobind extension entry for the in-process sandbox.
//
// Exposes SandboxInstance and SandboxedProcess to Python via nanobind
// (stable-ABI-compatible binding layer). The C++ core
// (WRITE_RESTRICTED token + capability-SID write allowlist + Job resource
// limits) loads into the Python interpreter process, so handles (HPCON
// included) are shared directly — no IPC, no protocol lines.
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/list.h>
#include <map>

#include "instance.h"

namespace nb = nanobind;
namespace {

// ---------------------------------------------------------------------------
// PyProcess — Python wrapper over SandboxedProcess.
// ---------------------------------------------------------------------------
class PyProcess {
public:
  explicit PyProcess(std::shared_ptr<winacl::SandboxedProcess> process)
      : process_(std::move(process)) {}

  uint32_t pid() const { return process_->pid(); }

  nb::tuple wait() {
    // Release the GIL while blocking; the C++ side is pure Win32. Reacquire
    // before translating exceptions / building the result.
    nb::gil_scoped_release release;
    try {
      const auto [code, reason] = process_->wait();
      nb::gil_scoped_acquire acquire;
      return nb::make_tuple(code, reason);
    } catch (const std::exception& e) {
      nb::gil_scoped_acquire acquire;
      PyErr_SetString(PyExc_RuntimeError, e.what());
      throw nb::python_error();
    }
  }

  nb::object poll_exit() {
    nb::gil_scoped_release release;
    try {
      const auto result = process_->pollExit();
      nb::gil_scoped_acquire acquire;
      if (!result) return nb::none();
      return nb::make_tuple(result->first, result->second);
    } catch (const std::exception& e) {
      nb::gil_scoped_acquire acquire;
      PyErr_SetString(PyExc_RuntimeError, e.what());
      throw nb::python_error();
    }
  }

  void terminate(uint32_t exit_code = 1) {
    nb::gil_scoped_release release;
    process_->terminate(exit_code);
  }

  nb::list query_process_list() {
    std::vector<DWORD> pids;
    {
      nb::gil_scoped_release release;
      pids = process_->queryProcessList();
    }
    nb::list out;
    for (const DWORD pid : pids) out.append(pid);
    return out;
  }

  // The parent's end of the pipes (only when the run was spawned with
  // pipe_stdio=True; 0 otherwise). Returned as plain handle values: they belong
  // to the caller, which is expected to turn them into fds or close them.
  uint64_t stdin_handle() const { return handleValue(process_->stdInWrite()); }
  uint64_t stdout_handle() const { return handleValue(process_->stdOutRead()); }
  uint64_t stderr_handle() const { return handleValue(process_->stdErrRead()); }

private:
  static uint64_t handleValue(HANDLE handle) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  }

  std::shared_ptr<winacl::SandboxedProcess> process_;
};

// ---------------------------------------------------------------------------
// PySandboxInstance — Python wrapper over SandboxInstance.
// ---------------------------------------------------------------------------
class PySandboxInstance {
public:
  PySandboxInstance() = default;
  ~PySandboxInstance() {
    try {
      nb::gil_scoped_release release;
      instance_.shutdown();
    } catch (...) {
    }
  }

  PyProcess start_process(const std::string& command_line,
                          const std::string& working_dir,
                          bool workspace_write,
                          const nb::dict& quota,
                          nb::object hpcon,
                          const nb::dict& env,
                          bool pipe_stdio) {
    // Parse quota BEFORE releasing the GIL — the nb::dict must not be
    // touched from a thread that doesn't hold the GIL.
    winacl::ResourceLimits limits;
    parseQuota(quota, limits);
    HPCON conpty = nullptr;
    if (!hpcon.is_none()) {
      conpty = reinterpret_cast<HPCON>(nb::cast<uint64_t>(hpcon));
    }
    // Parse env overrides into a C++ map BEFORE releasing the GIL.
    std::map<std::wstring, std::wstring> envOverrides;
    if (!env.is_none()) {
      for (const auto& item : env) {
        auto key = nb::borrow(item.first);
        auto val = nb::borrow(item.second);
        envOverrides[winacl::utf8ToWide(nb::cast<std::string>(key))] =
            winacl::utf8ToWide(nb::cast<std::string>(val));
      }
    }
    std::shared_ptr<winacl::SandboxedProcess> process;
    {
      nb::gil_scoped_release release;
      try {
        process = instance_.startProcess(
            winacl::utf8ToWide(command_line), winacl::utf8ToWide(working_dir),
            workspace_write, limits, conpty,
            envOverrides.empty() ? nullptr : &envOverrides, pipe_stdio);
      } catch (const std::exception& e) {
        nb::gil_scoped_acquire acquire;
        PyErr_SetString(PyExc_RuntimeError, e.what());
        throw nb::python_error();
      }
    }
    return PyProcess(std::move(process));
  }

  void shutdown() {
    nb::gil_scoped_release release;
    instance_.shutdown();
  }

private:
  static void parseQuota(const nb::dict& quota, winacl::ResourceLimits& limits) {
    auto num = [&](const char* key, uint64_t& target) {
      if (quota.contains(key)) {
        target = nb::cast<uint64_t>(quota[key]);
      }
    };
    num("memory_mb", limits.memoryMb);
    num("job_memory_mb", limits.jobMemoryMb);
    num("cpu_ms", limits.cpuMs);
    num("wall_clock_timeout_ms", limits.wallClockMs);
    if (quota.contains("cpu_rate_percent"))
      limits.cpuRatePercent = nb::cast<uint32_t>(quota["cpu_rate_percent"]);
    if (quota.contains("max_processes"))
      limits.maxProcesses = nb::cast<uint32_t>(quota["max_processes"]);
    if (quota.contains("no_ui"))
      limits.noUi = nb::cast<bool>(quota["no_ui"]);
    if (quota.contains("crash_silent"))
      limits.crashSilent = nb::cast<bool>(quota["crash_silent"]);
    if (quota.contains("breakaway_ok"))
      limits.breakawayOk = nb::cast<bool>(quota["breakaway_ok"]);
  }

  winacl::SandboxInstance instance_;
};

}  // namespace

NB_MODULE(win_sandbox_native, m) {
  m.doc() = "in-process sandbox native extension: WRITE_RESTRICTED token + "
            "capability-SID write allowlist + Job resource limits";

  nb::class_<PyProcess>(m, "Process")
      .def(nb::init<std::shared_ptr<winacl::SandboxedProcess>>())
      .def_prop_ro("pid", &PyProcess::pid)
      .def("wait", &PyProcess::wait)
      .def("poll_exit", &PyProcess::poll_exit)
      .def("terminate", &PyProcess::terminate, nb::arg("exit_code") = 1)
      .def("query_process_list", &PyProcess::query_process_list)
      .def_prop_ro("stdin_handle", &PyProcess::stdin_handle)
      .def_prop_ro("stdout_handle", &PyProcess::stdout_handle)
      .def_prop_ro("stderr_handle", &PyProcess::stderr_handle);

  nb::class_<PySandboxInstance>(m, "SandboxInstance")
      .def(nb::init<>())
      .def("start_process", &PySandboxInstance::start_process,
           nb::arg("command_line"), nb::arg("working_dir"),
           nb::arg("workspace_write") = true, nb::arg("quota") = nb::dict(),
           nb::arg("hpcon") = nb::none(), nb::arg("env") = nb::dict(),
           nb::arg("pipe_stdio") = false)
      .def("shutdown", &PySandboxInstance::shutdown);
}
