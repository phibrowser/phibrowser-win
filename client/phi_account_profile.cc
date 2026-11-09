// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_account_profile.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_account_storage.h"
#include "client/phi_auth.h"
#include "client/phi_file_io.h"
#include "client/phi_http.h"

#pragma comment(lib, "windowscodecs.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kProfileSchema = 1;
// A second Refresh inside this window is served from the cache (the
// settings window re-activating twice in a row).
constexpr uint64_t kRefreshIntervalMs = 60 * 1000;
// The fork's profile_info_handler caps bridge-supplied avatars at 5 MB;
// the same here, decoded synchronously on the UI thread.
constexpr size_t kMaxAvatarBytes = 5 * 1024 * 1024;

std::optional<PhiAccountProfile> ProfileFromDict(const base::DictValue& d) {
  PhiAccountProfile profile;
  const auto read = [&](const char* key) {
    const std::string* value = d.FindString(key);
    return value ? *value : std::string();
  };
  profile.id = d.FindInt("id").value_or(0);
  profile.auth0_id = read("auth0_id");
  profile.email = read("email");
  profile.name = read("name");
  profile.picture = read("picture");
  profile.last_login = read("last_login");
  profile.created_at = read("created_at");
  if (profile.auth0_id.empty()) {
    return std::nullopt;
  }
  return profile;
}

base::DictValue DictFromProfile(const PhiAccountProfile& p) {
  base::DictValue d;
  d.Set("id", p.id);
  d.Set("auth0_id", p.auth0_id);
  d.Set("email", p.email);
  d.Set("name", p.name);
  d.Set("picture", p.picture);
  d.Set("last_login", p.last_login);
  d.Set("created_at", p.created_at);
  return d;
}

// One account-service call with the bearer, the Mac APIClient's shape: the
// envelope {code, message, data}; a 401 invalidates the token and retries
// once. Returns the error token, or empty with `data` filled.
std::string AccountCall(PhiAuth* auth,
                        const wchar_t* method,
                        const std::string& path,
                        const std::string& body,
                        base::DictValue* data) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string token = auth->AccessTokenBlocking();
    if (token.empty()) {
      return "no_token";
    }
    std::wstring headers = L"Accept: application/json\r\nAuthorization: Bearer " +
                           base::UTF8ToWide(token) + L"\r\n";
    if (!body.empty()) {
      headers += L"Content-Type: application/json\r\n";
    }
    const PhiHttpResult result =
        PhiHttpRequest(method, PhiAuth::AccountBaseUrl() + path, headers, body);
    if (!result.transport_ok) {
      return "offline";
    }
    if (result.status == 401) {
      auth->InvalidateAccessToken();
      continue;
    }
    if (result.status < 200 || result.status >= 300) {
      return "http_" + std::to_string(result.status);
    }
    std::optional<base::DictValue> envelope =
        base::JSONReader::ReadDict(result.body, base::JSON_PARSE_RFC);
    if (!envelope) {
      return "unreadable";
    }
    if (envelope->FindInt("code").value_or(-1) != 0) {
      return "refused";
    }
    if (base::DictValue* payload = envelope->FindDict("data")) {
      *data = std::move(*payload);
    }
    return std::string();
  }
  return "unauthorized";
}

// Downloads the avatar. The URL is stable while its content changes, so
// the request bypasses caches (the Mac's .forceRefresh).
bool FetchAvatar(const std::string& url, std::string* bytes) {
  if (url.rfind("https://", 0) != 0) {
    return false;  // Also refuses an empty URL.
  }
  const PhiHttpResult result = PhiHttpRequest(
      L"GET", url, L"Cache-Control: no-cache\r\nPragma: no-cache\r\n",
      std::string());
  if (!result.transport_ok || result.status < 200 || result.status >= 300 ||
      result.body.empty() || result.body.size() > kMaxAvatarBytes) {
    return false;
  }
  *bytes = result.body;
  return true;
}

ComPtr<IWICImagingFactory> WicFactory() {
  ComPtr<IWICImagingFactory> factory;
  ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  return factory;
}

