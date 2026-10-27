# Windows-native UI — implementation log

Status: **in progress** (started 2026-09-17). The design is
`docs/windows-native-ui.md`; this file is the build log Jixiang reviews:
which phase landed, what each one changed, and every decision that was
taken without asking (§D — check these). Phases follow the design's §10
order; each leaves the box build usable.

Jixiang's brief (2026-09-17): "make a plan to impl the ui one by one; if
there is a decision you need me to make, decide yourself first, document
it; after you have implemented the full UI, show me the questions and
decisions and I will check." The box is mine for testing.

## Phase status

| Phase | What | State |
|---|---|---|
| P0 | tokens, type ramp, icon font, `PaintIconButton`, palette roles | done 2026-09-17 |
| P1 | app menu (⋯) replaces the menu bar; Fluent owner-drawn menus; Alt / F10 | done 2026-09-17, box-verified |
| P2 | the two layouts, flush page pane, Performance removed, floating panel | done 2026-09-17, box-verified (split-time row sheet, Esc → P6) |
| P3 | Spaces: pips, chip + switcher, two-line hover flyout | done 2026-09-17, box-verified |
| P4 | dialogs as ContentDialogs | done 2026-09-17, box-verified (scrim added the same day, f3f38f6) |
| P5 | Settings as a Windows Settings window | done 2026-09-17, box-verified (expander cards, rail search, remembered size: round B) |
| P6 | secondary windows, Kiosk, dark pass, polish | done 2026-09-17 (round D: Peek header, row tiles, incognito glyph, agent cues) |

## D. Decisions taken without asking (review these)

* **D2 — dialog button order: Windows** (affirmative left, Cancel right).
  The design's recommendation; dialogs are where Windows habit is
  strongest. Keyboard: Enter = default (never on destructive), Esc =
  Cancel.
* **D4 — floating panel trigger: edge hover + the toggle's right-click**
  menu (Show sidebar / Auto-hide sidebar). Left-edge hover slides the
  tucked panel in; the toggle button (ClosePane/OpenPane) docks it.
* **D5 — no Mica.** Flat Space tint. Not revisited.
* **D6 — menus stay HMENUs, owner-drawn.** Every menu (the app menu, its
  submenus, every context menu) is a Win32 popup menu with
  `MFT_OWNERDRAW` rows and a `MENUINFO` background brush, painted to the
  Fluent look (34 dip rows, 16 dip icon column, right-aligned chords in
  the tertiary ink, danger rows, accent check). Keyboard navigation,
  mnemonics, submenus, `TrackPopupMenu`'s modal loop and the remote
  `menu.list` / `menu.invoke` surface come for free; the popup frame
  (rounded, shadowed on Windows 11) is the OS's. A custom popup window
  would have cost all of that for a 1 px border colour.
* **D7 — the zoom row is a submenu**, `Zoom ▸ Zoom in · Zoom out · Actual
  size · ─ · Full screen`, with the current level in the parent row's
  chord column ("100 %"). The design allowed this fallback; an inline
  three-target owner-drawn row cannot take keyboard focus per target.
* **D8 — icon font chosen at start-up**: Segoe Fluent Icons when the
  face is installed, Segoe MDL2 Assets otherwise. No third fallback. The
  Space icon catalogue masks and the agent brand masks stay compiled.
* **D9 — Performance maps to Balanced at load** and on the wire:
  `PhiLayoutModeFromName("performance")` answers `kBalanced`; the enum
  keeps the value so the Mac's stored setting still parses. The Settings
  radio, the app menu's Layout submenu and the onboarding page show two
  choices. `agentSpace.layout.set {mode:"performance"}` is accepted and
  lands on balanced.
* **D10 — the remote menu paths use the app menu's English labels**
  (`["More tools","Task manager"]`, `["New tab"]`), as the design says;
  `test/suite-menu.mjs` is rewritten to them.
* **D11 — Chat stays out** (`kPhiAiEnabled` paths untouched, no menu rows
  for it). The Feedback pill stays in the sidebar bottom bar.
* **D13 — dark menu frames through uxtheme's undocumented
  `SetPreferredAppMode` / `FlushMenuThemes`** (ordinals 135 / 136,
  Windows 10 1903+), called when a themed menu opens. The documented API
  cannot recolour a popup menu's frame or its submenu arrows; every
  dark-mode Win32 app (Notepad++, Explorer++) uses this pair. A missing
  export leaves the frame light and the rows still ours.
* **D14 — the app menu's translations are authored in this repo**
  (`strings-source.json` `translations` blocks for de / es / fr / ja / ko
  / nl / zh-Hans / zh-Hant): the Mac catalogue has no sentence-case
  counterparts to join against. The translation repo can overwrite them.
* **D15 — the incognito row stays live** through Chromium's
  IDC_NEW_INCOGNITO_WINDOW route, as the menu bar's was (the design
  said "disabled until supported"; the route already works).
* **D17 — RETIRED (round B).** Settings › Spaces / Profiles are the §7.1
  / §7.2 expander cards now (see the round B notes). The one liberty:
  the expander hint reads "profile · Ctrl+n" — the settings host has no
  per-Space tab count, so "N tabs" is left out rather than plumbed.
* **D18 — RETIRED (round B).** The rail has "Find a setting": typing on
  any page but Shortcuts filters every page's rows by title and
  description into one results page, under per-page headings.
* **D21 — the Settings size is remembered as a preference**
  (`settingsWindowWidth` / `settingsWindowHeight` in PhiSettings.json,
  written on WM_EXITSIZEMOVE, never for a maximized window); no
  position, which Windows Settings also does not keep.
* **D31 — §4.8's mechanics beyond the vocabulary are deferred**:
  auto-scroll in the list's edge bands, Ctrl (keep the source) and Shift
  (force stacked) modifiers, the whole-group drag, the bookmarks bar as
  a drag source / target, the cross-window insertion slot, OLE drops
  from other apps, the pip lift at 50 %, and the hidden sidebar's edge
  reveal during a drag. Each is its own piece of session plumbing; the
  ghost / gap / line / ring / badge / snap vocabulary is in.
* **D32 — no ring on a Space pip while a tab hovers it**: the strip has
  no drop-highlight state yet; the badge and the closed list gap carry
  the cue.
* **D33 — the badge's labels are literals** (no `strings.json` keys
  yet), like the context menus were before round C1.
* **D34 — the ghost does not grow a caption strip** over the New window
  zone (§4.8 says it previews a window): the badge and the lit tile say
  "New window".
* **D26 — agent Spaces keep the app accent** on the graphite ground:
  the brand colour per agent (§5.10) needs a colour table beside the
  marks; the mark itself is the brand's.
* **D27 — no omnibox dim / eye glyph while the agent holds the tab**
  (§5.10 cue 3's last sentence); the ring, fog and capsule carry the
  held-input state.
* **D28 — the ring is steady**, not pulsing (60 → 100 % over 1.2 s):
  a pulse means recomposing a shaped layer on a timer; deferred.
* **D29 — no "Claude — Research" window title**: the shell never sets
  its own title today (nothing in the client calls SetWindowText on
  it), so cue 7 is a separate piece of plumbing.
  2026-10-19: the shell now titles itself after the active tab (Space
  name on the new-tab page; `RetitleShell`, `docs/mac-parity.md`); the
  agent-task title of cue 7 is still not done.
* **D30 — the expanded capsule keeps a Finish button** where §5.10 puts
  a ⋯ menu (Take control · ⋯ · chevron); the pip's agent menu already
  has the rest. Tile tooltips ("Close Peek") are not wired either: the
  sidebar rows are not window hits yet.
* **D24 — the icon picker flyout has no "Recently used" row and no
  skin-tone variants** (§5.9 lists both): both need a stored recents
  list and the catalogue's tone tables; the search box, sections and
  category strip are in.
* **D25 — the create-Space form keeps its current control styling in
  the flyout host**; only the host changed (§5.5's "Flyout" case).
  Restyling `PhiCreateSpacePanel` to Fluent controls touches the docked
  form too and is a follow-up.
* **D23 — the omnibox's context menu is the Fluent flyout**, replacing
  the EDIT control's stock menu rather than adding one row to it (§3.5
  said "the native EDIT menu plus one Phi row"): one menu family
  everywhere, and the stock menu cannot be themed dark.
* **D22 — a Space's Profile is a ComboBox in its expander** backed by
  the coordinator's existing `SetSpaceProfile` (the Space's browser
  respawns in the new profile); the default Space refuses with a
  notice, as the coordinator does.
* **D19 — the You page keeps the id `account`** on the remote surface
  (verbs, suites, rigs aim at it); only its label is "You".
* **D20 — the rail has no account block**; the "You" row is the way to
  the account page.
