# Picture-in-Picture extras: YouTube chapters, Skip Ad, auto-PiP

PHI-2252, from GitHub phibrowser/phibrowser-win#26 (parent PHI-2246). The
Mac lists these in its 2.3.0 and 2.19.0 release notes. This note records where
each piece lives, what Windows had on 2026-11-27, and what changed.

## Where the pieces live

| Feature | Mac client | Fork (Chromium) | Mirage |
|---|---|---|---|
| Chapter segments and the current chapter's title over the PiP progress bar | nothing | `chrome/browser/phinomenon/phi_pip_page_state.{h,cc}` keeps the tab's chapters; `chrome/browser/ui/views/overlay/video_overlay_window_views.cc` draws the segments (`PhiProgressView` boundaries) and the title label; `phinomenonPrivate.setVideoChapters` | `assets/youtube.ts-*.js` reads YouTube's chapters panel (`ytd-engagement-panel-section-list-renderer`, description or auto chapters) and the player's ld+json duration once a second, and reports them |
| Skip Ad button in the PiP window | nothing | `skip_ad_label_button.{h,cc}` and its layout in `video_overlay_window_views.cc` (upstream left it out, crbug.com/459517293); a press is recorded in `PhiPipPageState`, which then allows one trusted click on `https://www.youtube.com` within 2 s (`phinomenonPrivate.clickSkipAdButton`, chromium ADR 0022) | the same script handles the Media Session `skipad` action while `#movie_player.ad-showing` shows a visible `button.ytp-skip-ad-button`, and asks for the click at its centre; a breaker stops after 3 failed skips |
| Auto-PiP when the tab is left | three-state setting (`b15db5ba`), Mini Player Space-exit buffer (`55d1519a`), macOS-only full-screen desktop and menu rules (`62e549a0`, `f77afd55`) and focus fixes (`494dab6a` PHI-2144, `7bcb67e5` PHI-2226) | `AutoPictureInPictureTabHelper` with Phi's park preference; browser-initiated auto-PiP (pages with no `enterpictureinpicture` handler, such as YouTube) is on by default in r158 | — |

The fork's history on `phi-r158` is one squashed base commit, `126067d629f7`
(2026-11-20, whose subject is the last Skip Ad layout fix), so the original
commits are not in this checkout. Every fork file above is built for Windows:
the code is under `IS_PHI_BROWSER`, and the extension functions under
`IS_MAC_PHI || IS_PHI_ABI_CLIENT`. No ABI change is involved.

The YouTube script arrived with the Mirage 2.19.0 drop (fork `6c0a5dc93051`,
2026-11-24). Fork builds before it carry Mirage 2.17.0, which has the window
code but nothing to report chapters or handle `skipad`. Official 0.8.0.25
(framework `win-v0.8.0.25`, fork `fc1454fb`) has 2.19.0.

## Windows on 2026-11-27

- **Chapters and Skip Ad already work in 0.8.0.25.** Nothing in the Windows
  client is involved. `suite-pipchapters` passes on 0.8.0.25's framework and
  fails on the fork's ABI-11 snapshot `e837b182` (Mirage 2.17.0). Mirage
  changes belong to phibrowser/phi-ai, not the fork's `phi_mirage` copy.
- **Auto-PiP on leaving a tab, a Space, or a minimised or covered window**
  was already ported (`picture-in-picture.md`, Settings ▸ Advanced ▸ Auto
  picture-in-picture: Off / Normal / Park at edge). `suite-pip` covers a tab
  switch with a page handler; `suite-pipchapters` covers the YouTube case
  with no handler.
- **The Mac's Space-exit buffer (`55d1519a`) was missing.** When a Space
  switch hid a playing video, the sidebar Mini Player showed it as a
  background source at once. Auto-PiP opened ~300 ms later and the card went
  away again. On nuc the card flashed from 54 ms to 371 ms after the switch.

## The change (client)

`client/phi_mini_player.{h,cc}`, after the Mac's `bufferLeavingSpaceSources`:

- A Space switch is any `Refresh()` where the shell presents a different
  browser than before (`PhiMiniPlayerHost::MiniPlayerPresentedBrowser`, new;
  the shell answers `active_browser_id()`).
- Each source that was on screen before the switch and is now hidden, playing,
  able to enter PiP and not in PiP is held off the card for 250 ms
  (`kSpaceExitBufferMs`, the Mac's figure).
- A held source counts as not eligible. It is released once its time is up,
  or once it is back on screen, in PiP, or replaced by other media.
- When the wait ends, the player reads the framework's sessions again before
  releasing it. A PiP that started meanwhile keeps the source off the card,
  even if its event has not arrived (the Mac inspects before releasing).
- A later switch starts the wait again. Turning the Mini Player off forgets
  every held source.

A plain tab switch is not buffered, as on the Mac.

## Not ported or not checked

- `62e549a0` (PiP on other apps' full-screen desktops) and `f77afd55`
  (greyed menus while only a PiP is up) are macOS Spaces and menu bar
  behaviour: skipped.
- `494dab6a` (PHI-2144, focus after Cmd+Tab with only a PiP up): skipped,
  since Alt+Tab raises the window the user picks.
- `7bcb67e5` (PHI-2226, the first click after a PiP window only focuses the
  browser window): not checked on Windows. Windows delivers the activating
  click to Chromium windows by default (`MA_ACTIVATE`).
- Real YouTube was not used: its ads and markup change. The suite serves a
  page with the markup Mirage reads at the real origin, over CDP `Fetch`.

## Tests

phibrowser-win-test:

- `tests/suite-pipchapters.mjs` checks chapters, Skip Ad, and auto-PiP of
  the YouTube page. It uses the fixture `tests/fixtures/pipextras/video.webm`
  (60 s).
- `tests/suite-pipspaceexit.mjs` checks the Mini Player wait.
- `cu/scenarios/pip-extras.md` checks the same things on screenshots.

`phi-test.mjs` gained 64-bit WebSocket frames and `openCdpEvents` (CDP events,
for `Fetch`).
