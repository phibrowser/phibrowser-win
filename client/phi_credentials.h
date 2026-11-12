// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CREDENTIALS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CREDENTIALS_H_

#include <string>
#include <vector>

namespace phi_client {

// One vault item — the Windows twin of the Mac CredentialProvider item. Only
// logins carry username/password/uri; other types keep their body in `notes`.
struct PhiCredentialItem {
  std::string type = "login";  // login | note | card | identity | sshKey
  std::string name;
  std::string domain;
  std::string username;
  std::string password;
  std::string uri;
  std::string credential_id;
  std::string notes;
};

// The client-side password vault backing the phi-browser skill's
// `credentials.*` surface — the Windows stand-in for the Mac's Bitwarden
// provider. The vault file (`PhiCredentials.dat` in the user-data dir) is
// DPAPI-encrypted for the current user. ONLY a --dev-hooks build
// (kPhiDevHooks) also accepts a plaintext JSON array with the same shape as
// a test seed; a release build refuses anything DPAPI cannot unwrap and
// leaves the vault empty, so a file dropped next to the profile can never
// become "the user's passwords".
//
// Pure data + matching — no UI. The coordinator owns the per-request
// approve/deny prompt and the origin binding; this class only stores items
// and answers queries. Host matching mirrors the skill's `credHostMatches`
// (equal, or one a subdomain of the other with the parent holding a dot).
class PhiCredentialVault {
 public:
  PhiCredentialVault();
  ~PhiCredentialVault();

  // Reads and decrypts the vault. Safe to call again to reload.
  void Load(const std::wstring& user_data_dir);

  // Whether the vault file exists (drives status logged_out vs ready).
  bool present() const { return present_; }
  bool ready() const { return present_; }
  size_t size() const { return items_.size(); }

  // Returns pointers to the items matching a query. Exactly one of `domain`,
  // `id`, `search` should be non-empty (domain wins, then id, then search),
  // matching the Mac precedence. `username` narrows a domain match to one
  // account. Pointers stay valid until the next Load().
  std::vector<const PhiCredentialItem*> Match(const std::string& domain,
                                              const std::string& id,
                                              const std::string& search,
                                              const std::string& username) const;

  // True when `page_host` and `cred_host` belong together (credHostMatches).
  static bool HostMatches(const std::string& page_host,
                          const std::string& cred_host);
  // Lowercased host of a URL/URI, tolerating a missing scheme (hostOfUri).
  // A bracketed IPv6 literal comes back whole, brackets on ("[::1]"), the
  // way GURL spells it; empty when there is no host to be had.
  static std::string HostOfUri(const std::string& uri);

 private:
  std::vector<PhiCredentialItem> items_;
  bool present_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CREDENTIALS_H_
