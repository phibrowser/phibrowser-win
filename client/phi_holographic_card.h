// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_HOLOGRAPHIC_CARD_H_
#define PHIBROWSER_WIN_CLIENT_PHI_HOLOGRAPHIC_CARD_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IWICImagingFactory;

namespace phi_client {

// The Mac's HolographicCardStyle (Sources/UserInterface/Onboarding/Welcome/
// HolographicCardMaterials.swift): the three discrete selections that make a
// card. Everything else — angles, opacities, the gradient stops — is fixed.
struct PhiHolographicCardStyle {
  // In the Mac's order; each palette pairs with a built-in theme id.
  enum class Palette {
    kSilver,  // pure
    kGreen,   // mint
    kAqua,    // aqua
    kBlue,    // mist
    kViolet,  // iris
    kPink,    // petal
    kCoral,   // coral
    kGold,    // amber
  };
  enum class Texture { kArcs, kWaves, kGuilloche, kBrushed, kDiamond, kStardust };
  enum class Ornament { kLace, kLotus, kPeriodic, kModulated, kEpicycloid, kNone };

  static constexpr int kPaletteCount = 8;
  static constexpr int kTextureCount = 6;
  static constexpr int kOrnamentCount = 6;

  Palette palette = Palette::kSilver;
  Texture texture = Texture::kArcs;
  Ornament ornament = Ornament::kLace;

  bool operator==(const PhiHolographicCardStyle&) const = default;

  // The Mac's `.random()`: every selection uniformly.
  static PhiHolographicCardStyle Random();
  // The palette <-> theme id pairing (silver <-> pure, green <-> mint, ...);
  // an unknown theme id is silver.
  static Palette PaletteForTheme(const std::string& theme_id);
  static const char* ThemeIdFor(Palette palette);
  // The Mac's raw values ("silver", "arcs", "lace") — the persisted shape.
  static const char* Name(Palette palette);
  static const char* Name(Texture texture);
  static const char* Name(Ornament ornament);
  std::map<std::string, std::string> ToMap() const;
  // Missing or unknown values fall back to the default selection.
  static PhiHolographicCardStyle FromMap(
      const std::map<std::string, std::string>& map);
  // The three enhanced ornaments draw a heavier groove (the Mac's
  // `Ornament.enhanced`).
  bool enhanced_ornament() const;
};

// The Mac's HolographicCardView: the membership card shared by the
// onboarding Welcome page and Settings ▸ You, authored at 292 x 423 dips
// with a 28 dip corner. The card is a stack of procedural layers — the
// palette's base gradient, an iridescence gradient and a radial highlight,
// one of six engraved foil textures, seeded grain, the Phi logo with a
// moving sweep, one of six engraved seal ornaments, a moving sheen, an edge
// hairline — composited here in software at the card's pixel size.
//
// The expensive layers (the foil texture, the grain, the seal's stroked
// masks) are rasterised ONCE per style and size on a worker thread with
// GDI+ and cached under the profile directory; the cheap, light-driven ones
// (gradients, sheen, sweep) are recomposed per frame from the pointer. The
// pointer over the card moves the light and tilts the card a little (a
// perspective warp of the composed face); when it leaves, the card settles
// flat. No idle sway. A card appearing spins in once (the Mac's entrance:
// scale, a full turn showing the dark reverse, fade) unless the OS asks
// for reduced motion.
//
// The card draws NOTHING until its materials are ready (the Mac keeps the
// rotor hidden): owners pre-warm with Prepare() and repaint on the notify
// message. UI thread except where noted.
class PhiHolographicCard {
 public:
  static constexpr int kDesignWidthDip = 292;
  static constexpr int kDesignHeightDip = 423;
  static constexpr int kCornerDip = 28;

  PhiHolographicCard();
  ~PhiHolographicCard();

  PhiHolographicCard(const PhiHolographicCard&) = delete;
  PhiHolographicCard& operator=(const PhiHolographicCard&) = delete;

  // `msg` is posted to `hwnd` when a material landed (repaint; Paint shows
  // the card once ready()).
  void SetNotify(HWND hwnd, UINT msg);
  // The material cache's directory (<user_data_dir>\PhiCardArtwork); the
  // artwork version is appended. Set before the first Prepare / Paint; an
  // empty dir rasterises without caching.
  void SetCacheDir(const std::wstring& dir);

  // The copy: the name (IvyPresto Headline Light 31 — Segoe UI Light when
  // the face lacks the glyphs), the member id (monospaced 10) and the
  // "Date Joined" + "yyyy.MM" rows, drawn only when `joined` is non-empty.
  void SetIdentity(const std::string& name,
                   const std::string& member_id,
                   const std::string& joined);
  // A new style keeps showing the old one until its materials land.
  void SetStyle(const PhiHolographicCardStyle& style);
  const PhiHolographicCardStyle& style() const { return style_; }

