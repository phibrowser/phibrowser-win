# Windows sidebar architecture

Status: DESIGN (2026-09-07). All five phases of §9 are IMPLEMENTED (same
day; phase 5 last):
the store holds one row per split cell with a structured tag;
`BuildSidebar()` in `phi_sidebar_model.{h,cc}` is the single reconciliation
boundary; `phi_sidebar_layout.{h,cc}` places the pinned grid, the lists and
the strip widths as pure functions; the rebuild runs on its own posted turn,
never in `WM_PAINT`; every sidebar mutation is a `SidebarCommand` applied
by `PhiSlotHost::Apply` over the `Move(key, place)` table, drags resolve a
`SidebarPlace` through the pure resolvers in `phi_sidebar_drop.{h,cc}`, and
drag state is keyed by `SidebarKey`; Peek and Reader View share one
registry and one lifecycle (`phi_surface_registry.{h,cc}`), the owning row
paints one indicator whose minus closes the surface, and the framework
recognises a reader surface from what it shows, panels it at insertion and
closes husks. The four pure modules are host-tested by `test/model/run.sh`;
drags by `test/sv-drag.mjs`, surfaces by `test/sv-surface.mjs`. The
remaining §7 items and the row-cache note are listed under §9. The
decisions in §10 were taken as proposed.

The sidebar is the tab list, the pinned grid, the bookmark outline, tab
groups, split views, and the two attached surfaces (Peek and Reader View),
plus every transfer of a tab between them. This document fixes the state
model, the ownership boundaries, the transition rules, and the framework API
the client needs. It was written after reading the complete Mac
implementation (`phibrowser-mac/Sources/UserInterface/Sidebar`, `Peek`,
`Reader`, `States/TabOrder`, `LocalStorage/TabDataModel`) and the current
Windows controller; §1 records what that reading taught us so the reasons
survive the code.

---

## 1. What the Mac got right, and what it pays for

### Keep

* **Off-strip by omission.** A Peek or Reader page is a real Chromium tab
  that is simply never added to the list the sidebar, MRU switcher and focus
  logic read from. Because hiding is "not in the list", the Mac's Cmd+Tab
  and native Search Tabs code contain zero peek/reader logic. We keep this
  invariant and push it one level down (§4.6): the framework hides surface
  tabs from the snapshot, so the client never sees them at all.
* **Pinned tabs are Phi records, not Chromium pinned tabs.** A pin can be
  *closed* (no live tab) and still occupy its cell; its live tab may sit
  anywhere in the strip. Chromium's pinned region gives us nothing we need
  and would forbid closed pins.
* **Ordering is synced by relative anchors, never absolute indices.** The
  sidebar's visible order is not the strip's index space (bound tabs are
  hidden), so a move is "before/after tab X", resolved to an index by the
  side that owns the strip.
* **`NativeTabDecisionEngine`** (where a new tab lands, what becomes active
  on close) is clean and takes exactly the right inputs: the *visible* list
  plus the set of hidden openers. Port it as-is when session-restore and
  opener placement land; it is out of scope for this document.
* **Group rows are containers, not outline branches**; membership is derived
  from each tab's group id, never cached.
* One store row type with an integer discriminator and one owner key
  `(profile_id, space_id)`, learned the hard way across ten schema versions
  (V1 parallel booleans, V3 three owner keys, V9 physical vs logical id).

### Fix

| Mac cost | Where it shows | Windows decision |
|---|---|---|
| Surface ownership is a **stringly side channel** (`custom_value`) shared by pin guids, bookmark guids, `split-pending:`, `phi-peek:`, `placeholder-`, `mock-` sentinels; three consumers re-derive membership with slightly different rules | `BrowserState.swift:988-1006`, `BrowserState+Split.swift:31-76`, the cross-domain throttle | The tag carries **one structured value** (§4.3). Sentinels are forbidden; transient state lives in client structs. Membership is derived in one function. |
| **Two incompatible split encodings**: a split bookmark is one row with `secondaryUrl`; a pinned split is two rows cross-linked by `splitPartnerGuid`, with a documented half-persisted-link bug and a synthesized `persisted-split:a\|b` identity | `TabDataModelSchemaV4/V5`, `PinnedTabViewController.swift:665-720` | **One row = one cell**, for pins and bookmarks alike (§4.1). |
| Split-bookmark binding lives in a runtime dictionary, not the tag; both panes untagged | `BrowserState+Bookmark.swift:367-392` | Both panes tagged with a pane role; binding survives restart symmetrically. |
| **N×M transfer verbs**: `moveNormalTab(toPinned:)`, `(toBookmark:)`, four `moveBookmarkOut` overloads, three `movePinnedTabOut`, batch variants | `BrowserState.swift:7010-7938` | One `Move(item, place)` command over a transition table (§5). |
| A closed pin is a `Tab` with `guid == -1`; two `Tab` objects per open pin, joined by mutating one to borrow the other's id | `BrowserState.swift:759-786` | Binding is an explicit enum on the cell; there is no fake tab. |
| Clear-tag → navigate → restore-tag on a 0.5 s timer, five copies, because the throttle keys on the tag | `BrowserState.swift:664-676` and four more | Framework one-shot exemption `NavigateTabInPlace` (§7). |
| Peek visibility kept alive by borrowing the split-partner marker; three call sites must remember to clear or not clear it | `BrowserState.swift:3647-3655, 3878, 3903, 4191` | Not applicable: Windows surfaces are separate Browsers. Never reintroduce it. |
| Peek indicator implemented three times (tab row, bookmark row, pinned badge); Reader has none | `SidebarCellViews`, `BookmarkCellView`, `PinnedTabItem` | One `attached_surface` field on the cell, one painter (§4.6). |
| Layout mode changes *which tabs exist in the list* from inside the state layer | `BrowserState.swift:987-1002` | A single projection policy parameter (§4.7). |
| Selection is three mechanisms plus a floating proxy row for a tab in a collapsed folder | `SidebarTabListViewController.swift:4645, 5424-5494` | One `active_key` plus `contains_active` on containers (§4.8). |
| `BrowserPeekState` and `BrowserReaderOverlayState` are the same 51 lines twice; panels duplicate five methods and three observers | `Sources/States/Browser*State.swift`, both panel controllers | One `PhiSurfaceRegistry`, one panel host base (§4.6). |

