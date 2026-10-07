// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>
#include <memory>
#include <set>

#include "SieveLexer.h"

namespace HM
{
   // A positional/tagged/numeric argument to a command or test.
   struct SieveArgument
   {
      enum class Kind { StringList, Number, Tag };

      Kind kind = Kind::StringList;
      std::vector<String> strings;  // one or more entries for a string-list argument
      __int64 number = 0;           // value for a Number argument
      String tag;                   // tag name (without ':') for a Tag argument
      int line = 0;
   };

   // A Sieve test (e.g. header :contains "Subject" "hello", or allof(...)).
   struct SieveTest
   {
      String name;
      std::vector<SieveArgument> arguments;
      std::vector<std::shared_ptr<SieveTest>> tests; // nested tests (allof/anyof/not)
      int line = 0;
   };

   // A Sieve command (e.g. if, require, fileinto, keep).
   struct SieveCommand
   {
      String name;
      std::vector<SieveArgument> arguments;
      std::shared_ptr<SieveTest> test;                  // optional test (if/elsif)
      std::vector<std::shared_ptr<SieveCommand>> block; // optional command block
      bool hasBlock = false;
      int line = 0;
   };

   // The tags, positional string lists and numeric argument of a single command
   // or test, split apart once.
   //
   // Both the parser's validation pass and the evaluator read this, which is the
   // point: a tag such as :comparator or :days swallows the argument that follows
   // it, so if the two disagreed about which arguments are a tag's value and which
   // are positional they would disagree about what the script says. One splitter
   // means one answer.
   struct SieveArgumentSet
   {
      // Every tag that appeared, lowercased and in order, with the line it was on.
      // The validator checks these against the set the command/test allows.
      std::vector<String> tags;
      std::vector<int> tagLines;

      // ":is" / ":contains" / ":matches", or ":value" / ":count" (relational).
      String matchType = _T("is");
      bool matchTypeGiven = false;

      // The relational operator that :value / :count carries ("gt", "eq", ...).
      String relation;

      String comparator = _T("i;ascii-casemap");
      bool comparatorGiven = false;

      // "all" / "localpart" / "domain", or "user" / "detail" (subaddress).
      String addressPart = _T("all");
      bool addressPartGiven = false;

      // size :over / :under. sizeOver keeps the historical default of :over so
      // that evaluation of an already-stored script cannot change meaning.
      bool sizeOver = true;
      bool sizeGiven = false;

      bool hasNumber = false;
      __int64 number = 0;

      // ":copy" (RFC 3894).
      bool copy = false;

      // ":flags" (RFC 5232), already split into individual flag names.
      bool flagsGiven = false;
      std::vector<String> flags;

      // vacation (RFC 5230) tags.
      bool daysGiven = false;
      __int64 days = 0;
      // ":seconds" (RFC 6131). Held separately from days rather than folded into it,
      // because the two are mutually exclusive and the validator has to be able to
      // say which one the script actually wrote.
      bool secondsGiven = false;
      __int64 seconds = 0;
      bool subjectGiven = false;
      String subject;
      bool handleGiven = false;
      String handle;
      bool fromGiven = false;
      String fromAddress;
      bool addressesGiven = false;
      std::vector<String> addresses;
      bool mime = false;

      // body (RFC 5173). The transform is ":text" unless the script says
      // otherwise; ":content" additionally carries the MIME types to look at.
      String bodyTransform = _T("text");
      bool bodyTransformGiven = false;
      std::vector<String> contentTypes;

      // fileinto :create (RFC 5490).
      bool mailboxCreate = false;

      // date / currentdate (RFC 5260): the zone the extracted parts are expressed
      // in. zone is a "+hhmm"/"-hhmm" string; originalzone (date only) means the
      // header's own zone.
      String zone;
      bool zoneGiven = false;
      bool originalZone = false;

      // :index / :last (RFC 5260 6), for selecting among repeated header fields.
      int indexValue = 0;
      bool indexGiven = false;
      bool lastGiven = false;

      // spamtest :percent (RFC 5235).
      bool percentGiven = false;

      // notify (RFC 5435): the notification subject (:message) and importance.
      String notifyMessage;
      bool notifyMessageGiven = false;
      int importance = 2;
      bool importanceGiven = false;

      // duplicate (RFC 7352): which identifier the test tracks - a named header's
      // value under :header, a literal under :uniqueid, Message-ID by default.
      String duplicateHeader;
      bool duplicateHeaderGiven = false;
      String uniqueId;
      bool uniqueIdGiven = false;

      // Positional string-list arguments, in order.
      std::vector<std::vector<String>> stringLists;
   };

