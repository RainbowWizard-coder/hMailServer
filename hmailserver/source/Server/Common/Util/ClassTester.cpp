// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "ClassTester.h"
#include "../Application/BackupManager.h"
#include "../Application/TimeoutCalculator.h"
#include "../Threading/WorkQueueManager.h"
#include "../BO/MessageData.h"
#include "../BO/Message.h"
#include "../BO/Account.h"
#include "../BO/FetchAccount.h"
#include "../BO/ScheduledTask.h"
#include "../Rules/RuleGuard.h"
#include "../Sieve/SieveVacationResponder.h"
#include "../Sieve/ManageSieveServer.h"
#include "../Mime/MimeTester.h"
#include "../Util/Utilities.h"
#include "../Util/MessageUtilities.h"
#include "../Util/MessageOrigin.h"
#include "../Util/ResourceBooking.h"
#include "../Util/Imip.h"
#include "../Util/PasswordBreachCheck.h"
#include "../Util/ObjectStoreSignature.h"
#include "../Util/ContentPolicy.h"
#include "../AntiSpam/QuarantineReview.h"
#include "../AntiSpam/QuarantineStore.h"
#include "../AntiSpam/FqdnCheck.h"
#include "../Idna/Idna.h"
#include "../Util/WebPush.h"
#include "../Util/PushSubscriptions.h"
#include "../Util/PushNotifier.h"
#include "../Util/ExternalSenderTagger.h"
#include "../Util/ImpersonationCheck.h"
#include "../Application/MailboxRestore.h"
#include "../Application/BackupExecuter.h"
#include "../Util/ReputationWatch.h"
#include "../Util/DisclaimerAdder.h"
#include "../Util/Journaling.h"
#include "../Util/InlineImageParts.h"
#include "../Util/GeoIpDatabase.h"
#include "../Util/CountryBlocking.h"
#include "../Util/SealedMessage.h"
#include "../Util/OpenPgpKey.h"
#include "../Util/OpenPgpWriter.h"
#include "../Util/GatewayEncryption.h"
#include "../Util/XmlDocument.h"
#include "JsonDocument.h"
#include "../Util/XmlSignature.h"
#include "../AntiSpam/DKIM/DkimRotationPlan.h"
#include "../AntiSpam/DKIM/DKIMSigner.h"
#include "../AntiSpam/DKIM/Arc.h"
#include "../Persistence/PersistentDkimRotation.h"
#include "../../SMTP/BounceClassifier.h"
#include "BrandLogo.h"
#include "InertMediaType.h"
#include "Compression.h"
#include "ZipWriter.h"
#include "ReaderOAuth.h"
#include "MigrationImapClient.h"
#include "MigrationSources.h"
#include "../Application/MigrationJob.h"
#include "../Application/MessageIndexer.h"
#include "../Persistence/PersistentMessageMetaData.h"
#include "../Application/RoleManager.h"
#include "../Pst/PstTester.h"
#include "../Pst/PstItemsTester.h"
#include "../Pst/PstReaderWireTester.h"
#include "WebAuthn.h"
#include "PasskeyStore.h"
#include "SignInBudget.h"
#include "SignInState.h"
#include "MaskedAddresses.h"
#include "ListManager.h"
#include "ListArchive.h"
#include "../../SMTP/BounceTracker.h"
#include "../../SMTP/SMTPForwarding.h"
#include "../../SMTP/DestinationBackoffTester.h"
#include "../../SMTP/WarmupPlanTester.h"
#include "../../SMTP/TlsPolicy.h"
#include "SnmpTester.h"
#include "XmlTreeTester.h"
#include "InputCostTester.h"
#include "InlineScriptHash.h"
#include "SignatureHtmlSanitizer.h"
#include "../AntiVirus/CustomVirusScanner.h"
#include "../Persistence/SchemaUpgrader.h"
#include "../SQL/Prerequisites/PreReqGreyListKeys.h"
#include "../../SMTP/GreyListing.h"
#include "ServiceAccount.h"
#include "../Persistence/MessageStoreFormat.h"
#include "MailboxCrypto.h"
#include "MailboxKeys.h"
#include "../../SMTP/DeliveryReportParser.h"
#include "../../SMTP/ComplaintTracker.h"
#include "DomainReports.h"
#include "../../SMTP/FeedbackReportParser.h"
#include "../Util/AcmeClient.h"
#include "../Util/FileUtilities.h"
#include "../Application/IniFileSettings.h"
#include "../Util/Charset.h"
#include "../Util/RegularExpression.h"
#include "../TCPIP/LocalIPAddresses.h"
#include "../TCPIP/DNSResolver.h"
#include "../TCPIP/IPAddress.h"
#include "../TCPIP/CertificateVerifier.h"
#include "../TCPIP/DeflateStreams.h"
#include "../TCPIP/SslContextInitializer.h"
#include "../SQL/DatabaseTls.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseClaimTester.h"
#include "../SQL/DatabaseTransaction.h"
#include "Time.h"
#include "Utilities.h"
#include "Parsing/AddresslistParser.h"
#include "../../IMAP/IMAPSimpleCommandParser.h"
#include "../../IMAP/IMAPFetchParser.h"
#include "../../IMAP/IMAPFetch.h"
#include "../../IMAP/IMAPThread.h"
#include "../../IMAP/IMAPCommandRangeAction.h"
#include "../../IMAP/IMAPStore.h"
#include "../../IMAP/IMAPSearchParser.h"
#include "LegacyBlowfish.h"
#ifndef HM_PLATFORM_POSIX
#include "AdminUpdate.h"
#include "AdminUpdateTester.h"
#endif

#ifdef HM_PLATFORM_POSIX
namespace
{
   // _mkgmtime64 is Microsoft's name for the inverse of gmtime - a struct tm
   // read as UTC, where mktime would read it as local time - with a 64-bit
   // result. POSIX spells the same function timegm, and its time_t is already
   // 64 bits here, so the cast loses nothing.
   inline long long _mkgmtime64(struct tm *parts)
   {
      return (long long) ::timegm(parts);
   }
}
#endif
#include "Crypt.h"
#include "DataProtector.h"
#include "StoredSecrets.h"
#include "SRS.h"
#include "BATV.h"
#include "Unicode.h"
#include "AlertManager.h"
#include "Hashing/ScramSha256.h"
#include "AccountLockout.h"
#include "Totp.h"
#include "../AntiSpam/DMARC/DmarcTreeWalk.h"
#include "../AntiSpam/DMARC/PublicSuffixList.h"
#include "NeverBanNameList.h"
#include "AutoBanWidener.h"
#include "../AntiSpam/SURBL.h"
#include "../AntiSpam/SpamAssassin/SpamAssassinReturnPath.h"
#include "RateLimiter.h"
#include "TransparentTransmissionBuffer.h"
#include "../Cache/Cache.h"
#include "../Persistence/PersistentMessage.h"
#include "../../SMTP/SPF/SPF.h"
#include "../../SMTP/BLCheck.h"
#include "../../SMTP/TlsRptReporterTask.h"
#include "../../SMTP/DmarcRptReporterTask.h"
#include "../Application/BackupManager.h"
#include "../Util/Encoding/Base64.h"
#include "../Util/Encoding/ModifiedUTF7.h"
#include "../Util/Hashing/HashCreator.h"
#include "../Util/OAuth2TokenValidator.h"
#include "../Util/CompromiseDetector.h"
#include "../Util/EventTester.h"
#include "../Util/HttpCompression.h"
#include "../Util/HttpServer.h"
#include "../Util/RestApiOpenApiTester.h"
#include "../Persistence/PersistentMessageIndex.h"
#include "../Util/UpdateChecker.h"
#include "../Util/Language.h"
#include "../SQL/PGConnection.h"
#include "../SQL/MySQLConnection.h"
#include "../SQL/ADOConnection.h"
#include "../SQL/SQLCEConnection.h"
#include "../Application/Profiles.h"
#include "../Scripting/ScriptServer.h"
#include "../SQL/SQLStatement.h"
#include <boost/pool/object_pool.hpp>
#include <atomic>

#ifdef _DEBUG
   #define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
   #define new DEBUG_NEW
#endif

namespace HM
{

   ClassTester::ClassTester()
   {

   }

   ClassTester::~ClassTester()
   {

   }

