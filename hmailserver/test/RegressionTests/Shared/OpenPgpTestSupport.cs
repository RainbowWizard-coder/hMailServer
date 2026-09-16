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
