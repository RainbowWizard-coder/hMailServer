// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the SNMP agent does with one datagram, and nothing about how it got one:
// no socket, no thread, no setting and no server state. SnmpAgent (the listener)
// gives it the bytes of a datagram that came from an allowed address and sends
// whatever it answers; SnmpTester gives it bytes it made up, truncated, turned
// over and malformed, and reads the answers. See docs/Snmp.md.
//
// WHAT IT ANSWERS
//
//   SNMPv3 (RFC 3412, 3414), User-based Security Model, one user, authPriv
//   only: HMAC-SHA-256-192 and AES-128-CFB (SnmpUsm.h). Discovery is answered
//   the RFC 3414 section 4 way - an unknown engine ID with a Report carrying
//   usmStatsUnknownEngineIDs and this engine's ID, boots and time, and a request
//   outside the time window with an authenticated Report carrying
//   usmStatsNotInTimeWindows - and every other refusal the USM names is a
//   Report too: an unknown user, a security level below authPriv, a wrong
//   digest, a PDU that would not decrypt. A Report is sent only when the request
//   set the reportable flag.
//
//   SNMPv2c (RFC 3416 over RFC 1901's community), when SnmpV2cEnabled is on,
//   with the one community configured. A wrong community is not answered and is
//   counted in snmpInBadCommunityNames.
//
//   SNMPv1 is not answered: counted in snmpInBadVersions, as any other version.
//
//   Get, GetNext and GetBulk. Set is answered noAccess, with the bindings as
//   they came: the write view is empty. Nothing is ever sent unasked - there
//   are no notifications in this agent.
//
// THE BOUNDS, all of them here so that none is a surprise
//
//   A datagram longer than MaxMessageSize is not decoded. A response is never
//   longer than the smaller of MaxMessageSize and the request's msgMaxSize (v3),
//   and an SNMPv2c answer no longer than V2cAmplification times its request, or
//   MinimumMessageSize when that is more: a community is no proof of where a
//   datagram came from, so the most a forged source can be sent back is held
//   to a small multiple of what it sent (docs/Snmp.md says what this does not
//   cover for SNMPv3). One that would be longer is answered tooBig with no
//   bindings, or for GetBulk with
//   as many bindings as fit, and one whose tooBig would not fit either is
//   dropped and counted in snmpSilentDrops. A request of more than MaxVarbinds
//   bindings is answered tooBig. GetBulk's max-repetitions is held to
//   MaxRepetitions and the response to MaxResponseVarbinds bindings. A packet
//   that does not decode is dropped and counted in snmpInASNParseErrs; nothing
//   is logged for it, here or anywhere, so the agent cannot be made to write the
//   log.

#pragma once

#include "SnmpBer.h"

#include <string>
#include <vector>

namespace HM
{
   // One value as a variable binding carries it.
   struct SnmpValue
   {
      SnmpValue();

      static SnmpValue MakeInteger(long long value);

      // A Counter32 wraps: the value is taken modulo 2^32, which is what a
      // manager computing a rate from two readings expects of one.
      static SnmpValue MakeCounter32(unsigned long long value);

      // A Gauge32 latches at its maximum rather than wrapping (RFC 2578 7.1.7).
      static SnmpValue MakeGauge32(unsigned long long value);

      // Hundredths of a second, modulo 2^32.
      static SnmpValue MakeTimeTicks(unsigned long long hundredths);

      static SnmpValue MakeText(const std::string &text);
      static SnmpValue MakeOctets(const SnmpBytes &octets);
      static SnmpValue MakeOid(const SnmpOid &oid);

      // The value's own TLV.
      SnmpBytes Encode() const;

      unsigned char tag;
      long long integer;
      unsigned long long number;
      SnmpBytes octets;
      SnmpOid identifier;
   };

   // One managed object instance as the agent answers it now: its name, how
   // many of the name's last sub-identifiers are the instance - 1 for a scalar's
   // .0, the length of the index for a table column - and its value.
   struct SnmpBinding
   {
      SnmpBinding() :
         instanceLength(1)
      {
      }

      SnmpBinding(const SnmpOid &objectName, size_t objectInstanceLength, const SnmpValue &objectValue) :
         name(objectName),
         instanceLength(objectInstanceLength),
         value(objectValue)
      {
      }

