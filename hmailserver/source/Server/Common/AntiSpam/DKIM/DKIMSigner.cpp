// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DKIMSigner.h"
#include "DKIM.h"
#include "Arc.h"

#include "Canonicalization.h"

#include "../../BO/Account.h"
#include "../../BO/Message.h"
#include "../../BO/MessageRecipient.h"
#include "../../BO/MessageRecipients.h"
#include "../../BO/Domain.h"
#include "../../BO/DomainAliases.h"
#include "../../Application/ObjectCache.h"
#include "../../Cache/CacheContainer.h"
#include "../../Util/Hashing/HashCreator.h"
#include "../../Mime/Mime.h"
#include "../../Persistence/PersistentMessage.h"
#include "../../Persistence/MessageStore.h"
#include "../../Util/Parsing/AddresslistParser.h"
#include "../../Util/Unicode.h"
#include "../../Util/ListManager.h"
#include "../QuarantineStore.h"
#include "../../../SMTP/SMTPConnection.h"

#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Appends a domain to the candidate list, lower-cased and without
      // duplicates. The order of the list is the order in which candidates are
      // tried, so it must be preserved.
      void AddCandidateDomain(std::vector<AnsiString> &candidates, const AnsiString &domainName)
      {
         if (domainName.IsEmpty())
            return;

         AnsiString candidate = domainName;
         candidate.MakeLower();

         for (const AnsiString &existing : candidates)
         {
            if (existing == candidate)
               return;
         }

         candidates.push_back(candidate);
      }

      // Appends the domain of an address. If the address is at a domain alias,
      // the domain the alias points at is appended as well - that is the domain
      // which actually owns the key.
      void AddCandidateAddress(std::vector<AnsiString> &candidates, const String &address)
      {
         if (address.IsEmpty())
            return;

         AnsiString domainName = StringParser::ExtractDomain(address);
         AddCandidateDomain(candidates, domainName);

         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
         if (domainAliases)
         {
            AnsiString aliasedDomainName = StringParser::ExtractDomain(domainAliases->ApplyAliasesOnAddress(address));
            AddCandidateDomain(candidates, aliasedDomainName);
         }
      }
   }

   DKIMSigner::DKIMSigner()
   {

   }

   void
   DKIMSigner::Sign(std::shared_ptr<Message> message)
   {
      // Load the message header once. It will be reused for both domain lookup
      // and signing, avoiding a second read from disk inside DKIM::Sign.
      const String fileName = MessageStore::WorkingFile(message);
      AnsiString header = PersistentMessage::LoadHeader(fileName);

      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), false);

      AnsiString sealingDomain;
      AnsiString sealingSelector;
      String sealingPrivateKeyFile;

      bool signedAsAuthorDomain = SignAsAuthorDomain_(message, header, mimeHeader, sealingDomain, sealingSelector, sealingPrivateKeyFile);

      // ARC sealing (RFC 8617). ARC exists to carry the authentication results
      // we observed across a hop that breaks SPF and DKIM - a forward, an alias,
      // a distribution list - so it must not depend on whether we happened to
      // DKIM-sign the message as the author domain. It previously did, which
      // meant that relayed third-party mail, the only mail ARC is actually for,
      // was never sealed.
      //
      // When we did sign as the author domain we seal with exactly the same
      // identity as before, so that behaviour is unchanged. Otherwise we look
      // for the local domain that is handling the relay.
      if (!IniFileSettings::Instance()->GetArcSealingEnabled())
         return;

      if (!signedAsAuthorDomain)
      {
         if (!IsRelayedOnward_(message))
         {
            // Nothing leaves this server. A seal would be of no use to any
            // downstream receiver, and we would rather not add three headers to
            // every message that is only delivered into a local mailbox.
            return;
         }

         if (!ResolveArcSealingIdentity_(message, mimeHeader, sealingDomain, sealingSelector, sealingPrivateKeyFile))
         {
            LOG_DEBUG("ARC: No local domain with a usable DKIM key handles this relayed message. Not sealing.");
            return;
         }

         if (!FileUtilities::Exists(sealingPrivateKeyFile))
         {
            // A configuration problem rather than a per-message one, but it must
            // never stop the message: it is delivered without a seal.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5725, "DKIMSigner::Sign",
               "ARC sealing is enabled but the private key file of the sealing domain does not exist. The message is delivered without an ARC seal.");
            return;
         }
      }

      Arc arc;
      if (!arc.Seal(message, sealingDomain, sealingSelector, sealingPrivateKeyFile))
      {
         // Sealing is best effort. An unsealable message is still delivered -
         // the seal is extra information for the next hop, never a precondition
         // for delivery.
         LOG_DEBUG("ARC: The message could not be sealed. Delivering it unsealed.");
      }
   }

   bool
   DKIMSigner::SignAsAuthorDomain_(std::shared_ptr<Message> message,
                                   const AnsiString &header,
                                   MimeHeader &mimeHeader,
                                   AnsiString &signingDomain,
                                   AnsiString &signingSelector,
                                   String &signingPrivateKeyFile)
   {
      // Only a message this server is entitled to sign as its author's domain - not
      // mail from outside, nor a copy of it (SignsAsAuthorDomain). Such a message goes
      // with the signature it came with, and Sign seals it with ARC when it relays it.
      if (!SignsAsAuthorDomain(*message))
      {
         if (message->GetSubmittedBySignedInUser())
         {
            LOG_DEBUG("DKIM: not signing as the From's domain: the account that signed in and submitted the message may not send as its From, which names another domain hosted here.");
         }
         else
         {
            LOG_DEBUG("DKIM: not signing as the From's domain: the message came from outside, and its From is its sender's.");
         }
         return false;
      }

      // Determine the signing domain from the RFC 5322 From: header, not the
      // envelope from (MAIL FROM). DMARC alignment requires d= to match the
      // From: header domain.
      AnsiString senderDomain;
      AnsiString senderAddress;
      AuthorOf(mimeHeader, message->GetFromAddress(), senderAddress, senderDomain);

      std::shared_ptr<DomainAliases> pDA = ObjectCache::Instance()->GetDomainAliases();
      // try to get mailbox from the alias (if it is an alias actually)
      String sSender = pDA->ApplyAliasesOnAddress(senderAddress);
      AnsiString mbDomain = StringParser::ExtractDomain(sSender);

      // was the sender address from the main domain already?
      bool sameDomain = senderDomain.CompareNoCase(mbDomain) == 0;

      // Check if signing is enabled for this domain.
      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(mbDomain);

      if (!pDomain || !pDomain->GetDKIMEnabled())
         return false;
      // main domain signing enabled, do we have the sender address from the main domain or do we allow signing of the aliases?
      if (!(pDomain->GetDKIMAliasesEnabled() || sameDomain))
         return false;

      LOG_DEBUG("Signing message using DKIM...");

      // Only the primary selector/key ever signs. The domain may also hold a
      // secondary pair (GetDKIMSecondarySelector), but that pair exists purely
      // to stage a key rotation: it is configuration waiting for its DNS record
      // to propagate, and by definition that record may not be resolvable yet.
      // Signing with it as well would attach a signature that fails
      // verification at every receiver until propagation completes - and while
      // RFC 6376 section 6.1 tells verifiers to accept a message on any one
      // passing signature, real-world filters are known to score broken
      // signatures negatively, so a second signature can only lower
      // deliverability, never raise it. A failure to read the new key must
      // also never break signing with the old key that is still valid. The
      // rotation therefore cuts over atomically via Domain::PromoteDKIMSecondary
      // instead of ever signing with both.
      AnsiString selector = pDomain->GetDKIMSelector();
      // the senderDomain is either the main domain or it is allowed to sign using the key from the main domain
      AnsiString domain = senderDomain;
      AnsiString privateKeyFile = pDomain->GetDKIMPrivateKeyFile();

      if (selector.IsEmpty() || privateKeyFile.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5305, "DKIMSigner::Sign", "Either the selector or private key file was not specified.");
         return false;
      }

      Canonicalization::CanonicalizeMethod headerMethod = (Canonicalization::CanonicalizeMethod) pDomain->GetDKIMHeaderCanonicalizationMethod();
      Canonicalization::CanonicalizeMethod bodyMethod = (Canonicalization::CanonicalizeMethod) pDomain->GetDKIMBodyCanonicalizationMethod();
      HashCreator::HashType algorithm = (HashCreator::HashType) pDomain->GetDKIMSigningAlgorithm();

      // The other half of RFC 8301 3.1: a signer MUST NOT use rsa-sha1. A domain
      // configured for it is signed with rsa-sha256 instead rather than left
      // unsigned, because the choice between them is the domain owner's expression
      // of "sign my mail", not of "sign it weakly" - and substituting the stronger
      // hash can only make the signature more likely to be honoured. It is announced
      // once per message rather than silently, since the configuration says one thing
      // and the header will say another.
      if (algorithm == HashCreator::SHA1 && !IniFileSettings::Instance()->GetDkimAcceptSha1())
      {
         algorithm = HashCreator::SHA256;

         LOG_APPLICATION("DKIM: domain " + domain + " is configured to sign with rsa-sha1, which RFC 8301 "
                         "removed from DKIM. Signing with rsa-sha256 instead. Change the domain's DKIM "
                         "algorithm to SHA-256 to stop this notice, or set DkimAcceptSha1=1 to sign with "
                         "SHA-1 anyway.");
      }

      DKIM dkim;
      if (!dkim.Sign(message, header, domain, selector, privateKeyFile, algorithm, headerMethod, bodyMethod))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5306, "DKIMSigner::Sign", "Message signing using DKIM failed.");
         return false;
      }

      signingDomain = domain;
      signingSelector = selector;
      signingPrivateKeyFile = privateKeyFile;

      return true;
   }

   bool
   DKIMSigner::SignsAsAuthorDomain(const Message &message)
   {
      // A signed-in submission is marked so too when its From names another domain
      // hosted here that the account may not send as (SMTPConnection::RecordOrigin_,
      // SubmitterMaySignAs), and every copy of it carries the mark.
      return !message.GetFromOutside();
   }

   void
   DKIMSigner::AuthorOf(MimeHeader &mimeHeader, const String &envelopeSender, AnsiString &address, AnsiString &domain)
   {
      address.Empty();
      domain.Empty();

      MimeField *fromField = mimeHeader.GetField("From");
      if (fromField)
      {
         AddresslistParser parser;
         auto addresses = parser.ParseList(String(fromField->GetValue()));
         if (!addresses.empty())
         {
            address = addresses[0]->sMailboxName + "@" + addresses[0]->sDomainName;
            domain = addresses[0]->sDomainName;
         }
      }

      if (domain.IsEmpty())
      {
         address = envelopeSender;
         domain = StringParser::ExtractDomain(envelopeSender);
      }
   }

   bool
   DKIMSigner::SubmitterMaySignAs(const String &accountAddress, const String &authorAddress, String &reason)
   {
      reason.Empty();

      // The domain the signature would be of, through the domain aliases, as
      // SignAsAuthorDomain_ finds its key: none hosted here, and no signature of
      // this server's says anything about the address.
      std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
      const String resolvedAuthor = domainAliases ? domainAliases->ApplyAliasesOnAddress(authorAddress) : authorAddress;
      const String signingDomainName = StringParser::ExtractDomain(resolvedAuthor);
      if (signingDomainName.IsEmpty())
         return true;

      std::shared_ptr<const Domain> signingDomain = CacheContainer::Instance()->GetDomain(signingDomainName);
      if (!signingDomain)
         return true;

      // The account's own domain, whatever the address in it: as it always was.
      const String resolvedAccount = domainAliases ? domainAliases->ApplyAliasesOnAddress(accountAddress) : accountAddress;
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(resolvedAccount);
      if (account ? account->GetDomainID() == signingDomain->GetID()
                  : StringParser::ExtractDomain(resolvedAccount).CompareNoCase(signingDomain->GetName()) == 0)
         return true;

      // Another domain's: only an address the account may send as, the rule the
      // webmail's send and COM's Message.Save hold a From to (SendAs.md).
      return SMTPConnection::SenderPermittedFor(accountAddress, authorAddress, reason);
   }

   bool
   DKIMSigner::SignAsDomain(std::shared_ptr<Message> message, const AnsiString &domainName)
   {
      if (!message || domainName.IsEmpty())
         return false;

      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(domainName);
      if (!pDomain || !pDomain->GetDKIMEnabled())
         return false;

      // A domain that signs but lacks its selector or its key is reported by
      // Sign, once per message, when the message's own delivery pass reaches
      // it; asked here for every member's copy of a posting, it would be the
      // same line a thousand times.
      AnsiString selector = pDomain->GetDKIMSelector();
      AnsiString privateKeyFile = pDomain->GetDKIMPrivateKeyFile();
      if (selector.IsEmpty() || privateKeyFile.IsEmpty())
         return false;

      Canonicalization::CanonicalizeMethod headerMethod = (Canonicalization::CanonicalizeMethod) pDomain->GetDKIMHeaderCanonicalizationMethod();
      Canonicalization::CanonicalizeMethod bodyMethod = (Canonicalization::CanonicalizeMethod) pDomain->GetDKIMBodyCanonicalizationMethod();
      HashCreator::HashType algorithm = (HashCreator::HashType) pDomain->GetDKIMSigningAlgorithm();

      // RFC 8301 3.1, as for the author's domain: rsa-sha256, not rsa-sha1.
      if (algorithm == HashCreator::SHA1 && !IniFileSettings::Instance()->GetDkimAcceptSha1())
         algorithm = HashCreator::SHA256;

      const String fileName = MessageStore::WorkingFile(message);
      AnsiString header = PersistentMessage::LoadHeader(fileName);

      DKIM dkim;
      if (!dkim.Sign(message, header, domainName, selector, privateKeyFile, algorithm, headerMethod, bodyMethod))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5306, "DKIMSigner::SignAsDomain", "Message signing using DKIM failed.");
         return false;
      }

      return true;
   }

   bool
   DKIMSigner::IsRelayedOnward_(std::shared_ptr<Message> message)
   {
      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return false;

      // owned by this call: the recipients of the message being signed, held by its delivery
      for (std::shared_ptr<MessageRecipient> recipient : recipients->GetVector())
      {
         // A recipient without a local account is delivered by the external
         // delivery path, which is to say it leaves this server.
         if (recipient && recipient->GetLocalAccountID() == 0)
            return true;
      }

      return false;
   }

   bool
   DKIMSigner::ResolveArcSealingIdentity_(std::shared_ptr<Message> message,
                                          MimeHeader &mimeHeader,
                                          AnsiString &sealingDomain,
                                          AnsiString &sealingSelector,
                                          String &sealingPrivateKeyFile)
   {
      // An explicit ArcSealingDomain setting, if one is ever added to
      // IniFileSettings, belongs at the top of this list: an administrator who
      // names a sealing domain means that domain and nothing else. Until then
      // the sealing identity is derived from the message, which for every relay
      // shape hMailServer supports names the local domain doing the relaying.
      std::vector<AnsiString> candidates;

      // 1) The envelope sender. When a forward rewrites MAIL FROM - SRS, or
      //    RewriteEnvelopeFromWhenForwarding - this is the forwarding domain.
      //    It is also the submitting domain for an authenticated local user who
      //    sends with somebody else's address in From:.
      AddCandidateAddress(candidates, message->GetFromAddress());

      // 2) The address each recipient was originally addressed as. An alias or a
      //    distribution list at a local domain rewrites the recipient address to
      //    the external member, but the original address still names the local
      //    domain that is relaying the message.
      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (recipients)
      {
         // owned by this call: the recipients of the message being signed, held by its delivery
         for (std::shared_ptr<MessageRecipient> recipient : recipients->GetVector())
         {
            if (recipient)
               AddCandidateAddress(candidates, recipient->GetOriginalAddress());
         }
      }

      // 3) X-Original-Rcpt-To, written at reception when AddXOriginalRcptToHeader
      //    is enabled. On the queued copy of an account-level forward this is the
      //    only remaining reference to the local mailbox that forwarded, since
      //    the copy carries the external forward target as its recipient.
      MimeField *originalRecipientField = mimeHeader.GetField("X-Original-Rcpt-To");
      if (originalRecipientField)
      {
         AddresslistParser parser;
         auto addresses = parser.ParseList(String(originalRecipientField->GetValue()));

         for (std::shared_ptr<Address> address : addresses)
         {
            if (address)
               AddCandidateAddress(candidates, address->sMailboxName + "@" + address->sDomainName);
         }
      }

      for (const AnsiString &candidate : candidates)
      {
         if (GetHostedDomainIdentity_(candidate, sealingDomain, sealingSelector, sealingPrivateKeyFile))
            return true;
      }

      return false;
   }

   bool
   DKIMSigner::GetHostedDomainIdentity_(const AnsiString &domainName,
                                        AnsiString &signingDomain,
                                        AnsiString &signingSelector,
                                        String &signingPrivateKeyFile)
   {
      if (domainName.IsEmpty())
         return false;

      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(domainName);

      if (!pDomain || !pDomain->GetIsActive() || !pDomain->GetDKIMEnabled())
         return false;

      // The primary pair only, for the same reason DKIM signing uses it: an
      // ARC seal has to verify at the very next hop, and the secondary pair is
      // by definition still waiting for its DNS record to propagate.
      AnsiString selector = pDomain->GetDKIMSelector();
      String privateKeyFile = pDomain->GetDKIMPrivateKeyFile();

      if (selector.IsEmpty() || privateKeyFile.IsEmpty())
         return false;

      // d= must name the domain which owns the key. Unlike DKIM there is no
      // alignment requirement on an ARC set, so a domain alias seals under the
      // name of the domain it points at rather than under the alias.
      signingDomain = pDomain->GetName();
      signingDomain.MakeLower();
      signingSelector = selector;
      signingPrivateKeyFile = privateKeyFile;

      return true;
   }

   void
   SigningOriginTester::Test()
   {
      std::string wrong;
      auto fail = [&wrong](const std::string &what)
      {
         wrong += (wrong.empty() ? "" : "; ") + what;
      };

      // Who may be signed as the From's domain.
      {
         Message generated(false);
         if (!DKIMSigner::SignsAsAuthorDomain(generated))
            fail("a message the server made is not signed as its From's domain");

         Message submitted(false);
         submitted.SetSubmittedBySignedInUser(true);
         if (!DKIMSigner::SignsAsAuthorDomain(submitted))
            fail("a signed-in submission is not signed as its From's domain");

         Message outside(false);
         outside.SetFromOutside(true);
         if (DKIMSigner::SignsAsAuthorDomain(outside))
            fail("a message from outside is signed as its From's domain");

         // A signed-in submission whose From names another domain hosted here that the
         // account may not send as is marked as mail from outside is
         // (SMTPConnection::RecordOrigin_), and neither it nor a copy of it is signed.
         std::shared_ptr<Message> both(new Message(false));
         both->SetFromOutside(true);
         both->SetSubmittedBySignedInUser(true);
         both->SetState(Message::Delivering);
         if (DKIMSigner::SignsAsAuthorDomain(*both))
            fail("a signed-in submission whose From the account may not send as is signed as its From's domain");

         std::shared_ptr<Message> bothForwarded = PersistentMessage::CreateCopy_(both, 0);
         if (!bothForwarded || DKIMSigner::SignsAsAuthorDomain(*bothForwarded))
            fail("a forward of a signed-in submission whose From the account may not send as is signed as its From's domain");
         if (DKIMSigner::SignsAsAuthorDomain(Message(*both)))
            fail("the copy constructor's copy of a signed-in submission whose From the account may not send as is signed as its From's domain");

         Message submittedRead(false);
         PersistentMessage::ReadOriginColumn(PersistentMessage::OriginColumn(*both), submittedRead);
         if (DKIMSigner::SignsAsAuthorDomain(submittedRead))
            fail("a signed-in submission whose From the account may not send as, read back from its queue row, is signed as its From's domain");
      }

      // The address the signer signs a message as: the From's first address, or the
      // envelope sender when the From names none.
      {
         struct AuthorCase
         {
            const char *header;
            const wchar_t *envelope;
            const char *address;
            const char *domain;
         };

         const AuthorCase cases[] =
         {
            { "From: Boss <ceo@example.test>\r\nTo: a@example.net\r\n\r\n", L"x@example.net", "ceo@example.test", "example.test" },
            { "From: ceo@example.test, other@example.org\r\n\r\n", L"x@example.net", "ceo@example.test", "example.test" },
            { "From: first@example.org\r\nFrom: ceo@example.test\r\n\r\n", L"x@example.net", "first@example.org", "example.org" },
            { "To: a@example.net\r\n\r\n", L"x@example.net", "x@example.net", "example.net" },
            { "From: Nobody at all\r\n\r\n", L"x@example.net", "x@example.net", "example.net" },
            { "To: a@example.net\r\n\r\n", L"", "", "" },
         };

         for (const AuthorCase &c : cases)
         {
            MimeHeader header;
            header.Load(c.header, strlen(c.header), false);

            AnsiString address;
            AnsiString domain;
            DKIMSigner::AuthorOf(header, String(c.envelope), address, domain);
            if (address.CompareNoCase(c.address) != 0 || domain.CompareNoCase(c.domain) != 0)
               fail(std::string("the author of \"") + c.header + "\" is \"" + address.c_str() + "\" at \"" + domain.c_str() +
                  "\", not \"" + c.address + "\" at \"" + c.domain + "\"");
         }
      }

      // Every copy carries the mark, and a copy of a message without it has none.
      for (bool fromOutside : { false, true })
      {
         std::shared_ptr<Message> source(new Message(false));
         source->SetFromAddress(_T("sender@example.net"));
         source->SetState(Message::Delivered);
         source->SetAccountID(17);
         source->SetFromOutside(fromOutside);
         source->SetSubmitterAccountID(0);
         source->SetAuthDomains(_T("dmarc=example.net"));

         const std::string what = fromOutside ? "a message from outside" : "a message not from outside";

         Message constructed(*source);
         if (constructed.GetFromOutside() != fromOutside)
            fail("the copy constructor's copy of " + what + " does not say so");

         std::shared_ptr<Message> forwarded = PersistentMessage::CreateCopy_(source, 0);
         if (!forwarded)
            fail("CreateCopy_ made no copy of " + what);
         else
         {
            if (forwarded->GetFromOutside() != fromOutside)
               fail("the queued copy (a forward, a redirect, a rule's copy) of " + what + " does not say so");
            if (DKIMSigner::SignsAsAuthorDomain(*forwarded) == fromOutside)
               fail("the queued copy of " + what + " is " + (fromOutside ? "" : "not ") + "signed as its From's domain");
         }

         std::shared_ptr<Message> stored = PersistentMessage::CreateCopy_(source, 23);
         if (!stored || stored->GetFromOutside() != fromOutside)
            fail("the recipient's copy of " + what + ", which an account's forward is made from, does not say so");
      }

      // The column: written, and read back as it was written.
      {
         struct Case
         {
            bool outside;
            const wchar_t *domains;
            const wchar_t *column;
         };

         const String longDomains = String(_T("dmarc=")) + String(std::wstring(250, L'd'));
         const Case cases[] =
         {
            { false, L"", L"" },
            { false, L"dmarc=example.net", L"dmarc=example.net" },
            { true, L"", L"outside" },
            { true, L"dmarc=example.net", L"outside,dmarc=example.net" },
         };

         for (const Case &c : cases)
         {
            Message written(false);
            written.SetFromOutside(c.outside);
            written.SetAuthDomains(String(c.domains));

            const String column = PersistentMessage::OriginColumn(written);
            if (column.Compare(c.column) != 0)
               fail("the column for " + std::string(c.outside ? "a message from outside" : "a message") + " with \"" +
                  std::string(Unicode::ToUtf8(String(c.domains)).c_str()) + "\" is \"" + std::string(Unicode::ToUtf8(column).c_str()) + "\"");

            Message read(false);
            PersistentMessage::ReadOriginColumn(column, read);
            if (read.GetFromOutside() != c.outside || read.GetAuthDomains().Compare(c.domains) != 0)
               fail("the column \"" + std::string(Unicode::ToUtf8(column).c_str()) + "\" read back as something else");
         }

         // Too long for the column: the domains go whole, and the mark stays.
         Message tooLong(false);
         tooLong.SetFromOutside(true);
         tooLong.SetAuthDomains(longDomains);
         const String tooLongColumn = PersistentMessage::OriginColumn(tooLong);
         if (tooLongColumn.Compare(_T("outside")) != 0)
            fail("a column too long for its 255 characters is \"" + std::string(Unicode::ToUtf8(tooLongColumn).c_str()) + "\", not the mark alone");

         Message tooLongWithout(false);
         tooLongWithout.SetAuthDomains(String(_T("dmarc=")) + String(std::wstring(260, L'd')));
         if (!PersistentMessage::OriginColumn(tooLongWithout).IsEmpty())
            fail("a column too long for its 255 characters, with no mark, is not left empty as it always was");

         // A row written before the mark existed, and a value of another shape.
         const wchar_t *legacy[] = { L"dmarc=example.net", L"example.test", L"" };
         for (const wchar_t *value : legacy)
         {
            Message read(false);
            PersistentMessage::ReadOriginColumn(String(value), read);
            if (read.GetFromOutside() || read.GetAuthDomains().Compare(value) != 0)
               fail("a column written before the mark, \"" + std::string(Unicode::ToUtf8(String(value)).c_str()) + "\", read as something else");
         }

         Message readBack(false);
         PersistentMessage::ReadOriginColumn(_T("outside,dmarc=example.net"), readBack);
         if (DKIMSigner::SignsAsAuthorDomain(readBack))
            fail("a message read back from outside is signed as its From's domain");
      }

      // A managed list's copies of a posting from outside: not signed as the list's
      // domain when that is the From's organisation, which DMARC would align with it.
      {
         struct ListCase
         {
            bool fromOutside;
            const wchar_t *from;
            bool signs;
         };

         const ListCase cases[] =
         {
            { false, L"ceo@example.test", true },
            { false, L"Boss <ceo@example.test>", true },
            { true, L"Boss <ceo@example.test>", false },
            { true, L"ceo@EXAMPLE.test", false },
            { true, L"ceo@sub.example.test", false },
            { true, L"author@example.net", true },
            { true, L"", true },
         };

         for (const ListCase &c : cases)
         {
            if (ListManager::SignsCopiesAsListDomain(c.fromOutside, String(c.from), _T("list@example.test")) != c.signs)
               fail(std::string("a list copy of a posting ") + (c.fromOutside ? "from outside" : "not from outside") + " from \"" +
                  std::string(Unicode::ToUtf8(String(c.from)).c_str()) + "\" is " + (c.signs ? "not " : "") + "signed as the list's domain");
         }

         // Every From field, and every address in one: any of the list domain's
         // organisation, from outside, and the copies are not signed as that domain.
         struct FieldsCase
         {
            bool fromOutside;
            std::vector<String> fields;
            bool signs;
            const char *what;
         };

         const FieldsCase fieldsCases[] =
         {
            { true, { L"x@evil.example", L"ceo@example.test" }, false, "a second From field of the list's domain under one of another" },
            { true, { L"x@evil.example, ceo@sub.example.test" }, false, "a second address of the list's organisation in one From field" },
            { true, { L"x@evil.example (ceo@example.test)" }, false, "an address of the list's domain in a comment" },
            { true, { L"staff: ceo@EXAMPLE.TEST;" }, false, "an address of the list's domain in a group" },
            { true, { L"Boss <x@evil.example>" }, true, "one address of another domain" },
            { true, { L"x@evil.example", L"y@other.example" }, true, "two From fields, neither of the list's organisation" },
            { true, { L"x@notexample.test" }, true, "a domain whose name ends in the list's without being of it" },
            { true, {}, true, "no From field" },
            { false, { L"x@evil.example", L"ceo@example.test" }, true, "two From fields of a posting not from outside" },
         };

         for (const FieldsCase &c : fieldsCases)
         {
            if (ListManager::SignsCopiesAsListDomain(c.fromOutside, c.fields, _T("list@example.test")) != c.signs)
               fail(std::string("a list copy of a posting with ") + c.what + " is " + (c.signs ? "not " : "") + "signed as the list's domain");
         }
      }

      // A quarantined copy keeps the mark in its name, and its release reads it back
      // from there; a copy held before the name carried it is read from its header.
      {
         const String guid = _T("{0F8E4C2A-6B1D-4E5F-9A3C-7D2B8E1F4A6C}");
         QuarantineStore::StoredMarks outsideMarks;
         outsideMarks.from_outside = true;
         QuarantineStore::StoredMarks insideMarks;
         const String outsideName = QuarantineStore::StoredName(guid, outsideMarks);
         const String insideName = QuarantineStore::StoredName(guid, insideMarks);

         if (outsideName.Find(guid) != 0 || insideName.Find(guid) != 0 || outsideName.Compare(insideName) == 0)
            fail("a quarantined copy's name is \"" + std::string(Unicode::ToUtf8(outsideName).c_str()) + "\" from outside and \"" +
               std::string(Unicode::ToUtf8(insideName).c_str()) + "\" otherwise");
         if (QuarantineStore::OriginOfStoredName(_T("2026-10-07\\") + outsideName) != QuarantineStore::OriginOutside)
            fail("the name of a quarantined copy from outside does not read back as from outside");
         if (QuarantineStore::OriginOfStoredName(_T("2026-10-07/") + insideName) != QuarantineStore::OriginInside)
            fail("the name of a quarantined copy not from outside does not read back as such");
         if (QuarantineStore::OriginOfStoredName(_T("2026-10-06\\") + guid + _T(".eml")) != QuarantineStore::OriginUnknown)
            fail("the name of a copy held before the mark read as if it said where the message came from");

         struct HeaderCase
         {
            const char *header;
            bool outside;
            const char *what;
         };

         const HeaderCase cases[] =
         {
            { "From: a@example.net\r\nSubject: x\r\n\r\n", false, "a message with no Received field, which this server made" },
            { "Received: from client (client [192.0.2.9])\r\n\tby MAILBOX with ESMTPSA id 41\r\n\t; Mon, 5 Oct 2026 09:00:00 +0000\r\nFrom: a@example.net\r\n\r\n",
              false, "this server's Received field for a session that signed in" },
            { "Received: from client (client [192.0.2.9])\r\n\tby MAILBOX with ESMTP id 41\r\n\t; Mon, 5 Oct 2026 09:00:00 +0000\r\nFrom: ceo@example.test\r\n\r\n",
              true, "this server's Received field for a session that did not sign in" },
            { "Received: by MAILBOX (hMailServer send API, key \"app\") with HTTPS; Mon, 5 Oct 2026 09:00:00 +0000\r\nFrom: a@example.test\r\n\r\n",
              false, "the send API's Received field" },
            { "Received: from mx.example.net by mx.example.net with ESMTPS id 7; Mon, 5 Oct 2026 09:00:00 +0000\r\nFrom: ceo@example.test\r\n\r\n",
              true, "a message downloaded from another server's mailbox" },
            { "Received: from x by MAILBOX with ESMTPA y (client [192.0.2.9])\r\n\tby OTHERHOST with ESMTP id 3\r\nFrom: ceo@example.test\r\n\r\n",
              true, "another server's Received field that names this one in its HELO" },
            { "Received: from client (client [192.0.2.9])\r\n\tby MAILBOX with ESMTPS id 41\r\n\t(version=TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 bits=256)\r\n\t; Mon, 5 Oct 2026 09:00:00 +0000\r\nFrom: ceo@example.test\r\n\r\n",
              true, "this server's Received field for a TLS session that did not sign in" },
            { "Received: from x by MAILBOX with ESMTP y (client [192.0.2.9])\r\n\tby MAILBOX with ESMTPSA id 41\r\n\t(version=TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 bits=256)\r\n\t; Mon, 5 Oct 2026 09:00:00 +0000\r\n\r\n",
              false, "this server's Received field for a TLS session that signed in, its HELO naming this server" },
            { "", true, "a header that could not be read" },
         };

         for (const HeaderCase &c : cases)
         {
            if (QuarantineStore::HeaderSaysFromOutside(AnsiString(c.header), _T("MAILBOX")) != c.outside)
               fail(std::string(c.what) + " is " + (c.outside ? "not " : "") + "read as from outside on its release");
         }
      }

      if (!wrong.empty())
         throw std::logic_error("DKIM signing as the From's domain: " + wrong);
   }
}
