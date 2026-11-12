# Split view chrome (flush panes inside the content card)

Since 2026-10-13 a split follows the Figma design (node 9044:26952): no
pane cards. The panes sit **flush** in the content card, under the
address row's separator — no inset, no per-pane border, radius or shadow,
no gap but the divider. The divider is a 1 DIP hairline, the panes flush
against it (its grab reaches 2 DIP into each pane); the focused pane wears a 2 DIP accent ring **inside** its bounds
(over the page edge), the other pane no edge at all. Snap points at 1/3,
1/2 and 2/3, no mini toolbars, no attention dimming. (It replaced the
2026-09-16 port of the Mac's `SplitPaneHostView` pane cards and the
§4.6 "row becomes a sheet" look of `windows-native-ui.md`.)

A pane's corners are square except where it meets the content card's
rounded outer corner: side by side, the left pane's lower-left and the
right pane's lower-right; stacked, the lower pane's two lower corners.
The lower-left is the card's corner against the sidebar (`kContentRadius`
8, the same corner the single page's clip rounds); the lower-right is the
shell window's own corner, which Windows 11 rounds at 8 only while the
window is restored — maximized it is square. Fullscreen squares all.
The design's card radius is 6; the Windows card is 8 (it follows the
window's corner), and the panes follow the card.

## Three and four panes (2026-11-10)

A split is one row or one column of two to four panes, each with its own
share of the split axis (the Mac's `b3a379e3`). Everything above holds for
every pane: flush, a 1 DIP hairline between each two neighbours, the ring
on the focused pane only. Only the first and last pane meet the card, so a
middle pane is square all round.

* **Layout.** Chromium's `MultiContentsView` lays out a pair as before.
  For three or four panes the fork lays them out itself under
  `IS_WIN_PHI_CLIENT` (`PhiPaneSizes`): each pane takes its share of the
  axis less the dividers, the last takes the remainder, and no pane goes
  below Chromium's minimum (the lesser of 200 DIP and a tenth of the axis,
  never under 50). The two extra dividers are `MultiContentsResizeArea`s
  created hidden with the view.
* **Divider drag.** Divider *i* changes panes *i* and *i + 1* only. The
  1/3, 1/2 and 2/3 snap points apply inside those two panes, and each stays
  at a tenth of the whole axis or more. In a small window every position is
  within 18 DIP of a snap point, so the divider moves in steps.
* **Framework calls.** A wider split starts as a pair and grows:
  `AddTabToSplitById`, `RemoveTabFromSplitById`, `MoveTabInSplitById` and
  `UpdateSplitFractions`. The snapshot's `SplitSnapshot::tabs` and
  `fractions` carry every pane; `primary`, `secondary` and `ratio` are the
  first two panes and the first share. `kPhiFrameworkAbiVersion` was 2 with
  these calls.
* **Sidebar and strip.** One merged row with an equal segment per pane; a
  click on a segment focuses that pane. In the horizontal strip a split of
  three or four takes two tabs' room.
* **Ways in.** "Split with Current" and Alt-click add the row as the pane
  after the focused one while the focused split has fewer than four panes.
  "Split with New Tab" does the same with a new-tab page. A tab dropped on
  the page inserts at the leading or trailing quarter of a pane (and on a
  divider) while the split has room, and replaces the pane under the
  pointer otherwise; a split of four only replaces.
* **Per-pane actions.** The row menu of a split of three or four has Copy
  Pane *n* URL, Move Focused Pane Left / Right (Up / Down when stacked),
  which move the focused pane wherever the menu opened and are greyed when
  the focus is outside the split, Remove from Split, which dissolves the
  whole split, and Close Split. The hover chip's menu has the same move
  rows with the same rule, and its Remove from split dissolves the split
  too.