      SnmpOid name;
      size_t instanceLength;
      SnmpValue value;
   };

   // Where the objects the server serves come from - the mail MIB and the
   // enterprise arc (SnmpAgent). Asked once for each request that got as far as
   // being answered, never for one that was refused.
   class SnmpMibSource
   {
   public:
      virtual ~SnmpMibSource() {}

      virtual void Collect(std::vector<SnmpBinding> &bindings) = 0;
   };

   // What the engine is, decided by SnmpAgent at start and fixed until it stops.
   struct SnmpEngineSettings
   {
      SnmpEngineSettings() :
         engineBoots(1),
         v2cEnabled(false)
      {
      }

      SnmpBytes engineId;

      // 1 to 2147483647. At 2147483647 the engine latches, as RFC 3414 2.2.2
      // says it must, and no authenticated request is ever in its time window.
      long long engineBoots;

      // The one SNMPv3 user, and its two keys localised to engineId. An empty
      // user is no user: every v3 request is answered usmStatsUnknownUserNames.
      std::string user;
      SnmpBytes authKey;
      SnmpBytes privKey;

      bool v2cEnabled;

      // Empty answers nobody, whatever v2cEnabled says.
      std::string community;

      // The system group (RFC 3418).
      std::string description;
      SnmpOid objectId;
      std::string contact;
      std::string name;
      std::string location;
   };

   // Every counter the engine keeps, each a Counter32 of its MIB: the snmp group
   // (RFC 3418), snmpMPDStats (RFC 3412), the two context counters of
   // SNMP-TARGET-MIB (RFC 3413) and usmStats (RFC 3414). Reset when the engine is
   // made, which is when the agent starts - the same moment sysUpTime counts from.
   struct SnmpCounters
   {
      SnmpCounters() :
         inPkts(0),
         inBadVersions(0),
         inBadCommunityNames(0),
         inBadCommunityUses(0),
         inAsnParseErrs(0),
         silentDrops(0),
         proxyDrops(0),
         unknownSecurityModels(0),
         invalidMsgs(0),
         unknownPduHandlers(0),
         unavailableContexts(0),
         unknownContexts(0),
         unsupportedSecLevels(0),
         notInTimeWindows(0),
         unknownUserNames(0),
         unknownEngineIds(0),
         wrongDigests(0),
         decryptionErrors(0)
      {
      }

      unsigned int inPkts;
      unsigned int inBadVersions;
      unsigned int inBadCommunityNames;
      unsigned int inBadCommunityUses;
      unsigned int inAsnParseErrs;
      unsigned int silentDrops;
      unsigned int proxyDrops;
      unsigned int unknownSecurityModels;
      unsigned int invalidMsgs;
      unsigned int unknownPduHandlers;
      unsigned int unavailableContexts;
      unsigned int unknownContexts;
      unsigned int unsupportedSecLevels;
      unsigned int notInTimeWindows;
      unsigned int unknownUserNames;
      unsigned int unknownEngineIds;
      unsigned int wrongDigests;
      unsigned int decryptionErrors;
   };

   class SnmpEngine
   {
   public:
      // This engine's snmpEngineMaxMessageSize: the largest datagram it decodes
      // and the largest it sends. RFC 3417 asks every engine to take 484; this
      // one takes 8192, which is a GetBulk of a useful size and still one read.
      static const size_t MaxMessageSize = 8192;

      // The smallest msgMaxSize RFC 3412 allows a request to name.
      static const long long MinimumMessageSize = 484;

      // The most bindings a request may carry before it is answered tooBig.
      static const size_t MaxVarbinds = 64;

      // GetBulk's max-repetitions is held to this, and a response to
      // MaxResponseVarbinds bindings whatever the product of the two.
      static const long long MaxRepetitions = 64;
      static const size_t MaxResponseVarbinds = 512;

      // RFC 3414 section 2.2.3: the time window, in seconds either side.
      static const long long TimeWindowSeconds = 150;

      static const long long MaxEngineBoots = 2147483647;

      // An SNMPv2c answer is held to this many times the octets of its request
      // (and never below MinimumMessageSize). UDP's source address is only what
      // the datagram says, and SNMPv2c has nothing but a community to check it
      // against, so this is what bounds what a forged request can have sent to
      // an allowed address.
      static const size_t V2cAmplification = 8;

