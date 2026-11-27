// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_page_screenshot.h"

#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_bookmark_manager.h"
#include "client/phi_i18n.h"
#include "client/phi_new_tab_page.h"
#include "client/phi_screenshot_area.h"
#include "client/phi_screenshot_editor.h"
#include "client/phi_screenshot_render.h"
#include "client/phi_shell_window.h"
#include "client/phi_toast.h"
#include "client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr UINT kMsgDiscovery = WM_APP + 41;  // wparam op, lparam std::string*
constexpr UINT kMsgFinish = WM_APP + 42;     // wparam op
constexpr UINT kMsgOpenEditor = WM_APP + 43; // lparam PendingEditor*
constexpr UINT_PTR kTickTimer = 1;
constexpr int kSaveToastMs = 6000;
// The longest discovery document the lookup accepts.
constexpr size_t kMaxDiscovery = 8u << 20;

class DiscoverySink : public phi_framework::PhiStringSink {
 public:
  explicit DiscoverySink(std::string* out) : out_(out) {}
  void Append(const char* data, size_t length) override {
    if (!data || overflow_ || length > kMaxDiscovery - out_->size()) {
      overflow_ = true;
      return;
    }
    out_->append(data, length);
  }
  bool overflow() const { return overflow_; }

 private:
  RAW_PTR_EXCLUSION std::string* out_;
  bool overflow_ = false;
};

struct LastResult {
  bool ran = false;
  bool ok = false;
  std::string mode;
  std::string error;
  int width = 0;
  int height = 0;
  size_t bytes = 0;
  bool copied = false;
  bool clipboard_changed = false;
  double scale = 1;
  double css_width = 0;
  double css_height = 0;
  double dpr = 1;
  uint64_t finished_at = 0;
};
LastResult& Last() {
  static base::NoDestructor<LastResult> last;
  return *last;
}

HWND MessageWindow();

// The editor a finished capture opens, handed to a later turn: opening it
// may ask about a dirty editor (a modal loop), which must never run inside
// a DevTools transport callback — the loop would run kMsgFinish, whose
// CloseDevToolsSession waits for the very callback it is nested in.
struct PendingEditor {
  HWND shell = nullptr;
  PhiScreenshotImageRef image;
  std::wstring title;
  double dpr = 2;
};

// The clipboard formats a capture lands in.
bool CopyToClipboard(HWND owner,
                     const std::string& png,
                     const PhiScreenshotImage& image) {
  const UINT png_format = ::RegisterClipboardFormatW(L"PNG");
  if (!::OpenClipboard(owner)) {
    return false;
  }
  bool ok = ::EmptyClipboard() != FALSE;
  if (ok && png_format) {
    HGLOBAL bytes = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
    void* dst = bytes ? ::GlobalLock(bytes) : nullptr;
    if (dst) {
      UNSAFE_BUFFERS(std::copy(png.begin(), png.end(),
                               static_cast<char*>(dst)));
      ::GlobalUnlock(bytes);
      if (!::SetClipboardData(png_format, bytes)) {
        ::GlobalFree(bytes);
        ok = false;
      }
    } else {
      if (bytes) {
        ::GlobalFree(bytes);
      }
      ok = false;
    }
  }
  if (ok) {
    // CF_DIB, 32 bpp bottom-up BI_RGB, for programs that read only bitmaps.
    const size_t stride = static_cast<size_t>(image.width) * 4;
    const size_t size = sizeof(BITMAPINFOHEADER) + stride * image.height;
    HGLOBAL dib = ::GlobalAlloc(GMEM_MOVEABLE, size);
    auto* header = dib ? static_cast<BITMAPINFOHEADER*>(::GlobalLock(dib))
                       : nullptr;
    if (header) {
      *header = {};
      header->biSize = sizeof(BITMAPINFOHEADER);
      header->biWidth = image.width;
      header->biHeight = image.height;
      header->biPlanes = 1;
      header->biBitCount = 32;
      header->biCompression = BI_RGB;
      header->biSizeImage = static_cast<DWORD>(stride * image.height);
      uint8_t* bits = UNSAFE_BUFFERS(reinterpret_cast<uint8_t*>(header + 1));
      for (int y = 0; y < image.height; ++y) {
        const size_t src = static_cast<size_t>(image.height - 1 - y) * stride;
        uint8_t* row = UNSAFE_BUFFERS(bits + static_cast<size_t>(y) * stride);
        for (size_t x = 0; x < stride; x += 4) {
          // Un-premultiply: BI_RGB carries straight colour.
          const uint8_t a = image.bgra[src + x + 3];
          for (size_t c = 0; c < 3; ++c) {
            const uint8_t v = image.bgra[src + x + c];
            UNSAFE_BUFFERS(row[x + c]) =
                a == 0 ? 0
                       : static_cast<uint8_t>(std::min(255, v * 255 / a));
          }
          UNSAFE_BUFFERS(row[x + 3]) = a;
        }
      }
      ::GlobalUnlock(dib);
      if (!::SetClipboardData(CF_DIB, dib)) {
        ::GlobalFree(dib);
      }
    } else if (dib) {
      ::GlobalFree(dib);
    }
  }
  ::CloseClipboard();
  return ok;
}

