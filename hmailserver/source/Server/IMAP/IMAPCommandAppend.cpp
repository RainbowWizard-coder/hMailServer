// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPCommandAppend.h"
#include "IMAPConnection.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/Sieve/SieveParser.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/IMAPFolders.h"
#include "../Common/BO/Message.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Application/EffectiveSetting.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/MailboxKeys.h"
#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Tracking/NotificationServer.h"
#include "../Common/Util/ByteBuffer.h"
#include "../Common/Util/DiskSpace.h"
#include "../Common/Util/File.h"
#include "../Common/Util/FileInfo.h"
#include "../Common/Util/Time.h"
#include "../SMTP/SMTPConfiguration.h"

#include "IMAPSimpleCommandParser.h"
#include "MessagesContainer.h"
#include "IMAPFolderView.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   IMAPCommandAppend::IMAPCommandAppend() :
      bytes_left_to_receive_(0)
   {
   }

   IMAPCommandAppend::~IMAPCommandAppend()
   {
      // A connection that goes away between a literal and the command's final
      // CRLF - the client closed it, the network dropped, the idle timeout -
      // leaves here with every literal already received placed in the store
      // (CompleteCurrentMessage_) and no row referring to any of them. Nothing
      // else ever removes a message file without a row, and the files counted
      // against no quota, so each abandoned APPEND used to leave up to the
      // maximum message size on the data volume for good, with its blob
      // references held.
      try
      {
         CleanupPendingMessages_();
      }
      catch (...)
      {

      }

      try
      {
         KillCurrentMessage_();
      }
      catch (...)
      {

      }
   }

   void
   IMAPCommandAppend::KillCurrentMessage_()
   {
      if (!current_message_)
         return;

      if (FileUtilities::Exists(message_file_name_))
         FileUtilities::DeleteFile(message_file_name_);

      // The name belongs to the message just dropped and to nothing after it.
      message_file_name_.Empty();

      RemoveAppendMarker_(current_marker_);
   }

   // The markers this process's commands hold. A marker in the directory and
   // not in here is a dead process's - or one a command could not remove, which
   // the sweep treats the same way, since the row decides.
   static boost::mutex &AppendMarkersMutex()
   {
      static boost::mutex mutex;
      return mutex;
   }

   static std::set<String> &AppendMarkersHeld()
   {
      static std::set<String> held;
      return held;
   }

   String
   IMAPCommandAppend::AppendMarkerDirectory_()
   {
      return FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(), _T("IMAPAppend"));
   }

   String
   IMAPCommandAppend::WriteAppendMarker_(const String &messageFile, bool placed, const String &existingMarker)
   {
      // One small text file per message received and not yet saved, named after
      // the message file, holding what the sweep needs: whether the file has
      // been placed in the store (a placed one lets go of what it references
      // when it is removed; one not yet placed is the client's bytes, whatever
      // they begin with, and is only discarded), and the file itself. UTF-8,
      // since a data directory's path may hold any character.
      const String directory = AppendMarkerDirectory_();
      if (!FileUtilities::Exists(directory) && !FileUtilities::CreateDirectory(directory))
         return String();

      const String marker = existingMarker.IsEmpty()
         ? FileUtilities::Combine(directory, FileUtilities::GetFileNameFromFullPath(messageFile) + _T(".append"))
         : existingMarker;

      String text;
      text.Format(_T("hMailServer IMAP APPEND in progress\nstate=%s\nfile=%s\n"),
         placed ? _T("placed") : _T("working"), messageFile.c_str());

      AnsiString utf8;
      if (!Unicode::WideToMultiByte(text, utf8))
         return String();

      {
         boost::lock_guard<boost::mutex> guard(AppendMarkersMutex());
         AppendMarkersHeld().insert(marker);
      }

      if (!FileUtilities::WriteToFileAtomically(marker, utf8))
      {
         // A marker that says "working" is left as it is when "placed" cannot
         // replace it: the sweep then discards the file without letting go of
         // its references, which leaks them rather than taking another
         // message's.
         if (existingMarker.IsEmpty())
         {
            boost::lock_guard<boost::mutex> guard(AppendMarkersMutex());
            AppendMarkersHeld().erase(marker);
            return String();
         }
      }

      return marker;
   }

   void
   IMAPCommandAppend::RemoveAppendMarker_(String &markerFile)
   {
      if (markerFile.IsEmpty())
         return;

      if (FileUtilities::Exists(markerFile))
         FileUtilities::DeleteFile(markerFile);

      {
         boost::lock_guard<boost::mutex> guard(AppendMarkersMutex());
         AppendMarkersHeld().erase(markerFile);
      }

      markerFile.Empty();
   }

   void
   IMAPCommandAppend::SweepAbandonedAppend_(const String &directory, const String &dataDirectory, const String &markerName)
   {
      String marker = FileUtilities::Combine(directory, markerName);

      {
         boost::lock_guard<boost::mutex> guard(AppendMarkersMutex());
         if (AppendMarkersHeld().find(marker) != AppendMarkersHeld().end())
            return;
      }

      // Read and checked before anything is removed. A marker that does not
      // say exactly what this server writes names nothing it will touch: it
      // goes, and whatever file it names stays.
      String text;
      {
         File markerFile;
         std::shared_ptr<ByteBuffer> bytes;
         if (markerFile.Open(marker, File::OTReadOnly))
            bytes = markerFile.ReadFile();
         if (bytes && bytes->GetSize() > 0)
            Unicode::MultiByteToWide(AnsiString(bytes->GetCharBuffer(), bytes->GetSize()), text);
      }

      String state;
      String file;
      bool header = false;
      std::vector<String> lines = StringParser::SplitString(text, _T("\n"));
      for (const String &line : lines)
      {
         if (line == _T("hMailServer IMAP APPEND in progress"))
            header = true;
         else if (line.StartsWith(_T("state=")))
            state = line.Mid(6);
         else if (line.StartsWith(_T("file=")))
            file = line.Mid(5);
      }

      // The file must be a message file of the store - under the data
      // directory, not the queue directly in it, named as the marker is,
      // ending ".eml" and climbing out of nothing, and at the place a message
      // of an account or of the public folders is kept (domain, mailbox and the
      // two-character fan-out, or #Public and the fan-out): GetPartialFilename
      // answers for exactly those. Being under the data directory is not enough
      // on its own: the Linux packages keep the temp, event and database
      // directories inside it.
      const String expectedName = markerName.Left(markerName.GetLength() - 7);
      const String fileDirectory = FileUtilities::GetFilePath(file);
      const String prefix = dataDirectory + FileUtilities::PathSeparator;
#ifdef HM_PLATFORM_POSIX
      const bool underData = file.GetLength() > prefix.GetLength() && file.Left(prefix.GetLength()) == prefix;
      const bool inQueue = fileDirectory == dataDirectory;
      const bool namedAsMarker = FileUtilities::GetFileNameFromFullPath(file) == expectedName;
#else
      const bool underData = file.GetLength() > prefix.GetLength() && file.Left(prefix.GetLength()).CompareNoCase(prefix) == 0;
      const bool inQueue = fileDirectory.CompareNoCase(dataDirectory) == 0;
      const bool namedAsMarker = FileUtilities::GetFileNameFromFullPath(file).CompareNoCase(expectedName) == 0;
#endif
      String partialName;
      const bool inStoreLayout = PersistentMessage::GetPartialFilename(file, partialName) &&
         partialName.CompareNoCase(expectedName) == 0;

      const bool wellFormed = header && (state == _T("working") || state == _T("placed")) &&
         underData && !inQueue && namedAsMarker && inStoreLayout && expectedName.EndsWith(_T(".eml")) &&
         file.Find(_T("..")) < 0;

      if (!wellFormed)
      {
         LOG_APPLICATION("IMAP APPEND: the marker " + marker + " is not one this server wrote in full; it is removed and the file it names, if any, is left alone.");
         FileUtilities::DeleteFile(marker);
         return;
      }

      // A row that names the file means the APPEND finished and only the
      // marker was left - the process stopped between the row and the marker,
      // or the marker could not be removed. The file is then mail. Any row
      // of any account, by the name a row stores (the partial name, or the
      // full path an old row may hold): a file some row names is never
      // removed here, whatever the marker says. Not indexed, and asked only
      // for the markers a stopped process left - a handful.
      SQLCommand command("select messageid from hm_messages where messagefilename = @PARTIALNAME or messagefilename = @FULLNAME");
      command.AddParameter("@PARTIALNAME", partialName);
      command.AddParameter("@FULLNAME", file);

      std::shared_ptr<DALRecordset> rows = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!rows)
      {
         // Not known: kept, marker and file, for the next start to ask again.
         return;
      }

      if (rows->IsEOF())
      {
         if (state == _T("placed"))
            MessageStore::Remove(file);
         else
            MessageStore::DiscardWorkingFile(file);

         LOG_APPLICATION("IMAP APPEND: removed " + file + ", the message of an APPEND the server stopped in the middle of; no row names it.");
      }

      FileUtilities::DeleteFile(marker);
   }

   void
   IMAPCommandAppend::SweepAbandonedAppends()
   {
      const String directory = AppendMarkerDirectory_();
      if (!FileUtilities::Exists(directory))
         return;

      String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();
      while (dataDirectory.EndsWith(_T("\\")) || dataDirectory.EndsWith(_T("/")))
         dataDirectory = dataDirectory.Left(dataDirectory.GetLength() - 1);
      if (dataDirectory.IsEmpty())
         return;

      std::vector<FileInfo> markers = FileUtilities::GetFilesInDirectory(directory, _T(".*\\.append"));
      for (FileInfo &entry : markers)
      {
         // One marker a stopped process left cannot stop the server starting,
         // nor the sweep of the others: whatever it throws is logged and the
         // marker left for the next start.
         try
         {
            SweepAbandonedAppend_(directory, dataDirectory, entry.GetName());
         }
         catch (...)
         {
            LOG_APPLICATION("IMAP APPEND: the marker " + FileUtilities::Combine(directory, entry.GetName()) + " could not be dealt with; it is left for the next start.");
         }
      }
   }

   IMAPResult
   IMAPCommandAppend::ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pConnection->IsAuthenticated())
         return IMAPResult(IMAPResult::ResultNo, "Authenticate first");

      current_tag_ = pArgument->Tag();

      // Reset these two so we don't re-use old values.
      flags_to_set_ = "";
      create_time_to_set_ = "";

      // RFC 8508 (REPLACE): the same machinery as APPEND with a target message
      // parameter in front, and the target's removal on success. The UID form
      // arrives via the UID command handler, which sets replace_uid_mode_ for
      // exactly one command.
      bool replaceUsesUid = replace_uid_mode_;
      replace_uid_mode_ = false;
      replace_mode_ = false;
      replace_target_.reset();

      // RFC 6855 section 4 (UTF8=ACCEPT): a client may send the message as
      //
      //    APPEND mailbox [flags] [date] UTF8 (~{n}
      //
      // with the closing ")" following the octets on the wire. Thunderbird 128+
      // does exactly this for every Sent copy once UTF8=ACCEPT is enabled, and
      // the simple parser refused the line outright - its parenthesis count is
      // odd until the octets have gone by - so the server answered "BAD APPEND
      // Command requires at least 2 parameter" and no Sent copy was ever stored
      // (issue #53). The wrapper is stripped here, leaving the plain literal8
      // form the rest of this handler already understands, and the ")" is taken
      // from the continuation text once the octets have arrived.
      String sCommand = pArgument->Command();
      utf8_wrapped_literal_ = StripUtf8LiteralWrapper_(sCommand);
      if (utf8_wrapped_literal_)
         pArgument->Command(sCommand);

      std::shared_ptr<IMAPSimpleCommandParser> pParser = std::shared_ptr<IMAPSimpleCommandParser>(new IMAPSimpleCommandParser());

      pParser->Parse(pArgument);

      if (pParser->WordCount() < 1)
         return IMAPResult(IMAPResult::ResultBad, "Command requires parameters.");

      bool isReplace = pParser->Word(0)->Value().CompareNoCase(_T("REPLACE")) == 0;

      // The folder name's position shifts by one when a target parameter
      // precedes it.
      size_t folderWordIndex = isReplace ? 2 : 1;
      size_t folderParamIndex = isReplace ? 1 : 0;

      if (pParser->WordCount() < folderWordIndex + 2)
         return IMAPResult(IMAPResult::ResultBad, isReplace
            ? "REPLACE Command requires at least 3 parameters."
            : "APPEND Command requires at least 2 parameter.");

      // The literal is read first, before anything that can refuse the command:
      // a refusal of a {n+} literal has to consume the octets the client is
      // already sending (RefuseWithLiteral_), and only the size says how many.
      //
      // last word.
      std::shared_ptr<IMAPSimpleWord> pWord = pParser->Word(pParser->WordCount()-1);

      if (!pWord)
         return IMAPResult(IMAPResult::ResultBad, "Missing literal");

      AnsiString literalSize;

      if (pWord->Clammerized())
      {
         literalSize = pWord->Value();
      }
      else
      {
         // RFC 3516 (BINARY): the literal8 form "~{n}" marks content that may
         // contain NUL octets. The simple parser reads it as a plain word, so
         // it is recognised here and treated exactly like a literal marker -
         // the binary receive path stores bytes as bytes either way.
         String sWordValue = pWord->Value();

         if (sWordValue.GetLength() > 3 &&
             sWordValue.GetAt(0) == '~' &&
             sWordValue.GetAt(1) == '{' &&
             sWordValue.Right(1) == _T("}"))
         {
            literalSize = sWordValue.Mid(2, sWordValue.GetLength() - 3);
         }
         else
         {
            return IMAPResult(IMAPResult::ResultBad, "Missing literal");
         }
      }

      // Strip a trailing '+' (non-synchronizing literal, RFC 7888) before
      // validating - and remember it: the client is not waiting for a
      // continuation before it sends the message octets.
      bool nonSynchronizingLiteral = false;
      if (literalSize.GetLength() > 0 && literalSize.GetAt(literalSize.GetLength() - 1) == '+')
      {
         literalSize = literalSize.Mid(0, literalSize.GetLength() - 1);
         nonSynchronizingLiteral = true;
      }

      // The octet count must be a plain non-negative integer; reject anything else
      // so atoi cannot overflow into a bogus (possibly enormous) size_t value.
      if (literalSize.IsEmpty())
         return IMAPResult(IMAPResult::ResultBad, "Invalid literal size.");

      for (int i = 0; i < literalSize.GetLength(); i++)
      {
         char ch = literalSize.GetAt(i);
         if (ch < '0' || ch > '9')
            return IMAPResult(IMAPResult::ResultBad, "Invalid literal size.");
      }

      __int64 declaredSize = _atoi64(literalSize);
      if (declaredSize <= 0)
         return IMAPResult(IMAPResult::ResultBad, "Empty message not permitted.");

      // A fresh command: forget everything a previous APPEND on this connection
      // left behind. Before the first refusal below, which may set some of it.
      // A message still pending was never saved, so its file goes with it.
      CleanupPendingMessages_();
      command_failed_ = false;
      failure_response_.Empty();
      connection_closing_ = false;
      continuation_line_.Empty();
      receive_state_ = ReceivingLiteral;
      write_failed_ = false;
      append_buffer_.Empty();
      KillCurrentMessage_();
      current_message_.reset();
      destination_folder_.reset();
      replace_mode_ = isReplace;

      if (isReplace)
      {
         std::shared_ptr<IMAPFolder> pCurrentFolder = pConnection->GetCurrentFolder();
         if (!pCurrentFolder)
            return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultBad, "REPLACE is only valid in the selected state."), nonSynchronizingLiteral, declaredSize);

         if (pConnection->GetCurrentFolderReadOnly())
            return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultNo, "REPLACE command on read-only folder."), nonSynchronizingLiteral, declaredSize);

         // Removing the replaced message needs the same permission EXPUNGE does.
         // NO, not BAD: the command is well formed and the server declines it
         // (RFC 4314 - a missing right is refused NO).
         if (!pConnection->CheckPermission(pCurrentFolder, ACLPermission::PermissionExpunge))
            return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Expunge permission denied (Required for REPLACE command)."), nonSynchronizingLiteral, declaredSize);

         String sTarget = pParser->Word(1)->Value();

         bool targetIsNumber = !sTarget.IsEmpty();
         for (int i = 0; i < sTarget.GetLength(); i++)
         {
            wchar_t ch = sTarget.GetAt(i);
            if (ch < '0' || ch > '9')
               targetIsNumber = false;
         }

         __int64 target = targetIsNumber ? _ttoi64(sTarget.c_str()) : 0;
         if (target <= 0)
            return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultBad, "REPLACE target must be a message number."), nonSynchronizingLiteral, declaredSize);

         auto currentMessages = MessagesContainer::Instance()->GetMessages(pCurrentFolder->GetAccountID(), pCurrentFolder->GetID());

         // Resolved against this session's view, like every other message number: the
         // number the client sent means the message it was last told is there.
         std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
         IMAPViewEntry entry;
         int sequence = 0;

         bool found = view && (replaceUsesUid
            ? view->GetEntryByUID((unsigned int) target, sequence, entry)
            : view->GetEntryBySequence((int) target, entry));

         if (found)
            replace_target_ = currentMessages->GetCopyByDBID(entry.message_id);

         if (!replace_target_)
            return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultNo, "No such message."), nonSynchronizingLiteral, declaredSize);
      }

         // Create a new mailbox
      String sFolderName = pParser->GetParamValue(pArgument, (int) folderParamIndex);
      if (!pParser->Word(folderWordIndex)->Clammerized())
         IMAPFolder::UnescapeFolderString(sFolderName);

      if (pParser->ParantheziedWord())
         flags_to_set_ = pParser->ParantheziedWord()->Value();

      // Locate the parameter containing the date to set.
      // Can't use pParser->QuotedWord() since there may
      // be many quoted words in the command. Start past the folder name, which
      // may itself be quoted.
      for (size_t i = folderWordIndex + 1; i < pParser->WordCount(); i++)
      {
         std::shared_ptr<IMAPSimpleWord> pDateWord = pParser->Word(i);

         if (pDateWord->Quoted())
         {
            create_time_to_set_ = pDateWord->Value();

            // date-day-fixed  = (SP DIGIT) / 2DIGIT
            //   ; Fixed-format version of date-day
            // If the date given starts with <space>number, we need
            // to Trim. Doesn't hurt to always do this.
            create_time_to_set_.TrimLeft();
         }
      }

      destination_folder_ = pConnection->GetFolderByFullPath(sFolderName);
      if (!destination_folder_)
         return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultNo, "[TRYCREATE] Folder could not be found."), nonSynchronizingLiteral, declaredSize);

      // NO, not BAD: a missing right is a refusal of a well-formed command
      // (RFC 4314 section 4), and BAD tells the client its command was broken.
      if (!pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionInsert))
         return RefuseWithLiteral_(pConnection, IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Insert permission denied (Required for APPEND command)."), nonSynchronizingLiteral, declaredSize);

      IMAPResult prepareResult = ValidateAndPrepareMessage_(pConnection, declaredSize);
      if (prepareResult.GetResult() != IMAPResult::ResultOK)
         return RefuseWithLiteral_(pConnection, prepareResult, nonSynchronizingLiteral, declaredSize);

      pConnection->SetReceiveBinary(true);

      // The continuation is the synchronizing half of the protocol; a {n+}
      // literal's octets are already on their way (RFC 7888).
      if (!nonSynchronizingLiteral)
         pConnection->SendAsciiData("+ Ready for literal data\r\n");

      return IMAPResult();
   }

   IMAPResult
   IMAPCommandAppend::RefuseWithLiteral_(std::shared_ptr<IMAPConnection> pConnection, const IMAPResult &refusal, bool nonSynchronizingLiteral, __int64 declaredSize)
   {
      // Whatever was prepared for this command is dropped either way.
      KillCurrentMessage_();
      current_message_.reset();
      replace_target_.reset();
      replace_mode_ = false;

      if (!nonSynchronizingLiteral || declaredSize <= 0)
      {
         destination_folder_.reset();
         return refusal;
      }

      // A {n+} literal larger than this account may store is not read at all:
      // RFC 7888 section 4 lets the server close the connection instead, with
      // TOOBIG (section 5) - reading it only to throw it away would hand any
      // client a way to keep a connection busy for as long as it likes. The
      // limit is the configured maximum message size (the one APPENDLIMIT
      // advertises), and never under 1 MB, so a small refused message is still
      // read and answered NO on a connection that stays open.
      if (declaredSize > RefusedLiteralLimitBytes_(pConnection->GetAccount()))
      {
         destination_folder_.reset();
         pConnection->SendAsciiData(current_tag_ + " BAD [TOOBIG] The literal is larger than this server accepts.\r\n");
         pConnection->Logout("* BYE [TOOBIG] The literal is larger than this server accepts.\r\n");
         return IMAPResult(IMAPResult::ResultOKSupressRead, "");
      }

      // The octets are already on their way. They are read and discarded - no
      // current message, so nothing is written - and the refusal is sent when
      // the command's final CRLF arrives (FinalizeCommand_), exactly as a later
      // message of a MULTIAPPEND refused under {n+} is handled. Without this the
      // message's lines were read as IMAP commands: a refused APPEND of a
      // message containing the line "x DELETE INBOX" would have run it.
      String refusalWord = refusal.GetResult() == IMAPResult::ResultBad ? _T(" BAD ") : _T(" NO ");
      FailCommand_(current_tag_ + refusalWord + refusal.GetMessage() + _T("\r\n"));

      bytes_left_to_receive_ = (size_t) declaredSize;
      receive_state_ = ReceivingLiteral;
      pConnection->SetReceiveBinary(true);

      return IMAPResult();
   }

   IMAPResult
   IMAPCommandAppend::ValidateAndPrepareMessage_(std::shared_ptr<IMAPConnection> pConnection, __int64 declaredSize)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Everything one message of an APPEND must pass before its octets are
   // accepted - shared between the first message (parsed by ExecuteCommand) and
   // every later one in a MULTIAPPEND (parsed from a continuation line).
   //---------------------------------------------------------------------------
   {
      // Free-space precondition, before anything about this particular message is
      // considered: the disk is not a property of the mailbox, and a public folder
      // has no quota to fall foul of but lives on the same volume as everything
      // else.
      //
      // NO [UNAVAILABLE] (RFC 5530): "temporary failure because a subsystem is
      // down" is the closest thing IMAP has to the SMTP 452, and the distinction
      // matters just as much here. An APPEND is a client saving a draft or filing
      // a Sent copy it may hold nowhere else, so the refusal has to read as "not
      // now" - a client that treats this as permanent discards the message. The
      // text says so in words as well, because the response code is advisory and
      // most clients only show the text.
      //
      // Placed alongside the ceilings below rather than in ExecuteCommand so that
      // it also covers the second and later messages of a MULTIAPPEND, whose
      // literals are parsed from a continuation line and never pass through
      // ExecuteCommand at all.
      if (!DiskSpace::DataDirectoryHasRoomForMail())
         return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] Insufficient system storage - the message was not stored. Please try again later.");

      // Absolute ceiling independent of the configured maximum, so an "unlimited"
      // (0) max message size cannot translate into an unbounded APPEND. TOOBIG
      // (RFC 4469, required alongside the RFC 7889 APPENDLIMIT advertisement)
      // tells the client the literal itself was the problem, so it does not
      // retry the same message.
      const __int64 absoluteMaxMessageBytes = (__int64) 2 * 1024 * 1024 * 1024; // 2 GB
      if (declaredSize > absoluteMaxMessageBytes)
         return IMAPResult(IMAPResult::ResultNo, "[TOOBIG] Message size exceeds the maximum permitted size.");

      // The literal is exactly the message; the CRLF after it belongs to the
      // command line and is handled as continuation text.
      bytes_left_to_receive_ = (size_t) declaredSize;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(pConnection->GetAccount()->GetDomainID());
      size_t maxMessageSizeKB = GetMaxMessageSize_(domain, pConnection->GetAccount());

      if (maxMessageSizeKB > 0 &&
          bytes_left_to_receive_ / 1024 > maxMessageSizeKB)
      {
         String sMessage;
         sMessage.Format(_T("[TOOBIG] Message size exceeds fixed maximum message size. Size: %d KB, Max size: %d KB"),
            bytes_left_to_receive_ / 1024, maxMessageSizeKB);

         return IMAPResult(IMAPResult::ResultNo, sMessage);
      }

      if (!destination_folder_->IsPublicFolder())
      {
         // Make sure that this message fits in the mailbox - counting the
         // messages of this same command that are received but, because a
         // MULTIAPPEND is atomic, not yet saved and so not yet in the account's
         // size. Without this, N-1 messages of headroom could be overshot.
         size_t pendingBytes = 0;
         for (const PendingMessage &pending : pending_messages_)
            pendingBytes += (size_t) pending.message->GetSize();

         // The quota that matters is the DESTINATION folder owner's, as COPY
         // and MOVE have it (IMAPCopy.cpp): the message is stored in the
         // owner's mailbox and counted in the owner's size. Charging the caller
         // let a delegate with the insert right push the owner past the limit,
         // and refused a delegate over their own quota an append to a mailbox
         // with room. Read fresh from the cache, not the session's copy, so a
         // limit an administrator has just changed applies at once.
         std::shared_ptr<const Account> pAccount = CacheContainer::Instance()->GetAccount(destination_folder_->GetAccountID());

         if (!pAccount)
            return IMAPResult(IMAPResult::ResultNo, "The destination folder could not be resolved.");

         if (!pAccount->SpaceAvailable(bytes_left_to_receive_ + pendingBytes))
            return IMAPResult(IMAPResult::ResultNo, "[OVERQUOTA] Your quota has been exceeded.");
      }

      // Construct a file name which we'll write the message to.
      //
      // The owner of the DESTINATION folder, not the caller. The row below is
      // stamped with destination_folder_->GetAccountID(), so using the
      // caller here wrote the bytes into one account's directory while the row
      // named another's - and every later FETCH, by the owner or by the appending
      // delegate, resolved the owner's directory, found nothing, and served the
      // "file does not exist" placeholder. The APPEND itself answered OK.
      //
      // Resolved before current_message_ is made, so that a current message
      // always has its own file name. Made first, a refusal here left a current
      // message beside the file name of the message before it - on this
      // connection, the last APPEND's, saved with its row - and the refusal's
      // KillCurrentMessage_ (RefuseWithLiteral_) deleted that saved message's file.
      std::shared_ptr<const Account> pMessageOwner;
      if (!destination_folder_->IsPublicFolder())
      {
         pMessageOwner = pConnection->GetAccountOwningFolder(destination_folder_);

         // No owner means no path we are willing to guess at.
         if (!pMessageOwner)
            return IMAPResult(IMAPResult::ResultNo, "The destination folder could not be resolved.");
      }

      current_message_ = std::shared_ptr<Message>(new Message);
      current_message_->SetAccountID(destination_folder_->GetAccountID());
      current_message_->SetFolderID(destination_folder_->GetID());

      message_file_name_ = MessageStore::WorkingFile(pMessageOwner, current_message_);

      // Before a byte of it is written: see SweepAbandonedAppends.
      current_marker_ = WriteAppendMarker_(message_file_name_, false, String());

      return IMAPResult();
   }

   void
   IMAPCommandAppend::ParseBinary(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<ByteBuffer> pBuf)
   {
      append_buffer_.Add(pBuf);

      // The connection stays in binary mode from the first literal to the final
      // CRLF of the command. Between literals the bytes are the rest of the
      // command line (RFC 3502 MULTIAPPEND: another flags/date/{size} group, or
      // nothing before the terminating CRLF), so one received buffer can span a
      // literal's end, the following line and the next literal's start - hence a
      // loop rather than one step per read.
      while (true)
      {
         if (receive_state_ == ReceivingLiteral)
         {
            if (append_buffer_.GetSize() < bytes_left_to_receive_)
            {
               TruncateBuffer_(pConnection);

               pConnection->EnqueueRead("");
               return;
            }

            // Write only the number of bytes still expected for this literal;
            // never spill trailing bytes - they are the continuation of the
            // command line - into the stored message.
            size_t writeLen = bytes_left_to_receive_;

            // A failed command still consumes its literals to keep the protocol
            // in step, but stores nothing.
            if (!command_failed_)
               WriteData_(pConnection, append_buffer_.GetBuffer(), writeLen);

            size_t remaining = append_buffer_.GetSize() - writeLen;
            append_buffer_.Empty(remaining);
            bytes_left_to_receive_ = 0;

            CompleteCurrentMessage_(pConnection);

            continuation_line_.Empty();
            receive_state_ = ReceivingContinuationLine;
            continue;
         }

         // ReceivingContinuationLine: accumulate up to the next CRLF. Everything
         // is appended to the line buffer first and the CRLF searched for there,
         // so a pair split across two reads is still found.
         continuation_line_.append((const char*) append_buffer_.GetBuffer(), append_buffer_.GetSize());
         append_buffer_.Empty();

         size_t lineEnd = continuation_line_.find("\r\n");

         if (lineEnd == std::string::npos)
         {
            // A between-literals line is a flags list, a date and a size - tiny.
            // Anything growing past this is not an APPEND continuation, and
            // buffering it forever would hand an authenticated client a memory
            // sink.
            const size_t maxContinuationLine = 4096;

            if (continuation_line_.size() > maxContinuationLine)
            {
               CleanupPendingMessages_();
               KillCurrentMessage_();

               pConnection->SetReceiveBinary(false);
               pConnection->SendAsciiData(current_tag_ + " BAD APPEND continuation line is too long.\r\n");
               pConnection->EnqueueRead();
               return;
            }

            pConnection->EnqueueRead("");
            return;
         }

         AnsiString line = continuation_line_.substr(0, lineEnd);
         AnsiString residue = continuation_line_.substr(lineEnd + 2);
         continuation_line_.Empty();

         // Bytes after the CRLF are the next literal's octets or, after the
         // command's final CRLF, whatever the client pipelined behind it.
         if (!residue.empty())
            append_buffer_.Add((const BYTE*) residue.c_str(), residue.size());

         ParseContinuationLine_(pConnection, line);

         // Closed with TOOBIG (CloseOnOversizedDiscard_): nothing more is read.
         if (connection_closing_)
            return;

         if (receive_state_ == ReceivingLiteral && bytes_left_to_receive_ > 0)
            continue;

         // The command completed (or failed terminally): back to line mode.
         //
         // What is left in the buffer arrived after the command's final CRLF, so
         // it is the client's next command - or several, and the start of a
         // literal - sent without waiting for this one's answer, which both
         // RFC 3501 (5.5) and RFC 7888 allow. mbsync pipelines APPENDs exactly
         // like this. It used to be thrown away with the buffer, and the commands
         // in it were never answered: mbsync stored one message and then waited
         // until it timed out. It is handed back to the connection as though it
         // had just arrived, ahead of anything still in the socket, and read as
         // command text. The paths that stop on a protocol error empty the
         // buffer first: where the framing is lost, nothing after it is trusted.
         if (append_buffer_.GetSize() > 0)
            pConnection->InjectPipelinedBytes(append_buffer_.GetBuffer(), append_buffer_.GetSize());

         append_buffer_.Empty();

         pConnection->EnqueueRead();
         return;
      }
   }

   bool
   IMAPCommandAppend::WriteData_(const std::shared_ptr<IMAPConnection>  pConn, const BYTE *pBuf, size_t WriteLen)
   {
      if (!current_message_)
         return false;

      String destinationPath = FileUtilities::GetFilePath(message_file_name_);
      if (!FileUtilities::Exists(destinationPath))
         FileUtilities::CreateDirectory(destinationPath);

      File oFile;

      try
      {
         // Both calls report failure by return value (a full or read-only volume,
         // a file locked by AV/backup); ignoring them meant APPEND answered OK
         // for a message that was never stored.
         if (!oFile.Open(message_file_name_, File::OTAppend))
         {
            write_failed_ = true;
            return false;
         }

         if (!oFile.Write(pBuf, WriteLen))
         {
            write_failed_ = true;
            return false;
         }

         // And the close, which writes what the write left buffered - up to
         // 4 KB of the literal on most systems (File::Close). It was left to
         // the destructor, so a disk that refused those bytes had APPEND
         // answer OK for a short message. [Settings]
         // SimulateSpoolWriteFailure=3 has the close say it failed, after it
         // has closed the file, so that the regression suite can run this
         // without filling a disk (SpoolWriteFailure.cs).
         const bool closed = oFile.Close();
         if (!closed || IniFileSettings::Instance()->GetSimulateSpoolWriteFailure() == 3)
         {
            write_failed_ = true;
            return false;
         }
      }
      catch (...)
      {
         write_failed_ = true;
         return false;
      }

      return true;
   }

   bool
   IMAPCommandAppend::TruncateBuffer_(const std::shared_ptr<IMAPConnection> pConn)
   {
      if (append_buffer_.GetSize() >= 20000)
      {
         WriteData_(pConn, append_buffer_.GetBuffer(), append_buffer_.GetSize());
         bytes_left_to_receive_ -= append_buffer_.GetSize();
         append_buffer_.Empty();
      }

      return true;

   }

   void
   IMAPCommandAppend::CompleteCurrentMessage_(std::shared_ptr<IMAPConnection> pConnection)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // One message's octets have all arrived. Its flags and date are applied and
   // it joins the received-but-unsaved list; nothing touches the database until
   // the whole command has succeeded, because a multi-message APPEND is atomic
   // (RFC 3502) - all stored or none.
   //---------------------------------------------------------------------------
   {
      if (command_failed_ || !current_message_)
      {
         KillCurrentMessage_();
         current_message_.reset();
         return;
      }

      // The stored file is the literal, byte for byte. It used to have the
      // command's terminating CRLF written after it - inherited from the code
      // before MULTIAPPEND, which read that CRLF in binary mode with the
      // message - so every appended message came back two octets longer than
      // the client sent it: RFC822.SIZE, BODY[] and BODY[TEXT] all said so, and
      // a client comparing sizes after a sync (imaptest, mbsync) saw a
      // different message. RFC 3501 6.3.11 appends the literal, not the
      // literal and a line break. A message that does not end with a line
      // break - a header block with no body, as the SORT-by-Date fixture
      // appends - still parses: MimeField::Load takes a last field that runs
      // to the end of the data.
      if (write_failed_)
      {
         // The message data never reached disk. Fail the whole command:
         // reporting OK here silently lost Sent Items copies, drafts and
         // migration uploads while the client believed they were saved.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5211, "IMAPCommandAppend::CompleteCurrentMessage_",
            "APPEND failed because the message file could not be written: " + message_file_name_);

         KillCurrentMessage_();
         write_failed_ = false;
         current_message_.reset();

         FailCommand_(current_tag_ + " NO [SERVERBUG] APPEND failed - the message could not be stored on the server.\r\n");
         return;
      }

      // Stored as a delivery stores a message (Persistence/MessageStore.h):
      // compressed, its large parts held once, when MessageStoreCompression is
      // on. One that cannot be is kept as it came, and the error log says why.
      // The literal is the client's, and is the message whatever it begins
      // with: one that begins with the mark of a container is wrapped, never
      // taken for one - and refused, like a write that failed, when it cannot
      // be wrapped (HM7353). Never placed, it counted nothing, and it goes
      // without letting go of anything. In a mailbox encrypted at rest it is
      // encrypted to the owner's key, and refused (HM7365) rather than kept in
      // the clear when it cannot be (MessageStore.h).
      std::shared_ptr<const Account> messageOwner;
      if (destination_folder_ && !destination_folder_->IsPublicFolder())
         messageOwner = pConnection->GetAccountOwningFolder(destination_folder_);

      MessageStore::PlaceRefusal refusal = MessageStore::PlaceAccepted;
      if (!MessageStore::PlaceWorkingFileFor(messageOwner, message_file_name_, false, refusal))
      {
         MessageStore::DiscardWorkingFile(message_file_name_);
         message_file_name_.Empty();
         RemoveAppendMarker_(current_marker_);
         current_message_.reset();

         // Whether the mailbox is encrypted at rest not known just now: the
         // client keeps its copy and tries again (RFC 5530 UNAVAILABLE).
         if (refusal == MessageStore::PlaceRefusedDamaged)
            FailCommand_(current_tag_ + " NO [CONTACTADMIN] APPEND failed - " + MailboxKeys::DamagedSentence() + "\r\n");
         else if (refusal == MessageStore::PlaceRefusedUnknown)
            FailCommand_(current_tag_ + " NO [UNAVAILABLE] APPEND failed - whether this mailbox is encrypted at rest could not be read just now, and the message is not stored in the clear on a guess. Try again in a moment.\r\n");
         else
            FailCommand_(current_tag_ + " NO APPEND failed - the message could not be stored on the server.\r\n");
         return;
      }

      // Placed: removing it from here on lets go of what it references.
      if (!current_marker_.IsEmpty())
         WriteAppendMarker_(message_file_name_, true, current_marker_);

      // The size after placing, of what the store now holds: the file is plain,
      // or a container the store wrote itself, and either answers the literal's
      // own size - never a size a header inside the literal claims.
      current_message_->SetSize(MessageStore::FileSize(message_file_name_));
      current_message_->SetState(Message::Delivered);

      // Set message flags.
      bool bSeen = (flags_to_set_.FindNoCase(_T("\\Seen")) >= 0);
      bool bDeleted = (flags_to_set_.FindNoCase(_T("\\Deleted")) >= 0);
      bool bDraft = (flags_to_set_.FindNoCase(_T("\\Draft")) >= 0);
      bool bAnswered = (flags_to_set_.FindNoCase(_T("\\Answered")) >= 0);
      bool bFlagged = (flags_to_set_.FindNoCase(_T("\\Flagged")) >= 0);

      // RFC 4314 section 4: the flags the user may not set in the folder are left
      // out - \Seen without the 's' right, \Deleted without 't', and \Draft,
      // \Answered and \Flagged without 'w', as the keywords below are. Only \Seen
      // was (upstream #666).
      if (bSeen && !pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionWriteSeen))
         bSeen = false;

      if (bDeleted && !pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionWriteDeleted))
         bDeleted = false;

      if (!pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionWriteOthers))
      {
         bDraft = false;
         bAnswered = false;
         bFlagged = false;
      }

      current_message_->SetFlagDeleted(bDeleted);
      current_message_->SetFlagSeen(bSeen);
      current_message_->SetFlagDraft(bDraft);
      current_message_->SetFlagAnswered(bAnswered);
      current_message_->SetFlagFlagged(bFlagged);

      // Keywords in the flag list are kept too - they are "the other flags"
      // of RFC 4314, so only where those may be written.
      if (pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionWriteOthers))
      {
         String rest = flags_to_set_;
         rest.Replace(_T("("), _T(" "));
         rest.Replace(_T(")"), _T(" "));
         std::vector<String> atoms = StringParser::SplitString(rest, " ");
         std::vector<String> keywords;
         for (size_t i = 0; i < atoms.size(); i++)
         {
            String atom = atoms[i];
            atom.TrimLeft();
            atom.TrimRight();
            if (atom.IsEmpty() || atom[0] == '\\' || !SieveParser::IsValidFlagName(atom))
               continue;
            keywords.push_back(atom);
         }
         current_message_->SetKeywordList(keywords);
      }

      // Set the create time
      if (!create_time_to_set_.IsEmpty())
      {
         // Convert to internal format
         create_time_to_set_ = Time::GetInternalDateFromIMAPInternalDate(create_time_to_set_);
         current_message_->SetCreateTime(create_time_to_set_);
      }

      PendingMessage pending;
      pending.message = current_message_;
      pending.fileName = message_file_name_;
      pending.markerFile = current_marker_;
      current_marker_.Empty();
      pending_messages_.push_back(pending);

      // Pending now: its file is the pending entry's to remove, never the
      // current message's (KillCurrentMessage_).
      message_file_name_.Empty();

      current_message_.reset();
   }

   void
   IMAPCommandAppend::ParseContinuationLine_(std::shared_ptr<IMAPConnection> pConnection, const AnsiString &line)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // The command-line text following a completed literal: empty means the
   // command's terminating CRLF, anything else must be another RFC 3502
   // "[flags] [date] {size}" group.
   //---------------------------------------------------------------------------
   {
      String continuation = line;
      continuation.Trim();

      // The message just received was sent as UTF8 (~{n}): its closing ")" is
      // the first thing on the line after the octets. Anything else means the
      // client and server disagree about where the message ended, and the only
      // safe answer is to stop rather than store bytes under the wrong framing.
      if (utf8_wrapped_literal_)
      {
         utf8_wrapped_literal_ = false;

         if (continuation.IsEmpty() || continuation.GetAt(0) != ')')
         {
            TerminateWithProtocolError_(pConnection, " BAD APPEND UTF8 literal is not closed.\r\n");
            return;
         }

         continuation = continuation.Mid(1);
         continuation.Trim();
      }

      if (continuation.IsEmpty())
      {
         FinalizeCommand_(pConnection);
         return;
      }

      // RFC 8508: REPLACE takes exactly one message - its grammar has no
      // MULTIAPPEND extension.
      if (replace_mode_)
      {
         TerminateWithProtocolError_(pConnection, " BAD REPLACE takes exactly one message.\r\n");
         return;
      }

      // Another message follows. Parse its optional flags list, optional quoted
      // date and mandatory literal size.
      flags_to_set_ = "";
      create_time_to_set_ = "";

      if (continuation.GetLength() > 0 && continuation.GetAt(0) == '(')
      {
         int flagsEnd = continuation.Find(_T(")"));
         if (flagsEnd < 0)
         {
            TerminateWithProtocolError_(pConnection, " BAD APPEND flag list is not closed.\r\n");
            return;
         }

         flags_to_set_ = continuation.Mid(1, flagsEnd - 1);
         continuation = continuation.Mid(flagsEnd + 1);
         continuation.Trim();
      }

      if (continuation.GetLength() > 0 && continuation.GetAt(0) == '"')
      {
         int dateEnd = continuation.Find(_T("\""), 1);
         if (dateEnd < 0)
         {
            TerminateWithProtocolError_(pConnection, " BAD APPEND date is not closed.\r\n");
            return;
         }

         create_time_to_set_ = continuation.Mid(1, dateEnd - 1);
         create_time_to_set_.TrimLeft();
         continuation = continuation.Mid(dateEnd + 1);
         continuation.Trim();
      }

      // RFC 6855: a later message of a MULTIAPPEND may be wrapped the same way
      // as the first - "UTF8 (~{n}" with the ")" after its octets.
      if (continuation.GetLength() >= 6 &&
          continuation.Mid(0, 4).CompareNoCase(_T("UTF8")) == 0 &&
          (continuation.GetAt(4) == ' ' || continuation.GetAt(4) == '('))
      {
         String wrapped = continuation.Mid(4);
         wrapped.TrimLeft();

         if (wrapped.GetLength() < 1 || wrapped.GetAt(0) != '(')
         {
            TerminateWithProtocolError_(pConnection, " BAD APPEND continuation must be a literal.\r\n");
            return;
         }

         continuation = wrapped.Mid(1);
         continuation.TrimLeft();
         utf8_wrapped_literal_ = true;
      }

      // RFC 3516: the literal8 form "~{n}" is accepted wherever a literal is.
      if (continuation.GetLength() > 0 && continuation.GetAt(0) == '~')
         continuation = continuation.Mid(1);

      if (continuation.GetLength() < 3 ||
          continuation.GetAt(0) != '{' ||
          continuation.Right(1) != _T("}"))
      {
         TerminateWithProtocolError_(pConnection, " BAD APPEND continuation must be a literal.\r\n");
         return;
      }

      String literalSize = continuation.Mid(1, continuation.GetLength() - 2);

      bool nonSynchronizingLiteral = false;
      if (literalSize.Right(1) == _T("+"))
      {
         literalSize = literalSize.Mid(0, literalSize.GetLength() - 1);
         nonSynchronizingLiteral = true;
      }

      if (literalSize.IsEmpty())
      {
         TerminateWithProtocolError_(pConnection, " BAD Invalid literal size.\r\n");
         return;
      }

      for (int i = 0; i < literalSize.GetLength(); i++)
      {
         wchar_t ch = literalSize.GetAt(i);
         if (ch < '0' || ch > '9')
         {
            TerminateWithProtocolError_(pConnection, " BAD Invalid literal size.\r\n");
            return;
         }
      }

      __int64 declaredSize = _ttoi64(literalSize.c_str());
      if (declaredSize <= 0)
      {
         TerminateWithProtocolError_(pConnection, " BAD Empty message not permitted.\r\n");
         return;
      }

      IMAPResult prepareResult = command_failed_
         ? IMAPResult()
         : ValidateAndPrepareMessage_(pConnection, declaredSize);

      if (prepareResult.GetResult() != IMAPResult::ResultOK)
      {
         if (!nonSynchronizingLiteral)
         {
            // The client is waiting for a continuation; the tagged refusal can
            // take its place and nothing more will be sent (RFC 3502 section 2:
            // refusing any message means storing none).
            CleanupPendingMessages_();
            KillCurrentMessage_();
            current_message_.reset();

            // A client waiting for the continuation has sent nothing after this
            // line; anything here would be the literal it should not have sent
            // yet, and must not be read as commands.
            append_buffer_.Empty();

            pConnection->SetReceiveBinary(false);

            String refusal = prepareResult.GetResult() == IMAPResult::ResultBad ? _T(" BAD ") : _T(" NO ");
            pConnection->SendAsciiData(current_tag_ + refusal + prepareResult.GetMessage() + _T("\r\n"));
            return;
         }

         // The octets are already on their way ({n+}); consume them to keep the
         // protocol in step, store nothing, and answer at the command's end -
         // unless there are more of them than a refused literal is read for.
         if (CloseOnOversizedDiscard_(pConnection, declaredSize))
            return;

         String refusal = prepareResult.GetResult() == IMAPResult::ResultBad ? _T(" BAD ") : _T(" NO ");
         FailCommand_(current_tag_ + refusal + prepareResult.GetMessage() + _T("\r\n"));

         bytes_left_to_receive_ = (size_t) declaredSize;
         current_message_.reset();
         receive_state_ = ReceivingLiteral;
         return;
      }

      if (command_failed_)
      {
         // Read only to be thrown away, as above.
         if (CloseOnOversizedDiscard_(pConnection, declaredSize))
            return;

         bytes_left_to_receive_ = (size_t) declaredSize;
      }

      receive_state_ = ReceivingLiteral;

      if (!nonSynchronizingLiteral)
         pConnection->SendAsciiData("+ Ready for literal data\r\n");
   }

   bool
   IMAPCommandAppend::StripUtf8LiteralWrapper_(String &command)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Recognises a command line ending in the RFC 6855 form
   //
   //    ... UTF8 (~{n}      or      ... UTF8 (~{n+}
   //
   // and rewrites it to end in the bare literal8 (~{n} / ~{n+}) instead, so the
   // simple command parser - which counts parentheses across the whole line -
   // accepts it. Returns true when the wrapper was present. Deliberately narrow:
   // the literal must be the last thing on the line, its size must be digits
   // (with the optional RFC 7888 "+"), and UTF8 must be a word of its own.
   //---------------------------------------------------------------------------
   {
      if (command.Right(1) != _T("}"))
         return false;

      int braceOpen = command.ReverseFind(_T("{"));
      if (braceOpen < 0)
         return false;

      // Only a size goes between the braces.
      String size = command.Mid(braceOpen + 1, command.GetLength() - braceOpen - 2);
      if (size.Right(1) == _T("+"))
         size = size.Mid(0, size.GetLength() - 1);

      if (size.IsEmpty())
         return false;

      for (int i = 0; i < size.GetLength(); i++)
      {
         wchar_t ch = size.GetAt(i);
         if (ch < '0' || ch > '9')
            return false;
      }

      int literalStart = braceOpen;
      if (literalStart > 0 && command.GetAt(literalStart - 1) == '~')
         literalStart--;

      if (literalStart < 1 || command.GetAt(literalStart - 1) != '(')
         return false;

      int keywordEnd = literalStart - 1;
      while (keywordEnd > 0 && command.GetAt(keywordEnd - 1) == ' ')
         keywordEnd--;

      const int keywordLength = 4;
      if (keywordEnd < keywordLength)
         return false;

      int keywordStart = keywordEnd - keywordLength;
      if (command.Mid(keywordStart, keywordLength).CompareNoCase(_T("UTF8")) != 0)
         return false;

      // "UTF8" has to be a separate word, not the tail of a folder name.
      if (keywordStart > 0 && command.GetAt(keywordStart - 1) != ' ')
         return false;

      command = command.Mid(0, keywordStart) + command.Mid(literalStart);
      return true;
   }

   void
   IMAPCommandAppend::TerminateWithProtocolError_(std::shared_ptr<IMAPConnection> pConnection, const String &responseAfterTag)
   {
      CleanupPendingMessages_();
      KillCurrentMessage_();
      current_message_.reset();
      append_buffer_.Empty();

      pConnection->SetReceiveBinary(false);
      pConnection->SendAsciiData(current_tag_ + responseAfterTag);
   }

   void
   IMAPCommandAppend::FailCommand_(const String &response)
   {
      // The first failure is the one reported; later ones are consequences.
      if (!command_failed_)
      {
         command_failed_ = true;
         failure_response_ = response;
      }
   }

   void
   IMAPCommandAppend::CleanupPendingMessages_()
   {
      // None of these have database rows yet, so removing the files is the
      // whole rollback.
      for (PendingMessage &pending : pending_messages_)
      {
         // Through the store: a placed file lets go of what it references -
         // and one in an object store has no file on the disk to find first.
         MessageStore::Remove(pending.fileName);
         RemoveAppendMarker_(pending.markerFile);
      }

      pending_messages_.clear();
   }

   void
   IMAPCommandAppend::FinalizeCommand_(std::shared_ptr<IMAPConnection> pConnection)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // The command's terminating CRLF has arrived. Save every received message,
   // or - if anything failed along the way - none of them (RFC 3502).
   //---------------------------------------------------------------------------
   {
      // append_buffer_ is left alone: what is in it now came after the final
      // CRLF, and ParseBinary hands it back to the connection.
      pConnection->SetReceiveBinary(false);

      if (command_failed_)
      {
         CleanupPendingMessages_();
         pConnection->SendAsciiData(failure_response_);

         command_failed_ = false;
         failure_response_.Empty();
         destination_folder_.reset();
         return;
      }

      // The saves that make an APPEND real. A database failure part-way rolls
      // back the rows already created - the command promised atomicity.
      //
      // NO rather than BAD: the command was well formed, the server could not
      // do it. The files are removed so none is left on disk with no row
      // referring to it.
      std::vector<__int64> savedUids;
      std::vector<std::shared_ptr<Message> > savedMessages;

      for (const PendingMessage &pending : pending_messages_)
      {
         if (!PersistentMessage::SaveObject(pending.message))
         {
            for (std::shared_ptr<Message> savedMessage : savedMessages)
               PersistentMessage::DeleteObject(savedMessage);

            CleanupPendingMessages_();

            ErrorManager::Instance()->ReportError(ErrorManager::High, 6091, "IMAPCommandAppend::FinalizeCommand_",
               "An APPEND could not be saved and has been refused. The message files have been removed rather than left on disk with nothing referring to them.");

            pConnection->SendAsciiData(current_tag_ + " NO APPEND failed: the message could not be saved.\r\n");

            destination_folder_.reset();
            return;
         }

         savedMessages.push_back(pending.message);
         savedUids.push_back((__int64) pending.message->GetUID());
         pConnection->AddRecentMessage(pending.message->GetID());
      }

      // Every one has its row now: none is pending any more, and nothing from
      // here on - the destructor included - may remove its file. Their markers
      // go too; one that cannot be removed is harmless, since the sweep asks
      // for the row first.
      for (PendingMessage &pending : pending_messages_)
         RemoveAppendMarker_(pending.markerFile);
      pending_messages_.clear();

      MessagesContainer::Instance()->SetFolderNeedsRefresh(destination_folder_->GetID());

      String sResponse;
      if (pConnection->GetCurrentFolder() &&
          pConnection->GetCurrentFolder()->GetID() == destination_folder_->GetID())
      {
         std::shared_ptr<Messages> messages = destination_folder_->GetMessages();

         // Appended at the end, so this session's existing numbering is unaffected; the
         // count reported is this session's, which may still hold messages expunged
         // elsewhere that it has not been told about.
         std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
         if (view)
            view->AppendNewMessages(messages);

         sResponse += IMAPNotificationClient::GenerateExistsString(view ? view->GetMessageCount() : messages->GetCount());
         sResponse += IMAPNotificationClient::GenerateRecentString((int) pConnection->GetRecentMessageCount());
      }

      // RFC 8508 (REPLACE): the replacement is safely stored, so the replaced
      // message leaves the selected mailbox now - reported before the tagged OK.
      if (replace_mode_ && replace_target_)
      {
         std::shared_ptr<IMAPFolder> selectedFolder = pConnection->GetCurrentFolder();
         if (selectedFolder)
         {
            __int64 targetId = replace_target_->GetID();

            std::set<__int64> target_ids;
            target_ids.insert(targetId);

            auto selectedMessages = MessagesContainer::Instance()->GetMessages(selectedFolder->GetAccountID(), selectedFolder->GetID());
            std::vector<__int64> deleted_message_ids = selectedMessages->DeleteMessagesById(target_ids);

            std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();

            if (!deleted_message_ids.empty() && view)
            {
               // RFC 8508: the replaced message is expunged, reported by this session's
               // own number - or as VANISHED under QRESYNC.
               std::vector<unsigned int> expunged_uids;
               std::vector<int> expunged_sequences = view->RemoveMessages(deleted_message_ids, &expunged_uids);

               sResponse += IMAPNotificationClient::FormatExpungeResponses(pConnection, expunged_sequences, expunged_uids);

               pConnection->RemoveRecentMessages(deleted_message_ids);

               std::shared_ptr<ChangeNotification> pDeleteNotification =
                  std::shared_ptr<ChangeNotification>(new ChangeNotification(selectedFolder->GetAccountID(), selectedFolder->GetID(), ChangeNotification::NotificationMessageDeleted, deleted_message_ids));
               Application::Instance()->GetNotificationServer()->SendNotification(pConnection->GetNotificationClient(), pDeleteNotification);
            }
         }

         replace_target_.reset();
         replace_mode_ = false;
      }

      // RFC 4315 (UIDPLUS): report the destination folder's UIDVALIDITY and the
      // UIDs assigned, so the client can reference the messages without a
      // search. With several messages (RFC 3502) the response carries the
      // uid-set, in the order the messages were appended. Not to a user who may
      // not SELECT or EXAMINE the folder - one it may only drop mail into - whom
      // RFC 4315 section 6 has it not told (upstream #650).
      String sAppendUid;
      if (pConnection->CheckPermission(destination_folder_, ACLPermission::PermissionRead))
      {
         sAppendUid.Format(_T("[APPENDUID %d %s] "),
            destination_folder_->GetCreationTime().ToInt(),
            IMAPConnection::CompactUidSet(savedUids).c_str());
      }

      // Send the OK response to the client.
      sResponse += current_tag_ + " OK " + sAppendUid + "APPEND completed\r\n";
      pConnection->SendAsciiData(sResponse);

      // Notify the mailbox notifier that the mailbox contents have changed.
      std::shared_ptr<ChangeNotification> pNotification =
         std::shared_ptr<ChangeNotification>(new ChangeNotification(destination_folder_->GetAccountID(), destination_folder_->GetID(), ChangeNotification::NotificationMessageAdded));
      Application::Instance()->GetNotificationServer()->SendNotification(pConnection->GetNotificationClient(), pNotification);

      pending_messages_.clear();
      destination_folder_.reset();
   }

   int
   IMAPCommandAppend::GetMaxMessageSize_(std::shared_ptr<const Domain> pDomain, std::shared_ptr<const Account> pAccount)
   {
      // The same resolution the SMTP session makes for the account that sends
      // (Application/EffectiveSetting.h): a limit - the account's own, else its
      // domain's, else the server's, each level held to the one above it. The
      // account is the session's own, so no address named anywhere lifts it.
      return EffectiveSetting::ResolveInt(_T("max_message_size_kb"), pDomain, pAccount);
   }

   // static
   __int64
   IMAPCommandAppend::RefusedLiteralLimitBytes_(std::shared_ptr<const Account> pAccount)
   {
      // The configured maximum message size (the one APPENDLIMIT advertises),
      // and never under 1 MB, so a small refused message is still read and
      // answered NO on a connection that stays open.
      const __int64 refusedLiteralFloorBytes = (__int64) 1024 * 1024;
      __int64 refusedLiteralLimitBytes = GetEffectiveAppendLimitBytes(pAccount);
      if (refusedLiteralLimitBytes < refusedLiteralFloorBytes)
         refusedLiteralLimitBytes = refusedLiteralFloorBytes;

      return refusedLiteralLimitBytes;
   }

   bool
   IMAPCommandAppend::CloseOnOversizedDiscard_(std::shared_ptr<IMAPConnection> pConnection, __int64 declaredSize)
   {
      // A later literal of a MULTIAPPEND that is read only to be thrown away -
      // its message refused under {n+}, or any literal once the command has
      // failed - was consumed whatever its size, up to the 2 GB ceiling and
      // past it: the TOOBIG cut-off RefuseWithLiteral_ applies to the first
      // literal did not reach these, so a client could keep a connection
      // reading and discarding for as long as it liked. The same limit and
      // the same answer (RFC 7888 sections 4 and 5).
      if (declaredSize <= RefusedLiteralLimitBytes_(pConnection->GetAccount()))
         return false;

      CleanupPendingMessages_();
      KillCurrentMessage_();
      current_message_.reset();
      append_buffer_.Empty();
      destination_folder_.reset();
      replace_target_.reset();
      replace_mode_ = false;
      command_failed_ = false;
      failure_response_.Empty();
      bytes_left_to_receive_ = 0;
      receive_state_ = ReceivingContinuationLine;
      connection_closing_ = true;

      pConnection->SetReceiveBinary(false);
      pConnection->SendAsciiData(current_tag_ + " BAD [TOOBIG] The literal is larger than this server accepts.\r\n");
      pConnection->Logout("* BYE [TOOBIG] The literal is larger than this server accepts.\r\n");
      return true;
   }

   // static
   __int64
   IMAPCommandAppend::GetEffectiveAppendLimitBytes(std::shared_ptr<const Account> pAccount)
   {
      // The same merge ExecuteCommand enforces: the global maximum, tightened by
      // the account's domain, capped by the 2 GB absolute ceiling. Advertising a
      // number the enforcement would not honour is worse than advertising none.
      const __int64 absoluteMaxMessageBytes = (__int64) 2 * 1024 * 1024 * 1024; // 2 GB

      const int maxMessageSizeKB = EffectiveSetting::ResolveInt(_T("max_message_size_kb"),
         pAccount ? CacheContainer::Instance()->GetDomain(pAccount->GetDomainID()) : std::shared_ptr<const Domain>(), pAccount);

      __int64 limitBytes = (__int64) maxMessageSizeKB * 1024;

      if (limitBytes <= 0 || limitBytes > absoluteMaxMessageBytes)
         limitBytes = absoluteMaxMessageBytes;

      return limitBytes;
   }

}
