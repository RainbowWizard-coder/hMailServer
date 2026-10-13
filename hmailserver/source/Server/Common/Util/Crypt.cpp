// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "Crypt.h"
#include "LegacyBlowfish.h"
#include "DataProtector.h"
#include "MailboxCrypto.h"
#include "Unicode.h"
#include "Hashing/HashCreator.h"
#include "../Application/IniFileSettings.h"

#include <algorithm>
#include <atomic>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The bytes a password is hashed over: its UTF-8, every character of it.
      // It was AnsiString(String), which on Windows stops at the first character
      // above U+00FF and makes it and every byte after it NUL, and on Linux makes
      // each such character '?': so a password of Cyrillic, Greek or CJK letters
      // was stored as the hash of NUL bytes or question marks of its length, and
      // any other password of that form opened the account. For ASCII the bytes
      // are the ones the old conversion gave, so every ASCII password's hash is
      // unchanged. A password that arrived as its UTF-8 widened byte by byte (an
      // IMAP LOGIN, a REST Basic credential) is those bytes, which the old
      // conversion hashed too (Unicode::ToUtf8Canonical), so it and the same
      // password decoded - SASL PLAIN, COM - are one password. A lone surrogate
      // half, which UTF-8 cannot carry, is U+FFFD.
      AnsiString PasswordBytes(const String &password)
      {
         return Unicode::ToUtf8(password);
      }

      // The password's characters as bytes, when each is below U+0100, one is
      // above U+007F, and the bytes are not UTF-8 (when they are, PasswordBytes
      // is those bytes already): Latin-1 text, which the old conversion kept
      // exactly, so a hash stored before 6.3.4 over "passw\xF6rd" still opens.
      // False otherwise - for ASCII the bytes are PasswordBytes' own, and a
      // character above U+00FF was never kept.
      bool CharactersAsBytes(const String &password, AnsiString &bytes)
      {
         if (Unicode::IsWidenedUtf8(password))
            return false;

         bool high = false;
         const int length = password.GetLength();
         for (int i = 0; i < length; i++)
         {
            const unsigned int unit = (unsigned int) password[i];
            if (unit > 0xFF)
               return false;
            if (unit > 0x7F)
               high = true;
         }

         if (!high)
            return false;

         bytes.resize(length);
         for (int i = 0; i < length; i++)
            bytes[i] = (char) (unsigned char) password[i];

         return true;
      }

      bool HasCharacterAboveLatin1(const String &password)
      {
         for (int i = 0; i < password.GetLength(); i++)
         {
            if ((unsigned int) password[i] > 0xFF)
               return true;
         }

         return false;
      }

      void Wipe(AnsiString &bytes)
      {
         std::fill(bytes.begin(), bytes.end(), '\0');
      }

      // DeCrypt's ETDPAPI arm: the secret this machine's store - DPAPI, or the
      // Linux key file - opens, made a String. Every return here returns the
      // one String, so that an optimising compiler builds it where the caller
      // receives it, as GCC, clang and MSVC do. The string class has no move,
      // and until 6.3.7 the arm returned its result beside DeCrypt's other
      // returns, which GCC 15 and MSVC 19.51 at /O2 answer with a copy -
      // measured - and the result the copy was made from went back to the heap
      // as it stood. Among what comes here is the stored-secrets key's line, the
      // key in hex, opened at every start and by the key verbs, which write over
      // every other copy they make of it.
      String OpenWithMachineStore(const String &sInput)
      {
         String opened;
         if (sInput.IsEmpty())
            return opened;

         AnsiString protectedBase64 = sInput;
         AnsiString utf8;
         MailboxCrypto::ForgetOnExit forgetUtf8(utf8);
         if (DataProtector::Unprotect(protectedBase64, utf8))
            Unicode::MultiByteToWide(utf8, opened);
         MailboxCrypto::Forget(utf8);
         return opened;
      }
   }

   Crypt::Crypt()
   {
   }

   Crypt::~Crypt()
   {
   }

   String
   Crypt::EnCrypt(const String &sInput,EncryptionType iType) const
   {
      switch (iType)
      {
      case ETNone:
         return sInput;
      case ETBlowFish:
         {
            if (sInput.IsEmpty())
               return "";

            return legacy_blowfish_.EncryptToString(sInput);
         }
      case ETMD5:
         {
            HashCreator crypter(HashCreator::MD5);
            AnsiString input = PasswordBytes(sInput);
            String sResult = crypter.GenerateHashNoSalt(input, HashCreator::hex);
            Wipe(input);
            return sResult;
         }
      case ETSHA256:
         {
            HashCreator encrypter(HashCreator::SHA256);
            AnsiString input = PasswordBytes(sInput);
            AnsiString result = encrypter.GenerateHash(input, "");
            Wipe(input);
            return result;
         }
      case ETPBKDF2:
         {
            // UTF-8 is also what a SCRAM client derives its SaltedPassword from
            // (RFC 5802), so a password outside ASCII now serves SCRAM too.
            AnsiString input = PasswordBytes(sInput);
            AnsiString result = HashCreator::GeneratePBKDF2(input);
            Wipe(input);
            return result;
         }
      case ETArgon2id:
         {
            AnsiString bytes = PasswordBytes(sInput);
            AnsiString input = ApplyPepper_(bytes);
            AnsiString result = HashCreator::GenerateArgon2id(input);
            Wipe(bytes);
            Wipe(input);
            return result;
         }
      case ETScrypt:
         {
            // Peppered like Argon2id: neither scheme can serve SCRAM, so there is no
            // SaltedPassword to keep unpeppered.
            AnsiString bytes = PasswordBytes(sInput);
            AnsiString input = ApplyPepper_(bytes);
            AnsiString result = HashCreator::GenerateScrypt(input);
            Wipe(bytes);
            Wipe(input);
            return result;
         }
      case ETDPAPI:
         {
            if (sInput.IsEmpty())
               return "";

            // The narrow copy of the secret - the stored-secrets key's line among
            // the secrets that come here - written over before it goes back to
            // the heap, as DataProtector wipes its own, however this ends.
            AnsiString utf8;
            MailboxCrypto::ForgetOnExit forgetUtf8(utf8);
            Unicode::WideToMultiByte(sInput, utf8);

            AnsiString protectedBase64;
            const bool protectedIt = DataProtector::Protect(utf8, protectedBase64);
            MailboxCrypto::Forget(utf8);

            if (!protectedIt)
               return "";

            return protectedBase64;
         }
      default:
         {
            HM_ASSERT(0);
         }
      }

      return "";

   }

   bool
   Crypt::Validate(const String &password, const String &originalHash, EncryptionType iType) const
   {
      return Validate(password, originalHash, iType, nullptr);
   }

   bool
   Crypt::Validate(const String &password, const String &originalHash, EncryptionType iType, PasswordMatch *match) const
   {
      if (match != nullptr)
      {
         match->text.Empty();
         match->refused_old_conversion = false;
         match->legacy_bytes = false;
      }

      if (iType != ETMD5 && iType != ETSHA256 && iType != ETPBKDF2 && iType != ETArgon2id && iType != ETScrypt)
      {
         HM_ASSERT(0);
         return false;
      }

      // No password holds U+0000, and a stored hash the old conversion made of a
      // password with a character above U+00FF is, on Windows, the hash of that
      // password's first characters and NUL bytes: a password sent as those NUL
      // bytes - a REST Basic credential can carry them - would open it.
      if (password.Find((wchar_t) 0) >= 0)
         return false;

      // Stored hashes are ASCII: hex, and the $-separated forms.
      const AnsiString hash = originalHash;

      // The password as text: decoded, when it arrived as its UTF-8 widened one
      // byte to a character (IMAP LOGIN, REST Basic), which PasswordBytes makes
      // the same bytes as the text. It is what a re-hash is made from, and what
      // the two checks of the old conversion below are made of - a password set
      // before 6.3.4 was set as text (COM, a REST body, the Control Panel).
      String decoded;
      const String text = Unicode::IsWidenedUtf8(password, nullptr, &decoded) ? decoded : password;

      AnsiString utf8 = PasswordBytes(password);
      bool oldPepper = false;
      bool matched = ValidateBytes_(utf8, hash, iType, &oldPepper);
      Wipe(utf8);

      if (matched)
      {
         if (match != nullptr)
         {
            match->text = text;
            match->legacy_bytes = oldPepper;
         }
         return true;
      }

      // Latin-1 text hashed before 6.3.4, as its characters' bytes; for one that
      // arrived as UTF-8 widened, those of the text it decodes to, so that an
      // IMAP LOGIN of "passw\xF6rd" from a client that sends UTF-8 opens the hash
      // COM made of it. Re-hashed by the caller (legacy_bytes), which moves it
      // to UTF-8.
      AnsiString asBytes;
      if (CharactersAsBytes(text, asBytes))
      {
         matched = ValidateBytes_(asBytes, hash, iType);
         Wipe(asBytes);

         if (matched)
         {
            if (match != nullptr)
            {
               match->text = text;
               match->legacy_bytes = true;
            }
            return true;
         }
      }

      // Not a match. Said, when it is so, that the stored hash is the one the old
      // conversion made of this very password - which is also the hash of every
      // other password of that form, so it is not accepted. Checked on the text,
      // so that a client sending the password as UTF-8 (IMAP LOGIN) is told
      // about as a SASL PLAIN one is.
      if (match != nullptr && HasCharacterAboveLatin1(text))
      {
         AnsiString old = text;
         match->refused_old_conversion = ValidateBytes_(old, hash, iType);
         Wipe(old);
      }

      return false;
   }

   bool
   Crypt::ValidateBytes_(const AnsiString &password, const AnsiString &originalHash, EncryptionType iType, bool *oldPepper) const
   {
      if (oldPepper != nullptr)
         *oldPepper = false;

      switch (iType)
      {
      case ETMD5:
         {
            // Salts are not used for the MD5 hashes.
            HashCreator encrypter(HashCreator::MD5);
            return encrypter.ValidateHash(password, originalHash, false);
         }
      case ETSHA256:
         {
            // Salts are always used for the SHA256 hashes.
            HashCreator encrypter(HashCreator::SHA256);
            return encrypter.ValidateHash(password, originalHash, true);
         }
      case ETPBKDF2:
         return HashCreator::ValidatePBKDF2(password, originalHash);
      case ETArgon2id:
      case ETScrypt:
         {
            // The pepper as UTF-8, and - for a pepper outside ASCII - as the old
            // conversion keyed it, so that a hash made before 6.3.4 still opens.
            // The old key is weaker (on Windows a Cyrillic pepper was NUL bytes),
            // but it protects only the stored hash; the password is checked
            // exactly either way.
            const String pepper = IniFileSettings::Instance()->GetPasswordPepper();
            const bool pepperOutsideAscii = StringParser::ContainsNonAscii(pepper);

            for (int attempt = 0; attempt < (pepperOutsideAscii ? 2 : 1); attempt++)
            {
               AnsiString peppered = ApplyPepper_(password, attempt == 1);
               const bool ok = iType == ETArgon2id ? HashCreator::ValidateArgon2id(peppered, originalHash)
                                                   : HashCreator::ValidateScrypt(peppered, originalHash);
               Wipe(peppered);
               if (ok)
               {
                  if (oldPepper != nullptr)
                     *oldPepper = attempt == 1;
                  return true;
               }
            }

            return false;
         }
      default:
         return false;
      }
   }

   int
   Crypt::StrengthRank(int encryptionType)
   {
      switch (encryptionType)
      {
      case ETBlowFish:
         return 1;
      case ETMD5:
         return 2;
      case ETSHA256:
         return 3;
      case ETPBKDF2:
         return 4;
      case ETArgon2id:
      case ETScrypt:
         return 5;
      default:
         // ETNone, ETDPAPI (stored secrets, not passwords) and anything unknown.
         return 0;
      }
   }

   Crypt::EncryptionType
   Crypt::GetHashType(const String &hash)
   {
      AnsiString ansiHash = hash;
      if (HashCreator::IsPBKDF2Hash(ansiHash))
         return ETPBKDF2;

      if (HashCreator::IsArgon2idHash(ansiHash))
         return ETArgon2id;
      if (HashCreator::IsScryptHash(ansiHash))
         return ETScrypt;

      int length = hash.GetLength();
      if (length == 32)
         return ETMD5;
      else if (length == 70)
         return ETSHA256;
      else
         return ETNone;
   }

   String
   Crypt::DeCrypt(const String &sInput, EncryptionType iType) const
   {
      switch (iType)
      {
         case ETNone:
            return sInput;
         case ETBlowFish:
            {
               if (sInput.IsEmpty())
                  return "";

               return legacy_blowfish_.DecryptFromString(sInput);
            }
            break;
         case ETDPAPI:
            // Returned as it is made, never copied: OpenWithMachineStore.
            return OpenWithMachineStore(sInput);
         default:
            HM_ASSERT(0);
      }
      
      return "";
   }

   String
   Crypt::ProtectSecret(const String &sInput) const
   {
      if (sInput.IsEmpty())
         return "";

      // When protection is enabled (the default) store a self-describing envelope
      // whose prefix names the platform store that wrote it, so that a value
      // carried to the other platform is recognised rather than fed to the wrong
      // cipher. ETDPAPI is the storage identifier for "the platform's secret
      // store" on both: DPAPI itself on Windows, the key file on Linux.
      if (IniFileSettings::Instance()->GetProtectStoredSecretsWithDPAPI())
      {
         String protectedValue = EnCrypt(sInput, ETDPAPI);
#ifdef HM_PLATFORM_POSIX
         if (!protectedValue.IsEmpty())
            return _T("LINUX1:") + protectedValue;

         // No Blowfish here. Its key is a constant in the source, so a value it
         // writes is protected from nobody who has the binary, and a server that
         // wrote one because the real store was unavailable would have downgraded
         // a secret without anyone choosing that. DataProtector has already said
         // why the store was unavailable and what to do; this says what it cost.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6416, "Crypt::ProtectSecret",
            "A secret was NOT stored: the stored-secret key file was unavailable (the error before this one "
            "says why), and this build does not fall back to the legacy Blowfish scheme. Fix the key file and "
            "enter the secret again.");
         return "";
#else
         // If DPAPI fails for any reason fall back to Blowfish so a secret is
         // never lost.
         if (!protectedValue.IsEmpty())
            return _T("DPAPI:") + protectedValue;
#endif
      }

      return EnCrypt(sInput, ETBlowFish);
   }

   String
   Crypt::UnprotectSecret(const String &sStored) const
   {
      if (sStored.IsEmpty())
         return "";

      // An envelope is self-describing; everything else is a legacy Blowfish
      // value, so existing stored secrets keep working transparently. An envelope
      // from the other platform is the one thing neither store can open, and it
      // is reported by name - once, since a moved database holds several - and
      // answered with an empty secret, which every caller treats as "re-enter".
      if (sStored.Left(6) == _T("DPAPI:"))
      {
#ifdef HM_PLATFORM_POSIX
         ReportForeignEnvelopeOnce_(6414,
            "A stored secret is a Windows DPAPI envelope, which only the Windows machine that wrote it can open: "
            "the database or the INI was carried over from a Windows installation. The secret is intact there and "
            "unreadable here; re-enter it on this machine and it is stored under this machine's key file. "
            "Reported once; there may be more than one such value.");
         return "";
#else
         return DeCrypt(sStored.Mid(6), ETDPAPI);
#endif
      }

      if (sStored.Left(7) == _T("LINUX1:"))
      {
#ifdef HM_PLATFORM_POSIX
         return DeCrypt(sStored.Mid(7), ETDPAPI);
#else
         ReportForeignEnvelopeOnce_(6415,
            "A stored secret is a LINUX1 envelope, written by a Linux installation under its key file, which "
            "DPAPI cannot open: the database was carried over from a Linux installation. The secret is intact "
            "there and unreadable here; re-enter it on this machine and it is stored with DPAPI. "
            "Reported once; there may be more than one such value.");
         return "";
#endif
      }

      return DeCrypt(sStored, ETBlowFish);
   }

   bool
   Crypt::IsSecretEnvelope(const String &stored)
   {
      return stored.Left(6) == _T("DPAPI:") || stored.Left(7) == _T("LINUX1:");
   }

   void
   Crypt::ReportForeignEnvelopeOnce_(int errorId, const String &message)
   {
      static std::atomic<bool> reported(false);

      bool expected = false;
      if (reported.compare_exchange_strong(expected, true))
         ErrorManager::Instance()->ReportError(ErrorManager::High, errorId, "Crypt::UnprotectSecret", message);
   }

   AnsiString
   Crypt::ApplyPepper_(const AnsiString &password, bool oldConversion) const
   {
      // The pepper is a server-wide secret that is never stored alongside the hash.
      // When configured, the password is HMAC-SHA256'd under the pepper before it is
      // fed to Argon2id, so a stolen hash database cannot be attacked without also
      // obtaining the pepper. An empty pepper preserves the previous behaviour.
      //
      // This is applied to Argon2id only: PBKDF2 hashes double as the SCRAM
      // SaltedPassword, which the client reconstructs from the raw password, so they
      // must remain un-peppered to keep SCRAM working.
      String pepper = IniFileSettings::Instance()->GetPasswordPepper();
      if (pepper.IsEmpty())
         return password;

      AnsiString ansiPepper = oldConversion ? AnsiString(pepper) : Unicode::ToUtf8(pepper);
      AnsiString peppered = HashCreator::ComputeHMACSHA256Hex(ansiPepper, password);
      Wipe(ansiPepper);
      return peppered;
   }
}