### The Windows controller today (2026-09-07)

Two systems are stacked. `ProjectSidebar()` (*exists*) is a correct
identity-based reconciliation whose result is read at exactly one line
(`owners.count(row.id)`); the painter still runs on the older path inside
`RebuildTabs`/`RebuildShortcuts`: tag scans (`flag_bound`, `is_pin`),
split merge by strip adjacency, pinned pairs merged by erasing the partner
from `pinned_` so grid indices no longer match store rows, the rebuild
executed inside `WM_PAINT`, two `ListTabsJson` parses per rebuild, and
`ComputeLayout()` reading counts that `RebuildShortcuts()` writes while
`RebuildShortcuts()` calls `ComputeLayout()`. Rows are keyed by vector
index for drag and by three different structs (`TabRow`, `ShortcutRow`,
`SidebarContentRow`). There are no tests. §9 removes all of this; nothing
from the old path survives beside the new one (the fork's AGENTS.md
"no parallel patterns" rule applies to this repo too).

---

## 2. Principles

1. **Three axes, three owners.** Stored identity (`PhiTabStore`), live
   content (the framework), presentation (`PhiWindowController`). No struct
   mixes axes: a cell has no `RECT`, a layout row has no favicon bytes, a
   store row has no HWND.
2. **One reconciliation boundary.** `BuildSidebar()` is the only place that
   decides what a tab *is* (plain, pin-bound, bookmark-bound, split pane,
   surface, grouped). Everything downstream reads its output.
3. **Stable identity at every boundary.** Session ids for tabs, guids for
   stored rows, Chromium tokens for groups and splits. A strip index is
   valid only inside the snapshot that produced it and never crosses a
   function boundary that could outlive that snapshot.
4. **The strip is the ordering authority.** The client keeps no private tab
   order. Reorder is a framework move by anchor; Chromium keeps splits
   adjacent and groups contiguous for us.
5. **Pure where possible.** Model, drop resolvers and the transition table
   are functions of snapshots, compiled and tested on the Linux host with
   no Win32 or framework dependency.
6. **Stale-safe by construction.** Every rule that could hide content on
   bad data instead leaves it visible as a plain tab. Repairs happen in
   store migrations, never in the projection.
7. **Nothing structural in `WM_PAINT`.** Paint and hit-test read a finished
   layout.

---

## 3. Module map

```
client/
  phi_tab_store.{h,cc}         stored identity (rows, owner, order, migrations)   exists, schema change §4.1
  phi_sidebar_model.{h,cc}     BuildSidebar(): snapshots -> SidebarTree           exists as ProjectSidebar, replaced
  phi_sidebar_layout.{h,cc}    LayoutSidebar(): SidebarTree + view state -> geometry   new
  phi_sidebar_drop.{h,cc}      ResolveDrop(): layout + pointer -> SidebarPlace        new (absorbs ResolveBookmarkDrop)
  phi_sidebar_actions.{h,cc}   Apply(SidebarCommand): transition table -> framework + store   new (coordinator side)
  phi_surface_registry.{h,cc}  peek/reader/... attached surfaces per window           new (absorbs the peek/reader panel bookkeeping)
  phi_window_controller.*      gestures -> SidebarCommand; paints SidebarLayout       shrinks
  phi_coordinator.*            owns store, registry, actions; framework delegate     shrinks (host verbs collapse)
test/model/                    host-compiled tests for model/layout/drop/actions     new
```

Dependency direction is strictly downward: `controller → layout → model →
(store snapshot, framework snapshot)`. `actions` depends on model output and
on the framework/store interfaces, never on the controller.

---

## 4. The model

### 4.1 Stored identity (`PhiTabStore`)

One row type, as today, with the split encoding unified:

```cpp
enum class PhiRowType { kPinnedTab = 1, kBookmark = 2, kBookmarkFolder = 3 };

struct PhiRow {
  std::string guid;              // physical + logical id; never reused
  PhiRowType type;
  std::string profile_id, space_id;   // owner; empty = not scoped (Mac tri-state)
  std::string parent_guid;       // folders/bookmarks: tree; pins: always empty
  int index;                     // dense order among siblings of the same type+owner+parent
  std::string lineage_id;        // survives pin-scope migration (Mac pinLineageId)
  // Pane A (always present for leaves)
  std::string title, url; Favicon favicon;
  // Pane B (present iff this cell is a split)
  std::optional<Pane> pane_b;    // {title, url, favicon}
  std::string layout;            // "vertical" | "horizontal" | "" (= vertical)
  std::string icon;              // folders only
  std::optional<Time> last_seen;
};
```