// Decodes `bytes` (any WIC container), centre-crops to a square, scales to
// `px`, converts to premultiplied BGRA and masks a circle into the alpha.
std::vector<uint8_t> DecodeAvatarCircle(const std::string& bytes, int px) {
  std::vector<uint8_t> out;
  ComPtr<IWICImagingFactory> factory = WicFactory();
  if (!factory || px <= 0) {
    return out;
  }
  // WIC's own memory stream (no shlwapi, whose StrCat macro collides with
  // base/strings/strcat.h). The buffer must outlive the decode; `bytes`
  // does.
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(
          reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
          static_cast<DWORD>(bytes.size()))) ||
      FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    return out;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || !width || !height) {
    return out;
  }
  ComPtr<IWICBitmapSource> source = frame;
  if (width != height) {
    const UINT side = std::min(width, height);
    WICRect rect = {static_cast<INT>((width - side) / 2),
                    static_cast<INT>((height - side) / 2),
                    static_cast<INT>(side), static_cast<INT>(side)};
    ComPtr<IWICBitmapClipper> clipper;
    if (FAILED(factory->CreateBitmapClipper(&clipper)) ||
        FAILED(clipper->Initialize(source.Get(), &rect))) {
      return out;
    }
    source = clipper;
  }
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(source.Get(), static_cast<UINT>(px),
                                static_cast<UINT>(px),
                                WICBitmapInterpolationModeFant))) {
    return out;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return out;
  }
  out.resize(static_cast<size_t>(px) * px * 4);
  if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(px * 4),
                                   static_cast<UINT>(out.size()),
                                   out.data()))) {
    out.clear();
    return out;
  }
  // The circle: coverage by distance from the centre, one pixel of
  // anti-aliasing, multiplied into the premultiplied channels.
  const double centre = px / 2.0;
  const double radius = px / 2.0;
  for (int y = 0; y < px; ++y) {
    for (int x = 0; x < px; ++x) {
      const double dx = x + 0.5 - centre;
      const double dy = y + 0.5 - centre;
      const double distance = std::sqrt(dx * dx + dy * dy);
      const double coverage =
          std::clamp(radius - distance + 0.5, 0.0, 1.0);
      if (coverage >= 1.0) {
        continue;
      }
      const size_t base = (static_cast<size_t>(y) * px + x) * 4;
      for (size_t c = 0; c < 4; ++c) {
        out[base + c] =
            static_cast<uint8_t>(std::lround(out[base + c] * coverage));
      }
    }
  }
  return out;
}

}  // namespace

// A worker's task: fetch (GET profile, then its avatar) or update (PUT
// name, then the avatar if the picture moved).
struct PhiAccountProfileService::Job {
  enum class Kind { kFetch, kUpdateName };
  Kind kind = Kind::kFetch;
  RAW_PTR_EXCLUSION PhiAccountProfileService* service = nullptr;
  RAW_PTR_EXCLUSION PhiAuth* auth = nullptr;  // Outlives every job.
  uint32_t generation = 0;
  std::string expected_sub;
  std::string fallback_picture;  // Fetched when the profile GET fails.
  std::string cached_avatar_url;  // Skip the download when unchanged and
  std::string name;               // the cache has bytes (see below).
  bool have_cached_avatar = false;
};

struct PhiAccountProfileService::Result {
  uint32_t generation = 0;
  std::optional<PhiAccountProfile> profile;  // Set when the call succeeded.
  bool profile_call_failed = false;
  std::string error;  // "profile" / "update" / empty.
  std::optional<std::string> avatar_bytes;  // Set when a download landed.
  std::string avatar_url;
};

PhiAccountProfileService::PhiAccountProfileService() {
  ::InitializeCriticalSection(&lock_);
}

PhiAccountProfileService::~PhiAccountProfileService() {
  ::DeleteCriticalSection(&lock_);
}

void PhiAccountProfileService::Load(const std::wstring& user_data_dir,
                                    HWND notify_hwnd,
                                    UINT notify_msg) {
  user_data_dir_ = user_data_dir;
  notify_hwnd_ = notify_hwnd;
  notify_msg_ = notify_msg;
}

