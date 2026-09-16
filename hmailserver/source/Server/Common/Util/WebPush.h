// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Web Push: the cryptography, and nothing else.
//
// Three specifications meet here and each one is small on its own.
//
//   RFC 8292 (VAPID)  says who is sending. The server holds one ECDSA P-256 key
//                     pair for its whole lifetime; every request to a push
//                     service carries a JWT signed with the private half and the
//                     public half beside it, so the push service can tell one
//                     sender from another and the subscription a browser made
//                     for this server cannot be used by anybody else.
//
//   RFC 8291          says what the message is encrypted to. The browser gives
//                     the server two secrets when it subscribes - a P-256 public
//                     key (p256dh) and sixteen random bytes (auth) - and the
//                     content encryption key is derived from an ECDH between a
//                     fresh key pair and that public key, combined with the auth
//                     secret through HKDF. The push service carries the bytes
//                     and cannot read them.
//
//   RFC 8188          says how the encrypted body is framed: a header of salt,
//                     record size, key id length and key id, then one AES-128-GCM
//                     record. RFC 8291 requires exactly one record, so there is
//                     no record loop here and there never needs to be one.
//
// WHY THE PAYLOAD IS ENCRYPTED RATHER THAN LEFT OUT
//
// A payload-free push - one that wakes the service worker so that the page can
// fetch for itself - is the smaller thing to write and was seriously considered.
// It was rejected because it is worse to run. The worker would have to reach the
// API on the session cookie, which a browser may not have, may have expired, and
// may refuse to send from a worker under a cross-site cookie policy; a push that
// cannot fetch has nothing to say, and a notification that says nothing trains
// people to ignore notifications. It also makes the person's own choice
// meaningless: "tell me who it is from but not what it says" cannot be honoured
// by a fetch, only by a server that decides what to put in the payload. The
// encryption is about two hundred lines, every primitive is in the OpenSSL this
// server already links, and it is pinned in WebPushTester against the worked
// example RFC 8291 publishes - so it is proved against the specification rather
// than against another copy of itself.
//
// Nothing in this class reads a setting, a database or a socket: it takes keys
// and bytes and returns bytes. PushSubscriptions owns the rows and PushNotifier
// owns the sending.

#pragma once

#include <vector>

namespace HM
{
   class WebPush
   {
   public:

      // The two halves of a VAPID key pair, base64url with no padding: the public
      // key as the 65-octet uncompressed point (X9.62, a 0x04 lead octet and the
      // two coordinates), the private key as the 32-octet scalar. Those are the
      // forms every push service and every browser uses, and the forms stored.
      static bool GenerateKeyPair(AnsiString &publicKey, AnsiString &privateKey, String &error);

      // The public key that belongs to a private key. An administrator moving a
      // server carries both halves; this is how a pair whose halves do not match
      // is refused when it is stored rather than by a push service months later.
      static bool PublicKeyOf(const AnsiString &privateKey, AnsiString &publicKey, String &error);

      // "https://push.example.net" for "https://push.example.net/push/abc" - the
      // aud claim of the JWT, which RFC 8292 defines as the origin of the endpoint.
      static bool AudienceOf(const AnsiString &endpoint, AnsiString &audience);

      // The value of the Authorization header: "vapid t=<jwt>, k=<public key>".
      // subject is the contact the push service may use when something is wrong -
      // a mailto: or https: URI - and expiry is an absolute Unix time, which RFC
      // 8292 bounds at 24 hours from now.
      static bool BuildVapidHeader(const AnsiString &endpoint, const AnsiString &subject, __int64 expiry,
                                   const AnsiString &publicKey, const AnsiString &privateKey,
                                   AnsiString &header, String &error);

      // The JWT alone. Public because a test reads the three parts back and
      // verifies the signature with the public key, which is the only way to know
      // the header is one a push service would accept.
      static bool SignJwt(const AnsiString &payload, const AnsiString &privateKey, AnsiString &jwt, String &error);

      // One push message body, RFC 8291 over RFC 8188: a fresh key pair and a
      // fresh 16-octet salt every time. receiverPublicKey and authSecret are the
      // subscription's p256dh and auth, base64url as the browser gave them.
      static bool Encrypt(const AnsiString &plainText, const AnsiString &receiverPublicKey,
                          const AnsiString &authSecret, AnsiString &body, String &error);

      // Encrypt with the sender key pair and the salt given rather than made.
      // The whole point of this entry is that it is deterministic: it is what
      // reproduces the worked example of RFC 8291 section 5, octet for octet.
      static bool EncryptWith(const AnsiString &plainText, const AnsiString &receiverPublicKey,
                              const AnsiString &authSecret, const AnsiString &senderPrivateKey,
                              const AnsiString &salt, AnsiString &body, String &error);

      // The other direction, for a receiver holding the subscription's private
      // key. The server never does this - it has no subscription private key -
      // but the regression suite's fake push service does, which is how a test
      // asserts what a push actually carried rather than that a push was sent.
      static bool Decrypt(const AnsiString &body, const AnsiString &receiverPrivateKey,
                          const AnsiString &authSecret, AnsiString &plainText, String &error);

      // The largest plaintext one record may hold at the record size below. A
      // caller that would exceed it must shorten what it says, not split it:
      // RFC 8291 allows exactly one record.
      static size_t MaximumPlainTextLength();

      static AnsiString Base64UrlEncode(const unsigned char *data, size_t length);
      static AnsiString Base64UrlEncode(const std::vector<unsigned char> &data);
      static bool Base64UrlDecode(const AnsiString &text, std::vector<unsigned char> &output);

      // The one JSON escape this corner needs: a subject, a sender's name or a
      // message subject goes into the JWT claims and into the payload.
      static AnsiString JsonEscape(const AnsiString &value);
   };

   // Run from ClassTester::DoTests, and so from the regression suite through
   // Utilities.RunTestSuite. Pins the encryption against the worked example of
   // RFC 8291 section 5 and appendix A - the salt, both key pairs, the auth
   // secret and the plaintext are the RFC's, and the body produced must be the
   // body the RFC prints. Also checks that a VAPID header verifies under the
   // public key it names, and that a body encrypted to a generated key pair
   // decrypts back to what went in.
   class WebPushTester
   {
   public:
      void Test();
   };
}
