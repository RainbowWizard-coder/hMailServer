// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The large mailbox providers, as the IP warm-up counts them.
//
// A new sending address earns its reputation provider by provider: Google,
// Microsoft, Yahoo, Apple and Proton each keep their own score for it, and each
// is what a warm-up schedule is written against. So the IP warm-up
// (SMTP/IpWarmup.h) counts a day's recipients per provider, and this decides
// which provider a delivery goes to.
//
// It is decided from the MAIL HOST a delivery is about to connect to - an MX
// host of an MX delivery, the target host of a route or a relay - and never
// from the recipient's domain: a company whose mail is on Google Workspace is
// reached through Google's MX hosts and is Google's to judge, whatever its
// domain is called. The host is the one the batch's source address was chosen
// for, the first it will try (ExternalDelivery::DeliverToSingleDomain_), so
// that the address and the provider are about the same connection: a backup
// MX of another provider further down the list decides nothing, and a batch
// that falls back to one is counted against the first host's provider. A host
// belongs to a provider when its name is one of the provider's suffixes or ends
// with a dot and one of them, lower-cased and with a trailing dot dropped - so
// aspmx.l.google.com is Google's and notgoogle.com is not.
//
// The table is fixed, and deliberately: it is in docs/IpWarmup.md, and it is
// not editable, because the five providers it names are the ones whose
// reputation systems a warm-up schedule is written for, and a sixth would have
// no schedule of its own to follow.

#pragma once

namespace HM
{
   class OutboundProviders
   {
   public:
      enum Provider
      {
         Google = 0,
         Microsoft = 1,
         Yahoo = 2,
         Apple = 3,
         Proton = 4,
         Other = 5
      };

      // How many there are, Other included: the providers are 0 to Count - 1.
      static const int Count = 6;

      // The provider one mail host belongs to, or Other.
      static Provider ClassifyHost(const String &hostName);

      // The provider's key: google, microsoft, yahoo, apple, proton, other. What
      // hm_warmupcounts stores and the REST routes answer.
      static String Key(Provider provider);

      // The provider a key names; false for a key that is none of the six.
      static bool FromKey(const String &key, Provider &provider);

      // The provider's name for a sentence: Google, Microsoft, Yahoo, Apple,
      // Proton, or "other providers".
      static String DisplayName(Provider provider);
   };
}
