// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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

      // Whether the server is entitled to sign this message as the domain its From
      // names: a message a signed-in user submitted (Message::FlagSubmittedBySignedInUser,
      // which a copy of it carries), or one whose From no outside sender wrote - one
      // this server generated, or one an IP range let in as one of this server's own
      // domains without signing in, as it always has. Not a message from outside
      // (Message::GetFromOutside), nor a forward, a redirect, a rule's copy, the mirror
      // or a managed list's copy of one: its From is the outside sender's, a signature
      // of the domain it names would vouch for a sender this server never saw sign in,
      // and the signature the message came with stays as it came, with the ARC set
      // this server seals over it when it relays the message onward. Nor, since 7
      // October 2026, a signed-in submission whose From names another domain hosted
      // here that the account may not send as (SubmitterMaySignAs), which
      // SMTPConnection::RecordOrigin_ marks as it marks mail from outside, so that no
      // copy of it is signed as that domain either.
      //
      // THE DEFECT. SignAsAuthorDomain_ signed every message whose From named a domain
      // hosted here with DKIM on, on its first delivery pass, whatever it was: mail
      // from outside with such a From was signed before local delivery, and a forward
      // of it - an account's, a rule's, a Sieve redirect, an alias or a distribution
      // list to an outside address - left this server signed as that domain, a forgery
      // DMARC passed.
      static bool SignsAsAuthorDomain(const Message &message);

      // The address SignAsAuthorDomain_ signs a message as, and its domain: the first
      // address of the message's first From field, or the envelope sender when that
      // field names none.
      static void AuthorOf(MimeHeader &mimeHeader, const String &envelopeSender, AnsiString &address, AnsiString &domain);

      // Whether a message an account signed in as accountAddress submitted may be
      // signed as the domain authorAddress (AuthorOf) names: yes when that is no
      // domain hosted here, since no signature of this server's is then made, and when
      // it is the account's own domain, through the domain aliases, whatever the
      // address in it - as it always was; and for another domain hosted here only
      // when the account may send as that address (SMTPConnection::SenderPermittedFor,
      // the rule the webmail's send and COM's Message.Save hold a From to), with
      // reason saying why not.
      //
      // Until 7 October 2026 a signed-in user's submission was signed as whatever
      // domain hosted here its From named. Authenticated SMTP holds no From to the
      // account (SmtpAuthenticatedSenderCheck, off by default, holds MAIL FROM alone),
      // so an account of one domain that wrote From: ceo@ another had the message
      // signed with that other domain's key, which DMARC then passed.
      static bool SubmitterMaySignAs(const String &accountAddress, const String &authorAddress, String &reason);

      // The id of the domain hosted here that sends the message on, whose key seals
      // the ARC set over it when this server does not sign it as its author's domain
      // (Sign), or 0: what the server recorded where it made the message
      // (Message::GetRelayingDomainID - an account's or a domain's forward, rule,
      // Sieve redirect, list or journal copy); else the domain of the account that
      // signed in and submitted it (Message::GetSubmitterAccountID); else the hosted
      // domain a recipient that leaves this server - every recipient, with
      // anyRecipient - was accepted at (MessageRecipient::GetOriginalAddress: an alias
      // or a distribution list expanded to an address outside). Each is the server's
      // own record.
      //
      // THE DEFECT. The sealing domain was the first hosted domain with a key among
      // the envelope sender, the recipients' original addresses and the
      // X-Original-Rcpt-To field - which a sender writes when AddXOriginalRcptTo is
      // off, and a Sieve addheader or a rule's Set header value can write above this
      // server's own: so whoever wrote it chose which hosted domain's key sealed the
      // forward, and an outside sender's envelope domain could be a hosted one. The
      // seal's results stayed this server's, but its signer did not.
      static __int64 RelayingDomainID(std::shared_ptr<Message> message, bool anyRecipient);

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

      // The identity that seals a message we relay on somebody else's behalf: the
      // relaying domain's (RelayingDomainID). False when none is recorded, or the one
      // recorded has no usable DKIM key: the message then goes unsealed, and no other
      // domain is tried.
      static bool ResolveArcSealingIdentity_(std::shared_ptr<Message> message,
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

   // Which messages DKIMSigner may sign as the domain their From names, held to
   // examples: a signed-in submission, a generated message and one from a trusted IP
   // range may be signed, and a message from outside may not, nor a signed-in
   // submission marked so because its From is another's (DKIMSigner::AuthorOf finds
   // the address the signer reads); the mark that says so is
   // carried by every copy a forward, a redirect, a rule or the mirror makes
   // (PersistentMessage::CreateCopy_) and by Message's copy constructor; and it is kept
   // on a queue row beside the domains DMARC vouched for, read back as it was written,
   // with the domains dropped and the mark kept when both would not fit the column, and
   // a row written before the mark existed read as it was. No configuration, database
   // or network. ClassTester runs it on Windows, business-object-tests on Linux. Throws
   // std::logic_error naming every case that went wrong.
   class SigningOriginTester
   {
   public:
      void Test();
   };
}
