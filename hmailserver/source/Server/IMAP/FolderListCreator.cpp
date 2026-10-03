// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "FolderListCreator.h"

#include <algorithm>

#include "../Common/Application/ACLManager.h"

#include "../Common/BO/ACLPermission.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/IMAPFolders.h"

#include "../IMAP/IMAPConfiguration.h"
#include "../IMAP/IMAPSpecialUse.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   FolderListCreator::FolderListCreator()
   {

   }

   FolderListCreator::~FolderListCreator()
   {

   }

   String
   FolderListCreator::GetIMAPFolderList(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, bool *pAnyFolderVisible)
   {
      ListRequest_ request;
      request.emit_as_list_ = true;

      return Create_(iAccountID, pStartFolders, sWildcard, sPrefix, request, pAnyFolderVisible);
   }

   String
   FolderListCreator::GetIMAPLSUBFolderList(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix)
   {
      ListRequest_ request;
      request.emit_as_list_ = false;
      request.only_subscribed_ = true;

      return Create_(iAccountID, pStartFolders, sWildcard, sPrefix, request, nullptr);
   }

   String
   FolderListCreator::GetIMAPFolderListExtended(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, bool bOnlySubscribed, bool bAnnotateSubscribed, bool bOnlySpecialUse, bool *pAnyFolderVisible, const std::vector<String> *pRecursiveMatchPatterns)
   {
      ListRequest_ request;
      request.emit_as_list_ = true;
      request.only_subscribed_ = bOnlySubscribed;
      request.annotate_subscribed_ = bAnnotateSubscribed;
      request.only_special_use_ = bOnlySpecialUse;
      request.recursive_match_patterns_ = bOnlySubscribed ? pRecursiveMatchPatterns : nullptr;

      return Create_(iAccountID, pStartFolders, sWildcard, sPrefix, request, pAnyFolderVisible);
   }

   String
   FolderListCreator::CreateNamespaceRootLine(const String &sRootPath, const String &sWildcard)
   {
      String hierarchyDelimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      if (!FolderWildcardMatch_(sRootPath, sWildcard, hierarchyDelimiter))
         return _T("");

      // The same dummy-folder emission the listing walk uses for a namespace
      // root it reached through a prefix ("#Public", "#Users.owner@domain").
      ListRequest_ request;
      request.emit_as_list_ = true;

      std::shared_ptr<IMAPFolder> pFolderDummy;
      String sPath = sRootPath;

      String sLine = CreateFolderLine_(pFolderDummy, true, sPath, sWildcard, false, hierarchyDelimiter, request);
      if (sLine.IsEmpty())
         return _T("");

      return sLine + _T("\r\n");
   }

   void
   FolderListCreator::TakeNewLines(const String &listing, std::unordered_set<std::wstring> &seen, std::vector<String> &fresh)
   {
      for (const String &line : StringParser::SplitString(listing, _T("\r\n")))
      {
         if (line.IsEmpty())
            continue;

         if (seen.insert(line).second)
            fresh.push_back(line);
      }
   }

   String
   FolderListCreator::Create_(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, ListRequest_ &request, bool *pAnyFolderVisible)
   {
      if (pAnyFolderVisible)
         *pAnyFolderVisible = false;

      // RFC 6154: work out which folder owns which special-use attribute once, before
      // the walk, rather than deciding per folder inside it.
      //
      // It has to be done up front because the rule is a global one - an attribute may
      // be handed out at most once per mailbox - and a walk that emits each line as it
      // reaches it cannot know whether a later folder has a better claim. The cost is
      // one extra pass over the (in-memory, already cached) folder tree per pattern;
      // the walk itself was already a pass over the same tree, so this doubles a cost
      // measured in microseconds for a realistic mailbox.
      //
      // The public folder collection is identified by account id zero and gets no
      // designations at all, deliberately: see ListRequest_::special_use_. A tree
      // being listed on behalf of an account that does not own it - a shared
      // mailbox under "#Users" - gets none either, for the same reason: the
      // owner's \Trash advertised to a delegate is an invitation for the
      // delegate's client to expunge the owner's mail.
      if (pStartFolders && pStartFolders->GetAccountID() != 0 && pStartFolders->GetAccountID() == iAccountID)
         IMAPSpecialUse::Resolve(pStartFolders, request.special_use_);

      std::vector<String> vecCurrentFolder;
      std::vector<String> vecMatchingFolders;

      CreateIMAPFolderList_(iAccountID, pStartFolders, sWildcard, sPrefix, vecCurrentFolder, vecMatchingFolders, request, pAnyFolderVisible);

      String sRet = StringParser::JoinVector(vecMatchingFolders, "\r\n");

      if (!sRet.IsEmpty())
         sRet += "\r\n";

      return sRet;
   }

   void
   FolderListCreator::CreateIMAPFolderList_(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, std::vector<String> &vecCurrentFolder, std::vector<String> &vecMatchingFolders, const ListRequest_ &request, bool *pAnyFolderVisible)
   {
      if (vecCurrentFolder.size() > IMAPFolder::MaxFolderDepth)
         return;

      String hierarchyDelimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      bool anyFolderVisible = false;

      for(std::shared_ptr<IMAPFolder> currentFolder : pStartFolders->GetSnapshot())
      {
         // Check if the user has access to this folder. Otherwise just skip it.
         if (!IsFolderVisible_(iAccountID, currentFolder))
         {
            continue;
         }

         // at least one folder at this level is visible to the user.
         anyFolderVisible = true;

         vecCurrentFolder.push_back(currentFolder->GetFolderName());

         String sFullPath = StringParser::JoinVector(vecCurrentFolder, hierarchyDelimiter);

         if (!sPrefix.IsEmpty())
            sFullPath = sPrefix + hierarchyDelimiter + sFullPath;

         std::shared_ptr<IMAPFolders> subFolders = currentFolder->GetSubFolders();
         bool hasSubFolders = subFolders->GetCount() > 0;

         // Do we match?
         if (FolderWildcardMatch_(sFullPath, sWildcard, hierarchyDelimiter))
         {
            // CreateFolderLine_ escapes and quotes the path it is given in place.
            String sLinePath = sFullPath;
            String sFolderLine = CreateFolderLine_(currentFolder, hasSubFolders, sLinePath, sWildcard, true, hierarchyDelimiter, request);

            if (request.only_subscribed_ && hasSubFolders)
            {
               if (!request.emit_as_list_)
               {
                  // RFC 3501 6.3.9: "if "foo/bar" ... is subscribed but "foo" is
                  // not. A "%" wildcard to LSUB must return foo, not foo/bar, in
                  // the LSUB response, and it MUST be flagged with the \Noselect
                  // attribute." LSUB listed nothing at that level, so a client
                  // walking its subscriptions one level at a time never found
                  // them.
                  std::vector<String> thisPattern;
                  thisPattern.push_back(sWildcard);

                  if (sFolderLine.IsEmpty() && !currentFolder->GetIsSubscribed() &&
                      HasSubscribedDescendantOutside_(iAccountID, subFolders, sFullPath, thisPattern, hierarchyDelimiter, 0))
                  {
                     String sParentPath = sFullPath;
                     sFolderLine = CreateFolderLine_(std::shared_ptr<IMAPFolder>(), true, sParentPath, sWildcard, false, hierarchyDelimiter, request);
                  }
               }
               else if (request.recursive_match_patterns_ &&
                        HasSubscribedDescendantOutside_(iAccountID, subFolders, sFullPath, *request.recursive_match_patterns_, hierarchyDelimiter, 0))
               {
                  // RFC 5258 section 3, RECURSIVEMATCH: the mailbox is returned,
                  // subscribed itself or not, and says that a descendant the
                  // patterns do not reach is subscribed.
                  if (sFolderLine.IsEmpty())
                  {
                     ListRequest_ unfiltered = request;
                     unfiltered.only_subscribed_ = false;

                     String sParentPath = sFullPath;
                     sFolderLine = CreateFolderLine_(currentFolder, hasSubFolders, sParentPath, sWildcard, true, hierarchyDelimiter, unfiltered);
                  }

                  if (!sFolderLine.IsEmpty())
                     sFolderLine += _T(" (\"CHILDINFO\" (\"SUBSCRIBED\"))");
               }
            }

            if (!sFolderLine.IsEmpty())
               vecMatchingFolders.push_back(sFolderLine);
         }

         if (hasSubFolders)
            CreateIMAPFolderList_(iAccountID, subFolders, sWildcard, sPrefix, vecCurrentFolder, vecMatchingFolders, request, nullptr);

         vecCurrentFolder.erase(vecCurrentFolder.end() - 1);
      }

      // Only the outermost frame reports visibility and emits the root: nested
      // frames enter with a non-empty vecCurrentFolder, and the flag means
      // "a TOP-level folder of this tree is visible" - the same condition the
      // root line has always been emitted on.
      if (vecCurrentFolder.size() == 0 && pAnyFolderVisible)
         *pAnyFolderVisible = anyFolderVisible;

      if (vecCurrentFolder.size() == 0 && anyFolderVisible && !sPrefix.IsEmpty())
      {
         // The user can see at least one folder under this namespace root and
         // we're on the top level, so report the root itself as \Noselect. The
         // root is the prefix this listing was invoked with: "#Public" for the
         // public tree (callers pass the configured public folder name as the
         // prefix), "#Users.owner@domain" for a shared mailbox tree.
         String rootName = sPrefix;

         if (FolderWildcardMatch_(rootName, sWildcard, hierarchyDelimiter))
         {
            std::shared_ptr<IMAPFolder> pFolderDummy;
            String sFolderLine = CreateFolderLine_(pFolderDummy, true, rootName, sWildcard, false, hierarchyDelimiter, request);

            if (!sFolderLine.IsEmpty())
               vecMatchingFolders.push_back(sFolderLine);
         }

      }

   }

   String
   FolderListCreator::CreateFolderLine_(std::shared_ptr<IMAPFolder> currentFolder, bool hasSubFolders, String &sFullPath, const String &sWildcard, bool isSelectable, String hierarchyDelimiter, const ListRequest_ &request)
   {
      String nameAttributes = hasSubFolders ? "\\HasChildren" : "\\HasNoChildren";

      if (!isSelectable)
         nameAttributes += " \\Noselect";

      // RFC 6154 special-use attributes, so that clients map Sent/Drafts/Trash/Junk
      // automatically instead of creating duplicates. Which folder owns which
      // attribute was decided once, for the whole mailbox, in Create_.
      //
      // They are emitted whether or not the client asked for them with
      // RETURN (SPECIAL-USE). RFC 6154 section 4.2 permits that, and hMailServer
      // 6.2.18 already emitted the name-derived ones unconditionally: making them
      // conditional now would take the attributes away from every client that relies
      // on the old behaviour without asking, which is the larger regression.
      int designations = GetDesignations_(currentFolder, request);
      if (isSelectable && designations != IMAPSpecialUse::DesignationNone)
         nameAttributes += " " + IMAPSpecialUse::FormatDesignations(designations);

      // RFC 6154 section 4.1, the SPECIAL-USE selection option: return only mailboxes
      // that have a special use. Checked after the attributes are computed and before
      // anything is emitted, so the \Noselect public-folder root - which can never
      // carry a designation - is filtered out too.
      if (request.only_special_use_ && (!isSelectable || designations == IMAPSpecialUse::DesignationNone))
         return _T("");

      // RFC 5258 (LIST-EXTENDED) return option SUBSCRIBED: annotate folders the
      // user is subscribed to with the \Subscribed attribute.
      if (request.annotate_subscribed_ && (!currentFolder || currentFolder->GetIsSubscribed()))
         nameAttributes += " \\Subscribed";

      // Workaround for Outlook "feature".
      AdjustCaseToClientCase_(sFullPath, sWildcard, hierarchyDelimiter);

      
      // We cannot send " or \ directly in the response.
      // We must escape those:
      IMAPFolder::EscapeFolderString(sFullPath);

      // 2008-11-04
      // Always quote the string. This is how GMail acts.
      sFullPath = "\"" + sFullPath + "\"";

      String sFolderLine = "";

      // \ needs to be escaped.
      hierarchyDelimiter.Replace(_T("\\"), _T("\\\\"));

      if (!request.emit_as_list_)
      {
         // LSUB listing: only subscribed folders, emitted as "* LSUB".
         if (request.only_subscribed_ && (!currentFolder || currentFolder->GetIsSubscribed()))
            sFolderLine.Format(_T("* LSUB (%s) \"%s\" %s"), nameAttributes.c_str(), hierarchyDelimiter.c_str(), sFullPath.c_str());
      }
      else
      {
         // LIST / LIST-EXTENDED listing, emitted as "* LIST". When the SUBSCRIBED
         // selection option is active, unsubscribed folders are filtered out.
         if (request.only_subscribed_ && currentFolder && !currentFolder->GetIsSubscribed())
            return _T("");

         sFolderLine.Format(_T("* LIST (%s) \"%s\" %s"), nameAttributes.c_str(), hierarchyDelimiter.c_str(), sFullPath.c_str());
      }

      return sFolderLine;
   }

   int
   FolderListCreator::GetDesignations_(std::shared_ptr<IMAPFolder> currentFolder, const ListRequest_ &request)
   {
      // The dummy folder used for the public-folder root has no row and therefore no
      // designation.
      if (!currentFolder)
         return IMAPSpecialUse::DesignationNone;

      auto iterDesignation = request.special_use_.find(currentFolder->GetID());
      if (iterDesignation == request.special_use_.end())
         return IMAPSpecialUse::DesignationNone;

      return (*iterDesignation).second;
   }

   bool
   FolderListCreator::FolderWildcardMatch_(const String &sFolderName, const String &sWildcard, const String &hierarchyDelimiter)
   {
      // RFC 3501 6.3.8: "*" matches zero or more characters at this position,
      // "%" the same except the hierarchy delimiter; everything else stands for
      // itself, compared without case as mailbox names are here.
      //
      // The matcher this replaces consumed greedily and never backed up: a "*"
      // swallowed the rest of the name, so it only worked as the LAST
      // character. "INBOX.*test4", "*test", "%3.%" and "%t*4" matched nothing,
      // and a LIST, an LSUB or a LIST-EXTENDED pattern list that used one
      // silently left mailboxes out (imaptest's list, listext and subscribe).
      //
      // This one is the textbook table: row i says which prefixes of the name
      // the first i pattern characters can match. One row at a time, so the
      // cost is the product of the two lengths - bounded, whatever the
      // pattern, which a backtracking matcher fed "*%*%*%..." would not be.
      //
      // The product is still the pattern's length times the name's, for every
      // folder of every tree listed, on the shared TCP/IP threads - a pattern
      // of ten million "*" cost tens of seconds of CPU per LIST. So the
      // commands refuse a pattern longer than MaxPatternLength, and here a run
      // of wildcards is one wildcard first: "**" and "*%" match what "*"
      // matches, and "%%" what "%" matches. After that, a pattern with more
      // characters that must each match one character of the name than the
      // name has cannot match, and a row with no prefix left matching ends it.
      String pattern;
      int literals = 0;
      for (int i = 0; i < sWildcard.GetLength(); i++)
      {
         const wchar_t patternChar = sWildcard.GetAt(i);
         const bool wildcard = patternChar == '*' || patternChar == '%';
         const int kept = pattern.GetLength();
         const wchar_t last = kept > 0 ? pattern.GetAt(kept - 1) : (wchar_t) 0;

         if (wildcard && (last == '*' || last == '%'))
         {
            // A "*" anywhere in the run makes the run "*".
            if (patternChar == '*' && last == '%')
               pattern.SetAt(kept - 1, '*');
            continue;
         }

         if (!wildcard)
            literals++;

         pattern += patternChar;
      }

      const int patternLength = pattern.GetLength();
      const int nameLength = sFolderName.GetLength();
      const wchar_t delimiter = hierarchyDelimiter.IsEmpty() ? (wchar_t) 0 : hierarchyDelimiter.GetAt(0);

      if (literals > nameLength)
         return false;

      std::vector<char> previous((size_t) nameLength + 1, 0);
      std::vector<char> current((size_t) nameLength + 1, 0);

      // The empty pattern matches the empty name only.
      previous[0] = 1;

      for (int i = 1; i <= patternLength; i++)
      {
         const wchar_t patternChar = pattern.GetAt(i - 1);

         if (patternChar == '*')
         {
            // Zero characters, or one more than a match already covered.
            current[0] = previous[0];
            for (int j = 1; j <= nameLength; j++)
               current[j] = (previous[j] || current[j - 1]) ? 1 : 0;
         }
         else if (patternChar == '%')
         {
            // The same, never across a delimiter.
            current[0] = previous[0];
            for (int j = 1; j <= nameLength; j++)
            {
               const bool extends = current[j - 1] && sFolderName.GetAt(j - 1) != delimiter;
               current[j] = (previous[j] || extends) ? 1 : 0;
            }
         }
         else
         {
            current[0] = 0;
            for (int j = 1; j <= nameLength; j++)
            {
               const bool same = towupper((wint_t) sFolderName.GetAt(j - 1)) == towupper((wint_t) patternChar);
               current[j] = (previous[j - 1] && same) ? 1 : 0;
            }
         }

         previous.swap(current);

         if (std::find(previous.begin(), previous.end(), (char) 1) == previous.end())
            return false;
      }

      return previous[(size_t) nameLength] != 0;
   }

   bool
   FolderListCreator::IsFolderVisible_(__int64 iAccountID, std::shared_ptr<IMAPFolder> folder)
   {
      // The lookup right is asked of ACLManager as any other: on the account's own
      // tree and the public tree it is CheckPermission (everything visible when
      // enforcement is off), under "#Users" it is a right the owner granted.
      return folder->GetAccountID() != 0 && folder->GetAccountID() != iAccountID
         ? ACLManager::CheckDelegatedRight(iAccountID, folder, ACLPermission::PermissionLookup)
         : ACLManager::CheckPermission(iAccountID, folder, ACLPermission::PermissionLookup);
   }

   bool
   FolderListCreator::HasSubscribedDescendantOutside_(__int64 iAccountID, std::shared_ptr<IMAPFolders> folders, const String &sParentPath, const std::vector<String> &patterns, const String &hierarchyDelimiter, size_t depth)
   {
      if (!folders || depth > (size_t) IMAPFolder::MaxFolderDepth)
         return false;

      for (std::shared_ptr<IMAPFolder> folder : folders->GetSnapshot())
      {
         if (!IsFolderVisible_(iAccountID, folder))
            continue;

         const String sPath = sParentPath + hierarchyDelimiter + folder->GetFolderName();

         if (folder->GetIsSubscribed())
         {
            bool matched = false;

            for (const String &pattern : patterns)
            {
               if (FolderWildcardMatch_(sPath, pattern, hierarchyDelimiter))
               {
                  matched = true;
                  break;
               }
            }

            if (!matched)
               return true;
         }

         if (HasSubscribedDescendantOutside_(iAccountID, folder->GetSubFolders(), sPath, patterns, hierarchyDelimiter, depth + 1))
            return true;
      }

      return false;
   }

   void
   FolderListCreator::AdjustCaseToClientCase_(String &sPath, const String &sWildcard, const String &hierarchyDelimiter)
   {
      // Outlook 2003 requires the correct case after creating a new folder.
      // If OE2003 executes CREATE Inbox.SubFolder it requires the response
      // to be Inbox.SubFolder and not INBOX.SubFolder... Even if the real
      // name of the inbox is INBOX and not inbox.. and even if the inbox
      // is working properly.......

      std::vector<String> vecPath = StringParser::SplitString(sPath, hierarchyDelimiter);
      std::vector<String> vecWildcard = StringParser::SplitString(sWildcard, hierarchyDelimiter);

      // Build the response string.
      auto pathIterator = vecPath.begin();
      auto wildIterator = vecWildcard.begin();

      std::vector<String> vecResult;
      while (pathIterator != vecPath.end())
      {
         String sFoldername = (*pathIterator);

         if (wildIterator == vecWildcard.end())
            return;

         String sWildFolder = (*wildIterator);

         if (sFoldername.CompareNoCase(sWildFolder) == 0)
            sFoldername = sWildFolder;

         vecResult.push_back(sFoldername);

         wildIterator ++;
         pathIterator++;

      }

      sPath = StringParser::JoinVector(vecResult, hierarchyDelimiter);
   }



}
