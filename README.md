# win-sandbox

Windows 进程沙箱：`WRITE_RESTRICTED` 受限令牌 + capability-SID 写白名单 + Job 资源配额，
以 nanobind in-process 扩展（`win_sandbox_native.pyd`，abi3 / Python 3.12+）加载

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

### `start_process` 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `command_line` | — | 整条命令行，原样交给 `CreateProcessAsUserW` |
| `working_dir` | — | 工作区根目录，必须已存在 |
| `workspace_write` | `True` | `False` = 只读档，见「写边界」 |
| `quota` | `{}` | 资源配额，键见下 |
| `hpcon` | `None` | 外部 ConPTY 句柄；`None` = 不用伪终端 |
| `env` | `{}` | 子进程环境变量，**同名项替换宿主值**，宿主环境不动 |
| `pipe_stdio` | `False` | `hpcon` 为 `None` 时：`True` = 三条匿名管道，`False` = 继承本进程标准句柄。与 `hpcon` 同给会报错 |

### 写边界

**可写档**（`workspace_write=True`）：工作区可写；另外每次 spawn 给子进程一个私有可写
临时目录（`TMP`/`TEMP` 指向它，退出即回收），名为 `<宿主临时根>\winsandbox-<hex>`。两者的
capability SID 一起构成写白名单

**只读档**（`workspace_write=False`）：不建私有 temp、不覆盖 `TMP`/`TEMP`、写白名单为空——
子进程在宿主临时根下也写不进去；需要可写临时目录的程序（解释器、部分 DLL 初始化路径）
可能起不来

两档的子进程都仍能创建自己的内核对象（匿名管道、事件），否则 DLL 初始化直接失败：令牌
默认 DACL 分别授予本次 run 的 temp SID / 子进程登录会话的 logon SID。这一项与文件系统
写权限无关

读权限不归沙箱管：沙箱只收紧写，读全看宿主环境 ACL，工作区必须对子进程自己持有的 SID
可读。宿主被提升时（CI runner、以管理员身份跑的宿主），新建目录的 owner 是
Administrators，ACE 也可能只落 `Administrators`/`SYSTEM`，而受限令牌的 LUA 语义把
Administrators 降成 deny-only，子进程连读都会失败；给工作区补一条用户本人或 `Everyone`
的读 ACE 即可

`Process` 接口：`pid`、`wait()` → `(exit_code, reason)`、`poll_exit()`（未结束为 `None`）、
`terminate(exit_code=1)`、`query_process_list()`，以及管道形态下的 `stdin_handle` /
`stdout_handle` / `stderr_handle`

### 管道 stdio

`pipe_stdio=True` 时三条管道各给一个裸句柄（`0` = 本次没开管道）：`stdin_handle` 往里写，
`stdout_handle` / `stderr_handle` 从里读。**句柄从 spawn 返回起归调用方所有**，库不再关
它们；子进程那一端库已关掉，所以读端能读到真 EOF，关掉 `stdin_handle` 就是发 EOF

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

句柄取一次就够：包成 fd 之后由 fd 负责关闭，别重复读同一个属性——句柄值会被系统复用

`quota` 键（全部可选，`0`/缺省 = 不限）：`memory_mb`、`job_memory_mb`、`cpu_ms`、
`cpu_rate_percent`、`max_processes`、`wall_clock_timeout_ms`、`no_ui`、`crash_silent`、
`breakaway_ok`

`reason` 取值：`normal` / `user` / `timeout` / `cpu_limit` / `memory_limit` /
`process_count_limit`

## 示例

`examples/tk_console.py`：Tk 控制台，把除 `pipe_stdio` 外的入参搬上界面

```
python examples/tk_console.py
```

**必须用 `python`，不能用 `pythonw`**：面板走继承 stdio 路径，而 `pythonw` 是 GUI 子系统、
没有控制台，那条路径会抛 `GetStdHandle returned an invalid handle`。面板按设计只当遥控器、
不接管标准句柄，所以不提供 `pipe_stdio` 开关

## 构建

| 方式 | 命令 | 产物 |
|------|------|------|
| wheel | `pip wheel . -w dist`（scikit-build-core，构建机需 Python 3.12+） | `dist/win_sandbox-*.whl`（abi3，Python 3.12+） |
| 独立组装 | `python build.py` | `dist/win_sandbox/`（`_native/*.pyd` + Python 包装） |
| 隔离自测 | `python build.py --selftest` | `build/selftest.exe`（不经绑定层） |

`build.py` 按主机架构构建（x64 / arm64）；跨架构产物由 CI 矩阵产出，x64 → `windows-latest`，
arm64 → `windows-11-arm`

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

调用链：`module.cpp` → `instance.cpp`（物化 workspace/temp 授权，`acl.cpp`）→ `token.cpp`
（受限令牌，SID 派生自 `sid.cpp`）→ `spawn.cpp`（启动）→ `job.cpp`（配额 + 树终止）。
宿主进程 DACL 加固（`hardenHostProcessDacl`）在每次 spawn 前执行：拒绝沙箱 restricting
SIDs 对宿主的进程写权限（含 `PROCESS_TERMINATE`）

## 由来

沙箱模型参照 `@deepseek-ai/dsh-sandbox-windows-acl` 的实现（受限令牌 + capability-SID
写白名单 + Job 资源配额 + 私有 temp 的 `TMP`/`TEMP` 重定向）。这里是一套独立的原生
C++ 重写（nanobind 绑定），不复用其源码，也不与其共享锁约定；能力 SID 的派生规则保持
冻结，以便复用磁盘上既有的 ACE

## License

MIT，见 `License`
