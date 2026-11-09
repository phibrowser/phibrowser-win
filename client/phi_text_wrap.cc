// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_text_wrap.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

// What IDWriteTextAnalyzer needs: the text in, the breakpoints out. It lives
// on the caller's stack for one synchronous call, so it counts no references.
class BreakAnalysis final : public IDWriteTextAnalysisSource,
                            public IDWriteTextAnalysisSink {
 public:
  explicit BreakAnalysis(std::wstring_view text)
      : text_(text), points_(text.size()) {}

  const std::vector<DWRITE_LINE_BREAKPOINT>& points() const { return points_; }

  // DirectWrite lets a line end on either side of a "+", which would split
  // "Ctrl+Shift+B", so those are taken back unless a space is what it touches.
  bool Run(IDWriteTextAnalyzer* analyzer) {
    if (!analyzer ||
        FAILED(analyzer->AnalyzeLineBreakpoints(
            this, 0, static_cast<UINT32>(text_.size()), this))) {
      return false;
    }
    for (size_t i = 0; i + 1 < text_.size(); ++i) {
      if ((text_[i] == L'+' && !points_[i + 1].isWhitespace) ||
          (text_[i + 1] == L'+' && !points_[i].isWhitespace)) {
        points_[i].breakConditionAfter = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
      }
    }
    return true;
  }

  // IUnknown.
  IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IDWriteTextAnalysisSource)) {
      *out = static_cast<IDWriteTextAnalysisSource*>(this);
      return S_OK;
    }
    if (iid == __uuidof(IDWriteTextAnalysisSink)) {
      *out = static_cast<IDWriteTextAnalysisSink*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return 1; }
  IFACEMETHODIMP_(ULONG) Release() override { return 1; }

  // IDWriteTextAnalysisSource.
  IFACEMETHODIMP GetTextAtPosition(UINT32 position,
                                   const WCHAR** text,
                                   UINT32* length) override {
    const bool inside = position < text_.size();
    // SAFETY: `position` was just checked against the text's size.
    *text = inside ? UNSAFE_BUFFERS(text_.data() + position) : nullptr;
    *length = inside ? static_cast<UINT32>(text_.size() - position) : 0;
    return S_OK;
  }
  IFACEMETHODIMP GetTextBeforePosition(UINT32 position,
                                       const WCHAR** text,
                                       UINT32* length) override {
    const bool inside = position > 0 && position <= text_.size();
    *text = inside ? text_.data() : nullptr;
    *length = inside ? position : 0;
    return S_OK;
  }
  IFACEMETHODIMP_(DWRITE_READING_DIRECTION)
  GetParagraphReadingDirection() override {
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
  }
  IFACEMETHODIMP GetLocaleName(UINT32 position,
                               UINT32* length,
                               const WCHAR** locale) override {
    *length = static_cast<UINT32>(text_.size()) - position;
    *locale = L"";
    return S_OK;
  }
  IFACEMETHODIMP GetNumberSubstitution(
      UINT32 position,
      UINT32* length,
      IDWriteNumberSubstitution** substitution) override {
    *length = static_cast<UINT32>(text_.size()) - position;
    *substitution = nullptr;
    return S_OK;
  }

  // IDWriteTextAnalysisSink: only the breakpoints are asked for.
  IFACEMETHODIMP SetScriptAnalysis(UINT32,
                                   UINT32,
                                   const DWRITE_SCRIPT_ANALYSIS*) override {
    return S_OK;
  }
  IFACEMETHODIMP SetLineBreakpoints(
      UINT32 position,
      UINT32 length,
      const DWRITE_LINE_BREAKPOINT* points) override {
    // SAFETY: the analyzer hands over `length` points for the text it was
    // given, starting at `position`, and `points_` is that text's size.
    UNSAFE_BUFFERS(std::copy(points, points + length,
                             points_.begin() + position));
    return S_OK;
  }
  IFACEMETHODIMP SetBidiLevel(UINT32, UINT32, UINT8, UINT8) override {
    return S_OK;
  }
  IFACEMETHODIMP SetNumberSubstitution(UINT32,
                                       UINT32,
                                       IDWriteNumberSubstitution*) override {
    return S_OK;
  }

 private:
  const std::wstring_view text_;
  std::vector<DWRITE_LINE_BREAKPOINT> points_;
};

// Shared, lazily created, never torn down (a process-lifetime singleton). Null
// without DirectWrite, and text then goes to DrawText as it always did.
IDWriteTextAnalyzer* Analyzer() {
  static IDWriteTextAnalyzer* const analyzer = []() -> IDWriteTextAnalyzer* {
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextAnalyzer> made;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
        FAILED(factory->CreateTextAnalyzer(made.GetAddressOf()))) {
      return nullptr;
    }
    return made.Detach();
  }();
  return analyzer;
}

