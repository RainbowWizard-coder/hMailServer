// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "RuleGuard.h"

#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"

#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"

#include "../Util/FileUtilities.h"
#include "../Util/Strings/Formatter.h"

// For LoopCountEditTester, which holds the Sieve validator and the delivery path's
// header edits to the same answer RuleGuard gives the rules, and the header of a
// notification built from a script's decision to the MIME writer's.
#include "../Mime/Mime.h"
#include "../Sieve/SieveScript.h"
#include "../Sieve/SieveNotifyResponder.h"
#include "../../SMTP/LocalDelivery.h"

// Common reaching into Server/SMTP for the configured loop limit. Not ideal layering,
// and not new: MessageUtilities.cpp and SieveVacationResponder.cpp both read
// SMTPConfiguration exactly this way. The limit belongs to the SMTP configuration
// because that is where an administrator sets it, and copying it somewhere tidier
// would give us two numbers that can disagree.
#include "../../SMTP/SMTPConfiguration.h"

// The criterion is evaluated here rather than through
// RegularExpression::TestExactMatch because that function cannot tell us which of the
// two failures happened - see the RegexCriteriaMatches comment in the header. The
// construction and the match are otherwise character for character what TestExactMatch
// does, so a criterion that matched before still matches.
#include <boost/regex.hpp>

