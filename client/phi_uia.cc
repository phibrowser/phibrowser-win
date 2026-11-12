// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_uia.h"

#include <algorithm>

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure — compiled on the Linux host too (test/model/uia_test.cc).

int PhiUiaControlType(PhiHitRole role) {
  switch (role) {
    case PhiHitRole::kButton:
      return kPhiUiaButtonControlType;
    case PhiHitRole::kToggle:
      // CheckBox, not Button: the state lives in PhiHit::selected and a
      // CheckBox is the control type whose Toggle pattern a screen reader
      // announces ("pinned" / "not pinned") without the surface saying so.
      return kPhiUiaCheckBoxControlType;
    case PhiHitRole::kListItem:
      return kPhiUiaListItemControlType;
    case PhiHitRole::kTab:
      return kPhiUiaTabItemControlType;
    case PhiHitRole::kText:
      return kPhiUiaTextControlType;
    case PhiHitRole::kSlider:
      return kPhiUiaSliderControlType;
    case PhiHitRole::kGroup:
      return kPhiUiaGroupControlType;
    case PhiHitRole::kImage:
      return kPhiUiaImageControlType;
    case PhiHitRole::kNone:
      break;
  }
  return kPhiUiaCustomControlType;
}

unsigned PhiUiaPatterns(PhiHitRole role) {
  switch (role) {
    case PhiHitRole::kButton:
      return kPhiUiaInvokePattern;
    case PhiHitRole::kToggle:
      return kPhiUiaInvokePattern | kPhiUiaTogglePattern;
    case PhiHitRole::kListItem:
    case PhiHitRole::kTab:
      return kPhiUiaInvokePattern | kPhiUiaSelectionItemPattern;
    case PhiHitRole::kSlider:
      // NOT RangeValue: PhiHit carries no minimum, maximum or value, and a
      // range pattern filled with invented numbers announces a position the
      // knob is not at. A surface with a real range extends PhiHit first.
    case PhiHitRole::kText:
    case PhiHitRole::kImage:
    case PhiHitRole::kGroup:
    case PhiHitRole::kNone:
      break;
  }
  return kPhiUiaNoPattern;
}

bool PhiUiaIsContent(PhiHitRole role) {
  return role != PhiHitRole::kGroup;
}

PhiRectI PhiUiaToScreen(const PhiRectI& bounds, int origin_x, int origin_y) {
  PhiRectI out;
  out.left = bounds.left + origin_x;
  out.top = bounds.top + origin_y;
  out.right = bounds.right + origin_x;
  out.bottom = bounds.bottom + origin_y;
  return out;
}

bool PhiUiaIsOffscreen(const PhiRectI& bounds, const PhiRectI& client) {
  if (bounds.empty() || client.empty()) {
    return true;
  }
  return bounds.right <= client.left || bounds.left >= client.right ||
         bounds.bottom <= client.top || bounds.top >= client.bottom;
}

int PhiUiaResolveHit(const std::vector<PhiHit>& hits, int index, int key) {
  const int count = static_cast<int>(hits.size());
  if (index >= 0 && index < count &&
      hits[static_cast<size_t>(index)].key == key) {
    return index;  // Nothing moved — the common case.
  }
  for (int i = 0; i < count; ++i) {
    if (hits[static_cast<size_t>(i)].key == key) {
      return i;
    }
  }
  return -1;
}

bool PhiUiaInvokePoint(const std::vector<PhiHit>& hits,
                       int index,
                       int* x,
                       int* y) {
  if (index < 0 || index >= static_cast<int>(hits.size())) {
    return false;
  }
  const PhiHit& hit = hits[static_cast<size_t>(index)];
  if (!hit.enabled || hit.bounds.empty()) {
    return false;
  }
  const int centre_x = hit.bounds.left + hit.bounds.width() / 2;
  const int centre_y = hit.bounds.top + hit.bounds.height() / 2;
  if (PhiHitAt(hits, centre_x, centre_y) == index) {
    *x = centre_x;
    *y = centre_y;
    return true;
  }
  // The centre is covered by a hit added later (a row with its own button
  // drawn over the middle). Sample the rest of the rect on a coarse grid —
  // the same places a user's pointer would find.
  constexpr int kSteps = 5;
  for (int row = 0; row < kSteps; ++row) {
    const int py = hit.bounds.top +
                   (hit.bounds.height() * (2 * row + 1)) / (2 * kSteps);
    for (int column = 0; column < kSteps; ++column) {
      const int px = hit.bounds.left +
                     (hit.bounds.width() * (2 * column + 1)) / (2 * kSteps);
      if (PhiHitAt(hits, px, py) == index) {
        *x = px;
        *y = py;
        return true;
      }
    }
  }
  return false;
}

std::vector<PhiUiaHitState> PhiUiaSnapshot(const std::vector<PhiHit>& hits) {
  std::vector<PhiUiaHitState> out;
  out.reserve(hits.size());
  for (const PhiHit& hit : hits) {
    PhiUiaHitState state;
    state.key = hit.key;
    state.role = hit.role;
    state.name = hit.name;
    state.selected = hit.selected;
    state.enabled = hit.enabled;
    out.push_back(std::move(state));
  }
  return out;
}

