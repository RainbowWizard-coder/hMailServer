// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Account;
   class Message;
   class MessageData;
   class MessageRecipient;
   class MimeHeader;

   class SMTPForwarding
   {
   public:

      bool PerformForwarding(std::shared_ptr<const Account> pRecipientAccount, std::shared_ptr<Message> pOriginalMessage);

      // Queues a copy of the message for delivery to an arbitrary address (used by
      // the Sieve "redirect" action). Applies the same loop-count guard and
      // SRS/envelope-from handling as account forwarding. Returns true if a copy
      // was queued.
      bool RedirectToAddress(std::shared_ptr<const Account> pRecipientAccount, std::shared_ptr<Message> pOriginalMessage, const String &targetAddress);

      // ---- The envelope a copy is sent on with ----------------------------------
      //
      // What a receiver's SPF check reads of a message this server sends on - the
      // domain of its envelope sender, or for a null one the name this server gives
      // in EHLO (RFC 7208 2.4) - is one DMARC aligns with the From when it is of the
      // From's organisation (RFC 7489 3.1). DKIMSigner::SignsAsAuthorDomain decides
      // which messages are signed as the From's domain; these decide which identity
      // a copy of mail from outside (Message::GetFromOutside) is given: one of the
      // organisation of a domain its From names only when the message passed DMARC
      // for that organisation when it arrived (Message::GetAuthDomainsOnArrival: the
      // record of the message that arrived, or the one a copy carries of it). Otherwise a
      // forward, a Sieve redirect or a rule's forward keeps the envelope sender the
      // message came with; a copy that would still leave with such an identity of
      // this server's own - a null envelope sender under its EHLO name, or the one
      // the message came with at that name or at a domain hosted here - goes to no
      // address outside; a managed list does not distribute the posting
      // (ListManager); a list's bounce address is not given to it
      // (DistributionListSender); an alias or a list does not send it on to an
      // address outside with such an identity (SMTPDeliverer); and a download does
      // not take an address hosted here from its From as its envelope sender
      // (ExternalFetchClientBase).

      // The values of a header's From fields: the first one decoded, as
      // MessageData::GetFrom reads it, and each one as it is written.
      static std::vector<String> FromFieldValues(MimeHeader &header);

      // Whether any of those values names a domain of identityDomain's organisation
      // (the public suffix list's answer, as DMARC's relaxed alignment reads it): the
      // address DMARC here reads in a value, or any domain written after an @
      // anywhere in it - a second field, a second address in one, one in a comment
      // or a group. A receiver may take any of them for the author. False for an
      // empty identityDomain.
      static bool FromNamesOrganisationOf(const std::vector<String> &fromFieldValues, const String &identityDomain);

      // Whether authDomains (Message::GetAuthDomainsOnArrival: whole tokens,
      // "dmarc=name") records a DMARC pass on arrival for a domain of identityDomain's
      // organisation.
      static bool DmarcPassedInOrganisationOf(const String &authDomains, const String &identityDomain);

      // THE RULE. Whether a message may be sent on with an SPF identity at
      // identityDomain: always, but not a message from outside whose From names a
      // domain of identityDomain's organisation and that did not pass DMARC for
      // that organisation when it arrived.
      static bool MaySendOnWithIdentity(bool fromOutside, const String &authDomains, const std::vector<String> &fromFieldValues, const String &identityDomain);

      // Whether the domain of an envelope sender a copy keeps from the message it came
      // with is an identity of this server's own: the name it gives in EHLO (heloName),
      // which is what SPF reads of a null sender (RFC 7208 2.4), or a domain whose
      // organisation is hosted here (senderOrganisationHosted, IsHostedOrganisation -
      // a hosted domain or a subdomain of one, both of which this server's DNS owner
      // publishes the SPF for). A receiver's SPF check of such an envelope reads a
      // record that names this server, as it does of a rewritten one; of any other,
      // the record of the sender's own domain.
      static bool IsOwnEnvelopeDomain(const String &senderDomain, const String &heloName, bool senderOrganisationHosted);

      // Whether domain is a domain hosted here, or a domain alias of one.
      static bool IsHostedDomain(const String &domain);

      // Whether domain, or its organisational domain (the public suffix list's), is
      // hosted here: a hosted domain, a domain alias of one, or a subdomain of a hosted
      // domain - every name this server's DNS owner publishes SPF for, so an envelope
      // at any of them makes a receiver's SPF check read a record that names this
      // server.
      static bool IsHostedOrganisation(const String &domain);

      // The envelope sender a copy of message is given, in envelope: rewritten - an SRS
      // address, or the forwarding account's own, as the settings ask, or empty for
      // none - when the message may be sent on with an identity at its domain, else
      // the envelope sender the message came with, and rewriteWithheld says that a
      // rewrite was not given. False when the message may not be sent on with the one
      // it came with: the null sender under the name this server gives in EHLO
      // (heloName), or one at that name or at a domain whose organisation is hosted
      // here (senderOrganisationHosted; IsOwnEnvelopeDomain). The copy then goes to no
      // address outside (WithholdOutsideRecipients), and envelope is the one it came with.
      static bool EnvelopeOfCopy(bool fromOutside, const String &authDomains, const String &envelopeSender, bool senderOrganisationHosted,
                                 const std::vector<String> &fromFieldValues, const String &rewritten, const String &heloName,
                                 String &envelope, bool &rewriteWithheld);

      // Takes the recipients outside off a copy its caller made and has not saved, for
      // one EnvelopeOfCopy answered false for, and names each in the application log
      // as not sent to: the copy of messageId that what says it is ("forward by
      // name@domain"), with envelope, the envelope sender it would have been sent with
      // (empty for the null sender). True when it took one; the copy then goes to the
      // addresses here alone, or is not made when there are none.
      static bool WithholdOutsideRecipients(std::shared_ptr<Message> copy, __int64 messageId, const String &what, const String &envelope);

      // Whether a recipient of a message is one this server reached by expanding an
      // address of its own - an alias, a distribution list - to an address outside:
      // not a local name, and accepted at an address hosted here that is not its own.
      // Not the mirror address, whose copy records the addresses the message was
      // delivered to in that place (MirrorMessage).
      static bool IsOutwardExpansion(const MessageRecipient &recipient, const String &mirrorAddress);

      // Takes off a message from outside the recipients it reached by such an
      // expansion, when it may not be sent on with the identity it would leave with
      // (MaySendOnWithIdentity): the name this server gives in EHLO for a null
      // envelope sender, or the domain of one at that name or at a domain hosted here
      // (IsOwnEnvelopeDomain) - a forward in all but name, as such a forward is not
      // made. The rows are deleted and the application log names each recipient; the
      // message goes to the others. Called by SMTPDeliverer before external delivery,
      // where the suppression list is applied. Costs a look at the recipients for any
      // other message.
      static void WithholdOutwardExpansions(std::shared_ptr<Message> message);

   private:

      // The envelope sender the settings give a forward or a redirect of a message
      // whose envelope sender is envelopeFrom: an SRS address at the forwarding
      // account's domain when SRS is on and the sender's domain is not hosted here,
      // else the account's own address when RewriteEnvelopeFromWhenForwarding is on,
      // else none - and none for a null sender, or when SRS cannot rewrite it.
      static String RewrittenEnvelope_(std::shared_ptr<const Account> account, const String &envelopeFrom);

      // EnvelopeOfCopy for a forward or a redirect of message by account, the From
      // fields read from messageData, as it was loaded for the loop count.
      static bool ForwardEnvelope_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                   std::shared_ptr<MessageData> messageData, String &envelope);

      // WithholdOutsideRecipients for the copy what ("forward", "redirect") of message
      // by account, sent with envelope.
      static bool WithholdOutsideRecipients_(std::shared_ptr<Message> copy, std::shared_ptr<const Message> message,
                                             std::shared_ptr<const Account> account, const String &what, const String &envelope);
   };

   // Which envelope a copy of a message is given, held to examples: the
   // organisations a From names, read the way ListManager reads them before it
   // signs a list's copy - every field, every address and every @ in a field -
   // through the public suffix list; the DMARC passes a message's record holds;
   // which envelope senders are this server's own identities; and the choice for
   // mail from inside and from outside, with and without a pass, with a rewrite and
   // without, for the envelope it came with at this server's EHLO name, at a domain
   // hosted here and at another, and for the null sender under a name of the
   // From's organisation and of another; and what DMARC passed for when the mail
   // arrived, which every copy carries apart from the record of the message that
   // arrived (Message::GetCopiedAuthDomains): a copy's and a copy of a copy's
   // (PersistentMessage::CreateCopy_), on a queue row (OriginColumn) and in the
   // quarantine's stored name (QuarantineStore::StoredMarks). No configuration,
   // database or network. ClassTester runs it on Windows, business-object-tests on
   // Linux. Throws std::logic_error naming the first case that went wrong.
   class CopyEnvelopeTester
   {
   public:
      void Test();
   };
}
