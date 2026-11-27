# Page screenshots (Windows)

The Windows port of the Mac's page screenshots (phibrowser-mac
`docs/page-screenshots.md`; series `f581e793`, `fec4739a`…`c8c6ac28`,
rebinding `7e731204`). Linear PHI-2251, from GitHub phibrowser-win #26
via PHI-2246.

The ⋯ menu has **Capture Full Page** (Ctrl+Shift+1) and **Capture
Area…** (Ctrl+Shift+2), just above Print, where the Mac's File menu
has them. Both chords are rebindable rows in Settings ▸ Shortcuts
(`captureFullPage`, `captureArea`, group File). A capture is copied to
the clipboard as PNG, a toast offers Save for six seconds, and the
screenshot editor opens over the page.

## Where each piece lives

| Piece | Mac | Windows |
|---|---|---|
| CDP capture (`Page.captureScreenshot`) | `PageScreenshotService` over `AppDevToolsPageSession` | client `phi_page_screenshot.cc` over the framework's in-process DevTools transport (`OpenDevToolsSession`, ABI 11, already shipped) |
| Area selection overlay | `PageScreenshotRegionSelector` (AppKit overlay) | client `phi_screenshot_area.cc`, a layered child panel over the page |
| Editor | `ScreenshotEditorWindowController` + canvas | client `phi_screenshot_editor.cc`, a child panel over the content card |
| Annotation model, undo, crop, blur | `ScreenshotEditorModel`, `ScreenshotAnnotation` | client `phi_screenshot_model.{h,cc}` (pure, no Win32) |
| Rendering and export | CoreGraphics / ImageIO | client `phi_screenshot_render.{h,cc}`: Direct2D for preview and export, WIC for PNG / JPG, a small PDF writer over zlib |
| Menu rows and shortcuts | File menu, `Shortcuts+Custom.swift` | `phi_app_menu.cc`, `kRebindableShortcuts` in `phi_preferences.cc` |
| Strings | `Localizable.xcstrings` | the Mac keys (`browser.screenshot.*`, `app.fileMenu.*`, `settings.shortcuts.description.capture*`) in `strings-source.json`, translated by key |

**No fork change and no ABI bump.** The client already holds the
DevTools transport the control pipe relays through; a page-target session
is not subject to the session scope filter (that filters `Target.*` on
browser-wide sessions), so `Page.*` and `Runtime.evaluate` reach the page
as sent. **No Mirage change:** nothing here touches the Mirage extension.

## Capture

