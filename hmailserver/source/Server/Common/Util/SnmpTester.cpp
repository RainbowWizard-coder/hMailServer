// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The SNMP agent's decoder and engine, tested from values. See SnmpTester.h.

#include "StdAfx.h"

#include "SnmpTester.h"
#include "SnmpBer.h"
#include "SnmpUsm.h"
#include "SnmpEngine.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Expect(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("SnmpTester: " + message);
      }

      SnmpBytes Hex(const char *hex)
      {
         SnmpBytes bytes;
         SnmpBer::FromHex(std::string(hex), bytes);
         return bytes;
      }

      SnmpBytes Text(const char *text)
      {
         return SnmpBytes(text, text + strlen(text));
      }

      const char *SysDescr = "1.3.6.1.2.1.1.1.0";
      const char *SysObjectId = "1.3.6.1.2.1.1.2.0";
      const char *SysUpTime = "1.3.6.1.2.1.1.3.0";
      const char *MtaTransmitted = "1.3.6.1.2.1.28.1.1.3.1";

      // ---- the manager's side: building requests and reading answers

      SnmpBytes Pdu(unsigned char tag, long long requestId, long long second, long long third, const std::vector<SnmpOid> &names)
      {
         SnmpBytes list;

         for (const SnmpOid &name : names)
         {
            SnmpBytes binding = SnmpBer::EncodeOid(name);
            SnmpBer::Append(binding, SnmpBer::EncodeNull(SnmpTagNull));
            SnmpBer::Append(list, SnmpBer::Wrap(SnmpTagSequence, binding));
         }

         SnmpBytes content = SnmpBer::EncodeInteger(requestId);
         SnmpBer::Append(content, SnmpBer::EncodeInteger(second));
         SnmpBer::Append(content, SnmpBer::EncodeInteger(third));
         SnmpBer::Append(content, SnmpBer::Wrap(SnmpTagSequence, list));

         return SnmpBer::Wrap(tag, content);
      }

      std::vector<SnmpOid> Names(const char *first, const char *second = nullptr)
      {
         std::vector<SnmpOid> names;
         names.push_back(SnmpBer::Oid(first));

         if (second)
            names.push_back(SnmpBer::Oid(second));

         return names;
      }

      SnmpBytes V2cMessage(long long version, const std::string &community, const SnmpBytes &pdu)
      {
         SnmpBytes content = SnmpBer::EncodeInteger(version);
         SnmpBer::Append(content, SnmpBer::EncodeOctets(community));
         SnmpBer::Append(content, pdu);
         return SnmpBer::Wrap(SnmpTagSequence, content);
      }

      // What a manager knows of the engine it is talking to.
      struct Manager
      {
         Manager() : boots(0), engineTime(0) {}

         SnmpBytes engineId;
         long long boots;
         long long engineTime;
         std::string user;
         SnmpBytes authKey;
         SnmpBytes privKey;
      };

      // Where msgAuthenticationParameters sits in a message, found by reading the
      // message back - a second route to the octets the engine finds by
      // arithmetic.
      bool FindAuthParameters(const SnmpBytes &message, size_t &offset, size_t &length)
      {
         SnmpBerReader top(&message[0], message.size());
         SnmpTlv whole, header, security, usm, engineId, user, auth, priv;
         SnmpBerReader body, securityReader, usmReader;
         long long version = 0, boots = 0, engineTime = 0;

         if (!top.Next(SnmpTagSequence, whole) || !top.Enter(whole, body) ||
             !body.NextInteger(version) || !body.Next(SnmpTagSequence, header) ||
             !body.Next(SnmpTagOctetString, security) || !body.Enter(security, securityReader) ||
             !securityReader.Next(SnmpTagSequence, usm) || !securityReader.Enter(usm, usmReader) ||
             !usmReader.Next(SnmpTagOctetString, engineId) || !usmReader.NextInteger(boots) ||
             !usmReader.NextInteger(engineTime) || !usmReader.Next(SnmpTagOctetString, user) ||
             !usmReader.Next(SnmpTagOctetString, auth) || !usmReader.Next(SnmpTagOctetString, priv))
         {
            return false;
         }

         offset = (size_t) (auth.value - &message[0]);
         length = auth.length;
         return true;
      }

      SnmpBytes ScopedPdu(const SnmpBytes &contextEngineId, const std::string &contextName, const SnmpBytes &pdu)
      {
         SnmpBytes scopedContent = SnmpBer::EncodeOctets(contextEngineId);
         SnmpBer::Append(scopedContent, SnmpBer::EncodeOctets(contextName));
         SnmpBer::Append(scopedContent, pdu);
         return SnmpBer::Wrap(SnmpTagSequence, scopedContent);
      }

      // A message around whatever scoped PDU it is given - one the test has cut
      // short or turned over included - encrypted and signed with the manager's
      // keys, so that it passes the digest and the decryption and the engine has
      // to find the fault in the plaintext.
      SnmpBytes V3MessageFromScoped(const Manager &manager, long long messageId, long long maxSize, unsigned char flags,
                                    const SnmpBytes &scoped, unsigned long long salt)
      {
         SnmpBytes privParameters;
         SnmpBytes data;

         if (flags & 0x02)
         {
            for (int i = 7; i >= 0; i--)
               privParameters.push_back((unsigned char) ((salt >> (8 * i)) & 0xFF));

            unsigned char iv[16];
            SnmpUsm::MakeIv((unsigned int) manager.boots, (unsigned int) manager.engineTime, &privParameters[0], iv);

            SnmpBytes encrypted;
            Expect(SnmpUsm::Encrypt(manager.privKey, iv, &scoped[0], scoped.size(), encrypted), "the manager could not encrypt its request.");
            data = SnmpBer::EncodeOctets(encrypted);
         }
         else
         {
            data = scoped;
         }

         SnmpBytes usmContent = SnmpBer::EncodeOctets(manager.engineId);
         SnmpBer::Append(usmContent, SnmpBer::EncodeInteger(manager.boots));
         SnmpBer::Append(usmContent, SnmpBer::EncodeInteger(manager.engineTime));
         SnmpBer::Append(usmContent, SnmpBer::EncodeOctets(manager.user));
         SnmpBer::Append(usmContent, SnmpBer::EncodeOctets(SnmpBytes((flags & 0x01) ? SnmpUsm::AuthParameterLength : 0, 0)));
         SnmpBer::Append(usmContent, SnmpBer::EncodeOctets(privParameters));

         SnmpBytes flagOctet(1, flags);

         SnmpBytes headerContent = SnmpBer::EncodeInteger(messageId);
         SnmpBer::Append(headerContent, SnmpBer::EncodeInteger(maxSize));
         SnmpBer::Append(headerContent, SnmpBer::EncodeOctets(flagOctet));
         SnmpBer::Append(headerContent, SnmpBer::EncodeInteger(3));

         SnmpBytes content = SnmpBer::EncodeInteger(3);
         SnmpBer::Append(content, SnmpBer::Wrap(SnmpTagSequence, headerContent));
         SnmpBer::Append(content, SnmpBer::EncodeOctets(SnmpBer::Wrap(SnmpTagSequence, usmContent)));
         SnmpBer::Append(content, data);

         SnmpBytes message = SnmpBer::Wrap(SnmpTagSequence, content);

         if (flags & 0x01)
         {
            size_t offset = 0;
            size_t length = 0;
            Expect(FindAuthParameters(message, offset, length) && length == SnmpUsm::AuthParameterLength,
                   "the manager could not find its own authentication parameters.");

            unsigned char mac[SnmpUsm::AuthParameterLength];
            Expect(SnmpUsm::Digest(manager.authKey, &message[0], message.size(), mac), "the manager could not sign its request.");
            memcpy(&message[offset], mac, SnmpUsm::AuthParameterLength);
         }

         return message;
      }

      SnmpBytes V3Message(const Manager &manager, long long messageId, long long maxSize, unsigned char flags,
                          const SnmpBytes &pdu, unsigned long long salt)
      {
         return V3MessageFromScoped(manager, messageId, maxSize, flags, ScopedPdu(manager.engineId, std::string(), pdu), salt);
      }

      // An answer, read.
      struct SnmpTestAnswer
      {
         SnmpTestAnswer() : version(-1), flags(0), boots(-1), engineTime(-1), pduTag(0), requestId(0), errorStatus(-1), errorIndex(-1) {}

         long long version;
         unsigned char flags;
         SnmpBytes engineId;
         long long boots;
         long long engineTime;
         std::string user;
         std::string community;
         unsigned char pduTag;
         long long requestId;
         long long errorStatus;
         long long errorIndex;
         std::vector<SnmpOid> names;
         std::vector<unsigned char> tags;
         std::vector<SnmpBytes> values;

         // msgPrivacyParameters: the salt the answer was encrypted under.
         SnmpBytes privParameters;
      };

      bool ReadPdu(const SnmpBerReader &parent, const SnmpTlv &pdu, SnmpTestAnswer &answer)
      {
         SnmpBerReader pduReader;
         SnmpBerReader listReader;
         SnmpTlv list;

         answer.pduTag = pdu.tag;

         if (!parent.Enter(pdu, pduReader) ||
             !pduReader.NextInteger(answer.requestId) ||
             !pduReader.NextInteger(answer.errorStatus) ||
             !pduReader.NextInteger(answer.errorIndex) ||
             !pduReader.Next(SnmpTagSequence, list) ||
             !pduReader.AtEnd() ||
             !pduReader.Enter(list, listReader))
         {
            return false;
         }

         while (!listReader.AtEnd())
         {
            SnmpTlv binding;
            SnmpBerReader bindingReader;
            SnmpOid name;
            SnmpTlv value;

            if (!listReader.Next(SnmpTagSequence, binding) ||
                !listReader.Enter(binding, bindingReader) ||
                !bindingReader.NextOid(name) ||
                !bindingReader.Next(value) ||
                !bindingReader.AtEnd())
            {
               return false;
            }

            answer.names.push_back(name);
            answer.tags.push_back(value.tag);
            answer.values.push_back(SnmpBytes(value.value, value.value + value.length));
         }

         return true;
      }

      bool DecodeV2c(const SnmpBytes &message, SnmpTestAnswer &answer)
      {
         SnmpBerReader top(&message[0], message.size());
         SnmpBerReader body;
         SnmpTlv whole, community, pdu;

         if (!top.Next(SnmpTagSequence, whole) || !top.AtEnd() || !top.Enter(whole, body) ||
             !body.NextInteger(answer.version) || !body.Next(SnmpTagOctetString, community) ||
             !body.Next(pdu) || !body.AtEnd())
         {
            return false;
         }

         answer.community.assign((const char *) community.value, community.length);
         return ReadPdu(body, pdu, answer);
      }

      bool ReadScoped(const SnmpBerReader &parent, const SnmpTlv &scoped, SnmpTestAnswer &answer)
      {
         SnmpBerReader scopedReader;
         SnmpTlv contextEngineId, contextName, pdu;

         if (!parent.Enter(scoped, scopedReader) ||
             !scopedReader.Next(SnmpTagOctetString, contextEngineId) ||
             !scopedReader.Next(SnmpTagOctetString, contextName) ||
             !scopedReader.Next(pdu) ||
             !scopedReader.AtEnd())
         {
            return false;
         }

         return ReadPdu(scopedReader, pdu, answer);
      }

      // Reads an SNMPv3 answer the way a manager would: the digest checked with
      // the manager's key when the answer says it is authenticated, the scoped
      // PDU decrypted with the manager's key when it says it is private.
      bool DecodeV3(const SnmpBytes &message, const Manager &manager, SnmpTestAnswer &answer)
      {
         if (message.empty())
            return false;

         SnmpBerReader top(&message[0], message.size());
         SnmpBerReader body, headerReader, securityReader, usmReader;
         SnmpTlv whole, header, security, data, flags, usm, engineId, user, auth, priv;
         long long messageId = 0, maxSize = 0, model = 0;

         if (!top.Next(SnmpTagSequence, whole) || !top.AtEnd() || !top.Enter(whole, body) ||
             !body.NextInteger(answer.version) || answer.version != 3 ||
             !body.Next(SnmpTagSequence, header) || !body.Next(SnmpTagOctetString, security) ||
             !body.Next(data) || !body.AtEnd() ||
             !body.Enter(header, headerReader) || !headerReader.NextInteger(messageId) ||
             !headerReader.NextInteger(maxSize) || !headerReader.Next(SnmpTagOctetString, flags) ||
             !headerReader.NextInteger(model) || flags.length != 1 ||
             !body.Enter(security, securityReader) || !securityReader.Next(SnmpTagSequence, usm) ||
             !securityReader.Enter(usm, usmReader) || !usmReader.Next(SnmpTagOctetString, engineId) ||
             !usmReader.NextInteger(answer.boots) || !usmReader.NextInteger(answer.engineTime) ||
             !usmReader.Next(SnmpTagOctetString, user) || !usmReader.Next(SnmpTagOctetString, auth) ||
             !usmReader.Next(SnmpTagOctetString, priv))
         {
            return false;
         }

         answer.flags = flags.value[0];
         answer.engineId.assign(engineId.value, engineId.value + engineId.length);
         answer.user.assign((const char *) user.value, user.length);
         answer.privParameters.assign(priv.value, priv.value + priv.length);

         if (answer.flags & 0x01)
         {
            if (auth.length != SnmpUsm::AuthParameterLength)
               return false;

            SnmpBytes copy = message;
            const size_t offset = (size_t) (auth.value - &message[0]);
            memset(&copy[offset], 0, SnmpUsm::AuthParameterLength);

            unsigned char mac[SnmpUsm::AuthParameterLength];
            if (!SnmpUsm::Digest(manager.authKey, &copy[0], copy.size(), mac) ||
                !SnmpUsm::DigestsEqual(mac, auth.value, SnmpUsm::AuthParameterLength))
            {
               return false;
            }
         }

         if (answer.flags & 0x02)
         {
            if (data.tag != SnmpTagOctetString || priv.length != SnmpUsm::PrivParameterLength)
               return false;

            unsigned char iv[16];
            SnmpUsm::MakeIv((unsigned int) answer.boots, (unsigned int) answer.engineTime, priv.value, iv);

            SnmpBytes plain;
            if (!SnmpUsm::Decrypt(manager.privKey, iv, data.value, data.length, plain) || plain.empty())
               return false;

            SnmpBerReader plainReader(&plain[0], plain.size());
            SnmpTlv scoped;
            if (!plainReader.Next(SnmpTagSequence, scoped))
               return false;

            return ReadScoped(plainReader, scoped, answer);
         }

         if (data.tag != SnmpTagSequence)
            return false;

         return ReadScoped(body, data, answer);
      }

      unsigned long long Number(const SnmpBytes &value)
      {
         unsigned long long result = 0;
         for (size_t i = 0; i < value.size(); i++)
            result = (result << 8) | value[i];
         return result;
      }

      // The most an SNMPv2c answer to a request of this many octets may take.
      size_t V2cCap(size_t requestLength)
      {
         size_t cap = requestLength * SnmpEngine::V2cAmplification;
         if (cap < (size_t) SnmpEngine::MinimumMessageSize)
            cap = (size_t) SnmpEngine::MinimumMessageSize;
         if (cap > SnmpEngine::MaxMessageSize)
            cap = SnmpEngine::MaxMessageSize;
         return cap;
      }

      // ---- a message as a tree, so that one element can be cut short and every
      // length around it written again to fit: the decoder then meets the fault
      // inside true outer lengths, where a cut of the whole datagram always
      // fails at the first one.

      struct SnmpTestElement
      {
         SnmpTestElement() : tag(0), nested(false) {}

         unsigned char tag;
         bool nested;                               // contents are the children
         SnmpBytes contents;                        // a primitive's contents
         std::vector<SnmpTestElement> children;
      };

      SnmpBytes ElementContents(const SnmpTestElement &element);

      SnmpBytes SerialiseElement(const SnmpTestElement &element)
      {
         return SnmpBer::Wrap(element.tag, ElementContents(element));
      }

      SnmpBytes ElementContents(const SnmpTestElement &element)
      {
         if (!element.nested)
            return element.contents;

         SnmpBytes contents;
         for (const SnmpTestElement &child : element.children)
            SnmpBer::Append(contents, SerialiseElement(child));
         return contents;
      }

      SnmpBytes SerialiseElements(const std::vector<SnmpTestElement> &elements)
      {
         SnmpBytes out;
         for (const SnmpTestElement &element : elements)
            SnmpBer::Append(out, SerialiseElement(element));
         return out;
      }

      // The test's own reading, bounded by depth; not the engine's. A
      // constructed element is read into its children, and so is an OCTET
      // STRING whose contents are exactly one SEQUENCE - the USM security
      // parameters - when reading them back gives the same octets.
      bool ParseElements(const unsigned char *data, size_t length, std::vector<SnmpTestElement> &elements, int depth)
      {
         elements.clear();

         if (depth > 12)
            return false;

         SnmpBerReader reader(data, length);

         while (!reader.AtEnd())
         {
            SnmpTlv tlv;
            if (!reader.Next(tlv))
               return false;

            SnmpTestElement element;
            element.tag = tlv.tag;

            const bool constructed = (tlv.tag & 0x20) != 0;
            const bool wrapsSequence = tlv.tag == SnmpTagOctetString && tlv.length > 0 && tlv.value[0] == SnmpTagSequence;

            std::vector<SnmpTestElement> children;
            const bool readable = (constructed || wrapsSequence) &&
                                  ParseElements(tlv.value, tlv.length, children, depth + 1) &&
                                  (constructed || (children.size() == 1 && children[0].tag == SnmpTagSequence)) &&
                                  SerialiseElements(children) == SnmpBytes(tlv.value, tlv.value + tlv.length);

            if (readable)
            {
               element.nested = true;
               element.children = children;
            }
            else
            {
               element.contents.assign(tlv.value, tlv.value + tlv.length);
            }

            elements.push_back(element);
         }

         return true;
      }

      void CollectPaths(const std::vector<SnmpTestElement> &elements, std::vector<size_t> &prefix,
                        std::vector<std::vector<size_t> > &paths)
      {
         for (size_t i = 0; i < elements.size(); i++)
         {
            prefix.push_back(i);
            paths.push_back(prefix);

            if (elements[i].nested)
               CollectPaths(elements[i].children, prefix, paths);

            prefix.pop_back();
         }
      }

      SnmpTestElement &ElementAt(std::vector<SnmpTestElement> &elements, const std::vector<size_t> &path)
      {
         SnmpTestElement *element = &elements[path[0]];
         for (size_t i = 1; i < path.size(); i++)
            element = &element->children[path[i]];
         return *element;
      }

      // One cut of one element: the message it makes, how deep the element sits
      // (0 for the outermost), and whether the decoder has to refuse it. It has
      // to when the cut leaves a child of a constructed element unfinished, and
      // when it leaves an INTEGER or an OBJECT IDENTIFIER with no contents at
      // all. A cut that ends exactly after a child, or that shortens a string,
      // can make a message that is still well formed, and is only held to
      // being answered readably.
      struct SnmpTestCut
      {
         SnmpTestCut() : depth(0), mustBeRefused(false) {}

         SnmpBytes message;
         size_t depth;
         bool mustBeRefused;
      };

      // Every element of the message cut short at every length below its own,
      // with every length around it written again to fit. deepest is set to how
      // deep the deepest constructed element sits, so that a caller can require
      // a refusal at every level down to it.
      std::vector<SnmpTestCut> InnerTruncations(const SnmpBytes &message, size_t &deepest)
      {
         deepest = 0;

         std::vector<SnmpTestElement> tree;
         Expect(ParseElements(&message[0], message.size(), tree, 0), "a message the test built could not be read as a tree.");
         Expect(SerialiseElements(tree) == message, "a message the test built did not read back as itself.");

         std::vector<size_t> prefix;
         std::vector<std::vector<size_t> > paths;
         CollectPaths(tree, prefix, paths);

         std::vector<SnmpTestCut> cuts;

         for (const std::vector<size_t> &path : paths)
         {
            const SnmpTestElement original = ElementAt(tree, path);
            const SnmpBytes contents = ElementContents(original);
            const size_t depth = path.size() - 1;

            // The lengths at which a constructed element's contents end exactly
            // after one of its children.
            std::vector<size_t> boundaries(1, 0);
            if (original.nested)
            {
               size_t offset = 0;
               for (const SnmpTestElement &child : original.children)
               {
                  offset += SerialiseElement(child).size();
                  boundaries.push_back(offset);
               }

               if (depth > deepest)
                  deepest = depth;
            }

            for (size_t cut = 0; cut < contents.size(); cut++)
            {
               std::vector<SnmpTestElement> copy = tree;
               SnmpTestElement &target = ElementAt(copy, path);
               target.nested = false;
               target.children.clear();
               target.contents.assign(contents.begin(), contents.begin() + (std::ptrdiff_t) cut);

               bool onBoundary = false;
               for (size_t boundary : boundaries)
               {
                  if (boundary == cut)
                     onBoundary = true;
               }

               SnmpTestCut made;
               made.message = SerialiseElements(copy);
               made.depth = depth;
               made.mustBeRefused = original.nested ? !onBoundary :
                  (cut == 0 && (original.tag == SnmpTagInteger || original.tag == SnmpTagObjectIdentifier));
               cuts.push_back(made);
            }
         }

         return cuts;
      }

      // A cut the decoder has to refuse is not answered and moves
      // snmpInASNParseErrs by exactly one: refused by the decoder's own check at
      // the level cut, not answered with a Report, not counted as anything else.
      // Each such refusal is tallied by depth.
      void ExpectRefusedWhereItMustBe(const SnmpTestCut &cut, bool answered, unsigned int parseErrorsBefore, unsigned int parseErrorsAfter,
                                      std::vector<size_t> &refusedAtDepth, const std::string &what)
      {
         if (!cut.mustBeRefused)
            return;

         Expect(!answered && parseErrorsAfter == parseErrorsBefore + 1,
                what + " leaves an element unfinished at depth " + std::to_string(cut.depth) + " and was not refused by the decoder and counted once.");

         if (refusedAtDepth.size() <= cut.depth)
            refusedAtDepth.resize(cut.depth + 1, 0);

         refusedAtDepth[cut.depth]++;
      }

      // Every level from the outermost down to the deepest constructed element
      // had cuts that the decoder refused - so a check missing at any one level
      // is a cut at that level that is not refused, and fails the test above.
      void ExpectRefusalsAtEveryDepth(const std::vector<size_t> &refusedAtDepth, size_t deepest, const std::string &what)
      {
         for (size_t depth = 0; depth <= deepest; depth++)
         {
            Expect(depth < refusedAtDepth.size() && refusedAtDepth[depth] > 0,
                   what + ": no cut at depth " + std::to_string(depth) + " was one the decoder had to refuse.");
         }
      }

      // ---- the engine under test

      class FakeMib : public SnmpMibSource
      {
      public:
         FakeMib() : transmitted(7) {}

         virtual void Collect(std::vector<SnmpBinding> &bindings)
         {
            bindings.push_back(SnmpBinding(SnmpBer::Oid("1.3.6.1.2.1.28.1.1.2.1"), 1, SnmpValue::MakeGauge32(3)));
            bindings.push_back(SnmpBinding(SnmpBer::Oid(MtaTransmitted), 1, SnmpValue::MakeCounter32(transmitted)));
            bindings.push_back(SnmpBinding(SnmpBer::Oid("1.3.6.1.2.1.28.2.1.13.1.1"), 2, SnmpValue::MakeGauge32(2)));
            bindings.push_back(SnmpBinding(SnmpBer::Oid("1.3.6.1.2.1.28.2.1.25.1.1"), 2, SnmpValue::MakeText("SMTP")));
            bindings.push_back(SnmpBinding(SnmpBer::Oid("1.3.6.1.2.1.28.2.1.25.1.3"), 2, SnmpValue::MakeText("IMAP")));
         }

         unsigned int transmitted;
      };

      // The objects the engine keeps itself: system 8, snmp 8, snmpEngine 4,
      // snmpMPDStats 3, the two context counters and usmStats 6.
      const int OwnObjects = 31;
      const int FakeObjects = 5;

      SnmpEngineSettings MakeSettings()
      {
         SnmpEngineSettings settings;
         settings.engineId = Hex("80007ed905000102030405060708090a0b");
         settings.engineBoots = 5;
         settings.user = "monitor";

         Expect(SnmpUsm::LocalisedKey("auth-password-1", settings.engineId, settings.authKey), "the authentication key could not be made.");

         SnmpBytes privFull;
         Expect(SnmpUsm::LocalisedKey("priv-password-1", settings.engineId, privFull), "the privacy key could not be made.");
         settings.privKey.assign(privFull.begin(), privFull.begin() + (std::ptrdiff_t) SnmpUsm::PrivKeyLength);

         settings.v2cEnabled = false;
         settings.community = "s3cret-community";
         settings.description = "hMailServer under test";
         settings.objectId = SnmpBer::Oid("1.3.6.1.4.1.32473.1.1");
         settings.contact = "postmaster@example.com";
         settings.name = "mail.example.com";
         settings.location = "Rack 4";
         return settings;
      }

      Manager Knowing(const SnmpEngineSettings &settings, long long engineTime)
      {
         Manager manager;
         manager.engineId = settings.engineId;
         manager.boots = settings.engineBoots;
         manager.engineTime = engineTime;
         manager.user = settings.user;
         manager.authKey = settings.authKey;
         manager.privKey = settings.privKey;
         return manager;
      }

      bool Run(SnmpEngine &engine, const SnmpBytes &message, SnmpBytes &response)
      {
         return engine.Process(message.empty() ? nullptr : &message[0], message.size(), response);
      }

      void ExpectReport(const SnmpBytes &response, const Manager &manager, const char *counter, const std::string &what)
      {
         SnmpTestAnswer answer;
         Expect(DecodeV3(response, manager, answer), what + ": the answer could not be read.");
         Expect(answer.pduTag == SnmpTagReport, what + ": the answer is not a Report.");
         Expect(answer.names.size() == 1 && SnmpBer::Compare(answer.names[0], SnmpBer::Oid(counter)) == 0,
                what + ": the Report does not carry " + counter + ".");
         Expect(answer.tags[0] == SnmpTagCounter32, what + ": the Report's counter is not a Counter32.");
      }
   }

   void
   SnmpTester::Test()
   {
      TestBer_();
      TestReaderBounds_();
      TestCryptography_();
      TestV2c_();
      TestV3_();
      TestWalkAndBulk_();
      TestMalformed_();
      TestInnerTruncations_();
   }

   void
   SnmpTester::TestBer_()
   {
      struct IntegerCase
      {
         long long value;
         const char *hex;
      };

      const IntegerCase integers[] =
      {
         { 0, "020100" },
         { 127, "02017f" },
         { 128, "02020080" },
         { 255, "020200ff" },
         { 256, "02020100" },
         { -1, "0201ff" },
         { -128, "020180" },
         { -129, "0202ff7f" },
         { 2147483647LL, "02047fffffff" },
         { -2147483648LL, "020480000000" },
      };

      for (const IntegerCase &item : integers)
      {
         const SnmpBytes encoded = SnmpBer::EncodeInteger(item.value);
         Expect(SnmpBer::ToHex(encoded) == item.hex, std::string("INTEGER ") + std::to_string(item.value) + " was not encoded as " + item.hex + ".");

         long long decoded = 0;
         Expect(SnmpBer::DecodeInteger(&encoded[2], encoded.size() - 2, decoded) && decoded == item.value,
                std::string("INTEGER ") + item.hex + " did not decode back.");
      }

      Expect(SnmpBer::ToHex(SnmpBer::EncodeUnsigned(SnmpTagCounter32, 0xFFFFFFFFULL)) == "410500ffffffff", "Counter32 2^32-1 was not given its leading zero.");
      Expect(SnmpBer::ToHex(SnmpBer::EncodeUnsigned(SnmpTagGauge32, 0)) == "420100", "Gauge32 0 was not one octet.");
      Expect(SnmpBer::ToHex(SnmpBer::EncodeUnsigned(SnmpTagCounter64, 0xFFFFFFFFFFFFFFFFULL)) == "460900ffffffffffffffff", "Counter64 2^64-1 was not nine octets.");
      Expect(SnmpBer::ToHex(SnmpBer::EncodeUnsigned(SnmpTagTimeTicks, 0x80)) == "43020080", "TimeTicks 128 was not given its leading zero.");

      Expect(SnmpBer::ToHex(SnmpBer::EncodeOid(SnmpBer::Oid(SysUpTime))) == "06082b06010201010300", "sysUpTime.0 was not encoded as RFC 3418 has it.");
      Expect(SnmpBer::ToHex(SnmpBer::EncodeOid(SnmpBer::Oid("2.999.3"))) == "0603883703", "2.999.3 was not encoded with its first arcs combined.");

      const SnmpOid wide = SnmpBer::Oid("1.3.6.1.4294967295.0");
      const SnmpBytes wideEncoded = SnmpBer::EncodeOid(wide);
      SnmpOid wideDecoded;
      Expect(SnmpBer::DecodeOid(&wideEncoded[2], wideEncoded.size() - 2, wideDecoded) && SnmpBer::Compare(wide, wideDecoded) == 0,
             "A sub-identifier of 2^32-1 did not survive the round trip.");

      SnmpOid roundTrip;
      const SnmpBytes rootTwo = SnmpBer::EncodeOid(SnmpBer::Oid("2.999.3"));
      Expect(SnmpBer::DecodeOid(&rootTwo[2], rootTwo.size() - 2, roundTrip) && SnmpBer::OidToString(roundTrip) == "2.999.3",
             "2.999.3 did not decode back.");

      Expect(SnmpBer::HeaderLength(127) == 2 && SnmpBer::HeaderLength(128) == 3 &&
             SnmpBer::HeaderLength(255) == 3 && SnmpBer::HeaderLength(256) == 4,
             "The header lengths are not the short and long forms.");

      Expect(SnmpBer::ToHex(SnmpBer::Wrap(SnmpTagOctetString, SnmpBytes(200, 'a'))).substr(0, 6) == "0481c8", "A length of 200 was not 81 c8.");
      Expect(SnmpBer::ToHex(SnmpBer::Wrap(SnmpTagOctetString, SnmpBytes(300, 'a'))).substr(0, 8) == "0482012c", "A length of 300 was not 82 01 2c.");

      Expect(SnmpBer::Compare(SnmpBer::Oid("1.3.6"), SnmpBer::Oid("1.3.6.1")) < 0 &&
             SnmpBer::Compare(SnmpBer::Oid("1.3.6.1"), SnmpBer::Oid("1.3.7")) < 0 &&
             SnmpBer::Compare(SnmpBer::Oid("1.3.6.1"), SnmpBer::Oid("1.3.6.1")) == 0 &&
             SnmpBer::Compare(SnmpBer::Oid("1.3.6.10"), SnmpBer::Oid("1.3.6.9")) > 0,
             "OIDs are not in GetNext's order.");

      SnmpOid refused;
      Expect(!SnmpBer::ParseOid("", refused) && !SnmpBer::ParseOid("1", refused) && !SnmpBer::ParseOid("1..3", refused) &&
             !SnmpBer::ParseOid("1.3.", refused) && !SnmpBer::ParseOid("a.b", refused) &&
             !SnmpBer::ParseOid("1.3.4294967296", refused),
             "A dotted OID that is not one was accepted.");
   }

   void
   SnmpTester::TestReaderBounds_()
   {
      long long value = 0;
      const unsigned char nineOctets[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
      Expect(!SnmpBer::DecodeInteger(nineOctets, 0, value), "An INTEGER of no octets was accepted.");
      Expect(!SnmpBer::DecodeInteger(nineOctets, 9, value), "An INTEGER of nine octets was accepted.");

      SnmpOid oid;
      const unsigned char unfinished[] = { 0x2B, 0x86 };
      Expect(!SnmpBer::DecodeOid(unfinished, sizeof(unfinished), oid), "An OID whose last sub-identifier never ended was accepted.");

      const unsigned char tooWide[] = { 0x2B, 0x90, 0x80, 0x80, 0x80, 0x00 };
      Expect(!SnmpBer::DecodeOid(tooWide, sizeof(tooWide), oid), "A sub-identifier of 2^32 was accepted.");

      const unsigned char sixOctets[] = { 0x2B, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00 };
      Expect(!SnmpBer::DecodeOid(sixOctets, sizeof(sixOctets), oid), "A sub-identifier of six octets was accepted.");

      SnmpBytes longest(1, 0x2B);
      longest.resize(127, 0x00);
      Expect(SnmpBer::DecodeOid(&longest[0], longest.size(), oid) && oid.size() == SnmpBer::MaxOidLength, "An OID of 128 sub-identifiers was refused.");

      longest.push_back(0x00);
      Expect(!SnmpBer::DecodeOid(&longest[0], longest.size(), oid), "An OID of 129 sub-identifiers was accepted.");

      SnmpTlv tlv;

      const unsigned char pastTheEnd[] = { 0x04, 0x05, 'a', 'b' };
      SnmpBerReader pastReader(pastTheEnd, sizeof(pastTheEnd));
      Expect(!pastReader.Next(tlv), "A length past the end of the buffer was read.");

      const unsigned char loneTag[] = { 0x04 };
      SnmpBerReader loneReader(loneTag, sizeof(loneTag));
      Expect(!loneReader.Next(tlv), "A tag with no length was read.");

      const unsigned char missingLengthOctets[] = { 0x04, 0x82, 0x01 };
      SnmpBerReader missingReader(missingLengthOctets, sizeof(missingLengthOctets));
      Expect(!missingReader.Next(tlv), "A long-form length missing its octets was read.");

      const unsigned char indefinite[] = { 0x30, 0x80, 0x00, 0x00 };
      SnmpBerReader indefiniteReader(indefinite, sizeof(indefinite));
      Expect(!indefiniteReader.Next(tlv), "An indefinite length was read.");

      const unsigned char fiveLengthOctets[] = { 0x04, 0x85, 0x00, 0x00, 0x00, 0x00, 0x01, 'a' };
      SnmpBerReader fiveReader(fiveLengthOctets, sizeof(fiveLengthOctets));
      Expect(!fiveReader.Next(tlv), "A length of five octets was read.");

      const unsigned char longTag[] = { 0x1F, 0x81, 0x01, 0x00 };
      SnmpBerReader longTagReader(longTag, sizeof(longTag));
      Expect(!longTagReader.Next(tlv), "A tag of more than one octet was read.");

      const unsigned char wrongTag[] = { 0x02, 0x01, 0x05 };
      SnmpBerReader wrongTagReader(wrongTag, sizeof(wrongTag));
      Expect(!wrongTagReader.Next(SnmpTagOctetString, tlv), "An INTEGER was read as an OCTET STRING.");
      Expect(wrongTagReader.NextInteger(value) && value == 5, "A refused read moved the reader.");

      // Nested one level deeper than MaxDepth allows: the reader stops at MaxDepth.
      SnmpBytes nested = SnmpBer::EncodeNull(SnmpTagNull);
      for (int i = 0; i < SnmpBerReader::MaxDepth + 4; i++)
         nested = SnmpBer::Wrap(SnmpTagSequence, nested);

      SnmpBerReader current(&nested[0], nested.size());
      int depth = 0;

      for (;;)
      {
         SnmpTlv element;
         if (!current.Next(element) || element.tag != SnmpTagSequence)
            break;

         SnmpBerReader child;
         if (!current.Enter(element, child))
            break;

         current = child;
         depth++;
      }

      Expect(depth == SnmpBerReader::MaxDepth, "The reader went deeper than MaxDepth, or not as deep.");
   }

   void
   SnmpTester::TestCryptography_()
   {
      // RFC 3414 A.2's algorithm with SHA-256 (RFC 7860 9.3), on A.3's inputs:
      // "maplesyrup" and the engine ID 00...02. The SHA-1 values RFC 3414 A.3.2
      // publishes come out of the same code with SHA-1 in place of SHA-256.
      SnmpBytes key;
      Expect(SnmpUsm::PasswordToKey("maplesyrup", key) &&
             SnmpBer::ToHex(key) == "ab51014d1e077f6017df2b12bee5f5aa72993177e9bb569c4dff5a4ca0b4afac",
             "The password-to-key algorithm did not give maplesyrup's SHA-256 key.");

      SnmpBytes localised;
      Expect(SnmpUsm::LocalisedKey("maplesyrup", Hex("000000000000000000000002"), localised) &&
             SnmpBer::ToHex(localised) == "8982e0e549e866db361a6b625d84cccc11162d453ee8ce3a6445c2d6776f0f8b",
             "maplesyrup's key was not localised to 00...02 as RFC 3414 A.2.2 does it.");

      Expect(!SnmpUsm::PasswordToKey("short", key), "A password of five characters was made into a key.");

      // HMAC-SHA-256 truncated to 192 bits, against Python's hmac module on the
      // same inputs.
      unsigned char mac[SnmpUsm::AuthParameterLength];
      const SnmpBytes hiThere = Text("Hi There");
      Expect(SnmpUsm::Digest(SnmpBytes(32, 0x0b), &hiThere[0], hiThere.size(), mac) &&
             SnmpBer::ToHex(SnmpBytes(mac, mac + SnmpUsm::AuthParameterLength)) == "198a607eb44bfbc69903a0f1cf2bbdc5ba0aa3f3d9ae3c1c",
             "HMAC-SHA-256-192 did not give the expected digest.");

      // AES-128-CFB128: NIST SP 800-38A F.3.13, and a final block cut short, since
      // a scoped PDU is rarely a whole number of blocks.
      const SnmpBytes aesKey = Hex("2b7e151628aed2a6abf7158809cf4f3c");
      const SnmpBytes aesIv = Hex("000102030405060708090a0b0c0d0e0f");
      const SnmpBytes plain = Hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");

      SnmpBytes encrypted;
      Expect(SnmpUsm::Encrypt(aesKey, &aesIv[0], &plain[0], plain.size(), encrypted) &&
             SnmpBer::ToHex(encrypted) == "3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b",
             "AES-128-CFB128 did not give NIST's ciphertext.");

      Expect(SnmpUsm::Encrypt(aesKey, &aesIv[0], &plain[0], 20, encrypted) &&
             SnmpBer::ToHex(encrypted) == "3b3fd92eb72dad20333449f8e83cfb4ac8a64537",
             "AES-128-CFB128 of 20 octets was not the first 20 octets of NIST's ciphertext.");

      SnmpBytes decrypted;
      Expect(SnmpUsm::Decrypt(aesKey, &aesIv[0], &encrypted[0], encrypted.size(), decrypted) &&
             SnmpBytes(plain.begin(), plain.begin() + 20) == decrypted,
             "AES-128-CFB128 did not decrypt what it encrypted.");

      unsigned char iv[16];
      const SnmpBytes salt = Hex("0102030405060708");
      SnmpUsm::MakeIv(5, 100, &salt[0], iv);
      Expect(SnmpBer::ToHex(SnmpBytes(iv, iv + 16)) == "00000005000000640102030405060708",
             "The IV is not boots, time and salt as RFC 3826 3.1.2.1 has it.");
   }

   void
   SnmpTester::TestV2c_()
   {
      FakeMib mib;
      SnmpBytes response;

      const SnmpBytes getDescription = Pdu(SnmpTagGetRequest, 21, 0, 0, Names(SysDescr));

      // Off: not answered, whatever the community.
      SnmpEngineSettings off = MakeSettings();
      off.v2cEnabled = false;
      SnmpEngine engineOff(off, &mib);

      Expect(!Run(engineOff, V2cMessage(1, off.community, getDescription), response), "SNMPv2c was answered while it is off.");
      Expect(engineOff.GetCounters().inBadVersions == 1, "SNMPv2c while off was not counted in snmpInBadVersions.");

      Expect(!Run(engineOff, V2cMessage(0, off.community, getDescription), response), "SNMPv1 was answered.");
      Expect(engineOff.GetCounters().inBadVersions == 2, "SNMPv1 was not counted in snmpInBadVersions.");

      // On.
      SnmpEngineSettings on = MakeSettings();
      on.v2cEnabled = true;
      SnmpEngine engine(on, &mib);

      Expect(!Run(engine, V2cMessage(1, "public", getDescription), response), "A wrong community was answered.");
      Expect(engine.GetCounters().inBadCommunityNames == 1, "A wrong community was not counted in snmpInBadCommunityNames.");

      Expect(!Run(engine, V2cMessage(1, "s3cret-communitx", getDescription), response), "A community one character out was answered.");

      Expect(Run(engine, V2cMessage(1, on.community, getDescription), response), "The right community was not answered.");

      SnmpTestAnswer answer;
      Expect(DecodeV2c(response, answer), "The SNMPv2c answer could not be read.");
      Expect(answer.version == 1 && answer.community == on.community && answer.pduTag == SnmpTagResponse &&
             answer.requestId == 21 && answer.errorStatus == 0 && answer.errorIndex == 0,
             "The SNMPv2c answer's envelope is not the request's.");
      Expect(answer.names.size() == 1 && answer.tags[0] == SnmpTagOctetString &&
             std::string(answer.values[0].begin(), answer.values[0].end()) == on.description,
             "sysDescr.0 was not the description.");

      // An object nobody serves, an instance of one that is served, and a served
      // object named without its instance (RFC 3416 4.2.1: noSuchInstance).
      std::vector<SnmpOid> absentNames = Names("1.3.6.1.2.1.99.1.0", "1.3.6.1.2.1.28.1.1.3.2");
      absentNames.push_back(SnmpBer::Oid("1.3.6.1.2.1.1.3"));

      Expect(Run(engine, V2cMessage(1, on.community, Pdu(SnmpTagGetRequest, 22, 0, 0, absentNames)), response),
             "A Get of absent objects was not answered.");

      SnmpTestAnswer absent;
      Expect(DecodeV2c(response, absent) && absent.names.size() == 3, "The answer to absent objects could not be read.");
      Expect(absent.tags[0] == SnmpTagNoSuchObject, "An object nobody serves was not noSuchObject.");
      Expect(absent.tags[1] == SnmpTagNoSuchInstance, "A missing instance of a served column was not noSuchInstance.");
      Expect(absent.tags[2] == SnmpTagNoSuchInstance, "sysUpTime without its .0 was not noSuchInstance.");

      // Set: noAccess, the bindings as they came, and counted as a bad use.
      Expect(Run(engine, V2cMessage(1, on.community, Pdu(SnmpTagSetRequest, 23, 0, 0, Names(SysDescr))), response),
             "A Set was not answered.");

      SnmpTestAnswer set;
      Expect(DecodeV2c(response, set), "The answer to a Set could not be read.");
      Expect(set.errorStatus == 6 && set.errorIndex == 1 && set.names.size() == 1 && set.tags[0] == SnmpTagNull,
             "A Set was not answered noAccess with its binding as it came.");
      Expect(engine.GetCounters().inBadCommunityUses == 1, "A Set was not counted in snmpInBadCommunityUses.");

      // A response sent to the agent is not answered.
      Expect(!Run(engine, V2cMessage(1, on.community, Pdu(SnmpTagResponse, 24, 0, 0, Names(SysDescr))), response),
             "A Response sent to the agent was answered.");

      // More bindings than MaxVarbinds: tooBig, with none.
      std::vector<SnmpOid> many;
      for (size_t i = 0; i <= SnmpEngine::MaxVarbinds; i++)
         many.push_back(SnmpBer::Oid(SysDescr));

      Expect(Run(engine, V2cMessage(1, on.community, Pdu(SnmpTagGetRequest, 25, 0, 0, many)), response), "A Get of too many bindings was not answered.");

      SnmpTestAnswer tooMany;
      Expect(DecodeV2c(response, tooMany) && tooMany.errorStatus == 1 && tooMany.names.empty(),
             "A Get of too many bindings was not answered tooBig with no bindings.");

      // A Get whose answer would pass the SNMPv2c cap: tooBig, with no bindings,
      // itself within the cap. A short community keeps the request small, so the
      // cap is the 484-octet floor and two contacts of 255 characters cannot fit.
      // Over SNMPv3, with room, the same two are answered - so it is the cap that
      // refused them, and not the size of the values.
      SnmpEngineSettings wordy = MakeSettings();
      wordy.v2cEnabled = true;
      wordy.community = "c";
      wordy.contact.assign(255, 'c');
      wordy.location.assign(255, 'l');
      SnmpEngine wordyEngine(wordy, &mib);

      const SnmpBytes twoLong = V2cMessage(1, wordy.community, Pdu(SnmpTagGetRequest, 26, 0, 0, Names("1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.6.0")));
      Expect(V2cCap(twoLong.size()) < (size_t) (2 * 255), "The tooBig case fits the SNMPv2c cap, so it proves nothing.");
      Expect(Run(wordyEngine, twoLong, response), "A Get past the SNMPv2c cap was not answered.");
      Expect(response.size() <= V2cCap(twoLong.size()), "The tooBig answer was longer than the SNMPv2c cap.");

      SnmpTestAnswer tooBig;
      Expect(DecodeV2c(response, tooBig) && tooBig.pduTag == SnmpTagResponse && tooBig.requestId == 26 &&
             tooBig.errorStatus == 1 && tooBig.errorIndex == 0 && tooBig.names.empty(),
             "A Get past the SNMPv2c cap was not answered tooBig with no bindings.");

      const Manager wordyManager = Knowing(wordy, 0);
      Expect(Run(wordyEngine, V3Message(wordyManager, 1901, 1500, 0x07, Pdu(SnmpTagGetRequest, 27, 0, 0, Names("1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.6.0")), 1), response),
             "The two long values were not answered over SNMPv3.");

      SnmpTestAnswer roomy;
      Expect(DecodeV3(response, wordyManager, roomy) && roomy.pduTag == SnmpTagResponse && roomy.errorStatus == 0 &&
             roomy.names.size() == 2 && roomy.values[0].size() == 255 && roomy.values[1].size() == 255,
             "The two long values were not answered in full over SNMPv3, where the request's msgMaxSize has room for them.");

      // An empty community answers nobody, even with SNMPv2c on.
      SnmpEngineSettings empty = MakeSettings();
      empty.v2cEnabled = true;
      empty.community.clear();
      SnmpEngine emptyEngine(empty, &mib);

      Expect(!Run(emptyEngine, V2cMessage(1, std::string(), getDescription), response), "An empty community was answered.");
   }

   void
   SnmpTester::TestV3_()
   {
      FakeMib mib;
      SnmpEngineSettings settings = MakeSettings();
      SnmpEngine engine(settings, &mib);

      // snmpEngineTime 100.
      engine.SetElapsedMilliseconds(100000);

      SnmpBytes response;
      const std::vector<SnmpOid> none;

      // Discovery (RFC 3414 section 4): no engine ID, no user, reportable.
      Manager nobody;
      Expect(Run(engine, V3Message(nobody, 1001, 1500, 0x04, Pdu(SnmpTagGetRequest, 11, 0, 0, none), 0), response),
             "A discovery probe was not answered.");

      SnmpTestAnswer discovered;
      ExpectReport(response, nobody, "1.3.6.1.6.3.15.1.1.4.0", "discovery");
      Expect(DecodeV3(response, nobody, discovered), "The discovery Report could not be read.");
      Expect(discovered.flags == 0 && discovered.engineId == settings.engineId && discovered.boots == 5 &&
             discovered.engineTime == 100 && discovered.requestId == 11,
             "The discovery Report does not carry the engine's ID, boots and time, or the probe's request-id.");

      // Not reportable: not answered.
      Expect(!Run(engine, V3Message(nobody, 1002, 1500, 0x00, Pdu(SnmpTagGetRequest, 12, 0, 0, none), 0), response),
             "A probe that did not ask for a Report was answered.");

      // Outside the time window: an authenticated Report with the boots and time.
      Manager stale = Knowing(settings, 0);
      stale.boots = 0;
      Expect(Run(engine, V3Message(stale, 1003, 1500, 0x07, Pdu(SnmpTagGetRequest, 13, 0, 0, Names(SysUpTime)), 1), response),
             "A request outside the time window was not answered.");

      SnmpTestAnswer window;
      Expect(DecodeV3(response, stale, window), "The time-window Report could not be read, or its digest was wrong.");
      Expect(window.flags == 0x01 && window.pduTag == SnmpTagReport && window.boots == 5 && window.engineTime == 100 &&
             window.names.size() == 1 && SnmpBer::Compare(window.names[0], SnmpBer::Oid("1.3.6.1.6.3.15.1.1.2.0")) == 0,
             "The time-window Report is not an authenticated usmStatsNotInTimeWindows with the engine's boots and time.");

      // In the window: the answer, authenticated and encrypted.
      Manager manager = Knowing(settings, 100);
      Expect(Run(engine, V3Message(manager, 1004, 1500, 0x07, Pdu(SnmpTagGetRequest, 14, 0, 0, Names(SysUpTime, MtaTransmitted)), 2), response),
             "An authPriv Get was not answered.");

      SnmpTestAnswer got;
      Expect(DecodeV3(response, manager, got), "The authPriv answer could not be verified and decrypted.");
      Expect(got.flags == 0x03 && got.pduTag == SnmpTagResponse && got.requestId == 14 && got.errorStatus == 0,
             "The authPriv answer is not an authPriv Response to the request.");
      Expect(got.names.size() == 2 && got.tags[0] == SnmpTagTimeTicks && Number(got.values[0]) == 10000,
             "sysUpTime.0 was not 10000 hundredths.");
      Expect(got.tags[1] == SnmpTagCounter32 && Number(got.values[1]) == 7, "mtaTransmittedMessages was not the source's value.");

      // The edges of the window: 150 seconds either way is in, 151 is out.
      Manager edge = Knowing(settings, 250);
      SnmpTestAnswer atEdge;
      Expect(Run(engine, V3Message(edge, 1005, 1500, 0x07, Pdu(SnmpTagGetRequest, 15, 0, 0, Names(SysUpTime)), 3), response) &&
             DecodeV3(response, edge, atEdge) && atEdge.pduTag == SnmpTagResponse,
             "A request 150 seconds ahead was not answered.");

      Manager past = Knowing(settings, 251);
      Expect(Run(engine, V3Message(past, 1006, 1500, 0x07, Pdu(SnmpTagGetRequest, 16, 0, 0, Names(SysUpTime)), 4), response),
             "A request 151 seconds ahead was not answered with a Report.");
      ExpectReport(response, past, "1.3.6.1.6.3.15.1.1.2.0", "151 seconds ahead");

      // A wrong authentication password: usmStatsWrongDigests, and no data.
      Manager impostor = manager;
      Expect(SnmpUsm::LocalisedKey("not-the-password", settings.engineId, impostor.authKey), "The impostor's key could not be made.");
      Expect(Run(engine, V3Message(impostor, 1007, 1500, 0x07, Pdu(SnmpTagGetRequest, 17, 0, 0, Names(SysUpTime)), 5), response),
             "A wrong digest was not answered with a Report.");
      ExpectReport(response, impostor, "1.3.6.1.6.3.15.1.1.5.0", "a wrong password");
      Expect(engine.GetCounters().wrongDigests == 1, "A wrong digest was not counted.");

      // A wrong privacy password: a Report of the decryption, or - if the octets
      // it made happened to start like a SEQUENCE - a message that did not decode.
      // Never data.
      Manager wrongPrivacy = manager;
      SnmpBytes otherPrivacy;
      Expect(SnmpUsm::LocalisedKey("not-the-privacy-key", settings.engineId, otherPrivacy), "The other privacy key could not be made.");
      wrongPrivacy.privKey.assign(otherPrivacy.begin(), otherPrivacy.begin() + (std::ptrdiff_t) SnmpUsm::PrivKeyLength);

      const unsigned int decryptionErrorsBefore = engine.GetCounters().decryptionErrors;
      const unsigned int parseErrorsBefore = engine.GetCounters().inAsnParseErrs;

      if (Run(engine, V3Message(wrongPrivacy, 1008, 1500, 0x07, Pdu(SnmpTagGetRequest, 18, 0, 0, Names(SysUpTime)), 6), response))
      {
         ExpectReport(response, manager, "1.3.6.1.6.3.15.1.1.6.0", "a wrong privacy password");
         Expect(engine.GetCounters().decryptionErrors == decryptionErrorsBefore + 1, "A decryption error was not counted.");
      }
      else
      {
         Expect(engine.GetCounters().inAsnParseErrs == parseErrorsBefore + 1, "A wrong privacy key was neither reported nor counted.");
      }

      // authNoPriv: below the level this agent answers.
      Expect(Run(engine, V3Message(manager, 1009, 1500, 0x05, Pdu(SnmpTagGetRequest, 19, 0, 0, Names(SysUpTime)), 7), response),
             "An authNoPriv request was not answered with a Report.");
      ExpectReport(response, manager, "1.3.6.1.6.3.15.1.1.1.0", "authNoPriv");

      // A user this engine does not have.
      Manager stranger = manager;
      stranger.user = "someone-else";
      Expect(Run(engine, V3Message(stranger, 1010, 1500, 0x07, Pdu(SnmpTagGetRequest, 20, 0, 0, Names(SysUpTime)), 8), response),
             "An unknown user was not answered with a Report.");
      ExpectReport(response, stranger, "1.3.6.1.6.3.15.1.1.3.0", "an unknown user");

      // Set: noAccess.
      SnmpTestAnswer set;
      Expect(Run(engine, V3Message(manager, 1011, 1500, 0x07, Pdu(SnmpTagSetRequest, 21, 0, 0, Names(SysDescr)), 9), response) &&
             DecodeV3(response, manager, set) && set.pduTag == SnmpTagResponse && set.errorStatus == 6 && set.errorIndex == 1,
             "An SNMPv3 Set was not answered noAccess.");

      // GetBulk within a msgMaxSize of 484: as many bindings as fit, and no more.
      Expect(Run(engine, V3Message(manager, 1012, 484, 0x07, Pdu(SnmpTagGetBulkRequest, 22, 0, 100000, Names("1.3.6.1")), 10), response),
             "A GetBulk in 484 octets was not answered.");
      Expect(response.size() <= 484, "A GetBulk answer was longer than the msgMaxSize the request named.");

      SnmpTestAnswer bulk;
      Expect(DecodeV3(response, manager, bulk) && bulk.errorStatus == 0 && !bulk.names.empty() &&
             bulk.names.size() < (size_t) (OwnObjects + FakeObjects),
             "A GetBulk in 484 octets did not answer with some but not all of the MIB.");

      // A latched engine answers nothing authenticated.
      SnmpEngineSettings latched = MakeSettings();
      latched.engineBoots = SnmpEngine::MaxEngineBoots;
      SnmpEngine latchedEngine(latched, &mib);
      latchedEngine.SetElapsedMilliseconds(100000);

      Manager atLatch = Knowing(latched, 100);
      Expect(Run(latchedEngine, V3Message(atLatch, 1013, 1500, 0x07, Pdu(SnmpTagGetRequest, 23, 0, 0, Names(SysUpTime)), 11), response),
             "A latched engine did not answer with a Report.");
      ExpectReport(response, atLatch, "1.3.6.1.6.3.15.1.1.2.0", "a latched engine");

      // The same request twice, inside the window, is answered twice - each answer
      // under a salt of its own, so that no IV is used twice under the key.
      const SnmpBytes twice = V3Message(manager, 1015, 1500, 0x07, Pdu(SnmpTagGetRequest, 25, 0, 0, Names(SysUpTime)), 13);
      SnmpBytes firstAnswer;
      SnmpBytes secondAnswer;
      Expect(Run(engine, twice, firstAnswer) && Run(engine, twice, secondAnswer), "A request sent twice was not answered twice.");

      SnmpTestAnswer first;
      SnmpTestAnswer second;
      Expect(DecodeV3(firstAnswer, manager, first) && DecodeV3(secondAnswer, manager, second), "The two answers could not be read.");
      Expect(first.privParameters.size() == SnmpUsm::PrivParameterLength && second.privParameters.size() == SnmpUsm::PrivParameterLength &&
             first.privParameters != second.privParameters,
             "Two answers were encrypted under one salt.");

      // A context this engine does not have: a Report, and authenticated, since
      // the request it answers was.
      const SnmpBytes otherContext = V3MessageFromScoped(manager, 1016, 1500, 0x07,
         ScopedPdu(settings.engineId, "another-context", Pdu(SnmpTagGetRequest, 26, 0, 0, Names(SysUpTime))), 14);
      Expect(Run(engine, otherContext, response), "A request for another context was not answered with a Report.");
      ExpectReport(response, manager, "1.3.6.1.6.3.12.1.5.0", "another context");

      SnmpTestAnswer contextReport;
      Expect(DecodeV3(response, manager, contextReport) && contextReport.flags == 0x01,
             "The Report of an unknown context was not authenticated.");

      // No user configured: every user is unknown.
      SnmpEngineSettings userless = MakeSettings();
      userless.user.clear();
      SnmpEngine userlessEngine(userless, &mib);
      userlessEngine.SetElapsedMilliseconds(100000);

      Expect(Run(userlessEngine, V3Message(manager, 1014, 1500, 0x07, Pdu(SnmpTagGetRequest, 24, 0, 0, Names(SysUpTime)), 12), response),
             "An engine with no user did not answer with a Report.");
      ExpectReport(response, manager, "1.3.6.1.6.3.15.1.1.3.0", "an engine with no user");
   }

   void
   SnmpTester::TestWalkAndBulk_()
   {
      FakeMib mib;
      SnmpEngineSettings settings = MakeSettings();
      settings.v2cEnabled = true;
      SnmpEngine engine(settings, &mib);

      SnmpBytes response;

      // A walk from the top: every object once, in order, then endOfMibView.
      SnmpOid cursor = SnmpBer::Oid("1.3.6.1");
      int steps = 0;
      bool ended = false;

      while (steps < 500)
      {
         std::vector<SnmpOid> names;
         names.push_back(cursor);

         Expect(Run(engine, V2cMessage(1, settings.community, Pdu(SnmpTagGetNextRequest, 30 + steps, 0, 0, names)), response),
                "A GetNext was not answered.");

         SnmpTestAnswer answer;
         Expect(DecodeV2c(response, answer) && answer.names.size() == 1, "A GetNext answer could not be read.");

         if (answer.tags[0] == SnmpTagEndOfMibView)
         {
            Expect(SnmpBer::Compare(answer.names[0], cursor) == 0, "endOfMibView did not name the binding it was asked about.");
            ended = true;
            break;
         }

         Expect(SnmpBer::Compare(answer.names[0], cursor) > 0, "GetNext did not move forward.");
         cursor = answer.names[0];
         steps++;
      }

      Expect(ended, "The walk did not end.");
      Expect(steps == OwnObjects + FakeObjects, "The walk did not visit every object exactly once.");

      // SNMPv2c's answer is held to eight times its request: the whole view asked
      // for in one small GetBulk comes back cut to fit, and still well formed.
      const SnmpBytes smallBulk = V2cMessage(1, settings.community, Pdu(SnmpTagGetBulkRequest, 39, 0, 64, Names("1.3.6.1")));
      Expect(Run(engine, smallBulk, response), "A small SNMPv2c GetBulk was not answered.");
      Expect(response.size() <= V2cCap(smallBulk.size()), "An SNMPv2c answer was longer than eight times its request.");

      SnmpTestAnswer capped;
      Expect(DecodeV2c(response, capped) && capped.errorStatus == 0 && !capped.names.empty() &&
             capped.names.size() < (size_t) (OwnObjects + FakeObjects),
             "A small SNMPv2c GetBulk was not cut to fit its cap.");

      // The rest over SNMPv3, whose answers the request's msgMaxSize bounds.
      const Manager manager = Knowing(settings, 0);

      // GetBulk: one non-repeater, one repeater from the mail MIB, max-repetitions
      // far past the cap - the repeater runs to the end of the view and stops.
      Expect(Run(engine, V3Message(manager, 4001, 8192, 0x07, Pdu(SnmpTagGetBulkRequest, 40, 1, 100000, Names(SysDescr, "1.3.6.1.2.1.28")), 41), response),
             "A GetBulk was not answered.");

      SnmpTestAnswer bulk;
      Expect(DecodeV3(response, manager, bulk), "A GetBulk answer could not be read.");
      Expect(!bulk.names.empty() && SnmpBer::Compare(bulk.names[0], SnmpBer::Oid(SysObjectId)) == 0,
             "GetBulk's non-repeater was not GetNext of sysDescr.0.");

      // After the mail MIB: the fake's five objects and the fifteen snmpEngine,
      // snmpMPDStats, context and usmStats objects, then one endOfMibView.
      Expect(bulk.names.size() == 1 + 5 + 15 + 1 && bulk.tags.back() == SnmpTagEndOfMibView,
             "GetBulk did not walk the rest of the view and stop at its end.");

      // Every binding a request may carry, each repeating to the cap: held to
      // MaxResponseVarbinds bindings and to the message size.
      std::vector<SnmpOid> columns;
      for (size_t i = 0; i < SnmpEngine::MaxVarbinds; i++)
         columns.push_back(SnmpBer::Oid("1.3.6.1"));

      Expect(Run(engine, V3Message(manager, 4002, 60000, 0x07, Pdu(SnmpTagGetBulkRequest, 41, 0, 100000, columns), 42), response),
             "A GetBulk of every column was not answered.");
      Expect(response.size() <= SnmpEngine::MaxMessageSize, "A GetBulk answer was longer than MaxMessageSize.");

      SnmpTestAnswer wide;
      Expect(DecodeV3(response, manager, wide) && wide.errorStatus == 0 && !wide.names.empty() &&
             wide.names.size() <= SnmpEngine::MaxResponseVarbinds,
             "A GetBulk of every column was not held within MaxResponseVarbinds.");
   }

   void
   SnmpTester::TestMalformed_()
   {
      FakeMib mib;
      SnmpEngineSettings settings = MakeSettings();
      settings.v2cEnabled = true;
      SnmpEngine engine(settings, &mib);
      engine.SetElapsedMilliseconds(100000);

      const Manager manager = Knowing(settings, 100);
      const SnmpBytes valid = V3Message(manager, 2001, 1500, 0x07, Pdu(SnmpTagGetRequest, 41, 0, 0, Names(SysUpTime)), 21);

      SnmpBytes response;
      Expect(Run(engine, valid, response), "The request the malformed ones are made from was not answered.");

      // Every cut of the whole datagram: none decodes, and each is counted. Every
      // one of these fails at the outermost length; the cuts inside true lengths
      // are TestInnerTruncations_'s.
      unsigned int before = engine.GetCounters().inAsnParseErrs;

      for (size_t cut = 0; cut < valid.size(); cut++)
      {
         SnmpBytes truncated(valid.begin(), valid.begin() + (std::ptrdiff_t) cut);
         Expect(!Run(engine, truncated, response), "A request cut to " + std::to_string(cut) + " octets was answered.");
      }

      Expect(engine.GetCounters().inAsnParseErrs == before + (unsigned int) valid.size(), "Not every truncated request was counted in snmpInASNParseErrs.");

      // One of each shape of wrong.
      std::vector<SnmpBytes> shapes;

      SnmpBytes trailing = valid;
      trailing.push_back(0x00);
      shapes.push_back(trailing);

      shapes.push_back(Hex("3080020103000000"));                    // an indefinite length
      shapes.push_back(Hex("308500000000030201030201"));            // a length of five octets
      shapes.push_back(Hex("30847fffffff020103"));                  // a length far past the datagram
      shapes.push_back(Hex("3f81010302010302"));                    // a tag of more than one octet
      shapes.push_back(Hex("300b0209000102030405060708"));          // a version of nine octets
      shapes.push_back(Hex("3000"));                                // an empty message
      shapes.push_back(Hex("30030201"));                            // a version with its length and no value
      shapes.push_back(Hex("300c020103020105040161020100"));        // v3 whose header is an INTEGER
      shapes.push_back(V2cMessage(1, settings.community, Hex("a00c020101020100020100020105"))); // a binding list that is an INTEGER

      // Bindings that are not SEQUENCEs of a name and a value.
      shapes.push_back(V2cMessage(1, settings.community, Hex("a00f020101020100020100300430020500")));

      SnmpBytes deep = SnmpBer::EncodeNull(SnmpTagNull);
      for (int i = 0; i < 200; i++)
         deep = SnmpBer::Wrap(SnmpTagSequence, deep);
      shapes.push_back(deep);

      shapes.push_back(SnmpBytes(SnmpEngine::MaxMessageSize + 1, 0x30));

      for (size_t i = 0; i < shapes.size(); i++)
      {
         const unsigned int errorsBefore = engine.GetCounters().inAsnParseErrs;
         Expect(!Run(engine, shapes[i], response), "Malformed request " + std::to_string(i) + " was answered.");
         Expect(engine.GetCounters().inAsnParseErrs == errorsBefore + 1, "Malformed request " + std::to_string(i) + " was not counted in snmpInASNParseErrs.");
      }

      // Turned over at random, deterministically: no throw, nothing longer than the
      // engine sends, and nothing but a Report to an SNMPv3 message that has been
      // changed - its digest cannot hold.
      const SnmpBytes validV2c = V2cMessage(1, settings.community, Pdu(SnmpTagGetBulkRequest, 51, 0, 10, Names("1.3.6.1")));
      unsigned int state = 0x12345678u;

      for (int round = 0; round < 4000; round++)
      {
         const bool isV3 = (round % 2) == 0;
         SnmpBytes mutated = isV3 ? valid : validV2c;

         state = state * 1103515245u + 12345u;
         const int flips = 1 + (int) ((state >> 16) % 4);

         for (int flip = 0; flip < flips; flip++)
         {
            state = state * 1103515245u + 12345u;
            const size_t at = (size_t) ((state >> 8) % (unsigned int) mutated.size());
            state = state * 1103515245u + 12345u;
            mutated[at] = (unsigned char) ((state >> 16) & 0xFF);
         }

         state = state * 1103515245u + 12345u;
         if ((state >> 16) % 5 == 0)
         {
            state = state * 1103515245u + 12345u;
            mutated.resize((size_t) ((state >> 8) % (unsigned int) mutated.size()));
         }

         bool answered = false;

         try
         {
            answered = Run(engine, mutated, response);
         }
         catch (...)
         {
            Expect(false, "A turned-over request made the engine throw, round " + std::to_string(round) + ".");
         }

         if (!answered)
            continue;

         Expect(response.size() <= SnmpEngine::MaxMessageSize, "An answer to a turned-over request was too long.");

         if (!isV3)
         {
            Expect(response.size() <= V2cCap(mutated.size()), "An answer to a turned-over SNMPv2c request was longer than its cap.");

            SnmpTestAnswer answer;
            Expect(DecodeV2c(response, answer) && answer.pduTag == SnmpTagResponse,
                   "An answer to a turned-over SNMPv2c request could not be read back, round " + std::to_string(round) + ".");
         }

         if (isV3)
         {
            // A message changed after it was signed cannot pass the digest, so
            // it gets a Report or nothing; flips that happened to write back the
            // octets they replaced leave the valid request, and its Response.
            // Either way the answer is one a manager can read.
            SnmpTestAnswer answer;
            Expect(DecodeV3(response, manager, answer),
                   "An answer to a turned-over SNMPv3 request could not be read back, round " + std::to_string(round) + ".");
            Expect(answer.pduTag == (mutated == valid ? SnmpTagResponse : SnmpTagReport),
                   "A turned-over SNMPv3 request was answered with data, round " + std::to_string(round) + ".");
         }
      }
   }

   void
   SnmpTester::TestInnerTruncations_()
   {
      FakeMib mib;
      SnmpEngineSettings settings = MakeSettings();
      settings.v2cEnabled = true;
      SnmpEngine engine(settings, &mib);
      engine.SetElapsedMilliseconds(100000);

      const Manager manager = Knowing(settings, 100);
      SnmpBytes response;

      // SNMPv2c: every element of a GetBulk cut short inside true lengths. A cut
      // that leaves a child unfinished - in the message, the PDU, the binding
      // list or one binding - must be refused by the decoder and counted once,
      // at every one of those levels; what is answered must be a Response
      // within the cap.
      const SnmpBytes v2c = V2cMessage(1, settings.community, Pdu(SnmpTagGetBulkRequest, 61, 1, 5, Names(SysDescr, "1.3.6.1.2.1.28")));
      size_t v2cDeepest = 0;
      const std::vector<SnmpTestCut> v2cCuts = InnerTruncations(v2c, v2cDeepest);
      Expect(v2cCuts.size() > 100, "The SNMPv2c request was cut into too few variants to mean anything.");
      Expect(v2cDeepest >= 3, "The SNMPv2c request does not reach the depth of one binding.");

      std::vector<size_t> v2cRefused;

      for (size_t i = 0; i < v2cCuts.size(); i++)
      {
         const unsigned int parseErrorsBefore = engine.GetCounters().inAsnParseErrs;
         bool answered = false;

         try
         {
            answered = Run(engine, v2cCuts[i].message, response);
         }
         catch (...)
         {
            Expect(false, "An SNMPv2c request cut inside made the engine throw, variant " + std::to_string(i) + ".");
         }

         ExpectRefusedWhereItMustBe(v2cCuts[i], answered, parseErrorsBefore, engine.GetCounters().inAsnParseErrs, v2cRefused,
                                    "SNMPv2c cut " + std::to_string(i));

         if (!answered)
            continue;

         Expect(response.size() <= V2cCap(v2cCuts[i].message.size()), "An answer to a cut SNMPv2c request was longer than its cap.");

         SnmpTestAnswer answer;
         Expect(DecodeV2c(response, answer) && answer.pduTag == SnmpTagResponse,
                "An answer to a cut SNMPv2c request could not be read back, variant " + std::to_string(i) + ".");
      }

      ExpectRefusalsAtEveryDepth(v2cRefused, v2cDeepest, "SNMPv2c");

      // SNMPv3, the envelope: every element of an authPriv request cut short, the
      // header and the security parameters included. The header and the USM
      // SEQUENCE inside its OCTET STRING are decoded before the digest is
      // checked, so a cut that leaves one of their children unfinished must be
      // refused there; anything else the digest turns away, and an answer is
      // only ever a Report.
      const SnmpBytes v3 = V3Message(manager, 3001, 1500, 0x07, Pdu(SnmpTagGetRequest, 62, 0, 0, Names(SysUpTime)), 31);
      size_t v3Deepest = 0;
      const std::vector<SnmpTestCut> envelopeCuts = InnerTruncations(v3, v3Deepest);
      Expect(envelopeCuts.size() > 100, "The SNMPv3 request was cut into too few variants to mean anything.");
      Expect(v3Deepest >= 2, "The SNMPv3 request was not read down to its USM security parameters.");

      std::vector<size_t> envelopeRefused;

      for (size_t i = 0; i < envelopeCuts.size(); i++)
      {
         const unsigned int parseErrorsBefore = engine.GetCounters().inAsnParseErrs;
         bool answered = false;

         try
         {
            answered = Run(engine, envelopeCuts[i].message, response);
         }
         catch (...)
         {
            Expect(false, "An SNMPv3 request cut inside made the engine throw, variant " + std::to_string(i) + ".");
         }

         ExpectRefusedWhereItMustBe(envelopeCuts[i], answered, parseErrorsBefore, engine.GetCounters().inAsnParseErrs, envelopeRefused,
                                    "SNMPv3 envelope cut " + std::to_string(i));

         if (!answered)
            continue;

         SnmpTestAnswer answer;
         Expect(DecodeV3(response, manager, answer) && answer.pduTag == SnmpTagReport,
                "A cut SNMPv3 request was answered with something other than a Report, variant " + std::to_string(i) + ".");
      }

      ExpectRefusalsAtEveryDepth(envelopeRefused, v3Deepest, "SNMPv3 envelope");

      // SNMPv3, the plaintext: every element of the scoped PDU cut short, then
      // encrypted and signed as a manager would, so that the digest holds, the
      // decryption succeeds and the fault is the plaintext's alone. The same
      // rule, at every level of the scoped PDU.
      const SnmpBytes scoped = ScopedPdu(settings.engineId, std::string(),
         Pdu(SnmpTagGetBulkRequest, 63, 1, 5, Names(SysDescr, "1.3.6.1.2.1.28")));
      size_t scopedDeepest = 0;
      const std::vector<SnmpTestCut> scopedCuts = InnerTruncations(scoped, scopedDeepest);
      Expect(scopedCuts.size() > 60, "The scoped PDU was cut into too few variants to mean anything.");
      Expect(scopedDeepest >= 3, "The scoped PDU does not reach the depth of one binding.");

      std::vector<size_t> scopedRefused;
      unsigned long long salt = 1000;

      for (size_t i = 0; i < scopedCuts.size(); i++)
      {
         const SnmpBytes message = V3MessageFromScoped(manager, 3100 + (long long) i, 1500, 0x07, scopedCuts[i].message, salt++);
         const unsigned int parseErrorsBefore = engine.GetCounters().inAsnParseErrs;
         bool answered = false;

         try
         {
            answered = Run(engine, message, response);
         }
         catch (...)
         {
            Expect(false, "A scoped PDU cut inside made the engine throw, variant " + std::to_string(i) + ".");
         }

         ExpectRefusedWhereItMustBe(scopedCuts[i], answered, parseErrorsBefore, engine.GetCounters().inAsnParseErrs, scopedRefused,
                                    "Scoped PDU cut " + std::to_string(i));

         if (!answered)
            continue;

         Expect(response.size() <= 1500, "An answer to a cut scoped PDU was longer than the request's msgMaxSize.");

         SnmpTestAnswer answer;
         Expect(DecodeV3(response, manager, answer) && (answer.pduTag == SnmpTagResponse || answer.pduTag == SnmpTagReport),
                "An answer to a cut scoped PDU could not be read back, variant " + std::to_string(i) + ".");
      }

      ExpectRefusalsAtEveryDepth(scopedRefused, scopedDeepest, "the scoped PDU");

      // SNMPv3, the plaintext turned over at random and then signed: two thousand
      // of them, none of which may throw or be answered with anything unreadable
      // or too long.
      unsigned int state = 0x2468ACE1u;

      for (int round = 0; round < 2000; round++)
      {
         SnmpBytes mutated = scoped;

         state = state * 1103515245u + 12345u;
         const int flips = 1 + (int) ((state >> 16) % 4);

         for (int flip = 0; flip < flips; flip++)
         {
            state = state * 1103515245u + 12345u;
            const size_t at = (size_t) ((state >> 8) % (unsigned int) mutated.size());
            state = state * 1103515245u + 12345u;
            mutated[at] = (unsigned char) ((state >> 16) & 0xFF);
         }

         const SnmpBytes message = V3MessageFromScoped(manager, 5000 + round, 1500, 0x07, mutated, salt++);
         bool answered = false;

         try
         {
            answered = Run(engine, message, response);
         }
         catch (...)
         {
            Expect(false, "A turned-over scoped PDU made the engine throw, round " + std::to_string(round) + ".");
         }

         if (!answered)
            continue;

         Expect(response.size() <= 1500, "An answer to a turned-over scoped PDU was longer than the request's msgMaxSize.");

         SnmpTestAnswer answer;
         Expect(DecodeV3(response, manager, answer) && (answer.pduTag == SnmpTagResponse || answer.pduTag == SnmpTagReport),
                "An answer to a turned-over scoped PDU could not be read back, round " + std::to_string(round) + ".");
      }
   }
}
