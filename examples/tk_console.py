"""win_sandbox 的 Tk 控制台。

设计取向：**面板的主要用途是看日志、偶尔调一个参数**，所以不给 15 个参数
各摆一个常驻输入框。界面只列"与默认值不同"的覆盖项，加/改参数走弹窗，
省下的面积全给日志。

面板只当遥控器——真正交互的 cmd 跑在新窗口里，面板不接管标准句柄。

关于默认命令 `cmd.exe /c start /wait cmd.exe /K`：
  - `/wait` 让会话等那个窗口关掉，`/K` 让新窗口停在提示符。
  - 不能写 `start "标题" cmd`：命令行是原样交给 CreateProcessAsUserW 的，
    cmd 见到带引号的首参就当窗口标题，后面的程序名反而成了标题。
  - 新窗口里的 cmd 带受限令牌，且仍在同一个 Job 里。Job 是 KILL_ON_CLOSE，
    所以关面板前必须先 terminate，否则整棵树跟着走。

用法（必须用 python，不能用 pythonw）：

    python examples/tk_console.py

start_process 走继承 stdio 路径，要求面板自己有有效标准句柄；pythonw 是
GUI 子系统没有控制台，那条路径会抛 "GetStdHandle returned an invalid handle"。
"""

from __future__ import annotations

import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

_ROOT = Path(__file__).resolve().parent.parent
if (_ROOT / "dist" / "win_sandbox").is_dir():
    sys.path.insert(0, str(_ROOT / "dist"))

import win_sandbox  # noqa: E402

DEFAULT_COMMAND = "cmd.exe /c start /wait cmd.exe /K"
DEFAULT_WORKSPACE = str(Path.home())


# ── 参数规格表 ──────────────────────────────────────────────────────────
# 界面完全由这张表驱动：加参数不用改任何界面代码。key 以 "quota." 开头的
# 归入 start_process 的 quota dict，其余是直接入参。
# kind: text | int | bool；optional=False 的常驻显示且不可移除。
@dataclass(frozen=True)
class Param:
    key: str
    label: str
    kind: str
    optional: bool = True
    minimum: int | None = None
    maximum: int | None = None
    default: object = None
    hint: str = ""


PARAMS: tuple[Param, ...] = (
    Param("working_dir", "工作区（可写根）", "text", optional=False),
    Param("hpcon", "外部伪控制台 HPCON", "text", hint="十进制；留空 = 不用伪控制台"),
    Param("quota.memory_mb", "单进程内存", "int", minimum=0, hint="MB"),
    Param("quota.job_memory_mb", "Job 总内存", "int", minimum=0, hint="MB"),
    Param("quota.cpu_ms", "Job CPU 时间", "int", minimum=0, hint="毫秒"),
    Param("quota.cpu_rate_percent", "CPU 上限", "int", minimum=1, maximum=100,
          hint="百分比 1-100"),
    Param("quota.max_processes", "最大进程数", "int", minimum=1),
    Param("quota.wall_clock_timeout_ms", "墙钟超时", "int", minimum=1, hint="毫秒"),
    Param("quota.no_ui", "禁用 UI 能力", "bool", default=False,
          hint="剪贴板 / 系统参数 / 显示设置"),
    Param("quota.crash_silent", "崩溃静默", "bool", default=False,
          hint="DIE_ON_UNHANDLED_EXCEPTION"),
    Param("quota.breakaway_ok", "允许脱离 Job", "bool", default=False),
)

PARAM_BY_KEY = {p.key: p for p in PARAMS}


def coerce(param: Param, raw):
    """界面原始值 → API 需要的类型；非法就抛 ValueError。"""
    if param.kind == "bool":
        return bool(raw)
    text = str(raw).strip()
    if param.kind != "int":
        return text
    if not text:
        raise ValueError("不能留空")
    try:
        value = int(text)
    except ValueError:
        raise ValueError(f"不是整数：{text!r}") from None
    if param.minimum is not None and value < param.minimum:
        raise ValueError(f"不能小于 {param.minimum}")
    if param.maximum is not None and value > param.maximum:
        raise ValueError(f"不能大于 {param.maximum}")
    return value


