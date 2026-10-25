# Analytics

Last updated: 2026-10-16 (Mac parity as of 2026-10-16: agent heartbeats and foreground edges no longer counted, `space_created.surface`, the kiosk-click and auto-PiP snapshot keys, `notification_card_action`)

The Windows client reports product analytics to PostHog Cloud (US), project
**phibrowser-win** (id 604357, <https://us.posthog.com/project/604357>). The
Mac app reports to its own project; the two are not joined, but every event
below carries the Mac's name and property keys so one insight definition can
be pasted across.

## Pipeline

```
capture site ──▶ PhiCoordinator::CaptureAnalytics (client events)
                 PhiCoordinator::RecordAnalyticsEvent (framework events)
                   │  the harness ring (agentSpace.analytics.list)
                   ▼
                 PhiTelemetry::RecordEvent
                   │  consent gate: Settings ▸ General ▸ "Share anonymous
                   │  usage analytics" (PhiSettings.json analyticsEnabled)
                   │  allow-list: PhiTelemetry::AllowedProperties, per module
                   ▼
                 spool (phi_telemetry_spool.json) ──▶ POST <endpoint>/batch/
```

The destination is deployment configuration, `phi_telemetry.json` in the
user data directory (`analytics_endpoint`, `analytics_key`,
`analytics_format: "posthog"`), read once at startup. Without it the client
spools nothing and sends nothing. Uploads go in batches of up to 64, after
30 s idle; a failed batch is retried with backoff, a 4xx is dropped.

Consent gates the uploader as well as the capture: nothing leaves while the
switch is off, whatever the queue holds. Flipping it off gets one final pass
(what was queued under consent, the `metrics_reporting_changed` event last;
no retry), after which the queue and `phi_telemetry_spool.json` are purged;
a launch that finds the switch off discards, unsent, whatever spool the
previous run left. The spool file exists only under consent.

Every event carries, stamped at send time (`PhiTelemetry::SenderProperties`):
`$lib` = `phibrowser-win`, `$lib_version` / `$app_version` (the client
version), `$app_build`, `$app_namespace` (`com.phibrowser.win`, with the
channel spelled in for canary/debug builds, `com.phibrowser.canary.win`),
`$os` = `Windows`, `$os_version`, `$device_type` = `Desktop`, plus
`distinct_id` and `phi_module`.

## Identity

`distinct_id` is a random GUID generated once per user data directory
(`phi_telemetry_id`). Nothing ties it to an account; there is no `identify`
on Windows yet.

## Privacy contract

Properties never carry site-level or user-content data — no URLs, page
titles, search terms, Space names, profile names, agent process paths. The
allow-list in `client/phi_telemetry.cc` is the enforcement: a property not
fenced there for its module is dropped at egress and counted
(`scrubbedProperties` in `agentSpace.telemetry.status`). Adding a property
is two edits, the capture site and the fence.

## Events

Client-originated events are built in `client/phi_client_analytics.cc`; the
module is the allow-list row.

| Event | Module | Properties | Trigger |
| --- | --- | --- | --- |
| `Application Installed` | `app` | `version`, `build` | First launch over a user data directory (no `phi_last_version` file) |
| `Application Updated` | `app` | `version`, `build`, `previous_version` | A launch whose version differs from the one recorded at the previous launch |
| `Application Opened` | `app` | `from_background` (always false), `version`, `layout_mode` (`performance` / `balanced` / `comfortable`), `ai_enabled` (always false today), `is_guest_mode` (Guest Mode — "Explore Phi without signing in") | Launch only (`PhiCoordinator::LoadStores`, once the settings and the consent are in). Until 2026-10-16 every `WM_ACTIVATEAPP` edge also sent an Opened (`from_background: true`) / `Application Backgrounded` pair — the Mac SDK's Cmd-Tab noise, which the Mac stopped counting (e748d9f4); neither is sent any more |
| `user_defaults_snapshot` | `settings` | `new_tab_behavior` (`ntp` / `omnibox`), `layout_mode`, `app_language`, `appearance` (`system` / `light` / `dark`), `is_phi_default_browser`, `peek_view_enabled`, `always_show_full_url`, `show_tab_previews`, `show_open_tab_indicators`, `dim_unloaded_tab_icons`, `restore_last_session_enabled`, `developer_mode_enabled`, `cdp_agent_access_enabled` (Developer ▸ "Allow agents to control Phi"), `open_kiosk_on_command_option_click` (the Mac's key; the Windows chord is Ctrl+Alt, pref `openKioskOnCtrlAltClick`), `auto_picture_in_picture_mode` (`off` / `normal` / `parked`, pref `autoPictureInPictureMode`), `agent_user_spaces_enabled`, `always_show_bookmark_bar` | Launch, right after `Application Opened`. The Mac's keys where the setting exists on Windows; the last two are Windows-only |
| `space_created` | `spaces` | `total_spaces`, `non_default_profile`, `surface` (`sidebar` — the strip's form and the pip flyout; `standalone_window` — the create-Space window; the Mac's `library` has no Windows host) | The user's own create form commits (`agentSpace.spaces.create` from an internal caller, `RequestCreateSpaceWithOptions` naming the surface); an agent's create is not counted |
| `space_switched` | `spaces` | `total_spaces` | The user switches Space in a shell window — a pip, the Space menu, Ctrl+N (`PhiShellWindow::SwitchToSpace`); not restores, not agent-driven switches |
| `agent_task_started` | `agent` | `origin` (always `phi_agent`), `persistent`, `agent_name` | An agent Space's window is up (`HostAgentWindow`), and a re-bind to an existing task |
| `agent_task_completed` | `agent` | `origin`, `persistent`, `agent_name`, `success` | `agentSpace.complete` — the completion path only; a Space the user closes under a task is not a completion |
| `agent_user_space_command` | `agent` | `command` (the verb name), `agent_name` | A pipe caller's verb passes the capability gate into the user's own Spaces: one aimed at a user window, or an app-wide one needing `kUserSpaceRead`/`kUserSpaceWrite`. Never for the test-rig host, and never for the reads and liveness probes the Mac exempts (`PhiAgentUserSpaceCommandIsTracked`: `agentSpace.bookmarks.list`, `pinnedTabs.list`, `shadow.list`, `shadow.ping`, `spaces.list`, `spaces.listTabs`, `urlRules.list`, `credentials.status`) — a CDP connection heartbeats with `spaces.list` every 30 s. `credentials.get` / `getTotp` stay counted |
| `agent_shadow_window_opened` | `agent` | `origin` (always `phi_agent`), `incognito` | A shadow window's browser is up (`HostAgentWindow` for `kShadow`) — a surface without a task |
| `agent_credential_access_requested` | `agent` | `kind` (`fill` / `reveal`; this client has no `run`), `approved`, `prompted`, `agent_name` | Every `credentials.get` / `credentials.autofill` decision (`PhiCredentialService::PromptCredentialApproval`): a remembered denial is `approved: false, prompted: false`; a live 10-minute grant `approved: true, prompted: false` |
| `agent_credential_access_approved` | `agent` | `kind`, `approval_type` (`existing_grant` / `once` / `ten_minutes`; there is no `always` here), `agent_name` | The allowed ones of the above. Never the scope, the purpose or anything from the vault |
| `space_deleted` | `spaces` | — | The user confirms a delete — the Spaces menu or Settings ▸ Spaces (`PhiCoordinator::DeleteSpace`, the hosts' override); an agent's `spaces.delete` goes to the manager directly and is not counted |
| `space_profile_changed` | `spaces` | `total_profiles` | A validated Space profile change begins (`PhiCoordinator::SetSpaceProfile` returning true) |
| `profile_created` | `profiles` | `total_profiles` | The framework names a created profile (`PhiProfileService::OnProfileCreated`), whoever asked — Settings' New Profile, the create-Space form, `profiles.create` |
| `peek_view_opened` | `peek` | `entry_point` (`auto` — the cross-site diversion, `explicit` — the page's "Open Link in Peek View"; never `restored`, a peek is omitted from session restore), `opener_kind` (`bookmark` / `pinned` / `normal`, what the opener tab is bound to in the sidebar tree) | The card presents (`PhiCoordinator::HostSurfacePanel`). The framework says which entry it was (`WindowCreation::peek_explicit`) |
| `peek_view_ended` | `peek` | `reason` (`closed` — the ✕ chip, Esc, a click outside, the sidebar's surface badge, Back/Forward or an address-bar navigation on the opener; `opened_as_tab` / `opened_as_split` — the other two chips; `opener_closed` — the opener left the strip; `replaced` — a second peek on the same opener; `page_closed` — the peek's own page closed itself), `duration_seconds` | The peek's browser closes (`PhiCoordinator::RetireClosingBrowser`), the closer having said why (`PhiSurfacePanel::NoteEndReason`). Window teardown and the app's exit report nothing |
| `peek_view_setting_changed` | `peek` | `enabled` | Settings ▸ Navigation ▸ "Enable Peek View" |
| `peek_view_auto_setting_changed` | `peek` | `enabled` | Settings ▸ Navigation ▸ "Automatically peek…" — its own toggle, and the follow-on flip the master toggle makes when it changes the value |
| `reader_view_opened` | `reader` | `entry_point` (`address_bar` — the in-field button, `view_menu`, `shortcut` — the View menu's chord (`WM_COMMAND` HIWORD 1), `context_menu` — the page's "Open in Reading Mode", `unknown`) | The extension's `reader.state` report says the surface mounted (`PhiReaderBridge`); the entry point is what the open request recorded |
| `reader_view_open_failed` | `reader` | `entry_point`, `reason` (the extension's token, e.g. `no_article_detected`; `transport_unavailable` when no extension answered the broadcast) | A refused open |
| `bookmark_manager_opened` | `bookmarks` | — | The native Bookmark Manager mounts over the content card (`PhiBookmarkManager::Show`) |
| `bookmark_manager_edited` | `bookmarks` | — | It unmounts after a new folder, a rename or a delete committed during that mount. Nothing about the bookmarks |
| `kiosk_opened` | `kiosk` | — | A Kiosk window is hosted (`registry_.AddKiosk`) |
| `kiosk_opened_in_space` | `kiosk` | `total_spaces` | The Kiosk toolbar's "Open in <Space>" (`PhiKioskWindow::OpenInSpace`) |
| `oobe_step_viewed` | `oobe` | `step` (`login` — the sign-in page and the embedded hosted login, `guest_privacy`, `set_name`, `set_theme`, `layout_selection`, `next_step`; `password_manager` (5) has no Windows page), `step_index` (the Mac's numbering), `is_guest` | The onboarding window shows a page (`PhiOnboardingWindow::ShowPage`); re-showing the same step is nothing |
| `oobe_step_completed` | `oobe` | `step`, `step_index`, `is_guest`, `duration_seconds` | The page's user action moved the flow forward (`AdvanceTo`). A step-back (Cancel, Esc, a refused sign-in) pauses the step without completing it |
| `oobe_finished` | `oobe` | `is_guest`, `steps_completed` (2 for a guest, else the last step's index), `total_duration_seconds` | "Let's Begin", a finished account's sign-in, or "Explore Phi without signing in" past the consents |
| `oobe_interrupted` | `oobe` | `last_step`, `step_index`, `is_guest`, `total_duration_seconds`, `reason` (`window_closed` / `app_terminated`) | The user closes the window before success (the caption's ✕, the harness's `close`), or Phi exits with it up (`FlushForShutdown`) |
| `guest_mode_entered` | `account` | — | The guest consents accepted (`OnboardingContinueAsGuest`) |
| `guest_mode_exited` | `account` | `trigger` (always `account_setting`) | A guest's sign-in from Settings ▸ You lands — at once for a finished account (`ReconcileAccessAfterAuth`), or when the rest of the onboarding finishes over the browser (`OnboardingFinished`) |
| `user_logged_in` | `account` | — | A sign-in the user started settles as a session (`PhiAccountAndFeedback::OnAuthChanged`); a background refresh or a restored session is not one |
| `user_logged_out` | `account` | — | Settings ▸ You ▸ Sign out (`RequestSignOut`), before the session clears |
| `login_retried` | `account` | — | "Sign in" pressed again on the onboarding's login page while a failed or abandoned attempt's note is showing |
| `metrics_reporting_changed` | `settings` | `enabled` | Settings ▸ General ▸ "Share anonymous usage analytics" flips. Captured BEFORE the switch moves, so an opt-out is the last event this install sends — in the uploader's final pass, after which nothing queued or spooled survives; an opt-in is dropped at the gate (the launch snapshot then says the new state) |
| `language_changed` | `settings` | `from`, `to` (the stored preference; `system` for follow-the-OS) | Settings ▸ General's language picker commits a different value |
| `feature_entry_tapped` | `feature` | `button` (only `download` exists here), `surface` (`sidebar` — the bottom bar; `web_content_header` — Comfortable's toolbar) | The downloads button's click (not the Window menu or Ctrl+J) |
| `import_viewed` | `import` | `entry_point` (always `menu`) | "Import from another browser…" opens the import window (not when it is only fronted) |
| `import_types_selected` | `import` | `source_browser` (`chrome`, `edge`, `brave`, `arc`, `file`), `types` (sorted, of `bookmarks`, `cookies`, `extensions`, `history`; `[]` for `file`) | Import is pressed: one event per configured source, in source-name order |
| `import_started` | `import` | `source_browsers` (sorted, unique) | The run is accepted |
| `import_finished` | `import` | `source_browsers`, `success`, `failed_sources`, `duration_seconds`, `error_code` (`file_unreadable`, `source_import_failed`, `bookmark_persistence_failed`; on failure only) | The last source completed and the bookmarks landed |
| `notification_card_action` | `notifications` | `action` (`show` — a card became the visible one, once per card, again after the stack was hidden and re-shown; `run` — its button; `dismiss` — the hide glyph; `delete` — the close glyph) | The agent notification cards (`PhiNotificationCards`, `notification.show`). A timeout or a queue eviction is nobody's action and is not counted |
| `first_time_action` | `app` | `action` (`space_created`, `agent_task`, `import_finished`), `seconds_since_install` | The action's first success over this user data directory. The record is `<user_data_dir>\phi_first_time_actions` (first line the install's unix ms, then one action per line); a profile from before the record counts from the first launch that wrote it |
| *Chromium-originated events* | `memory_saver`, `renderer_health` | see the registry | Captured in the browser core through `phi_analytics::Capture()`; inventoried in `chrome/browser/phinomenon/analytics/README.md` in the fork |

`agent_name` is the Mac's `AgentDriverBadge.telemetryName`: a known brand's
product label (`Claude Code`, `Codex`, `Pi`, …), `other` for a name the
badge resolver gives no mark to, `unknown` for none. The raw name is never
sent.

## Not ported

Mac events with no Windows counterpart, and why:

| Event | Why |
| --- | --- |
| `ai_sidebar_opened` / `ai_sidebar_closed`, `ai_features_toggled`, `connector_status`, `first_time_action` with `ai_sidebar_opened` / `memory_opened` / `connector_connected` / `phi_link_paired`, `feature_entry_tapped` with `chat` / `memory` / `organize_tabs` | No AI features in this client |
| `kiosk_profile_changed` | The Kiosk toolbar's profile chip offers no menu here (no framework call re-opens a Kiosk in another profile) |
| `browser_migration_finished` | No Arc / Zen migration wizard |
| `highlight_link_copied` | The copy callback reaches the Mac bridge only, and this client has no `shortHighlightLinksEnabled` setting |
| `scripting_command_invoked` | AppleScript |
| `onboarding_completed` | The Mac's legacy compatibility event; `oobe_*` is the funnel |
| `agent_credential_access_*` with `kind: run`, `approval_type: always` | This client has no command-env injection and offers no standing grant |
| `peek_view_opened` with `entry_point: restored` | A peek is omitted from session restore here |
| `reader_view_opened` with `entry_point: automation` | No `-phi-auto-reader` trigger |
| `library_section_viewed` | No native Library; Windows opens Mirage's `library.html` |
| `mini_player_action` | No Mini Player |
| `space_created` with `surface: library` | No Library card hosts the create form |
| `notification_card_action` with `popup` / `mute` | The Mac's sidebar notification presentation settings; the Windows cards have no such menu |
| `user_defaults_snapshot` keys `mini_player_enabled`, `mini_player_presentation_mode`, `short_highlight_links_enabled`, `bitwarden_enabled`, `proactive_suggestions_enabled`, `automatically_add_context_enabled`, `open_external_links_in_kiosk` | No Mini Player, short highlight links, Bitwarden, AI features or external-link Kiosk setting in this client |
| `user_defaults_snapshot` key `selection_tint_enabled` | Synced from the Mac (`PhiSelectionTintEnabled`) but no Windows UI or effect reads it |
| `user_defaults_snapshot` keys `default_browser_name`, `default_browser_bundle_id` | A bundle id is a Mac notion; `is_phi_default_browser` is the one default-browser fact reported |
| `user_defaults_snapshot` keys `block_ads_enabled`, `block_trackers_enabled`, `block_cookie_banners_enabled` | No profile-wide content blocking switches in this client (the site menu's per-site toggle is the only control) |

Add a row above in the same change that adds a capture site.

## Verifying on the box

```
node C:\Users\<you>\phitest\tele-status.mjs     # endpoint, sent, lastStatus
```

`agentSpace.analytics.list` over the control pipe returns everything
captured this session (properties included, pre-scrub) — a `--dev-hooks`
client with `testRig` only (`test_rig_only` otherwise).
`test/probes/analytics-verify.mjs` drives the 2026-10-16 parity through it
(a menu-created Space, a card's Run, a foreground round trip); PostHog's
Activity view for project 604357 shows what arrived. `/batch/` answers 200 even for
a wrong key, so the Activity view is the proof, not the status.
