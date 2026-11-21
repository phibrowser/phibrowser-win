# Non-component ship build — plan

Status: ADOPTED (2026-09-13, phases A-E done and box-verified; see the
progress log). Decides what
`windows-framework-design.md` §3 item 4 left open ("the shipping framework
should be `is_component_build=false` … decide at packaging time").

## Progress log

* **Phase A — done, box-verified on the component build.** The three
  signatures, rule 6, the sinks, `PhiWindowModel::Pull`, the client
  logging owner, the gate check, chrome_elf import reorder, the
  `framework` assembly rename (E0 pulled forward), `phi-client-resources.txt`.
  suite-core ×3, mc-matrix, Reader View, sv-drag 10/10, the armed
  cold-start plan (parked window) all pass. Rig facts learned: the replay
  needs `restoreLastSession` on, a command-line URL suppresses it, a
  force-kill leaves `exit_type=Crashed`; new box task PhiDevLaunch.
* **Phase B — done.** `out/PhiFxWinShip` built in ≈3 h 40 m at -j 20
  (peak RSS well under the 38 GB free). chrome.dll 308 MB, chrome.dll.pdb
  2.5 GB, 22 DLLs beside it, 19 GB on disk. Kit: no `-DCOMPONENT_BUILD`,
  import libs = chrome.dll.lib + chrome_elf.dll.lib only, base is
  `obj/base/base.lib`.
* **Phase C — done.** Two things the plan did not foresee: (1) the static
  link needs the **Rust rlibs** (base's rust_logger, cxx, serde, the Rust
  std) — the link rule's rsp ends with `${rlibs}`, which the kit never
  carried because a component build has none; kit + build-client.py now
  pass them through, and the SDK zip ships them. (2) **Switches the exe
  appends to its base::CommandLine never reach chrome.dll** in a static
  build (its own copy parses the process command line): `--no-first-run`,
  `--no-default-browser-check`, `--profile-directory=Default` were lost,
  and a first-run profile produced a browser window the framework never
  announced (the client sat at storesLoaded; `agentSpace.list` empty). The
  three switches now live in `ChromeMainDelegate::BasicStartupComplete`
  under IS_PHI_ABI_CLIENT. Phi.exe: 5.8 MB, imports chrome_elf.dll first
  then system DLLs only, assembly dep `framework`; the gate passes except
  the expected empty release key. Static link of the client ≈ 14 s.
* **Phase D — done.** phi-fx-ship synced (36 files). The FULL box sweep
  (18 suites, 81 cases) on the ship build has exactly the same result as
  on the component build, down to the same 14 pre-existing harness
  failures (menu/settings enumerations and pixel checks that drifted from
  the client — recorded, not chased). Two-Space cold restore parks the
  other window; mc-matrix, Reader View, sv-drag as before; client log
  lines land in chrome_debug.log, no stray debug.log; chrome://crash →
  Sentry event a90288e3… (PHIBROWSER-WIN-3) symbolicated through the
  static chrome.dll, module context chrome.dll + chrome_elf.dll only.
  Measured, warm cache: cold start to the shell window 1.0-1.1 s / 141
  modules / 206 MB working set (component: 1.3-1.6 s / 633 / 311 MB).
* **Phase E — done.** `make-installer.sh --from phi-fx-ship` →
  `stage-payload.ps1` (layout auto → framework; both box-side, replaced by
  the local build on 2026-09-15) → PhiBrowserSetup-0.1.0.exe
  **174 MB** (flat component build: 323 MB), sha256 f9ab8609908c7bf0…;
  LAN copy refreshed. Installed OVER the flat install on the box: the 548
  flat DLLs gone, `bin\` = Phi.exe + WinSparkle.dll + the client's
  resources\, `bin\framework\` = 34 files / 22 DLLs / framework.manifest /
  Chromium's resources\; launched from the install: chrome.dll from
  `bin\framework\chrome.dll`, 147 modules, client lines in the log, the
  crash handler carries the Sentry URL from the installer-dropped config.
  Runbook artifact, status doc, memory updated. Committed: fork
  7121035590, client f85db53.
* **2026-09-15 — Windows on ARM twin built and packaged** (§10):
  `out/PhiFxWinShipArm64`, `PhiBrowserSetup-<ver>-arm64.exe` via
  `make-installer.sh --arch arm64`. First install on a Windows 11 ARM VM:
  every renderer crashed at V8 start-up — the snapshot had been generated
  by an x64 mksnapshot (Linux host + win/arm64 target quirk; §10) — fixed
  with `v8_snapshot_toolchain = "//build/toolchain/linux:clang_x64_v8_arm64"`.
  Same day the installer build moved OFF the box: `make-installer.sh`
  stages from the out dir and runs the Linux `makensis` (stage-payload.ps1
  removed).
* **2026-09-14 — the component build is retired.** Development moves to
  the non-component tree only: `out/PhiFxWinShip` / `phi-fx-ship` are the
  defaults of every script (`build-fx.sh`, `build-fx-client.sh`,
  `sync-fx-win.sh`, `upload-symbols.sh`, `run-fx-*.sh`, `box.env`,
  `make-installer.sh`, `release-gate.py`), the box's PhiFxLaunch /
  PhiDevLaunch tasks and a "Phi Dev" desktop shortcut run
  `phi-fx-ship\Phi.exe` on `C:\phi-fx-test`. `out/PhiFxWin` and
  `phi-fx-dev` are left on disk but nothing points at them; the E4 "rebuild
  both" rule is void. Symbols go to Sentry only on request
  (`PHI_UPLOAD_SYMBOLS=1` for the sync hook).

## 0. Decision and constraints

* **Two framework configs, one client build.** `out/PhiFxWin` stays the
  component build (framework iteration relinks one small DLL). A new
  `out/PhiFxWinShip` is the same args with `is_component_build = false`:
  one fat `chrome.dll`, a handful of DLLs beside it. Evaluation installers
  and releases come from the ship dir only.
* **phibrowser-win stays a separate repo and a separate build.** No client
  source enters GN. `//chrome:phi_client` keeps being the recipe carrier
  (chrome.exe's link line around a stub), `phi_sdk_package.py` keeps
  exporting it as `phi_clientkit/kit.json`, `build-client.py` keeps
  replaying it. The only difference the client sees is that the ship kit's
  link inputs are thin static archives (`obj/base/base.lib`, …) instead of
  `.dll.lib` import libs, and its compile line lacks `-DCOMPONENT_BUILD`.
  Both kits live in their own out dir (own `phi_clientkit/obj`), so nothing
  is ever mixed.
* **Why this is feasible now** when the 2026-08 attempt was abandoned
  (`windows-framework-status.md` ~4550: "the client resolves base from
  chrome.dll.lib, which only exists as an exported surface in a component
  build"): that layout linked the client against the framework DLL. The kit
  links the client exactly like `chrome.exe`, which in a static build
  carries its own copy of base — that is how every official chrome.exe
  works.

## 1. The one thing that changes: no shared heap

Component build: one `base.dll`, one `libc++.dll`, one PartitionAlloc.
Anything allocated anywhere can be freed anywhere.

Ship build: `Phi.exe` and `chrome.dll` EACH contain base, libc++ and the
PartitionAlloc allocator shim. `free()` of a pointer that the other module
allocated goes to a PartitionAlloc root lookup on a foreign address and
dies immediately. It is not silent — but `std::string` up to 22 bytes never
allocates (SSO), so a crossing that only ever carried short strings passes
every test and crashes in the field on a long title.

The boundary is `chrome/browser/phinomenon/phi_framework/phi_framework_api.h`
(the whole contract; no other client↔chrome.dll link exists — Phi.exe
imports nothing from chrome.dll, `ChromeMain` is reached through
`MainDllLoader` with POD arguments, the delegate is fetched by
`GetProcAddress` on the exe). Contract rule 6, to be added beside rules 1-5:

> Nothing is allocated on one side and freed on the other. Parameters are
> POD, `const char*`, or `const T&` read during the call. Results are
> framework-owned scratch (rule 5) or are delivered through a sink the
> caller owns. `std::string`/`std::vector` layouts match across the boundary
> only because `build-client.py` replays the framework's exact compiler
> flags — a client built with different `_LIBCPP_*` settings is unsupported.

### Already safe

| Crossing | Why it is fine |
|---|---|
| The three vtable interfaces (`PhiFrameworkDelegate`, `PhiFramework`, `PhiDevToolsClient`) | Same compiler both sides; objects are owned and deleted by their creator; a virtual destructor deletes in the creator's module. |
| `OnWindowChanged(const WindowSnapshot&, …)` | Framework-owned for the duration of the call; `PhiCoordinator::NoteWindowSnapshot` copies with the client's allocator. |
| Every `const char*` in and out (rule 5 scratch), every POD struct (`WindowCreation`, `CreateRequest`, ids) | No ownership moves. |
| `PhiClientCreateDelegate` (the exe's one export, phi_client_main_win.cc:74), `ChromeMain` via `MainDllLoader` | chrome.exe's own pattern: POD arguments, resolved by `GetProcAddress` in both directions. |

### The three crossings that break (and their replacements)

1. `bool PhiFramework::Snapshot(WindowId, WindowSnapshot* out)`
   (phi_framework_impl.cc:1420, `BuildWindowSnapshot(browser, out)` assigns
   into client-owned strings and vectors). Five client call sites, all
   copying into a client-owned object: phi_kiosk_window.cc:252,
   phi_peek_panel.cc:524, phi_reader_panel.cc:228,
   `PhiShellWindow::PullSnapshot` (phi_shell_window.cc:2732, what the
   sidebar drag session pulls through) and `PhiWindowModel` refresh
   (phi_window_model.cc:29).

   → `virtual const WindowSnapshot* Snapshot(WindowId window) = 0;`
   Framework-owned scratch per rule 5 (a `WindowSnapshot` member on the impl,
   rebuilt per call); `nullptr` for an unknown window. One client helper in
   phi_window_model.h keeps the five sites one-liners:
   `bool PullSnapshot(PhiFramework*, WindowId, WindowSnapshot* out)` =
   `if (auto* s = fw->Snapshot(id)) { *out = *s; return true; }`. The copy
   allocates on the client side.

2. `void PhiFrameworkDelegate::OnColdStartRestorePlan(std::vector<int>* eager_window_ids, bool* armed)`
   (phi_framework_impl.cc:4542 `DispatchColdStartPlan`; client
   `PhiSessionRestore::OnColdStartRestorePlan` assigns
   `ghosts_.EagerWindowIds(...)` into the framework's vector).

   → a framework-owned sink, semantics unchanged (`*armed` stays the POD out
   parameter so the do-nothing default body stays empty):
   ```cpp
   class ColdStartPlan {            // framework-owned; valid during the callback
    public:
     virtual void AddEagerWindow(int32_t window_id) = 0;
    protected:
     ~ColdStartPlan() = default;
   };
   virtual void OnColdStartRestorePlan(ColdStartPlan* plan, bool* armed) {}
   ```
   `DispatchColdStartPlan` wraps its local vector in an adapter; the client
   loops `plan->AddEagerWindow(id)`.

3. `bool PhiFramework::DevToolsDiscoveryJson(const char* path, std::string* out)`
   (phi_framework_impl.cc:1577 → `PhiDevToolsTransport::DiscoveryJson`;
   client `phi_control_server.cc:879`, any non-UI thread, bounded UI hop).

   → the same sink shape, invoked synchronously on the calling thread before
   the call returns:
   ```cpp
   class PhiStringSink {
    public:
     virtual void Append(const char* data, size_t length) = 0;
    protected:
     ~PhiStringSink() = default;
   };
   virtual bool DevToolsDiscoveryJson(const char* path, PhiStringSink* out) = 0;
   ```
   The transport keeps its `std::string` and hands it to `Append` after the
   hop; the client's sink appends into its own `std::string doc`.

### Base process-globals: what the exe's private copy of base means

Verified by grep over `client/*.cc`: the client uses base for values/JSON,
strings, `FilePath`, `Time`, `Lock`, `RandUint64`, `Uuid`, `base64`, `sha1`,
`win::RegKey`, `PathService(DIR_EXE)`, `CommandLine`. No task runners, no
`ThreadPool`, no `RunLoop`, no `FeatureList` reads (the
`FailOnFeatureAccessWithoutFeatureList` guard is already the
`!COMPONENT_BUILD` idiom in phi_client_main_win.cc:302). All of these work
identically in a private copy. Two need attention:

* **Logging.** Today the client's `LOG`/`VLOG`/`PHI_LIFECYCLE` lines ride
  base.dll's logging state, which chrome.dll's `InitChromeLogging` points at
  `<user data dir>\chrome_debug.log` under `--enable-logging` — the file the
  feedback report zips (`phi_feedback_outbox.cc:39`). With a private base,
  base's Windows default is `LOG_TO_FILE` into `<exe dir>\debug.log`: a stray
  file beside Phi.exe and no client lines in any support bundle. Fix in
  phi_client_main_win.cc, browser process only, `#if !defined(COMPONENT_BUILD)`:
  `logging::InitLogging` with `LOG_TO_STDERR` (nothing, harmless for a
  windowed exe) before `ChromeMain`, then re-init to the same destination
  `InitChromeLogging` computes (file only under `--enable-logging`, the user
  data dir's `chrome_debug.log`, `APPEND_TO_OLD_LOG_FILE`) from
  `OnFrameworkReady` — after chrome.dll has deleted/rotated the old file, so
  the two writers never fight over it (base allows exactly one re-init:
  logging.h:208). `SetLogItems(pid, tid, timestamp)` to
  match Chromium's line format; `--v`/`--vmodule` are read from the exe's own
  CommandLine by `InitLogging`, so VLOG levels follow automatically.
* **Crash keys / Crashpad.** `chrome_elf.dll` (the primary module, still a
  DLL in the ship build) starts Crashpad for the browser process; the exe
  and chrome.dll keep separate annotation lists and the handler reads every
  module's — chrome.exe/chrome.dll behave the same in every official build.
  Nothing to do; verify with the Sentry crash test in phase D.

## 2. Phase A — boundary hardening (component dir, no new build)

Everything here builds and is testable in `out/PhiFxWin` today, so the
contract is proven before the 3½-hour build exists.

* A1 fork: the three signatures above in phi_framework_api.h, rule 6 in the
  contract comment, `ColdStartPlan` and `PhiStringSink` next to
  `PhiDevToolsClient`.
* A2 fork impl: `Snapshot` → scratch member; `DispatchColdStartPlan` →
  vector adapter; `DevToolsDiscoveryJson` → transport appends into the
  sink.
* A3 client: `PullSnapshot` helper + the 5 sites listed in §1;
  `PhiFrameworkDispatcher` +
  `PhiSessionRestore::OnColdStartRestorePlan` (phi_ghost_spaces.h:82
  comment); `phi_control_server.cc:879` sink.
* A4 client: the logging init described above.
* A5 gate: `tools/release-gate.py` gains `check_api_boundary()` — strip
  comments from phi_framework_api.h, fail on any non-const pointer or
  non-const reference to a `std::` type in a parameter list and on any
  virtual returning a `std::` type by value. Struct FIELDS of `std::` types
  stay allowed (they only ever cross as `const&`/`const*`). No allowlist.
* A6 verify on the box (targeted probes, not the suites): Kiosk, Peek,
  Reader open (pull sites); sidebar drag reorder + split drop; cold start
  with saved Spaces (armed plan → parked windows materialize); `/json/version`
  and `/json/list` over the pipe plus one CDP session; a feedback report
  whose logs.zip contains a client line.
* A7 commit fork + client (on ask). Both remain valid for the component
  build, so nothing else waits on this.

## 3. Phase B — the ship out dir

* B1 `out/PhiFxWinShip/args.gn` — PhiFxWin's file with one change and one
  explicit non-change:
  ```
  is_component_build = false   # ship: one chrome.dll; the exe carries its own base (docs/static-build-plan.md)
  is_official_build  = false   # PGO/ThinLTO is a separate decision (§7); not needed for a working static build
  ```
  (`is_debug = false`, `dcheck_always_on = false`, `symbol_level = 1`,
  `is_phi_browser`, `is_phi_abi_client`, codecs, keys — unchanged.)
* B2 `gn gen out/PhiFxWinShip` with build-fx.sh's env
  (`DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL=<PHI_ROOT>/win_sdk`,
  `GYP_MSVS_HASH_e66617bc68=800603caaa`). The 2026-08 gn-gen breakage was
  the missing SDK on disk; it is real now.
* B3 `build-fx.sh`: `OUT=${PHI_OUT:-out/PhiFxWin}`, log
  `/tmp/fx-build-<dirname>.log`. Default behaviour unchanged.
* B4 build: `PHI_OUT=out/PhiFxWinShip ./build-fx.sh chrome phi_client -j 20`.
  Full compile ≈ 3 h 20 m (every TU recompiles: the export macros expand
  differently) plus the static chrome.dll link (≈ 5-10 min, 10-15 GB RSS
  peak). Precheck `free -g` and top RSS — the 2026-09-12 -j 10 build
  OOM-killed a 36 GB ComfyUI; do not start with it resident. Disk ≈ 30 GB
  (216 GB free). Expect chrome.dll ≈ 200-250 MB, chrome.dll.pdb 2-4 GB,
  about ten DLLs beside it.
* B5 kit: `python3 phi_sdk_package.py --kit-only --out out/PhiFxWinShip`,
  then check it is really static:
  ```
  python3 -c 'import json;k=json.load(open("out/PhiFxWinShip/phi_clientkit/kit.json"));
  print([i for i in k["link_inputs"] if i.endswith(".dll.lib")]);
  print("-DCOMPONENT_BUILD" in k["compile_template"])'
  ```
  Expected: only `chrome.dll.lib` (unreferenced, from the scaffold's
  `:chrome_dll` dep) and `chrome_elf.dll.lib`; `False`.

## 4. Phase C — the client against the ship kit

* C1 `build-fx-client.sh`: honour `PHI_OUT` (pass `--out` to both
  phi_sdk_package.py and build-client.py). build-client.py already takes
  `--out`.
* C2 `PHI_OUT=out/PhiFxWinShip ./build-fx-client.sh -f`. Expected failure
  modes and their fixes: an unresolved symbol = the client uses something
  outside chrome.exe's static closure (zlib, base64, sha1, uuid, rand are
  inside it) → add the dep to `//chrome:phi_client` in chrome/BUILD.gn,
  never a library to the client; a duplicate-symbol error = a client TU
  defines something base also defines statically → rename on the client
  side. Link time grows from ~2 s to ~15 s (base is linked in).
* C3 inspect: `llvm-readobj --coff-imports out/PhiFxWinShip/Phi.exe` — must
  list chrome_elf.dll and system DLLs only (today: base.dll, chrome_elf.dll,
  sandbox_policy.dll, absl, partition_alloc, perfetto, libc++, zlib, …).
  `tools/release-gate.py --out out/PhiFxWinShip`.
* C4 import order: static chrome.exe is post-processed by
  `//chrome:reorder_imports` (`build/win/reorder-imports.py`, pefile) so
  chrome_elf.dll is the FIRST import and its blocklist/crash hooks are in
  place before anything else loads. Phi.exe never had this (base.dll loads
  first today). Add the same step to build-client.py after the link (the
  script and `third_party/pefile_py3` are in the tree; add both to
  phi_sdk_package.py's `WHOLE_TREES` for the SDK zip). Cheap, and it makes
  Phi.exe indistinguishable from chrome.exe in the loader.
* C5 `upload-symbols.sh`: `OUT=${PHI_OUT:-…}`; on the ship dir `--all` is
  ~10 modules, so make it the default for that dir (chrome.dll.pdb is the
  big one; sentry-cli skips what the server already holds).

## 5. Phase D — box verification

* D1 `sync-fx-win.sh`: `SRC=${PHI_OUT:-…}`, `REMOTE_DIR=${PHI_REMOTE_DIR:-phi-fx-dev}`;
  ship → `phi-fx-ship`. A fresh remote dir is mandatory: the sync never
  deletes, so a static tree pushed into phi-fx-dev would sit on 548 stale
  DLLs and make-installer.sh would package them. The AppContainer ACL grant
  and the Phi.exe symbol hook follow the variable.
* D2 box task `PhiShipLaunch` — a clone of PhiFxLaunch pointing at
  `C:\Users\<you>\phi-fx-ship\Phi.exe` (mk-eval-task.ps1 is the template
  for cloning a task from XML).
* D3 smoke (targeted, in this order): cold start with saved Spaces;
  window/tab/Space operations; Kiosk, Peek, Reader; sidebar drag reorder and
  split drop; the agent pipe `/json/version`, `/json/list`, one CDP session;
  feedback report with logs.zip carrying a client line; `chrome://crash` →
  Sentry event symbolicated through the ship symbols; updater smoke
  (WinSparkle.dll is loaded from the exe dir — unchanged).
* D4 the box suites (`test/suites.txt`) — run when asked.
* D5 measure for the runbook: cold-start time and loaded-module count, ship
  vs component (the component build loads ~550 DLLs).

## 6. Phase E — packaging and release tooling

### The installed tree: the framework under a folder

Today `File /r "${PHI_STAGE}\*.*"` (phi-installer.nsi:71) drops the flat
staging dir into `%LOCALAPPDATA%\PhiBrowser\bin\`: Phi.exe beside 548 DLLs,
the paks, locales and everything else. With the ship build the installed
tree becomes Chrome's own `Application\chrome.exe` + `Application\<version>\`
layout:

```
%LOCALAPPDATA%\PhiBrowser\
  Uninstall.exe
  bin\
    Phi.exe
    WinSparkle.dll              the updater loads it from the exe dir (phi_updater.cc:98)
    resources\                  the client's own: emoji catalog, strings, fonts, skill, onboarding (DIR_EXE)
    framework\
      framework.manifest        the private-assembly manifest naming chrome_elf.dll
      chrome.dll
      chrome_elf.dll  chrome_wer.dll        must stay together (chrome_crash_reporter_client_win.cc:237)
      chrome_100_percent.pak  chrome_200_percent.pak  resources.pak
      icudtl.dat  snapshot_blob.bin  locales\  MEIPreload\
      libEGL.dll  libGLESv2.dll  vk_swiftshader.dll  vulkan-1.dll
      d3dcompiler_47.dll  dxcompiler.dll  dxil.dll
      resources\                Chromium's: accessibility, inspector_overlay
      notification_helper.exe … (the secondary exes, where upstream keeps them)
```

Why it needs no runtime tricks — all three mechanisms are already compiled
into Phi.exe through the kit:

1. `main_dll_loader_win.cc:86` — `GetModulePath` probes
   `<exe dir>\<folder>\chrome.dll` FIRST and falls back to the exe dir, so
   one Phi.exe serves both the installed (foldered) and the dev-box (flat)
   tree.
2. The version assembly: the kit's link line embeds
   `version_assembly.manifest`, which declares a dependency on a private
   assembly; `<folder>\<folder>.manifest` (built today as
   `out/PhiFxWin/150.0.7871.47.manifest`) lists `chrome_elf.dll`, so the
   loader resolves the exe's one static import from inside the folder. The
   flat dev tree works for the same reason: that manifest sits beside the
   exe, and sync-fx-win.sh already ships `*.manifest`.
3. Every resource path (paks, `locales/`, `icudtl.dat`, `snapshot_blob.bin`,
   `MEIPreload/`, the GL/Vulkan/DXC DLLs) derives from `DIR_MODULE` /
   `DIR_ASSETS`, which in a static build is chrome.dll's directory because
   base lives inside chrome.dll. Child processes are `Phi.exe --type=…`
   through the same loader.

Why it cannot be done with the component build: Phi.exe statically imports
base.dll, libc++.dll, sandbox_policy.dll and nine more, and Windows resolves
static imports from the exe's directory before any of our code runs — the
wall the 2026-08 attempt hit (windows-framework-status.md ~4600).

Folder name: `150.0.7871.47` needs no fork change; `framework` needs two
small ones — the assembly `name=` in `chrome/app/version_assembly/*.template`
(the version attribute stays) and `GetModulePath` probing `framework` under
`is_phi_abi_client`. Use `framework`: a version-named folder changes every
release and confuses the rollback copy.

* E0 fork: the two `framework` changes above (chrome/app/version_assembly
  templates, main_dll_loader_win.cc — the latter is compiled into Phi.exe by
  the kit, so a client rebuild follows).
* E1 `make-installer.sh --from phi-fx-ship` (default stays phi-fx-dev), and
  the staging split by owner: top level = `Phi.exe`, `WinSparkle.dll`, the
  client's `resources/` files (`RESOURCES` + the trees build-client.py
  syncs); `framework\` = everything else in the runtime set, including
  Chromium's `resources/` (accessibility, inspector_overlay). This ends
  today's shared `resources/` dir where client and Chromium files are mixed.
  Expect the setup exe to shrink well below today's 323 MB (LZMA over one
  chrome.dll compresses far better than 548 DLLs + their import glue) —
  measure, don't guess.
* E1b phi-installer.nsi: `File /r` from the two staging roots; the live-tree
  probe (`FileOpen "$INSTDIR\chrome.dll"`, lines 46/50) and the rollback copy
  get the `framework\` prefix; the AppContainer icacls on `$INSTDIR` already
  covers subfolders (OI)(CI). The eval smoke (`eval-smoke.ps1`) checks the
  new paths. The dev box tree `phi-fx-ship` stays flat; the installer smoke
  is where the foldered layout is proven, because that is what ships.
* E2 `release-gate.py`, `upload-symbols.sh`, `build-fx*.sh`: defaults
  unchanged; the runbook names `out/PhiFxWinShip` for every release step.
* E3 docs: the Phi Win Release Runbook artifact (Build section), this file
  → status "ADOPTED", a windows-framework-status.md entry, README build
  section, memory `phi-win-eval-build`.
* E4 policy: dev = component (`out/PhiFxWin`, phi-fx-dev), evaluation and
  release = ship (`out/PhiFxWinShip`, phi-fx-ship). A fork change is not
  release-ready until the ship dir rebuilt it (incremental: touched TUs +
  the chrome.dll relink, minutes, not hours) and D3 passed.

## 7. Deferred, deliberately

* `is_official_build = true`: PGO (win64 profile via
  `tools/update_pgo_profiles.py`), ThinLTO (hours of link, >30 GB RAM),
  `/OPT:REF`. Worth it for a public build; not needed to leave the
  component build behind.
* SDK zip size: a static kit's thin archives drag their member `.obj` files
  into the zip (`thin_members`); if that gets unwieldy, bundle them with a
  `complete_static_lib` target later.
* `out/PhiWinDev` (Aug 8, pre-split component dir, 27 GB) is dead weight —
  reclaim when convenient.

## 8. Risks

| Risk | Handling |
|---|---|
| Latent heap-mismatch bug hidden by SSO strings | Rule 6 + the gate check (A5); the boundary is one header, greppable. |
| Build OOM | Precheck RSS; never start the ship build with ComfyUI resident. |
| Two dirs drift (fork change verified only on component) | E4 policy; ship incremental rebuilds are minutes. |
| A client symbol outside chrome.exe's static closure | Fix in `//chrome:phi_client` deps (C2), keeps the client library-free. |
| Client log lines vanish from support bundles | A4, verified by the feedback-report step in A6/D3. |
| gn gen fails on the new dir | The SDK is on disk since 2026-08; if it recurs the cause is the toolchain probe, not the args. |

## 9. Effort

Phase A is a small day of code (three fork functions, ~8 client sites, one
logging block, one gate check) verifiable immediately. Phase B is a ~4 h
unattended build. C-D a few hours of build/inspect/box work. E half a day
(the two folder-name fork changes, the staging split, the NSIS paths, one
install smoke). About two elapsed days, most of it the build.

## 10. Windows on ARM (arm64)

One more ship tree, same recipe, second architecture. Nothing in the
client or the framework's Phi code is architecture-specific (no
intrinsics, no inline asm — checked); the whole port is toolchain,
naming and packaging.

* **Toolchain.** The packaged VS 2026 toolchain (`vs_files/800603caaa`,
  MSVC 14.51.36231 + SDK 10.0.26100.8249) was made with
  `package_from_installed.py --noarm`, so it had no arm64 libraries at
  all. The arm64 side is GRAFTED into the same directory (additions only;
  `800603caaa/ARM64-GRAFT.txt` lists every piece and its source): the
  MSVC arm64 static + dynamic CRT, ATL and redist DLLs from the VS 18.10
  feed's `Microsoft.VC.14.51.*.ARM64.*` vsix payloads (they unpack into
  the very same `14.51.36231` toolset directory), the SDK's
  `Lib/um|ucrt|ucrt_enclave/arm64`, `bin/10.0.26100.0/arm64`,
  `Redist/D3D|MBN/arm64` from the `Win11SDK_10.0.26100` MSIs (an
  administrative image made with `msiexec /a` on the box, then pulled
  back), `Debuggers/arm64` (dbghelp/dbgcore/symsrv) from the WinDbg
  arm64 package — the SDK feed VS uses carries no debugger MSIs, and
  `vs_toolchain.py copy_dlls` (run by gn gen) hard-requires
  `Debuggers/<cpu>/dbghelp.dll` — a generated `SetEnv.arm64.json`, and a
  `VC/Redist -> redist` symlink (vs_toolchain.py spells it with a
  capital, the package does not). Lowercase twins of the SDK libs, as the
  x64 dirs have (the package sits on a plain case-sensitive directory
  here, not the ciopfs mount Chromium expects). `build/win_toolchain.json`
  already listed `sysarm64` as the third runtime dir.
* **Out dir.** `out/PhiFxWinShipArm64` = the ship `args.gn` with
  `target_cpu = "arm64"` AND `v8_snapshot_toolchain =
  "//build/toolchain/linux:clang_x64_v8_arm64"`. The second line is the
  one that makes the build RUN: `v8/gni/snapshot_toolchain.gni` hands a
  win/arm64 target's mksnapshot to the plain host toolchain (a rule
  written for Windows x64 hosts, where that toolchain's V8 still targets
  arm64 through the simulator), but on a Linux host
  `gcc_toolchain.gni` pins the host toolchain's `v8_current_cpu` to x64
  — so the first arm64 build shipped an **x64 snapshot and x64 embedded
  builtins inside the arm64 chrome.dll** (both dirs' `snapshot_blob.bin`
  were byte-for-byte the same size), and every renderer died at V8
  start-up: `ReadOnlyDeserializer::PostProcessNewObjects →
  ReadOnlyHeapObjectIterator::Next → HeapObject::SizeFromMap` reading a
  map word of `0x80808080` (EXCEPTION_ACCESS_VIOLATION_READ at
  `0x…80808083`, Sentry PHIBROWSER-WIN-4, 53 identical dumps). The
  simulator toolchain is what every other arm64 cross-compile uses; with
  it `clang_x64_v8_arm64/mksnapshot` is built with `V8_TARGET_ARCH_ARM64`.
  Build: `PHI_OUT=out/PhiFxWinShipArm64
  ./build-fx.sh chrome phi_client`, client `PHI_OUT=out/PhiFxWinShipArm64
  ./build-fx-client.sh` (the kit's link recipe carries `/MACHINE:ARM64`;
  build-client.py stages the WinSparkle.dll matching that machine —
  `third_party/winsparkle/arm64/` is vendored from the same 0.9.4
  release zip as the x64 one). The Rust std is built from source per
  target as before (`local_rustc_sysroot`).
* **Deploy.** `PHI_OUT=out/PhiFxWinShipArm64 PHI_REMOTE_DIR=phi-fx-ship-arm64
  ./sync-fx-win.sh` → the box's `phi-fx-ship-arm64` — a STAGING copy only:
  the box is x64 and cannot run it. It runs on the user's Windows 11 ARM
  VM (Apple silicon Mac); its Crashpad dumps land in
  `%LOCALAPPDATA%\Phi\User Data\Crashpad\reports\*.dmp`, and
  `~/.cargo/bin/minidump-stackwalk --symbols-path <store> x.dmp` walks
  them here with a breakpad store made by `dump_syms --store <store>
  out/PhiFxWinShipArm64/{Phi.exe,chrome.exe,chrome_elf.dll,chrome.dll}`
  (rust-minidump + mozilla dump_syms, both `cargo install`ed 2026-09-15;
  Sentry keeps no minidump and the org token cannot read events).
* **Installer.** `make-installer.sh --arch arm64` → stages
  `out/PhiFxWinShipArm64` into `installer/stage/arm64` (reads Phi.exe's and
  chrome.dll's PE machine, refuses a mismatch, drops the x64 msdia140.dll)
  → `makensis -DPHI_ARCH=arm64` → `installer/dist/PhiBrowserSetup-<ver>-arm64.exe`.
  The x64 name stays `PhiBrowserSetup-<ver>.exe`. SINCE 2026-09-15 THE
  INSTALLER IS BUILT ON THE LINUX MACHINE (the Debian `nsis` package;
  makensis on POSIX maps the script's `\` paths, and resolves a bare
  `OutFile` against the .nsi's directory — hence `-DPHI_OUTDIR`); the
  box-side staging (`stage-payload.ps1`, robocopy, the portable NSIS under
  `C:\Users\<you>\nsis`) is gone, the box is no longer part of the release
  path. The NSIS stub is 32-bit x86 either way and
  runs on every Windows; `.onInit` checks the MACHINE (`x64.nsh`
  `${IsNativeARM64}`): the arm64 installer refuses a non-ARM PC, the x64
  one warns on an ARM PC (its build runs there through emulation) and
  continues silently (`/SD IDYES`) so a WinSparkle-driven x64 update on an
  ARM PC still goes through. Same install folder, registry keys and
  Apps & Features entry for both, so either installer upgrades the other.
* **Updates.** `make-appcast.sh` tags each enclosure
  `sparkle:os="windows-x64"` / `"windows-arm64"` and can put both in the
  one item (`--arm64-setup/--arm64-url`); WinSparkle 0.9.4 picks the
  enclosure by the running process's architecture. The appcast rig's
  existing feeds stay x64-only until an arm64 setup is added to them.
* **Symbols.** `PHI_OUT=out/PhiFxWinShipArm64 ./upload-symbols.sh` when
  asked; Sentry symbolicates arm64 PE/PDB like x64.
* **Status (2026-09-15).** Toolchain grafted; `out/PhiFxWinShipArm64`
  built in ≈3 h 25 m of compile at -j 20 after one restart (the first run
  died 25 min in on `lld-link: could not open 'Cfgmgr32.lib'` — the x64
  lib dir had accumulated exact-case copies for the spellings Chromium's
  `#pragma comment(lib)` lines use; every x64 spelling is now hard-linked
  in the arm64 dir too). chrome.dll 280 MB (x64: 308), 19 GB on disk;
  Phi.exe 5.0 MB linked `/MACHINE:ARM64`, chrome_elf.dll already import
  slot 0. All 27 shipped binaries are ARM64; the payload imports only
  msvcp140.dll + vcruntime140.dll (both shipped as arm64;
  vcruntime140_1.dll is x64-only in Chromium's copy list and nothing
  imports it). Box: `phi-fx-ship-arm64` (789 files, 558 MB);
  `PhiBrowserSetup-0.1.0-arm64.exe` 159 MB (sha256 6d5aae85…), the x64
  one rebuilt with the same script (174 MB, payload unchanged), both in
  the box's Downloads. Release gate on the arm64 dir: same as x64 (only
  the empty release key). NOT DONE: running it — there is no Windows on
  ARM machine; the installer's machine gate has not been exercised either
  (on the x64 box `PhiBrowserSetup-0.1.0-arm64.exe /S` should exit 1 and
  touch nothing).

## 11. Authenticode signing

Every PE the installer ships, the setup exe and the uninstaller carry an
Authenticode signature; the appcast's ed25519 `sparkle:edSignature` is a
separate, already-existing gate (WinSparkle's) and stays as it is.

* **Tool.** `phibrowser-win-build/sign-pe.sh` (osslsigncode, on the Linux
  build host): signs in place, SHA-256, RFC 3161 timestamp, skips a PE
  that already carries a signature — the Microsoft-signed redistributables
  in the payload (vcruntime140, msvcp140, dxil, d3dcompiler_47, dbghelp,
  …) are left alone. The identity comes from
  `phibrowser-win-build/phi-signing.env` (gitignored; keys in
  `phi-signing.env.example`): a PKCS#12 + password, the TSA URL, the
  description and URL the UAC prompt shows, and what `--verify` trusts.
* **Where.** `make-installer.sh` signs the staged `*.exe`/`*.dll` after the
  architecture check, passes `-DPHI_SIGN_CMD` to makensis, and verifies
  the setup exe afterwards; the script's `!finalize` signs the setup exe
  and `!uninstfinalize` (NSIS 3.08+, works under the Debian makensis)
  signs the uninstaller stub before it is packed — `WriteUninstaller`
  only drops that stub, so this is the one place `Uninstall.exe` can get
  its signature. Signing is on whenever the env file exists; `--no-sign`
  skips it and the log says `signing: none`. Signing leaves the CodeView
  debug id alone, so Sentry's symbols keep matching.
* **The TEST chain (LAN only).** `sign-pe.sh --make-test-cert` mints
  `signing-test/`: a TEST root CA and a code-signing leaf under it, the
  same two-tier shape as a CA-issued cert, and writes the env file. To
  Windows a chain ending in an unknown root is unsigned, so the root is
  imported on the test machines (`Import-Certificate -FilePath
  TEST-root.crt -CertStoreLocation Cert:\LocalMachine\Root`, elevated) —
  done on the x64 box 2026-09-16 (thumbprint 47701B2C…). Never ship it.
* **Verified 2026-09-16 (x64 box).** `Get-AuthenticodeSignature` reports
  `Valid` with the TEST signer and DigiCert's timestamp responder on the
  setup exe, and after a silent install (exit 0 — NSIS's CRC check
  tolerates the appended signature) on Phi.exe, WinSparkle.dll,
  Uninstall.exe, framework\chrome.dll, chrome_elf.dll and the helper exes;
  vcruntime140.dll still shows Microsoft's signature. The installed,
  signed Phi launches (PhiEvalLaunch, which now resolves the install
  folder from the uninstall key's `InstallLocation`).
* **Release identity.** Azure Artifact Signing (Microsoft's CA, identity
  validated once, short-lived certs, ~$10/month; eligible regions include
  the UK and EU) or a CA-issued OV certificate on a cloud HSM; either
  replaces the env file's PKCS#12 with a `jsign` invocation in
  `sign-pe.sh` — the callers do not change. SmartScreen's "unrecognized
  app" persists for a new signer until reputation accrues to that
  identity; that is why no public build goes out unsigned.
