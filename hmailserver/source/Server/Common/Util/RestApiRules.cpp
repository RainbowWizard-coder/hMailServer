// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's global rule writes: POST /api/v1/rules, PUT and DELETE /api/v1/rules/<id>. See RestApiServer.h.
//
// What a rule is here, and where each part of it goes: a row in hm_rules with
// its criteria in hm_rule_criterias and its actions in hm_rule_actions, saved
// through PersistentRule::SaveObject exactly as InterfaceRule::Save saves one
// for the Control Panel - the rule row first with ruleactive = 0, then every
// criterion and action beneath it, then the row set active, then
// ObjectCache::SetGlobalRulesNeedsReload so that the delivery path's next
// GetGlobalRules() reads the table again. That flag is the whole of the
// refresh: nothing else caches a global rule, and the Control Panel's own
// collection (InterfaceApplication::get_Rules) is a fresh Rules(0) each time.
// So a rule created here acts on the next message delivered, without a
// restart, for the same reason one saved in the Control Panel does.
//
// The document is the listing's: the field names, the words for a criterion's
// field and match type and for an action's type are those HandleListRules_
// emits, so that what a client reads from GET is what it may send to POST.
// The one addition is that an action carries the parameters its type takes
// (a reply's sender and text, a route's id, a header's name) beside the
// single "value" the listing shows, because a reply rule cannot be described
// by one string.
//
// Validation is stricter than COM's in three places, on purpose and reported
// in the wave's notes: an action type that needs a parameter is refused
// without it (COM saves a forward with no recipient, which then forwards to
// nobody), a regular expression that will not compile is refused (COM saves
// it, and the rule is then active in the editor and matches nothing, which
// RuleGuard names the worst failure to diagnose), and a value longer than
// its column is refused with the width, so that the answer is a 400 naming
// the field rather than a failed INSERT. Nothing is persisted here that COM
// would have refused.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "Encoding/ModifiedUTF7.h"

#include "../BO/Rules.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../BO/Rule.h"
#include "../BO/RuleCriterias.h"
#include "../BO/RuleCriteria.h"
#include "../BO/RuleActions.h"
#include "../BO/RuleAction.h"
#include "../Persistence/PersistentRule.h"
#include "../Persistence/PersistentRuleCriteria.h"
#include "../Persistence/PersistentRuleAction.h"
#include "../Persistence/PersistenceMode.h"
#include "../Application/ObjectCache.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"
#include "../../SMTP/RuleApplier.h"
#include "../Rules/RuleGuard.h"

// The same construction RuleGuard::RegexCriteriaMatches makes - boost::wregex
// from the String, default (Perl) flags - so that "compiles here" means
// "compiles on the delivery path" and nothing else.
#include <boost/regex.hpp>