void PhiAccountProfileService::BindSubject(const std::string& sub) {
  profile_.reset();
  avatar_bytes_.clear();
  avatar_url_.clear();
  avatar_circles_.clear();
  const std::wstring dir = PhiEnsureAccountDir(user_data_dir_, sub);
  if (dir.empty()) {
    profile_path_.clear();
    avatar_path_.clear();
    return;
  }
  profile_path_ = dir + L"\\PhiAccountProfile.json";
  avatar_path_ = dir + L"\\PhiAvatar.img";
  base::DictValue dict;
  if (PhiReadJsonFile(profile_path_, &dict) == PhiJsonFileStatus::kOk) {
    profile_ = ProfileFromDict(dict);
    if (const std::string* url = dict.FindString("avatarUrl")) {
      avatar_url_ = *url;
    }
  }
  std::string bytes;
  if (PhiReadFileBytes(avatar_path_, &bytes) && !bytes.empty() &&
      bytes.size() <= kMaxAvatarBytes) {
    avatar_bytes_ = std::move(bytes);
  }
}

void PhiAccountProfileService::SyncWithSession(
    PhiAuth* auth,
    const std::string& sub,
    const std::string& fallback_picture) {
  if (sub.empty()) {
    if (!synced_sub_.empty()) {
      synced_sub_.clear();
      ++generation_;  // Results for the old subject are stale now.
    }
    return;
  }
  if (sub != synced_sub_) {
    ++generation_;
    synced_sub_ = sub;
    last_fetch_tick_ = 0;
    last_error_.clear();
    // Each account keeps its own cache: read this one's.
    BindSubject(sub);
  }
  if (!busy_ && last_fetch_tick_ == 0) {
    auto job = std::make_unique<Job>();
    job->kind = Job::Kind::kFetch;
    job->auth = auth;
    job->expected_sub = sub;
    job->fallback_picture = fallback_picture;
    StartJob(std::move(job));
  }
}

void PhiAccountProfileService::Refresh(PhiAuth* auth, bool force) {
  if (synced_sub_.empty() || busy_) {
    return;
  }
  const uint64_t now = ::GetTickCount64();
  if (!force && last_fetch_tick_ != 0 &&
      now - last_fetch_tick_ < kRefreshIntervalMs) {
    return;
  }
  auto job = std::make_unique<Job>();
  job->kind = Job::Kind::kFetch;
  job->auth = auth;
  job->expected_sub = synced_sub_;
  job->fallback_picture = profile_ ? profile_->picture : std::string();
  StartJob(std::move(job));
}

void PhiAccountProfileService::UpdateName(PhiAuth* auth,
                                          const std::string& name) {
  if (synced_sub_.empty() || busy_) {
    return;
  }
  auto job = std::make_unique<Job>();
  job->kind = Job::Kind::kUpdateName;
  job->auth = auth;
  job->expected_sub = synced_sub_;
  job->name = name;
  StartJob(std::move(job));
}

void PhiAccountProfileService::StartJob(std::unique_ptr<Job> job) {
  job->service = this;
  job->generation = generation_;
  job->cached_avatar_url = avatar_url_;
  job->have_cached_avatar = !avatar_bytes_.empty();
  HANDLE thread = ::CreateThread(nullptr, 0, &PhiAccountProfileService::JobThread,
                                 job.get(), 0, nullptr);
  if (!thread) {
    return;
  }
  ::CloseHandle(thread);
  job.release();  // The thread owns it now.
  busy_ = true;
  last_fetch_tick_ = ::GetTickCount64();
}