# ═══════════════════════════════════════════════════════════════════════
# 逻辑层（与 Tk 无关，可 headless 测试）
# ═══════════════════════════════════════════════════════════════════════


@dataclass
class Overrides:
    """界面上"与默认不同"的项。命令与可写开关是常驻项。"""

    command_line: str = DEFAULT_COMMAND
    workspace_write: bool = True
    values: dict = field(default_factory=dict)   # 参数 key → 已强转的值
    env: dict = field(default_factory=dict)      # 环境变量覆盖

    def workspace(self) -> str:
        return str(self.values.get("working_dir", DEFAULT_WORKSPACE))

    def quota(self) -> dict:
        return {k.split(".", 1)[1]: v for k, v in self.values.items()
                if k.startswith("quota.")}

    def arguments(self) -> dict:
        """start_process 的位置/关键字参数（不含 quota、env）。"""
        out: dict = {
            "command_line": self.command_line,
            "working_dir": self.workspace(),
            "workspace_write": self.workspace_write,
        }
        hpcon = str(self.values.get("hpcon", "") or "").strip()
        if hpcon:
            out["hpcon"] = int(hpcon)
        return out


class Session:
    """一次沙箱会话：spawn → 退出监视 → 进程树查询。"""

    def __init__(self, emit: Callable[[str], None]) -> None:
        self._emit = emit
        self._sb: win_sandbox.SandboxInstance | None = None
        self._proc = None
        self._watch: threading.Thread | None = None
        self._watching = False

    @property
    def pid(self) -> int | None:
        return None if self._proc is None else self._proc.pid

    @property
    def running(self) -> bool:
        if self._proc is None:
            return False
        try:
            return self._proc.poll_exit() is None
        except RuntimeError:
            return False  # 已 dispose

    def tree(self) -> list[int]:
        """Job 内全部 pid（新窗口的 cmd 与它的子进程都在）。"""
        if not self.running:
            return []
        try:
            return self._proc.query_process_list()
        except RuntimeError:
            return []

    def start(self, cfg: Overrides) -> None:
        if self._proc is not None:
            raise RuntimeError("已有会话，先「关闭实例」")
        if not Path(cfg.workspace()).is_dir():
            raise FileNotFoundError(f"工作区不存在：{cfg.workspace()}")

        self._sb = win_sandbox.SandboxInstance()
        try:
            self._proc = self._sb.start_process(
                quota=cfg.quota(),
                env=cfg.env,
                **cfg.arguments(),
            )
        except BaseException:
            # fail-closed：起不来就拆实例，不留半截会话
            self._sb.shutdown()
            self._sb = None
            raise

        self._emit(f"已启动 pid={self._proc.pid}")
        for key, value in sorted(cfg.values.items()):
            self._emit(f"    {key} = {value}")
        if cfg.env:
            self._emit(f"    env = {cfg.env}")
        self._watch = threading.Thread(target=self._watch_exit, name="watchdog", daemon=True)
        self._watch.start()

    def terminate(self) -> None:
        """杀整棵进程树（Job 内全部进程），新窗口一起关。"""
        if self._proc is None:
            return
        try:
            self._proc.terminate(1)
        except RuntimeError:
            pass  # 已退出

    def close(self) -> None:
        """终止会话并关闭沙箱实例。幂等。"""
        self.terminate()
        if self._watch is not None:
            self._watch.join(timeout=3.0)
            self._watch = None
        self._watching = False  # 允许关掉后再开一个新会话
        if self._sb is not None:
            self._sb.shutdown()
            self._sb = None
        self._proc = None

    def _watch_exit(self) -> None:
        proc = self._proc
        if proc is None or self._watching:
            return
        self._watching = True
        try:
            code, reason = proc.wait()
        except RuntimeError:
            return
        self._emit(f"会话结束 code={code} reason={reason}")


# ═══════════════════════════════════════════════════════════════════════
# 界面层
# ═══════════════════════════════════════════════════════════════════════


