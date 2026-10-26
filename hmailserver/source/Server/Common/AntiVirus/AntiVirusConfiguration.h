// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class SURBLServers;
   class GreyListingWhiteAddresses;
   class WhiteListAddresses;
   class DNSBlackLists;

   class AntiVirusConfiguration
   {
   public:
      enum eAVAction
      {
         ActionDelete = 0,
         ActionStripAttachments = 1
      };

      AntiVirusConfiguration();
      virtual ~AntiVirusConfiguration();

      bool ClamWinEnabled() const;
      void ClamWinEnabled(bool Value);
      String ClamWinExecutable() const;
      void ClamWinExecutable(const String & Value);
      String ClamWinDatabase() const;
      void ClamWinDatabase(const String & Value);
      
      bool GetCustomScannerEnabled() const;
      void SetCustomScannerEnabled(bool Value);
      String GetCustomScannerExecutable() const;
      void SetCustomScannerExecutable(const String & Value);
      long GetCustomScannerReturnValue() const;
      void SetCustomScannerReturnValue(long Value);

      int GetVirusScanMaxSize() const;
      void SetVirusScanMaxSize(int iNewVal);

      bool GetEnableAttachmentBlocking() const;
      void SetEnableAttachmentBlocking(bool iNewVal);

      eAVAction AVAction() const;
      void AVAction(eAVAction Value);
      bool AVNotifySender();
      void AVNotifySender(bool Value);
      bool AVNotifyReceiver();
      void AVNotifyReceiver(bool Value);

      bool GetClamAVEnabled();
      void SetClamAVEnabled(bool bNewVal);

      String GetClamAVHost();
      void SetClamAVHost(const String &sNewVal);

      int GetClamAVPort();
      void SetClamAVPort(int bNewVal);

      // ClamAVHost names clamd's host, reached over TCP on ClamAVPort, or, on Linux
      // and the other POSIX systems, the absolute path of clamd's Unix socket
      // (LocalSocket in clamd.conf), when ClamAVPort is not used. No host name starts
      // with a slash, so that is what tells the two apart.
      static bool IsClamAVSocketPath(const String &host);

      // Why ClamAVHost cannot be host on this server, or nothing when it can. A socket
      // path is refused on Windows, where clamd listens on TCP and this server reaches
      // no Unix socket, and elsewhere when it is longer than a socket address holds.
      // The COM and REST setters refuse with it, and the scanner and its connection
      // test report it for a value that reached the database some other way.
      static String ClamAVHostRefusal(const String &host);


   private:
      
      std::shared_ptr<PropertySet> GetSettings_() const;

   };
}