      // The source may be null: the engine then answers its own objects only.
      SnmpEngine(const SnmpEngineSettings &settings, SnmpMibSource *source);
      ~SnmpEngine();

      // The engine's clock: milliseconds since it booted. The agent sets it
      // before each datagram; the tester sets it to where it wants it.
      void SetElapsedMilliseconds(unsigned long long elapsed);

      // snmpEngineTime, in seconds since the engine booted, capped at 2^31-1.
      long long GetEngineTime() const;

      // One datagram in. True with the bytes to send back; false to send
      // nothing. Never throws for anything in the datagram.
      bool Process(const unsigned char *data, size_t length, SnmpBytes &response);

      // A datagram the transport could not hand over whole because it was larger
      // than the buffer: counted as received and as a decoding error.
      void CountOversizedDatagram();

      const SnmpCounters &GetCounters() const { return counters_; }
      const SnmpEngineSettings &GetSettings() const { return settings_; }

   private:
      SnmpEngine(const SnmpEngine &);
      SnmpEngine &operator=(const SnmpEngine &);

      // One binding of a request: its name, and its value's TLV where it sits in
      // the (decrypted) request, which only a Set's answer reads.
      struct RequestBinding
      {
         RequestBinding() : value(nullptr), valueLength(0) {}

         SnmpOid name;
         const unsigned char *value;
         size_t valueLength;
      };

      struct PduRequest
      {
         PduRequest() : type(0), requestId(0), second(0), third(0), tooManyBindings(false) {}

         unsigned char type;
         long long requestId;
         long long second;       // error-status, or non-repeaters for GetBulk
         long long third;        // error-index, or max-repetitions for GetBulk
         std::vector<RequestBinding> bindings;
         bool tooManyBindings;
      };

      enum PduParse
      {
         PduParsed = 0,
         PduMalformed = 1,
         PduNotHandled = 2
      };

      // What wraps a PDU on the way out.
      struct Outgoing
      {
         Outgoing() : version(0), messageId(0), secure(false), authenticated(false), limit(MaxMessageSize) {}

         int version;              // 1 for SNMPv2c, 3 for SNMPv3
         std::string community;    // v2c
         long long messageId;      // v3
         std::string user;         // v3
         bool secure;              // v3 authPriv: an answer to a request
         bool authenticated;       // v3 authNoPriv: a Report sent at the level a request was verified at
         size_t limit;             // the most octets the whole message may take
      };

      bool ProcessV2c_(SnmpBerReader &body, size_t requestLength, SnmpBytes &response);
      bool ProcessV3_(const unsigned char *data, size_t length, SnmpBerReader &body, SnmpBytes &response);

      PduParse ParsePdu_(const SnmpBerReader &parent, const SnmpTlv &pdu, PduRequest &request);

      // Builds the answer to a request, within envelope.limit.
      bool Answer_(const PduRequest &request, const Outgoing &envelope, SnmpBytes &response);

      // A Report of one usmStats (or MPD, or context) counter.
      bool Report_(const Outgoing &envelope, bool reportable, long long requestId,
                   const char *counterOid, unsigned int counterValue, SnmpBytes &response);

      // The message around a PDU. With finalise false the digest and the
      // encryption are left out, which changes no length: used to measure.
      bool Wrap_(const Outgoing &envelope, const SnmpBytes &pdu, bool finalise, SnmpBytes &message);

      static SnmpBytes EncodePdu_(unsigned char tag, long long requestId, long long errorStatus, long long errorIndex,
                                  const std::vector<SnmpBytes> &bindings, size_t count);

      void CollectMib_(std::vector<SnmpBinding> &mib);
      void AddOwnObjects_(std::vector<SnmpBinding> &mib) const;

      SnmpEngineSettings settings_;
      SnmpMibSource *source_;
      SnmpCounters counters_;
      unsigned long long elapsed_;

      // RFC 3826 section 3.1.2.1: the 64-bit integer each outgoing encryption
      // takes as its salt, begun at a random value and moved on by one each time.
      unsigned long long salt_;

      // SHA-256 of the community, or empty for none. A community is compared by
      // its digest, so the comparison takes the same time whatever the length of
      // what was sent and whatever the length of what is configured.
      SnmpBytes community_digest_;
   };
}