class ParamDialog:
    """单个参数的编辑弹窗：输入即校验，非法值存不进来。"""

    def __init__(self, parent, param: Param, current=None) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.param = param
        self.result = None

        self.top = tk.Toplevel(parent)
        self.top.title(f"{param.label}   [{param.key}]")
        self.top.resizable(False, False)
        self.top.transient(parent)
        self.top.grab_set()

        body = ttk.Frame(self.top, padding=12)
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=1)

        if param.hint:
            ttk.Label(body, text=param.hint, foreground="#666").grid(row=0, column=0, sticky="w")

        if param.kind == "bool":
            self._var = tk.BooleanVar(value=bool(current))
            ttk.Checkbutton(body, text="启用", variable=self._var).grid(
                row=1, column=0, sticky="w", pady=(8, 0)
            )
        else:
            self._var = tk.StringVar(value="" if current is None else str(current))
            entry = ttk.Entry(body, textvariable=self._var, width=44)
            entry.grid(row=1, column=0, sticky="we", pady=(8, 0))
            entry.focus_set()
            entry.select_range(0, "end")
            self._var.trace_add("write", lambda *_: self._validate())

        self._hint = ttk.Label(body, text="", foreground="#c0392b")
        self._hint.grid(row=2, column=0, sticky="w", pady=(6, 0))

        buttons = ttk.Frame(body)
        buttons.grid(row=3, column=0, sticky="e", pady=(12, 0))
        self._ok = ttk.Button(buttons, text="确定", command=self._accept)
        ttk.Button(buttons, text="取消", command=self.top.destroy).pack(side="right", padx=(6, 0))
        self._ok.pack(side="right")

        self.top.bind("<Return>", lambda _e: self._accept())
        self.top.bind("<Escape>", lambda _e: self.top.destroy())
        self._validate()

    def _validate(self) -> bool:
        error = None
        if self.param.kind != "bool":
            try:
                coerce(self.param, self._var.get())
            except ValueError as exc:
                error = str(exc)
        self._hint.configure(text=error or "")
        self._ok.configure(state="disabled" if error else "normal")
        return error is None

    def _accept(self) -> None:
        if not self._validate():
            return
        self.result = coerce(self.param, self._var.get())
        self.top.destroy()

    def show(self):
        self.top.wait_window()
        return self.result


class EnvDialog:
    """环境变量覆盖：每行 KEY=VALUE，实时校验。"""

    def __init__(self, parent, current: dict) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.result = None
        self.top = tk.Toplevel(parent)
        self.top.title("环境变量覆盖")
        self.top.transient(parent)
        self.top.grab_set()

        body = ttk.Frame(self.top, padding=12)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="每行一条 KEY=VALUE，# 开头为注释").pack(anchor="w")

        self.text = tk.Text(body, width=52, height=8, wrap="none")
        self.text.pack(fill="both", expand=True, pady=(6, 0))
        self.text.insert("1.0", "\n".join(f"{k}={v}" for k, v in current.items()))

        self._hint = ttk.Label(body, text="", foreground="#c0392b")
        self._hint.pack(anchor="w", pady=(6, 0))

        buttons = ttk.Frame(body)
        buttons.pack(anchor="e", pady=(10, 0))
        self._ok = ttk.Button(buttons, text="确定", command=self._accept)
        ttk.Button(buttons, text="取消", command=self.top.destroy).pack(side="right", padx=(6, 0))
        self._ok.pack(side="right")

        self.top.bind("<Escape>", lambda _e: self.top.destroy())
        self.text.bind("<KeyRelease>", lambda _e: self._validate())
        self._validate()

    def _parse(self) -> dict:
        env: dict = {}
        for line in self.text.get("1.0", "end").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ValueError(f"缺少 = ：{line!r}")
            name, _, value = line.partition("=")
            name = name.strip()
            if not name or " " in name:
                raise ValueError(f"非法变量名：{name!r}")
            env[name] = value.strip()
        return env

    def _validate(self) -> None:
        try:
            self._parse()
            self._hint.configure(text="")
            self._ok.configure(state="normal")
        except ValueError as exc:
            self._hint.configure(text=str(exc))
            self._ok.configure(state="disabled")

    def _accept(self) -> None:
        try:
            self.result = self._parse()
        except ValueError:
            return
        self.top.destroy()

    def show(self):
        self.top.wait_window()
        return self.result


