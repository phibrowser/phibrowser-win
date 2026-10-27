// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_feedback_window.h"

#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_feedback_image.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_shell_menus.h"
#include "phibrowser-win/client/phi_version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiFeedbackWindow";
// The canvas's "Send feedback" board (docs/windows-native-ui.md §13): a
// 560 wide window, 24 of margin, fields 20 apart; the height follows the
// content. Everything below is in design units under the caption band.
constexpr int kWidth = 560;
constexpr int kMargin = 24;
constexpr int kInner = kWidth - 2 * kMargin;  // 512
constexpr int kGap = 20;
constexpr int kLabelH = 20;
constexpr int kLabelGap = 6;
constexpr int kDescriptionH = 144;
constexpr int kFieldH = 32;
constexpr int kInfoBarH = 64;
constexpr int kTileH = 88;
constexpr int kTileGap = 8;
constexpr int kTileNameH = 34;
constexpr int kTileColumns = 4;
constexpr int kExpanderH = 60;
constexpr int kDiagnosticRowH = 24;
constexpr int kLegalLineH = 16;
constexpr int kFooterH = 64;
constexpr int kButtonW = 120;
constexpr int kSentH = 204;  // The confirmation above its footer.
// One wheel notch of the form's scroll, when the work area is too short
// for the whole form (the body scrolls between the band and the footer).
constexpr int kScrollStep = 48;
constexpr int kRadius = 4;
constexpr int kMaxDescriptionLength = 4096;
constexpr int kCounterThreshold = 3000;
// How long Send waits for Chromium's system logs before filing the report
// without them. The id comes from the window's own pool (logs_timer_).
constexpr UINT kLogsTimeoutMs = 15 * 1000;
constexpr UINT kProgressFrameMs = 33;

enum : UINT_PTR {
  kIdDescription = 101,
  kIdUrl,
  kIdUrlClear,
  kIdAdd,
  kIdDiagnostics,
  kIdSignIn,
  kIdErrorClose,
  kIdPrivacy,
  kIdTerms,
  kIdSend,
  kIdCancel,
  kIdTileBase = 200,  // + the attachment's index.
};

constexpr char kPrivacyUrl[] = "https://phibrowser.com/privacy/";
constexpr char kTermsUrl[] = "https://phibrowser.com/terms-of-service/";

PhiFeedbackWindow* g_window = nullptr;
// WM_CLOSE's wParam (unused by the system) marking a close that skips the
// discard prompt: CloseForSignOut.
constexpr WPARAM kCloseWithoutPrompt = 1;
int g_next_request = 0;

std::wstring Widen(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}
std::string Narrow(const std::wstring& wide) {
  return base::WideToUTF8(wide);
}

std::string Trim(const std::string& in) {
  const size_t begin = in.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = in.find_last_not_of(" \t\r\n");
  return in.substr(begin, end - begin + 1);
}

std::wstring WindowText(HWND hwnd) {
  const int length = ::GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = ::GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(std::max(copied, 0)));
  return text;
}

std::wstring Leaf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// "TXT" for notes.txt — the tile's stand-in for a file with no picture.
std::wstring Extension(const std::wstring& filename) {
  const size_t dot = filename.find_last_of(L'.');
  if (dot == std::wstring::npos || dot + 1 >= filename.size() ||
      filename.size() - dot > 6) {
    return std::wstring();
  }
  std::wstring ext = filename.substr(dot + 1);
  for (wchar_t& ch : ext) {
    ch = static_cast<wchar_t>(::towupper(ch));
  }
  return ext;
}

// The Mac's phiBrandEnsuredUrlString: chrome:// shows as phi://.
std::string BrandedUrl(const std::string& url) {
  if (url.rfind("chrome://", 0) == 0) {
    return "phi://" + url.substr(9);
  }
  return url;
}

std::wstring FormatSize(int64_t bytes) {
  if (bytes >= 1024 * 1024) {
    const int64_t tenths = bytes * 10 / (1024 * 1024);
    return std::to_wstring(tenths / 10) + L"." +
           std::to_wstring(tenths % 10) + L" MB";
  }
  if (bytes >= 1024) {
    return std::to_wstring((bytes + 512) / 1024) + L" KB";
  }
  return std::to_wstring(bytes) + L" B";
}

// A pasted image is written out for the viewer — the Mac's temporary
// preview file.
std::wstring WritePreviewFile(const std::wstring& filename,
                              const std::string& bytes) {
  std::wstring temp(MAX_PATH, L'\0');
  const DWORD length =
      ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
  if (!length || length >= temp.size()) {
    return std::wstring();
  }
  temp.resize(length);
  const std::wstring dir = temp + L"PhiFeedbackPreviews";
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path =
      dir + L"\\" +
      Widen(base::Uuid::GenerateRandomV4().AsLowercaseString()) + L"-" +
      filename;
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }
  DWORD written = 0;
  ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
              nullptr);
  ::CloseHandle(f);
  return path;
}

// The paths an HDROP carries (a drop, or files copied in Explorer).
std::vector<std::wstring> DropPaths(HDROP drop) {
  std::vector<std::wstring> paths;
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    ::DragQueryFileW(drop, i, path.data(), length + 1);
    path.resize(length);
    paths.push_back(std::move(path));
  }
  return paths;
}

std::vector<std::wstring> DataObjectPaths(IDataObject* data) {
  FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium = {};
  if (!data || FAILED(data->GetData(&format, &medium))) {
    return {};
  }
  std::vector<std::wstring> paths;
  if (auto* drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal))) {
    paths = DropPaths(drop);
    ::GlobalUnlock(medium.hGlobal);
  }
  ::ReleaseStgMedium(&medium);
  return paths;
}

bool ClipboardHasImage() {
  return ::IsClipboardFormatAvailable(CF_BITMAP) ||
         ::IsClipboardFormatAvailable(CF_DIB);
}

// One line of text in `rect`, the given colour.
void Text(HDC dc,
          HFONT font,
          COLORREF ink,
          const std::wstring& text,
          RECT rect,
          UINT format = DT_LEFT | DT_TOP) {
  HGDIOBJ old = ::SelectObject(dc, font);
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, ink);
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              format | DT_NOPREFIX |
                  ((format & DT_WORDBREAK) ? 0 : DT_SINGLELINE));
  ::SelectObject(dc, old);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ old = ::SelectObject(dc, font);
  SIZE size = {};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &size);
  ::SelectObject(dc, old);
  return size.cx;
}

// The height `text` wraps to in `width` px.
int WrappedHeight(HDC dc, HFONT font, const std::wstring& text, int width) {
  HGDIOBJ old = ::SelectObject(dc, font);
  RECT r = {0, 0, width, 0};
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
  ::SelectObject(dc, old);
  return r.bottom;
}

void Glyph(HDC dc, const RECT& rect, const wchar_t* glyph, int px,
           COLORREF ink) {
  PhiDrawIconGlyph(dc, rect, glyph, px, ink);
}

}  // namespace

// ---------------------------------------------------------------------------
// Drops from Explorer: the tile row turns into the drop zone while files
// hover over the window, and the shell's drag image (IDropTargetHelper)
// keeps its "Copy" description.

class PhiFeedbackWindow::DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(PhiFeedbackWindow* owner) : owner_(owner) {
    ::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&helper_));
  }
  void Detach() { owner_ = nullptr; }

  // IUnknown.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *out = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (!left) {
      delete this;
    }
    return left;
  }

  // IDropTarget.
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,
                                      DWORD,
                                      POINTL pt,
                                      DWORD* effect) override {
    paths_ = DataObjectPaths(data);
    accept_ = owner_ && !paths_.empty() && !owner_->sent_ &&
              !owner_->submitting_;
    *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_ && owner_) {
      POINT p = {pt.x, pt.y};
      helper_->DragEnter(owner_->hwnd(), data, &p, *effect);
    }
    if (accept_) {
      owner_->SetDragging(true, static_cast<int>(paths_.size()));
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
    *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_) {
      POINT p = {pt.x, pt.y};
      helper_->DragOver(&p, *effect);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (helper_) {
      helper_->DragLeave();
    }
    if (owner_) {
      owner_->SetDragging(false, 0);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,
                                 DWORD,
                                 POINTL pt,
                                 DWORD* effect) override {
    *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_) {
      POINT p = {pt.x, pt.y};
      helper_->Drop(data, &p, *effect);
    }
    if (owner_) {
      owner_->SetDragging(false, 0);
      if (accept_) {
        owner_->AddFiles(paths_, /*interactive=*/true);
      }
    }
    return S_OK;
  }

 private:
  ~DropTarget() = default;

  RAW_PTR_EXCLUSION PhiFeedbackWindow* owner_;
  Microsoft::WRL::ComPtr<IDropTargetHelper> helper_;
  std::vector<std::wstring> paths_;
  bool accept_ = false;
  ULONG refs_ = 1;
};

// ---------------------------------------------------------------------------

// static
void PhiFeedbackWindow::Open(PhiFeedbackHost* host,
                             const PhiFeedbackContext& context,
                             HWND anchor) {
  if (g_window && g_window->alive() && !g_window->sent_) {
    // App-global, like the Mac's: rebind to the window reopening it so the
    // report's context follows the current window.
    g_window->Rebind(context);
    g_window->RefreshAccount();
    ::ShowWindow(g_window->hwnd(), SW_SHOWNORMAL);
    ::SetForegroundWindow(g_window->hwnd());
    ::SetFocus(g_window->description_);
    return;
  }
  if (g_window && g_window->alive()) {
    // The last report's confirmation is still up: a new report starts
    // fresh.
    g_window->Destroy();
  }
  auto* window = new PhiFeedbackWindow(host, context);
  window->DeleteWhenDestroyed();
  if (!window->OpenWindow()) {
    delete window;
    return;
  }
  g_window = window;
  // Centered on the invoking shell, as the Mac centers its panel.
  window->CenterOn(anchor);
  // The first layout capped the height to whichever monitor the window was
  // created on; CenterOn may have moved it to another.
  window->Layout();
  ::ShowWindow(window->hwnd(), SW_SHOW);
  ::SetForegroundWindow(window->hwnd());
  ::SetFocus(window->description_);
}