   // Recursive-descent parser that turns a Sieve token stream into an AST,
   // validating the RFC 5228 grammar, the set of supported commands/tests, and
   // - in a second pass - that every extension the script uses was named in a
   // "require" and is one this server can actually honour.
   class SieveParser
   {
   public:
      SieveParser();

      // What a script is parsed for. One being stored or checked - PUTSCRIPT,
      // CHECKSCRIPT, the self-service filter, COM's CheckSieveSyntax, the evaluation
      // diagnostics - is refused for every edit WhyFieldMayNotBeEdited refuses, and
      // for a notify ":from" that is not an address. One already stored and now run
      // at delivery is not refused for what a script stored before those checks came
      // in can hold: an edit of X-hMailServer-LoopCount, of Auto-Submitted, of a field
      // that names who the message is from (NamesWhoTheMessageIsFrom) or of
      // DKIM-Signature is let through
      // for LocalDelivery::EditSieveHeader to skip, and a notify ":from" for
      // SieveFromAddress to put the account's own address in place of, each with a
      // line in the application log, so that the rest of the script still runs rather
      // than none of it. An edit of Received or Return-Path, refused since editheader
      // came in, is refused at delivery too.
      enum class Purpose { Storing, Running };

      void SetPurpose(Purpose purpose) { purpose_ = purpose; }

      bool Parse(const std::vector<SieveToken> &tokens,
                 std::vector<std::shared_ptr<SieveCommand>> &commands,
                 String &errorMessage);

      // Why a script may not add or delete a header field of this name, or nothing
      // when it may: no name; a name that is not one (RFC 5322 2.2: printable ASCII
      // without a colon); Received and Return-Path, the trace of how the message
      // travelled (RFC 5293 6 and 9); X-hMailServer-LoopCount, in any case, which counts
      // the generated messages a message has been through and is what ends a mail loop
      // (RuleGuard::WhyHeaderMayNotBeSet says what editing it let through);
      // Auto-Submitted, in any case, which says whether a program sent the message
      // (IsAutoSubmittedHeader: RFC 5293 6 forbids deleting it); the fields
      // that say who the message is from (NamesWhoTheMessageIsFrom) and DKIM-Signature,
      // in any case, since a redirect sends the edited message on from this server and
      // DKIMSigner signs it as the domain its From names (RFC 5293 6 leaves which
      // fields may be edited to the implementation). The validator asks it of the name
      // a script writes, and LocalDelivery::EditSieveHeader asks it again of the name
      // as the script's variables expanded it: "${name}" is a legal name to the
      // validator, and can be anything at all once expanded. The sentence carries no
      // line number; the validator puts one before it.
      static String WhyFieldMayNotBeEdited(const String &fieldName);

      // From, Sender, Resent-From and Resent-Sender, in any case: the fields that say
      // who wrote or sent a message, which no script may edit (WhyFieldMayNotBeEdited).
      static bool NamesWhoTheMessageIsFrom(const String &fieldName);

      // Auto-Submitted, in any case: whether a program sent the message, which no
      // script may delete (RFC 5293 6) nor add a second of (RFC 3834 5.1), since the
      // first is what every automatic responder reads (WhyFieldMayNotBeEdited).
      static bool IsAutoSubmittedHeader(const String &fieldName);

      // True for the capability names this implementation can honour in a
      // "require". Anything else is refused when the script is uploaded rather
      // than quietly ignored at delivery time, because a script that asks for
      // something we cannot do does not mean what its author thinks it means.
      // Capability names are case-sensitive (RFC 5228 2.10.5).
      static bool IsSupportedExtension(const String &name);

      // Splits an argument vector. Returns false only when a tag that takes a
      // value is missing it; the evaluator treats that as an internal error
      // because the validation pass has already rejected such scripts.
      static bool SplitArguments(const std::vector<SieveArgument> &arguments,
                                 SieveArgumentSet &result,
                                 String &errorMessage);

      // Splits a list of flag strings into individual flag names. A single string
      // may hold several space-separated flags (RFC 5232 2). System flags are
      // canonicalised ("\\seen" -> "\\Seen") and duplicates removed, so that the
      // set handed to the delivery path is directly usable as IMAP flags.
      static std::vector<String> SplitFlagList(const std::vector<String> &values);

      // True for a flag name that may be set on a message. \Recent is excluded:
      // RFC 3501 does not allow a client to set it.
      static bool IsValidFlagName(const String &flag);

   private:
      const SieveToken &Current_() const;
      const SieveToken &Peek_(int offset) const;
      void Advance_();

