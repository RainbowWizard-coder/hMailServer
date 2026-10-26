// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net.Sockets;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Which account a name given at sign-in names (Common/Util/LogonName.h): the one
   ///    whose stored address IS the name, in other case, under the default domain or a
   ///    domain alias - and no other spelling the database's own comparison takes for it.
   ///    Up to 6.3.7 the database chose: MySQL and MariaDB took an accented letter for a
   ///    plain one, and they, SQL Server and SQL Server Compact took the address with
   ///    spaces after it, and SQL Server Compact a soft hyphen inside it, while the
   ///    per-name lockout and the compromise hold counted such a spelling apart. A
   ///    spelling with the right password now fails as a name that does not exist
   ///    fails, with the same reply, and is counted as a failure; and a locked name is
   ///    not got round by one. The names go over IMAP LOGIN as bytes in the server's
   ///    narrow code page (Windows-1252 on Windows, Latin-1 on Linux; the same bytes
   ///    for every character here), as a client typing them sends them. On PostgreSQL,
   ///    which compares lower() of both sides, none of these spellings found the
   ///    account before either, so there the tests hold of the old build too.
   ///    The held account's case is in API/RestApiCompromisedAccount.
   /// </summary>
   [TestFixture]
   public class LogonNameVariants : TestFixtureBase
   {
      private const string Password = "Variant-Passw0rd!";

      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "variant@" + _domain.Name, Password);
      }

      /// <summary>
      ///    The spellings of address that the databases took for it: an accented letter
      ///    (MySQL, MariaDB), a soft hyphen inside it (SQL Server Compact, and SQL Server
      ///    under its _100 collations) and a space after it (MySQL, MariaDB, SQL Server,
      ///    SQL Server Compact). address must hold an "a" after its first character.
      /// </summary>
      public static List<string> Variants(string address)
      {
         int a = address.IndexOf('a', 1);
         return new List<string>
         {
            address.Substring(0, a) + "\u00e0" + address.Substring(a + 1),
            address.Substring(0, a) + "\u00ad" + address.Substring(a),
            address + " "
         };
      }

      [Test]
      [Description("Each spelling the databases took for the account fails with its right password exactly as a name that does not exist fails; its own spelling and other case sign in")]
      public void AVariantSpellingFailsAsAWrongNameDoes()
      {
         string wrongName = ImapLogin("nobody-at-all@" + _domain.Name, Password);
         StringAssert.StartsWith("NO", wrongName);

         foreach (string variant in Variants(_account.Address))
         {
            ClassicAssert.AreEqual(wrongName, ImapLogin(variant, Password),
               "The spelling '" + Escape(variant) + "' with the account's right password must fail as a name that does not exist fails.");
         }

         StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password), "The account's own address signs in.");
         StringAssert.StartsWith("OK", ImapLogin(_account.Address.ToUpperInvariant(), Password), "The address in other case signs in.");
      }

      [Test]
      [Description("The address under a domain alias and the local part alone under the default domain sign in, in other case too; an accented spelling under either fails as a name that does not exist fails")]
      public void AnAliasAndTheDefaultDomainStillSignIn()
      {
         string wrongName = ImapLogin("nobody-at-all@" + _domain.Name, Password);
         StringAssert.StartsWith("NO", wrongName);

         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "logon-alias.example";
         alias.Save();

         string defaultDomain = _settings.DefaultDomain;
         try
         {
            StringAssert.StartsWith("OK", ImapLogin("variant@logon-alias.example", Password), "The address under a domain alias signs in.");
            StringAssert.StartsWith("OK", ImapLogin("VARIANT@Logon-Alias.Example", Password), "The address under a domain alias in other case signs in.");
            ClassicAssert.AreEqual(wrongName, ImapLogin("v\u00e0riant@logon-alias.example", Password),
               "An accented spelling under a domain alias fails as a name that does not exist fails.");

            _settings.DefaultDomain = _domain.Name;
            StringAssert.StartsWith("OK", ImapLogin("variant", Password), "The local part alone signs in under the default domain.");
            // A bare name that does not exist, for the reply a bare name gets.
            string wrongBareName = ImapLogin("nobody-at-all", Password);
            StringAssert.StartsWith("NO", wrongBareName);
            ClassicAssert.AreEqual(wrongBareName, ImapLogin("v\u00e0riant", Password),
               "An accented local part under the default domain fails as a name that does not exist fails.");
         }
         finally
         {
            _settings.DefaultDomain = defaultDomain;
            _domain.DomainAliases.DeleteByDBID(alias.ID);
         }
      }

      [Test]
      [Description("A spelling the sign-in refuses is counted as a failure: three of the address followed by a space, with the right password, lock the account's name")]
      public void AVariantSpellingIsCountedAsAFailure()
      {
         try
         {
            SetIniSetting("AccountLockoutThreshold", "3");
            _application.Reinitialize();

            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("NO", ImapLogin(_account.Address + " ", Password), "The address followed by a space does not sign in.");

            StringAssert.StartsWith("NO", ImapLogin(_account.Address, Password),
               "Three failures under a spelling the lockout counts as the account lock the account's name.");
         }
         finally
         {
            SetIniSetting("AccountLockoutThreshold", null);
            _application.Reinitialize();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      [Test]
      [Description("A locked name is not got round by another spelling of it: once three wrong passwords lock it, no spelling signs in with the right one")]
      public void ALockedNameIsNotGotRoundByAVariantSpelling()
      {
         try
         {
            SetIniSetting("AccountLockoutThreshold", "3");
            _application.Reinitialize();

            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("NO", ImapLogin(_account.Address, "wrong" + i));

            string locked = ImapLogin(_account.Address, Password);
            StringAssert.StartsWith("NO", locked, "The locked name refuses the right password.");

            foreach (string variant in Variants(_account.Address))
            {
               ClassicAssert.AreEqual(locked, ImapLogin(variant, Password),
                  "The spelling '" + Escape(variant) + "' must not sign in to a locked name with its right password.");
            }
         }
         finally
         {
            SetIniSetting("AccountLockoutThreshold", null);
            _application.Reinitialize();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      /// <summary>
      ///    IMAP LOGIN with the name and the password as quoted strings, each character
      ///    sent as the one byte of its code (so below U+0100), on a connection of its
      ///    own; the tagged reply without its tag.
      /// </summary>
      public static string ImapLogin(string user, string password)
      {
         using (var client = new TcpClient(TestPorts.HostAddress.AddressFamily))
         {
            client.ReceiveTimeout = 60000;
            client.SendTimeout = 60000;
            client.Connect(TestPorts.HostAddress, TestPorts.Actual(143));
            NetworkStream stream = client.GetStream();
            ReadLine(stream);

            Write(stream, "A1 LOGIN \"" + user + "\" \"" + password + "\"\r\n");

            string line;
            do
            {
               line = ReadLine(stream);
               if (line == null)
                  throw new InvalidOperationException("The connection closed before the LOGIN was answered.");
            } while (!line.StartsWith("A1 ", StringComparison.Ordinal));

            try
            {
               Write(stream, "A2 LOGOUT\r\n");
            }
            catch (Exception)
            {
               // The server may have closed the connection after the reply.
            }

            return line.Substring(3);
         }
      }

      private static void Write(NetworkStream stream, string text)
      {
         var bytes = new byte[text.Length];
         for (int i = 0; i < text.Length; i++)
         {
            if (text[i] > 0xFF)
               throw new ArgumentException("A character past U+00FF has no one-byte form.");
            bytes[i] = (byte) text[i];
         }

         stream.Write(bytes, 0, bytes.Length);
      }

      private static string ReadLine(NetworkStream stream)
      {
         var chars = new System.Text.StringBuilder();
         int b;
         while ((b = stream.ReadByte()) >= 0)
         {
            if (b == '\n')
               return chars.ToString();
            if (b != '\r')
               chars.Append((char) b);
         }

         return chars.Length > 0 ? chars.ToString() : null;
      }

      private static string Escape(string text)
      {
         var escaped = new System.Text.StringBuilder();
         foreach (char c in text)
         {
            if (c >= 0x20 && c < 0x7F)
               escaped.Append(c);
            else
               escaped.AppendFormat("<U+{0:X4}>", (int) c);
         }

         return escaped.ToString();
      }

      // Stores the setting, or returns it to its default when value is null, as
      // AccountLockoutTests does: the settings store in the database, applied by
      // the Reinitialize after each call.
      private static void SetIniSetting(string key, string value)
      {
         if (value == null)
            IniFileSetting.Delete(key);
         else
            IniFileSetting.Write(key, value);
      }
   }
}