// static
DWORD WINAPI PhiAccountProfileService::JobThread(void* param) {
  std::unique_ptr<Job> job(static_cast<Job*>(param));
  auto result = std::make_unique<Result>();
  result->generation = job->generation;

  base::DictValue data;
  std::string error;
  if (job->kind == Job::Kind::kFetch) {
    error = AccountCall(job->auth, L"GET", "/api/auth/profile", std::string(),
                        &data);
  } else {
    base::DictValue body;
    body.Set("name", job->name);
    std::string json;
    base::JSONWriter::Write(body, &json);
    error = AccountCall(job->auth, L"PUT", "/api/auth/profile", json, &data);
  }
  std::string picture = job->fallback_picture;
  if (error.empty()) {
    result->profile = ProfileFromDict(data);
    if (!result->profile || result->profile->auth0_id != job->expected_sub) {
      // Not this account's profile: the service answered for somebody
      // else, or the session moved underneath the call.
      result->profile.reset();
      error = "mismatch";
    } else {
      picture = result->profile->picture;
    }
  }
  if (!error.empty()) {
    LOG(WARNING) << "[PhiAccount] profile "
                 << (job->kind == Job::Kind::kFetch ? "fetch" : "update")
                 << " failed: " << error;
    result->profile_call_failed = true;
    result->error =
        job->kind == Job::Kind::kFetch ? "profile" : "update";
  }
  // The avatar: a fresh download when the profile landed (the content can
  // change behind a stable URL), else only when the cache has nothing for
  // the picture we know of.
  const bool want_avatar =
      !picture.empty() &&
      (error.empty() || picture != job->cached_avatar_url ||
       !job->have_cached_avatar);
  if (want_avatar) {
    std::string bytes;
    if (FetchAvatar(picture, &bytes)) {
      result->avatar_bytes = std::move(bytes);
      result->avatar_url = picture;
    }
  }

  PhiAccountProfileService* service = job->service;
  ::EnterCriticalSection(&service->lock_);
  service->pending_ = std::move(result);
  ::LeaveCriticalSection(&service->lock_);
  if (service->notify_hwnd_ && ::IsWindow(service->notify_hwnd_)) {
    ::PostMessageW(service->notify_hwnd_, service->notify_msg_, 0, 0);
  }
  return 0;
}

bool PhiAccountProfileService::ApplyPending() {
  std::unique_ptr<Result> result;
  ::EnterCriticalSection(&lock_);
  result = std::move(pending_);
  ::LeaveCriticalSection(&lock_);
  if (!result) {
    return false;
  }
  bool changed = busy_;
  busy_ = false;
  if (result->generation != generation_) {
    return changed;  // Computed for a subject that is no longer this one.
  }
  if (result->profile) {
    profile_ = std::move(result->profile);
    PersistProfile();
    changed = true;
  }
  if (last_error_ != result->error) {
    last_error_ = result->error;
    changed = true;
  }
  if (result->avatar_bytes) {
    avatar_bytes_ = std::move(*result->avatar_bytes);
    avatar_url_ = result->avatar_url;
    avatar_circles_.clear();
    PersistAvatar();
    PersistProfile();  // The URL rides the profile file.
    changed = true;
  }
  return changed;
}

void PhiAccountProfileService::Clear() {
  ++generation_;
  synced_sub_.clear();
  profile_.reset();
  avatar_bytes_.clear();
  avatar_url_.clear();
  avatar_circles_.clear();
  last_error_.clear();
  last_fetch_tick_ = 0;
  if (!profile_path_.empty()) {
    ::DeleteFileW(profile_path_.c_str());
    ::DeleteFileW(avatar_path_.c_str());
  }
}

const std::vector<uint8_t>& PhiAccountProfileService::AvatarCircleBgra(
    int px) {
  static const std::vector<uint8_t> kNone;
  if (avatar_bytes_.empty() || px <= 0) {
    return kNone;
  }
  auto found = avatar_circles_.find(px);
  if (found == avatar_circles_.end()) {
    found = avatar_circles_.emplace(px, DecodeAvatarCircle(avatar_bytes_, px))
                .first;
    if (found->second.empty()) {
      LOG(WARNING) << "[PhiAccount] avatar bytes do not decode";
    }
  }
  return found->second;
}

void PhiAccountProfileService::PersistProfile() {
  if (profile_path_.empty()) {
    return;
  }
  base::DictValue dict = profile_ ? DictFromProfile(*profile_) : base::DictValue();
  dict.Set("avatarUrl", avatar_url_);
  PhiWriteJsonFile(profile_path_, std::move(dict), kProfileSchema);
}

void PhiAccountProfileService::PersistAvatar() {
  if (avatar_path_.empty()) {
    return;
  }
  if (avatar_bytes_.empty()) {
    ::DeleteFileW(avatar_path_.c_str());
    return;
  }
  PhiWriteFileAtomic(avatar_path_, avatar_bytes_);
}

}  // namespace phi_client
