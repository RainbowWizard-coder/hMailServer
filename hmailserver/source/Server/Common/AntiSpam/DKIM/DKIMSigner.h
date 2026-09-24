// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Message;
   class MimeHeader;

   class DKIMSigner
   {
   public:
      DKIMSigner();

      // Applies the outbound authentication headers of this server to the
      // message: a DKIM signature when we are the author domain, and an ARC set
      // when we are relaying. The two are independent - see Sign().
      void Sign(std::shared_ptr<Message> message);

      // DKIM-signs the message as a hosted domain that is not its author's:
      // a managed mailing list's copies, signed as the list's domain
      // (Util/ListManager.h), because the list changed the posting and RFC 8058
      // section 4 asks for a valid signature over List-Unsubscribe and
      // List-Unsubscribe-Post before a receiver offers its one-click. Signs with
      // the domain's primary selector and key and its own canonicalization, as
      // Sign does for the author's domain. False, and nothing written, when the
      // domain does not sign - DKIM off, or no selector or key - or signing
      // failed; the message then goes as it is.
      bool SignAsDomain(std::shared_ptr<Message> message, const AnsiString &domainName);

   private:

      // DKIM-signs the message as the author domain. Returns true if a
      // signature was added, in which case the identity that was used is
      // returned in the out parameters so ARC can seal with the same key.
      bool SignAsAuthorDomain_(std::shared_ptr<Message> message,
                               const AnsiString &header,
                               MimeHeader &mimeHeader,
                               AnsiString &signingDomain,
                               AnsiString &signingSelector,
                               String &signingPrivateKeyFile);

      // True if at least one recipient is delivered off this server, which is
      // the only case where an ARC set is of any use to anyone.
      static bool IsRelayedOnward_(std::shared_ptr<Message> message);

      // Works out which local domain should seal a message we are relaying on
      // somebody else's behalf. Returns false when no local domain with a
      // usable DKIM key can be determined.
      static bool ResolveArcSealingIdentity_(std::shared_ptr<Message> message,
                                             MimeHeader &mimeHeader,
                                             AnsiString &sealingDomain,
                                             AnsiString &sealingSelector,
                                             String &sealingPrivateKeyFile);

      // Returns the signing identity of a hosted domain, if that domain has
      // DKIM enabled and both a selector and a private key configured.
      static bool GetHostedDomainIdentity_(const AnsiString &domainName,
                                           AnsiString &signingDomain,
                                           AnsiString &signingSelector,
                                           String &signingPrivateKeyFile);
   };

}
