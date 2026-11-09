#!/usr/bin/env python3
# Copyright 2026 Phinomenon Inc.
#
# Use of this source code is governed by an Apache license that can be
# found in the LICENSE file.
"""Downloads the prebuilt Phi framework this client commit is built for.

The release is pinned in framework.lock at the repository root. The zip is
checked against the pinned SHA-256, then unpacked into the build directory,
next to where build-client.py puts phi_client.dll.

  python tools/fetch-framework.py [--out out\\Release] [--arch x64|arm64]
  python tools/fetch-framework.py --zip <local framework zip>
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(REPO, "framework.lock")
HEADER = os.path.join(
    REPO, "client", "compat", "chrome", "browser", "phinomenon",
    "phi_framework", "phi_framework_api.h")
INFO = "phi-framework.json"


def client_abi():
    with open(HEADER, encoding="utf-8") as f:
        match = re.search(r"kPhiFrameworkAbiVersion\s*=\s*(\d+)", f.read())
    if not match:
        sys.exit("cannot find kPhiFrameworkAbiVersion in the framework header")
    return int(match.group(1))


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(lock, arch, directory):
    if not lock.get("tag"):
        sys.exit("framework.lock names no release yet: pass --zip with a "
                 "framework zip")
    asset = lock["assets"].get(arch)
    if not asset:
        sys.exit(f"framework.lock has no {arch} framework")
    url = (f"https://github.com/{lock['repository']}/releases/download/"
           f"{lock['tag']}/{asset['name']}")
    path = os.path.join(directory, asset["name"])
    print(f"downloading {url}")
    with urllib.request.urlopen(url) as response, open(path, "wb") as f:
        shutil.copyfileobj(response, f)
    actual = sha256_of(path)
    if actual != asset["sha256"]:
        sys.exit(f"{asset['name']} has SHA-256 {actual}, framework.lock says "
                 f"{asset['sha256']}")
    return path


def unpack(zip_path, out_dir):
    root = os.path.realpath(out_dir)
    with zipfile.ZipFile(zip_path) as archive:
        for member in archive.infolist():
            target = os.path.realpath(os.path.join(root, member.filename))
            if os.path.commonpath([root, target]) != root:
                sys.exit(f"refusing unsafe path in the zip: {member.filename}")
        archive.extractall(root)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(REPO, "out", "Release"))
    parser.add_argument("--arch", choices=("x64", "arm64"), default="x64")
    parser.add_argument("--zip", help="use this framework zip, no download")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(LOCK, encoding="utf-8") as f:
        lock = json.load(f)
    with tempfile.TemporaryDirectory() as directory:
        unpack(args.zip or download(lock, args.arch, directory), args.out)
    with open(os.path.join(args.out, INFO), encoding="utf-8") as f:
        info = json.load(f)
    if info["abi"] != client_abi():
        sys.exit(f"this framework has ABI {info['abi']}, the client was "
                 f"written for {client_abi()}: use the release framework.lock "
                 "names")
    if info["arch"] != args.arch:
        sys.exit(f"this framework is {info['arch']}, not {args.arch}")
    print(f"framework {info['version']} ({info['arch']}) in {args.out}")


if __name__ == "__main__":
    main()
