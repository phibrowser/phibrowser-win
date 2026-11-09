#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Release gate for Phi.exe (docs/architecture-review.md §3.9 "Gating").

Asserts, on a BUILT exe and the tree it came from, that the developer-only
hooks are absent and the packaging is consistent. Every check is one the
review found violated at least once; each failure names the finding.

  tools/release-gate.py [--out <dir>] [--source-only] [--signer <subject>]
                        [--release]

Exit 0 = all checks pass. Run it from build-client.py's release path and
before cutting an installer. --source-only skips the binary checks.
"""

import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(REPO, "client")
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "out", "PhiFxWinShip")

# Strings that only exist in the exe when PHI_DEV_HOOKS was defined. They
# are the environment-variable NAMES the hooks read, which the compiler
# keeps verbatim; a release exe must not contain them.
DEV_HOOK_STRINGS = [b"PHI_APPCAST_URL", b"PHI_APPCAST_ED25519",
                    b"PHI_VAULT_HELPER",
                    # phi_auth.cc's EnvOr tenant / sync overrides.
                    b"PHI_AUTH0_DOMAIN", b"PHI_AUTH0_CLIENT_ID",
                    b"PHI_AUTH0_AUDIENCE", b"PHI_SYNC_URL",
                    b"PHI_SYNC_BASE_URL", b"PHI_ACCOUNT_BASE_URL"]

# PHI_WIN_CHANNEL values the gate knows how to pair with a feed and signer.
CHANNELS = ("stable", "canary", "nightly")


def fail(msg):
    print("FAIL: " + msg)
    return 1


def strings_in(path, needles):
    with open(path, "rb") as f:
        data = f.read()
    hits = []
    for n in needles:
        # Both narrow and UTF-16LE spellings: the switch name is narrow, the
        # env names are read through GetEnvironmentVariableA (narrow) but
        # check wide too so a future W-variant cannot slip through.
        wide = n.decode("ascii").encode("utf-16-le")
        if n in data or wide in data:
            hits.append(n.decode("ascii"))
    return hits


def check_binary(out_dir):
    rc = 0
    client = os.path.join(out_dir, "phi_client.dll")
    if not os.path.isfile(client):
        return fail(f"{client} missing — build first (or pass --source-only)")
    hits = strings_in(client, DEV_HOOK_STRINGS)
    if hits:
        rc |= fail("dev hooks compiled in (built with --dev-hooks?): "
                   + ", ".join(hits))
    # The switch NAME is not checked in the binary: Chromium's own switch
    # tables linked into the exe spell it too. The source check above is
    # the gate for the injection (review §4 item 1).
    res = os.path.join(out_dir, "client_obj", "phi_client.res")
    if os.path.isfile(res):
        with open(res, "rb") as f:
            blob = f.read()
        if "VS_VERSION_INFO".encode("utf-16-le") not in blob:
            rc |= fail("phi_client.res carries no VS_VERSION_INFO "
                       "(review §4 item 42)")
    host = os.path.join(out_dir, "Phi.exe")
    if os.path.isfile(host) and strings_in(host, DEV_HOOK_STRINGS):
        rc |= fail("the host exe carries developer hooks")
    if rc == 0:
        print("ok: binary")
    return rc


def check_sources():
    rc = 0
    # Installer and rigs must name the exe the build produces.
    installer_dir = os.path.join(REPO, "installer")
    for root, _, files in os.walk(installer_dir):
        for name in files:
            if not name.endswith((".nsi", ".sh", ".ps1")):
                continue
            path = os.path.join(root, name)
            with open(path, errors="replace") as f:
                if "phi_client.exe" in f.read():
                    rc |= fail(f"{os.path.relpath(path, REPO)} references "
                               "phi_client.exe; the build produces Phi.exe "
                               "(review §4 item 8)")
    main_cc = os.path.join(CLIENT, "phi_client_main_win.cc")
    with open(main_cc, errors="replace") as f:
        if re.search(r'"remote-debugging-port"', f.read()):
            rc |= fail("phi_client_main_win.cc still injects "
                       "remote-debugging-port (review §4 item 1)")
    # Every environment override must sit behind kPhiDevHooks.
    for name in ("phi_updater.cc", "phi_coordinator.cc"):
        path = os.path.join(CLIENT, name)
        with open(path, errors="replace") as f:
            src = f.read()
        for env in ("PHI_APPCAST_URL", "PHI_APPCAST_ED25519",
                    "PHI_VAULT_HELPER"):
            idx = src.find(f'"{env}"')
            if idx < 0:
                continue
            window = src[max(0, idx - 1500):idx]
            if "kPhiDevHooks" not in window:
                rc |= fail(f"{name}: {env} is read without a kPhiDevHooks "
                           "gate nearby (review §4 items 3, 4)")
    # The update feed must be signed: with kEdDsaPublicKey empty the client
    # turns its updater off (phi_updater.cc), so a release built this way
    # ships without updates — mint the key (installer/make-appcast.sh says
    # how) before cutting one.
    updater_cc = os.path.join(CLIENT, "phi_updater.cc")
    with open(updater_cc, errors="replace") as f:
        m = re.search(r'kEdDsaPublicKey\[\]\s*=\s*"([^"]*)"', f.read())
    if not m:
        rc |= fail("phi_updater.cc: kEdDsaPublicKey not found")
    elif not m.group(1):
        rc |= fail("phi_updater.cc: kEdDsaPublicKey is empty — the updater "
                   "stays off without the appcast signing key")
    # Analytics ship opt-in: the model's default must be off (review §3.9).
    model_h = os.path.join(CLIENT, "phi_app_model.h")
    if os.path.isfile(model_h):
        with open(model_h, errors="replace") as f:
            if not re.search(r"analytics_enabled\s*=\s*false", f.read()):
                rc |= fail("phi_app_model.h: analytics_enabled must default "
                           "to false (review §4 item 32)")
    # The agent-facing verb reference is generated from the C++ table; a
    # stale one is a contract the skill cannot trust (review §3.7 item 4).
    check_verbs = os.path.join(REPO, "tools", "check-verbs-md.sh")
    if os.path.isfile(check_verbs):
        proc = subprocess.run(["bash", check_verbs], capture_output=True,
                              text=True)
        if proc.returncode != 0:
            rc |= fail("references/verbs.md is stale — run "
                       "tools/gen-verbs-md.py")
    rc_path = os.path.join(CLIENT, "phi_client.rc")
    with open(rc_path, errors="replace") as f:
        if "VS_VERSION_INFO" not in f.read():
            rc |= fail("phi_client.rc has no VS_VERSION_INFO")
    if rc == 0:
        print("ok: sources")
    return rc


def check_channel(signer, release):
    """PHI_WIN_CHANNEL, the appcast feed and the signing identity must name
    the same ring: a stable build reads a stable feed and is signed with a
    real certificate; a canary/nightly build reads a pre-release feed
    (win-nightly) and may carry the TEST identity. --release additionally
    demands the stable channel."""
    rc = 0
    with open(os.path.join(CLIENT, "phi_version.h"), errors="replace") as f:
        m = re.search(r'#define\s+PHI_WIN_CHANNEL\s+"([^"]*)"', f.read())
    if not m:
        return fail("phi_version.h: PHI_WIN_CHANNEL not found")
    channel = m.group(1)
    if channel not in CHANNELS:
        return fail(f"phi_version.h: PHI_WIN_CHANNEL \"{channel}\" is not one "
                    "of " + ", ".join(CHANNELS))
    with open(os.path.join(CLIENT, "phi_updater.cc"), errors="replace") as f:
        m = re.search(r'kDefaultAppcastUrl\[\]\s*=\s*((?:"[^"]*"\s*)+);',
                      f.read())
    if not m:
        return fail("phi_updater.cc: kDefaultAppcastUrl not found")
    feed = "".join(re.findall(r'"([^"]*)"', m.group(1)))
    prerelease_feed = re.search(r"nightly|canary", feed) is not None
    stable = channel == "stable"
    if release and not stable:
        rc |= fail(f"--release needs PHI_WIN_CHANNEL \"stable\", not "
                   f"\"{channel}\" (phi_version.h)")
    if stable and prerelease_feed:
        rc |= fail(f"channel stable reads the pre-release feed {feed} "
                   "(phi_updater.cc kDefaultAppcastUrl)")
    if not stable and not prerelease_feed:
        rc |= fail(f"channel {channel} reads the non-nightly feed {feed}; a "
                   "pre-release build would update from the stable ring")
    if stable:
        if not signer:
            rc |= fail("channel stable needs the signing identity checked — "
                       "pass --signer \"$(sign-pe.sh --info)\"")
        elif "TEST" in signer:
            rc |= fail(f"channel stable signed with the TEST identity "
                       f"({signer})")
    if rc == 0:
        print(f"ok: channel {channel}, feed {feed}"
              + (f", signer {signer}" if signer else ""))
    return rc


API_HEADER = "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"


def split_params(text):
    """Top-level comma split (template commas stay inside their <>)."""
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def check_api_boundary(header):
    """The framework/client boundary must not move ownership across modules
    (phi_framework_api.h contract rule 6; docs/static-build-plan.md §1): in
    a non-component build each side has its own allocator, so a std::
    container written, returned or destroyed on the other side is a crash.
    Allowed: POD, const char*, const T& / const T* read during the call,
    sink interfaces. Flagged: any std:: type (or a struct holding one)
    passed by value, by non-const pointer or by non-const reference, and any
    std:: type returned by value."""
    if not os.path.isfile(header):
        return fail(f"{header} missing — the gate needs the framework tree "
                   "(or SDK source twin) beside the out dir")
    with open(header, errors="replace") as f:
        src = f.read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    # The client may be built with a different standard library, so no std::
    # type can cross, however it is passed.
    if re.search(r"\bstd::", src):
        return fail("phi_framework_api.h names a std:: type; the boundary is "
                    "POD, const char* and const T& only (contract rule 6)")
    # Structs that carry std:: members: passing them mutably is the same
    # ownership move as passing the member.
    owning = set()
    for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\};", src, re.S):
        if "std::" in m.group(2):
            owning.add(m.group(1))
    owning_re = r"std::|\b(?:%s)\b" % "|".join(sorted(owning) or ["$^"])

    def type_of(decl):
        """'const T& name = x' -> 'const T&'; a bare return type is kept."""
        d = re.sub(r"\s*=.*$", "", decl.strip())
        m = re.match(r"^(.*?[*&>\w])\s+\w+$", d)
        return m.group(1) if m else d

    def moves_ownership(decl):
        t = type_of(decl)
        if not re.search(owning_re, t):
            return False
        # const T& and const T* are reads; everything else (by value,
        # T*, T&) writes or destroys on the wrong side.
        return not (t.startswith("const ") and t.endswith(("&", "*")))

    rc = 0
    for m in re.finditer(r"virtual\s+(.*?)\s+(\w+)\s*\((.*?)\)\s*"
                         r"(?:const\s*)?(?:override|=\s*0|\{)", src, re.S):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        ret = " ".join(ret.split())
        params = " ".join(params.split())
        bad = []
        if moves_ownership(ret):
            bad.append(f"returns {ret}")
        for p in split_params(params):
            if moves_ownership(p):
                bad.append(f"parameter '{p}'")
        if bad:
            rc |= fail(f"phi_framework_api.h: {name}() moves ownership "
                       "across the module boundary (contract rule 6): "
                       + "; ".join(bad))
    if rc == 0:
        print("ok: api boundary")
    return rc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--signer", default="",
                        help="Authenticode subject the build is signed with "
                        "(sign-pe.sh --info); required for channel stable")
    parser.add_argument("--release", action="store_true",
                        help="require PHI_WIN_CHANNEL stable")
    args = parser.parse_args()
    out_dir = os.path.abspath(args.out)
    rc = check_sources()
    rc |= check_channel(args.signer, args.release)
    # The out dir sits two levels under the source root (out/<name>), in a
    # checkout and in an unzipped SDK alike.
    rc |= check_api_boundary(
        os.path.join(REPO, "client", "compat", API_HEADER))
    if not args.source_only:
        rc |= check_binary(out_dir)
    print("RELEASE GATE " + ("PASSED" if rc == 0 else "FAILED"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
