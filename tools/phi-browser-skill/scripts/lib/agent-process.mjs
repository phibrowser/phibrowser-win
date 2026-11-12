// Copyright 2026 Phinomenon Inc.
//
// Which process is the driving agent: the ancestry walk helpers.mjs uses to
// name the agent on the app channel and to notice a round that was orphaned
// from its agent session.

import { execFileSync } from 'node:child_process'
import { basename } from 'node:path'

/**
 * The driving agent process's pid: the nearest ancestor that isn't a shell
 * or wrapper (for a heredoc, the agent above the tool shell). Null when ps
 * is unavailable, as it is on Windows.
 */
export function agentRootPid() {
  try {
    const out = execFileSync('/bin/ps', ['-axo', 'pid=,ppid=,comm='],
                             { encoding: 'utf8' })
    const table = new Map()
    for (const line of out.split('\n')) {
      const m = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(line)
      if (m) table.set(Number(m[1]), { ppid: Number(m[2]), comm: m[3] })
    }
    let pid = process.ppid
    for (let hops = 0; hops < 64 && pid > 1; hops++) {
      const proc = table.get(pid)
      if (!proc) break
      // Login shells report as "-zsh"; strip the marker before matching.
      const name = basename(proc.comm).replace(/^-/, '').toLowerCase()
      if (!PASSTHROUGH.has(name)) return pid
      if (proc.ppid === pid) break
      pid = proc.ppid
    }
  } catch {}
  return null
}

/**
 * Every live ancestor pid of this process, nearest first (from the parent up
 * to just below launchd). Empty when ps is unavailable. Used to judge
 * whether a pid the app resolved for a connection is genuinely above us —
 * the app's ancestry walk may stop on a different ancestor than
 * agentRootPid() does (wrapper binaries, helper bundles), and any ancestor
 * is proof the round is parented to what the app bound, not orphaned.
 */
export function ancestorPids() {
  try {
    const out = execFileSync('/bin/ps', ['-axo', 'pid=,ppid='],
                             { encoding: 'utf8' })
    const ppidOf = new Map()
    for (const line of out.split('\n')) {
      const m = /^\s*(\d+)\s+(\d+)/.exec(line)
      if (m) ppidOf.set(Number(m[1]), Number(m[2]))
    }
    const ancestors = []
    let pid = process.ppid
    for (let hops = 0; hops < 64 && pid > 1; hops++) {
      ancestors.push(pid)
      const next = ppidOf.get(pid)
      if (!next || next === pid) break
      pid = next
    }
    return ancestors
  } catch { return [] }
}

// Shell and wrapper processes skipped when walking ancestry for the agent
// root — the JS mirror of AgentPeerIdentity.passthroughNames plus the shells
// that sit between an agent and the tools it spawns.
const PASSTHROUGH = new Set([
  'sh', 'bash', 'zsh', 'dash', 'fish', 'csh', 'tcsh', 'ksh',
  'env', 'login', 'sudo', 'xargs', 'timeout', 'nohup', 'caffeinate', 'script',
  // Codex wraps sandboxed shell commands in seatbelt.
  'sandbox-exec',
])
