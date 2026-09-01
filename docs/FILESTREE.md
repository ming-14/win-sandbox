# sandbox/ 目录树

> C++ 原生沙箱工程（nanobind 编译为 `win_sandbox_native.pyd`，Windows 专属，Stable ABI）

```
sandbox/
├── build.py                  # 独立构建脚本（CMake + Ninja；--selftest 隔离自测）
├── pyproject.toml            # wheel 打包（scikit-build-core；split mode → abi3，Python 3.10+）
├── src/                       # ═══════ C++ 核心（WRITE_RESTRICTED 受限令牌 + Job） ═══════
│   ├── CMakeLists.txt         # nanobind 构建（唯一目标 win_sandbox_native.pyd；vcvars + Ninja）
│   ├── winacl.h               # 共享定义：Mode/ResourceLimits/LimitKind/Job/SpawnedChild + 函数声明
│   ├── token.cpp              # CreateRestrictedToken：LUA_TOKEN|WRITE_RESTRICTED，restricting SIDs 构建
│   ├── acl.cpp                # DACL 操作：grantWrite/revokeWrite（capability SID ACE）+ 宿主进程加固
│   ├── sid.cpp                # 能力 SID 派生（workspace/temp，S-1-4-x-y，sha256 确定性）+ SHA-256
│   ├── job.cpp                # Job Object：资源配额（内存/CPU/进程数/墙钟）+ KILL_ON_CLOSE + IOCP 通知
│   ├── spawn.cpp              # CreateProcessAsUserW 受限 spawn：ConPTY（HPCON）与继承 stdio 两路
│   ├── instance.cpp           # SandboxInstance/SandboxedProcess：授权物化、令牌、spawn、wait/terminate/查询
│   ├── module.cpp             # nanobind 绑定（SandboxInstance / Process，GIL 桥接回调）
│   ├── selftest.cpp           # 独立 C++ 自测入口（不经绑定层，隔离崩溃；build.py --selftest 编译运行）
│   └── python/win_sandbox/    # Python 包装（__init__.py / helpers.py / exceptions.py；wheel 与独立构建共用）
└── docs/
    └── FILESTREE.md           # 本文件
```

## 构建产物（gitignore 忽略，不列出）

- `src/build/` —— CMake/Ninja 构建目录
- `dist/` —— `python build.py` 组装的可分发包（`dist/win_sandbox/`）与 `pip wheel` 产物（`.whl`）
- `*.pyd` / `*.obj` / `*.exe` —— 编译中间产物

## 构建方式

| 方式 | 命令 | 产物 |
|------|------|------|
| wheel | `pip wheel . -w dist`（scikit-build-core） | `dist/win_sandbox-*.whl`（abi3，Python 3.10+） |
| 独立组装 | `python build.py` | `dist/win_sandbox/`（含 `_native/*.pyd` + Python 包装） |
| 隔离自测 | `python build.py --selftest` | `src/selftest.exe`（不经绑定层） |

绑定层为 **nanobind split mode**（`BACKEND_MODULE nanobind_backend`）：扩展模块以 Stable ABI
（`Py_LIMITED_API`）编译，wheel 标记 `cp310-abi3`，跨 Python 3.10~3.x 版本直接使用；
运行期需 `nanobind-backend` pip 包（pyproject.toml 已声明依赖）。

## 模块关系

- `module.cpp` 暴露 `SandboxInstance.start_process` → `instance.cpp` 物化 workspace/temp 授权（`acl.cpp`）→ 构建受限令牌（`token.cpp`，SID 派生自 `sid.cpp`）→ `spawn.cpp` 以 ConPTY 或继承 stdio 启动 → 进程挂入 `job.cpp` 的 Job（配额 + 树终止）
- 宿主进程 DACL 加固（`acl.cpp` `hardenHostProcessDacl`）在每次 spawn 前执行：拒绝沙箱 restricting SIDs 对宿主的进程写权限（含 PROCESS_TERMINATE）
