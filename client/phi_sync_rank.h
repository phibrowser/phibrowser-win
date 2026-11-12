// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_RANK_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_RANK_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Fractional ranks, the wire's ordering channel for Spaces, bookmarks, pinned
// tabs and URL rules — phibrowser-mac Sources/Sync/Phi/SyncableSpaces.swift
// §7 (rankBetween, longestIncreasingKeptSet, assignRanks, isLegalRank),
// ported line for line. A rank is a base-62 string over an ASCII-ascending
// alphabet, so byte comparison IS numeric comparison of the fraction
// 0.<rank>; locally the stores keep dense integer indexes and only the sync
// layer speaks ranks.
//
// Comparison is std::string's bytewise order. Swift compares Strings by
// Unicode scalars, which agrees for the ASCII ranks and uuids these functions
// are ever handed — isLegalRank is the decoding boundary that keeps anything
// else out, exactly as on the Mac.
//
// PURE: STL only, host-tested against the Mac-generated vectors
// (test/sync-vectors: rank_between, rank_is_legal, rank_assign).

namespace phi_client::sync {

inline constexpr char kRankAlphabet[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// Non-empty, not ending in the lowest digit, every character in the
// alphabet: a rank RankBetween could itself have produced, and so one it may
// safely be handed as a bound.
bool IsLegalRank(const std::string& rank);

// A rank strictly between `lower` and `upper` (either may be absent), never
// ending in the lowest digit. Preconditions, as on the Mac (which traps):
// lower < upper when both are present; upper is legal as an upper bound. A
// violated precondition aborts — returning a rank outside the bounds would
// silently corrupt the account's order.
std::string RankBetween(const std::optional<std::string>& lower,
                        const std::optional<std::string>& upper);

struct RankedItem {
  std::string uuid;
  std::optional<std::string> rank;
};

// Indices to leave alone: the longest strictly increasing subsequence of
// `order` under (rank, uuid). Unranked items never join it.
std::set<size_t> LongestIncreasingKeptSet(const std::vector<RankedItem>& order);

// New ranks for the items outside the kept set, keyed by uuid; kept items are
// absent from the result (they are not rewritten and not re-stamped).
std::map<std::string, std::string> AssignRanks(
    const std::vector<RankedItem>& order);

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_RANK_H_
