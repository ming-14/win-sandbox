# win-sandbox

Windows 进程沙箱：`WRITE_RESTRICTED` 受限令牌 + capability-SID 写白名单 + Job 资源配额，
以 nanobind in-process 扩展（`win_sandbox_native.pyd`，abi3 / Python 3.10+）加载。

## 用法

```python
import os
import win_sandbox

sb = win_sandbox.SandboxInstance()
proc = sb.start_process(
    command_line="cmd /c echo hello",
    working_dir=os.getcwd(),
    workspace_write=True,
    quota={"wall_clock_timeout_ms": 30_000, "no_ui": True},
)
code, reason = proc.wait()
sb.shutdown()
```

`start_process` 参数：`command_line`（整条命令行）、`working_dir`（工作区根目录，
必须存在）、`workspace_write`（True = 可写，False = 只读）、`quota`（资源配额，
见下）、`hpcon`（外部 ConPTY 句柄，默认 None = 继承 stdio）。

`Process` 接口：`pid`、`wait()` → `(exit_code, reason)`、`poll_exit()`（未结束返回
None）、`terminate(exit_code=1)`、`query_process_list()`。

`quota` 键（全部可选，0/缺省 = 不限）：`memory_mb`、`job_memory_mb`、`cpu_ms`、
`cpu_rate_percent`、`max_processes`、`wall_clock_timeout_ms`、`no_ui`、
`crash_silent`、`breakaway_ok`。

`reason` 取值：`normal` / `user` / `timeout` / `cpu_limit` / `memory_limit` /
`process_count_limit`。

## 构建

| 方式 | 命令 | 产物 |
|------|------|------|
| wheel | `pip wheel . -w dist`（scikit-build-core） | `dist/win_sandbox-*.whl`（abi3，Python 3.10+） |
| 独立组装 | `python build.py` | `dist/win_sandbox/`（`_native/*.pyd` + Python 包装） |
| 隔离自测 | `python build.py --selftest` | `src/build/selftest.exe`（不经绑定层） |

`build.py` 按主机架构构建（x64 / arm64）。跨架构产物由 CI 矩阵产出：
x64 → `windows-latest`，arm64 → `windows-11-arm`。

wheel 由 scikit-build-core 按构建机架构产出，支持 x64 / arm64。

## 源码结构

```
src/
├── CMakeLists.txt    # nanobind 构建（唯一目标 win_sandbox_native.pyd）
├── winacl.h          # 共享定义：Mode / ResourceLimits / LimitKind / Job / SpawnedChild
├── token.cpp         # CreateRestrictedToken：DISABLE_MAX_PRIVILEGE|LUA_TOKEN|WRITE_RESTRICTED
├── acl.cpp           # grantWrite / revokeWrite（capability SID ACE）+ 宿主进程 DACL 加固
├── sid.cpp           # 能力 SID 派生（S-1-4-x-y，sha256 确定性）+ SHA-256 + UTF 转换
├── job.cpp           # Job Object：资源配额 + KILL_ON_CLOSE + IOCP 硬限终止
├── spawn.cpp         # CreateProcessAsUserW 受限 spawn：ConPTY 与继承 stdio 两路
├── instance.cpp      # SandboxInstance / SandboxedProcess：授权物化、令牌、wait/terminate
├── module.cpp        # nanobind 绑定（SandboxInstance / Process）
├── selftest.cpp      # 独立 C++ 自测入口（build.py --selftest）
└── python/win_sandbox/__init__.py
tests/smoke.py        # 跨架构冒烟测试（CI 与本地通用）
```

调用链：`module.cpp` 暴露 `SandboxInstance.start_process` → `instance.cpp` 物化
workspace/temp 授权（`acl.cpp`）→ 构建受限令牌（`token.cpp`，SID 派生自 `sid.cpp`）
→ `spawn.cpp` 以 ConPTY 或继承 stdio 启动 → 进程挂入 `job.cpp` 的 Job（配额 + 树终止）。

宿主进程 DACL 加固（`acl.cpp` `hardenHostProcessDacl`）在每次 spawn 前执行：拒绝沙箱
restricting SIDs 对宿主的进程写权限（含 `PROCESS_TERMINATE`）。

## License

MIT，见 `License`。
