# Native media controls: the sidebar Mini Player (Windows)

The Windows port of the Mac's sidebar Mini Player (`phibrowser-mac`
`docs/native-media-controls.md`, `SidebarMediaController.swift`,
`SidebarMediaPlayerView.swift`). GitHub phibrowser/phibrowser-win#12.

A card on top of the sidebar's bottom bar controls media playing in one of the
window's background tabs: play/pause, previous/next track, seek, volume and mute,
picture-in-picture, "Show playing tab", dismiss, and a sideways drag between
sources. Settings ▸ Advanced ▸ **Mini Player** offers Off, Auto, Always expanded
and Always compact, stored as the Mac's `sidebarMediaPlayerEnabled` and
`sidebarMediaPresentationMode` keys (not synced, as on the Mac).

## Framework (fork, ABI 6)

`chrome/browser/phinomenon/phi_framework/phi_media_controls.{h,cc}` is the C++
twin of the Mac bridge's `phi_app_bridge/phi_media_controls.mm`: one
`MediaTabWatch` (a WebContentsUserData that is a WebContentsObserver and a
`media_session::mojom::MediaSessionObserver`) per tab, the same snapshot keys,
the same token policy (`phi_app_bridge/media_control_policy.h`, shared) and the
same action checks. The content-layer pieces it reads (element mute, source
silence, element volume in `content/public/browser/media_session_volume.h`) are
the Mac team's `is_phi_browser` patches, already in the Windows build.

The Mac creates a controller per WebContentWrapper on request. Windows has no
per-tab object at the boundary, so the client turns observation on for every tab
at once:

| `PhiFramework` | |
|---|---|
| `SetMediaControlsEnabled(bool)` | On: every open tab is watched, and every tab created later (the `DispatchTabsChanged` hook). Off: every watch dropped, tokens invalid. Neither pauses or closes PiP. |
| `MediaSessionsJson()` | One object per watched tab with a controllable session: `windowId`, `tabId` and the Mac snapshot (`token`, `sourceToken`, `title`, `artist`, `playing`, `muted`, `sourceSilent`, `canSetMute`, `tabMuted`, `canSetVolume`, `volume`, `volumePending`, `canPlay`, `canPause`, `canSeek`, `currentTime`, `playbackRate`, `duration`, `canPreviousTrack`, `canNextTrack`, `canEnterPictureInPicture`, `canExitPictureInPicture`, `isPictureInPicture`). |
| `MediaSessionAction(tab, action, token, seconds)` | play, pause, seek, enter/exit PiP, previous/next track. Stale tokens and unsupported actions return false. |
| `ReadMediaSessionVolume`, `SetMediaSessionVolume`, `SetMediaSessionMuted` | The Mac's `readVolume`, `setVolume`, `setMuted`. |
| `PhiFrameworkDelegate::OnMediaSessionsChanged()` | Coalesced into one posted task for any number of tabs. |

## Client

`client/phi_mini_player.{h,cc}`: `PhiMiniPlayer`, one per shell window, shared by
the docked sidebar and the floating panel. The shell implements
`PhiMiniPlayerHost` (its Space browsers, the presented Space's visible tabs, tab
titles, relayout, repaint, capture, "show tab" through `ActivateSearchTab`).

- **Layout.** `ShellLayoutInputs::media_card_dip` reserves the band above the
  bottom bar in the vertical layouts (`ShellLayout::media_card`); the tab list
  ends above it. Heights are the Mac's: 38 compact, 124 expanded, +34 for the
  volume row, 5 dip above and below; changes animate over 200 ms.
- **Selection** follows the Mac's policy: background tabs of this window's
  Spaces only; the presented Space's active tab and its split panes count as
  visible and never show; PiP media is excluded; a muted, silent, tab-muted or
  zero-volume source cannot enter the card but keeps it once shown; a shown
  source stays while paused until its tab is visited, it is dismissed or another
  source replaces it; the order is the tabs' visit order. Dismissal is bound to
  the source token.
- **Track changes.** An accepted previous/next keeps the card (controls
  disabled) until the new track's state arrives, five seconds at most.
- **Input.** `OnChromeMouseMove` / `OnChromeClick` (shared by both surfaces)
  feed the card first; button-up and capture loss are routed in both window
  procedures. Alt-click on the speaker (the Mac's Option-click) toggles the
  player's mute; a click while muted unmutes.
- **Readout.** `agentSpace.window.layout` → `miniPlayer` (source, playback,
  part rects); `agentSpace.settings.set/get` take and report the two keys.

## Not ported

- Analytics: the Mac's `mini_player_action` event and the launch properties
  `mini_player_enabled` / `mini_player_presentation_mode` need the client
  collector's allow-list first.
- Keyboard focus and screen-reader actions on the card (the sidebar is
  painted, without focusable controls), and trackpad swipes (a mouse drag of
  40 dip cycles instead).
- The tab speaker indicator preferring MediaSession mute (Mac `4e6de510`).
- The Mac's 250 ms buffer that keeps a video leaving with its Space off the
  card while it enters auto-PiP (`55d1519a`).
- The Mac doc's per-WebContents "gain" multiplier: the r156 bridge, which this
  port follows, sets the element volume instead.