      bool ParseCommands_(std::vector<std::shared_ptr<SieveCommand>> &commands, bool topLevel, String &errorMessage);
      bool ParseCommand_(std::shared_ptr<SieveCommand> &command, bool requireAllowed, String &errorMessage);
      bool ParseArguments_(std::vector<SieveArgument> &arguments, std::shared_ptr<SieveTest> &test, String &errorMessage);
      bool ParseArgument_(SieveArgument &argument, String &errorMessage);
      bool ParseStringList_(SieveArgument &argument, String &errorMessage);
      bool ParseTest_(std::shared_ptr<SieveTest> &test, String &errorMessage);

      static bool IsKnownCommand_(const String &name);
      static bool IsKnownTest_(const String &name);

      // The edits WhyFieldMayNotBeEdited refuses that a script stored before they
      // were refused can hold, and that Purpose::Running so lets through for delivery
      // to skip: X-hMailServer-LoopCount, NamesWhoTheMessageIsFrom and DKIM-Signature.
      static bool IsRefusedOnlyWhenStored_(const String &fieldName);

      bool CollectRequire_(const std::shared_ptr<SieveCommand> &command, String &errorMessage);

      // Second pass: extension gating and per-command/per-test argument checking.
      bool ValidateCommands_(const std::vector<std::shared_ptr<SieveCommand>> &commands, String &errorMessage);
      bool ValidateCommand_(const std::shared_ptr<SieveCommand> &command, String &errorMessage);
      bool ValidateTest_(const std::shared_ptr<SieveTest> &test, String &errorMessage);
      // expectedStringLists is how many positional lists the test takes: two for
      // header/address/envelope (the names, then the keys) and one for body, which
      // has no name list because its subject is the message itself.
      bool ValidateMatchArguments_(const SieveArgumentSet &set, const String &context, int line, String &errorMessage,
                                   size_t expectedStringLists = 2);
      bool ValidateFlagList_(const std::vector<String> &flags, int line, String &errorMessage);

      // :index/:last (RFC 5260 6), shared by the header, address and date tests:
      // :index needs its extension and counts from 1; :last rides on :index.
      bool ValidateIndexArguments_(const SieveArgumentSet &set, int line, String &errorMessage);

      bool HasExtension_(const String &extension) const;
      bool NeedExtension_(const String &extension, const String &feature, int line, String &errorMessage) const;

      // RFC 5463: extensions named by an "ihave" test are usable inside that
      // branch's block as if they had been required, because the block only runs
      // when the test - "are these available" - was true. Grants are collected
      // from a branch's test (the test itself, or any conjunct of an enclosing
      // "allof"; a name inside "anyof" or "not" guarantees nothing about the
      // block and grants nothing) and are scoped to the block by the caller
      // truncating the vector back afterwards.
      static void CollectIhaveGrants_(const std::shared_ptr<SieveTest> &test, std::vector<String> &granted);
      std::vector<String> ihave_granted_;

      static bool CheckTags_(const SieveArgumentSet &set, const String &allowed, const String &context, String &errorMessage);

      const std::vector<SieveToken> *tokens_;
      size_t index_;
      bool seenNonRequire_;

      Purpose purpose_ = Purpose::Storing;

      // How deep the two recursive descents currently are: blocks
      // (ParseCommands_ -> ParseCommand_ -> ParseCommands_, one level per "{") and
      // tests (ParseTest_ -> ParseTest_, one level per nested anyof/allof/not).
      //
      // Neither had a bound. A Sieve script is supplied by an authenticated user
      // through ManageSieve PUTSCRIPT or CHECKSCRIPT, and the size limit is 1 MB -
      // "if true{" is eight bytes, so a script well inside the limit asks for tens
      // of thousands of levels of recursion and takes the stack with it. A stack
      // overflow is not an exception this can catch: it is the process.
      //
      // The same shape as the MIME nesting defect fixed in the same week, and the
      // same answer - a bound far above anything a person writes, enforced where
      // the recursion happens rather than trusted to the input.
      int block_depth_ = 0;
      int test_depth_ = 0;

      // Real scripts nest a handful deep; generated ones from a rule builder rarely
      // pass ten. Fifty is beyond any legitimate script and thousands below the
      // stack, which is the useful place for a limit like this to sit.
      static constexpr int MaxNestingDepth = 50;

      // Increments a depth counter for the lifetime of one parse level and puts it
      // back however the level is left, including the error returns.
      class DepthGuard
      {
      public:
         explicit DepthGuard(int &depth) : depth_(depth) { depth_++; }
         ~DepthGuard() { depth_--; }
      private:
         DepthGuard(const DepthGuard &) = delete;
         DepthGuard &operator=(const DepthGuard &) = delete;
         int &depth_;
      };

      // Capability names named by the script's "require" statements.
      std::set<String> required_;
   };
}
