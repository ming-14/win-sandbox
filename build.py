#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sandbox 独立构建脚本（Windows 专属，pybind11 + CMake + MSVC）。

编译 win_sandbox_native.pyd 并组装可分发的 win_sandbox Python 包。

用法:
    python build.py                         # 增量构建（默认 Release，输出到 dist/win_sandbox）
    python build.py --clean                 # 删除 src/build 强制全量重新生成
    python build.py --config Debug          # 选择配置
    python build.py --out <dir>             # 指定输出目录
    python build.py --selftest              # 编译并运行 selftest.exe（不经 pybind11）
    python build.py --verbose               # 详细日志
"""

import argparse
import ctypes
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SRC_DIR = SCRIPT_DIR / "src"
BUILD_DIR = SRC_DIR / "build"
THIRD_PARTY_DIR = SCRIPT_DIR / "third_party"
PYTHON_SRC_DIR = SRC_DIR / "python" / "win_sandbox"

IS_WINDOWS = sys.platform == "win32"

logger = logging.getLogger("sandbox-build")


# =============================================================================
# 工具函数
# =============================================================================

def run_cmd(args, cwd=None):
    """运行外部命令并返回退出码。"""
    proc = subprocess.Popen(args, cwd=cwd)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        raise


def find_vcvars():
    """定位 vcvars64.bat：优先 vswhere 探测实际安装，回退常见版本/版本目录路径。"""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio/Installer/vswhere.exe"
    if vswhere.is_file():
        try:
            result = subprocess.run(
                [str(vswhere), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, timeout=60)
        except (subprocess.TimeoutExpired, OSError):
            result = None
        if result and result.returncode == 0:
            candidate = Path(result.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.is_file():
                return candidate
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    candidates = []
    for version in ("18", "2022", "17", "2019"):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools", "Preview"):
            candidates.append(pf / Path("Microsoft Visual Studio") / version / edition /
                              "VC" / "Auxiliary" / "Build" / "vcvars64.bat")
    return next((p for p in candidates if p.is_file()), None)


def write_cmd_wrapper(prefix, lines):
    """写临时 .cmd 脚本（vcvars 环境注入/跨进程环境配置只能经 cmd 执行）。"""
    cmd_file = Path(tempfile.gettempdir()) / "{}_{}.cmd".format(prefix, uuid.uuid4().hex[:8])
    content = "@echo off\nchcp 65001 >nul\n" + "\n".join(lines) + "\nexit /b %errorlevel%\n"
    encoding = "cp{}".format(ctypes.windll.kernel32.GetACP()) if IS_WINDOWS else "utf-8"
    try:
        content.encode(encoding)
    except (UnicodeEncodeError, LookupError):
        logger.warning("[cmd] 路径含 %s 无法表示的字符，回退 UTF-8 写入 %s", encoding, cmd_file)
        encoding = "utf-8"
    cmd_file.write_text(content, encoding=encoding)
    return cmd_file


def setup_logging(verbose: bool):
    """配置日志系统。"""
    if IS_WINDOWS:
        try:
            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
        except Exception:
            pass
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-7s %(message)s",
        handlers=[logging.StreamHandler()],
    )


# =============================================================================
# 构建步骤
# =============================================================================

def build_sandbox(config: str, clean: bool, output_dir: Path):
    """编译 win_sandbox_native.pyd 并组装 Python 包。"""
    # ---- 检测工具链 ----
    cmake = shutil.which("cmake")
    if not cmake:
        logger.error("cmake 未找到，请先安装 CMake（https://cmake.org/download/）")
        sys.exit(1)

    vcvars = find_vcvars()
    if not vcvars:
        logger.error("vcvars64.bat 未找到，请安装 Visual Studio（含 x64 工具链）")
        sys.exit(1)
    logger.info("vcvars64.bat: %s", vcvars)

    # ---- 检测 Ninja ----
    ninja = shutil.which("ninja")
    if ninja:
        generator = "Ninja"
        logger.info("generator: Ninja")
    else:
        generator = 'Visual Studio 17 2022'
        logger.warning("ninja 未找到，回退 %s -A x64（速度较慢，推荐安装 Ninja）", generator)

    # ---- 清理（可选） ----
    if clean and BUILD_DIR.exists():
        logger.info("--clean: 删除 %s", BUILD_DIR)
        shutil.rmtree(BUILD_DIR)

    # ---- 构建 ----
    src = str(SRC_DIR)
    build = str(BUILD_DIR)
    if ninja:
        configure_cmd = f'cmake -S "{src}" -B "{build}" -G "{generator}" -DCMAKE_BUILD_TYPE={config}'
    else:
        configure_cmd = f'cmake -S "{src}" -B "{build}" -G "{generator}" -A x64'

    build_cmd = f'cmake --build "{build}" --config {config}'

    lines = [
        'call "{}" >nul 2>&1'.format(vcvars),
        configure_cmd,
        build_cmd,
    ]
    cmd_file = write_cmd_wrapper("sandbox", lines)
    logger.info("配置并构建 win_sandbox_native.pyd (config=%s) ...", config)
    try:
        rc = run_cmd(["cmd", "/c", str(cmd_file)])
    finally:
        cmd_file.unlink(missing_ok=True)

    if rc != 0:
        logger.error("构建失败（exit=%s），详情见上方日志", rc)
        sys.exit(1)

    # ---- 查找产物 ----
    pyd_pattern = "win_sandbox_native*.pyd"
    pyd = None
    for p in BUILD_DIR.rglob(pyd_pattern):
        if p.is_file():
            pyd = p
            break
    if not pyd:
        logger.error("未找到构建产物 %s", pyd_pattern)
        sys.exit(1)
    logger.info("产物: %s", pyd.name)

    # ---- 组装 Python 包 ----
    package_dir = output_dir / "win_sandbox"
    native_dir = package_dir / "_native"
    native_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(pyd), str(native_dir / pyd.name))
    logger.info("复制 .pyd → %s", native_dir / pyd.name)

    if PYTHON_SRC_DIR.is_dir():
        for py_file in PYTHON_SRC_DIR.glob("*.py"):
            shutil.copy2(str(py_file), str(package_dir / py_file.name))
            logger.info("复制 Python 包装 → %s", package_dir / py_file.name)
    else:
        logger.warning("Python 包装源目录不存在: %s", PYTHON_SRC_DIR)

    logger.info("Python 包已组装到: %s", package_dir)


def build_selftest():
    """编译并运行 selftest.exe（不经 pybind11，隔离绑定层崩溃）。"""
    vcvars = find_vcvars()
    if not vcvars:
        logger.error("vcvars64.bat 未找到，跳过 selftest")
        return

    # 收集所有 .cpp 源文件（排除 module.cpp — 它依赖 pybind11）
    cpp_files = sorted(SRC_DIR.glob("*.cpp"))
    selftest_srcs = [f for f in cpp_files if f.name != "module.cpp"]
    src_list = " ".join(str(f) for f in selftest_srcs)
    out_exe = SRC_DIR / "selftest.exe"

    lines = [
        'call "{}" >nul 2>&1'.format(vcvars),
        f'cl /EHsc /std:c++20 /I"{SRC_DIR}" {src_list} /Fe:"{out_exe}" /link advapi32.lib bcrypt.lib shell32.lib',
    ]
    cmd_file = write_cmd_wrapper("selftest", lines)
    logger.info("编译 selftest.exe ...")
    try:
        rc = run_cmd(["cmd", "/c", str(cmd_file)])
    finally:
        cmd_file.unlink(missing_ok=True)

    if rc != 0:
        logger.error("selftest 编译失败（exit=%s）", rc)
        sys.exit(1)

    logger.info("运行 selftest.exe ...")
    rc = run_cmd([str(out_exe)])
    if rc != 0:
        logger.error("selftest 运行失败（exit=%s）", rc)
        sys.exit(1)
    logger.info("selftest OK")


# =============================================================================
# 入口
# =============================================================================

def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="sandbox 独立构建脚本（Windows 专属）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
示例:
  python build.py                         # 增量构建，输出到 dist/win_sandbox
  python build.py --clean                 # 全量重新构建
  python build.py --config Debug          # Debug 构建
  python build.py --out pkg               # 输出到 pkg/win_sandbox
  python build.py --selftest              # 编译并运行自测
""")
    parser.add_argument("--clean", action="store_true",
                        help="删除 src/build 强制全量重新生成")
    parser.add_argument("--config", default="Release",
                        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                        help="CMake 构建配置（默认 Release）")
    parser.add_argument("--out", type=Path, default=SCRIPT_DIR / "dist",
                        help="输出目录（默认 dist/win_sandbox）")
    parser.add_argument("--selftest", action="store_true",
                        help="编译并运行 selftest.exe（不经 pybind11 隔离测试）")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="详细日志")
    return parser.parse_args(argv)


def main():
    args = parse_args()
    setup_logging(args.verbose)

    if not IS_WINDOWS:
        logger.error("sandbox 为 Windows 专属组件，仅支持 Windows 编译")
        sys.exit(1)

    logger.info("sandbox 独立构建")
    logger.info("源目录: %s", SRC_DIR)
    logger.info("构建目录: %s", BUILD_DIR)
    logger.info("输出目录: %s", args.out / "win_sandbox")

    build_sandbox(args.config, args.clean, args.out)

    if args.selftest:
        logger.info("")
        logger.info("=== selftest ===")
        build_selftest()

    logger.info("")
    logger.info("构建完成。")
    logger.info("Python 包: %s", args.out / "win_sandbox")
    logger.info("用法: import win_sandbox; sb = win_sandbox.SandboxInstance()")


if __name__ == "__main__":
    main()