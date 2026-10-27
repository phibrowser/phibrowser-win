# Windows-native UI (the un-Mac redesign)

Status: DESIGN (2026-09-17). Nothing here is implemented. Mockups of every
surface below are on the canvas *Phi Windows UI redesign*
(https://claude.ai/artifact/52wTf6ySxBBYbuVatMzRjt): the two layouts, the
floating sidebar, the app menu, the Space flyouts, the dialogs, the
Settings window and the token sheet. §10 is the implementation plan; §11
the decisions still open. Jixiang's direction while reviewing the first
cut (2026-09-17): design the floating sidebar too; since the vertical
layout needs a title-bar row anyway, remove the Performance layout
entirely; and lay the vertical layout out like the reference screenshot
he sent — the address bar shares the page pane, which also carries the
window buttons and runs flush to the window's top and bottom, with the
sidebar beside it on the ground. All three are in.

Today the Windows client is the Mac client transcribed: a hidden-until-hover
menu bar on the caption row, a floating rounded page card with an 8 dip
gutter, capsule buttons, `#EAEAEA` popup grounds with a Mac alert layout, a
Settings window with a top toolbar of panes. It works, but it reads as a
port. This document replaces that with the conventions a Windows 11 user
already has in their hands — Chrome's and Edge's window structure, Fluent's
geometry, WinUI's dialogs and Settings — while keeping what makes Phi Phi:
Spaces (tinted chrome, the pip strip, agent Spaces), the vertical sidebar
with pinned cells and bookmarks, Peek/Reader.

What the research said, in one paragraph. Chrome and Edge put the tabs in
the title bar and a 40 dip toolbar under it; neither has a menu bar — every
command lives behind a trailing "⋯ / ⋮" app menu, context menus and
shortcuts, and Alt/F10/Alt+F open that menu. Edge's vertical-tabs mode hides
the title bar altogether: the caption buttons move into the toolbar row and
the tab pane starts below it. Fluent dialogs (ContentDialog) have no title
bar, a 20 px semibold title, equal-width buttons in a tinted footer, and the
affirmative button on the LEFT. Windows Settings is a left navigation rail
with a search box and full-width "settings cards" (icon, title, description,
control at the trailing edge). Arc's Windows build kept its sidebar but
never had the Mac's title-bar hiding — the caption buttons cost a band, and
that is why Phi's Performance layout is removed on Windows (§4.3).

---

## 1. Principles

1. **Windows geometry, Phi colour.** Radii, heights, type sizes, button
   shapes and dialog anatomy are Fluent's. The chrome's tint, the accent and
   the pip icons still come from the Space palette (`PhiPalette`), which is
   how Windows 11's own Mica reads: a tinted, flat ground.
2. **No menu bar.** One app menu (§3) reachable from the toolbar, from Alt,
   F10, Alt+F and Alt+E; everything else is a context menu, a toolbar
   button or a shortcut. No command loses a home (§3.2 is the full map).
3. **The page pane is a sheet, never a floating card.** In the vertical
   layout the page and its address row share one pane that runs flush to
   the window's top, right and bottom and meets the sidebar with rounded
   left corners (the reference screenshot's structure); in the horizontal
   layout the page is flush under Chrome's toolbar. The Mac's gutters,
   14 dip radius, corner caps and outer hairline go either way.
4. **The address row carries the caption buttons.** Vertical: at the
   trailing end of the pane's address row; Horizontal: in the tab-strip
   row (Chrome). There is never a caption-only band — which is why there
   are two layouts, not three.
5. **Density is Windows density.** Tab titles and sidebar rows at 12 dip,
   menus and dialogs at 14, page titles at 20/28 (§2.2). The current 13 pt
   rows are Mac-sized.
6. **Same model underneath.** `ShellLayout` stays a pure function of the
   inputs; the sidebar tree, the Space strip, the surface registry and the
   verb registry are untouched. This is a chrome redesign, not a controller
   rewrite.

---

## 2. Tokens

### 2.1 Geometry (dip)

| Token | Value | Replaces |
|---|---|---|
| toolbar row | 40 (horizontal), 44 in the page pane (vertical) | `kTitleBarHeight` 32 + `kTopBarHeightNav` 32 / `kTopBarHeight` 40 |
| tab-strip row (horizontal) | 40, tab 34 bottom-aligned | `kTitleBarHeightTabs` 40, `kHTabStripTop` 6 (same) |
| caption button | 46 × row height | `kCaptionButtonWidth` 46 (same) |
| toolbar button | 32 × 32, radius 4 | `kNavButton` 24, radius 4 |
| omnibox | 32 high, radius 8 | 32 / r8 sidebar, 24 bare top, 28 pill comfortable |
| Space pip | 28 in a 36 row, radius 6 | 24–26 in 32, radius 9 |
| Space chip (horizontal) | 28 high, radius 6, icon + name + chevron | the lone compact pip |
| sidebar width | 240 default, 200–480 | 240, 193–500 |
| sidebar row | 36, radius 6, 8 side inset | 36, radius 8 |
| pinned cell | 44 high, radius 6, 8 gap | 45 / r8 |
| group header | 32 | 32 |
| bottom bar | 40 | 24 + 8 spacer |
| page pane (vertical) | flush top / right / bottom, left corners radius 8 against the sidebar, no gutter | 14 + caps, gutter right / bottom, 4 under the band |
| page sheet (horizontal) | flush, square | — |
| flyout / dialog surface radius | 8 | 12 (hover card), 8 |
| control radius | 4 | 4–8 |
| gutter | 8 | 8 |
| window corners | DWM (8) | DWM |

### 2.2 Type (Segoe UI Variable; Segoe UI on Windows 10)

| Role | Size / weight | Used by |
|---|---|---|
| Title | 28 / 600 (Display) | Settings page title |
| Subtitle | 20 / 600 (Display) | dialog title |
| Body | 14 / 400 | omnibox, menus, dialogs, settings rows |
| Body strong | 14 / 600 | settings group headers, chip name |
| Caption | 12 / 400 | tab titles, sidebar rows, bookmark bar, tooltips, shortcuts in menus |
| Hint | 11 / 400 | second lines |

Weights: 400 and 600 only. Faces: Segoe UI Variable Text for 11–14,
Segoe UI Variable Display for 20 and 28 (Segoe UI on Windows 10).

### 2.2a Icons and buttons — one rule for every surface

Jixiang's review (2026-09-17) caught the pin, the reload arrow and the
search glyph drawn in three different weights. The rule, so it cannot
happen in the product (the "System" artboard on the canvas shows it):

* **One icon family**: Segoe Fluent Icons on Windows 11, Segoe MDL2
  Assets on Windows 10, chosen at start-up by font presence. No Mac
  artwork masks, no hand-rasterised PDFs, no per-surface glyph fonts —
  `PhiSidebarToggleIcon` and the other masks in `phi_mac_icons.cc` go.
  The Space icon catalogue and the agent brand marks are the only
  compiled masks that stay, and they draw inside the 28 dip pip only.
* **One grid and weight**: every glyph sits on the font's 24-unit em
  grid with a 2-unit safe margin and a ~1 px stroke at 16 dip (the
  Fluent hairline). Nothing is drawn with GDI arcs any more: the reload
  arrow, the pin, the chevrons, the "+" are all font glyphs
  (`PhiDrawTextAA` through the DirectWrite layer, as the caption glyphs
  are today).
* **Three sizes only**: **16** in toolbar buttons and in-field slots,
  **14** inside pips, **20** in Settings cards; the OS caption glyphs
  stay at their own 10 dip / 1 px (ChromeMinimize E921, ChromeMaximize
  E922, ChromeRestore E923, ChromeClose E8BB — unchanged).
* **One button**: 32 × 32, radius 4, icon centred, in `label` ink.
  States — rest: transparent; hover: ink 4.5 %; pressed: ink 9 % with
  the icon at 75 %; disabled: icon at 36 %, no hover; toggled on
  (Reader active, sidebar shown): `accent_soft` fill (accent 14 %) with
  the icon in the accent. In-field slots are 24 × 24 with the same
  states and the icon in `dim`. Pips are 28 × 28, radius 6. Text
  buttons (dialog footer, Settings) are 32 high, radius 4, 14 dip.
* **Glyph map** (Fluent / MDL2 names; verify the code points against
  the installed font at implementation): Back E72B / Forward E72A (the
  arrow pair, not the Chrome* caption variants), Reload `Refresh` E72C, Search tabs `Search` E721, New tab
  `Add` E710, App menu `More` E712, Sidebar toggle = the stateful pair
  `ClosePane` E89F while the sidebar is shown / `OpenPane` E8A0 while it
  is hidden (a panel outline with an arrow into or out of it — the
  Windows spelling of « / », which would read as Back beside the nav
  trio; Jixiang 2026-09-17), Pin `Pin` E718 / `Unpin` E77A, Downloads
  `Download` E896, Extensions `Puzzle`, Split `DockLeft`/`Split`,
  Bookmark `FavoriteStar` E734, Reader `ReadingMode` E736, History
  `History` E81C, Print `Print` E749, Zoom in/out `ZoomIn` E8A3 /
  `ZoomOut` E71F, Full screen `FullScreen` E740, Settings `Settings`
  E713, Help `Help` E897, Feedback `Feedback` ED15.
  Where the Windows 10 font lacks a Fluent-only glyph the MDL2 twin is
  used; there is no third fallback.
* The nav-hover ordinal table (`phi_shell_window.cc:2576`) stays the
  hit-test key; painting goes through ONE `PaintIconButton(dc, rect,
  glyph, state)` so no surface can invent its own wash or size.

### 2.2b The brand mark

The Phi mark is the shipped app icon (`client/resources/phi.ico`,
16 / 32 / 48 / 256 — the Windows mark, not the Mac's `PhiIcon.icon`),
never redrawn as a glyph. Where it appears: the **caption of every
secondary window** (Settings, Feedback, URL rules, Create Space,
Onboarding) at 16 dip with a 3 dip radius, leading the title; the
**About page** at 48; the taskbar, Start, the installer and the
default-browser prompt through the OS. Where it does not: the main
window's chrome (the Space icon owns that identity; the old Phi logo
button and menu title go with the menu bar) and dialogs (§6 drops the
Phi mark).

### 2.3 Colour

Light: chrome tint = the Space's `ground_top` (flat, no vertical band
gradient any more); card / toolbar / active tab `#FFFFFF`; flyout `#F9F9F9`;
dialog body `#FFFFFF`, dialog footer `#F3F3F3`; stroke ink 9 %; separator
ink 7 %; hover ink 4.5 %, pressed ink 9 %; accent = Space accent; danger
`#C42B1C`; label ink 87 %, dim 62 %, tertiary 42 %.

Dark (the canvas's "Dark mode" page shows every board in it): tint =
the Space's dark ground; card / toolbar / active tab `#2B2B2B`; flyout
`#2C2C2C`; dialog body `#2B2B2B`, footer `#202020`; control fill
`#333333`; stroke white 8 %, strong 16 %; separator white 7 %; hover
white 6 %, pressed 10 %; label white 90 %, dim 62 %, tertiary 42 %;
accent = the Space's dark accent (lighter than the light one, e.g.
`#5FB3DE` for `#2A86B5`) with near-black text on it; danger `#E0463A`.
The address row paints on `card` (`#2B2B2B` in dark) — unless the page
reports its own colour. Then it follows the page, as the Mac's
WebContentHeader does: Mirage's page-chrome-color script reports the
page's background (`phinomenonPrivate.setPageColor`), the framework carries
it as `TabSnapshot::page_color_argb`, and the row takes it as its ground
with the ink WCAG contrast picks (white when white contrasts more than
black), every wash and hairline re-derived over it (`PhiPageBarPalette`).
In comfortable the strip's active tab and the bookmark bar follow it too;
the rest of the strip keeps the Space tint. The native chrome stays on the
new tab and bookmark pages, a placeholder, a split, the group overview and
a crashed tab, and the strip's active tab keeps it while Reader View covers
the page. A cross-document navigation drops the colour until the new page
reports; a reload keeps it. Appearance follows Settings › Appearance ›
Color appearance (System / Light / Dark); "System" tracks the Windows app
mode.

The `#EAEAEA` / `#252525` "fixed popup ground" is retired: secondary
windows (Settings, Feedback, URL rules, Create Space, Onboarding) take the
Windows Settings ground `#F3F3F3` / `#202020` and card surfaces.

The eight built-in themes (Pure, Mint, Mist, Aqua, Iris, Petal, Coral,
Amber — `phi_theme.cc`) and the saturation slider are unchanged; the
"Themes" artboard on each canvas page shows all eight on the vertical
layout at the default saturation, plus Mint at 0 / 10 / 50 / 100, with
the tint and accent values the client computes (tint = the theme's
overlay over the ground at 20 → 80 / 255 by the slider).

### 2.4 Elevation and motion

Sheets (the page pane, the split panes, the address row while split):
**0 2 8 @12 % plus a 1 px ink 3 % halo** (dark: 0 2 8 @45 %), so the pane
floats on the tint instead of lying on it (Jixiang, 2026-09-17).
Flyouts: 8 dip radius, 1 px stroke, shadow 0 8 16 @14 %. Dialogs: 0 32 64
@19 % + 0 2 21 @15 %. Space swap and theme ramp keep their 150 / 600 ms;
flyouts open with Fluent's 83 ms fade + 8 dip slide; the tab-strip
drag-reorder animation is unchanged.

---

## 3. The app menu (the menu bar's replacement)

### 3.1 Anatomy

A "Settings and more" button (⋯, `more`) is the trailing item of every
toolbar cluster. It opens a Fluent flyout: 320 dip wide, radius 8, items
34 high with a 16 dip icon column, a 14 dip label and a right-aligned
12 dip shortcut in the tertiary colour, submenus with a chevron, separators
inset 12. Chrome's inline zoom row (− 100 % + ⛶) is kept; Chrome's inline
Edit row is not — Phi's Edit commands only ever drove the address EDIT,
which has its own context menu.

```
New tab                         Ctrl+T
New window                      Ctrl+N
New incognito window            Ctrl+Shift+N   (disabled until supported)
─
Spaces                               ▸
History                              ▸
Bookmarks                            ▸
Downloads                       Ctrl+J
Extensions                           ▸
─
Zoom            [−]  100 %  [+]  [⛶]
Print…                          Ctrl+P
Find on page…                   Ctrl+F
Reader view                     Ctrl+Alt+R     (checked while active)
Layout                               ▸
More tools                           ▸
─
Settings                        Ctrl+,
Help                                 ▸
Exit                            Ctrl+Shift+Q
```

Submenus:

* **Spaces ▸** New Space… · Rename Space… · Change icon… · Edit theme… ·
  Change profile ▸ · Delete Space… · ─ · Next Space Ctrl+Alt+→ · Previous
  Space Ctrl+Alt+← · ─ · one row per Space (icon, name, Ctrl+1…9, active
  row checked; agent Spaces after a separator with their status dot) · ─ ·
  URL rules… · ─ · New profile… · Delete profile ▸.
* **History ▸** Back Alt+← · Forward Alt+→ · Home Alt+Home · ─ · Recently
  closed (rows) · ─ · Show full history Ctrl+H.
* **Bookmarks ▸** Bookmark this tab Ctrl+D · Bookmark all tabs Ctrl+Shift+D
  · ─ · Show bookmarks bar Ctrl+Shift+B (horizontal layout only) ·
  Bookmark manager Ctrl+Shift+O · Export bookmarks… · ─ · the tree.
* **Extensions ▸** Manage extensions · ─ · one row per extension (opens its
  action, the same rows the puzzle popover lists).
* **Layout ▸** Horizontal tabs · Vertical tabs (radio) · ─ · Show sidebar
  Ctrl+S · Show bookmarks bar Ctrl+Shift+B.
* **More tools ▸** Open file… Ctrl+O · Save page as… · Task manager · ─ ·
  View source Ctrl+U · Developer tools Ctrl+Shift+I · JavaScript console
  Ctrl+Shift+J · Inspect elements · ─ · Delete browsing data… · Import from
  another browser… · Export user data… · Import user data… · ─ · Agent
  autoview (check).
* **Help ▸** About Phi · Check for update… · What's new · Documentation F1
  · ─ · Report an issue….

Items whose feature has no Windows surface yet stay listed and disabled,
exactly as the bar does today (`phi_menu_bar.cc` `BuildStatic`).

### 3.2 Where every menu-bar item goes

| Today (menu bar) | Tomorrow |
|---|---|
| Phi ▸ About / Check for update / Delete browsing data / Import / Quit | Help ▸ About, Help ▸ Check for update; More tools ▸ Delete browsing data, Import; Exit |
| Phi ▸ Preferences | Settings (and Ctrl+,) |
| File ▸ New Tab / Window / Incognito / Reopen closed / Open file / Open location / Close window / Close tab / Save page / Print | top of the app menu; Reopen closed = tab context menu + Ctrl+Shift+T; Open file, Save page = More tools; Open location = Ctrl+L only (Chrome's rule); Close window / Close tab = caption × / tab × + chords |
| Edit ▸ Undo … Select all, Paste and match style, Delete | the address field's own context menu (already a native EDIT menu) |
| Edit ▸ Find ▸ Find / Next / Previous / Search the web | Find on page… (app menu); Next / Previous = the find bar's arrows + Enter / Shift+Enter |
| View ▸ Stop / Reload / Force reload | reload button (stop while loading); Force reload = Ctrl+Shift+R and the reload button's right-click menu |
| View ▸ Full screen / Actual size / Zoom in / out | the zoom row |
| View ▸ Reader view / Agent autoview | Reader view (app menu + omnibox button); Agent autoview = More tools |
| View ▸ Developer ▸ … | More tools |
| View ▸ Layout mode ▸ / Bookmark bar / Toggle sidebar | Layout ▸ |
| View ▸ Toggle chatbar / New conversation | dropped for now (Chat is out of scope) |
| Spaces ▸ everything | Spaces ▸, the Space chip / pip context menu (§5.4), the switcher |
| History ▸ everything | History ▸ |
| Bookmarks ▸ everything | Bookmarks ▸, the bookmarks bar's own menu |
| Tab ▸ New tab to the right / Duplicate / Mute / Next / Previous / Search tabs / tab rows | tab context menu (New tab to the right, Duplicate, Mute); Ctrl+Tab MRU; the tab-search button; the tab rows ARE the sidebar / strip |
| Window ▸ Minimize / Zoom / Name window / Downloads / Extensions / Task manager | caption buttons; Downloads and Extensions in the app menu; Task manager = More tools; Name window dropped |
| Help ▸ Report an issue / What's new / Documentation / Manage user data | Help ▸; user data = More tools |
| comfortable overflow ▸ Chat / Send feedback | Chat dropped for now; Help ▸ Report an issue and the sidebar Feedback pill |

### 3.3 Keyboard

* **Alt** (solo tap) and **F10** open the app menu with the first item
  focused — the Alt-reveal state machine (`PhiCaptionBand::SetRevealed`,
  `ToggleAltReveal`) goes away. **Alt+F** and **Alt+E** open it too
  (Chrome's and Edge's mnemonics).
* Inside the flyout: arrows, Enter, Esc, letter mnemonics as any Win32
  menu, because it IS one: the flyout is a `TrackPopupMenu` over the same
  HMENU tree `PhiMenuBar` builds today, owner-drawn to the Fluent look
  (icon column, right-aligned chords, 34 dip rows — the code the tab
  context menus already have in `phi_shell_menus.cc`). The zoom row is the
  one custom item (MFT_OWNERDRAW with an inline hit test) — if that proves
  awkward, Zoom ▸ becomes a plain submenu.
* Every accelerator stays (`phi_keyboard_accelerators.cc`, the rebindable
  set in `phi_preferences.cc`). Two additions: **Alt+Home** is already
  there; **F1** opens Documentation once there is a page to open.

### 3.4 The remote surface

`agentSpace.menu.list` / `menu.invoke` (`PhiMenuBar::ListJson`,
`InvokePath`) keep working: they walk the app menu's HMENU tree instead of
the bar's. Paths change from `File/New Tab` to `New tab`,
`More tools/Task manager` etc.; `test/suite-menu.mjs` is updated with the
new paths in the same change.

---

### 3.5 Context menus (the "Context menus" artboard)

What exists (`phi_shell_menus.cc`): one HMENU builder per surface — tab
row (with split and group variants), tab area, pinned cell, bookmark
row and folder, bookmarks-bar item, group header, agent pip — no
shortcut text on any item, no menu at all on a user Space pip, the
Space block (New / Rename / Change icon / Set theme / Change profile /
Delete) appended to the tab-area menu, the horizontal strip's empty
run opening the tab-area menu and the vertical layouts' caption run
falling through to the Windows system menu.

Windows version — every context menu is the §3.1 flyout (radius 8,
32 dip rows, a 16 dip icon column on the primary items, 13 dip labels,
right-aligned shortcuts in `tertiary`, chevron submenus, destructive
rows in `danger` at the bottom; disabled rows grey — the "inert but not
greyed" incognito state goes):

* **Sidebar empty space** (and the horizontal strip's empty run, and
  the pip row's empty run): New tab Ctrl+T · Reopen closed tab
  Ctrl+Shift+T · ─ · New folder… · Bookmark all tabs Ctrl+Shift+D · ─ ·
  Layout ▸ · Hide sidebar Ctrl+S · ─ · **Space ▸** · ─ · Task manager.
* **Space ▸** — also what a right-click on a Space pip or the chip
  opens (new): New Space… · ─ · Rename Space… · Change icon… · Edit
  theme… · Change profile ▸ · ─ · Delete Space… (danger; disabled on
  the last Space).
* **Address row empty run** (vertical): the Windows **system menu**,
  untouched — Restore · Move · Size · Minimize · Maximize · ─ · Close
  Alt+F4. Windows users expect it on a title bar and it is where the
  keyboard's Alt+Space lands.
* **Tab row**: Pin · Duplicate · Copy link Ctrl+Shift+C · ─ · Split with
  current · ─ · Add to bookmark Ctrl+D · Add to folder ▸ · ─ · Add to
  group ▸ / New tab group (Move to group ▸ · Move to new group · Remove
  from group when grouped) · ─ · Move to Space ▸ · Clone to Space ▸ ·
  **Move to new window** (new, Chrome's) · ─ · Close Ctrl+W · Close
  other tabs. Split rows keep their variant (Pin split, Copy left /
  right URL, Reverse panes, Convert to stacked / side-by-side, Remove
  from split, Add split to bookmark). The agent prefix (Take control /
  Hand back) stays on driven rows.
* **Pinned cell**: Open · Duplicate · Copy link · Rename… · Edit URL… ·
  ─ · Unpin · ─ · Close Ctrl+W (disabled while the pin is closed). Split
  pins as today.
* **Bookmark row** (outline and bookmarks bar): Open in new tab · Open
  as split · Copy link · Rename… · Edit URL… · ─ · Move to Space ▸ ·
  Clone to Space ▸ · ─ · Delete (danger). **Folder**: Rename… · Change
  icon… · New nested folder… · ─ · Delete folder (danger). Bookmarks-bar
  empty run: New folder….
* **Group header**: **New tab in group** (new, the Mac's) · Rename group…
  · Change color ▸ (nine swatches, current checked) · ─ · Ungroup ·
  Close group (danger). Labels move to `strings.json` (today they are
  hard-coded English).
* **Omnibox**: the native EDIT menu plus one Phi row at the top, **Paste
  and go** (Chrome's and Edge's), shown only when the clipboard holds
  text. **Extension icons**: Chromium's own menu, unchanged.
* Every menu is still refused while an agent holds the window, except
  the agent pip's and the bookmarks bar's.

Implementation: `PhiMenuSpec` gains `shortcut` and `danger` per item and
the owner-draw path from P1 paints them; the pip hit in
`OnChromeContextMenu` opens the Space submenu instead of returning; the
vertical caption run is left to `DefWindowProc`; `shell_menus_test.cc`
pins the new orders.

## 4. The two layouts

The Settings labels become **Horizontal tabs** and **Vertical tabs**
(Edge's words); the wire names `comfortable` / `balanced` do not change,
so the setting still round-trips with the Mac. **Performance is removed
on Windows** (§4.3). Both layouts share: one address row that carries
the caption buttons at the window's top-right, the tool cluster, the
omnibox, the app menu.

The **tool cluster** (right-to-left, 8 from the caption divider or the
edge): ⋯ app menu · Extensions (puzzle) · the pinned-extension icons
(28 wide slots, 2 apart). No Downloads button (Jixiang, 2026-09-20):
downloads live in the sidebar's bottom bar, the app menu and Ctrl+J. No
split button (Jixiang, 2026-09-17): splitting stays a tab-menu /
split-row / drag action. Peek and memory keep
their current homes. **Chat is out for now** (Jixiang, 2026-09-17): no
capsule anywhere, no chatbar items in the app menu; the AI-build code
paths stay behind `kPhiAiEnabled` untouched until the feature returns.

The **omnibox** in every layout: 32 high, radius 8, 14 dip text with the
host in `label` and the path in `dim`, a leading 24 dip site-info slot
(lock / page glyph, click = Chromium's page-info bubble via the existing
anchor query), trailing 24 dip slots for Reader (when offerable) and
Bookmark-this-tab (star, new — Ctrl+D already exists). Fill is `field`
(ink 4.5 %) on a white toolbar and `card` on a tinted ground; focus adds a
1 px stroke and a 2 px accent bottom edge (Fluent TextBox). The 84 dip
minimum-text rule for hiding the trailing stack stays. **Resting fill
everywhere** (Jixiang, 2026-09-17): the field is never bare — a tinted
box is Windows' "type here" signal, and the row is too busy for floating
text. Rest: `field` (ink 4.5 % light / white 6 % dark), radius 8, no
stroke, text at full ink, placeholder "Search or enter address" in
`tertiary` with the search glyph leading when empty; hover: ink 7 %;
focus: `card` body, 1 px stroke, 2 px accent bottom edge, the full URL
selected. **Read-only addresses** (a tab an agent holds, the Kiosk) keep
no fill and draw their text in `dim` with the eye / lock glyph, so a
box always means "you can type" and dim text always means "you cannot".

**Window drag (HTCAPTION)**: the address row minus its controls, the
empty run of the tab strip, the sidebar header's empty run in the
vertical layout, and a reserved 32 dip gap between the omnibox and the
tool cluster so a maximised omnibox never swallows every drag point. `WM_NCCALCSIZE` / `WM_NCHITTEST`
code in `PhiCaptionBand` stays; only the rects it reads change.

### 4.1 Horizontal tabs (`comfortable`) — Chrome

```
┌ tint ──────────────────────────────────────────────────────────────┐
│ [Work ▾] ▐ tab ▌ tab  tab  [Reference ▾]  +            🔍 │ – ▢ × │ 40
├ white ─────────────────────────────────────────────────────────────┤
│ ← → ⟳  [🔒 github.com/phibrowser…        ≡ ☆]  ⚙ ⤓ ⋯             │ 40
│ Gmail  ▸Work  Design system notes  LinkedIn                        │ 28 (optional)
├────────────────────────────────────────────────────────────────────┤
│ page                                                               │
```

* Row 1 is the tab strip on the tint. Leading: the **Space chip** (§5.2)
  at x 8; then tabs from x 8 after it. Tabs are 34 high, bottom-aligned,
  200 wide (240 active, 56 min); the active tab is a white rounded-top
  (radius 8) shape continuous with the toolbar; inactive tabs are text
  with a radius-8 hover wash and a 1 × 20 separator between neighbours
  (Chrome's CR23 strip). Group chips stay (28 high, radius 6, 1 px stroke,
  the group colour dot). "+" is a 28 button after the last tab.
  Trailing: the tab-search button, a 16 gap, the 1 × 16 divider, the three
  caption buttons at 46 × 40.
* Row 2 is the toolbar on white: nav trio (32 buttons, 4 apart) at x 8,
  the omnibox filling the middle, the tool cluster.
* Row 3, optional, is the bookmarks bar: 28 high, white, 12 dip font,
  favicon + title runs 14 apart, folders with a folder glyph.
* Page: full width, flush, a 1 px stroke under the toolbar. No card.
* The Phi logo button goes (Chrome has none; the app menu carries the
  brand under Help ▸ About). The system menu stays on Alt+Space.

### 4.2 Vertical tabs (`balanced`) — sidebar beside the page pane

```
┌ tint ────────────────────────────────────────────────────────────────┐
│ ◼ ◻ ◻ ◻ +              ╭ ▤ 🔍  ← → ⟳  🔒 github.com/…  ≡ ☆   ⚙ ⤓ ⋯ │ – ▢ × │
│ Pinned                ├──────────────────────────────────────────────────┤
│ [M][G][N][L]          │ page                                             │
│ Bookmarks             │                                                  │
│ ▸ Work                │                                                  │
│   Design notes        │                                                  │
│ Tabs                  │                                                  │
│ + New tab      Ctrl+T │                                                  │
│ ▐ GitHub            × │                                                  │
│   Edge docs           │                                                  │
│ ▾ Chromium refresh    │                                                  │
│ ⤓            Feedback ╰──────────────────────────────────────────────────┤
└───────────────────────────────────────────────────────────────────────────┘
```

The reference is the screenshot Jixiang sent: the sidebar stands on the
tinted ground with its own header; the page pane carries the address row
and the window buttons and runs flush to the window's top, right and
bottom — no gutter around it (Jixiang: "no need for space around the
page pane, left and right is ok").

* **The pane**: x = sidebar width to w, y 0 to h; left corners radius 8,
  a 1 px stroke on its left edge, the **sheet shadow** (§2.4) all round
  so it floats on the tint; `card` fill. Its first row (44) is the **address row**: the **sidebar toggle**
  (`ClosePane` while shown, `OpenPane` while hidden; tooltips "Hide
  sidebar (Ctrl+S)" / "Show sidebar (Ctrl+S)") and **tab search** at x 8 (Jixiang, 2026-09-17: the Space row has no
  room for them — they moved out of the sidebar header), a 4 dip gap,
  the nav trio, the omnibox with its **resting fill** (§4's rule — the
  bare Mac field went on 2026-09-17), a 32 dip drag gap,
  the tool cluster, a 1 × 16 divider, and the **three caption buttons**
  at the row's trailing end (46 × 44, square, in the window's top-right
  corner as Windows expects). A 1 px separator closes the row; the page
  fills the rest. `WM_NCCALCSIZE` still removes the OS caption; the row's
  empty run is `HTCAPTION`; the window's first `border_y` pixels stay
  `HTTOP`.
* **The sidebar** (240 default, 200–480; the 4 dip grip at its edge is
  unchanged): a **header row** (44, from y 0 — the same row as the
  pane's address row, so pips and omnibox share one centre line across
  the window) holding **only the Space pips** at x 8 (28 dip, 4 apart,
  "+" trailing them; §5.1) — the whole width is theirs, so eight Spaces
  fit before the overflow dots. With a single Space the row collapses
  and the sections start at y 0. Below it the bands follow each other
  with **no section labels** (Jixiang, 2026-09-18: "no Pinned /
  Bookmarks / Tabs header needed"; an 8 dip gap parts them): the 44 dip
  cell grid, the outline (with the dashed "Drag tabs here" well when
  empty, as the drag zone already does), then "+ New tab" (Ctrl+T keycap
  trailing) and the tab list. The one line that stays is the agent
  Space's "Claude's tabs" cue (§5.10). The bottom bar (40): the
  **profile button** (the Mac's `ProfileButton`, 2026-10-27) left-most at
  x 8 — a 24 dip disc, the account picture circle-clipped with a 1 px
  ink-8 % ring when signed in, the Fluent Contact glyph otherwise, no
  hover wash — then Downloads and memory, the Feedback pill trailing. No
  Chat capsule (out of scope for now). The profile button drops the Mac's
  `populateProfileMenu` upward (its bottom edge 5 dip above the button,
  left-aligned): Download List (→ Library ▸ Downloads), Manage Bookmarks,
  Manage Extensions, ─, New Tab, New Incognito Space, New Incognito
  Window, ─, Settings, and Sign in while no account is signed in — each
  row with the user's effective chord.
* Rows: 36 high, radius 6; the active tab is a `card` tile with a 1 px
  stroke and a 0 1 2 shadow (the screenshot's raised "New Tab" row);
  hover ink 4.5 %; close box 20 on hover / active; dimmed rows at 62 %.
* Hidden sidebar: §4.4.

### 4.3 Performance is removed

The Mac's Performance layout exists to give the page every vertical
pixel: no toolbar above it, the address bar in the sidebar. On Windows the
caption buttons have to sit in a row at the window's top-right, so the
page gets a 40 dip band above it in every layout and Performance would
only move the address field into the sidebar for no gain. It goes:

* `PhiLayoutMode` keeps `kPerformance` in the enum for one release so the
  wire name still parses; `PhiPreferences::RequestLayoutMode` and the
  loader map it to `kBalanced` (and write `balanced` back), the Settings
  page and the Layout ▸ submenu show two choices, the onboarding step
  shows two cards, the `layout.set` verb refuses `performance`. The Mac
  keeps its three; a profile moving from a Mac loads as Vertical tabs.
* `ComputeShellLayout` loses its performance branch, the sidebar address
  bar (`SideAddressBar`, `kAddressBarTop`, `kHeaderHeight`), the copy-link
  in-field button (Ctrl+Shift+C stays), `PhiOmniboxSession::HostField /
  UnhostField` (the field no longer lives in the sidebar), the
  performance-only nav trio on the band and the `top_caps` corner mode.
* `test/suite-layout.mjs`, `fs-mode.mjs` and the PhiCap layout cycle drop
  their performance step.

### 4.4 The floating sidebar (Vertical tabs, sidebar hidden)

The address row's toggle hides the docked sidebar; the pane then fills
the window (x 0, square corners) and the toggle stays where it is, the
leading button of the address row, with tab search beside it (Edge's
placement; the "sidebar hidden" artboard on the canvas). The sidebar comes back two
ways:

* **Docked**: click the toggle, or Ctrl+S. The page shrinks to x 240.
* **Floating**: while hidden, the panel is completely invisible, including
  its border and shadow (Jixiang, 2026-09-28). Hovering the invisible
  8 dip strip at the left edge for 300 ms slides the panel over the page.
  Leaving slides it fully out and hides its native window. No strip of
  the panel remains over the browser when the animation finishes.

The panel: x 8, **y 52** (below the 44 address row plus 8 — the panel
never covers the address bar, the nav buttons or the toggle), width 240
(the docked width), bottom at h − 8, radius 8, over the pane's left
edge, the Space tint as ground and a 1 px `stroke_strong` border clipped
to the browser area. Native popup shadows are disabled
because they escape the clipping region during the slide. The popup never
reserves page space: the browser viewport keeps the collapsed shell's
width and height during reveal, interaction and dismissal. Inside it is the
docked sidebar exactly — the pip row, the sections, the tab list, the
bottom bar — and nothing more: no pin button (dropped 2026-09-18 at
Jixiang's request; the caption's toggle docks the
sidebar). The omnibox stays in the address row; there is no field in the panel any more (the
reparenting code goes with Performance, §4.3).

Shows: left-edge hover after 300 ms; a tab drag from the bookmarks bar or
from another window crossing the left edge (at once); the Spaces menu /
switcher opened from the keyboard while hidden. Slide: 150 ms ease-out
from x −16 with a fade, the panel painted as one layered window
(`PhiCommitShapedLayer`, as the hover card).

Hides: 150 ms after the pointer leaves the panel AND the hot strip; Esc;
a click on the page or the toolbar; the window deactivating; a Space
switch that leaves nothing hovered. Stays while: a drag is in flight, a
context menu or the switcher flyout is open from it, a rename field in
it has focus.

Follows the shell: the panel is repositioned on `WM_WINDOWPOSCHANGED` /
`WM_SIZE` of the shell (today it does not — noted as open in the floating
sidebar work of 2026-09-12) and hidden while the shell is minimised.

Hit-testing: the panel is a `WS_POPUP` owned by the shell, activation-
free (`WS_EX_NOACTIVATE`), so the page keeps focus; its rows route to
the same `PhiSidebarView` hit path with the panel's origin, exactly as
`floating_layout_` does today. Hidden, the popup sits at x = −240 and
uses `SW_HIDE`; its timer observes the invisible edge target independently
of the popup's visibility. Repositioning the shell preserves this state.

Remote surface: `agentSpace.window.layout` keeps reporting `sidebarToggle`
and `sidebarCollapsed`, and gains `sidebarFloating` (true while the panel
is up); `agentSpace.layout.set {sidebar: "docked" | "hidden"}` docks or
hides.

### 4.5 Sidebar states (the "Sidebar states" artboard)

Every state the sidebar tree can produce, with its Windows look. Rows are
36 high, radius 6; the metrics are §2.1's.

* **Pinned, one row**: 44 dip cells, 8 gap, 8 inset, 4 columns at the
  240 default (3 at 200, 5 from 280 — `phi_sidebar_layout.h`'s
  breakpoints stay). A cell shows its 18 dip favicon centred; an **open
  pin** carries the open pill (§5.8) under its favicon when "Show open
  tab indicators" is on. Active pin = `card` tile with the 1 px stroke
  and the 0 1 2 shadow.
* **Pinned, several rows**: rows add 44 + 8 each (two rows = 96 + 16
  inset); the empty band is 0 high and grows to the 100 dip drop zone
  only during a drag, as today. A **split pin** shows its two favicons
  side by side (16 each, 2 apart) in one cell.
* **Bookmarks, folder closed**: the closed folder glyph (16, `dim`),
  name, a trailing right chevron (12, `tertiary`). **Folder open**: the
  open-folder glyph (the flap lifted, Fluent's OpenFolder) and a down
  chevron, children indented 10 per level (`kBookmarkIndent`), nested
  folders the same. A bookmark whose tab is **open** shows the open pill
  under its favicon (§5.8); an **unloaded** one draws its favicon at
  30 % (the "Dim unloaded tab icons" pref, unchanged). Empty outline
  while a drag is in flight: one dashed 36 dip well "Drag tabs here"
  (the screenshot's).
* **Tab group, closed**: a 32 dip header — 10 dip colour dot, name
  (12 / 600), a count badge in the group colour (18 high pill, 10 dip
  white text), a right chevron. **Open**: the header with a down chevron
  and the members in an ink 3 % well (radius 6, 4 bottom pad), members
  indented 8 (`kGroupMemberLeadInset`) with 4 trailing. Click on the
  header toggles; the group colour palette is Chromium's, unchanged.
* **Split rows**: a split is ONE row split in two halves — favicon +
  title, a 1 × 16 hairline, favicon + title — each half ellipsising on
  its own. Active = `card` tile with the close box (closes both panes);
  the tab preview card shows both panes stacked, as today. Right-click:
  Convert to side-by-side / stacked, Unsplit, plus the tab menu.
* **Dimmed rows** (unloaded tabs, when "Dim unloaded tab icons" is on):
  62 % ink on the title, favicon at 62 %.

### 4.6 Split view (the two "Split view" artboards)

> **Superseded 2026-10-13 for the panes' chrome** (Figma 9044:26952): the
> panes sit flush in the content card with no sheets, no gutter and an
> unchanged address row; a 1 dip divider; the 2 dip ring inside the
> focused pane only. See `split-view-chrome.md`. The one-omnibox rule and
> the hover chip below still hold.

The reference is the second pair of screenshots Jixiang sent: **each pane
is its own sheet with its own address row**, and the window buttons sit
in whichever pane touches the window's top-right corner. The divider is
thinner than the reference's.

* **Two sheets, not one.** The page area (x = sidebar to w, y 0 to h)
  holds two `card` sheets on the Space tint with a **4 dip gap** between
  them — that gap is the divider: transparent, the resize grip, showing
  a 3 × 28 (or 28 × 3) `stroke_strong` pill at its middle on hover and
  while dragging. Snap points at 1/3, 1/2, 2/3 within 18 dip stay.
  Corners: every corner that meets the tint is radius 8; corners on the
  window edge are square. Side by side: A `8 8 8 8`, B `8 0 0 8`.
  Stacked: A `8 0 8 8`, B `8 8 0 8`. Each sheet has a 1 px stroke.
* **One omnibox — Chrome's way** (Jixiang, 2026-09-17, after trying
  per-pane rows and a parted band: neither read well). The address row
  is **unchanged by a split**: its one omnibox shows the **focused
  pane's** URL and page info, and clicking a pane focuses it — the field
  swaps with a 120 ms cross-fade, Ctrl+L goes there. The window items
  stay exactly where the unsplit row has them.
* **Which pane is focused**: the focused sheet's edge is a **2 dip
  accent stroke** in place of the hairline. The unfocused pane
  shows its identity on hover: a **hover chip** at its top-right (28 dip
  pill, `flyout` ground, flyout shadow) with the favicon, the host in
  `dim`, and a ⋯ **pane menu** — Swap panes · Convert to stacked / side
  by side · Unsplit · Close pane. The chip fades in over 150 ms when the
  pointer enters the pane and out 600 ms after it leaves; it is never
  shown on the focused pane (the address row speaks for it). No per-pane
  rows, so both panes keep their full height.
* **Stacked**: the same rule; the ring and the chip do the work, the
  divider runs across.
* **Sheets merge with the window border, and every pane keeps its
  corners** (Jixiang, 2026-09-17): a pane edge that touches the window's
  border is flush — no gutter — but **all four corners of every pane
  stay radius 8**. Windows 11 rounds the window itself at 8, so a flush
  pane's corner coincides with the window's corner and the rounding
  costs nothing. The panes' left edge is the **same x as the address
  row's** — flush to the sidebar, no 4 dip inset there (Jixiang,
  2026-09-17), so the row and the sheets share one left border line;
  the tint shows only in the gutter under the row and at the divider.
* **The focus ring is the pane's edge**: on the focused pane the 1 dip
  hairline is **replaced** by a 2 dip accent stroke on the same edge —
  not a ring drawn inside or outside it — so there is no hairline
  between ring and pane (Jixiang spotted that gap), nothing spills into
  the gutter or over the window border, and the ring follows the pane's
  corners.
* **The row becomes a sheet of its own while split**: the bottom
  hairline goes and the gutter shows the tint under it; its corners
  round to 8 at the bottom (top-left stays 8 against the sidebar,
  top-right square at the window edge; in the horizontal layout both
  bottom corners round under the strip). Unsplitting restores the
  unsplit pane.
* **Sidebar**: the split is one row — favicon + title, a 1 × 16
  hairline, favicon + title, each half ellipsising on its own (the
  reference's "New Tab | 选择标签页" row); the close box on the active
  row closes both panes. Right-click: Convert to side-by-side / stacked,
  Unsplit, plus the tab menu.
* **Framework**: `phi_split_view_style.h` pushes radius 8, outer inset
  4, divider 4, focus ring 2 px in the accent (the numbers it already
  has, restyled); the hover chip is the client's, a small layered popup
  over the unfocused pane's corner; the address row reads the focused
  pane through the existing active-pane notification. The replace-drop
  cards trace the rects unchanged.

### 4.7 The Kiosk window (the "Kiosk window" artboard)

The Mac's Kiosk (`UserInterface/Kiosk`): an ephemeral single-tab window
for a link opened outside the user's Spaces — an external link, a URL
rule routed to Kiosk, Ctrl+Alt-click, "Open Link in Kiosk" — with one
toolbar band: profile chip, the domain in a capsule with the
extensions / copy accessories, "Open in <Space>" trailing. The Windows
port has it as a `PhiSecondaryWindow` with a 52 dip band
(`phi_kiosk_window.cc`).

Windows version: **the vertical layout's address row with nothing under
it but the page.**

* **Window**: 980 × 680 default, remembered per profile; DWM corners;
  **neutral ground** `#F3F3F3` / `#202020` (a Kiosk belongs to no Space,
  so it wears no tint); the page flush below the row with a 1 px stroke,
  square corners (no sidebar to round against).
* **The row** (44, the §4.2 row minus the sidebar pair and Forward):
  Back, Reload, the
  **read-only address** (lock + host in `label`, path in `dim`; click =
  copy the URL with a "Copied" tooltip — the Mac's copy accessory folded
  into the field), the pinned-extension icons and the Extensions button,
  the **profile chip** (26 dip pill on ink 4.5 %, the profile avatar 16 +
  name in `dim`; a menu on click lists the other profiles and reopens
  the Kiosk in the chosen one once the framework can — today the chip
  only names it), the **"Open in <Space>" split button** (28 high,
  standard button style, the Space's icon + name; click moves the page
  into that Space as a tab and closes the Kiosk; the chevron lists the
  other Spaces), the 1 × 16 divider, the caption buttons. Everything but
  the controls is `HTCAPTION`.
* No app menu, no Downloads, no sidebar toggle, no tab search: a Kiosk
  cannot grow a second tab (Ctrl+T and middle-clicks open in the last
  active Space's window, as today). Ctrl+W / Esc-in-fullscreen close it.
* Reader and Peek do not attach to a Kiosk; a link that would auto-peek
  simply navigates.
* The band's `CaptionClaimsClient` hook and the shared `PhiPopupChrome`
  frame stay; only the band's height (44) and contents change.

### 4.8 Drag and drop (the two "Drag & drop" artboards)

What exists today (read from `phi_sidebar_drag_session.cc`,
`phi_sidebar_drop.cc`, `phi_drag_image.cc`, `phi_split_drop.cc`): one
raw-capture drag session for tab rows, split rows, pinned cells,
bookmark rows and Space pips. A row lifts as an opaque snapshot of its
own pixels; lists and the pinned grid open a **gap** that siblings slide
into over 200 ms (no insertion line anywhere except a 2 dip accent bar
for pip reorder); the image **morphs** row⇄cell between bands and becomes
a 220 × 160 page card over the page or outside the window; empty bands
reserve a dashed drop zone; a collapsed folder or group takes the drop
whole after a 400 ms dwell; the page shows four dashed 160 × 460 /
300 × 120 "Add Left/Right/Top/Bottom Split" cards, or the two live panes
in replace mode; a release outside every window tears the tab off into a
new window placed under the drop point; a release over another Phi
window appends the tab there. Not today: group-header drag, Esc to
cancel, auto-scroll, modifier keys, the bookmarks bar as a source or
target, dropping on a Space pip, any drop from another app (no OLE
target), an insertion slot in a cross-window drop, and a pip that lifts
while reordering.

Windows version — one vocabulary, used everywhere:

* **Ghost**: the lifted row, cell, chip or card at **85 %** opacity with
  the flyout shadow (Explorer's translucent drag image), following the
  grip; it slides home over 250 ms on cancel or a no-op drop.
* **Gap**: wherever rows or cells can part — the tab list, group runs,
  the pinned grid, the bookmark outline, the horizontal strip — the gap
  IS the indicator (200 ms slide, unchanged). No line is drawn there.
* **Line**: a **2 dip accent bar** only where nothing can part: between
  Space pips and between bookmarks-bar items.
* **Ring**: a container that takes the drop whole — a collapsed folder,
  a group well, a Space pip, an empty-band zone, a snap preview — shows
  `accent_soft` fill with a 1 dip (2 dip on previews) accent ring, radius
  6. The dashed resting zones keep a 1 dip dashed `stroke_strong`.
  In the bookmark outline the ring also marks the **folder the drop
  lands in** for as long as that holds — at a gap among its children as
  much as on its collapsed row — and it covers the folder's **whole
  area**: header, children and the gap the drop opens among them, one
  wash with one ring, so "the end of this folder" never reads like
  "after this folder" (Jixiang, 2026-09-18).
* **Space at the end**: the gap IS the placeholder — nothing is drawn at
  rest and no space is reserved ahead of time (a "Drop here" well came
  and went the same day). The lower half of a folder's last child opens
  a gap at the folder's end, inside the washed area; an empty folder
  opens it right under its header (a collapsed one hover-expands after
  the 400 ms dwell, then does the same); the lower half of the last root
  row opens it at the outline's end, the tab list parting below. The
  space appears only where the drag is and closes as it leaves. A
  folder row has three zones (the tree-view rule): its **top third**
  drops before it among its siblings — which is how the slot ahead of
  the outline's first row opens — the rest goes in (an expanded folder's
  first child; a collapsed one's middle third takes it whole, its bottom
  third drops after it).
* **Badge**: Windows' drop-description pill by the cursor (22 dip,
  `flyout` ground, 1 dip stroke, 11 dip text): **Move · Move to
  <group> · Pin · Bookmark · Move to <Space> · Split left / right / top /
  bottom · Replace left / right · New window · Move to <window> · Open in
  new tab**, with a leading "+" glyph whenever the source is kept.
* **Snap tiles on the page** (Windows 11 Snap Layouts): while a draggable
  is over the page, a **five-tile** flyout appears at the page's top
  centre (56 × 40 tiles in a radius-8 flyout, 8 gap, a 1 dip divider
  before the fifth): Left, Right, Top, Bottom split, then **New
  window**. Hovering a split tile or the matching edge third highlights
  it and paints the **preview** — the target half of the page in
  `accent_soft` with a 2 dip accent ring, inset 8. **The centre of the
  page picks New window** (Jixiang, 2026-09-17 — Chrome's and Edge's
  rule: a tab dragged onto the page detaches): the New window tile
  lights, the ghost grows a 24 dip caption strip (favicon, title, the
  three glyphs) so it previews a window, and the badge reads "New
  window"; release places the window under the drop point with the
  cursor over its tab, exactly as the outside-every-window tear-off
  does. A **24 dip cancel band** along the sidebar edge and under the
  address row (hatched on the artboard, invisible in the product)
  treats a release as a cancel, so a drag that slips just past the
  sidebar never tears off. In a split, the two panes are the first
  tiles (Replace left / right, or top / bottom) and the centre rule
  applies to the divider gap only. The dashed cards and their 1.5×
  active scale go; the edge-third zones, the 12 px exit tolerance and
  the resolution rules (`CommitSplitDrop`) stay. Split rows detach both
  panes as one window; pinned cells and bookmarks open their page and
  leave the cell behind; folders refuse.
* **Mechanics added**: **Esc** cancels (ghost slides home; a tear-off is
  suppressed); **auto-scroll** in 24 dip bands at the list's top and
  bottom (4 dip per 16 ms frame, accelerating); **Ctrl** keeps the
  source where a drop would otherwise convert it (bookmark → tab keeps
  the bookmark, tab → bookmark keeps the tab; the badge gains "+");
  **Shift** on a page drop forces stacked instead of side-by-side.
* **New cases**: (1) **whole-group drag** — the header lifts the group as
  one slice (ghost = header with its count badge and first member),
  reorders as a slice, drops into another window, tears off, or drops
  into a bookmark folder as a folder; (2) **drop on a Space pip** moves
  the tab (or slice) to that Space — ring on the pip, badge "Move to
  <Space>"; the strip does not scroll; (3) **bookmarks bar** as source
  and target — items drag to the page, to another window, out to the
  sidebar; drops between items show the line; (4) **cross-window slot**:
  the target window's list opens a gap at the pointer and the drop lands
  there, not at the end; (5) **from another app** through an OLE
  `IDropTarget` on the shell: a URL or text onto the list opens a tab at
  the slot, onto the pinned band pins it, onto a folder bookmarks it, a
  file opens; the page keeps handing drops to Chromium; (6) the **pip
  reorder** lifts the pip at 50 % and slides the others; (7) a drag
  hovering the **hidden sidebar's left edge** reveals the floating panel
  and the panel accepts the drop (§4.4). The bookmark-bar quirk (a
  release over the bar resolving as a tab move) goes with (3).
* Unchanged: the drag threshold (`SM_CXDRAG`), the morph rule and its
  cell width cap, the drop-zone reservation table, the 400 ms dwell,
  the landing glide on the strip (180 ms), the card's live thumbnail.
* Implementation: the badge is a second layered window owned by the
  drag session; snap tiles reuse `phi_split_drop.cc`'s overlay with the
  tile/preview painter; `PhiDragKindForKey` gains `kGroup`; OLE via
  `RegisterDragDrop` on the shell with a `PhiDropTarget` that feeds the
  same `SidebarPlace` resolvers; `sv-drag.mjs` and the dropzone/cell/row
  probes re-aim once, and new probes cover group drag, pip drop and the
  OLE path.

### 4.9 Bookmarks bar, fullscreen, incognito

* Bookmarks bar in the vertical layout stays off the page (the outline is
  in the sidebar); in horizontal it is row 3.
* Content fullscreen: unchanged (every chrome rect empty).
* Incognito: the tint is Fluent's dark chrome (`#202020` ground, `#2B2B2B`
  card) regardless of appearance, with the incognito glyph in the
  toolbar's leading slot; the current `MenuIncognito` guards carry over.

---

## 5. Spaces

### 5.1 The pip strip (vertical layout)

28 dip pips, radius 6, 4 apart, leading the sidebar's 44 dip header row
at x 8 (the tab-search and toggle buttons trail); the active pip is a
`card` tile with a 1 px stroke; hover is ink 4.5 %; agent pips keep the
1 × 14 rule before the first one and the 7 dip status badge (running
green, idle grey, held orange, error red) with a ring in the tint. The
"+" pip trails and fades in over 150 ms on row hover as today; overflow
shows the three-dot pip. Wheel scrolls the row; Ctrl+1…9 unchanged.

### 5.2 The Space chip (horizontal layout)

Replaces the lone compact pip: a 28 dip chip, radius 6, `card` fill with
a 1 px stroke, Space icon (16) + name (12 / 600, max 120 dip) + chevron.
Click opens the **switcher flyout** (§5.3); right-click opens the Space
context menu (§5.4). Hidden with a single Space, exactly as the pip is.
This is Edge's Workspaces switcher shape, so Windows users read it at
once.

### 5.3 The switcher flyout

A 300 dip flyout (radius 8, `flyout` ground): 40 dip rows with a 16 check
column (the active Space), the Space icon (20), name (13) over a hint
line (11, dim) "profile · N tabs", and the Ctrl+n keycap trailing; agent
Spaces after a separator with the status dot in their hint line; a final
"+ New Space…" row. Replaces the HMENU popup the compact pip and the
sidebar overflow open today (`phi_shell_window.cc:4740`); the hover card's
`ResolveHoverCard` already computes profile and shortcut, the tab count
comes from the sidebar tree.

### 5.4 The Space context menu

Right-click on a pip or the chip: Rename Space… · Change icon… · Edit
theme… · Change profile ▸ · ─ · Delete Space… — the Spaces submenu's
top block, so the Spaces ▸ menu is the keyboard path and the pip is the
mouse path.

### 5.5 Create a Space (the "Create a Space" artboard)

One form, three hosts — the existing `PhiCreateSpacePanel` with its
`PhiCreateSpaceFormHost` seam already separates the two:

* **Docked** (vertical layout, sidebar shown): the strip's "+" mounts
  the form over the sidebar below the header row, as today; the header
  shows the dashed placeholder pip with the chosen icon and the whole
  chrome previews the chosen colour live. Width = the sidebar's, 8
  inset.
* **Flyout** (horizontal layout, or the sidebar hidden): a 300 dip
  flyout drops from the Space chip (or from the switcher's "New Space…"
  row), radius 8, `flyout` ground, 14 × 16 padding. No live chrome
  preview — the swatch row is the preview. Replaces the standalone
  `PhiCreateSpaceWindow` for these cases.
* **Dialog** (Settings › Spaces "+"): the §6 frame — 448 wide, 20 / 600
  title, the form in the body, Create and Cancel equal-width in the
  footer, centred on the Settings window.

The form, top to bottom (labels 12 dip `dim`, 14 dip between groups):
**Name** TextBox (32, Fluent, focused on mount; Enter = Create); **Profile**
ComboBox (32, the profile glyph + name; rows = profiles + "New profile…",
which opens the §6 text prompt and selects the result, as today);
**Icon** segmented control Icon | Emoji (28 high, the selected segment in
`accent_soft`) over a 7-column grid of 28 dip cells (6 columns in the
flyout), the chosen cell in `accent_soft` with the accent glyph, the
emoji tab a scrolling grid of the same cells; **Color** the eight theme
swatches (22, 8 apart, the chosen one ringed in the accent) and
**Saturation** a 20 dip slider; then **Create · Cancel** (32, Windows
order, Create accent and default, Cancel = Esc). A random icon and a
colour distinct from the source Space are preselected, as today.

### 5.6 The hover flyout (the "hover tip")

Replaces `SpaceHoverCard` (the Mac capsule with a name pill, a dot, the
profile, a dot, keycaps — one line, radius 12). The new card is a Fluent
tooltip with two lines:

```
[icon]  Home                              Ctrl+2
        Personal profile · 4 tabs
```

and for an agent Space:

```
[bot]   Claude — Research                 Ctrl+4
        ● Running · Default profile · 3 tabs
```

* 300 ms dwell (unchanged), instant swap pip-to-pip while warm, dismissed
  on leave, on click (suppressed until the pointer leaves the pip), on drag.
* Anchored centred under the pip, **4 dip** below the header row, clamped
  8 from the window edges; radius 8, `flyout` ground, 1 px stroke, flyout shadow,
  padding 8 × 10, min width 200, max 280 (name ellipsised first).
* Line 1: icon 16 in its Space colour tile, name 12 / 600 in `label`,
  keycap trailing (20 high, radius 4, 1 px stroke, 10 dip text).
* Line 2: 11 dip `dim`, indented under the name; the agent variant leads
  with the 7 dip status dot and the status word.
* Same popup window class (`PhiSpaceHoverCard`, layered, `HTTRANSPARENT`);
  only `MeasureHoverCard` / `PaintHoverCard` change. The tab count is a
  new field on `PhiSpaceChip` filled by the strip's rebuild from the tree.

The tab preview card (`PhiTabPreview`) is restyled to match: radius 8,
`flyout` ground, 12 padding, title 12 / 600 over URL 11 dim, snapshot
corner radius 4; timings unchanged.

---

### 5.7 Peek View (the "Peek view" artboard)

What the Mac does (read from `UserInterface/Peek`): a peek is a live tab
kept out of the list, shown as a floating card over the page card only
(inset 4 % / 24, radius 12, heavy shadow, no header) with three chips in
a 44 pt gutter to its right (close, open as tab, open as split); it
opens from a cross-site link out of a bound pinned/bookmark tab
(auto-peek) or from Shift-click / the link menu; Esc, Cmd-W, a click on
the page around it, or a navigation on the opener closes it; the owning
row shows the **peeked page's favicon** in its trailing 24 pt slot
(hover → a minus), and a pinned cell gets a 21 pt circular badge on its
corner. The Windows port already mirrors the card and the gutter
(`phi_peek_panel.cc`) and paints the row badge as an accent plate with a
window glyph.

Windows version:

* **The sheet.** A `card` sheet over the page area of the owning pane:
  inset 40 horizontally (24 under 900 dip) and 24 vertically, radius 8,
  1 px stroke, the dialog shadow (§2.4); the page behind stays live and
  undimmed. No floating chip gutter — the sheet has a **36 dip header**
  on the `flyout` ground: the peeked page's favicon (16), its host in
  `label` and path in `dim` (12 dip, ellipsised), then three 28 dip
  buttons trailing: **Open as tab** (`OpenInNewWindow`), **Open in split
  view** (`Split`), **Close** (`ChromeClose`-style ✕ at 16). The header is
  the peek's drag/identity strip and shows where the page came from,
  which the Mac's bare card never did. Tooltips name the three actions
  and their chords.
* **Open**: unchanged — auto-peek from a bound row's cross-site link,
  Shift-click, the link context menu; never in the horizontal layout,
  incognito, agent windows or split openers. The appear animation stays
  the grow-from-the-click (260 ms) with the content fading in.
* **Close**: Esc, Ctrl+W (closes the peek, not the opener), a click on
  the page around the sheet, Back / Forward / a navigation on the
  opener, the opener leaving the list; a click in the sidebar or the
  address row hides it with its opener (it comes back with the tab).
* **Reader View** covers the page below the address row — the row and
  its controls stay live — with **no header** (Jixiang, 2026-09-20): the
  reading page's own HUD, the omnibox button, Ctrl+Alt+R and the row's
  indicator tile close it.

### 5.8 The two row indicators (the "Peek indicator · open tab indicator" artboard)

* **Peek / Reader indicator** — "another icon in the tab bar". While a
  surface is attached, the owning row's trailing 24 dip slot (where the
  ✕ close box lives) shows a **20 dip tile**: radius 5, `card` fill, 1 px
  `stroke_strong`, a 0 1 2 shadow, holding the **peeked page's favicon**
  (14) — or the reader glyph for Reader View. The row's own ✕ is
  suppressed while the tile is there. **Hover is scoped to the tile**
  (today the Windows badge flips on row hover — a divergence from the
  Mac, fixed here): hovering the tile swaps the favicon for a 12 dip ✕
  on an ink 4.5 % wash and shows the tooltip "Close Peek" / "Close
  Reader"; click closes the surface without activating the row.
  Bookmark rows: the same tile in the same slot. **Pinned cells**: an
  18 dip tile overhanging the cell's top-right corner by 2 (the Mac's
  21 pt circle, squared to Fluent), favicon 12 inside, the same hover
  swap; the press is swallowed so the cell never activates. The tile is
  shown even while the peek is hidden behind another active tab.
  Horizontal layout: none (Peek is sidebar-only, as on the Mac).
* **Open tab indicator** — "Show open tab indicators" (Settings ›
  Appearance › Tabs, default **off**, unchanged). The Mac draws a 2 pt
  ink dot; Windows draws **the taskbar's running mark**: an **8 × 2 dip
  pill**, radius 1, `label` ink at 60 %, centred under the favicon with
  a 3 dip gap — on pinned cells (both layouts' pinned sections) and on
  bookmark rows; never on the active row (open && !active, as today).
  Unloaded items draw the favicon at 30 % and the pill with it when
  "Dim unloaded tab icons" is on. No animation.
* Implementation: `DrawSurfaceBadge` becomes the tile painter with a
  favicon from the surface's tab (`PhiSurfacePanel` already exposes the
  surface tab's id; `TabFaviconBgraById` fetches it); `surface_box` hit
  and hover become tile-scoped (a new hover kind, appended to the
  nav-hover table); `DrawOpenDot` becomes `DrawOpenPill` with the §2.1
  metrics; `kOpenDot*` constants go.

### 5.9 Space icons and emoji (the "Space icons & emoji" artboard)

The shared catalogue contains 88 solid icons
(`client/phi_icon_masks.cc`, 48 px alpha masks rasterised from
`Assets.xcassets/icons`: academic-cap … view-grid-add, the letters A–Z
and the digits 0–9) plus the shared emoji catalogue
(`resources/emoji-catalog.json`, Unicode 17, nine groups, 1914 emoji),
stored as `phi:<id>` / `emoji:<id>` so a Space means the same on both
clients. The **stored ids do not change**; only how Windows draws them.

* **Artwork**: every `phi:` id renders the shared solid artwork from
  `client/phi_icon_masks.cc`, including the original heavy letters, digits
  and punctuation. Display codepoints remain stable lookup keys; they no
  longer substitute Segoe Fluent icons or system-font text. The shared draw
  path tints the masks and fits their ink to the existing Space icon slots.
  The picker draws the complete 20 dip artwork canvas, preserving the
  supplied design's proportions.
* **Colour**: solid icons are monochrome, with no coloured tile.
  An inactive pip draws its glyph in `label` ink on the transparent 28
  dip pip; the **active pip** is the `card` tile with the glyph in the
  Space's accent; hover is the ink wash. The Space colour appears where
  the Space is the subject — the active pip, the chip, the switcher rows,
  the hover flyout, the create form's placeholder pip — never as a
  filled square. Agent pips keep the status badge.
* **Emoji**: drawn by DirectWrite from **Segoe UI Emoji** (Windows 11's
  Fluent Emoji, colour) at 14 in the pip and chip, 18 in the picker,
  20 in Settings rows; no tile, no tint. The GDI fallback path and the
  first-letter fallback stay for missing fonts.
* **Picker**: the "Change icon…" flyout is 262 dip wide, with 16 dip
  padding, Icon | Emoji tabs and search. The icon grid has eight columns
  of 20 dip cells with 10 dip gaps, in the supplied order: 48 pictograms,
  digits 1–9 and 0, letters A–Z, then ? ! ~ and backtick. All 11 rows are
  visible. Selected cells retain the accent fill and inset ring. Emoji
  retain their category strip, group headings and name search. The create
  form and Settings use the same catalog artwork in their existing layouts.
* Implementation: `PhiIconCatalogAll` preserves all 88 stored ids and
  display keys. `PhiDrawCatalogArtwork` draws the padded artwork canvas;
  `PhiDrawCatalogIcon` fits its ink to a Space icon slot. Both resolve the
  same compiled masks, with no dependency on an installed icon font.

### 5.10 Telling an agent Space from a user Space (the "Agent Space vs user Space" artboard)

Today the only cues are the 1 × 14 rule before the first agent pip, an
8 dip status badge on it, the switcher's separator, and — while the
agent holds input — the watch-mode overlay (`phi_agent_overlay.cc`: a
fog over the page that swallows clicks, a pill with the agent name,
status and Take control / Hand back / Finish, and the agent's virtual
cursor). Everything else in the window looks exactly like a user Space,
so a glance at a window cannot tell whose it is. Seven cues, strongest
first, all Windows-flavoured (Quick Assist's "controlled by" frame,
Teams' share ring, Chrome's automation bar):

1. **Graphite ground.** An agent Space never wears a user theme: its
   chrome tint is a fixed neutral `#E8EAEE` (dark `#26282C`) and its
   accent is the agent's brand colour. The user's Spaces stay colourful;
   the machine's stay grey. This is the cue that works from across the
   room and in the taskbar thumbnail.
2. **Agent pips after the divider** — in every Space, so the strip
   always separates people from machines: after the user pips and a
   1 × 16 divider, one plain 28 dip pip per agent (Jixiang, 2026-09-17:
   one icon, no well or label — the pod was too big) showing the agent's
   mark with its status badge; the active one is a `card` tile with the
   mark in the brand colour.
3. **The agent chip in the address row** — always, not only while the
   agent drives. In a **user Space** it takes its **compact form**
   whenever any agent has a live task elsewhere: the mark and the status
   dot only (28 × ~40 dip, brand-soft), tooltip "Claude is running in
   Research — click to watch", click = switch to that Space; hidden when
   no agent is working. Every vertical board on the canvas shows this
   ambient form after the nav trio. In the **agent's own Space** it is
   the full chip: after the nav trio, a 28 dip chip on brand-soft
   (brand 12 %) with the agent's mark, its name, a hairline, a status
   dot + word ("Running", "Waiting for you", "Idle", "Done", "Error"), a
   chevron. Click = the agent menu (Take control / Hand back, Finish,
   Autoview, Show transcript, Open in Space…). The omnibox reads in `dim`
   with an eye glyph in its trailing slot while the agent holds the tab.
4. **The held-input treatment** — only while the agent owns input: a
   **2 dip brand ring** inside the page pane, a brand fog at 5 % that
   swallows clicks (today's barrier, lighter), and the **control
   capsule in the page's bottom-right corner**, inset 24 from the right
   (clear of the scrollbar) and 16 from the bottom, so it never covers
   what the user wants to read (Jixiang, 2026-09-17: the centred pill
   did). **Collapsed by default**: 36 high, radius 18, `flyout` ground,
   flyout shadow — the mark, the status dot, the agent's name; no chevron
   (the whole capsule is the target, and a down chevron in a bottom
   corner reads as a menu or "hide"); the current step is its tooltip. **Expanded** (hover 300 ms or click):
   the same corner grows to the 44 dip pill — mark, "Claude is working"
   over the current step, a divider, a brand-filled **Take control**
   button, ⋯, and a collapse chevron; it collapses again 6 s after the
   pointer leaves. It expands on its own when the agent is **Waiting
   for you** and stays open until answered. The ring pulses 60 → 100 %
   over 1.2 s while Running, stays steady while Waiting, fades out over
   400 ms on Done.
5. **The named cursor**: the agent's virtual cursor is the brand-coloured
   arrow with a name tag (Teams / Figma style), so its movements are
   never mistaken for the user's.
6. **Sidebar labels and rows**: the tab section reads "Claude's tabs"
   with the mark; the row the agent is driving shows a 20 dip brand-soft
   tile with the mark in its trailing slot (the §5.8 tile family).
7. **Window title** "Claude — Research" and the taskbar thumbnail's
   graphite chrome; the hover flyout and switcher rows keep their status
   line (§5.3, §5.5).

Status colours (badge, chip dot, ring): Running green `#1E9E4A`,
Waiting orange `#F0A020`, Idle grey `#8E8E93`, Done green (then the ring
goes), Error red `#E0463A` — the five states the strip already knows.

Implementation: a `PhiPalette::ForAgent(brand)` variant selected by
`PhiSpacesStrip` whenever the active Space is an agent's; the pod is a
strip layout flag (`agent_first`, already known) plus a well rect; the
chip is a `ShellLayout` rect fed by the agent host's task snapshot; the
overlay restyles the fog, ring and pill in `phi_agent_overlay.cc` and
adds the name tag to the cursor; the section label and row tile come
from the sidebar tree's agent flag.

## 6. Dialogs (the "popup confirm")

`PhiAlert` / `PhiNotice` / `PhiConfirm` / `PhiPromptText` /
`PhiPromptShortcut` / `PhiPromptChoose*` all render through one
**ContentDialog** layout:

* Window: 448 wide (320 for a one-line notice, up to 548 for a list
  chooser), owned, centred on the owner, radius 8 (DWM), 1 px stroke,
  dialog shadow. **No caption band, no close button, no Phi mark** — the
  band, the mark and the `#EAEAEA` ground go. Esc = cancel, Enter = the
  default button. The owner is disabled for the duration (unchanged
  `PhiModalScope`) and, as a follow-up, dimmed by a layered scrim window
  over its client area (WinUI's smoke layer; optional, §10 P3).
* Body: padding 24; title 20 / 600 in `ink`; 12 gap; message 14 / 400,
  20 line height, wrapped; an accessory (text box 32 high, Fluent TextBox
  with the 2 px accent bottom edge on focus; shortcut recorder 32; list
  rows 32 in a `card` well with a 1 px stroke, radius 4) 16 below the
  message.
* Footer: `#F3F3F3` ground with a 1 px top separator, padding 24, buttons
  **equal width** filling the row with 8 gaps, 32 high, radius 4, 14 dip
  labels. Roles: **accent** (accent fill, white text, 1 px 12 % stroke),
  **standard** (`#FBFBFB`, 1 px stroke), **danger** (`#C42B1C` fill,
  white text). A notice's single button spans the footer.
* **Order: affirmative first, Cancel last** (Windows' convention — WinUI
  puts PrimaryButton left and CloseButton right; Edge does the same). This
  flips the Mac's `[Cancel] [Delete]`. `PhiAlertAction.leading` is
  retired; `BuildActions` lays them out left-to-right in declaration
  order.
* Destructive confirms keep the Mac rule that matters: **no default
  button** (Enter is inert, Esc cancels); the danger button is leftmost and
  the title is a question naming the object ("Delete this Space?", the
  message repeats the Space's name in bold).
* Titles are rewritten to Windows voice: "Delete this Space?", "Delete
  this profile?", "Sign out of Phi?", "Rename Space", "Replace the installed
  skill?", "Import from Google Chrome?", "Updater unavailable". Bodies stay.

`PhiPopupChrome` survives for the windows that ARE windows — Settings,
Feedback, URL rules, Create Space, Onboarding, the transcript panel — with
one change: the Phi mark option defaults off and the title sits at the
leading edge (Windows), not centred (Mac). Its palette becomes §2.3's.

---

## 7. Settings

Windows Settings, not a Mac preferences toolbar.

* **Window**: resizable, default 1120 × 760, minimum 760 × 560, remembered
  size; `PhiPopupChrome` caption (32) with the Phi icon and "Settings" at
  the leading edge, min / max / close. Ground `#F3F3F3` / `#202020`.
* **Navigation rail** (280, collapses to a 48 dip icon rail under 900
  window width with tooltips; Windows Settings' breakpoint): the account
  block at the top (44 avatar, name, email → the You page), a 32 "Find a
  setting" search box (filters rows across pages by label; the Shortcuts
  page's own search stays), then 36 dip items with a 16 icon, 14 label,
  a 3 × 16 accent indicator on the selected item, ink 4.5 % hover.
* **Page**: 28 / 600 title, 20 gap, groups with a 14 / 600 header and a
  stack of **settings cards**: full width, `card` fill, 1 px stroke,
  radius 4 on the group's outer corners (the cards inside a group share
  edges, WinUI's SettingsCard stack), min height 68, padding 12 × 16,
  20 dip icon, 14 title over a 12 dim description, the control at the
  trailing edge: ToggleSwitch 40 × 20, ComboBox 32 high min 160,
  Button 32, Slider 200, swatch row 22 dots 8 apart, chevron for a row
  that opens a page. Content scrolls with a Fluent scrollbar (2 dip
  at rest, 6 on hover) that takes wheel AND thumb drag (today only the
  wheel works).
* **Pages** (the regrouping; every pref keeps its key and its verb):

| Page | Rows (from today's pane) |
|---|---|
| **You** | avatar / name / email / sign in-out (Account); Default browser card (Account) |
| **General** | Display language; New tab behaviour (image radio → ComboBox "New Tab Page / Omnibox"); Restore last session (Advanced); Share anonymous usage analytics (General); Search engine, Download location (Profile detail, for the single-profile case — else a link to Profiles) |
| **Appearance** | Layout (two image radio cards: Horizontal tabs / Vertical tabs); Color appearance ComboBox; Space color swatches + Saturation (per active Space, locked with a link when there are several, as today); Tabs group: tab preview card, open tab indicators, dim unloaded icons, always show full URL (Advanced) |
| **Spaces** | one **expander card** per Space (icon, name, profile; expand → icon/emoji picker, colour, saturation, rename, change profile, delete) instead of the master-detail band; "+ New Space" button in the group header; Pinned tab scope ComboBox; URL rules ▸ |
| **Profiles** | one expander per profile (search engine, download location, Privacy & security ▸, Notifications ▸, Clear browsing data ▸, rename, delete); "+ New profile" |
| **Navigation** | Kiosk group (two toggles + hint); Peek group (two toggles); URL rules ▸ |
| **Phi & AI** | unchanged rows, as cards |
| **Shortcuts** | "Restore all" button in the header; the search box; one card per group with keycap controls (the recorder opens as a §6 dialog) |
| **Advanced** | Shorten links, Auto picture-in-picture, Developer mode, Additional browser settings ▸ |
| **Developer** | unchanged rows, as cards (gated on Developer mode) |
| **About** (new) | version card with "Check for update" button; What's new ▸; Documentation ▸; Report an issue ▸; licences ▸ — the Help menu's home |

* Opening: the app menu's Settings, Ctrl+, and the verbs; the Spaces
  page's expander opens pre-expanded for `RequestSpaceIconPicker`.

### 7.1 The Spaces page (the "Settings · Spaces" artboard)

* Title "Spaces"; group **Your Spaces** with a "+ New Space" standard
  button in the group header (opens the §5.5 dialog). One **expander
  card** per Space in strip order: the Space's glyph or emoji (20, in
  the Space colour), the name (14), a hint line "profile · N tabs ·
  Ctrl+n" (12 dim), a chevron trailing. Click the row to expand it in
  place (one open at a time); expanded, the card continues below the row
  with a 52 dip label column: **Name** (TextBox 260), **Profile**
  (ComboBox), **Icon** (the current glyph in a 32 accent-soft cell + a
  "Change…" button that opens the §5.9 picker anchored to it), **Color**
  (the eight swatches), **Saturation** (slider 200), **Shortcut** (read-
  only keycaps "Ctrl n — follows the strip order"), and a right-aligned
  **Delete Space** button (danger text on a standard button; disabled on
  the last Space; opens the §6 confirm). Drag the row handle to reorder
  the strip.
* Group **Pinned tabs**: one card, "Pinned tab scope" ComboBox
  (Space / Profile / App) with the scope description.
* Group **Rules**: one card, "URL rules ▸" → the rules editor window.

### 7.2 The Profiles page (the "Settings · Profiles" artboard)

* Title "Profiles"; group **Your profiles** with "+ New profile" (the
  §6 text prompt). One expander per profile: a 20 dip avatar circle in
  the profile colour, the name, "Used by Work, Research · 2 Spaces", a
  **Default** badge on the default profile, the chevron. Expanded:
  **Name**, **Search engine** (ComboBox), **Download location**
  (ComboBox + "Choose…"), **Used by** (read-only), then three chevron
  cards that open the browser pages — Privacy & security
  (chrome://settings/privacy), Notifications, Clear browsing data — and
  a right-aligned **Delete profile** (disabled on Default and on a
  profile a Space uses, with the reason as its tooltip; else the §6
  confirm).
* A footer line explains what a profile is and that deleting one removes
  everything in it.

---

## 8. What is dropped

* The menu bar, its title reveal, the Alt-reveal state machine, the
  comfortable overflow clone, the Phi logo button and the Phi menu.
* The Mac card geometry: radius 14, the corner caps, the outer hairline,
  the 8 dip gutters, the 4 dip band gap, the 48-band ground gradient
  (the vertical pane keeps rounded LEFT corners at Fluent's radius 8 and
  gains the address row).
* The Mac alert layout: Phi mark, caption band on alerts, `[Cancel]
  [Primary]` order, `#EAEAEA` ground.
* The Settings toolbar of panes, the fixed 680 × 561 window, the
  master-detail Spaces / Profiles bands, the image-card New-tab picker.
* The Mac capsule hover card.
* The Performance layout, the sidebar address bar and the field
  reparenting into the floating panel (§4.3).
* Window ▸ Name window (never worked), Edit ▸ Search the web (never
  worked) — both had no Windows surface; they are not carried into the
  app menu.

---

## 9. What is kept on purpose

The sidebar tree and every drag; pinned cells and bookmarks in the
sidebar; group chips in the strip; the Space slide and drag animations;
Peek / Reader surfaces and their row indicators; the
Feedback pill; agent Spaces (pips, badges, hand-over, autoview); the
`PhiPalette` themes and per-Space theming; the layout-mode wire names; the
verb registry and the box rigs' command ids (`kCmd*` numbers are
APPEND-ONLY and keep their values).

---

## 10. Implementation plan

Each phase leaves the box build usable and deletes what it replaces in
the same change (no parallel patterns). Host tests in `test/model` run
after every phase; box captures with PhiCap / PhiRm.

**P0 — Tokens and type.** `shell_metrics` gains the §2.1 values (old
constants removed as their last reader goes); `PhiShellWindow::EnsureFonts`
builds the §2.2 ramp (12 / 14 / 20 / 28); `PhiPalette` gains `flyout`,
`dialog_footer`, `stroke_strong`; the popup ground constants in
`phi_popup_chrome.h` move to the palette. Pure churn, one build.

**P1 — App menu, menu bar out.** New `client/phi_app_menu.{h,cc}` builds
§3.1 from the existing `kCmd*` ids and reuses `PhiMenuBar`'s dynamic
builders (Spaces, Bookmarks, History rows, extension rows) — those move
into it and `phi_menu_bar.{h,cc}` is deleted; the `PhiMenuBarShell`
interface is renamed `PhiAppMenuShell` unchanged. `PhiCaptionBand` loses
`Install / Rebuild / SetRevealed / ToggleAltReveal / the title rects`; it
keeps the frame (`WM_NCCALCSIZE`, hit-testing, caption buttons). The
accelerator table routes Alt-solo / F10 / Alt+F / Alt+E to
`ShowAppMenu(anchor)`. Owner-drawn items come from `phi_shell_menus.cc`'s
existing bitmap path plus a right-aligned chord column. `ListJson` /
`InvokePath` walk the new tree; `test/suite-menu.mjs` gets the new paths.
The ⋯ button is added to the vertical layout's band (ordinal 5 already
exists for comfortable). Ship this before P2: it is the biggest visible
change and independent of geometry.

**P2 — The two layouts and the floating sidebar.** `ComputeShellLayout`
per §4: comfortable = strip row 40 + toolbar 40 (+ bookmarks 28) + flush
page; balanced = sidebar with a 44 header row beside a flush page pane
(left corners radius 8) whose 44 address row ends in the caption
buttons;
the performance branch, the sidebar address bar and the field hosting are
deleted and `performance` maps to `balanced` at load (§4.3). `ShellLayout`
gains `tool_cluster` rects (`app_menu`, `star`), `drag_gutter`,
`chip`, `pin`, `pane`; loses `phi_menu`, `band_limit`, `top_caps`,
`side_address_bar`, `title_bar` (the caption rects move into the card
row in balanced and the strip row in comfortable). The floating panel (§4.4) is the existing
`floating_layout_` path with the new frame (inset, radius, shadow via a
layered window), the pin button, the dwell timer, the leave grace and the
shell-follow; `agentSpace.layout.set {sidebar}` and `sidebarFloating`. Paint: `PaintChrome` draws the
flat tint, the white toolbar / active tab, the sheet corner
(`kContentRadius` 8, top-left only; `PositionCornerCaps` becomes one cap);
the omnibox at 32 / r8; 32 dip buttons; 12 dip rows. `phi_split_view_style`
numbers follow. Host test `shell_layout_test.cc` rewritten for the new
rects; PhiCap band captures re-baselined. The nav-hover ordinal table is
appended (star, app_menu, chip).

**P3 — Spaces.** Pip metrics (28 / r6 / 36 row); the Space chip and the
switcher flyout (a new owner-drawn popup, `phi_space_switcher.{h,cc}`,
sharing the flyout painter with P1's menu); the two-line hover flyout
(`MeasureHoverCard` / `PaintHoverCard`, `PhiSpaceChip::tab_count`); the tab
preview restyle. Probe: `test/probes/space-flyout.mjs` on the box with
PhiSkillCap's `move` + `shot`.

**P4 — Dialogs.** `phi_prompt.cc` `Build()` / `BuildActions()` to §6;
`PhiPopupChrome` gains `caption = kNone` and a leading title; the
`#EAEAEA` palette goes; call sites get the new titles (strings.json keys
added, old ones removed); `leading` retired from `PhiAlertAction`. The
scrim (a `WS_EX_LAYERED` 30 % ink window over the owner's client rect,
created by `PhiModalScope`) is a separate, optional change. Box check with
PhiCap2 (command ids 1055 Delete Space, 1052 Rename Space) + PhiRm.

**P5 — Settings.** The largest piece (`phi_settings_window.cc` is 4.1 k
lines): a `PhiSettingsNav` rail, `PhiSettingsCard` rows replacing
`AddRow` / `AddToggleRow` / `kListRow` (the paint helpers for toggles,
combos, sliders, swatches and keycaps are reused with the §7 sizes), a
resizable frame with a real scrollbar, the page regrouping of §7, the
About page, the expander cards for Spaces / Profiles. `set-shot.mjs` and
`suite-settings.mjs` follow the section ids (`appearance` and `about` are
new; `account` → `you`).

**P6 — Secondary windows and polish.** Feedback, URL rules, Create Space,
Onboarding on the new ground and controls; the incognito chrome; the Fluent
icon font swap; dark-mode pass over every surface; the sidebar-collapsed
panel's shadow.

Estimated size: P1 ≈ 2 k lines net negative, P2 ≈ 1.5 k, P3 ≈ 800,
P4 ≈ 400, P5 ≈ 3 k, P6 ≈ 1 k. P1 and P4 can start today and touch no
shared file with P2; P5 is independent of all of them.

The existing memory note on the floating sidebar lists the field
reparenting and the panel-follow as its gotchas; both are resolved by
this phase (the first by deletion).

Risks. (1) Owner-drawn HMENUs at 34 dip rows with a right-aligned chord
column: `phi_shell_menus.cc` already owner-draws bitmaps, but
`MFT_OWNERDRAW` for every item means measuring every label — cache per
DPI as the caption band does. (2) The zoom row in an HMENU: a custom
popup (like the switcher flyout) if the owner-drawn item cannot host three
hit targets. (3) Dropping the caption band in the vertical layouts moves
the `HTTOP` resize edge onto the toolbar's first `border_y` pixels —
`OnNcHitTest` already handles that for the band. (4) `agentSpace.
window.layout` rows and every box rig aim at rects; re-baseline once after
P2, not per phase. (5) The Mac client keeps the old geometry; nothing
here changes shared preferences or the framework API — no fork change is
needed except `phi_split_view_style.h`.

---

## 11. Decisions needed

* ~~D1 — Keep the Performance layout?~~ Decided 2026-09-17 by Jixiang:
  removed entirely (§4.3).
* **D2 — Button order in dialogs.** Windows convention (affirmative left,
  Cancel right) as designed, or keep the Mac order for cross-platform
  muscle memory? Recommendation: Windows order — the dialogs are the one
  surface where Windows users have the strongest habit.
* ~~D3 — Split-view button in the tool cluster.~~ Decided 2026-09-17 by
  Jixiang: no split button; the drag and the context menus are enough.
* **D4 — Floating panel trigger.** Left-edge hover only (as designed), or
  also a hover on the toolbar's toggle button? Edge auto-expands on
  hovering the collapsed pane; Phi has no collapsed pane, so the edge is
  the only hover target. Recommendation: edge hover + the toggle's
  right-click menu ("Show sidebar / Auto-hide sidebar").
* **D5 — Mica.** Real Mica (`DWMWA_SYSTEMBACKDROP_TYPE`) needs the ground
  painted transparent, which the GDI chrome cannot do; the flat Space tint
  is the stand-in. Not planned.

## 12. The installer and uninstaller (the two "Installer" artboards, added 2026-09-18)

`installer/phi-installer.nsi` is a stock MUI2 wizard: the black
onboarding sidebar on the Welcome page, a header bitmap, five pages
(Welcome, Program folder, Browsing data, progress, Finish), four
`MessageBox` prompts and a Yes/No box for the browsing data on uninstall.
It is the only Phi surface that still looks like 2005. The redesign makes
it the same kind of window as Settings: one 600 × 420 dip window on the
Windows Settings ground (`#F3F3F3` / `#202020`, §2.3), Segoe UI Variable,
the brand mark (§2.2b), Fluent controls, no wizard chrome at all.

### 12.1 Anatomy (every screen)

* **Caption** 32 dip: the mark at 16 with a 3 dip radius, the title
  "Phi Browser Setup" (the uninstaller: "Uninstall Phi Browser"), Minimize
  and Close. No Back / Next row, no branding text, no header bitmap.
* **Header**: the mark at 48 (10 dip radius) + the product name at 28 / 600
  Display + a 12 dip dim line "Version 0.1.0 · x64 · Fyde Innovations".
  The version line is also where an update shows "Update 0.1.0 → 0.2.0".
* **Body** on a 32 dip left margin: one 14 dip lead sentence, then ONE
  expander card (§7's card: white / `#2B2B2B`, 1 dip stroke, 4 dip radius,
  56 dip header row with a folder glyph, label, hint and chevron).
* **Footer** 76 dip with a 1 dip separator: a 12 dip link or status on the
  left, the buttons right-aligned in ContentDialog order (primary first,
  then Cancel), 32 dip high, 4 dip radius, min 96 dip wide.

### 12.2 The screens (Installer · install flow)

1. **Setup opens** — the whole install is one click: header, lead
   ("Setup puts the program in Program Files and keeps your browsing data
   in a folder of its own"), the expander COLLAPSED with the hint "Program
   in Program Files · browsing data in this user's app data", footer
   "License terms" · [Install] [Cancel]. The two folder pages of the wizard
   are gone; nothing is asked of someone who wants the default.
2. **Expander open** — three rows: Program folder (field + Browse…),
   Browsing data (field + Browse…, hint "Spaces, history, bookmarks,
   settings and passwords. An update never touches this folder."), Desktop
   shortcut (toggle, on; "A Start menu entry is always added."). The
   write-probe checks of `DirLeave` / `DataDirLeave` run when Install is
   pressed; a failure turns the row's hint red (§6's danger ink) instead of
   a MessageBox.
3. **Installing** — the header stays; "Installing…" 14 / 600 with the
   percentage on the right, a 4 dip Fluent progress bar (accent fill on a
   12 % ink track), the current file in 12 dip dim, and a hint about the
   rollback copy. Footer: the destination on the left, [Cancel] alone.
4. **Done** — a card with a 40 dip green check circle, "Phi Browser is
   installed", a key/value list (Program, Browsing data, Shortcuts).
   Footer: "Updates arrive on their own; Settings › About checks now." ·
   [Launch Phi Browser] [Close]. Launch goes through the shell as today
   (the browser runs as the user, not the elevated installer).

### 12.3 The states (Installer · update · running · architecture · uninstall)

5. **Update** (WinSparkle's payload runs this same installer): the version
   line reads "Update 0.1.0 → 0.2.0", the lead promises that Spaces,
   settings and data stay put and the previous version is kept for
   rollback, the expander hint says "Recorded from the previous install ·
   C:\Program Files\Phi Browser", the footer link is "What's new in
   0.2.0", the primary is [Update].
6. **Phi still running**: a Fluent InfoBar (warning tone `#FFF4CE` /
   `#433519`, triangle glyph) above the expander — "Phi Browser is still
   running. Close it to continue. Setup checks again on its own for a
   minute, then asks." with a small [Try again]; the primary is DISABLED
   (6 % ink fill, 36 % ink text) and the footer status reads "Waiting for
   Phi Browser to close…". This replaces the Retry/Cancel MessageBox; the
   60 s poll of `check_running` stays.
7. **Wrong architecture** (the arm64 installer on a non-ARM PC): the body
   is the notice itself — a 48 dip blocked-circle glyph on the control
   fill, a 20 / 600 title "This installer is for Windows on ARM", the file
   name as the sub line, the lead naming the standard installer, footer
   "Get the standard installer" · [Close]. The mirror case (x64 on an ARM
   PC) is an INFORMATIONAL InfoBar on screen 1 with a "Get the ARM64
   build" link; Install stays enabled, as today's Yes/No box allows.
8. **Uninstall**: header "Remove Phi Browser" / "Version 0.1.0 · C:\Program
   Files\Phi Browser", lead "The program, its shortcuts and the Apps &
   Features entry will be removed. Rollback copies go with them.", one
   card with the checkbox "Also delete my browsing data" (OFF by default;
   hint names the folder and says leaving it off keeps the data for a
   later install), footer [Remove] in danger red · [Cancel]. The Yes/No
   box after the files are gone is replaced by this checkbox before.

### 12.4 Decisions (as implemented 2026-09-18)

* **D-I1 NSIS stays; the UI is nsDialogs plus one helper DLL.** Every
  screen is a `Page custom` (the progress screens are NSIS's own instfiles
  pages, redecorated); MUI2 is gone with the header, welcome bitmaps,
  branding text and the Back button. `installer/ui/phi_setup_ui.c` is the
  x86-unicode plugin `PhiSetupUI` (built by `build-plugin.sh` with the
  Chromium checkout's clang-cl + lld-link, the DLL committed): it sizes the
  window, owner-draws the buttons, expander card, chevron, toggle, checkbox,
  progress bar, InfoBar and the mark, sets Segoe UI Variable on the stock
  labels and edits, and pins the caption. Controls that need a click
  callback are created by nsDialogs and only painted by the plugin.
* **D-I2 Dark mode follows the Windows app mode**: `AppsUseLightTheme`
  read by the plugin, `DWMWA_USE_IMMERSIVE_DARK_MODE` for the caption,
  `DarkMode_CFD` for the edits, the dark token block for everything else.
* **D-I3 One primary per screen**: Enter is Install / Update / Reinstall /
  Launch, Esc is Cancel / Close; Remove is the red button. A focus ring is
  drawn only after keyboard input. (A same-version run says "Reinstall".)
* **D-I4 Desktop shortcut is a toggle** (on by default); the Start menu
  entry is always written.
* **D-I5 No license link**: the repo ships no LICENSE file, so the footer
  stays empty on the Setup screen; the "What's new" link of the update
  state waits for a release-notes URL.
* **D-I6 Errors stay in the window**: an unwritable folder and the
  running-Phi wait are InfoBars; the abort confirmation ("Stop installing
  Phi Browser?") is still a MessageBox. NSIS disables Cancel and the
  caption's close while files are copied, so the progress screen's Cancel
  is greyed rather than the design's live one.
* **D-I7 The uninstaller quits when the files are gone** (`Quit` at the
  end of its section - AutoCloseWindow does not carry an uninstaller past
  its last page); a silent uninstall keeps the data as before.

## 13. Send feedback (the two "Send feedback" artboards, added 2026-09-23)

The Mac port's form (a well, an "Additional info" card with a list box and
Preview / Remove buttons, legal links under the notice, a lone Send, a
modal sign-in prompt and a modal "Could Not Add Attachment") becomes a
Windows Settings-style form. Implemented in `client/phi_feedback_window.cc`
the same day; the outbox, the harness verbs and the upload are unchanged.

### 13.1 Anatomy

* **Window**: 560 wide, `PhiPopupChrome` caption with the Phi mark and
  "Send feedback", minimize + close. The height follows the content: an
  InfoBar, a second tile row or the open diagnostics card grow it; the top
  edge stays put.
* **Header**: "Tell us what went wrong" (20 / 600) and, signed in, a
  "Sending as <email>" line with an initial disc in the accent.
* **Signed out**: an informational InfoBar above the title — "Sign in to
  send" / "Reports are tied to your Phi account so we can follow up. You can
  keep writing — nothing is lost." — with an accent **Sign in** button.
  Send stays disabled until an account exists; the window follows sign-in
  and sign-out live (`PhiFeedbackWindow::OnAccountChanged`, called from
  `SyncFeedbackAccount`). The modal "Sign in now?" alert is gone.
* **Description**: Fluent multi-line TextBox, 144 high (control fill at
  rest, card + 2 dip accent bottom edge on focus); the scrollbar appears only
  when the text overflows. Helper line under it; from 3,000 characters the
  counter "3,412 / 4,096" sits at its right (red at the limit).
* **Page (optional)**: one-line TextBox with a globe glyph and a clear ✕,
  prefilled from the focused tab ("From the tab you were on").
* **Attachments (optional)** — "Up to 5 files, 10 MB each": a four-column
  grid of 88 dip tiles (thumbnail — WIC-decoded, cover-cropped — or a
  document glyph with the extension; name; size, "Pasted · 96 KB" for a
  pasted image) followed by a dashed **Add files / or paste an image** tile
  that disappears at five. A tile is a focusable button: click / Enter
  opens it, hover shows a ✕ in its corner, Delete removes it, right-click or
  Shift+F10 opens the Fluent menu (Open · Show in Explorer · Remove; Show in
  Explorer is off for pasted images). The five-file limit is now enforced
  when adding (it used to be a silent trim at submit).
* **Adding**: the picker, **drag and drop from Explorer** (OLE drop target
  with the shell's drag image; while files hover the tile row becomes an
  accent-dashed "Drop to attach N files" zone; WM_DROPFILES is the fallback),
  and **Ctrl+V anywhere but the Page field** — files copied in Explorer attach
  as files, an image as `image-N.png`.
* **Refused files**: an error InfoBar above Attachments ("Some files
  weren't added" + "name: reason" for up to three, "and N more."), with a
  ✕; the next add attempt replaces it. No modal.
* **Diagnostic info**: an expander card ("Diagnostic info is included" /
  "Phi and Windows versions, extensions and recent browser logs"); open, it
  lists Phi, Windows, page title, Phi extensions and "Browser logs —
  Collected when you press Send". Informational only: the pipeline always
  sends these, the card just says so.
* **Notice**: the privacy sentence flows word by word with **Privacy Policy**
  and **Terms of Service** inline (they are link buttons placed where their
  words land, so they stay tab stops).
* **Footer**: card-coloured band with a separator, **Send** (accent) ·
  **Cancel**, 120 wide each, right-aligned. Enter anywhere but the
  description sends; Ctrl+Enter there.

### 13.2 States

* **Sending**: fields and tiles lock, Send shows "Sending…", the footer runs
  Fluent's indeterminate bar with "Collecting diagnostics…". Cancel (or
  Esc) stops it — the framework's late log answer is dropped by request id.
* **Sent**: the window becomes a confirmation — green check disc, "Thanks
  for the report", "Phi is uploading it in the background. If you're
  offline or Phi closes first, it finishes the next time Phi runs." —
  with **Close** (Enter / Esc). It used to vanish silently. Reopening the
  form from a browser window while the confirmation is up starts a fresh
  report.
* Harness: `agentSpace.feedback.status` gains `sent`, `signedIn` and
  `diagnosticsExpanded`; `test/suite-feedback.mjs` waits for `sent` instead
  of the window closing.

### 13.3 Not verified on the box (2026-09-23)

The signed-out InfoBar (the box has a real session, and the harness's
stand-in account never overrides one), the OLE drop zone, Ctrl+V of an
image or of Explorer files, and the Sending / Sent states (a real Send
files a report on staging under the signed-in account). The rest —
fields, tiles and thumbnails, hover ✕, the tile menu, the refused-files
InfoBar, the five-file limit, the counter, the expander, light and dark —
was captured on the box.