PhiUiaChanges PhiUiaDiff(const std::vector<PhiUiaHitState>& before,
                         const std::vector<PhiUiaHitState>& after) {
  PhiUiaChanges changes;
  if (before.size() != after.size()) {
    changes.structure = true;
    return changes;
  }
  for (size_t i = 0; i < after.size(); ++i) {
    if (before[i].key != after[i].key || before[i].role != after[i].role) {
      // The element at this position is a DIFFERENT element: the tree
      // changed shape, so the client re-reads it whole and the per-element
      // events below would be about elements it has already dropped.
      changes.structure = true;
      changes.renamed.clear();
      changes.reselected.clear();
      changes.reenabled.clear();
      return changes;
    }
  }
  for (size_t i = 0; i < after.size(); ++i) {
    const int index = static_cast<int>(i);
    if (before[i].name != after[i].name) {
      changes.renamed.push_back(index);
    }
    if (before[i].selected != after[i].selected) {
      changes.reselected.push_back(index);
    }
    if (before[i].enabled != after[i].enabled) {
      changes.reenabled.push_back(index);
    }
  }
  return changes;
}

bool PhiUiaShouldRaise(bool clients_are_listening,
                       const PhiUiaChanges& changes) {
  return clients_are_listening && changes.any();
}

}  // namespace phi_client

#if defined(_WIN32)

// objbase.h before uiautomation.h: the UIA headers are MIDL output and use
// the `interface` keyword macro, which the client's WIN32_LEAN_AND_MEAN
// windows.h does not pull in.
#include <objbase.h>

#include <uiautomation.h>
#include <wrl/client.h>

#include <map>
#include <utility>

#include "base/no_destructor.h"

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

// The pure table above spells the control-type ids itself so the host can
// compile it; these pin it to the SDK. A typo is a client build failure,
// not a wrong announcement on someone's screen reader.
static_assert(kPhiUiaButtonControlType == UIA_ButtonControlTypeId, "");
static_assert(kPhiUiaCheckBoxControlType == UIA_CheckBoxControlTypeId, "");
static_assert(kPhiUiaImageControlType == UIA_ImageControlTypeId, "");
static_assert(kPhiUiaListItemControlType == UIA_ListItemControlTypeId, "");
static_assert(kPhiUiaSliderControlType == UIA_SliderControlTypeId, "");
static_assert(kPhiUiaTabItemControlType == UIA_TabItemControlTypeId, "");
static_assert(kPhiUiaTextControlType == UIA_TextControlTypeId, "");
static_assert(kPhiUiaCustomControlType == UIA_CustomControlTypeId, "");
static_assert(kPhiUiaGroupControlType == UIA_GroupControlTypeId, "");

// ---------------------------------------------------------------------------
// uiautomationcore.dll, loaded by hand.
//
// The framework's link recipe (out/phi_clientkit/kit.json) is generated on
// the Chromium side and carries no uiautomationcore.lib; importing one
// would mean regenerating the kit for every consumer of the SDK. The seven
// entry points below are resolved once instead, which also means a machine
// without the UIA core (or with it disabled) degrades to "no accessibility"
// rather than failing to start.
struct UiaApi {
  decltype(&::UiaReturnRawElementProvider) return_provider = nullptr;
  decltype(&::UiaHostProviderFromHwnd) host_from_hwnd = nullptr;
  decltype(&::UiaDisconnectProvider) disconnect = nullptr;
  decltype(&::UiaClientsAreListening) clients_are_listening = nullptr;
  decltype(&::UiaRaiseStructureChangedEvent) raise_structure = nullptr;
  decltype(&::UiaRaiseAutomationPropertyChangedEvent) raise_property = nullptr;
  decltype(&::UiaRaiseAutomationEvent) raise_event = nullptr;
  bool ready = false;
};

const UiaApi& Uia() {
  // A function-local static of a trivially destructible type: no
  // exit-time destructor, and the DLL is loaded once, lazily, only if
  // something actually asks this process for accessibility.
  static const UiaApi api = [] {
    UiaApi loaded;
    HMODULE core = ::LoadLibraryExW(L"uiautomationcore.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!core) {
      return loaded;
    }
    loaded.return_provider =
        reinterpret_cast<decltype(loaded.return_provider)>(
            ::GetProcAddress(core, "UiaReturnRawElementProvider"));
    loaded.host_from_hwnd = reinterpret_cast<decltype(loaded.host_from_hwnd)>(
        ::GetProcAddress(core, "UiaHostProviderFromHwnd"));
    loaded.disconnect = reinterpret_cast<decltype(loaded.disconnect)>(
        ::GetProcAddress(core, "UiaDisconnectProvider"));
    loaded.clients_are_listening =
        reinterpret_cast<decltype(loaded.clients_are_listening)>(
            ::GetProcAddress(core, "UiaClientsAreListening"));
    loaded.raise_structure = reinterpret_cast<decltype(loaded.raise_structure)>(
        ::GetProcAddress(core, "UiaRaiseStructureChangedEvent"));
    loaded.raise_property = reinterpret_cast<decltype(loaded.raise_property)>(
        ::GetProcAddress(core, "UiaRaiseAutomationPropertyChangedEvent"));
    loaded.raise_event = reinterpret_cast<decltype(loaded.raise_event)>(
        ::GetProcAddress(core, "UiaRaiseAutomationEvent"));
    loaded.ready = loaded.return_provider && loaded.host_from_hwnd &&
                   loaded.clients_are_listening;
    return loaded;
  }();
  return api;
}

bool ClientsAreListening() {
  const UiaApi& api = Uia();
  return api.ready && api.clients_are_listening() != FALSE;
}

// ---------------------------------------------------------------------------
// VARIANT helpers.