* **D16 — the Space switcher stays a menu.** It is now the Fluent
  owner-drawn menu (icon column, checked row semibold, "New Space…"),
  not the §5.3 custom flyout with 40 dip two-line rows: the design's
  reason for a custom popup (the HMENU look) went away with D6, and a
  menu keeps keyboard navigation. Revisit if the hint line ("profile ·
  N tabs") is wanted in the switcher.
* **D12 — the horizontal layout keeps its Chrome shape** (strip row + full
  toolbar row) and gets the Space chip at the strip's leading edge in
  place of the compact pip; the overflow ⋮ becomes ⋯ (`More` E712) — the
  same button, restyled.

## Phase notes

### P0 — tokens (2026-09-17)

* New `client/phi_fluent.{h,cc}`: the glyph map (`fluent::k*`, Segoe
  Fluent Icons / MDL2 code points), the three icon sizes and the 32 dip
  button, `PhiPaintIconButton` / `PhiPaintButtonWash` / `PhiButtonInk`
  (the §2.2a states), `PhiFluentFont` (Segoe UI Variable Text / Display,
  Segoe UI on Windows 10), `PhiIconFont`.
* `phi_dwrite`: `PhiIconFaceName()` picks Segoe Fluent Icons when
  installed, MDL2 otherwise; every literal `L"Segoe MDL2 Assets"` in the
  client now goes through it (12 files). `PhiDrawTextAA` accepts either
  face.
* `PhiPalette` gains `flyout`, `dialog_footer`, `stroke_strong`,
  `accent_soft`, `control_fill`, `pressed`; the ladder is retuned to §2.3
  (hover 4.5 %, pressed 9 %, stroke 9 %, separator 7 %, label 87 %, dim
  62 %, tertiary 42 %; dark card `#2B2B2B`); `ground_fixed` is the
  Windows Settings ground `#F3F3F3` / `#202020`; `destructive` is
  Windows' `#C42B1C` / `#E0463A`. `util_test.cc` pins them.

### P1 — the app menu (2026-09-17)

* New `client/phi_app_menu.{h,cc}` (from `phi_menu_bar`, which is
  deleted): one popup tree in the §3.1 order with the submenus Spaces /
  History / Bookmarks / Extensions / Zoom / Layout / More tools / Help;
  the command handler, the bookmark exporter and the Chromium command
  routes carried over unchanged. `PhiMenuBarShell` became
  `PhiAppMenuShell` (+ `MenuTriggerExtension`, `MenuSidebarShown`).
  Extensions ▸ lists the framework's `ListExtensionsJson` rows and runs
  an action anchored to the ⋯ button; Manage extensions opens
  chrome://extensions. New ids appended: `kCmdShowAppMenu`,
  `kCmdManageExtensions`.
* New `client/phi_menu_paint.{h,cc}`: the Fluent owner-drawn HMENU rows
  (D6) — `PhiMenuRow` / `PhiMenuRowStore`, `PhiMenuApplyTheme`
  (MENUINFO flyout brush + the uxtheme dark-frame hook),
  `PhiMenuOnMeasureItem` / `PhiMenuOnDrawItem` / `PhiMenuOnMenuChar`.
  The context menus (`phi_shell_menus.cc`'s runner) use the same rows;
  a `kRowDecor` table adds their chords, icons and danger rows by key
  so the pure builders and `shell_menus_test.cc` stay untouched.
* `PhiCaptionBand` keeps the frame and the caption buttons only: the
  titles, the reveal state machine, the Phi button press-as-grip and the
  overflow are gone. `ShellLayout` lost `phi_menu` / `band_limit`,
  `overflow` became `app_menu` and every layout lays one out (the
  vertical band's cluster: search-tabs · ⋯ · divider · caption).
  Nav-hover ordinal 12 (the Phi button) is an empty rect so the rigs'
  ordinals hold.
* Keyboard: F10, Alt+F, Alt+E in the fixed accelerator table and the
  solo Alt tap all post `kCmdShowAppMenu`; `kMsgPhiToggleMenuReveal` is
  gone.
* Strings: 64 `appMenu.*` keys in `strings-source.json` (sentence case,
  chords in the `\t` suffix, translations authored here — D14) and the
  catalogue regenerated.
* Remote surface: `agentSpace.menu.list` returns the root's rows (with
  `shortcut`), `menu.click` takes root-relative paths;
  `agentSpace.window.layout` reports `appMenu`. `test/suite-menu.mjs`
  rewritten (7/7 pass on the box); new `test/probes/app-menu-rects.mjs`.
* Box check: PhiRm shots of the open app menu and of the sidebar's
  context menu at 125 % — the flyout ground, icon column, chords, the
  system's submenu arrows and the disabled row all read as designed.
* Left for later phases: the context-menu labels are still Title Case
  English literals (P6 moves them to `strings.json`, §3.5); the §3.5
  additions (Move to new window, New tab in group, Paste and go, the pip
  right-click Space submenu, the system menu on the band run) are P3/P6;
  the zoom level readout needs a framework query the API lacks.

### P2 — the two layouts (2026-09-17)

* `phi_shell_layout.{h,cc}` rewritten: `shell_metrics` carries the §2.1
  values (44 address row, 40 strip / toolbar rows, 32 buttons 4 apart,
  32 dip r8 omnibox with 24 dip in-field slots, 28 pips, 44 pinned
  cells, 28 bookmarks bar, 40 bottom bar, 28 dip section labels, 32
  drag gutter); the Performance branch, the sidebar address bar, the
  copy-link slot and the card margins are gone. Vertical = sidebar
  (header row of pips only, Pinned / Bookmarks / Tabs labels, bands,
  bottom bar with Feedback) beside a flush pane whose 44 address row
  holds toggle · search · nav trio · omnibox · drag gutter · pinned
  extension slots · extensions · downloads · ⋯ · divider · caption
  buttons. Horizontal = strip row (pip, tabs, +, search, divider,
  caption) + toolbar (trio, omnibox, cluster) + optional bookmarks bar +
  edge-to-edge page. `PhiLayoutModeFromName("performance")` answers
  balanced (D9); `ComputeShellLayout` treats a stale `kPerformance` as
  balanced. `shell_layout_test.cc` rewritten (11 cases).
* Shell paint: flat tint (no gradient); the pane's left corners at r8
  with a 1 px stroke and a three-ring shadow; the address row on `card`
  with the caption buttons painted after it; every toolbar button
  through `PhiPaintIconButton` (Back / Forward / Refresh-or-Stop /
  Search / Add / More / ClosePane-OpenPane / Download / Puzzle /
  ReadingMode / FavoriteStar / Feedback); the omnibox with the resting
  `control_fill`, hover a step darker, focus = card + stroke + 2 px
  accent bottom edge, the globe site slot leading, the star trailing
  (Bookmark this tab, nav-hover ordinal 15 — `kCmdBookmarkThisTab`);
  the Mac masks for downloads / feedback / the toggle are no longer
  drawn. Corner caps: top caps never, the bottom-right cap only when
  the pane is not flush with the window (`PhiContentLayout.right_caps`).
  `phi_split_view_style.h` (fork): pane inset 0, radius 8.
* Floating sidebar (§4.4, D4): the panel is a persistent `WS_POPUP`
  while the sidebar is hidden — TUCKED at x = 8 − width (8 dip
  peeking), y 52 to h − 8, `CS_DROPSHADOW`, DWM-rounded, 1 px
  `stroke_strong`; a 300 ms dwell on the peek or the frame's HTLEFT
  hover slides it in (150 ms ease-out on the tick timer); it tucks 150
  ms after the pointer leaves (min 500 ms shown), on a chrome click, on
  deactivation; it stays while a drag or a modal menu is up; it follows
  `WM_MOVE` / `WM_SIZE` and hides while minimised. Inside: the docked
  sidebar laid out for the panel's own height (`floating_panel_h_`)
  with the header row always present (`ShellLayoutInputs::floating_panel`;
  the pin button that trailed it was removed 2026-09-18 — the tucked edge
  and the caption toggle dock the sidebar). `PhiOmniboxSession::HostField /
  UnhostField` deleted (the field never leaves the address row).
  `SetSidebarCollapsed(bool)` is the one setter; `ToggleSidebar` flips it.
* Remote surface: `agentSpace.layout.set {sidebar: "docked"|"hidden"
  [, windowId]}` and `window.layout.sidebarFloating`. `test/suite-
  layout.mjs` rewritten (7/7 on the box); `test/probes/layout-set.mjs`.
* Strings: `sidebar.section.pinned / bookmarks / tabs` (D14).
* Box checks: vertical and horizontal PhiRm shots at 125 %; the panel
  tucked → slid in (pin, labels, rows) → tucked again through PhiDragCap.
* Left for later: the address row as its own sheet while split and the
  split hover chip (§4.6, P6 with the fork), Esc tucking the panel, the
  panel's slide-in fade, the Settings / onboarding layout radios still
  list three (P5 / P6), the PhiCap layout-cycling rig still switches to
  performance (box script, not code).

### P3 — Spaces (2026-09-17)

* Pips: 28 dip in the 44 header row, radius 6 (`kSpaceCardRadius`), the
  active pip a `card` tile with the 1 px stroke, hover ink 4.5 % — the
  strip's existing paint on the new metrics. The header row is part of
  the caption row, so `PhiCaptionBand::HitTest` now asks the shell
  whether a pip / the "+" slot is under the pointer there (HTCLIENT),
  leaving the bare run as HTCAPTION; a right-click on that run opens the
  tab-area menu (§3.5), the address row's run keeps the system menu.
* The Space chip (§5.2, horizontal layout): the shell measures the
  active Space's name in the chip's 12/600 face and passes
  `ShellLayoutInputs::space_chip_dip` (8 + 16 + 6 + name ≤ 120 + 4 + 12
  + 8); the strip lays the one chip out at that width and paints it as
  a `card` tile with icon, name and a ChevronDown. Click = the existing
  switcher (now a Fluent menu, D16), right-click = the Space menu.
* The Space context menu (§5.4): right-click on a user pip or the chip
  opens the Space block alone — `PhiTabAreaMenuInputs::space_only` in
  the pure builder (`shell_menus_test.cc` pins it) — after switching to
  that Space; the actions run through `ShowTabAreaMenu`'s dispatcher.
* The hover flyout (§5.6): `MeasureHoverCard` / `PaintHoverCard`
  rewritten — radius 8, `flyout` ground, 1 px stroke, padding 8 × 10,
  200–280 wide, a 20 dip accent tile with the icon, the name 12/600 with
  a `Ctrl+n` keycap trailing (20 high, r4), the hint line 11 dip `dim`
  ("profile · N tabs"; agent Spaces lead with the status dot and word).
  The strip owns the three fonts per DPI; the popup re-bases every rect
  on its own origin. New shell hook `SpacesTabCount(space_id)` (the
  window model's snapshot of the Space's browser).
* Tab preview (`PhiTabPreview`): `flyout` ground, title 12/600, URL 11.
* Menu painter: a row whose picture is a bare icon-face glyph (the
  switcher's "New Space…") draws through the icon face.
* Strings: `spaces.hoverCard.status.*` (5) and the `spaces.hoverCard.
  tabs` plural family (D14).
* Box checks: pip hover flyout, pip context menu, the horizontal chip and
  its switcher at 125 %. `test/probes/space-hover-setup.mjs` seeds a
  second Space and prints the pip / chip aim points.
* Left for later: the "+" pip's 150 ms fade and the wheel scroll are
  the strip's existing behaviour, unchanged; the 40-dip two-line
  switcher rows of §5.3 (D16 keeps the menu rows); the create-Space
  flyout host (§5.5) is P6.

### P4 — dialogs (2026-09-17)

* `phi_prompt.cc`: every `PhiAlert` / `PhiNotice` / `PhiConfirm` /
  `PhiPromptText` / `PhiPromptShortcut` / `PhiPromptChoose*` renders as
  the §6 ContentDialog — no caption band, close button or Phi mark
  (`caption_height_dip` 0, no buttons), 448 wide, body on `card` with
  padding 24, title 20/600, 12 to the message, 16 to an accessory (32
  dip field / recorder, 32 dip list rows), then a footer band on the
  dialog ground with a 1 px separator, padding 24 and EQUAL-WIDTH 32
  dip buttons 8 apart. Actions run left to right in declaration order:
  affirmative first, Cancel last (D2); `PhiAlertAction::leading` is
  gone. Destructive confirms keep no default button.
* `phi_popup_chrome`: the ladder is §2.3's (Windows Settings ground
  `#F3F3F3` / `#202020`, cards white / `#2B2B2B`, ink percentages,
  standard buttons `#FBFBFB` with a 1 px stroke, danger `#C42B1C` /
  `#E0463A`); button radius 4; `PhiPopupFont` is the Segoe UI Variable
  ramp; a secondary window's caption title sits at the leading edge
  after the 16 dip mark (Windows), no longer centred.
* Titles in Windows voice: `dialog.deleteSpace.title` "Delete this
  Space?" and `dialog.deleteProfile.title` "Delete this profile?" (the
  app menu's and Settings' confirms). Other titles unchanged.
* Box check: PhiCap2 commands 1055 (Delete Space) and 1001 (About) —
  the red Delete leads Cancel, the single OK spans the footer.
* Left for later: the owner scrim (a layered smoke window, optional in
  the design), the remaining title rewrites ("Sign out of Phi?",
  "Replace the installed skill?", "Import from Google Chrome?",
  "Updater unavailable").

### P5 — Settings (2026-09-17)

* `phi_settings_window.cc`: a Windows Settings window — resizable
  (min / max / close, 1120 × 760 default, never under 760 × 560), the
  "Settings" title after the 16 dip mark; a **navigation rail** down
  the left (280, a 48 dip icon rail under 900 wide) with 36 dip items
  (16 icon, 14 label, ink 6 % selection with a 3 × 16 accent indicator,
  ink 4.5 % hover) replacing the Mac toolbar; the page column 40 in from
  the rail (≤ 1000 wide) with a 28/600 **page title**, 14/600 group
  headers and **settings cards** (white / `#2B2B2B`, 1 px stroke,
  radius 4, 14 dip rows, 12 dip descriptions, 14 dip padding);
  ToggleSwitch 40 × 20, ComboBox 32 high min 160 on the control fill
  with a stroke, sliders 200, buttons radius 4; the Windows Settings
  ground `#F3F3F3` / `#202020`. The type is the §2.2 ramp
  (`PhiFluentFont`). Every fixed-width `kWidth` / `kMargin` expression
  became the `content_left_` / `content_right_` column.
* Pages regrouped (§7): **You** (`account`), **General** (language, new
  tab behaviour, restore last session, analytics, the single-profile
  detail), **Appearance** (new: Layout with the TWO layout cards
  "Horizontal tabs" / "Vertical tabs", Color appearance, Space color +
  saturation, the Tabs group: tab previews, open indicators, dim
  unloaded, full URL), Spaces, Profiles, Navigation, Phi & AI,
  Shortcuts, **Advanced** (shorten links, auto PiP, developer mode,
  browser settings), Developer, **About** (new: version card with
  "Check for update", What's new / Documentation placeholders).
* The scrollbar: 2 dip at rest, 6 while dragged, thumb DRAG through
  `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` / capture (the wheel as before).
* Remote surface: `agentSpace.settings.get` lists the eleven pages in
  rail order (`appearance`, `about` new; `account` kept — D19);
  `settings.set {section}` takes them. `test/suite-settings.mjs`
  updated (8/8 on the box); `test/probes/settings-open.mjs`.
* Box check: PhiSetShot of General, Appearance, Spaces and About.
* Round B (same day): **expander cards** for Spaces and Profiles
  (`ItemKind::kExpander`, 56 dip header: 28 tile, name 14, hint 12,
  chevron; group header row with "+ New Space" / "+ New profile"; a
  Space's body = Name + Rename…, Profile ComboBox, Icon cell + Change…
  unfolding the four-line picker in place, Color dots, Saturation,
  Shortcut keycap, Delete Space in the danger red; a profile's body =
  Name, Search engine, Download location, Used by, the three data
  pages, Delete profile with the reason it is disabled; the "Default"
  badge on the header), the **About mark at 48** (`kAppMark`), the
  **rail search** (results page "Results for “x”", AddRow drops
  non-matching rows, EndSection drops emptied cards and trailing
  dividers, General's inline profile block and loose hints stay out),
  the **remembered window size** (D21). Box-verified: suite-settings
  8/8, PhiSetShot of Spaces (folded, Work open, picker open),
  Profiles (Default open), About, "peek" and "tab" results.
* Left for later: the account block at the rail's top (D20 — the "You"
  row stands in), the Shortcuts recorder as a §6 dialog (it already is
  a PhiAlert).

### P6 — secondary windows and polish (2026-09-17)

* Secondary windows (Feedback, URL rules, Create Space, Onboarding, the
  transcript) inherit the P4 chrome: the Windows Settings ground, the
  leading title after the 16 dip mark, radius-4 buttons, the ramp.
* Kiosk (§4.7): a 44 dip row, 32 dip buttons through
  `PhiPaintIconButton`, the read-only address with no fill in `dim`;
  the neutral ground comes from the popup ladder.
* Onboarding: the layout step offers two rows — "Vertical tabs" /
  "Horizontal tabs" (the `appMenu.*` strings); Performance is gone.
* Split view (§4.6): the framework's pane inset 0 / radius 8 with a 4
  dip top gutter, so the address row becomes a sheet of its own while
  the active tab is split — its bottom corners round and its hairline
  goes (`ActiveTabIsSplit`). Box check: a vertical split at 125 %.
* Dark mode pass: the omnibox EDIT now paints on the field's own fill
  (control fill / hover / card) in every layout; the flyout menus, the
  panel and the dialogs read their dark values from the palette. Box
  check: the vertical layout and the app menu in dark.
* The Mac masks the Fluent glyphs replaced (`PhiSidebarToggleIcon`,
  `PhiDownloadIcon`, `PhiFeedbackIcon`) are deleted from
  `phi_mac_icons`; the memory and chat masks stay for the AI build.
* After the first commit (b3bb322), the same day: the split view's
  **hover chip** on the unfocused pane (favicon · host · ⋯ pane menu:
  Swap panes / Convert / Unsplit / Close pane; a click on the chip
  focuses that pane; polled while the active tab is split, 600 ms leave)
  — box-verified; the **dialog scrim** (a 30 % layered popup over the
  owner for the dialog's lifetime) — box-verified; **Esc tucks the
  floating panel** (through the keyboard hook's escape ladder, before
  the Peek cards); the **toggle's right-click menu** Show / Hide sidebar
  (D4) — box-verified; **New tab in group** on the group header; the
  context-menu labels in sentence case (still literals, not
  `strings.json`); the bookmarks bar on the 12 dip face with Fluent
  glyphs.
* Round C1 (same day): the **switcher's two-line rows** (§5.3 —
  `PhiMenuRow::hint`, 40 dip, 300 wide: "profile · N tabs" under the
  name, the Ctrl+n keycap trailing; agent rows lead with the status
  word) — retires D16's "menu, not flyout" caveat (it is still an
  owner-drawn HMENU, now with the designed content); **Move to new
  window** on the tab row (Chrome's; `MoveTabsToNewWindowByIds`, the
  tear-off's own call); the **omnibox field menu** as the Fluent flyout
  with "Paste and go" leading when the clipboard holds text (Undo ·
  Cut · Copy · Paste · Delete · Select all with live states — D23: the
  native EDIT menu is replaced outright, not extended, so every menu
  in the app is one family); the **context-menu labels in
  `strings.json`** (`contextMenu.*`, 63 keys, eight translations
  authored, impl log D14) — the host test's stub echoes keys, so its
  label checks name keys now. Box: suite-menu 7/7; PhiRm shots of the
  omnibox menu and the switcher.
* Round C2 (same day): the **§5.9 icon picker flyout**
  (`client/phi_icon_picker.{h,cc}`, `PhiOpenSpaceIconPicker` /
  `PhiPickSpaceIcon`): 300 dip PhiPopover, Icon | Emoji segments, a
  search box (type-to-filter on the catalogue names; the IME is
  detached from the window or the first letter opens a composition),
  a 7-column grid of 28 dip cells in sections ("Letters & numbers",
  "Symbols"; the nine emoji groups behind a category strip of their
  first emoji, the current one underlined), arrows / Enter / Esc, the
  wheel scrolling the grid in place. Opened by the pip's "Change
  icon…" (hung off the click point), the app menu's (under the ⋯
  button, `MenuChangeSpaceIcon`) and Settings › Spaces "Change…" /
  the icon cell (under the cell; the inline grid of round B is gone
  again — one control, as §5.9 asks). D24: no "Recently used" row and
  no skin-tone variants yet. Box: PhiDragCap keyboard drive (F10 →
  Spaces → Change icon…, "mail" → the mail glyph).
* Round C3 (same day): the **§5.5 create-Space flyout**
  (`client/phi_create_space_flyout.{h,cc}`): a 300 dip PhiPopover
  hosting the same `PhiCreateSpacePanel` (standalone host seam), hung
  off the strip's "+", the Space chip, the sidebar toggle or the ⋯
  button (`SpacesCreateFlyoutAnchor`), for the horizontal layout and
  the hidden sidebar — `PhiCreateSpaceWindow` remains for Settings'
  "+" and the windows with no anchor (agent, incognito). The form's
  "New profile…" prompt is owned by the flyout, so its activation does
  not dismiss it. D25: the form itself keeps its current control
  styling in all three hosts (the §5.5 Fluent TextBox / ComboBox /
  segmented restyle of `PhiCreateSpacePanel` is a follow-up). Box:
  keyboard drive F10 → Spaces → New Space…, PhiDragCap shot.
* Round D (same day): **§5.7 Peek sheet** — the floating chip column is
  gone; the card (inset 40 / 24 under 900, radius 8, 1 px stroke) has a
  36 dip header on the flyout ground: the peeked page's favicon
  (`TabFaviconBgraById`), host in `label` + path in `dim`, then Open as
  tab · Open in split view · Close as 28 dip buttons in the panel's own
  hit list; **Reader View** gets the same header (origin favicon, host,
  Close) and radius 8. **§5.8 row tiles**: `DrawSurfaceBadge` →
  `DrawSurfaceTile` (20 dip card tile with 1 px stroke and a one-line
  shadow holding the surface tab's favicon — the reader glyph for
  Reader — 18 dip overhanging a pinned cell's corner by 2), hover
  scoped to the tile (`surface_tile_hot_` on the shell, invalidating
  just the row; ✕ at 12 while hot), `SurfaceFaviconForOrigin` on the
  sidebar host; `DrawOpenDot` → `DrawOpenPill` 8 × 2 at 60 % under the
  favicon on pinned cells AND bookmark rows. The caption band now asks
  `CaptionStripTakesPoint` for every point (a tile overhanging into the
  band was HTCAPTION; the old badge only cleared it by a pixel).
  **§4.9 incognito**: the field's leading slot wears the incognito
  glyph (the chrome was already fixed dark). **§5.10 agent cues**: the
  graphite ground (`PhiSpaceManager::agent_palette_`, #E8EAEE / #26282C
  for any `agentspace-*` id), the address-row agent chip (`agent_chip`
  layout rect after the nav trio: full form = mark · name · hairline ·
  status dot + word · chevron, click = the agent menu; compact form =
  mark + dot in a user Space while an agent runs elsewhere, click =
  switch), "Claude Code's tabs" as the tab section label, the control
  capsule in the page's bottom-right (collapsed 36: mark · dot · name;
  hover 300 ms / click expands to the 44 pill: "<Agent> is working" over
  the step, divider, Take control / Hand back, Finish, chevron;
  collapses 6 s after the pointer leaves; "waiting" opens and stays),
  the 2 dip status-coloured ring inside the page pane (`Ring` shaped
  layer), the fog at 5 %, and the cursor's name tag (`DrawNameTag`).
  Box: sv-surface 16/16 (its reader path re-keyed to the app menu's
  "Reader view"), PhiDragCap shots of the Peek sheet, the agent Space
  (Ctrl+2 into `agentspace-1` created by a probe) and the hovered
  capsule.
* Round E (same day): **§4.8 drag & drop** — the ghost at 85 %
  (`PhiLayerSurface::Commit` takes an alpha; the drag image passes 217),
  the **snap-tile flyout** (`phi_split_drop.cc`: five 56 × 40 tiles at
  the page's top centre in a radius-8 flyout, the arrangement drawn as
  the tile's glyph, the active tile in accent_soft with an accent ring,
  the target half previewed in accent_soft with a 2 dip ring inset 8;
  replace mode shows the two pane tiles and New window), the **New
  window zone** (`PhiSplitDropZone::kNewWindow`: the page's centre and
  the fifth tile detach the row through the tear-off's own path,
  `TearOffToNewWindow`; in a split, the divider gap), the **badge**
  (`client/phi_drop_badge.{h,cc}`: a 22 dip layered pill 16 × 20 past
  the cursor — Move · Pin · Unpin · Bookmark · Open as tab · Split
  left/right/top/bottom · Replace … · New window · Move to window ·
  Move to <Space>), **Esc cancels** (the accelerator's Esc reaches
  `PhiShellWindow::CancelSidebarDrag` before the floating panel and the
  Peeks; the ghost slides home), the **24 dip cancel band** along the
  sidebar edge and under the address row, and **drop on a Space pip**
  (a tab row over another user Space's pip: the list closes its gap, the
  badge reads "Move to <Space>", the drop is `SidebarMoveToSpace`).
  The dashed hint cards and their 1.5× scale are gone.
* Left for later: PhiCap re-baselines, the create form's Fluent restyle
  (D25), the picker's recents / skin tones (D24), D26–D30 below, and the
  §4.8 remainder (D31–D33).

## What is left (examined 2026-09-17, after eeb15c6)

Against `docs/windows-native-ui.md` section by section, everything a
user sees is in. What remains is either a deferred mechanic with a
decision above, or a small polish item:

| Design | Left | Note |
|---|---|---|
| §3.1 app menu | the Zoom submenu has no live "100 %" readout | the framework API has no zoom query (P1 note) |
| §4.4 floating panel | no fade on the slide-in | the 300 ms slide is there; the fade is a layered-alpha ramp |
| §4.8 drag & drop | auto-scroll, Ctrl / Shift, whole-group drag, bookmarks-bar drag, cross-window slot, OLE drops, pip lift, hidden-sidebar reveal, ring on a pip, ghost caption strip | D31 / D32 / D34; the vocabulary (ghost, gap, line, ring, badge, snap tiles, New window) is in |
| §5.5 create-Space form | the form's controls are still the Mac's styling in all three hosts | D25; the flyout host is new |
| §5.8 row tiles | no "Close Peek" / "Close Reader" tooltips on the tiles | D30; the rows are not window hits yet |
| §5.9 icon picker | no "Recently used" row, no skin-tone variants | D24 |
| §5.10 agent cues | brand accent per agent, ring pulse, "Claude — Research" window title, ⋯ menu on the expanded capsule, omnibox dim + eye while held | D26 – D30 |
| §6 dialogs | four titles still the old wording ("Sign out of Phi?" is done; the skill-replace, Chrome-import and updater-unavailable prompts keep theirs) | strings only |
| §7 Settings | no account block at the rail's top (the "You" row stands in); the expander hint has no tab count | D20 / D17 note |
| rigs | PhiCap band baselines and the layout-cycling rig still name "performance"; suite-drag wants three tab rows seeded | box scripts, not code |

Nothing in the tree is half-migrated: no parallel patterns were left
(the menu bar, the dashed drop cards, the Settings master-detail band,
the Peek chip column and the inline icon grid are all deleted).

## Questions for Jixiang

Every phase and round of the design is implemented and committed
(b3bb322, f3f38f6, b564a87, a62f96c, 5642ae5, bf79198, d9b6d47; fork
8a780c05f2 — nothing pushed). The decisions I took without asking are
D2–D34 above; the ones worth your eye, in order of how much they change
what you see:

1. **D2 — dialog button order** is Windows' (affirmative left, Cancel
   right). Keep, or the Mac order?