class ChooseDialog:
    """从未设置的参数里挑一个。"""

    def __init__(self, parent, params) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.params = list(params)
        self.result = None
        self.top = tk.Toplevel(parent)
        self.top.title("添加参数")
        self.top.transient(parent)
        self.top.grab_set()

        body = ttk.Frame(self.top, padding=12)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="选择要覆盖的参数").pack(anchor="w")
        self.listbox = tk.Listbox(body, height=min(12, len(self.params)), width=48)
        self.listbox.pack(fill="both", expand=True, pady=(6, 0))
        for param in self.params:
            suffix = f"  （{param.hint}）" if param.hint else ""
            self.listbox.insert("end", f"{param.label}{suffix}")
        self.listbox.selection_set(0)

        buttons = ttk.Frame(body)
        buttons.pack(anchor="e", pady=(10, 0))
        ttk.Button(buttons, text="确定", command=self._accept).pack(side="right")
        ttk.Button(buttons, text="取消", command=self.top.destroy).pack(side="right", padx=(0, 6))
        self.listbox.bind("<Double-1>", lambda _e: self._accept())
        self.top.bind("<Return>", lambda _e: self._accept())
        self.top.bind("<Escape>", lambda _e: self.top.destroy())

    def _accept(self) -> None:
        selection = self.listbox.curselection()
        if selection:
            self.result = self.params[selection[0]].key
        self.top.destroy()

    def show(self):
        self.top.wait_window()
        return self.result


