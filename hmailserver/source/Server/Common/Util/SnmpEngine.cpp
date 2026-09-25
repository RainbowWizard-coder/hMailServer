// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the SNMP agent does with one datagram. See SnmpEngine.h for what it
// answers and the bounds it keeps, and docs/Snmp.md for the operator's view.

#include "StdAfx.h"

#include "SnmpEngine.h"
#include "SnmpUsm.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const size_t SnmpEngine::MaxMessageSize;
   const long long SnmpEngine::MinimumMessageSize;
   const size_t SnmpEngine::MaxVarbinds;
   const long long SnmpEngine::MaxRepetitions;
   const size_t SnmpEngine::MaxResponseVarbinds;
   const long long SnmpEngine::TimeWindowSeconds;
   const long long SnmpEngine::MaxEngineBoots;
   const size_t SnmpEngine::V2cAmplification;

   namespace
   {
      // error-status values of RFC 3416 section 3.
      const long long ErrorNoError = 0;
      const long long ErrorTooBig = 1;
      const long long ErrorNoAccess = 6;

      // The Report counters (RFC 3414 usmStats, RFC 3412 snmpMPDStats and
      // RFC 3413's context counters), each with its instance.
      const char *UsmStatsUnsupportedSecLevels = "1.3.6.1.6.3.15.1.1.1.0";
      const char *UsmStatsNotInTimeWindows = "1.3.6.1.6.3.15.1.1.2.0";
      const char *UsmStatsUnknownUserNames = "1.3.6.1.6.3.15.1.1.3.0";
      const char *UsmStatsUnknownEngineIds = "1.3.6.1.6.3.15.1.1.4.0";
      const char *UsmStatsWrongDigests = "1.3.6.1.6.3.15.1.1.5.0";
      const char *UsmStatsDecryptionErrors = "1.3.6.1.6.3.15.1.1.6.0";
      const char *SnmpUnknownPduHandlers = "1.3.6.1.6.3.11.2.1.3.0";
      const char *SnmpUnknownContexts = "1.3.6.1.6.3.12.1.5.0";

      // sysServices (RFC 3418): an application host - layers 4 and 7, 8 + 64.
      const long long SysServices = 72;

      // snmpEnableAuthenTraps: disabled(2). This agent sends no notification.
      const long long AuthenTrapsDisabled = 2;

      // The User-based Security Model's number (RFC 3411 SnmpSecurityModel).
      const long long SecurityModelUsm = 3;

      const unsigned char FlagAuth = 0x01;
      const unsigned char FlagPriv = 0x02;
      const unsigned char FlagReportable = 0x04;

      void Add(std::vector<SnmpBinding> &mib, const char *oid, const SnmpValue &value)
      {
         mib.push_back(SnmpBinding(SnmpBer::Oid(oid), 1, value));
      }

      bool LessByName(const SnmpBinding &left, const SnmpBinding &right)
      {
         return SnmpBer::Compare(left.name, right.name) < 0;
      }

      // The first binding whose name is exactly this one, or null.
      const SnmpBinding *FindExact(const std::vector<SnmpBinding> &mib, const SnmpOid &name)
      {
         std::vector<SnmpBinding>::const_iterator found = std::lower_bound(mib.begin(), mib.end(), name,
            [](const SnmpBinding &binding, const SnmpOid &wanted) { return SnmpBer::Compare(binding.name, wanted) < 0; });

         if (found == mib.end() || SnmpBer::Compare(found->name, name) != 0)
            return nullptr;

         return &*found;
      }

      // The first binding whose name comes after this one - GetNext - or null
      // at the end of the MIB view.
      const SnmpBinding *FindNext(const std::vector<SnmpBinding> &mib, const SnmpOid &name)
      {
         std::vector<SnmpBinding>::const_iterator found = std::upper_bound(mib.begin(), mib.end(), name,
            [](const SnmpOid &wanted, const SnmpBinding &binding) { return SnmpBer::Compare(wanted, binding.name) < 0; });

         if (found == mib.end())
            return nullptr;

         return &*found;
      }

      // Whether some object this agent serves is the object the name asks for an
      // instance of: the difference between noSuchInstance and noSuchObject. The
      // object's own name counts - a Get of sysUpTime without its .0 names an
      // object this agent has and an instance it has not (RFC 3416 4.2.1).
      bool NamesAKnownObject(const std::vector<SnmpBinding> &mib, const SnmpOid &name)
      {
         for (const SnmpBinding &binding : mib)
         {
            if (binding.instanceLength == 0 || binding.instanceLength >= binding.name.size())
               continue;

            SnmpOid object(binding.name.begin(), binding.name.end() - (std::ptrdiff_t) binding.instanceLength);

            if (name.size() >= object.size() && SnmpBer::StartsWith(name, object))
               return true;
         }

         return false;
      }

      SnmpBytes Binding(const SnmpOid &name, const SnmpBytes &valueTlv)
      {
         SnmpBytes content = SnmpBer::EncodeOid(name);
         SnmpBer::Append(content, valueTlv);
         return SnmpBer::Wrap(SnmpTagSequence, content);
      }

      bool BytesEqual(const unsigned char *left, size_t leftLength, const SnmpBytes &right)
      {
         if (leftLength != right.size())
            return false;

         if (leftLength == 0)
            return true;

         return memcmp(left, &right[0], leftLength) == 0;
      }

      // A user name, which is no secret - it crosses the network in the clear in
      // every SNMPv3 message - compared as it is. An empty expected name matches
      // nothing.
      bool UserEqual(const unsigned char *given, size_t givenLength, const std::string &expected)
      {
         if (expected.empty() || givenLength != expected.size())
            return false;

         return memcmp(given, expected.data(), givenLength) == 0;
      }

      void AppendUnsigned64(SnmpBytes &out, unsigned long long value)
      {
         out.push_back((unsigned char) ((value >> 56) & 0xFF));
         out.push_back((unsigned char) ((value >> 48) & 0xFF));
         out.push_back((unsigned char) ((value >> 40) & 0xFF));
         out.push_back((unsigned char) ((value >> 32) & 0xFF));
         out.push_back((unsigned char) ((value >> 24) & 0xFF));
         out.push_back((unsigned char) ((value >> 16) & 0xFF));
         out.push_back((unsigned char) ((value >> 8) & 0xFF));
         out.push_back((unsigned char) (value & 0xFF));
      }
   }

   // ------------------------------------------------------------ SnmpValue

   SnmpValue::SnmpValue() :
      tag(SnmpTagNull),
      integer(0),
      number(0)
   {
   }

   SnmpValue
   SnmpValue::MakeInteger(long long value)
   {
      SnmpValue made;
      made.tag = SnmpTagInteger;
      made.integer = value;
      return made;
   }

   SnmpValue
   SnmpValue::MakeCounter32(unsigned long long value)
   {
      SnmpValue made;
      made.tag = SnmpTagCounter32;
      made.number = value & 0xFFFFFFFFULL;
      return made;
   }

   SnmpValue
   SnmpValue::MakeGauge32(unsigned long long value)
   {
      SnmpValue made;
      made.tag = SnmpTagGauge32;
      made.number = value > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : value;
      return made;
   }

   SnmpValue
   SnmpValue::MakeTimeTicks(unsigned long long hundredths)
   {
      SnmpValue made;
      made.tag = SnmpTagTimeTicks;
      made.number = hundredths & 0xFFFFFFFFULL;
      return made;
   }

   SnmpValue
   SnmpValue::MakeText(const std::string &text)
   {
      SnmpValue made;
      made.tag = SnmpTagOctetString;
      made.octets.assign(text.begin(), text.end());
      return made;
   }

   SnmpValue
   SnmpValue::MakeOctets(const SnmpBytes &octets)
   {
      SnmpValue made;
      made.tag = SnmpTagOctetString;
      made.octets = octets;
      return made;
   }

   SnmpValue
   SnmpValue::MakeOid(const SnmpOid &oid)
   {
      SnmpValue made;
      made.tag = SnmpTagObjectIdentifier;
      made.identifier = oid;
      return made;
   }

   SnmpBytes
   SnmpValue::Encode() const
   {
      switch (tag)
      {
      case SnmpTagInteger:
         return SnmpBer::EncodeInteger(integer);
      case SnmpTagCounter32:
      case SnmpTagGauge32:
      case SnmpTagTimeTicks:
      case SnmpTagCounter64:
         return SnmpBer::EncodeUnsigned(tag, number);
      case SnmpTagOctetString:
         return SnmpBer::EncodeOctets(octets);
      case SnmpTagObjectIdentifier:
         return SnmpBer::EncodeOid(identifier);
      default:
         return SnmpBer::EncodeNull(tag);
      }
   }

   // ------------------------------------------------------------ SnmpEngine

   SnmpEngine::SnmpEngine(const SnmpEngineSettings &settings, SnmpMibSource *source) :
      settings_(settings),
      source_(source),
      elapsed_(0),
      salt_(0)
   {
      unsigned char seed[8];
      if (RAND_bytes(seed, (int) sizeof(seed)) == 1)
      {
         for (size_t i = 0; i < sizeof(seed); i++)
            salt_ = (salt_ << 8) | (unsigned long long) seed[i];
      }

      if (settings_.engineBoots < 1)
         settings_.engineBoots = 1;

      if (settings_.engineBoots > MaxEngineBoots)
         settings_.engineBoots = MaxEngineBoots;

      if (!settings_.community.empty())
         SnmpUsm::Sha256((const unsigned char *) settings_.community.data(), settings_.community.size(), community_digest_);
   }

   SnmpEngine::~SnmpEngine()
   {
      // The two keys and the community are the secrets this object holds; they
      // are overwritten before the memory goes back.
      SnmpUsm::Scrub(settings_.authKey);
      SnmpUsm::Scrub(settings_.privKey);
      SnmpUsm::Scrub(settings_.community);
      SnmpUsm::Scrub(community_digest_);
   }

   void
   SnmpEngine::SetElapsedMilliseconds(unsigned long long elapsed)
   {
      elapsed_ = elapsed;
   }

   long long
   SnmpEngine::GetEngineTime() const
   {
      const unsigned long long seconds = elapsed_ / 1000ULL;
      return seconds > 2147483647ULL ? 2147483647LL : (long long) seconds;
   }

   void
   SnmpEngine::CountOversizedDatagram()
   {
      counters_.inPkts++;
      counters_.inAsnParseErrs++;
   }

   bool
   SnmpEngine::Process(const unsigned char *data, size_t length, SnmpBytes &response)
   {
      response.clear();
      counters_.inPkts++;

      if (!data || length == 0 || length > MaxMessageSize)
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // The whole datagram is one SEQUENCE and nothing after it.
      SnmpBerReader top(data, length);
      SnmpTlv message;
      SnmpBerReader body;

      long long version = -1;

      if (!top.Next(SnmpTagSequence, message) || !top.AtEnd() ||
          !top.Enter(message, body) || !body.NextInteger(version))
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      if (version == 1)
         return ProcessV2c_(body, length, response);

      if (version == 3)
         return ProcessV3_(data, length, body, response);

      // SNMPv1 (0), and anything else.
      counters_.inBadVersions++;
      return false;
   }

   bool
   SnmpEngine::ProcessV2c_(SnmpBerReader &body, size_t requestLength, SnmpBytes &response)
   {
      SnmpTlv community;
      SnmpTlv pdu;

      if (!body.Next(SnmpTagOctetString, community) || !body.Next(pdu) || !body.AtEnd())
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // Off is an unsupported version, which is what snmpInBadVersions counts.
      if (!settings_.v2cEnabled)
      {
         counters_.inBadVersions++;
         return false;
      }

      // Compared by SHA-256 digest, so that neither where the two differ nor how
      // long either is shows in the time the comparison takes.
      SnmpBytes given;
      if (community_digest_.size() != 32 ||
          !SnmpUsm::Sha256(community.value, community.length, given) || given.size() != 32 ||
          !SnmpUsm::DigestsEqual(&given[0], &community_digest_[0], 32))
      {
         counters_.inBadCommunityNames++;
         return false;
      }

      PduRequest request;
      const PduParse parsed = ParsePdu_(body, pdu, request);

      if (parsed == PduMalformed)
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // A Response, a Trap, an Inform or a Report sent to an agent: nothing here
      // receives them.
      if (parsed == PduNotHandled)
         return false;

      // A Set with a community that may only read (RFC 3418 snmpInBadCommunityUses).
      if (request.type == SnmpTagSetRequest)
         counters_.inBadCommunityUses++;

      // At most V2cAmplification times the request, and never below the 484
      // octets every engine takes: a forged source gets back no more than a small
      // multiple of what it sent (SnmpEngine.h).
      size_t limit = requestLength * V2cAmplification;
      if (limit < (size_t) MinimumMessageSize)
         limit = (size_t) MinimumMessageSize;
      if (limit > MaxMessageSize)
         limit = MaxMessageSize;

      Outgoing envelope;
      envelope.version = 1;
      envelope.community = settings_.community;
      envelope.limit = limit;

      return Answer_(request, envelope, response);
   }

   bool
   SnmpEngine::ProcessV3_(const unsigned char *data, size_t length, SnmpBerReader &body, SnmpBytes &response)
   {
      // ---- msgGlobalData, msgSecurityParameters, msgData (RFC 3412 section 6)
      SnmpTlv header;
      SnmpTlv securityParameters;
      SnmpTlv scopedData;

      if (!body.Next(SnmpTagSequence, header) ||
          !body.Next(SnmpTagOctetString, securityParameters) ||
          !body.Next(scopedData) ||
          !body.AtEnd())
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      SnmpBerReader headerReader;
      long long messageId = -1;
      long long messageMaxSize = 0;
      long long securityModel = 0;
      SnmpTlv flagsTlv;

      if (!body.Enter(header, headerReader) ||
          !headerReader.NextInteger(messageId) ||
          !headerReader.NextInteger(messageMaxSize) ||
          !headerReader.Next(SnmpTagOctetString, flagsTlv) ||
          !headerReader.NextInteger(securityModel) ||
          !headerReader.AtEnd())
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // The ranges RFC 3412 gives each field. A value outside one is a message
      // that does not decode.
      if (messageId < 0 || messageId > 2147483647LL ||
          messageMaxSize < MinimumMessageSize || messageMaxSize > 2147483647LL ||
          flagsTlv.length != 1 ||
          securityModel < 1 || securityModel > 2147483647LL)
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      const unsigned char flags = flagsTlv.value[0];
      const bool authFlag = (flags & FlagAuth) != 0;
      const bool privFlag = (flags & FlagPriv) != 0;
      const bool reportable = (flags & FlagReportable) != 0;

      // RFC 3412 7.2 step 4: a security model this engine does not have.
      if (securityModel != SecurityModelUsm)
      {
         counters_.unknownSecurityModels++;
         return false;
      }

      // RFC 3412 7.2 step 5: privacy without authentication is no level at all.
      if (privFlag && !authFlag)
      {
         counters_.invalidMsgs++;
         return false;
      }

      // ---- UsmSecurityParameters (RFC 3414 section 2.4), inside the OCTET STRING
      SnmpBerReader securityReader;
      SnmpBerReader usmReader;
      SnmpTlv usm;
      SnmpTlv engineIdTlv;
      SnmpTlv userTlv;
      SnmpTlv authTlv;
      SnmpTlv privTlv;
      long long boots = -1;
      long long engineTime = -1;

      if (!body.Enter(securityParameters, securityReader) ||
          !securityReader.Next(SnmpTagSequence, usm) ||
          !securityReader.AtEnd() ||
          !securityReader.Enter(usm, usmReader) ||
          !usmReader.Next(SnmpTagOctetString, engineIdTlv) ||
          !usmReader.NextInteger(boots) ||
          !usmReader.NextInteger(engineTime) ||
          !usmReader.Next(SnmpTagOctetString, userTlv) ||
          !usmReader.Next(SnmpTagOctetString, authTlv) ||
          !usmReader.Next(SnmpTagOctetString, privTlv) ||
          !usmReader.AtEnd())
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      if (engineIdTlv.length > 32 || userTlv.length > 32 ||
          boots < 0 || boots > 2147483647LL ||
          engineTime < 0 || engineTime > 2147483647LL)
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // Every Report below is addressed back with these.
      Outgoing reportEnvelope;
      reportEnvelope.version = 3;
      reportEnvelope.messageId = messageId;
      reportEnvelope.user.assign((const char *) userTlv.value, userTlv.length);
      reportEnvelope.limit = (size_t) messageMaxSize < MaxMessageSize ? (size_t) messageMaxSize : MaxMessageSize;

      // The request-id a Report echoes, where it can be read without keys: a
      // plaintext scoped PDU, which is what a discovery probe sends. Otherwise 0.
      long long plainRequestId = 0;

      if (!privFlag && scopedData.tag == SnmpTagSequence)
      {
         SnmpBerReader scopedReader;
         SnmpTlv contextEngineId;
         SnmpTlv contextName;
         SnmpTlv pdu;
         SnmpBerReader pduReader;
         long long requestId = 0;

         if (body.Enter(scopedData, scopedReader) &&
             scopedReader.Next(SnmpTagOctetString, contextEngineId) &&
             scopedReader.Next(SnmpTagOctetString, contextName) &&
             scopedReader.Next(pdu) &&
             scopedReader.Enter(pdu, pduReader) &&
             pduReader.NextInteger(requestId) &&
             requestId >= -2147483648LL && requestId <= 2147483647LL)
         {
            plainRequestId = requestId;
         }
      }

      // ---- RFC 3414 section 3.2, in its order.

      // Step 3: a request for another engine, or for none - the discovery probe.
      if (!BytesEqual(engineIdTlv.value, engineIdTlv.length, settings_.engineId))
      {
         counters_.unknownEngineIds++;
         return Report_(reportEnvelope, reportable, plainRequestId, UsmStatsUnknownEngineIds, counters_.unknownEngineIds, response);
      }

      // Step 4: a user this engine does not have.
      if (!UserEqual(userTlv.value, userTlv.length, settings_.user) ||
          settings_.authKey.size() != SnmpUsm::KeyLength ||
          settings_.privKey.size() < SnmpUsm::PrivKeyLength)
      {
         counters_.unknownUserNames++;
         return Report_(reportEnvelope, reportable, plainRequestId, UsmStatsUnknownUserNames, counters_.unknownUserNames, response);
      }

      // Step 5: this agent answers its user at authPriv and at nothing lower.
      if (!authFlag || !privFlag)
      {
         counters_.unsupportedSecLevels++;
         return Report_(reportEnvelope, reportable, plainRequestId, UsmStatsUnsupportedSecLevels, counters_.unsupportedSecLevels, response);
      }

      // Step 6: the digest, over the whole message with its own 24 octets zeroed.
      bool digestMatches = false;

      if (authTlv.length == SnmpUsm::AuthParameterLength)
      {
         const size_t offset = (size_t) (authTlv.value - data);

         if (offset + SnmpUsm::AuthParameterLength <= length)
         {
            SnmpBytes copy(data, data + length);
            memset(&copy[offset], 0, SnmpUsm::AuthParameterLength);

            unsigned char digest[SnmpUsm::AuthParameterLength];
            if (SnmpUsm::Digest(settings_.authKey, &copy[0], copy.size(), digest))
               digestMatches = SnmpUsm::DigestsEqual(digest, authTlv.value, SnmpUsm::AuthParameterLength);
         }
      }

      if (!digestMatches)
      {
         counters_.wrongDigests++;
         return Report_(reportEnvelope, reportable, plainRequestId, UsmStatsWrongDigests, counters_.wrongDigests, response);
      }

      // Step 7: the time window. The Report that says so is authenticated, so a
      // manager can believe the boots and time it carries (RFC 3414 section 4).
      const long long now = GetEngineTime();
      const long long drift = engineTime > now ? engineTime - now : now - engineTime;

      if (settings_.engineBoots >= MaxEngineBoots || boots != settings_.engineBoots || drift > TimeWindowSeconds)
      {
         counters_.notInTimeWindows++;

         Outgoing timeEnvelope = reportEnvelope;
         timeEnvelope.authenticated = true;

         return Report_(timeEnvelope, reportable, 0, UsmStatsNotInTimeWindows, counters_.notInTimeWindows, response);
      }

      // Step 8: decryption. msgPrivacyParameters is the sender's salt, and the IV
      // is this engine's boots and time as the message states them.
      SnmpBytes plaintext;
      bool decrypted = false;

      if (scopedData.tag == SnmpTagOctetString && privTlv.length == SnmpUsm::PrivParameterLength && scopedData.length > 0)
      {
         unsigned char iv[16];
         SnmpUsm::MakeIv((unsigned int) boots, (unsigned int) engineTime, privTlv.value, iv);
         decrypted = SnmpUsm::Decrypt(settings_.privKey, iv, scopedData.value, scopedData.length, plaintext);
      }

      // What decrypted has to begin with a SEQUENCE that fits it; anything else is
      // the mark of a wrong privacy key, and a manager is told so. Octets after the
      // SEQUENCE are not read.
      SnmpBerReader plainReader(plaintext.empty() ? nullptr : &plaintext[0], plaintext.size(), body.GetDepth() + 1);
      SnmpTlv scoped;

      if (!decrypted || plaintext.empty() || !plainReader.Next(SnmpTagSequence, scoped))
      {
         counters_.decryptionErrors++;
         return Report_(reportEnvelope, reportable, 0, UsmStatsDecryptionErrors, counters_.decryptionErrors, response);
      }

      // ---- ScopedPDU (RFC 3412 section 6.8)
      SnmpBerReader scopedReader;
      SnmpTlv contextEngineId;
      SnmpTlv contextName;
      SnmpTlv pdu;

      if (!plainReader.Enter(scoped, scopedReader) ||
          !scopedReader.Next(SnmpTagOctetString, contextEngineId) ||
          !scopedReader.Next(SnmpTagOctetString, contextName) ||
          !scopedReader.Next(pdu) ||
          !scopedReader.AtEnd())
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      PduRequest request;
      const PduParse parsed = ParsePdu_(scopedReader, pdu, request);

      if (parsed == PduMalformed)
      {
         counters_.inAsnParseErrs++;
         return false;
      }

      // An agent is the command responder and nothing else: a notification or a
      // response sent to it has no application here (RFC 3412 4.2.2.1).
      if (parsed == PduNotHandled)
      {
         counters_.unknownPduHandlers++;
         return false;
      }

      // The context: this engine's (or left empty, meaning this engine), and the
      // default context, the only one there is. The request has been verified by
      // now, so these two Reports are authenticated as the time window's is,
      // rather than sent in the clear in answer to an authPriv request.
      Outgoing contextEnvelope = reportEnvelope;
      contextEnvelope.authenticated = true;

      if (contextEngineId.length > 0 && !BytesEqual(contextEngineId.value, contextEngineId.length, settings_.engineId))
      {
         counters_.unknownPduHandlers++;
         return Report_(contextEnvelope, reportable, request.requestId, SnmpUnknownPduHandlers, counters_.unknownPduHandlers, response);
      }

      if (contextName.length > 0)
      {
         counters_.unknownContexts++;
         return Report_(contextEnvelope, reportable, request.requestId, SnmpUnknownContexts, counters_.unknownContexts, response);
      }

      Outgoing envelope = reportEnvelope;
      envelope.secure = true;

      const bool answered = Answer_(request, envelope, response);

      SnmpUsm::Scrub(plaintext);
      return answered;
   }

   SnmpEngine::PduParse
   SnmpEngine::ParsePdu_(const SnmpBerReader &parent, const SnmpTlv &pdu, PduRequest &request)
   {
      request = PduRequest();
      request.type = pdu.tag;

      switch (pdu.tag)
      {
      case SnmpTagGetRequest:
      case SnmpTagGetNextRequest:
      case SnmpTagSetRequest:
      case SnmpTagGetBulkRequest:
         break;
      case SnmpTagResponse:
      case SnmpTagTrapV1:
      case SnmpTagInformRequest:
      case SnmpTagTrapV2:
      case SnmpTagReport:
         return PduNotHandled;
      default:
         return PduMalformed;
      }

      SnmpBerReader pduReader;
      SnmpTlv list;

      if (!parent.Enter(pdu, pduReader) ||
          !pduReader.NextInteger(request.requestId) ||
          !pduReader.NextInteger(request.second) ||
          !pduReader.NextInteger(request.third) ||
          !pduReader.Next(SnmpTagSequence, list) ||
          !pduReader.AtEnd())
      {
         return PduMalformed;
      }

      // Integer32, all three (RFC 3416 section 3).
      if (request.requestId < -2147483648LL || request.requestId > 2147483647LL ||
          request.second < -2147483648LL || request.second > 2147483647LL ||
          request.third < -2147483648LL || request.third > 2147483647LL)
      {
         return PduMalformed;
      }

      SnmpBerReader listReader;
      if (!pduReader.Enter(list, listReader))
         return PduMalformed;

      while (!listReader.AtEnd())
      {
         // More than MaxVarbinds is answered tooBig; the rest is not read.
         if (request.bindings.size() >= MaxVarbinds)
         {
            request.tooManyBindings = true;
            break;
         }

         SnmpTlv binding;
         SnmpBerReader bindingReader;
         RequestBinding item;
         SnmpTlv value;

         if (!listReader.Next(SnmpTagSequence, binding) ||
             !listReader.Enter(binding, bindingReader) ||
             !bindingReader.NextOid(item.name) ||
             !bindingReader.Next(value) ||
             !bindingReader.AtEnd())
         {
            return PduMalformed;
         }

         // The value is measured and not descended into: a Get's is a NULL nobody
         // reads, and a Set's is only ever copied back.
         item.value = value.start;
         item.valueLength = value.total;

         request.bindings.push_back(item);
      }

      return PduParsed;
   }

   void
   SnmpEngine::AddOwnObjects_(std::vector<SnmpBinding> &mib) const
   {
      // system (RFC 3418 section 2).
      Add(mib, "1.3.6.1.2.1.1.1.0", SnmpValue::MakeText(settings_.description));
      Add(mib, "1.3.6.1.2.1.1.2.0", SnmpValue::MakeOid(settings_.objectId));
      Add(mib, "1.3.6.1.2.1.1.3.0", SnmpValue::MakeTimeTicks(elapsed_ / 10ULL));
      Add(mib, "1.3.6.1.2.1.1.4.0", SnmpValue::MakeText(settings_.contact));
      Add(mib, "1.3.6.1.2.1.1.5.0", SnmpValue::MakeText(settings_.name));
      Add(mib, "1.3.6.1.2.1.1.6.0", SnmpValue::MakeText(settings_.location));
      Add(mib, "1.3.6.1.2.1.1.7.0", SnmpValue::MakeInteger(SysServices));
      Add(mib, "1.3.6.1.2.1.1.8.0", SnmpValue::MakeTimeTicks(0));

      // snmp (RFC 3418 section 2).
      Add(mib, "1.3.6.1.2.1.11.1.0", SnmpValue::MakeCounter32(counters_.inPkts));
      Add(mib, "1.3.6.1.2.1.11.3.0", SnmpValue::MakeCounter32(counters_.inBadVersions));
      Add(mib, "1.3.6.1.2.1.11.4.0", SnmpValue::MakeCounter32(counters_.inBadCommunityNames));
      Add(mib, "1.3.6.1.2.1.11.5.0", SnmpValue::MakeCounter32(counters_.inBadCommunityUses));
      Add(mib, "1.3.6.1.2.1.11.6.0", SnmpValue::MakeCounter32(counters_.inAsnParseErrs));
      Add(mib, "1.3.6.1.2.1.11.30.0", SnmpValue::MakeInteger(AuthenTrapsDisabled));
      Add(mib, "1.3.6.1.2.1.11.31.0", SnmpValue::MakeCounter32(counters_.silentDrops));
      Add(mib, "1.3.6.1.2.1.11.32.0", SnmpValue::MakeCounter32(counters_.proxyDrops));

      // snmpEngine (RFC 3411 section 5).
      Add(mib, "1.3.6.1.6.3.10.2.1.1.0", SnmpValue::MakeOctets(settings_.engineId));
      Add(mib, "1.3.6.1.6.3.10.2.1.2.0", SnmpValue::MakeInteger(settings_.engineBoots));
      Add(mib, "1.3.6.1.6.3.10.2.1.3.0", SnmpValue::MakeInteger(GetEngineTime()));
      Add(mib, "1.3.6.1.6.3.10.2.1.4.0", SnmpValue::MakeInteger((long long) MaxMessageSize));

      // snmpMPDStats (RFC 3412 section 5).
      Add(mib, "1.3.6.1.6.3.11.2.1.1.0", SnmpValue::MakeCounter32(counters_.unknownSecurityModels));
      Add(mib, "1.3.6.1.6.3.11.2.1.2.0", SnmpValue::MakeCounter32(counters_.invalidMsgs));
      Add(mib, "1.3.6.1.6.3.11.2.1.3.0", SnmpValue::MakeCounter32(counters_.unknownPduHandlers));

      // snmpUnavailableContexts and snmpUnknownContexts (RFC 3413 SNMP-TARGET-MIB).
      Add(mib, "1.3.6.1.6.3.12.1.4.0", SnmpValue::MakeCounter32(counters_.unavailableContexts));
      Add(mib, "1.3.6.1.6.3.12.1.5.0", SnmpValue::MakeCounter32(counters_.unknownContexts));

      // usmStats (RFC 3414 section 5).
      Add(mib, "1.3.6.1.6.3.15.1.1.1.0", SnmpValue::MakeCounter32(counters_.unsupportedSecLevels));
      Add(mib, "1.3.6.1.6.3.15.1.1.2.0", SnmpValue::MakeCounter32(counters_.notInTimeWindows));
      Add(mib, "1.3.6.1.6.3.15.1.1.3.0", SnmpValue::MakeCounter32(counters_.unknownUserNames));
      Add(mib, "1.3.6.1.6.3.15.1.1.4.0", SnmpValue::MakeCounter32(counters_.unknownEngineIds));
      Add(mib, "1.3.6.1.6.3.15.1.1.5.0", SnmpValue::MakeCounter32(counters_.wrongDigests));
      Add(mib, "1.3.6.1.6.3.15.1.1.6.0", SnmpValue::MakeCounter32(counters_.decryptionErrors));
   }

   void
   SnmpEngine::CollectMib_(std::vector<SnmpBinding> &mib)
   {
      mib.clear();
      mib.reserve(128);

      AddOwnObjects_(mib);

      if (source_)
         source_->Collect(mib);

      std::sort(mib.begin(), mib.end(), LessByName);
   }

   SnmpBytes
   SnmpEngine::EncodePdu_(unsigned char tag, long long requestId, long long errorStatus, long long errorIndex,
                          const std::vector<SnmpBytes> &bindings, size_t count)
   {
      SnmpBytes list;
      for (size_t i = 0; i < count && i < bindings.size(); i++)
         SnmpBer::Append(list, bindings[i]);

      SnmpBytes content = SnmpBer::EncodeInteger(requestId);
      SnmpBer::Append(content, SnmpBer::EncodeInteger(errorStatus));
      SnmpBer::Append(content, SnmpBer::EncodeInteger(errorIndex));
      SnmpBer::Append(content, SnmpBer::Wrap(SnmpTagSequence, list));

      return SnmpBer::Wrap(tag, content);
   }

   bool
   SnmpEngine::Answer_(const PduRequest &request, const Outgoing &envelope, SnmpBytes &response)
   {
      response.clear();

      std::vector<SnmpBytes> bindings;
      long long errorStatus = ErrorNoError;
      long long errorIndex = 0;
      bool tooBig = request.tooManyBindings;

      if (!tooBig)
      {
         std::vector<SnmpBinding> mib;

         // A Set reads nothing: its answer is the bindings it came with.
         if (request.type != SnmpTagSetRequest)
            CollectMib_(mib);

         switch (request.type)
         {
         case SnmpTagGetRequest:
            for (const RequestBinding &asked : request.bindings)
            {
               const SnmpBinding *found = FindExact(mib, asked.name);

               if (found)
                  bindings.push_back(Binding(asked.name, found->value.Encode()));
               else if (NamesAKnownObject(mib, asked.name))
                  bindings.push_back(Binding(asked.name, SnmpBer::EncodeNull(SnmpTagNoSuchInstance)));
               else
                  bindings.push_back(Binding(asked.name, SnmpBer::EncodeNull(SnmpTagNoSuchObject)));
            }
            break;

         case SnmpTagGetNextRequest:
            for (const RequestBinding &asked : request.bindings)
            {
               const SnmpBinding *next = FindNext(mib, asked.name);

               if (next)
                  bindings.push_back(Binding(next->name, next->value.Encode()));
               else
                  bindings.push_back(Binding(asked.name, SnmpBer::EncodeNull(SnmpTagEndOfMibView)));
            }
            break;

         case SnmpTagGetBulkRequest:
            {
               // RFC 3416 section 4.2.3. A negative count is taken as zero; the
               // repetitions are held to MaxRepetitions and then to as many as
               // keep the whole answer within MaxResponseVarbinds.
               const size_t total = request.bindings.size();

               long long nonRepeaters = request.second < 0 ? 0 : request.second;
               if (nonRepeaters > (long long) total)
                  nonRepeaters = (long long) total;

               long long repetitions = request.third < 0 ? 0 : request.third;
               if (repetitions > MaxRepetitions)
                  repetitions = MaxRepetitions;

               const size_t fixed = (size_t) nonRepeaters;
               const size_t repeating = total - fixed;

               if (repeating > 0)
               {
                  const size_t room = MaxResponseVarbinds > fixed ? MaxResponseVarbinds - fixed : 0;
                  const long long fits = (long long) (room / repeating);

                  if (repetitions > fits)
                     repetitions = fits;
               }

               for (size_t i = 0; i < fixed; i++)
               {
                  const SnmpBinding *next = FindNext(mib, request.bindings[i].name);

                  if (next)
                     bindings.push_back(Binding(next->name, next->value.Encode()));
                  else
                     bindings.push_back(Binding(request.bindings[i].name, SnmpBer::EncodeNull(SnmpTagEndOfMibView)));
               }

               std::vector<SnmpOid> cursors;
               for (size_t i = fixed; i < total; i++)
                  cursors.push_back(request.bindings[i].name);

               for (long long round = 0; round < repetitions && repeating > 0; round++)
               {
                  bool anyLeft = false;

                  for (size_t column = 0; column < repeating; column++)
                  {
                     const SnmpBinding *next = FindNext(mib, cursors[column]);

                     if (next)
                     {
                        bindings.push_back(Binding(next->name, next->value.Encode()));
                        cursors[column] = next->name;
                        anyLeft = true;
                     }
                     else
                     {
                        bindings.push_back(Binding(cursors[column], SnmpBer::EncodeNull(SnmpTagEndOfMibView)));
                     }
                  }

                  // Every column is past the end: another round would be the same
                  // row of endOfMibView again (RFC 3416 4.2.3 allows stopping).
                  if (!anyLeft)
                     break;
               }
            }
            break;

         case SnmpTagSetRequest:
            // The write view is empty. noAccess, pointing at the first binding,
            // with the bindings as they came (RFC 3416 section 4.2.5).
            for (const RequestBinding &asked : request.bindings)
            {
               SnmpBytes value;
               if (asked.value && asked.valueLength > 0)
                  value.assign(asked.value, asked.value + asked.valueLength);

               bindings.push_back(Binding(asked.name, value));
            }

            if (!request.bindings.empty())
            {
               errorStatus = ErrorNoAccess;
               errorIndex = 1;
            }
            break;

         default:
            return false;
         }
      }

      SnmpBytes measured;

      if (!tooBig)
      {
         SnmpBytes pdu = EncodePdu_(SnmpTagResponse, request.requestId, errorStatus, errorIndex, bindings, bindings.size());

         if (Wrap_(envelope, pdu, false, measured) && measured.size() <= envelope.limit)
            return Wrap_(envelope, pdu, true, response);

         // GetBulk answers with fewer bindings rather than none: the most that fit,
         // found by halving, since each one only lengthens the message.
         if (request.type == SnmpTagGetBulkRequest && !bindings.empty())
         {
            size_t low = 0;
            size_t high = bindings.size();

            while (low < high)
            {
               const size_t middle = low + (high - low + 1) / 2;
               SnmpBytes candidate = EncodePdu_(SnmpTagResponse, request.requestId, errorStatus, errorIndex, bindings, middle);

               if (Wrap_(envelope, candidate, false, measured) && measured.size() <= envelope.limit)
                  low = middle;
               else
                  high = middle - 1;
            }

            if (low > 0)
            {
               SnmpBytes fitted = EncodePdu_(SnmpTagResponse, request.requestId, errorStatus, errorIndex, bindings, low);
               return Wrap_(envelope, fitted, true, response);
            }
         }
      }

      // tooBig, with no bindings (RFC 3416 4.2.1 and 4.2.2) - and if even that
      // does not fit, nothing, counted.
      std::vector<SnmpBytes> none;
      SnmpBytes tooBigPdu = EncodePdu_(SnmpTagResponse, request.requestId, ErrorTooBig, 0, none, 0);

      if (!Wrap_(envelope, tooBigPdu, false, measured) || measured.size() > envelope.limit)
      {
         counters_.silentDrops++;
         return false;
      }

      return Wrap_(envelope, tooBigPdu, true, response);
   }

   bool
   SnmpEngine::Report_(const Outgoing &envelope, bool reportable, long long requestId,
                       const char *counterOid, unsigned int counterValue, SnmpBytes &response)
   {
      response.clear();

      // RFC 3412 7.1: a Report only for a request that asked for one.
      if (!reportable)
         return false;

      std::vector<SnmpBytes> bindings;
      bindings.push_back(Binding(SnmpBer::Oid(counterOid), SnmpValue::MakeCounter32(counterValue).Encode()));

      SnmpBytes pdu = EncodePdu_(SnmpTagReport, requestId, ErrorNoError, 0, bindings, bindings.size());

      Outgoing reportEnvelope = envelope;
      reportEnvelope.secure = false;

      SnmpBytes measured;
      if (!Wrap_(reportEnvelope, pdu, false, measured) || measured.size() > reportEnvelope.limit)
      {
         counters_.silentDrops++;
         return false;
      }

      return Wrap_(reportEnvelope, pdu, true, response);
   }

   bool
   SnmpEngine::Wrap_(const Outgoing &envelope, const SnmpBytes &pdu, bool finalise, SnmpBytes &message)
   {
      message.clear();

      if (envelope.version == 1)
      {
         SnmpBytes content = SnmpBer::EncodeInteger(1);
         SnmpBer::Append(content, SnmpBer::EncodeOctets(envelope.community));
         SnmpBer::Append(content, pdu);
         message = SnmpBer::Wrap(SnmpTagSequence, content);
         return true;
      }

      if (envelope.version != 3)
         return false;

      // The scoped PDU: this engine's ID as the context engine, the default context.
      SnmpBytes scopedContent = SnmpBer::EncodeOctets(settings_.engineId);
      SnmpBer::Append(scopedContent, SnmpBer::EncodeOctets(std::string()));
      SnmpBer::Append(scopedContent, pdu);
      SnmpBytes scoped = SnmpBer::Wrap(SnmpTagSequence, scopedContent);

      // The boots and time go in the security parameters and, for a secure
      // answer, into the IV - one reading of the clock for both.
      const long long boots = settings_.engineBoots;
      const long long engineTime = GetEngineTime();

      const bool digest = envelope.secure || envelope.authenticated;

      unsigned char flags = 0;
      if (envelope.secure)
         flags = FlagAuth | FlagPriv;
      else if (envelope.authenticated)
         flags = FlagAuth;

      SnmpBytes salt;
      SnmpBytes data;

      if (envelope.secure)
      {
         if (finalise)
         {
            AppendUnsigned64(salt, salt_);
            salt_++;

            unsigned char iv[16];
            SnmpUsm::MakeIv((unsigned int) boots, (unsigned int) engineTime, &salt[0], iv);

            SnmpBytes encrypted;
            if (!SnmpUsm::Encrypt(settings_.privKey, iv, &scoped[0], scoped.size(), encrypted))
               return false;

            data = SnmpBer::EncodeOctets(encrypted);
         }
         else
         {
            // CFB keeps the length: measuring the plaintext measures the answer.
            salt.assign(SnmpUsm::PrivParameterLength, 0);
            data = SnmpBer::EncodeOctets(scoped);
         }
      }
      else
      {
         data = scoped;
      }

      // UsmSecurityParameters.
      SnmpBytes prefix = SnmpBer::EncodeOctets(settings_.engineId);
      SnmpBer::Append(prefix, SnmpBer::EncodeInteger(boots));
      SnmpBer::Append(prefix, SnmpBer::EncodeInteger(engineTime));
      SnmpBer::Append(prefix, SnmpBer::EncodeOctets(envelope.user));

      SnmpBytes usmContent = prefix;
      SnmpBer::Append(usmContent, SnmpBer::EncodeOctets(SnmpBytes(digest ? SnmpUsm::AuthParameterLength : 0, 0)));
      SnmpBer::Append(usmContent, SnmpBer::EncodeOctets(salt));

      SnmpBytes usm = SnmpBer::Wrap(SnmpTagSequence, usmContent);
      SnmpBytes securityParameters = SnmpBer::Wrap(SnmpTagOctetString, usm);

      // HeaderData.
      SnmpBytes flagOctet;
      flagOctet.push_back(flags);

      SnmpBytes headerContent = SnmpBer::EncodeInteger(envelope.messageId);
      SnmpBer::Append(headerContent, SnmpBer::EncodeInteger((long long) MaxMessageSize));
      SnmpBer::Append(headerContent, SnmpBer::EncodeOctets(flagOctet));
      SnmpBer::Append(headerContent, SnmpBer::EncodeInteger(SecurityModelUsm));
      SnmpBytes header = SnmpBer::Wrap(SnmpTagSequence, headerContent);

      SnmpBytes version = SnmpBer::EncodeInteger(3);

      SnmpBytes content = version;
      SnmpBer::Append(content, header);
      SnmpBer::Append(content, securityParameters);
      SnmpBer::Append(content, data);

      message = SnmpBer::Wrap(SnmpTagSequence, content);

      if (finalise && digest)
      {
         // Where the 24 octets of msgAuthenticationParameters sit: past the
         // message's header, the version, HeaderData, the OCTET STRING's and the
         // SEQUENCE's headers, the four fields before them, and their own tag and
         // length (24 is one length octet).
         const size_t offset = SnmpBer::HeaderLength(content.size()) + version.size() + header.size() +
                               SnmpBer::HeaderLength(usm.size()) + SnmpBer::HeaderLength(usmContent.size()) +
                               prefix.size() + 2;

         if (offset + SnmpUsm::AuthParameterLength > message.size())
            return false;

         unsigned char mac[SnmpUsm::AuthParameterLength];
         if (!SnmpUsm::Digest(settings_.authKey, &message[0], message.size(), mac))
            return false;

         memcpy(&message[offset], mac, SnmpUsm::AuthParameterLength);
      }

      return true;
   }
}