  // Starts rasterising style()'s materials for a card of `card_rect`'s
  // pixel size (its width sets the scale: 292 dips). Paint calls this too;
  // calling it early (the onboarding window opening) hides the wait.
  void Prepare(const RECT& card_rect);
  // style()'s materials at the last prepared size have landed.
  bool ready() const;

  // The pointer over `card_rect` in client px, or nothing when it left
  // (the Mac's normalizedPointer). `card_rect` is the rect Paint gets.
  void SetPointer(const std::optional<POINT>& client_pt, const RECT& card_rect);
  // Plays the entrance again (a page showing the card anew).
  void ReplayEntrance();
  // Advances the motion; true when the card wants a repaint. animating()
  // says whether the owner should keep a ~16 ms timer running for Tick().
  bool Tick();
  bool animating() const;

  // Paints the card into `card_rect` (W x H px of the design aspect). The
  // shadow and a tilted card spill up to MarginPx() beyond the rect on every
  // side, over whatever ground the owner painted there.
  void Paint(HDC dc, const RECT& card_rect);
  static int MarginPx(const RECT& card_rect);

  // The Mac's ProfileCardViewController.exportImage: the front face, flat
  // light, at 2x, centred on a canvas twice its size filled with `canvas`,
  // saved as PNG through the system save dialog (default "<name>.png").
  // Blocks on the dialog; renders after it. False when nothing was saved.
  bool ExportPng(HWND owner, COLORREF canvas);

  // The Mac's memberID: the first 8 bytes of SHA-256(sub) as lowercase hex;
  // empty for an empty sub.
  static std::string MemberId(const std::string& sub);
  // The Mac's formattedJoinDate: ISO 8601 ("2025-11-05T11:32:18Z", with or
  // without fractional seconds) -> "yyyy.MM" in local time; empty when the
  // string is not that shape.
  static std::string FormatJoinDate(const std::string& iso);

 private:
  struct Material;   // The worker's product for one (style, size).
  struct Mailbox;    // Where workers leave finished materials.
  struct Job;
  struct Statics;    // The per-size, light-independent layers.
  struct Motion;
  struct Light;

  static DWORD WINAPI MaterialThread(void* param);
  // Rasterises (or loads from `dir`) the materials for `style` at `width` x
  // `height`. Any thread; the worker's and the export's shared body.
  static std::shared_ptr<const Material> BuildMaterial(IWICImagingFactory* factory,
                                                       const std::wstring& dir,
                                                       const PhiHolographicCardStyle& style,
                                                       int width,
                                                       int height);
  // Moves finished materials from the mailbox into the caches.
  void PumpMailbox();
  // Materials for `style` at `width` x `height`, or null while pending
  // (and a job is started when none is).
  std::shared_ptr<const Material> MaterialFor(const PhiHolographicCardStyle& style,
                                              int width,
                                              int height,
                                              bool start_job);
  // (Re)builds the static layers when the size, the style or the copy
  // changed.
  void EnsureStatics(int width, int height, const PhiHolographicCardStyle& style);
  // The composed card (premultiplied BGRA, width x height) for the current
  // light, for the front or the reverse.
  std::vector<uint8_t> ComposeFace(const Material& material,
                                   const Light& light,
                                   bool back) const;
  // The card drawn through the entrance / tilt transform into a layer of
  // (width + 2 margin) x (height + 2 margin) premultiplied BGRA.
  std::vector<uint8_t> ComposeLayer(const Material& material, int margin);
  bool ReducedMotion() const;

  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
  std::wstring cache_dir_;
  PhiHolographicCardStyle style_;
  // The style whose card is on screen: style_ once its materials landed,
  // else the previous one (the Mac keeps the old card until the new is
  // ready).
  std::optional<PhiHolographicCardStyle> shown_style_;
  std::string name_;
  std::string member_id_;
  std::string joined_;

  // The materials: one entry per (texture, ornament, w, h); the worker
  // reuses cached files across styles that share a texture or ornament.
  std::map<std::string, std::shared_ptr<const Material>> materials_;
  std::map<std::string, uint32_t> pending_;  // key -> generation
  std::shared_ptr<Mailbox> mailbox_;
  int width_ = 0;
  int height_ = 0;
  // The static layers for (width_, height_, style_, copy).
  std::unique_ptr<Statics> statics_;
  // The last composed layer and the light it was composed for, so a paint
  // with nothing moved is a blit.
  std::vector<uint8_t> layer_;
  int layer_margin_ = 0;
  std::string layer_key_;
  std::unique_ptr<Motion> motion_;
  std::optional<std::pair<double, double>> pointer_;  // Normalised -1..1.
  uint64_t last_tick_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_HOLOGRAPHIC_CARD_H_
