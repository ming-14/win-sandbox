"""win_sandbox - Windows 进程沙箱隔离（nanobind in-process 扩展）。

本包直接加载 win_sandbox_native.pyd 扩展（Stable ABI / abi3，Python 3.10+），
以 nanobind in-process 形态交互，无命名管道通信。

用法：
    import win_sandbox
    sb = win_sandbox.SandboxInstance()
    proc = sb.start_process(command_line="cmd.exe /c echo hello",
                            working_dir=os.getcwd())
    proc.wait()
    sb.shutdown()
"""

from __future__ import annotations

import os as _os
import sys as _sys

# 加载 nanobind 扩展：优先包内 _native/（wheel 安装），回退 build/bin/（开发态）
_native_dir = _os.path.join(_os.path.dirname(__file__), "_native")
if _os.path.isdir(_native_dir):
    _sys.path.insert(0, _native_dir)

from win_sandbox_native import SandboxInstance, Process  # noqa: E402

__version__ = "1.0.0"

__all__ = ["SandboxInstance", "Process", "__version__"]
