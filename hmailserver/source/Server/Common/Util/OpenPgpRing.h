// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The keys the webmail keeps for one account (hm_pgpring, schema 6080), and the
// one thing the page cannot do for itself: fetch a correspondent's key from
// their own domain.
//
// TWO KINDS OF ROW, AND ONE OF THEM HOLDS A SECRET THIS SERVER CANNOT READ.
//
//   KindOwn is a key the account made in the browser or imported there. The
//   public half is stored as base64 of the transferable public key; the secret
//   half is stored as the ARMOURED OpenPGP secret key, encrypted the OpenPGP
//   way - iterated-and-salted string-to-key, AES-256 in CFB - under a
//   passphrase the page asks for and never sends. So this server holds a secret
//   key it cannot open, and the same bytes, saved to a file, import into GnuPG.
//   That is the whole design: no format of this project's own stands between a
//   person and their own key.
//
//   KindCorrespondent is somebody else's public key, with where it came from
//   (imported, autocrypt, wkd, keyserver), when it was first seen and when it
//   was last seen. Nothing secret, and nothing this server acts on: the page
//   decides what to encrypt to.
//
//   A correspondent's row also carries what this server can say about TRUST,
//   which is not much and is therefore said exactly (schema 6081). It
//   remembers whether the account marked the key "verified in person" and
//   when (SetVerified); and, when it was filed under an address that already
//   held a DIFFERENT key, which key that was and when this one arrived
//   (changed_from, changed_at) and whether the person has accepted the change
//   (accepted). Save records a change; nothing here acts on one. The page
//   does: a key that replaced another and was not accepted is not encrypted
//   to, and a signature it made is shown with the warning that names both
//   fingerprints, because the one thing a man in the middle needs is for a
//   key to change in silence. Marking a key verified accepts the change it
//   carries, since a fingerprint read back to its owner is the stronger of
//   the two acts.
//
// This server never signs, verifies, encrypts or decrypts an OpenPGP message.
// It parses a key far enough to be sure of what it is (OpenPgpKey::Parse, the
// same reader the Web Key Directory publishes through) and to fill the columns
// a listing shows, and it stores what it was given. Everything else happens in
// the browser, in PortalOpenPgp.js.
//
// THE LOOKUP. Lookup() is the server-side half of finding a correspondent's
// key, because a browser cannot make a cross-origin request to somebody else's
// domain and read the answer. It is draft-koch-openpgp-webkey-service in the
// client direction: the advanced method under openpgpkey.<domain> first, then
// the direct method on the domain itself, and - only when the administrator has
// turned it on - the keyserver's HKP /vks/v1/by-email/. Every fetch goes
// through HttpsClient::RequestPublic, so a domain whose name resolves to an
// address inside this network is refused before a connection is made, the
// answer is capped, and the whole thing has a deadline a REST worker can wait
// on. What comes back is parsed before it is answered, so the page is never
// handed something that is not a key.
#pragma once

#include <vector>

namespace HM
{
   class Account;

   class OpenPgpRing
   {
   public:
      enum Kind
      {
         KindOwn = 1,
         KindCorrespondent = 2
      };

      // What one account may hold. A person has one key, or two while they
      // rotate; twenty is room for every mistake. The correspondents' side is
      // fed by Autocrypt on every message opened, so it is the one that grows.
      static const int OwnPerAccount = 20;
      static const int CorrespondentsPerAccount = 500;

      // Base64 characters of the public key, and characters of the armoured
      // secret key. A key with a few hundred certifications on it is tens of
      // kilobytes; these are generous and bounded.
      static const int PublicMaximum = 256 * 1024;
      static const int SecretMaximum = 512 * 1024;

      // And what the whole ring may weigh, both kinds together: the listing
      // route answers every row whole in one document, so the row counts alone
      // would let one account make that document tens of megabytes.
      static const __int64 BytesPerAccount = 8 * 1024 * 1024;

      struct Row
      {
         Row() : id(0), kind(0), created(0), expires(0), revoked(false), prefer(0), first_seen(0), last_seen(0),
                 verified(false), verified_at(0), changed_at(0), accepted(false) {}