Changes from the current file:

* `split_partner_guid` is **removed**. A pinned split is one row with
  `pane_b`, exactly like a split bookmark. The `LinkSplitPair`,
  `SplitPartner`, and the "remove takes the partner" rule go away; so do
  `paired_pins` in the model and the `pinned_` erase in the controller.
  Migration (`PhiTabStore::Load`, schema 1→2): for each reciprocal pair,
  fold the higher-index row into the lower one's `pane_b`, delete it,
  renumber siblings. Log the count. The dev boxes are the only installs.
* `PhiRowType::kTab` (0) is dropped; nothing writes it.
* Pane B keeps its own favicon; today it is fetched live on every rebuild.
* A `schema` integer is written at the top of the JSON file so the next
  change is a migration, not a guess.

Ownership and pin scope are unchanged (`PhiPinScope`, `OwnerForScope`).
The "owner trap" (a new row must take the *slot's* owner, not the source
row's) becomes a rule of `actions`, stated once.

### 4.2 Live content (framework snapshot)

The framework reports each window's strip once per rebuild. Per tab:

```jsonc
{ "id": 7, "index": 0, "title": "…", "url": "…", "active": true,
  "loading": false, "unloaded": false,
  "splitId": "…",  "groupId": "…",  "tag": "…" }   // keys absent when N/A
```

Requirements on this snapshot (see §7 for the fork changes):

* **Surface tabs are absent.** A Peek or Reader page never appears in
  `ListTabsJson` of a user window. The client stops parsing
  `reader.html#tab=…` URLs, and `SidebarSurface`/`origin_id` leave the
  live-tab struct.
* `id` is the session id. Nothing in the client stores an `index` beyond
  the current rebuild.

The client parses this **once** into `std::vector<LiveTab>`; groups and
splits come from `TabGroupsJson` and `SplitViewsJson` in the same rebuild.

### 4.3 Binding: how a live tab belongs to a stored cell

The carrier stays the framework's per-tab tag (`PhiTabData::custom_value`),
because it is the one thing Chromium session-restores for us and the fork
already keys "excluded from tab groups" and the cross-domain throttle on it.
Its content becomes structured and exclusive:

```
<guid>        pane A of the stored row <guid>  (also a single, non-split cell)
<guid>#b      pane B of the stored row <guid>
```

Rules:

* Nothing else is ever written into the tag by the Windows client. No
  pending markers, no placeholders. (The Mac's `phi-peek:` namespaces are
  irrelevant here; Windows Peeks are separate Browsers.)
* `ParseTag(tag) -> {guid, pane}` is the only decoder; `TagFor(guid, pane)`
  the only encoder. Both live in `phi_sidebar_model.h`.
* Binding state is **derived** per cell, never stored:

```cpp
enum class BindState { kClosed, kOpen, kSplitOpen, kPartial };
struct CellBinding {
  BindState state;
  int tab_a = 0, tab_b = 0;     // session ids, 0 = no live pane
  std::string split_id;         // when both panes share one live split
  bool active = false;          // a live pane is the strip's active tab
};
```

  * `kOpen`: pane A tagged, not in a split; cell has no `pane_b`.
  * `kSplitOpen`: cell has `pane_b`; two tabs tagged `<guid>` and
    `<guid>#b`; both in the *same* live split.
  * `kPartial`: cell has `pane_b`; one pane's tab is live and not in any
    split, the other is missing. Clicking reopens the missing pane and
    re-forms the split (Mac `openPinnedSplit`).
  * `kClosed`: no tagged live tab.
* **Safety rules** (all *exist* in `ProjectSidebar` today and carry over):
  a duplicate tag binds the first tab in strip order, later duplicates stay
  plain; a tagged tab sitting in a split that the cell does not describe is
  **not** hidden (it renders as a plain split row); a live pane can have at
  most one owner; folders never own tabs.
* **A split never lives in a stored cell by accident.** Forming a split
  from a bound tab first unbinds it (clears the tag); the cell stays as a
  closed placeholder (Mac `demotePinnedTabLeavingPlaceholder`). Storing a
  live split (pin/bookmark the pair) writes `pane_b` and tags both panes.

### 4.4 Keys

```cpp
struct SidebarKey {
  enum Kind { kTab, kSplit, kGroup, kCell, kFolder, kNewTab } kind;
  std::string id;   // "7" | split token | group token | guid
  static SidebarKey Tab(int session_id); …
};
```

One key type for every row, hit target, drag payload, hover, selection and
animation identity. No `AnyHashable`, no `Int`/`String` namespace mixing, no
vector indices. A split row is keyed by the split token, so pane swaps and
pane replacement keep the row identity (Mac `SplitPairSidebarItem`).

### 4.5 The projection: `BuildSidebar()`

```cpp
struct SidebarInputs {
  std::vector<PhiRow> pins;               // this slot's pinned cells, ordered
  std::vector<PhiRow> bookmarks;          // the WHOLE tree for this owner, incl. collapsed
  std::vector<LiveTab> strip;             // §4.2
  std::vector<LiveGroup> groups;          // id, title, color, collapsed
  std::vector<LiveSplit> splits;          // id, primary, secondary, layout, ratio
  std::map<int, SurfaceKind> surfaces;    // origin tab id -> attached surface
  SidebarPolicy policy;                   // §4.7
};

struct SidebarTree {
  std::vector<PinnedCell> pinned;         // key kCell, row, CellBinding, attached surface
  std::vector<BookmarkNode> bookmarks;    // key kCell|kFolder, depth, parent, CellBinding, contains_active
  std::vector<ContentNode> content;       // in strip order:
                                          //   kGroup header {group, collapsed, contains_active, members...}
                                          //   kTab / kSplit rows {tab ids, group id, attached surface}
  std::map<int, SidebarKey> owner_of;     // live tab -> cell that hides it (absent = visible)
  SidebarKey active_key;                  // the row that highlights (§4.8)
  int active_tab = 0;
};

SidebarTree BuildSidebar(const SidebarInputs&);
```

Guarantees:

* Every live tab appears exactly once: as a plain/split content row, or in
  `owner_of`. Nothing is dropped.
* Splits are resolved **by split id**, never by adjacency. A split whose
  panes straddle a group boundary or a hidden pane renders as two plain
  rows.
* A group header is emitted once, at its first member in strip order, with
  members listed under it. Contiguity is Chromium's job; if a snapshot ever
  shows a non-contiguous group the header is still emitted once (dedup by
  id, not run-flush, the Mac's `TabSectionController.swift:245-253` lesson).
* Collapsed groups keep their members in the tree flagged `hidden`; the
  layout omits them. Collapse is Chromium state (`SetTabGroupCollapsed`)
  and is never written locally.
* Bookmark nodes are the whole tree. Folder expansion is **not** an input;
  the layout applies it. This is what makes a collapsed folder's bound tab
  still count as owned (today it does not, because `bookmarks_` only holds
  expanded rows).
* The function is pure and deterministic; it performs no repairs and no
  writes.

### 4.6 Attached surfaces (Peek, Reader View)

On Windows both surfaces are separate `Browser`s hosted in `WS_CHILD`
panels over the content card; they are not tabs of the user window. The
model needs one fact per origin tab: *which surface is attached*.

```cpp
enum class SurfaceKind { kPeek, kReader };
class PhiSurfaceRegistry {         // one per shell window
  struct Entry { SurfaceKind kind; int origin_tab; int64_t host_window;
                 PhiSurfacePanel* panel; };
  // Attach/Detach/ForOrigin/CloseForOrigin/DetachAllForWindowClose
  // One $tabs-style orphan watch: on every rebuild, entries whose origin
  // left the strip are closed.
};
```

* One registry replaces the peek-specific bookkeeping in
  `PhiPeekPanel`/`PhiCoordinator::UpdateReaderPanelsForWindow`. One panel
  host class parameterised by geometry (card-inset with a control gutter for
  Peek; full card, no chrome for Reader) and one eclipse/conceal contract
  for the omnibox dropdown and Search Tabs.
* The sidebar consumes only `surfaces[origin] -> kind`, rendered as a
  trailing indicator on whichever row owns the origin (tab row, split row,
  bookmark row, pinned cell): **one field, one painter**. Reader gets the
  same indicator as Peek; the Mac's asymmetry is a gap, not a feature.
* Cardinality: at most one surface of each kind per origin; attaching a
  second Peek to the same origin replaces the first.
* Lifecycle rules, identical for both kinds: origin closes → surface
  closes; origin leaves the window (tear-off, move to Space) → surface
  closes; address-bar navigation on the origin → surface closes first;
  Back/Forward while a surface is up → consumed as "close surface";
  window close → detach without closing (Chromium tears down).
* Reader's one extra transition: a link click inside the reader page
  navigates the surface away from `reader.html`; the panel gives the tab up
  and it becomes a plain tab in the origin's window (Mac
  `expandReaderOverlayIntoTab`). Peek's extras: "Open as Tab" and "Open as
  Split with origin" (both normalize the origin to plain first).
* Session restore: surfaces never restore. The framework must not report
  a restored `reader.html` husk; it closes it.

The transient "Mirage created the surface tab in the origin's strip and the
client sweeps it out a frame later" path is removed by having the framework
divert the surface tab before the strip notification fires (§7). Until that
lands, the client keeps the `WM_PHI_READER_SWEEP` deferral, but the model
never sees the tab because the framework marks it and the snapshot filter
drops it.

### 4.7 Policy

```cpp
struct SidebarPolicy {
  bool horizontal_layout;   // comfortable: pins hidden from strip, bookmark-bound tabs VISIBLE in the strip
  bool incognito;           // no stored cells at all
  bool agent_space;         // read-only rows
};
```

This is the only place layout mode influences membership, and it does so by
choosing whether `owner_of` hides bookmark-bound tabs. Everything else about
comfortable vs. balanced vs. performance is layout, not model.

### 4.8 Active row

`active_key` is computed once in `BuildSidebar()`:

1. The active tab's owner cell, if hidden by one (pin or bookmark).
2. Else its split row, if it is a visible split pane.
3. Else its tab row.

Containers carry `contains_active` (group header when collapsed, bookmark
folder chain regardless of expansion) so a collapsed container paints a wash.
The Mac's floating proxy row (a phantom sibling for a tab inside a collapsed
folder) is **not** ported: the wash plus "expand on activation from the
omnibox/search" covers the need with no extra row kind. Group overview
suppresses the highlight in the layout (`SidebarViewState::overview_group`),
not in the model.

