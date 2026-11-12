// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SKILL_INSTALLER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SKILL_INSTALLER_H_

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace phi_client {

// The phi-browser skill installer behind Settings ▸ Developer ▸ "Add skill
// to…" — the Mac's SkillInstallRowView (DeveloperSettingsView.swift).
//
// The skill tree ships beside the exe as resources\phi-browser-skill (the
// Mac bundles the same folder under Contents/Resources). Installing links
// it into an agent's user-level skills folder as <skills>\phi-browser, so
// the agent reads the copy this build carries and an update of Phi updates
// the skill. The Mac makes a symlink; Windows makes a DIRECTORY JUNCTION,
// which is what references/install.md tells a hand-installer to make too:
// a symlink needs Developer Mode or elevation, a junction needs neither,
// and Node's realpath follows both.

// A coding agent that loads skills from a folder. Paths are relative to
// the user's profile folder, each agent's documented user-level skills
// folder (the agentskills.io convention), the same list as the Mac's.
struct PhiSkillTarget {
  const char* id;
  const wchar_t* name;
  // The brand mark leading the agent's menu row (phi_agent_icons.h id).
  const char* icon;
  const wchar_t* skills_subdir;
};

const std::vector<PhiSkillTarget>& PhiSkillTargets();

// The link an install creates for `target`, and the folder shown beside
// its name in the menu ("~\.claude\skills").
std::wstring PhiSkillLinkPath(const PhiSkillTarget& target);
std::wstring PhiSkillDisplayPath(const PhiSkillTarget& target);

// <exe dir>\resources\phi-browser-skill, whether or not it exists.
std::wstring PhiBundledSkillDir();

// Ids of the targets whose skills folder already links to THIS exe's
// bundle (a link left by another build reads as not installed, so picking
// the agent again refreshes it).
std::set<std::string> PhiInstalledSkillTargetIds();

enum class PhiSkillInstallOutcome {
  kSuccess,
  kCancelled,  // The user declined to replace a real folder in the way.
  kFailure,
};
struct PhiSkillInstallResult {
  PhiSkillInstallOutcome outcome = PhiSkillInstallOutcome::kFailure;
  std::wstring error;  // kFailure: the localized reason.
};

// Links the bundled skill into `target`'s skills folder.
// A real directory or file already at a link path is never clobbered
// unasked: `confirm_overwrite` is called with that path and a false answer
// cancels the whole install before anything changed — so declining the
// companion replacement cannot leave a partial install. Silent otherwise;
// the caller presents the outcome, so "All agents" can aggregate.
PhiSkillInstallResult PhiInstallSkill(
    const PhiSkillTarget& target,
    const std::function<bool(const std::wstring& path)>& confirm_overwrite);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SKILL_INSTALLER_H_
