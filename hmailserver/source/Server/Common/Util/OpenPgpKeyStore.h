// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The OpenPGP public keys this server holds for its accounts (hm_pgpkeys,
// schema 6050), and the one lookup a Web Key Directory request makes.
//
// ONE PLACE, FOUR CALLERS. COM (Utilities.ImportOpenPGPKey and the rest), the
// administrator's REST routes, the account's own /api/v1/me/openpgp and hmctl
// all store a key through Store() and read one through List(). None of them
// parses, judges or counts for itself, so the refusals are the same sentence
// wherever an upload arrives and a rule can only be changed in one place.
//
// WHAT IS STORED. The transferable public key as base64 of its bytes, in an
// ntext column, which is what the S/MIME key store next door does with a
// certificate - the backends this server runs on take a long text column
// everywhere and a binary parameter nowhere in the same way. Beside it: the
// fingerprint, the key version, the algorithm and its size, the primary user
// id, when the key was made and when it expires, how many bytes it is, and who
// uploaded it and when. Never a secret key: OpenPgpKey::Parse refuses one, and
// Store() has no other way in.
//
// MORE THAN ONE KEY AN ACCOUNT, deliberately. A person rotating a key needs the
// old one and the new one to be findable at the same time, or every
// correspondent who fetched the old one is writing to a key its owner is about
// to stop using. OpenPGPKeysPerAccount (8) bounds it, the answer concatenates
// them in the order they were uploaded, and a client that fetches a directory
// answer imports all of them - which is what the rotation needs.
//
// THE LOOKUP. Lookup() is the whole of the Web Key Directory's read path: the
// domain's id, the hashed local part, and the three switches, in two queries
// whose only parameters are integers. It cannot invert the hash, so it asks the
// database for the accounts of that domain which are published AND have a key -
// usually none or a handful - and hashes each of their local parts. There is no
// stored hash to go stale when an account is renamed.
#pragma once

#include <memory>
#include <vector>

namespace HM
{
   class Account;
   class Domain;

   class OpenPgpKeyStore
   {
   public:
      struct Row
      {
         Row() : id(0), account_id(0), version(0), algorithm(0), bits(0), created(0), expires(0), size(0), uploaded(0) {}

         __int64 id;
         __int64 account_id;
         AnsiString fingerprint;     // lower-case hex, 40 or 64 characters
         int version;
         int algorithm;
         int bits;
         String user_id;             // the key's primary user id
         __int64 created;            // from the key packet
         __int64 expires;            // 0 when the key does not expire
         int size;                   // the key's own bytes, not the base64
         AnsiString key_base64;
         __int64 uploaded;
         String uploaded_by;
      };

      // One key of one account of a domain, for the administrator's listing.
      // published says whether THIS key is actually served - the account's own
      // switch, with the server's and the domain's already true.
      struct DomainKey
      {
         DomainKey() : published(false) {}
         String address;
         bool published;
         Row row;
      };

      static bool List(__int64 accountId, std::vector<Row> &rows);

      // Every key held by an account of this domain, published or not, oldest
      // account first. An administrator looking at a domain needs to see the
      // keys that are NOT being served as well as the ones that are, which is
      // why this does not apply the switches the directory applies.
      static bool ListDomain(std::shared_ptr<const Domain> domain, std::vector<DomainKey> &keys);
      static bool Find(__int64 accountId, const AnsiString &fingerprint, Row &row);
      static int Count(__int64 accountId);

      // Parse the upload, judge it against the server's limits and the account's
      // own address, and store it. A key whose fingerprint the account already
      // has REPLACES that row (replaced is then true) rather than being refused,
      // so re-uploading a key that has gained a signature is not an error.
      //
      // allowAddressMismatch is an administrator's override of one rule and one
      // rule only: that the key's user ids carry the account's own address. It is
      // never true for an upload the account itself made, because a person
      // publishing a key under their own address that names somebody else's is
      // the mistake the rule exists to catch.
      //
      // False with a whole sentence in problem, which every caller shows as it is.
      static bool Store(std::shared_ptr<const Account> account, const AnsiString &keyText,
                        bool allowAddressMismatch, const String &uploadedBy,
                        Row &row, bool &replaced, AnsiString &problem);

      static bool Remove(__int64 accountId, const AnsiString &fingerprint);

      // Every published key of the account whose local part hashes to this
      // value, concatenated, as the binary a Web Key Directory answers with.
      // False when there is nothing to serve, for ANY reason - no such account,
      // no key, the account not published, the domain not published, the server
      // not publishing. The caller answers 404 to all of them in the same words,
      // which is what keeps the directory from saying who has an account here.
      static bool Lookup(std::shared_ptr<const Domain> domain, const AnsiString &hashedLocalPart, AnsiString &keyBytes);

      // Whether anything of this domain could be served: the server setting and
      // the domain's switch, and the domain being one this server hosts and
      // active. The account's own switch is the third gate and is asked in Lookup.
      static bool PublishingAllowed(std::shared_ptr<const Domain> domain);

      // draft-koch-openpgp-webkey-service section 3.1, for a whole address.
      // Empty when the address has no local part.
      static AnsiString HashedLocalPartOf(const String &address);

      // Where a client would look for this address's key. advanced is the method
      // with the domain in the path, under openpgpkey.<domain>; the direct method
      // is on the domain itself. Both carry the ?l= the draft allows, which is
      // what makes a URL an administrator can paste somewhere useful.
      static AnsiString DirectoryUrl(const String &address, bool advanced);

      // Everything one account's surfaces report about its keys, as one JSON
      // document, so that COM, the administrator's REST route, the account's own
      // route, the Control Deck and hmctl cannot describe the same account
      // differently. Carries the account's switch, whether anything is ACTUALLY
      // served (all three switches together), where a client would look, and the
      // keys themselves - never a secret key, because none is ever held.
      static AnsiString AccountJson(std::shared_ptr<const Account> account);

      // One row as JSON, without its key bytes - the shape COM, the REST routes
      // and the Control Deck all report, so that a field cannot be spelled two
      // ways. keyIncluded adds "key", the base64 of the key itself.
      static AnsiString RowJson(const Row &row, const String &address, bool keyIncluded);
   };
}
