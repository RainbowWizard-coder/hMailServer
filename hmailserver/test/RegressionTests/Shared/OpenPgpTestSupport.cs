// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    What the OpenPGP fixtures share: the keys they upload, the values they
   ///    assert, and the COM members they reach.
   ///
   ///    THE KEYS ARE REAL. Both were made by GnuPG 2.4.7 and exported with
   ///    gpg --export; the fingerprints below are the ones gpg printed for them,
   ///    so a fixture that asserts one is checking this server against another
   ///    implementation rather than against itself. Neither address exists, and
   ///    neither secret half is anywhere in this repository - only the public
   ///    half is here, which is the whole point of the feature under test.
   ///
   ///    THE COM MEMBERS BIND LATE. Settings.OpenPGP*, Domain.OpenPGPPublishKeys,
   ///    Account.OpenPGPPublish and OpenPGPKeys and the two Utilities methods are
   ///    new members on interfaces that already exist, and the test project is
   ///    compiled against the type library REGISTERED on the machine - which does
   ///    not carry them until an elevated build/register-com.ps1 has run. Every
   ///    one of them therefore goes through a dynamic call here, exactly as the
   ///    bounce fixtures' helpers do, so a refusal still arrives as the
   ///    COMException an assertion expects.
   /// </summary>
   public static class OpenPgpTestSupport
   {
      /// <summary>
      ///    "Suite Key &lt;pgp@example.test&gt;", Ed25519, no expiry - the key for an
      ///    account in the suite's own domain. base64 of the bytes gpg --export wrote.
      /// </summary>
      public const string SuiteKeyBase64 =
         "mDMEaqoO7RYJKwYBBAHaRw8BAQdArAf5OYyuP+RBd4jNPvcP57ALwX6bYeRdH6vi/SA0QCS0HFN1" +
         "aXRlIEtleSA8cGdwQGV4YW1wbGUudGVzdD6IkwQTFgoAOxYhBIZqw6B4X1Gv1eraRjXvtdE9uQoR" +
         "BQJqqg7tAhsDBQsJCAcCAiICBhUKCQgLAgQWAgMBAh4HAheAAAoJEDXvtdE9uQoRGVsBAIkwx8ql" +
         "l/WD5FsT8SYf+0JVuDRImYrOY7eh8EgWGILXAQDsNFQ/6/Vh+ohhiEexIhNNzyH9mFROcPk68d4Q" +
         "IkVeDw==";

      /// <summary>The fingerprint gpg prints for the key above, lower-cased.</summary>
      public const string SuiteKeyFingerprint = "866ac3a0785f51afd5eada4635efb5d13db90a11";

      /// <summary>
      ///    "Ann Example &lt;ann@example.org&gt;" and a second user id, RSA 2048,
      ///    expiring a year after it was made: 1030 bytes, which is what makes it
      ///    the key for the size-cap case, since 1024 is the lowest cap there is.
      ///    The same bytes as AnnKeyBase64 in the server's OpenPgpKeyTester.
      /// </summary>
      public const string RsaKeyBase64 =
         "mQENBGqqAiEBCADFsurRCxIYwqBxuwUf9vktSLk3PkTNlDCklnFzS7R18aq/nnKVfK6iQK1YHFxr" +
         "Tam53W9V7mH8H68uysFupWca7Btar+fX4LhmTeo6A44yiZXQlw6u6hZisA/GaLWsuGKvFQVBufbd" +
         "AsIfXByY5ZVdDnF35DaEJPKZUESWXPHedISljkYB2gl7MtvNF5uMnKnDlzQNS+VRiEXhf9pTltLm" +
         "wulM7c7pNzHpTw+kC1bvG5uhJwEXFes/f02rzWorXvrz8+ZP3/+kOgQp6B6jeORAsioG8HERC/us" +
         "oR93PiUiDPuAMvSLVDsaY6hJdD7S1VXnz3wPj7ZL+ABHUaU07mjHABEBAAG0HUFubiBFeGFtcGxl" +
         "IDxhbm5AZXhhbXBsZS5vcmc+iQFXBBMBCABBFiEEgAsmzMNuDjtKYaJrGf56L+kF3yAFAmqqAiEC" +
         "GwMFCQHhM4AFCwkIBwICIgIGFQoJCAsCBBYCAwECHgcCF4AACgkQGf56L+kF3yAcLwgAoLJBC96F" +
         "/vOtmOu5DCelFeiRGWxYiq4Rm0W9KN/cHYNvTNamD5BSb/aiXXZqzlbGvMA9zqebYvxMStX/OLZt" +
         "8TBLxjV5bV3QAykDtATJW1T7tmqjgy355E9yeRd06t+kBmSmmY6HiSykqd+9Z9YfGyiygOlpKA+N" +
         "Kk7qxFBrcL4w6UOWBdM2w7X65eR9BcDydfMJHrwNS/Gtv/3ZrVclCXrk5E1+BL0cKL+TSbUXjns+" +
         "LhSc69ggi3D60H/a8R/pWcCPTFmplpeCFO+ygwS8bOoSv9LdZ09CjSuPwUmrzHHKsMVMuA11wNPi" +
         "moIlj+S8cqLYiQfsP1srRTfDgWeIfLQhQW5uIEUgRXhhbXBsZSA8YW5uLmVAZXhhbXBsZS5vcmc+" +
         "iQFXBBMBCABBFiEEgAsmzMNuDjtKYaJrGf56L+kF3yAFAmqqAiUCGwMFCQHhM4AFCwkIBwICIgIG" +
         "FQoJCAsCBBYCAwECHgcCF4AACgkQGf56L+kF3yDkWgf/T1/tjVq1T6G+LZMwt0LIF5pbqBglDqfm" +
         "c6xqr4huHQA9tAhqupAT7o/mfRxqRD5CdfzRTfY087Gv9u51KAkRvEbMjnq6oWxQ+YDwtotFSZeY" +
         "TTFChNTkrM2WLRjhipoJW7/+ulcYVE4VUDmDkIChaApsT90m28GGqcvDiQhxsyxPV6xy3VqJOFg7" +
         "kOS/A3/GANaOgk7D9aPy44JcnDTlCNZksU4JkktHY2GCyB+5Pjk3ZI9Fq+omDecdokfn67868rDv" +
         "84C6PqbVF7xiuKYsU4z63S/Kzj6TCDVe20Hi8T/ylRQE8PV9zHacExOkEIj/vwpo2WPcWps/meuw" +
         "owKaAQ==";

      /// <summary>
      ///    "Joe Doe &lt;joe.doe@example.org&gt;", Ed25519 with a Curve25519 subkey.
      ///    Its user id names an address OUTSIDE the suite's domain, which is what
      ///    makes it the key for the address-mismatch cases.
      /// </summary>
      public const string OtherKeyBase64 =
         "mDMEaqoCExYJKwYBBAHaRw8BAQdA1BnI224Vt/ATv5Bz3LFdMeQvJKTMb+poSTbQQ/LMCbq0HUpv" +
         "ZSBEb2UgPGpvZS5kb2VAZXhhbXBsZS5vcmc+iJMEExYKADsWIQTCm2zONh5zuhNyTLi77kUrmwoL" +
         "twUCaqoCEwIbAwULCQgHAgIiAgYVCgkICwIEFgIDAQIeBwIXgAAKCRC77kUrmwoLt58WAQDdPjL0" +
         "sHXuW7D6eqbmCmbURqQ9Ov8scnFyKsEPkA85lAD/VT/yJn8zo+3dCGtmBudvsQbexnGQkjVW3Yzo" +
         "jkj7RAi4OARqqgITEgorBgEEAZdVAQUBAQdAHKnYcC+ab6m2F5NyPeVPCwzQFzNIqPMaIkoeuyvE" +
         "wzYDAQgHiHgEGBYKACAWIQTCm2zONh5zuhNyTLi77kUrmwoLtwUCaqoCEwIbDAAKCRC77kUrmwoL" +
         "t4etAQDidGBnPZ3BKXDgd3iwtPNfDxmdaae4hRPJ6rBBXhGAdgD8DDeT9bI9oZqHeCt5mGu1DzJh" +
         "zDzrRPuZ7gABvV9puQc=";

      public const string OtherKeyFingerprint = "c29b6cce361e73ba13724cb8bbee452b9b0a0bb7";

      /// <summary>
      ///    A SECRET-key packet (tag 5) and nothing else: 0xC5, a length of four,
      ///    and four bytes the parser never reaches, because the refusal is on the
      ///    tag. No real secret key is in this repository, and this is why there
      ///    does not need to be one.
      /// </summary>
      public const string SecretKeyBase64 = "xQQEAAAA";

      /// <summary>Three letters. Not packets, not base64 of packets: not a key.</summary>
      public const string MalformedKeyBase64 = "QUJD";

      /// <summary>An armoured PRIVATE KEY BLOCK, refused by its header line.</summary>
      public const string ArmouredSecretKey =
         "-----BEGIN PGP PRIVATE KEY BLOCK-----\n\nAAAA\n-----END PGP PRIVATE KEY BLOCK-----\n";

      /// <summary>
      ///    The z-base-32 of the SHA-1 of the lower-cased local part, as
      ///    draft-koch-openpgp-webkey-service section 3.1 defines it. Computed with
      ///    Python's hashlib rather than by this server, and "Joe.Doe" is the
      ///    draft's own worked example.
      /// </summary>
      public const string HashedJoeDoe = "iy9q119eutrkn8s1mk4r39qejnbu3n5q";
      public const string HashedPgp = "eb5d7bz4mnfcwrx9uxz14t6ezm8kesjf";
      public const string HashedNobody = "g3xcn6u8mh388xysa7dsdmcd6m8oxtc4";


      // ---- the webmail's key ring (hm_pgpring, RestApiOpenPgpKeys.cpp) --------
      //
      // The rows the ring holds are a public key and, for a key of the
      // account's own, a SECRET key that is encrypted under a passphrase the
      // browser chose. Both halves below were made by GnuPG 2.4.8
      // (build/make-openpgp-fixtures.sh, which is also where
      // build/check-portal-openpgp.js gets them) and exported with gpg, so a
      // fixture that puts one here is handing this server a key another
      // implementation wrote. The addresses are in the .test top-level domain,
      // which can never be delivered to, and nothing anywhere trusts them.
      //
      // THE PASSPHRASE IS PRINTED HERE, and that is fine: it protects a key made
      // for this check and for nothing else. RingNakedSecretArmoured is the
      // opposite case and the one that matters most - a secret key with NO
      // passphrase at all, which the store must refuse rather than keep.
      public const string RingPassphrase = "openpgp-fixture-pass";
      public const string RingOwnAddress = "alice@fixtures.test";
      public const string RingPeerAddress = "bob@fixtures.test";
      public const string RingOwnFingerprint = "3c6ff5e3458173dd5db3f4a4a14263c97de6857d";
      public const string RingPeerFingerprint = "34b2ac7e52143a1a820323d1cf6875a2ec40ecd5";

      /// <summary>Alice's transferable public key, Ed25519 with a Curve25519 subkey.</summary>
      public const string RingOwnPublicBase64 =
         "mDMEaq0hphYJKwYBBAHaRw8BAQdAogB/YdOhOFS5ajaz62eLUepoNXY/3nTQgUXElQCO+fy0I0Fs" +
         "aWNlIEZpeHR1cmUgPGFsaWNlQGZpeHR1cmVzLnRlc3Q+iJAEExYKADgWIQQ8b/XjRYFz3V2z9KSh" +
         "QmPJfeaFfQUCaq0hpgIbAwULCQgHAgYVCgkICwIEFgIDAQIeAQIXgAAKCRChQmPJfeaFfaSaAP4z" +
         "1aPIosKBxbw2o6g4StHegDSHti6i6BaakoCP3DqqoAEAvvu1HHt590Ku50WUoHSMaO9EdiPdNpDF" +
         "lOZv3UVyfAi4OARqrSGmEgorBgEEAZdVAQUBAQdAGQu/CHDJ97RxQz9zk4Jk8BUAvFvLbNRtnTzZ" +
         "ll8CQzEDAQgHiHgEGBYKACAWIQQ8b/XjRYFz3V2z9KShQmPJfeaFfQUCaq0hpgIbDAAKCRChQmPJ" +
         "feaFfTkvAP9vpnBYjBI3tYEJqpJteokGrh4/EWyM3WNO/DUJtc62aAEAukeUO3aWgpkHejO2t5W8" +
         "4vVO3MeX4UFOng31q2kkOA0=";

      /// <summary>Bob's, RSA 3072 with an RSA subkey - a correspondent's key.</summary>
      public const string RingPeerPublicBase64 =
         "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" +
         "SQrVBZKl4woqqSL3mK3tie3OFliobdOY2FyvU7w=";

      /// <summary>Alice's secret key, encrypted under RingPassphrase by GnuPG.</summary>
      public const string RingOwnSecretArmoured =
         "-----BEGIN PGP PRIVATE KEY BLOCK-----\n" +
         "\n" +
         "lIYEaq0hphYJKwYBBAHaRw8BAQdAogB/YdOhOFS5ajaz62eLUepoNXY/3nTQgUXE\n" +
         "lQCO+fz+BwMCCm53ArilrWT/NX44Rjo8cR7EpU9XO/Y33Da+NHf2CxHms0HYRCsT\n" +
         "0lYUIHzEVF3TZ9tnnYVwBJn0cB8xH8iGC6QO9Cxh57af2OKjs+QX+bQjQWxpY2Ug\n" +
         "Rml4dHVyZSA8YWxpY2VAZml4dHVyZXMudGVzdD6IkAQTFgoAOBYhBDxv9eNFgXPd\n" +
         "XbP0pKFCY8l95oV9BQJqrSGmAhsDBQsJCAcCBhUKCQgLAgQWAgMBAh4BAheAAAoJ\n" +
         "EKFCY8l95oV9pJoA/jPVo8iiwoHFvDajqDhK0d6ANIe2LqLoFpqSgI/cOqqgAQC+\n" +
         "+7Uce3n3Qq7nRZSgdIxo70R2I902kMWU5m/dRXJ8CJyLBGqtIaYSCisGAQQBl1UB\n" +
         "BQEBB0AZC78IcMn3tHFDP3OTgmTwFQC8W8ts1G2dPNmWXwJDMQMBCAf+BwMC3qYr\n" +
         "q5qt6h///g3hlmftJwAP99Nf1Fzz/yETtp2oRZRJAvurpTiPd0ZqP9H8vM5uMTMV\n" +
         "JLkVYM37YGhTe6SgkV9YrV7e8QAWxvTjau2xzYh4BBgWCgAgFiEEPG/140WBc91d\n" +
         "s/SkoUJjyX3mhX0FAmqtIaYCGwwACgkQoUJjyX3mhX05LwD/b6ZwWIwSN7WBCaqS\n" +
         "bXqJBq4ePxFsjN1jTvw1CbXOtmgBALpHlDt2loKZB3oztreVvOL1TtzHl+FBTp4N\n" +
         "9atpJDgN\n" +
         "=Te2g\n" +
         "-----END PGP PRIVATE KEY BLOCK-----\n";

      /// <summary>Bob's, for the case where the two halves are not one key.</summary>
      public const string RingPeerSecretArmoured =
         "-----BEGIN PGP PRIVATE KEY BLOCK-----\n" +
         "\n" +
         "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\n" +
         "SQrVBZKl4woqqSL3mK3tie3OFliobdOY2FyvU7w=\n" +
         "=IiSR\n" +
         "-----END PGP PRIVATE KEY BLOCK-----\n";

      /// <summary>A secret key GnuPG made with %no-protection: no passphrase at all, which the store refuses by name.</summary>
      public const string RingNakedSecretArmoured =
         "-----BEGIN PGP PRIVATE KEY BLOCK-----\n" +
         "\n" +
         "lFgEaq0vORYJKwYBBAHaRw8BAQdA/ZAsUmr/pqVTd49DWeXLSV3oMONR9s0FIALs\n" +
         "G+YVlS8AAQD5NZFLF9bX418k3g3P+a8M6aAbqnyyjND3ogZ3bPIrKBFCtCNOYWtl\n" +
         "ZCBGaXh0dXJlIDxuYWtlZEBmaXh0dXJlcy50ZXN0PoiQBBMWCgA4FiEE2YKo1uMG\n" +
         "/XDLuKLbKZ34NtYJrpYFAmqtLzkCGwMFCwkIBwIGFQoJCAsCBBYCAwECHgECF4AA\n" +
         "CgkQKZ34NtYJrpZbXQD/VWYH5uUA0JU1CbhYpL8KIJ52F+4HN4y2xeqcEUHW8mkB\n" +
         "ALGWLx/SaczVm6RsQJJoL0kZLybnx7J4wrbm4OKNtG8GnF0Eaq0vORIKKwYBBAGX\n" +
         "VQEFAQEHQFHHxUwRDwNZoyqSTVlY9EmVDzb30PVeGm8H2jPFhzpUAwEIBwAA/2Ft\n" +
         "IrZQu/zmM5Ypou+wW3jpUsMYQ5j6eYRG5apuS6mgEleIeAQYFgoAIBYhBNmCqNbj\n" +
         "Bv1wy7ii2ymd+DbWCa6WBQJqrS85AhsMAAoJECmd+DbWCa6WDToBAMUQD5M/4my+\n" +
         "AHeXtlWjt4kx0DdD6dlnKk6NWg85VXQfAQDBzenmtJ5NmxAxSMI1CHgCrrUEF2kC\n" +
         "4+94/KrhLxeIBw==\n" +
         "=Wmqc\n" +
         "-----END PGP PRIVATE KEY BLOCK-----\n";

      /// <summary>The same bytes as an armoured block, with no checksum line.</summary>
      public static string Armoured(string base64Key)
      {
         return "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n" + base64Key + "\n-----END PGP PUBLIC KEY BLOCK-----\n";
      }

      // ---- the settings, late-bound -------------------------------------------

      public static bool WebKeyDirectoryEnabled(Settings settings) =>
         (bool) ((dynamic) settings).OpenPGPWebKeyDirectoryEnabled;

      public static void SetWebKeyDirectoryEnabled(Settings settings, bool value) =>
         ((dynamic) settings).OpenPGPWebKeyDirectoryEnabled = value;

      public static int KeyMaximumBytes(Settings settings) => (int) ((dynamic) settings).OpenPGPKeyMaximumBytes;

      public static void SetKeyMaximumBytes(Settings settings, int value) =>
         ((dynamic) settings).OpenPGPKeyMaximumBytes = value;

      public static int KeysPerAccount(Settings settings) => (int) ((dynamic) settings).OpenPGPKeysPerAccount;

      public static void SetKeysPerAccount(Settings settings, int value) =>
         ((dynamic) settings).OpenPGPKeysPerAccount = value;

      public static bool RequireAddressInUserId(Settings settings) =>
         (bool) ((dynamic) settings).OpenPGPRequireAddressInUserId;

      public static void SetRequireAddressInUserId(Settings settings, bool value) =>
         ((dynamic) settings).OpenPGPRequireAddressInUserId = value;

      // ---- the domain and the account, late-bound -----------------------------

      public static bool DomainPublishes(Domain domain) => (bool) ((dynamic) domain).OpenPGPPublishKeys;

      public static void SetDomainPublishes(Domain domain, bool value) =>
         ((dynamic) domain).OpenPGPPublishKeys = value;

      public static bool DomainPublishesNewAccounts(Domain domain) =>
         (bool) ((dynamic) domain).OpenPGPPublishNewAccounts;

      public static void SetDomainPublishesNewAccounts(Domain domain, bool value) =>
         ((dynamic) domain).OpenPGPPublishNewAccounts = value;

      public static bool AccountPublishes(Account account) => (bool) ((dynamic) account).OpenPGPPublish;

      public static void SetAccountPublishes(Account account, bool value) =>
         ((dynamic) account).OpenPGPPublish = value;

      public static string AccountKeys(Account account) => (string) ((dynamic) account).OpenPGPKeys;

      // ---- Utilities, late-bound ---------------------------------------------

      public static string ImportKey(Application application, string address, string key, bool allowMismatch) =>
         (string) ((dynamic) application.Utilities).ImportOpenPGPKey(address, key, allowMismatch);

      public static bool RemoveKey(Application application, string address, string fingerprint) =>
         (bool) ((dynamic) application.Utilities).RemoveOpenPGPKey(address, fingerprint);

      /// <summary>
      ///    Every OpenPGP setting back to what the server ships, whatever it holds
      ///    now. Unconditional, for a TearDown: a test that failed half way may have
      ///    changed any of them, and publishing left switched on would be a server
      ///    handing out keys to every fixture that came after.
      /// </summary>
      public static void RestoreDefaults(Settings settings)
      {
         SetWebKeyDirectoryEnabled(settings, false);
         SetKeyMaximumBytes(settings, 65536);
         SetKeysPerAccount(settings, 8);
         SetRequireAddressInUserId(settings, true);
      }
   }
}
