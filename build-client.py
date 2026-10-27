#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds Phi.exe from this repo against a PREBUILT framework.

The framework build compiles no client code; it exports its recipe —
compile / rc / link commands plus every chromium-side input — as
out/phi_clientkit/kit.json (written by the fork's phi_sdk_package.py,
either into a local out dir or into a distributable SDK zip). This script
replays that recipe against client/*.cc: identical clang-cl flags (so the
objects are ABI-interchangeable with the framework's), the framework's own
objects and import libs on the link line, the stub scaffold object swapped
out for the real client ones. No gn, no ninja — a client edit rebuilds in
seconds, and no Chromium checkout is needed when building from an SDK.

Modes:
  --sdk <dir>   build inside an unzipped framework SDK (see README-SDK.md);
                the winsysroot comes from --winsysroot or $PHI_WINSYSROOT
                if the SDK doesn't already have one placed
  --out <dir>   build inside a local framework out dir (the default,
                <chromium-src>/out/PhiFxWinShip; the arm64
                ship build is out/PhiFxWinShipArm64 — the kit's link
                recipe carries the /MACHINE, nothing here is per-arch
                except the WinSparkle.dll staged beside the exe)

The exe lands at <out>/Phi.exe; client resources (emoji catalog,
icon) are copied to <out>/resources/.
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
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "out", "PhiFxWinShip")
KIT = "phi_clientkit/kit.json"
OBJ_DIR = "phi_clientkit/obj"
RESOURCES = ("emoji-catalog.json", "phi.ico", "strings.json")
# The client-owned subtrees under <out>/resources (staged below), and the
# manifest naming them all for the installer's layout split.
CLIENT_RESOURCE_TREES = ("phi-browser-skill", "onboarding", "fonts")
CLIENT_RESOURCES_MANIFEST = "phi-client-resources.txt"
# The phi-browser skill ships beside the exe as resources/phi-browser-skill:
# the tree Settings > Developer > "Add skill to..." junctions into an agent's
# skills folder (the Mac bundles the same folder under Contents/Resources).
SKILL_DIR = os.path.join(REPO_DIR, "tools", "phi-browser-skill")


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


def stem_of(path):
    return os.path.splitext(os.path.basename(path))[0]


def load_kit(out_dir):
    kit_path = os.path.join(out_dir, KIT)
    if not os.path.exists(kit_path):
        sys.exit(f"{kit_path} missing — run the fork's phi_sdk_package.py "
                 "(or unzip a framework SDK) first")
    return json.load(open(kit_path))


def prepare_tree(out_dir, winsysroot):
    """The recipe's paths are relative to an out dir two levels below the
    source root (out/PhiFxWin): make sure the client repo and the
    winsysroot are reachable from there."""
    src_root = os.path.normpath(os.path.join(out_dir, "..", ".."))
    # Zip extraction drops execute bits — heal the bundled tools.
    for tool in ("third_party/llvm-build/Release+Asserts/bin/clang-cl",
                 "third_party/llvm-build/Release+Asserts/bin/lld-link",
                 "build/toolchain/win/rc/linux64/rc"):
        path = os.path.join(src_root, tool)
        if os.path.isfile(path) and not os.access(path, os.X_OK):
            os.chmod(path, 0o755)
    link = os.path.join(src_root, "phibrowser-win")
    if not os.path.exists(link):
        os.symlink(REPO_DIR, link)
        print(f"linked {link} -> {REPO_DIR}")
    m = re.search(r"/winsysroot:?(\S+)", json.load(
        open(os.path.join(out_dir, KIT)))["compile_template"])
    sysroot = os.path.normpath(os.path.join(out_dir, m.group(1)))
    if not os.path.isdir(sysroot):
        winsysroot = winsysroot or os.environ.get("PHI_WINSYSROOT")
        if not winsysroot or not os.path.isdir(winsysroot):
            sys.exit(f"winsysroot missing at {sysroot} — pass --winsysroot "
                     "(or set $PHI_WINSYSROOT) to your MSVC/Windows-SDK "
                     "vs_files/<hash> directory")
        os.makedirs(os.path.dirname(sysroot), exist_ok=True)
        os.symlink(os.path.abspath(winsysroot), sysroot)
        print(f"linked {sysroot} -> {winsysroot}")


def reorder_imports(exe_path, src_root):
    """chrome_elf.dll must be the FIRST import so its blocklist and crash
    hooks are in place before any other DLL initializes — what
    //chrome:reorder_imports (build/win/reorder-imports.py) does to
    chrome.exe. The link emits imports in library order, which puts the exe's
    other imports (base.dll in a component build, system DLLs in a static
    one) ahead of it; this swaps chrome_elf into slot 0 in place. Only the
    two import directory entries change, so the debug directory (the PDB
    binding) is untouched."""
    sys.path.insert(0, os.path.join(src_root, "third_party", "pefile_py3"))
    import pefile  # noqa: E402 (vendored with the framework tree / SDK)
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


def target_machine(kit):
    """The architecture the kit links for ('x64' or 'arm64'), read off the
    framework's own link command so the client can never disagree with
    the chrome.dll it is built against."""
    match = re.search(r"/MACHINE:(\w+)", kit["link_command"], re.IGNORECASE)
    if not match:
        sys.exit("the kit's link command carries no /MACHINE")
    return match.group(1).lower()


# The vendored SQLite (third_party/sqlite, README.phi.md) is C, and the kit's
# only compile recipe is the C++ one. The C recipe is that recipe with the
# language and C++-only switches swapped out: /TC for /TP, no /std:c++*,
# no Chromium clang plugins (they are C++ checks), and every warning off
# (-w) because the amalgamation is not ours to fix — the target, CRT,
# optimisation and debug-info switches are kept, so the object links like
# any other.
SQLITE_SOURCE = "../../phibrowser-win/third_party/sqlite/sqlite3.c"
SQLITE_DEFINES = (
    "-DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 "
    "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_DEPRECATED "
    "-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_SHARED_CACHE "
    "-DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_USE_ALLOCA")


def c_recipe(template):
    if " /TP" not in template:
        sys.exit("the kit's compile recipe has no /TP to swap for /TC")
    command = template.replace(" /TP", " /TC")
    command = re.sub(r" /std:\S+", "", command)
    command = re.sub(r" /WX\b", "", command)
    command = re.sub(
        r" -Xclang (?:-add-plugin|-plugin-arg-[\w-]+) -Xclang \S+", "",
        command)
    # Clang modules (r154+): the prebuilt system module is a C++ one, and a C
    # compile that loads it fails on a configuration mismatch.
    command = re.sub(r" -Xclang -fmodules?[\w-]*(?:=\S+)?", "", command)
    command = re.sub(
        r' "?-fmodule-(?:file|map-file|name)=[^" ]+"?|'
        r" -fmodules\b|-fno-implicit-module(?:-map)?s\b", "", command)
    return command.replace(" /c ", f" -w {SQLITE_DEFINES} /c ", 1)


def retarget(template, old_source, old_out, source, out):
    if old_source not in template or old_out not in template:
        sys.exit(f"cannot retarget recipe command onto {source}")
    return template.replace(old_source, source).replace(old_out, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", help="unzipped framework SDK root")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="local framework build directory")
    parser.add_argument("--winsysroot",
                        help="MSVC/Windows-SDK vs_files/<hash> directory")
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
    args = parser.parse_args()
    out_dir = os.path.abspath(
        os.path.join(args.sdk, "out/PhiFxWin") if args.sdk else args.out)

    kit = load_kit(out_dir)
    prepare_tree(out_dir, args.winsysroot)
    os.makedirs(os.path.join(out_dir, OBJ_DIR), exist_ok=True)

    sources = sorted(
        os.path.join(CLIENT_DIR, f) for f in os.listdir(CLIENT_DIR)
        if f.endswith(".cc"))
    if args.only:
        wanted = {stem_of(s) for s in args.only}
        unknown = wanted - {stem_of(s) for s in sources}
        if unknown:
            sys.exit(f"--only: no such client source: {sorted(unknown)}")
        sources = [s for s in sources if stem_of(s) in wanted]
    compile_template = kit["compile_template"]
    if args.dev_hooks:
        compile_template = compile_template.replace(
            " /c ", " /DPHI_DEV_HOOKS=1 /c ", 1)
    headers_mtime = max(
        (os.path.getmtime(os.path.join(CLIENT_DIR, f))
         for f in os.listdir(CLIENT_DIR) if f.endswith(".h")), default=0)

    # Compile what's stale. Sources are addressed src-root-relative, exactly
    # as the recipe's own compile was.
    jobs = []
    client_objs = []
    for source in sources:
        stem = stem_of(source)
        obj = f"{OBJ_DIR}/{stem}.obj"
        client_objs.append(obj)
        cmd = retarget(compile_template, kit["template_source"],
                       kit["template_obj"],
                       f"../../phibrowser-win/client/{stem}.cc", obj)
        obj_path = os.path.join(out_dir, obj)
        if not args.force and os.path.exists(obj_path) and \
                os.path.getmtime(obj_path) >= max(
                    os.path.getmtime(source), headers_mtime):
            continue
        jobs.append((os.path.basename(source), cmd))

    sqlite_obj = f"{OBJ_DIR}/sqlite3.obj"
    client_objs.append(sqlite_obj)
    sqlite_obj_path = os.path.join(out_dir, sqlite_obj)
    sqlite_path = os.path.join(REPO_DIR, "third_party", "sqlite", "sqlite3.c")
    if not args.only and (
            args.force or not os.path.exists(sqlite_obj_path) or
            os.path.getmtime(sqlite_obj_path) < max(
                os.path.getmtime(sqlite_path), os.path.getmtime(__file__))):
        jobs.append(("sqlite3.c", retarget(
            c_recipe(compile_template), kit["template_source"],
            kit["template_obj"], SQLITE_SOURCE, sqlite_obj)))

    res = f"{OBJ_DIR}/phi_client.res"
    client_objs.append(res)
    res_path = os.path.join(out_dir, res)
    rc_source = os.path.join(CLIENT_DIR, "phi_client.rc")
    # The resource includes the version header and icon. Recompile it when
    # either changes, even if phi_client.rc itself is untouched.
    rc_inputs_mtime = max(
        os.path.getmtime(rc_source),
        os.path.getmtime(os.path.join(CLIENT_DIR, "phi_version.h")),
        os.path.getmtime(os.path.join(CLIENT_DIR, "resources", "phi.ico")))
    if args.force or not os.path.exists(res_path) or \
            os.path.getmtime(res_path) < rc_inputs_mtime:
        jobs.append(("phi_client.rc", retarget(
            kit["rc_template"], kit["rc_template_source"],
            kit["rc_template_out"],
            "../../phibrowser-win/client/phi_client.rc", res)))

    failed = False
    if jobs:
        print(f"compiling {len(jobs)} file(s)...")
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=os.cpu_count()) as pool:
            def run(job):
                name, cmd = job
                proc = subprocess.run(cmd, shell=True, cwd=out_dir,
                                      capture_output=True, text=True,
                                      errors="replace")
                return name, proc
            for name, proc in pool.map(run, jobs):
                output = "\n".join(
                    line for line in (proc.stdout + proc.stderr).splitlines()
                    if not line.startswith("Note: including file:")
                    and line.strip())
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

    # Link: the recipe's inputs with the scaffold stub swapped for the real
    # client objects, and the output renamed to Phi.exe.
    # The scaffold links Chromium's version resource; Phi.exe carries its
    # own VERSIONINFO (phi_client.rc), and two resources of the same id are
    # a link error, so the Chromium one is dropped here.
    inputs = [i for i in kit["link_inputs"]
              if i not in kit["scaffold_objs"]
              and not i.endswith("chrome_exe_version.res")] + client_objs
    rsp_path = os.path.join(out_dir, "phi_clientkit/Phi.exe.rsp")
    # The same rsp shape as the framework's link rule: inputs, libs,
    # solibs, ldflags, then the rlibs (Rust code and its std — non-empty
    # only in a non-component build; the SDK carries them like any input).
    with open(rsp_path, "w") as f:
        f.write("\n".join(inputs) + "\n")
        f.write(kit["libs"] + " " + kit["solibs"] + " " +
                kit["ldflags"] + " " + kit.get("rlibs", "") + "\n")
    link = kit["link_command"].replace(kit["scaffold_exe"], "Phi.exe")
    link = re.sub(r'"@[^"]*"', f'"@{rsp_path}"', link)
    print("linking Phi.exe...")
    proc = subprocess.run(link, shell=True, cwd=out_dir,
                          capture_output=True, text=True, errors="replace")
    if proc.returncode != 0:
        print(proc.stdout + proc.stderr)
        sys.exit(1)
    print("imports: " + reorder_imports(
        os.path.join(out_dir, "Phi.exe"),
        os.path.normpath(os.path.join(out_dir, "..", ".."))))

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
    # Stage the vendored WinSparkle.dll beside the client: the sync ships
    # every top-level DLL, and the updater loads it from the exe's own dir.
    # The DLL must match the exe's architecture (third_party/winsparkle
    # carries the release's x64 and arm64 builds).
    winsparkle = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "third_party", "winsparkle",
                              target_machine(kit), "WinSparkle.dll")
    if not os.path.exists(winsparkle):
        sys.exit(f"no vendored WinSparkle.dll for {target_machine(kit)}: "
                 f"{winsparkle}")
    staged = os.path.join(out_dir, "WinSparkle.dll")
    if (not os.path.exists(staged) or
            os.path.getmtime(staged) < os.path.getmtime(winsparkle)):
        shutil.copy2(winsparkle, staged)
        print("staged WinSparkle.dll")

    print("CLIENT BUILD OK: " + os.path.join(out_dir, "Phi.exe"))


if __name__ == "__main__":
    main()