// Asks DrawText itself, in the mode that will draw the line. GDI's other
// measures disagree with it by a few pixels on kerned text ("nT", "Ty"), and a
// line it found too long by those few pixels would lose its last word.
bool FitsOneLine(HDC dc, std::wstring_view line, int width, int line_height) {
  RECT box = {0, 0, width, 0};
  const int height =
      ::DrawTextW(dc, line.data(), static_cast<int>(line.size()), &box,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  return height <= line_height && box.right <= width;
}

// False for text DrawText already breaks correctly, which stays on its path:
// plain ASCII, and East Asian text, which may break between any two characters
// and costs several times what Latin does to measure (font linking).
bool DrawTextMayCutAWord(std::wstring_view text) {
  const auto ascii_letter = [](wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c < 0x80 || c >= 0x2E80) {
      continue;
    }
    // An accented letter, Greek or Cyrillic.
    if (c < 0x2000) {
      return true;
    }
    // General punctuation only matters touching a word ("don’t"): a Chinese
    // sentence is full of curly quotes and dashes.
    if ((i > 0 && ascii_letter(text[i - 1])) ||
        (i + 1 < text.size() && ascii_letter(text[i + 1]))) {
      return true;
    }
  }
  return false;
}

std::wstring WrapLines(HDC dc,
                       std::wstring_view text,
                       int width,
                       int max_lines) {
  TEXTMETRICW metrics = {};
  if (!::GetTextMetricsW(dc, &metrics)) {
    return {};
  }
  const int line_height = metrics.tmHeight;
  if (FitsOneLine(dc, text, width, line_height)) {
    return {};
  }
  BreakAnalysis analysis(text);
  if (!analysis.Run(Analyzer())) {
    return {};
  }
  const std::vector<DWRITE_LINE_BREAKPOINT>& points = analysis.points();
  // A line's text without the spaces its break swallows.
  const auto trimmed = [&](size_t start, size_t end) {
    while (end > start && points[end - 1].isWhitespace) {
      --end;
    }
    return text.substr(start, end - start);
  };

  std::wstring wrapped;
  wrapped.reserve(text.size() + 8);
  std::vector<size_t> ends;
  size_t start = 0;
  int lines = 1;
  while (start < text.size() && (max_lines <= 0 || lines < max_lines)) {
    const size_t stop = std::min(text.find_first_of(L"\r\n", start),
                                 text.size());
    // Every place this line may end: the paragraph's end is always one.
    ends.clear();
    for (size_t i = start; i + 1 < stop; ++i) {
      if (points[i].breakConditionAfter == DWRITE_BREAK_CONDITION_CAN_BREAK) {
        ends.push_back(i + 1);
      }
    }
    ends.push_back(stop);
    // The last of them that fits. Longer only ever measures wider.
    size_t low = 0;
    size_t high = ends.size();
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      if (FitsOneLine(dc, trimmed(start, ends[middle]), width, line_height)) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    // None fits: the first word is wider than the line. It gets a line of its
    // own and DrawText cuts it where it always has.
    const size_t end = ends[low > 0 ? low - 1 : 0];
    if (end < stop) {
      wrapped.append(trimmed(start, end));
      wrapped.push_back(L'\n');
      ++lines;
      start = end;
      continue;
    }
    // The paragraph is done. Its own break, if it has one, stays as written.
    size_t next = stop;
    if (next < text.size()) {
      next += text.compare(next, 2, L"\r\n") == 0 ? 2 : 1;
      ++lines;
    }
    wrapped.append(text.substr(start, next - start));
    start = next;
  }
  wrapped.append(text.substr(start));
  return wrapped;
}

struct WrapKey {
  std::wstring text;
  std::wstring face;
  LONG font_height = 0;
  LONG font_weight = 0;
  BYTE font_italic = 0;
  int width = 0;
  int max_lines = 0;
  auto operator<=>(const WrapKey&) const = default;
};

// Past this the cache starts over: a resize leaves an entry per width.
constexpr size_t kMaxCachedWraps = 1024;

bool IsEditSpace(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
}

// Whether a line may start at `text[i]`, for 0 < i < text.size().
bool EditLineMayStartAt(std::wstring_view text, size_t i) {
  const wchar_t before = text[i - 1];
  const wchar_t after = text[i];
  if (IsEditSpace(after)) {
    return false;
  }
  if (before == L'\r' || before == L'\n') {
    return true;
  }
  // Wrapping asks this at every character it walks back over, and these two
  // answers are most of them, so they stay away from the analyzer.
  if (after < 0x2E80 && ::IsCharAlphaNumericW(after)) {
    if (IsEditSpace(before)) {
      return true;
    }
    if (before < 0x2E80 && ::IsCharAlphaNumericW(before)) {
      return false;
    }
  }
  // The rules only look a few characters either way.
  constexpr size_t kContext = 16;
  const size_t start = i > kContext ? i - kContext : 0;
  const std::wstring_view nearby = text.substr(start, i - start + kContext);
  BreakAnalysis analysis(nearby);
  if (!analysis.Run(Analyzer())) {
    return false;
  }
  return analysis.points()[i - start - 1].breakConditionAfter ==
         DWRITE_BREAK_CONDITION_CAN_BREAK;
}

