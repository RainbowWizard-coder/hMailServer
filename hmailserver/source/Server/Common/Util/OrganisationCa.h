// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// An organisation CA for S/MIME: each domain may have an authority of its own
// that certifies its accounts' keys, so that S/MIME works inside the
// organisation without anybody buying a certificate. Exchange with AD CS is the
// shape; docs/SmimeAuthority.md is the operator's account of it.
//
// WHO HOLDS WHAT
//
// The authority signs; it never makes a person's key. The webmail
// (PortalSmime.js) generates the key pair in the browser, sends a PKCS#10
// request, and keeps the private key wrapped under the account password in the
// existing store (hm_smimekeys), which this server cannot open. A CA that
// generated the keys could decrypt everybody's mail, and this server is built
// so that it cannot - the zero-access design - so the private key never comes
// here, and nothing here can recover mail encrypted to a key that was lost.
//
// The authority's OWN key is here: a self-signed root per domain (P-256, or
// RSA-3072 when asked), CN "<domain> S/MIME Authority", CA:TRUE pathlen:0,
// keyCertSign and cRLSign, emailProtection, a name constraint to the domain's
// own addresses, ten years, in hm_smimeca (schema 6095) with its private key
// protected by the platform's secret store directly - DPAPI with the machine's
// key on Windows, the stored-secret key file on Linux - whatever
// ProtectStoredSecretsWithDPAPI says, and never under the legacy Blowfish
// scheme: when the store is unavailable no authority is created. On Windows the
// envelope opens only on the machine that wrote it (to any process on it); on
// Linux it opens wherever the key file goes. A database or a backup carried to
// a machine where it does not open holds an authority that is refused at every
// signature with a sentence saying to rotate it.
//
// WHAT IS ISSUED
//
// One year (never past the root's own end), X.509 v3, the serial the domain's
// counter gives - carried on to a rotated root, so that a serial names one
// certificate of the domain - subject CN = the account's name (or its
// address) and emailAddress, SAN rfc822Name, EKU emailProtection, keyUsage
// digitalSignature and keyEncipherment for an RSA key or digitalSignature and
// keyAgreement for an EC one, basicConstraints CA:FALSE, SKI and AKI, and -
// when SmimeAuthorityBaseUrl is set - a CRL distribution point and an
// authority-information-access caIssuers URL under it, which name the root by
// its SHA-256 fingerprint so that they survive a restore and a rename. The
// request's key is judged against GatewayEncryption's floors (RSA of 2048 bits
// or more, or P-256 or P-384); its address must be the account's own or an
// active alias of the account's own domain that points at it, and nothing
// else. Five enrolments per account a day, ten valid certificates at once.
//
// TWO PHASES
//
// An enrolment issues and answers the certificate, which is then PENDING: not
// answered to colleagues, and superseding nothing yet. It is confirmed when the
// key arrives in the account's own store (the page's PUT of it, which is the
// key's first use on this server) or by POST .../issued/{serial}/confirm from a
// client that keeps its key elsewhere; only then are the address's earlier
// certificates revoked as superseded, unless the request said keep. A store
// that is refused therefore leaves the earlier certificate valid.
//
// PUBLICATION
//
// The root and the CRL are public and are served over plain HTTP by the web
// services listener (WebServicesServer, /smime-ca/...), never by the REST
// listener, as RFC 5280 recommends for a distribution point. A colleague's
// certificate is answered to an authenticated account of the same domain
// (GET /api/v1/me/directory/{address}/smime), confirmed and valid, while its
// address still belongs to the account it was issued to. The CRL is X.509 v2,
// signed by the root it speaks for, made again when one of that root's
// certificates is revoked and otherwise at most daily, nextUpdate a week on.
//
// TRUST IN THIS WEBMAIL
//
// The chain check (POST /api/v1/me/smime/chain) trusts the domain's current
// root and a retired one while it is published, and a chain ending at one of
// them only when its leaf is in hm_smimeissued and not revoked: the check reads
// the authority's own record rather than a CRL.
//
// ROTATION
//
// A rotation makes a new root and retires the old one without deleting it: the
// old root keeps its key, signs its own CRL and stays published and trusted
// until the last certificate it signed has expired; after that its key is
// wiped (at the next create, rotation, enrolment or backup) and the backup no
// longer carries it.
//
// WHAT IT IS NOT
//
// No AD CS, no OCSP, no key escrow and so no recovery of mail encrypted to a
// lost key. The Control Panel shows nothing of it - it is managed in the
// Control Deck and over REST - because the Control Panel reads the server
// over COM alone and this has no COM surface.
//
// Every write reaches the audit trail through the statement chokepoint
// (hm_smimeca and hm_smimeissued are audited tables), each row named - a root
// by its domain, a certificate by its address and serial.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace HM
{
   class Account;

   class OrganisationCa
   {
   public:

      // One of a domain's roots, as hm_smimeca holds it.
      struct Authority
      {
         __int64 id = 0;
         __int64 domainId = 0;
         AnsiString certificate;   // base64 DER
         AnsiString fingerprint;   // SHA-256 of the DER, 64 lowercase hex digits: its name in the URLs
         String protectedKey;      // the platform store's envelope, as stored; empty once wiped
         __int64 created = 0;
         __int64 retired = 0;      // 0 for the current root
         __int64 serial = 0;       // the next serial number
      };

      // Where a certificate stands between its issue and its key being stored.
      enum Pending
      {
         PendingNone = 0,          // confirmed
         PendingSupersedes = 1,    // on confirmation, revokes the address's others as superseded
         PendingKeeps = 2          // on confirmation, keeps them
      };

      // One certificate a root signed, as hm_smimeissued holds it.
      struct Issued
      {
         __int64 id = 0;
         __int64 caId = 0;
         __int64 accountId = 0;
         String address;           // empty once erased
         __int64 serial = 0;
         AnsiString fingerprint;   // SHA-256 of the DER, 64 lowercase hex digits
         __int64 notBefore = 0;
         __int64 notAfter = 0;
         __int64 revoked = 0;      // 0, or when it was revoked
         String reason;            // superseded, keyCompromise or cessationOfOperation
         AnsiString certificate;   // base64 DER; empty once erased
         int pending = PendingNone;
      };

      // What a root is, read from its certificate for the surfaces.
      struct RootFacts
      {
         AnsiString subject;
         AnsiString fingerprint;
         AnsiString keyType;       // "P-256", "RSA-3072", ...
         __int64 notBefore = 0;
         __int64 notAfter = 0;
         bool readable = false;    // the certificate parses at all
         bool keyOpens = false;    // the private key opens on this machine
      };

      enum KeyKind
      {
         KeyP256 = 0,
         KeyRsa3072 = 1
      };

      // RFC 5280's CRLReason numbers for the three this authority records.
      enum Reason
      {
         ReasonKeyCompromise = 1,
         ReasonSuperseded = 4,
         ReasonCessationOfOperation = 5
      };

      static const int CertificateDays = 365;
      static const int RootYears = 10;
      static const int CrlLifetimeDays = 7;
      static const int CrlRefreshSeconds = 86400;
      // A person with two devices keeps two; nobody needs more than this at once.
      static const int MaximumValidPerAccount = 10;
      // Enrolments an account may make in a day: each writes a row and, once
      // it supersedes, a permanent entry on a public CRL.
      static const int EnrolmentsPerDay = 5;
      static const int MaximumRequestLength = 16 * 1024;
      // ub-emailaddress-length (RFC 5280): what a certificate's emailAddress holds.
      static const int MaximumAddressLength = 128;

      // ---- reading --------------------------------------------------------

      // The domain's current root. False when it has none (or the table could
      // not be read).
      static bool Current(__int64 domainId, Authority &authority);

      // Every root the domain has, the current one first and then the retired
      // ones newest first.
      static bool ForDomain(__int64 domainId, std::vector<Authority> &authorities);

      // One root of the domain by its id.
      static bool Find(__int64 domainId, __int64 caId, Authority &authority);

      // A root by its fingerprint, whichever domain it is (the public URLs name
      // it so, with no domain in them).
      static bool FindByFingerprint(const AnsiString &fingerprint, Authority &authority);

      // Whether a root is published: the current one always, a retired one
      // while a certificate it signed has not expired.
      static bool IsPublished(const Authority &authority, __int64 now);

      // The facts the surfaces show about a root. tryKey opens the private key
      // to say whether it opens on this machine, which the administrator's view
      // asks and a person's page load does not.
      static RootFacts Facts(const Authority &authority, bool tryKey);

      // The certificates the domain's roots issued, newest first, at most limit.
      static bool IssuedForDomain(__int64 domainId, int limit, std::vector<Issued> &issued);

      // The certificates issued to one account by its domain's roots, newest first.
      static bool IssuedForAccount(__int64 domainId, __int64 accountId, std::vector<Issued> &issued);

      // The confirmed, valid (not revoked, not expired) certificates of an
      // address, when the address belongs to an active account of the domain
      // and still belongs to the account it was issued to - what a colleague
      // is answered. Empty when there are none.
      static bool Published(__int64 domainId, const String &address, std::vector<Issued> &issued);

      // The domain's record of the certificate with this fingerprint. False
      // when its roots issued none.
      static bool IssuedByFingerprint(__int64 domainId, const AnsiString &fingerprint, Issued &issued);

      // Whether the domain's authority issued the certificate with this
      // fingerprint - the mark hm_smimekeys.smimeauthority carries. The caller
      // gives the fingerprint of the certificate's own bytes.
      static bool IssuedHere(__int64 domainId, const AnsiString &fingerprint);

      // Where each of an account's stored certificates that carry the mark
      // stands in the authority's record, by fingerprint: "valid", "revoked" or
      // "expired". A marked row missing from the map has no record in the
      // account's domain any more.
      static bool StoredStatuses(__int64 accountId, __int64 domainId, std::map<AnsiString, AnsiString> &statuses);

      // The DER of every root the chain check trusts for an account of the
      // domain: the current one, and a retired one while it is published.
      static bool TrustAnchors(__int64 domainId, std::vector<std::string> &ders);

      // ---- acting -----------------------------------------------------------

      // Makes the domain's first root (rotate false; 409 when it has one) or a
      // new root in place of the current one (rotate true; 404 when it has
      // none). Answers an HTTP status: 201 or 200 with made filled, else the
      // status with problem saying why.
      static int Create(__int64 domainId, const String &domainName, KeyKind kind, bool rotate, Authority &made, String &problem);

      // Signs the account's request. Answers 201 with made filled - pending -
      // and the serials its confirmation will supersede, or the status with
      // problem saying why: 400 for a request refused, 404 when the domain has
      // no authority, 409 when the account already holds as many valid
      // certificates as it may, 429 when it has enrolled as often as it may
      // today, 503 when the authority's key does not open on this machine.
      static int Enrol(std::shared_ptr<const Account> account, const AnsiString &requestText, bool keep,
                       Issued &made, std::vector<__int64> &supersedes, String &problem);

      // Confirms one of the account's certificates by its serial: its key is
      // stored. 200 with the serials it superseded (none when it was
      // confirmed already or keeps the others); 404 when the account has no
      // such certificate; 409 when it is revoked.
      static int Confirm(std::shared_ptr<const Account> account, __int64 serial, std::vector<__int64> &superseded, String &problem);

      // The same, for a certificate that has just been put in the account's own
      // store: by the fingerprint of its bytes, and silent when it is not one
      // of the account's pending certificates.
      static void ConfirmStored(std::shared_ptr<const Account> account, const AnsiString &fingerprint);

      // Revokes the domain's certificate with this serial. accountId, when not
      // 0, is the one account the certificate must belong to (a person revoking
      // their own). 200 when revoked; 404 when there is no such certificate;
      // 409 when it is revoked already (the first revocation's date and reason
      // stand).
      static int Revoke(__int64 domainId, __int64 serial, __int64 accountId, Reason reason, Issued &revoked, String &problem);

      // An account is being deleted: every valid certificate issued to it is
      // revoked (cessationOfOperation) and the account id cleared from all of
      // them, so that a number handed out again owns none of them. They stay on
      // the CRL. Nothing is written when no certificate names the account.
      // Answers false when the table could not be read or written.
      static bool AccountDeleted(__int64 accountId, const String &address);

      // The address erasure (AddressTraceEraser): every certificate issued for
      // the address is revoked (cessationOfOperation) if it is valid, and the
      // address, the account and the certificate itself - which names the
      // person - are taken off the row; its serial, fingerprint and revocation
      // stay, which is what its root's CRL needs. Answers the rows changed, -1
      // on a failure.
      static int EraseAddress(const String &address);

      // ---- publication -------------------------------------------------------

      // The root's CRL, signed by it: made again when one of its certificates
      // was revoked since the last, and otherwise at most once a day. 200 with
      // der filled; 404 when the root's key does not open on this machine
      // (reported once, since it stays so until the authority is rotated); 500
      // when it could not be made.
      static int Crl(const Authority &authority, AnsiString &der, String &problem);

      // PEM of base64 DER under the label ("CERTIFICATE").
      static AnsiString Pem(const AnsiString &base64Der, const char *label);

      // SmimeAuthorityBaseUrl without a trailing slash, or empty when unset or
      // not usable - in which case certificates carry no distribution point.
      static String BaseUrl();

      // Whether a value may be stored as SmimeAuthorityBaseUrl. reason is the
      // sentence the settings route refuses with.
      static bool IsUsableBaseUrl(const String &url, String &reason);

      // The paths under the web services listener of one root, by its
      // fingerprint: /smime-ca/<fingerprint>.pem, .cer or .crl - what an issued
      // certificate names.
      static AnsiString RootPath(const AnsiString &fingerprint, const char *extension);
      static AnsiString CrlPath(const AnsiString &fingerprint);

      // The path of the domain's CURRENT root or CRL, whichever root that is:
      // /smime-ca/<domain>.pem, .cer or .crl - what an administrator hands out.
      static AnsiString CurrentPath(const String &domainName, const char *extension);

      // 64 lowercase hex digits.
      static bool IsFingerprint(const AnsiString &text);

      // ---- reasons ------------------------------------------------------------

      static bool ParseReason(const String &text, Reason &reason);
      static String ReasonName(int reason);

      // ---- backup --------------------------------------------------------------

      // The domain's roots - certificate, key envelope as stored (none for a
      // retired root no longer published), counter - and everything they
      // issued, under the domain in the backup.
      static bool XMLStore(XNode *parentNode, __int64 domainId);

      // Puts them back under the domain's new id, after its accounts, each
      // certificate under the account its address names now (0 when none).
      // Every value is checked and re-encoded: a certificate that does not
      // parse, or a fingerprint that is not one, is left out and said.
      static bool XMLLoad(XNode *parentNode, __int64 domainId);
   };
}