### 4.9 Presentation state (`SidebarViewState`, controller-owned)

Expanded folder set, scroll offset, hover key, armed drag (key + press
point), multi-selection (set of keys + anchor), group-overview token, Space
switch band snapshot. Keyed by `SidebarKey`, never by index. Any of these
may change without a model rebuild.

---

## 5. Transitions

### 5.1 One verb

```cpp
struct SidebarPlace {
  enum Section { kPinned, kBookmarks, kTabs } section;
  std::string parent_guid;    // kBookmarks: folder ("" = root)
  std::string group_id;       // kTabs: inside this group ("" = ungrouped)
  std::optional<SidebarKey> before;   // anchor; absent = end of the section/parent/group
};

struct SidebarCommand = variant<
  Activate{key, pane}, Close{key, pane}, CloseCellTabs{key},
  Move{key, SidebarPlace},                 // reorder, pin, unpin, bookmark, unbookmark, join/leave group
  FormSplit{key_a, key_b, layout}, BreakSplit{key}, SwapPanes{key},
  SetLayout{key, layout}, ReplacePane{split_key, pane, key},
  ToggleFolder{key}, SetGroupCollapsed{key, bool},
  Rename{key, title}, SetUrl{key, url}, SetIcon{key, icon},
  MoveToSpace{key, space_id, clone}, TearOff{keys, screen_pt},
  OpenSurface{origin_key, kind}, CloseSurface{origin_key, kind}>;
```