// Where Ctrl+arrows and a double-click stop: the start of a word, or the end of
// a line, which they treat as a word of its own.
bool IsEditWordStop(std::wstring_view text, size_t i) {
  const wchar_t c = text[i];
  if (c == L'\r' || (c == L'\n' && text[i - 1] != L'\r')) {
    return true;
  }
  return !IsEditSpace(c) &&
         (IsEditSpace(text[i - 1]) || EditLineMayStartAt(text, i));
}

// What the last WB_ISDELIMITER answered, for the calls that follow it. See
// EditWordBreak.
int g_edit_no_at = -1;
int g_edit_yes_at = -1;
bool g_edit_second_right = false;

// EM_SETWORDBREAKPROC. An EDIT wraps by asking WB_ISDELIMITER about the first
// character that overflows, where yes ends the line before it, then about each
// character before that, where yes ends the line after it. If none says yes it
// cuts at the width. This cannot tell which question it is answering, so it
// says yes only when a line may end on both sides: a break is now and then
// missed, never misplaced.
//
// Ctrl+Left and a double-click call WB_LEFT, which must answer the word before
// when the caret starts a word, then WB_RIGHT from just inside that answer.
// Ctrl+Right asks WB_ISDELIMITER first. On a no it takes the same two calls,
// but WB_LEFT must then answer the word holding the caret, or the caret would
// never leave a word's start. On a yes it calls WB_RIGHT for where the next
// word starts, then WB_RIGHT from just inside that, and goes where the second
// lands. So the answer is remembered, and the second of those may land where
// it starts: after a one-character East Asian word the next begins right there.
int CALLBACK EditWordBreak(LPWSTR text, int current, int length, int code) {
  if (length <= 0) {
    return 0;
  }
  // SAFETY: the EDIT passes its text and that text's length.
  const std::wstring_view view =
      UNSAFE_BUFFERS(std::wstring_view(text, static_cast<size_t>(length)));
  const size_t size = view.size();
  const size_t at = static_cast<size_t>(std::clamp(current, 0, length));
  // A word no line could hold ends the search, or a long unbroken run would
  // cost a walk over all of it for each of its lines.
  constexpr size_t kReach = 1024;
  switch (code) {
    case WB_ISDELIMITER: {
      bool yes = false;
      if (at < size && IsEditSpace(view[at])) {
        yes = at + 1 == size || IsEditSpace(view[at + 1]) ||
              EditLineMayStartAt(view, at + 1);
      } else if (at < size) {
        yes = (at == 0 || EditLineMayStartAt(view, at)) &&
              (at + 1 == size || EditLineMayStartAt(view, at + 1));
      }
      g_edit_no_at = yes ? -1 : current;
      g_edit_yes_at = yes ? current : -1;
      g_edit_second_right = false;
      return yes;
    }
    case WB_LEFT: {
      const bool holding_caret = g_edit_no_at == current;
      g_edit_no_at = g_edit_yes_at = -1;
      g_edit_second_right = false;
      size_t i = holding_caret ? at : at - (at > 0 ? 1 : 0);
      i = std::min(i, size - 1);
      const size_t floor = i > kReach ? i - kReach : 0;
      while (i > floor && !IsEditWordStop(view, i)) {
        --i;
      }
      return static_cast<int>(i);
    }
    case WB_RIGHT: {
      const bool first_right = g_edit_yes_at == current;
      const bool second_right = g_edit_second_right;
      g_edit_yes_at = -1;
      g_edit_second_right = first_right;
      if (first_right && at < size &&
          (!IsEditSpace(view[at]) || view[at] == L'\r')) {
        return current;
      }
      size_t i = second_right && at > 0 ? at : at + 1;
      const size_t ceiling = std::min(size, at + kReach);
      while (i < ceiling && !IsEditWordStop(view, i)) {
        ++i;
      }
      return static_cast<int>(std::min(i, size));
    }
    default:
      return 0;
  }
}

}  // namespace

std::wstring PhiWrapTextLines(HDC dc,
                              std::wstring_view text,
                              int width,
                              int max_lines) {
  if (width <= 0 || text.empty() ||
      text.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !DrawTextMayCutAWord(text)) {
    return {};
  }
  LOGFONTW font = {};
  if (!::GetObjectW(::GetCurrentObject(dc, OBJ_FONT), sizeof(font), &font)) {
    return {};
  }
  // A window measures a row at layout and draws it on every paint, and each
  // line here costs several DrawText measures, so the answer is kept. UI
  // thread only, like the rest of the drawing layer.
  static base::NoDestructor<std::map<WrapKey, std::wstring>> cache;
  WrapKey key = {std::wstring(text), font.lfFaceName, font.lfHeight,
                 font.lfWeight,      font.lfItalic,   width,
                 max_lines};
  const auto found = cache->find(key);
  if (found != cache->end()) {
    return found->second;
  }
  std::wstring wrapped = WrapLines(dc, text, width, max_lines);
  if (cache->size() >= kMaxCachedWraps) {
    cache->clear();
  }
  cache->emplace(std::move(key), wrapped);
  return wrapped;
}

void PhiUseTextWrapRules(HWND edit) {
  ::SendMessageW(edit, EM_SETWORDBREAKPROC, 0,
                 reinterpret_cast<LPARAM>(&EditWordBreak));
}

}  // namespace phi_client
