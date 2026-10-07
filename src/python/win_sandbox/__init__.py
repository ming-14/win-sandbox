"""win_sandbox — Windows 进程沙箱（nanobind in-process 扩展，abi3 / Python 3.12+）

直接加载 win_sandbox_native.pyd，无命名管道通信。用法见 README
"""

from __future__ import annotations

import os as _os
import sys as _sys

# nanobind 扩展就在包内 _native/（wheel 与 build.py 都是这个布局）
_native_dir = _os.path.join(_os.path.dirname(__file__), "_native")
if _os.path.isdir(_native_dir):
    _sys.path.insert(0, _native_dir)

from win_sandbox_native import SandboxInstance, Process  # noqa: E402

__version__ = "1.1.0"

__all__ = ["SandboxInstance", "Process", "__version__"]