HRESULT VariantEmpty(VARIANT* out) {
  ::VariantInit(out);
  return S_OK;
}

HRESULT VariantBool(VARIANT* out, bool value) {
  out->vt = VT_BOOL;
  out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

HRESULT VariantInt(VARIANT* out, int value) {
  out->vt = VT_I4;
  out->lVal = value;
  return S_OK;
}

HRESULT VariantString(VARIANT* out, const std::wstring& value) {
  BSTR text = ::SysAllocStringLen(value.c_str(), static_cast<UINT>(value.size()));
  if (!text) {
    return E_OUTOFMEMORY;
  }
  out->vt = VT_BSTR;
  out->bstrVal = text;
  return S_OK;
}

UiaRect ToUiaRect(const PhiRectI& rect) {
  UiaRect out;
  out.left = static_cast<double>(rect.left);
  out.top = static_cast<double>(rect.top);
  out.width = static_cast<double>(rect.width());
  out.height = static_cast<double>(rect.height());
  return out;
}

// The screen position of a window's client origin.
POINT ClientOrigin(HWND hwnd) {
  POINT origin = {0, 0};
  ::ClientToScreen(hwnd, &origin);
  return origin;
}

// ---------------------------------------------------------------------------
// Keyboard focus among a surface's hits (PhiUiaSetFocusedHit).
//
// Kept beside the providers, not in PhiHitSource, for the same reason the
// roots are: a window that never moves a focus ring over its hits has no
// entry and pays nothing. The index is re-resolved by key, exactly as an
// element's is, so a rebuild between two reports cannot point it at a
// neighbour. UI thread only.

struct FocusedHit {
  int index = -1;
  int key = 0;
};

std::map<PhiHitSource*, FocusedHit>& FocusedHits() {
  static base::NoDestructor<std::map<PhiHitSource*, FocusedHit>> focused;
  return *focused;
}

// Whether `window` reports its focus at all.
bool TracksFocus(PhiHitSource* window) {
  return window && FocusedHits().count(window) > 0;
}

// The hits() index holding the window's keyboard focus, or -1.
int FocusedHitOf(PhiHitSource* window) {
  if (!window) {
    return -1;
  }
  const auto it = FocusedHits().find(window);
  if (it == FocusedHits().end() || it->second.index < 0) {
    return -1;
  }
  return PhiUiaResolveHit(window->hits(), it->second.index, it->second.key);
}

// The roles a focus ring can sit on: everything a user can act on, and not
// the static text, pictures and group boxes announced around them.
bool RoleTakesFocus(PhiHitRole role) {
  return role != PhiHitRole::kText && role != PhiHitRole::kImage &&
         role != PhiHitRole::kGroup;
}

class PhiUiaRoot;

// ---------------------------------------------------------------------------
// One hit, as a UIA fragment.
//
// Holds a strong reference to the fragment root and NO reference from it:
// the root creates a child on demand and forgets it, so there is no cycle
// and no per-hit allocation kept alive across rebuilds. Identity is carried
// by GetRuntimeId (the hit's key), which is what a UIA client reconciles
// on, so two Element objects for the same hit are the same element to the
// client even though they are different C++ objects.
class PhiUiaElement final : public IRawElementProviderSimple,
                            public IRawElementProviderFragment,
                            public IInvokeProvider,
                            public ISelectionItemProvider,
                            public IToggleProvider {
 public:
  PhiUiaElement(PhiUiaRoot* root, int index, int key);

  // IUnknown.
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IRawElementProviderSimple.
  IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override;
  IFACEMETHODIMP GetPatternProvider(PATTERNID pattern,
                                    IUnknown** provider) override;
  IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* value) override;
  IFACEMETHODIMP get_HostRawElementProvider(
      IRawElementProviderSimple** provider) override;

  // IRawElementProviderFragment.
  IFACEMETHODIMP Navigate(NavigateDirection direction,
                          IRawElementProviderFragment** target) override;
  IFACEMETHODIMP GetRuntimeId(SAFEARRAY** id) override;
  IFACEMETHODIMP get_BoundingRectangle(UiaRect* rect) override;
  IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** roots) override;
  IFACEMETHODIMP SetFocus() override;
  IFACEMETHODIMP get_FragmentRoot(
      IRawElementProviderFragmentRoot** root) override;

  // IInvokeProvider.
  IFACEMETHODIMP Invoke() override;

  // ISelectionItemProvider.
  IFACEMETHODIMP Select() override;
  IFACEMETHODIMP AddToSelection() override;
  IFACEMETHODIMP RemoveFromSelection() override;
  IFACEMETHODIMP get_IsSelected(BOOL* selected) override;
  IFACEMETHODIMP get_SelectionContainer(
      IRawElementProviderSimple** container) override;

  // IToggleProvider.
  IFACEMETHODIMP Toggle() override;
  IFACEMETHODIMP get_ToggleState(ToggleState* state) override;

 private:
  // Reference-counted: Release() is the only caller. Declared here and
  // defined below PhiUiaRoot, because ~ComPtr<PhiUiaRoot> needs the root's
  // complete type.
  ~PhiUiaElement();

  // The live PhiHit, or null when the window is gone or the hit was
  // dropped by a rebuild. Refreshes index_ when the hit merely moved.
  const PhiHit* Resolve();
  PhiHitSource* window() const;

  ULONG ref_ = 1;
  ComPtr<PhiUiaRoot> root_;
  int index_ = -1;
  const int key_;
};

// ---------------------------------------------------------------------------
// The window, as a UIA fragment root.

