# Chromium popover windows on the hosted Windows shell

Research note, 2026-09-16. What Chromium's own floating surfaces — the
extension action popup first, then the bubble and dialog families around
it — do when the browser is hosted inside `phi_client.exe`'s shell, how the
Mac client copes with the same surfaces, and what is still misaligned.
Companion to the extension-button port recorded in the fork's
`windows-framework-status.md` (2026-09-16 entries).

## 1. The two hosting models

| | Mac (`IS_MAC_PHI`) | Windows (`IS_PHI_ABI_CLIENT`) |
|---|---|---|
| Chromium's window | The real NSWindow. The Mac client swaps its `contentView`; Chromium's BrowserView (tab strip, toolbar, `ExtensionsToolbarDesktop`, location bar) keeps existing in a hidden views tree, its compositor output suppressed (`BrowserNativeWidgetMac::ShouldHideCompositorOutput`). | A child HWND of the client's shell window. `WindowFeatureController::NormalBrowserSupportsWindowFeature` answers **false** for toolbar, location bar and bookmark bar, so there is no `ExtensionsToolbarDesktop`, no `ToolbarActionView`, no location-bar icons at all. Only the tab strip stays *model*-supported. |
| What a views bubble is anchored to | A real (hidden) view: the reference `ToolbarActionView`, the location-bar icon, the extensions button. Positions are meaningless (the window top), so the fork re-anchors case by case. | Nothing but the `BrowserView` itself. Any bubble that asks the toolbar for an anchor gets the whole content area or a null view. |
| Bubble parent / owner | The NSWindow. Clicking anywhere in the Mac window activates the views widget tree. | The BrowserView widget; Windows resolves the owner to the shell top-level, so bubbles stack above the shell. Clicking the client's own chrome (sidebar, caption, address field) activates the shell HWND, which is **not** a views widget. |

Everything below follows from the last row.

## 2. The extension action popup (`ExtensionPopup`)

Trigger path on Windows: client `TriggerExtensionAction(window, id,
anchor px)` → framework converts px→DIP → `ExtensionsProxy::TriggerAction`
stores the rect (`StoreExtensionPopupAnchor`) → the `IS_PHI_ABI_CLIENT`
branch builds the `ExtensionViewHost` itself and calls
`ExtensionPopup::ShowPopup(browser, host, views::BubbleAnchor(browser_view),
TOP_LEFT, …)` → the popup's constructor takes the stored rect
(`TakeExtensionPopupAnchor`) and `GetAnchorRect()` returns it, so bubble
placement (arrow, mirroring, display choice) runs against the icon.

Verified on the box (125 % DPI): the popup drops down from the clicked
icon in all three layouts. Four things are wrong or fragile:

1. **Size collapses to the strip under the content.** `ExtensionViewViews::
   Init` calls `EnableSizingFromWebContents(GetMinBounds(), GetMaxBounds())`
   once, and `ExtensionPopup::GetMaxBounds()` measures free space with
   `BubbleDialogDelegate::GetMaxAvailableScreenSpaceToPlaceBubble(GetAnchor(),
   …)`, which reads the anchor **view's** screen bounds — on Windows the
   BrowserView, whose bottom edge is the window's bottom edge. The space
   "below the anchor" is the few pixels between the content and the work
   area, so `max_size.height` is tiny; the renderer's auto-resize is clamped
   to it and the popup shows as a short strip with a scrollbar (Obsidian Web
   Clipper: 365×74 DIP; the probe popup: ~300×80). The Mac does not hit this
   because its anchor is the hidden `ToolbarActionView` near the window top.
   `GetAvailableSpaceToPlaceBubble` is file-local in views, so the fix is a
   Phi override of `GetMaxBounds()` that measures from `phi_anchor_rect_`
   (work area below/above the icon rect, mirrored) when the rect is set.