// The Save toast's action: the captured PNG, unchanged, where the user says.
void SaveCapture(HWND owner, const std::string& png, const std::wstring& title) {
  std::wstring name = PhiScreenshotFileName(
      title, PhiStr("browser.screenshot.fileName"), L"png");
  std::wstring buffer(MAX_PATH * 4, L'\0');
  std::copy(name.begin(),
            name.begin() + static_cast<ptrdiff_t>(
                               std::min(name.size(), buffer.size() - 1)),
            buffer.begin());
  std::wstring filter = L"PNG";
  filter.push_back(L'\0');
  filter += L"*.png";
  filter.push_back(L'\0');
  filter.push_back(L'\0');
  OPENFILENAMEW ofn = {sizeof(ofn)};
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  ofn.lpstrDefExt = L"png";
  if (!::GetSaveFileNameW(&ofn)) {
    return;
  }
  std::string error;
  if (!PhiScreenshotWriteBytes(png, buffer.c_str(), &error)) {
    PhiShellWindow* shell = PhiShellWindow::FromHwnd(owner);
    PhiShowToast(owner, shell ? shell->BarPalette() : PhiPalette(),
                 PhiStr("browser.screenshot.saveFailed"));
  }
}

// --- One capture --------------------------------------------------------------

class Capture : public phi_framework::PhiDevToolsClient,
                public PhiScreenshotAreaHost {
 public:
  using Reply = std::function<void(const base::DictValue& result)>;

  Capture(PhiShellWindow* shell,
          PhiPageScreenshot::Mode mode,
          std::optional<PhiRectF> test_rect)
      : shell_(shell),
        framework_(shell->framework()),
        shell_hwnd_(shell->shell_hwnd()),
        mode_(mode),
        test_rect_(test_rect),
        id_(++next_id_),
        started_(::GetTickCount64()),
        clipboard_seq_(::GetClipboardSequenceNumber()) {
    browser_ = shell->PageCommandBrowser();
    tab_ = shell->PageCommandTab();
    if (const PhiWindowModel* model = shell->model()) {
      if (const PhiWindowModel::Tab* t = model->FindTab(browser_, tab_)) {
        title_ = base::UTF8ToWide(t->title);
      }
    }
  }

  ~Capture() override { CloseSession(); }

  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

  uint64_t id() const { return id_; }
  PhiShellWindow* shell() const { return shell_; }
  bool area() const { return mode_ == PhiPageScreenshot::Mode::kArea; }

  void Begin() {
    ::SetTimer(MessageWindow(), kTickTimer, 200, nullptr);
    // DevToolsDiscoveryJson must not run on the UI thread.
    struct Job {
      RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework;
      uint64_t op;
      HWND reply_to;
    };
    auto* job = new Job{framework_, id_, MessageWindow()};
    HANDLE thread = ::CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
          std::unique_ptr<Job> job(static_cast<Job*>(param));
          auto* json = new std::string();
          DiscoverySink sink(json);
          if (!job->framework->DevToolsDiscoveryJson("/json/list", &sink) ||
              sink.overflow()) {
            json->clear();
          }
          if (!::PostMessageW(job->reply_to, kMsgDiscovery,
                              static_cast<WPARAM>(job->op),
                              reinterpret_cast<LPARAM>(json))) {
            delete json;
          }
          return 0;
        },
        job, 0, nullptr);
    if (!thread) {
      delete job;
      Fail("no_target");
      return;
    }
    ::CloseHandle(thread);
  }

  // The discovery document arrived on the UI thread.
  void OnDiscovery(const std::string& json) {
    std::optional<base::Value> list =
        base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    std::string target;
    if (list && list->is_list()) {
      for (const base::Value& entry : list->GetList()) {
        const base::DictValue* dict = entry.GetIfDict();
        const std::string* type = dict ? dict->FindString("type") : nullptr;
        const std::string* id = dict ? dict->FindString("id") : nullptr;
        if (type && id && *type == "page" &&
            framework_->TabIdForCdpTarget(id->c_str()) == tab_.value) {
          target = *id;
          break;
        }
      }
    }
    if (target.empty()) {
      Fail("no_target");
      return;
    }
    session_ = framework_->OpenDevToolsSession(target.c_str(), this);
    if (!session_) {
      Fail("no_session");
      return;
    }
    first_command_ = true;
    Send("Page.getFrameTree", base::DictValue(),
         [this](const base::DictValue& r) {
           if (!ReadFrame(r, &frame_id_, &loader_id_, &url_)) {
             Fail("no_frame");
             return;
           }
           if (area() && !test_rect_) {
             MeasureViewport([this](const PhiScreenshotViewport& v) {
               OpenOverlay(v);
             });
           } else {
             Measure();
           }
         });
  }

  // --- PhiDevToolsClient (UI thread) --------------------------------------
  void OnDevToolsMessage(uint64_t session_id,
                         const char* json,
                         size_t length) override {
    if (session_id != session_ || finished_) {
      return;
    }
    std::optional<base::DictValue> message = base::JSONReader::ReadDict(
        std::string_view(json, length), base::JSON_PARSE_RFC);
    if (!message) {
      return;
    }
    const std::optional<int> id = message->FindInt("id");
    if (!id) {
      return;  // An event; nothing here subscribes.
    }
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    Reply reply = std::move(it->second.reply);
    pending_.erase(it);
    if (message->FindDict("error")) {
      Fail("cdp_error");
      return;
    }
    const base::DictValue* result = message->FindDict("result");
    const base::DictValue empty;
    reply(result ? *result : empty);
  }

  void OnDevToolsSessionClosed(uint64_t session_id) override {
    if (session_id == session_ && !finished_) {
      Fail("session_closed");
    }
  }

  // --- PhiScreenshotAreaHost ------------------------------------------------
  void AreaScroll(double dx, double dy) override {
    if (finished_) {
      return;
    }
    base::DictValue params;
    params.Set("expression",
               // Instant even under `scroll-behavior: smooth`, so the
               // offsets read back are where the page already is.
               "window.scrollTo({left:scrollX+(" + Number(dx) +
                   "),top:scrollY+(" + Number(dy) + "),behavior:'instant'});" +
                   kMetricsExpression);
    params.Set("returnByValue", true);
    Send("Runtime.evaluate", std::move(params),
         [this](const base::DictValue& r) {
           PhiScreenshotViewport v;
           if (!ReadViewport(r, &v)) {
             Fail("no_viewport");
             return;
           }
           if (std::abs(v.inner_width - viewport_.inner_width) > 0.5 ||
               std::abs(v.inner_height - viewport_.inner_height) > 0.5) {
             // A resize or a zoom moved the mapping under the selection.
             Cancel();
             return;
           }
           viewport_.scroll_x = v.scroll_x;
           viewport_.scroll_y = v.scroll_y;
           viewport_.doc_width = v.doc_width;
           viewport_.doc_height = v.doc_height;
           PhiScreenshotArea::SetViewport(viewport_);
         });
  }

  void AreaDone(const PhiRectF& rect) override {
    if (finished_) {
      return;
    }
    test_rect_ = rect;
    Measure();
  }

  void AreaCancel() override { Cancel(); }

  // The deadline and the per-command timeouts.
  void Tick() {
    if (finished_) {
      return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - started_ > static_cast<uint64_t>(kPhiScreenshotBudgetMs)) {
      Fail("timed_out");
      return;
    }
    for (const auto& [id, call] : pending_) {
      if (now > call.deadline) {
        Fail("timed_out");
        return;
      }
    }
  }

  // Abandons without a toast (the shell is going away, or the user
  // cancelled the overlay).
  void Cancel() {
    if (finished_) {
      return;
    }
    Record(false, "cancelled");
    Finish();
  }

 private:
  struct Call {
    Reply reply;
    uint64_t deadline = 0;
  };

  static constexpr char kMetricsExpression[] =
      "JSON.stringify({w:innerWidth,h:innerHeight,x:scrollX,y:scrollY,"
      "dw:document.documentElement.scrollWidth,"
      "dh:document.documentElement.scrollHeight,dpr:devicePixelRatio,"
      "url:location.href})";

  static std::string Number(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", std::isfinite(v) ? v : 0.0);
    return buffer;
  }

  void Send(const std::string& method, base::DictValue params, Reply reply) {
    if (finished_ || !session_) {
      return;
    }
    const int id = next_call_++;
    base::DictValue message;
    message.Set("id", id);
    message.Set("method", method);
    message.Set("params", std::move(params));
    std::string json;
    if (!base::JSONWriter::Write(message, &json)) {
      Fail("cdp_error");
      return;
    }
    const uint64_t now = ::GetTickCount64();
    const uint64_t budget_end =
        started_ + static_cast<uint64_t>(kPhiScreenshotBudgetMs);
    uint64_t deadline =
        now + (first_command_ ? kPhiScreenshotOpenMs : kPhiScreenshotCommandMs);
    first_command_ = false;
    // While the user is selecting an area the clock that matters is the
    // operation's, not a command's.
    deadline = std::min(deadline, budget_end);
    pending_[id] = Call{std::move(reply), deadline};
    framework_->SendDevToolsMessage(session_, json.data(), json.size());
  }

  static bool ReadFrame(const base::DictValue& r,
                        std::string* frame,
                        std::string* loader,
                        std::string* url) {
    const base::DictValue* tree = r.FindDict("frameTree");
    const base::DictValue* f = tree ? tree->FindDict("frame") : nullptr;
    const std::string* id = f ? f->FindString("id") : nullptr;
    const std::string* l = f ? f->FindString("loaderId") : nullptr;
    const std::string* u = f ? f->FindString("url") : nullptr;
    if (!id || !l || !u) {
      return false;
    }
    *frame = *id;
    *loader = *l;
    *url = *u;
    return true;
  }

  // Runtime.evaluate's {result:{value:"<json>"}} → the page's metrics.
  static bool ReadViewport(const base::DictValue& r,
                           PhiScreenshotViewport* v,
                           double* dpr = nullptr) {
    const base::DictValue* result = r.FindDict("result");
    const std::string* text = result ? result->FindString("value") : nullptr;
    std::optional<base::DictValue> m =
        text ? base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC)
             : std::nullopt;
    if (!m) {
      return false;
    }
    v->inner_width = m->FindDouble("w").value_or(0);
    v->inner_height = m->FindDouble("h").value_or(0);
    v->scroll_x = m->FindDouble("x").value_or(0);
    v->scroll_y = m->FindDouble("y").value_or(0);
    v->doc_width = m->FindDouble("dw").value_or(0);
    v->doc_height = m->FindDouble("dh").value_or(0);
    if (dpr) {
      *dpr = m->FindDouble("dpr").value_or(0);
    }
    return v->inner_width > 0 && v->inner_height > 0;
  }

  void MeasureViewport(std::function<void(const PhiScreenshotViewport&)> then) {
    base::DictValue params;
    params.Set("expression", kMetricsExpression);
    params.Set("returnByValue", true);
    Send("Runtime.evaluate", std::move(params),
         [this, then = std::move(then)](const base::DictValue& r) {
           PhiScreenshotViewport v;
           if (!ReadViewport(r, &v)) {
             Fail("no_viewport");
             return;
           }
           then(v);
         });
  }

  // The page's rect in the shell's client pixels: its split pane when it
  // is one on screen, else the content card.
  RECT ViewportRect() const {
    RECT pane = {};
    if (shell_->SplitPaneRectForTab(tab_.value, &pane) &&
        pane.right > pane.left && pane.bottom > pane.top) {
      return pane;
    }
    return shell_->ContentRect();
  }

  void OpenOverlay(const PhiScreenshotViewport& measured) {
    const RECT rect = ViewportRect();
    viewport_ = measured;
    viewport_.left = 0;
    viewport_.top = 0;
    viewport_.width = rect.right - rect.left;
    viewport_.height = rect.bottom - rect.top;
    if (!viewport_.valid() ||
        !PhiScreenshotArea::Open(shell_, rect, viewport_, this)) {
      Fail("no_viewport");
    }
  }

  // Layout metrics and the device pixel ratio, then the capture itself.
  void Measure() {
    Send("Page.getLayoutMetrics", base::DictValue(),
         [this](const base::DictValue& r) {
           const base::DictValue* size = r.FindDict("cssContentSize");
           if (!size) {
             Fail("no_metrics");
             return;
           }
           content_ = {size->FindDouble("x").value_or(0),
                       size->FindDouble("y").value_or(0), 0, 0};
           content_.right =
               content_.left + size->FindDouble("width").value_or(0);
           content_.bottom =
               content_.top + size->FindDouble("height").value_or(0);
           base::DictValue params;
           params.Set("expression", "devicePixelRatio");
           params.Set("returnByValue", true);
           Send("Runtime.evaluate", std::move(params),
                [this](const base::DictValue& eval) {
                  const base::DictValue* result = eval.FindDict("result");
                  dpr_ = result ? result->FindDouble("value").value_or(0) : 0;
                  Shoot();
                });
         });
  }

  void Shoot() {
    PhiRectF rect = content_;
    if (test_rect_) {
      rect = PhiRectIntersect(*test_rect_, content_);
    }
    const PhiScreenshotClip clip = PhiScreenshotClipFor(
        rect.left, rect.top, rect.width(), rect.height(), dpr_);
    if (!clip.ok) {
      Fail(test_rect_ ? "empty_area" : "no_metrics");
      return;
    }
    clip_ = clip;
    base::DictValue c;
    c.Set("x", clip.x);
    c.Set("y", clip.y);
    c.Set("width", clip.width);
    c.Set("height", clip.height);
    c.Set("scale", clip.scale);
    base::DictValue params;
    params.Set("format", "png");
    params.Set("fromSurface", true);
    params.Set("captureBeyondViewport", true);
    params.Set("clip", std::move(c));
    Send("Page.captureScreenshot", std::move(params),
         [this](const base::DictValue& r) {
           const std::string* data = r.FindString("data");
           if (!data || data->empty()) {
             Fail("capture_failed");
             return;
           }
           if (data->size() > kPhiScreenshotMaxBase64) {
             Fail("too_large");
             return;
           }
           if (!PhiScreenshotDecodeBase64(*data, &png_)) {
             Fail("capture_failed");
             return;
           }
           // The page must still be the document the capture started on.
           Send("Page.getFrameTree", base::DictValue(),
                [this](const base::DictValue& tree) {
                  std::string frame, loader, url;
                  if (!ReadFrame(tree, &frame, &loader, &url) ||
                      frame != frame_id_ || loader != loader_id_ ||
                      url != url_) {
                    Fail("navigated");
                    return;
                  }
                  Deliver();
                });
         });
  }

  void Deliver() {
    const PhiPngInfo info = PhiScreenshotValidatePng(png_);
    if (!info.ok) {
      Fail(info.error);
      return;
    }
    PhiScreenshotImageRef image = PhiScreenshotDecodePng(png_);
    if (!image) {
      Fail("decode_failed");
      return;
    }
    if (finished_ || ::GetTickCount64() - started_ >
                         static_cast<uint64_t>(kPhiScreenshotBudgetMs)) {
      Fail("timed_out");
      return;
    }
    const PhiPalette palette = shell_->BarPalette();
    const bool changed = ::GetClipboardSequenceNumber() != clipboard_seq_;
    bool copied = false;
    std::wstring toast;
    if (changed) {
      toast = PhiStr("browser.screenshot.clipboardChanged");
    } else if (CopyToClipboard(shell_hwnd_, png_, *image)) {
      copied = true;
      toast = PhiStr(area() ? "browser.screenshot.areaCopied"
                            : "browser.screenshot.copied");
    } else {
      toast = PhiStr("browser.screenshot.copyFailed");
    }
    Record(true, "");
    Last().width = info.width;
    Last().height = info.height;
    Last().bytes = png_.size();
    Last().copied = copied;
    Last().clipboard_changed = changed;
    const HWND owner = shell_hwnd_;
    const std::string png = png_;
    const std::wstring title = title_;
    PhiShowToastWithAction(
        owner, palette, toast, std::wstring(), kSaveToastMs,
        PhiStr("browser.screenshot.saveAction"),
        [owner, png, title] { SaveCapture(owner, png, title); });
    auto* pending =
        new PendingEditor{shell_hwnd_, std::move(image), title, dpr_};
    if (!::PostMessageW(MessageWindow(), kMsgOpenEditor, 0,
                        reinterpret_cast<LPARAM>(pending))) {
      delete pending;
    }
    Finish();
  }

  void Record(bool ok, const std::string& error) {
    Last() = LastResult();
    Last().ran = true;
    Last().ok = ok;
    Last().mode = area() ? "area" : "full";
    Last().error = error;
    Last().scale = clip_.scale;
    Last().css_width = clip_.width;
    Last().css_height = clip_.height;
    Last().dpr = dpr_;
    Last().finished_at = ::GetTickCount64();
  }

  void Fail(const std::string& error) {
    if (finished_) {
      return;
    }
    LOG(WARNING) << "page screenshot failed: " << error;
    Record(false, error);
    PhiScreenshotArea::Close();
    if (::IsWindow(shell_hwnd_)) {
      PhiShowToast(shell_hwnd_, shell_->BarPalette(),
                   PhiStr(error == "timed_out"
                              ? "browser.screenshot.timedOut"
                              : "browser.screenshot.captureFailed"));
    }
    Finish();
  }

  void Finish();

  void CloseSession() {
    if (session_) {
      framework_->CloseDevToolsSession(session_);
      session_ = 0;
    }
  }

  RAW_PTR_EXCLUSION PhiShellWindow* shell_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  HWND shell_hwnd_;
  PhiPageScreenshot::Mode mode_;
  std::optional<PhiRectF> test_rect_;
  uint64_t id_;
  uint64_t started_;
  DWORD clipboard_seq_;
  BrowserId browser_;
  TabId tab_;
  std::wstring title_;
  uint64_t session_ = 0;
  bool first_command_ = false;
  bool finished_ = false;
  int next_call_ = 1;
  std::map<int, Call> pending_;
  std::string frame_id_;
  std::string loader_id_;
  std::string url_;
  PhiScreenshotViewport viewport_;
  PhiRectF content_;
  double dpr_ = 0;
  PhiScreenshotClip clip_;
  std::string png_;

  static uint64_t next_id_;
};