#include <exception>
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
      // The words of the listing, and their inverses. HandleListRules_ keeps its
      // own copies of the first three in RestApiServer.cpp; these must say the
      // same thing, and the fixture reads a rule back through the listing after
      // every write to hold them to it.
      const char *RuleFieldWord(RuleCriteria::PredefinedField field)
      {
         switch (field)
         {
         case RuleCriteria::FTFrom: return "from";
         case RuleCriteria::FTTo: return "to";
         case RuleCriteria::FTCC: return "cc";
         case RuleCriteria::FTSubject: return "subject";
         case RuleCriteria::FTBody: return "body";
         case RuleCriteria::FTMessageSize: return "message_size";
         case RuleCriteria::FTRecipientList: return "recipient_list";
         case RuleCriteria::FTDeliveryAttempts: return "delivery_attempts";
         case RuleCriteria::FTAttachmentName: return "attachment_name";
         case RuleCriteria::FTSensitiveContent: return "sensitive_content";
         case RuleCriteria::FTAttachmentType: return "attachment_type";
         case RuleCriteria::FTExternalRecipient: return "external_recipient";
         case RuleCriteria::FTContent: return "content";
         default: return "unknown";
         }
      }

      bool RuleFieldFromWord(const std::string &word, RuleCriteria::PredefinedField &field)
      {
         static const RuleCriteria::PredefinedField fields[] =
         {
            RuleCriteria::FTFrom, RuleCriteria::FTTo, RuleCriteria::FTCC, RuleCriteria::FTSubject,
            RuleCriteria::FTBody, RuleCriteria::FTMessageSize, RuleCriteria::FTRecipientList,
            RuleCriteria::FTDeliveryAttempts, RuleCriteria::FTAttachmentName,
            RuleCriteria::FTSensitiveContent, RuleCriteria::FTAttachmentType,
            RuleCriteria::FTExternalRecipient, RuleCriteria::FTContent
         };

         for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
         {
            if (word == RuleFieldWord(fields[i]))
            {
               field = fields[i];
               return true;
            }
         }

         return false;
      }

      const char *RuleMatchWord(RuleCriteria::MatchType match)
      {
         switch (match)
         {
         case RuleCriteria::Equals: return "equals";
         case RuleCriteria::Contains: return "contains";
         case RuleCriteria::LessThan: return "less_than";
         case RuleCriteria::GreaterThan: return "greater_than";
         case RuleCriteria::MatchesRegEx: return "regex";
         case RuleCriteria::NotContains: return "not_contains";
         case RuleCriteria::NotEquals: return "not_equals";
         case RuleCriteria::Wildcard: return "wildcard";
         default: return "none";
         }
      }

      bool RuleMatchFromWord(const std::string &word, RuleCriteria::MatchType &match)
      {
         static const RuleCriteria::MatchType matches[] =
         {
            RuleCriteria::Equals, RuleCriteria::Contains, RuleCriteria::LessThan, RuleCriteria::GreaterThan,
            RuleCriteria::MatchesRegEx, RuleCriteria::NotContains, RuleCriteria::NotEquals, RuleCriteria::Wildcard
         };

         for (size_t i = 0; i < sizeof(matches) / sizeof(matches[0]); i++)
         {
            if (word == RuleMatchWord(matches[i]))
            {
               match = matches[i];
               return true;
            }
         }

         return false;
      }

      const char *RuleActionWord(RuleAction::Type type)
      {
         switch (type)
         {
         case RuleAction::Delete: return "delete";
         case RuleAction::Forward: return "forward";
         case RuleAction::Reply: return "reply";
         case RuleAction::MoveToIMAPFolder: return "move_to_folder";
         case RuleAction::ScriptFunction: return "script_function";
         case RuleAction::StopRuleProcessing: return "stop";
         case RuleAction::SetHeaderValue: return "set_header";
         case RuleAction::SendUsingRoute: return "send_using_route";
         case RuleAction::CreateCopy: return "copy";
         case RuleAction::BindToAddress: return "bind_to_address";
         case RuleAction::AddDisclaimer: return "add_disclaimer";
         case RuleAction::Reject: return "reject";
         case RuleAction::RequireTLS: return "require_tls";
         case RuleAction::Quarantine: return "quarantine";
         case RuleAction::WarnSender: return "warn_sender";
         default: return "unknown";
         }
      }

      bool RuleActionFromWord(const std::string &word, RuleAction::Type &type)
      {
         static const RuleAction::Type types[] =
         {
            RuleAction::Delete, RuleAction::Forward, RuleAction::Reply, RuleAction::MoveToIMAPFolder,
            RuleAction::ScriptFunction, RuleAction::StopRuleProcessing, RuleAction::SetHeaderValue,
            RuleAction::SendUsingRoute, RuleAction::CreateCopy, RuleAction::BindToAddress,
            RuleAction::AddDisclaimer, RuleAction::Reject, RuleAction::RequireTLS, RuleAction::Quarantine,
            RuleAction::WarnSender
         };

         for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
         {
            if (word == RuleActionWord(types[i]))
            {
               type = types[i];
               return true;
            }
         }

         return false;
      }

      // The column widths of hm_rules, hm_rule_criterias and hm_rule_actions, as
      // the create scripts declare them on every backend. A value over its
      // width is refused here with the number, for the reason
      // PersistentRuleCriteria gives for its own check: the alternative is the
      // driver deciding, which on one backend is an error and on another a
      // silent truncation.
      const int NameWidth = 100;
      const int HeaderFieldWidth = 255;
      const int MatchValueWidth = 2000;
      const int ActionTextWidth = 255;
      const int ActionHeaderNameWidth = 80;

      // A caller's word as it appears in a refusal: bounded, so that a
      // kilobyte sent as a field name does not come back as a kilobyte of
      // error, and quoted. The handler escapes the whole sentence once.
      std::string Quoted(const std::string &word)
      {
         const size_t Most = 64;
         if (word.size() <= Most)
            return "'" + word + "'";
         return "'" + word.substr(0, Most) + "...'";
      }

      std::string Positioned(const char *array, size_t index)
      {
         char buffer[48];
         sprintf_s(buffer, sizeof(buffer), "%s[%u]: ", array, (unsigned) index);
         return buffer;
      }

      // A JSON string member, as the server's wide String. False, with the
      // sentence, when the member is there and is not a string.
      bool ReadString(const JsonValue &object, const char *key, const std::string &position, bool &present, String &out, std::string &error)
      {
         const JsonValue *member = object.Get(key);
         present = member != nullptr;
         if (!member)
            return true;

         if (!member->IsString())
         {
            error = position + key + " must be a string";
            return false;
         }

         Unicode::MultiByteToWide(AnsiString(member->AsString()), out);
         return true;
      }

      bool WithinWidth(const String &value, int width, const char *key, const std::string &position, std::string &error)
      {
         if (value.GetLength() <= width)
            return true;

         char buffer[32];
         sprintf_s(buffer, sizeof(buffer), "%d", width);
         error = position + key + " is longer than " + buffer + " characters";
         return false;
      }

      // A CR, LF, NUL or any other control character. A rule's name goes into
      // one line of a 550 reply and into the Subject and X-hMailServer-Policy
      // headers of the notice a policy sends, and a reject's text is that 550
      // line; a line break in either would end the line and begin another of
      // the author's choosing. A warn_sender text is a body, so its line
      // breaks and tabs are paragraphs, and only the rest is refused.
      bool HasControlCharacters(const String &text, bool lineBreaksAllowed)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text[i];
            if (lineBreaksAllowed && (c == L'\r' || c == L'\n' || c == L'\t'))
               continue;
            if (c < 0x20 || c == 0x7f)
               return true;
         }
         return false;
      }

      // Everything a POST or PUT body says, checked, before a row is touched:
      // a body refused half-way must leave the rule it names as it was.
      struct RuleDocument
      {
         RuleDocument() : active(true), useAnd(true) { }

         String name;
         bool active;
         bool useAnd;
         std::vector<std::shared_ptr<RuleCriteria> > criteria;
         std::vector<std::shared_ptr<RuleAction> > actions;
      };

      bool ParseCriterion(const JsonValue &object, size_t index, std::shared_ptr<RuleCriteria> &criterion, std::string &error)
      {
         const std::string position = Positioned("criteria", index);

         if (!object.IsObject())
         {
            error = position + "must be an object";
            return false;
         }

         // Unknown keys first, so a misspelt one is named rather than silently
         // ignored - a criterion whose "vlaue" was dropped is a criterion that
         // matches everything or nothing.
         const std::vector<std::pair<std::string, JsonValue> > &members = object.Members();
         for (size_t i = 0; i < members.size(); i++)
         {
            const std::string &key = members[i].first;
            if (key != "field" && key != "header" && key != "match" && key != "value")
            {
               error = position + "unknown key " + Quoted(key);
               return false;
            }
         }

         const JsonValue *fieldValue = object.Get("field");
         if (!fieldValue || !fieldValue->IsString())
         {
            error = position + "field is required";
            return false;
         }

         const std::string &fieldWord = fieldValue->AsString();
         bool isHeader = fieldWord == "header";
         RuleCriteria::PredefinedField field = RuleCriteria::FTUnknown;
         if (!isHeader && !RuleFieldFromWord(fieldWord, field))
         {
            error = position + "unknown field " + Quoted(fieldWord);
            return false;
         }

         bool headerPresent = false;
         String headerName;
         if (!ReadString(object, "header", position, headerPresent, headerName, error))
            return false;
         headerName.Trim();

         if (isHeader && headerName.IsEmpty())
         {
            error = position + "header is required when field is header";
            return false;
         }
         if (!isHeader && !headerName.IsEmpty())
         {
            error = position + "header is only used when field is header";
            return false;
         }
         if (!WithinWidth(headerName, HeaderFieldWidth, "header", position, error))
            return false;

         const JsonValue *matchValue = object.Get("match");
         if (!matchValue || !matchValue->IsString())
         {
            error = position + "match is required";
            return false;
         }

         RuleCriteria::MatchType match = RuleCriteria::None;
         if (!RuleMatchFromWord(matchValue->AsString(), match))
         {
            error = position + "unknown match type " + Quoted(matchValue->AsString());
            return false;
         }

         bool valuePresent = false;
         String value;
         if (!ReadString(object, "value", position, valuePresent, value, error))
            return false;
         if (!valuePresent)
         {
            error = position + "value is required";
            return false;
         }

         // The sentence PersistentRuleCriteria::SaveObject gives the Control
         // Panel for the same value, before any row is written.
         if (value.GetLength() > MatchValueWidth)
         {
            error = position + "The criterion's value is longer than 2000 characters, which is the most the database stores. Shorten the value; it has not been saved.";
            return false;
         }

         if (match == RuleCriteria::MatchesRegEx)
         {
            try
            {
               boost::wregex expression(value);
            }
            catch (const std::exception &e)
            {
               // Boost's own reason, which names the construct it could not read
               // and may quote the pattern; the handler escapes it.
               error = position + "the regular expression does not compile: " + e.what();
               return false;
            }
         }

         criterion = std::shared_ptr<RuleCriteria>(new RuleCriteria());
         criterion->SetUsePredefined(!isHeader);
         criterion->SetPredefinedField(isHeader ? RuleCriteria::FTUnknown : field);
         criterion->SetHeaderField(headerName);
         criterion->SetMatchType(match);
         criterion->SetMatchValue(value);
         return true;
      }

      // Does this action type take this parameter? "value" is answered
      // separately: the listing shows one value per action, and it is accepted
      // for every type as the parameter it stands for there.
      bool ActionTakes(RuleAction::Type type, const std::string &key)
      {
         switch (type)
         {
         // abort_spam_flagged is the desktop dialog's "Abort on messages
         // marked as spam", offered for the two actions that send mail on.
         case RuleAction::Forward: return key == "to" || key == "abort_spam_flagged";
         case RuleAction::Reply: return key == "from_name" || key == "from_address" || key == "subject" || key == "body" || key == "abort_spam_flagged";
         case RuleAction::MoveToIMAPFolder: return key == "folder";
         case RuleAction::ScriptFunction: return key == "script_function";
         case RuleAction::SetHeaderValue: return key == "header";
         case RuleAction::SendUsingRoute: return key == "route_id";
         case RuleAction::AddDisclaimer: return key == "body";
         default: return false;
         }
      }

      // The parameter the listing's "value" stands for, per type; nullptr for a
      // type whose value is always empty there. bind_to_address and set_header
      // keep the address and the header's value in the action's Value column,
      // so for those "value" is the parameter itself.
      const char *ValueStandsFor(RuleAction::Type type)
      {
         switch (type)
         {
         case RuleAction::Forward: return "to";
         case RuleAction::MoveToIMAPFolder: return "folder";
         case RuleAction::ScriptFunction: return "script_function";
         case RuleAction::SetHeaderValue: return "value";
         case RuleAction::BindToAddress: return "value";
         case RuleAction::AddDisclaimer: return "body";
         case RuleAction::Reject: return "value";
         case RuleAction::WarnSender: return "value";
         default: return nullptr;
         }
      }

      bool ParseAction(const JsonValue &object, size_t index, int sortOrder, std::shared_ptr<RuleAction> &action, std::string &error)
      {
         const std::string position = Positioned("actions", index);

         if (!object.IsObject())
         {
            error = position + "must be an object";
            return false;
         }

         const JsonValue *typeValue = object.Get("type");
         if (!typeValue || !typeValue->IsString())
         {
            error = position + "type is required";
            return false;
         }

         RuleAction::Type type = RuleAction::Unknown;
         if (!RuleActionFromWord(typeValue->AsString(), type))
         {
            error = position + "unknown action type " + Quoted(typeValue->AsString());
            return false;
         }

         // Every key must be one an action can carry, and one this type takes:
         // a "to" on a delete is not ignored, because the caller meant a
         // forward and would otherwise get a delete.
         static const char *known[] =
         {
            "type", "value", "to", "from_name", "from_address", "subject", "body",
            "folder", "header", "route_id", "script_function", "abort_spam_flagged"
         };

         const std::vector<std::pair<std::string, JsonValue> > &members = object.Members();
         for (size_t i = 0; i < members.size(); i++)
         {
            const std::string &key = members[i].first;

            bool recognised = false;
            for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            {
               if (key == known[k])
               {
                  recognised = true;
                  break;
               }
            }
            if (!recognised)
            {
               error = position + "unknown key " + Quoted(key);
               return false;
            }

            if (key == "type" || key == "value")
               continue;

            if (!ActionTakes(type, key))
            {
               error = position + key + " is not a parameter of a " + typeValue->AsString() + " action";
               return false;
            }
         }

         // "value", read once: the parameter it stands for, or nothing for a
         // type whose listing value is empty (where a non-empty one is a
         // mistake, not something to drop).
         bool valuePresent = false;
         String value;
         if (!ReadString(object, "value", position, valuePresent, value, error))
            return false;

         const char *standsFor = ValueStandsFor(type);
         if (!standsFor && !value.IsEmpty())
         {
            error = position + "a " + typeValue->AsString() + " action has no value";
            return false;
         }

         // The named parameter, and the value when it stands for the same one;
         // both given and different is a contradiction, not a preference.
         String to, fromName, fromAddress, subject, body, folder, headerName, scriptFunction;
         bool present = false;
         if (!ReadString(object, "to", position, present, to, error)) return false;
         if (!ReadString(object, "from_name", position, present, fromName, error)) return false;
         if (!ReadString(object, "from_address", position, present, fromAddress, error)) return false;
         if (!ReadString(object, "subject", position, present, subject, error)) return false;
         if (!ReadString(object, "body", position, present, body, error)) return false;
         if (!ReadString(object, "folder", position, present, folder, error)) return false;
         if (!ReadString(object, "header", position, present, headerName, error)) return false;
         if (!ReadString(object, "script_function", position, present, scriptFunction, error)) return false;

         if (standsFor && valuePresent && std::string(standsFor) != "value")
         {
            String *named = nullptr;
            if (std::string(standsFor) == "to") named = &to;
            else if (std::string(standsFor) == "folder") named = &folder;
            else if (std::string(standsFor) == "script_function") named = &scriptFunction;
            else if (std::string(standsFor) == "body") named = &body;

            if (named)
            {
               if (!named->IsEmpty() && named->Compare(value) != 0)
               {
                  error = position + std::string(standsFor) + " and value disagree";
                  return false;
               }
               if (named->IsEmpty())
                  *named = value;
            }
         }

         __int64 routeId = 0;
         const JsonValue *routeValue = object.Get("route_id");
         if (routeValue)
         {
            if (!routeValue->IsNumber())
            {
               error = position + "route_id must be a number";
               return false;
            }
            routeId = routeValue->AsInt64();
         }

         // Off unless said, as a new action is in the desktop dialog.
         bool abortSpamFlagged = false;
         const JsonValue *abortValue = object.Get("abort_spam_flagged");
         if (abortValue && !abortValue->IsNull())
         {
            if (!abortValue->IsBool())
            {
               error = position + "abort_spam_flagged must be true or false";
               return false;
            }
            abortSpamFlagged = abortValue->AsBool();
         }

         to.Trim();
         fromAddress.Trim();
         folder.Trim();
         headerName.Trim();
         scriptFunction.Trim();

         // What each type cannot do without. COM saves an action missing these
         // and the delivery path then forwards to nobody, moves to "" or calls
         // an unnamed function; here that is the caller's mistake, named.
         switch (type)
         {
         case RuleAction::Forward:
            if (to.IsEmpty()) { error = position + "to is required for a forward action"; return false; }
            break;
         case RuleAction::Reply:
            if (fromAddress.IsEmpty()) { error = position + "from_address is required for a reply action"; return false; }
            break;
         case RuleAction::MoveToIMAPFolder:
            if (folder.IsEmpty()) { error = position + "folder is required for a move_to_folder action"; return false; }
            break;
         case RuleAction::ScriptFunction:
            if (scriptFunction.IsEmpty()) { error = position + "script_function is required for a script_function action"; return false; }
            break;
         case RuleAction::SetHeaderValue:
            if (headerName.IsEmpty()) { error = position + "header is required for a set_header action"; return false; }
            {
               // Not X-hMailServer-LoopCount, which ends a mail loop, and not a name
               // that would write a field of another name - as COM's Save refuses it.
               const String refusal = RuleGuard::WhyHeaderMayNotBeSet(headerName);
               if (!refusal.IsEmpty()) { error = position + "header: " + std::string(Unicode::ToUtf8(refusal).c_str()); return false; }
            }
            break;
         case RuleAction::SendUsingRoute:
            if (routeId <= 0) { error = position + "route_id is required for a send_using_route action"; return false; }
            break;
         case RuleAction::BindToAddress:
            if (value.IsEmpty()) { error = position + "value is required for a bind_to_address action"; return false; }
            break;
         case RuleAction::AddDisclaimer:
            if (body.IsEmpty()) { error = position + "body is required for an add_disclaimer action: the text appended"; return false; }
            break;
         default:
            break;
         }

         // The folder as COM stores it: InterfaceRuleAction::put_IMAPFolder
         // encodes the name to modified UTF-7 because that is the form the IMAP
         // folder tree keeps, and the delivery path compares against that.
         AnsiString encodedFolder = ModifiedUTF7::Encode(folder);

         if (!WithinWidth(to, ActionTextWidth, "to", position, error)) return false;
         if (!WithinWidth(fromName, ActionTextWidth, "from_name", position, error)) return false;
         if (!WithinWidth(fromAddress, ActionTextWidth, "from_address", position, error)) return false;
         if (!WithinWidth(subject, ActionTextWidth, "subject", position, error)) return false;
         if (!WithinWidth(String(encodedFolder), ActionTextWidth, "folder", position, error)) return false;
         if (!WithinWidth(scriptFunction, ActionTextWidth, "script_function", position, error)) return false;
         if (!WithinWidth(headerName, ActionHeaderNameWidth, "header", position, error)) return false;
         if (!WithinWidth(value, ActionTextWidth, "value", position, error)) return false;

         if (type == RuleAction::Reject && HasControlCharacters(value, false))
         {
            error = position + "value must not contain control characters: it is the one line of the 550 reply";
            return false;
         }
         if (type == RuleAction::WarnSender && HasControlCharacters(value, true))
         {
            error = position + "value must not contain control characters other than line breaks and tabs";
            return false;
         }

         action = std::shared_ptr<RuleAction>(new RuleAction());
         action->SetType(type);
         action->SetSortOrder(sortOrder);
         action->SetTo(to);
         action->SetFromName(fromName);
         action->SetFromAddress(fromAddress);
         action->SetSubject(subject);
         action->SetBody(body);
         action->SetIMAPFolder(encodedFolder);
         action->SetHeaderName(headerName);
         action->SetScriptFunction(scriptFunction);
         action->SetRouteID(routeId);
         action->SetAbortSpamFlagged(abortSpamFlagged);
         action->SetValue(standsFor && std::string(standsFor) == "value" ? value : String());
         return true;
      }

      bool ParseRuleDocument(const AnsiString &requestBody, RuleDocument &document, std::string &error)
      {
         JsonValue root;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody), root, parseError))
         {
            error = "the body is not JSON: " + parseError;
            return false;
         }

         if (!root.IsObject())
         {
            error = "the body must be a JSON object";
            return false;
         }

         const JsonValue *nameValue = root.Get("name");
         if (!nameValue || !nameValue->IsString())
         {
            error = "name is required";
            return false;
         }
         Unicode::MultiByteToWide(AnsiString(nameValue->AsString()), document.name);
         document.name.Trim();
         if (document.name.IsEmpty())
         {
            error = "name is required";
            return false;
         }
         if (!WithinWidth(document.name, NameWidth, "name", "", error))
            return false;
         if (HasControlCharacters(document.name, false))
         {
            error = "name must not contain control characters: it is written into a 550 reply line and into the headers of a policy notice";
            return false;
         }

         const JsonValue *activeValue = root.Get("active");
         if (activeValue)
         {
            if (!activeValue->IsBool())
            {
               error = "active must be true or false";
               return false;
            }
            document.active = activeValue->AsBool();
         }

         const JsonValue *allCriteriaValue = root.Get("all_criteria");
         if (allCriteriaValue)
         {
            if (!allCriteriaValue->IsBool())
            {
               error = "all_criteria must be true or false";
               return false;
            }
            document.useAnd = allCriteriaValue->AsBool();
         }

         const JsonValue *criteriaValue = root.Get("criteria");
         if (criteriaValue)
         {
            if (!criteriaValue->IsArray())
            {
               error = "criteria must be an array";
               return false;
            }

            const std::vector<JsonValue> &items = criteriaValue->Items();
            for (size_t i = 0; i < items.size(); i++)
            {
               std::shared_ptr<RuleCriteria> criterion;
               if (!ParseCriterion(items[i], i, criterion, error))
                  return false;
               document.criteria.push_back(criterion);
            }
         }

         const JsonValue *actionsValue = root.Get("actions");
         if (actionsValue)
         {
            if (!actionsValue->IsArray())
            {
               error = "actions must be an array";
               return false;
            }

            // Sort order is array order, numbered from 1 as
            // InterfaceRuleAction::Save numbers each action it adds.
            const std::vector<JsonValue> &items = actionsValue->Items();
            for (size_t i = 0; i < items.size(); i++)
            {
               std::shared_ptr<RuleAction> action;
               if (!ParseAction(items[i], i, (int) i + 1, action, error))
                  return false;
               document.actions.push_back(action);
            }
         }

         return true;
      }
   }

   // The rule as the listing shows it - id, name, active, all_criteria, the
   // criteria and the actions, in the listing's words - with each action also
   // carrying the parameters its type takes, and the folder of a move decoded
   // from the modified UTF-7 the row stores, as InterfaceRuleAction::get_IMAPFolder
   // decodes it for the Control Panel.
   //
   // RestApiServer's escaper is private to it and reachable from its member
   // functions only, so the members hand it in; these helpers own no access.
   namespace
   {
      typedef AnsiString (*JsonEscapeFunction)(const AnsiString &);

      // A wide String as UTF-8 for the JSON, as RestApiServer::Utf8_ does it.
      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      AnsiString RuleEntryJson(std::shared_ptr<Rule> rule, JsonEscapeFunction escape)
      {
         AnsiString criteria = "[";
         std::shared_ptr<RuleCriterias> criterias = rule->GetCriterias();
         int criterionCount = 0;
         if (criterias)
         {
            for (int c = 0; c < criterias->GetCount(); c++)
            {
               std::shared_ptr<RuleCriteria> criterion = criterias->GetItem(c);
               if (!criterion)
                  continue;
               if (criterionCount > 0)
                  criteria += ",";
               AnsiString entry;
               entry.Format("{\"field\":\"%hs\",\"header\":\"%hs\",\"match\":\"%hs\",\"value\":\"%hs\"}",
                  criterion->GetUsePredefined() ? RuleFieldWord(criterion->GetPredefinedField()) : "header",
                  escape(Utf8(criterion->GetHeaderField())).c_str(),
                  RuleMatchWord(criterion->GetMatchType()),
                  escape(Utf8(criterion->GetMatchValue())).c_str());
               criteria += entry;
               criterionCount++;
            }
         }
         criteria += "]";

         AnsiString actions = "[";
         std::shared_ptr<RuleActions> ruleActions = rule->GetActions();
         int actionCount = 0;
         if (ruleActions)
         {
            for (int a = 0; a < ruleActions->GetCount(); a++)
            {
               std::shared_ptr<RuleAction> action = ruleActions->GetItem(a);
               if (!action)
                  continue;
               if (actionCount > 0)
                  actions += ",";

               String folder = ModifiedUTF7::Decode(AnsiString(action->GetIMAPFolder()));

               AnsiString value;
               AnsiString extra;
               switch (action->GetType())
               {
               case RuleAction::Forward:
                  value = Utf8(action->GetTo());
                  extra.Format(",\"to\":\"%hs\",\"abort_spam_flagged\":%hs", escape(value).c_str(),
                     action->GetAbortSpamFlagged() ? "true" : "false");
                  break;
               case RuleAction::Reply:
                  extra.Format(",\"from_name\":\"%hs\",\"from_address\":\"%hs\",\"subject\":\"%hs\",\"body\":\"%hs\",\"abort_spam_flagged\":%hs",
                     escape(Utf8(action->GetFromName())).c_str(),
                     escape(Utf8(action->GetFromAddress())).c_str(),
                     escape(Utf8(action->GetSubject())).c_str(),
                     escape(Utf8(action->GetBody())).c_str(),
                     action->GetAbortSpamFlagged() ? "true" : "false");
                  break;
               case RuleAction::MoveToIMAPFolder:
                  value = Utf8(folder);
                  extra.Format(",\"folder\":\"%hs\"", escape(value).c_str());
                  break;
               case RuleAction::ScriptFunction:
                  value = Utf8(action->GetScriptFunction());
                  extra.Format(",\"script_function\":\"%hs\"", escape(value).c_str());
                  break;
               case RuleAction::SetHeaderValue:
                  value = Utf8(action->GetValue());
                  extra.Format(",\"header\":\"%hs\"", escape(Utf8(action->GetHeaderName())).c_str());
                  break;
               case RuleAction::SendUsingRoute:
                  extra.Format(",\"route_id\":%I64d", action->GetRouteID());
                  break;
               case RuleAction::BindToAddress:
                  value = Utf8(action->GetValue());
                  break;
               case RuleAction::AddDisclaimer:
                  value = Utf8(action->GetBody());
                  extra.Format(",\"body\":\"%hs\"", escape(value).c_str());
                  break;
               case RuleAction::Reject:
               case RuleAction::WarnSender:
                  value = Utf8(action->GetValue());
                  break;
               default:
                  break;
               }

               AnsiString entry;
               entry.Format("{\"type\":\"%hs\",\"value\":\"%hs\"%hs}",
                  RuleActionWord(action->GetType()), escape(value).c_str(), extra.c_str());
               actions += entry;
               actionCount++;
            }
         }
         actions += "]";

         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"name\":\"%hs\",\"active\":%hs,\"all_criteria\":%hs,\"criteria\":%hs,\"actions\":%hs}",
            rule->GetID(),
            escape(Utf8(rule->GetName())).c_str(),
            rule->GetActive() ? "true" : "false",
            rule->GetUseAND() ? "true" : "false",
            criteria.c_str(), actions.c_str());
         return entry;
      }

      // The 400 body for a sentence the parser wrote: escaped once, here, so
      // the caller's own word inside it cannot break out of the string.
      AnsiString RefusalBody(const std::string &sentence, JsonEscapeFunction escape)
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", escape(AnsiString(sentence)).c_str());
         return body;
      }

      // Fills a rule's own collections from a parsed document, the way
      // InterfaceRuleCriteria::Save and InterfaceRuleAction::Save add each part
      // to the rule's collection before InterfaceRule::Save writes them all.
      // The delivery path's cache of the collection a write touched: the
      // global rules, or one account's.
      void FlagRulesReload(__int64 accountId, __int64 domainId = 0)
      {
         if (domainId > 0)
            ObjectCache::Instance()->SetDomainRulesNeedsReload(domainId);
         else if (accountId == 0)
            ObjectCache::Instance()->SetGlobalRulesNeedsReload();
         else
            ObjectCache::Instance()->SetAccountRulesNeedsReload(accountId);
      }

      void AttachParts(std::shared_ptr<Rule> rule, const RuleDocument &document)
      {
         std::shared_ptr<RuleCriterias> criterias = rule->GetCriterias();
         for (size_t i = 0; i < document.criteria.size(); i++)
         {
            document.criteria[i]->SetRuleID(rule->GetID());
            criterias->AddItem(document.criteria[i]);
         }

         std::shared_ptr<RuleActions> actions = rule->GetActions();
         for (size_t i = 0; i < document.actions.size(); i++)
         {
            document.actions[i]->SetRuleID(rule->GetID());
            actions->AddItem(document.actions[i]);
         }
      }
   }

   AnsiString
   RestApiServer::RuleEntryJson_(const std::shared_ptr<Rule> &rule)
   {
      return RuleEntryJson(rule, &RestApiServer::JsonEscape_);
   }

   AnsiString
   RestApiServer::OpenApiRulesPaths_()
   {
      // The whole /api/v1/rules entry lives here, both verbs: the head of the
      // document in HandleOpenApi_ no longer carries the path, so the key
      // appears once.
      static const char *paths =
         ",\"/api/v1/rules\":{"
         "\"get\":{\"summary\":\"List the global rules with their criteria and actions\",\"description\":\"Each entry: id, name, active, all_criteria, criteria (field, header, match, value) and actions (type, value, and the parameters the type takes beside it - a forward's and a reply's abort_spam_flagged among them). Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of rules\"}}},"
         "\"post\":{\"summary\":\"Create a global rule\",\"description\":\"Body: name (required, at most 100 characters, no control characters: it is written into a 550 reply and into the headers of a policy notice), active (default true), all_criteria (default true: every criterion must match; false: any one), criteria and actions as arrays of objects in the order they run. A criterion: field (from, to, cc, subject, body, message_size, recipient_list, delivery_attempts, attachment_name, sensitive_content, attachment_type, external_recipient, content, or header with the header's name in header), match (equals, not_equals, contains, not_contains, less_than, greater_than, regex, wildcard) and value (at most 2000 characters; a regex must compile). An action: type and the parameters that type takes - forward: to and abort_spam_flagged; reply: from_name, from_address (required), subject, body and abort_spam_flagged; move_to_folder: folder; script_function: script_function; set_header: header (printable ASCII without a colon, and not X-hMailServer-LoopCount, the count that ends a mail loop) and value (a line break in it that does not fold the line is written as a space); send_using_route: route_id; bind_to_address: value; add_disclaimer: body (the text appended to the message's plain and HTML parts); reject: value (one line, no control characters: the text of the 550 the SMTP session answers with, before the message is accepted, when the rule matches there, and of the 403 policy_rejected the self-service send route answers with before it queues; a message that reached delivery another way is dropped, and a sender this server hosts is mailed a notice saying it was not sent); warn_sender: value (the text of a notice mailed to the sender naming the rule, once per message on its first delivery attempt and not again on a release from the quarantine, only when this server hosts the sender's domain - the message itself is not held by this action); require_tls, quarantine, delete, stop and copy take none. The sensitive_content condition is the detectors that found something in the readable text, with ; between them: card_number (13 to 19 digits that pass the Luhn check), iban (a country in the IBAN registry, its length and check digits) and account_number (the dlp_account_number_pattern setting's regular expression); attachment_type is every attachment's type by its first bytes rather than its name (executable, elf, pdf, zip, office_xml, jar, office, rar, 7z, gzip, png, jpeg, gif, rtf, script, text, empty, binary); external_recipient is every recipient this server does not host; content is the readable text itself - the plain and HTML parts and every attachment whose bytes are text - for a pattern of the rule's own; a sealed message declines all but attachment_type and external_recipient, as the body condition does. value is also accepted as the parameter the listing shows under that name. An unknown key, word or type, a parameter the type does not take, or a missing one it needs, is refused naming it. Saved as the Control Panel saves a rule; it applies to the next message delivered. Server-wide; refused for domain-restricted keys.\","
         "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"},\"active\":{\"type\":\"boolean\"},\"all_criteria\":{\"type\":\"boolean\"},\"criteria\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"required\":[\"field\",\"match\",\"value\"],\"properties\":{\"field\":{\"type\":\"string\",\"enum\":[\"from\",\"to\",\"cc\",\"subject\",\"body\",\"message_size\",\"recipient_list\",\"delivery_attempts\",\"attachment_name\",\"sensitive_content\",\"attachment_type\",\"external_recipient\",\"content\",\"header\"]},\"header\":{\"type\":\"string\"},\"match\":{\"type\":\"string\",\"enum\":[\"equals\",\"contains\",\"less_than\",\"greater_than\",\"regex\",\"not_contains\",\"not_equals\",\"wildcard\"]},\"value\":{\"type\":\"string\"}}}},\"actions\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"required\":[\"type\"],\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"delete\",\"forward\",\"reply\",\"move_to_folder\",\"script_function\",\"stop\",\"set_header\",\"send_using_route\",\"copy\",\"bind_to_address\",\"add_disclaimer\",\"reject\",\"require_tls\",\"quarantine\",\"warn_sender\"]},\"value\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"},\"from_name\":{\"type\":\"string\"},\"from_address\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"folder\":{\"type\":\"string\"},\"header\":{\"type\":\"string\"},\"route_id\":{\"type\":\"integer\"},\"script_function\":{\"type\":\"string\"},\"abort_spam_flagged\":{\"type\":\"boolean\"}}}}}}}}},"
         "\"responses\":{\"201\":{\"description\":\"Created: the rule as the listing shows it, with its id, and each action's parameters beside its value\"},\"400\":{\"description\":\"name missing, the body not JSON, or a criterion or action refused (the reason names it in error)\"}}}},"
         "\"/api/v1/rules/{id}\":{"
         "\"put\":{\"summary\":\"Replace a global rule\",\"description\":\"The same body as POST. The rule's name, flags, criteria and actions are all replaced - the old criteria and actions are deleted and the new ones created, which is what saving an edited rule in the Control Panel comes to - and its place in the order is kept. A rule that belongs to an account is not reachable here.\","
         "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"},\"active\":{\"type\":\"boolean\"},\"all_criteria\":{\"type\":\"boolean\"},\"criteria\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"required\":[\"field\",\"match\",\"value\"],\"properties\":{\"field\":{\"type\":\"string\",\"enum\":[\"from\",\"to\",\"cc\",\"subject\",\"body\",\"message_size\",\"recipient_list\",\"delivery_attempts\",\"attachment_name\",\"sensitive_content\",\"attachment_type\",\"external_recipient\",\"content\",\"header\"]},\"header\":{\"type\":\"string\"},\"match\":{\"type\":\"string\",\"enum\":[\"equals\",\"contains\",\"less_than\",\"greater_than\",\"regex\",\"not_contains\",\"not_equals\",\"wildcard\"]},\"value\":{\"type\":\"string\"}}}},\"actions\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"required\":[\"type\"],\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"delete\",\"forward\",\"reply\",\"move_to_folder\",\"script_function\",\"stop\",\"set_header\",\"send_using_route\",\"copy\",\"bind_to_address\",\"add_disclaimer\",\"reject\",\"require_tls\",\"quarantine\",\"warn_sender\"]},\"value\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"},\"from_name\":{\"type\":\"string\"},\"from_address\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"folder\":{\"type\":\"string\"},\"header\":{\"type\":\"string\"},\"route_id\":{\"type\":\"integer\"},\"script_function\":{\"type\":\"string\"},\"abort_spam_flagged\":{\"type\":\"boolean\"}}}}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"The rule as saved\"},\"400\":{\"description\":\"As for POST; the rule is unchanged\"},\"404\":{\"description\":\"No global rule with that id\"}}},"
         "\"delete\":{\"summary\":\"Delete a global rule\",\"description\":\"With its criteria and actions, as the Control Panel deletes one. A rule that belongs to an account is not reachable here.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"No global rule with that id\"}}}},"
         "\"/api/v1/rules/match\":{\"post\":{\"summary\":\"Try a rule criterion against a value, without a rule\",\"description\":\"Body: match_value (the criterion's value, as a rule stores it), match_type (equals, contains, less_than, greater_than, regex, not_contains, not_equals or wildcard - the words the rule listing uses) and test_value (what the criterion is tried against). match is what the delivery path would decide for a criterion of that value and type meeting that text. What Utilities.CriteriaMatch answers over COM: nothing is read or written. Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"match_value\",\"match_type\",\"test_value\"],\"properties\":{\"match_value\":{\"type\":\"string\"},\"match_type\":{\"type\":\"string\"},\"test_value\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{match}\"},\"400\":{\"description\":\"A field missing or of the wrong type, a match_type not among the eight words, or an unknown field (error names it)\"}}}}";

      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleCreateRule_(__int64 accountId, const AnsiString &requestBody, __int64 domainId)
   {
      RuleDocument document;
      std::string error;
      if (!ParseRuleDocument(requestBody, document, error))
         return BuildResponse_(400, RefusalBody(error, &RestApiServer::JsonEscape_));

      // The collection, read as InterfaceApplication::get_Rules reads the global
      // one for the Control Panel and InterfaceAccount::get_Rules an account's:
      // a fresh Rules(accountId) - 0 is the global rules - so the sort order the
      // new rule gets is one past the last rule that exists now, as
      // InterfaceRule::Save gives it.
      Rules rules(accountId, domainId);
      rules.Refresh();

      std::vector<std::shared_ptr<Rule> > existing = rules.GetSnapshot();
      int sortOrder = existing.empty() ? 1 : existing.back()->GetSortOrder() + 1;

      std::shared_ptr<Rule> rule = std::shared_ptr<Rule>(new Rule());
      rule->SetAccountID(accountId);
      rule->SetDomainID(domainId);
      rule->SetName(document.name);
      rule->SetActive(document.active);
      rule->SetUseAND(document.useAnd);
      rule->SetSortOrder(sortOrder);
      AttachParts(rule, document);

      String saveError;
      if (!PersistentRule::SaveObject(rule, saveError, PersistenceModeNormal))
      {
         // PersistentRule has already said what it could in the error log, and
         // left a half-written rule inactive rather than live.
         LOG_APPLICATION(String(_T("RestApi: Failed to create rule '")) + document.name + _T("'."));
         return BuildResponse_(500, "{\"error\":\"failed to save rule\"}");
      }

      String logLine;
      logLine.Format(_T("RestApi: Rule '%s' (id %I64d) created with %d criteria and %d actions."),
         rule->GetName().c_str(), rule->GetID(), (int) document.criteria.size(), (int) document.actions.size());
      LOG_APPLICATION(logLine);

      return BuildResponse_(201, RuleEntryJson(rule, &RestApiServer::JsonEscape_));
   }

   HttpResponse
   RestApiServer::HandleUpdateRule_(__int64 accountId, __int64 ruleId, const AnsiString &requestBody, __int64 domainId)
   {
      // The one collection the route names. Rules(0) holds the rules with no
      // account and Rules(id) one account's, so a rule that belongs elsewhere -
      // another account's under this address, an account's under /api/v1/rules
      // - is simply not found here, and answered as such, whatever the body says.
      Rules rules(accountId, domainId);
      rules.Refresh();

      std::shared_ptr<Rule> existing = rules.GetItemByDBID(ruleId);
      if (!existing)
         return BuildResponse_(404, "{\"error\":\"rule not found\"}");

      // Parsed in full before the rule is touched: a 400 leaves it exactly as
      // it was.
      RuleDocument document;
      std::string error;
      if (!ParseRuleDocument(requestBody, document, error))
         return BuildResponse_(400, RefusalBody(error, &RestApiServer::JsonEscape_));

      // The old criteria and actions go, then the new are written beneath the
      // same row by the same cascade a new rule's are. Between the two the row
      // is set inactive by SaveObject itself, and the delivery path is still
      // reading the collection it loaded before - the reload is flagged only
      // once the save has completed - so no message is judged against a rule
      // that is half replaced.
      if (!PersistentRuleCriteria::DeleteObjects(ruleId) || !PersistentRuleAction::DeleteObjects(ruleId))
      {
         // The table may now disagree with what the delivery path holds; make
         // it read the table again rather than keep serving parts that are gone.
         FlagRulesReload(accountId, domainId);
         LOG_APPLICATION(String(_T("RestApi: Failed to replace the criteria and actions of rule '")) + existing->GetName() + _T("'."));
         return BuildResponse_(500, "{\"error\":\"failed to replace the rule's criteria and actions\"}");
      }

      // A new Rule object with the row's id, not the loaded one: the loaded
      // one's collections still hold the parts just deleted, and SaveObject
      // would write them again. This one's collections load from the table,
      // which is now empty beneath this id, and take the document's parts.
      std::shared_ptr<Rule> rule = std::shared_ptr<Rule>(new Rule());
      rule->SetID(existing->GetID());
      rule->SetAccountID(accountId);
      rule->SetDomainID(domainId);
      rule->SetSortOrder(existing->GetSortOrder());
      rule->SetName(document.name);
      rule->SetActive(document.active);
      rule->SetUseAND(document.useAnd);
      AttachParts(rule, document);

      String saveError;
      if (!PersistentRule::SaveObject(rule, saveError, PersistenceModeNormal))
      {
         FlagRulesReload(accountId, domainId);
         LOG_APPLICATION(String(_T("RestApi: Failed to update rule '")) + document.name + _T("'."));
         return BuildResponse_(500, "{\"error\":\"failed to save rule\"}");
      }

      String logLine;
      logLine.Format(_T("RestApi: Rule '%s' (id %I64d) replaced with %d criteria and %d actions, active: %s."),
         rule->GetName().c_str(), rule->GetID(), (int) document.criteria.size(), (int) document.actions.size(),
         rule->GetActive() ? _T("true") : _T("false"));
      LOG_APPLICATION(logLine);

      return BuildResponse_(200, RuleEntryJson(rule, &RestApiServer::JsonEscape_));
   }

   HttpResponse
   RestApiServer::HandleDeleteRule_(__int64 accountId, __int64 ruleId, __int64 domainId)
   {
      Rules rules(accountId, domainId);
      rules.Refresh();

      std::shared_ptr<Rule> rule = rules.GetItemByDBID(ruleId);
      if (!rule)
         return BuildResponse_(404, "{\"error\":\"rule not found\"}");

      String name = rule->GetName();

      // Through the collection, as InterfaceRule::Delete goes through its
      // parent: Collection::DeleteItemByDBID calls PersistentRule::DeleteObject,
      // which removes the row, its actions and its criteria and flags the
      // global rules for reload.
      if (!rules.DeleteItemByDBID(ruleId))
         return BuildResponse_(500, "{\"error\":\"failed to delete rule\"}");

      LOG_APPLICATION(String(_T("RestApi: Rule '")) + name + _T("' deleted."));

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   // An account's own rules, under the account: the same documents, the same
   // checks and the same handlers as the global rules, on the collection
   // Rules(accountId) - what Account.Rules reaches over COM. Scoped to the
   // address's domain by DomainNamedByRoute_, permissioned as the account's
   // other writes are.
   HttpResponse
   RestApiServer::HandleAccountRules_(const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(route.identifier)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      switch (route.kind)
      {
      case RouteAccountRuleList:
         return HandleListRules_(account->GetID());
      case RouteAccountRuleCreate:
         return HandleCreateRule_(account->GetID(), requestBody);
      case RouteAccountRuleUpdate:
         return HandleUpdateRule_(account->GetID(), route.record_id, requestBody);
      case RouteAccountRuleDelete:
         return HandleDeleteRule_(account->GetID(), route.record_id);
      default:
         return BuildResponse_(404, "{\"error\":\"not found\"}");
      }
   }

   // A domain's own rules, under the domain: the same documents and handlers
   // on the collection Rules(0, domainId) - what Domain.Rules reaches over
   // COM. Scoped to the domain by DomainNamedByRoute_, so a domain
   // administrator's key reaches its own domain's rules and no other's.
   HttpResponse
   RestApiServer::HandleDomainRules_(const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(String(route.identifier));
      if (!domain || domain->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      switch (route.kind)
      {
      case RouteDomainRuleList:
         return HandleListRules_(0, domain->GetID());
      case RouteDomainRuleCreate:
         return HandleCreateRule_(0, requestBody, domain->GetID());
      case RouteDomainRuleUpdate:
         return HandleUpdateRule_(0, route.record_id, requestBody, domain->GetID());
      case RouteDomainRuleDelete:
         return HandleDeleteRule_(0, route.record_id, domain->GetID());
      default:
         return BuildResponse_(404, "{\"error\":\"not found\"}");
      }
   }

   AnsiString
   RestApiServer::OpenApiDomainRulesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{name}/rules\":{"
         "\"get\":{\"summary\":\"List a domain's own rules\",\"description\":\"The rules that run for this domain's mail - every message with a recipient in the domain or a sender it hosts - after the server's rules and before delivery, in the order they run, with the same criteria and actions as GET /api/v1/rules shows. A domain-restricted key sees its own domains'.\",\"responses\":{\"200\":{\"description\":\"rules\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"post\":{\"summary\":\"Create a rule for a domain\",\"description\":\"The same body POST /api/v1/rules takes; the rule is the domain's, appended to its order, and runs for the domain's mail alone.\",\"responses\":{\"201\":{\"description\":\"Created\"},\"400\":{\"description\":\"As for POST /api/v1/rules\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domains/{name}/rules/{id}\":{"
         "\"put\":{\"summary\":\"Replace one of a domain's rules\",\"description\":\"The whole rule, as PUT /api/v1/rules/{id} replaces a global one. A rule that is not this domain's is 404.\",\"responses\":{\"200\":{\"description\":\"The rule as saved\"},\"400\":{\"description\":\"As for POST; the rule is unchanged\"},\"404\":{\"description\":\"No such rule of this domain\"}}},"
         "\"delete\":{\"summary\":\"Delete one of a domain's rules\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"No such rule of this domain\"}}}}";

      return AnsiString(paths);
   }

   AnsiString
   RestApiServer::OpenApiAccountRulesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/accounts/{address}/rules\":{"
         "\"get\":{\"summary\":\"List an account's own rules\",\"description\":\"The rules that run for this account's mail alone, in the order they run, each as GET /api/v1/rules lists a global rule - what Account.Rules holds over COM. Scoped to the address's domain.\",\"responses\":{\"200\":{\"description\":\"The rules\"},\"404\":{\"description\":\"Unknown account\"}}},"
         "\"post\":{\"summary\":\"Create a rule for an account\",\"description\":\"The same body POST /api/v1/rules takes - name, active, all_criteria, criteria and actions - saved under the account and in force for its next message. Scoped to the address's domain.\",\"responses\":{\"201\":{\"description\":\"The rule as saved\"},\"400\":{\"description\":\"The body was refused, with the reason\"},\"404\":{\"description\":\"Unknown account\"}}}}"
         ",\"/api/v1/accounts/{address}/rules/{id}\":{"
         "\"put\":{\"summary\":\"Replace one of an account's rules\",\"description\":\"The whole rule, as PUT /api/v1/rules/{id} replaces a global one; a rule that is not this account's is not found here.\",\"responses\":{\"200\":{\"description\":\"The rule as saved\"},\"400\":{\"description\":\"The body was refused, with the reason\"},\"404\":{\"description\":\"Unknown account or rule\"}}},"
         "\"delete\":{\"summary\":\"Delete one of an account's rules\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown account or rule\"}}}}";

      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleRuleMatch_(const AnsiString &requestBody)
   {
      // Utilities.CriteriaMatch over COM, to the call: RuleApplier::TestMatch,
      // which is what the delivery path asks of every criterion. The three
      // fields are all required - a criterion with no value, no type or
      // nothing to try it against is not a question - and the type is one of
      // the eight words the rule listing emits, so what a client reads from
      // GET /api/v1/rules is what it may send here.
      JsonValue body;
      std::string error;
      if (!JsonValue::Parse(std::string(requestBody), body, error) || !body.IsObject())
         return BuildResponse_(400, RefusalBody("the body must be a JSON object", &RestApiServer::JsonEscape_));

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         if (member.first != "match_value" && member.first != "match_type" && member.first != "test_value")
            return BuildResponse_(400, RefusalBody("unknown field: " + member.first.substr(0, 48), &RestApiServer::JsonEscape_));
      }

      String matchValue, matchType, testValue;
      bool present = false;
      if (!ReadString(body, "match_value", "", present, matchValue, error))
         return BuildResponse_(400, RefusalBody(error, &RestApiServer::JsonEscape_));
      if (!present)
         return BuildResponse_(400, RefusalBody("match_value is required", &RestApiServer::JsonEscape_));
      if (!ReadString(body, "match_type", "", present, matchType, error))
         return BuildResponse_(400, RefusalBody(error, &RestApiServer::JsonEscape_));
      if (!present)
         return BuildResponse_(400, RefusalBody("match_type is required", &RestApiServer::JsonEscape_));
      if (!ReadString(body, "test_value", "", present, testValue, error))
         return BuildResponse_(400, RefusalBody(error, &RestApiServer::JsonEscape_));
      if (!present)
         return BuildResponse_(400, RefusalBody("test_value is required", &RestApiServer::JsonEscape_));

      RuleCriteria::MatchType match = RuleCriteria::None;
      if (!RuleMatchFromWord(std::string(AnsiString(matchType)), match))
         return BuildResponse_(400, RefusalBody("match_type must be equals, contains, less_than, greater_than, regex, not_contains, not_equals or wildcard", &RestApiServer::JsonEscape_));

      const bool matched = RuleApplier::TestMatch(matchValue, match, testValue);

      return BuildResponse_(200, matched ? "{\"match\":true}" : "{\"match\":false}");
   }
}