`PhiSidebarActions::Apply(cmd, const SidebarTree&)` is the only mutator.
Every gesture, menu item, keyboard shortcut and agent verb produces a
command; the controller never calls the framework or the store for sidebar
state directly. This replaces `RequestStoreTab`, `RequestStoreSplit`,
`RequestDropTab`, `RequestDropSplit`, `ConvertStoredRow`, `MoveStoredRow`,
`ReorderStoredRow`, `RequestTabToSpace` and their siblings on `PhiSlotHost`.

### 5.2 `Move` transition table

Rows: what the key currently is. Columns: destination section. Each entry
is the *whole* effect; "tag" means the framework tag on the live tab(s).

| from ↓ / to → | Tabs | Pinned | Bookmarks |
|---|---|---|---|
| **plain tab** | move by anchor (`MoveTabBefore/After`); if `group_id` differs: leave/join group first, then move within it | leave group if any; new pin row at anchor (slot owner); tag = guid. Strip position unchanged | same, as bookmark row under `parent_guid` |
| **plain split** (both panes visible) | move the split as a unit (`MoveSplit` by anchor) | new pin row with `pane_b`; tags `guid`, `guid#b`; layout copied | same as bookmark |
| **pin cell** (closed) | new tab at anchor, url = pane A (both panes + split when `pane_b`); row deleted | reorder pins (`Reorder`) | new bookmark row copies panes/favicons/layout; old row deleted |
| **pin cell** (open) | clear tag(s); move live tab(s) to anchor; row deleted | reorder | retag live tab(s) to the new guid; new row at tree target; old row deleted |
| **bookmark cell** | symmetric to pin cell | symmetric | `Move(guid, parent, index)` in the tree |
| **folder** | refused | refused | tree move; refused into own descendant |
| **group header** | move slice by anchor (whole group) | refused (groups and pins are exclusive) | convert group to a folder: each member bookmarked (tags set), group closed |
| **surface** | n/a: surfaces are commands `OpenSurface/CloseSurface`, not places | | |

Invariants enforced here and nowhere else:

* Pins and groups are exclusive; bookmark-bound tabs and groups are
  exclusive in vertical layouts (the framework refuses grouping of tagged
  tabs; `actions` unbinds first when the user explicitly groups a bound
  tab, Mac `graduateBookmarkTabToPlainTab`).
* A new stored row takes the **destination slot's** owner.
* Every write to the store happens after the framework calls that could
  fail; on framework refusal nothing is written.
* A `Move` that resolves to the same slot is a no-op (post-removal index
  arithmetic done once, in `ResolveDrop`).

### 5.3 Pinned and bookmark cells: open, close, snap back

| Gesture | Closed cell | Open cell |
|---|---|---|
| click (release without drag) | `NewTab(url)` then `SetTabTagById(tab, guid)`; for `pane_b`: second tab, `CreateSplitViewForTabs`, tags both | `ActivateTabById(tab_a)` (split: the pane under the pointer) |
| close glyph / middle-click | hidden | `CloseTabById` on each bound pane, higher strip index first; row stays, becomes closed |
| double-click | — | `NavigateTabInPlace(tab_a, stored url)` snap back |
| Ctrl+double-click | — | open current page as a background plain tab, then snap back |
| context "Unpin / Remove bookmark" | delete row | clear tags, then delete row; tabs stay plain |
| delete folder | recursive: for each leaf, close bound tabs, then delete | |

The **address bar on a bound row opens a new tab** rather than navigating in
place (Mac rule, because in-place navigation would re-point the pin or
divert into a Peek). The omnibox asks `tree.owner_of.count(active_tab)`.

### 5.4 Splits