         __int64 id;
         int kind;
         String address;
         String name;
         AnsiString fingerprint;    // lower-case hex, 40 characters for a version 4 key
         AnsiString key_id;         // the last 16 of the fingerprint
         String algorithm;          // "Ed25519", "RSA 3072", as the page names it
         AnsiString public_key;     // base64 of the transferable public key
         AnsiString secret_key;     // the armoured secret key, passphrase-encrypted; empty unless KindOwn
         __int64 created;           // from the key packet
         __int64 expires;           // 0 when the key does not expire
         bool revoked;
         String source;             // generated, imported, autocrypt, wkd, keyserver
         int prefer;                // Autocrypt prefer-encrypt: 0 nopreference, 1 mutual
         __int64 first_seen;
         __int64 last_seen;
         bool verified;             // marked "verified in person" by the account; never on a KindOwn row
         __int64 verified_at;       // when; 0 unless verified
         AnsiString changed_from;   // the fingerprint this key replaced for its address; empty when it replaced none
         __int64 changed_at;        // when it arrived; 0 unless changed_from is set
         bool accepted;             // whether the person accepted that change
      };

      static bool List(__int64 accountId, int kind, std::vector<Row> &rows);
      static bool Find(__int64 accountId, int kind, const AnsiString &fingerprint, Row &row);
      // By the row's id, and only among the rows of that account: a row id is
      // what the trust routes name, and one account's number must not reach
      // another's key.
      static bool FindById(__int64 accountId, __int64 id, Row &row);
      static int Count(__int64 accountId, int kind);

      // Adds the row, or replaces the one with the same fingerprint. replaced
      // says which happened, so the route can answer 201 or 200. first_seen,
      // the address and the trust columns are kept from the row that was
      // there - a key comes back to where it was filed, whatever address the
      // request named - and last_seen is set to now. A correspondent's row
      // filed under an address that already holds a DIFFERENT key, the
      // account's own included, is recorded as a change of that address's key
      // - changed_from names the key it replaced, changed_at when, accepted
      // false - and a key seen again is not, whatever brought it back.
      static bool Save(__int64 accountId, Row &row, bool &replaced);
      static bool Remove(__int64 accountId, int kind, const AnsiString &fingerprint);

      // The account's own "verified in person" mark on one of its
      // correspondents' keys. Verifying a key that replaced another accepts
      // the change as well; unmarking leaves the acceptance as it was.
      static bool SetVerified(__int64 accountId, __int64 id, bool verified);
      // Whether the person accepted that this key replaced the one before it
      // for the same address.
      static bool SetAccepted(__int64 accountId, __int64 id, bool accepted);

      // The row as the page reads it. A KindOwn row carries its secret key,
      // because the page is the only thing that can open it; a correspondent's
      // never does, because there is none. Every row carries verified and
      // verified_at; a row that replaced another carries changed_from,
      // changed_at and accepted, and one that replaced none carries none of
      // the three. secretKeyIncluded false leaves a KindOwn row's secret key
      // out and says so with secret_key_withheld: the answer to a request the
      // REST API does not give it to (RestApiServer::WithholdsWrappedKeys_).
      static AnsiString RowJson(const Row &row, bool secretKeyIncluded = true);

      // Fills fingerprint, key_id, algorithm, created, expires, address and
      // name from the key itself, so that a listing cannot say something the
      // key does not. False with a sentence in problem when the bytes are not a
      // transferable public key.
      static bool DescribeFrom(const AnsiString &publicKeyBase64, Row &row, AnsiString &problem);

      struct Found
      {
         Found() : status(0) {}

         bool Ok() const { return !key_base64.IsEmpty(); }

         AnsiString key_base64;
         String source;    // wkd-advanced, wkd-direct, keyserver
         String url;       // where it was found, for the page to show
         String error;     // why nothing was found, when nothing was
         int status;       // the last HTTP status seen, 0 when no answer came
      };

      // A correspondent's key, from their domain and then - if the
      // administrator turned it on - from the keyserver. Never throws; a
      // failure is an empty key_base64 with a sentence in error.
      static Found Lookup(const String &address);

      static bool IsFingerprint(const AnsiString &text);

      // imported, autocrypt, wkd, keyserver, generated - and nothing else, so a
      // listing's source column can be shown without being escaped into a word
      // nobody wrote.
      static bool IsSource(const String &text);
   };
}
