"""win_sandbox 冒烟测试（CI 用；x64 / arm64 通用）

覆盖点：
  - 包可导入、扩展模块可加载（本地为 in-process nanobind 扩展）
  - SandboxInstance.start_process 能启动真实进程并正常 wait
  - 进程树查询 / poll_exit / terminate 基本命令可用
  - 写边界：可写档拿到可写的 winsandbox-* 私有 temp；只读档没有私有 temp，
    TMP/TEMP 保持宿主值且写不进去

退出码：0 = 全部通过；非 0 = 失败（任意断言错误即失败）
"""

from __future__ import annotations

import msvcrt
import os
import sys
import tempfile
import threading

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

    # ── 4. 写边界：可写档有私有 temp，只读档什么都没有 ──
    # 只读档刻意不建私有 temp、也不覆盖 TMP/TEMP；钉住这个设计，免得哪天又
    # 「顺手」给只读档补个可写目录
    def drain(handle: int) -> str:
        """把一条管道读到 EOF，返回文本（句柄归调用方，读完即关）"""
        if not handle:
            return ""
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        chunks: list[bytes] = []
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            chunks.append(chunk)
        os.close(fd)
        return b"".join(chunks).decode("utf-8", "replace").strip()

    def run_piped(command_line: str, workspace_write: bool, cwd: str) -> tuple[str, int, str]:
        """跑一条命令，返回 (stdout, 退出码, stderr)

        两条管道串行读会在子进程写满缓冲时互相卡住，所以 stderr 交给另一个线程
        """
        proc = sb.start_process(
            command_line=command_line,
            working_dir=cwd,
            workspace_write=workspace_write,
            pipe_stdio=True,
        )
        box: list[str] = []
        watcher = threading.Thread(target=lambda: box.append(drain(proc.stderr_handle)))
        watcher.start()
        text = drain(proc.stdout_handle)
        watcher.join()
        code, _ = proc.wait()
        return text, code, box[0] if box else ""

    # 第 2 步的 workdir 已经删了，这里另开一个：只用它当 cwd，不靠它可写
    workdir2 = tempfile.mkdtemp(prefix="ws_smoke_boundary_")
    try:
        echo_temp = 'cmd /c "echo %TEMP%"'
        writable_temp, code_w, err_w = run_piped(echo_temp, True, workdir2)
        read_only_temp, code_r, err_r = run_piped(echo_temp, False, workdir2)
        print(f"[smoke] writable TMP  = {writable_temp} (exit {code_w})")
        print(f"[smoke] read-only TMP = {read_only_temp} (exit {code_r})")
        assert code_w == 0 and code_r == 0, (
            f"echo %TEMP% failed: {code_w} / {code_r} (stderr {err_w!r} / {err_r!r})")
        assert os.path.basename(writable_temp).startswith("winsandbox-"), \
            f"writable run should get a private temp: {writable_temp}"
        assert not os.path.basename(read_only_temp).startswith("winsandbox-"), \
            f"read-only run must not get a private temp: {read_only_temp}"

        # 只读档连宿主临时根都写不进去（写白名单为空）
        probe = os.path.join(read_only_temp.rstrip("\\"), "ws_smoke_ro.txt")
        verdict, _, _ = run_piped(
            'cmd /c "echo x > %TEMP%\\ws_smoke_ro.txt && echo WROTE || echo DENIED"',
            False, workdir2)
        print(f"[smoke] read-only write to TEMP -> {verdict}")
        assert verdict.endswith("DENIED"), f"read-only run should not write at all: {verdict}"
        assert not os.path.exists(probe), f"read-only run created {probe}"

        # 但只读档仍须能创建自己的内核对象（匿名管道、事件）：靠令牌默认 DACL
        # 授予 logon SID——文件写权限全关，对象创建照旧
        obj_probe = os.path.join(workdir2, "ws_smoke_obj.py")
        with open(obj_probe, "w", encoding="utf-8") as f:
            f.write("import os\nos.pipe()\nprint(\"PIPE_OK\")\n")
        obj_out, obj_code, obj_err = run_piped(
            f'"{sys.executable}" "{obj_probe}"', False, workdir2)
        print(f"[smoke] read-only os.pipe -> {obj_out} (exit {obj_code})")
        if obj_code != 0:
            print(f"[diag] stderr={obj_err!r}")
            print(f"[diag] cwd={workdir2} exe={sys.executable} probe={obj_probe}")
            diag = [
                ("py -V", f'"{sys.executable}" -V'),
                ("py -c", f'"{sys.executable}" -c "print(1)"'),
                ("py -B script", f'"{sys.executable}" -B "{obj_probe}"'),
                ("py -I -S script", f'"{sys.executable}" -I -S "{obj_probe}"'),
                ("type script", f'cmd /c "type {obj_probe}"'),
                ("env", 'cmd /c "echo TMP=%TMP% TEMP=%TEMP% USERPROFILE=%USERPROFILE%"'),
            ]
            for label, cmd in diag:
                o, c, e = run_piped(cmd, False, workdir2)
                print(f"[diag] {label}: exit={c} out={o!r} err={e!r}")
        assert obj_code == 0 and "PIPE_OK" in obj_out, (
            f"read-only run must still create its own objects: {obj_out!r} "
            f"(exit {obj_code}, stderr {obj_err!r})")
    finally:
        try:
            os.rmdir(workdir2)
        except OSError:
            pass

    # ── 5. 收尾 ──
    sb.shutdown()
    print("[smoke] ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
