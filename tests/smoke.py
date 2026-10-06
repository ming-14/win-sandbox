"""win_sandbox 冒烟测试（CI 用；x64 / arm64 通用）。

覆盖点：
  - 包可导入、扩展模块可加载（本地为 in-process nanobind 扩展）
  - SandboxInstance.start_process 能启动真实进程并正常 wait
  - 进程树查询 / poll_exit / terminate 基本命令可用

退出码：0 = 全部通过；非 0 = 失败（任意断言错误即失败）。
"""

from __future__ import annotations

import os
import sys
import tempfile

import win_sandbox


def main() -> int:
    # ── 1. 包/扩展加载 ──
    print(f"[smoke] win_sandbox {win_sandbox.__version__}")
    print(f"[smoke] python {sys.version.split()[0]} on {sys.platform}")
    sb = win_sandbox.SandboxInstance()
    print("[smoke] SandboxInstance created")

    # ── 2. 启动子进程（workspace-write，用临时目录当工作区） ──
    workdir = tempfile.mkdtemp(prefix="ws_smoke_")
    try:
        proc = sb.start_process(
            command_line="cmd /c echo smoke-ok",
            working_dir=workdir,
            workspace_write=True,
            quota={"no_ui": True},
        )
        print(f"[smoke] process started pid={proc.pid}")

        # 进程列表应包含刚启动的进程
        pids = proc.query_process_list()
        print(f"[smoke] process list = {pids}")
        assert pids, "process list empty"

        # 等待结束（cmd 会很快退出）
        code, reason = proc.wait()
        print(f"[smoke] wait -> code={code} reason={reason}")
        assert code == 0, f"unexpected exit code: {code}"
        assert reason in ("normal", "ended"), f"unexpected reason: {reason}"

        # 结束后 poll_exit 返回退出码
        polled = proc.poll_exit()
        print(f"[smoke] poll_exit -> {polled}")
        assert polled is not None and polled[0] == 0, f"poll_exit: {polled}"
    finally:
        try:
            os.rmdir(workdir)
        except OSError:
            pass

    # ── 3. terminate 命令路径（启动长驻进程再杀） ──
    proc2 = sb.start_process(
        command_line="cmd /c ping -n 30 127.0.0.1 >nul",
        working_dir=tempfile.gettempdir(),
        workspace_write=False,
    )
    print(f"[smoke] long process started pid={proc2.pid}")
    proc2.terminate(1)
    code2, reason2 = proc2.wait()
    print(f"[smoke] terminate -> code={code2} reason={reason2}")
    assert reason2 == "user", f"terminate reason: {reason2}"

    # ── 4. 收尾 ──
    sb.shutdown()
    print("[smoke] ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
