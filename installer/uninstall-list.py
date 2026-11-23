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
# --owned prints the other half (PHI-2117): one RMDir /r per client resource
# folder that is Phi's whole, by a name no one else would use. Setup clears
# them before it copies an update and the uninstaller before its list, so
# the files a later version renamed or stopped shipping (onboarding's image
# frames, phi-browser-skill's scripts, the old profile-card) do not stay in
# the program folder for good. A folder that went from the payload stays in
# OWNED for the installs that still have it. Every folder the stage puts in
# resources\ must be in OWNED or BY_NAME, so a new one is a decision here,
# not a silent leftover later.
#
# Usage: uninstall-list.py [--owned] <staged payload dir>
import os
import sys

# resources\ folders that are Phi's whole.
OWNED = ('onboarding', 'phi-browser-skill', 'profile-card')
# resources\ folders under a common name: a folder the user picked may have
# one of its own, so only the payload's files go, by name.
BY_NAME = ('fonts',)


def nsis_path(rel):
    rel = rel.replace(os.sep, '\\')
    if '"' in rel:
        sys.exit(f'uninstall-list.py: unquotable name in the stage: {rel}')
    return '"$INSTDIR\\' + rel.replace('$', '$$') + '"'


def owned(stage):
    res = os.path.join(stage, 'resources')
    found = sorted(d for d in os.listdir(res) if os.path.isdir(os.path.join(res, d)))
    new = [d for d in found if d not in OWNED + BY_NAME]
    if new:
        sys.exit('uninstall-list.py: resources\\%s is in neither OWNED nor BY_NAME'
                 % ', resources\\'.join(new))
    for d in OWNED:
        print('  RMDir /r ' + nsis_path(os.path.join('resources', d)))


def main():
    args = sys.argv[1:]
    only_owned = args[:1] == ['--owned']
    if only_owned:
        args = args[1:]
    if len(args) != 1 or not os.path.isdir(args[0]):
        sys.exit('usage: uninstall-list.py [--owned] <staged payload dir>')
    stage = args[0]
    if only_owned:
        return owned(stage)
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