   void
   ClassTester::DoTests()
   {
      EventTester *pEventTester = new EventTester;
      pEventTester->Test();
      delete pEventTester;

      OutputDebugString(_T("hMailServer: Testing mime parser\n"));
      MimeTester *pMimeTester = new MimeTester;
      pMimeTester->Test();
      delete pMimeTester;

      OutputDebugString(_T("hMailServer: Testing StringParser\n"));
      StringParserTester *pParser = new StringParserTester();
      pParser->Test();
      delete pParser;
  
      OutputDebugString(_T("hMailServer: Testing FileUtilities\n"));
      FileUtilitiesTester fileUtilitiesTester;
      fileUtilitiesTester.Test();

      OutputDebugString(_T("hMailServer: Testing IPAddress\n"));
      IPAddressTester ipAddressTester;
      ipAddressTester.Test();

      // An account's copy - what COM's Authenticate hands a webmail, which
      // saves it - holds every member: from 6.2.25 to 6.3.5 it left the
      // retention unset and dropped the mailbox key wrap (Account.cpp).
      OutputDebugString(_T("hMailServer: Testing the account's copy\n"));
      AccountTester accountTester;
      accountTester.Test();

      // DaysToKeepMessages: one meaning for both fetchers, what a value stored
      // before schema 6106 means in it, and an archive written before 6.3.6 read
      // through that rule (FetchAccount.h).
      OutputDebugString(_T("hMailServer: Testing the fetch account's retention\n"));
      FetchAccountTester fetchAccountTester;
      fetchAccountTester.Test();

      // A scheduled task due at a second is due at a turn within it, in whole
      // seconds: until 9 October 2026 a local DATE moved on by a minute's span
      // came out above the second it meant, and the task waited a turn more
      // (ScheduledTask.h).
      OutputDebugString(_T("hMailServer: Testing the scheduled tasks' run times\n"));
      ScheduledTaskTester scheduledTaskTester;
      scheduledTaskTester.Test();

      // X-hMailServer-LoopCount, which comes with mail from anyone, read as a
      // count only when it is digits, and never above the limit: before 6 October
      // 2026 "-5" passed every RuleLoopLimit test and "2147483647" overflowed the
      // count + 1 a generated message carries (RuleGuard.h, ReadLoopCount).
      OutputDebugString(_T("hMailServer: Testing the rule loop count's reader\n"));
      LoopCountTester loopCountTester;
      loopCountTester.Test();

      // A message over the 80 MB a message is parsed whole at, read header only
      // where a caller asks: its loop count read, raised and written back with the
      // body copied byte for byte, where until the large-message row a forward,
      // a rule or a bounce read no count and could write none (MessageData.h,
      // LargeMessage). The refusals, which report, run on Linux
      // (file-utilities-tests), where the reports are counted.
      OutputDebugString(_T("hMailServer: Testing a message too large to be read whole\n"));
      LargeMessageTester largeMessageTester;
      largeMessageTester.Test(IniFileSettings::Instance()->GetTempDirectory());

      // And no edit resets it: a Sieve script's addheader or deleteheader, refused
      // when stored and skipped when run, a rule that sets it, and a line break in a
      // value that would write it as a second field (RuleGuard.h, LoopCountEditTester).
      OutputDebugString(_T("hMailServer: Testing the header edits that could reset the rule loop count\n"));
      LoopCountEditTester loopCountEditTester;
      loopCountEditTester.Test();

      // A rule's actions held to who owns the rule: no From, Sender, Resent- pair or
      // DKIM-Signature set by a domain's or an account's rule, a reply sent only from an
      // address the rule's owner may send as, a reply's From one mailbox whatever
      // its name says, and a Run function action stored only naming a function the
      // server would run (RuleGuard.h, RuleOwnerTester).
      OutputDebugString(_T("hMailServer: Testing a rule's actions against its owner\n"));
      RuleOwnerTester ruleOwnerTester;
      ruleOwnerTester.Test();

      // A grant holding administer_server with a list of domains is confined to
      // that list - it administers the domains it names and is not the server
      // administrator - and COM's server-administrator question and REST's read
      // that one rule (RoleManager.h, RoleManagerTester).
      OutputDebugString(_T("hMailServer: Testing that serveradmin assigned with a domain list is confined to it\n"));
      RoleManagerTester roleManagerTester;
      roleManagerTester.Test();

      // Which messages DKIM signs as the domain their From names: not mail from
      // outside, nor a forward, a redirect or a copy of it, which were signed until 7
      // October 2026 - and the mark that says so, carried by every copy and kept on
      // the queue row (DKIMSigner.h, SigningOriginTester).
      OutputDebugString(_T("hMailServer: Testing which messages are signed as their From's domain\n"));
      SigningOriginTester signingOriginTester;
      signingOriginTester.Test();

      // Which envelope sender a copy of a message is sent on with, and which copy is
      // sent outside at all: the organisations its From names, the DMARC passes its
      // record holds, which envelope senders are this server's own, the choice, and
      // what DMARC passed for when the mail arrived, which every copy carries
      // (SMTPForwarding.h, CopyEnvelopeTester).
      OutputDebugString(_T("hMailServer: Testing the envelope a copy is sent on with\n"));
      CopyEnvelopeTester copyEnvelopeTester;
      copyEnvelopeTester.Test();

      // The spf= an ARC seal attests is this server's own SPF result for the message,
      // carried by its copies, and never a Received-SPF field it carries, which a
      // sender, a Sieve addheader or a rule could have written until 7 October 2026
      // (Arc.h, ArcAuthenticationResultsTester).
      OutputDebugString(_T("hMailServer: Testing the ARC-Authentication-Results a seal attests\n"));
      ArcAuthenticationResultsTester arcAuthenticationResultsTester;
      arcAuthenticationResultsTester.Test();

      // The address a Sieve vacation reply or notification may say it is from: one
      // that reaches the account, or else the account's own. Until 6 October 2026 a
      // notification was sent from whatever its :from said (SieveVacationResponder.h,
      // SieveFromAddress). And no editheader of From, Sender, the Resent- pair or
      // DKIM-Signature, which a redirect sent on from this server, signed as the
      // domain its From named (SieveParser::WhyFieldMayNotBeEdited).
      OutputDebugString(_T("hMailServer: Testing the Sieve :from rule\n"));
      SieveFromAddressTester sieveFromAddressTester;
      sieveFromAddressTester.Test();

      // A ManageSieve quoted string holds no line break: a refusal that quotes a
      // script's line with one in it was sent as it stood until 7 October 2026, and
      // split the NO response (ManageSieveServer::EscapeQuoted).
      OutputDebugString(_T("hMailServer: Testing ManageSieve's quoted strings\n"));
      ManageSieveResponseTester manageSieveResponseTester;
      manageSieveResponseTester.Test();

      // Every TLS context's options read back with SSL_CTX_get_options: exactly what
      // SetContextOptions_ asks for, nothing from bit 32 up but certificate compression
      // off, and the mail client's context as this server's settings set it up.
      OutputDebugString(_T("hMailServer: Testing the TLS contexts' options\n"));
      SslContextInitializerTester sslContextInitializerTester;
      sslContextInitializerTester.Test();

      // Whether a server certificate is for the host: an address matched against
      // the certificate's iPAddress entries as well as its names, which is what
      // the Linux build did not do (docs/DatabaseTls.md, "Certificates for an
      // address").
      OutputDebugString(_T("hMailServer: Testing CertificateVerifier\n"));
      CertificateVerifierTester certificateVerifierTester;
      certificateVerifierTester.Test();

      // TLS to the database: the four modes and what they are refused as, and the
      // SQL Server keywords each mode and provider comes to.
      OutputDebugString(_T("hMailServer: Testing DatabaseTls\n"));
      DatabaseTlsTester databaseTlsTester;
      databaseTlsTester.Test();

      OutputDebugString(_T("hMailServer: Testing MessageUtilities\n"));
      MessageUtilitiesTester messageUtilitiesTester;
      messageUtilitiesTester.Test();

      OutputDebugString(_T("hMailServer: Testing Formatter\n"));
      FormatterTester formatterTester;
      formatterTester.Test();

      // What a value may do to the call text the Windows script engines are
      // handed: a password that tries to end its JScript string, each escape, the
      // VBScript splices, and the rule function names FireEvent lets through.
      OutputDebugString(_T("hMailServer: Testing ScriptLiteral\n"));
      ScriptLiteralTester scriptLiteralTester;
      scriptLiteralTester.Test();

      // The watchdog's due time, clamped before it is made milliseconds.
      OutputDebugString(_T("hMailServer: Testing ScriptWatchdog\n"));
      ScriptWatchdogTester scriptWatchdogTester;
      scriptWatchdogTester.Test();

      // What a domain does to a message: the rule that decides whether a sender
      // is outside, the tag that rule produces, and the reply-chain guard on the
      // disclaimer. All three are decisions made from values, so they are tested
      // here rather than by delivering mail and reading it back - a delivery test
      // proves the wiring, and these prove the rule.
      OutputDebugString(_T("hMailServer: Testing MessageOrigin\n"));
      MessageOriginTester messageOriginTester;
      messageOriginTester.Test();

      // The breach check's hashing, endpoint rules and range parsing, and the
      // verdict from a fetcher that stands in for the network: everything but the
      // request itself, which the regression fixtures make against a fake range
      // server. The hash vectors are what catch a password hashed in the ANSI code
      // page rather than UTF-8, a defect no ASCII password can show.
      OutputDebugString(_T("hMailServer: Testing PasswordBreachCheck\n"));
      PasswordBreachCheckTester passwordBreachCheckTester;
      passwordBreachCheckTester.Test();

      // The outbound content policy's detectors, on published vectors: the
      // card numbers and IBANs every payment page tests with, a phone number
      // and a run of one digit that must not count, the registry's lengths.
      OutputDebugString(_T("hMailServer: Testing ContentPolicy\n"));
      ContentPolicyTester contentPolicyTester;
      contentPolicyTester.Test();

      // The quarantine review: the HELO read back from this server's own
      // Received header and nobody else's, the organizations' domains the
      // pages group by, the public-suffix guard, the bulk request's refusals,
      // and the preview's text of an HTML part.
      OutputDebugString(_T("hMailServer: Testing QuarantineReview\n"));
      QuarantineReviewTester quarantineReviewTester;
      quarantineReviewTester.Test();

      // The quarantine's search (review finding F261): each LIKE with a
      // parameter of its own, in the order SQL Server binds them by position,
      // the text's own wildcards escaped, and a recipient found whole.
      OutputDebugString(_T("hMailServer: Testing QuarantineSearch\n"));
      QuarantineSearchTester quarantineSearchTester;
      quarantineSearchTester.Test();

      // A HELO name or a sender's domain that is not a fully qualified domain
      // name (GitLab issue 51): addresses in and out of brackets, one label, the
      // public suffixes by the same list, and the carve-outs of the refusal.
      OutputDebugString(_T("hMailServer: Testing FqdnCheck\n"));
      FqdnCheckTester fqdnCheckTester;
      fqdnCheckTester.Test();

      OutputDebugString(_T("hMailServer: Testing ExternalSenderTagger\n"));
      ExternalSenderTaggerTester externalSenderTaggerTester;
      externalSenderTaggerTester.Test();

      // Impersonation protection: the folding table, the two judgements and
      // the two lists' canonical forms, all decisions made from values.
      OutputDebugString(_T("hMailServer: Testing ImpersonationCheck\n"));
      ImpersonationCheckTester impersonationCheckTester;
      impersonationCheckTester.Test();

      // Restoring one mailbox from a backup: the shape a file name read out of
      // an archive's index must have before it may name a path.
      OutputDebugString(_T("hMailServer: Testing MailboxRestore\n"));
      MailboxRestoreTester mailboxRestoreTester;
      mailboxRestoreTester.Test();

      // A backup's copy of the data directory and a restore of one, on scratch
      // trees: a destination inside the data directory, however it is written,
      // the temp directory and the live update's folder are left out of the one
      // and left in place by the other; the restore copies first, removes what
      // the archive does not hold only after, undoes a copy that fails, keeps
      // this server's own files at the root and this installation's own
      // self-signed pair, and leaves a file the service may not change.
      OutputDebugString(_T("hMailServer: Testing the backup's exclusions\n"));
      BackupExecuterTester backupExecuterTester;
      backupExecuterTester.Test();

      // The update feed's release page: kept only as an https:// address with a
      // host and nothing a browser would read differently, since it is shown as
      // a link; anything else is dropped and the release is still read.
      OutputDebugString(_T("hMailServer: Testing the update feed's release page\n"));
      UpdateCheckerTester updateCheckerTester;
      updateCheckerTester.Test();

      // A JSON number as an integer: exact from its digits, and held at the end of
      // the range it is asked for when it is past it - never a conversion out of
      // range, which 1e20 was in every REST route that named a row by its id.
      OutputDebugString(_T("hMailServer: Testing JsonValue's integers\n"));
      JsonValueTester jsonValueTester;
      jsonValueTester.Test();

      // The archive index's XML: written byte for byte as every earlier version
      // wrote it, every character read back as written, and indexes that stop
      // inside a tag, a declaration, a comment, a CDATA section or quotes, or
      // nest too deep, and entity references without their ";" - each an error
      // or the text as earlier versions read it.
      OutputDebugString(_T("hMailServer: Testing XmlTree\n"));
      XmlTreeTester xmlTreeTester;
      xmlTreeTester.Test();

      // The quadratic-time sweep of 6.3.6: encoded words, header fields deleted
      // by name and by position, the DKIM and ARC canonicalisations' choice of
      // fields, Sieve's :matches, SPF's cut of a long name, MIME parameters
      // removed, LIST's lines and JSON members - each as 6.3.5 made them, and
      // in one pass over what was its worst.
      OutputDebugString(_T("hMailServer: Testing InputCost\n"));
      InputCostTester inputCostTester;
      inputCostTester.Test();

      // The name the SURBL lookups ask about and the MX-record test falls back
      // to, cut from a host by the compiled-in Public Suffix List: wildcard and
      // exception rules, PRIVATE names, bare suffixes, addresses, and the same
      // name as DMARC's organizational domain.
      OutputDebugString(_T("hMailServer: Testing PublicSuffixList\n"));
      PublicSuffixListTester publicSuffixListTester;
      publicSuffixListTester.Test();

      // The never-ban list by reverse name (GitLab work item #56): the patterns
      // it holds and refuses, the reverse names it may match, the forward
      // confirmation that keeps anybody from naming their own scanner after a
      // listed provider, and the verdicts it keeps and forgets.
      OutputDebugString(_T("hMailServer: Testing NeverBanNameList\n"));
      NeverBanNameListTester neverBanNameListTester;
      neverBanNameListTester.Test();

      // An auto-ban widened to its reverse-name neighbours (GitLab work item
      // #55): the run of a shared registrable domain inside one /24, each
      // direction's stops, the banned address's own name forward-confirmed,
      // a lookup not answered leaving the single address, and the shapes the
      // firewall mirrors.
      OutputDebugString(_T("hMailServer: Testing AutoBanWidener\n"));
      AutoBanWidenerTester autoBanWidenerTester;
      autoBanWidenerTester.Test();

      // UTS 46, which the next reads a link's host with when it is not ASCII:
      // Unicode's own conformance tests, IdnaTestV2.txt, every line - the
      // mapping table, NFC, the validity, bidi and joiner rules, Punycode -
      // and the URL standard's domain parser.
      OutputDebugString(_T("hMailServer: Testing UTS 46 (IDNA)\n"));
      IdnaTester idnaTester;
      idnaTester.Test();

      // The host each link in a message goes to, as a browser reads the link:
      // after the last '@' of its userinfo, without its port, percent-decoded,
      // an IPv4 address in any form a browser takes, HTML's character
      // references and quoted values; the names the URI lists are asked about
      // for it; and the answers the lists publish as errors, never a listing.
      OutputDebugString(_T("hMailServer: Testing SURBL\n"));
      SURBLTester surblTester;
      surblTester.Test();

      // What an address lookup answers when one of its queries fails - a
      // failure, not "no addresses", unless addresses were found - which the
      // HELO and PTR tests read; the lookup the DNS blacklists and SURBL make,
      // of the A record alone and once more past the DNS cache when it fails;
      // a lookup of an alias whose CNAME query failed, which has failed, one
      // answered NODATA whose CNAME query failed, which has no records, and
      // one answered NXDOMAIN, which asks no CNAME query; and a domain's mail
      // servers when an exchange's lookup failed, each address once. Against
      // answers scripted on this thread (upstream PR 640).
      OutputDebugString(_T("hMailServer: Testing DNSResolver\n"));
      DNSResolverTester dnsResolverTester;
      dnsResolverTester.Test();

      // The Return-Path a SpamAssassin scan is sent ahead of the message, and the
      // Return-Path fields left out of the request and of spamd's reply byte for
      // byte, in whatever pieces they arrive; and the path read from the topmost
      // Return-Path of a message collected from an external account, which the
      // scan of such a message is sent in place of a sender guessed from its From
      // field. Functions of values only; platform/public_suffix_tests_main.cpp
      // runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing SpamAssassinReturnPath\n"));
      SpamAssassinReturnPathTester spamAssassinReturnPathTester;
      spamAssassinReturnPathTester.Test();

      // The command lines of the custom virus scanner and ClamWin: the file's
      // path one argument wherever %FILE% stands, quoted or not, and ClamWin's
      // folders without the trailing backslash that escaped their quotes.
      OutputDebugString(_T("hMailServer: Testing scanner command lines\n"));
      ScannerCommandLineTester scannerCommandLineTester;
      scannerCommandLineTester.Test();

      // The restore's check of an archive's entries before anything is
      // extracted: an absolute path, a ".." segment or a link with a target,
      // found in the "7za l -slt" listing, and what a backup holds let through.
      OutputDebugString(_T("hMailServer: Testing Compression\n"));
      CompressionTester compressionTester;
      compressionTester.Test();

      // COM's Language reads its translation from the Languages directory its
      // English came from, not the one ProgramFolder names by its first use.
      OutputDebugString(_T("hMailServer: Testing Language\n"));
      LanguageTester languageTester;
      languageTester.Test();

      // The connection strings: every value of libpq's conninfo read back by
      // libpq's own parser as given, and the server and database names of SQL
      // Server's read back by ADO's, whatever quotes, semicolons or backslashes
      // they hold - a password above all, which up to 6.3.5 could end its value.
      OutputDebugString(_T("hMailServer: Testing the PostgreSQL connection string\n"));
      PGConnectionTester pgConnectionTester;
      pgConnectionTester.Test();

      // MySQL's values as the client is handed them, and the database's name quoted
      // in the statements that name it.
      OutputDebugString(_T("hMailServer: Testing the MySQL connection's text\n"));
      MySQLConnectionTester mySqlConnectionTester;
      mySqlConnectionTester.Test();

#ifndef HM_PLATFORM_POSIX
      OutputDebugString(_T("hMailServer: Testing the SQL Server connection string\n"));
      ADOConnectionTester adoConnectionTester;
      adoConnectionTester.Test();

      // SQL Server Compact's: the database file's path and its password read back
      // by ADO whatever semicolons, quotes or equals signs they hold.
      OutputDebugString(_T("hMailServer: Testing the SQL Server Compact connection string\n"));
      SQLCEConnectionTester sqlCeConnectionTester;
      sqlCeConnectionTester.Test();
#endif

      // A profile verb asked for by an event script that an apply's or a revert's
      // own write ran, on the same thread: refused at once, not a second lock of
      // the mutex that thread holds.
      OutputDebugString(_T("hMailServer: Testing the profiles' lock\n"));
      ProfilesTester profilesTester;
      profilesTester.Test();

      // The .zip the person's export and an eDiscovery export are written as:
      // from 65,535 entries on the count goes into the ZIP64 end record, where a
      // 16-bit count alone would have said 70,000 was 4,464.
      OutputDebugString(_T("hMailServer: Testing ZipWriter\n"));
      ZipWriterTester zipWriterTester;
      zipWriterTester.Test();

      // The per-destination back-off: the signal table, the window and its
      // doubling to the ceiling, the probe, all against a clock the test moves.
      OutputDebugString(_T("hMailServer: Testing DestinationBackoff\n"));
      DestinationBackoffTester destinationBackoffTester;
      destinationBackoffTester.Test();

      // The IP warm-up's arithmetic: the three plans' ceilings on sample days,
      // the day a schedule is on, the dates and addresses the routes accept,
      // and which provider a list of mail hosts is.
      OutputDebugString(_T("hMailServer: Testing WarmupPlan\n"));
      WarmupPlanTester warmupPlanTester;
      warmupPlanTester.Test();

      // The SNMP agent's decoder and engine: the BER both ways and its bounds,
      // the USM cryptography (AES against NIST's vectors, HMAC-SHA-256 and the
      // SHA-256 key localisation against values computed independently),
      // SNMPv2c and SNMPv3 exchanges against a clock the test sets, and packets
      // truncated, malformed and turned over - none unanswerable, none uncounted.
      OutputDebugString(_T("hMailServer: Testing SNMP\n"));
      SnmpTester snmpTester;
      snmpTester.Test();

      // The upgrade chain walked in-process: the next script found by its
      // shape, and - on a Compact database of the tester's own, with a chain of
      // its own - a database at N-1 reaching N one step at a time, one at N
      // left alone, and a failing statement leaving the version where it was
      // and naming the step.
      OutputDebugString(_T("hMailServer: Testing SchemaUpgrader\n"));
      SchemaUpgraderTester schemaUpgraderTester;
      schemaUpgraderTester.Test();

#ifndef HM_PLATFORM_POSIX
      // The database layer's row counts and its UTC clock (ActiveActive.md, D1 and
      // D3), on a SQL Server Compact database of the tester's own - the engine the
      // Windows gate runs on - and on the server's own database when it is SQL
      // Server: what an UPDATE that matches a row and leaves it as it was counts, a
      // claim that recognises its own epoch when it is run again, two connections
      // racing for the same rows and named leases, a reconnect said, a lease on the
      // UTC clock refused, renewed, taken and fenced, and whether Compact's
      // provider has a UTC function of its own.
      OutputDebugString(_T("hMailServer: Testing the database's row counts, claims and UTC clock\n"));
      DatabaseClaimTester databaseClaimTester;
      databaseClaimTester.TestCompact();
#endif

#ifndef HM_PLATFORM_POSIX
      // The transaction scope the persistence layer uses (DatabaseTransaction),
      // on a Compact database and a pool of the tester's own: a rollback
      // discards, the pool's own statements on the transaction's thread join it,
      // two transactions on one row wait for each other, a lock wait that times
      // out is run again whole by Run, and a connection lost in the middle
      // commits nothing and the retry commits once - each beside the control
      // that shows the same thing without it - and an insert a foreign key
      // refused is not taken for a row that is there already.
      // database-transaction-tests runs the same on PostgreSQL, MySQL and MariaDB.
      OutputDebugString(_T("hMailServer: Testing the database transaction scope\n"));
      DatabaseTransactionTester databaseTransactionTester;
      databaseTransactionTester.TestCompact();

      // And, only while event scripts run, a statement that fails inside a
      // transaction on the server's own database, so that the OnError script of
      // RegressionTests.API.Events' fixture runs while it is reported - outside
      // the transaction, which the fixture reads from what the script saved.
      databaseTransactionTester.TestOnErrorOnTheServersDatabase();
#endif

      // The end of the greylisting key migration (schema 6107): the key against
      // MD5 vectors, what becomes of a row without one, which copy of a triplet
      // is kept; on a Compact database of the tester's own, a pass stopped
      // part-way and resumed, and the step's script refusing a NULL key and a
      // second row. Then, on the server's own database, an insert refused by
      // the unique key answered as the triplet that is there.
      OutputDebugString(_T("hMailServer: Testing the greylisting key\n"));
      PreReqGreyListKeysTester greyListKeysTester;
      greyListKeysTester.Test();
      GreyListingTester greyListingTester;
      greyListingTester.Test();

#ifndef HM_PLATFORM_POSIX
      // The grant hMailServer.exe /GrantServiceAccess makes (docs/ServiceAccount.md),
      // on a directory of the tester's own and a SID of its own: made, made again
      // with nothing changed, the DACL it leaves, inherited by the files in it,
      // an administrator's own entry kept, a protected file granted without
      // losing its protection, a hard-linked file and a path through a link
      // refused. Which folders a grant may reach - a [Directories] value naming
      // Bin among them - and the logon /Register passes for each
      // --service-account. The service SID against the one Windows publishes.
      OutputDebugString(_T("hMailServer: Testing ServiceAccount\n"));
      ServiceAccountTester serviceAccountTester;
      serviceAccountTester.Test();
#endif

      // The settings block swapped under load: a thousand publishes and more while
      // this thread reads a String getter ten thousand times, every read one of the
      // two values; a save of a key marked now in effect when it returns and told to
      // its subscriber once; a key marked reinitialize held at its running value and
      // pending until the stored value is put back (docs/SettingsApply.md).
      OutputDebugString(_T("hMailServer: Testing IniFileSettings\n"));
      IniFileSettingsTester iniFileSettingsTester;
      iniFileSettingsTester.Test();

      OutputDebugString(_T("hMailServer: Testing DisclaimerAdder\n"));
      DisclaimerAdderTester disclaimerAdderTester;
      disclaimerAdderTester.Test();

      JournalingTester journalingTester;
      journalingTester.Test();

      // Which messages this server cannot read. Six parts of the server act on
      // the answer - the footer, the signature, the blocked-attachment
      // rewriter, the attachment stripper, the virus verdict and the rule
      // engine - so a false positive here stops an ordinary message being
      // filtered and a false negative destroys a sealed one.
      OutputDebugString(_T("hMailServer: Testing SealedMessage\n"));
      SealedMessageTester sealedMessageTester;
      sealedMessageTester.Test();

      OutputDebugString(_T("hMailServer: Testing InlineImageParts\n"));
      InlineImagePartsTester inlineImagePartsTester;
      inlineImagePartsTester.Test();

      // The MaxMind database reader, against databases these tests write byte by
      // byte, and the country policy grammar that decides what one means.
      GeoIpDatabaseTester geoIpDatabaseTester;
      geoIpDatabaseTester.Test();

      CountryBlockingTester countryBlockingTester;
      countryBlockingTester.Test();

      // DKIM keys that rotate: every clock boundary of the walk, the record match,
      // the selector rules and a generated key's round trip through a signature.
      // The regression fixtures prove the wiring against the suite's DNS zone;
      // these prove the rule, at the second either side of each period.
      OutputDebugString(_T("hMailServer: Testing DkimRotationPlan\n"));
      DkimRotationTester dkimRotationTester;
      dkimRotationTester.Test();

      // The rotation's row, written when another node made it between this
      // pass's lookup and its insert: updated, not left as the other node wrote
      // it because the refused insert answered success.
      OutputDebugString(_T("hMailServer: Testing PersistentDkimRotation\n"));
      PersistentDkimRotationTester persistentDkimRotationTester;
      persistentDkimRotationTester.Test();

      // Bounces read, and the suppression list: the classification that decides
      // whether an address is dead, the report parser that reads hostile input,
      // the gate a report must pass, and the key that ties a report to a message
      // this server really sent. All four are functions of values.
      OutputDebugString(_T("hMailServer: Testing BounceClassifier\n"));
      BounceClassifierTester bounceClassifierTester;
      bounceClassifierTester.Test();

      OutputDebugString(_T("hMailServer: Testing DeliveryReportParser\n"));
      DeliveryReportParserTester deliveryReportParserTester;
      deliveryReportParserTester.Test();

      OutputDebugString(_T("hMailServer: Testing the bounce report gate\n"));
      BounceReportGateTester bounceReportGateTester;
      bounceReportGateTester.Test();

      OutputDebugString(_T("hMailServer: Testing the bounce report key\n"));
      BounceKeyTester bounceKeyTester;
      bounceKeyTester.Test();

      // The list manager: its address grammar, VERP both ways, the token rule
      // and the member tag; the archive's month arithmetic.
      OutputDebugString(_T("hMailServer: Testing ListManager\n"));
      ListManagerTester listManagerTester;
      listManagerTester.Test();

      OutputDebugString(_T("hMailServer: Testing ListArchive\n"));
      ListArchiveTester listArchiveTester;
      listArchiveTester.Test();

      // Brand logos: the BIMI record, the DMARC policy that has to enforce, and
      // the SVG Tiny PS check a logo from a stranger's server has to pass.
      OutputDebugString(_T("hMailServer: Testing BrandLogo\n"));
      BrandLogoTester brandLogoTester;
      brandLogoTester.Test();

      // The type a file sent as a link and a contact's photo go out under: a
      // type on the list of those no browser runs or renders, or
      // application/octet-stream - for every JavaScript name, a list of types,
      // and a parameter other than a charset. Functions of values only;
      // platform/media_type_tests_main.cpp runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing InertMediaType\n"));
      InertMediaTypeTester inertMediaTypeTester;
      inertMediaTypeTester.Test();

      // Importing with a sign-in: base64url both ways, the S256 challenge against
      // RFC 7636's example, and the address an id_token names.
      OutputDebugString(_T("hMailServer: Testing ReaderOAuth\n"));
      ReaderOAuthTester readerOAuthTester;
      readerOAuthTester.Test();

      // Migration jobs: the table of the three kinds of source with the hosts,
      // endpoints and OAuth scopes each needs (the Google and Microsoft kinds
      // are not exercised end to end on a bench that has neither provider),
      // Graph's contacts and events as the vCards and iCalendar objects the
      // stores take, a multistatus read for its cards, the IMAP replies the
      // client reads, and a job's report through its own JSON and back.
      OutputDebugString(_T("hMailServer: Testing migration jobs\n"));
      MigrationSourcesTester migrationSourcesTester;
      migrationSourcesTester.Test();
      MigrationImapClientTester migrationImapClientTester;
      migrationImapClientTester.Test();
      MigrationJobTester migrationJobTester;
      migrationJobTester.Test();

      // The PST reader: the format's CRC and encodings, compressed RTF against
      // [MS-OXRTFCP]'s own examples, HTML carried in RTF, the header's
      // refusals, data trees, subnode trees, heaps, property contexts and
      // tables built byte by byte in memory - with the damage the reader must
      // refuse without looping or allocating what a file claims - and the
      // import's rules for names, keywords and dates.
      OutputDebugString(_T("hMailServer: Testing the PST reader\n"));
      PstTester pstTester;
      pstTester.Test();

      // The PST reader's ANSI files and what is not mail: an ANSI NDB layer
      // built byte by byte, the recurrence blob cut at every length, the time
      // zones across a clock change, and the iCalendar and vCard written from
      // an appointment, a task and a contact, through this server's own
      // ICalendar and VCard.
      OutputDebugString(_T("hMailServer: Testing the PST reader's ANSI files, contacts, calendar and tasks\n"));
      PstItemsTester pstItemsTester;
      pstItemsTester.Test();

      // What the PST reader's process and the service say to each other: each
      // record written and read back, frames read from a stream in pieces,
      // what a reply cut short, a count past its payload or a header that
      // claims 300 MB is refused with, and each status thrown in the service as
      // what the reader threw.
      OutputDebugString(_T("hMailServer: Testing the PST reader's protocol\n"));
      PstReaderWireTester pstReaderWireTester;
      pstReaderWireTester.Test();

      // Passkeys: the CBOR attestation object, the COSE key inside it, the
      // authenticator data, the two signature algorithms and what each reader
      // refuses, against vectors a software authenticator produced.
      OutputDebugString(_T("hMailServer: Testing WebAuthn\n"));
      WebAuthnTester webAuthnTester;
      webAuthnTester.Test();

      // And the challenges a passkey stands on: a sign-in's spent once and
      // never held here, and a full registration table evicts its oldest rather
      // than refusing the next person.
      OutputDebugString(_T("hMailServer: Testing PasskeyStore\n"));
      PasskeyStoreTester passkeyStoreTester;
      passkeyStoreTester.Test();

      // Who may push whom out of a full table of sign-in state: the browser
      // sessions and the passkey registrations.
      OutputDebugString(_T("hMailServer: Testing SignInBudget\n"));
      SignInBudgetTester signInBudgetTester;
      signInBudgetTester.Test();

      // The pending single sign-on and passkey sign-ins, sealed into the
      // browser rather than held in a table here.
      OutputDebugString(_T("hMailServer: Testing SignInState\n"));
      SignInStateTester signInStateTester;
      signInStateTester.Test();

      // Masked addresses: the local part a label gives.
      OutputDebugString(_T("hMailServer: Testing MaskedAddresses\n"));
      MaskedAddressesTester maskedAddressesTester;
      maskedAddressesTester.Test();

      // The OpenPGP key reader the Web Key Directory stands on: the packet
      // framing, the fingerprint of each key version against vectors GnuPG and
      // hashlib produced, the refusal of a secret key, and the hashed local part
      // against the example in draft-koch-openpgp-webkey-service.
      OutputDebugString(_T("hMailServer: Testing OpenPgpKey\n"));
      OpenPgpKeyTester openPgpKeyTester;
      openPgpKeyTester.Test();

      // The one OpenPGP message this server writes - a zero-access mailbox's
      // delivery, encrypted to the account's own key: the X25519 arithmetic
      // against RFC 7748, the packets' shape against RFC 9580, and the
      // message opened again with the session key the test fixed.
      OutputDebugString(_T("hMailServer: Testing OpenPgpWriter\n"));
      OpenPgpWriterTester openPgpWriterTester;
      openPgpWriterTester.Test();

      // Gateway encryption to a partner domain: the partner's copy's header
      // (the signatures over the old body off, the subject kept), what is
      // encrypted end to end already, the keys refused and the ones taken, and
      // a message sealed to an OpenPGP key and enveloped to a certificate.
      OutputDebugString(_T("hMailServer: Testing GatewayEncryption\n"));
      GatewayEncryptionTester gatewayEncryptionTester;
      gatewayEncryptionTester.Test();

      // SAML sign-in's ground: the strict XML reader and Exclusive XML
      // Canonicalization held to the W3C's own examples byte for byte (a
      // failure names the example), and XML-Signature verification against
      // vectors .NET's SignedXml signed, every single-byte change to them
      // refused, and each refusal - SHA-1, HMAC, an XSLT transform, an empty
      // Reference, a second signature, an ID written twice - by name.
      OutputDebugString(_T("hMailServer: Testing XmlCanonicalization\n"));
      XmlCanonicalizationTester xmlCanonicalizationTester;
      xmlCanonicalizationTester.Test();

      OutputDebugString(_T("hMailServer: Testing XmlSignature\n"));
      XmlSignatureTester xmlSignatureTester;
      xmlSignatureTester.Test();

      // Complaints read, the feedback loops: the RFC 5965 parser that is handed
      // hostile input by construction, and the decisions taken on what it read -
      // which mailbox is the complaint mailbox, which feedback types count, what
      // makes two reports one complaint, and the rate arithmetic.
      OutputDebugString(_T("hMailServer: Testing FeedbackReportParser\n"));
      FeedbackReportParserTester feedbackReportParserTester;
      feedbackReportParserTester.Test();

      OutputDebugString(_T("hMailServer: Testing ComplaintTracker\n"));
      ComplaintTrackerTester complaintTrackerTester;
      complaintTrackerTester.Test();

      // The reports our own domains receive: the XML reader, the gzip and zip
      // unwrapping, the MIME walk, the TLS-RPT reader and the keys - every one
      // of them handed hostile input by construction.
      OutputDebugString(_T("hMailServer: Testing DomainReports\n"));
      DomainReportsTester domainReportsTester;
      domainReportsTester.Test();

      // The reputation watch's rules: the two settings as an administrator writes
      // them, which answers mean listed, which addresses are worth asking about,
      // and the precedence that keeps a refusal from reading as a verdict. None of
      // it touches DNS or the database, which is why it is tested here.
      OutputDebugString(_T("hMailServer: Testing ReputationWatch\n"));
      ReputationWatchTester reputationWatchTester;
      reputationWatchTester.Test();

      // Rooms and equipment: the message's own record of a DMARC pass, which a
      // room reads before it believes an organiser from outside, never longer
      // than its column whatever the domain - a value that did not fit would
      // fail the message's save - and nothing but DMARC for the organiser's
      // own domain vouching for them. Functions of values only.
      OutputDebugString(_T("hMailServer: Testing ResourceBooking\n"));
      ResourceBookingTester resourceBookingTester;
      resourceBookingTester.Test();

      // iMIP on delivery: who may change a stored event - what decides that
      // mail from outside was sent by an address (the one From a header
      // names, and DMARC for exactly that address's domain), and what a
      // REPLY or a CANCEL changes given who sent it - with every case that
      // must not change or vouch for anything. Functions of values only;
      // platform/imip_tests_main.cpp runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing Imip\n"));
      ImipTester imipTester;
      imipTester.Test();

      // The message store's container (docs/MessageStore.md): a large part found
      // and cut out at its wire bytes, the container encoded and decoded back to
      // the message byte for byte, its start read without the blob, a changed
      // byte and a file cut short refused, and a message that begins with the
      // magic kept as a container with no stages. Functions of values only.
      OutputDebugString(_T("hMailServer: Testing MessageStoreFormat\n"));
      MessageStoreFormat::Test();

      // The object store's request signatures (docs/ObjectStorage.md): AWS
      // Signature Version 4 against AWS's own test suite - the get-vanilla
      // family - and the four S3 examples of its documentation, and Azure's
      // Shared Key against a string to sign written out from the
      // specification. Functions of values only; platform/
      // object_store_signature_tests_main.cpp runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing ObjectStoreSignature\n"));
      ObjectStoreSignatureTester objectStoreSignatureTester;
      objectStoreSignatureTester.Test();

      // Mailbox encryption at rest (docs/EncryptionAtRest.md): X25519 against
      // RFC 7748's vector, HKDF-SHA256 against RFC 5869's, and every seal and
      // wrap round-tripped with its refusals - another key, a wrong secret, a
      // changed byte, a copy made for another purpose. The container's own
      // encryption stage is in MessageStoreFormat's test above.
      OutputDebugString(_T("hMailServer: Testing MailboxCrypto\n"));
      MailboxCrypto::Test();

      // The idle sweep closes no key in use: a key used in the clock's next
      // second while a sweep waits for the keys' lock stays open. Until 10
      // October 2026 a sweep read the clock before it took the lock, and such a
      // use read as a clock gone back (MailboxKeys.h).
      OutputDebugString(_T("hMailServer: Testing the mailbox keys' idle sweep against a key in use\n"));
      MailboxKeyIdleTester mailboxKeyIdleTester;
      mailboxKeyIdleTester.Test();

      // The message indexer's thread stopped and waited for before what it uses
      // goes, and a Start straight after a Stop beginning a new one, over a
      // stand-in pass with no database (Application/MessageIndexer.h). Until 10
      // October 2026 a Stop interrupted and returned: a pass went on to the
      // database a Reinitialize had closed, and the Start after it started none.
      // And a Start on the indexer's own thread, as an OnError script makes one,
      // neither asks the thread to join itself nor ends it after a Stop there; a
      // Stop with a limit gives up at it; the indexer's own Stop and Start while
      // a Stop waits for it are not made to wait for that stop; a Stop ends a
      // pass that swallowed its request to stop; and the servers' mutex such a
      // Stop holds is not waited for on the indexer's own thread.
      OutputDebugString(_T("hMailServer: Testing the message indexer's stop and start\n"));
      MessageIndexerTester messageIndexerTester;
      messageIndexerTester.Test();

      // The servers' mutex, which every stop, start and reinitialise of the
      // servers holds (Application/Application.h): a reinitialise, a stop or a
      // start asked on the thread that holds it - by a script the stop or the
      // start runs - or on the message indexer's own thread while a stop that
      // holds it waits for the indexer - by an OnError script - is refused at
      // once, where it would have waited for ever; and a reinitialise asked on
      // the indexer's own thread is refused with the mutex free too, touching
      // nothing, where it stopped the servers and ended part way. What it takes
      // it lets go of at once, and it starts and stops nothing.
      OutputDebugString(_T("hMailServer: Testing the servers' mutex\n"));
      ServersLockTester serversLockTester;
      serversLockTester.Test();

      // A work queue whose stop gives up on a thread still inside it, removed
      // through the manager as the server's queues are: kept, with what it was
      // given to keep alive, until the thread has left, and freed by the sweep
      // after (WorkQueueManager::RemoveQueue). workqueue-teardown-tests runs
      // the same on Linux.
      OutputDebugString(_T("hMailServer: Testing a work queue's teardown while a thread is still in it\n"));
      WorkQueueTeardownTester workQueueTeardownTester;
      workQueueTeardownTester.Test();

      // The TCP/IP service across a stop of the servers that gives up on a task
      // of a session queue, and on a handler on a listener's strand: kept until
      // the thread has left and freed after, and the start made meanwhile
      // listening on that listener's port. It stops and starts the servers.
      OutputDebugString(_T("hMailServer: Testing the TCP/IP service across a stop that gives up on one of its threads\n"));
      IOServiceLifetimeTester ioServiceLifetimeTester;
      ioServiceLifetimeTester.Test();

      // The close before the process ends (Application::ShutDown), over a
      // manager of the test's own with stand-in connections: an idle one is
      // closed at once and one in use only when it is handed back, nothing is
      // handed out meanwhile, and SQL Server Compact's flush interval is waited
      // out for one still in use; a connection made after the close is closed,
      // not pooled, and one the start's checks run on is held for them. The
      // server's own manager is not touched.
      OutputDebugString(_T("hMailServer: Testing the database's close before the process ends\n"));
      DatabaseConnectionManagerTester databaseConnectionManagerTester;
      databaseConnectionManagerTester.Test();

      // Parameter substitution matters most on the two backends the bench does not
      // usually run: MySQL and PostgreSQL report GetSupportsCommandParameters() as
      // false, so they take the string-substitution path, while MSSQL and SQL CE use
      // real parameters and never exercise it. That is why this is tested here in
      // process rather than through a query - the defect is invisible on the backend
      // the suite runs against.
      OutputDebugString(_T("hMailServer: Testing SQLStatement parameter substitution\n"));
      SQLStatementTester sqlStatementTester;
      sqlStatementTester.Test();

      // The two readers of a statement's literals - the marker test and the
      // masking of the slow-query line and the trace - each literal read as each
      // engine reads it (DALConnection::EndOfLiteral).
      OutputDebugString(_T("hMailServer: Testing the reading of a statement's literals\n"));
      SqlLiteralScanTester sqlLiteralScanTester;
      sqlLiteralScanTester.Test();

      // The sort's copy of the headers of a message that is gone by the time the
      // indexer writes it: inserted as nothing, against the bench's database.
      OutputDebugString(_T("hMailServer: Testing the write of the sort's copy for a message that is gone\n"));
      MessageMetaDataTester messageMetaDataTester;
      messageMetaDataTester.Test();

      OutputDebugString(_T("hMailServer: Testing TimeoutCalculator\n"));
      TimeoutCalculatorTester timeoutCalculatortester;
      timeoutCalculatortester.Test();

      OutputDebugString(_T("hMailServer: Test DateTime\n"));
      DateTimeTests tests;
      tests.Test();

      OutputDebugString(_T("hMailServer: Testing Utilities\n"));
      UtilitiesTester *pTestUtilities = new UtilitiesTester();
      pTestUtilities->Test();
      delete pTestUtilities;

      OutputDebugString(_T("hMailServer: Test BLChecktester\n"));
      BLCheckTester blchecktester;
      blchecktester.Test();

      // The grammar, the macros, check_host() and the openspf.org conformance suite
      // for RFC 7208 - 203 cases, all of them, none of them touching the network.
      OutputDebugString(_T("hMailServer: Testing SPF against RFC 7208 and its conformance suite\n"));
      SPFTester *pSPF = new SPFTester();
      pSPF->Test();
      delete pSPF;

      OutputDebugString(_T("hMailServer: Testing SHA256\n"));
      HashCreatorTester tester;
      tester.Test();

      // Web Push, against the worked example of RFC 8291 section 5 and appendix A:
      // the salt, both key pairs, the auth secret and the plaintext are the RFC's,
      // so the encryption is proved against the specification rather than against
      // another copy of itself. Then the person's own choices - the sender list,
      // the labels and the quiet hours - and the payload each level of detail is
      // allowed to carry.
      OutputDebugString(_T("hMailServer: Testing Web Push against RFC 8291 and RFC 8292\n"));
      {
         WebPushTester webPushTester;
         webPushTester.Test();

         PushSubscriptionsTester pushSubscriptionsTester;
         pushSubscriptionsTester.Test();

         PushNotifierTester pushNotifierTester;
         pushNotifierTester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing password hash dispatch (Crypt)\n"));
      {
         // Validate the exact server-side login dispatch (EnCrypt -> GetHashType ->
         // Validate) for the strong password KDFs, including Argon2id.
         const String testPwd = _T("Corr3ct H0rse Battery Staple!");

         String argonHash = Crypt::Instance()->EnCrypt(testPwd, Crypt::ETArgon2id);
         if (Crypt::Instance()->GetHashType(argonHash) != Crypt::ETArgon2id)
            throw 0;
         if (!Crypt::Instance()->Validate(testPwd, argonHash, Crypt::ETArgon2id))
            throw 0;
         if (Crypt::Instance()->Validate(_T("the wrong password"), argonHash, Crypt::ETArgon2id))
            throw 0;

         String pbkdf2Hash = Crypt::Instance()->EnCrypt(testPwd, Crypt::ETPBKDF2);
         if (Crypt::Instance()->GetHashType(pbkdf2Hash) != Crypt::ETPBKDF2)
            throw 0;
         if (!Crypt::Instance()->Validate(testPwd, pbkdf2Hash, Crypt::ETPBKDF2))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing OAuth2TokenValidator\n"));
      {
         OAuth2TokenValidatorTester oauth2Tester;
         oauth2Tester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing TOTP against the RFC 6238 vectors\n"));
      {
         TotpTester totpTester;
         totpTester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing the DMARC tree walk (RFC 9989 4.10.2)\n"));
      {
         DmarcTreeWalkTester treeWalkTester;
         treeWalkTester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing SRS\n"));
      {
         // Sender Rewriting Scheme: sign/verify round-trip + tamper detection.
         const String secret = _T("srs-test-secret-key");
         const String original = _T("alice@external-sender.com");
         const String forwarder = _T("relay.example.com");

         String srs = SRS::Forward(original, forwarder, secret);

         if (srs.IsEmpty())
            throw 0;
         if (!SRS::IsSRS0(srs))
            throw 0;
         if (srs.Find(_T("@relay.example.com")) < 0)
            throw 0;

         // A valid SRS0 address reverses to the original sender.
         String reversed;
         if (!SRS::Reverse(srs, secret, reversed))
            throw 0;
         if (reversed.CompareNoCase(original) != 0)
            throw 0;

         // Tamper detection: a flipped character in the signed local-part fails.
         wchar_t hashChar = srs.GetAt(5);
         String tampered = srs.Mid(0, 5) + ((hashChar == L'0') ? _T("1") : _T("0")) + srs.Mid(6);
         if (SRS::Reverse(tampered, secret, reversed))
            throw 0;

         // A different secret must not validate.
         if (SRS::Reverse(srs, _T("a-different-secret"), reversed))
            throw 0;

         // A plain address is neither detected as SRS nor reversible.
         if (SRS::IsSRS0(_T("bob@example.com")))
            throw 0;
         if (SRS::Reverse(_T("bob@example.com"), secret, reversed))
            throw 0;

         // An original local-part containing '=' and '+' survives the round trip.
         const String original2 = _T("a=b+tag@ext.example.org");
         String srs2 = SRS::Forward(original2, forwarder, secret);
         if (srs2.IsEmpty())
            throw 0;
         String reversed2;
         if (!SRS::Reverse(srs2, secret, reversed2))
            throw 0;
         if (reversed2.CompareNoCase(original2) != 0)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing BATV\n"));
      {
         // BATV (prvs): sign/verify round-trip + tamper and wrong-secret detection.
         const String secret = _T("batv-test-secret-key");
         const String mailbox = _T("alice@example.test");

         String tagged = BATV::Sign(mailbox, secret);

         if (tagged.IsEmpty())
            throw 0;
         if (!BATV::IsPrvs(tagged))
            throw 0;
         // The original domain is preserved (so SPF stays aligned).
         if (tagged.Find(_T("@example.test")) < 0)
            throw 0;
         if (tagged.Find(_T("prvs=")) != 0)
            throw 0;

         // A valid prvs address verifies back to the original mailbox.
         String original;
         if (!BATV::Verify(tagged, secret, original))
            throw 0;
         if (original.CompareNoCase(mailbox) != 0)
            throw 0;

         // Tamper detection: flip a character of the signature (the last hex digit
         // of the 10-char tag value, at index 5 + 9 = 14 of the local-part).
         wchar_t sigChar = tagged.GetAt(14);
         String tampered = tagged.Mid(0, 14) + ((sigChar == L'0') ? _T("1") : _T("0")) + tagged.Mid(15);
         if (BATV::Verify(tampered, secret, original))
            throw 0;

         // A different secret must not validate.
         if (BATV::Verify(tagged, _T("a-different-secret"), original))
            throw 0;

         // A plain address is neither detected as prvs nor verifiable, and an
         // empty sender (a bounce) is never signed.
         if (BATV::IsPrvs(_T("bob@example.test")))
            throw 0;
         if (BATV::Verify(_T("bob@example.test"), secret, original))
            throw 0;
         if (!BATV::Sign(_T(""), secret).IsEmpty())
            throw 0;
         // An already-tagged address is never double-signed.
         if (!BATV::Sign(tagged, secret).IsEmpty())
            throw 0;

         // An original local-part containing '=' survives the round trip.
         const String mailbox2 = _T("a=b@example.test");
         String tagged2 = BATV::Sign(mailbox2, secret);
         if (tagged2.IsEmpty())
            throw 0;
         String original2;
         if (!BATV::Verify(tagged2, secret, original2))
            throw 0;
         if (original2.CompareNoCase(mailbox2) != 0)
            throw 0;
      }

      // The windows the two schemes' timestamps are held to, at fixed times. A
      // timestamp is a day number modulo a cycle - 1024 days for SRS, 1000 for BATV -
      // so one day ahead of today and one day short of a whole cycle old read the
      // same, and both schemes accepted "one day ahead" as clock skew: an SRS address
      // 1023 days old, and a BATV tag 999 days old, passed as fresh (upstream #649).
      // Tomorrow's stamp is now taken in the last hour of today only, and an
      // address's last day still ends at midnight by this server's clock.
      OutputDebugString(_T("hMailServer: Testing the SRS timestamp window\n"));
      {
         const String secret = _T("srs-test-secret-key");
         const String original = _T("alice@external-sender.com");
         const String forwarder = _T("relay.example.com");
         const time_t minute = 60;
         const time_t day = 86400;
         const time_t t0 = 1760000000; // 9 October 2025, 08:53 UTC
         String reversed;

         const String made = SRS::ForwardAt(original, forwarder, secret, t0);
         if (made.IsEmpty())
            throw 0;
         if (!SRS::ReverseAt(made, secret, t0, reversed) || reversed.CompareNoCase(original) != 0)
            throw 0;

         // Good for 21 days, and not on the 22nd.
         if (!SRS::ReverseAt(made, secret, t0 + 21 * day, reversed))
            throw 0;
         if (SRS::ReverseAt(made, secret, t0 + 22 * day, reversed))
            throw 0;

         // To the end of the 21st day by this server's clock - 23:30 UTC on it - as
         // before: the hour of skew is for tomorrow's stamp, and does not take the
         // last hour off an address's last day.
         if (!SRS::ReverseAt(made, secret, (t0 / day) * day + 22 * day - 30 * minute, reversed))
            throw 0;

         // 1023 days old reads one day ahead.
         const String ancient = SRS::ForwardAt(original, forwarder, secret, t0 - 1023 * day);
         if (ancient.IsEmpty() || SRS::ReverseAt(ancient, secret, t0, reversed))
            throw 0;

         // A node whose clock is half an hour ahead stamps tomorrow at 23:45 today.
         const time_t lateEvening = (t0 / day) * day + day - 15 * minute;
         const String ahead = SRS::ForwardAt(original, forwarder, secret, lateEvening + 30 * minute);
         if (ahead.IsEmpty() || !SRS::ReverseAt(ahead, secret, lateEvening, reversed))
            throw 0;

         // Two days ahead is no clock skew.
         const String farAhead = SRS::ForwardAt(original, forwarder, secret, t0 + 2 * day);
         if (farAhead.IsEmpty() || SRS::ReverseAt(farAhead, secret, t0, reversed))
            throw 0;

         // The hash is hex in either case; one a digit short is refused. It is
         // compared in constant time, which no assertion here can see.
         String upperHash = made.Mid(5, 8);
         upperHash.MakeUpper();
         if (!SRS::ReverseAt(made.Mid(0, 5) + upperHash + made.Mid(13), secret, t0, reversed))
            throw 0;
         if (SRS::ReverseAt(made.Mid(0, 5) + made.Mid(6), secret, t0, reversed))
            throw 0;

         // An address no server would take is not made, and the sender is forwarded
         // as it came: a quoted local part, and a path past RFC 5321's 254 characters
         // (5 + 8 + 1 + 2 + 1 + 19 + 1 + 200 + 1 + 17 = 255) - but one inside it is.
         if (!SRS::ForwardAt(_T("\"john doe\"@external-sender.com"), forwarder, secret, t0).IsEmpty())
            throw 0;

         String longLocal;
         for (int i = 0; i < 200; i++)
            longLocal += _T("a");
         if (!SRS::ForwardAt(longLocal + _T("@external-sender.com"), forwarder, secret, t0).IsEmpty())
            throw 0;
         if (SRS::ForwardAt(longLocal.Mid(0, 150) + _T("@external-sender.com"), forwarder, secret, t0).IsEmpty())
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the BATV day window\n"));
      {
         const String secret = _T("batv-test-secret-key");
         const String mailbox = _T("alice@example.test");
         const time_t minute = 60;
         const time_t day = 86400;
         const time_t t0 = 1760000000;
         String original;

         // prvs=1DDDhhhhhh=alice@example.test
         const String tagged = BATV::SignAt(mailbox, secret, t0);
         if (tagged.IsEmpty() || !BATV::VerifyAt(tagged, secret, t0, original) || original.CompareNoCase(mailbox) != 0)
            throw 0;

         // Good for 7 days, and not on the 8th.
         if (!BATV::VerifyAt(tagged, secret, t0 + 7 * day, original))
            throw 0;
         if (BATV::VerifyAt(tagged, secret, t0 + 8 * day, original))
            throw 0;

         // To the end of the 7th day by this server's clock - 23:30 UTC on it - as
         // before.
         if (!BATV::VerifyAt(tagged, secret, (t0 / day) * day + 8 * day - 30 * minute, original))
            throw 0;

         // 999 days old reads one day ahead.
         const String ancient = BATV::SignAt(mailbox, secret, t0 - 999 * day);
         if (ancient.IsEmpty() || BATV::VerifyAt(ancient, secret, t0, original))
            throw 0;

         // Half an hour of clock skew across midnight is still taken.
         const time_t lateEvening = (t0 / day) * day + day - 15 * minute;
         const String ahead = BATV::SignAt(mailbox, secret, lateEvening + 30 * minute);
         if (ahead.IsEmpty() || !BATV::VerifyAt(ahead, secret, lateEvening, original))
            throw 0;

         // The signature is hex in either case.
         String upperSignature = tagged.Mid(9, 6);
         upperSignature.MakeUpper();
         if (!BATV::VerifyAt(tagged.Mid(0, 9) + upperSignature + tagged.Mid(15), secret, t0, original))
            throw 0;
      }

      // What a hash, a MAC or a key is made over: every character, as UTF-8
      // (Unicode::ToUtf8Exact), where AnsiString(String) stopped at the first
      // character above U+00FF on Windows and wrote NUL bytes for the rest, and
      // wrote '?' for each on Linux. Each check against the old conversion below
      // is the proof that a tag, a key or a signature made before 6.3.4 over
      // ASCII is made again the same way; each check over Cyrillic text is the
      // negative control, which the old conversion fails on both platforms.
      OutputDebugString(_T("hMailServer: Testing UTF-8 for hashes\n"));
      {
         AnsiString bytes;
         if (!Unicode::ToUtf8Exact(L"caf\u00E9 \u0438", bytes) || bytes != "caf\xC3\xA9 \xD0\xB8")
            throw 0;
         if (!Unicode::ToUtf8Exact(L"", bytes) || !bytes.IsEmpty())
            throw 0;

         // An unpaired surrogate, alone and before a letter, is refused rather
         // than written as U+FFFD; a pair is a character (on Windows, where
         // wchar_t is two bytes; on Linux one unit is the character).
         String lone;
         lone += (wchar_t) 0xD800;
         if (Unicode::ToUtf8Exact(lone, bytes) || !bytes.IsEmpty())
            throw 0;
         String loneThenLetter = lone + _T("a");
         if (Unicode::ToUtf8Exact(loneThenLetter, bytes))
            throw 0;
         String lowAlone;
         lowAlone += (wchar_t) 0xDC00;
         if (!Unicode::HasUnpairedSurrogate(lowAlone))
            throw 0;
         if (sizeof(wchar_t) == 2)
         {
            String pair;
            pair += (wchar_t) 0xD83D;
            pair += (wchar_t) 0xDE00;
            if (Unicode::HasUnpairedSurrogate(pair) || !Unicode::ToUtf8Exact(pair, bytes) || bytes != "\xF0\x9F\x98\x80")
               throw 0;
         }

         // A String holding UTF-8 widened one byte to a character - as the SMTP
         // and IMAP command lines give it - stands for those bytes; Latin-1 text,
         // a byte sequence RFC 3629 refuses, and ASCII do not.
         String widenedIvan;
         const unsigned char ivanBytes[] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD0, 0xB0, 0xD0, 0xBD };
         for (unsigned char b : ivanBytes)
            widenedIvan += (wchar_t) b;
         AnsiString widenedBytes;
         String widenedText;
         if (!Unicode::IsWidenedUtf8(widenedIvan, &widenedBytes, &widenedText) ||
             widenedBytes != "\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD" || widenedText != L"\u0438\u0432\u0430\u043D")
            throw 0;
         AnsiString canonicalWidened, canonicalText;
         if (!Unicode::ToUtf8Canonical(widenedIvan, canonicalWidened) || !Unicode::ToUtf8Canonical(widenedText, canonicalText) ||
             canonicalWidened != canonicalText || Unicode::ToUtf8(widenedIvan) != canonicalText)
            throw 0;
         if (Unicode::IsWidenedUtf8(L"caf\u00E9") || Unicode::IsWidenedUtf8(_T("plain ascii")) ||
             Unicode::IsWidenedUtf8(L"\u0438") || Unicode::ToUtf8(L"caf\u00E9") != "caf\xC3\xA9")
            throw 0;
         const unsigned int refused[][4] = {
            { 0xC0, 0x80, 0, 0 },       // overlong
            { 0xED, 0xA0, 0x80, 0 },    // a surrogate
            { 0xF4, 0x90, 0x80, 0x80 }, // above U+10FFFF
            { 0xE2, 0x82, 0, 0 },       // cut short
            { 0x80, 0x41, 0, 0 }        // a trail byte first
         };
         for (const auto &sequence : refused)
         {
            String candidate = _T("x");
            for (unsigned int b : sequence)
            {
               if (b != 0)
                  candidate += (wchar_t) b;
            }
            if (Unicode::IsWidenedUtf8(candidate))
               throw 0;
         }

         const String asciiSamples[] = { _T("alice@example.test"), _T("Bob.Smith+tag@Example.TEST"), _T("https://push.example.test/v2/AbC-_~.%20x?y=1"), _T("") };
         for (const String &sample : asciiSamples)
         {
            if (!Unicode::ToUtf8Exact(sample, bytes) || bytes != AnsiString(sample))
               throw 0;
         }
      }

      OutputDebugString(_T("hMailServer: Testing BATV over UTF-8\n"));
      {
         const String secret = _T("batv-test-secret-key");

         // A tag made the old way, AnsiString(String) of the secret and of the
         // canonical string, over an ASCII mailbox in mixed case, still verifies.
         const int day = (int) (((__int64) time(0) / 86400) % 1000);
         String keyAndDay;
         keyAndDay.Format(_T("1%03d"), day);
         String lowerMailbox = _T("Alice.Old@Example.test");
         lowerMailbox.ToLower();
         AnsiString legacyHex = HashCreator::ComputeHMACSHA256Hex(AnsiString(secret), AnsiString(keyAndDay) + AnsiString(lowerMailbox));
         String legacyTag = _T("prvs=") + keyAndDay + String(legacyHex.Mid(0, 6)) + _T("=Alice.Old@Example.test");
         String original;
         if (!BATV::Verify(legacyTag, secret, original) || original != _T("Alice.Old@Example.test"))
            throw 0;

         // And what Sign makes today is that tag (unless the day turned over
         // between the two, which the second computation allows for).
         String signedNow = BATV::Sign(_T("Alice.Old@Example.test"), secret);
         const int dayAfter = (int) (((__int64) time(0) / 86400) % 1000);
         if (day == dayAfter && signedNow.CompareNoCase(legacyTag) != 0)
            throw 0;

         // A mailbox as the SMTP command line hands it to Sign and Verify - its
         // UTF-8 widened byte by byte - is hashed as those bytes, as the old
         // conversion hashed it: a tag made before this change for such a
         // mailbox still verifies, and the mailbox decoded gets the same tag.
         String widenedMailbox;
         const unsigned char mailboxBytes[] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD0, 0xB0, 0xD0, 0xBD };
         for (unsigned char b : mailboxBytes)
            widenedMailbox += (wchar_t) b;
         widenedMailbox += _T("@example.test");
         const int widenedDay = (int) (((__int64) time(0) / 86400) % 1000);
         String widenedKeyAndDay;
         widenedKeyAndDay.Format(_T("1%03d"), widenedDay);
         AnsiString oldWidenedHex = HashCreator::ComputeHMACSHA256Hex(AnsiString(secret), AnsiString(widenedKeyAndDay) + AnsiString(widenedMailbox));
         String oldWidenedTag = _T("prvs=") + widenedKeyAndDay + String(oldWidenedHex.Mid(0, 6)) + _T("=") +
                                widenedMailbox.Mid(0, widenedMailbox.Find(_T("@"))) + _T("@example.test");
         if (!BATV::Verify(oldWidenedTag, secret, original))
            throw 0;
         String signedWidened = BATV::Sign(widenedMailbox, secret);
         String signedDecoded = BATV::Sign(L"\u0438\u0432\u0430\u043D@example.test", secret);
         if (signedWidened.IsEmpty() || signedWidened.Mid(0, 15) != signedDecoded.Mid(0, 15))
            throw 0;

         // Two internationalized mailboxes of one length that differ only in a
         // letter above U+00FF: the old conversion gave both the same canonical
         // bytes, so the tag of one was the tag of the other.
         const String ivan = L"\u0438\u0432\u0430\u043D@example.test";
         const String ivam = L"\u0438\u0432\u0430\u043C@example.test";
         String taggedIvan = BATV::Sign(ivan, secret);
         if (taggedIvan.IsEmpty() || !BATV::Verify(taggedIvan, secret, original) || original != ivan)
            throw 0;
         String forged = taggedIvan;
         forged.Replace(L"\u0438\u0432\u0430\u043D@", L"\u0438\u0432\u0430\u043C@");
         if (forged == taggedIvan || BATV::Verify(forged, secret, original))
            throw 0;
         if (BATV::Sign(ivam, secret).Mid(0, 15) == taggedIvan.Mid(0, 15))
            throw 0;

         // The same after an ASCII prefix, which the old conversion kept and then
         // lost at the first Cyrillic letter.
         String taggedMixed = BATV::Sign(L"ops.\u043F\u0451\u0442\u0440@example.test", secret);
         String forgedMixed = taggedMixed;
         forgedMixed.Replace(L"\u043F\u0451\u0442\u0440@", L"\u043F\u0451\u0442\u0444@");
         if (taggedMixed.IsEmpty() || forgedMixed == taggedMixed || BATV::Verify(forgedMixed, secret, original))
            throw 0;

         // A secret that differs only in a Cyrillic letter is another secret.
         String taggedBySecret = BATV::Sign(_T("carol@example.test"), L"\u043A\u043B\u044E\u0447-1");
         if (BATV::Verify(taggedBySecret, L"\u043A\u043B\u044E\u0447-2", original) ||
             BATV::Verify(taggedBySecret, L"\u043A\u043B\u044E\u0448-1", original) ||
             !BATV::Verify(taggedBySecret, L"\u043A\u043B\u044E\u0447-1", original))
            throw 0;

         // Text UTF-8 cannot carry is neither signed nor verified.
         String loneMailbox;
         loneMailbox += (wchar_t) 0xD800;
         loneMailbox += _T("x@example.test");
         if (!BATV::Sign(loneMailbox, secret).IsEmpty())
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing SRS over UTF-8\n"));
      {
         const String secret = _T("srs-test-secret-key");

         // ASCII: the hash in what Forward makes today is the hash the old
         // conversion made over the same time slot, domain and local part.
         String srs = SRS::Forward(_T("Alice@External-Sender.com"), _T("relay.example.com"), secret);
         // SRS0=HHHHHHHH=TT=external-sender.com=Alice@relay.example.com
         std::vector<String> fields = StringParser::SplitString(srs.Mid(5, srs.ReverseFind(_T('@')) - 5), _T("="));
         if (fields.size() != 4)
            throw 0;
         String lowerDomain = fields[2];
         lowerDomain.ToLower();
         AnsiString legacyHex = HashCreator::ComputeHMACSHA256Hex(AnsiString(secret),
            AnsiString(fields[1]) + AnsiString(lowerDomain) + AnsiString(fields[3]));
         if (fields[0].CompareNoCase(String(legacyHex.Mid(0, fields[0].GetLength()))) != 0)
            throw 0;

         // Two internationalized senders that differ only in a letter above
         // U+00FF: the old conversion gave one hash for both, so the SRS address
         // of one reversed to the other.
         const String original = L"\u0438\u0432\u0430\u043D@\u043F\u0440\u0438\u043C\u0435\u0440.\u0440\u0444";
         String srsCyrillic = SRS::Forward(original, _T("relay.example.com"), secret);
         String reversed;
         if (srsCyrillic.IsEmpty() || !SRS::Reverse(srsCyrillic, secret, reversed) || reversed != original)
            throw 0;
         String forged = srsCyrillic;
         forged.Replace(L"=\u0438\u0432\u0430\u043D@", L"=\u0438\u0432\u0430\u043C@");
         if (forged == srsCyrillic || SRS::Reverse(forged, secret, reversed))
            throw 0;
         String forgedDomain = srsCyrillic;
         forgedDomain.Replace(L"=\u043F\u0440\u0438\u043C\u0435\u0440.", L"=\u043F\u0440\u0438\u043C\u0435\u0439.");
         if (forgedDomain == srsCyrillic || SRS::Reverse(forgedDomain, secret, reversed))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing password hashes over UTF-8\n"));
      {
         Crypt *crypt = Crypt::Instance();

         // ASCII: unchanged. MD5 of "password" is the value every table has, and
         // a salted SHA-256 and a PBKDF2 hash made the old way - over
         // AnsiString(String) - still open.
         if (crypt->EnCrypt(_T("password"), Crypt::ETMD5) != _T("5f4dcc3b5aa765d61d8327deb882cf99"))
            throw 0;
         const String ascii = _T("SeC-r3t Pass! ~#");
         HashCreator sha(HashCreator::SHA256);
         const String oldSha = String(sha.GenerateHash(AnsiString(ascii), ""));
         if (!crypt->Validate(ascii, oldSha, Crypt::ETSHA256) || crypt->Validate(_T("SeC-r3t Pass! ~$"), oldSha, Crypt::ETSHA256))
            throw 0;
         const String oldPbkdf2 = String(HashCreator::GeneratePBKDF2(AnsiString(ascii)));
         Crypt::PasswordMatch asciiMatch;
         if (!crypt->Validate(ascii, oldPbkdf2, Crypt::ETPBKDF2, &asciiMatch) || asciiMatch.text != ascii)
            throw 0;

         // Cyrillic: the hash is of the password's UTF-8, so another password of
         // the same length - which the old conversion made the same NUL bytes on
         // Windows and the same question marks on Linux - does not open it, and
         // neither do those NUL bytes or question marks.
         const String cyrillic = L"\u043F\u0430\u0440\u043E\u043B\u044C-\u0442\u0435\u0441\u0442";
         const String sameShape = L"\u043A\u043B\u044E\u0447\u0438\u043A-\u0432\u0445\u043E\u0434";
         const String newSha = crypt->EnCrypt(cyrillic, Crypt::ETSHA256);
         if (!crypt->Validate(cyrillic, newSha, Crypt::ETSHA256))
            throw 0;
         if (crypt->Validate(sameShape, newSha, Crypt::ETSHA256) ||
             crypt->Validate(_T("??????-????"), newSha, Crypt::ETSHA256) ||
             crypt->Validate(String(11, (wchar_t) 0), newSha, Crypt::ETSHA256))
            throw 0;
         if (crypt->EnCrypt(cyrillic, Crypt::ETMD5) != _T("9ab2c725cd6055f5020fe986d4f90c46") ||
             crypt->EnCrypt(cyrillic, Crypt::ETMD5) == crypt->EnCrypt(sameShape, Crypt::ETMD5))
            throw 0;

         // A hash made before 6.3.4 of a password that arrived as its UTF-8
         // widened (the old conversion hashed those bytes exactly) opens to the
         // password decoded as well - SASL PLAIN and COM give it that way.
         AnsiString cyrillicUtf8;
         if (!Unicode::ToUtf8Exact(cyrillic, cyrillicUtf8))
            throw 0;
         String cyrillicWidened;
         for (int i = 0; i < cyrillicUtf8.GetLength(); i++)
            cyrillicWidened += (wchar_t) (unsigned char) cyrillicUtf8[i];
         const String oldWidenedSha = String(sha.GenerateHash(AnsiString(cyrillicWidened), ""));
         if (!crypt->Validate(cyrillic, oldWidenedSha, Crypt::ETSHA256) || !crypt->Validate(cyrillicWidened, oldWidenedSha, Crypt::ETSHA256))
            throw 0;

         // The same password as a client's UTF-8 widened byte by byte (the REST
         // API's Basic credential, IMAP's command line) opens it too, and the text
         // to re-hash from is the password itself, so a re-hash keeps its bytes.
         AnsiString utf8;
         if (!Unicode::ToUtf8Exact(cyrillic, utf8))
            throw 0;
         String widened;
         for (int i = 0; i < utf8.GetLength(); i++)
            widened += (wchar_t) (unsigned char) utf8[i];
         const String newPbkdf2 = crypt->EnCrypt(cyrillic, Crypt::ETPBKDF2);
         Crypt::PasswordMatch widenedMatch;
         if (!crypt->Validate(widened, newPbkdf2, Crypt::ETPBKDF2, &widenedMatch) || widenedMatch.text != cyrillic)
            throw 0;

         // Latin-1 text hashed before 6.3.4 - its characters as bytes, which the
         // old conversion kept exactly - still opens, and is re-hashed from the
         // password as it is, which moves it to UTF-8.
         const String latin1 = L"passw\u00F6rd-\u00E9t\u00E9";
         AnsiString latin1Bytes;
         for (int i = 0; i < latin1.GetLength(); i++)
            latin1Bytes += (char) (unsigned char) latin1[i];
         const String oldLatin1 = String(sha.GenerateHash(latin1Bytes, ""));
         Crypt::PasswordMatch latin1Match;
         if (!crypt->Validate(latin1, oldLatin1, Crypt::ETSHA256, &latin1Match) || latin1Match.text != latin1 ||
             !latin1Match.legacy_bytes)
            throw 0;

         // The same Latin-1 password from a client that sends UTF-8 (IMAP LOGIN,
         // REST Basic: its UTF-8 widened one byte to a character) opens that hash
         // too, is said to need a re-hash, and names the text to make it from.
         AnsiString latin1Utf8;
         if (!Unicode::ToUtf8Exact(latin1, latin1Utf8))
            throw 0;
         String latin1Widened;
         for (int i = 0; i < latin1Utf8.GetLength(); i++)
            latin1Widened += (wchar_t) (unsigned char) latin1Utf8[i];
         Crypt::PasswordMatch widenedLatin1Match;
         if (!crypt->Validate(latin1Widened, oldLatin1, Crypt::ETSHA256, &widenedLatin1Match) ||
             widenedLatin1Match.text != latin1 || !widenedLatin1Match.legacy_bytes)
            throw 0;

         // A hash over UTF-8 is no legacy match, whichever form the password came in.
         const String utf8Latin1Hash = crypt->EnCrypt(latin1, Crypt::ETSHA256);
         Crypt::PasswordMatch currentMatch;
         if (!crypt->Validate(latin1Widened, utf8Latin1Hash, Crypt::ETSHA256, &currentMatch) || currentMatch.legacy_bytes ||
             currentMatch.text != latin1)
            throw 0;
         if (crypt->Validate(L"passw\u00F6rd-\u00E9t\u00E8", oldLatin1, Crypt::ETSHA256))
            throw 0;
         if (!crypt->Validate(latin1, crypt->EnCrypt(latin1, Crypt::ETSHA256), Crypt::ETSHA256))
            throw 0;

         // A hash the old conversion made of a password above U+00FF is refused
         // to that very password, and said to be one: every password of its form
         // matched it. A password holding U+0000 is refused whatever the hash.
         const String oldLossy = String(sha.GenerateHash(AnsiString(cyrillic), ""));
         Crypt::PasswordMatch lossyMatch;
         if (crypt->Validate(cyrillic, oldLossy, Crypt::ETSHA256, &lossyMatch) || !lossyMatch.refused_old_conversion)
            throw 0;
         Crypt::PasswordMatch otherMatch;
         if (crypt->Validate(sameShape, oldLossy, Crypt::ETSHA256, &otherMatch) || !otherMatch.refused_old_conversion)
            throw 0;
         // Said too when the password arrives as its UTF-8 widened (IMAP LOGIN).
         Crypt::PasswordMatch widenedLossyMatch;
         if (crypt->Validate(widened, oldLossy, Crypt::ETSHA256, &widenedLossyMatch) || !widenedLossyMatch.refused_old_conversion)
            throw 0;
         const String nulHash = String(sha.GenerateHash(AnsiString("ab\0\0", 4), ""));
         String nulPassword = _T("ab");
         nulPassword += (wchar_t) 0;
         nulPassword += (wchar_t) 0;
         if (crypt->Validate(nulPassword, nulHash, Crypt::ETSHA256))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the SCRAM anti-enumeration key over UTF-8\n"));
      {
         // ASCII: the old key, byte for byte.
         AnsiString legacy = "hMailServer-SCRAM-anti-enumeration-key";
         const String asciiParts[] = { _T("5f4dcc3b5aa765d61d8327deb882cf99"), _T("db pass!"), _T("hMailServer"), _T("db.example.test\\SQLEXPRESS") };
         for (const String &part : asciiParts)
         {
            legacy += "\x1f";
            legacy += AnsiString(part);
         }
         if (ScramSha256::AntiEnumerationKey(asciiParts[0], asciiParts[1], asciiParts[2], asciiParts[3]) != legacy)
            throw 0;

         // A database password of Cyrillic letters: the old key held its length
         // and nothing else, so two such passwords of one length gave one key.
         AnsiString first = ScramSha256::AntiEnumerationKey(_T("h"), L"\u043F\u0430\u0440\u043E\u043B\u044C", _T("n"), _T("s"));
         AnsiString second = ScramSha256::AntiEnumerationKey(_T("h"), L"\u043F\u0430\u0440\u043E\u043B\u0438", _T("n"), _T("s"));
         if (first == second)
            throw 0;
         if (first.Find("\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C") < 0)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the push endpoint hash over UTF-8\n"));
      {
         const String endpoint = _T("https://fcm.googleapis.com/fcm/send/dQw4w9WgXcQ:APA91bE-x_y");
         HashCreator hasher(HashCreator::SHA256);
         if (PushSubscriptions::HashOf(endpoint) != String(hasher.GenerateHashNoSalt(AnsiString(endpoint), HashCreator::hex)))
            throw 0;
         if (PushSubscriptions::HashOf(L"https://push.example.test/\u0430") == PushSubscriptions::HashOf(L"https://push.example.test/\u0431"))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the alert webhook signature\n"));
      {
         // The recipe AlertsAndAuditTrail.md gives - HMAC-SHA256 over the
         // timestamp, a full stop and the body - held to a value computed apart
         // from this code, with the secret as the old conversion gave it and as
         // UTF-8: for an ASCII secret and body the two are one signature.
         const String secret = _T("a-shared-secret-nobody-else-has");
         const AnsiString body = "{\"condition\":\"test.webhook\",\"summary\":\"ASCII only\"}";
         const AnsiString expected = "21dbdc037651f57dd9aa730e6034e21f647b627a8042986c1f43f5a3b0bb137d";
         AnsiString utf8Secret;
         if (!Unicode::ToUtf8Exact(secret, utf8Secret))
            throw 0;
         if (AlertManager::SignWebhook(utf8Secret, 1700000000, body) != expected)
            throw 0;
         if (AlertManager::SignWebhook(AnsiString(secret), 1700000000, body) != expected)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing RateLimiter\n"));
      {
         // Sliding-window rate limiter: limit is enforced per key, 0 = unlimited.
         RateLimiter::Instance()->Clear();

         // A maxPerMinute of 0 never throttles.
         for (int i = 0; i < 1000; i++)
         {
            if (!RateLimiter::Instance()->TryConsume(_T("unlimited"), 0))
               throw 0;
         }

         // With a limit of 3, the first three succeed and the fourth fails.
         if (!RateLimiter::Instance()->TryConsume(_T("keyA"), 3))
            throw 0;
         if (!RateLimiter::Instance()->TryConsume(_T("keyA"), 3))
            throw 0;
         if (!RateLimiter::Instance()->TryConsume(_T("keyA"), 3))
            throw 0;
         if (RateLimiter::Instance()->TryConsume(_T("keyA"), 3))
            throw 0;

         // A different key has an independent budget.
         if (!RateLimiter::Instance()->TryConsume(_T("keyB"), 3))
            throw 0;

         RateLimiter::Instance()->Clear();
      }

      OutputDebugString(_T("hMailServer: Testing DataProtector (DPAPI)\n"));
      {
         DataProtectorTester dpapiTester;
         dpapiTester.Test();

         // Also exercise the Crypt secret envelope used for stored secrets.
         const String secret = _T("route-relay-p@ssw0rd");
         String envelope = Crypt::Instance()->ProtectSecret(secret);
         if (envelope.IsEmpty())
            throw 0;
         if (Crypt::Instance()->UnprotectSecret(envelope) != secret)
            throw 0;
         // A legacy Blowfish value (no DPAPI prefix) must still round-trip.
         String legacy = Crypt::Instance()->EnCrypt(secret, Crypt::ETBlowFish);
         if (Crypt::Instance()->UnprotectSecret(legacy) != secret)
            throw 0;

         // The seal an account's second factor and the [Settings] secrets are kept
         // under in the database (docs/SecretsAtRest.md), under a key made for the
         // test: round-tripped, and refused for another purpose, another key and a
         // changed byte.
         StoredSecrets::Test();
         if (Crypt::IsSecretEnvelope(legacy))
            throw 0;
      }


      OutputDebugString(_T("hMailServer: Testing TransparentTransmissionBuffer (SMTP dot transparency)\n"));
      {
         TransparentTransmissionBufferTester dotTester;
         dotTester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing Cache size accounting\n"));
      {
         CacheAccountingTester cacheTester;
         cacheTester.Test();
      }

      OutputDebugString(_T("hMailServer: Testing RegularExpressionTester\n"));
      RegularExpressionTester *pRegExTest = new RegularExpressionTester();
      pRegExTest->Test();
      delete pRegExTest;

      OutputDebugString(_T("hMailServer: Testing Base64\n"));
      Base64Tester base64Tester;
      base64Tester.Test();

      // What the HTTP listeners compress: the Accept-Encoding negotiation, the
      // media-type policy, and the proof that what goes out under a
      // Content-Encoding decodes to exactly the bytes that would have gone out
      // without one. The regression fixture proves the same thing over a
      // socket; this proves it at the byte, where a wrapper put on the wrong
      // stream would show.
      OutputDebugString(_T("hMailServer: Testing HttpCompression\n"));
      HttpCompressionTester httpCompressionTester;
      httpCompressionTester.Test();

      // The HTTP listeners' deadlines, over the loopback against a server of
      // the tester's own: an answer a handler returns after the request
      // deadline, or after the connection's, arrives whole; a reader that
      // stops part way through an answer is reset, not given an orderly end
      // it could take for the whole; and the connection still answers the
      // request after a late one. Some ten seconds.
      OutputDebugString(_T("hMailServer: Testing HttpServer\n"));
      HttpServerTester httpServerTester;
      httpServerTester.Test();

      // The REST API's OpenAPI document held to the router that answers it:
      // one JSON value with no member named twice (a path written as two
      // entries loses one of them in every reader), every path item as
      // OpenAPI has it, every documented operation one the router knows, and
      // every route the router names one the document describes.
      OutputDebugString(_T("hMailServer: Testing RestApiOpenApi\n"));
      RestApiOpenApiTester restApiOpenApiTester;
      restApiOpenApiTester.Test();

      // What the full-text index's lookups prove: a message excluded only when
      // it is complete, not overflowed and some needle is in none of its terms
      // as the scan would find it - a collation's looser match, or a marker
      // spelt otherwise, never excludes one.
      OutputDebugString(_T("hMailServer: Testing PersistentMessageIndex\n"));
      PersistentMessageIndexTester persistentMessageIndexTester;
      persistentMessageIndexTester.Test();

      // The Control Deck's script-src: the SHA-256 of the text a browser
      // hashes for the page's one inline script - CR LF made LF, the script
      // found as the tokenizer finds it - against values another
      // implementation computed, and 'none' for a page with no single inline
      // script.
      OutputDebugString(_T("hMailServer: Testing InlineScriptHash\n"));
      InlineScriptHashTester inlineScriptHashTester;
      inlineScriptHashTester.Test();

      // The account signature's HTML reduced to the subset the webmail's cleaner
      // admits: the elements and attributes kept and dropped, a dangerous
      // element dropped whole, the http(s)/mailto and data: image rules, the
      // fixed point on the page's own output, and idempotence.
      OutputDebugString(_T("hMailServer: Testing SignatureHtmlSanitizer\n"));
      SignatureHtmlSanitizerTester signatureHtmlSanitizerTester;
      signatureHtmlSanitizerTester.Test();

      // IMAP COMPRESS=DEFLATE's inflation, in pieces no larger than the limit the
      // connection asks for: a round trip, a stream of command lines hundreds of
      // times its compressed size put back whole, what zlib holds after the input
      // is all taken, a limit of nothing, and a corrupt stream refused.
      OutputDebugString(_T("hMailServer: Testing DeflateStreams\n"));
      DeflateStreamsTester deflateStreamsTester;
      deflateStreamsTester.Test();

      // DANE as an outbound connection does it: the usable TLSA records, a
      // validated RRset of none requiring TLS without authentication, and
      // handshakes in memory for DANE-TA and DANE-EE.
      OutputDebugString(_T("hMailServer: Testing DANE\n"));
      DaneTester daneTester;
      daneTester.Test();

      OutputDebugString(_T("hMailServer: Testing the ACME TLSA record\n"));
      {
         // Issue #93. GetCertificateTlsa read the certificate through a FILE* of
         // this executable's C runtime, and on Windows the OpenSSL DLL reads such
         // a FILE* through its "uplink" table, which needs an OPENSSL_Applink
         // export this executable does not have. The DLL's answer to the missing
         // export is TerminateProcess, so the service died the moment a
         // certificate had been issued. This runs the same function on a
         // certificate written to the temp directory: with the FILE* code it
         // kills the process, which no test framework can miss. The expected
         // digest is the SHA-256 of the certificate's DER SubjectPublicKeyInfo,
         // computed independently with the openssl command line:
         //   openssl x509 -pubkey -noout | openssl pkey -pubin -outform DER | openssl dgst -sha256
         const char *fixture =
            "-----BEGIN CERTIFICATE-----\n"
            "MIIBnTCCAUOgAwIBAgIUTt9dEsQtTAfFIIM+5viNWZ1No2wwCgYIKoZIzj0EAwIw\n"
            "JDEiMCAGA1UEAwwZdGxzYS1maXh0dXJlLmV4YW1wbGUudGVzdDAeFw0yNjA5MDYw\n"
            "MTE5NDlaFw00NjA5MDEwMTE5NDlaMCQxIjAgBgNVBAMMGXRsc2EtZml4dHVyZS5l\n"
            "eGFtcGxlLnRlc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAARcQAyXS3jMUHe7\n"
            "R1o4DUn+iwKaVorSVrb/ajttx1R+PcKDb2ftkaDapR7MHbBRLTNrY02KJjzoreKa\n"
            "mGx4JxOJo1MwUTAdBgNVHQ4EFgQU+KRDFDaeMDV/BbibOpNAb0tBARswHwYDVR0j\n"
            "BBgwFoAU+KRDFDaeMDV/BbibOpNAb0tBARswDwYDVR0TAQH/BAUwAwEB/zAKBggq\n"
            "hkjOPQQDAgNIADBFAiEAoRJcE3fgMRiHhf3LglFlRkznZENHdc9SwMOGjNpsUokC\n"
            "IHt+CXzwoEOtys7tKUt6k5y/Qftj4xirfGPZCy00rzX/\n"
            "-----END CERTIFICATE-----\n";

         String path = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("tlsa-fixture.pem");
         if (!FileUtilities::WriteToFile(path, AnsiString(fixture)))
            throw 0;

         AnsiString hex;
         bool computed = AcmeClient::GetCertificateTlsa(path, hex);
         FileUtilities::DeleteFile(path);

         if (!computed)
            throw 0;
         if (hex != "fece5d0297fa1a2bd1a4e510275367f904fc631c70a3e2a5515e554e1ab67a38")
            throw 0;

         // A file that is not a certificate is a false, not a crash, and leaves
         // the answer empty.
         if (!FileUtilities::WriteToFile(path, AnsiString("not a certificate\n")))
            throw 0;

         bool rejected = !AcmeClient::GetCertificateTlsa(path, hex);
         FileUtilities::DeleteFile(path);

         if (!rejected || !hex.IsEmpty())
            throw 0;

         // And so is a file that does not exist.
         if (AcmeClient::GetCertificateTlsa(path, hex))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the ACME challenge locator\n"));
      {
         // Boulder pretty-prints its JSON - '"type": "http-01"', space included -
         // so the compact-form search this locator replaced never matched a real
         // Let's Encrypt response and every issuance failed (issue #34). The
         // pretty shape below is the real CA's, byte for byte in the ways that
         // matter.
         AnsiString pretty =
            "{\n"
            "  \"identifier\": {\n"
            "    \"type\": \"dns\",\n"
            "    \"value\": \"mta-sts.example.com\"\n"
            "  },\n"
            "  \"status\": \"pending\",\n"
            "  \"challenges\": [\n"
            "    {\n"
            "      \"type\": \"tls-alpn-01\",\n"
            "      \"url\": \"https://ca.example/chall/1\",\n"
            "      \"token\": \"alpntoken\"\n"
            "    },\n"
            "    {\n"
            "      \"type\": \"http-01\",\n"
            "      \"url\": \"https://ca.example/chall/2\",\n"
            "      \"token\": \"httptoken\"\n"
            "    }\n"
            "  ]\n"
            "}";
         if (AcmeClient::FindChallengeOfType(pretty, "http-01") < 0)
            throw 0;

         // The compact form stays accepted - simulators and other CAs emit it.
         AnsiString compact = "{\"challenges\":[{\"type\":\"http-01\",\"url\":\"u\",\"token\":\"t\"}]}";
         if (AcmeClient::FindChallengeOfType(compact, "http-01") < 0)
            throw 0;

         // A wildcard authorization offers dns-01 only: not-found is the answer.
         AnsiString dnsOnly = "{\"challenges\": [{\"type\": \"dns-01\", \"url\": \"u\", \"token\": \"t\"}]}";
         if (AcmeClient::FindChallengeOfType(dnsOnly, "http-01") >= 0)
            throw 0;

         // The type string appearing as some OTHER key's value must not count.
         AnsiString decoy = "{\"failedChallenge\": \"http-01\", \"challenges\": []}";
         if (AcmeClient::FindChallengeOfType(decoy, "http-01") >= 0)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the ACME header lookup\n"));
      {
         // The header block is cut at the blank line, so the header that comes last
         // has no line break after it, and until 6.3.6 the lookup took it for no
         // header at all: a CA that sent Replay-Nonce last left every request
         // without a nonce. Field order carries no meaning (RFC 9110 section 5.3).
         // network-deadline-tests holds the same cases on Linux.
         const std::string headers = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nLocation: https://ca.example/acct/1\r\nReplay-Nonce: last-nonce ";
         if (AcmeClient::GetHeaderValue(headers, "replay-nonce") != "last-nonce")
            throw 0;
         if (AcmeClient::GetHeaderValue(headers, "location") != "https://ca.example/acct/1")
            throw 0;
         if (AcmeClient::GetHeaderValue(headers, "content-type") != "application/json")
            throw 0;
         if (!AcmeClient::GetHeaderValue(headers, "link").IsEmpty())
            throw 0;

         // A name that only ends another header's is not that header.
         if (!AcmeClient::GetHeaderValue("HTTP/1.1 200 OK\r\nContent-Location: https://ca.example/x", "location").IsEmpty())
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the ACME renewal window against short certificate lifetimes\n"));
      {
         // The renewal decision is a fraction of the lifetime, not a fixed number of
         // days, and these vectors are the reason. Certificate lifetimes are falling
         // on a published schedule - Let us Encrypt to 64 days in February 2027, the
         // maximum to 100 days in March 2027 and to 47 in March 2029 - and the fixed
         // 30-day window this replaced is longer than half of the last of those.
         const time_t day = 86400;
         const time_t issued = 1800000000;

         // 64 days and 47 days - Let us Encrypt's 2027 default and the 2029 maximum.
         // Neither divides by three, so these assert the PROPERTY rather than
         // restating the arithmetic: about a third of the lifetime is left when
         // renewal begins. Restating it would pass against any implementation,
         // including a wrong one copied from the same expression.
         const time_t lifetimes[] = { 64 * day, 47 * day, 100 * day, 10 * day };

         for (time_t lifetime : lifetimes)
         {
            time_t renewAt = AcmeClient::GetRenewalTime(issued, issued + lifetime);
            time_t remaining = (issued + lifetime) - renewAt;

            // A third, give or take the integer division.
            if (remaining < lifetime / 3 || remaining > lifetime / 3 + day)
               throw 0;

            // And never so late that a failure has no room for another attempt.
            if (remaining < day)
               throw 0;
         }

         // The one exact vector, because it is the claim that nothing moved for
         // anybody today: a 90-day certificate still renews with 30 days left.
         if (AcmeClient::GetRenewalTime(issued, issued + 90 * day) != issued + 60 * day)
            throw 0;


         // The floor. A two-day certificate would otherwise be renewed with 16 hours
         // left, and the renewal task runs hourly - one failed attempt and there is
         // no certificate. A full day leaves room for two dozen attempts.
         if (AcmeClient::GetRenewalTime(issued, issued + 2 * day) != issued + 1 * day)
            throw 0;

         // Shorter than the floor: renew immediately, which is the only honest
         // answer for a certificate that cannot be given a day of margin.
         if (AcmeClient::GetRenewalTime(issued, issued + 12 * 3600) != issued)
            throw 0;

         // An unreadable notBefore falls back to the 90-day assumption rather than
         // refusing to renew - a certificate with a bad start date still has a
         // perfectly good expiry to work back from.
         if (AcmeClient::GetRenewalTime(0, issued + 90 * day) != issued + 60 * day)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the ARI certificate identifier against RFC 9773's worked example\n"));
      {
         // RFC 9773 section 4.1 publishes a worked example, which is the only way to
         // check this without a certificate from a real CA - and the only way to
         // catch the mistake it is easy to make here. The serial's DER content
         // octets carry a leading zero when the top bit is set, so 0x8765432101 is
         // 00 87 65 43 21; the magnitude bytes OpenSSL stores do not have it. An
         // identifier built from those is one no CA has ever issued, and the only
         // symptom is a 404 and a silent fall back to deciding for ourselves.
         const unsigned char keyIdentifier[] = {
            0x69, 0x88, 0x5B, 0x6B, 0x87, 0x46, 0x40, 0x41, 0xE1, 0xB3,
            0x7B, 0x84, 0x7B, 0xA0, 0xAE, 0x2C, 0xDE, 0x01, 0xC8, 0xD4
         };

         const unsigned char serialDer[] = { 0x00, 0x87, 0x65, 0x43, 0x21 };

         AnsiString ariId = AcmeClient::BuildAriId(keyIdentifier, sizeof(keyIdentifier),
                                                   serialDer, sizeof(serialDer));

         if (ariId != "aYhba4dGQEHhs3uEe6CuLN4ByNQ.AIdlQyE")
            throw 0;

         // Missing either half is no identifier at all rather than half of one.
         if (!AcmeClient::BuildAriId(nullptr, 0, serialDer, sizeof(serialDer)).IsEmpty())
            throw 0;
         if (!AcmeClient::BuildAriId(keyIdentifier, sizeof(keyIdentifier), nullptr, 0).IsEmpty())
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing ARI (RFC 9773) timestamp parsing and window selection\n"));
      {
         const time_t day = 86400;

         // RFC 3339, the shape an ARI suggestedWindow carries.
         if (AcmeClient::ParseRfc3339("1970-01-01T00:00:00Z") != 0)
            throw 0;
         if (AcmeClient::ParseRfc3339("2026-01-02T03:04:05Z") != 1767323045)
            throw 0;

         // Fractional seconds are legal in RFC 3339 and real CAs emit them.
         if (AcmeClient::ParseRfc3339("2026-01-02T03:04:05.123Z") != 1767323045)
            throw 0;

         // Anything that cannot be read is 0, which the caller reads as "no opinion
         // from the CA" and falls back to its own calculation. Rejecting is safe;
         // half-understanding a timestamp that decides a renewal is not.
         if (AcmeClient::ParseRfc3339("") != 0)
            throw 0;
         if (AcmeClient::ParseRfc3339("not a timestamp at all") != 0)
            throw 0;
         if (AcmeClient::ParseRfc3339("2026-13-45T99:99:99Z") != 0)
            throw 0;

         // The window point has to land INSIDE the window - that is the whole
         // contract - and has to be the same answer every time for one certificate,
         // because the renewal decision is re-taken hourly and a fresh roll each
         // time would drift renewals towards the start of the window.
         const time_t windowStart = 1800000000;
         const time_t windowEnd = windowStart + 7 * day;

         time_t first = AcmeClient::GetRenewalTimeInWindow("aki.serial", windowStart, windowEnd);

         if (first < windowStart || first >= windowEnd)
            throw 0;
         if (AcmeClient::GetRenewalTimeInWindow("aki.serial", windowStart, windowEnd) != first)
            throw 0;

         // ...and a DIFFERENT certificate has to land somewhere else, or the whole
         // point of the window - spreading a fleet's renewals - is lost and every
         // server renews at the same instant.
         if (AcmeClient::GetRenewalTimeInWindow("other.serial", windowStart, windowEnd) == first)
            throw 0;

         // A degenerate window is the start of it rather than an error.
         if (AcmeClient::GetRenewalTimeInWindow("aki.serial", windowStart, windowStart) != windowStart)
            throw 0;
         if (AcmeClient::GetRenewalTimeInWindow("aki.serial", windowStart, windowStart - day) != windowStart)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the TLS-RPT record parser and report builder\n"));
      {
         // The shape RFC 8460 section 3 gives, with two mailto targets and the
         // URI parameter a real-world record carries.
         std::vector<String> addresses;
         if (!TlsRptReporterTask::ParseTlsRptRecord(
                "v=TLSRPTv1; rua=mailto:reports@example.com,mailto:backup@example.net?subject=tls", addresses))
            throw 0;
         if (addresses.size() != 2)
            throw 0;
         if (addresses[0] != _T("reports@example.com"))
            throw 0;
         // The ?subject parameter is part of the URI, not the mailbox.
         if (addresses[1] != _T("backup@example.net"))
            throw 0;

         // A policy whose rua names only https endpoints is a valid TLSRPTv1
         // record this implementation cannot deliver to: recognized, zero
         // addresses. The distinction matters - "not a policy" would make the
         // caller keep scanning TXT records that will never match.
         addresses.clear();
         if (!TlsRptReporterTask::ParseTlsRptRecord(
                "v=TLSRPTv1;rua=https://reporting.example.com/v1/tlsrpt", addresses))
            throw 0;
         if (!addresses.empty())
            throw 0;

         // An unrelated TXT record at the same name (an SPF policy, say) is not
         // a TLSRPT record at all.
         addresses.clear();
         if (TlsRptReporterTask::ParseTlsRptRecord("v=spf1 mx -all", addresses))
            throw 0;
         if (!addresses.empty())
            throw 0;

         // The report body: RFC 8460 section 4's required members, the counts,
         // the failure detail, and JSON escaping of a quote in a value that
         // reaches the report verbatim (the organization name is the
         // administrator's own text).
         TlsRptStore::DomainBucket bucket;
         bucket.policy_type = "no-policy-found";
         bucket.successful_sessions = 3;

         TlsRptStore::FailureDetail failure;
         failure.result_type = "validation-failure";
         failure.receiving_mx = "mx1.example.org";
         failure.count = 2;
         bucket.failures.push_back(failure);

         AnsiString json = TlsRptReporterTask::BuildReportJson(
            "2026-08-16", _T("example.org"), bucket, "report-id-1", _T("postmaster@sender.test"), "Acme \"Mail\" Ltd");

         if (json.Find("\"start-datetime\":\"2026-08-16T00:00:00Z\"") < 0)
            throw 0;
         if (json.Find("\"end-datetime\":\"2026-08-16T23:59:59Z\"") < 0)
            throw 0;
         if (json.Find("\"policy-domain\":\"example.org\"") < 0)
            throw 0;
         if (json.Find("\"policy-type\":\"no-policy-found\"") < 0)
            throw 0;
         if (json.Find("\"total-successful-session-count\":3") < 0)
            throw 0;
         if (json.Find("\"total-failure-session-count\":2") < 0)
            throw 0;
         if (json.Find("\"result-type\":\"validation-failure\"") < 0)
            throw 0;
         if (json.Find("\"receiving-mx-hostname\":\"mx1.example.org\"") < 0)
            throw 0;
         if (json.Find("\"failed-session-count\":2") < 0)
            throw 0;
         if (json.Find("\"organization-name\":\"Acme \\\"Mail\\\" Ltd\"") < 0)
            throw 0;
         if (json.Find("\"contact-info\":\"postmaster@sender.test\"") < 0)
            throw 0;
         if (json.Find("\"report-id\":\"report-id-1\"") < 0)
            throw 0;

         // An empty policy_string must not emit an empty policy-string array -
         // the member is simply absent.
         if (json.Find("policy-string") >= 0)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing the DMARC rua parser and the report builder in both schemas\n"));
      {
         // The rua tag with two mailto targets, one carrying the RFC 7489 6.2
         // size suffix and one a URI parameter: neither is part of the mailbox.
         std::vector<String> addresses;
         if (!DmarcRptReporterTask::ParseRuaTargets(
                "v=DMARC1; p=reject; rua=mailto:agg@example.com!10m,mailto:second@example.net?x=1", addresses))
            throw 0;
         if (addresses.size() != 2)
            throw 0;
         if (addresses[0] != _T("agg@example.com"))
            throw 0;
         if (addresses[1] != _T("second@example.net"))
            throw 0;

         // https-only rua: a recognized policy with zero deliverable targets.
         addresses.clear();
         if (!DmarcRptReporterTask::ParseRuaTargets(
                "v=DMARC1;p=none;rua=https://collector.example/dmarc", addresses))
            throw 0;
         if (!addresses.empty())
            throw 0;

         // An SPF record at the same name is not a DMARC policy at all.
         addresses.clear();
         if (DmarcRptReporterTask::ParseRuaTargets("v=spf1 mx -all", addresses))
            throw 0;

         // RFC 7489 7.1 draws the external-destination line at the
         // organizational domain: a report mailbox on another host of the same
         // registrant needs no consent record; anywhere else does.
         //
         // IsExternalDestination is GetOrganizationalDomain on both sides, and for
         // any name of two labels or more that function asks DNS (the RFC 9989 tree
         // walk). The check that used to sit here compared mail.example.com with
         // example.com, and its answer was whatever the resolver made of it: under
         // a live resolver the walk hit an error at _dmarc.com and fell back to the
         // Public Suffix List, which put both under example.com - "not external",
         // as asserted; under the regression suite's own zone, where nothing is
         // published and every lookup succeeds with no records, the walk completes
         // and each name is its own organizational domain - "external". Both are
         // this code doing what it should with the answer it got, so the assertion
         // was passing by accident. The organizational-domain rules are pinned by
         // DmarcTreeWalkTester with records in memory; this pins only the
         // comparison, with names the walk never consults DNS for (a single label
         // consults nothing).
         if (DmarcRptReporterTask::IsExternalDestination(_T("example"), _T("example")))
            throw 0;
         if (!DmarcRptReporterTask::IsExternalDestination(_T("example"), _T("collector")))
            throw 0;

         // The report body: RFC 7489 Appendix C's members, and XML escaping of
         // a value the administrator controls.
         //
         // ONE bucket drives both schemas below. That is the point of it: with
         // the same input on both sides, every difference between the two
         // documents is attributable to the schema selection and nothing else.
         DmarcRptStore::DomainBucket bucket;
         bucket.policy_domain = "example.org";
         bucket.adkim = "s";
         bucket.p = "quarantine";
         bucket.pct = 50;

         // Recorded whatever the configured schema is, and invisible in the 7489
         // form, which has nowhere to put any of them.
         bucket.np = "reject";
         bucket.testing = "y";
         bucket.discovery_method = "treewalk";

         DmarcRptStore::Row row;
         row.source_ip = "192.0.2.7";
         row.disposition = "quarantine";
         row.dkim = "fail";
         row.spf = "fail";
         row.header_from = "example.org";
         row.envelope_from_domain = "bounce.example.org";
         row.spf_passed = true;
         row.dkim_passing_domains.push_back("other.example");
         row.dkim_passing_selectors.push_back("sel2026");

         // A second passing signature whose s= could not be read. The selector is
         // optional in the schema, so this one must produce no <selector> element at
         // all rather than an empty one - and it has to stay index-aligned with its
         // domain, which is why an unreadable selector is pushed as empty rather
         // than skipped.
         row.dkim_passing_domains.push_back("nosel.example");
         row.dkim_passing_selectors.push_back("");

         row.count = 3;
         bucket.rows.push_back(row);

         // A message that PASSED DMARC, and one that failed under a policy asking
         // for no action. Both are disposition "none" as this server records them,
         // and telling them apart is the whole of what RFC 9990's extra "pass"
         // member buys - so the pair has to be present in both documents for the
         // difference to be visible at all.
         DmarcRptStore::Row passingRow;
         passingRow.source_ip = "192.0.2.8";
         passingRow.disposition = "none";
         passingRow.dkim = "pass";
         passingRow.spf = "fail";
         passingRow.header_from = "example.org";
         passingRow.envelope_from_domain = "bounce.example.org";
         passingRow.spf_passed = false;
         passingRow.dkim_passing_domains.push_back("example.org");
         passingRow.dkim_passing_selectors.push_back("keyA");
         passingRow.count = 5;
         bucket.rows.push_back(passingRow);

         DmarcRptStore::Row failingRow;
         failingRow.source_ip = "192.0.2.9";
         failingRow.disposition = "none";
         failingRow.dkim = "fail";
         failingRow.spf = "fail";
         failingRow.header_from = "example.org";
         failingRow.envelope_from_domain = "bounce.example.org";
         failingRow.spf_passed = false;
         failingRow.count = 7;
         bucket.rows.push_back(failingRow);

         AnsiString xml = DmarcRptReporterTask::BuildReportXml(
            "2026-08-16", bucket, "rpt-1", _T("postmaster@sender.test"), "A & B <Ltd>",
            DmarcRptReporterTask::SchemaRfc7489, "ignored in this form");

         // The day's epoch range, computed the same way the builder computes
         // it rather than hand-derived, so the assertion cannot drift from a
         // timezone or leap assumption.
         tm dayStart = {};
         dayStart.tm_year = 2026 - 1900;
         dayStart.tm_mon = 7;
         dayStart.tm_mday = 16;
         __int64 expectedBegin = _mkgmtime64(&dayStart);

         AnsiString expectedRange;
         expectedRange.Format("<date_range><begin>%I64d</begin><end>%I64d</end></date_range>",
            expectedBegin, expectedBegin + 86399);
         if (xml.Find(expectedRange) < 0)
            throw 0;

         if (xml.Find("<org_name>A &amp; B &lt;Ltd&gt;</org_name>") < 0)
            throw 0;
         if (xml.Find("<email>postmaster@sender.test</email>") < 0)
            throw 0;
         if (xml.Find("<report_id>rpt-1</report_id>") < 0)
            throw 0;
         if (xml.Find("<domain>example.org</domain>") < 0)
            throw 0;
         if (xml.Find("<adkim>s</adkim>") < 0)
            throw 0;
         // aspf was not published; the default is reported explicitly.
         if (xml.Find("<aspf>r</aspf>") < 0)
            throw 0;
         if (xml.Find("<p>quarantine</p>") < 0)
            throw 0;
         // No sp tag published: the member is absent, not empty.
         if (xml.Find("<sp>") >= 0)
            throw 0;
         if (xml.Find("<pct>50</pct>") < 0)
            throw 0;
         if (xml.Find("<source_ip>192.0.2.7</source_ip>") < 0)
            throw 0;
         if (xml.Find("<count>3</count>") < 0)
            throw 0;
         if (xml.Find("<disposition>quarantine</disposition>") < 0)
            throw 0;
         if (xml.Find("<header_from>example.org</header_from>") < 0)
            throw 0;
         // The selector names WHICH key signed, which is what a domain owner needs
         // in order to act on the report during a rotation or after a compromise.
         if (xml.Find("<dkim><domain>other.example</domain><selector>sel2026</selector><result>pass</result></dkim>") < 0)
            throw 0;

         // ...and a signature whose selector could not be read carries none, rather
         // than an empty element that a parser would read as a key called "".
         if (xml.Find("<dkim><domain>nosel.example</domain><result>pass</result></dkim>") < 0)
            throw 0;
         if (xml.Find("<selector></selector>") >= 0)
            throw 0;

         // The raw SPF result, distinct from the aligned fail above it, and the
         // scope that says which identity it was evaluated on - without it a reader
         // cannot tell an envelope-sender check from a HELO one.
         if (xml.Find("<spf><domain>bounce.example.org</domain><scope>mfrom</scope><result>pass</result></spf>") < 0)
            throw 0;

         // The DEFAULT form is this one, and the negative half is the half that
         // matters: none of the DMARCbis-only members may appear in it. A schema
         // that grew extra elements would be rejected whole by a receiver
         // validating against RFC 7489's XSD, which is to say the reports would
         // stop arriving while the log went on saying they were sent.
         if (xml.Find("urn:ietf:params:xml:ns:dmarc-2.0") >= 0)
            throw 0;
         if (xml.Find("<version>") >= 0)
            throw 0;
         if (xml.Find("<np>") >= 0)
            throw 0;
         if (xml.Find("<testing>") >= 0)
            throw 0;
         if (xml.Find("<discovery_method>") >= 0)
            throw 0;
         if (xml.Find("<generator>") >= 0)
            throw 0;
         if (xml.Find("<envelope_from>") >= 0)
            throw 0;

         // 7489 has no "pass" disposition, so a message that passed DMARC and one
         // that failed under p=none are both reported as "none" here. That is not
         // a defect of this code - it is the limitation RFC 9990 exists to lift,
         // and it is pinned so that the difference below is a real one.
         if (xml.Find("<disposition>pass</disposition>") >= 0)
            throw 0;

         AnsiString rfc9990Xml = DmarcRptReporterTask::BuildReportXml(
            "2026-08-16", bucket, "rpt-1", _T("postmaster@sender.test"), "A & B <Ltd>",
            DmarcRptReporterTask::SchemaRfc9990, "hMailServer test-generator");

         // The namespace is what identifies the document as the 2.0 schema, and it
         // is declared as the default xmlns on the root so that every element
         // inherits it - Appendix A sets elementFormDefault="qualified".
         if (rfc9990Xml.Find("<feedback xmlns=\"urn:ietf:params:xml:ns:dmarc-2.0\">") < 0)
            throw 0;

         // 1.0, NOT 2.0. RFC 9990 3.1.1.2: the version element "MUST have the
         // value 1.0", and the Appendix B sample agrees. The 2.0 is the
         // namespace's, and writing it here is the single easiest way to produce
         // a document that fails validation against the schema it claims.
         if (rfc9990Xml.Find("<version>1.0</version>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<version>2.0</version>") >= 0)
            throw 0;

         // pct is GONE from the 2.0 schema. Present in the 7489 form above, from
         // the same bucket, so this is the setting choosing and not the data.
         if (xml.Find("<pct>50</pct>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<pct>") >= 0)
            throw 0;

         // The three additions to policy_published.
         if (rfc9990Xml.Find("<np>reject</np>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<testing>y</testing>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<discovery_method>treewalk</discovery_method>") < 0)
            throw 0;

         // report_metadata/generator: who to complain to about a malformed report.
         if (rfc9990Xml.Find("<generator>hMailServer test-generator</generator>") < 0)
            throw 0;

         // ActionDispositionType's extra member. The passing row becomes "pass",
         // the p=none failure stays "none", and the quarantine is untouched -
         // this server never took no action on the last of those.
         if (rfc9990Xml.Find("<disposition>pass</disposition>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<disposition>none</disposition>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<disposition>quarantine</disposition>") < 0)
            throw 0;

         // selector is minOccurs="1" in 2.0 - required. A signature whose s= could
         // not be read therefore carries an EMPTY one, which is the opposite of
         // what the 7489 form does with the same row, because there the element is
         // optional and an empty one would read as a key named "".
         if (rfc9990Xml.Find("<dkim><domain>nosel.example</domain><selector></selector><result>pass</result></dkim>") < 0)
            throw 0;
         if (rfc9990Xml.Find("<dkim><domain>other.example</domain><selector>sel2026</selector><result>pass</result></dkim>") < 0)
            throw 0;

         // identifiers/envelope_from, which the 7489 form has never carried: it is
         // what tells a reader which envelope the auth_results/spf domain came
         // from rather than leaving them to assume.
         if (rfc9990Xml.Find("<envelope_from>bounce.example.org</envelope_from>") < 0)
            throw 0;

         // mfrom is the ONLY scope 2.0 admits (helo is gone), and it is what this
         // server has always emitted - DMARC evaluates no other identity.
         if (rfc9990Xml.Find("<scope>mfrom</scope>") < 0)
            throw 0;

         // An out-of-range schema version produces the 7489 form rather than an
         // empty document or a half-written one. The ini read rejects such values
         // before they get here; this is the same decision made again at the point
         // of use, because the safe fallback is the form every receiver parses.
         AnsiString outOfRangeXml = DmarcRptReporterTask::BuildReportXml(
            "2026-08-16", bucket, "rpt-1", _T("postmaster@sender.test"), "A & B <Ltd>",
            7, "hMailServer test-generator");

         if (!(outOfRangeXml == xml))
            throw 0;

         // A discovery_method the store never recorded - an older bucket, or one
         // built by hand - omits the element rather than guessing a mechanism.
         // Every other 2.0 member is unaffected, so the omission is local.
         DmarcRptStore::DomainBucket unknownDiscovery = bucket;
         unknownDiscovery.discovery_method = "";

         AnsiString unknownDiscoveryXml = DmarcRptReporterTask::BuildReportXml(
            "2026-08-16", unknownDiscovery, "rpt-1", _T("postmaster@sender.test"), "A & B <Ltd>",
            DmarcRptReporterTask::SchemaRfc9990, "hMailServer test-generator");

         if (unknownDiscoveryXml.Find("<discovery_method>") >= 0)
            throw 0;
         if (unknownDiscoveryXml.Find("<testing>y</testing>") < 0)
            throw 0;

         // A record with no t= tag reports the RFC's default rather than nothing:
         // 3.1.1.5 says unspecified tags have their default values, and n is the
         // default. Reporting "testing" for a domain that is enforcing would tell
         // its owner the failures they are looking at are harmless.
         DmarcRptStore::DomainBucket noTesting = bucket;
         noTesting.testing = "";

         AnsiString noTestingXml = DmarcRptReporterTask::BuildReportXml(
            "2026-08-16", noTesting, "rpt-1", _T("postmaster@sender.test"), "A & B <Ltd>",
            DmarcRptReporterTask::SchemaRfc9990, "hMailServer test-generator");

         if (noTestingXml.Find("<testing>n</testing>") < 0)
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing AccountLockout\n"));
      {
         // Config travels as parameters (the /Test ini has no lockout keys),
         // clock as an explicit epoch - the TryConsumeAt pattern. Threshold 3,
         // ten-minute window, five-minute lockout.
         AccountLockout *lockout = AccountLockout::Instance();
         const time_t t0 = 1000000;

         // Two failures do not lock; the third does, and the lock expires on
         // schedule rather than early or never.
         lockout->RecordFailureAt(_T("selftest-a@x.test"), t0, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-a@x.test"), t0 + 1, 3, 600, 300);
         if (lockout->IsLockedOutAt(_T("selftest-a@x.test"), t0 + 2, 300))
            throw 0;
         lockout->RecordFailureAt(_T("selftest-a@x.test"), t0 + 2, 3, 600, 300);
         if (!lockout->IsLockedOutAt(_T("selftest-a@x.test"), t0 + 3, 300))
            throw 0;
         if (!lockout->IsLockedOutAt(_T("SELFTEST-A@X.TEST"), t0 + 3, 300))
            throw 0; // the name is one identity whatever its case
         if (lockout->IsLockedOutAt(_T("selftest-a@x.test"), t0 + 2 + 300, 300))
            throw 0;

         // Failures outside the window have aged out: two stale plus one
         // fresh is not three.
         lockout->RecordFailureAt(_T("selftest-b@x.test"), t0, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-b@x.test"), t0 + 1, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-b@x.test"), t0 + 700, 3, 600, 300);
         if (lockout->IsLockedOutAt(_T("selftest-b@x.test"), t0 + 701, 300))
            throw 0;

         // A successful logon clears the groundwork an attacker laid.
         lockout->RecordFailureAt(_T("selftest-c@x.test"), t0, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-c@x.test"), t0 + 1, 3, 600, 300);
         lockout->RecordSuccess(_T("selftest-c@x.test"));
         lockout->RecordFailureAt(_T("selftest-c@x.test"), t0 + 2, 3, 600, 300);
         if (lockout->IsLockedOutAt(_T("selftest-c@x.test"), t0 + 3, 300))
            throw 0;

         // A clock that stepped backwards after the lock was set must not
         // extend it past its nominal duration.
         lockout->RecordFailureAt(_T("selftest-d@x.test"), t0, 1, 600, 300);
         if (!lockout->IsLockedOutAt(_T("selftest-d@x.test"), t0 - 10000, 300))
            throw 0;
         if (lockout->IsLockedOutAt(_T("selftest-d@x.test"), t0 - 10000 + 301, 300))
            throw 0;

         // Attempts made WHILE locked are not counted, and the window restarts
         // when the lock expires - so a re-lock costs a fresh threshold rather
         // than one guess. Without both halves an attacker holding a trickle of
         // guesses on a name kept it locked for ever, which is a denial of
         // service against the mailbox the feature exists to protect.
         // Threshold 3, window 600, lock 300.
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 1, 3, 600, 300);
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 2, 3, 600, 300);
         if (!lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 3, 300))
            throw 0;

         // Twenty attempts across the whole lock, all refused without the password
         // being checked and therefore all uncounted.
         for (int i = 0; i < 20; i++)
            lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 10 + (i * 10), 3, 600, 300);

         // The lock still ends when it was always going to end...
         if (!lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 2 + 299, 300))
            throw 0;
         if (lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 2 + 300, 300))
            throw 0;

         // ...and the first two failures afterwards do not re-lock it, which is
         // the assertion that fails if either half of the fix is removed.
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 400, 3, 600, 300);
         if (lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 401, 300))
            throw 0;
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 402, 3, 600, 300);
         if (lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 403, 300))
            throw 0;

         // The third does.
         lockout->RecordFailureAt(_T("selftest-e@x.test"), t0 + 404, 3, 600, 300);
         if (!lockout->IsLockedOutAt(_T("selftest-e@x.test"), t0 + 405, 300))
            throw 0;

         lockout->RecordSuccess(_T("selftest-e@x.test"));

         lockout->RecordSuccess(_T("selftest-a@x.test"));
         lockout->RecordSuccess(_T("selftest-b@x.test"));
         lockout->RecordSuccess(_T("selftest-c@x.test"));
         lockout->RecordSuccess(_T("selftest-d@x.test"));
      }

      OutputDebugString(_T("hMailServer: Testing CompromiseDetector\n"));
      {
         // The scoring alone (CompromiseDetector::Score), with the shipped
         // thresholds - baseline 30 days, minimum history 20, factor 4, floors
         // 30 and 20, share 80, lock at 3 - and no database and no clock: the
         // inputs are what Assess would have read.
         const CompromiseDetector::Thresholds shipped;
         CompromiseDetector::Inputs inputs;

         // The everyday message: one to a known address, at a seen hour, from
         // a seen network, from an account with a baseline. Nothing scores.
         inputs.history = 100;
         inputs.last_hour = 1;
         inputs.peak_hour = 6;
         inputs.hour_recipients = 1;
         inputs.hour_new_recipients = 0;
         CompromiseDetector::Verdict verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 0 || verdict.takeover || !verdict.has_baseline || !verdict.signals.IsEmpty())
            throw 0;

         // A surge past four times the busiest hour: volume, two points, not
         // yet a takeover. One short of the floor is no surge; at the floor
         // but within four times the peak is none either.
         inputs.last_hour = 30;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 2 || verdict.takeover || verdict.signals != _T("volume"))
            throw 0;
         inputs.last_hour = 29;
         if (CompromiseDetector::Score(inputs, shipped).score != 0)
            throw 0;
         inputs.last_hour = 30;
         inputs.peak_hour = 8;
         if (CompromiseDetector::Score(inputs, shipped).score != 0)
            throw 0;
         inputs.peak_hour = 6;

         // Strangers: twenty recipients in the hour of whom sixteen are new is
         // the recipients signal; fifteen is not.
         inputs.last_hour = 1;
         inputs.hour_recipients = 20;
         inputs.hour_new_recipients = 16;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 2 || verdict.takeover || verdict.signals != _T("recipients"))
            throw 0;
         inputs.hour_new_recipients = 15;
         if (CompromiseDetector::Score(inputs, shipped).score != 0)
            throw 0;

         // The two small signals alone never lock: a late night from a hotel
         // is two points.
         inputs.hour_recipients = 1;
         inputs.hour_new_recipients = 0;
         inputs.hour_seen = false;
         inputs.address_seen = false;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 2 || verdict.takeover || verdict.signals != _T("hour, address"))
            throw 0;

         // A surge at a strange hour is three, and the takeover.
         inputs.last_hour = 30;
         inputs.address_seen = true;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 3 || !verdict.takeover || verdict.signals != _T("volume, hour") || verdict.reason.IsEmpty())
            throw 0;

         // Sensitivity: relaxed needs one point more, so the same three is not
         // a takeover; strict needs one fewer, so a surge alone is.
         inputs.sensitivity = CompromiseDetector::SensitivityRelaxed;
         if (CompromiseDetector::Score(inputs, shipped).takeover)
            throw 0;
         inputs.sensitivity = CompromiseDetector::SensitivityStrict;
         inputs.hour_seen = true;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 2 || !verdict.takeover)
            throw 0;

         // The clamp: strict with a lock score of 1 still needs one point,
         // never none.
         CompromiseDetector::Thresholds hair = shipped;
         hair.score_to_lock = 1;
         inputs.last_hour = 1;
         if (CompromiseDetector::Score(inputs, hair).takeover)
            throw 0;
         inputs.hour_seen = false;
         if (!CompromiseDetector::Score(inputs, hair).takeover)
            throw 0;
         inputs.sensitivity = CompromiseDetector::SensitivityDefault;
         inputs.hour_seen = true;

         // No baseline (history below the minimum): the floors alone count,
         // whatever the peak, and the hour and the address never score. A
         // surge to strangers locks a brand-new account; a surge alone does not.
         inputs.history = 19;
         inputs.last_hour = 30;
         inputs.peak_hour = 100;
         inputs.hour_seen = false;
         inputs.address_seen = false;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.has_baseline || verdict.score != 2 || verdict.takeover || verdict.signals != _T("volume"))
            throw 0;
         inputs.hour_recipients = 20;
         inputs.hour_new_recipients = 20;
         verdict = CompromiseDetector::Score(inputs, shipped);
         if (verdict.score != 4 || !verdict.takeover || verdict.signals != _T("volume, recipients"))
            throw 0;

         // The network an address is judged by: the /24, the /64, and an
         // address that is neither as itself.
         if (CompromiseDetector::NetworkOf(_T("192.0.2.77")) != _T("192.0.2.0/24"))
            throw 0;
         if (CompromiseDetector::NetworkOf(_T("2001:db8:1:2:3:4:5:6")) != _T("2001:db8:1:2::/64"))
            throw 0;
         if (CompromiseDetector::NetworkOf(_T("2001:DB8::1")) != _T("2001:db8::/64"))
            throw 0;
         if (CompromiseDetector::NetworkOf(_T("Not An Address")) != _T("not an address"))
            throw 0;
      }

      OutputDebugString(_T("hMailServer: Testing Base64\n"));
      ModifiedUTF7Tester modifiedUTF7Tester;
      modifiedUTF7Tester.Test();

      OutputDebugString(_T("hMailServer: Testing AddresslistParser\n"));
      AddresslistParserTester *pTest2 = new AddresslistParserTester();
      pTest2->Test();
      delete pTest2;

      OutputDebugString(_T("hMailServer: Testing charset\n"));
      CharsetTester *pCharsetTester = new CharsetTester;
      pCharsetTester->Test();
      delete pCharsetTester;


      OutputDebugString(_T("hMailServer: Testing LocalIPAddresses\n"));
      LocalIPAddressesTester *pTest4 = new LocalIPAddressesTester();
      pTest4->Test();
      delete pTest4;

      OutputDebugString(_T("hMailServer: Testing MessageData\n"));
      MessageDataTester *pMsgData = new MessageDataTester();
      pMsgData->Test();
      delete pMsgData;

      OutputDebugString(_T("hMailServer: Testing Time\n"));
      TimeTester *pTimeT = new TimeTester();
      pTimeT->Test();
      delete pTimeT;




      // The legacy Blowfish scheme of stored secrets: the cipher against Eric
      // Young's published vectors, and the scheme against values 6.3.4 wrote and
      // read. legacy_blowfish_tests_main.cpp runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing LegacyBlowfish\n"));
      LegacyBlowfishTester legacyBlowfishTester;
      legacyBlowfishTester.Test();

      OutputDebugString(_T("hMailServer: Testing IMAPSimpleCommandParserTester\n"));
      IMAPSimpleCommandParserTester *pTest = new IMAPSimpleCommandParserTester();
      pTest->Test();
      delete pTest;

      // What the parsers of a client's text make, held to what they made up to
      // 6.3.5 over generated cases, and the time they take held to one pass:
      // String's case-ignoring search and replace, the items of a FETCH, the
      // base subject SORT and THREAD read, a message set, a STORE's keywords,
      // a FETCH's HEADER.FIELDS sections and the bodies its part numbers name;
      // and the criteria of a SEARCH, SORT or THREAD, each named again beside
      // itself asked of a message once. imap-parse-tests runs the same on Linux.
      OutputDebugString(_T("hMailServer: Testing the IMAP parsers' time\n"));
      StringNoCaseTester stringNoCaseTester;
      stringNoCaseTester.Test();
      IMAPFetchParserTester imapFetchParserTester;
      imapFetchParserTester.Test();
      IMAPThreadSubjectTester imapThreadSubjectTester;
      imapThreadSubjectTester.Test();
      IMAPSequenceSetTester imapSequenceSetTester;
      imapSequenceSetTester.Test();
      IMAPStoreKeywordsTester imapStoreKeywordsTester;
      imapStoreKeywordsTester.Test();
      IMAPFetchHeaderFieldsTester imapFetchHeaderFieldsTester;
      imapFetchHeaderFieldsTester.Test();
      IMAPFetchSectionsTester imapFetchSectionsTester;
      imapFetchSectionsTester.Test();
      IMAPSearchParserTester imapSearchParserTester;
      imapSearchParserTester.Test();

      // The bytes a FETCH answers for a message's parts - each part's MIME
      // header and body, a multipart's with its preamble, boundaries and
      // epilogue - held to the bytes generated messages were written with; and
      // a BODYSTRUCTURE of messages nested past its depth or of a multipart with
      // no parts, and a BODY, that a client can read. imap-parse-tests runs the
      // same on Linux.
      OutputDebugString(_T("hMailServer: Testing the bytes of a FETCH's parts\n"));
      IMAPFetchPartBytesTester imapFetchPartBytesTester;
      imapFetchPartBytesTester.Test();

      // A message's keywords, changed and read by several threads at once. A
      // message in an IMAP folder is one object every session on the mailbox
      // shares, and STORE is handed that live object, so two STOREs, or a
      // STORE beside a FETCH, a SELECT or a copy of the message, meet on the
      // same keyword string. It was a plain string with no lock: two threads
      // assigning it freed one buffer twice, and a read-modify-write lost the
      // other thread's keyword. Two writers here add and remove keywords of
      // their own while two readers copy the message and list its keywords;
      // at the end each writer's last additions must all be there, and no
      // reader may ever have seen a keyword that neither writer wrote.
      OutputDebugString(_T("hMailServer: Testing Message keywords under concurrent STORE\n"));
      {
         Message shared(false);
         const int keywordsPerWriter = 20;
         const int rounds = 300;
         std::atomic<bool> stop(false);
         std::atomic<int> strangers(0);

         auto writer = [&](const String prefix)
         {
            for (int round = 0; round < rounds; round++)
            {
               for (int k = 0; k < keywordsPerWriter; k++)
                  shared.AddKeyword(prefix + StringParser::IntToString(k));
               for (int k = 0; k < keywordsPerWriter; k++)
                  shared.RemoveKeyword(prefix + StringParser::IntToString(k));
            }
            for (int k = 0; k < keywordsPerWriter; k++)
               shared.AddKeyword(prefix + StringParser::IntToString(k));
         };

         auto reader = [&]()
         {
            while (!stop)
            {
               Message copy(shared);
               std::vector<String> list = copy.GetKeywordList();
               std::vector<String> live = shared.GetKeywordList();
               list.insert(list.end(), live.begin(), live.end());
               for (const String &keyword : list)
               {
                  if (keyword.GetLength() < 2 || (keyword[0] != 'a' && keyword[0] != 'b'))
                     strangers++;
               }
            }
         };

         boost::thread readerOne(reader);
         boost::thread readerTwo(reader);
         boost::thread writerA(writer, String(_T("a")));
         boost::thread writerB(writer, String(_T("b")));
         writerA.join();
         writerB.join();
         stop = true;
         readerOne.join();
         readerTwo.join();

         if (strangers != 0)
            HM_TEST_FAILED();

         std::vector<String> result = shared.GetKeywordList();
         if (result.size() != (size_t) (2 * keywordsPerWriter))
            HM_TEST_FAILED();
         for (int k = 0; k < keywordsPerWriter; k++)
         {
            if (!shared.HasKeyword(String(_T("a")) + StringParser::IntToString(k)) ||
                !shared.HasKeyword(String(_T("b")) + StringParser::IntToString(k)))
               HM_TEST_FAILED();
         }
      }

      // A message's flags, changed by several threads at once: STORE, FETCH's
      // \Seen, the REST API and the collection's \Recent claim all set and
      // clear bits of the same shared message. A one-flag setter was
      // "flags_ = flags_ | bit", a read and a write: a thread that read the
      // flags just before another set its bit wrote them back without it. Each
      // thread here owns one bit, sets it and clears it over and over, and
      // checks after every set that its bit is still there - no other thread
      // touches it, so only a lost update can take it away.
      OutputDebugString(_T("hMailServer: Testing Message flags under concurrent change\n"));
      {
         Message shared(false);
         const int rounds = 200000;
         std::atomic<int> lost(0);

         auto owner = [&](int bit)
         {
            for (int round = 0; round < rounds; round++)
            {
               switch (bit)
               {
               case Message::FlagSeen: shared.SetFlagSeen(true); break;
               case Message::FlagFlagged: shared.SetFlagFlagged(true); break;
               case Message::FlagAnswered: shared.SetFlagAnswered(true); break;
               default: shared.SetFlagRecent(true); break;
               }

               if ((shared.GetFlags() & bit) == 0)
                  lost++;

               switch (bit)
               {
               case Message::FlagSeen: shared.SetFlagSeen(false); break;
               case Message::FlagFlagged: shared.SetFlagFlagged(false); break;
               case Message::FlagAnswered: shared.SetFlagAnswered(false); break;
               default: shared.SetFlagRecent(false); break;
               }
            }
         };

         boost::thread seen(owner, (int) Message::FlagSeen);
         boost::thread flagged(owner, (int) Message::FlagFlagged);
         boost::thread answered(owner, (int) Message::FlagAnswered);
         boost::thread recent(owner, (int) Message::FlagRecent);
         seen.join();
         flagged.join();
         answered.join();
         recent.join();

         if (lost != 0 || shared.GetFlags() != 0)
            HM_TEST_FAILED();

         // SetFlagBits changes the masked bits only.
         shared.SetFlags((short) (Message::FlagSeen | Message::FlagDeleted | Message::FlagRecent));
         shared.SetFlagBits((short) (Message::FlagSeen | Message::FlagFlagged), (short) Message::FlagFlagged);
         if (shared.GetFlags() != (short) (Message::FlagFlagged | Message::FlagDeleted | Message::FlagRecent))
            HM_TEST_FAILED();
      }

#ifndef HM_PLATFORM_POSIX
      // An administrator's install of an update (Util/AdminUpdate.h): the checks
      // the elevated stage makes - the permission rule, the work folder, the safe
      // copy, Authenticode and the publisher pin against the real signed
      // installers, the lowered child - and the stage's protocol with its
      // Sigstore child, with hMailServer.exe --verify-bundle run as the stage
      // runs it. Last, because the installers are fixtures fetched onto the bench
      // (build/fetch-installer-fixtures.ps1), and a bench without them fails here
      // rather than hiding every tester after it.
      OutputDebugString(_T("hMailServer: Testing AdminUpdateTester\n"));
      AdminUpdateTester adminUpdateTester;
      adminUpdateTester.Test();

      OutputDebugString(_T("hMailServer: Testing AdminUpdateStageTester\n"));
      AdminUpdateStageTester adminUpdateStageTester;
      adminUpdateStageTester.Test();
#endif
   }

   void 
   ClassTester::LoadSettings_()
   {
      String sAppPath = Utilities::GetBinDirectory();
      if (sAppPath.Right(1) != FileUtilities::PathSeparator)
         sAppPath += FileUtilities::PathSeparator;

      String sConfigFile = sAppPath + "test_config.xml";
      String sTestSpec = FileUtilities::ReadCompleteTextFile(sConfigFile);

      if (sTestSpec.IsEmpty())
         return;

      XDoc oDoc;
      oDoc.Load(sTestSpec);

      XNode *pBackupNode = oDoc.GetChild(_T("Config"));

      if (!pBackupNode)
         HM_TEST_FAILED();

      mime_data_path_ = pBackupNode->GetChildValue(_T("MimeDataPath"));
   }

   void 
   ClassTester::TestBackup_()
   {
      std::shared_ptr<BackupManager> pBackupManager = Application::Instance()->GetBackupManager();

      // No passphrase: the archive this names is an old unencrypted one. An
      // encrypted archive would be opened by passing the passphrase here, and
      // would answer nothing at all without it - which is what failureReason
      // would then say.
      String failureReason;
      std::shared_ptr<Backup> pBackup = pBackupManager->LoadBackup("C:\\Temp\\Backup\\HMBackup 2006-12-10 091555.zip", String(), failureReason);

      if (!pBackup)
         return;

      pBackup->SetRestoreOptions(1 | 2 | 4 | 8 | 16 | 32);
      pBackupManager->StartRestore(pBackup);

      Sleep(1000000);
   }
}