#include <limits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How long one criterion evaluation may take before the pattern is suspended.
      //
      // Local delivery of one message is normally single-digit milliseconds end to end,
      // so a quarter of a second inside ONE comparison is already three orders of
      // magnitude out and cannot be anything but pathological. It is also short enough
      // that a sender feeding crafted subjects cannot accumulate much cost before the
      // breaker trips - which is the point, because the cost that matters is the
      // aggregate over a stream of messages rather than the cost of any one of them.
      const unsigned int EvaluationBudgetMilliseconds = 250;

      // How long a pattern stays suspended after failing to compile or blowing its
      // budget.
      //
      // Long enough that a flood of messages costs one evaluation rather than one per
      // message; short enough that an administrator watching the error log sees the
      // situation recur rather than assuming it went away. Being wrong in either
      // direction is cheap, because a suspended regex criterion only ever means "this
      // rule does not fire", never "this message is discarded". Note that suspension is
      // keyed on the pattern text: a corrected pattern is a different key and is tried
      // at once, so the window never delays a fix.
      const unsigned int SuspensionSeconds = 300;

      // How many distinct patterns may be held in the suspension table.
      //
      // The key comes from the rule set, not from the message, so this table cannot be
      // grown by sending mail. The cap is here because an installation with thousands
      // of rules would otherwise accumulate an entry per pattern for no benefit, and an
      // unbounded map on a delivery path is the kind of thing that gets found two years
      // later. When the table is full and pruning freed nothing we decline to suspend
      // rather than evict: evicting would silently un-suspend a pattern already known
      // to be expensive, and re-paying its cost is the exact failure this mechanism
      // exists to stop.
      const size_t MaxSuspendedPatterns = 256;

      // The white space a header field's value can carry around an
      // X-hMailServer-LoopCount: the blanks after the colon, and what is left of a
      // fold. Anything else beside the digits makes the value no count.
      bool
      IsLoopCountSpace(wchar_t character)
      {
         return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n';
      }

      // The longest pattern that will appear in a log line. A criterion is at most 255
      // characters (hm_rule_criterias.criteriamatchvalue is nvarchar(255)), so this
      // truncates almost nothing; it exists so that the size of an error entry stays a
      // property of this code rather than of the rule set.
      const int MaxDescribedPatternLength = 120;

      boost::recursive_mutex regex_state_mutex_;

      // pattern -> GetTickCount64 value at which it may be tried again. Presence in
      // this map also means "already reported", which is what keeps a hostile stream
      // from turning the breaker into a log flood: HM6042 and HM6043 are emitted on
      // insertion, never on a lookup that hits.
      std::map<String, ULONGLONG> suspended_patterns_;

      // Caller must hold regex_state_mutex_.
      void PruneExpiredSuspensions_(ULONGLONG now)
      {
         auto iter = suspended_patterns_.begin();
         while (iter != suspended_patterns_.end())
         {
            if (iter->second <= now)
               iter = suspended_patterns_.erase(iter);
            else
               iter++;
         }
      }

      // The pattern as it should appear in a log line: bounded in length, and with the
      // control characters that would otherwise let it forge extra log lines replaced.
      // A rule set is administrator-supplied and so is not hostile in the way a message
      // is, but a log line is a log line and being careful here costs nothing.
      String DescribePattern_(const String &pattern)
      {
         String described = pattern.Left(MaxDescribedPatternLength);

         described.Replace(_T("\r"), _T(" "));
         described.Replace(_T("\n"), _T(" "));
         described.Replace(_T("\t"), _T(" "));

         if (pattern.GetLength() > MaxDescribedPatternLength)
            described += _T("...");

         return described;
      }

      // True when the pattern is currently suspended. Prunes as it goes, so there is no
      // separate sweep to schedule.
      bool PatternIsSuspended_(const String &pattern)
      {
         boost::lock_guard<boost::recursive_mutex> guard(regex_state_mutex_);

         const ULONGLONG now = ::GetTickCount64();
         PruneExpiredSuspensions_(now);

         auto iter = suspended_patterns_.find(pattern);
         if (iter == suspended_patterns_.end())
            return false;

         return iter->second > now;
      }

      // Suspends the pattern, returning true only if this call is the one that did it -
      // and therefore the one that should report. A second delivery thread arriving with
      // the same pattern inside the window gets false and stays quiet.
      bool SuspendPatternAndClaimReport_(const String &pattern)
      {
         boost::lock_guard<boost::recursive_mutex> guard(regex_state_mutex_);

         const ULONGLONG now = ::GetTickCount64();
         PruneExpiredSuspensions_(now);

         auto iter = suspended_patterns_.find(pattern);
         if (iter != suspended_patterns_.end() && iter->second > now)
            return false;

         if (iter == suspended_patterns_.end() && suspended_patterns_.size() >= MaxSuspendedPatterns)
         {
            // Full, and the prune above already removed everything it could. Declining
            // to suspend means the pattern is evaluated again next time, which is the
            // behaviour we had before this unit existed: worse than suspending, and
            // strictly better than evicting a live entry.
            LOG_DEBUG(_T("RuleGuard: the regex suspension table is full; pattern not suspended."));
            return false;
         }

         suspended_patterns_[pattern] = now + (static_cast<ULONGLONG>(SuspensionSeconds) * 1000ULL);
         return true;
      }
   }

   bool
   RuleGuard::MessageIsUsableForRules(bool loadSucceeded,
                                      std::shared_ptr<const Account> account,
                                      std::shared_ptr<Message> message)
   {
      if (!message)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6040, "RuleGuard::MessageIsUsableForRules",
            "Rules were not applied: no message was supplied to the rule engine.");
         return false;
      }

      // The filename is recomputed here rather than taken from the MessageData, which
      // keeps it private with no accessor. That is not a workaround, it is the point:
      // this has to be the same derivation LoadFromMessage used, so it is the same
      // (account, message) pair through the same PersistentMessage::GetFileName
      // overload. If the two ever disagree, they disagree together.
      const String fileName = PersistentMessage::GetFileName(account, message);

      const String scope = account ? account->GetAddress() : String(_T("global rules"));

      if (!loadSucceeded)
      {
         // LoadFromMessage said no: a file over the 80 MB parser limit, or one whose
         // MIME parse threw - in which case it has already been copied to "Problematic
         // messages" and reported as HM4218. Either way the MessageData holds nothing,
         // and nothing is not what the rule set was written against.
         String errorMessage = Formatter::Format("Rules were not applied to message {0} ({1}) because its contents could not be read. The message is being delivered unfiltered; a rule matching on an empty subject, body or header would otherwise have acted on it. File: {2}",
            message->GetID(), scope, fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6040, "RuleGuard::MessageIsUsableForRules", errorMessage);
         return false;
      }

      // LoadFromMessage said yes, which is not the same as "there was a message there".
      // Its bNewMessage branch reports success for a file it could not open, because it
      // cannot tell an unreadable existing message from a message being composed from
      // nothing - both arrive as LoadFromFile returning false. So the file is checked
      // directly: a message this server has accepted always has a file, and that file
      // always has bytes in it.
      //
      // FileSize and not Exists, deliberately. FileUtilities::FileSize passes an
      // error_code to boost::filesystem and answers 0 on any failure, so it covers
      // "missing" and "empty" in one call and cannot throw. FileUtilities::Exists calls
      // boost::filesystem::exists with no error_code, which throws - and an exception
      // escaping a guard on the delivery path would be a worse defect than the one the
      // guard is here to prevent. The two cases are worth telling apart diagnostically
      // and are not worth a throwing call to tell apart.
      if (MessageStore::FileSize(fileName) <= 0)
      {
         String errorMessage = Formatter::Format("Rules were not applied to message {0} ({1}) because its file is missing or empty, even though the message data reported loading successfully. The message is being delivered unfiltered. File: {2}",
            message->GetID(), scope, fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6040, "RuleGuard::MessageIsUsableForRules", errorMessage);
         return false;
      }

      return true;
   }

   int
   RuleGuard::LoopCountForGeneratedMessage(std::shared_ptr<MessageData> source)
   {
      if (!source)
      {
         // No source means the depth of the chain cannot be established. Returning the
         // configured limit rather than 0 is deliberate: an unknown depth must not be
         // read as "no hops used", or a lost source would hand the generated message the
         // whole budget - which is this function's own defect arrived at from the other
         // side.
         return Configuration::Instance()->GetSMTPConfiguration()->GetRuleLoopLimit();
      }

      return source->GetRuleLoopCount() + 1;
   }

   void
   RuleGuard::CarryLoopCountForward(std::shared_ptr<MessageData> source,
                                    std::shared_ptr<MessageData> generated)
   {
      if (!generated)
         return;

      generated->SetRuleLoopCount(LoopCountForGeneratedMessage(source));
   }

   int
   RuleGuard::ReadLoopCount(const String &value, int loopLimit)
   {
      // The limit, held to 0 to INT_MAX - 1: a count read here is carried forward as
      // count + 1, which has to be an int.
      const int most = (std::numeric_limits<int>::max)();
      const int ceiling = loopLimit < 0 ? 0 : (loopLimit < most ? loopLimit : most - 1);

      // Through the pointer: CStdStr's own operator[] and std::wstring's are
      // ambiguous for a size_t.
      const wchar_t *text = value.c_str();
      size_t first = 0;
      size_t last = value.size();
      while (first < last && IsLoopCountSpace(text[first]))
         first++;
      while (last > first && IsLoopCountSpace(text[last - 1]))
         last--;

      // Nothing at all is no count.
      if (first == last)
         return ceiling;

      long long count = 0;
      for (size_t position = first; position < last; position++)
      {
         // ASCII digits only: no sign, and not iswdigit, which takes the digits of
         // other scripts that this server never writes.
         const wchar_t character = text[position];
         if (character < L'0' || character > L'9')
            return ceiling;

         // Past the ceiling the answer is decided, and the rest of the value is only
         // read to see that it is digits; count stays at most ten times an int and
         // nine, which a long long holds.
         if (count <= ceiling)
            count = count * 10 + (character - L'0');
      }

      return count > ceiling ? ceiling : static_cast<int>(count);
   }

   int
   RuleGuard::ReadLoopCount(const String &value)
   {
      return ReadLoopCount(value, Configuration::Instance()->GetSMTPConfiguration()->GetRuleLoopLimit());
   }

   namespace
   {
      // A header name as a refusal shows it: printable ASCII as it is, and every
      // other character as a backslash, a u and its number, so that a line break or
      // a character outside ASCII is seen for what it is and does not break the line.
      String
      HeaderNameForMessage(const String &name)
      {
         String out;
         const wchar_t *text = name.c_str();
         for (size_t position = 0; position < name.size(); position++)
         {
            const unsigned long character = static_cast<unsigned long>(text[position]);
            if (character >= 0x20 && character < 0x7F)
            {
               out += static_cast<wchar_t>(character);
            }
            else
            {
               wchar_t escaped[16];
               swprintf(escaped, sizeof(escaped) / sizeof(escaped[0]), L"\\u%04lX", character);
               out += escaped;
            }
         }

         return out;
      }
   }

   bool
   RuleGuard::IsLoopCountHeader(const String &fieldName)
   {
      static const wchar_t loopCount[] = L"X-hMailServer-LoopCount";
      const size_t loopCountLength = sizeof(loopCount) / sizeof(loopCount[0]) - 1;

      const wchar_t *text = fieldName.c_str();
      size_t first = 0;
      size_t last = fieldName.size();
      while (first < last && (text[first] == L' ' || text[first] == L'\t'))
         first++;
      while (last > first && (text[last - 1] == L' ' || text[last - 1] == L'\t'))
         last--;

      if (last - first != loopCountLength)
         return false;

      // ASCII letters folded, and nothing else: the MIME reader matches a name with
      // _stricmp in the C locale, and a character of another script that a wider
      // folding would take for one of these letters is no match there either.
      for (size_t position = 0; position < loopCountLength; position++)
      {
         wchar_t given = text[first + position];
         wchar_t wanted = loopCount[position];
         if (given >= L'A' && given <= L'Z')
            given = static_cast<wchar_t>(given - L'A' + L'a');
         if (wanted >= L'A' && wanted <= L'Z')
            wanted = static_cast<wchar_t>(wanted - L'A' + L'a');

         if (given != wanted)
            return false;
      }

      return true;
   }

   String
   RuleGuard::WhyHeaderMayNotBeSet(const String &fieldName)
   {
      String name = fieldName;
      name.Trim();

      if (name.IsEmpty())
         return _T("A Set header value action needs the name of the header it sets.");

      // Printable ASCII but the colon, the space and the tab between words aside: a
      // line break writes a second field under the name after it, a NUL ends the name
      // where the writer stops, a character outside ASCII is not written as given -
      // on Windows the narrowing to bytes can stop part-way through one and drop the
      // rest of the name - and a colon makes what comes before it the name.
      const wchar_t *text = fieldName.c_str();
      for (size_t position = 0; position < fieldName.size(); position++)
      {
         const wchar_t character = text[position];
         const bool blank = character == L' ' || character == L'\t';
         if (!blank && (character < 0x21 || character > 0x7E || character == L':'))
         {
            return _T("'") + HeaderNameForMessage(fieldName) + _T("' is not a header name a rule can set: a header name is ")
               _T("printable ASCII without a colon (RFC 5322 2.2), and a line break, a control character, a colon or ")
               _T("a character outside ASCII in it would write a field of another name.");
         }
      }

      if (IsLoopCountHeader(fieldName))
      {
         return _T("X-hMailServer-LoopCount counts the forwards, redirects and replies a message has been through, ")
            _T("which is how the server ends a mail loop, and a rule may not set it.");
      }

      return String();
   }

   String
   RuleGuard::HeaderValueAsOneField(const String &value)
   {
      String out;
      const wchar_t *text = value.c_str();
      const size_t length = value.size();

      for (size_t position = 0; position < length; position++)
      {
         const wchar_t character = text[position];

         // A fold: CR LF, the space or tab that makes the next line part of this
         // field, and something more than white space on that line - a line of
         // white space alone is the end of the header to some readers. Kept as it is,
         // so that a value copied from a folded header field is written as it was.
         if (character == L'\r' && position + 1 < length && text[position + 1] == L'\n')
         {
            size_t next = position + 2;
            while (next < length && (text[next] == L' ' || text[next] == L'\t'))
               next++;

            if (next > position + 2 && next < length && text[next] != L'\r' && text[next] != L'\n')
            {
               out += L"\r\n";
               position++;
               continue;
            }
         }

         if (character == L'\r' || character == L'\n')
         {
            out += L' ';
            continue;
         }

         out += character;
      }

      return out;
   }

   bool
   RuleGuard::RegexCriteriaMatches(const String &pattern, const String &subject)
   {
      if (PatternIsSuspended_(pattern))
         return false;

      // Distinguishes which side of the try threw. Boost raises the same
      // std::runtime_error family for a pattern it cannot compile and for a match it
      // gives up on, and those are different problems with different answers.
      bool compiled = false;

      try
      {
         // Constructed from the String itself, not from c_str(), because that is what
         // RegularExpression::TestExactMatch does and the whole point is that a working
         // criterion keeps working. Default flags, i.e. Perl syntax.
         boost::wregex expression(pattern);
         compiled = true;

         const ULONGLONG startTick = ::GetTickCount64();

         // regex_match, not regex_search: the whole value must match. Unchanged.
         const bool matched = boost::regex_match(subject, expression);

         const ULONGLONG elapsed = ::GetTickCount64() - startTick;

         // The expensive-but-successful case. Boost never complains about it, so a
         // wall-clock measurement is the only thing that catches it.
         if (elapsed >= static_cast<ULONGLONG>(EvaluationBudgetMilliseconds) &&
             SuspendPatternAndClaimReport_(pattern))
         {
            // ULONGLONG is unsigned __int64 on Windows, so elapsed binds exactly to that
            // FormatArgument constructor. The POSIX platform layer spells ULONGLONG as
            // uint64_t, which is unsigned long rather than unsigned long long, so there
            // it matches none of them exactly and is no closer to one than to the others.
            // The cast is an identity on Windows and names the same constructor.
            String errorMessage = Formatter::Format("A rule regular-expression criterion took {0} ms against a {1} character value, over the {2} ms budget, and will not be evaluated again for {3} seconds. Any sender can make this cost recur once per message, so it is being paid once per pattern instead. Pattern begins: {4}",
               (unsigned __int64) elapsed, subject.GetLength(), EvaluationBudgetMilliseconds, SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6043, "RuleGuard::RegexCriteriaMatches", errorMessage);
         }

         return matched;
      }
      catch (const std::runtime_error &)
      {
         // Boost's regex_error derives from std::runtime_error, and this is the only
         // exception type either the constructor or the matcher raises. Catching the
         // base rather than boost::regex_error keeps this identical in reach to the
         // catch in RegularExpression::TestExactMatch that it replaces.
         if (!compiled)
         {
            // The criterion is not a regular expression. Before this it silently never
            // matched, for ever, on an Active rule - which is the hardest kind of
            // configuration fault to find, because everything looks correct.
            if (SuspendPatternAndClaimReport_(pattern))
            {
               String errorMessage = Formatter::Format("A rule regular-expression criterion is not a valid regular expression and can never match. The rule containing it is active and doing nothing. Pattern: {0}",
                  DescribePattern_(pattern));

               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6042, "RuleGuard::RegexCriteriaMatches", errorMessage);
            }

            return false;
         }

         // The matcher gave up: Boost's state-count heuristic (error_complexity) or its
         // stack guard. Treated as no match, which is what happened before - the
         // difference is that it is now said out loud and not re-attempted on the next
         // message.
         if (SuspendPatternAndClaimReport_(pattern))
         {
            String errorMessage = Formatter::Format("A rule regular-expression criterion was abandoned as too complex against a {0} character value, and will not be evaluated again for {1} seconds. Treated as no match. Pattern begins: {2}",
               subject.GetLength(), SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6043, "RuleGuard::RegexCriteriaMatches", errorMessage);
         }

         return false;
      }

      // Unreachable: both the try and the only catch return. It is here because /WX
      // turns "not all control paths return a value" into a build failure, and whether
      // the compiler can see through a try/catch to prove otherwise is not something
      // worth depending on.
      return false;
   }

   bool
   RuleGuard::SieveRegexMatches(const String &pattern, const String &subject, bool caseSensitive)
   {
      if (PatternIsSuspended_(pattern))
         return false;

      bool compiled = false;

      try
      {
         // Perl syntax, as for a rule criterion; icase is what the script's default
         // comparator (i;ascii-casemap) means, and i;octet turns it off.
         boost::wregex expression(pattern,
            caseSensitive ? boost::regex::normal : (boost::regex::normal | boost::regex::icase));
         compiled = true;

         const ULONGLONG startTick = ::GetTickCount64();

         // regex_search, not regex_match: draft-ietf-sieve-regex matches anywhere in
         // the value, and the script anchors with ^ and $ when it means the whole.
         const bool matched = boost::regex_search(subject, expression);

         const ULONGLONG elapsed = ::GetTickCount64() - startTick;

         if (elapsed >= static_cast<ULONGLONG>(EvaluationBudgetMilliseconds) &&
             SuspendPatternAndClaimReport_(pattern))
         {
            // As above: ULONGLONG is unsigned __int64 on Windows and uint64_t - unsigned
            // long - in the POSIX platform layer, so the cast names the FormatArgument
            // constructor the Windows build already binds and is an identity there.
            String errorMessage = Formatter::Format("A Sieve ':regex' match took {0} ms against a {1} character value, over the {2} ms budget, and will not be evaluated again for {3} seconds. Any sender can make this cost recur once per message, so it is being paid once per pattern instead. Pattern begins: {4}",
               (unsigned __int64) elapsed, subject.GetLength(), EvaluationBudgetMilliseconds, SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6043, "RuleGuard::SieveRegexMatches", errorMessage);
         }

         return matched;
      }
      catch (const std::runtime_error &)
      {
         if (!compiled)
         {
            // Upload validation compiles every ':regex' key, so reaching this means a
            // script stored before that check existed, or edited on disk. Saying so
            // matters for the same reason it does for a rule: a test that can never
            // match looks exactly like a test that happens not to.
            if (SuspendPatternAndClaimReport_(pattern))
            {
               String errorMessage = Formatter::Format("A Sieve ':regex' key is not a valid regular expression and can never match. The script containing it is active and that test is doing nothing. Pattern: {0}",
                  DescribePattern_(pattern));

               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6042, "RuleGuard::SieveRegexMatches", errorMessage);
            }

            return false;
         }

         if (SuspendPatternAndClaimReport_(pattern))
         {
            String errorMessage = Formatter::Format("A Sieve ':regex' match was abandoned as too complex against a {0} character value, and will not be evaluated again for {1} seconds. Treated as no match. Pattern begins: {2}",
               subject.GetLength(), SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6043, "RuleGuard::SieveRegexMatches", errorMessage);
         }

         return false;
      }

      // Unreachable, for the same /WX reason as in RegexCriteriaMatches.
      return false;
   }

   bool
   RuleGuard::PolicyRegexMatches(const String &pattern, const String &subject)
   {
      if (PatternIsSuspended_(pattern))
         return false;

      bool compiled = false;

      try
      {
         // Perl syntax, as for a rule criterion and as the setting's own check
         // over REST compiles it (RestApiSettings.cpp).
         boost::wregex expression(pattern);
         compiled = true;

         const ULONGLONG startTick = ::GetTickCount64();

         // regex_search, not regex_match: the number is somewhere in the text,
         // which is the question a policy asks.
         const bool matched = boost::regex_search(subject, expression);

         const ULONGLONG elapsed = ::GetTickCount64() - startTick;

         if (elapsed >= static_cast<ULONGLONG>(EvaluationBudgetMilliseconds) &&
             SuspendPatternAndClaimReport_(pattern))
         {
            // The cast, as in the two siblings above: it names the FormatArgument
            // constructor the Windows build binds and is an identity there.
            String errorMessage = Formatter::Format("The DlpAccountNumberPattern setting took {0} ms to search a {1} character message text, over the {2} ms budget, and the account_number detector will find nothing for {3} seconds. Any sender can make this cost recur once per message, so it is being paid once per pattern instead. Pattern begins: {4}",
               (unsigned __int64) elapsed, subject.GetLength(), EvaluationBudgetMilliseconds, SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6822, "RuleGuard::PolicyRegexMatches", errorMessage);
         }

         return matched;
      }
      catch (const std::runtime_error &)
      {
         if (!compiled)
         {
            // The setting's check refuses one that does not compile over REST,
            // so reaching this means a restore or a direct edit of the row.
            // Said once per window rather than once per pattern for ever, as
            // for a rule: an administrator watching the log sees it recur.
            if (SuspendPatternAndClaimReport_(pattern))
            {
               String errorMessage = Formatter::Format("The DlpAccountNumberPattern setting is not a regular expression this server can use, so the account_number detector of the sensitive_content rule condition finds nothing. Set a pattern that compiles, or an empty one. Pattern: {0}",
                  DescribePattern_(pattern));

               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6821, "RuleGuard::PolicyRegexMatches", errorMessage);
            }

            return false;
         }

         // Boost gave up: error_complexity or its stack guard. No match, said
         // as what it is - the message this happened on has left without the
         // detector's answer, and the pattern is not asked again this window.
         if (SuspendPatternAndClaimReport_(pattern))
         {
            String errorMessage = Formatter::Format("The DlpAccountNumberPattern setting was abandoned as too complex against a {0} character message text, and the account_number detector will find nothing for {1} seconds. The message it happened on was judged without that detector. Pattern begins: {2}",
               subject.GetLength(), SuspensionSeconds, DescribePattern_(pattern));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6822, "RuleGuard::PolicyRegexMatches", errorMessage);
         }

         return false;
      }

      // Unreachable, for the same /WX reason as in RegexCriteriaMatches.
      return false;
   }

   void
   RuleGuard::ReportActionFailed(const String &actionName, const String &detail)
   {
      String errorMessage = Formatter::Format("The rule action {0} failed. The message file may have been left partly rewritten, and delivery is continuing with it. Detail: {1}",
         actionName, detail);

      ErrorManager::Instance()->ReportError(ErrorManager::High, 6041, "RuleGuard::ReportActionFailed", errorMessage);
   }

   unsigned int
   RuleGuard::RegexBudgetMilliseconds()
   {
      return EvaluationBudgetMilliseconds;
   }

   unsigned int
   RuleGuard::RegexSuspensionSeconds()
   {
      return SuspensionSeconds;
   }

   namespace
   {
      // A value as printable ASCII, for a failure's sentence: every other character
      // as a backslash, a u and its number, so that a tab, a fold or a digit of
      // another script is told apart from a blank.
      std::string
      LoopCountValueForMessage(const String &value)
      {
         std::string out;
         const wchar_t *text = value.c_str();
         for (size_t position = 0; position < value.size(); position++)
         {
            const unsigned long character = static_cast<unsigned long>(text[position]);
            if (character >= 0x20 && character < 0x7F)
            {
               out += static_cast<char>(character);
            }
            else
            {
               char escaped[16];
               snprintf(escaped, sizeof(escaped), "\\u%04lX", character);
               out += escaped;
            }
         }

         return out;
      }
   }

   void
   LoopCountTester::Test()
   {
      struct Case
      {
         const wchar_t *value;
         int limit;
         int expected;
      };

      const int most = (std::numeric_limits<int>::max)();

      const Case cases[] =
      {
         // A count, read as itself, the header's white space around it aside.
         { L"0", 5, 0 },
         { L"3", 5, 3 },
         { L"4", 5, 4 },
         { L"5", 5, 5 },
         { L"004", 5, 4 },
         { L" 3", 5, 3 },
         { L"3 \t", 5, 3 },
         { L"\t2\r\n", 5, 2 },
         { L"999", 1000, 999 },
         { L"7", most, 7 },

         // Above the limit: the limit. 2147483647 was a signed overflow at count + 1,
         // and 2147483648 and 4294967295 read as INT_MIN and -1 through Linux's
         // (int) wcstol.
         { L"6", 5, 5 },
         { L"2147483647", 5, 5 },
         { L"2147483648", 5, 5 },
         { L"4294967295", 5, 5 },
         { L"4294967296", 5, 5 },
         { L"99999999999", 5, 5 },
         { L"123456789012345678901234567890", 5, 5 },

         // No count: the limit, never 0 and never below 0. -5 passed every
         // "count >= limit" test.
         { L"-5", 5, 5 },
         { L"-0", 5, 5 },
         { L"-2147483648", 5, 5 },
         { L"+3", 5, 5 },
         { L"12abc", 5, 5 },
         { L"abc", 5, 5 },
         { L"3 4", 5, 5 },
         { L"3\r\n 4", 5, 5 },
         { L"3.0", 5, 5 },
         { L"0x10", 5, 5 },
         { L"1e3", 5, 5 },
         { L"", 5, 5 },
         { L"   ", 5, 5 },
         { L"\u0663", 5, 5 },  // ARABIC-INDIC DIGIT THREE
         { L"\uFF13", 5, 5 },  // FULLWIDTH DIGIT THREE
         { L"\u00B3", 5, 5 },  // SUPERSCRIPT THREE
         { L"3\u00A0", 5, 5 }, // a no-break space is not the header's white space

         // A limit of 0 refuses every generated message, and a negative one is held
         // to 0.
         { L"0", 0, 0 },
         { L"3", 0, 0 },
         { L"-5", 0, 0 },
         { L"", 0, 0 },
         { L"3", -1, 0 },
         { L"x", -1, 0 },
         { L"3", -2147483647 - 1, 0 },

         // The largest limit is held to 2147483646, so that + 1 is still an int.
         { L"2147483646", most, most - 1 },
         { L"2147483647", most, most - 1 },
         { L"99999999999", most, most - 1 },
         { L"-5", most, most - 1 },
         { L"x", most, most - 1 },
      };

      // Every case judged before the answer, so that a failure names each value read
      // wrongly and not only the first.
      std::string wrong;
      for (const Case &c : cases)
      {
         const String value(c.value);
         const int count = RuleGuard::ReadLoopCount(value, c.limit);
         if (count != c.expected)
         {
            wrong += (wrong.empty() ? "" : "; ") + std::string("\"") + LoopCountValueForMessage(value) + "\" at the limit " +
               std::to_string(c.limit) + " read as " + std::to_string(count) + ", not " + std::to_string(c.expected);
         }

         // Whatever the case expects: between 0 and the limit held to 0 to INT_MAX - 1,
         // so that count + 1 is an int.
         const int ceiling = c.limit < 0 ? 0 : (c.limit < most ? c.limit : most - 1);
         if (count < 0 || count > ceiling)
         {
            wrong += (wrong.empty() ? "" : "; ") + std::string("\"") + LoopCountValueForMessage(value) + "\" at the limit " +
               std::to_string(c.limit) + " read as " + std::to_string(count) + ", outside 0 to " + std::to_string(ceiling);
         }
      }

      // One character of any of the code points from 1 to 0x2FFF: a count only when it
      // is an ASCII digit, the limit otherwise - a sweep for a reader that takes some
      // other script's digit, a sign or a control character as a number.
      for (unsigned int character = 1; character < 0x3000; character++)
      {
         String value;
         value += static_cast<wchar_t>(character);

         const int count = RuleGuard::ReadLoopCount(value, 5);
         const bool digit = character >= '0' && character <= '9';
         const int expected = digit ? ((character - '0') < 5 ? static_cast<int>(character - '0') : 5) : 5;
         if (count != expected)
         {
            wrong += (wrong.empty() ? "" : "; ") + std::string("\"") + LoopCountValueForMessage(value) + "\" at the limit 5 read as " +
               std::to_string(count) + ", not " + std::to_string(expected);
         }
      }

      if (!wrong.empty())
         throw std::logic_error("X-hMailServer-LoopCount: " + wrong);
   }

   namespace
   {
      // The header of the message the edit cases start from: a count of 3 that no
      // edit may change.
      const char *EditedMessageHeader =
         "From: sender@example.test\r\n"
         "To: recipient@example.test\r\n"
         "Subject: Loop count\r\n"
         "X-hMailServer-LoopCount: 3\r\n"
         "\r\n";

      const wchar_t *EditedMessage =
         L"From: sender@example.test\r\n"
         L"To: recipient@example.test\r\n"
         L"Subject: Loop count\r\n"
         L"X-hMailServer-LoopCount: 3\r\n"
         L"\r\n"
         L"hello\r\n"
         L"X-hMailServer-LoopCount: 0\r\n";

      void
      LoadHeader(MimeHeader &header, const char *text)
      {
         header.Load(text, strlen(text), true);
      }

      // The header as the writer writes it, read back as the reader reads it: the
      // fields named so, and the value of the first.
      int
      FieldsNamedAfterRewrite(MimeHeader &header, const char *name, std::string &firstValue)
      {
         AnsiString written;
         header.Store(written);

         MimeHeader reread;
         reread.Load(written.c_str(), written.GetLength(), true);

         int count = 0;
         firstValue.clear();
         for (MimeField &field : reread.Fields())
         {
            if (_stricmp(field.GetName(), name) != 0)
               continue;

            if (count == 0)
               firstValue = field.GetValue() ? field.GetValue() : "";
            count++;
         }

         return count;
      }

      bool
      Holds(const String &text, const wchar_t *part)
      {
         return text.Find(part) >= 0;
      }
   }

   void
   LoopCountEditTester::Test()
   {
      std::string wrong;
      auto fail = [&wrong](const std::string &what)
      {
         wrong += (wrong.empty() ? "" : "; ") + what;
      };

      // Which names are X-hMailServer-LoopCount: ASCII letters in either case, blanks
      // around it, and nothing that only looks like it.
      {
         struct NameCase
         {
            const wchar_t *name;
            bool loopCount;
         };

         const NameCase cases[] =
         {
            { L"X-hMailServer-LoopCount", true },
            { L"x-hmailserver-loopcount", true },
            { L"X-HMAILSERVER-LOOPCOUNT", true },
            { L" X-hMailServer-LoopCount\t", true },
            { L"\tx-HMailserver-loopCOUNT  ", true },
            { L"", false },
            { L"X-hMailServer-LoopCounts", false },
            { L"X-hMailServer-LoopCounter", false },
            { L"X-hMailServer-LoopCoun", false },
            { L"X-hMailServer-Loop-Count", false },
            { L"XhMailServerLoopCount", false },
            { L"X-hMailServer-LoopCount:", false },
            { L"X-hMailServer-LoopCount\r\n", false },
            { L"X-hMailServer-L\u043EopCount", false },  // CYRILLIC SMALL LETTER O
            { L"X-hMail\u017Ferver-LoopCount", false },  // LATIN SMALL LETTER LONG S
            { L"X-hMailServer-LoopCount\u0100", false },
         };

         for (const NameCase &c : cases)
         {
            if (RuleGuard::IsLoopCountHeader(String(c.name)) != c.loopCount)
               fail("IsLoopCountHeader(\"" + LoopCountValueForMessage(String(c.name)) + "\") is not " + (c.loopCount ? "true" : "false"));
         }
      }

      // The names a rule's Set header value action may not set, and the reasons.
      {
         String withNul(L"X-hMailServer-LoopCount");
         withNul.push_back(L'\0');
         withNul.push_back(L'x');

         std::vector<String> refused =
         {
            L"X-hMailServer-LoopCount", L"x-hmailserver-loopcount", L"  X-HMAILSERVER-LOOPCOUNT ", L"", L"   ",
            L"X-Note\r\nX-hMailServer-LoopCount", L"X-Note\nX-hMailServer-LoopCount", L"X-hMailServer-LoopCount:",
            L"X-Note:", L"X-Note: x", withNul, L"X-hMailServer-LoopCount\u0100", L"X-Caf\u00E9", L"X-Note\u007F",
         };

         for (const String &name : refused)
         {
            const String reason = RuleGuard::WhyHeaderMayNotBeSet(name);
            if (reason.IsEmpty())
               fail("WhyHeaderMayNotBeSet(\"" + LoopCountValueForMessage(name) + "\") let a rule set it");
            else if (reason.Find(L"\r") >= 0 || reason.Find(L"\n") >= 0)
               fail("WhyHeaderMayNotBeSet(\"" + LoopCountValueForMessage(name) + "\") says why with a line break in it");
         }

         const String loopReason = RuleGuard::WhyHeaderMayNotBeSet(L"x-hmailserver-loopcount");
         if (!Holds(loopReason, L"X-hMailServer-LoopCount") || !Holds(loopReason, L"mail loop"))
            fail("the refusal of x-hmailserver-loopcount does not say it is the count that ends a mail loop: " + LoopCountValueForMessage(loopReason));

         const String breakReason = RuleGuard::WhyHeaderMayNotBeSet(L"X-Note\r\nX-hMailServer-LoopCount");
         if (!Holds(breakReason, L"\\u000D\\u000A"))
            fail("the refusal of a name with a line break does not show the line break: " + LoopCountValueForMessage(breakReason));

         const wchar_t *allowed[] = { L"X-Note", L"Subject", L"SomeHeader", L"X-Domain-Rule", L"My Header", L"X-hMailServer-LoopCounter" };
         for (const wchar_t *name : allowed)
         {
            const String reason = RuleGuard::WhyHeaderMayNotBeSet(String(name));
            if (!reason.IsEmpty())
               fail("WhyHeaderMayNotBeSet(\"" + LoopCountValueForMessage(String(name)) + "\") refused a header a rule may set: " + LoopCountValueForMessage(reason));
         }
      }

      // A value kept to one field: a fold stays, every other line break is a space.
      {
         struct ValueCase
         {
            const wchar_t *value;
            const wchar_t *expected;
         };

         const ValueCase cases[] =
         {
            { L"0", L"0" },
            { L"a b", L"a b" },
            { L"a\r\nX-hMailServer-LoopCount: 0", L"a  X-hMailServer-LoopCount: 0" },
            { L"a\nX-hMailServer-LoopCount: 0", L"a X-hMailServer-LoopCount: 0" },
            { L"a\rX-hMailServer-LoopCount: 0", L"a X-hMailServer-LoopCount: 0" },
            { L"a\n b", L"a  b" },
            { L"a\r\n b", L"a\r\n b" },
            { L"a\r\n\tb", L"a\r\n\tb" },
            { L"a\r\n \r\nX: 0", L"a     X: 0" },
            { L"a\r\n\r\n b", L"a  \r\n b" },
            { L"a\r\n ", L"a   " },
            { L"\r\n", L"  " },
         };

         for (const ValueCase &c : cases)
         {
            const String result = RuleGuard::HeaderValueAsOneField(String(c.value));
            if (result.Compare(c.expected) != 0)
               fail("HeaderValueAsOneField(\"" + LoopCountValueForMessage(String(c.value)) + "\") is \"" +
                  LoopCountValueForMessage(result) + "\", not \"" + LoopCountValueForMessage(String(c.expected)) + "\"");
         }
      }

      // The writer's own rule (MimeField::AppendAsOneField) is the same rule, on the
      // coded bytes.
      {
         auto narrow = [](const wchar_t *text)
         {
            std::string bytes;
            for (const wchar_t *character = text; *character; character++)
               bytes.push_back(static_cast<char>(*character));
            return bytes;
         };

         const wchar_t *const values[][2] =
         {
            { L"0", L"0" },
            { L"a b", L"a b" },
            { L"a\r\nX-hMailServer-LoopCount: 0", L"a  X-hMailServer-LoopCount: 0" },
            { L"a\nX-hMailServer-LoopCount: 0", L"a X-hMailServer-LoopCount: 0" },
            { L"a\rX-hMailServer-LoopCount: 0", L"a X-hMailServer-LoopCount: 0" },
            { L"a\n b", L"a  b" },
            { L"a\r\n b", L"a\r\n b" },
            { L"a\r\n\tb", L"a\r\n\tb" },
            { L"a\r\n \r\nX: 0", L"a     X: 0" },
            { L"a\r\n\r\n b", L"a  \r\n b" },
            { L"a\r\n ", L"a   " },
            { L"\r\n", L"  " },
            { L"\r\n\tc=x;\r\n\tbh=y;\r\n\tb=", L"\r\n\tc=x;\r\n\tbh=y;\r\n\tb=" },
         };

         for (const auto &value : values)
         {
            const std::string given = narrow(value[0]);
            AnsiString written("[");
            MimeField::AppendAsOneField(written, given.c_str(), given.size());
            if (std::string(written.c_str(), written.size()) != "[" + narrow(value[1]))
               fail("MimeField::AppendAsOneField(\"" + LoopCountValueForMessage(String(value[0])) + "\") wrote \"" +
                  LoopCountValueForMessage(String(written)) + "\", not \"[" + LoopCountValueForMessage(String(value[1])) + "\"");
         }
      }

      // The writer keeps a value to one field itself: a field whose value holds a line
      // break, put at the top of the header where no check before the writer saw the
      // value, reads back as one, and the count below it is the one read. As given
      // (MimeHeader::StoreAsGiven, which COM's Message.Save checks and nothing
      // writes) it is two fields with the reset count first: the control, which shows
      // the case is the injection it stands for.
      {
         MimeHeader header;
         LoadHeader(header, EditedMessageHeader);

         MimeField injected;
         injected.SetName("X-Note");
         injected.SetValue("a\r\nX-hMailServer-LoopCount: 0");
         header.Fields().insert(header.Fields().begin(), injected);

         AnsiString asGiven;
         header.StoreAsGiven(asGiven);
         MimeHeader rereadAsGiven;
         rereadAsGiven.Load(asGiven.c_str(), asGiven.GetLength(), true);
         const char *firstAsGiven = rereadAsGiven.GetRawFieldValue(String(L"X-hMailServer-LoopCount"));
         if (firstAsGiven == nullptr || strcmp(firstAsGiven, "0") != 0)
            fail("the control: a field whose value holds a line break, as given, no longer reads back with a count of 0 first, so the case beside it proves nothing");

         std::string first;
         std::string note;
         const int counts = FieldsNamedAfterRewrite(header, "X-hMailServer-LoopCount", first);
         const int notes = FieldsNamedAfterRewrite(header, "X-Note", note);
         if (counts != 1 || first != "3" || notes != 1 || note != "a  X-hMailServer-LoopCount: 0")
            fail("a field whose value holds a line break was written as " + std::to_string(counts) + " loop counts, the first \"" +
               first + "\", and " + std::to_string(notes) + " X-Note, the first \"" + note + "\"");
      }

      // The header of a generated message, built as its generator builds it - the
      // values through SetUnicodeFieldValue (MessageData::SetFieldValue, SetSubject,
      // SetFrom), the count through SetRawFieldValue (MessageData::SetRuleLoopCount) -
      // holds the generator's Auto-Submitted and count and no other, whatever a value
      // set before them held. A Sieve notification, from the decision a script's notify
      // made, as SieveNotifyResponder::Send builds it: a :message or :from with line
      // breaks in it wrote "Auto-Submitted: no" and a count of 0 above the
      // notification's own, so that a script notifying its own account was not held by
      // the auto-submitted rule or the loop limit, and notified about its own
      // notifications for ever.
      {
         const wchar_t *scripts[] =
         {
            L"require \"enotify\";\r\nnotify :message \"Note\r\nAuto-Submitted: no\r\nX-hMailServer-LoopCount: 0\" \"mailto:a@example.test\";\r\n",
            L"require \"enotify\";\r\nnotify :from \"a@example.test\r\nAuto-Submitted: no\r\nX-hMailServer-LoopCount: 0\" \"mailto:a@example.test\";\r\n",
            L"require \"enotify\";\r\nnotify :message text:\r\nNote\r\nAuto-Submitted: no\r\nX-hMailServer-LoopCount: 0\r\n.\r\n\"mailto:a@example.test\";\r\n",
         };

         // No count of its own: the evaluator reads one against the configured limit.
         const String trigger(L"From: sender@example.test\r\nTo: a@example.test\r\nSubject: Trigger\r\n\r\nhello\r\n");

         for (const wchar_t *script : scripts)
         {
            const std::string what = "the notification of \"" + LoopCountValueForMessage(String(script)) + "\"";

            SieveEnvelope envelope;
            SieveResult result;
            SieveScript::Evaluate(String(script), trigger, envelope, result);
            if (result.notifications.size() != 1)
            {
               fail(what + ": the script decided " + std::to_string(result.notifications.size()) + " notifications, not one");
               continue;
            }

            const SieveNotifyDecision &decision = result.notifications[0];
            if (decision.subject.Find(L"\r\n") < 0 && decision.from.Find(L"\r\n") < 0)
               fail(what + ": neither the subject nor the sender holds a line break, so the case proves nothing");

            MimeHeader notification;
            notification.SetUnicodeFieldValue("From", decision.from.IsEmpty() ? String(L"a@example.test") : decision.from, "");
            notification.SetUnicodeFieldValue("To", decision.to, "");
            notification.SetUnicodeFieldValue("Subject", decision.subject, "");
            notification.SetUnicodeFieldValue("Auto-Submitted", String(L"auto-notified"), "");
            notification.SetRawFieldValue("X-hMailServer-LoopCount", "4", "");
            notification.SetUnicodeFieldValue("Content-Type", String(L"text/plain; charset=\"utf-8\""), "");

            std::string count;
            std::string submitted;
            std::string type;
            const int counts = FieldsNamedAfterRewrite(notification, "X-hMailServer-LoopCount", count);
            const int marks = FieldsNamedAfterRewrite(notification, "Auto-Submitted", submitted);
            const int types = FieldsNamedAfterRewrite(notification, "Content-Type", type);
            if (counts != 1 || count != "4" || marks != 1 || submitted != "auto-notified" || types != 1)
            {
               fail(what + ": " + std::to_string(counts) + " loop counts, the first \"" + count + "\", " + std::to_string(marks) +
                  " Auto-Submitted, the first \"" + submitted + "\", and " + std::to_string(types) + " Content-Type");
               continue;
            }

            // Delivered to the account whose script it is, the notification is
            // auto-submitted, and not notified about.
            AnsiString written;
            notification.Store(written);
            SieveResult again;
            SieveScript::Evaluate(String(script), String(written) + String(L"You have new mail.\r\n"), envelope, again);
            if (!again.notifications.empty())
               fail(what + ": the script notified about its own notification");
         }
      }

      // A copy, as RuleApplier::ApplyAction_Copy builds it: the count raised, then the
      // rule's name in X-CopyRule - in place, where the message already has one, which
      // is above its count from the second copy on. A name with a line break in it
      // wrote a count of 0 above the copy's own at every copy, and an account copying
      // its own mail made copies without end.
      {
         MimeHeader copy;
         LoadHeader(copy, "X-CopyRule: earlier\r\nFrom: sender@example.test\r\nSubject: Copied\r\nX-hMailServer-LoopCount: 3\r\n\r\n");
         copy.SetRawFieldValue("X-hMailServer-LoopCount", "4", "");
         copy.SetUnicodeFieldValue("X-CopyRule", String(L"Copy\r\nX-hMailServer-LoopCount: 0"), "");

         std::string count;
         std::string name;
         const int counts = FieldsNamedAfterRewrite(copy, "X-hMailServer-LoopCount", count);
         const int names = FieldsNamedAfterRewrite(copy, "X-CopyRule", name);
         if (counts != 1 || count != "4" || names != 1)
            fail("a copy whose rule's name holds a line break holds " + std::to_string(counts) + " loop counts, the first \"" + count +
               "\", and " + std::to_string(names) + " X-CopyRule");
      }

      // Sieve's addheader: a value with a line break is one field, :last or not, and
      // the count stays 3.
      {
         struct AddCase
         {
            const wchar_t *value;
            const char *written;
         };

         const AddCase cases[] =
         {
            { L"a\r\nX-hMailServer-LoopCount: 0", "a  X-hMailServer-LoopCount: 0" },
            { L"a\nX-hMailServer-LoopCount: 0", "a X-hMailServer-LoopCount: 0" },
            { L"a\rX-hMailServer-LoopCount: 0", "a X-hMailServer-LoopCount: 0" },
         };

         for (const AddCase &c : cases)
         {
            const wchar_t *value = c.value;
            for (int last = 0; last < 2; last++)
            {
               MimeHeader header;
               LoadHeader(header, EditedMessageHeader);

               SieveHeaderEdit edit;
               edit.isAdd = true;
               edit.name = L"X-Note";
               edit.value = value;
               edit.addLast = last == 1;

               std::vector<String> refusedEdits;
               const bool edited = LocalDelivery::EditSieveHeader(header, std::vector<SieveHeaderEdit>(1, edit), refusedEdits);

               std::string first;
               const int counts = FieldsNamedAfterRewrite(header, "X-hMailServer-LoopCount", first);
               std::string note;
               const int notes = FieldsNamedAfterRewrite(header, "X-Note", note);
               if (!edited || !refusedEdits.empty() || counts != 1 || first != "3" || notes != 1 || note != c.written)
                  fail(std::string("addheader") + (last ? " :last" : "") + " \"X-Note\" \"" + LoopCountValueForMessage(String(value)) +
                     "\" left " + std::to_string(counts) + " loop counts, the first \"" + first + "\", and " + std::to_string(notes) +
                     " X-Note, the first \"" + note + "\"");
            }
         }
      }

      // Scripts that edit the count are refused when stored or checked, with the
      // line and the reason; the name is matched in any case.
      {
         const wchar_t *scripts[] =
         {
            L"require \"editheader\";\r\ndeleteheader \"X-hMailServer-LoopCount\";\r\n",
            L"require \"editheader\";\r\ndeleteheader \"x-hmailserver-loopcount\";\r\n",
            L"require \"editheader\";\r\naddheader \"X-HMAILSERVER-LOOPCOUNT\" \"0\";\r\n",
            L"require \"editheader\";\r\naddheader :last \"X-hMailServer-LoopCount\" \"0\";\r\n",
            L"require [\"editheader\", \"index\"];\r\ndeleteheader :index 1 \"X-hMailServer-LoopCount\";\r\n",
            L"require \"editheader\";\r\ndeleteheader :contains \"X-hMailServer-LoopCount\" \"5\";\r\n",
            L"require \"editheader\";\r\nif true { deleteheader \"X-hMailServer-LoopCount\"; }\r\n",
         };

         for (const wchar_t *script : scripts)
         {
            const String error = SieveScript::CheckSyntax(String(script));
            if (!Holds(error, L"Line 2:") || !Holds(error, L"mail loop"))
               fail("the script \"" + LoopCountValueForMessage(String(script)) + "\" was not refused for editing the loop count: \"" +
                  LoopCountValueForMessage(error) + "\"");
         }

         // The name as the script wrote it is the name the refusal gives.
         const String lower = SieveScript::CheckSyntax(String(L"require \"editheader\";\r\ndeleteheader \"x-hmailserver-loopcount\";\r\n"));
         if (!Holds(lower, L"x-hmailserver-loopcount"))
            fail("the refusal does not name the header as the script wrote it: \"" + LoopCountValueForMessage(lower) + "\"");

         // The trace headers keep their own refusal, word for word.
         const String received = SieveScript::CheckSyntax(String(L"require \"editheader\";\r\ndeleteheader \"Received\";\r\n"));
         if (received.Compare(L"Line 2: the Received header records how a message travelled and cannot be edited by a script.") != 0)
            fail("the refusal of an edit of Received changed: \"" + LoopCountValueForMessage(received) + "\"");

         const wchar_t *accepted[] =
         {
            L"require \"editheader\";\r\ndeleteheader \"X-Note\";\r\n",
            L"require \"editheader\";\r\naddheader \"X-Note\" \"0\";\r\n",
            L"require \"editheader\";\r\naddheader \"X-hMailServer-LoopCounter\" \"1\";\r\n",
            // A name the variables make is a legal name to the validator, and is
            // refused where the edit is made (below).
            L"require [\"editheader\", \"variables\"];\r\nset \"h\" \"X-hMailServer-LoopCount\";\r\ndeleteheader \"${h}\";\r\n",
         };

         for (const wchar_t *script : accepted)
         {
            const String error = SieveScript::CheckSyntax(String(script));
            if (!error.IsEmpty())
               fail("the script \"" + LoopCountValueForMessage(String(script)) + "\" was refused: \"" + LoopCountValueForMessage(error) + "\"");
         }
      }

      // A script already stored is run with its edit of the count let through, and
      // with an edit of Received still refused.
      {
         const String stored(L"require \"editheader\";\r\ndeleteheader \"X-hMailServer-LoopCount\";\r\n");
         String error;

         SieveScript storing;
         if (storing.Parse(stored, error))
            fail("SieveScript::Parse stored a script that edits the loop count");

         SieveScript running;
         if (!running.Parse(stored, error, SieveParser::Purpose::Running))
            fail("a stored script that edits the loop count did not run: \"" + LoopCountValueForMessage(error) + "\"");

         SieveScript trace;
         if (trace.Parse(String(L"require \"editheader\";\r\ndeleteheader \"Received\";\r\n"), error, SieveParser::Purpose::Running))
            fail("a stored script that edits Received ran");
      }

      // At delivery: the edits of the count refused, and said; the edit beside them
      // made; and the count the message came with kept. By name, in any case, as the
      // variables made it, with a line break in the name, from an included script, and
      // with a value a sender wrote in the body.
      {
         struct DeliveryCase
         {
            const wchar_t *what;
            const wchar_t *script;
            size_t refusals;
            bool edited;
            const wchar_t *refusal;
         };

         const DeliveryCase cases[] =
         {
            { L"the count by name", L"require \"editheader\";\r\naddheader \"X-Sieve\" \"yes\";\r\ndeleteheader \"X-hMailServer-LoopCount\";\r\naddheader \"x-hmailserver-loopcount\" \"0\";\r\n",
              2, true, L"mail loop" },
            { L"the count as a variable made it", L"require [\"editheader\", \"variables\"];\r\nset \"h\" \"X-HMAILSERVER-LOOPCOUNT\";\r\ndeleteheader \"${h}\";\r\naddheader :last \"${h}\" \"0\";\r\n",
              2, false, L"mail loop" },
            { L"Received as a variable made it", L"require [\"editheader\", \"variables\"];\r\nset \"h\" \"Received\";\r\ndeleteheader \"${h}\";\r\n",
              1, false, L"travelled" },
            { L"a name with a line break", L"require [\"editheader\", \"variables\"];\r\nset \"h\" \"X-Note\r\nX-hMailServer-LoopCount\";\r\naddheader \"${h}\" \"0\";\r\n",
              1, false, L"not a legal header field name" },
            { L"a value from the body", L"require [\"editheader\", \"variables\", \"body\"];\r\nif body :raw :matches \"*\" { addheader \"X-Body\" \"${1}\"; }\r\n",
              0, true, nullptr },
            { L"an included script", L"require [\"editheader\", \"include\"];\r\naddheader \"X-Sieve\" \"yes\";\r\ninclude :personal \"inner\";\r\n",
              1, true, L"mail loop" },
         };

         auto includeFetch = [](const String &name, bool global) -> String
         {
            if (global || name.Compare(L"inner") != 0)
               return String();

            return String(L"require \"editheader\";\r\ndeleteheader \"X-hMailServer-LoopCount\";\r\n");
         };

         for (const DeliveryCase &c : cases)
         {
            const std::string what = LoopCountValueForMessage(String(c.what));

            SieveEnvelope envelope;
            SieveResult result;
            const String actions = SieveScript::Evaluate(String(c.script), String(EditedMessage), envelope, result, nullptr, false, nullptr, includeFetch);
            if (actions.StartsWith(_T("error:")))
            {
               fail(what + ": the script did not run: " + LoopCountValueForMessage(actions));
               continue;
            }

            MimeHeader header;
            LoadHeader(header, EditedMessageHeader);

            std::vector<String> refusedEdits;
            const bool edited = LocalDelivery::EditSieveHeader(header, result.headerEdits, refusedEdits);

            if (refusedEdits.size() != c.refusals)
               fail(what + ": " + std::to_string(refusedEdits.size()) + " edits refused, not " + std::to_string(c.refusals));

            for (const String &refusal : refusedEdits)
            {
               if (c.refusal && !Holds(refusal, c.refusal))
                  fail(what + ": the refusal does not say why: " + LoopCountValueForMessage(refusal));
            }

            if (edited != c.edited)
               fail(what + ": EditSieveHeader answered " + (edited ? "true" : "false"));

            std::string first;
            const int counts = FieldsNamedAfterRewrite(header, "X-hMailServer-LoopCount", first);
            if (counts != 1 || first != "3")
               fail(what + ": the header holds " + std::to_string(counts) + " loop counts, the first \"" + first + "\", not one of 3");

            if (c.edited)
            {
               std::string added;
               if (FieldsNamedAfterRewrite(header, "X-Sieve", added) + FieldsNamedAfterRewrite(header, "X-Body", added) != 1)
                  fail(what + ": the edit beside the refused ones was not made");
            }
         }

         // The body case is the sender's reach: the value the script added held the
         // body's line break, and so the second field the cases above did not get.
         SieveEnvelope envelope;
         SieveResult result;
         SieveScript::Evaluate(String(cases[4].script), String(EditedMessage), envelope, result);
         if (result.headerEdits.size() != 1 || result.headerEdits[0].value.Find(L"\r\nX-hMailServer-LoopCount: 0") < 0)
            fail("the body case's value does not hold the body's line break, so it no longer shows a sender reaching the header");
      }

      if (!wrong.empty())
         throw std::logic_error("Header edits of X-hMailServer-LoopCount: " + wrong);
   }
}
