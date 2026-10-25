// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class MimeHeader;
   class DateTime;
   class CipherInfo;

   class Utilities  
   {
   public:
      Utilities();
      virtual ~Utilities();

      static String ComputerName();
      static String GetBinDirectory();
      static String GetUniqueTempDirectory();
      static String GetIPAddress (SOCKADDR_IN addr);
      static String GenerateMessageID();


      static std::shared_ptr<MimeHeader> GetMimeHeader(const BYTE *pByteBuf, size_t iBufSize);

      static bool IsLocalHost(const String &sHostname);


      static String GetHostNameFromReceivedHeader(const String &sReceivedHeader);
      static String GetRecipientFromReceivedHeader(const String &sReceivedHeader);
      static DateTime GetDateTimeFromReceivedHeader(const String &sReceivedHeader);
      static IPAddress GetIPAddressFromReceivedHeader(const String &sReceivedHeader);

      bool IsValidIPAddress(const AnsiString string);

      // Overwrites a string's characters and then empties it, for a string that
      // held a secret.
      //
      // Empty() alone sets the length to zero and leaves the characters where
      // they were, in a buffer that is then freed - and this build writes
      // minidumps on a crash by design, so "freed" is not "gone". The backup
      // passphrase is the reason this exists: it is write-only at every
      // interface, which means the one read nobody plans for is a process dump,
      // and the passphrase opens every archive this server has ever written.
      //
      // SecureZeroMemory (explicit_bzero on POSIX) rather than memset, because a
      // compiler is entitled to delete a memset over storage nothing reads
      // afterwards, which is exactly this case.
      //
      // What it does NOT claim: that the secret is gone from the process. A
      // std::wstring that has been copied has been copied, and this reaches only
      // the buffer it is given. It is worth doing where a copy is long-lived or
      // is one of several - see ProcessLauncher::Launch, which holds three copies
      // of the passphrase in the bytes it gives 7-Zip.
      static void SecureWipe(String &value);
      static void SecureWipe(AnsiString &value);

   private:

      

      static String cached_win_32computer_name_;

      static String cached_win_32temp_dir_;
   };

   class UtilitiesTester
   {
   public:
      UtilitiesTester() {};
      void Test();

   private:
      void TestComputerName_();
      void TestReceivedHeaderParse_();
      void TestHeloDependentReceivedHeaderParse_();
      void TestMalformedReceivedHeaderParse_();

      void AssertHostName_(const String &receivedHeader, const String &expectedHostName);
      void AssertIPAddress_(const String &receivedHeader, const String &expectedIPAddress);
   };

}