2. **D5 — no Mica / acrylic**: the chrome is a flat tint. Do you want
   Mica on the caption row when the window is active?
3. **D9 / D15 — Performance is gone** from every surface; a stored
   "performance" loads as Vertical tabs. The incognito window row is
   live through Chromium's command.
4. **D13 — dark menu frames** use uxtheme's undocumented ordinals
   135 / 136 (as every dark Win32 app does). OK to ship?
5. **D14 — ~135 new strings carry translations I wrote** (app menu,
   context menus, dialogs); the translation repo may overwrite them.
6. **D17 / D16 retired** — Settings › Spaces / Profiles are the expander
   cards, the switcher has the two-line rows; the expander hint has no
   tab count (the settings host has none). Want it plumbed?
7. **D26 — agent Spaces wear the app accent** on the graphite ground,
   not the brand colour. Want a per-agent colour table (Claude, OpenAI,
   Cursor, Copilot…)?
8. **D30 / D29 — the expanded agent capsule keeps a Finish button**
   instead of a ⋯ menu, and the window title is not "Claude — Research"
   (nothing sets the shell's title today). Worth their own pieces?
9. **D31 — drag & drop's remaining mechanics** (auto-scroll, Ctrl /
   Shift modifiers, whole-group drag, bookmarks-bar drag, the
   cross-window slot, OLE drops, the pip lift) are deferred; the ghost /
   gap / ring / badge / snap vocabulary is in. Which of these matter
   first?