`PhiPageScreenshot` is one per process and runs one capture at a time
(the Mac's single-flight gate). The target is the page in front of the
user, `PhiShellWindow::PageCommandTab()` (a presented Peek's page, else
the active tab), snapped with its title and the shell when the command
runs; switching tabs while it runs does not move the capture. The rows
are disabled when there is no such tab or when a native page covers it
(the new-tab page, the Bookmarks page), as the Mac disables them on its
native new-tab page.

Steps, all on the UI thread except the first:

1. `DevToolsDiscoveryJson("/json/list")` on a worker thread (the call is
   refused on the UI thread), then `TabIdForCdpTarget` on each page entry
   to find the target behind the tab.
2. `OpenDevToolsSession(target)`.
3. `Page.getFrameTree` (top frame id, loader id, URL), then
   `Page.getLayoutMetrics` and `Runtime.evaluate("devicePixelRatio")`.
4. The clip is `cssContentSize` (full page) or the selection (area),
   rounded up. Its scale first fits `w×dpr` and `h×dpr` within 16,384 px
   per axis, then within 64 million pixels, with a one-pixel rounding
   margin; a page within both keeps scale 1 (`PhiScreenshotClipScale`).
5. `Page.captureScreenshot` with `format: "png"`, `fromSurface: true`,
   `captureBeyondViewport: true` and the clip.
6. `Page.getFrameTree` again: a changed top frame, loader or URL rejects
   the result (the page navigated under the capture).
7. Base64 decode and validate: PNG signature, IHDR dimensions at most
   32,768 per axis and 64 Mpx in total, IEND trailer, at most 30 MiB of
   base64 (`PhiScreenshotValidatePng`).
8. Clipboard: the PNG bytes under the registered `PNG` format plus a
   `CF_DIBV5` copy for programs that only read bitmaps. If the clipboard
   sequence number moved since step 1, the user copied something newer:
   it is kept and the toast says "Screenshot not copied because the
   clipboard changed". A failed write reports "Could not copy the
   screenshot".
9. Toast: "Full page screenshot copied" / "Area screenshot copied" with a
   Save action for six seconds. Save writes the captured PNG bytes
   unchanged, to a file named after the tab title (invalid characters
   replaced, empty → `Screenshot.png`), through the standard save dialog.
10. The editor opens over the shell.

The whole operation, selection included, has a 60-second deadline; each
CDP command answers within 15 seconds and the session open within 10,
both bounded by what is left. A late answer after the deadline is
dropped before anything reaches the clipboard. Failures show "Could not
take a screenshot of this page" or "Screenshot timed out. Please try
again." and leave the clipboard alone. The session is closed on every
path.

The in-process transport has no message size cap (the control pipe's
16 MiB WebSocket cap applies only to pipe clients), so the 30 MiB base64
limit is the only bound, as on the Mac.

## Area capture

Capture Area… lays a layered child panel over the page's viewport: the
split pane's rect when the tab is a pane on screen, else the content
card. It dims the page, shows the hint "Drag to select · Scroll or drag
to an edge to extend · Escape to cancel", and takes the focus.

The page is mapped with what the page reports, not with a DPI guess:
`Runtime.evaluate` returns `innerWidth`, `innerHeight`, `scrollX`,
`scrollY` and `devicePixelRatio`; a screen pixel maps to
`scroll + (px - viewport.left) × innerWidth / viewport.width` CSS pixels
(`PhiScreenshotViewportMap`). The anchor is kept in document coordinates,
so the selection can extend past the viewport in any direction.

While the button is down:

* the wheel scrolls the main document (`window.scrollBy`, then the real
  scroll offset is read back, never accumulated);
* holding the pointer within 32 DIP of an edge scrolls in that direction
  every 30 ms, faster nearer the edge; the wheel takes precedence while it
  is turning;
* release finishes a selection of at least 2 DIP on each axis.

Escape, a right click, or the overlay losing the focus or the mouse
capture (a click elsewhere, another window, the shell deactivating)
cancels the selection and leaves the clipboard alone; while the overlay
has the keyboard no tab or Space shortcut reaches the window. A viewport
resize or zoom found at the next scroll cancels it too. A navigation
during the selection is caught by the final frame check and fails the
capture ("Could not take a screenshot of this page"). Only the main
document scrolls; inner scrolling containers are not followed (as on the
Mac). The overlay is a sibling of the browser window, outside Chromium's
surface, so it never appears in the capture; after the capture the page
stays where it was scrolled to, as on the Mac.

The clip goes to `Page.captureScreenshot` as-is (no stitching), with the
same scale rules and checks as a full page.

## Editor

A child panel over the shell's content card (the group overview's
pattern: it follows the card through moves, resizes and layout changes,
and the shell's corner caps round it). It opens after a successful
capture, takes the focus, and returns it to the page when it closes.
Escape (after cancelling an edit in progress) and Ctrl+W close it; with
unsaved edits a prompt asks "Close without saving your edits?" (Keep
Editing / Close Without Saving; Escape keeps editing). Closing the browser
window closes its editor without asking, so unsaved edits go with it (the
Mac moves them to a standalone window; see "Not ported"). Re-activating
the window gives the keyboard back to the editor (or the area overlay),
not to the page under it. One editor exists at a time; a new capture
replaces a clean one and asks for a dirty one (the new capture is already
on the clipboard by then).

While the editor or the area overlay has the focus the client keyboard
hook lets every chord through to it (`PhiScreenshotOwnsKeyboard`), so
Ctrl+Z, Ctrl+W, Ctrl+D and the arrow keys are the editor's and not the
browser's (Ctrl+Shift+1 / 2 included: a new capture while the editor is up
comes from the ⋯ menu). The canvas and the overlay have no IME context
(`ImmAssociateContextEx(…, nullptr, 0)`), so the tool letters and Escape
work under a Chinese, Japanese or Korean IME, which would otherwise turn
them into VK_PROCESSKEY (the Mac's `7fe71d5c`); the text field keeps its
IME.

**Toolbar** (a floating capsule at the bottom centre, drawn with the
client's AA layer): Select, Arrow, Freehand, Text, Rectangle, Ellipse,
Blur, Crop | Undo, Redo | Export (PNG / JPG / PDF). A zoom pill sits at
the bottom right: −, the zoom level (click for Fit), +.

**Keys** (canvas focused): V Select, A Arrow, P Freehand, T Text,
R Rectangle, O Ellipse, M Blur, C Crop; Ctrl+Z undo, Ctrl+Shift+Z and
Ctrl+Y redo; Delete / Backspace delete the selection; Ctrl+D
duplicates; Ctrl+A selects all; the arrow keys move the selection one
source pixel (Shift: 10); Ctrl+Plus / Ctrl+Minus change the selection's
stroke width or text size by one; Ctrl+wheel zooms around the pointer;
Ctrl+0 fits. Shift held while finishing a shape keeps the tool armed;
otherwise drawing returns to Select (the Mac's default). Freehand stays
armed after drawing (Mac `6e399950`).

**Objects** stay editable: click selects (Shift adds), drag moves,
drag a handle resizes (eight for shapes, blur and freehand, two
endpoint handles for an arrow, left and right for text), marquee
drag on empty canvas selects a set. Right-click an object for Bring to
Front, Send to Back, Duplicate and Delete. Double-click text to edit it.

**Style strip.** Where the Mac anchors a property popover at the
selected object, Windows shows one strip above the toolbar while a
drawing tool is armed or a styled object is selected: the Mac's six
colour swatches, its five stroke widths (2.5, 4, 8, 16, 32) or text sizes
(15, 24, 44, 60, 80), and Fill (80 % of the colour, as on the Mac) for
rectangles and ellipses. It edits the selection (one undo step; text being
typed is committed first) and the defaults for the next object. Blur has
no style. The Mac's sizes are image pixels of a 2x Retina capture, so a
capture at a higher device scale multiplies them by dpr / 2
(`PhiScreenshotStyleUnit`; 1.5 at 300 %): the buttons keep the Mac's
numbers and the default text reads the same size on the page as on a
Mac.

**Text** is entered in a borderless multiline EDIT over the object, in
the object's colour and size; Ctrl+Return, a click elsewhere, a tool
change or export commits, Escape cancels. Height follows the content.

**Arrows** have the Mac's open head: two 45-degree wings
min(0.3 × length, 32) px long, round-joined at the tip; a stroke thicker
than the Mac's 2.5 grows the 32 in proportion (`PhiArrowHeadLength`).

**Freehand** keeps its samples and draws midpoint-joined quadratic
Béziers through them (the Mac's `715c8b67`); preview, hit test and
export share that geometry.

**Blur** covers its rectangle with opaque blocks of about 20 source
pixels sampled from the screenshot (not from annotations below it):
earlier annotations are covered, later ones stay above. It is an
obscuring effect, not a guarantee.

**Crop** (C) arms a rectangle on the image; drag inside it to move it,
its eight handles to resize. The toolbar shows the crop's size, Cancel
and Apply; Return applies, Escape cancels. Apply crops the source pixels,
moves every object by the crop origin, drops objects wholly outside and
returns to Select; it is one undo step that restores image and objects
together.

**Undo** keeps at most 100 transactions; each finished gesture, text
session, style change or command is one.

## Export

Export… opens a menu of PNG, JPG and PDF, then the save dialog. The
document is rendered at source size into a WIC bitmap through the same
Direct2D drawing code the canvas uses (zoom never changes exported
pixels), on a white backdrop. PNG is lossless; JPG quality is 0.9; PDF is
one page at one point per source pixel carrying the flattened picture as
a Flate-compressed RGB image. The Mac keeps vector annotations in PDF
unless blur or out-of-bounds content forces flattening; Windows always
flattens, which never leaks the pixels under a blur. A successful
export marks the document saved (a generation number guards against
edits made while the file was written); a failure keeps the document and
says "Could not save the screenshot". Rendering and encoding run on the UI
thread (the Mac encodes off the main actor): a page-long PNG holds the
window for about a second. The capture's PNG decode and clipboard copy run
there too.

## Test hooks

`--dev-hooks` builds only (`test_rig_only`, kHost):

| Verb | Does |
|---|---|
| `agentSpace.screenshot.capture {windowId?, mode, rect?}` | runs Capture Full Page (`mode: "full"`) or Capture Area (`"area"`); with `rect` (document CSS px) the area skips the overlay |
| `agentSpace.screenshot.state` | busy, the last result (ok, mode, error, image size, bytes, copied, clipboard), the overlay, and the editor (open, hwnd, tool, image size, objects, undo/redo depth, dirty, zoom) |
| `agentSpace.screenshot.export {format, path}` | exports the editor's document to `path` without the dialog |
| `agentSpace.screenshot.close {discard?}` | closes the editor (with `discard`, without the prompt) |

Tests: `phibrowser-win-test` `tests/suite-screenshot.mjs` (menu rows,
shortcut rows, Ctrl+Shift+1 and Ctrl+Shift+2 with real input, image size
against the page, clipboard, editor tools through real keys and drags,
undo / redo, crop, blur, exports) and `cu/scenarios/page-screenshots.md`.

## Not ported (yet)

* The object-anchored property popover (the style strip stands in) and
  the system colour picker (six swatches).
* Clipboard Cut / Copy / Paste of annotation objects.
* Option-mirrored resize, Shift / Ctrl marquee add / subtract, Space-drag
  pan (the scroll bars and the wheel pan instead).
* Autoscroll while dragging past the canvas edge in the editor.
* A standalone editor window that survives its browser window: closing
  the window closes the editor and drops unsaved edits.
* Export and capture decoding off the UI thread.
* The Shortcuts pane titles the rows with the menu strings, so the area
  row reads "Capture Area…" where the Mac's pane says "Capture Area".
