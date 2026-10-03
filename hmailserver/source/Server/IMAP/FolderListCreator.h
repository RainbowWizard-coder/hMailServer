// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace HM
{
   class IMAPFolders;
   class IMAPFolder;

   class FolderListCreator
   {
   public:

      FolderListCreator();
      virtual ~FolderListCreator();


      // pAnyFolderVisible, where supplied, is set to whether the account can see
      // (has the lookup right on) at least one top-level folder of the listed
      // tree, INDEPENDENT of whether anything matched the wildcard. The "#Users"
      // namespace root must be reported for "%" exactly like "#Public" is, and
      // "%" matches none of the folders underneath either root - so an
      // output-was-empty test cannot answer the question and this flag exists.
      static String GetIMAPFolderList(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, bool *pAnyFolderVisible = nullptr);
      static String GetIMAPLSUBFolderList(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix);

      // RFC 5258 (LIST-EXTENDED) and RFC 6154 (SPECIAL-USE): a normal "* LIST" listing
      // that can be filtered to subscribed folders only (selection option SUBSCRIBED),
      // filtered to folders carrying a special-use attribute (selection option
      // SPECIAL-USE), and/or annotate the \Subscribed attribute (return option
      // SUBSCRIBED).
      //
      // pRecursiveMatchPatterns, where supplied, is RFC 5258's RECURSIVEMATCH with
      // SUBSCRIBED: every pattern of the command (reference prepended). A mailbox
      // that matches this listing's pattern and has a subscribed descendant that
      // none of those patterns matches is listed - subscribed or not - with the
      // CHILDINFO extended data item, ("CHILDINFO" ("SUBSCRIBED")).
      static String GetIMAPFolderListExtended(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, bool bOnlySubscribed, bool bAnnotateSubscribed, bool bOnlySpecialUse, bool *pAnyFolderVisible = nullptr, const std::vector<String> *pRecursiveMatchPatterns = nullptr);

      // The "* LIST (\Noselect \HasChildren) ..." line (with terminating CRLF)
      // for a namespace root the client cannot select - "#Users" - or "" when
      // the root does not match the client's wildcard. The per-owner nodes
      // ("#Users.owner@domain") are emitted by the listing walk itself, as the
      // prefix root, the same way "#Public" is; the namespace root above them
      // has no folder tree of its own, hence this helper. LIST output only.
      static String CreateNamespaceRootLine(const String &sRootPath, const String &sWildcard);

      // The lines of a listing (CRLF between them) not yet in seen, in their
      // order, each once, added to seen; empty lines are left out. LIST's guard
      // against naming a mailbox twice when several patterns match it. It kept
      // the lines it had seen in a list it searched from the top for every
      // line, so a listing of N mailboxes cost N squared comparisons: 40,000
      // folders took 1.4 s a command in the POSIX build. Kept in a hash set.
      static void TakeNewLines(const String &listing, std::unordered_set<std::wstring> &seen, std::vector<String> &fresh);

      // The most a LIST, LSUB or LIST-EXTENDED command is matched with: each
      // pattern, its reference in front, at most MaxPatternLength characters,
      // and at most MaxPatterns patterns in one command. Matching costs the
      // pattern's length times the name's for every folder listed, so without
      // them one authenticated client's ten-megabyte pattern held the shared
      // TCP/IP threads for tens of seconds a command. A name a folder can have
      // - 25 levels of 255 characters, and "#Users" or the public folders'
      // prefix before them - is well inside the first; the commands answer BAD
      // beyond either.
      static const int MaxPatternLength = 8192;
      static const size_t MaxPatterns = 32;

   private:

      // Everything one listing needs, carried as a unit through the recursive walk.
      //
      // This used to be a positional parameter list, and adding the RFC 6154 options
      // would have taken CreateFolderLine_ to ten parameters, five of them bool. That
      // is the shape where a caller transposes two flags, every call site still
      // compiles clean at /W3, and the only symptom is that LSUB starts answering like
      // LIST. Naming them costs a struct and removes the whole class of mistake.
      struct ListRequest_
      {
         ListRequest_() :
            only_subscribed_(false),
            annotate_subscribed_(false),
            only_special_use_(false),
            emit_as_list_(true),
            recursive_match_patterns_(nullptr)
         {
         }

         // LSUB, and the LIST-EXTENDED SUBSCRIBED selection option: skip folders the
         // user is not subscribed to.
         bool only_subscribed_;

         // LIST-EXTENDED SUBSCRIBED return option: add \Subscribed to the attributes.
         bool annotate_subscribed_;

         // RFC 6154 SPECIAL-USE selection option: skip folders that carry no
         // special-use attribute.
         bool only_special_use_;

         // True for "* LIST" output, false for "* LSUB" output.
         bool emit_as_list_;

         // LIST-EXTENDED RECURSIVEMATCH (with SUBSCRIBED): every pattern of the
         // command. Null when the option was not given.
         const std::vector<String> *recursive_match_patterns_;

         // Folder database id -> RFC 6154 designation bitmask, resolved once for the
         // whole mailbox before the walk starts. Deliberately empty when listing
         // public folders: a special use belongs to one user's mailbox, and a shared
         // folder advertised as \Trash to everybody who can see it would have one
         // user's client expunging another user's mail.
         std::map<__int64, int> special_use_;
      };

      static String Create_(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, ListRequest_ &request, bool *pAnyFolderVisible);

      static String CreateFolderLine_(std::shared_ptr<IMAPFolder> currentFolder, bool hasSubFolders, String &sFullPath, const String &sWildcard, bool isSelectable, String hierarchyDelimiter, const ListRequest_ &request);
      static void CreateIMAPFolderList_(__int64 iAccountID, std::shared_ptr<IMAPFolders> pStartFolders, const String &sWildcard, const String &sPrefix, std::vector<String> &vecCurrentFolder, std::vector<String> &vecMatchingFolders, const ListRequest_ &request, bool *pAnyFolderVisible);

      // The designations resolved for this folder, or IMAPSpecialUse::DesignationNone.
      static int GetDesignations_(std::shared_ptr<IMAPFolder> currentFolder, const ListRequest_ &request);

      static bool FolderWildcardMatch_(const String &sFolderName, const String &sWildcard, const String &hierarchyDelimiter_);

      // Whether the account may see (has the lookup right on) this folder.
      static bool IsFolderVisible_(__int64 iAccountID, std::shared_ptr<IMAPFolder> folder);

      // Whether a folder below sParentPath is subscribed and visible and matched by
      // none of the patterns - the test both LSUB's "%" parent (RFC 3501 6.3.9) and
      // RECURSIVEMATCH's CHILDINFO (RFC 5258 3) are built on.
      static bool HasSubscribedDescendantOutside_(__int64 iAccountID, std::shared_ptr<IMAPFolders> folders, const String &sParentPath, const std::vector<String> &patterns, const String &hierarchyDelimiter, size_t depth);
      static void AdjustCaseToClientCase_(String &sPath, const String &sWildcard, const String &hierarchyDelimiter);

   };


}
