// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The BER the SNMP agent reads and writes (X.690, as SNMP uses it: definite
// lengths, single-octet tags, and the handful of types RFC 3416 and RFC 2578
// name). See SnmpAgent.h for the agent and SnmpEngine.h for what it does with a
// message.
//
// THE READER IS THE ONLY CODE IN THE AGENT THAT TOUCHES BYTES FROM THE NETWORK,
// and it is written to one rule: nothing is read that has not first been proved
// to be inside the buffer. Every length is checked against what remains before
// a byte of the value is looked at; an indefinite length, a length of more than
// four octets, a tag of more than one octet, an INTEGER of no octets or more
// than eight, an OBJECT IDENTIFIER of more than 128 sub-identifiers or with a
// sub-identifier past 2^32-1, and a reader nested deeper than MaxDepth are each
// refused - the reader answers false and the caller drops the packet. There is
// no recursion anywhere: the agent reads the fixed shape of an SNMP message one
// level at a time, and a value inside a variable binding is never descended
// into, only measured and, for a Set, copied back.
//
// The writer builds each element whole and wraps it, which makes every length it
// writes the length of something it has already built.

#pragma once

#include <string>
#include <vector>

namespace HM
{
   typedef std::vector<unsigned char> SnmpBytes;
   typedef std::vector<unsigned int> SnmpOid;

   // The tags an SNMP message uses. Universal, then the SNMPv2-SMI application
   // types, then the three exception values of a variable binding, then the PDU
   // types of RFC 3416.
   const unsigned char SnmpTagInteger = 0x02;
   const unsigned char SnmpTagOctetString = 0x04;
   const unsigned char SnmpTagNull = 0x05;
   const unsigned char SnmpTagObjectIdentifier = 0x06;
   const unsigned char SnmpTagSequence = 0x30;
   const unsigned char SnmpTagIpAddress = 0x40;
   const unsigned char SnmpTagCounter32 = 0x41;
   const unsigned char SnmpTagGauge32 = 0x42;
   const unsigned char SnmpTagTimeTicks = 0x43;
   const unsigned char SnmpTagOpaque = 0x44;
   const unsigned char SnmpTagCounter64 = 0x46;
   const unsigned char SnmpTagNoSuchObject = 0x80;
   const unsigned char SnmpTagNoSuchInstance = 0x81;
   const unsigned char SnmpTagEndOfMibView = 0x82;
   const unsigned char SnmpTagGetRequest = 0xA0;
   const unsigned char SnmpTagGetNextRequest = 0xA1;
   const unsigned char SnmpTagResponse = 0xA2;
   const unsigned char SnmpTagSetRequest = 0xA3;
   const unsigned char SnmpTagTrapV1 = 0xA4;
   const unsigned char SnmpTagGetBulkRequest = 0xA5;
   const unsigned char SnmpTagInformRequest = 0xA6;
   const unsigned char SnmpTagTrapV2 = 0xA7;
   const unsigned char SnmpTagReport = 0xA8;

   // One element as it sits in the buffer. Every pointer points into the buffer
   // the reader was given; nothing is copied.
   struct SnmpTlv
   {
      SnmpTlv() :
         tag(0),
         start(nullptr),
         total(0),
         value(nullptr),
         length(0)
      {
      }

      unsigned char tag;

      // The tag octet, and the length of the whole element from there.
      const unsigned char *start;
      size_t total;

      // The contents octets.
      const unsigned char *value;
      size_t length;
   };

   class SnmpBerReader
   {
   public:
      // The deepest a message this agent reads is nested - the message, the
      // header or the security parameters, the USM sequence inside them, the
      // scoped PDU, the PDU, the binding list, one binding - with one to spare. A
      // reader asked to go deeper is refused, so no packet can make the decoder
      // walk further than the grammar does.
      static const int MaxDepth = 8;

      // An empty reader, for a child to be entered into.
      SnmpBerReader();

      // A reader over a whole buffer (depth 0), or over bytes that sit at a known
      // depth - the plaintext of a decrypted scoped PDU is read at the depth the
      // encrypted one sat at.
      SnmpBerReader(const unsigned char *data, size_t length);
      SnmpBerReader(const unsigned char *data, size_t length, int depth);

      // The next element, whatever its tag. False, with the reader unmoved, when
      // there is none or it does not fit what remains.
      bool Next(SnmpTlv &tlv);

      // The next element, which must carry this tag.
      bool Next(unsigned char expectedTag, SnmpTlv &tlv);

      // An INTEGER of one to eight octets, sign-extended.
      bool NextInteger(long long &value);

      // An OBJECT IDENTIFIER.
      bool NextOid(SnmpOid &oid);

      // A reader over an element's contents, one level deeper. False beyond
      // MaxDepth.
      bool Enter(const SnmpTlv &tlv, SnmpBerReader &child) const;

      bool AtEnd() const { return position_ == length_; }
      int GetDepth() const { return depth_; }

   private:
      const unsigned char *data_;
      size_t length_;
      size_t position_;
      int depth_;
   };

   class SnmpBer
   {
   public:
      // RFC 2578 section 3.5: at most 128 sub-identifiers, each at most 2^32-1.
      static const size_t MaxOidLength = 128;

      static bool DecodeInteger(const unsigned char *value, size_t length, long long &result);
      static bool DecodeOid(const unsigned char *value, size_t length, SnmpOid &oid);

      // The octets a tag and a length of this many content octets take.
      static size_t HeaderLength(size_t contentLength);

      static void AppendLength(SnmpBytes &out, size_t length);
      static void Append(SnmpBytes &out, const SnmpBytes &more);
      static void AppendTlv(SnmpBytes &out, unsigned char tag, const unsigned char *content, size_t length);

      static SnmpBytes Wrap(unsigned char tag, const SnmpBytes &content);
      static SnmpBytes EncodeInteger(long long value);
      static SnmpBytes EncodeUnsigned(unsigned char tag, unsigned long long value);
      static SnmpBytes EncodeOctets(const SnmpBytes &value);
      static SnmpBytes EncodeOctets(const std::string &value);
      static SnmpBytes EncodeOctets(const unsigned char *value, size_t length);
      static SnmpBytes EncodeNull(unsigned char tag);
      static SnmpBytes EncodeOid(const SnmpOid &oid);

      // Lexicographic order of the sub-identifiers, a prefix first: the order
      // GetNext walks. Negative, zero or positive.
      static int Compare(const SnmpOid &left, const SnmpOid &right);

      // Whether prefix is a leading part of oid (equal counts).
      static bool StartsWith(const SnmpOid &oid, const SnmpOid &prefix);

      // "1.3.6.1.2.1.1.3.0" and back; a leading dot is allowed. False for
      // anything else.
      static bool ParseOid(const std::string &dotted, SnmpOid &oid);
      static SnmpOid Oid(const char *dotted);
      static std::string OidToString(const SnmpOid &oid);

      static std::string ToHex(const SnmpBytes &bytes);
      static bool FromHex(const std::string &hex, SnmpBytes &bytes);
   };
}