2. **It ignores the client's chrome.** Upstream closes the popup in
   `OnWidgetTreeActivated` when *another widget in the anchor widget tree*
   activates — never on plain deactivation, so a user may keep the popup
   open while working in another app. Clicking the Phi sidebar, caption or
   address field activates the shell HWND, no views widget → nothing fires
   → the popup stays up (verified: sidebar click, popup still listed).
   Clicking the page does close it (the WebContents' widget activates).
   On the Mac every click in the window activates the views tree. Fix
   candidates: (a) the client tells the framework on every chrome click /
   shell activation (`CloseExtensionPopup(window)`); the proxy keeps the
   `ExtensionViewHost*` it created (cleared through
   `ExtensionHostRegistry::Observer::OnExtensionHostDestroyed`) and calls
   `ExtensionHost::Close()`, which runs the popup's close handler; or (b) a
   Phi override in `ExtensionPopup` that also closes when its own widget
   deactivates to a window in the shell's owner chain.
3. **It does not follow the window.** The anchor is a fixed screen rect.
   `BubbleDialogDelegate` repositions on anchor-widget bounds changes, but
   through `GetAnchorRect()` → the stale rect, so after
   `agentSpace.window.setBounds` the shell moved and the popup stayed at
   223,105 (verified). Upstream's comment accepts this ("moving the window
   typically results in the popup closing") because upstream's anchor is a
   view that moves with the window. The Mac has the same latent gap. Fix:
   store the anchor as an offset from the BrowserView's origin and rebuild
   the screen rect in `GetAnchorRect()`, or close on shell move.
4. **Arrow side.** Upstream and the Mac show action popups with
   `BubbleBorder::TOP_RIGHT` (right edge of the popup on the right edge of
   the icon, mirrored to TOP_LEFT in RTL); the Windows branch passes
   `TOP_LEFT`, so the popup hangs to the right of the icon instead of
   ending at it. One-token change in `ExtensionsProxy::TriggerAction`.

Also noted: `set_close_on_deactivate(false)` and the DevTools / JS-dialog
guards are upstream and fine; `host_->host_contents()->Focus()` moves
keyboard focus into the popup, and closing it returns focus to whatever
Windows activates next (the shell), which is acceptable.

## 3. Extension dialogs (`extension_dialog_utils_views.cc`)

Post-install notice, uninstall confirmation, MV2 deprecation notices,
"install friction". Upstream: a `BubbleDialogModelHost` off the
`ExtensionsToolbarDesktop`, else `constrained_window::ShowBrowserModal`.
Windows always took the modal fallback; the post-install notice (no
buttons, closes on deactivation as a bubble) became a modal nothing could
dismiss — the 2026-09-16 blocker. Fixed in the fork: the no-container branch
shows the same bubble anchored, in order, at the client's context-menu
point (`phi_extension_dialog_anchor`, promotion widened to Windows), at the
client's puzzle button (`PhiFrameworkDelegate::OnExtensionsButtonRectRequested`
through `phi_framework_hooks::QueryExtensionsButtonRect`), or at
`phi::BubbleAnchorRectForWebContents` (the page's inner top corner).
Box-verified with the uninstall confirmation.

## 4. Context menus (`ExtensionContextMenuModel`)

Windows runs the model through a `views::MenuRunner` parented to the
BrowserView widget (`ExtensionsProxy::ShowContextMenu`), because the Mac's
route (`ExtensionsToolbarDesktop::ShowContextMenuAsFallback` →
`ToolbarActionView`) needs the hidden toolbar. Menus are their own
top-level widgets with mouse capture, so the shell's HWND does not disturb
them; verified at 125 % DPI with the px→DIP conversion.

## 5. Other Chromium surfaces on the hosted shell (code reading)

| Surface | Mac fork | Windows today |
|---|---|---|
| Permission prompt (`PermissionPromptBubbleBaseView`) | `IS_PHI_BROWSER`: `GetBubbleBounds` pins the bubble to the page's top-left + 6 pt; `PermissionRequestManager` defers across the Mac's view switch. | Same `IS_PHI_BROWSER` positioning applies. Not box-checked. |
| Translate bubble, content-setting bubbles | `IS_MAC_PHI`: `phi::BubbleAnchorRectForWebContents` (page's inner top corner, `TOP_RIGHT`). | **Mac-only gate.** Falls back to `toolbar_button_provider()->GetBubbleAnchor(...)` on a toolbar that is never laid out — anchor at the window top, not the page corner. Candidate: widen those two gates to `IS_PHI_HOSTED`. |
| Find bar (`FindBarHost`) | `IS_MAC_PHI`: widget `Activatable::kYes` and explicit `Activate()` so the child NSWindow takes key input. | The client's Edit ▸ Find posts `IDC_FIND` through `chrome::ExecuteCommand`, but on the box **no find-bar widget appeared** after `kCmdFind` (the only shell-owned widget listed was the open extension popup). Upstream's `FindBarHost` positions itself from `BrowserView::GetFindBarBoundingBox`, which leans on the toolbar; whether the command is refused or the widget is laid out empty needs its own pass, together with keyboard focus. |
| Web-modal dialogs (`constrained_window::ShowWebModal`) | Upstream. | Upstream; centred over the WebContents — fine. |
| Browser-modal dialogs | Upstream, centred on the NSWindow. | Centred on the BrowserView; blocks the hosted child only — the client's chrome stays live, which upstream never intended. Avoid for anything the user must dismiss (see §3). |
| JavaScript dialogs | Bridge. | The framework answers them through the DevTools-shaped API; the visible dialog is upstream's. |
| Location-bar page actions (AI mode, etc.) | `IS_MAC_PHI` short-circuits their icon computation (use-after-free hazard, nothing reads them). | The views exist but are never laid out; nothing has crashed, not audited. |

## 6. Recommended order

1. `ExtensionPopup::GetMaxBounds()` from `phi_anchor_rect_` (§2.1) — every
   popup is visibly wrong today. **Done 2026-09-16** (the fork's
   `IS_PHI_ABI_CLIENT` branch measures from the icon rect). One trap: the
   constructor used to take the stored rect at its END, after
   `extension_view_->Init()` had already called `GetMaxBounds()` once for
   `EnableSizingFromWebContents` — the override saw no rect and the popup
   stayed 74 DIP tall. The take now precedes the view's Init(). Box:
   Obsidian Web Clipper 365×74 → 365×554 DIP.
2. `TOP_RIGHT` in the Windows `TriggerAction` branch (§2.4). **Done
   2026-09-16.**
3. Close the popup on client chrome activity (§2.2). **Done 2026-09-16**:
   `PhiFramework::CloseExtensionPopup(window)` → `CloseExtensionActionPopups`
   finds live `ExtensionPopup` instances by Browser identity and applies the
   same guarded dismissal used for widget activation. The shell
   calls it from every chrome press — left, right and middle clicks in
   `OnChromeClick` / `OnChromeContextMenu` / `OnChromeMiddleClick` — and from
   the address field's own `WM_LBUTTONDOWN` (a child EDIT the shell's handler
   never sees) through `PhiOmniboxShell::OmniboxFieldPressed`. Box: sidebar
   click and address-field click each dismiss an open Obsidian popup. The
   2026-09-29 client also dismisses on non-client presses, sidebar-resize and
   group-header double-click paths, all address-field button presses and its
   context menu, child-window presses (native pages and their text fields),
   and shell deactivation to another top-level window. Activation of the popup
   itself remains exempt. Moving or resizing the shell dismisses the
   fixed-anchor popup (§2.3).
4. Widen the translate / content-setting bubble anchors to `IS_PHI_HOSTED`
   (§5) and box-check the permission prompt.
5. Find bar placement and focus (§5).

## 7. Box rig for this note

Probe extensions `C:\Users\<you>\phitest\ext-badge` (badge "12", popup) and
`ext-gray` (`chrome.action.disable()`), task `PhiFxLaunchExt2`; PhiSkillCap
`list Chrome_WidgetWin_1` prints every Chromium widget with its owner and
rect (a bubble/popup is `owner=<shell hwnd>`); `phitest\bounds.mjs` moves
the shell through the control pipe; PhiNtpCap `precmd.txt` `cmd 1028`
fires the client's Find command.
