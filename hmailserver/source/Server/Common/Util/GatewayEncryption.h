// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Gateway encryption to a partner domain: a message to a domain whose remote
// domain policy says so is encrypted HERE, to the partner's own key, whatever
// the sender's mail client did - the enterprise shape of end-to-end mail, and
// what Zix, Cisco, Proofpoint and SEPPmail gateways do between organisations.
// docs/RemoteDomains.md, "Gateway encryption", is the administrator's account
// of it; this is the mechanism.
//
// WHAT IS WRITTEN
//
// The partner's copy of the message - the batch of recipients at that domain,
// never the whole message: a recipient elsewhere in the same submission gets
// it as it was (ExternalDelivery groups the recipients so that a batch is one
// policy's or none's). Its header is the original's with the fields that
// described the old body taken off - every Content-* field and MIME-Version -
// and with the signatures over the old body taken off too: DKIM-Signature and
// the three ARC fields, which would fail against the new body and which the
// caller replaces by signing the copy afresh (DKIMSigner). Everything else is
// kept, From, To, Cc, Date, Message-ID and Subject among them. THE SUBJECT IS
// NOT HIDDEN: it is a header, and a gateway that has to route and file the
// message reads it. Then:
//
//   * OpenPGP: PGP/MIME, RFC 3156 - multipart/encrypted with the version part
//     and the armoured message, written by OpenPgpWriter::Encrypt (the one
//     OpenPGP writer this server has, the zero-access mailbox's) to the
//     encryption subkey OpenPgpWriter::ChooseEncryptionKey picks - a subkey
//     whose binding signature verifies against the primary, so that a subkey
//     somebody appended to the partner's key on its way here is never the one
//     encrypted to. The subkey's fingerprint is shown beside the primary's
//     everywhere the key is named (KeyName).
//   * S/MIME: application/pkcs7-mime; smime-type=enveloped-data, RFC 8551 -
//     OpenSSL's CMS, AES-256-CBC for the content, and the content key to the
//     certificate by RSAES-OAEP (RFC 3560, its default SHA-1 parameters, which
//     every S/MIME client reads) for an RSA certificate, or by ECDH key
//     agreement with the X9.63 KDF over SHA-256 (RFC 5753) for an
//     elliptic-curve one. RSA under 2048 bits, and a curve under 256, are
//     refused, OpenPGP's RSA subkeys included.
//
// What goes under the key is the WHOLE original message, headers and body,
// exactly as it would have been sent - so the partner's gateway, or the
// person's client, opens it to the message that was written, this server's
// own DKIM signature on it included.
//
// WHAT IS NOT DONE, ON PURPOSE
//
// No signing at the gateway: a signature made here would claim for the person
// what this server cannot know about them. No key per recipient address: one
// key per partner domain, which is how gateways publish theirs. No key
// discovery (WKD, LDAP, a certificate directory): the administrator pastes the
// key, and the save judges it. No decryption of anything that comes back: this
// server holds no gateway private key, so a partner's reply is opened by the
// person's own client or not at all - and a paste that carries private-key
// material anywhere in it, of either kind, is refused whole (ContainsPrivateKey).
//
// A message that is already encrypted end to end goes as it is: it is under a
// key already, and a second layer the partner's gateway takes off would hand
// the person the first one unchanged. "Already encrypted" is decided from
// what the body holds, never from the label alone: a CMS object whose content
// type is enveloped-data or authenticated-enveloped-data at the top level, or
// a PGP/MIME message whose encrypted part is an OpenPGP message that begins
// with a session key packet. A SIGNED message whose content is in the clear -
// multipart/signed, or opaque S/MIME signed-data however it is labelled - is
// encrypted, signature and all, because the signature is inside the content.
//
// WHAT IS HELD IN MEMORY
//
// The message is read whole, as DKIM signing reads it, up to
// MaximumMessageBytes. SealFile lets go of each form as soon as the next one
// is made - the original once it is encrypted, the binary ciphertext once it
// is armoured - and writes the copy's parts to the file one after the other
// rather than joining them, so a message of N bytes peaks at about three
// times N (the original and the ciphertext, then the ciphertext and its
// armour) rather than the five it was.
#pragma once

#include "../BO/RemoteDomainPolicy.h"

namespace HM
{
   class GatewayEncryption
   {
   public:
      // The longest key text a policy holds, in characters. An armoured OpenPGP
      // key with a few certifications is a few kilobytes and a certificate with
      // its chain less; the column is a MySQL TEXT, 65,535 bytes, and a paste
      // longer than this is a keyring or a certificate bundle, not a gateway key.
      static const int MaximumKeyCharacters = 60000;

      // The largest message encrypted at the gateway, in bytes: the message is
      // read and encrypted in memory, as DKIM signing reads it, and the same
      // 50 MB ceiling applies. A larger message cannot be encrypted and follows
      // the policy's fallback. The copy is about 1.37 times the original (the
      // armour, or the base64 of the CMS object), so a copy of an original over
      // about 36.5 MB is over DKIM's own 50 MB ceiling and goes unsigned; the
      // delivery log says so for that message.
      static const int MaximumMessageBytes = 52428800;

      // The smallest RSA modulus encrypted to, in bits, S/MIME and OpenPGP
      // alike, and the smallest elliptic curve.
      static const int MinimumRsaBits = 2048;
      static const int MinimumEcBits = 256;

      // What a pasted key is, for the listing, the Control Panel and the save.
      struct KeyFacts
      {
         KeyFacts() : usable(false), expires(0) {}

