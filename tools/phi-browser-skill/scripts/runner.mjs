#!/usr/bin/env node
// Copyright 2026 Phinomenon Inc.
//
// phi-browser heredoc runner: reads a script from stdin and executes it with
// all helpers in scope. Usage:
//   node runner.mjs <<'EOF'
//   const task = await ensureAgentSpace('my task')
//   cliLog(await snapshotText())
//   EOF

// The CDP client speaks WebSocket over the app's Unix socket with a
// hand-rolled frame codec (no global WebSocket needed), but still uses modern
// Node APIs throughout. Fail with the actual requirement instead of an obscure
// error from deep inside the first connect.
if (Number(process.versions.node.split('.')[0]) < 22) {
  console.error(
    `phi-browser: Node >= 22 required (running ${process.version})`)
  process.exit(2)
}

const { __dispose, ...surface } = await import('./lib/helpers.mjs')

// The heredoc body compiles as a plain async-function body inside this ES
// module: `import … from` statements can't appear there, and ESM has no
// ambient `require`. Provide one anchored at the caller's cwd, so
// require('node:fs') works and relative/node_modules lookups resolve the way
// a script run from that directory would.
const { createRequire } = await import('node:module')
const { join } = await import('node:path')
surface.require = createRequire(join(process.cwd(), '__phi-heredoc__.mjs'))

// Everything this process prints reaches the agent's context — scrub any
// secret the round handled out of ALL of it, not just cliLog.
const scrub = surface.__scrubSessionSecrets ?? ((t) => t)

// The heredoc body's ambient `console` writes would bypass cliLog's
// scrubber — shadow it with a scrubbing shim, injected as one more surface
// name so it wins inside the function body without touching the global.
const { inspect } = await import('node:util')
const scrubArg = (a) => scrub(typeof a === 'string' ? a : inspect(a))
surface.console = Object.assign(Object.create(console), {
  log: (...a) => console.log(...a.map(scrubArg)),
  info: (...a) => console.info(...a.map(scrubArg)),
  warn: (...a) => console.warn(...a.map(scrubArg)),
  error: (...a) => console.error(...a.map(scrubArg)),
  debug: (...a) => console.debug(...a.map(scrubArg)),
  trace: (...a) => console.trace(...a.map(scrubArg)),
})

// A floating rejection (or a throw from a fire-and-forget callback) kills
// Node WITHOUT running finally blocks: the busy badge would stick and the
// round-end keep-alive would never be granted, expiring the Space
// mid-conversation — and the default reporter bypasses the secret scrubber.
// Treat both like a normal failure: scrub, dispose, exit 1.
for (const event of ['unhandledRejection', 'uncaughtException']) {
  process.on(event, (err) => {
    console.error(scrub(`phi-browser ${event}: ${err?.message || err}`))
    __dispose().catch(() => {}).finally(() => process.exit(1))
  })
}

// A killed round (Bash-tool timeout, Ctrl-C) should still flip the Space's
// busy badge back to idle — best effort, the default handler would just die.
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => {
    __dispose().catch(() => {}).finally(() => process.exit(130))
  })
}

let source = ''
process.stdin.setEncoding('utf8')
for await (const chunk of process.stdin) source += chunk

if (!source.trim()) {
  console.error('phi-browser: empty script on stdin')
  process.exit(2)
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor
const names = Object.keys(surface)
const values = names.map((n) => surface[n])

let exitCode = 0
try {
  const fn = new AsyncFunction(...names, source)
  await fn(...values)
} catch (err) {
  console.error(scrub(`phi-browser error: ${err?.message || err}`))
  // A few stack frames locate the failing line inside the heredoc (the
  // script compiles as an anonymous async function, so frames read
  // "<anonymous>:LINE"). Skip the message line already printed above. The
  // body sits behind a 2-line Function-constructor wrapper, so V8 reports
  // heredoc line N as <anonymous>:N+2 — compensate before printing.
  const fixLines = (t) => t.replace(/<anonymous>:(\d+)/g,
    (m, n) => `<anonymous>:${Math.max(1, Number(n) - 2)}`)
  const frames = fixLines(String(err?.stack || '')
    .split('\n').slice(1, 6).join('\n'))
  if (frames) console.error(scrub(frames))
  exitCode = 1
} finally {
  await __dispose().catch(() => {})
}
process.exit(exitCode)