10. **D24 / D25** — the icon picker has no "Recently used" row or skin
    tones, and the create-Space form keeps its Mac-styled controls in
    all three hosts (only the flyout host is new). Restyle the form?
11. **D23** — the omnibox's right-click menu is the Fluent flyout
    (replacing the EDIT's stock menu), with "Paste and go" leading.
12. **D21** — the Settings window remembers its size (not its position).

## Round F — the installer and uninstaller (§12, 2026-09-18)

Implemented and box-verified against a throwaway payload
(`-DPHI_UI_TEST`, see `installer/test/README.md`) and the real ship
payload: the Setup screen (one-click install, the expander with program
folder / browsing data / desktop toggle, Install · Update · Reinstall
wording from the Apps & Features entry), the redecorated progress screen
(4 dip bar, live percentage, NSIS's status line), the Done screen (Launch
through the shell, Close), the folder-error and running-Phi InfoBars
("Try again" plus a 1 s poll on chrome.dll, primary disabled, footer
"Waiting for Phi Browser to close…"), the wrong-architecture screen (the
arm64 stub on an x64 PC), dark mode, silent `/S`, the uninstaller's
confirm screen with the data checkbox (off; only offered when the data
folder looks like Phi's) and its self-closing progress.

Files: `installer/phi-installer.nsi` (rewritten; MUI2 dropped),
`installer/ui/phi_setup_ui.c` + `build-plugin.sh` +
`x86-unicode/PhiSetupUI.dll`, `installer/test/` (the box rig),
`installer/make-installer.sh` (`-INPUTCHARSET UTF8`, no `PHI_ART`, builds
the DLL when missing); `installer/art` deleted.

Gotchas that cost a round each: NSIS unloads a plugin after every call
unless it calls `RegisterPluginCallback` (the subclassed windows then
crashed on the next message: "PhiSetupUI.dll_unloaded"); a built-in NSIS
page keeps its dialog-template size (`SWP_NOSIZE`) so the plugin's `Page`
call resizes it; NSIS sets the Next/Cancel texts, re-shows Back and
disables Cancel per page, so `Buttons` re-hides Back and re-enables
Cancel each screen; the uninstaller ignores AutoCloseWindow on its last
page (hence `Quit`); the elevated installer needs an elevated driver task
(UIPI drops injected clicks), `SetWindowText` cannot reach another
process's edit (`WM_SETTEXT` can), the box IME eats typed backslashes,
and a lock held by a process spawned from a finished ssh command dies
with the session.

## Round G — floating sidebar and split view follow-ups (2026-09-18)

* Floating sidebar: the panel is clipped to the shell's client area while
  tucked or sliding (`ClipFloatingPanel`, a window region; a popup is not
  clipped by its owner, so the tucked part painted over the desktop when
  the window was not at the screen edge); the hover zone reaches the
  window's outer left edge so a pointer parked on the frame holds the
  panel instead of flipping it in and out; the pin button is gone from the
  panel (Jixiang) — the tucked edge and the caption toggle dock.
* Split view: the pane sheets stand on the Space TINT (`ground_top`,
  what §4.6 draws) — the client pushed `window_bg`, so the gutter, the
  4 dip divider and the row's gutter were grey on a tinted Space. The
  native new-tab page inside a split pane is inset by the 2 dip ring and
  clipped to radius 6, so the framework's hairline / accent ring and the
  sheet's corners show around it instead of a square white page covering
  them (the "focus ring missing on a new-tab pane" report).
* Split view, the seam: the client's frame for the UNSPLIT pane (1 px
  border + three shadow rings around the whole content rect) kept painting
  while split, a stray line down the sidebar seam beside the framework's
  sheets — it is skipped while split; the split address row draws its own
  §2.4 sheet shadow and stroke instead.
* Split view, the sheet shadows: the panes' §2.4 shadow is painted by the
  FORK's `MultiContentsBackgroundView` (a textured layer now under a Phi
  split chrome: the pushed tint plus three 1 dip rings outside every
  visible pane, alpha 14/8/4 on a light ground, 48/28/14 on a dark one,
  none above the top edge; it observes the panes' bounds for divider
  drags). The client cannot paint there — the gutters belong to the
  framework's window.
