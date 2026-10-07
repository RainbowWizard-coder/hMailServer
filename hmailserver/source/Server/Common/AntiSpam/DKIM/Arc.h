// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// ARC (Authenticated Received Chain, RFC 8617): sealing and chain evaluation.
//
// When a message is relayed onward (forwarding, distribution lists), the
// SPF/DKIM evaluation of the next hop typically fails. ARC preserves the
// authentication results observed by this server in a cryptographically
// signed chain so downstream receivers can make informed decisions.
//
// The same chain, read inbound, is what lets THIS server recover the
// authentication results a forwarder observed before it broke SPF/DKIM for
// us - see SpamTestArc. Both directions share the chain validation here.

#pragma once

namespace HM
{
   class Message;

   class Arc
   {
   public:

      // Adds one ARC set (ARC-Seal, ARC-Message-Signature,
      // ARC-Authentication-Results) to the message, signed with the given
      // identity's DKIM key. The identity is the domain that is sealing, which
      // is the local domain handling the relay - it is not required to be, and
      // for relayed mail is not, the author domain of the message. Returns
      // false if no set was added; the caller must still deliver the message.
      bool Seal(std::shared_ptr<Message> message,
                const AnsiString &domain,
                const AnsiString &selector,
                const String &privateKeyFile);

      // The ARC-Authentication-Results value of the set Seal adds: the instance, this
      // server's identity as given (Seal gives its computer name), dkim= as given - Seal gives the verdict of verifying the
      // message as it is stored now (DkimResultOf_) - and spf= from spfResult, the
      // result this server's own check reached when the message arrived
      // (Message::GetReceivedSpfResult), "none" when it did not run or is no RFC 8601
      // SPF keyword.
      //
      // THE DEFECT. spf= was read from the first Received-SPF field of the message,
      // which is whoever's put it there: the sender's own, on mail whose arrival this
      // server did not record one for, and on mail it did, a field a Sieve addheader or
      // a rule's Set header value placed above this server's own before a redirect or a
      // forward. The seal then attested "spf=pass" for a check that never passed, to
      // every receiver that trusts this server's ARC sets. No field of the message is
      // read for it now. ArcAuthenticationResultsTester holds it to examples.
      static AnsiString AuthenticationResultsValue(int instance, const AnsiString &authservId, const AnsiString &dkimResult, const String &spfResult);

      // ---- Inbound chain evaluation (used by SpamTestArc) ----

      enum class ChainParseStatus
      {
         NoChain = 0,     // The message carries no ARC header fields.
         Malformed = 1,   // ARC fields exist but do not form a well-formed chain.
         Parsed = 2
      };

      // What a receiver needs to know about a chain BEFORE spending DNS
      // lookups and signature verifications on it: who sealed it, and what the
      // hop closest to the origin said it observed. Deliberately free of any
      // cryptography, so a caller can refuse an untrusted chain without ever
      // resolving an attacker-chosen domain.
      struct ChainInfo
      {
         int instance_count = 0;

         // The d= of every ARC-Seal plus the d= of the newest
         // ARC-Message-Signature, lower-cased, duplicates included.
         //
         // The newest AMS signer is in this list on purpose: the seals carry
         // the chain's custody, but the newest AMS is the only signature that
         // pins the CURRENT message content (RFC 8617 5.2 validates only the
         // newest AMS), so a trust decision that skipped its d= would honour
         // content vouched for by a domain nobody chose to trust.
         std::vector<AnsiString> sealer_domains;

         // The ARC-Authentication-Results value of instance 1 - the results
         // recorded by the hop closest to the message's origin, which is where
         // the original SPF/DKIM/DMARC outcome survives forwarding.
         AnsiString oldest_authentication_results;
      };

      static ChainParseStatus ParseChain(const AnsiString &header, ChainInfo &info);

      enum class ChainValidationStatus
      {
         Fail = 0,        // Definitive: the chain does not validate.
         TempError = 1,   // A DNS lookup did not complete; nothing was proven either way.
         Pass = 2
      };

      // Full RFC 8617 section 5.2 validation: structural integrity, the cv=
      // ladder (i=1 says cv=none, every later seal says cv=pass), the newest
      // ARC-Message-Signature including its body hash, and EVERY ARC-Seal in
      // the chain. Anything less is not enough to act on: verifying only the
      // newest seal proves the previous hop sealed *something*, not that the
      // recorded results were ever attested by the domains that claim them.
      static ChainValidationStatus ValidateChain(const String &messageFileName, const AnsiString &header);

   private:

      struct ArcSet
      {
         AnsiString authentication_results;
         AnsiString message_signature;
         AnsiString seal;
      };

      // One DNS key lookup outcome, cached so a chain sealed throughout by the
      // same domain costs one lookup, not one per seal.
      struct KeyLookupResult
      {
         ChainValidationStatus status = ChainValidationStatus::Fail;
         AnsiString public_key;
      };

      static bool ParseExistingSets_(const AnsiString &header, std::map<int, ArcSet> &sets);

      static ChainValidationStatus ValidateChainSets_(const std::map<int, ArcSet> &sets,
                                                      const AnsiString &header,
                                                      const String &messageFileName);
      static ChainValidationStatus VerifyNewestMessageSignature_(const std::map<int, ArcSet> &sets,
                                                                 const AnsiString &header,
                                                                 const String &messageFileName,
                                                                 std::map<AnsiString, KeyLookupResult> &keyCache);
      static ChainValidationStatus VerifySealSignature_(const std::map<int, ArcSet> &sets,
                                                        int instance,
                                                        std::map<AnsiString, KeyLookupResult> &keyCache);
      static KeyLookupResult RetrieveArcPublicKey_(const AnsiString &selector,
                                                   const AnsiString &domain,
                                                   std::map<AnsiString, KeyLookupResult> &keyCache);

      static AnsiString StripSealSignatureValue_(const AnsiString &sealValue);

      // pass, fail, temperror or none: what verifying the message's DKIM signatures as
      // it is stored now concludes.
      static AnsiString DkimResultOf_(const String &messageFileName);
   };

   // Arc::AuthenticationResultsValue held to examples: spf= is the result given -
   // none, pass, fail, softfail, temperror, in any case - a value that is no SPF keyword,
   // or holds a line break and a field after it, is none, and dkim= is the verdict given.
   // And the result it is given: carried
   // by every copy (PersistentMessage::CreateCopy_, Message's copy constructor) and kept
   // on the queue row beside the outside mark and the DMARC domain, read back as it was
   // written, and absent from a row written before. No configuration, database, file or
   // network. ClassTester runs
   // it on Windows, business-object-tests on Linux. Throws std::logic_error naming every
   // case that went wrong.
   class ArcAuthenticationResultsTester
   {
   public:
      void Test();
   };
}