* `FormSplit(a, b, layout)`: normalize both to plain (unbind, leave group
  only if the two differ), `CreateSplitViewForTabs(a, b, layout)`. Primary
  is `a`. No reverse-after-the-fact dance: the framework API is id-ordered.
* `BreakSplit`: `RemoveSplitById`; if the row was a stored `kSplitOpen`
  cell the cell's tags are cleared and the cell stays as a closed split
  placeholder.
* `SwapPanes`: `SwapSplitView`; row key unchanged; for a stored cell,
  swap `pane_a`/`pane_b` and retag.
* `ReplacePane`: `SwapTabIntoSplit`; the evicted tab stays as a plain tab.
* Stored split reopen: two `NewTab`s, split, tags; if only one pane is
  missing (`kPartial`), reopen that pane and re-form.
* The page drop zones (`phi_split_drop`) produce `FormSplit`/`ReplacePane`
  commands and nothing else.

### 5.5 Groups

* Join/leave are `Move` with a `group_id`. `actions` does: leave/join via
  the id-addressed group API, then the anchor move inside the group. The
  Mac's optimistic-membership patch and interloper relocation exist only
  because its list is a private copy; we rebuild from the strip after the
  coalesced notification and have nothing to reconcile.
* Header click → `SetGroupCollapsed(!collapsed)`; the header's chevron
  reflects the echo. Header double-click → group overview (view state).
* Collapsing a group that contains the active tab first activates the
  nearest tab outside it (framework rule, already implemented).
* Whole-group drag: `Move(group_key, place)`; onto a folder →
  convert-to-folder; onto the page/outside → `TearOff`.

### 5.6 Surfaces

`OpenSurface{origin, kPeek}` is issued by the framework's peek path
(`CreatedWindowPeekOpener`) and by the link context menu;
`OpenSurface{origin, kReader}` by the reader button, View menu, shortcut,
and the fork's `OnOpenReaderView`. `CloseSurface` by the panel chrome, Esc,
Ctrl+W (swallowed while a surface is up, so it cannot close the origin),
Back/Forward, the omnibox commit, and the sidebar indicator's minus. All
routes end in `PhiSurfaceRegistry`; the model only sees the resulting
`surfaces` map on the next rebuild.

---

## 6. Pipeline

```
framework OnTabsChanged / OnTabGroups… / store changed / registry changed
        │  (coalesced: set dirty bit, PostMessage(kMsgPhiSidebarRebuild) once)
        ▼
PhiWindowController::RebuildSidebar()          never from WM_PAINT
   1. one ListTabsJson + TabGroupsJson + SplitViewsJson parse -> LiveTab/LiveGroup/LiveSplit
   2. host_->StoredRows(slot) -> pins + whole bookmark tree
   3. registry_->Surfaces(window) -> map
   4. tree_ = BuildSidebar({...})              pure
   5. layout_ = LayoutSidebar(tree_, view_state_, metrics_)   pure geometry
   6. diff old/new layout keys -> row slide animations (optional, later)
   7. InvalidateRect; post kMsgPhiSyncNativePages if the active url changed
        ▼
WM_PAINT: paints layout_ only.   Hit-test / hover / drag: read layout_ only.
```

* `LayoutSidebar` takes the pinned and bookmark *counts* from the tree, so
  the `ComputeLayout ↔ RebuildShortcuts` cycle disappears: geometry is a
  function of `(tree, view_state, sidebar width, DPI)`.
* Favicon bytes are fetched during step 5 into a `FaviconCache` keyed by
  session id / guid, not copied per rebuild from the framework's scratch
  buffer by index.
* View-state-only changes (hover, scroll, folder toggle, drag slide) rerun
  step 5 only.
* The Space-switch slide keeps the *previous layout* (a `SidebarLayout`
  value), not a second copy of four row vectors.
* `RebuildTabs()`, `RebuildShortcuts()`, `ReadSidebarProjection()`,
  `flag_bound`, `is_pin`, `TabRow`, `ShortcutRow`, `BandModel`,
  `StripTab` and the adjacency merge are deleted in phase 3 (§9).

---

## 7. Framework API changes (fork, phi-owned, `IS_PHI_ABI_CLIENT`)

All additive; index-addressed methods remain for the agent dialect until
nothing else calls them.

| Addition | Why |
|---|---|
| `ActivateTabById`, `CloseTabById`, `SetTabTagById`, `TabFaviconBgraById` — DONE | indices shift during the very notification the client is reacting to |
| `MoveTabBefore(window, tab_id, anchor_id, group_id)`, `MoveTabAfter(...)` — DONE (a pane moves its whole split; `group_id` nullptr = Chromium assigns, "" = ungrouped); `MoveGroupBefore/After` — open | the sidebar's index space is not the strip's; Chromium resolves anchors |
| `NavigateTabInPlace(window, tab_id, url)` | one-shot exemption from `CrossDomainNewTabNavigationThrottle` for a bound tab (same shape as `PhiURLRouter::ExemptNextNavigation`); ends the clear-tag/navigate/restore timer pattern |
| `ListTabsJson` marks surface tabs (`"surface":"reader","originTabId"`), recognised from the visible url — DONE (marked rather than excluded: the client's fallback sweep and the panel's own list still need to see them; the model drops them) | the client never parses `reader.html` URLs |
| Reader surface diverted at insertion — DONE as a posted move right after the strip insertion (`DivertReaderSurfaceAfterInsert`), not a pre-insertion redirect; husks (restore, no origin) close there | removes the client-side sweep's work and the strip flash |
| `OnTabsChanged` payload gains `"reason"` (added/removed/moved/activated/updated) | lets the layout animate; still a full rebuild |

