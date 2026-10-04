// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "IMAPThread.h"
#include "IMAPConnection.h"

#include "../Common/BO/Message.h"
#include "../Common/Mime/Mime.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Util/Time.h"

#include <chrono>
#include <locale>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One node of the thread tree: a searched message, or a message known only
      // from somebody's References line. The latter kind ("dummy") is what lets two
      // replies thread together when the message they both answer was deleted or
      // never arrived - RFC 5256 builds the tree out of every id it has heard OF,
      // then prunes the ones it cannot show.
      struct ThreadContainer
      {
         std::shared_ptr<Message> message;   // null for a dummy
         int sequence = 0;                   // 1-based mailbox sequence number

         ThreadContainer *parent = nullptr;
         std::vector<ThreadContainer*> children;

         // Loaded once from the header, used by every later phase.
         DateTime sent_date;
         String base_subject_key;            // upper-cased base subject; the grouping key
         bool is_reply_or_forward = false;
      };

      // A References header on a hostile message can carry any number of ids, and
      // every id becomes a container. The cap keeps one message from manufacturing
      // unbounded tree nodes; the LAST ids are the ones kept, because References
      // lists ancestors root-first and the nearest ancestors are what place the
      // message correctly. A century of ancestry places it identically.
      const size_t kMaxReferencesPerMessage = 100;

      // The deepest chain a thread may be linked into.
      //
      // This is a STACK bound, not a taste judgement. Prune_,
      // SortChildrenRecursively_ and (on branching nodes) RenderContainer_ each
      // recurse once per level, and a worker thread here gets the 1MB default -
      // nothing in this project sets /STACK. An adversarial review measured the
      // real limit by compiling a probe against these exact frames: 6000 levels
      // survive, 8000 terminate with STATUS_STACK_OVERFLOW. A stack overflow is not
      // reliably catchable, so this is a crash of the whole service, reachable by
      // any authenticated user willing to deliver a long enough reply chain to
      // themselves.
      //
      // 512 is far below the measured cliff and far above any conversation a person
      // has. Beyond it a message simply becomes its own root rather than being
      // linked deeper - the same thing that already happened past the old cycle
      // guard, at a depth that is survivable rather than fatal.
      const int kMaxThreadDepth = 512;

      // How far below the virtual root a node sits.
      int
      DepthOf_(ThreadContainer *node)
      {
         int depth = 0;

         for (ThreadContainer *current = node; current != nullptr; current = current->parent)
         {
            depth++;

            // A cycle would make this walk endless. Nothing should create one - the
            // link rules below refuse them - so reaching the cap means something is
            // already wrong, and reporting "too deep" makes the caller refuse the
            // link, which is the safe answer either way.
            if (depth > kMaxThreadDepth)
               break;
         }

         return depth;
      }

      // Whether candidate is node itself or an ancestor of node. This is the loop
      // guard: the References lines of different messages can disagree, and linking
      // blindly on a disagreement creates a cycle that the render walk would never
      // leave.
      bool
      IsSelfOrAncestor_(ThreadContainer *candidate, ThreadContainer *node)
      {
         int depth = 0;

         for (ThreadContainer *current = node; current != nullptr; current = current->parent)
         {
            if (current == candidate)
               return true;

            // Belt for a cycle that slipped in anyway: better a wrong answer to one
            // link question than a spin.
            depth++;
            if (depth > kMaxThreadDepth)
               return true;
         }

         return false;
      }

      // The single test every link goes through: refuse a cycle, and refuse a link
      // that would push the tree past the depth the stack can walk.
      bool
      CanLink_(ThreadContainer *parent, ThreadContainer *child)
      {
         if (parent == child)
            return false;

         if (IsSelfOrAncestor_(child, parent))
            return false;

         if (DepthOf_(parent) >= kMaxThreadDepth)
            return false;

         return true;
      }

      void
      DetachFromParent_(ThreadContainer *node)
      {
         if (!node->parent)
            return;

         std::vector<ThreadContainer*> &siblings = node->parent->children;

         for (size_t i = 0; i < siblings.size(); i++)
         {
            if (siblings[i] == node)
            {
               siblings.erase(siblings.begin() + i);
               break;
            }
         }

         node->parent = nullptr;
      }

      void
      AttachChild_(ThreadContainer *parent, ThreadContainer *child)
      {
         child->parent = parent;
         parent->children.push_back(child);
      }

      // Every "<...>" token in a header value, in order. Message-ID grammar is far
      // richer than this, but the angle brackets are the one part every producer
      // agrees on, and threading only needs the ids to MATCH EACH OTHER - the same
      // extraction is applied to Message-ID and References, so a peculiar id still
      // threads against itself.
      void
      ExtractMessageIds_(const String &headerValue, std::vector<String> &ids)
      {
         int position = 0;

         while (true)
         {
            int start = headerValue.Find(_T("<"), position);
            if (start < 0)
               return;

            int end = headerValue.Find(_T(">"), start + 1);
            if (end < 0)
               return;

            String id = headerValue.Mid(start, end - start + 1);

            if (id.GetLength() > 2)
               ids.push_back(id);

            position = end + 1;
         }
      }

      String
      ExtractFirstMessageId_(const String &headerValue)
      {
         std::vector<String> ids;
         ExtractMessageIds_(headerValue, ids);

         return ids.empty() ? String(_T("")) : ids[0];
      }

      bool
      IsWsp_(wchar_t c)
      {
         return c == ' ' || c == '\t';
      }

      // Whether the text of value from position on begins with word, case
      // ignored as CompareNoCase ignores it, no further than end.
      bool
      BeginsWithNoCase_(const String &value, int position, int end, const wchar_t *word)
      {
         const std::ctype<wchar_t> &ct = std::use_facet<std::ctype<wchar_t> >(std::locale::classic());

         for (int i = 0; word[i] != 0; i++)
         {
            if (position + i >= end || ct.tolower(value.GetAt(position + i)) != ct.tolower(word[i]))
               return false;
         }

         return true;
      }

      // Matches "[...]" *WSP at position, answering the position after it, or -1;
      // nothing at or past end is read.
      // A blob holds no bracket of either kind (RFC 5256: subj-blob = "["
      // *BLOBCHAR "]", BLOBCHAR any CHAR but "[" and "]"), so "[fwd: [x] y]" is
      // not one. Taking the first "]" whatever came before it made the step-4
      // strip turn "[fwd: [fwd: re: [fwd: babylon]]]" into "]]", and SORT
      // (SUBJECT) put it after "[xyz]" instead of beside "b" (imaptest's
      // sort-subject). The first bracket after the "[" decides: a "]" closes the
      // blob, a "[" means it is none - which is what looking for the first of
      // each answered, without reading on to a "]" far beyond a "[" near.
      int
      MatchSubjBlob_(const String &value, int position, int end)
      {
         if (position >= end || value.GetAt(position) != '[')
            return -1;

         int close = -1;
         for (int i = position + 1; i < end; i++)
         {
            const wchar_t c = value.GetAt(i);

            if (c == '[')
               return -1;

            if (c == ']')
            {
               close = i;
               break;
            }
         }

         if (close < 0)
            return -1;

         int after = close + 1;
         while (after < end && IsWsp_(value.GetAt(after)))
            after++;

         return after;
      }

      // Matches ("re" / "fwd" / "fw") *WSP [subj-blob] ":" at position, answering
      // the position after the colon, or -1; nothing at or past end is read.
      int
      MatchSubjRefwd_(const String &value, int position, int end)
      {
         int wordLength = 0;
         if (BeginsWithNoCase_(value, position, end, L"fwd"))
            wordLength = 3;
         else if (BeginsWithNoCase_(value, position, end, L"re") ||
                  BeginsWithNoCase_(value, position, end, L"fw"))
            wordLength = 2;
         else
            return -1;

         int cursor = position + wordLength;

         while (cursor < end && IsWsp_(value.GetAt(cursor)))
            cursor++;

         int afterBlob = MatchSubjBlob_(value, cursor, end);
         if (afterBlob >= 0)
            cursor = afterBlob;

         if (cursor >= end || value.GetAt(cursor) != ':')
            return -1;

         return cursor + 1;
      }

      // The RFC 5256 section 2.1 "base subject" - the subject with every layer of
      // "Re:", "Fwd:", "[listname]" and "(fwd)" peeled off, which is what decides
      // whether two messages belong to the same conversation when no References
      // chain connects them.
      //
      // Implemented as the RFC's own step machine rather than a regular expression,
      // because the steps interact: "[fwd: Re: [x] hello]" unwraps outside-in, and
      // each unwrap can expose new trailers for step 2 and new leaders for step 3.
      //
      // What is left of the subject is value from begin up to end, and a layer
      // peeled moves one of the two. Each used to make a new string of what was
      // left, and the step-3 match copied the rest again: a subject of a hundred
      // thousand "Re: " or "(fwd)" - a sender's, read whenever a client asks to
      // SORT by subject or to THREAD the folder - took a time that grew with the
      // square of its length.
      String
      CalculateBaseSubject_(const String &subject, bool &isReplyOrForward)
      {
         isReplyOrForward = false;

         // Step 1: one canonical form for whitespace, so " Re:  x" and "Re: x"
         // strip identically. The header decoder has already handled RFC 2047.
         String value;
         bool lastWasSpace = false;

         for (int i = 0; i < subject.GetLength(); i++)
         {
            wchar_t c = subject.GetAt(i);

            if (c == '\t' || c == '\r' || c == '\n')
               c = ' ';

            if (c == ' ')
            {
               if (lastWasSpace)
                  continue;
               lastWasSpace = true;
            }
            else
            {
               lastWasSpace = false;
            }

            value += c;
         }

         int begin = 0;
         int end = value.GetLength();

         bool outerChanged = true;

         while (outerChanged)
         {
            outerChanged = false;

            // Step 2: trailing "(fwd)" and whitespace, repeatedly.
            bool trailerChanged = true;
            while (trailerChanged)
            {
               trailerChanged = false;

               while (end > begin && IsWsp_(value.GetAt(end - 1)))
               {
                  end--;
                  trailerChanged = true;
               }

               if (end - begin >= 5 && BeginsWithNoCase_(value, end - 5, end, L"(fwd)"))
               {
                  end -= 5;
                  isReplyOrForward = true;
                  trailerChanged = true;
               }
            }

            // Steps 3 and 4: leading "Re:"/"Fwd:" leaders (with any bracketed blobs
            // inside them), and bare leading "[...]" blobs so long as removing one
            // leaves something behind.
            bool leaderChanged = true;
            while (leaderChanged)
            {
               leaderChanged = false;

               // Step 3's subj-leader is "(*subj-blob subj-refwd) / WSP": leading
               // white space comes off too. Unwrapping "[fwd: ...]" in step 5
               // leaves some, and with it in front nothing below matched, so the
               // second of two nested wrappers was never unwrapped.
               if (begin < end && IsWsp_(value.GetAt(begin)))
               {
                  while (begin < end && IsWsp_(value.GetAt(begin)))
                     begin++;

                  leaderChanged = true;
                  continue;
               }

               // Step 3: *subj-blob subj-refwd ":". The blobs are only consumed if
               // the "re/fwd:" that licenses them actually follows - "[x] hello" is
               // a step-4 blob, not half a leader.
               int cursor = begin;

               while (true)
               {
                  int afterBlob = MatchSubjBlob_(value, cursor, end);
                  if (afterBlob < 0)
                     break;
                  cursor = afterBlob;
               }

               int afterRefwd = MatchSubjRefwd_(value, cursor, end);

               if (afterRefwd >= 0)
               {
                  while (afterRefwd < end && IsWsp_(value.GetAt(afterRefwd)))
                     afterRefwd++;

                  begin = afterRefwd;
                  isReplyOrForward = true;
                  leaderChanged = true;
                  continue;
               }

               // Step 4: a bare leading blob comes off only if a subject remains -
               // "[looking for a job]" IS the whole subject for somebody.
               //
               // All of the run of blobs step 3 just read comes off here, one at a
               // time, while a subject remains after each, as it did a pass of this
               // loop at a time: with one taken off, step 3 read the rest of the
               // same run, met the same text after it, and failed the same way, so
               // only reading the run again, once per blob, is saved.
               int afterBare = MatchSubjBlob_(value, begin, end);

               while (afterBare >= 0 && afterBare < end)
               {
                  begin = afterBare;
                  afterBare = MatchSubjBlob_(value, begin, end);
               }
            }

            // Step 5: the "[fwd: ...]" wrapper, outside-in. Unwrapping exposes a
            // fresh subject, so the whole machine runs again.
            if (end - begin >= 6 &&
                BeginsWithNoCase_(value, begin, end, L"[fwd:") &&
                value.GetAt(end - 1) == ']')
            {
               begin += 5;
               end -= 1;
               isReplyOrForward = true;
               outerChanged = true;
            }
         }

         value = value.Mid(begin, end - begin);
         value.Trim();

         return value;
      }

      // The message a container sorts as: itself, or - for a dummy that survived
      // pruning - its first child, which the children's own sort has already made
      // its earliest ("sort its children by sent date and then use the first child
      // for the top-level sort", RFC 5256 section 3).
      ThreadContainer *
      SortMessage_(ThreadContainer *node)
      {
         ThreadContainer *current = node;
         int steps = 0;

         while (current && !current->message && !current->children.empty() && steps <= kMaxThreadDepth)
         {
            current = current->children.front();
            steps++;
         }

         return (current && current->message) ? current : nullptr;
      }

      // Ascending by sent date (RFC 5256 2.2, with the fallback the caller already
      // resolved into sent_date), and "when comparing two sent dates that match
      // exactly, the order in which the two messages appear in the mailbox (that
      // is, by sequence number) is used as a tie-breaker". The tie used to be left
      // to std::stable_sort, which keeps whatever order the containers happened to
      // be in - creation order under REFERENCES, base-subject order under
      // ORDEREDSUBJECT - so equal dates came out in an order nothing specified.
      struct AscendingBySentDate
      {
         bool operator()(ThreadContainer *left, ThreadContainer *right) const
         {
            ThreadContainer *leftMessage = SortMessage_(left);
            ThreadContainer *rightMessage = SortMessage_(right);

            DateTime dl = leftMessage ? leftMessage->sent_date : DateTime();
            DateTime dr = rightMessage ? rightMessage->sent_date : DateTime();

            if (dl < dr)
               return true;

            if (dr < dl)
               return false;

            const int sl = leftMessage ? leftMessage->sequence : 0;
            const int sr = rightMessage ? rightMessage->sequence : 0;

            return sl < sr;
         }
      };

      // Children before parents, so that a dummy's first child is its earliest by
      // the time the dummy itself is placed among its siblings.
      void
      SortChildrenRecursively_(ThreadContainer *node)
      {
         for (ThreadContainer *child : node->children)
            SortChildrenRecursively_(child);

         std::stable_sort(node->children.begin(), node->children.end(), AscendingBySentDate());
      }

      // RFC 5256's pruning walk. virtualRoot's children are the root set; a dummy
      // below the root is always spliced out, a dummy AT the root survives only
      // while it has two or more children - it is the thing that holds two sibling
      // replies together when their common parent is missing.
      void
      Prune_(ThreadContainer *node, ThreadContainer *virtualRoot)
      {
         size_t i = 0;

         while (i < node->children.size())
         {
            ThreadContainer *child = node->children[i];

            Prune_(child, virtualRoot);

            if (!child->message && child->children.empty())
            {
               node->children.erase(node->children.begin() + i);
               continue;
            }

            if (!child->message &&
                (node != virtualRoot || child->children.size() == 1))
            {
               // Splice the grandchildren into the dummy's place, preserving order.
               std::vector<ThreadContainer*> grandChildren = child->children;

               for (ThreadContainer *grandChild : grandChildren)
                  grandChild->parent = node;

               node->children.erase(node->children.begin() + i);
               node->children.insert(node->children.begin() + i,
                                     grandChildren.begin(), grandChildren.end());

               // The spliced items were already pruned by the recursion above, so
               // the loop can move across them normally.
               continue;
            }

            i++;
         }
      }

      // The base subject a whole thread files under: the root's, or - when the
      // root is a dummy - its first child's.
      String
      ThreadSubjectKey_(ThreadContainer *root)
      {
         if (root->message)
            return root->base_subject_key;

         if (!root->children.empty())
            return root->children.front()->base_subject_key;

         return String(_T(""));
      }

      bool
      ThreadIsReply_(ThreadContainer *root)
      {
         if (root->message)
            return root->is_reply_or_forward;

         if (!root->children.empty())
            return root->children.front()->is_reply_or_forward;

         return false;
      }

      // One node's rendering, without its own parentheses: the chain of numbers
      // down single-child links, then each branch as a nested list. RFC 5256's
      // thread-members / thread-nested grammar, written to match its examples -
      // "(3 6 (4 23)(44 7 96))" - and, for a dummy root, "((3)(5))".
      void
      RenderContainer_(ThreadContainer *node, bool useUid, String &out)
      {
         ThreadContainer *current = node;
         bool first = true;

         while (current->message)
         {
            String number;

            if (useUid)
               number.Format(_T("%u"), current->message->GetUID());
            else
               number.Format(_T("%d"), current->sequence);

            if (!first)
               out += _T(" ");

            out += number;
            first = false;

            // The chain continues while there is exactly one child. Anything else -
            // none, or a branch - is handled below.
            if (current->children.size() != 1)
               break;

            current = current->children.front();
         }

         if (current->message && current->children.size() == 1)
         {
            // Unreachable by construction (the loop above only leaves a
            // single-child node by walking into it), but the render must never
            // drop a message silently, so the belt stays.
            current = current->children.front();
            RenderContainer_(current, useUid, out);
            return;
         }

         if (current->children.size() > 1 ||
             (!current->message && !current->children.empty()))
         {
            if (!first)
               out += _T(" ");

            for (ThreadContainer *child : current->children)
            {
               out += _T("(");
               RenderContainer_(child, useUid, out);
               out += _T(")");
            }
         }
      }
   }

   String
   IMAPThread::GetBaseSubject(const String &subject)
   {
      bool isReplyOrForward = false;
      return CalculateBaseSubject_(subject, isReplyOrForward);
   }

   String
   IMAPThread::GetBaseSubject(const String &subject, bool &isReplyOrForward)
   {
      return CalculateBaseSubject_(subject, isReplyOrForward);
   }

   bool
   IMAPThread::ParseAlgorithm(const String &name, Algorithm &algorithm)
   {
      if (name.CompareNoCase(_T("ORDEREDSUBJECT")) == 0)
      {
         algorithm = AlgorithmOrderedSubject;
         return true;
      }

      if (name.CompareNoCase(_T("REFERENCES")) == 0)
      {
         algorithm = AlgorithmReferences;
         return true;
      }

      return false;
   }

   bool
   IMAPThread::BuildThreads(std::shared_ptr<IMAPConnection> pConnection,
                            const std::vector<std::pair<int, std::shared_ptr<Message> > > &vecMessages,
                            bool useUid, Algorithm algorithm,
                            const std::function<bool()> &abortRequested,
                            String &responseBody)
   {
      responseBody.Empty();

      if (vecMessages.empty())
         return true;

      // The arena owns every container; raw parent/child pointers between them are
      // safe because nothing is destroyed until the arena goes out of scope.
      std::vector<std::unique_ptr<ThreadContainer> > arena;
      arena.reserve(vecMessages.size());

      auto makeContainer = [&arena]() -> ThreadContainer*
      {
         arena.push_back(std::unique_ptr<ThreadContainer>(new ThreadContainer()));
         return arena.back().get();
      };

      std::map<String, ThreadContainer*> byMessageId;
      std::vector<ThreadContainer*> searchedContainers;

      // ------------------------------------------------------------------ load +
      // link. One header read per matched message serves everything: the id, the
      // ancestry, the subject and the date.
      for (const std::pair<int, std::shared_ptr<Message> > &messagePair : vecMessages)
      {
         // Asked BEFORE the read, so the ceiling stops the next file rather than
         // being consulted once every file has already been read.
         if (abortRequested && abortRequested())
            return false;

         const int sequence = messagePair.first;
         const std::shared_ptr<Message> pMessage = messagePair.second;

         AnsiString sHeader = MessageStore::LoadHeader(pConnection->GetAccountOwningCurrentFolder(), pMessage);
         MimeHeader oHeader;
         oHeader.Load(sHeader, sHeader.GetLength());

         String messageId = ExtractFirstMessageId_(oHeader.GetUnicodeFieldValue("Message-ID"));

         std::vector<String> references;
         ExtractMessageIds_(oHeader.GetUnicodeFieldValue("References"), references);

         if (references.empty())
         {
            // RFC 5256: References first; only when it is absent does In-Reply-To
            // speak, and then only its first id.
            String inReplyTo = ExtractFirstMessageId_(oHeader.GetUnicodeFieldValue("In-Reply-To"));

            if (!inReplyTo.IsEmpty())
               references.push_back(inReplyTo);
         }

         if (references.size() > kMaxReferencesPerMessage)
            references.erase(references.begin(),
                             references.end() - (int) kMaxReferencesPerMessage);

         // Find or create this message's own container. A missing id, or an id some
         // other message already claimed, means this message threads as itself - RFC
         // 5256 says to treat both as unique.
         ThreadContainer *container = nullptr;

         if (!messageId.IsEmpty())
         {
            auto found = byMessageId.find(messageId);

            if (found == byMessageId.end())
            {
               container = makeContainer();
               byMessageId[messageId] = container;
            }
            else if (!found->second->message)
            {
               container = found->second;
            }
         }

         if (!container)
            container = makeContainer();

         container->message = pMessage;
         container->sequence = sequence;

         // RFC 5256 2.2: the sent date, and INTERNALDATE when it "cannot be
         // determined". That covers a header that is absent AND one that is present
         // and unparseable - which is the commoner case, because a broken client
         // writes something, it just is not a date.
         //
         // Testing emptiness alone is not enough, and the failure is silent:
         // Time::GetDateTimeFromMimeHeader hands back a default DateTime for
         // anything it cannot read, whose raw value is the 1899 OLE epoch, and the
         // comparator sorts on that raw value without consulting the status flag.
         // So every message with a malformed Date would sort to the very front of
         // its siblings, all tied with each other - a thread that opens with its
         // most broken message rather than its first.
         String dateHeader = oHeader.GetUnicodeFieldValue("Date");

         container->sent_date = dateHeader.IsEmpty()
            ? DateTime()
            : Time::GetDateTimeFromMimeHeader(dateHeader);

         if (container->sent_date.GetStatus() != DateTime::valid)
            container->sent_date = Time::GetDateFromSystemDate(pMessage->GetCreateTime());

         String baseSubject = CalculateBaseSubject_(oHeader.GetUnicodeFieldValue("Subject"),
                                                    container->is_reply_or_forward);
         baseSubject.ToUpper();
         container->base_subject_key = baseSubject;

         searchedContainers.push_back(container);

         if (algorithm == AlgorithmOrderedSubject)
            continue;

         // Link the ancestry chain, oldest first. Each adjacent pair becomes a
         // parent-child link, but only where it invents nothing: a container that
         // already has a parent keeps it, and a link that would close a cycle is
         // refused outright.
         ThreadContainer *previous = nullptr;

         for (const String &referenceId : references)
         {
            ThreadContainer *referenced = nullptr;

            auto found = byMessageId.find(referenceId);

            if (found != byMessageId.end())
            {
               referenced = found->second;
            }
            else
            {
               referenced = makeContainer();
               byMessageId[referenceId] = referenced;
            }

            if (previous && !referenced->parent && CanLink_(previous, referenced))
               AttachChild_(previous, referenced);

            previous = referenced;
         }

         // This message's parent IS the last reference - authoritatively, per the
         // RFC: an existing parent from somebody else's References line is broken
         // in favour of what the message says about itself.
         //
         // RFC 5256 1.B: "break the current parent/child link before creating the
         // new correct one", and "do not create the parent/child link if creating
         // that link would introduce a loop". The break comes first, so a link
         // refused because it would close a loop still leaves the message cut
         // from the parent somebody else's References gave it - imaptest's thread
         // test, where the message a reply's References placed under the root
         // turns out to be that reply's parent: (3 2)(1), not (1 3 2). Only a link
         // refused for the depth limit keeps the placement it had, rather than
         // cut loose at a depth the stack must never reach.
         if (previous)
         {
            if (IsSelfOrAncestor_(container, previous))
            {
               DetachFromParent_(container);
            }
            else if (CanLink_(previous, container))
            {
               DetachFromParent_(container);
               AttachChild_(previous, container);
            }
         }
         else if (references.empty())
         {
            DetachFromParent_(container);
         }
      }

      // A virtual root whose children are the root set keeps every later phase
      // free of "is this the top level" special cases except the one the RFC
      // actually has (dummies survive at the root).
      ThreadContainer virtualRoot;

      if (algorithm == AlgorithmReferences)
      {
         for (const std::unique_ptr<ThreadContainer> &owned : arena)
         {
            if (!owned->parent)
               AttachChild_(&virtualRoot, owned.get());
         }

         // AttachChild_ set parent pointers to virtualRoot; the pruning walk
         // compares against it by address, so that is exactly right.

         Prune_(&virtualRoot, &virtualRoot);
         SortChildrenRecursively_(&virtualRoot);

         // ------------------------------------------------------- subject grouping.
         // Two roots with the same base subject are one conversation whose
         // connecting References were lost. The merge rules are the RFC's: a dummy
         // absorbs, a reply files under the non-reply it answers, and two equals
         // get a new dummy over both.
         std::map<String, ThreadContainer*> bySubject;
         std::vector<ThreadContainer*> rootSet = virtualRoot.children;

         for (ThreadContainer *root : rootSet)
         {
            String key = ThreadSubjectKey_(root);

            if (key.IsEmpty())
               continue;

            auto existing = bySubject.find(key);

            if (existing == bySubject.end())
            {
               bySubject[key] = root;
               continue;
            }

            ThreadContainer *table = existing->second;

            // Prefer the entry a merge would file things UNDER: a dummy beats a
            // message, a non-reply beats a reply.
            if ((!root->message && table->message) ||
                (table->message && ThreadIsReply_(table) && !ThreadIsReply_(root)))
            {
               bySubject[key] = root;
            }
         }

         for (ThreadContainer *root : rootSet)
         {
            String key = ThreadSubjectKey_(root);

            if (key.IsEmpty())
               continue;

            ThreadContainer *table = bySubject[key];

            if (table == root)
               continue;

            DetachFromParent_(root);

            if (!table->message && !root->message)
            {
               // Two dummies: one keeps both families.
               std::vector<ThreadContainer*> orphans = root->children;

               for (ThreadContainer *orphan : orphans)
               {
                  DetachFromParent_(orphan);
                  AttachChild_(table, orphan);
               }
            }
            else if (!table->message)
            {
               AttachChild_(table, root);
            }
            else if (ThreadIsReply_(root) && !ThreadIsReply_(table))
            {
               AttachChild_(table, root);
            }
            else
            {
               // Neither is the other's answer: a new dummy holds them side by
               // side, and takes the table entry's place in the root set.
               ThreadContainer *dummy = makeContainer();

               size_t tablePosition = 0;
               for (size_t i = 0; i < virtualRoot.children.size(); i++)
               {
                  if (virtualRoot.children[i] == table)
                  {
                     tablePosition = i;
                     break;
                  }
               }

               DetachFromParent_(table);
               AttachChild_(dummy, table);
               AttachChild_(dummy, root);

               dummy->parent = &virtualRoot;
               virtualRoot.children.insert(virtualRoot.children.begin() + tablePosition, dummy);

               bySubject[key] = dummy;
            }
         }

         SortChildrenRecursively_(&virtualRoot);
      }
      else
      {
         // ORDEREDSUBJECT (RFC 5256 section 3): "sorted by base subject and then by
         // the sent date", each run of one base subject a thread. "The top level
         // or "root" ... contains the first message of every thread ... The second
         // message of a thread is the child of the first message, and subsequent
         // messages of the thread are siblings of the second message and hence
         // children of the message at the root" - the RFC's own example is
         // "(174 (175)(176)(178)(181)(180))". Each message used to be made the
         // child of the one before it, a chain, "(174 175 176 178 181 180)": a
         // different tree, with every reply nested one deeper than the last.
         std::stable_sort(searchedContainers.begin(), searchedContainers.end(),
            [](ThreadContainer *left, ThreadContainer *right)
            {
               int subjectCompare = left->base_subject_key.Compare(right->base_subject_key);

               if (subjectCompare != 0)
                  return subjectCompare < 0;

               if (left->sent_date < right->sent_date)
                  return true;

               if (right->sent_date < left->sent_date)
                  return false;

               return left->sequence < right->sequence;
            });

         ThreadContainer *threadRoot = nullptr;

         for (ThreadContainer *container : searchedContainers)
         {
            if (!threadRoot || container->base_subject_key != threadRoot->base_subject_key)
            {
               AttachChild_(&virtualRoot, container);
               threadRoot = container;
            }
            else
            {
               AttachChild_(threadRoot, container);
            }
         }

         // "Finally, the threads are sorted by the sent date of the first message
         // in the thread" - sequence breaking ties, as everywhere. The children of
         // each root are in date order already from the sort above.
         std::stable_sort(virtualRoot.children.begin(), virtualRoot.children.end(),
                          AscendingBySentDate());
      }

      // ------------------------------------------------------------------ render.
      // One space between "THREAD" and the first tree, and NONE between the trees:
      // RFC 5256's own examples read "(2)(3 6 (4 23)(44 7 96))", adjacent. The
      // first version of this loop put a space before every root, and the fixture
      // caught it - a threading client tokenises this response character by
      // character, so a stray space is a parse error in somebody's inbox, not
      // cosmetics.
      for (ThreadContainer *root : virtualRoot.children)
      {
         responseBody += responseBody.IsEmpty() ? _T(" (") : _T("(");
         RenderContainer_(root, useUid, responseBody);
         responseBody += _T(")");
      }

      return true;
   }
   namespace
   {
      // The base subject as 6.3.5 calculated it, word for word but for the
      // names and the comments: what the one by offsets is held to.
      int
      OldMatchSubjBlob_(const String &value, int position)
      {
         if (position >= value.GetLength() || value.GetAt(position) != '[')
            return -1;

         int close = value.Find(_T("]"), position + 1);
         if (close < 0)
            return -1;

         int nested = value.Find(_T("["), position + 1);
         if (nested >= 0 && nested < close)
            return -1;

         int after = close + 1;
         while (after < value.GetLength() && IsWsp_(value.GetAt(after)))
            after++;

         return after;
      }

      int
      OldMatchSubjRefwd_(const String &value, int position)
      {
         String rest = value.Mid(position);

         int wordLength = 0;
         if (rest.Mid(0, 3).CompareNoCase(_T("fwd")) == 0)
            wordLength = 3;
         else if (rest.Mid(0, 2).CompareNoCase(_T("re")) == 0 ||
                  rest.Mid(0, 2).CompareNoCase(_T("fw")) == 0)
            wordLength = 2;
         else
            return -1;

         int cursor = position + wordLength;

         while (cursor < value.GetLength() && IsWsp_(value.GetAt(cursor)))
            cursor++;

         int afterBlob = OldMatchSubjBlob_(value, cursor);
         if (afterBlob >= 0)
            cursor = afterBlob;

         if (cursor >= value.GetLength() || value.GetAt(cursor) != ':')
            return -1;

         return cursor + 1;
      }

      String
      OldCalculateBaseSubject_(const String &subject, bool &isReplyOrForward)
      {
         isReplyOrForward = false;

         String value;
         bool lastWasSpace = false;

         for (int i = 0; i < subject.GetLength(); i++)
         {
            wchar_t c = subject.GetAt(i);

            if (c == '\t' || c == '\r' || c == '\n')
               c = ' ';

            if (c == ' ')
            {
               if (lastWasSpace)
                  continue;
               lastWasSpace = true;
            }
            else
            {
               lastWasSpace = false;
            }

            value += c;
         }

         bool outerChanged = true;

         while (outerChanged)
         {
            outerChanged = false;

            bool trailerChanged = true;
            while (trailerChanged)
            {
               trailerChanged = false;

               int length = value.GetLength();

               while (length > 0 && IsWsp_(value.GetAt(length - 1)))
                  length--;

               if (length != value.GetLength())
               {
                  value = value.Mid(0, length);
                  trailerChanged = true;
               }

               if (value.GetLength() >= 5 &&
                   value.Mid(value.GetLength() - 5).CompareNoCase(_T("(fwd)")) == 0)
               {
                  value = value.Mid(0, value.GetLength() - 5);
                  isReplyOrForward = true;
                  trailerChanged = true;
               }
            }

            bool leaderChanged = true;
            while (leaderChanged)
            {
               leaderChanged = false;

               int leading = 0;
               while (leading < value.GetLength() && IsWsp_(value.GetAt(leading)))
                  leading++;

               if (leading > 0)
               {
                  value = value.Mid(leading);
                  leaderChanged = true;
                  continue;
               }

               int cursor = 0;

               while (true)
               {
                  int afterBlob = OldMatchSubjBlob_(value, cursor);
                  if (afterBlob < 0)
                     break;
                  cursor = afterBlob;
               }

               int afterRefwd = OldMatchSubjRefwd_(value, cursor);

               if (afterRefwd >= 0)
               {
                  while (afterRefwd < value.GetLength() && IsWsp_(value.GetAt(afterRefwd)))
                     afterRefwd++;

                  value = value.Mid(afterRefwd);
                  isReplyOrForward = true;
                  leaderChanged = true;
                  continue;
               }

               int afterBare = OldMatchSubjBlob_(value, 0);

               if (afterBare >= 0 && afterBare < value.GetLength())
               {
                  value = value.Mid(afterBare);
                  leaderChanged = true;
               }
            }

            if (value.GetLength() >= 6 &&
                value.Mid(0, 5).CompareNoCase(_T("[fwd:")) == 0 &&
                value.GetAt(value.GetLength() - 1) == ']')
            {
               value = value.Mid(5, value.GetLength() - 6);
               isReplyOrForward = true;
               outerChanged = true;
            }
         }

         value.Trim();

         return value;
      }

      // xorshift64*: the same cases on every run and every platform.
      unsigned long long NextSubjectRandom_(unsigned long long &state)
      {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return state * 2685821657736338717ULL;
      }
   }

   void
   IMAPThreadSubjectTester::Test(int cases)
   {
      // The time first: subjects of a hundred thousand layers. 6.3.5 copied
      // what was left of the subject for every layer it peeled.
      struct Timed
      {
         const char *what;
         const wchar_t *repeated;
         int count;
         const wchar_t *after;
         const wchar_t *closing;
         const wchar_t *expected;
      };

      static const Timed timed[] =
      {
         { "100,000 \"Re: \"", L"Re: ", 100000, L"hello", L"", L"hello" },
         { "100,000 \"(fwd)\"", L"(fwd)", 100000, L"", L"", L"" },
         { "100,000 \"[list] \"", L"[list] ", 100000, L"hello", L"", L"hello" },
         { "20,000 \"[fwd: \"", L"[fwd: ", 20000, L"hello", L"]", L"hello" },
      };

      for (const Timed &one : timed)
      {
         String subject;
         for (int i = 0; i < one.count; i++)
            subject += one.repeated;
         subject += one.after;
         for (int i = 0; i < one.count && one.closing[0] != 0; i++)
            subject += one.closing;

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         bool isReplyOrForward = false;
         String base = IMAPThread::GetBaseSubject(subject, isReplyOrForward);
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (base != one.expected)
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
         {
            throw std::logic_error(std::string("The base subject of ") + one.what + " took " + std::to_string((long long) milliseconds) + " ms");
         }
      }

      // Then generated subjects: the base subject 6.3.5 made, and whether it
      // was a reply's or a forward's.
      static const wchar_t *pieces[] =
      {
         L"Re", L"re", L"RE", L"Fwd", L"FWD", L"fw", L"Fw", L":", L" ", L"\t", L"[", L"]",
         L"(fwd)", L"(FwD)", L"[fwd:", L"[FWD:", L"x", L"y", L"list", L"\r\n", L"(", L")", L"  "
      };
      const int pieceCount = (int) (sizeof(pieces) / sizeof(pieces[0]));

      unsigned long long state = 0x6A09E667F3BCC909ULL;

      for (int number = 0; number < cases; number++)
      {
         String subject;
         const int length = (int) (NextSubjectRandom_(state) % 20);
         for (int i = 0; i < length; i++)
            subject += pieces[NextSubjectRandom_(state) % pieceCount];

         bool wasReplyOrForward = false;
         String expected = OldCalculateBaseSubject_(subject, wasReplyOrForward);

         bool isReplyOrForward = false;
         String got = IMAPThread::GetBaseSubject(subject, isReplyOrForward);

         if (got != expected || isReplyOrForward != wasReplyOrForward)
            throw std::logic_error("The base subject was other than 6.3.5's in case " + std::to_string(number));
      }
   }
}
