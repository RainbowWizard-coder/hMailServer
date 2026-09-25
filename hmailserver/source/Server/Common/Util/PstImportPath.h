// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Where a PST import job's file is (docs/Migration.md, "Outlook data files
// (PST)"). A job names a file, never a path: it is looked for ONLY in
// <PstImportDirectory>/<the target account's domain>/, a directory an
// administrator of the server copies files into. A domain administrator holds
// manage_accounts on their own domain, and without the per-domain rule could
// import - into an account of their own, and then read - a file left there for
// another domain; so the rule is the same for every caller.
//
// The name is checked as text first (relative, no drive, no leading separator,
// no "." or ".." component, no UNC form, nothing a file name cannot hold), and
// then as the file system resolves it: the domain's directory and the file are
// each resolved to their final path - every link and junction followed, short
// names and case as they are on disk - and the file must lie inside the
// directory, and neither may be on another machine. The job checks the file it
// opened again, by the final path of its own handle, so a file swapped after
// the check is refused rather than read.

#pragma once

#include <string>

namespace HM
{
   class PstImportPath
   {
   public:
      // The file a job's name leads to, and the domain's directory it is in,
      // both resolved. False with the sentence when refused; missing is true
      // when the one thing wrong is that no file of that name is there.
      static bool Resolve(const String &name, const String &domain, String &file, String &directory, String &problem, bool &missing);

      // Whether a name is one a job may carry. False with the sentence.
      static bool IsPlainName(const String &name, String &problem);

      // A resolved file's name below its resolved directory, "/" between its
      // parts: the one name a file has however it was spelled - 2019\a.pst,
      // 2019/a.pst, a.pst. or its 8.3 name - which is what a job keeps, and what
      // a sentence a domain's administrator reads names it by.
      static String RelativeName(const String &file, const String &directory);

      // Whether the setting's value is a directory this server may name: empty
      // (no PST import), or an absolute local path. The sentence when not.
      static String SettingProblem(const String &directory);

      // Whether path lies inside directory, both resolved; without regard to
      // case on Windows, where the file system does not regard it either.
      static bool IsInside(const String &path, const String &directory);

      // A final path as the platform gives it, made comparable: on Windows the
      // \\?\ prefix taken off. Empty for a path on another machine.
      static String Comparable(const std::wstring &finalPath);

      // For the tester: the final path of an existing file or directory, or
      // false.
      static bool FinalPathOf(const String &path, String &finalPath, bool &isDirectory);
   };
}