* **Stored splits** (a pinned cell or a bookmark stands for the split; its
  panes carry the cell's tag). Their panes stay as saved: move, swap and
  Remove from Split are greyed in the menus and refused by the commands
  (`stored_split_row` in `phi_coordinator.cc`), and there is no move grip.
  Two things do change what the cell saves. Edit URL in the cell's menu
  opens a name and an address per pane (`PhiPromptPanes`,
  `SetStoredSplitPanes`); an open pane whose address changed loads it.
  A tab
  dropped onto a pane of a bookmark-bound split is saved as that pane
  (`NoteStoredPaneReplaced`); a pinned split takes no drop.
  Ctrl+W on any pane of a pinned or bookmark-bound split closes the whole
  split at once (`CloseStoredSplitOfTab`, decided 2026-11-12; the Mac's
  Cmd+W closes only the focused pane); the cell keeps every pane and reads
  as not open. An ordinary split still loses only the focused pane.
  Closing just one pane (the chip's Close pane) does NOT change the cell: it keeps every pane it saved, and the panes
  that stay are let go of (`NoteStoredPaneClosing` clears their tags), so
  what is left of the split becomes a row of the ordinary tab list and the
  cell reads as not open. Decided 2026-11-11; the Mac drops the closed pane
  from the cell instead. Opening the cell again brings back all its panes.
* **The move grip** (the Mac's `PaneMoveHandle`, 2026-11-11). A pill at the
  top of the pane under the pointer, shown while the pointer is in the top
  44 DIP of a pane. Dragging it along the split moves the pane to the
  nearest slot (the slot is washed with the accent, and a card with the
  pane's icon and title rides the pointer; a pair swaps). Pulling it 120 DIP
  off the split's axis, or 40 DIP outside the split, takes the pane out on
  release. Reaching the tab list (the sidebar, or the strip in the
  horizontal layout) takes the pane out at once and the drag carries on as
  that tab's, so it lands anywhere a dragged tab can. Esc cancels. A split a
  pinned cell or a bookmark stands for has no grip: every pane there carries
  its cell's tag, and `SplitAllowsPaneMove` refuses a tagged pane. The page
  panes are hosted windows, so they do not slide under the drag as the
  Mac's do. Code: `PhiShellWindow::UpdatePaneGrip` and the `PaneDrag`
  members in `phi_shell_window.cc`.
* **Pinned and bookmarked.** A split of three or four pins, bookmarks and
  duplicates like a pair. The cell stays one row in the store: the first
  two panes as before, the later ones as JSON in a new optional column,
  `splitExtraPanes` (`[{"url","title","favicon","faviconWidth",
  "faviconHeight"}]`). For a bookmark that is the Mac's V14 column; a
  pinned split keeps its later panes there too, on its first row, where the
  Mac links N rows by `splitPaneGuids`. The column is local on both
  platforms: sync and export carry the first two panes. The store format
  stays 13, so an older build still opens the store and shows such a cell
  as a pair; if it rewrites the row the later panes are lost. Live panes
  bind by tag: `<guid>`, `<guid>#b`, `<guid>#c`, `<guid>#d`. A pinned cell
  draws its favicons at the Mac's sizes (16, 14 or 12 dip for two, three or
  four panes); a bookmark row draws one favicon per pane and joins the
  titles.
* **Not built yet.** The hover preview card and the drag card show the
  first two panes. A stored cell's own menu still names only the left and
  right pane's URL, and Reverse Panes on a stored cell of three or four
  swaps the first two stored panes only. The bookmark manager and the
  Library list the first two panes.

Checked on nuc at 300 % on 2026-11-10 with a 17-case end-to-end run over
the control pipe and real input; the suite was not kept.

## Who draws what

Chromium's `MultiContentsView` still lays the panes out and paints the
chrome — the Windows client drives the native side-by-side through
`PhiFramework` and reads geometry back (`TabContentBoundsById`,
`OnContentBoundsChanged`). The fork restyles that chrome under
`IS_WIN_PHI_CLIENT`, its numbers in one Phi-owned header,
`chrome/browser/ui/views/frame/phi_split_view_style.h`:

| Design | Windows |
| --- | --- |
| panes flush, inset 0 | `kPhiSplitPaneInset` 0; top from the pushed style (0) |
| no per-pane border | split border insets 0 (`kSplitViewContentPadding` = −outline thickness): the page fills its pane |
| divider 1 px, rgba(0,0,0,.04) over #FAFAFA | resize area `kPhiSplitDivider` 1 DIP (handle never shown); the ground paints it in `divider_ground_argb` with the `kPhiSplitHairline` 1 DIP line (whole device pixels) over it in `border_argb` |
| focus ring 2 px inside the active pane | `kPhiSplitFocusRing` 2, stroked inside the pane's bounds on the outline's layer (over the page); nothing on the inactive pane |
| corners: square but at the card's | `PhiSplitPaneCorners` (`kPhiSplitPaneRadius` 8) clips the page and shapes the ring |
| snap (unspecified) | `snap_points_` 1/3, 1/2, 2/3 + `kSnapDistance` 18 |
| no mini toolbar, no dimming | mini toolbar hidden, no `SplitTabHighlightController` |

Until 2026-10-14 the ground only started watching the panes on its first
paint while split, and forming a split does not resize the ground, so the
gap kept the unsplit paint — the translucent tint — and showed the shell's
backdrop. It now watches them (bounds and visibility) from its first paint.

The divider is 1 DIP wide but grabs `kPhiSplitDividerGrab` (2 DIP) into
each pane (2026-10-14; it was a 4 DIP strip whose #FAFAFA showed either
side of the hairline). A view over the page cannot take the mouse — the
page's aura window is hit first — so `PhiInstallSplitDividerTargeting`
installs two targeters: an `aura::WindowTargeter` on the browser widget's
window makes the panes' page windows (the native views of the
MultiContentsView's `NativeViewHost`s — they carry no `kHostViewKey`, their
layer is views-managed) decline the band, and a `views::ViewTargeter` on the
MultiContentsView hands the band to its resize area (the end pane sits
above it in z-order). The 2 DIP match the inset of a pane-hosted native
NTP, which is a client HWND the framework cannot target through.

Colours are the client's: `PhiShellWindow::PushSplitViewStyle` sends
`PhiFramework::SetSplitViewStyle(window_id, {background, border, focus,
top_inset, divider_ground})` for every hosted browser on its own Space's
settled palette, from `LayoutContents` and `ApplyPalette`, only when a
value moved. Until the first push Chromium paints its own colours.

| Field | Light | Dark (not in the design: derived) |
| --- | --- | --- |
| `background_argb` — outside a pane's rounded corner | Space tint (`ground_top`, or its backdrop colour) | same |
| `divider_ground_argb` — the drag strip | #FAFAFA (fixed neutral) | `ground_top`, opaque |
| `border_argb` — the hairline, blended over the strip | black at 4 % (0x0A000000) | white at 6 % (0x0FFFFFFF) |
| `focus_argb` — the ring | the Space accent (Pure: #3AA4D5, the design's) | the Space accent (Pure: #1E7099) |
| `top_inset` | 0 | 0 |

A contrast theme keeps its own edges: strip `ground_top`, hairline
`border`. `divider_ground_argb` was appended to the API struct on
2026-10-13 (a client/framework ABI change: ship both together).

The shell paints the card the same split or not: its left frame and
sheet shadow, the address row's rounded top-left and its separator, and
(without the backdrop) the opaque corner caps — the panes meet the
card's corners as a single page does.

Surfaces over a pane — the pane-hosted new-tab page
(`PhiNewTabPage::Reposition`) and a pane reader
(`PhiReaderPanel::Reposition`) — sit 2 dip inside it so the ring shows,
and round only the corners the pane shares with the card
(`PhiShellWindow::SplitPaneCardCorners`, the framework's rule read
geometrically) at the card's radius less the inset, 6, concentric with
it. The unfocused pane's hover chip is placed from the real pane rect
(8 dip in from its top-right), so the flush panes needed no offset change.

When the Windows backdrop is available, the ground uses the sidebar's
theme-preserving tint, converted from premultiplied DIB pixels to straight
ARGB for Skia (`PhiBackdropColor`). The shell extends Acrylic under the
whole content area and leaves its backing pixels clear; Chromium paints the tint
once, with the page surfaces remaining opaque. Split activation/dismissal,
sidebar collapse and system backdrop changes refresh this contract.
Without the backdrop, the pushed ground stays fully opaque.

The framework must preserve alpha at every layer: the normal browser
widget requests native translucency, its desktop host keeps full DWM
frame margins and a transparent compositor root, and the split background
layer advertises the pushed color's actual opacity. The hosted browser
does not paint Chromium's hidden caption beneath the top gutter. These
changes live in `browser_widget.cc`, `browser_desktop_window_tree_host_win.cc`,
`browser_frame_view_win.cc` and `multi_contents_background_view.cc` in the
Chromium fork; deployment needs both the matching client and framework.

## Drop cards

`phi_split_drop.cc`'s replace-mode cards trace the panes Chromium actually
laid out: `PhiSplitDropHints` carries the two pane rects from
`PhiSidebarShell::SidebarSplitPaneRects` (the framework's
`TabContentBoundsById`, mapped into the shell). The ratio estimate (flush
panes + the 1 DIP divider over the page area) only stands in before
the first layout.

## Model notifications

A layout flip or a settled divider drag now marks the window snapshot
(`WindowChangeReason::kSplits`): `PhiBridgeWrapper::OnSplitVisualsChanged`
was a no-op on Windows before, so the sidebar's "Convert to … Split" title
and the drop cards could read a stale layout or ratio. Mid-drag ratio steps
(`is_intermediate`) stay in the view.

## Checking it on the box

`test/probes/sv-split-look.mjs` (run from `phitest\probes`): `form
[vertical|horizontal]`, `ratio <r>`, `layout <axis>`, `active
<primary|secondary>`, `mode <layout mode>`, `appearance <light|dark>`,
`theme <id>`, `unsplit`, `state`. Shoot with PhiRm (a page click moves the
ring), drag the divider or a strip tab with PhiDragCap.

Transparency verification uses desktop captures, because `PrintWindow`
does not include the composed Windows backdrop. Put a black, then white,
window behind Phi: a split pane's rounded corner should show the same
backdrop as the sidebar, the divider stays opaque (#FAFAFA + 4 % ink in light)
and opaque page pixels must remain unchanged. Check both split axes and a hidden sidebar.
The host layout test checks that Skia's straight-alpha tint reconstructs
the same premultiplied color as the sidebar in light and dark appearances.

Rounded corners use the same ground in normal tabs and splits. A normal
page is clipped at its lower-left corner by Chromium's compositor; the
background view stays visible behind it. Native new-tab and bookmark
overlays use a matching window region. Opaque corner-cap windows are
removed while the backdrop is active. The shell preserves alpha outside
the address row's rounded upper corner, while
known card coverage keeps the row itself opaque even when its RGB matches
the ground. Live palette changes therefore recolor the exposed ground
without waiting for a resize. Fullscreen removes the single-page clip.
Ordinary native surfaces use an opaque 32-bit GDI buffer (`PhiWindow`)
so extending glass beneath them does not make the new-tab/bookmark page
itself translucent. Shaped panels keep their existing per-pixel alpha path.

Include the bottom-left page corner and the address row's upper-left
corner in desktop pixel probes. In a split, also check the left pane's
lower-left and the right pane's lower-right (restored and maximized), the
divider strip and its hairline, and the ring's corners. Change theme
without relayout and check an ordinary web page and a native new-tab page.
