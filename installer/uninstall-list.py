#!/usr/bin/env python3
# Copyright 2026 Phinomenon Inc.
# Licensed under the Apache License, Version 2.0; see the LICENSE file.
#
# Prints the NSIS lines that remove exactly the staged payload from $INSTDIR:
# one Delete per file, then one plain (non-recursive) RMDir per folder,
# deepest first, so a folder goes only once it is empty. phi-installer.nsi
# runs this at compile time (!system) and !includes the output into the
# uninstall section - the uninstaller never deletes by wildcard or RMDir /r
# in a folder the user may have picked.
#
# Usage: uninstall-list.py <staged payload dir>
import os
import sys


def nsis_path(rel):
    rel = rel.replace(os.sep, '\\')
    if '"' in rel:
        sys.exit(f'uninstall-list.py: unquotable name in the stage: {rel}')
    return '"$INSTDIR\\' + rel.replace('$', '$$') + '"'


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit('usage: uninstall-list.py <staged payload dir>')
    stage = sys.argv[1]
    files, dirs = [], []
    for root, dnames, fnames in os.walk(stage):
        rel = os.path.relpath(root, stage)
        if rel != '.':
            dirs.append(rel)
        for f in fnames:
            files.append(os.path.normpath(os.path.join(rel, f)))
    if not files:
        sys.exit(f'uninstall-list.py: {stage} is empty')
    for f in sorted(files):
        print('  Delete ' + nsis_path(f))
    for d in sorted(dirs, key=lambda d: (-d.count(os.sep), d)):
        print('  RMDir ' + nsis_path(d))


if __name__ == '__main__':
    main()
