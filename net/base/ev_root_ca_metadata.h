// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_EV_ROOT_CA_METADATA_H_
#define NET_BASE_EV_ROOT_CA_METADATA_H_
#pragma once

#include "build/build_config.h"

#if defined(USE_NSS)
#include <secoidt.h>
#endif

#include <map>
#include <vector>

#include "net/base/net_export.h"
#include "net/base/x509_certificate.h"

namespace base {
template <typename T>
struct DefaultLazyInstanceTraits;
}  // namespace base

namespace net {

// A singleton.  This class stores the meta data of the root CAs that issue
// extended-validation (EV) certificates.
class NET_EXPORT_PRIVATE EVRootCAMetadata {
 public:
#if defined(USE_NSS)
  typedef SECOidTag PolicyOID;
#elif defined(OS_WIN)
  typedef const char* PolicyOID;
#endif

  static EVRootCAMetadata* GetInstance();

#if defined(USE_NSS)
  // If the root CA cert has an EV policy OID, returns true and appends the
  // policy OIDs to |*policy_oids|.  Otherwise, returns false.
  bool GetPolicyOIDsForCA(const SHA1Fingerprint& fingerprint,
                          std::vector<PolicyOID>* policy_oids) const;
  const PolicyOID* GetPolicyOIDs() const;
  int NumPolicyOIDs() const;
#elif defined(OS_WIN)
  // Returns true if policy_oid is an EV policy OID of some root CA.
  bool IsEVPolicyOID(PolicyOID policy_oid) const;

  // Returns true if the root CA with the given certificate fingerprint has
  // the EV policy OID policy_oid.
  bool HasEVPolicyOID(const SHA1Fingerprint& fingerprint,
                      PolicyOID policy_oid) const;
#endif

  // AddEVCA adds an EV CA to the list of known EV CAs with the given policy.
  // |policy| is expressed as a string of dotted numbers. It returns true on
  // success.
  bool AddEVCA(const SHA1Fingerprint& fingerprint, const char* policy);

  // RemoveEVCA removes an EV CA that was previously added by AddEVCA. It
  // returns true on success.
  bool RemoveEVCA(const SHA1Fingerprint& fingerprint);

 private:
  friend struct base::DefaultLazyInstanceTraits<EVRootCAMetadata>;

  EVRootCAMetadata();
  ~EVRootCAMetadata();

#if defined(USE_NSS)
  typedef std::map<SHA1Fingerprint, std::vector<PolicyOID>,
                   SHA1FingerprintLessThan> PolicyOIDMap;

  // RegisterOID registers |policy|, a policy OID in dotted string form, and
  // writes the memoized form to |*out|. It returns true on success.
  static bool RegisterOID(const char* policy, PolicyOID* out);

  PolicyOIDMap ev_policy_;
  std::vector<PolicyOID> policy_oids_;
#elif defined(OS_WIN)
  typedef std::map<SHA1Fingerprint, std::string,
                   SHA1FingerprintLessThan> ExtraEVCAMap;

  // extra_cas_ contains any EV CA metadata that was added at runtime.
  ExtraEVCAMap extra_cas_;
#endif

  DISALLOW_COPY_AND_ASSIGN(EVRootCAMetadata);
};

}  // namespace net

#endif  // NET_BASE_EV_ROOT_CA_METADATA_H_