* Sidebar section labels removed (canvas comment 2026-09-18, "no Pinned
  Bookmarks Tabs header needed"): `kSectionLabelHeight` rows give way to
  an 8 dip band gap; only the agent Space's "<Agent>'s tabs" cue keeps a
  label row (`ShellLayoutInputs::owned_tabs_label`). The
  `sidebar.section.*` strings stay for the drop-zone wells and menus.

* Bookmark folder rows follow §4.2 (2026-09-18): the outline folder glyph
  (Fluent `kFolder` closed, `kOpenFolder` open — 16 dip, `dim`)
  replaces the Mac's 20 dip two-tone folder art for the catalog
  "default" (a chosen catalog symbol still draws as before), the title
  carries the row's regular weight, and a
  trailing 12 dip chevron in `tertiary` — right closed, down open — is
  the disclosure; the title stops 4 dip short of it. The bookmarks bar's
  folder pills share `DrawFolderIcon`, so they wear the same glyph.
  Verified on the box with `test/probes/bm-folders.mjs` + PhiRm.
* Folder drop space + container ring (Jixiang, 2026-09-18, "hard to move
  to an empty folder / to the last slot of a folder"; then "dim the whole
  folder, not the row"; then "no placeholder — just the space, and only
  when the tab is near"): the gap the resolver already opened at a
  folder's end (lower half of the last child; the header of an empty
  folder) IS the space, so nothing is added to the row model. What
  changed is the paint: `PhiSidebarDragSession::drop_folder()` (the
  resolved place's parent while the band is bookmarks) names the folder
  the drop lands in, and `PaintChrome` washes + rings that folder's WHOLE
  area — the union of its slid row rects, plus one pitch when the gap
  slot sits right after its subtree (`insert_slot()` == last row + 1) —
  before the rows paint; the header-only highlight branch is gone. A
  first cut spliced dashed "Drop here" tail wells (one per open folder
  and one for the root) into the rows for the drag's duration; it read as
  clutter and reserved space far from the pointer, so it was removed the
  same day — `PaintDropZone` and the string table are back to their
  committed state. `window.layout` now reports `drag` {kind, key, band,
  gapSlot, folderRow, dropFolder} for probes. Box:
  `test/probes/bm-drop-space.mjs` (PhiDragCap; its points are
  client-relative physical — the rig adds the parked origin; a stop
  needs ~2 s before its shot for the slide to settle). Canvas: Drag &
  drop · sidebar scene 5 redrawn (Versions 54–57).
* Space icons drawn from the catalogue (§5.9, Jixiang 2026-09-18, "the
  space icon is different from our icon catalogue"): `PhiCatalogIcon`
  carries the Fluent code point and a `fluent` flag, and the 88-row table
  in `phi_icons.cc` maps every id to the family's own glyph. Letters and
  digits draw as TYPE (the shell's face at 13/16 of the box, semibold),
  not as artwork; the compiled masks stay only for the ten ids with no
  Fluent twin (sparkles, ticket, chip, cube-transparent, truck, cake,
  table, database, server, rss) and for the Fluent-only codes that fall
  back on Windows 10's MDL2 (F0E3, F133, F196). `PhiDrawCatalogIcon` and
  `PhiDrawCatalogArtwork` try the glyph first and keep the mask path
  behind it, so every draw site (pips, chips, Settings rows, the picker,
  menus, kiosk, the agent overlay) follows without a change. Box:
  `test/probes/space-icons.mjs` seeds eight Spaces across the three
  cases; verified on the strip and in Settings › Spaces.
* Space-switch pill, a scrolled-out source (Jixiang, 2026-09-18): the
  strip's selected pill used to capture the leaving pip's RECT when the
  switch began, so it did not animate at all when the row had been
  wheeled past that pip, and started from a stale position whenever the
  switch re-centred the viewport. `PhiSpacesStrip` now remembers the
  leaving Space's id and resolves the start rect each frame
  (`SwapPillOrigin`): the pip where the viewport still shows it, else one
  gap outside the edge it lies behind — the leading edge for a forward
  switch, the trailing one for a backward switch — with the pip row
  clipped for that frame so the pill emerges from the edge. Box:
  `test/probes/sp-strip.mjs` seeds ten Spaces; caught with skillcap's new
  `wheel` and `bpost` steps (the burst has to start on the same tick the
  command is posted to catch a 150 ms slide).
* Sidebar bottom bar aligned with the board (Jixiang, 2026-09-18): the
  40 dip bar now leads with its OWN Downloads button (32 × 32, radius 4,
  16 dip glyph at the 8 dip inset — `sidebar_downloads`, beside the tool
  cluster's copy in the address row, as both Vertical and Floating draw
  it) and trails with the full Feedback pill: 26 high, a 1 px
  `stroke_strong` capsule, 10 dip of padding, the 16 dip glyph, a 6 dip
  gap and the 12/600 label. It was an icon-only 32 × 24 capsule on the
  hairline `border` with an 11/400 label in the layouts that showed one.
  The pill is laid out around the measured label
  (`ShellLayoutInputs::feedback_dip`, taken in the same 12/600 face the
  Space and agent chips use — now one `EnsureChipNameFont`), and falls
  back to the icon-only slot if a bar is ever too narrow for it. Fixed
  with it: `bottom_hot` keyed the bottom bar's hover washes at
  NavHoverTable ordinal 4 instead of 6, so hovering Feedback lit the
  extensions button and the pill itself never washed. The icon is the
  canvas's own bug, not a Fluent stand-in (Jixiang rejected both the
  family's `Feedback` mark, ED15, a person and a speech bubble, and its
  `Bug`, EBE8, a thin straight-legged tick): `PhiFeedbackIcon()` — the
  48 px `solar_bug-line-duotone` mask §2.2a had retired — comes back
  beside the memory and chat masks as the one artwork the Fluent face
  has no answer for. Its master carries a 4 px margin the canvas's 16 dip
  drawing does not, so the artwork's box is 20 dip and its INK then
  measures the canvas's 16. The pill is 10 + 20 + 6 + label + 10, the
  label rounded UP into dip and the text rect NOT re-clipped to the
  trailing padding the width already reserves — truncating it and
  clipping it again cost the word's last letter (Jixiang: "do not make
  the button show half word").
* First slot of the outline (Jixiang, 2026-09-18): a folder's top third
  now resolves BEFORE it among its siblings (`ResolveBookmarkDrop`; the
  tree-view three-zone rule — an expanded folder used to take its whole
  row as "first child", so nothing could open the slot ahead of a
  leading folder). Collapsed: top third before, middle INTO, bottom
  third after. Host test updated; `bm-drop-space.mjs` stop e shows the
  gap opening above Work. The probe keeps one pin so the empty band's
  reserved zone does not move the outline under its pre-read rects.

## Round H — search-engine keyword chip in the address field (2026-10-16)

* The Mac's keyword search (OmniBoxViewModel `acceptKeywordSearch` /
  `exitKeywordSearchIfEmpty`, the accent `searchEngineBadge`) on the
  Windows field. Typing an engine's keyword prefix ("bing") brings the
  keyword provider's engine-selection row — shown as "Search Microsoft
  Bing — bing.com" (`addressBar.keywordSearch.suggestionTitle`) — and a
  hint at the capsule's trailing end, "Tab to search Microsoft Bing", or
  "Tab or Space to search …" once that row is armed
  (`addressBar.keywordSearch.tabHint` / `tabOrSpaceHint`, in the 11
  small face, at most a third of the capsule). Tab, Space on the armed
  row (or with the whole keyword typed — Chrome's own rule; gated on the
  profile's "Space enters keyword search" pref), or opening that row
  raises the CHIP: a 24-dip `accent` capsule with the engine's name in the
  13/600 face and `on_accent` ink, ahead of the text; the EDIT yields its
  width plus 8 (`PhiOmniboxTextRect` leading / trailing extras) and shows
  `addressBar.keywordSearch.queryPlaceholder` ("Search…") while empty.
  Enter builds the URL through the new framework entry
  `PhiFramework::KeywordSearchUrl` (the fork's
  `OmniboxProxy::GetKeywordSearchURL`, TemplateURL::GenerateSearchURL —
  POST engines, starter packs and inactive engines refused) and
  navigates as a commit does (new-tab mode and placeholder birth
  included). Backspace on an empty query takes the chip off and puts the
  keyword back (its hint returns with the requery; handled at the WM_CHAR
  stage, since TranslateMessage has queued that character before the
  key-down is dispatched and the EDIT deletes on it); Escape takes it off
  and clears; losing focus takes it off and restores the URL. The chip
  and hint belong to the shell's capsule only — the new-tab page's box
  never enters keyword search. Behind the chip no autocomplete runs (the
  Mac's handleInputChanged guard). Pure rules (`PhiOmniboxRow`,
  `PhiOmniboxRowOffersKeyword`, `PhiOmniboxKeywordHintRow`,
  `PhiOmniboxSpaceKeywordRow`) are host-tested in
  `test/model/omnibox_session_test.cc`. Framework json: a
  search-other-engine match carries `keyword` + `engine`; empty `url` is
  the selection hint. Box rig: `modclick.ps1` grew `keys` / `vk`
  (SendInput — the user's path) and `edit,*` steps (`selall`, `chars`,
  `key`, `space`, `back`, `text`, `ime`) that post straight to the EDIT;
  a POSTED key-down arrives twice through the box's Chinese IME layer
  (one real `vk,32` = one space, one `edit,space` = two), and a relaunched
  Phi starts with that IME in Chinese mode — `edit,ime,eng` first, then
  drive keyword tests with `keys`/`vk`; `edit,chars` only for letters.

## Round I — Mac parity: hover card, Space order, fullscreen sidebar (2026-10-27)

* Space hover card (§5.6) restyled neutral after the Mac's 5588d798: the
  icon tile and the Ctrl+N keycap sit on an ink 8 % wash (`BlendOver(ink,
  flyout, 20)`), no Space / theme accent anywhere; `card_accent_` and its
  theme lookup in `ResolveHoverCard` are gone. Geometry, the agent status
  line and the `label_dim` hint are unchanged (the Mac's 30 % profile line
  and forced light scheme were not adopted — the card follows the app
  appearance).
* A new Space lands right after the Space its form opened from (the Mac's
  `afterSpaceId`): `PhiNewSpaceSpec::after_space_id`, captured by
  `PhiCreateSpacePanel::Show` from the controller's active Space (inline
  form, §5.5 flyout and the standalone window alike), applied in
  `RequestCreateSpaceWithOptions` as `ReorderSpace(new, index(source) + 1)`
  before the switch; sync-created Spaces and the verb keep appending. The
  strip's dashed placeholder pip occupies that slot (`creating_source_`,
  `PhiSpacesStrip::Rebuild`): the following pips shift over, the viewport
  keeps the source pip in place and scrolls only when the slot would fall
  outside. No shift animation (the Mac's 0.2 s neighbour slide).
* Floating sidebar over content fullscreen (the Mac's acd6b265 / 189babda /
  86a43c0e): `RelayoutShell` no longer tears the panel down in content
  fullscreen (the shell is the fullscreen window, the panel its owned
  popup); `FloatingPanelRect` drops the address-row offset there (y 8);
  `ComputeLayoutInputs` clears `content_fullscreen` for the panel's
  expanded layout so it still has a sidebar to paint; `HideFloatingSidebar`
  posts `kPhiFocusPage` while a page is fullscreen, so Esc after a panel
  click leaves fullscreen through the page.
* Shortcuts pane (the Mac's 60dfd31a / c89c05cb / 896b36d0): every row
  title and group heading resolves a Mac `settings.shortcuts.*` key
  (`kRebindableShortcuts[].label_key`, `settings.shortcuts.group.<id>`);
  "Reset All to Defaults" asks first (`PhiAlert`, destructive button, no
  Return default); customised rows get a per-row Reset button; Delete or
  Backspace while recording UNBINDS (a `vk 0` override, drawn "—"); Open
  File, Print, Find, zoom, View Source, DevTools, Console, Back / Forward /
  Home and Settings are rebindable rows now — the hook's fixed table keeps
  Quit, F5, the numpad zoom twins, F10 / Alt+F / Alt+E and Ctrl+1..9; a
  default chord the user moved away from is swallowed
  (`PhiPreferences::IsDisplacedDefaultChord`) so Chromium's own accelerator
  cannot fire it; the ⋯ menu shows the effective chord
  (`PhiAppMenu::Label` + `ChromePrefsHost::RebindableChordLabel`, rebuilt on
  `PreferenceChange::kShortcuts`). General's inline profile card hides its
  Folio Folder row (`show_folio_folder = false`, the Mac's d509f24d).
* Peek as the page-command target (the Mac's a19624bb + c11849cf,
  PHI-1745): `PhiShellWindow::PageCommandBrowser()` (the Peek presented
  over the active tab — `PhiSurfaceCoordinator::PresentedPeek` — else the
  tab) drives Reload / Stop / zoom / print / save / view source / DevTools /
  Find / Copy URL / Bookmark / Folio in `PhiAppMenu` and seeds the address
  field (`PhiOmniboxShell::OmniboxPageBrowser`, `RebuildNavState(nav,
  page_browser)`): Ctrl+L shows the Peek's URL, a commit navigates the Peek
  in place (no `OmniboxLeaveReader`), a suggestion row's URL is navigated
  directly on it, and a Peek closing under a focused field re-seeds the
  field and drops a late Enter (`CurrentTabTarget`). Back / Forward and the
  Reader toggle stay on the opener (reader-over-peek is not wired here).
* Omnibox new-tab behaviour in empty windows (the Mac's 936d090e): the
  preference is pushed to the framework (`SetNewTabOpensOmnibox`); under it
  the fork's `Browser::TabStripEmpty` skips the refill tab and shows the
  placeholder at once, and a user / Space window is created WITHOUT a seed
  tab and put into placeholder mode (`CreateUserBrowserIn`,
  `Browser::PhiEnterPlaceholderModeIfEmpty`) before `Show()`, so the
  client's existing placeholder path (content hidden, native NTP + focused
  field, tab born on commit) is the omnibox over the empty window.
  Off-the-record windows keep their blank tab. Alt+Shift+N no longer opens
  a split tab (the Mac's ce059932: a menu action only).
* Sign-out closes every auxiliary window before the Login page (the Mac's
  3a4469a3): `RequireLogin` posts WM_CLOSE to each Kiosk
  (`PhiWindowRegistry::Kiosks()`), and `CloseForSignOut()` on the Feedback,
  URL-rules and Create-Space windows (a WM_CLOSE with `kCloseWithoutPrompt`
  skips the discard prompt), the Sync setup window and the Import window.

## Round J — Space theme palette editor (2026-10-27)

The Mac's SpaceThemeEditorView (`3d045cf6`, `4c9d919e`, the harmony half of
`23c728d9`) on Windows; `mac-parity.md` §2 row ◐.

* **Record.** `PhiThemePalette` (`client/phi_theme_palette.h`, host-clean:
  STL + `phi_util.h`) is the Mac's `ThemePalette`: anchor hue, saturation,
  brightness (0…1) and a harmony (single / analogous +45° / complementary
  +180° / split complementary +150° and +210°); only the anchor is stored.
  Its geometry and colour maps are the Mac's to the constant: hue → angle
  through the Figma spectrum stops, radius = saturation × 0.42, centre snap
  within 0.025; light S = sat × 0.52, B = 0.65 + 0.35 × brightness; dark S
  from the two-branch radial curve (squared smoothstep to 90 % at 40 % of
  the radius, falling to 45 % at the edge), B = 0.12 + brightness × (0.24 +
  0.54 × sat); accent S .65 / B .60 light, S .50 / B .85 dark; the inverse
  (`FromColor`, the seed from a preset's overlay) prefers the outer dark
  radius when it can carry the input brightness. `model::Space::palette`
  is optional; `PhiSpaceManager` loads and saves it in the account
  defaults under the Mac's `spaceThemePalettes` key with the Mac's coding
  keys (`version` 3, `hue`, `saturation`, `brightness`, `colorCount`,
  `harmony`; a record before version 2 decodes at brightness 1, a missing
  harmony takes the count's). LOCAL ONLY: `phi_sync_spaces*` is untouched
  and the Space proto carries nothing new; a Mac palette Space renders here
  as its pinned preset, as the Mac Theme README says it should.
* **Resolution.** `PhiResolvePalette(base, appearance, palette)`: the
  palette's anchor colour composited at the Mac's fixed 0.8 overlay alpha
  over the BASE theme's window background (the pin, else the app-wide
  theme — its dark background shows through, as `applying(to:)` keeps it),
  the anchor hue's accent, then the one shared ladder (`ResolveFromColors`,
  split out of the theme resolver). Precedence is the Mac's
  `resolvedTheme`: a palette beats the pin's colours, the per-Space
  saturation and the wire hex; `SetSpaceTheme` / `PinSpaceTheme` drop the
  record; `SetSpaceSaturation` under a palette writes the palette's radius
  and `SpaceSaturation` reads it back, so Settings ▸ Spaces' slider keeps
  working; `PreviewSpacePalette` re-resolves one Space's entry and repaints
  the shells without a save, a sync note or a Settings rebuild.
  **Deviation:** a 2–3-colour palette paints the Mac sidebar as a vertical
  Oklab gradient. The Windows ground is flat by design (§2.3) and every
  wash is pre-blended over `ground_top` (GDI composites nothing), so a
  gradient would leave the pinned cells and the active row in the primary
  hue over the secondary colour — the chrome takes the PRIMARY colour; the
  linked colours show on the editor's handles. Doing it properly means
  re-deriving each row's washes over the ground at its own y.
* **Editor** (`phi_space_theme_panel.cc`, a `PhiPopover`, 260×296 DIP): the
  244 field with a 12 radius, 38 / 28 / 14 handles filled with the exact
  resolved colours under a 2 white rim, a 24 toolbar — Light / Dark in a
  padded capsule at the left (the APP-WIDE appearance, through the new
  `ChromePrefsHost::RequestAppearance`), the four harmony buttons at the
  right (ring + dots glyphs drawn with the AA primitives, names = the Mac
  keys as tooltips). The field bitmap is sampled per device pixel through
  `ColorAt` (one bitmap per appearance, keyed by brightness and dpi),
  softened in linear light (three box passes ≈ the Mac's 7 % Gaussian,
  then saturation 0.84 / contrast 0.96) and drawn with `PhiDrawBgra`, its
  corners bitten back with `PhiKnockOutRoundRectAA`. A press takes the
  handle within 20 of it (else the primary); under 3 of travel it is a
  click that moves the PRIMARY with the Mac's 0.3 s ease-out; a drag
  previews per move and writes ONCE on release (capture loss keeps the
  edit); Left / Right rotate the hue a degree. The panel reads the record
  back from the host on every paint and seeds from the resolved preset's
  overlay at brightness 1 when the Space has none. Ground is the flyout
  colour, which does not move with the tint being edited.
* **Create form.** The theme dots and slider are gone; a 32 "Color ›" row
  (`sidebar.createSpace.colorPicker.label`, ChevronRight) opens the same
  editor beside it (`PhiOpenSpaceThemeDraftPanel`, `kRightTop`, owned by
  the shell / flyout / standalone window) on a DRAFT: every edit lands in
  `PhiCreateSpacePanel::palette_` and previews on the sidebar
  (`PreviewCreateSpacePalette` → `PhiSpacesStrip::PreviewPalette`); the row
  toggles (reopen guard); the form dismisses the editor when it hides.
  `PhiNewSpaceSpec::palette` is committed in
  `RequestCreateSpaceWithOptions` after `agentSpace.spaces.create` and the
  pin (`SetSpacePalette`); the form's `colorHex` is the draft's resolved
  anchor (the Mac's `resolvedColorHex` over `effectiveTheme()`); the legacy
  slider value is no longer sent (`saturation` -1). The strip now keeps
  the preview's INPUTS and re-resolves them on `ApplyPalette`
  (`RefreshPreview`), so Light / Dark pressed in the editor re-tints the
  previewing form instead of leaving the other appearance's colours.
* **Nested popovers.** The pip flyout's form hosts the editor inside a
  popover: `PhiPopover` no longer dismisses on `WA_INACTIVE` when the
  window taking activation is one it owns, and a child dismissed for a
  window outside its owner forwards the deactivation to an owner that is
  itself a popover (`IsPopoverWindow`, a window property), so no parent
  lingers inactive.
* **Strings.** The Mac keys `theme.editor.singleColor / analogousColors /
  twoColors / threeColors / colorHandle`, `sidebar.createSpace.colorPicker.label`
  (translations by key).
* **Verified on gmk** (100 % DPI): editor over the active Space in light
  and dark; click, split-complementary (three handles), Dark → the chrome
  follows; the record in `account_defaults.json`; the create form's row →
  draft editor → live sidebar preview → Create lands the pin + palette;
  `spaces.setTheme coral` removes the record; preset-pinned Spaces
  unchanged; a drag rig run shows the chrome recolouring mid-drag and the
  file written on release.
* **Open:** the sidebar gradient (above); per-handle UIA names (one field
  hit, so no "Color n" tooltip covers the field); palettes are not carried
  by the guest→account migration (`GuestThemes`); the Mac's Pure-brightness
  slider has no Windows twin, so `brightness` only ever changes through a
  record the Mac wrote (never synced) and stays 1 here.

## Round K — holographic membership card (2026-10-27)

The Mac's `HolographicCardView` (6b300dc0 … cda12eaf) on the onboarding
Welcome page and in Settings ▸ You, replacing the conch frame-sequence card
(`phi_profile_card.{h,cc}` and 12.5 MB of JPEG frames under
`resources/profile-card` + `resources/onboarding`, both deleted; the `.rc`
never referenced them — the trees were staged by `build-client.py`, whose
`profile-card` entry is gone).

* `client/phi_holographic_card.{h,cc}` — `PhiHolographicCardStyle`
  (palette ↔ theme id, texture, ornament, `Random()`, `ToMap`/`FromMap` in
  the Mac's raw values) and `PhiHolographicCard`. The card is authored at
  292 × 423 dips (28 dip corner) and composed in software at its pixel
  size. Materials — the six engraved foil textures (arcs, waves, guilloche,
  brushed, diamond, stardust; the Mac's exact formulas incl. its LCG seeds
  and the contrast(1.18) pass), the feTurbulence-style grain tile and the
  two stroked seal masks (lace, lotus, periodic, modulated, epicycloid) —
  are rasterised ONCE per (style, size) with GDI+ on a worker thread and
  cached as PNG under `<user data>\PhiCardArtwork\v1` (`foil-<texture>-
  WxH.png`, `grain-WxH.png`, `seal-<ornament>-WxH.png`; bump
  `kCacheVersion` when a formula changes). The light-driven layers
  (iridescence gradient + radial highlight, sheen, logo sweep, the seal's
  groove / reflection gradients) recompose per frame from the pointer with
  LUT blends (overlay / soft light / screen / hard light tables), over
  static layers built on the UI thread (base gradient, logo ink, tear line,
  the copy, the edge hairline, the rounded-corner coverage, the shadow).
* Motion is the Mac's `HolographicCardMotion`: the pointer over the card
  moves the light angle (125 + 24x − 20y) and tilts the card (rotations
  about x/y/z of 10°/13°/1.7° under a 1/1100 perspective — a software
  homography warp of the composed face with bilinear sampling), settling
  flat when the pointer leaves; no idle sway. A card appearing spins in
  once (1.7 s: scale 0.16 → 1, a full turn showing the dark reverse, fade
  over the first 18 %); `SPI_GETCLIENTAREAANIMATION` off skips the motion.
  The card draws nothing until its materials landed (the Mac's hidden
  rotor); a style change keeps the old card until the new one is ready.
* Copy: the name in IvyPresto Headline Light 31 (the face now ships in
  `resources/fonts`, registered by `phi_fonts.cc`; Segoe UI Light for
  scripts it lacks), the member id (Consolas 10) = first 8 bytes of
  SHA-256(Auth0 `sub`) as lowercase hex (BoringSSL `SHA256`), and "Date
  Joined" + `yyyy.MM` (local time) when the profile's `created_at` is known.
* Onboarding Welcome (`phi_onboarding_window.cc`): the shared dark ground,
  the card centred at centre-y 337.5, a 40 dip dice button at y 585
  (`oobe-dice.svg` transcribed as GDI+ paths) that draws a random style and
  switches the app theme to the palette's theme id (the Mac's
  `randomizeCard`), the base's gradient-ring Next. No title, no swatches
  (`oobe.welcome.title` / `.personalizedTitle` retired from the catalog;
  `oobe.welcome.randomizeCard`, `oobe.welcome.cardSaveFailed`,
  `profile.card.dateJoined` added under the Mac's keys). Next persists the
  style for the account (`PhiPreferences::SetProfileCardStyle`, the Mac's
  `profileCardStyle` key in the account defaults, bound or on disk like
  `loginPhase`) and shows `cardSaveFailed` when it cannot. The window
  pre-warms the default style's artwork on open. Harness: `swatch` (theme
  id) now sets the palette + app theme, `randomize` is the dice; `state`
  carries `cardStyle` and `memberId`.
* Settings ▸ You (`phi_settings_window.cc`): the card at 240 × 348 (the
  design aspect), the account's saved style with the palette overridden by
  the CURRENT theme on every paint (so theme / appearance changes re-apply,
  as the Mac's `applyCurrentTheme`), the pointer lights it, "Download
  image" exports the front at 2× centred on a canvas twice its size in the
  Mac's accent (theme hue at saturation .65 / brightness .28; black for
  Pure) through the save dialog. The old grid drag is gone.
* `PhiHsbColor` / `PhiHueOf` moved from the onboarding window into
  `phi_popup_chrome.{h,cc}` so Settings can derive the export accent.
* Not ported: the Mac's back-face flip on demand (`showsReverse`, unused
  there too), its 3× export (the card is rendered directly at 2×), and the
  `onboarding_completed` analytics event with the theme id (never ported).
* **Sidebar profile button** (the Mac's `ProfileButton` +
  `populateProfileMenu`, 2026-10-27): back in the bottom bar, LEFT-MOST
  (`ShellLayout::sidebar_profile`, a 32 dip hit box at x 8 before
  Downloads; §4.2). A 24 dip disc: the account picture circle-clipped
  (`WindowHost::AccountAvatarCircle`, the Settings rail's buffer) under a
  1 px `border` ring when signed in and not in Guest Mode, else the Fluent
  Contact glyph in `label_dim`; no hover wash, no badge, tooltip
  `profile.button.accessibilityLabel`. Nav-hover ordinal 22. A click drops
  `PhiBuildProfileMenu` (phi_shell_menus) upward, left-aligned, its bottom
  5 dip above the button: Download List (`PhiLibrary::Open` ▸ Downloads),
  Manage Bookmarks, Manage Extensions, ─, New Tab, New Incognito Space,
  New Incognito Window, ─, Settings, and Sign in (`WindowHost::
  RequestSignIn`) while no account is signed in. Chords are the user's
  EFFECTIVE ones, resolved in the shell by the ⋯ menu's rule
  (`ChromePrefsHost::RebindableChordLabel`, else the catalogue's
  `appMenu.*` suffix) and carried on the rows; the rows run through
  `PhiAppMenu::OnCommand`. `PhiCoordinator::NoteAccountChanged` invalidates
  every shell's bottom bar so a late avatar fetch repaints the button. The
  `profile.*` keys are the Mac's; the layout verb publishes `profile`.

## Round L — the Library (2026-10-27)

The Mac Library (`LibraryViewModule` + `LibraryOverlayController` +
`LibraryWindowController`, `Library/Space/*`, `Downloads/AllDownloadsListView`)
without its Folio section: `client/phi_library.{h,cc}`, with the downloads
model the popover and the Library share pulled out into
`client/phi_downloads_model.{h,cc}`.

### Hosting and z-order

* `PhiLibrary::Open(shell, section)` builds a `LibraryOverlay` (not a window)
  over ONE shell: a layered `LibraryScrim` panel (black 28 %, per-pixel alpha
  through `PhiLayerSurface`; a click on it dismisses) and the opaque
  `LibraryView` panel (the card), both `PhiPanel` children of the shell in
  its `PhiCardStack`, both SELF-OWNED (`DeleteWhenDestroyed`) because Escape
  and the double-click open tear the view down from inside its own handlers
  — the group overview's shape. The overlay object is freed by `Finish()`
  after the close fade; `g_dying` keeps the fading ones so a shell going
  away (`CloseIfBoundTo`) ends them first.
* The overlay covers the shell's client area BELOW the caption band (sidebar
  and content both, like the Mac's panel over the content view; the caption
  buttons stay reachable) — a panel sits above the shell's own sidebar paint
  by being a child, and above the browser child through the card stack. The
  card is that rect inset 36, radius 14 cut by `SetCornerFringe` (the card
  hosts EDIT children, so it cannot be a shaped layer), 1 px `border` stroke,
  ground `palette.flyout`. The Mac's 12-blur shadow is not drawn.
* Raise order: the shell's `kMsgRaiseCaps` turn ends with
  `PhiLibrary::RaiseIfBoundTo` AFTER `RaiseOverlays` (the caps), not between
  the NTP and the reader panels as first planned: the scrim covers the caps
  as the Mac's panel covers the whole content view, and nothing needs to sit
  above the Library. `RepositionIfBoundTo` from `LayoutContents` /
  `OnContentBoundsChanged`; `RefreshIfBoundTo` from `RebuildTabs` (which is
  also where a changed active tab or Space closes it unanimated — the Mac's
  `focusingTab` subscription); `CloseIfBoundTo` from the destructor; the
  page-colour and edge-hover gates read `PhiLibrary::IsOpen()`; F6
  (`kMsgPhiCycleFocus`) closes it; `MenuFocusPageForFind` hands Ctrl+F to
  it (Spaces swallows, Downloads focuses its search).
* Motion: in, the scrim 0 → 1 and the card from a rect scaled to 2–12 %
  centred on the source control (the ⋯ button; `OpenFrom` takes any other)
  to its place, ease-out 200 ms (`PhiAnimator`; the content paints once the
  flight lands); out, ease-in 160 ms back. `SPI_GETCLIENTAREAANIMATION` off
  skips both. Not the Mac's spring.
* Dismissal: Escape (a card drag in flight snaps back first, an inline
  rename or the create card cancels first), the scrim, the window's active
  tab / Space changing, the shell closing. Never focus loss: the icon picker,
  theme panel, menus and prompts all take the focus. The window's last
  section is remembered per shell (Downloads at first); `Toggle` reopens it.

### Entry points

* ⋯ menu row **Library** (`library.navigation.title`, `fluent::kLibrary`,
  after Reader View) → `kCmdLibrary` (appended after `kCmdOpenFolio`;
  `OnCommand`'s range check widened) → `PhiAppMenuShell::MenuToggleLibrary`.
* Rebindable chord `{"library", "View", "library.navigation.title",
  kCmdLibrary, Ctrl+Shift+L}` (Ctrl+L is the address field).
* Settings ▸ Spaces row "Manage Spaces in Library"
  (`settings.spaces.manageInLibrary`, button "Library") →
  `PhiSettingsHost::OpenLibrarySpaces` (coordinator: the focused user shell)
  and the Settings window hides (WM_CLOSE).
* The downloads popover's "All Downloads" → `PhiLibrary::Open(shell,
  kDownloads)`, the shell found from the owner HWND through the new
  `PhiShellWindow::FromHwnd` (the popover is only handed the HWND).
* `OpenInNewWindow` (the rail's ↗ button) hosts the same `LibraryView` in a
  `LibraryWindow : PhiSecondaryWindow` (1120×780, min 840×560, all three
  caption buttons, no title text, frame remembered for the process only —
  no preference exists for window frames). One per process; another shell's
  request takes it over. Its rail has no ↗ and no sidebar collapse (the
  Mac's HSplitView collapse is not ported).

### Spaces section

* One horizontal row, padding 12, cards 240 × (content − 38), gap 22, wheel
  scrolls sideways outside a card body and vertically inside one; user
  Spaces only (agent and Incognito skipped); after the last card the 48 "+"
  circle. Card ground = the Space's overlay at 80 % over the card ground:
  `SpacePalette(id)->ResolvedColor(0, dark)` when linked, else
  `PhiThemeById(SpaceThemeId(id) or the app theme).light|dark.overlay`;
  text colours from `PaletteForSpace(id)`. Radius 18; a 2-dip ring offset 4
  stands in for the Mac's blurred shadow.
* Header (h-pad 20, 34 high, 17 semibold): icon button 32 (the Space's
  catalog / colour glyph at 23) → `ShowSpaceIconPicker` (now public on the
  shell); the name → inline EDIT rename (Return / blur commit through
  `RenameSpace`, Escape cancels); palette button 28 (`fluent::kColor`) →
  `PhiShowSpaceThemePanel`.
* Body (h-pad 8): the pins grid (3 × 64×45, radius 8, ink 4 % / hover 8 % /
  selected `card`, favicon 18 or two 16s for a split pin, collapsed to 0
  without pins), 8, the bookmark outline (rows 36 / pill 30 radius 8, indent
  10 per level, favicon 14 drawn scaled through `PhiDrawBgra`, folders with
  a chevron + the catalog mask as the bookmark manager draws it). Single
  click: a folder toggles (expanded ids kept per Space for the overlay's
  life), a pin / bookmark selects; double-click opens; Delete deletes the
  selection; right-click → Open / Edit… / Move to Space ▸ / Delete, on the
  blank body the three New… rows.
* Footer: the move-glyph handle 24 (bottom-left, `library.spaces.reorder`)
  — press + horizontal move > 4 dip starts the drag, the card follows in x,
  the others slide to their new slots (ease-out 150 ms, target =
  round(Δx / 262)), mouse-up commits `ReorderSpace(id, global index of the
  card that held the slot)`, Escape snaps back; "…" 24 (bottom-right) →
  New Pinned Tab… / New Bookmark… / New Folder… / ─ / Set Profile ▸ (radio
  over `ProfileSummaries`, disabled for the default Space; `PhiConfirm` +
  `SetSpaceProfile`) / Delete Space… (`CanDeleteSpace`;
  `PhiAppMenu::DeleteSpace`'s confirming flow). Menus are `PhiMenuSpec`s
  built in phi_library.cc with their own glyphs (no `kRowDecor` rows).
* Data through new `SidebarHost` methods the coordinator implements over
  `PhiTabStore` by Space (not by slot): `PinnedRowsForSpace`,
  `BookmarkRowsForSpace`, `AddPinnedToSpace`, `AddBookmarkToSpace`,
  `MoveStoredRowToSpace` (the existing private coordinator method, now on
  the interface), `TransferPinToSpace` (the first caller of
  `LocalStore::TransferPin`, destination INT_MAX = the end). Favicons are
  upgraded through the slot whose window shows the Space.
* Opening a row: `PhiShellWindow::OpenStoredRowInSpace(space, guid, url)`
  — the Library yields, the shell switches Space, and opens at once when the
  Space's browser is attached, else when `BrowserHostNoteActiveSpace` lands
  on that Space (`pending_stored_open_`; a different Space landing first
  drops it) — the Mac's `slot.activate(onSwapSettled:)`.
* "+" hosts `PhiCreateSpacePanel` INLINE as a 240 card (the panel takes a
  host HWND + `PhiCreateSpaceFormHost`; its 280 column fits the card's
  200), ground = the source theme's overlay at 80 %. Commit →
  `SpacesHost::RequestCreateSpaceInBackground(slot, spec, "library")`: the
  coordinator's one creation flow (`CreateSpaceWithOptions(…, activate)`)
  with the switch skipped, so the new Space lands after the window's current
  one (`spec.after_space_id`) and is NOT activated; `space_created` carries
  `surface: "library"` (the three existing literals untouched).

### Downloads section

* Content max 800 centred, h-pad 80; header pad 16 with a 32 search pill
  (an EDIT inside, `downloads.all.search`, filters name + URL live; Ctrl+F
  focuses it; Escape clears then dismisses); the list in day groups (header
  13 semibold dim, 16/4 padding) of the popover's rows (56; icon 30, name 13
  with the struck-through failed/cancelled/deleted tag, 11 dim second line:
  relative time / "From host" / the 3-dip progress bar, figures while
  hovered), hover ink 5 % radius 10, hover-only 24 action buttons at the
  right (the popover's per-state set through `PhiDownloadActionsFor`;
  actions through `DownloadAction(window, id, verb)`, Copy Link to the
  clipboard); double-click opens a finished file. Empty state: a 36 glyph +
  `downloads.all.empty` / `downloads.all.noSearchResults` in `label_dim`.
  500 ms poll while the section is up.
* Deviation: `ListDownloadsJson` carries `endTime` only and is capped at 20
  per window profile, so the groups are by the local day the download
  ENDED (a live one groups under today; "Today" / "Yesterday" labels, else
  the long date) and only the newest 20 appear; the Mac groups by start
  over every profile's full history. No profile filter: no per-profile
  listing exists in the framework (`DownloadsJsonForAgent` is per window
  too and refuses open/show).

### Strings

Under the Mac's keys with the Mac English: `library.navigation.title`
("Library"), `.spaces`, `.downloads`, `.openInNewWindow`;
`library.spaces.create` ("New Space"), `.editIcon` ("Change Space icon"),
`.name` ("Space name"), `.editTheme` ("Edit Theme"), `.moreActions` ("More
Actions"), `.newPin` ("New Pinned Tab…"), `.newBookmark` ("New Bookmark…"),
`.newFolder` ("New Folder…"), `.setProfile` ("Set Profile"), `.deleteSpace`
("Delete Space…"), `.openItem` ("Open"), `.editItem` ("Edit…"),
`.moveToSpace` ("Move to Space"), `.deleteItem` ("Delete") (`.reorder`
already existed); `downloads.all.search` ("Search Downloads"), `.empty` ("No
Downloads"), `.noSearchResults` ("No Matching Downloads"), `.unknownDate`
("Unknown Date"); `settings.spaces.manageInLibrary` ("Manage Spaces in
Library"). Analytics: module `library`, event `library_section_viewed`
(`section`, `is_standalone_window`), allow-listed in phi_telemetry.cc.

### Deferred / not ported

* Dragging pins and bookmarks between cards (the Mac's
  `LibrarySpaceItemDrag` pasteboard flows) — context-menu Move to Space ▸
  stands in.
* The Mac's title + URL editor (`EditPinnedTabPresenter`) for New / Edit:
  the sidebar's address prompt is used; a new pin or bookmark is titled by
  its address until the favicon/title upgrade.
* Moving a folder to another Space (the Mac moves whole subtrees).
* The standalone window's collapsible rail, its frame across launches, the
  Folio section, the card shadow blur, the spring flight.
* Split-pin editing of the second pane; "Edit…" edits the primary address.

### Box verification

1. ⋯ ▸ Library (and Ctrl+Shift+L, and Settings ▸ Spaces ▸ "Manage Spaces in
   Library" — Settings hides, the Library opens on Spaces): the scrim covers
   sidebar + content below the caption band, the card flies out of the ⋯
   button; the rail's Spaces / Downloads switch; Downloads popover ▸ "All
   Downloads" opens the Library on Downloads.
2. Spaces: one card per user Space with its theme ground, pins grid and
   outline; the icon button opens the picker; the name → inline rename
   (Return commits, Esc cancels, click elsewhere commits); the palette
   button opens the theme panel; a folder click discloses; double-click on
   a bookmark closes the Library, switches the window to that Space and
   focuses/opens the row (also for a Space whose browser is not yet
   attached); the handle drag slides the others and mouse-up reorders the
   strip; "+" shows the create card after the last one, Create inserts the
   new Space right after the window's current one WITHOUT switching to it
   (and `analytics.list` shows `space_created` with `surface: library`).
3. Downloads: the search filters live; hover shows the row actions; Copy
   Link / Pause / Resume / Cancel / Show in folder / Remove / Retry act;
   double-click opens a finished file; the empty states.
4. Esc, a click on the scrim, switching the active tab or Space (Ctrl+N,
   a tab click behind a torn-down Library) and closing the window all close
   it; a menu or prompt opened from the Library does not.
5. "Open Library in New Window": a 1120×780 window with the same view, the
   rail's ↗ hidden; its row double-click fronts the browser window.

### Fix-ups after the first box pass (2026-10-27)

* A row of a Space whose browser was not attached yet never opened: the
  fresh browser lands through `PhiShellWindow::AttachSpaceBrowser` (the
  attach makes it active; only the ShowSpace path notes the active Space).
  `ConsumePendingStoredOpen(space, drop_on_mismatch)` now runs from both —
  the attach keeps a pending open for another Space, the visible landing
  drops it.
* Escape while Chromium holds the focus: the view retakes the keyboard on
  the next turn and 80 ms later (`kMsgTakeFocus` / `focus_retake_`, never
  over its own fields), and the thread hook routes VK_ESCAPE through
  `PhiLibrary::HandleEscapeIfBoundTo` before the peeks — the view's
  `HandleEscape` (drag snaps back, rename / create card cancel, a filled
  search clears, else dismiss), swallowed either way.
* The Library ground is `CurrentPalette().ground_top` (the sidebar's own
  ground), the dark content base the window's resolved overlay (the linked
  palette's when there is one); the search pill and its EDIT share
  `FieldFill()` (ink over the content ground), answered for
  WM_CTLCOLOREDIT and WM_CTLCOLORSTATIC with the rename on its card's
  ground and ink.
* Hover is re-derived from the real pointer after every Refresh
  (`RefreshHover`: a synthetic WM_MOUSEMOVE / WM_MOUSELEAVE), so a row no
  longer keeps its pill after a context menu or a handle drop.
* The card's "…" menu opens upward from the button's top
  (`TPM_BOTTOMALIGN`); the ⋯ menu's Library row wears `fluent::kTaskView`
  (ViewAll), Open Folio keeps `fluent::kLibrary`.
* Second pass: the search EDIT and the pill share ONE stored value
  (`search_fill_`, computed in the layout pass), the rename EDIT and its
  card another (`rename_fill_`); the fields ERASE themselves with that value
  in `FieldProc` (WM_ERASEBKGND) and WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC
  answer it too, so the control's own background path is never trusted.
  The hosted create card's "Space name" EDIT does the same in
  `PhiCreateSpacePanel::FieldProc` (ink 4 % over `FormGround()`, the row
  wash its frame uses). `HandleEscapeIfBoundTo` stands down while
  `PhiModalDepth() > 0` or a `#32768` menu window is visible, so a tracking
  menu / prompt / picker keeps its Escape. A left press anywhere but on a
  pin or row (blank body, another card, the content ground, the rail)
  clears the selection. The Space card's shadow is three concentric rings
  2 dip down (black 18 / 10 / 4 %, light 10 / 5 / 2 %) plus a 1 px border
  in the card palette's `border`.
* Third pass (2026-10-27): the Library's search and rename EDITs, and the
  create-Space panel's "Space name" EDIT when it is hosted (Library card,
  create window), now paint through `PhiPaintEditOpaque`
  (`client/phi_opaque_edit.h`): WM_PRINTCLIENT into an offscreen 32bpp
  surface, alpha forced to 255, then blitted; `PhiEditDrawsDirectly` lists
  the messages after which the native EDIT drew synchronously, so the field
  is invalidated and redrawn through the same path. The shell extends its
  DWM frame under the whole client area, so GDI's alpha-0 output had the
  backdrop ADDED to it (each interior read ≈ intended + 128 per channel on
  the box). This is the address field's technique (`PaintField`), now
  shared. Remaining nits: the cue banner is the system grey text (low
  contrast on saturated dark grounds) and the caret is the native XOR caret.
