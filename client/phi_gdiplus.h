// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GDIPLUS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GDIPLUS_H_

namespace phi_client {

// Starts GDI+ for the process, once, and reports whether it is usable.
// EVERY client file that touches a Gdiplus:: type must call this before its
// first one and honour a false: GDI+ objects created without a live token
// fail in ways that read as "the image would not load".
//
// One token, not one per file: GdiplusStartup is refcounted per token, and
// four independent tokens (the agent overlay, the drag image, the split
// drop target, the window capture) meant four background threads and four
// heaps for one process's worth of drawing. The token is deliberately never
// shut down — GdiplusShutdown invalidates every outstanding GDI+ object, and
// this layer's callers hold theirs inside painting code that runs until the
// message loop stops, so a shutdown at exit would race the very objects it
// exists to free. Process teardown reclaims it.
//
// UI thread only, like the rest of the client's drawing.
bool PhiEnsureGdiPlus();

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GDIPLUS_H_