// static
void PhiFeedbackWindow::OnSystemLogs(int request_id, const char* text) {
  if (!g_window || !g_window->pending_request_ ||
      g_window->pending_request_ != request_id) {
    return;
  }
  g_window->logs_timer_.Stop();
  if (!text) {
    LOG(WARNING) << "[Feedback] Chromium system logs skipped because the "
                 << "framework returned no text";
  }
  g_window->CompleteSend(text);
}

// static
HWND PhiFeedbackWindow::Handle() {
  return g_window ? g_window->hwnd() : nullptr;
}

// static
void PhiFeedbackWindow::CloseForSignOut() {
  if (const HWND hwnd = Handle()) {
    ::PostMessageW(hwnd, WM_CLOSE, kCloseWithoutPrompt, 0);
  }
}

// static
void PhiFeedbackWindow::OnAccountChanged() {
  if (g_window && g_window->alive()) {
    g_window->RefreshAccount();
  }
}

// static
std::string PhiFeedbackWindow::StateJson() {
  base::DictValue out;
  out.Set("open", g_window != nullptr);
  if (g_window) {
    out.Set("windowId", static_cast<double>(g_window->context_.window_id));
    out.Set("url", g_window->UrlText());
    out.Set("title", g_window->context_.title);
    out.Set("description", g_window->DescriptionText());
    out.Set("submitting", g_window->submitting_);
    out.Set("sent", g_window->sent_);
    out.Set("signedIn", g_window->signed_in_);
    out.Set("diagnosticsExpanded", g_window->diagnostics_expanded_);
    out.Set("canSend", !g_window->sent_ &&
                           ::IsWindowEnabled(g_window->send_) != 0);
    base::ListValue attachments;
    for (const Attachment& a : g_window->attachments_) {
      base::DictValue row;
      row.Set("filename", Narrow(a.draft.filename));
      row.Set("size", static_cast<double>(a.draft.size));
      row.Set("image", a.draft.is_image);
      row.Set("pasted", a.draft.file_path.empty());
      attachments.Append(std::move(row));
    }
    out.Set("attachments", std::move(attachments));
    out.Set("attachmentError", g_window->attachment_error_);
  }
  std::optional<std::string> json = base::WriteJson(out);
  return json ? *json : std::string("{}");
}

// static
std::string PhiFeedbackWindow::Drive(const std::string& description,
                              bool set_description,
                              const std::string& url,
                              bool set_url,
                              const std::vector<std::wstring>& attach_files,
                              bool send) {
  if (!g_window || g_window->sent_) {
    return "not_open";
  }
  if (set_description) {
    ::SetWindowTextW(g_window->description_, Widen(description).c_str());
    g_window->RefreshCounter();
    g_window->RefreshSendState();
  }
  if (set_url) {
    ::SetWindowTextW(g_window->url_, Widen(url).c_str());
  }
  if (!attach_files.empty()) {
    g_window->AddFiles(attach_files, /*interactive=*/false);
  }
  if (send) {
    if (Trim(g_window->DescriptionText()).empty()) {
      return "empty_description";
    }
    std::string email;
    if (!g_window->host_->FeedbackAccount(&email)) {
      return "no_account";
    }
    if (!g_window->Send(/*interactive=*/false)) {
      return "not_sent";
    }
  }
  return std::string();
}

PhiFeedbackWindow::PhiFeedbackWindow(PhiFeedbackHost* host,
                                     const PhiFeedbackContext& context)
    : host_(host), context_(context) {}

bool PhiFeedbackWindow::OpenWindow() {
  ClassSpec spec;
  spec.name = kClassName;

  Options options;
  // The Mac window is closable only; Windows keeps the minimize it had.
  options.caption_buttons = kPhiCaptionClose | kPhiCaptionMinimize;
  // Tab walks the control set from the MAIN loop. Escape, Return and
  // Ctrl+Return mean something specific here and stay in ControlProc.
  options.dialog_keys = kPhiDialogTab;

  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                       bounds, nullptr, PhiStr("feedback.window.title"),
                       options)) {
    LOG(ERROR) << "[Feedback] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  OnColorsChanged();
  logs_timer_ = NewTimer();
  progress_timer_ = NewTimer();
  signed_in_ = host_->FeedbackAccount(&email_);
  Build();
  Rebind(context_);
  // Files dropped from Explorer attach. OLE gives the drop-zone highlight
  // and the shell's drag image; WM_DROPFILES is the fallback when the
  // thread has no OLE.
  drop_target_ = new DropTarget(this);
  if (FAILED(::RegisterDragDrop(hwnd(), drop_target_))) {
    drop_target_->Detach();
    drop_target_->Release();
    drop_target_ = nullptr;
    ::DragAcceptFiles(hwnd(), TRUE);
  }
  return true;
}