Not requested: any change to the throttle's keying, group-exclusion rule or
`phiSaved` semantics. They read "non-empty tag", which the structured tag
satisfies.

---

## 8. Testing

`test/model/` compiles `phi_sidebar_model.cc`, `phi_sidebar_layout.cc`,
`phi_sidebar_drop.cc` and the transition table with host `g++` (they
depend on the STL only; the store's JSON codec is split into a header-only
`phi_tab_store_codec.h` so migrations are testable too). Cases to carry
from the Mac's comment-documented bugs:

* duplicate tag → first wins, others plain
* tagged tab in an unrelated split → not hidden
* split straddling a group boundary → two rows
* collapsed folder's bound tab → still owned, folder `contains_active`
* half-persisted pinned pair (schema 1) → migrated to one cell
* drop past the last row, drop between split panes (snap outside), drop
  on a collapsed folder's upper half → into, same-slot no-op
* comfortable policy: pins hidden, bookmark-bound visible
* surface attached to a hidden origin → indicator on the owner cell

Box verification stays as it is (PhiCap/PhiDragCap rigs, `test/*.mjs`).

---

## 9. Migration plan

Each phase leaves the client building and usable on the box. Order matters:
the store change first so nothing new is written in the old encoding.

1. **Store: one cell per split.** DONE 2026-09-07. `schema` 2 in the JSON,
   pane B as `url_b`/`title_b`/`favicon_b_*` on the one row, partner API
   removed, `Load` folds schema-1 pairs and persists `legacyTagAliases`
   (old partner guid → `<survivor>#b`) which the coordinator applies to
   session-restored tabs on the deferred sweep and then drops. `TagFor` /
   `ParseTag` in `phi_sidebar_model.h`; both panes tagged for pins AND
   bookmarks; `ProjectSidebar`, `RebuildShortcuts`, `flag_bound`,
   `OpenStoredSplit`, `StoreSplitInternal`, `ConvertStoredRow`,
   `RemoveStoredRow` read the one encoding. `agentSpace.spaces.listTabs`
   rows gained `index`, `tag`, `splitId` so probes can assert bindings.
2. **Model: `BuildSidebar` replaces `ProjectSidebar`.** DONE 2026-09-07.
   `SidebarKey`, `SidebarInputs` (whole stored tree with sibling index,
   strip, groups, surfaces, policy) → `SidebarTree` (pins, depth-first
   bookmarks, content nodes with group headers, `owner_of`, `active_key`).
   `RebuildTabs` reads the strip ONCE, builds the tree, takes every
   binding, split row, group header and the active tab from it;
   `flag_bound`, `is_pin`, the adjacency merge, the second `ListTabsJson`
   parse and the duplicated reader-origin logic are gone. `PhiSlotHost`
   gained `AttachedSurfaces(browser)` (peek/reader origins), consumed by
   the model though not yet painted (phase 5). 17 host cases in
   `test/model/sidebar_model_test.cc`. Deviations from §4.5: no
   `LiveSplit` input yet (pane order is strip order, which Chromium keeps
   primary-first); the reader surface is still recognised client-side from
   its URL in `ReadStrip` until §7 lands.
3. **Layout and pipeline.** DONE IN PART 2026-09-07. Done:
   `phi_sidebar_layout.{h,cc}` — `LayoutPinnedGrid` (the Mac's breakpoints,
   stretch, wrap, two-unit split cells), `LayoutList` (rows/headers, hidden
   members, scroll, viewport clipping, unclipped edges for group cards),
   `SolveStripWidths` — with host tests; `RebuildTabs` builds the tree,
   derives the pinned-grid row count and the outline's visible rows from it,
   calls `ComputeLayout` ONCE, then `RebuildShortcuts(layout)` (tree cells +
   store views, no layout call inside) and the list placement; the
   `ComputeLayout ↔ RebuildShortcuts` cycle is gone; `PinnedGridWalk`
   delegates to the pure grid. `ScheduleRefresh` posts a coalesced
   `kMsgPhiRebuildRows`; `WM_PAINT` never rebuilds (a stale dirty flag only
   posts one). Deferred, deliberately: replacing `TabRow`/`ShortcutRow`/
   `BandModel` with key-addressed layout rows and hit-testing by key — those
   structs are read at ~240 sites (paint, hover, drag, menus, previews) and
   the drag/hit paths are what phase 4 rewrites around `SidebarKey`, so the
   swap belongs there rather than as a second churn now; and `FaviconCache`
   — the framework refetch by index per rebuild is cheap and a cache keyed
   by session id would need its own invalidation on favicon changes, which
   `OnTabsChanged` does not distinguish (§7).
