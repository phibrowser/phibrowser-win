// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_

#include <windows.h>

#include <string>
#include <vector>

#include "client/phi_agent_transcript.h"
#include "client/phi_theme.h"

namespace phi_client {

// What the panel needs from its owner (the coordinator). All calls on the
// browser UI thread.
class PhiTranscriptHost {
 public:
  virtual ~PhiTranscriptHost() = default;
  // The task's transcript, oldest first (empty when unknown). An empty
  // `task_id` means every task, interleaved by seq (the "All tasks" filter).
  virtual std::vector<PhiTranscriptEntry> TranscriptEntries(
      const std::string& task_id) = 0;
  // The driver's live tail row ("working…"), or empty.
  virtual std::string TranscriptActivity(const std::string& task_id) = 0;
  // Every buffered task, for the filter menu: live ones first.
  virtual std::vector<PhiTranscriptTaskInfo> TranscriptTasks() = 0;
  // The driving agent's display name, for the panel title.
  virtual std::string TranscriptAgentName(const std::string& task_id) = 0;
  // Whether the task is live; `running` reports its run state (the console
  // hints that an idle agent only sees a command at its next round).
  virtual bool TranscriptTaskLive(const std::string& task_id,
                                  bool* running) = 0;
  // Queues a console message from the user (the driver drains it on its
  // next poll). False when the task is gone.
  virtual bool PushTranscriptMessage(const std::string& task_id,
                                     const std::string& text) = 0;
  // The console's Clear: one task's buffer, or all when empty.
  virtual void ClearTranscript(const std::string& task_id) = 0;
  // The console closed: ended buffers it was keeping alive may go.
  virtual void TranscriptConsoleClosed() = 0;
};

// The live transcript console for one agent Space — the Windows shape of the
// Mac AgentTranscriptPanel's floating-window placement: a resizable tool
// window with the task's log feed above and a console input row below. One
// panel per process, retargeted by Toggle; the feed follows agentSpace.log
// through RefreshOpen (the extensions popover's pattern).
class PhiTranscriptPanel {
 public:
  // Opens the panel on `task_id` (or closes it when it is already showing
  // that task — the Mac toggle). `owner` is the shell window the panel
  // parks beside.
  static void Toggle(HWND owner,
                     const PhiPalette& palette,
                     const std::string& task_id,
                     PhiTranscriptHost* host);
  // Re-reads the feed if the panel is open (new log lines arrived).
  static void RefreshOpen();
  // Closes the panel if it is showing `task_id` (the task ended).
  static void CloseFor(const std::string& task_id);
  // Whether the console is open at all — the skill's session-mirror gate
  // (agentSpace.list's `transcriptMirror`): with nobody reading a
  // transcript, the driving agent spawns no mirror daemon.
  static bool IsOpen();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_
