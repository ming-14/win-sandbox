# win-sandbox

Windows 进程沙箱：`WRITE_RESTRICTED` 受限令牌 + capability-SID 写白名单 + Job 资源配额，
以 nanobind in-process 扩展（`win_sandbox_native.pyd`，abi3 / Python 3.12+）加载。

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

`start_process` 参数：`command_line`（整条命令行，原样交给
`CreateProcessAsUserW`）、`working_dir`（工作区根目录，必须存在）、
`workspace_write`（True = 工作区可写，False = 工作区只读）、`quota`（资源配额，
见下）、`hpcon`（外部 ConPTY 句柄，默认 None = 不用伪终端）、`env`（子进程环境变量，
默认空；**与宿主同名的项被替换掉**，宿主环境本身不动）、`pipe_stdio`（`hpcon` 为
None 时：True = 给子进程三条匿名管道，False = 让它继承本进程的标准句柄；两个都给
会报错）。

每次 spawn 都给子进程一个**私有的可写临时目录**（`TMP`/`TEMP` 指向它，进程退出即
回收），**只读档也有**：宿主临时根目录在写白名单之外，拿不到可写临时目录的进程
（DLL 初始化、解释器）根本起不来。两档的差别只有工作区本身。目录名固定为
`<宿主临时根>\winsandbox-<hex>`。

`Process` 接口：`pid`、`wait()` → `(exit_code, reason)`、`poll_exit()`（未结束返回
None）、`terminate(exit_code=1)`、`query_process_list()`，以及管道形态下的
`stdin_handle` / `stdout_handle` / `stderr_handle`。

### 管道 stdio

`pipe_stdio=True` 时子进程的 stdin/stdout/stderr 各走一条独立管道，父进程这一端从
`Process` 上取：`stdin_handle`（往里写）、`stdout_handle` / `stderr_handle`（从里读），
值就是裸句柄（`0` = 这次 spawn 没开管道）。**这三个句柄从 spawn 返回起归调用方所有**——
库不再关它们，调用方自己包成 fd 或 `CloseHandle`。子进程那一端库已经关掉，所以读端
能读到真的 EOF；关掉 `stdin_handle` 就是给子进程发 EOF。

```python
import msvcrt
import os

proc = sb.start_process(
    command_line="cmd /c findstr .",
    working_dir=os.getcwd(),
    pipe_stdio=True,
)
stdout_fd = msvcrt.open_osfhandle(proc.stdout_handle, os.O_RDONLY | os.O_BINARY)
stdin_fd = msvcrt.open_osfhandle(proc.stdin_handle, os.O_WRONLY | os.O_BINARY)
os.write(stdin_fd, b"hello\r\n")
os.close(stdin_fd)          # 发 EOF
print(os.read(stdout_fd, 4096))
```

句柄取一次就够：包成 fd 之后由 fd 负责关闭，别再从 `Process` 上读同一个属性——
句柄值会被系统复用，拿旧值去用是拿别人的。

`quota` 键（全部可选，0/缺省 = 不限）：`memory_mb`、`job_memory_mb`、`cpu_ms`、
`cpu_rate_percent`、`max_processes`、`wall_clock_timeout_ms`、`no_ui`、
`crash_silent`、`breakaway_ok`。

`reason` 取值：`normal` / `user` / `timeout` / `cpu_limit` / `memory_limit` /
`process_count_limit`。

## 示例

`examples/tk_console.py` —— Tk 控制台，把上面除 `pipe_stdio` 外的入参搬上界面，默认开
一个新 cmd 窗口跑命令。

```
python examples/tk_console.py
```

**必须用 `python`，不能用 `pythonw`**：面板走继承 stdio 路径，而 `pythonw` 是 GUI
子系统、没有控制台，那条路径会抛 `GetStdHandle returned an invalid handle`。面板
按设计只当遥控器、不接管标准句柄，所以不提供 `pipe_stdio` 开关；要用管道就直接调
`start_process(pipe_stdio=True)`。

## 构建

| 方式 | 命令 | 产物 |
|------|------|------|
| wheel | `pip wheel . -w dist`（scikit-build-core，构建机需 Python 3.12+） | `dist/win_sandbox-*.whl`（abi3，Python 3.12+） |
| 独立组装 | `python build.py` | `dist/win_sandbox/`（`_native/*.pyd` + Python 包装） |
| 隔离自测 | `python build.py --selftest` | `src/build/selftest.exe`（不经绑定层） |

`build.py` 按主机架构构建（x64 / arm64）。跨架构产物由 CI 矩阵产出：
x64 → `windows-latest`，arm64 → `windows-11-arm`。

wheel 由 scikit-build-core 按构建机架构产出，支持 x64 / arm64。

## 源码结构

```
src/
├── CMakeLists.txt    # nanobind 构建（唯一目标 win_sandbox_native.pyd）
├── winacl.h          # 共享定义：ResourceLimits / LimitKind / Job / SpawnedChild
├── token.cpp         # CreateRestrictedToken：DISABLE_MAX_PRIVILEGE|LUA_TOKEN|WRITE_RESTRICTED
├── acl.cpp           # grantWrite / revokeWrite（capability SID ACE）+ 宿主进程 DACL 加固
├── sid.cpp           # 能力 SID 派生（S-1-4-x-y，sha256 确定性）+ SHA-256 + UTF 转换
├── job.cpp           # Job Object：资源配额 + KILL_ON_CLOSE + IOCP 硬限终止
├── spawn.cpp         # CreateProcessAsUserW 受限 spawn：继承 stdio / 自建管道 / ConPTY
├── instance.cpp      # SandboxInstance / SandboxedProcess：授权物化、令牌、wait/terminate
├── module.cpp        # nanobind 绑定（SandboxInstance / Process）
├── selftest.cpp      # 独立 C++ 自测入口（build.py --selftest）
└── python/win_sandbox/__init__.py
examples/tk_console.py  # Tk 控制台示例
tests/smoke.py          # 跨架构冒烟测试（CI 与本地通用）
```

调用链：`module.cpp` 暴露 `SandboxInstance.start_process` → `instance.cpp` 物化
workspace/temp 授权（`acl.cpp`）→ 构建受限令牌（`token.cpp`，SID 派生自 `sid.cpp`）
→ `spawn.cpp` 以继承 stdio、自建管道或 ConPTY 启动 → 进程挂入 `job.cpp` 的 Job
（配额 + 树终止）。

宿主进程 DACL 加固（`acl.cpp` `hardenHostProcessDacl`）在每次 spawn 前执行：拒绝沙箱
restricting SIDs 对宿主的进程写权限（含 `PROCESS_TERMINATE`）。

## 由来

沙箱模型参照 `@deepseek-ai/dsh-sandbox-windows-acl` 的实现（受限令牌 + capability-SID
写白名单 + Job 资源配额 + 私有 temp 的 `TMP`/`TEMP` 重定向）。这里是一套独立的原生
C++ 重写（nanobind 绑定），不复用其源码，也不与其共享锁约定；能力 SID 的派生规则保持
冻结，以便复用磁盘上既有的 ACE。

## License

MIT，见 `License`。
