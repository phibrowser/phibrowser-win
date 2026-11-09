// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_CONTAINERS_SPAN_H_
#define BASE_CONTAINERS_SPAN_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace base {

inline constexpr size_t dynamic_extent = std::dynamic_extent;

// std::span with the handful of base::span additions the client uses.
template <typename T, size_t N = dynamic_extent>
class span : public std::span<T, N> {
 public:
  using std::span<T, N>::span;
  constexpr span() = default;
  constexpr span(const std::span<T, N>& other) : std::span<T, N>(other) {}
  template <typename U, size_t M>
    requires(!std::is_same_v<std::span<U, M>, std::span<T, N>> &&
             std::is_convertible_v<U (*)[], T (*)[]> &&
             (N == dynamic_extent || M == dynamic_extent || N == M))
  constexpr span(const span<U, M>& other)
      : std::span<T, N>(other.data(), other.size()) {}

  constexpr span<T> first(size_t count) const {
    return std::span<T, N>::first(count);
  }
  constexpr span<T> last(size_t count) const {
    return std::span<T, N>::last(count);
  }
  constexpr span<T> subspan(size_t offset,
                            size_t count = dynamic_extent) const {
    return std::span<T, N>::subspan(offset, count);
  }
  template <size_t M>
  constexpr span<T, M> first() const {
    return span<T, M>(this->data(), M);
  }
  template <size_t M>
  constexpr span<T, M> last() const {
    return span<T, M>(this->data() + this->size() - M, M);
  }
  template <size_t Offset, size_t M>
  constexpr span<T, M> subspan() const {
    return span<T, M>(this->data() + Offset, M);
  }

  // Requires the sizes to match, as base::span does.
  template <typename U, size_t M>
  void copy_from(const span<U, M>& other) const {
    if (other.size() != this->size()) {
      __fastfail(0);
    }
    if (!other.empty()) {
      std::copy_n(other.data(), other.size(), this->data());
    }
  }
  template <typename U, size_t M>
  void copy_from(const std::span<U, M>& other) const {
    copy_from(span<U, M>(other));
  }
  void copy_from_nonoverlapping(const span<const std::remove_cv_t<T>>& other) const {
    copy_from(other);
  }
};

template <typename It>
span(It, size_t) -> span<std::remove_reference_t<decltype(*std::declval<It>())>>;
template <typename T, size_t N>
span(T (&)[N]) -> span<T, N>;
template <typename Container>
span(Container&) -> span<std::remove_reference_t<decltype(*std::declval<Container&>().data())>>;
template <typename Container>
span(const Container&) -> span<const std::remove_reference_t<decltype(*std::declval<const Container&>().data())>>;

template <typename T>
span<const uint8_t> as_byte_span(const T& container) {
  auto view = std::as_bytes(std::span(container));
  return span<const uint8_t>(reinterpret_cast<const uint8_t*>(view.data()),
                             view.size());
}
inline span<const uint8_t> as_byte_span(std::string_view text) {
  return span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
                             text.size());
}

}  // namespace base

#endif  // BASE_CONTAINERS_SPAN_H_
