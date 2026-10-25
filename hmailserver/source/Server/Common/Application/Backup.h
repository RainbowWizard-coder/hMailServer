// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class Backup
   {
   public:
      Backup(void);
      ~Backup(void);

      enum BackupOptions
      {
         BOSettings = 1,
         BODomains = 2,
         BOMessages = 4,
         BOCompression = 8,
      };      

      void SetBackupFile(const String &sBackupFile) {backup_file_ = sBackupFile; }
      String GetBackupFile() {return backup_file_; }

      // The passphrase this archive is encrypted with, as whoever asked for the
      // restore gave it. Held for the life of this object and nowhere else: it is
      // never stored, never logged, and never part of the archive. Empty for an
      // archive that is not encrypted.
      void SetPassphrase(const String &passphrase) {passphrase_ = passphrase; }
      String GetPassphrase() const {return passphrase_; }

      void SetContains(int iContains) {contains_ = iContains;}
      int GetContains() const {return contains_; }

      bool GetContains(int iOption) const;

      void SetRestoreOptions(int iRestoreOptions) {restore_options_ = iRestoreOptions; }
      int GetRestoreOptions() const {return restore_options_; }
      
      int GetRestoreOption(int iRestoreOption) const;
      void SetRestoreOption(int iOption, bool bSet);
      
   private:

      int contains_;
      // Contains information on what parts that are backuped.
      
      int restore_options_;
      // Contains the properties that should be restored when running a restore
      // For example, it may be a combination of settings, domains and compression.

      String backup_file_;
      // The path to the XML backup file.

      String passphrase_;
      // The passphrase the archive is encrypted with, or empty.
   };
}