class PhiUiaRoot final : public IRawElementProviderSimple,
                         public IRawElementProviderFragment,
                         public IRawElementProviderFragmentRoot {
 public:
  explicit PhiUiaRoot(PhiHitSource* window)
      : window_(window), hwnd_(window->hwnd()) {}

  // IUnknown.
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IRawElementProviderSimple.
  IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override;
  IFACEMETHODIMP GetPatternProvider(PATTERNID pattern,
                                    IUnknown** provider) override;
  IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* value) override;
  IFACEMETHODIMP get_HostRawElementProvider(
      IRawElementProviderSimple** provider) override;

  // IRawElementProviderFragment.
  IFACEMETHODIMP Navigate(NavigateDirection direction,
                          IRawElementProviderFragment** target) override;
  IFACEMETHODIMP GetRuntimeId(SAFEARRAY** id) override;
  IFACEMETHODIMP get_BoundingRectangle(UiaRect* rect) override;
  IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** roots) override;
  IFACEMETHODIMP SetFocus() override;
  IFACEMETHODIMP get_FragmentRoot(
      IRawElementProviderFragmentRoot** root) override;

  // IRawElementProviderFragmentRoot.
  IFACEMETHODIMP ElementProviderFromPoint(
      double x,
      double y,
      IRawElementProviderFragment** target) override;
  IFACEMETHODIMP GetFocus(IRawElementProviderFragment** target) override;

  // The HWND is going. Tell the UIA core to drop its cached references
  // while we can still answer, then go dead: every method afterwards
  // returns UIA_E_ELEMENTNOTAVAILABLE, however long a client holds on.
  void Detach();

  PhiHitSource* window() const { return window_; }
  HWND hwnd() const { return hwnd_; }
  bool attached() const { return window_ != nullptr; }

  // Creates a provider for hits()[index]. Null when out of range.
  ComPtr<PhiUiaElement> ChildAt(int index);

  // Diffs hits() against the last announced snapshot and raises what the
  // difference calls for. No-op unless a client is listening.
  void AnnounceHits();

 private:
  // Reference-counted: Release() is the only caller.
  ~PhiUiaRoot() = default;

  void RaiseProperty(int index, PROPERTYID property);

  ULONG ref_ = 1;
  // Cleared by Detach(); never dereferenced afterwards. Not a raw_ptr: the
  // client's smart pointers do not reach this file's COM objects, and the
  // window explicitly outlives the pointer by detaching at WM_NCDESTROY.
  RAW_PTR_EXCLUSION PhiHitSource* window_ = nullptr;
  HWND hwnd_ = nullptr;
  std::vector<PhiUiaHitState> announced_;
  uint32_t announced_version_ = 0;
  bool announced_once_ = false;
};

// ---------------------------------------------------------------------------
// PhiUiaElement.

PhiUiaElement::PhiUiaElement(PhiUiaRoot* root, int index, int key)
    : root_(root), index_(index), key_(key) {}

PhiUiaElement::~PhiUiaElement() = default;