class Console:
    def __init__(self, root) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.root = root
        root.title("win_sandbox 控制台")
        root.geometry("1020x660")
        root.minsize(780, 480)

        self._overrides = Overrides()
        self._session = Session(self._queue_log)
        self._log_queue: list[str] = []
        self._closed = False
        self._show_all = False
        self._last_workspace = self._overrides.workspace()

        if "vista" in ttk.Style().theme_names():
            ttk.Style().theme_use("vista")

        self._build_command_bar()
        self._build_body()
        self._build_status_bar()
        self._refresh_rows()
        self._sync_buttons()

        root.after(100, self._tick)
        root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── 布局 ────────────────────────────────────────────────

    def _build_command_bar(self) -> None:
        import tkinter as tk
        from tkinter import ttk

        bar = ttk.Frame(self.root, padding=(10, 8))
        bar.pack(fill="x")
        bar.columnconfigure(1, weight=1)

        ttk.Label(bar, text="命令").grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.var_command = tk.StringVar(value=self._overrides.command_line)
        entry = ttk.Entry(bar, textvariable=self.var_command)
        entry.grid(row=0, column=1, sticky="we")
        entry.bind("<Return>", lambda _e: self._on_start())

        self.var_writable = tk.BooleanVar(value=self._overrides.workspace_write)
        ttk.Checkbutton(bar, text="工作区可写", variable=self.var_writable).grid(
            row=0, column=2, padx=(10, 0)
        )

        buttons = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        buttons.pack(fill="x")
        self.btn_start = ttk.Button(buttons, text="启动", command=self._on_start)
        self.btn_stop = ttk.Button(buttons, text="终止", command=self._on_stop)
        self.btn_close = ttk.Button(buttons, text="关闭实例", command=self._on_close_session)
        self.btn_env = ttk.Button(buttons, text="环境变量…", command=self._on_env)
        self.btn_clear = ttk.Button(buttons, text="清空日志", command=self._clear_log)
        for btn in (self.btn_start, self.btn_stop, self.btn_close, self.btn_env, self.btn_clear):
            btn.pack(side="left", padx=(0, 6))

    def _build_body(self) -> None:
        import tkinter as tk
        from tkinter import ttk

        panes = ttk.PanedWindow(self.root, orient="horizontal")
        panes.pack(fill="both", expand=True, padx=10, pady=(0, 6))

        # 左：覆盖项表。默认状态下只有工作区一行，不占面积。
        left = ttk.Frame(panes, width=400)
        panes.add(left, weight=0)

        head = ttk.Frame(left)
        head.pack(fill="x")
        ttk.Label(head, text="配置覆盖").pack(side="left")
        self.btn_show_all = ttk.Button(head, text="显示全部", width=9,
                                       command=self._toggle_show_all)
        self.btn_show_all.pack(side="right")

        self.tree = ttk.Treeview(left, columns=("value",), show="tree headings", height=14)
        self.tree.heading("#0", text="参数")
        self.tree.heading("value", text="值")
        self.tree.column("#0", width=180, stretch=False)
        self.tree.column("value", width=200)
        self.tree.pack(fill="both", expand=True, pady=(4, 4))
        self.tree.bind("<Double-1>", lambda _e: self._on_edit())
        self.tree.bind("<Delete>", lambda _e: self._on_remove())

        row = ttk.Frame(left)
        row.pack(fill="x")
        self.btn_add = ttk.Button(row, text="添加…", command=self._on_add, width=9)
        self.btn_edit = ttk.Button(row, text="修改…", command=self._on_edit, width=9)
        self.btn_remove = ttk.Button(row, text="移除", command=self._on_remove, width=9)
        for btn in (self.btn_add, self.btn_edit, self.btn_remove):
            btn.pack(side="left", padx=(0, 5))

        self.lbl_env = ttk.Label(left, text="环境变量：无", foreground="#555", wraplength=370)
        self.lbl_env.pack(anchor="w", pady=(6, 0))

        # 右：日志——面板的主用途，占满剩余面积
        right = ttk.Frame(panes)
        panes.add(right, weight=1)
        ttk.Label(right, text="日志（命令行的输出在新窗口里）").pack(anchor="w")
        self.log = tk.Text(right, wrap="word", state="disabled", height=10)
        self.log.pack(fill="both", expand=True, pady=(4, 0))
        self.log.tag_configure("err", foreground="#c0392b")
        self.log.tag_configure("ok", foreground="#1e7a3c")

    def _build_status_bar(self) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.status = tk.StringVar(value="未启动")
        ttk.Label(self.root, textvariable=self.status, relief="sunken", padding=(8, 3)).pack(
            fill="x", side="bottom"
        )

    # ── 覆盖项表 ────────────────────────────────────────────

    def _visible_keys(self) -> list[str]:
        if self._show_all:
            return [p.key for p in PARAMS]
        keys = [p.key for p in PARAMS if not p.optional]
        keys += [k for k in self._overrides.values if k not in keys]
        return keys

    def _shown_value(self, key: str) -> str:
        param = PARAM_BY_KEY[key]
        if key in self._overrides.values:
            value = self._overrides.values[key]
            if param.kind == "bool":
                return "启用" if value else "禁用"
            suffix = f"  {param.hint}" if param.kind == "int" and param.hint else ""
            return f"{value}{suffix}"
        if key == "working_dir":
            return self._overrides.workspace()
        return "—"

    def _refresh_rows(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for key in self._visible_keys():
            self.tree.insert("", "end", iid=key, text=PARAM_BY_KEY[key].label,
                             values=(self._shown_value(key),))
        env_text = "、".join(f"{k}={v}" for k, v in self._overrides.env.items()) or "无"
        self.lbl_env.configure(text=f"环境变量：{env_text}")

    def _selected_key(self) -> str | None:
        selection = self.tree.selection()
        return selection[0] if selection else None

    def _toggle_show_all(self) -> None:
        self._show_all = not self._show_all
        self.btn_show_all.configure(text="只看改动" if self._show_all else "显示全部")
        self._refresh_rows()

    def _on_add(self) -> None:
        if not self._show_all:
            self._toggle_show_all()
        available = [p for p in PARAMS if p.optional and p.key not in self._overrides.values]
        if not available:
            self._log("所有参数都已设置")
            return
        key = ChooseDialog(self.root, available).show()
        if key is None:
            return
        param = PARAM_BY_KEY[key]
        value = ParamDialog(self.root, param, self._overrides.values.get(key, param.default)).show()
        if value is not None:
            self._set_value(key, value)

    def _on_edit(self) -> None:
        key = self._selected_key()
        if key is None:
            return
        param = PARAM_BY_KEY[key]
        current = self._overrides.values.get(key)
        if current is None and key == "working_dir":
            current = self._overrides.workspace()
        value = ParamDialog(self.root, param, current).show()
        if value is not None:
            self._set_value(key, value)

    def _on_remove(self) -> None:
        key = self._selected_key()
        if key is None or not PARAM_BY_KEY[key].optional:
            return
        self._overrides.values.pop(key, None)
        self._refresh_rows()

    def _set_value(self, key: str, value) -> None:
        if key == "working_dir":
            new_dir = str(value)
            if new_dir != self._last_workspace and Path(new_dir).is_dir():
                # 旧工作区的 ACE 是常驻授权（不回收），换目录时点一句
                self._log(f"工作区：{self._last_workspace} → {new_dir}")
            self._last_workspace = new_dir
        self._overrides.values[key] = value
        self._refresh_rows()

    def _on_env(self) -> None:
        result = EnvDialog(self.root, self._overrides.env).show()
        if result is not None:
            self._overrides.env = result
            self._refresh_rows()

    # ── 动作 ────────────────────────────────────────────────

    def _current_overrides(self) -> Overrides:
        return Overrides(
            command_line=self.var_command.get().strip(),
            workspace_write=bool(self.var_writable.get()),
            values=dict(self._overrides.values),
            env=dict(self._overrides.env),
        )

    def _on_start(self) -> None:
        cfg = self._current_overrides()
        if not cfg.command_line:
            self._log("命令行为空", err=True)
            return
        try:
            self._session.start(cfg)
        except Exception as exc:  # 配置错误进日志，不炸窗
            self._log(f"启动失败：{exc}", err=True)
        finally:
            self._sync_buttons()

    def _on_stop(self) -> None:
        self._session.terminate()
        self._log("已终止整棵进程树（新窗口一起关）")
        self._sync_buttons()

    def _on_close_session(self) -> None:
        self._session.close()
        self._log("已关闭沙箱实例", ok=True)
        self._sync_buttons()

    def _on_close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._session.close()  # KILL_ON_CLOSE：必须先收掉再退
        self.root.destroy()

    # ── 刷新 ────────────────────────────────────────────────

    def _sync_buttons(self) -> None:
        pid = self._session.pid
        running = self._session.running
        self.btn_start.configure(state="disabled" if running else "normal")
        self.btn_stop.configure(state="normal" if running else "disabled")
        self.btn_close.configure(state="normal" if pid is not None else "disabled")
        editing = "disabled" if running else "normal"
        for btn in (self.btn_add, self.btn_edit, self.btn_remove):
            btn.configure(state=editing)

    def _tick(self) -> None:
        if self._closed:
            return
        while self._log_queue:
            self._log(self._log_queue.pop(0))
        pid = self._session.pid
        if pid is None:
            self.status.set("未启动")
        else:
            state = "运行中" if self._session.running else "已结束"
            count = len(self._session.tree())
            self.status.set(f"pid={pid}   {state}   Job 内进程={count or '—'}")
        self._sync_buttons()
        self.root.after(150, self._tick)

    # ── 日志 ────────────────────────────────────────────────

    def _queue_log(self, line: str) -> None:
        """后台线程入口：只入队，UI 线程统一刷。"""
        self._log_queue.append(line)

    def _log(self, line: str, *, err: bool = False, ok: bool = False) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n", "err" if err else "ok" if ok else None)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")


def main() -> int:
    import tkinter as tk

    root = tk.Tk()
    Console(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