4. **Actions.** DONE 2026-09-07. `phi_sidebar_actions.{h,cc}`: `SidebarPlace`,
   the `SidebarCommand` variant (Move, Activate, Close, RemoveCell,
   FormSplit, BreakSplit, SwapPanes, SetLayout, JoinGroup, LeaveGroup,
   SetGroupCollapsed, BookmarkCopy, MoveToSpace) and the pure place helpers
   (`PlaceSiblingIndex`, `PlaceAnchorTab`, `TabOfKey`, `MoveIsNoop`).
   `PhiCoordinator::Apply` is the one mutator; `ApplyMove` is the §5.2
   table (live → tabs: `MoveTabBefore` with the anchor and explicit group;
   live → stored: `StoreTabsById`; cell → tabs: open-if-closed, remove,
   anchor move; same section: reorder / tree move; across sections:
   `ConvertStoredRow`). `phi_sidebar_drop.{h,cc}`: `ResolveBookmarkDrop`
   (the Mac's folder resolver), `ResolvePinnedDrop`, `ResolveTabDrop`
   (group runs, headers) over `DropRow`s the controller builds from its
   placed rows. Controller: `drag_key_` replaces `drag_index_` (the index
   is derived per use by `DragIndex()`, so a rebuild mid-drag cannot
   invalidate it); `CommitDragImpl` is one `Apply(SidebarMove)` plus the
   tear-off and page-drop branches; every context-menu and click action
   that mutates sidebar state is a command; `RequestStoreTab`,
   `RequestStoreSplit`, `RequestDropTab`, `RequestDropSplit`,
   `MoveStoredRow`, `ReorderStoredRow`, `ConvertStoredRow`,
   `RequestTabToSpace`, `MoveStoredRowToSpace` left `PhiSlotHost`;
   `RequestDropTabOnWindow` takes tab ids. Fork: `ActivateTabById`,
   `CloseTabById`, `SetTabTagById`, `TabFaviconBgraById`,
   `MoveTabBefore/After` (unit = the tab or its whole split, anchor 0 =
   end, explicit/implicit group) landed. NOT in this phase: group-header
   drag and group → folder conversion (never existed on Windows; the
   table's row stays a design), `NavigateTabInPlace`, `MoveGroupBefore/
   After`, and the row caches: `TabRow`/`ShortcutRow`/`BandModel` remain as
   the presentation caches, now carrying a `SidebarKey` and derived from
   the tree only — the doc's "delete" is satisfied in substance (no
   identity decided in them, no index addressing) and the struct names are
   kept. `agentSpace.window.layout` gained `rows` (band/key/rect/ids) so
   a box probe can aim a real drag.
5. **Surfaces.** DONE 2026-09-07. `phi_surface_registry.{h,cc}`:
   `PhiSurfacePanel` (the interface both panels implement — kind, ids,
   hwnd, Reposition, SetVisibleForActiveTab, RefreshState, Raise) and
   `PhiSurfaceRegistry` (Attach/Detach/Find/ForOrigin/OverWindow/Surfaces)
   replacing the coordinator's two maps. One lifecycle for both kinds:
   origin closes or leaves the strip → close (the sweep, one strip read);
   origin window closes → close (new for Peek); a surface's own browser
   closing → detach, activate the origin, repaint by origin WINDOW (the
   origin may be hidden in a cell); address bar / Back / Forward on the
   origin → `RequestCloseSurfaces` closes both kinds (new for Peek); a
   second Peek on an origin replaces the first; a surface browser entering
   placeholder mode retires either kind. `SidebarCloseSurface` command;
   the indicator: `attached_surface` + `surface_box` on the paint rows,
   the trailing slot on tab and bookmark rows (the row's close box is
   withheld while attached, as on the Mac), a corner badge on pinned
   cells, hovered = minus; `DrawSurfaceBadge` is the one painter. Fork:
   `ReaderSurfaceOriginTab/Of` recognise the reader page from the VISIBLE
   url; `ListTabsJson` emits `"surface":"reader","originTabId"`; the tabs
   proxy calls `DivertReaderSurfaceAfterInsert` on every insertion in a
   user window (posted): the surface moves into a panel over its origin
   and the origin is re-activated, or — during session restore, or with no
   origin in the strip — closes as a husk. The client parses no reader
   URLs any more (`ReadStrip`, the sweep, `PanelizeReaderSurface`,
   `PhiReaderPanel::RefreshState` read the field; the panel adopts its
   surface tab from it). KEPT: the client sweep as a fallback path and
   `WM_PHI_READER_SWEEP` as the deferral all surfaces settle on; the two
   panel classes (shared behaviour is the interface, not a base class —
   their geometry and chrome differ enough that a base would hold little).
   `agentSpace.window.layout` reports `surface`/`surfaceBox` per row and
   `surfaces` (the registry's map).
6. **Framework additions** (§7) can land alongside 2–5 as each consumer
   needs them; id-addressed ops first.

Out of scope here, tracked separately: `NativeTabDecisionEngine` port
(new-tab placement, close-selection), session restore of the sidebar's
open/closed cell state, multi-selection drag preview, AI chat surfaces.

---

## 10. Decisions for Jixiang

1. **Store migration**: fold pinned pairs into one cell on load (proposed)
   vs. keep reading pairs forever. Proposed is one-way and pre-release.
2. **Pin press behaviour**: today a pin opens on press and also arms a
   drag; tab rows activate on release. Proposed: all rows activate on
   release.
3. **Reader indicator**: proposed same trailing indicator as Peek on the
   origin's row (the Mac shows none).
4. **Floating bookmark proxy**: proposed not ported (wash on the collapsed
   folder instead).
5. **Group headers in comfortable layout**: today none; the model emits
   them regardless and the horizontal layout may draw a group pill later.