uint64_t Capture::next_id_ = 0;
// Owned; deleted on kMsgFinish or when its shell goes.
Capture* g_capture = nullptr;

void Capture::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  pending_.clear();
  PhiScreenshotArea::Close();
  ::KillTimer(MessageWindow(), kTickTimer);
  // The session closes and the object goes on a later turn: this runs
  // inside a transport callback.
  ::PostMessageW(MessageWindow(), kMsgFinish, static_cast<WPARAM>(id_), 0);
}

LRESULT CALLBACK MessageProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case kMsgDiscovery: {
      std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lparam));
      if (g_capture && g_capture->id() == static_cast<uint64_t>(wparam)) {
        g_capture->OnDiscovery(*json);
      }
      return 0;
    }
    case kMsgOpenEditor: {
      std::unique_ptr<PendingEditor> pending(
          reinterpret_cast<PendingEditor*>(lparam));
      // The window may have closed since the capture finished.
      if (PhiShellWindow* shell = PhiShellWindow::FromHwnd(pending->shell)) {
        PhiScreenshotEditor::Open(shell, std::move(pending->image),
                                  pending->title, pending->dpr);
      }
      return 0;
    }
    case kMsgFinish:
      if (g_capture && g_capture->id() == static_cast<uint64_t>(wparam)) {
        delete std::exchange(g_capture, nullptr);
      }
      return 0;
    case WM_TIMER:
      if (g_capture) {
        g_capture->Tick();
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND MessageWindow() {
  static HWND hwnd = [] {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = &MessageProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiPageScreenshotMessages";
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  }();
  return hwnd;
}

bool PageOk(PhiShellWindow* shell, std::string* error) {
  if (!shell || !shell->framework() || !shell->shell_hwnd()) {
    *error = "no_window";
    return false;
  }
  const BrowserId browser = shell->PageCommandBrowser();
  const TabId tab = shell->PageCommandTab();
  const PhiWindowModel* model = shell->model();
  const PhiWindowModel::Tab* t =
      model && browser.valid() && tab.valid() ? model->FindTab(browser, tab)
                                              : nullptr;
  if (!t) {
    *error = "no_tab";
    return false;
  }
  // A native page covers the web contents: the capture would not show
  // what the user sees (the Mac disables its native new-tab page too).
  if (t->url.empty() || PhiNewTabPage::IsNewTabUrl(t->url) ||
      PhiBookmarkManager::IsManagerUrl(t->url)) {
    *error = "native_page";
    return false;
  }
  return true;
}

}  // namespace