         // Whether a message can be encrypted to it now.
         bool usable;

         // Why not, a sentence; empty when usable.
         AnsiString problem;

         // Accepted, and something to know: the key names no address in the
         // partner domain (a gateway's key often names the gateway), or a
         // certificate that does not say it is for e-mail.
         AnsiString warning;

         // Lower-case hex. OpenPGP: the primary key's fingerprint, the number a
         // partner publishes. S/MIME: the SHA-256 of the first certificate.
         AnsiString fingerprint;

         // OpenPGP only: the fingerprint of the subkey a message is encrypted
         // to, when it is a subkey and not the primary itself; empty otherwise.
         // This is the one the partner confirms as well: gpg --fingerprint
         // --fingerprint prints both.
         AnsiString subkey_fingerprint;

         // Seconds since the epoch; 0 when it does not expire. OpenPGP: the
         // earlier of the primary key's and the encryption subkey's.
         __int64 expires;

         // OpenPGP: the primary user id. S/MIME: the first certificate's subject.
         AnsiString subject;
      };

      // Judges key text as the given method's. domainPattern is the policy's
      // domain name, for the warning only. now is the clock expiry is judged at.
      // Refuses private-key material and an over-long paste before anything
      // else, whatever the method.
      static KeyFacts Inspect(RemoteEncryption method, const String &key, const String &domainPattern, __int64 now);

      // Whether key text carries private-key material of either kind anywhere
      // in it: a PEM PRIVATE KEY block of any sort (PKCS#8, encrypted PKCS#8,
      // RSA, EC, OpenSSH) or an OpenPGP PRIVATE or SECRET KEY BLOCK - beside a
      // public key or a certificate as much as on its own.
      static bool ContainsPrivateKey(const AnsiString &text);

      // What a save refuses in key text whatever the method, before it is
      // judged as a key: private-key material, and a paste over
      // MaximumKeyCharacters. text is trimmed; empty text (no key) passes.
      // False with a sentence in problem.
      static bool AcceptableKeyText(const AnsiString &text, AnsiString &problem);

      // One sentence on the key a policy holds, for a reader who cannot parse
      // it: the method, the fingerprints, whom it names, when it expires, and
      // whether a message can be encrypted to it now, with any warning. What
      // COM answers as EncryptKeySummary and the Control Panel shows. UTF-8.
      static AnsiString Summary(RemoteEncryption method, const String &key, const String &domainPattern, __int64 now);

      // The key as the log, the trace and the copy's mark name it: the
      // fingerprint, and for OpenPGP "subkey" and the subkey's after it.
      static AnsiString KeyName(const KeyFacts &facts);

      // Which method a key held under a policy that encrypts nothing looks like:
      // PEM certificates are S/MIME, anything else is read as OpenPGP. So that a
      // key kept while encryption is switched off is still described.
      static RemoteEncryption MethodOfKey(const String &key);

      // Whether a message on disk is already encrypted end to end, judged from
      // its body as the header comment says: CMS enveloped-data or
      // authenticated-enveloped-data at the top level, or PGP/MIME whose
      // encrypted part begins with a session key packet.
      static bool IsEncryptedEndToEnd(const AnsiString &message);

      enum Outcome
      {
         // The copy is written.
         Sealed,

         // The message is encrypted end to end already and goes as it is.
         AlreadyEncrypted,

         // It cannot be encrypted, and will not be on the next attempt either:
         // the key (no key, expired, of a kind not written to) or the size.
         // problem says why; the policy's fallback decides.
         Failed,

         // It could not be encrypted THIS time, for a reason of this server's
         // own: the message file could not be read, the copy could not be
         // written, no random bytes, no memory, the cryptography library
         // failed. problem says why; the delivery is retried, whatever the
         // fallback says.
         Deferred
      };

      // The whole original message, CRLF lines, as the partner's copy. facts
      // is what the key was judged to be (its fingerprints name it).
      static Outcome Seal(RemoteEncryption method, const String &key, const AnsiString &original, __int64 now,
                          AnsiString &sealed, KeyFacts &facts, AnsiString &problem);

      // Seal, from one file to another: the message file is read, never written.
      static Outcome SealFile(RemoteEncryption method, const String &key, const String &messageFile, const String &copyFile,
                              __int64 now, KeyFacts &facts, AnsiString &problem);

      // The copy's header without its body: the original's fields less the ones
      // listed above, as CRLF lines. Seal adds the server's mark and the copy's
      // own Content-* fields after them.
      static AnsiString OuterHeaders(const AnsiString &original);

      // "OpenPGP", "S/MIME", or "no" for a policy that encrypts nothing.
      static const char *MethodName(RemoteEncryption method);

      // The word the REST API uses: none, openpgp, smime.
      static const char *MethodWord(RemoteEncryption method);

      // An expiry as the REST API and COM answer it: "2027-01-31T12:00:00Z", or
      // empty for 0.
      static AnsiString ExpiryText(__int64 expires);

      // The field stamped on the partner's copy, naming the method and the key,
      // so that the gateway's administrator - and a test - can see what was done.
      static const char *MarkField() { return "X-hMailServer-Gateway-Encrypted"; }
   };

   // Run from ClassTester::DoTests: the header filtering, the end-to-end
   // recognition, the refusals, and a message sealed to each kind of key.
   class GatewayEncryptionTester
   {
   public:
      void Test();
   };
}