PhiFeedbackWindow::~PhiFeedbackWindow() {
  for (HFONT font :
       {font_, title_font_, strong_font_, small_font_, tiny_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (HBRUSH brush :
       {bg_brush_, card_brush_, field_brush_, disabled_brush_}) {
    if (brush) {
      ::DeleteObject(brush);
    }
  }
  for (const Attachment& a : attachments_) {
    if (!a.preview_path.empty()) {
      ::DeleteFileW(a.preview_path.c_str());
    }
  }
}

HWND PhiFeedbackWindow::Child(const wchar_t* cls,
                              const wchar_t* text,
                              DWORD style,
                              UINT_PTR id,
                              DWORD ex_style) {
  HWND control = ::CreateWindowExW(
      ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
      hwnd(), reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr),
      nullptr);
  if (control) {
    // Keyboard: Escape closes, Ctrl+Enter sends, Ctrl+V attaches, Delete
    // removes a tile; the pointer: the owner-drawn controls' hover — the
    // manners a modeless window does not get for free.
    ::SetWindowSubclass(control, &PhiFeedbackWindow::ControlProc, id,
                        reinterpret_cast<DWORD_PTR>(this));
  }
  return control;
}

void PhiFeedbackWindow::EnsureFonts() {
  // The §2.2 ramp: 20 semibold title, 14 body, 12 captions, 11 tile sizes.
  // Rebuilt on every DPI move.
  for (HFONT* slot :
       {&font_, &title_font_, &strong_font_, &small_font_, &tiny_font_}) {
    if (*slot) {
      ::DeleteObject(*slot);
      *slot = nullptr;
    }
  }
  font_ = PhiPopupFont(dpi(), 14);
  title_font_ = PhiPopupFont(dpi(), 20, FW_SEMIBOLD);
  strong_font_ = PhiPopupFont(dpi(), 14, FW_SEMIBOLD);
  small_font_ = PhiPopupFont(dpi(), 12);
  tiny_font_ = PhiPopupFont(dpi(), 11);
}

// Gives every child the font its role calls for; re-run after a DPI move.
void PhiFeedbackWindow::ApplyFonts() {
  for (HWND child = ::GetWindow(hwnd(), GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    const UINT_PTR id = static_cast<UINT_PTR>(::GetDlgCtrlID(child));
    HFONT font = (id == kIdPrivacy || id == kIdTerms) ? small_font_ : font_;
    if (font) {
      ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
  }
}

void PhiFeedbackWindow::Build() {
  EnsureFonts();

  // Creation order is the Tab order: the InfoBar's Sign in, the fields,
  // the refused-files ✕, the tiles (inserted after it), Add files, the
  // expander, the links, Send, Cancel.
  sign_in_ = Child(L"BUTTON", PhiStr("feedback.form.signIn.button"),
                   WS_TABSTOP | BS_PUSHBUTTON, kIdSignIn);
  description_ = Child(L"EDIT", L"",
                       WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                           ES_AUTOVSCROLL | ES_WANTRETURN,
                       kIdDescription);
  ::SendMessageW(description_, EM_SETLIMITTEXT, kMaxDescriptionLength, 0);
  url_ = Child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdUrl);
  url_clear_ = Child(L"BUTTON", PhiStr("feedback.form.page.clear"),
                     BS_OWNERDRAW, kIdUrlClear);
  error_close_ = Child(L"BUTTON", PhiStr("feedback.form.attachmentFailure.dismiss"),
                       BS_OWNERDRAW, kIdErrorClose);
  add_ = Child(L"BUTTON", PhiStr("feedback.form.attachments.add"),
               WS_TABSTOP | BS_OWNERDRAW, kIdAdd);
  diagnostics_ = Child(L"BUTTON", PhiStr("feedback.form.diagnostics.title"),
                       WS_TABSTOP | BS_OWNERDRAW, kIdDiagnostics);
  privacy_ = Child(L"BUTTON", PhiStr("feedback.form.privacyPolicyLink"),
                   WS_TABSTOP | BS_PUSHBUTTON, kIdPrivacy);
  terms_ = Child(L"BUTTON", PhiStr("feedback.form.termsOfServiceLink"),
                 WS_TABSTOP | BS_PUSHBUTTON, kIdTerms);
  send_ = Child(L"BUTTON", PhiStr("feedback.form.sendButton"),
                WS_TABSTOP | BS_DEFPUSHBUTTON, kIdSend);
  cancel_ = Child(L"BUTTON", PhiStr("feedback.form.cancelButton"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);

  ApplyFonts();
  PhiPopupStyleButton(sign_in_, PhiPopupButtonRole::kPrimary);
  PhiPopupStyleButton(send_, PhiPopupButtonRole::kPrimary);
  PhiPopupStyleButton(cancel_, PhiPopupButtonRole::kSecondary);
  PhiPopupStyleButton(privacy_, PhiPopupButtonRole::kLink);
  PhiPopupStyleButton(terms_, PhiPopupButtonRole::kLink);
  for (HWND edit : {description_, url_}) {
    ::SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    PhiPopupThemeControl(edit, colors().dark);
  }
  RebuildTiles();  // Lays out.
  RefreshCounter();
  RefreshSendState();
}

void PhiFeedbackWindow::LayoutLegal(HDC dc, int x, int* y, int width) {
  // The notice flows word by word; the two links are buttons placed where
  // their words land, so they stay real tab stops inside the sentence.
  const std::wstring pattern = PhiStr("feedback.form.privacyNotice2");
  struct Piece {
    std::wstring text;
    HWND link;
  };
  std::vector<Piece> pieces;
  size_t at = 0;
  while (at < pattern.size()) {
    const size_t open = pattern.find(L'{', at);
    const size_t stop = open == std::wstring::npos ? pattern.size() : open;
    // Plain words, each keeping its trailing space.
    size_t word = at;
    while (word < stop) {
      size_t end = pattern.find(L' ', word);
      end = (end == std::wstring::npos || end >= stop) ? stop : end + 1;
      pieces.push_back({pattern.substr(word, end - word), nullptr});
      word = end;
    }
    if (open == std::wstring::npos) {
      break;
    }
    const size_t close = pattern.find(L'}', open);
    if (close == std::wstring::npos) {
      break;
    }
    const std::wstring slot = pattern.substr(open + 1, close - open - 1);
    pieces.push_back({slot == L"0" ? PhiStr("feedback.form.privacyPolicyLink")
                                   : PhiStr("feedback.form.termsOfServiceLink"),
                      slot == L"0" ? privacy_ : terms_});
    at = close + 1;
  }

  const int band = CaptionHeight();
  const int left = Scaled(x);
  const int right = Scaled(x + width);
  const int line_h = Scaled(kLegalLineH);
  int px = left;
  int line = 0;
  geo_.legal_runs.clear();
  for (const Piece& piece : pieces) {
    const int w = TextWidth(dc, small_font_, piece.text);
    // A trailing space may hang past the edge.
    const int ink_w =
        !piece.text.empty() && piece.text.back() == L' '
            ? TextWidth(dc, small_font_,
                        piece.text.substr(0, piece.text.size() - 1))
            : w;
    if (px > left && px + ink_w > right) {
      ++line;
      px = left;
    }
    const int top = band + Scaled(*y) + line * line_h - scroll_;
    const RECT r = {px, top, px + w, top + line_h};
    if (piece.link) {
      ::SetWindowPos(piece.link, nullptr, r.left, r.top, w, line_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
      geo_.legal_runs.push_back({piece.text, r});
    }
    px += w;
  }
  geo_.legal = {left, band + Scaled(*y) - scroll_, right,
                band + Scaled(*y) - scroll_ + (line + 1) * line_h};
  *y += (line + 1) * kLegalLineH;
}

void PhiFeedbackWindow::Layout() {
  const int band = CaptionHeight();
  // The form's body, scrolled when the work area is too short for it.
  const auto R = [&](int x, int y, int w, int h) {
    return RECT{Scaled(x), band + Scaled(y) - scroll_, Scaled(x + w),
                band + Scaled(y + h) - scroll_};
  };
  const auto place = [&](HWND control, const RECT& r, bool show) {
    if (!control) {
      return;
    }
    ::SetWindowPos(control, nullptr, r.left, r.top, r.right - r.left,
                   r.bottom - r.top,
                   SWP_NOZORDER | SWP_NOACTIVATE |
                       (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  };
  const auto show = [](HWND control, bool visible) {
    if (control) {
      ::ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }
  };
  geo_ = Geometry();
  const bool form = !sent_;
  int y = 12;

  if (sent_) {
    y = kSentH;
  } else {
    if (!signed_in_) {
      geo_.sign_in_bar = R(kMargin, y, kInner, kInfoBarH);
      place(sign_in_, R(kMargin + kInner - 12 - 88, y + 18, 88, 28), true);
      y += kInfoBarH + 16;
    }
    geo_.title = R(kMargin, y, kInner, 28);
    y += 28;
    if (signed_in_) {
      geo_.from = R(kMargin, y + 4, kInner, 20);
      y += 24;
    }
    y += kGap;
    geo_.description_label = R(kMargin, y, kInner, kLabelH);
    y += kLabelH + kLabelGap;
    geo_.description_box = R(kMargin, y, kInner, kDescriptionH);
    // The edit sits inside the drawn box: 11 of text inset, its scrollbar
    // against the right edge.
    place(description_,
          R(kMargin + 11, y + 6, kInner - 13, kDescriptionH - 10), true);
    y += kDescriptionH + 6;
    geo_.help = R(kMargin, y, kInner, 16);
    y += 16 + kGap;
    geo_.page_label = R(kMargin, y, kInner, kLabelH);
    y += kLabelH + kLabelGap;
    geo_.page_box = R(kMargin, y, kInner, kFieldH);
    place(url_, R(kMargin + 36, y + 6, kInner - 36 - 34, 20), true);
    place(url_clear_, R(kMargin + kInner - 28, y + 4, 24, 24), true);
    y += kFieldH + kGap;

    if (!error_text_.empty()) {
      HDC dc = ::GetDC(hwnd());
      const int text_w = Scaled(kInner - 42 - 44);
      const int message_h = WrappedHeight(dc, font_, error_text_, text_w);
      ::ReleaseDC(hwnd(), dc);
      const int h = 12 + 20 + (message_h * 96 + static_cast<int>(dpi()) - 1) /
                                  static_cast<int>(dpi()) + 12;
      geo_.error_bar = R(kMargin, y, kInner, h);
      place(error_close_, R(kMargin + kInner - 8 - 28, y + 8, 28, 28), true);
      y += h + 12;
    }

    geo_.attach_label = R(kMargin, y, kInner, kLabelH);
    y += kLabelH + kLabelGap;
    const bool can_add = static_cast<int>(attachments_.size()) <
                         PhiFeedbackOutbox::kMaxSubmitAttachments;
    const int tiles = static_cast<int>(attachments_.size()) + (can_add ? 1 : 0);
    const int rows = (tiles + kTileColumns - 1) / kTileColumns;
    const int tile_w = (kInner - (kTileColumns - 1) * kTileGap) / kTileColumns;
    const int grid_h = rows * kTileH + (rows - 1) * kTileGap;
    geo_.tiles = R(kMargin, y, kInner, grid_h);
    const auto tile_rect = [&](int i) {
      return R(kMargin + (i % kTileColumns) * (tile_w + kTileGap),
               y + (i / kTileColumns) * (kTileH + kTileGap), tile_w, kTileH);
    };
    for (size_t i = 0; i < attachments_.size(); ++i) {
      place(attachments_[i].tile, tile_rect(static_cast<int>(i)), !dragging_);
    }
    place(add_, tile_rect(static_cast<int>(attachments_.size())),
          can_add && !dragging_);
    y += grid_h + kGap;

    const int body =
        diagnostics_expanded_
            ? 8 + static_cast<int>(diagnostic_rows_.size()) * kDiagnosticRowH +
                  12
            : 0;
    geo_.diagnostics = R(kMargin, y, kInner, kExpanderH + body);
    place(diagnostics_, R(kMargin, y, kInner, kExpanderH), true);
    y += kExpanderH + body + 16;

    HDC dc = ::GetDC(hwnd());
    LayoutLegal(dc, kMargin, &y, kInner);
    ::ReleaseDC(hwnd(), dc);
    y += 16;
  }
  show(sign_in_, form && !signed_in_);
  show(error_close_, form && !error_text_.empty());
  for (HWND control :
       {description_, url_, url_clear_, diagnostics_, privacy_, terms_}) {
    show(control, form);
  }
  if (!form) {
    show(add_, false);
    for (Attachment& a : attachments_) {
      show(a.tile, false);
    }
  }

  geo_.content_height = y + kFooterH;
  // Never taller than the work area: what does not fit comes off the
  // body, which then scrolls; the footer (Send / Cancel) stays pinned to
  // the bottom edge.
  SIZE outer = chrome().OuterSize(kWidth, geo_.content_height);
  const RECT work = WorkArea();
  const int over = std::max<int>(0, outer.cy - (work.bottom - work.top));
  outer.cy -= over;
  if (std::clamp(scroll_, 0, over) != scroll_) {
    // The form shrank (or the screen grew) under a scrolled body: lay out
    // again from the clamped offset.
    scroll_ = std::clamp(scroll_, 0, over);
    Layout();
    return;
  }
  max_scroll_ = over;
  // The footer, in px from the (possibly cut) body's end.
  const int footer_top = band + Scaled(y) - over;
  const auto F = [&](int x, int dy, int w, int h) {
    return RECT{Scaled(x), footer_top + Scaled(dy), Scaled(x + w),
                footer_top + Scaled(dy + h)};
  };
  geo_.footer = F(0, 0, kWidth, kFooterH);
  geo_.progress = F(kMargin, 30, 64, 3);
  if (sent_) {
    show(send_, false);
    place(cancel_, F(kWidth - kMargin - kButtonW, 16, kButtonW, 32), true);
  } else {
    place(send_, F(kWidth - kMargin - 2 * kButtonW - 8, 16, kButtonW, 32),
          true);
    place(cancel_, F(kWidth - kMargin - kButtonW, 16, kButtonW, 32), true);
  }
  ClipBodyControls();
  if (form) {
    RefreshCounter();
  }

  RECT now = {};
  ::GetWindowRect(hwnd(), &now);
  if (now.right - now.left != outer.cx || now.bottom - now.top != outer.cy) {
    ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Grown past the taskbar: back up into the work area.
    FitToWorkArea();
  }
  Invalidate();
}

void PhiFeedbackWindow::ClipBodyControls() {
  // Child windows paint over the parent, so a field scrolled under the
  // band or the footer must be cut to the body's viewport by a plain
  // rectangular window region (no curve, so no AA concern).
  if (max_scroll_ <= 0 && !clipped_children_) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const RECT view = {client.left, CaptionHeight(), client.right,
                     geo_.footer.top};
  for (HWND child = ::GetWindow(hwnd(), GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    if (child == send_ || child == cancel_) {
      continue;
    }
    RECT r = {};
    ::GetWindowRect(child, &r);
    POINT top_left = {r.left, r.top};
    POINT bottom_right = {r.right, r.bottom};
    ::ScreenToClient(hwnd(), &top_left);
    ::ScreenToClient(hwnd(), &bottom_right);
    const RECT box = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    RECT shown = {};
    const bool any = ::IntersectRect(&shown, &box, &view) != FALSE;
    if (max_scroll_ <= 0 || ::EqualRect(&shown, &box)) {
      ::SetWindowRgn(child, nullptr, TRUE);
      continue;
    }
    HRGN region =
        any ? ::CreateRectRgn(shown.left - box.left, shown.top - box.top,
                              shown.right - box.left, shown.bottom - box.top)
            : ::CreateRectRgn(0, 0, 0, 0);
    // The window owns the region from here.
    ::SetWindowRgn(child, region, TRUE);
  }
  clipped_children_ = max_scroll_ > 0;
}

void PhiFeedbackWindow::ScrollTo(int px) {
  const int next = std::clamp(px, 0, max_scroll_);
  if (next == scroll_) {
    return;
  }
  scroll_ = next;
  Layout();
}

void PhiFeedbackWindow::ScrollIntoView(HWND control) {
  if (max_scroll_ <= 0 || !control || control == send_ ||
      control == cancel_) {
    return;
  }
  // The fields' drawn boxes are a little larger than the EDITs inside.
  RECT r = {};
  if (control == description_) {
    r = geo_.description_box;
  } else if (control == url_) {
    r = geo_.page_box;
  } else {
    ::GetWindowRect(control, &r);
    POINT top_left = {r.left, r.top};
    POINT bottom_right = {r.right, r.bottom};
    ::ScreenToClient(hwnd(), &top_left);
    ::ScreenToClient(hwnd(), &bottom_right);
    r = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  }
  const int margin = Scaled(12);
  const int view_top = CaptionHeight() + margin;
  const int view_bottom = geo_.footer.top - margin;
  if (r.top < view_top) {
    ScrollTo(scroll_ - (view_top - r.top));
  } else if (r.bottom > view_bottom) {
    // Taller than the view: its top edge wins.
    ScrollTo(scroll_ + std::min<int>(r.bottom - view_bottom, r.top - view_top));
  }
}

void PhiFeedbackWindow::OnWheel(int delta, POINT) {
  if (max_scroll_ > 0) {
    ScrollTo(scroll_ - delta * Scaled(kScrollStep) / WHEEL_DELTA);
  }
}

// --- Painting --------------------------------------------------------------

COLORREF PhiFeedbackWindow::Ink(int alpha) const {
  return PhiPopupBlend(colors().ink, colors().bg, alpha);
}

COLORREF PhiFeedbackWindow::FieldFill() const {
  return colors().button;  // Fluent's control fill: #FBFBFB / #333333.
}

void PhiFeedbackWindow::DrawFocusRing(HDC dc, const RECT& r, int radius) {
  PhiStrokeRoundRectAA(dc, r, radius, colors().label, Scaled(2));
}

void PhiFeedbackWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  PaintBand(dc);
  // The body paints only between the band and the footer (it scrolls
  // there when the work area is short).
  const int saved = ::SaveDC(dc);
  ::IntersectClipRect(dc, client.left, CaptionHeight(), client.right,
                      geo_.footer.top);
  if (sent_) {
    PaintSent(dc);
  } else {
    PaintForm(dc);
  }
  ::RestoreDC(dc, saved);
  PaintFooter(dc);
  if (max_scroll_ > 0) {
    // A thin overlay thumb on the body's trailing edge, as Fluent's
    // ScrollViewer shows while it can scroll.
    const int view_top = CaptionHeight();
    const int view_h = geo_.footer.top - view_top;
    const int total = view_h + max_scroll_;
    const int thumb_h = std::max(
        Scaled(24),
        static_cast<int>(static_cast<int64_t>(view_h) * view_h /
                         std::max(1, total)));
    const int thumb_top =
        view_top + static_cast<int>(static_cast<int64_t>(view_h - thumb_h) *
                                    scroll_ / max_scroll_);
    const RECT thumb = {client.right - Scaled(6), thumb_top + Scaled(2),
                        client.right - Scaled(3),
                        thumb_top + thumb_h - Scaled(2)};
    PhiFillCapsuleAA(dc, thumb, Ink(110));
  }
}

void PhiFeedbackWindow::PaintTextBox(HDC dc, const RECT& box, HWND edit) {
  const bool focused = ::GetFocus() == edit;
  const bool enabled = ::IsWindowEnabled(edit) != 0;
  const int radius = Scaled(kRadius);
  const PhiPopupColors& c = colors();
  const COLORREF fill = focused   ? c.card
                        : enabled ? FieldFill()
                                  : PhiPopupBlend(c.card, c.bg, 128);
  PhiFillRoundRectAA(dc, box, radius, fill);
  PhiStrokeRoundRectAA(dc, box, radius, enabled ? Ink(41) : Ink(23), 1);
  if (focused) {
    // Fluent's focus: a 2 dip accent edge along the bottom.
    RECT edge = {box.left + 1, box.bottom - Scaled(2), box.right - 1,
                 box.bottom};
    PhiFillRoundRectAA(dc, edge, Scaled(1), c.accent);
  } else if (enabled) {
    RECT edge = {box.left + radius, box.bottom - 1, box.right - radius,
                 box.bottom};
    HBRUSH b = ::CreateSolidBrush(Ink(107));
    ::FillRect(dc, &edge, b);
    ::DeleteObject(b);
  }
}

void PhiFeedbackWindow::PaintInfoBar(HDC dc,
                                     const RECT& bar,
                                     bool error,
                                     const std::wstring& title,
                                     const std::wstring& message,
                                     int trailing_px) {
  const PhiPopupColors& c = colors();
  const COLORREF fill = error ? (c.dark ? RGB(0x44, 0x27, 0x26)
                                        : RGB(0xFD, 0xE7, 0xE9))
                              : c.card;
  const COLORREF badge = error ? (c.dark ? RGB(0xFF, 0x99, 0xA4)
                                         : RGB(0xC4, 0x2B, 0x1C))
                               : c.accent;
  PhiFillRoundRectAA(dc, bar, Scaled(kRadius), fill);
  PhiStrokeRoundRectAA(dc, bar, Scaled(kRadius), c.card_border, 1);
  const RECT icon = {bar.left + Scaled(14), bar.top + Scaled(14),
                     bar.left + Scaled(30), bar.top + Scaled(30)};
  PhiFillEllipseAA(dc, icon, badge);
  if (error) {
    Glyph(dc, icon, fluent::kCancel, Scaled(8), fill);
  } else {
    Text(dc, strong_font_,
         c.dark ? RGB(0x0B, 0x1A, 0x22) : RGB(0xFF, 0xFF, 0xFF), L"i", icon,
         DT_CENTER | DT_VCENTER);
  }
  const int text_left = bar.left + Scaled(42);
  const int text_right = bar.right - trailing_px;
  Text(dc, strong_font_, c.label, title,
       {text_left, bar.top + Scaled(12), text_right, bar.top + Scaled(32)});
  Text(dc, font_, c.label, message,
       {text_left, bar.top + Scaled(32), text_right, bar.bottom},
       DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void PhiFeedbackWindow::PaintForm(HDC dc) {
  const PhiPopupColors& c = colors();
  if (!signed_in_) {
    PaintInfoBar(dc, geo_.sign_in_bar, /*error=*/false,
                 PhiStr("feedback.form.signIn.title"),
                 PhiStr("feedback.form.signIn.message"), Scaled(12 + 88 + 12));
  }
  Text(dc, title_font_, c.ink, PhiStr("feedback.form.heading"), geo_.title,
       DT_LEFT | DT_VCENTER);
  if (signed_in_) {
    // The account the report goes out under: an initial disc and the
    // address.
    const RECT disc = {geo_.from.left, geo_.from.top,
                       geo_.from.left + Scaled(20), geo_.from.top + Scaled(20)};
    PhiFillEllipseAA(dc, disc, c.accent);
    std::wstring initial = email_.empty()
                               ? std::wstring(L"?")
                               : std::wstring(1, static_cast<wchar_t>(
                                                     ::towupper(email_[0])));
    Text(dc, tiny_font_, c.dark ? RGB(0x0B, 0x1A, 0x22) : RGB(0xFF, 0xFF, 0xFF),
         initial, disc, DT_CENTER | DT_VCENTER);
    RECT line = geo_.from;
    line.left += Scaled(28);
    Text(dc, small_font_, c.secondary,
         PhiStrF("feedback.form.sendingAs", {Widen(email_)}), line,
         DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  }

  // A field label with its dim "(optional)" and a trailing aside.
  const auto label = [&](const RECT& r, const wchar_t* text, bool optional,
                         const wchar_t* aside) {
    Text(dc, font_, c.label, text, r, DT_LEFT | DT_VCENTER);
    if (optional) {
      RECT o = r;
      o.left += TextWidth(dc, font_, text) + Scaled(6);
      Text(dc, font_, c.tertiary, PhiStr("feedback.form.optional"), o,
           DT_LEFT | DT_VCENTER);
    }
    if (aside) {
      Text(dc, small_font_, c.tertiary, aside, r, DT_RIGHT | DT_VCENTER);
    }
  };
  label(geo_.description_label, PhiStr("feedback.form.description.label"),
        false, nullptr);
  PaintTextBox(dc, geo_.description_box, description_);
  Text(dc, small_font_, c.tertiary, PhiStr("feedback.form.description.help"),
       geo_.help, DT_LEFT | DT_VCENTER);
  const int length = ::GetWindowTextLengthW(description_);
  if (length >= kCounterThreshold) {
    const std::wstring counter =
        PhiFormatNumber(static_cast<long long>(length)) + L" / " +
        PhiFormatNumber(static_cast<long long>(kMaxDescriptionLength));
    Text(dc, small_font_, length >= kMaxDescriptionLength ? c.error : c.label,
         counter, geo_.help, DT_RIGHT | DT_VCENTER);
  }

  label(geo_.page_label, PhiStr("feedback.form.page.label"), true,
        PhiStr("feedback.form.page.aside"));
  PaintTextBox(dc, geo_.page_box, url_);
  const RECT globe = {geo_.page_box.left + Scaled(10), geo_.page_box.top,
                      geo_.page_box.left + Scaled(26), geo_.page_box.bottom};
  Glyph(dc, globe, fluent::kGlobe, Scaled(16),
        ::IsWindowEnabled(url_) ? c.secondary : c.tertiary);

  if (!error_text_.empty()) {
    PaintInfoBar(dc, geo_.error_bar, /*error=*/true,
                 PhiStr("feedback.form.attachmentFailure.heading"), error_text_,
                 Scaled(44));
  }

  label(geo_.attach_label, PhiStr("feedback.form.attachments.label"), true,
        PhiStr("feedback.form.attachments.aside"));
  if (dragging_) {
    // The drop zone: the whole tile row, accent-dashed on the accent wash.
    RECT zone = geo_.tiles;
    zone.bottom = std::max<LONG>(zone.bottom, zone.top + Scaled(kTileH));
    PhiFillRoundRectAA(dc, zone, Scaled(kRadius),
                       PhiPopupBlend(c.accent, c.bg, 26));
    PhiStrokeRoundRectDashedAA(dc, zone, Scaled(kRadius), c.accent, Scaled(2),
                               Scaled(6), Scaled(4));
    const std::wstring text =
        PhiStrN("feedback.form.attachments.drop", drag_count_, {});
    const int text_w = TextWidth(dc, font_, text);
    const int total = Scaled(20 + 10) + text_w;
    const int x = (zone.left + zone.right - total) / 2;
    Glyph(dc, {x, zone.top, x + Scaled(20), zone.bottom}, fluent::kUpload,
          Scaled(20), c.label);
    Text(dc, font_, c.label, text,
         {x + Scaled(30), zone.top, zone.right, zone.bottom},
         DT_LEFT | DT_VCENTER);
  }

  // The expander card: the header is the button; the body is painted here.
  if (diagnostics_expanded_) {
    PhiFillRoundRectAA(dc, geo_.diagnostics, Scaled(kRadius), c.card);
    PhiStrokeRoundRectAA(dc, geo_.diagnostics, Scaled(kRadius), c.card_border,
                         1);
    RECT sep = {geo_.diagnostics.left + 1,
                geo_.diagnostics.top + Scaled(kExpanderH),
                geo_.diagnostics.right - 1,
                geo_.diagnostics.top + Scaled(kExpanderH) + 1};
    HBRUSH b = ::CreateSolidBrush(c.separator);
    ::FillRect(dc, &sep, b);
    ::DeleteObject(b);
    int row_y = sep.bottom + Scaled(8);
    const int key_x = geo_.diagnostics.left + Scaled(52);
    const int value_x = key_x + Scaled(140);
    for (const auto& [key, value] : diagnostic_rows_) {
      const int bottom = row_y + Scaled(kDiagnosticRowH);
      Text(dc, small_font_, c.secondary, key,
           {key_x, row_y, value_x - Scaled(8), bottom},
           DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
      Text(dc, small_font_, c.label, value,
           {value_x, row_y, geo_.diagnostics.right - Scaled(16), bottom},
           DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
      row_y = bottom;
    }
  }

  for (const Geometry::Run& run : geo_.legal_runs) {
    Text(dc, small_font_, c.secondary, run.text, run.rect,
         DT_LEFT | DT_VCENTER);
  }
}

void PhiFeedbackWindow::PaintSent(HDC dc) {
  const PhiPopupColors& c = colors();
  const int band = CaptionHeight();
  const int cx = Scaled(kWidth) / 2;
  const RECT disc = {cx - Scaled(24), band + Scaled(36), cx + Scaled(24),
                     band + Scaled(84)};
  PhiFillEllipseAA(dc, disc,
                   c.dark ? RGB(0x39, 0x3D, 0x1B) : RGB(0xDF, 0xF6, 0xDD));
  Glyph(dc, disc, fluent::kCheckMark, Scaled(24),
        c.dark ? RGB(0x6C, 0xCB, 0x5F) : RGB(0x0F, 0x7B, 0x0F));
  Text(dc, title_font_, c.ink, PhiStr("feedback.form.sent.title"),
       {Scaled(kMargin), band + Scaled(96), Scaled(kWidth - kMargin),
        band + Scaled(124)},
       DT_CENTER | DT_VCENTER);
  Text(dc, font_, c.secondary, PhiStr("feedback.form.sent.message"),
       {Scaled(48), band + Scaled(132), Scaled(kWidth - 48), band + Scaled(184)},
       DT_CENTER | DT_TOP | DT_WORDBREAK);
}

void PhiFeedbackWindow::PaintFooter(HDC dc) {
  const PhiPopupColors& c = colors();
  ::FillRect(dc, &geo_.footer, card_brush_);
  RECT sep = geo_.footer;
  sep.bottom = sep.top + 1;
  HBRUSH b = ::CreateSolidBrush(c.separator);
  ::FillRect(dc, &sep, b);
  ::DeleteObject(b);
  if (!submitting_) {
    return;
  }
  // Fluent's indeterminate ProgressBar: a 40 % segment sliding along the
  // track, then the status.
  const RECT track = geo_.progress;
  PhiFillCapsuleAA(dc, track, PhiPopupBlend(c.ink, c.card, 23));
  const int w = track.right - track.left;
  const int seg = w * 2 / 5;
  const int start = track.left - seg + (w + seg) * progress_phase_ / 100;
  RECT bar = {std::max<LONG>(start, track.left), track.top,
              std::min<LONG>(start + seg, track.right), track.bottom};
  if (bar.right > bar.left) {
    PhiFillCapsuleAA(dc, bar, c.accent);
  }
  Text(dc, small_font_, c.secondary, PhiStr("feedback.form.collecting"),
       {track.right + Scaled(10), geo_.footer.top, geo_.footer.right,
        geo_.footer.bottom},
       DT_LEFT | DT_VCENTER);
}

// --- Owner-drawn controls --------------------------------------------------

bool PhiFeedbackWindow::DrawOwnerItem(const DRAWITEMSTRUCT& item) {
  if (item.CtlType != ODT_BUTTON) {
    return false;
  }
  const UINT_PTR id = item.CtlID;
  if (id == kIdAdd) {
    DrawAddTile(item);
    return true;
  }
  if (id == kIdDiagnostics) {
    DrawDiagnosticsHeader(item);
    return true;
  }
  if (id == kIdUrlClear || id == kIdErrorClose) {
    DrawGlyphButton(item, fluent::kCancel);
    return true;
  }
  if (id >= kIdTileBase && id < kIdTileBase + attachments_.size()) {
    DrawTile(item, id - kIdTileBase);
    return true;
  }
  return false;
}

void PhiFeedbackWindow::DrawTile(const DRAWITEMSTRUCT& item, size_t index) {
  Attachment& a = attachments_[index];
  const PhiPopupColors& c = colors();
  HDC dc = item.hDC;
  const RECT r = item.rcItem;
  const int radius = Scaled(kRadius);
  const bool hot = hot_ == item.hwndItem;
  const bool enabled = (item.itemState & ODS_DISABLED) == 0;
  ::FillRect(dc, &r, bg_brush_);
  PhiFillRoundRectAA(dc, r, radius, c.card);

  RECT thumb = {r.left + 1, r.top + 1, r.right - 1,
                r.bottom - Scaled(kTileNameH)};
  const COLORREF well = PhiPopupBlend(c.ink, c.card, c.dark ? 20 : 15);
  bool drew = false;
  if (a.draft.is_image) {
    EnsureThumbnail(&a, {thumb.right - thumb.left, thumb.bottom - thumb.top});
    if (!a.thumb.empty()) {
      drew = PhiBlendBgra(dc, thumb, a.thumb.data(), a.thumb_size.cx,
                          a.thumb_size.cy);
    }
  }
  if (!drew) {
    HBRUSH b = ::CreateSolidBrush(well);
    ::FillRect(dc, &thumb, b);
    ::DeleteObject(b);
    const int mid = (thumb.top + thumb.bottom) / 2;
    Glyph(dc, {thumb.left, mid - Scaled(20), thumb.right, mid + Scaled(6)},
          fluent::kDocument, Scaled(24),
          c.secondary);
    Text(dc, tiny_font_, c.secondary, Extension(a.draft.filename),
         {thumb.left, mid + Scaled(6), thumb.right, mid + Scaled(20)},
         DT_CENTER | DT_VCENTER);
  }
  PhiKnockOutRoundRectAA(dc, r, radius, c.bg);
  PhiStrokeRoundRectAA(dc, r, radius, hot ? Ink(41) : c.card_border, 1);

  const COLORREF name_ink = enabled ? c.label : c.tertiary;
  RECT name = {r.left + Scaled(8), r.bottom - Scaled(kTileNameH) + Scaled(3),
               r.right - Scaled(8), r.bottom - Scaled(kTileNameH) + Scaled(18)};
  Text(dc, small_font_, name_ink, a.draft.filename, name,
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  const std::wstring size =
      a.draft.file_path.empty()
          ? PhiStrF("feedback.form.attachment.pastedSize",
                    {FormatSize(a.draft.size)})
          : FormatSize(a.draft.size);
  Text(dc, tiny_font_, c.tertiary, size,
       {name.left, name.bottom, name.right, name.bottom + Scaled(14)},
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

  if (hot && enabled) {
    // The hovered tile's remove button, a small flyout-coloured square.
    const RECT x = TileRemoveRect(item.hwndItem);
    const COLORREF flyout = c.dark ? RGB(0x2C, 0x2C, 0x2C) : RGB(0xF9, 0xF9, 0xF9);
    PhiFillRoundRectAA(dc, x, radius,
                       hot_remove_ ? PhiPopupBlend(c.ink, flyout, 20) : flyout);
    PhiStrokeRoundRectAA(dc, x, radius, Ink(41), 1);
    Glyph(dc, x, fluent::kCancel, Scaled(10), c.label);
  }
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    DrawFocusRing(dc, r, radius);
  }
}

void PhiFeedbackWindow::DrawAddTile(const DRAWITEMSTRUCT& item) {
  const PhiPopupColors& c = colors();
  HDC dc = item.hDC;
  const RECT r = item.rcItem;
  const int radius = Scaled(kRadius);
  const bool enabled = (item.itemState & ODS_DISABLED) == 0;
  ::FillRect(dc, &r, bg_brush_);
  if (enabled && (hot_ == item.hwndItem || (item.itemState & ODS_SELECTED))) {
    PhiFillRoundRectAA(dc, r, radius,
                       Ink((item.itemState & ODS_SELECTED) ? 23 : 11));
  }
  PhiStrokeRoundRectDashedAA(dc, r, radius, Ink(41), 1, Scaled(4), Scaled(3));
  const COLORREF ink = enabled ? c.label : c.tertiary;
  const int mid = (r.top + r.bottom) / 2;
  Glyph(dc, {r.left, mid - Scaled(30), r.right, mid - Scaled(10)},
        fluent::kAdd, Scaled(16), ink);
  Text(dc, small_font_, ink, PhiStr("feedback.form.attachments.add"),
       {r.left, mid - Scaled(8), r.right, mid + Scaled(10)},
       DT_CENTER | DT_VCENTER);
  Text(dc, tiny_font_, c.tertiary, PhiStr("feedback.form.attachments.addHint"),
       {r.left + Scaled(4), mid + Scaled(10), r.right - Scaled(4),
        mid + Scaled(26)},
       DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    DrawFocusRing(dc, r, radius);
  }
}

void PhiFeedbackWindow::DrawDiagnosticsHeader(const DRAWITEMSTRUCT& item) {
  const PhiPopupColors& c = colors();
  HDC dc = item.hDC;
  const RECT r = item.rcItem;
  const int radius = Scaled(kRadius);
  ::FillRect(dc, &r, bg_brush_);
  // Open, the card runs on below the header (the parent paints the body):
  // the header's bottom corners stay square by drawing the card taller.
  RECT card = r;
  if (diagnostics_expanded_) {
    card.bottom += radius * 2 + 2;
  }
  const bool hot = hot_ == item.hwndItem;
  COLORREF fill = c.card;
  if (item.itemState & ODS_SELECTED) {
    fill = PhiPopupBlend(c.ink, c.card, 15);
  } else if (hot) {
    fill = PhiPopupBlend(c.ink, c.card, 8);
  }
  PhiFillRoundRectAA(dc, card, radius, fill);
  PhiStrokeRoundRectAA(dc, card, radius, c.card_border, 1);
  Glyph(dc, {r.left + Scaled(16), r.top, r.left + Scaled(36), r.bottom},
        fluent::kDiagnostic, Scaled(20), c.label);
  const int text_left = r.left + Scaled(52);
  const int text_right = r.right - Scaled(52);
  Text(dc, font_, c.label, PhiStr("feedback.form.diagnostics.title"),
       {text_left, r.top + Scaled(10), text_right, r.top + Scaled(30)},
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  Text(dc, small_font_, c.secondary,
       PhiStr("feedback.form.diagnostics.subtitle"),
       {text_left, r.top + Scaled(30), text_right, r.top + Scaled(48)},
       DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  const RECT chevron = {r.right - Scaled(44), r.top, r.right - Scaled(12),
                        r.bottom};
  Glyph(dc, chevron,
        diagnostics_expanded_ ? fluent::kChevronUp : fluent::kChevronDown,
        Scaled(12), c.label);
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    DrawFocusRing(dc, r, radius);
  }
}

void PhiFeedbackWindow::DrawGlyphButton(const DRAWITEMSTRUCT& item,
                                        const wchar_t* glyph) {
  const PhiPopupColors& c = colors();
  HDC dc = item.hDC;
  const RECT r = item.rcItem;
  // The button stands on whatever its host painted: the text box's fill,
  // or the error InfoBar's.
  COLORREF under = c.card;
  if (item.CtlID == kIdUrlClear) {
    under = ::GetFocus() == url_ ? c.card : FieldFill();
  } else {
    under = c.dark ? RGB(0x44, 0x27, 0x26) : RGB(0xFD, 0xE7, 0xE9);
  }
  HBRUSH b = ::CreateSolidBrush(under);
  ::FillRect(dc, &r, b);
  ::DeleteObject(b);
  if (hot_ == item.hwndItem || (item.itemState & ODS_SELECTED)) {
    PhiFillRoundRectAA(dc, r, Scaled(kRadius),
                       PhiPopupBlend(c.ink, under,
                                     (item.itemState & ODS_SELECTED) ? 23 : 11));
  }
  Glyph(dc, r, glyph, Scaled(10), c.secondary);
}

RECT PhiFeedbackWindow::TileRemoveRect(HWND tile) const {
  RECT r = {};
  ::GetClientRect(tile, &r);
  return {r.right - Scaled(28), r.top + Scaled(4), r.right - Scaled(4),
          r.top + Scaled(28)};
}

void PhiFeedbackWindow::EnsureThumbnail(Attachment* a, SIZE px) {
  if (px.cx <= 0 || px.cy <= 0) {
    return;
  }
  if (a->thumb_size.cx == px.cx && a->thumb_size.cy == px.cy &&
      (!a->thumb.empty() || a->thumb_failed)) {
    return;
  }
  a->thumb_size = px;
  a->thumb.clear();
  a->thumb_failed = !PhiDecodeImageThumbnail(
      a->draft.file_path, a->draft.pasted_png, px.cx, px.cy, &a->thumb);
  if (a->thumb_failed) {
    a->thumb.clear();
  }
}

// --- State -----------------------------------------------------------------

void PhiFeedbackWindow::Rebind(const PhiFeedbackContext& context) {
  // An in-flight Send already captured its window and is awaiting the logs
  // (the Mac's rebindHost guard).
  if (submitting_) {
    return;
  }
  context_ = context;
  if (context_.incognito) {
    context_.url.clear();  // Nothing of an off-the-record page leaves it.
    context_.title.clear();
  }
  ::SetWindowTextW(url_, Widen(BrandedUrl(context_.url)).c_str());
  if (diagnostics_expanded_) {
    diagnostics_expanded_ = false;  // Its rows named the old window.
    Layout();
  }
}

void PhiFeedbackWindow::RefreshAccount() {
  std::string email;
  const bool signed_in = host_->FeedbackAccount(&email);
  if (signed_in == signed_in_ && email == email_) {
    return;
  }
  signed_in_ = signed_in;
  email_ = email;
  if (!sent_) {
    Layout();
    RefreshSendState();
  }
}

void PhiFeedbackWindow::RebuildTiles() {
  for (Attachment& a : attachments_) {
    if (a.tile) {
      ::DestroyWindow(a.tile);
      a.tile = nullptr;
    }
  }
  hot_ = nullptr;
  HWND after = error_close_;
  for (size_t i = 0; i < attachments_.size(); ++i) {
    Attachment& a = attachments_[i];
    a.tile = Child(L"BUTTON", a.draft.filename.c_str(),
                   WS_TABSTOP | BS_OWNERDRAW, kIdTileBase + i);
    ::EnableWindow(a.tile, !submitting_);
    // Tab order = z-order: the tiles follow the fields, before Add files.
    ::SetWindowPos(a.tile, after, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    after = a.tile;
  }
  Layout();
}

void PhiFeedbackWindow::RefreshSendState() {
  const bool can_send =
      !submitting_ && signed_in_ && !Trim(DescriptionText()).empty();
  ::EnableWindow(send_, can_send);
  ::SetWindowTextW(send_, submitting_ ? PhiStr("feedback.form.sending")
                                      : PhiStr("feedback.form.sendButton"));
  for (HWND control : {description_, url_, url_clear_, add_}) {
    ::EnableWindow(control, !submitting_);
  }
  for (const Attachment& a : attachments_) {
    ::EnableWindow(a.tile, !submitting_);
  }
  Invalidate();
}

void PhiFeedbackWindow::RefreshCounter() {
  // The counter shares the help line; repaint it.
  ::InvalidateRect(hwnd(), &geo_.help, FALSE);
  // The multi-line edit keeps a disabled scrollbar up even when the text
  // fits; Fluent shows one only when there is something to scroll.
  RECT text = {};
  ::SendMessageW(description_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
  HDC dc = ::GetDC(description_);
  HGDIOBJ old = ::SelectObject(dc, font_);
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(dc, &metrics);
  ::SelectObject(dc, old);
  ::ReleaseDC(description_, dc);
  const LRESULT lines = ::SendMessageW(description_, EM_GETLINECOUNT, 0, 0);
  const bool overflows =
      metrics.tmHeight > 0 &&
      lines * metrics.tmHeight > text.bottom - text.top;
  ::ShowScrollBar(description_, SB_VERT, overflows);
}

std::string PhiFeedbackWindow::DescriptionText() const {
  return Narrow(WindowText(description_));
}

std::string PhiFeedbackWindow::UrlText() const {
  return Trim(Narrow(WindowText(url_)));
}

bool PhiFeedbackWindow::ConfirmDiscard() {
  if (sent_ || Trim(DescriptionText()).empty()) {
    return true;
  }
  return PhiConfirm(hwnd(), PhiStr("feedback.form.discardAlert.title"),
                    PhiStr("feedback.form.discardAlert.message"),
                    PhiStr("feedback.form.discardAlert.discardButton"),
                    /*destructive=*/true);
}

std::string PhiFeedbackWindow::PageTitleForReport() const {
  const std::string url = UrlText();
  return !url.empty() && url == Trim(BrandedUrl(context_.url))
             ? context_.title
             : std::string();
}

void PhiFeedbackWindow::ShowAttachmentError(const std::wstring& text) {
  error_text_ = text;
  attachment_error_ = Narrow(text);
  Layout();
}

bool PhiFeedbackWindow::AddFiles(const std::vector<std::wstring>& paths,
                                 bool interactive) {
  std::vector<std::wstring> errors;
  const size_t before = attachments_.size();
  for (const std::wstring& path : paths) {
    if (static_cast<int>(attachments_.size()) >=
        PhiFeedbackOutbox::kMaxSubmitAttachments) {
      errors.push_back(PhiStrF("feedback.form.attachmentFailure.limit",
                               {Leaf(path)}));
      continue;
    }
    int64_t size = 0;
    std::string mime_type;
    bool is_image = false;
    std::string error;
    if (!PhiFeedbackOutbox::SelectedAttachmentInfo(path, &size, &mime_type,
                                                   &is_image, &error)) {
      errors.push_back(Leaf(path) + L": " + Widen(error));
      continue;
    }
    Attachment attachment;
    attachment.draft.filename = Leaf(path);
    attachment.draft.size = size;
    attachment.draft.is_image = is_image;
    attachment.draft.file_path = path;
    attachments_.push_back(std::move(attachment));
  }
  std::wstring shown;
  for (size_t i = 0; i < errors.size() && i < 3; ++i) {
    shown += (i ? L" " : L"") + errors[i];
  }
  if (errors.size() > 3) {
    shown += L" " + PhiStrN("feedback.form.attachmentFailure.more",
                            static_cast<long long>(errors.size() - 3), {});
  }
  // The InfoBar reports this attempt only; a clean attempt clears it.
  error_text_ = shown;
  attachment_error_ = Narrow(shown);
  if (attachments_.size() != before) {
    RebuildTiles();
  } else {
    Layout();
  }
  if (interactive && !attachments_.empty() && attachments_.size() != before) {
    ::SetFocus(attachments_.back().tile);
  }
  return errors.empty();
}

void PhiFeedbackWindow::ChooseFiles() {
  using Microsoft::WRL::ComPtr;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                     FOS_FILEMUSTEXIST);
  dialog->SetTitle(PhiStr("feedback.form.attachments.add"));
  if (FAILED(dialog->Show(hwnd()))) {
    return;  // Cancelled.
  }
  ComPtr<IShellItemArray> items;
  DWORD count = 0;
  if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count))) {
    return;
  }
  std::vector<std::wstring> paths;
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    wchar_t* path = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
      paths.emplace_back(path);
      ::CoTaskMemFree(path);
    }
  }
  AddFiles(paths, /*interactive=*/true);
}

bool PhiFeedbackWindow::PasteAttachments() {
  if (submitting_ || sent_) {
    return false;
  }
  if (::IsClipboardFormatAvailable(CF_HDROP) && ::OpenClipboard(hwnd())) {
    std::vector<std::wstring> paths;
    if (auto drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP))) {
      paths = DropPaths(drop);
    }
    ::CloseClipboard();
    if (!paths.empty()) {
      AddFiles(paths, /*interactive=*/true);
      return true;
    }
  }
  if (ClipboardHasImage()) {
    PasteImage();
    return true;
  }
  return false;
}

void PhiFeedbackWindow::PasteImage() {
  std::string png;
  if (!PhiClipboardImageToPng(&png)) {
    LOG(ERROR) << "[Feedback] paste image failed: could not encode image";
    return;
  }
  if (static_cast<int>(attachments_.size()) >=
      PhiFeedbackOutbox::kMaxSubmitAttachments) {
    ShowAttachmentError(
        PhiStrF("feedback.form.attachmentFailure.limit",
                {PhiStr("feedback.form.attachment.pastedImage")}));
    return;
  }
  int images = 0;
  for (const Attachment& a : attachments_) {
    images += a.draft.is_image ? 1 : 0;
  }
  Attachment attachment;
  attachment.draft.filename =
      L"image-" + std::to_wstring(images + 1) + L".png";
  attachment.draft.size = static_cast<int64_t>(png.size());
  attachment.draft.is_image = true;
  attachment.draft.pasted_png = std::move(png);
  attachments_.push_back(std::move(attachment));
  error_text_.clear();
  attachment_error_.clear();
  RebuildTiles();
}

void PhiFeedbackWindow::RemoveAttachment(size_t index) {
  if (index >= attachments_.size() || submitting_) {
    return;
  }
  Attachment& a = attachments_[index];
  if (!a.preview_path.empty()) {
    ::DeleteFileW(a.preview_path.c_str());
  }
  const bool had_focus = ::GetFocus() == a.tile;
  ::DestroyWindow(a.tile);
  a.tile = nullptr;
  attachments_.erase(attachments_.begin() + static_cast<ptrdiff_t>(index));
  RebuildTiles();
  if (had_focus) {
    // Focus stays in the row: the tile that moved into the slot, else the
    // one before, else Add files.
    HWND next = index < attachments_.size() ? attachments_[index].tile
                : !attachments_.empty()     ? attachments_.back().tile
                                            : add_;
    ::SetFocus(next);
  }
}

void PhiFeedbackWindow::OpenAttachment(size_t index) {
  if (index >= attachments_.size()) {
    return;
  }
  Attachment& a = attachments_[index];
  std::wstring path = a.draft.file_path;
  if (path.empty()) {
    if (a.preview_path.empty()) {
      a.preview_path = WritePreviewFile(a.draft.filename, a.draft.pasted_png);
    }
    path = a.preview_path;
  }
  if (!path.empty()) {
    ::ShellExecuteW(hwnd(), L"open", path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
  }
}

void PhiFeedbackWindow::RevealAttachment(size_t index) {
  if (index >= attachments_.size() ||
      attachments_[index].draft.file_path.empty()) {
    return;
  }
  const std::wstring args =
      L"/select,\"" + attachments_[index].draft.file_path + L"\"";
  ::ShellExecuteW(hwnd(), L"open", L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void PhiFeedbackWindow::ShowTileMenu(size_t index, POINT screen) {
  if (index >= attachments_.size()) {
    return;
  }
  PhiMenuSpec spec;
  spec.items.push_back(PhiMenuItem::Command(
      "feedback.open", PhiStr("feedback.form.attachment.open")));
  PhiMenuItem reveal = PhiMenuItem::Command(
      "feedback.reveal", PhiStr("feedback.form.attachment.showInExplorerTooltip"));
  reveal.enabled = !attachments_[index].draft.file_path.empty();
  spec.items.push_back(std::move(reveal));
  spec.items.push_back(PhiMenuItem::Separator());
  spec.items.push_back(PhiMenuItem::Command(
      "feedback.remove", PhiStr("feedback.form.attachment.remove")));
  PhiPopupMenuRequest request;
  request.owner = hwnd();
  request.x = screen.x;
  request.y = screen.y;
  request.palette = &menu_palette_;
  const PhiMenuAction action = PhiRunPopupMenu(spec, request);
  if (action.is("feedback.open")) {
    OpenAttachment(index);
  } else if (action.is("feedback.reveal")) {
    RevealAttachment(index);
  } else if (action.is("feedback.remove")) {
    RemoveAttachment(index);
  }
}

size_t PhiFeedbackWindow::TileIndex(HWND tile) const {
  for (size_t i = 0; i < attachments_.size(); ++i) {
    if (attachments_[i].tile == tile) {
      return i;
    }
  }
  return attachments_.size();
}

void PhiFeedbackWindow::SetDragging(bool dragging, int count) {
  if (dragging_ == dragging && drag_count_ == count) {
    return;
  }
  dragging_ = dragging;
  drag_count_ = count;
  Layout();
}

bool PhiFeedbackWindow::Send(bool interactive) {
  if (submitting_ || sent_ || Trim(DescriptionText()).empty()) {
    return false;
  }
  std::string email;
  if (!host_->FeedbackAccount(&email)) {
    // The sign-in InfoBar says why Send is off; bring it up to date.
    RefreshAccount();
    if (interactive) {
      ::SetFocus(sign_in_);
    }
    return false;
  }
  submitting_ = true;
  progress_phase_ = 0;
  progress_timer_.Start(kProgressFrameMs, /*coalescable=*/true);
  RefreshSendState();
  ::SetFocus(cancel_);
  pending_request_ = ++g_next_request;
  if (!host_->FetchFeedbackSystemLogs(context_.window_id, pending_request_)) {
    LOG(WARNING) << "[Feedback] Chromium system logs skipped because the "
                 << "framework will not answer";
    CompleteSend(nullptr);
    return true;
  }
  logs_timer_.Start(kLogsTimeoutMs);
  return true;
}

void PhiFeedbackWindow::CancelSend() {
  // The framework's late answer finds no pending request and is dropped.
  pending_request_ = 0;
  logs_timer_.Stop();
  progress_timer_.Stop();
  submitting_ = false;
  RefreshSendState();
  ::SetFocus(send_);
}

void PhiFeedbackWindow::CompleteSend(const char* logs) {
  pending_request_ = 0;
  PhiFeedbackDraft draft;
  draft.description = Trim(DescriptionText());
  draft.page_url = UrlText();
  draft.page_title = PageTitleForReport();
  draft.extension_versions = host_->PhiExtensionVersions(context_.window_id);
  if (logs) {
    draft.chromium_system_logs = std::string(logs);
  }
  for (const Attachment& a : attachments_) {
    draft.attachments.push_back(a.draft);
  }
  std::string error;
  if (PhiFeedbackOutbox::Get().Enqueue(draft, &error)) {
    ShowSent();
    return;
  }
  LOG(ERROR) << "[Feedback] enqueue failed: " << error;
  progress_timer_.Stop();
  submitting_ = false;
  RefreshSendState();
  PhiNotice(hwnd(), PhiStr("feedback.form.localSaveFailure.title"),
            Widen(error).c_str());
}

void PhiFeedbackWindow::ShowSent() {
  progress_timer_.Stop();
  submitting_ = false;
  sent_ = true;
  error_text_.clear();
  ::SetWindowTextW(cancel_, PhiStr("feedback.form.closeButton"));
  Layout();
  ::SetFocus(cancel_);
}

// --- Messages --------------------------------------------------------------

// static
LRESULT CALLBACK PhiFeedbackWindow::ControlProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM wparam,
                                                LPARAM lparam,
                                                UINT_PTR id,
                                                DWORD_PTR data) {
  auto* self = reinterpret_cast<PhiFeedbackWindow*>(data);
  if (msg == WM_NCDESTROY) {
    ::RemoveWindowSubclass(hwnd, &PhiFeedbackWindow::ControlProc, id);
    if (self && self->hot_ == hwnd) {
      self->hot_ = nullptr;
    }
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (!self) {
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  const bool is_tile = id >= kIdTileBase;
  const bool hover_tracked = is_tile || id == kIdAdd || id == kIdDiagnostics ||
                             id == kIdUrlClear || id == kIdErrorClose;
  if (hover_tracked && msg == WM_MOUSEMOVE) {
    const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const RECT x = self->TileRemoveRect(hwnd);
    const bool on_remove = is_tile && ::PtInRect(&x, pt);
    if (self->hot_ != hwnd || self->hot_remove_ != on_remove) {
      if (self->hot_ && self->hot_ != hwnd) {
        ::InvalidateRect(self->hot_, nullptr, FALSE);
      }
      self->hot_ = hwnd;
      self->hot_remove_ = on_remove;
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
      ::TrackMouseEvent(&track);
      ::InvalidateRect(hwnd, nullptr, FALSE);
    }
  } else if (hover_tracked && msg == WM_MOUSELEAVE) {
    if (self->hot_ == hwnd) {
      self->hot_ = nullptr;
      self->hot_remove_ = false;
      ::InvalidateRect(hwnd, nullptr, FALSE);
    }
  } else if (is_tile &&
             (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
    const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const RECT x = self->TileRemoveRect(hwnd);
    self->press_on_remove_ = ::PtInRect(&x, pt) != 0;
  }

  if (msg == WM_SETFOCUS) {
    // Tab onto a field scrolled out of the body brings it into view.
    self->ScrollIntoView(hwnd);
  }

  const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
  if (msg == WM_KEYDOWN) {
    switch (wparam) {
      // VK_TAB is not here any more: the window registers kPhiDialogTab
      // and the thread's dialog hook runs IsDialogMessageW for it from
      // the main loop (phi_secondary_window.h).
      case VK_ESCAPE:
        if (self->submitting_) {
          self->CancelSend();
        } else {
          ::PostMessageW(self->hwnd(), WM_CLOSE, 0, 0);
        }
        return 0;
      case VK_RETURN: {
        wchar_t cls[16] = {};
        ::GetClassNameW(hwnd, cls, 16);
        const bool is_button = ::_wcsicmp(cls, L"Button") == 0;
        if (is_button) {
          self->press_on_remove_ = false;
          ::SendMessageW(hwnd, BM_CLICK, 0, 0);
          return 0;
        }
        if (hwnd != self->description_ || ctrl) {
          // Enter anywhere but the description sends; there, Ctrl+Enter.
          if (::IsWindowEnabled(self->send_)) {
            self->Send(/*interactive=*/true);
          }
          return 0;
        }
        break;
      }
      case VK_SPACE:
        if (is_tile) {
          self->press_on_remove_ = false;
        }
        break;
      case VK_DELETE:
        if (is_tile) {
          self->RemoveAttachment(self->TileIndex(hwnd));
          return 0;
        }
        break;
      case 'V':
        if (ctrl && hwnd != self->url_ && self->PasteAttachments()) {
          self->swallow_paste_char_ = true;
          return 0;
        }
        break;
      default:
        break;
    }
  }
  if (msg == WM_CHAR && wparam == VK_ESCAPE) {
    return 0;  // Swallow the beep the edit would emit.
  }
  if (msg == WM_CHAR && wparam == VK_RETURN && hwnd != self->description_) {
    return 0;
  }
  if (msg == WM_CHAR && wparam == 0x16 && self->swallow_paste_char_) {
    self->swallow_paste_char_ = false;
    return 0;  // The Ctrl+V character after an attachment paste.
  }
  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT PhiFeedbackWindow::OnWindowMessage(UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        bool* handled) {
  switch (msg) {
    case WM_MEASUREITEM:
      if (PhiMenuOnMeasureItem(hwnd(),
                               reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) {
        *handled = true;
        return TRUE;
      }
      break;
    case WM_DRAWITEM: {
      const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item.CtlType == ODT_MENU &&
          PhiMenuOnDrawItem(hwnd(), item, menu_palette_)) {
        *handled = true;
        return TRUE;
      }
      if (DrawOwnerItem(item) ||
          PhiPopupDrawButton(item, colors(), nullptr)) {
        *handled = true;
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLOREDIT: {
      // The fields paint on their box: the card while focused, the control
      // fill at rest.
      HDC dc = reinterpret_cast<HDC>(wparam);
      const bool focused = reinterpret_cast<HWND>(lparam) == ::GetFocus();
      ::SetTextColor(dc, colors().label);
      ::SetBkColor(dc, focused ? colors().card : FieldFill());
      *handled = true;
      return reinterpret_cast<LRESULT>(focused ? card_brush_ : field_brush_);
    }
    case WM_CTLCOLORSTATIC: {
      // A disabled edit (locked while sending) asks here.
      HDC dc = reinterpret_cast<HDC>(wparam);
      ::SetTextColor(dc, colors().tertiary);
      ::SetBkColor(dc, PhiPopupBlend(colors().card, colors().bg, 128));
      *handled = true;
      return reinterpret_cast<LRESULT>(disabled_brush_);
    }
    case WM_CONTEXTMENU: {
      const HWND source = reinterpret_cast<HWND>(wparam);
      const size_t index = TileIndex(source);
      if (index < attachments_.size() && !submitting_) {
        POINT at = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (lparam == -1) {
          // Shift+F10 / the menu key: under the tile.
          RECT r = {};
          ::GetWindowRect(source, &r);
          at = {r.left, r.bottom};
        }
        ShowTileMenu(index, at);
        *handled = true;
        return 0;
      }
      break;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      const std::vector<std::wstring> paths = DropPaths(drop);
      ::DragFinish(drop);
      if (!sent_ && !submitting_) {
        AddFiles(paths, /*interactive=*/true);
      }
      *handled = true;
      return 0;
    }
    case WM_ACTIVATE:
      // Back from the browser: the user may have signed in there.
      if (LOWORD(wparam) != WA_INACTIVE) {
        RefreshAccount();
      }
      break;
    case WM_COMMAND: {
      const UINT_PTR id = LOWORD(wparam);
      const UINT code = HIWORD(wparam);
      if (id == kIdDescription && code == EN_CHANGE) {
        RefreshCounter();
        RefreshSendState();
        *handled = true;
        return 0;
      }
      if ((id == kIdDescription || id == kIdUrl) &&
          (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
        // The box's fill and its accent edge follow the focus.
        const RECT& box =
            id == kIdDescription ? geo_.description_box : geo_.page_box;
        ::InvalidateRect(hwnd(), &box, FALSE);
        ::InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, TRUE);
        if (id == kIdUrl) {
          ::InvalidateRect(url_clear_, nullptr, FALSE);
        }
        *handled = true;
        return 0;
      }
      // Owner-drawn buttons report a quick second click as a double click;
      // only the tiles (a double tap on ✕) take it as a click.
      if (code != BN_CLICKED &&
          !(code == BN_DOUBLECLICKED && id >= kIdTileBase)) {
        break;
      }
      if (id >= kIdTileBase) {
        const size_t index = id - kIdTileBase;
        if (press_on_remove_) {
          press_on_remove_ = false;
          RemoveAttachment(index);
        } else {
          OpenAttachment(index);
        }
        *handled = true;
        return 0;
      }
      switch (id) {
        case kIdUrlClear:
          ::SetWindowTextW(url_, L"");
          ::SetFocus(url_);
          break;
        case kIdAdd:
          ChooseFiles();
          break;
        case kIdDiagnostics:
          diagnostics_expanded_ = !diagnostics_expanded_;
          if (diagnostics_expanded_) {
            diagnostic_rows_.clear();
            diagnostic_rows_.emplace_back(
                PhiStr("feedback.form.diagnostics.phi"),
                Widen(PHI_WIN_VERSION_FULL_STRING));
            diagnostic_rows_.emplace_back(
                PhiStr("feedback.form.diagnostics.windows"),
                Widen(PhiFeedbackOutbox::OsVersion()));
            const std::string title = PageTitleForReport();
            if (!title.empty()) {
              diagnostic_rows_.emplace_back(
                  PhiStr("feedback.form.diagnostics.pageTitle"),
                  Widen(title));
            }
            std::wstring extensions;
            for (const auto& [name, version] :
                 host_->PhiExtensionVersions(context_.window_id)) {
              extensions += (extensions.empty() ? L"" : L" · ") +
                            Widen(name) + L" " + Widen(version);
            }
            diagnostic_rows_.emplace_back(
                PhiStr("feedback.form.diagnostics.extensions"),
                extensions.empty() ? PhiStr("feedback.form.diagnostics.none")
                                   : extensions);
            diagnostic_rows_.emplace_back(
                PhiStr("feedback.form.diagnostics.logs"),
                PhiStr("feedback.form.diagnostics.logsValue"));
          }
          Layout();
          break;
        case kIdSignIn:
          host_->RequestFeedbackSignIn();
          break;
        case kIdErrorClose:
          error_text_.clear();
          attachment_error_.clear();
          Layout();
          ::SetFocus(add_);
          break;
        case kIdPrivacy:
          host_->OpenFeedbackLink(context_.window_id, kPrivacyUrl);
          break;
        case kIdTerms:
          host_->OpenFeedbackLink(context_.window_id, kTermsUrl);
          break;
        case kIdSend:
          if (sent_) {
            Destroy();
          } else {
            Send(/*interactive=*/true);
          }
          break;
        case kIdCancel:
          if (submitting_) {
            CancelSend();
          } else {
            Destroy();
          }
          break;
        default:
          return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam,
                                                     handled);
      }
      *handled = true;
      return 0;
    }
    case WM_CLOSE: {
      // Escape and the caption's close land here: a typed report is only
      // thrown away once the user says so. The prompt's modal loop runs
      // other messages, so the window is re-checked before it is touched.
      *handled = true;
      const HWND self = hwnd();
      if (wparam != kCloseWithoutPrompt && !ConfirmDiscard()) {
        return 0;
      }
      if (::IsWindow(self) && g_window == this) {
        Destroy();
      }
      return 0;
    }
    case WM_DISPLAYCHANGE:
      Layout();  // Re-caps the height to the new work area.
      FitToWorkArea();
      break;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETWORKAREA) {
        Layout();  // The taskbar moved or grew.
        FitToWorkArea();
      }
      break;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiFeedbackWindow::OnTimerFired(UINT_PTR id) {
  if (id == progress_timer_.id()) {
    progress_phase_ = (progress_phase_ + 2) % 100;
    ::InvalidateRect(hwnd(), &geo_.footer, FALSE);
    return;
  }
  if (id != logs_timer_.id()) {
    return;
  }
  logs_timer_.Stop();
  if (pending_request_) {
    LOG(WARNING) << "[Feedback] Chromium system logs skipped: the "
                 << "framework did not answer in time";
    CompleteSend(nullptr);
  }
}

void PhiFeedbackWindow::OnColorsChanged() {
  for (HBRUSH* slot :
       {&bg_brush_, &card_brush_, &field_brush_, &disabled_brush_}) {
    if (*slot) {
      ::DeleteObject(*slot);
      *slot = nullptr;
    }
  }
  const PhiPopupColors& c = colors();
  bg_brush_ = ::CreateSolidBrush(c.bg);
  card_brush_ = ::CreateSolidBrush(c.card);
  field_brush_ = ::CreateSolidBrush(FieldFill());
  disabled_brush_ = ::CreateSolidBrush(PhiPopupBlend(c.card, c.bg, 128));
  const std::vector<PhiTheme>& themes = PhiBuiltInThemes();
  if (!themes.empty()) {
    menu_palette_ = PhiResolvePalette(
        themes.front(), c.dark ? PhiAppearance::kDark : PhiAppearance::kLight);
  }
  PhiPopupThemeControl(description_, c.dark);
  PhiPopupThemeControl(url_, c.dark);
}

void PhiFeedbackWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  EnsureFonts();
  ApplyFonts();
  Layout();
}

void PhiFeedbackWindow::OnNcDestroy() {
  logs_timer_.Stop();
  progress_timer_.Stop();
  if (drop_target_) {
    ::RevokeDragDrop(hwnd());
    drop_target_->Detach();
    drop_target_->Release();
    drop_target_ = nullptr;
  }
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client
