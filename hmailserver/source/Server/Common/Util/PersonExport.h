// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Account;

   // Everything the server holds on one person, as one archive - what article
   // 20 asks for and what a person is handed, rather than a directory of
   // files on the server's disk (MessageExport, which stays for the operator
   // who wants exactly that). Inside the .zip:
   //
   //   manifest.json            what is here, counted
   //   mail/<folder path>.mbox  every folder with mail, one mbox each, From_ lines
   //   account.xml              what the backup carries for the account: the
   //                            settings, rules, fetch accounts, app passwords,
   //                            sender list, the address book, the calendars,
   //                            the webmail's preferences, the S/MIME keys, the
   //                            files sent as links, scheduled mail
   //   contacts.vcf             the address book as vCard 3.0
   //   calendars/<name>/<uri>   every calendar object as the client sent it
   //   preferences.json         the webmail's preferences as they are stored
   //   filters.sieve            the active Sieve script
   //   logs.txt                 the log lines that name the address
   class PersonExport
   {
   public:

      struct Summary
      {
         Summary() : messages(0), folders(0), contacts(0), calendarObjects(0), logLines(0) { }

         int messages;
         int folders;
         int contacts;
         int calendarObjects;
         int logLines;
      };

      // Writes the archive to zipPath. Stops on the first failure and says
      // why, because a partial export that reports success is exactly the
      // wrong artefact to hand to a person exercising a right.
      static bool ExportArchive(std::shared_ptr<Account> account, const String &zipPath, Summary &summary, String &error);

      // One message onto an mbox: a From_ line, the message, body lines that
      // begin "From " quoted, a blank line after. Shared with the folder
      // download of the self-service API.
      static void AppendMbox(AnsiString &mbox, const AnsiString &message, const AnsiString &from, const AnsiString &date);

      // The log lines that name the address, from the server's own log files,
      // up to cap bytes; the count of lines is returned.
      static int LogLinesNaming(const String &address, size_t cap, AnsiString &lines);
   };
}