IFACEMETHODIMP PhiUiaElement::QueryInterface(REFIID iid, void** object) {
  if (!object) {
    return E_INVALIDARG;
  }
  *object = nullptr;
  if (iid == __uuidof(IUnknown) ||
      iid == __uuidof(IRawElementProviderSimple)) {
    *object = static_cast<IRawElementProviderSimple*>(this);
  } else if (iid == __uuidof(IRawElementProviderFragment)) {
    *object = static_cast<IRawElementProviderFragment*>(this);
  } else if (iid == __uuidof(IInvokeProvider)) {
    *object = static_cast<IInvokeProvider*>(this);
  } else if (iid == __uuidof(ISelectionItemProvider)) {
    *object = static_cast<ISelectionItemProvider*>(this);
  } else if (iid == __uuidof(IToggleProvider)) {
    *object = static_cast<IToggleProvider*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

IFACEMETHODIMP_(ULONG) PhiUiaElement::AddRef() {
  return static_cast<ULONG>(
      ::InterlockedIncrement(reinterpret_cast<LONG volatile*>(&ref_)));
}

IFACEMETHODIMP_(ULONG) PhiUiaElement::Release() {
  const ULONG count = static_cast<ULONG>(
      ::InterlockedDecrement(reinterpret_cast<LONG volatile*>(&ref_)));
  if (count == 0) {
    delete this;
  }
  return count;
}

PhiHitSource* PhiUiaElement::window() const {
  return root_ ? root_->window() : nullptr;
}

const PhiHit* PhiUiaElement::Resolve() {
  PhiHitSource* owner = window();
  if (!owner) {
    return nullptr;
  }
  const std::vector<PhiHit>& hits = owner->hits();
  const int index = PhiUiaResolveHit(hits, index_, key_);
  if (index < 0) {
    return nullptr;
  }
  index_ = index;
  return &hits[static_cast<size_t>(index)];
}

IFACEMETHODIMP PhiUiaElement::get_ProviderOptions(ProviderOptions* options) {
  if (!options) {
    return E_INVALIDARG;
  }
  *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                          ProviderOptions_UseComThreading);
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::GetPatternProvider(PATTERNID pattern,
                                                 IUnknown** provider) {
  if (!provider) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const PhiHit* hit = Resolve();
  if (!hit) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const unsigned patterns = PhiUiaPatterns(hit->role);
  if (pattern == UIA_InvokePatternId && (patterns & kPhiUiaInvokePattern)) {
    *provider = static_cast<IInvokeProvider*>(this);
  } else if (pattern == UIA_SelectionItemPatternId &&
             (patterns & kPhiUiaSelectionItemPattern)) {
    *provider = static_cast<ISelectionItemProvider*>(this);
  } else if (pattern == UIA_TogglePatternId &&
             (patterns & kPhiUiaTogglePattern)) {
    *provider = static_cast<IToggleProvider*>(this);
  }
  if (*provider) {
    AddRef();
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::GetPropertyValue(PROPERTYID property,
                                               VARIANT* value) {
  if (!value) {
    return E_INVALIDARG;
  }
  ::VariantInit(value);
  const PhiHit* hit = Resolve();
  if (!hit) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  switch (property) {
    case UIA_NamePropertyId:
      // PhiHit::name is the accessible name AND the tooltip text: one
      // string, so a surface cannot label one and forget the other.
      return VariantString(value, hit->name);
    case UIA_ControlTypePropertyId:
      return VariantInt(value, PhiUiaControlType(hit->role));
    case UIA_IsEnabledPropertyId:
      return VariantBool(value, hit->enabled);
    case UIA_IsControlElementPropertyId:
      return VariantBool(value, true);
    case UIA_IsContentElementPropertyId:
      return VariantBool(value, PhiUiaIsContent(hit->role));
    case UIA_HasKeyboardFocusPropertyId: {
      // Only a surface that moves a focus ring over its hits reports one
      // (PhiUiaSetFocusedHit), and only while its HWND holds the system
      // focus; everywhere else this stays false rather than a claim the
      // surface cannot honour.
      PhiHitSource* owner = window();
      return VariantBool(value, owner && owner->hwnd() &&
                                    ::GetFocus() == owner->hwnd() &&
                                    FocusedHitOf(owner) == index_);
    }
    case UIA_IsKeyboardFocusablePropertyId:
      return VariantBool(value, TracksFocus(window()) && hit->enabled &&
                                    RoleTakesFocus(hit->role));
    case UIA_IsOffscreenPropertyId: {
      PhiHitSource* owner = window();
      RECT client = {};
      if (owner && owner->hwnd()) {
        ::GetClientRect(owner->hwnd(), &client);
      }
      return VariantBool(value,
                         PhiUiaIsOffscreen(hit->bounds, PhiFromRect(client)));
    }
    case UIA_AutomationIdPropertyId:
      return VariantString(value,
                           L"phi-hit-" + std::to_wstring(hit->key));
    case UIA_ProviderDescriptionPropertyId:
      return VariantString(value, L"Phi hit-rect provider");
    default:
      break;
  }
  return VariantEmpty(value);
}

IFACEMETHODIMP PhiUiaElement::get_HostRawElementProvider(
    IRawElementProviderSimple** provider) {
  if (!provider) {
    return E_INVALIDARG;
  }
  // Only the fragment root is hosted by an HWND.
  *provider = nullptr;
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** target) {
  if (!target) {
    return E_INVALIDARG;
  }
  *target = nullptr;
  const PhiHit* hit = Resolve();
  if (!hit) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  switch (direction) {
    case NavigateDirection_Parent:
      *target = static_cast<IRawElementProviderFragment*>(root_.Get());
      root_->AddRef();
      return S_OK;
    case NavigateDirection_NextSibling:
    case NavigateDirection_PreviousSibling: {
      const int step = direction == NavigateDirection_NextSibling ? 1 : -1;
      if (ComPtr<PhiUiaElement> sibling = root_->ChildAt(index_ + step)) {
        *target = static_cast<IRawElementProviderFragment*>(sibling.Detach());
      }
      return S_OK;
    }
    case NavigateDirection_FirstChild:
    case NavigateDirection_LastChild:
      // The hit list is flat (see the shape note in phi_uia.h).
      return S_OK;
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::GetRuntimeId(SAFEARRAY** id) {
  if (!id) {
    return E_INVALIDARG;
  }
  *id = nullptr;
  // {UiaAppendRuntimeId, key}: the core prefixes the host window's own id,
  // so the element is unique process-wide and stable across a rebuild for
  // as long as the surface keeps giving that row the same key.
  SAFEARRAY* array = ::SafeArrayCreateVector(VT_I4, 0, 2);
  if (!array) {
    return E_OUTOFMEMORY;
  }
  LONG slot = 0;
  int value = UiaAppendRuntimeId;
  HRESULT hr = ::SafeArrayPutElement(array, &slot, &value);
  if (SUCCEEDED(hr)) {
    slot = 1;
    value = key_;
    hr = ::SafeArrayPutElement(array, &slot, &value);
  }
  if (FAILED(hr)) {
    ::SafeArrayDestroy(array);
    return hr;
  }
  *id = array;
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::get_BoundingRectangle(UiaRect* rect) {
  if (!rect) {
    return E_INVALIDARG;
  }
  *rect = {};
  const PhiHit* hit = Resolve();
  PhiHitSource* owner = window();
  if (!hit || !owner || !owner->hwnd()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const POINT origin = ClientOrigin(owner->hwnd());
  *rect = ToUiaRect(PhiUiaToScreen(hit->bounds, static_cast<int>(origin.x),
                                   static_cast<int>(origin.y)));
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::GetEmbeddedFragmentRoots(SAFEARRAY** roots) {
  if (!roots) {
    return E_INVALIDARG;
  }
  *roots = nullptr;
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::SetFocus() {
  PhiHitSource* owner = window();
  if (!owner || !owner->hwnd()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  // A hit is not separately focusable, so the honest move is to focus the
  // surface that owns it — and only when it is not already focused, so a
  // client walking the tree cannot steal focus from a child EDIT.
  if (::GetFocus() != owner->hwnd()) {
    ::SetFocus(owner->hwnd());
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::get_FragmentRoot(
    IRawElementProviderFragmentRoot** root) {
  if (!root) {
    return E_INVALIDARG;
  }
  *root = static_cast<IRawElementProviderFragmentRoot*>(root_.Get());
  root_->AddRef();
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::Invoke() {
  const PhiHit* hit = Resolve();
  PhiHitSource* owner = window();
  if (!hit || !owner) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!(PhiUiaPatterns(hit->role) & kPhiUiaInvokePattern)) {
    return UIA_E_INVALIDOPERATION;
  }
  // THE mouse path, not a copy of it: PhiHitSource::InvokeHit sends the same
  // WM_LBUTTONDOWN / WM_LBUTTONUP pair a click sends, through the same
  // wndproc, the same hit test and the same OnMouseDown/OnClick overrides.
  // The surface cannot tell the difference and cannot drift from it.
  const int index = index_;
  return owner->InvokeHit(index) ? S_OK : UIA_E_INVALIDOPERATION;
}

IFACEMETHODIMP PhiUiaElement::Select() {
  return Invoke();
}

IFACEMETHODIMP PhiUiaElement::AddToSelection() {
  // Every hit list that carries a selection carries exactly one.
  return UIA_E_INVALIDOPERATION;
}

IFACEMETHODIMP PhiUiaElement::RemoveFromSelection() {
  return UIA_E_INVALIDOPERATION;
}

IFACEMETHODIMP PhiUiaElement::get_IsSelected(BOOL* selected) {
  if (!selected) {
    return E_INVALIDARG;
  }
  *selected = FALSE;
  const PhiHit* hit = Resolve();
  if (!hit) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  *selected = hit->selected ? TRUE : FALSE;
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::get_SelectionContainer(
    IRawElementProviderSimple** container) {
  if (!container) {
    return E_INVALIDARG;
  }
  *container = static_cast<IRawElementProviderSimple*>(root_.Get());
  root_->AddRef();
  return S_OK;
}

IFACEMETHODIMP PhiUiaElement::Toggle() {
  return Invoke();
}

IFACEMETHODIMP PhiUiaElement::get_ToggleState(ToggleState* state) {
  if (!state) {
    return E_INVALIDARG;
  }
  *state = ToggleState_Off;
  const PhiHit* hit = Resolve();
  if (!hit) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  *state = hit->selected ? ToggleState_On : ToggleState_Off;
  return S_OK;
}

// ---------------------------------------------------------------------------
// PhiUiaRoot.

IFACEMETHODIMP PhiUiaRoot::QueryInterface(REFIID iid, void** object) {
  if (!object) {
    return E_INVALIDARG;
  }
  *object = nullptr;
  if (iid == __uuidof(IUnknown) ||
      iid == __uuidof(IRawElementProviderSimple)) {
    *object = static_cast<IRawElementProviderSimple*>(this);
  } else if (iid == __uuidof(IRawElementProviderFragment)) {
    *object = static_cast<IRawElementProviderFragment*>(this);
  } else if (iid == __uuidof(IRawElementProviderFragmentRoot)) {
    *object = static_cast<IRawElementProviderFragmentRoot*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

IFACEMETHODIMP_(ULONG) PhiUiaRoot::AddRef() {
  return static_cast<ULONG>(
      ::InterlockedIncrement(reinterpret_cast<LONG volatile*>(&ref_)));
}

IFACEMETHODIMP_(ULONG) PhiUiaRoot::Release() {
  const ULONG count = static_cast<ULONG>(
      ::InterlockedDecrement(reinterpret_cast<LONG volatile*>(&ref_)));
  if (count == 0) {
    delete this;
  }
  return count;
}

IFACEMETHODIMP PhiUiaRoot::get_ProviderOptions(ProviderOptions* options) {
  if (!options) {
    return E_INVALIDARG;
  }
  // UseComThreading is what keeps the promise in phi_uia.h's threading
  // note: UIA marshals client calls into this object's apartment instead
  // of calling in on an RPC worker, so hits_ is only ever read on the
  // thread that writes it.
  *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                          ProviderOptions_UseComThreading);
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::GetPatternProvider(PATTERNID, IUnknown** provider) {
  if (!provider) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::GetPropertyValue(PROPERTYID property,
                                            VARIANT* value) {
  if (!value) {
    return E_INVALIDARG;
  }
  ::VariantInit(value);
  if (!attached()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  switch (property) {
    case UIA_IsControlElementPropertyId:
      return VariantBool(value, true);
    case UIA_ProviderDescriptionPropertyId:
      return VariantString(value, L"Phi window (hit-rect fragment root)");
    default:
      break;
  }
  // Name and control type are deliberately left to the HWND host provider
  // (get_HostRawElementProvider): the window text and the Window/Pane
  // distinction are the system's answer and it is already correct.
  return VariantEmpty(value);
}

IFACEMETHODIMP PhiUiaRoot::get_HostRawElementProvider(
    IRawElementProviderSimple** provider) {
  if (!provider) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  if (!attached() || !hwnd_) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const UiaApi& api = Uia();
  return api.ready ? api.host_from_hwnd(hwnd_, provider) : S_OK;
}

IFACEMETHODIMP PhiUiaRoot::Navigate(NavigateDirection direction,
                                    IRawElementProviderFragment** target) {
  if (!target) {
    return E_INVALIDARG;
  }
  *target = nullptr;
  if (!attached()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const int count = static_cast<int>(window_->hits().size());
  ComPtr<PhiUiaElement> child;
  switch (direction) {
    case NavigateDirection_FirstChild:
      child = ChildAt(0);
      break;
    case NavigateDirection_LastChild:
      child = ChildAt(count - 1);
      break;
    case NavigateDirection_Parent:
    case NavigateDirection_NextSibling:
    case NavigateDirection_PreviousSibling:
      // The HWND host provider owns everything above this window.
      break;
  }
  if (child) {
    *target = static_cast<IRawElementProviderFragment*>(child.Detach());
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::GetRuntimeId(SAFEARRAY** id) {
  if (!id) {
    return E_INVALIDARG;
  }
  // Null: the fragment root is hosted by an HWND, so the core derives its
  // runtime id from the window handle.
  *id = nullptr;
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::get_BoundingRectangle(UiaRect* rect) {
  if (!rect) {
    return E_INVALIDARG;
  }
  *rect = {};
  if (!attached() || !hwnd_) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  RECT window = {};
  ::GetWindowRect(hwnd_, &window);
  *rect = ToUiaRect(PhiFromRect(window));
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::GetEmbeddedFragmentRoots(SAFEARRAY** roots) {
  if (!roots) {
    return E_INVALIDARG;
  }
  *roots = nullptr;
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::SetFocus() {
  if (!attached() || !hwnd_) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (::GetFocus() != hwnd_) {
    ::SetFocus(hwnd_);
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::get_FragmentRoot(
    IRawElementProviderFragmentRoot** root) {
  if (!root) {
    return E_INVALIDARG;
  }
  *root = static_cast<IRawElementProviderFragmentRoot*>(this);
  AddRef();
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::ElementProviderFromPoint(
    double x,
    double y,
    IRawElementProviderFragment** target) {
  if (!target) {
    return E_INVALIDARG;
  }
  *target = nullptr;
  if (!attached() || !hwnd_) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  POINT screen = {static_cast<LONG>(x), static_cast<LONG>(y)};
  ::ScreenToClient(hwnd_, &screen);
  // THE hit test the mouse uses, not a second one.
  const int index = window_->HitTest(screen);
  if (ComPtr<PhiUiaElement> child = ChildAt(index)) {
    *target = static_cast<IRawElementProviderFragment*>(child.Detach());
  }
  return S_OK;
}

IFACEMETHODIMP PhiUiaRoot::GetFocus(IRawElementProviderFragment** target) {
  if (!target) {
    return E_INVALIDARG;
  }
  *target = nullptr;
  if (!attached()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  // The hit holding the surface's focus ring (PhiUiaSetFocusedHit), while
  // the window has the system focus; otherwise no hit takes keyboard focus
  // and the focused element is the window itself.
  if (::GetFocus() == hwnd_) {
    if (ComPtr<PhiUiaElement> child = ChildAt(FocusedHitOf(window_))) {
      *target = static_cast<IRawElementProviderFragment*>(child.Detach());
    }
  }
  return S_OK;
}

ComPtr<PhiUiaElement> PhiUiaRoot::ChildAt(int index) {
  if (!attached()) {
    return nullptr;
  }
  const std::vector<PhiHit>& hits = window_->hits();
  if (index < 0 || index >= static_cast<int>(hits.size())) {
    return nullptr;
  }
  ComPtr<PhiUiaElement> child;
  child.Attach(new PhiUiaElement(this, index,
                                 hits[static_cast<size_t>(index)].key));
  return child;
}

void PhiUiaRoot::RaiseProperty(int index, PROPERTYID property) {
  const UiaApi& api = Uia();
  if (!api.raise_property) {
    return;
  }
  ComPtr<PhiUiaElement> child = ChildAt(index);
  if (!child) {
    return;
  }
  VARIANT old_value = {};
  VARIANT new_value = {};
  ::VariantInit(&old_value);
  if (FAILED(child->GetPropertyValue(property, &new_value))) {
    ::VariantClear(&new_value);
    return;
  }
  // An empty "old" is allowed and is what a provider that does not keep the
  // previous VARIANT passes; clients read the new value.
  api.raise_property(static_cast<IRawElementProviderSimple*>(child.Get()),
                     property, old_value, new_value);
  ::VariantClear(&new_value);
  ::VariantClear(&old_value);
}

void PhiUiaRoot::AnnounceHits() {
  if (!attached()) {
    return;
  }
  const uint32_t version = window_->hits_version();
  if (announced_once_ && version == announced_version_) {
    return;  // No rebuild since the last look.
  }
  std::vector<PhiUiaHitState> now = PhiUiaSnapshot(window_->hits());
  const PhiUiaChanges changes =
      announced_once_ ? PhiUiaDiff(announced_, now) : PhiUiaChanges();
  announced_ = std::move(now);
  announced_version_ = version;
  const bool first = !announced_once_;
  announced_once_ = true;
  if (first) {
    // The first snapshot IS the tree the client just read; there is
    // nothing to announce against it.
    return;
  }
  if (!PhiUiaShouldRaise(ClientsAreListening(), changes)) {
    return;
  }
  const UiaApi& api = Uia();
  if (changes.structure) {
    if (api.raise_structure) {
      api.raise_structure(static_cast<IRawElementProviderSimple*>(this),
                          StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }
    return;  // The client re-reads the tree; per-element events would be
             // about elements it has already dropped.
  }
  for (int index : changes.renamed) {
    RaiseProperty(index, UIA_NamePropertyId);
  }
  for (int index : changes.reselected) {
    const std::vector<PhiHit>& hits = window_->hits();
    if (index < 0 || index >= static_cast<int>(hits.size())) {
      continue;
    }
    RaiseProperty(index,
                  hits[static_cast<size_t>(index)].role == PhiHitRole::kToggle
                      ? UIA_ToggleToggleStatePropertyId
                      : UIA_SelectionItemIsSelectedPropertyId);
  }
  for (int index : changes.reenabled) {
    RaiseProperty(index, UIA_IsEnabledPropertyId);
  }
}

void PhiUiaRoot::Detach() {
  if (!window_) {
    return;
  }
  const UiaApi& api = Uia();
  // Called BEFORE window_ is cleared: UiaDisconnectProvider asks the
  // provider to identify itself, so it has to still be able to answer.
  if (api.disconnect) {
    api.disconnect(static_cast<IRawElementProviderSimple*>(this));
  }
  window_ = nullptr;
  hwnd_ = nullptr;
  announced_.clear();
}

// ---------------------------------------------------------------------------
// The window → root table.
//
// PhiHitSource holds NO reference to its provider (and phi_window.h therefore
// pulls in no COM header): a window that no assistive client has ever
// asked about has no entry here at all, and the three seams below are an
// empty-map lookup. UI thread only.

std::map<PhiHitSource*, ComPtr<PhiUiaRoot>>& Roots() {
  static base::NoDestructor<std::map<PhiHitSource*, ComPtr<PhiUiaRoot>>> roots;
  return *roots;
}

PhiUiaRoot* FindRoot(PhiHitSource* window) {
  std::map<PhiHitSource*, ComPtr<PhiUiaRoot>>& roots = Roots();
  const auto it = roots.find(window);
  return it == roots.end() ? nullptr : it->second.Get();
}

}  // namespace

// ---------------------------------------------------------------------------
// The seams PhiHitSource calls.

LRESULT PhiUiaOnGetObject(PhiHitSource* window,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) {
  if (!window || !window->hwnd() ||
      static_cast<long>(lparam) != static_cast<long>(UiaRootObjectId)) {
    return 0;  // OBJID_CLIENT and friends stay with DefWindowProcW.
  }
  const UiaApi& api = Uia();
  if (!api.ready) {
    return 0;
  }
  std::map<PhiHitSource*, ComPtr<PhiUiaRoot>>& roots = Roots();
  ComPtr<PhiUiaRoot>& root = roots[window];
  if (!root) {
    root.Attach(new PhiUiaRoot(window));
  }
  // ALWAYS through UiaReturnRawElementProvider — it is what hands the core
  // a marshalled reference and what keeps the provider alive for the
  // client. Never return a raw pointer as the LRESULT.
  const LRESULT result = api.return_provider(
      window->hwnd(), wparam, lparam,
      static_cast<IRawElementProviderSimple*>(root.Get()));
  if (result == 0) {
    return 0;  // The core refused it; let DefWindowProcW answer.
  }
  *handled = true;
  return result;
}

void PhiUiaHitsChanged(PhiHitSource* window) {
  if (!window) {
    return;
  }
  if (PhiUiaRoot* root = FindRoot(window)) {
    root->AnnounceHits();
  }
}

void PhiUiaSetFocusedHit(PhiHitSource* window, int index, bool announce) {
  if (!window) {
    return;
  }
  const std::vector<PhiHit>& hits = window->hits();
  FocusedHit& focused = FocusedHits()[window];
  focused.index = index >= 0 && index < static_cast<int>(hits.size()) ? index
                                                                       : -1;
  focused.key =
      focused.index >= 0 ? hits[static_cast<size_t>(focused.index)].key : 0;
  if (!announce || focused.index < 0) {
    return;
  }
  // Raised only into a room someone is in, for a window a client has
  // already asked about (no root: nobody holds this tree), and while the
  // window actually has the system focus.
  PhiUiaRoot* root = FindRoot(window);
  const UiaApi& api = Uia();
  if (!root || !api.raise_event || !ClientsAreListening() ||
      ::GetFocus() != window->hwnd()) {
    return;
  }
  // A rebuild that came with the move is announced first, so the client
  // resolves the focused element in the tree it now has.
  root->AnnounceHits();
  if (ComPtr<PhiUiaElement> child = root->ChildAt(focused.index)) {
    api.raise_event(static_cast<IRawElementProviderSimple*>(child.Get()),
                    UIA_AutomationFocusChangedEventId);
  }
}

void PhiUiaWindowDestroyed(PhiHitSource* window) {
  if (!window) {
    return;
  }
  FocusedHits().erase(window);
  std::map<PhiHitSource*, ComPtr<PhiUiaRoot>>& roots = Roots();
  const auto it = roots.find(window);
  if (it == roots.end()) {
    return;
  }
  ComPtr<PhiUiaRoot> root = std::move(it->second);
  roots.erase(it);
  // The map's reference goes here; the client's may not. Whatever it still
  // holds now answers UIA_E_ELEMENTNOTAVAILABLE instead of reading a freed
  // PhiHitSource — the whole reason the provider is reference-counted and not
  // a member of the window.
  root->Detach();
}

}  // namespace phi_client

#endif  // defined(_WIN32)
