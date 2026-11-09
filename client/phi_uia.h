// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_UIA_H_
#define PHIBROWSER_WIN_CLIENT_PHI_UIA_H_

#include <string>
#include <vector>

#include "client/phi_window.h"

// THE accessibility provider (docs/architecture-review.md §3.6 and §4 item
// 31; appendix lane3-ui-substrate.md §2 "Cross-cutting absences", §6.1 last
// row, ranked finding 2). Before this file the client had zero
// accessibility: no WM_GETOBJECT, no UIA, no MSAA, no NotifyWinEvent
// anywhere, so the sidebar, the tab strip, every popover, Settings, the NTP
// and the bookmark manager were invisible to assistive technology — only
// the handful of raw EDIT/BUTTON/LISTBOX children the client creates showed
// up at all.
//
// It is nearly free because the substrate already keeps the data. Every
// PhiHitSource rebuilds `hits_` — a std::vector<PhiHit> of {bounds, key, name,
// role, selected, enabled} — from its layout pass, and hover and click
// routing read nothing else. This file turns THAT LIST, unchanged, into a
// UI Automation fragment root: one child fragment per hit, named by
// PhiHit::name, typed by PhiHit::role, positioned by PhiHit::bounds,
// invoked through the same messages a mouse click sends. A surface that
// wants to be accessible does not write a line of code here; it fills in
// hits_ (which it must do anyway to be clickable) and gets a tree.
//
// SHAPE
//
//   PhiHitSource's HWND  ── WM_GETOBJECT / UiaRootObjectId ──▶ fragment root
//     │                    IRawElementProviderSimple
//     │                    IRawElementProviderFragment
//     │                    IRawElementProviderFragmentRoot
//     └── one child per hits_[i]
//                          IRawElementProviderSimple
//                          IRawElementProviderFragment
//                          IInvokeProvider          (actionable roles)
//                          ISelectionItemProvider   (list item, tab)
//                          IToggleProvider          (toggle)
//
// The tree is exactly two levels deep, which is what the hit list is. A
// surface that needs grouping emits a PhiHitRole::kGroup hit around the
// finer ones; the group is announced but stays a sibling, because hits_ is
// flat and inventing a hierarchy from overlapping rectangles would guess.
//
// THREADING. Every entry point below, and every method of every provider
// object this file creates, runs ON THE UI THREAD. WM_GETOBJECT arrives
// there; the providers declare ProviderOptions_UseComThreading so UIA
// marshals client calls into the apartment that answered instead of
// calling in on an RPC thread; and the events are raised from WM_PAINT,
// again on that thread. Nothing here takes a lock, and nothing here may be
// called from the relay, outbox or updater threads.
//
// LIFETIME. A UIA client is a separate process holding COM references that
// can outlive the HWND by any amount. The fragment root is therefore
// reference-counted, NOT owned by PhiHitSource: WM_NCDESTROY detaches it
// (PhiUiaWindowDestroyed) rather than freeing it, and after that every
// method answers UIA_E_ELEMENTNOTAVAILABLE. UiaDisconnectProvider tells the
// UIA core to drop its own cached references at the same moment.