// static
bool PhiPageScreenshot::CanCapture(PhiShellWindow* shell) {
  std::string error;
  return !Busy() && PageOk(shell, &error);
}

// static
void PhiPageScreenshot::Start(PhiShellWindow* shell, Mode mode) {
  std::string error;
  StartForTest(shell, mode, nullptr, &error);
}

// static
bool PhiPageScreenshot::StartForTest(PhiShellWindow* shell,
                                     Mode mode,
                                     const PhiRectF* rect,
                                     std::string* error) {
  if (Busy()) {
    *error = "busy";
    return false;
  }
  if (!PageOk(shell, error)) {
    return false;
  }
  MessageWindow();
  std::optional<PhiRectF> test_rect;
  if (rect) {
    test_rect = *rect;
  }
  g_capture = new Capture(shell, mode, test_rect);
  g_capture->Begin();
  return true;
}

// static
bool PhiPageScreenshot::Busy() {
  return g_capture != nullptr;
}

// static
void PhiPageScreenshot::ShellGoingAway(PhiShellWindow* shell) {
  PhiScreenshotArea::CloseIfBoundTo(shell);
  if (g_capture && g_capture->shell() == shell) {
    g_capture->Cancel();
    delete std::exchange(g_capture, nullptr);
  }
}

// static
base::DictValue PhiPageScreenshot::StateForTest() {
  base::DictValue out;
  out.Set("busy", Busy());
  base::DictValue last;
  last.Set("ran", Last().ran);
  last.Set("ok", Last().ok);
  last.Set("mode", Last().mode);
  last.Set("error", Last().error);
  last.Set("width", Last().width);
  last.Set("height", Last().height);
  last.Set("bytes", static_cast<double>(Last().bytes));
  last.Set("copied", Last().copied);
  last.Set("clipboardChanged", Last().clipboard_changed);
  last.Set("scale", Last().scale);
  last.Set("cssWidth", Last().css_width);
  last.Set("cssHeight", Last().css_height);
  last.Set("dpr", Last().dpr);
  last.Set("ageMs", Last().ran ? static_cast<double>(::GetTickCount64() -
                                                     Last().finished_at)
                               : -1.0);
  out.Set("last", std::move(last));
  out.Set("area", PhiScreenshotArea::StateForTest());
  out.Set("editor", PhiScreenshotEditor::StateForTest());
  // What the clipboard holds now.
  base::DictValue clipboard;
  clipboard.Set("png", ::IsClipboardFormatAvailable(
                           ::RegisterClipboardFormatW(L"PNG")) != FALSE);
  clipboard.Set("dib", ::IsClipboardFormatAvailable(CF_DIB) != FALSE);
  clipboard.Set("sequence",
                static_cast<double>(::GetClipboardSequenceNumber()));
  out.Set("clipboard", std::move(clipboard));
  return out;
}

bool PhiScreenshotOwnsKeyboard() {
  return PhiScreenshotArea::OwnsFocus() || PhiScreenshotEditor::OwnsFocus();
}

}  // namespace phi_client
