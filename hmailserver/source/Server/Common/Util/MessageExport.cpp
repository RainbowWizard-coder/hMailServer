// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// See MessageExport.h. This lived inside COM/InterfaceAccount.cpp until the
// REST API needed the same export, and two copies of a walk that must not
// drop a folder is one too many.
#include "StdAfx.h"
#include "MessageExport.h"

#include "FileUtilities.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../BO/Messages.h"
#include "../Persistence/PersistentMessage.h"

#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A folder name is IMAP data, not a path: it can contain every character
      // Windows refuses in a directory name, and all of them map to '_'. That
      // makes the mapping lossy - "A:B" and "A|B" both become "A_B" - so the
      // CALLER must disambiguate a collision between siblings; this function's
      // only jobs are to produce a legal name and to refuse a traversal.
      String SanitizeFolderNameForExport(const String &name)
      {
         String result = name;

         const TCHAR *forbidden = _T("\\/:*?\"<>|");
         for (const TCHAR *c = forbidden; *c != 0; ++c)
            result.Replace(*c, _T('_'));

         result.Trim();

         // A folder named "." or ".." would climb out of the export directory:
         // the separators above are neutralised, but the dots are not, so
         // directory + separator + ".." resolves to the parent. IsValidFolderName
         // accepts such a name, so it can genuinely arrive here. A legitimate
         // folder may contain dots ("2024.Archive"), so only a segment that is
         // ENTIRELY dots is replaced - that is the whole traversal set and
         // nothing a real folder is called.
         bool allDots = !result.IsEmpty();
         for (int i = 0; i < result.GetLength(); i++)
         {
            if (result[i] != _T('.'))
            {
               allDots = false;
               break;
            }
         }

         if (result.IsEmpty() || allDots)
            result = _T("_");

         return result;
      }

      // Walks the folder tree depth-first, copying every message file into a
      // directory mirroring the folder structure. Stops on the FIRST failure and
      // says which file, because a partial export that reports success is exactly
      // the wrong artefact to hand to a person exercising a data-access right.
      bool ExportFolderTree(std::shared_ptr<Account> account,
                            std::shared_ptr<IMAPFolders> folders,
                            const String &directory,
                            int &exported,
                            String &error)
      {
         if (!folders)
            return true;

         // No Refresh() here, and the absence is load-bearing: IMAPFolders::Refresh
         // loads the account's ENTIRE tree into whichever collection it is called
         // on - it is the top-level collection's loader, and it populates the
         // sub-folder collections itself as it builds the hierarchy. Calling it on
         // a sub-folder collection turns that collection into a second copy of the
         // whole tree, and this walk into infinite descent. The one Refresh this
         // export needs is done by ExportAccount, on the root, before the walk.

         // Sibling folders can sanitise to the same directory name ("A:B" and
         // "A|B" both become "A_B"), and their per-folder UIDs repeat, so without
         // this two folders would write into one directory and the second would
         // overwrite the first - a data-access export that silently drops mail
         // while the count says otherwise. Names taken at THIS level are tracked
         // (case-insensitively, because the file system is), and a collision gets
         // a " (2)", " (3)" suffix, the way a file manager resolves the same clash.
         std::set<String> usedNames;

         for (unsigned int i = 0; i < (unsigned int) folders->GetCount(); i++)
         {
            std::shared_ptr<IMAPFolder> folder = folders->GetItem(i);
            if (!folder)
               continue;

            String folderName = SanitizeFolderNameForExport(folder->GetFolderName());
            String lowered = folderName;
            lowered.ToLower();

            if (usedNames.find(lowered) != usedNames.end())
            {
               for (int suffix = 2; ; suffix++)
               {
                  String candidate;
                  candidate.Format(_T("%s (%d)"), folderName.c_str(), suffix);
                  String candidateLower = candidate;
                  candidateLower.ToLower();
                  if (usedNames.find(candidateLower) == usedNames.end())
                  {
                     folderName = candidate;
                     lowered = candidateLower;
                     break;
                  }
               }
            }

            usedNames.insert(lowered);

            const String folderDirectory = directory + FileUtilities::PathSeparator + folderName;

            std::shared_ptr<Messages> messages = folder->GetMessages();
            if (messages)
            {
               messages->Refresh(false);

               for (unsigned int j = 0; j < (unsigned int) messages->GetCount(); j++)
               {
                  std::shared_ptr<Message> message = messages->GetItem(j);
                  if (!message)
                     continue;

                  const String source = PersistentMessage::GetFileName(account, message);

                  String target;
                  target.Format(_T("%s%s%u.eml"), folderDirectory.c_str(), FileUtilities::PathSeparator.c_str(), message->GetUID());

                  // The copy creates the folder directory on first use, so an
                  // empty folder produces no empty directory - the export mirrors
                  // the mail, not the folder skeleton.
                  if (!FileUtilities::Copy(source, target, true))
                  {
                     error.Format(_T("The message file %s could not be copied to %s. The export is incomplete and should be re-run."),
                        source.c_str(), target.c_str());
                     return false;
                  }

                  exported++;
               }
            }

            if (!ExportFolderTree(account, folder->GetSubFolders(), folderDirectory, exported, error))
               return false;
         }

         return true;
      }
   }

   bool
   MessageExport::ExportAccount(std::shared_ptr<Account> account, const String &directory, int &exported, String &error)
   {
      exported = 0;
      if (!account)
      {
         error = _T("No account.");
         return false;
      }

      std::shared_ptr<IMAPFolders> rootFolders = account->GetFolders();
      if (rootFolders)
         rootFolders->Refresh();

      return ExportFolderTree(account, rootFolders, directory, exported, error);
   }
}