namespace phi_client {

// ---------------------------------------------------------------------------
// PURE half — every decision, none of the SDK.
//
// The role mapping, the coordinate transform, the identity resolution and
// the event decision table are plain functions over PhiHit, so
// test/model/uia_test.cc pins them on the Linux host. The Win32 half below
// static_asserts each id against the SDK's own constant, so a typo here
// fails the client build rather than shipping a wrong control type.

// UIA control-type ids (UIAutomationClient.h).
inline constexpr int kPhiUiaButtonControlType = 50000;
inline constexpr int kPhiUiaCheckBoxControlType = 50002;
inline constexpr int kPhiUiaImageControlType = 50006;
inline constexpr int kPhiUiaListItemControlType = 50007;
inline constexpr int kPhiUiaSliderControlType = 50015;
inline constexpr int kPhiUiaTabItemControlType = 50019;
inline constexpr int kPhiUiaTextControlType = 50020;
inline constexpr int kPhiUiaCustomControlType = 50025;
inline constexpr int kPhiUiaGroupControlType = 50026;

// role → control type. THE table (see the report for the prose version):
//
//   kButton    Button     invokable; the nav buttons, "All Downloads"
//   kToggle    CheckBox   invokable + Toggle; a pin/star, state in `selected`
//   kListItem  ListItem   invokable + SelectionItem; a downloads/search row
//   kTab       TabItem    invokable + SelectionItem; a sidebar tab
//   kText      Text       announced only — a static label
//   kImage     Image      announced only — a favicon, a preview thumbnail
//   kSlider    Slider     announced only: PhiHit carries no range, and a
//                         RangeValue pattern over data we do not have would
//                         announce a number that is not the knob's
//   kGroup     Group      a container announced around finer hits
//   kNone      Custom     a hit that only wants to be clickable
int PhiUiaControlType(PhiHitRole role);

// Which control patterns a role genuinely has. A pattern is offered ONLY
// where the hit list actually carries the state behind it, so a screen
// reader is never told about an action that does nothing.
enum PhiUiaPattern : unsigned {
  kPhiUiaNoPattern = 0u,
  kPhiUiaInvokePattern = 1u << 0,
  kPhiUiaSelectionItemPattern = 1u << 1,
  kPhiUiaTogglePattern = 1u << 2,
};
unsigned PhiUiaPatterns(PhiHitRole role);

// True for the roles that carry content rather than structure. A kGroup is
// a control element but not a content element, so a screen reader's content
// walk reads the rows and not the box drawn round them.
bool PhiUiaIsContent(PhiHitRole role);

// ---------------------------------------------------------------------------
// Geometry.

// `bounds` (client coordinates) moved into screen space. `origin` is the
// screen position of the window's client origin — ClientToScreen({0,0}).
PhiRectI PhiUiaToScreen(const PhiRectI& bounds, int origin_x, int origin_y);

// True when the hit does not intersect the window's client rect at all: a
// row scrolled out of a popover's viewport is still in the tree (so the
// client can page to it) but must report IsOffscreen.
bool PhiUiaIsOffscreen(const PhiRectI& bounds, const PhiRectI& client);

// ---------------------------------------------------------------------------
// Identity across a rebuild.
//
// A surface replaces hits_ wholesale (ClearHits + AddHit) whenever its
// layout changes, so a provider handed out five seconds ago must find its
// hit again in a NEW vector. `index` is where it was and `key` is the
// surface's own id for it; the index is trusted only when the key still
// matches there, which makes the common case (nothing moved) O(1) and the
// reordered case correct. Returns -1 when the hit is gone.
int PhiUiaResolveHit(const std::vector<PhiHit>& hits, int index, int key);

// A point inside hits[index] that PhiHitAt() resolves BACK to `index`, so
// an Invoke lands exactly where a mouse click on that element would. The
// centre is tried first; a hit whose centre is covered by a later, smaller
// hit (a row with a button drawn on top of its middle) falls back to a
// scan of its own area. False when the hit is unreachable — fully covered
// by later hits, or disabled, in which case a real click cannot reach it
// either and Invoke must fail rather than pretend.
bool PhiUiaInvokePoint(const std::vector<PhiHit>& hits,
                       int index,
                       int* x,
                       int* y);

// ---------------------------------------------------------------------------
// The event decision table.
//
// hits_version() bumps once per ClearHits(), which tells us the list was
// rebuilt but not what changed. A screen reader stuck on a stale tree is
// the failure mode; a screen reader flooded with structure-changed events
// is the other one. So: snapshot the list, diff the snapshots, and raise
// the narrowest event that describes the difference.

struct PhiUiaHitState {
  int key = 0;
  PhiHitRole role = PhiHitRole::kNone;
  std::wstring name;
  bool selected = false;
  bool enabled = true;
};

std::vector<PhiUiaHitState> PhiUiaSnapshot(const std::vector<PhiHit>& hits);

// What the difference between two snapshots has to be announced as.
//
//   count, key or role moved   ▶ structure    (the tree itself changed; the
//                                client re-reads it, so no per-element
//                                events are raised alongside)
//   name changed in place      ▶ renamed[]    UIA_NamePropertyId
//   selected changed in place  ▶ reselected[] SelectionItem_IsSelected /
//                                             Toggle_ToggleState
//   enabled changed in place   ▶ reenabled[]  UIA_IsEnabledPropertyId
//   nothing changed            ▶ nothing (a rebuild that produced the same
//                                list — the common case for a repaint —
//                                raises no event at all)
struct PhiUiaChanges {
  bool structure = false;
  std::vector<int> renamed;
  std::vector<int> reselected;
  std::vector<int> reenabled;

  bool any() const {
    return structure || !renamed.empty() || !reselected.empty() ||
           !reenabled.empty();
  }
};

PhiUiaChanges PhiUiaDiff(const std::vector<PhiUiaHitState>& before,
                         const std::vector<PhiUiaHitState>& after);

// The whole gate, in one place: nothing is raised when no assistive client
// is listening (UiaClientsAreListening), and nothing is raised when the
// rebuild produced an identical list. Both halves matter — raising into an
// empty room costs a cross-process call per repaint.
bool PhiUiaShouldRaise(bool clients_are_listening, const PhiUiaChanges& changes);

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Win32 — the three seams PhiHitSource calls. UI THREAD ONLY (see the
// threading note at the top).

// WM_GETOBJECT. Answers UiaRootObjectId with this window's fragment root,
// through UiaReturnRawElementProvider (never a raw pointer), creating the
// root on first ask. Leaves *handled false for every other object id —
// OBJID_CLIENT included — so DefWindowProcW keeps answering MSAA for the
// child controls exactly as it does today.
LRESULT PhiUiaOnGetObject(PhiHitSource* window,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled);

// hits_ may have been rebuilt. Cheap and safe to call on every repaint: it
// returns immediately unless this window has actually handed out a
// provider AND hits_version() moved since the last announcement.
void PhiUiaHitsChanged(PhiHitSource* window);

// The HWND is going away (WM_NCDESTROY). Detaches the fragment root and
// calls UiaDisconnectProvider; the root itself lives until the last client
// reference goes, answering UIA_E_ELEMENTNOTAVAILABLE.
void PhiUiaWindowDestroyed(PhiHitSource* window);

// Keyboard focus AMONG a surface's hits. A surface that moves a focus ring
// over its own drawn controls (Settings) reports it here, after every
// rebuild of hits_ and on every move: from the first call on, that
// surface's actionable hits answer IsKeyboardFocusable, the focused one
// answers HasKeyboardFocus while the HWND has the system focus, and the
// root's GetFocus returns it. `index` indexes hits(), -1 for none.
// `announce` raises UIA_AutomationFocusChangedEventId on it — what a screen
// reader speaks — and is a no-op when no client is listening. A surface
// that never calls this keeps the old answer: nothing is focusable.
void PhiUiaSetFocusedHit(PhiHitSource* window, int index, bool announce);

#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_UIA_H_
