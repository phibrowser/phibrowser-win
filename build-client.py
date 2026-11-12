#!/usr/bin/env python3
# Copyright 2026 Phinomenon Inc.
# Licensed under the Apache License, Version 2.0; see the LICENSE file.
"""Builds phi_client.dll from this repo. No Chromium checkout is needed.

The client is an ordinary Windows DLL: everything it compiles is in this
repository (client/, its small base:: layer in client/compat, and the vendored
code in third_party/). The prebuilt Phi framework supplies Phi.exe, which loads
this DLL in the browser process.

On Windows, run it from a Visual Studio developer prompt:

  python build-client.py [--out out\\Release]

It uses clang-cl when found and otherwise MSVC's cl.

Cross-compiling from another host takes the tools and the Windows SDK by path:

  build-client.py --clang-dir <llvm>/bin --winsysroot <vs_files/hash>

--with-kit-host relinks Phi.exe from a framework build's kit (the framework's
own maintainers need it to stamp the icon and version); nobody else does.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

REPO_DIR = os.path.dirname(os.path.abspath(__file__))
CLIENT_DIR = os.path.join(REPO_DIR, "client")
COMPAT_DIR = os.path.join(CLIENT_DIR, "compat")
THIRD_PARTY = os.path.join(REPO_DIR, "third_party")
DEFAULT_OUT = os.path.join(REPO_DIR, "out", "Release")
KIT = "phi_clientkit/kit.json"
OBJ_DIR = "client_obj"
RESOURCES = ("emoji-catalog.json", "phi.ico", "strings.json")
# The client-owned subtrees under <out>/resources (staged below), and the
# manifest naming them all for the installer's layout split.
CLIENT_RESOURCE_TREES = ("phi-browser-skill", "onboarding", "fonts")
CLIENT_RESOURCES_MANIFEST = "phi-client-resources.txt"
# The phi-browser skill ships beside the exe as resources/phi-browser-skill:
# the tree Settings > Developer > "Add skill to..." junctions into an agent's
# skills folder (the Mac bundles the same folder under Contents/Resources).
SKILL_DIR = os.path.join(REPO_DIR, "tools", "phi-browser-skill")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "deflate.c", "trees.c", "zutil.c",
                "cpu_features.c")
SQLITE_DEFINES = (
    "-DSQLITE_THREADSAFE=1", "-DSQLITE_DQS=0", "-DSQLITE_DEFAULT_MEMSTATUS=0",
    "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1", "-DSQLITE_OMIT_DEPRECATED",
    "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_SHARED_CACHE",
    "-DSQLITE_OMIT_PROGRESS_CALLBACK", "-DSQLITE_USE_ALLOCA")
COMMON_FLAGS = (
    "/c", "/nologo", "/utf-8", "/O2", "/Oy-", "/Gy", "/Gw", "/MT", "/guard:cf",
    "/Z7", "/bigobj", "/Brepro",
    "-DNDEBUG", "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DWIN32",
    "-DWIN32_LEAN_AND_MEAN", "-D_WINDOWS", "-D_WIN32_WINNT=0x0A00",
    "-DWINVER=0x0A00", "-D_HAS_EXCEPTIONS=0", "-D_CRT_SECURE_NO_DEPRECATE",
    "-D_CRT_NONSTDC_NO_WARNINGS", "-D_CRT_RAND_S",
    "-D_SILENCE_ALL_CXX23_DEPRECATION_WARNINGS",
)
# What Phi's own releases add: the checks Chromium's recipe gave the client.
HARDENING_FLAGS = (
    "-ftrivial-auto-var-init=pattern", "-fno-delete-null-pointer-checks",
    "/clang:-fno-strict-overflow", "-fsanitize=array-bounds",
    "-fsanitize-trap=array-bounds", "-fsanitize=return",
    "-fsanitize-trap=return", "-fsanitize=unreachable",
    "-fsanitize-trap=unreachable", "-D_MSVC_STL_HARDENING=1",
)
LIBS = (
    "advapi32.lib", "bcrypt.lib", "comctl32.lib", "comdlg32.lib", "crypt32.lib",
    "d2d1.lib", "dbghelp.lib", "dwmapi.lib", "dwrite.lib", "gdi32.lib",
    "gdiplus.lib", "imm32.lib", "kernel32.lib", "msimg32.lib", "ole32.lib",
    "oleaut32.lib", "propsys.lib", "runtimeobject.lib", "shell32.lib",
    "shlwapi.lib", "synchronization.lib", "user32.lib", "userenv.lib",
    "usp10.lib", "uuid.lib", "uxtheme.lib", "version.lib", "windowscodecs.lib",
    "winhttp.lib", "winmm.lib", "ws2_32.lib",
)


def sync_tree(src, dst):
    """Mirrors src into dst: copies changed files (mtime preserved, so the
    box sync's manifest diff stays quiet), removes files src no longer has."""
    wanted = set()
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for name in files:
            s_path = os.path.join(root, name)
            d_path = os.path.join(out, name)
            wanted.add(os.path.normpath(d_path))
            s_stat = os.stat(s_path)
            if (not os.path.exists(d_path) or
                    os.stat(d_path).st_size != s_stat.st_size or
                    abs(os.stat(d_path).st_mtime - s_stat.st_mtime) > 2):
                shutil.copy2(s_path, d_path)
    for root, dirs, files in os.walk(dst, topdown=False):
        for name in files:
            path = os.path.join(root, name)
            if os.path.normpath(path) not in wanted:
                os.remove(path)
        for name in dirs:
            path = os.path.join(root, name)
            if not os.listdir(path):
                os.rmdir(path)


def tool(clang_dir, name):
    """The tool's path: from --clang-dir, PATH, or a Visual Studio install."""
    suffix = ".exe" if sys.platform == "win32" else ""
    if clang_dir:
        path = os.path.join(clang_dir, name + suffix)
        if not os.path.isfile(path):
            sys.exit(f"{path} not found")
        return path
    found = shutil.which(name)
    if found:
        return found
    if sys.platform == "win32":
        root = os.environ.get("VCINSTALLDIR")
        if root:
            path = os.path.join(root, "Tools", "Llvm", "x64", "bin",
                                name + suffix)
            if os.path.isfile(path):
                return path
    sys.exit(f"{name} not found: run from a Visual Studio developer prompt "
             "with the C++ Clang tools installed, or pass --clang-dir")


def compiler(args):
    """("clang", path) or ("msvc", path), honouring --compiler."""
    if args.compiler == "msvc":
        found = shutil.which("cl")
        if not found:
            sys.exit("cl not found: run from a Visual Studio developer prompt")
        return "msvc", found
    if args.compiler == "auto" and not args.clang_dir:
        if not shutil.which("clang-cl") and shutil.which("cl"):
            return "msvc", shutil.which("cl")
    return "clang", tool(args.clang_dir, "clang-cl")


def newest_mtime(*roots):
    newest = 0
    for root in roots:
        for folder, _dirs, files in os.walk(root):
            for name in files:
                if name.endswith((".h", ".inc")):
                    newest = max(newest, os.path.getmtime(
                        os.path.join(folder, name)))
    return newest


def stem_of(path):
    return os.path.splitext(os.path.basename(path))[0]


def reorder_imports(exe_path, src_root):
    """chrome_elf.dll must be the FIRST import so its blocklist and crash
    hooks are in place before any other DLL initializes — what
    //chrome:reorder_imports (build/win/reorder-imports.py) does to
    chrome.exe. Only the two import directory entries change."""
    sys.path.insert(0, os.path.join(src_root, "third_party", "pefile_py3"))
    import pefile  # noqa: E402 (vendored with the framework tree)
    pe = pefile.PE(exe_path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    imports = pe.DIRECTORY_ENTRY_IMPORT
    index = next((i for i, imp in enumerate(imports)
                  if imp.dll.lower() == b"chrome_elf.dll"), None)
    if index is None:
        sys.exit("Phi.exe does not import chrome_elf.dll — the link recipe "
                 "lost //chrome/chrome_elf")
    if index == 0:
        pe.close()
        return "chrome_elf.dll already first"
    elf, first = imports[index].struct, imports[0].struct
    # OriginalFirstThunk and Characteristics are a union at offset 0;
    # swapping one of them is enough (as reorder-imports.py notes).
    for field in ("OriginalFirstThunk", "TimeDateStamp", "ForwarderChain",
                  "Name", "FirstThunk"):
        elf_value, first_value = getattr(elf, field), getattr(first, field)
        setattr(elf, field, first_value)
        setattr(first, field, elf_value)
    tmp = exe_path + ".reorder"
    pe.write(filename=tmp)
    pe.close()
    os.replace(tmp, exe_path)
    return f"chrome_elf.dll moved from import slot {index} to 0"


def relink_host(out_dir):
    """Relinks Phi.exe from a framework build's kit so it carries the
    client's icon and VERSIONINFO. Framework maintainers only."""
    kit = json.load(open(os.path.join(out_dir, KIT)))
    rc = kit["rc_template"]
    if kit["rc_template_source"] not in rc or kit["rc_template_out"] not in rc:
        sys.exit("cannot retarget the kit's rc command")
    res = f"{OBJ_DIR}/phi_client.res"
    rc = rc.replace(kit["rc_template_source"],
                    "../../phibrowser-win/client/phi_client.rc").replace(
                        kit["rc_template_out"], res)
    proc = subprocess.run(rc, shell=True, cwd=out_dir, capture_output=True,
                          text=True, errors="replace")
    if proc.returncode != 0:
        print(proc.stdout + proc.stderr)
        sys.exit(1)
    # Phi.exe carries the client's VERSIONINFO, and two resources of the same
    # id are a link error, so the Chromium one is dropped.
    inputs = [i for i in kit["link_inputs"]
              if not i.endswith("chrome_exe_version.res")] + [res]
    rsp_path = os.path.join(out_dir, "phi_clientkit/Phi.exe.rsp")
    with open(rsp_path, "w") as f:
        f.write("\n".join(inputs) + "\n")
        f.write(kit["libs"] + " " + kit["solibs"] + " " + kit["ldflags"] +
                " " + kit.get("rlibs", "") + "\n")
    link = kit["link_command"].replace(kit["scaffold_exe"], "Phi.exe")
    link = re.sub(r'"@[^"]*"', f'"@{rsp_path}"', link)
    print("linking Phi.exe...")
    proc = subprocess.run(link, shell=True, cwd=out_dir, capture_output=True,
                          text=True, errors="replace")
    if proc.returncode != 0:
        print(proc.stdout + proc.stderr)
        sys.exit(1)
    print("imports: " + reorder_imports(
        os.path.join(out_dir, "Phi.exe"),
        os.path.normpath(os.path.join(out_dir, "..", ".."))))


def target_machine(args):
    return args.arch


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="output directory (default out/Release)")
    parser.add_argument("--clang-dir", default=os.environ.get("PHI_CLANG_DIR"),
                        help="directory holding clang-cl and lld-link")
    parser.add_argument("--compiler", choices=("auto", "clang", "msvc"),
                        default="auto")
    parser.add_argument("--winsysroot",
                        default=os.environ.get("PHI_WINSYSROOT"),
                        help="MSVC/Windows SDK root, when cross-compiling")
    parser.add_argument("--arch", choices=("x64", "arm64"), default="x64")
    parser.add_argument("-f", "--force", action="store_true",
                        help="recompile everything")
    parser.add_argument("--dev-hooks", action="store_true",
                        help="define PHI_DEV_HOOKS: compile in the "
                             "developer-only hooks (env overrides for the "
                             "appcast and vault helper, plaintext dev vault)."
                             " Never for a release build.")
    parser.add_argument("--only", action="append", default=[],
                        metavar="STEM",
                        help="compile only these client sources (stem, e.g. "
                             "phi_coordinator); repeatable")
    parser.add_argument("--no-link", action="store_true",
                        help="stop after compiling (syntax/type check)")
    parser.add_argument("--hardened", action="store_true",
                        help="add the release hardening checks (clang-cl)")
    parser.add_argument("--with-kit-host", action="store_true",
                        help="relink Phi.exe from this out dir's framework "
                             "kit (framework maintainers)")
    args = parser.parse_args()
    out_dir = os.path.abspath(args.out)
    obj_root = os.path.join(out_dir, OBJ_DIR)
    os.makedirs(obj_root, exist_ok=True)

    kind, compiler_path = compiler(args)
    if kind == "clang":
        base = [compiler_path,
                "--target=" + {"x64": "x86_64", "arm64": "aarch64"}[args.arch] +
                "-pc-windows-msvc", "-w", *COMMON_FLAGS]
        if args.hardened:
            base.extend(HARDENING_FLAGS)
    else:
        if args.hardened:
            sys.exit("--hardened needs clang-cl")
        base = [compiler_path, "/w", *COMMON_FLAGS]
    if args.winsysroot:
        base.append("/winsysroot" + os.path.abspath(args.winsysroot))
    cpp_flags = [
        "/std:c++23preview", "/GR-", "-I" + COMPAT_DIR, "-I" + REPO_DIR,
        "-I" + os.path.join(THIRD_PARTY, "boringssl", "src", "include"),
    ]
    if args.dev_hooks:
        cpp_flags.append("-DPHI_DEV_HOOKS=1")

    # What each object depends on, so an unrelated edit rebuilds nothing.
    client_headers = max(newest_mtime(CLIENT_DIR), os.path.getmtime(__file__))
    vendored_stamp = os.path.getmtime(__file__)
    jobs = []
    objects = []

    def add(name, source, flags, depends_on):
        obj = os.path.join(obj_root, name + ".obj")
        objects.append(obj)
        if not args.force and os.path.exists(obj) and os.path.getmtime(
                obj) >= max(os.path.getmtime(source), depends_on):
            return
        jobs.append((os.path.basename(source),
                     [*base, *flags, "/Fo" + obj, source]))

    wanted = {*args.only}
    client_sources = sorted(
        os.path.join(CLIENT_DIR, f) for f in os.listdir(CLIENT_DIR)
        if f.endswith(".cc"))
    compat_sources = sorted(
        os.path.join(COMPAT_DIR, f) for f in os.listdir(COMPAT_DIR)
        if f.endswith(".cc"))
    known = {stem_of(s) for s in client_sources + compat_sources}
    if wanted - known:
        sys.exit(f"--only: no such client source: {sorted(wanted - known)}")
    for source in client_sources:
        if not wanted or stem_of(source) in wanted:
            add(stem_of(source), source, [*cpp_flags, "/TP"], client_headers)
    for source in compat_sources:
        if not wanted or stem_of(source) in wanted:
            add("compat_" + stem_of(source), source, [*cpp_flags, "/TP"],
                client_headers)

    if not wanted:
        add("sqlite3", os.path.join(THIRD_PARTY, "sqlite", "sqlite3.c"),
            ["/TC", *SQLITE_DEFINES], vendored_stamp)
        for name in ZLIB_SOURCES:
            add("zlib_" + stem_of(name),
                os.path.join(THIRD_PARTY, "zlib", name),
                ["/TC", "-I" + os.path.join(THIRD_PARTY, "zlib"),
                 "-DZLIB_IMPLEMENTATION", "-DCPU_NO_SIMD",
                 "-DARMV8_OS_WINDOWS" if args.arch == "arm64"
                 else "-DX86_WINDOWS"], vendored_stamp)
        boringssl = os.path.join(THIRD_PARTY, "boringssl", "src")
        with open(os.path.join(THIRD_PARTY, "boringssl", "sources.txt")) as f:
            for relative in f.read().split():
                add("ssl_" + relative.replace("/", "_").rsplit(".", 1)[0],
                    os.path.join(boringssl, relative),
                    ["/TP", "/std:c++17", "/GR-", "-I" + boringssl,
                     "-I" + os.path.join(boringssl, "include"),
                     "-DOPENSSL_NO_ASM", "-DBORINGSSL_IMPLEMENTATION"],
                    vendored_stamp)

    failed = False
    if jobs:
        print(f"compiling {len(jobs)} file(s)...")
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=os.cpu_count()) as pool:
            def run(job):
                name, cmd = job
                proc = subprocess.run(cmd, cwd=out_dir, capture_output=True,
                                      text=True, errors="replace")
                return name, proc
            for name, proc in pool.map(run, jobs):
                output = "\n".join(
                    line for line in (proc.stdout + proc.stderr).splitlines()
                    if line.strip() and line.strip() != name)
                if proc.returncode != 0:
                    failed = True
                    print(f"FAILED {name}\n{output}")
                elif output:
                    print(f"{name}: {output}")
    if failed:
        sys.exit(1)
    if args.no_link or args.only:
        print("compile OK (no link)")
        return

    rsp_path = os.path.join(obj_root, "phi_client.dll.rsp")
    with open(rsp_path, "w") as f:
        f.write("\n".join(o.replace("\\", "/") for o in objects) + "\n")
    linker = "link" if kind == "msvc" else tool(args.clang_dir, "lld-link")
    link = [linker, "/nologo", "/DLL",
            "/OUT:" + os.path.join(out_dir, "phi_client.dll"),
            "/PDB:" + os.path.join(out_dir, "phi_client.dll.pdb"),
            "/DEBUG:FULL", "/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO",
            "/guard:cf", "/DYNAMICBASE", "/NXCOMPAT",
            "/SUBSYSTEM:WINDOWS,10.0", "/MANIFEST:NO",
            "/MACHINE:" + args.arch.upper(), "@" + rsp_path, *LIBS]
    # Shadow stacks are an x86 feature: MSVC's link refuses the flag for ARM64.
    if args.arch == "x64":
        link.append("/CETCOMPAT")
    if args.winsysroot:
        link.append("/winsysroot:" + os.path.abspath(args.winsysroot))
    print("linking phi_client.dll...")
    proc = subprocess.run(link, cwd=out_dir, capture_output=True, text=True,
                          errors="replace")
    if proc.returncode != 0:
        print(proc.stdout + proc.stderr)
        sys.exit(1)

    if args.with_kit_host:
        relink_host(out_dir)

    resources_dir = os.path.join(out_dir, "resources")
    os.makedirs(resources_dir, exist_ok=True)
    for name in RESOURCES:
        shutil.copy2(os.path.join(CLIENT_DIR, "resources", name),
                     resources_dir)
    sync_tree(SKILL_DIR, os.path.join(resources_dir, "phi-browser-skill"))
    # The onboarding window's pages (login still, wallpaper, dot pattern,
    # tiles) and the brand faces it registers privately (phi_fonts.cc). The
    # membership card (phi_holographic_card.cc) is procedural: no frames.
    sync_tree(os.path.join(CLIENT_DIR, "resources", "onboarding"),
              os.path.join(resources_dir, "onboarding"))
    sync_tree(os.path.join(CLIENT_DIR, "resources", "fonts"),
              os.path.join(resources_dir, "fonts"))
    # What the CLIENT owns under resources/ — the installer's staging splits
    # the shared dev-tree folder by this list (client entries stay beside
    # Phi.exe, Chromium's accessibility/ and inspector_overlay/ go under
    # framework/; installer/stage-payload.ps1).
    with open(os.path.join(resources_dir, CLIENT_RESOURCES_MANIFEST),
              "w") as f:
        f.write("\n".join(sorted(RESOURCES + CLIENT_RESOURCE_TREES)) + "\n")
    # Stage the vendored WinSparkle.dll beside the client: the updater loads
    # it from the exe's own dir (third_party/winsparkle carries the release's
    # x64 and arm64 builds).
    winsparkle = os.path.join(THIRD_PARTY, "winsparkle", args.arch,
                              "WinSparkle.dll")
    if not os.path.exists(winsparkle):
        sys.exit(f"no vendored WinSparkle.dll for {args.arch}: {winsparkle}")
    staged = os.path.join(out_dir, "WinSparkle.dll")
    if (not os.path.exists(staged) or
            os.path.getmtime(staged) < os.path.getmtime(winsparkle)):
        shutil.copy2(winsparkle, staged)
        print("staged WinSparkle.dll")

    print("CLIENT BUILD OK: " + os.path.join(out_dir, "phi_client.dll"))


if __name__ == "__main__":
    main()
