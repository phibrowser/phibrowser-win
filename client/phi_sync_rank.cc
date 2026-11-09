// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_sync_rank.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace phi_client::sync {

namespace {

constexpr std::string_view kAlphabet(kRankAlphabet, sizeof(kRankAlphabet) - 1);
constexpr int kBase = static_cast<int>(kAlphabet.size());

// The Mac's rankIndex lookup: an unknown character reads as the lowest digit.
int DigitOf(char c) {
  const size_t at = kAlphabet.find(c);
  return at == std::string_view::npos ? 0 : static_cast<int>(at);
}

std::vector<int> Digits(const std::string& rank) {
  std::vector<int> out;
  out.reserve(rank.size());
  for (char c : rank) {
    out.push_back(DigitOf(c));
  }
  return out;
}

bool KeyLess(const std::string& rank_a,
             const std::string& uuid_a,
             const std::string& rank_b,
             const std::string& uuid_b) {
  return rank_a == rank_b ? uuid_a < uuid_b : rank_a < rank_b;
}

}  // namespace

bool IsLegalRank(const std::string& rank) {
  if (rank.empty() || rank.back() == '0') {
    return false;
  }
  for (char c : rank) {
    if (kAlphabet.find(c) == std::string_view::npos) {
      return false;
    }
  }
  return true;
}

std::string RankBetween(const std::optional<std::string>& lower,
                        const std::optional<std::string>& upper) {
  // The Mac's two preconditions. Both are unanswerable requests; a rank
  // outside the bounds would corrupt the order, so this stops instead.
  if (lower && upper && !(*lower < *upper)) {
    std::abort();
  }
  if (upper && (upper->empty() || upper->back() == '0')) {
    std::abort();
  }
  const std::vector<int> low = Digits(lower.value_or(std::string()));
  std::optional<std::vector<int>> high;
  if (upper) {
    high = Digits(*upper);
  }

  std::vector<int> out;
  size_t position = 0;
  while (true) {
    const int lo = position < low.size() ? low[position] : 0;
    int hi = kBase;
    if (high) {
      // Once `out` is already strictly greater than the upper bound's prefix,
      // the bound stops constraining later digits. `out` always holds exactly
      // `position` digits here.
      const bool on_prefix =
          position < high->size() &&
          std::equal(out.begin(), out.end(), high->begin());
      if (on_prefix) {
        hi = (*high)[position];
      }
    }
    if (hi - lo > 1) {
      out.push_back((lo + hi) / 2);
      break;
    }
    // Adjacent (or equal) digits: keep the lower one and refine.
    out.push_back(lo);
    ++position;
  }
  std::string rank;
  rank.reserve(out.size());
  for (int digit : out) {
    rank.push_back(kAlphabet[static_cast<size_t>(digit)]);
  }
  return rank;
}

std::set<size_t> LongestIncreasingKeptSet(
    const std::vector<RankedItem>& order) {
  // Patience sorting, strict: the first tail NOT less than the key is
  // replaced.
  std::vector<size_t> tail_index;  // Indices into `order`.
  std::vector<long> previous(order.size(), -1);
  for (size_t i = 0; i < order.size(); ++i) {
    if (!order[i].rank) {
      continue;
    }
    const std::string& rank = *order[i].rank;
    const std::string& uuid = order[i].uuid;
    size_t lo = 0;
    size_t hi = tail_index.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      const RankedItem& tail = order[tail_index[mid]];
      if (KeyLess(*tail.rank, tail.uuid, rank, uuid)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    previous[i] = lo > 0 ? static_cast<long>(tail_index[lo - 1]) : -1;
    if (lo == tail_index.size()) {
      tail_index.push_back(i);
    } else {
      tail_index[lo] = i;
    }
  }
  std::set<size_t> kept;
  if (tail_index.empty()) {
    return kept;
  }
  // The last tail slot always ends SOME longest increasing subsequence.
  long cursor = static_cast<long>(tail_index.back());
  while (cursor >= 0) {
    kept.insert(static_cast<size_t>(cursor));
    cursor = previous[static_cast<size_t>(cursor)];
  }
  return kept;
}

std::map<std::string, std::string> AssignRanks(
    const std::vector<RankedItem>& order) {
  const std::set<size_t> kept = LongestIncreasingKeptSet(order);
  std::vector<bool> kept_flags(order.size(), false);
  for (size_t i : kept) {
    kept_flags[i] = true;
  }

  std::map<std::string, std::string> assigned;
  // A freshly assigned rank wins over the item's shadow rank.
  const auto effective = [&](size_t i) -> std::optional<std::string> {
    const auto it = assigned.find(order[i].uuid);
    if (it != assigned.end()) {
      return it->second;
    }
    return order[i].rank;
  };

  for (size_t i = 0; i < order.size(); ++i) {
    if (kept_flags[i]) {
      continue;
    }
    // Left: the nearest finalised rank to the left (kept, or assigned earlier
    // in this pass), so several items in one gap come out increasing.
    std::optional<std::string> left;
    for (size_t j = i; j-- > 0;) {
      if (std::optional<std::string> r = effective(j)) {
        left = std::move(r);
        break;
      }
    }
    // Right: the nearest kept rank to the right. While it is not strictly
    // greater than `left` it is evicted into the complement (it gets a new
    // rank when the loop reaches it) and the search moves on.
    std::optional<std::string> right;
    for (size_t k = i + 1; k < order.size(); ++k) {
      if (kept_flags[k] && order[k].rank) {
        const std::string& r = *order[k].rank;
        if (left && !(*left < r)) {
          kept_flags[k] = false;
          continue;
        }
        right = r;
        break;
      }
    }
    assigned[order[i].uuid] = RankBetween(left, right);
  }
  return assigned;
}

}  // namespace phi_client::sync
