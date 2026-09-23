// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's domain update in full, the domain aliases, the distribution list create and update, and the IP range update: what the Control Panel's domain, list and IP range pages do. See RestApiServer.h.

// The REST API's domain update in full, the domain aliases and the IP range update: what the Control Panel's domain and IP range pages do. See RestApiServer.h.
//
// Four things the write surface of wave 162 left out, each the whole of a
// COM object rather than two of its fields:
//
//   - POST /api/v1/domains/{domain}/lists and PUT /api/v1/lists/{address}:
//     every scalar InterfaceDistributionList saves - active, the mode, the
//     required sender, the moderator and the bounce address - where the
//     create once fixed public and active whatever the body said; the mode
//     takes the four words the server implements and refuses anything else
//     as put_Mode does. The entry every list route answers with is here too.
//
//   - PUT /api/v1/domains/{domain} took active and postmaster and said of
//     itself that the name could not be changed there. It now takes every
//     scalar InterfaceDomain saves - the limits and their switches, plus
//     addressing, greylisting, the signature, DKIM, retention, the relay and
//     the domain's automatic reply - and a new name: renaming is
//     Domain::SetName and PersistentDomain::SaveObject, which runs the same
//     cascade (NameChanger::RenameDomain) that InterfaceDomain::put_Name and
//     Save run, so every address in the domain follows it. active stays
//     required, as the route always had it, so that the callers written to
//     the two-field contract keep working unchanged.
//   - GET/POST /api/v1/domains/{domain}/domain-aliases and DELETE
//     .../domain-aliases/{name}: InterfaceDomainAliases::Add + Save, and
//     Delete, through PersistentDomainAlias, which marks the alias cache for
//     reload so the next RCPT TO sees the alias.
//   - PUT /api/v1/ipranges/{id}: the setters and Save of InterfaceSecurityRange,
//     through PersistentSecurityRange::SaveObject with its own limitation
//     check; a field left out keeps its value.
//
// Bodies are parsed as JSON documents. A member present with the wrong type,
// or a key that is not a field, is a 400 naming it, and nothing is applied
// until everything in the body has been read.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "SessionPolicies.h"
#include "CountryBlocking.h"
#include "RestrictedDelivery.h"
#include "ImpersonationCheck.h"
#include "RestApiSettingLevels.h"
#include "../Persistence/PersistentSettingOverride.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/Domains.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../BO/DomainAlias.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/SecurityRanges.h"
#include "../BO/SecurityRange.h"
#include "../Cache/CacheContainer.h"
#include "../Util/DistributionListMembership.h"
#include "../Persistence/PersistentDomain.h"
#include "../Persistence/PersistentDomainAlias.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../Persistence/PersistenceMode.h"
#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../AntiSpam/DKIM/DkimRotationPlan.h"
#include "../TCPIP/IPAddress.h"
#include "Time.h"
#include "VariantDateTime.h"

#include <cmath>
#include <functional>
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
      typedef std::function<AnsiString(const String &)> Quote;

      AnsiString ErrorBody(const Quote &quote, const String &sentence)
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", quote(sentence).c_str());
         return body;
      }

      String Utf8ToString(const std::string &utf8)
      {
         String value;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
         return value;
      }

      bool ParseObjectBody(const AnsiString &requestBody, JsonValue &body)
      {
         std::string parseError;
         std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }

      // The typed reads: untouched when the member is absent or null; false,
      // with error naming the member, when it is present with another type.
      bool ReadString(const JsonValue &object, const char *key, String &out, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsString())
         {
            error.Format("%hs must be a string", key);
            return false;
         }

         out = Utf8ToString(member->AsString());
         return true;
      }

      bool ReadBool(const JsonValue &object, const char *key, bool &out, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsBool())
         {
            error.Format("%hs must be true or false", key);
            return false;
         }

         out = member->AsBool();
         return true;
      }

      bool ReadInteger(const JsonValue &object, const char *key, long minimum, long maximum, long &out, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         double number = member->IsNumber() ? member->AsNumber() : 0.0;

         if (!member->IsNumber() || std::floor(number) != number ||
             number < (double) minimum || number > (double) maximum)
         {
            error.Format("%hs must be a whole number between %ld and %ld", key, minimum, maximum);
            return false;
         }

         out = (long) number;
         return true;
      }

      bool UnknownKey(const JsonValue &object, const char *const *known, size_t knownCount, AnsiString &error,
                      bool (*alsoKnown)(const std::string &) = nullptr)
      {
         for (const std::pair<std::string, JsonValue> &member : object.Members())
         {
            bool found = false;
            for (size_t i = 0; i < knownCount; i++)
            {
               if (member.first == known[i])
               {
                  found = true;
                  break;
               }
            }
            if (!found && alsoKnown && alsoKnown(member.first))
               found = true;
            // What the entry answers about the levels - <name>_effective and
            // <name>_source - is ignored when a caller sends the entry back, as
            // HandleGetDomain_ says a caller may.
            if (!found && SettingLevelsRest::IsReadOnlyKey(member.first))
               continue;
            if (!found)
            {
               error = AnsiString("unknown field: ") + AnsiString(member.first.substr(0, 48).c_str());
               return true;
            }
         }
         return false;
      }

      // A setting with levels whose domain level the overlay holds
      // (RestApiSettingLevels.h): a member the domain route takes beside its own
      // fields, read and written apart from them.
      bool DomainOverlayKey(const std::string &key)
      {
         return SettingLevelsRest::IsOverlayKey(key, PersistentSettingOverride::ScopeDomain);
      }

      const char *ConnectionSecurityWord(ConnectionSecurity security)
      {
         switch (security)
         {
         case CSSSL:
            return "tls";
         case CSSTARTTLSOptional:
            return "starttls_optional";
         case CSSTARTTLSRequired:
            return "starttls_required";
         case CSNone:
         default:
            return "none";
         }
      }

      bool ParseConnectionSecurityWord(const String &word, ConnectionSecurity &security)
      {
         AnsiString value = word;

         if (value.CompareNoCase("none") == 0)
            security = CSNone;
         else if (value.CompareNoCase("tls") == 0)
            security = CSSSL;
         else if (value.CompareNoCase("starttls_optional") == 0)
            security = CSSTARTTLSOptional;
         else if (value.CompareNoCase("starttls_required") == 0)
            security = CSSTARTTLSRequired;
         else
            return false;

         return true;
      }

      const char *SignatureMethodWord(Domain::DomainSignatureMethod method)
      {
         switch (method)
         {
         case Domain::SMOverwriteAccountSignature:
            return "overwrite";
         case Domain::SMSetIfNotSpecifiedInAccount:
            return "set_if_not_specified";
         case 3:
            // eSMAppendToAccountSignature over COM, a value the business object's
            // enum does not name but the Control Panel stores.
            return "append";
         default:
            return "unknown";
         }
      }

      const char *DkimAlgorithmWord(int algorithm)
      {
         // HashCreator::HashType: SHA1 is 1, SHA256 is 2. Nothing else signs.
         return algorithm == 1 ? "sha1" : "sha256";
      }

      // eDKIMCanonicalizationMethod over COM: simple is 1, relaxed is 2, and
      // Domain stores nothing else (a value that is not 1 reads back as 2).
      const char *CanonicalizationWord(int method)
      {
         return method == 1 ? "simple" : "relaxed";
      }

      bool ParseCanonicalizationWord(const String &word, int &method)
      {
         AnsiString value = word;

         if (value.CompareNoCase("simple") == 0)
            method = 1;
         else if (value.CompareNoCase("relaxed") == 0)
            method = 2;
         else
            return false;

         return true;
      }

      // The domain's members, the names the API uses. The relay password is
      // accepted and never emitted.
      const char *const DomainKeys[] =
      {
         "name", "active", "postmaster", "max_message_size_kb", "max_size_mb", "max_account_size_mb",
         "max_accounts", "max_aliases", "max_lists", "max_accounts_enabled", "max_aliases_enabled", "max_lists_enabled",
         "plus_addressing_enabled", "plus_addressing_character", "use_greylisting",
         "signature_enabled", "signature_method", "signature_plain_text", "signature_html",
         "signature_add_to_replies", "signature_add_to_local_mail",
         "dkim_enabled", "dkim_selector", "dkim_private_key_file", "dkim_signing_algorithm",
         "dkim_header_canonicalization", "dkim_body_canonicalization", "dkim_secondary_selector",
         "dkim_secondary_private_key_file", "dkim_sign_aliases",
         "dkim_rotation_enabled", "dkim_rotation_interval_days", "dkim_rotation_propagation_hours",
         "dkim_rotation_retention_days", "dkim_rotation_key_bits",
         "message_retention_days", "relay_host", "relay_port", "relay_requires_auth", "relay_username",
         "relay_password", "relay_connection_security",
         "vacation_enabled", "vacation_subject", "vacation_message",
         "vacation_internal_subject", "vacation_internal_message", "vacation_external_override", "vacation_days",
         "quota_warning_percent", "quota_warning_subject", "quota_warning_text", "quota_grace_percent", "quota_send_stop_percent",
         "ad_domain_name",
         "external_tag_subject", "external_tag_header", "external_tag_text", "first_contact_tip",
         "disclaimer_enabled", "disclaimer_plain_text", "disclaimer_html",
         "quarantine_digest", "quarantine_digest_hour",
         "openpgp_publish", "openpgp_publish_new_accounts",
         "webhooks_enabled", "webhook_subjects",
         "country_policy",
         "session_idle_seconds", "session_lifetime_seconds", "session_one_per_device", "session_bind_to_address",
         "report_mailbox", "restricted_domains",
         "impersonation_action", "protected_persons", "protected_partners"
      };

      // Applies the body to the domain. Everything is read and checked first;
      // the first problem is the answer and nothing has been applied when it
      // is given. renamed says whether the name changed, for the log line.
      // callerDomains is the list a confined credential reaches (empty for one
      // that is not confined to a list), and forbidden is set with error when
      // the refusal is about the caller's authority rather than the body, so
      // the handler answers 403 and not 400.
      bool ApplyDomainBody(const JsonValue &body, std::shared_ptr<Domain> domain, const std::vector<String> &callerDomains,
                           AnsiString &error, bool &renamed, bool &forbidden)
      {
         forbidden = false;

         if (UnknownKey(body, DomainKeys, sizeof(DomainKeys) / sizeof(DomainKeys[0]), error, &DomainOverlayKey))
            return false;

         String name = domain->GetName(), postmaster = domain->GetPostmaster();
         String plusChar = domain->GetPlusAddressingChar();
         String signaturePlain = domain->GetSignaturePlainText(), signatureHtml = domain->GetSignatureHTML();
         String signatureMethod, dkimAlgorithm, relaySecurity;
         String dkimSelector = String(domain->GetDKIMSelector()), dkimKeyFile = domain->GetDKIMPrivateKeyFile();
         String relayHost = domain->GetRelayHost(), relayUser = domain->GetRelayUsername(), relayPassword = domain->GetRelayPassword();
         String vacationSubject = domain->GetVacationSubject(), vacationMessage = domain->GetVacationMessage();
         String headerCanonicalization, bodyCanonicalization;
         String dkimSecondarySelector = String(domain->GetDKIMSecondarySelector()), dkimSecondaryKeyFile = domain->GetDKIMSecondaryPrivateKeyFile();
         String vacationInternalSubject = domain->GetVacationInternalSubject(), vacationInternalMessage = domain->GetVacationInternalMessage();
         long vacationDays = domain->GetVacationDays();
         long quotaWarningPercent = domain->GetQuotaWarningPercent(), quotaGracePercent = domain->GetQuotaGracePercent();
         long quotaSendStopPercent = domain->GetQuotaSendStopPercent();
         String quotaWarningSubject = domain->GetQuotaWarningSubject(), quotaWarningText = domain->GetQuotaWarningText();
         String adDomainName = domain->GetADDomainName();
         String externalTagText = domain->GetExternalTagText();
         String disclaimerPlainText = domain->GetDisclaimerPlainText(), disclaimerHtml = domain->GetDisclaimerHTML();
         String countryPolicy = domain->GetCountryPolicy();
         String reportMailbox = domain->GetReportMailbox();
         String restrictedDomains = domain->GetRestrictedDomains();
         String protectedPersons = domain->GetProtectedPersons(), protectedPartners = domain->GetProtectedPartners();
         int impersonationAction = domain->GetImpersonationAction();

         if (!ReadString(body, "name", name, error) ||
             !ReadString(body, "postmaster", postmaster, error) ||
             !ReadString(body, "plus_addressing_character", plusChar, error) ||
             !ReadString(body, "signature_method", signatureMethod, error) ||
             !ReadString(body, "signature_plain_text", signaturePlain, error) ||
             !ReadString(body, "signature_html", signatureHtml, error) ||
             !ReadString(body, "dkim_selector", dkimSelector, error) ||
             !ReadString(body, "dkim_private_key_file", dkimKeyFile, error) ||
             !ReadString(body, "dkim_signing_algorithm", dkimAlgorithm, error) ||
             !ReadString(body, "dkim_header_canonicalization", headerCanonicalization, error) ||
             !ReadString(body, "dkim_body_canonicalization", bodyCanonicalization, error) ||
             !ReadString(body, "dkim_secondary_selector", dkimSecondarySelector, error) ||
             !ReadString(body, "dkim_secondary_private_key_file", dkimSecondaryKeyFile, error) ||
             !ReadString(body, "relay_host", relayHost, error) ||
             !ReadString(body, "relay_username", relayUser, error) ||
             !ReadString(body, "relay_password", relayPassword, error) ||
             !ReadString(body, "relay_connection_security", relaySecurity, error) ||
             !ReadString(body, "vacation_subject", vacationSubject, error) ||
             !ReadString(body, "vacation_message", vacationMessage, error) ||
             !ReadString(body, "vacation_internal_subject", vacationInternalSubject, error) ||
             !ReadString(body, "vacation_internal_message", vacationInternalMessage, error) ||
             !ReadString(body, "quota_warning_subject", quotaWarningSubject, error) ||
             !ReadString(body, "quota_warning_text", quotaWarningText, error) ||
             !ReadString(body, "ad_domain_name", adDomainName, error) ||
             !ReadString(body, "external_tag_text", externalTagText, error) ||
             !ReadString(body, "disclaimer_plain_text", disclaimerPlainText, error) ||
             !ReadString(body, "disclaimer_html", disclaimerHtml, error) ||
             !ReadString(body, "country_policy", countryPolicy, error) ||
             !ReadString(body, "report_mailbox", reportMailbox, error) ||
             !ReadString(body, "restricted_domains", restrictedDomains, error) ||
             !ReadString(body, "protected_persons", protectedPersons, error) ||
             !ReadString(body, "protected_partners", protectedPartners, error))
            return false;

         // Impersonation protection (ImpersonationCheck.h): the action is 0, 1
         // or 2, or the words off, flag or quarantine; the two lists are
         // canonicalised and refused in the unit's own sentences. Each is
         // judged only when the body names it, so a PUT of another field never
         // fails on a value the caller did not write.
         const JsonValue *actionMember = body.Get("impersonation_action");

         if (actionMember && !actionMember->IsNull())
         {
            String word;
            bool wellFormed = false;

            if (actionMember->IsNumber())
            {
               const double number = actionMember->AsNumber();
               wellFormed = std::floor(number) == number;
               word = StringParser::IntToString((int) number);
            }
            else if (actionMember->IsString())
            {
               wellFormed = true;
               word = Utf8ToString(actionMember->AsString());
            }

            if (!wellFormed || !ImpersonationCheck::ReadAction(word, impersonationAction))
            {
               error = "impersonation_action must be 0 (off), 1 (flag) or 2 (quarantine), or the words off, flag or quarantine";
               return false;
            }
         }

         if (body.Get("protected_persons"))
         {
            String canonical;
            String refusal;

            if (!ImpersonationCheck::CanonicalisePersons(protectedPersons, canonical, refusal))
            {
               error = AnsiString(refusal);
               return false;
            }

            protectedPersons = canonical;
         }

         if (body.Get("protected_partners"))
         {
            String canonical;
            String refusal;

            if (!ImpersonationCheck::CanonicalisePartners(protectedPartners, canonical, refusal))
            {
               error = AnsiString(refusal);
               return false;
            }

            protectedPartners = canonical;
         }

         // Restricted delivery: the list is canonicalised - trimmed,
         // lower-cased, the names separated by ", " - and refused in the
         // unit's own sentence when a name is not a domain, the text is not
         // ASCII or it is longer than 2,000 characters. Judged only when the
         // body names it, as the report mailbox is, so a PUT of another field
         // never fails on a value the caller did not write.
         if (body.Get("restricted_domains"))
         {
            String canonical;
            String refusal;

            if (!RestrictedDelivery::CanonicaliseList(restrictedDomains, canonical, refusal))
            {
               error = AnsiString(refusal);
               return false;
            }

            restrictedDomains = canonical;
         }

         // The report mailbox: empty, or one address in a domain this server
         // hosts - this one, under the name it is being saved with, or another.
         // A provider delivers a DMARC or TLS-RPT report to the address the rua=
         // tag names, so an address somewhere else would read no report and never
         // say so; the same rule the complaint mailbox is held to. Whether an
         // account or alias answers at it is the administrator's to arrange - the
         // domain may be new and its mailboxes not yet made.
         //
         // Judged only when the body names it. The stored value is kept valid by
         // PersistentDomain - rewritten on a rename, cleared when the domain that
         // hosts it is deleted - and judging it again on every save would refuse
         // a PUT of some other field, and the Control Panel's Save, for a value
         // the caller did not touch.
         reportMailbox.Trim();
         reportMailbox.ToLower();

         if (body.Get("report_mailbox") && !reportMailbox.IsEmpty())
         {
            const String mailboxDomain = StringParser::ExtractDomain(reportMailbox);
            const bool own = !mailboxDomain.IsEmpty() && mailboxDomain.CompareNoCase(name) == 0;
            const bool hosted = own || (!mailboxDomain.IsEmpty() && CacheContainer::Instance()->GetDomain(mailboxDomain));

            if (!StringParser::IsValidEmailAddress(reportMailbox) || !hosted)
            {
               error = "report_mailbox is one address in a domain this server hosts, or empty; the DMARC and TLS-RPT reports are delivered to it.";
               return false;
            }

            // A mailbox in ANOTHER hosted domain makes this server open every
            // attachment delivered to that address alone, so a credential
            // confined to a list of domains may name one only in a domain on
            // its list; the cross-domain form is otherwise the server
            // administrator's.
            if (!own && !callerDomains.empty())
            {
               bool reaches = false;
               for (const String &allowed : callerDomains)
               {
                  if (allowed.CompareNoCase(mailboxDomain) == 0)
                  {
                     reaches = true;
                     break;
                  }
               }

               if (!reaches)
               {
                  error = "report_mailbox names an address in a domain this credential does not administer; a key confined to a list of domains may name a mailbox only in one of them.";
                  forbidden = true;
                  return false;
               }
            }
         }

         // The country policy, in the grammar CountryBlocking owns: refused here
         // in the setting's own words, and stored in the one canonical spelling
         // that COM, hmctl and the Control Deck also store.
         String canonicalCountryPolicy;
         String countryPolicyError;

         if (!CountryBlocking::Canonicalise(countryPolicy, canonicalCountryPolicy, countryPolicyError))
         {
            error = AnsiString(countryPolicyError);
            return false;
         }

         long maxMessageSize = domain->GetMaxMessageSize(), maxSize = domain->GetMaxSizeMB(), maxAccountSize = domain->GetMaxAccountSize();
         long maxAccounts = domain->GetMaxNoOfAccounts(), maxAliases = domain->GetMaxNoOfAliases(), maxLists = domain->GetMaxNoOfDistributionLists();
         long retention = domain->GetMessageRetentionDays(), relayPort = domain->GetRelayPort();
         long digestHour = domain->GetQuarantineDigestHour();

         // The session policies. Both numbers are seconds, 0 meaning "take the
         // server's answer"; both switches are 0 inherit, 1 on, 2 off. The bounds
         // are SessionPolicies', so REST, COM and the command line refuse the same
         // numbers in the same words.
         long sessionIdle = domain->GetSessionIdleSeconds();
         long sessionLifetime = domain->GetSessionLifetimeSeconds();
         long sessionOnePerDevice = domain->GetSessionOnePerDevice();
         long sessionBindToAddress = domain->GetSessionBindToAddress();

         if (!ReadInteger(body, "max_message_size_kb", 0, 2000000000L, maxMessageSize, error) ||
             !ReadInteger(body, "max_size_mb", 0, 2000000000L, maxSize, error) ||
             !ReadInteger(body, "max_account_size_mb", 0, 2000000000L, maxAccountSize, error) ||
             !ReadInteger(body, "max_accounts", 0, 2000000000L, maxAccounts, error) ||
             !ReadInteger(body, "max_aliases", 0, 2000000000L, maxAliases, error) ||
             !ReadInteger(body, "max_lists", 0, 2000000000L, maxLists, error) ||
             !ReadInteger(body, "message_retention_days", 0, 100000, retention, error) ||
             !ReadInteger(body, "relay_port", 0, 65535, relayPort, error) ||
             !ReadInteger(body, "vacation_days", 0, 3650, vacationDays, error) ||
             !ReadInteger(body, "quota_warning_percent", -1, 100, quotaWarningPercent, error) ||
             !ReadInteger(body, "quota_grace_percent", -1, 1000, quotaGracePercent, error) ||
             !ReadInteger(body, "quota_send_stop_percent", -1, 1000, quotaSendStopPercent, error) ||
             !ReadInteger(body, "quarantine_digest_hour", 0, 23, digestHour, error) ||
             !ReadInteger(body, "session_idle_seconds", 0, SessionPolicies::MaximumSeconds, sessionIdle, error) ||
             !ReadInteger(body, "session_lifetime_seconds", 0, SessionPolicies::MaximumSeconds, sessionLifetime, error) ||
             !ReadInteger(body, "session_one_per_device", 0, 2, sessionOnePerDevice, error) ||
             !ReadInteger(body, "session_bind_to_address", 0, 2, sessionBindToAddress, error))
            return false;

         // The rotation policy, in the ranges DkimRotationPlan::Policy::Validate
         // holds every interface to; the key size is a set, not a range.
         long rotationInterval = domain->GetDKIMRotationIntervalDays();
         long rotationPropagation = domain->GetDKIMRotationPropagationHours();
         long rotationRetention = domain->GetDKIMRotationRetentionDays();
         long rotationKeyBits = domain->GetDKIMRotationKeyBits();

         if (!ReadInteger(body, "dkim_rotation_interval_days", 1, 3650, rotationInterval, error) ||
             !ReadInteger(body, "dkim_rotation_propagation_hours", 1, 720, rotationPropagation, error) ||
             !ReadInteger(body, "dkim_rotation_retention_days", 1, 365, rotationRetention, error) ||
             !ReadInteger(body, "dkim_rotation_key_bits", 2048, 4096, rotationKeyBits, error))
            return false;

         DkimRotationPlan::Policy rotationPolicy;
         rotationPolicy.interval_days = (int) rotationInterval;
         rotationPolicy.propagation_hours = (int) rotationPropagation;
         rotationPolicy.retention_days = (int) rotationRetention;
         rotationPolicy.key_bits = (int) rotationKeyBits;

         AnsiString rotationField;
         if (!DkimRotationPlan::Policy::Validate(rotationPolicy, rotationField, error))
            return false;

         bool active = domain->GetIsActive();
         bool maxAccountsEnabled = domain->GetMaxNoOfAccountsEnabled(), maxAliasesEnabled = domain->GetMaxNoOfAliasesEnabled();
         bool maxListsEnabled = domain->GetMaxNoOfDistributionListsEnabled();
         bool plusAddressing = domain->GetUsePlusAddressing(), greylisting = domain->GetASUseGreyListing();
         bool signatureEnabled = domain->GetEnableSignature(), signatureReplies = domain->GetAddSignaturesToReplies();
         bool signatureLocal = domain->GetAddSignaturesToLocalMail(), dkimEnabled = domain->GetDKIMEnabled();
         bool relayAuth = domain->GetRelayRequiresAuth(), vacationOn = domain->GetVacationMessageIsOn();
         bool dkimSignAliases = domain->GetDKIMAliasesEnabled(), vacationExternalOverride = domain->GetVacationExternalOverride();
         bool externalTagSubject = domain->GetExternalTagSubject(), externalTagHeader = domain->GetExternalTagHeader();
         bool firstContactTip = domain->GetFirstContactTip(), disclaimerEnabled = domain->GetDisclaimerEnabled();
         bool webhooksEnabled = domain->GetWebhooksEnabled();
         bool webhookSubjects = domain->GetWebhookSubjects();
         bool quarantineDigest = domain->GetQuarantineDigest();
         bool openPgpPublish = domain->GetOpenPGPPublish();
         bool openPgpPublishNewAccounts = domain->GetOpenPGPPublishNewAccounts();
         bool dkimRotationEnabled = domain->GetDKIMRotationEnabled();

         if (!ReadBool(body, "active", active, error) ||
             !ReadBool(body, "max_accounts_enabled", maxAccountsEnabled, error) ||
             !ReadBool(body, "max_aliases_enabled", maxAliasesEnabled, error) ||
             !ReadBool(body, "max_lists_enabled", maxListsEnabled, error) ||
             !ReadBool(body, "plus_addressing_enabled", plusAddressing, error) ||
             !ReadBool(body, "use_greylisting", greylisting, error) ||
             !ReadBool(body, "signature_enabled", signatureEnabled, error) ||
             !ReadBool(body, "signature_add_to_replies", signatureReplies, error) ||
             !ReadBool(body, "signature_add_to_local_mail", signatureLocal, error) ||
             !ReadBool(body, "dkim_enabled", dkimEnabled, error) ||
             !ReadBool(body, "dkim_sign_aliases", dkimSignAliases, error) ||
             !ReadBool(body, "dkim_rotation_enabled", dkimRotationEnabled, error) ||
             !ReadBool(body, "relay_requires_auth", relayAuth, error) ||
             !ReadBool(body, "vacation_enabled", vacationOn, error) ||
             !ReadBool(body, "vacation_external_override", vacationExternalOverride, error) ||
             !ReadBool(body, "external_tag_subject", externalTagSubject, error) ||
             !ReadBool(body, "external_tag_header", externalTagHeader, error) ||
             !ReadBool(body, "first_contact_tip", firstContactTip, error) ||
             !ReadBool(body, "disclaimer_enabled", disclaimerEnabled, error) ||
             !ReadBool(body, "quarantine_digest", quarantineDigest, error) ||
             !ReadBool(body, "openpgp_publish", openPgpPublish, error) ||
             !ReadBool(body, "openpgp_publish_new_accounts", openPgpPublishNewAccounts, error) ||
             !ReadBool(body, "webhooks_enabled", webhooksEnabled, error) ||
             !ReadBool(body, "webhook_subjects", webhookSubjects, error))
            return false;

         // The two canonicalisation methods, as put_DKIMHeaderCanonicalizationMethod
         // and put_DKIMBodyCanonicalizationMethod take eDKIMCanonicalizationMethod.
         int headerMethod = domain->GetDKIMHeaderCanonicalizationMethod();
         if (!headerCanonicalization.IsEmpty() && !ParseCanonicalizationWord(headerCanonicalization, headerMethod))
         {
            error = "dkim_header_canonicalization must be simple or relaxed";
            return false;
         }

         int bodyMethod = domain->GetDKIMBodyCanonicalizationMethod();
         if (!bodyCanonicalization.IsEmpty() && !ParseCanonicalizationWord(bodyCanonicalization, bodyMethod))
         {
            error = "dkim_body_canonicalization must be simple or relaxed";
            return false;
         }

         Domain::DomainSignatureMethod method = domain->GetSignatureMethod();
         if (!signatureMethod.IsEmpty())
         {
            AnsiString word = signatureMethod;
            if (word.CompareNoCase("overwrite") == 0)
               method = Domain::SMOverwriteAccountSignature;
            else if (word.CompareNoCase("set_if_not_specified") == 0)
               method = Domain::SMSetIfNotSpecifiedInAccount;
            else if (word.CompareNoCase("append") == 0)
               method = (Domain::DomainSignatureMethod) 3;
            else
            {
               error = "signature_method must be set_if_not_specified, overwrite or append";
               return false;
            }
         }

         int algorithm = domain->GetDKIMSigningAlgorithm();
         if (!dkimAlgorithm.IsEmpty())
         {
            AnsiString word = dkimAlgorithm;
            if (word.CompareNoCase("sha1") == 0)
               algorithm = 1;
            else if (word.CompareNoCase("sha256") == 0)
               algorithm = 2;
            else
            {
               error = "dkim_signing_algorithm must be sha1 or sha256";
               return false;
            }
         }

         ConnectionSecurity security = domain->GetRelayConnectionSecurity();
         if (!relaySecurity.IsEmpty() && !ParseConnectionSecurityWord(relaySecurity, security))
         {
            error = "relay_connection_security must be one of none, starttls_optional, starttls_required, tls";
            return false;
         }

         // As InterfaceDomain::put_Name trims; an empty name is no domain.
         name.Trim();
         if (name.IsEmpty())
         {
            error = "name must not be empty";
            return false;
         }

         plusChar.Trim();
         if (plusChar.GetLength() > 1)
         {
            error = "plus_addressing_character is one character";
            return false;
         }

         // The column holds a hundred characters. Refused rather than
         // truncated, as put_ExternalTagText refuses it: a tag cut off in the
         // middle is a tag every reader learns to ignore.
         externalTagText.Trim();
         if (externalTagText.GetLength() > 100)
         {
            error = "external_tag_text must be 100 characters or fewer";
            return false;
         }

         renamed = name.CompareNoCase(domain->GetName()) != 0;

         domain->SetName(name);
         domain->SetIsActive(active);
         domain->SetPostmaster(postmaster);
         domain->SetMaxMessageSize((int) maxMessageSize);
         domain->SetMaxSizeMB((int) maxSize);
         domain->SetMaxAccountSize((int) maxAccountSize);
         domain->SetMaxNoOfAccounts((int) maxAccounts);
         domain->SetMaxNoOfAliases((int) maxAliases);
         domain->SetMaxNoOfDistributionLists((int) maxLists);
         domain->SetMaxNoOfAccountsEnabled(maxAccountsEnabled);
         domain->SetMaxNoOfAliasesEnabled(maxAliasesEnabled);
         domain->SetMaxNoOfDistributionListsEnabled(maxListsEnabled);
         domain->SetUsePlusAddressing(plusAddressing);
         domain->SetPlusAddressingChar(plusChar);
         domain->SetASUseGreyListing(greylisting);
         domain->SetEnableSignature(signatureEnabled);
         domain->SetSignatureMethod(method);
         domain->SetSignaturePlainText(signaturePlain);
         domain->SetSignatureHTML(signatureHtml);
         domain->SetAddSignaturesToReplies(signatureReplies);
         domain->SetAddSignaturesToLocalMail(signatureLocal);
         domain->SetDKIMEnabled(dkimEnabled);
         domain->SetDKIMSelector(dkimSelector);
         domain->SetDKIMPrivateKeyFile(dkimKeyFile);
         domain->SetDKIMSigningAlgorithm(algorithm);
         domain->SetDKIMHeaderCanonicalizationMethod(headerMethod);
         domain->SetDKIMBodyCanonicalizationMethod(bodyMethod);
         domain->SetDKIMSecondarySelector(dkimSecondarySelector);
         domain->SetDKIMSecondaryPrivateKeyFile(dkimSecondaryKeyFile);
         domain->SetDKIMAliasesEnabled(dkimSignAliases);
         domain->SetDKIMRotationEnabled(dkimRotationEnabled);
         domain->SetDKIMRotationIntervalDays((int) rotationInterval);
         domain->SetDKIMRotationPropagationHours((int) rotationPropagation);
         domain->SetDKIMRotationRetentionDays((int) rotationRetention);
         domain->SetDKIMRotationKeyBits((int) rotationKeyBits);
         domain->SetWebhooksEnabled(webhooksEnabled);
         domain->SetWebhookSubjects(webhookSubjects);
         domain->SetMessageRetentionDays((int) retention);
         domain->SetRelayHost(relayHost);
         domain->SetRelayPort(relayPort);
         domain->SetRelayRequiresAuth(relayAuth);
         domain->SetRelayUsername(relayUser);
         domain->SetSessionIdleSeconds((int) sessionIdle);
         domain->SetSessionLifetimeSeconds((int) sessionLifetime);
         domain->SetSessionOnePerDevice((int) sessionOnePerDevice);
         domain->SetSessionBindToAddress((int) sessionBindToAddress);
         domain->SetRelayPassword(relayPassword);
         domain->SetRelayConnectionSecurity(security);
         domain->SetVacationMessageIsOn(vacationOn);
         domain->SetVacationSubject(vacationSubject);
         domain->SetVacationMessage(vacationMessage);
         domain->SetVacationInternalSubject(vacationInternalSubject);
         domain->SetVacationInternalMessage(vacationInternalMessage);
         domain->SetVacationExternalOverride(vacationExternalOverride);
         domain->SetVacationDays((int) vacationDays);
         domain->SetQuotaWarningPercent((int) quotaWarningPercent);
         domain->SetQuotaWarningSubject(quotaWarningSubject);
         domain->SetQuotaWarningText(quotaWarningText);
         domain->SetQuotaGracePercent((int) quotaGracePercent);
         domain->SetQuotaSendStopPercent((int) quotaSendStopPercent);
         domain->SetQuarantineDigest(quarantineDigest);
         domain->SetOpenPGPPublish(openPgpPublish);
         domain->SetOpenPGPPublishNewAccounts(openPgpPublishNewAccounts);
         domain->SetQuarantineDigestHour((int) digestHour);
         domain->SetCountryPolicy(canonicalCountryPolicy);
         domain->SetReportMailbox(reportMailbox);
         domain->SetRestrictedDomains(restrictedDomains);
         domain->SetImpersonationAction(impersonationAction);
         domain->SetProtectedPersons(protectedPersons);
         domain->SetProtectedPartners(protectedPartners);
         domain->SetADDomainName(adDomainName);
         domain->SetExternalTagSubject(externalTagSubject);
         domain->SetExternalTagHeader(externalTagHeader);
         domain->SetExternalTagText(externalTagText);
         domain->SetFirstContactTip(firstContactTip);
         domain->SetDisclaimerEnabled(disclaimerEnabled);
         domain->SetDisclaimerPlainText(disclaimerPlainText);
         domain->SetDisclaimerHTML(disclaimerHtml);
         return true;
      }

      std::shared_ptr<Domain> DomainByName(const String &domainName)
      {
         Domains domains;
         domains.Refresh();
         return domains.GetItemByName(domainName);
      }

      AnsiString DomainAliasJson(std::shared_ptr<DomainAlias> alias, const Quote &quote)
      {
         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"name\":\"%hs\"}", alias->GetID(), quote(alias->GetAlias()).c_str());
         return entry;
      }
   }

   // ------------------------------------------------------------------------
   // The domain, whole.
   // ------------------------------------------------------------------------

   AnsiString
   RestApiServer::DomainEntryJson_(const std::shared_ptr<Domain> &domain, bool withLevels)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      AnsiString entry;
      entry.Format("{\"name\":\"%hs\",\"active\":%hs,\"postmaster\":\"%hs\",\"max_message_size_kb\":%d,\"max_size_mb\":%d,\"max_account_size_mb\":%d",
         quote(domain->GetName()).c_str(),
         domain->GetIsActive() ? "true" : "false",
         quote(domain->GetPostmaster()).c_str(),
         domain->GetMaxMessageSize(),
         domain->GetMaxSizeMB(),
         domain->GetMaxAccountSize());

      AnsiString limits;
      limits.Format(",\"max_accounts\":%d,\"max_aliases\":%d,\"max_lists\":%d,\"message_retention_days\":%d,\"relay_port\":%ld,\"quarantine_digest_hour\":%d",
         domain->GetMaxNoOfAccounts(),
         domain->GetMaxNoOfAliases(),
         domain->GetMaxNoOfDistributionLists(),
         domain->GetMessageRetentionDays(),
         domain->GetRelayPort(),
         domain->GetQuarantineDigestHour());
      entry += limits;

      AnsiString sessions;
      sessions.Format(",\"session_idle_seconds\":%d,\"session_lifetime_seconds\":%d,\"session_one_per_device\":%d,\"session_bind_to_address\":%d",
         domain->GetSessionIdleSeconds(),
         domain->GetSessionLifetimeSeconds(),
         domain->GetSessionOnePerDevice(),
         domain->GetSessionBindToAddress());
      entry += sessions;

      auto flag = [&entry](const char *name, bool value)
      {
         entry += ",\"";
         entry += name;
         entry += value ? "\":true" : "\":false";
      };
      auto text = [&entry, &quote](const char *name, const String &value)
      {
         entry += ",\"";
         entry += name;
         entry += "\":\"" + quote(value) + "\"";
      };

      flag("max_accounts_enabled", domain->GetMaxNoOfAccountsEnabled());
      flag("max_aliases_enabled", domain->GetMaxNoOfAliasesEnabled());
      flag("max_lists_enabled", domain->GetMaxNoOfDistributionListsEnabled());
      flag("plus_addressing_enabled", domain->GetUsePlusAddressing());
      text("plus_addressing_character", domain->GetPlusAddressingChar());
      flag("use_greylisting", domain->GetASUseGreyListing());
      flag("signature_enabled", domain->GetEnableSignature());
      text("signature_method", String(SignatureMethodWord(domain->GetSignatureMethod())));
      text("signature_plain_text", domain->GetSignaturePlainText());
      text("signature_html", domain->GetSignatureHTML());
      flag("signature_add_to_replies", domain->GetAddSignaturesToReplies());
      flag("signature_add_to_local_mail", domain->GetAddSignaturesToLocalMail());
      flag("dkim_enabled", domain->GetDKIMEnabled());
      text("dkim_selector", String(domain->GetDKIMSelector()));
      text("dkim_private_key_file", domain->GetDKIMPrivateKeyFile());
      text("dkim_signing_algorithm", String(DkimAlgorithmWord(domain->GetDKIMSigningAlgorithm())));
      text("dkim_header_canonicalization", String(CanonicalizationWord(domain->GetDKIMHeaderCanonicalizationMethod())));
      text("dkim_body_canonicalization", String(CanonicalizationWord(domain->GetDKIMBodyCanonicalizationMethod())));
      text("dkim_secondary_selector", String(domain->GetDKIMSecondarySelector()));
      text("dkim_secondary_private_key_file", domain->GetDKIMSecondaryPrivateKeyFile());
      flag("dkim_sign_aliases", domain->GetDKIMAliasesEnabled());
      flag("dkim_rotation_enabled", domain->GetDKIMRotationEnabled());

      AnsiString rotation;
      rotation.Format(",\"dkim_rotation_interval_days\":%d,\"dkim_rotation_propagation_hours\":%d,\"dkim_rotation_retention_days\":%d,\"dkim_rotation_key_bits\":%d",
         domain->GetDKIMRotationIntervalDays(),
         domain->GetDKIMRotationPropagationHours(),
         domain->GetDKIMRotationRetentionDays(),
         domain->GetDKIMRotationKeyBits());
      entry += rotation;
      text("relay_host", domain->GetRelayHost());
      flag("relay_requires_auth", domain->GetRelayRequiresAuth());
      text("relay_username", domain->GetRelayUsername());
      text("relay_connection_security", String(ConnectionSecurityWord(domain->GetRelayConnectionSecurity())));
      flag("vacation_enabled", domain->GetVacationMessageIsOn());
      text("vacation_subject", domain->GetVacationSubject());
      text("vacation_message", domain->GetVacationMessage());
      text("vacation_internal_subject", domain->GetVacationInternalSubject());
      text("vacation_internal_message", domain->GetVacationInternalMessage());
      flag("webhooks_enabled", domain->GetWebhooksEnabled());
      flag("webhook_subjects", domain->GetWebhookSubjects());
      flag("vacation_external_override", domain->GetVacationExternalOverride());
      AnsiString vacationDays;
      vacationDays.Format(",\"vacation_days\":%d", domain->GetVacationDays());
      entry += vacationDays;
      AnsiString quota;
      quota.Format(",\"quota_warning_percent\":%d,\"quota_grace_percent\":%d,\"quota_send_stop_percent\":%d",
                   domain->GetQuotaWarningPercent(), domain->GetQuotaGracePercent(), domain->GetQuotaSendStopPercent());
      entry += quota;
      text("quota_warning_subject", domain->GetQuotaWarningSubject());
      text("quota_warning_text", domain->GetQuotaWarningText());
      flag("quarantine_digest", domain->GetQuarantineDigest());
      flag("openpgp_publish", domain->GetOpenPGPPublish());
      flag("openpgp_publish_new_accounts", domain->GetOpenPGPPublishNewAccounts());
      text("ad_domain_name", domain->GetADDomainName());
      flag("external_tag_subject", domain->GetExternalTagSubject());
      flag("external_tag_header", domain->GetExternalTagHeader());
      text("external_tag_text", domain->GetExternalTagText());
      flag("first_contact_tip", domain->GetFirstContactTip());
      flag("disclaimer_enabled", domain->GetDisclaimerEnabled());
      text("disclaimer_plain_text", domain->GetDisclaimerPlainText());
      text("disclaimer_html", domain->GetDisclaimerHTML());
      text("country_policy", domain->GetCountryPolicy());
      text("report_mailbox", domain->GetReportMailbox());
      text("restricted_domains", domain->GetRestrictedDomains());
      AnsiString impersonation;
      impersonation.Format(",\"impersonation_action\":%d", domain->GetImpersonationAction());
      entry += impersonation;
      text("protected_persons", domain->GetProtectedPersons());
      text("protected_partners", domain->GetProtectedPartners());

      // The settings with levels: what is in effect for the domain's accounts
      // that hold none of their own, and where it came from, and the domain's
      // own value where the overlay holds it (RestApiSettingLevels.h).
      if (withLevels)
         entry += SettingLevelsRest::EntryFields(domain, std::shared_ptr<const Account>());

      entry += "}";
      return entry;
   }

   HttpResponse
   RestApiServer::HandleGetDomain_(const String &domainName)
   {
      // One domain, read: the entry the update answers with - the listing's
      // entry and the settings with levels beside it - so that a caller reading
      // a domain, changing one field and sending it back sends the server what
      // it gave, the answered level fields ignored on the way back. hmctl domain get and
      // Get-HmDomain -Name have asked for this document since they were
      // written; until now the API had no GET under /api/v1/domains/{domain} at
      // all and both were answered "not found" for a domain that is there.
      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      return BuildResponse_(200, DomainEntryJson_(domain));
   }

   HttpResponse
   RestApiServer::HandleUpdateDomain_(const Caller &caller, const String &domainName, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      // active has always had to be named, as enabled has to be for the
      // automatic reply, so that a client that sends {"active":false} to switch
      // a domain off does not also blank anything; the contract stays.
      const JsonValue *active = body.Get("active");
      if (!active || active->IsNull())
         return BuildResponse_(400, "{\"error\":\"active is required\"}");

      // The eleven fields COM gates on the server administrator (put_MaxSize,
      // put_MaxMessageSize, put_MaxAccountSize, the three counts and their
      // switches, put_MessageRetentionDays, and put_PlusAddressingEnabled, which
      // refuses a domain administrator a change since plus addressing became the
      // gate docs/SettingLevels.md calls it - it did not before): a key
      // restricted to this domain may not lift its own limits through them, or
      // open the gate.
      if (!HoldsServerAuthority_(caller))
      {
         static const char *const serverAdministratorsOnly[] =
         {
            "max_message_size_kb", "max_size_mb", "max_account_size_mb", "max_accounts", "max_aliases", "max_lists",
            "max_accounts_enabled", "max_aliases_enabled", "max_lists_enabled", "message_retention_days", "plus_addressing_enabled"
         };
         for (size_t i = 0; i < sizeof(serverAdministratorsOnly) / sizeof(serverAdministratorsOnly[0]); i++)
         {
            if (body.Get(serverAdministratorsOnly[i]))
               return BuildResponse_(403, "{\"error\":\"" + AnsiString(serverAdministratorsOnly[i]) + " is the server administrator's to set; a key restricted to a domain cannot change its limits\"}");
         }
      }
      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      String oldName = domain->GetName();

      // The domain as stored, before the body is applied, for the DKIM rotation's
      // guard: a configuration replayed from an old export carries the selector
      // the rotation has since moved away from.
      std::shared_ptr<Domain> stored = std::shared_ptr<Domain>(new Domain());
      if (!PersistentDomain::ReadObject(stored, domain->GetID()))
         return BuildResponse_(500, "{\"error\":\"failed to read domain\"}");

      AnsiString error;
      bool renamed = false;
      bool forbidden = false;
      if (!ApplyDomainBody(body, domain, caller.domains, error, renamed, forbidden))
         return BuildResponse_(forbidden ? 403 : 400, ErrorBody(quote, String(error)));

      // The domain's own values of the settings with levels, judged now with
      // everything else and written once the domain itself is saved.
      std::vector<SettingLevelsRest::Change> levelChanges;
      if (!SettingLevelsRest::Read(body, PersistentSettingOverride::ScopeDomain, domain->GetID(), HoldsServerAuthority_(caller), levelChanges, error, forbidden))
         return BuildResponse_(forbidden ? 403 : 400, ErrorBody(quote, String(error)));

      String rotationRefusal;
      if (DkimRotation::RefusesKeyChange(*stored, *domain, rotationRefusal))
      {
         LOG_APPLICATION("RestApi: Refused to update domain " + oldName + ": " + rotationRefusal);
         return BuildResponse_(409, ErrorBody(quote, rotationRefusal));
      }

      String saveError;
      if (!PersistentDomain::SaveObject(domain, saveError, PersistenceModeNormal))
      {
         if (!saveError.IsEmpty())
         {
            LOG_APPLICATION("RestApi: Refused to update domain " + oldName + ": " + saveError);
            return BuildResponse_(400, ErrorBody(quote, saveError));
         }
         return BuildResponse_(500, "{\"error\":\"failed to save domain\"}");
      }

      if (!SettingLevelsRest::Apply(levelChanges, PersistentSettingOverride::ScopeDomain, domain->GetID(), error))
      {
         LOG_APPLICATION("RestApi: Domain " + domain->GetName() + " was saved, and one of its own setting values was not: " + String(error));
         return BuildResponse_(500, ErrorBody(quote, String("the domain was saved, and one of its own setting values was not: ") + String(error)));
      }

      if (renamed)
      {
         LOG_APPLICATION("RestApi: Domain " + oldName + " renamed to " + domain->GetName() + ".");
      }
      else
      {
         LOG_APPLICATION("RestApi: Domain " + domain->GetName() + " updated, active: " +
            String(domain->GetIsActive() ? _T("true") : _T("false")) + ".");
      }

      return BuildResponse_(200, DomainEntryJson_(domain));
   }

   // ------------------------------------------------------------------------
   // Domain aliases.
   // ------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleListDomainAliases_(const String &domainName)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      DomainAliases aliases(domain->GetID());
      aliases.Refresh();

      AnsiString body = "[";
      int count = 0;
      for (std::shared_ptr<DomainAlias> alias : aliases.GetSnapshot())
      {
         if (!alias)
            continue;
         if (count > 0)
            body += ",";
         body += DomainAliasJson(alias, quote);
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateDomainAlias_(const String &domainName, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      String name;
      AnsiString error;
      if (!ReadString(body, "name", name, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      name.Trim();
      if (name.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name is required\"}");

      // As InterfaceDomainAliases::Add makes one and InterfaceDomainAlias::Save
      // saves it: owned by this domain, refused by the persistence layer's own
      // check when the name is a domain or an alias already.
      std::shared_ptr<DomainAlias> alias = std::shared_ptr<DomainAlias>(new DomainAlias());
      alias->SetDomainID(domain->GetID());
      alias->SetAlias(name);

      String saveError;
      if (!PersistentDomainAlias::SaveObject(alias, saveError, PersistenceModeNormal))
      {
         if (!saveError.IsEmpty())
            return BuildResponse_(400, ErrorBody(quote, saveError));
         return BuildResponse_(500, "{\"error\":\"failed to save the domain alias\"}");
      }

      LOG_APPLICATION("RestApi: Domain alias " + name + " created for " + domain->GetName() + ".");

      return BuildResponse_(201, DomainAliasJson(alias, quote));
   }

   HttpResponse
   RestApiServer::HandleDeleteDomainAlias_(const String &domainName, const String &aliasName)
   {
      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      DomainAliases aliases(domain->GetID());
      aliases.Refresh();

      std::shared_ptr<DomainAlias> alias = aliases.GetItemByName(aliasName);
      if (!alias)
         return BuildResponse_(404, "{\"error\":\"domain alias not found\"}");

      if (!PersistentDomainAlias::DeleteObject(alias))
         return BuildResponse_(500, "{\"error\":\"failed to delete the domain alias\"}");

      LOG_APPLICATION("RestApi: Domain alias " + aliasName + " of " + domain->GetName() + " deleted.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   // ------------------------------------------------------------------------
   // Distribution lists: the entry every list route answers with, the create
   // and the update. The listing and the delete stay in RestApiServer.cpp.
   // ------------------------------------------------------------------------

   namespace
   {
      // eDistributionListMode over COM, the four values the server implements.
      // put_Mode refuses the fifth the type library still declares
      // (eLMServerMembers), and so does the parse below, by having no word
      // for it.
      const char *ListModeWord(DistributionList::ListMode mode)
      {
         switch (mode)
         {
         case DistributionList::LMMembership:
            return "membership";
         case DistributionList::LMAnnouncement:
            return "announcement";
         case DistributionList::LMDomainMembers:
            return "domain_members";
         case DistributionList::LMPublic:
         default:
            return "public";
         }
      }

      bool ParseListModeWord(const String &word, DistributionList::ListMode &mode)
      {
         AnsiString value = word;

         if (value.CompareNoCase("public") == 0)
            mode = DistributionList::LMPublic;
         else if (value.CompareNoCase("membership") == 0)
            mode = DistributionList::LMMembership;
         else if (value.CompareNoCase("announcement") == 0)
            mode = DistributionList::LMAnnouncement;
         else if (value.CompareNoCase("domain_members") == 0)
            mode = DistributionList::LMDomainMembers;
         else
            return false;

         return true;
      }

      const char *ListModeRefusal = "{\"error\":\"mode must be public, membership, announcement or domain_members\"}";

      // A membership query that reaches past the domains a confined key
      // administers is the server administrator's to set: a key issued for one
      // tenant could otherwise make a list of its own that delivers to every
      // account of another. The parse itself is not judged here - the save
      // judges it, in the one sentence COM and REST share - so an unreadable
      // query falls through to that rather than being called a permission
      // problem. Empty callerDomains is a key that reaches every domain.
      bool MembershipReachesPastCaller(const String &membership, const String &ownDomain, const std::vector<String> &callerDomains)
      {
         if (callerDomains.empty() || membership.IsEmpty())
            return false;

         DistributionListMembership::Query query;
         String reason;

         if (!DistributionListMembership::Parse(membership, query, reason))
            return false;

         return !DistributionListMembership::WithinDomains(query, ownDomain, callerDomains);
      }

      const char *MembershipRefusal = "{\"error\":\"membership names accounts outside the domains this key administers; domain, a pattern in one of them, or nothing\"}";

      // The list an address names, through its domain, or null.
      std::shared_ptr<DistributionList> ListByAddress(const String &address)
      {
         std::shared_ptr<Domain> domain = DomainByName(StringParser::ExtractDomain(address));
         if (!domain)
            return std::shared_ptr<DistributionList>();

         DistributionLists lists(domain->GetID());
         lists.Refresh();
         return lists.GetItemByAddress(address);
      }
   }

   AnsiString
   RestApiServer::ListEntryJson_(const std::shared_ptr<DistributionList> &list)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      AnsiString entry;
      entry.Format("{\"address\":\"%hs\",\"active\":%hs,\"require_auth\":%hs,\"mode\":\"%hs\",\"require_sender_address\":\"%hs\",\"moderator_address\":\"%hs\",\"bounce_address\":\"%hs\",\"membership\":\"%hs\",\"members\":[",
         quote(list->GetAddress()).c_str(),
         list->GetActive() ? "true" : "false",
         list->GetRequireAuth() ? "true" : "false",
         ListModeWord(list->GetListMode()),
         quote(list->GetRequireAddress()).c_str(),
         quote(list->GetModeratorAddress()).c_str(),
         quote(list->GetBounceAddress()).c_str(),
         quote(list->GetMembership()).c_str());

      // GetMembers reads the recipients again each time, so a create that has
      // just saved them answers with them.
      std::shared_ptr<DistributionListRecipients> recipients = list->GetMembers();
      if (recipients)
      {
         int count = 0;
         for (int m = 0; m < recipients->GetCount(); m++)
         {
            std::shared_ptr<DistributionListRecipient> recipient = recipients->GetItem(m);
            if (!recipient)
               continue;
            if (count > 0)
               entry += ",";
            entry += "\"" + quote(recipient->GetAddress()) + "\"";
            count++;
         }
      }
      entry += "]}";
      return entry;
   }

   HttpResponse
   RestApiServer::HandleCreateList_(const String &domainName, const std::vector<String> &callerDomains, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      static const char *const keys[] =
      {
         "address", "members", "active", "require_auth", "mode", "require_sender_address", "moderator_address", "bounce_address", "membership"
      };
      AnsiString error;
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      String address, modeWord, requireAddress, moderatorAddress, bounceAddress, membership;
      if (!ReadString(body, "address", address, error) ||
          !ReadString(body, "mode", modeWord, error) ||
          !ReadString(body, "require_sender_address", requireAddress, error) ||
          !ReadString(body, "moderator_address", moderatorAddress, error) ||
          !ReadString(body, "bounce_address", bounceAddress, error) ||
          !ReadString(body, "membership", membership, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      address.Trim();
      if (address.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"address is required\"}");

      String addressDomain = StringParser::ExtractDomain(address);
      if (addressDomain.CompareNoCase(domainName) != 0)
         return BuildResponse_(400, "{\"error\":\"address does not belong to the domain\"}");

      // The defaults InterfaceDistributionLists::Add leaves in place: active,
      // no authentication required, public.
      bool active = true, requireAuth = false;
      if (!ReadBool(body, "active", active, error) ||
          !ReadBool(body, "require_auth", requireAuth, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      DistributionList::ListMode mode = DistributionList::LMPublic;
      if (!modeWord.IsEmpty() && !ParseListModeWord(modeWord, mode))
         return BuildResponse_(400, ListModeRefusal);

      // The members: an array of addresses, empty ones skipped, as the route
      // has always taken them.
      std::vector<String> members;
      const JsonValue *memberList = body.Get("members");
      if (memberList && !memberList->IsNull())
      {
         if (!memberList->IsArray())
            return BuildResponse_(400, "{\"error\":\"members must be an array of e-mail addresses\"}");

         for (const JsonValue &item : memberList->Items())
         {
            if (!item.IsString())
               return BuildResponse_(400, "{\"error\":\"members must be an array of e-mail addresses\"}");

            String member = Utf8ToString(item.AsString());
            member.Trim();
            if (!member.IsEmpty())
               members.push_back(member);
         }
      }

      std::shared_ptr<Domain> domain = DomainByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      if (MembershipReachesPastCaller(membership, domain->GetName(), callerDomains))
         return BuildResponse_(403, MembershipRefusal);

      DistributionLists lists(domain->GetID());
      lists.Refresh();
      if (lists.GetItemByAddress(address))
         return BuildResponse_(409, "{\"error\":\"a list with that address exists\"}");

      std::shared_ptr<DistributionList> list(new DistributionList);
      list->SetDomainID(domain->GetID());
      list->SetAddress(address);
      list->SetActive(active);
      list->SetRequireAuth(requireAuth);
      list->SetListMode(mode);
      list->SetRequireAddress(requireAddress);
      list->SetModeratorAddress(moderatorAddress);
      list->SetBounceAddress(bounceAddress);
      list->SetMembership(membership);

      String saveError;
      if (!PersistentDistributionList::SaveObject(list, saveError, PersistenceModeNormal))
      {
         if (!saveError.IsEmpty())
            return BuildResponse_(400, ErrorBody(quote, saveError));
         return BuildResponse_(500, "{\"error\":\"failed to save the list\"}");
      }

      int saved = 0;
      for (const String &member : members)
      {
         std::shared_ptr<DistributionListRecipient> recipient(new DistributionListRecipient);
         recipient->SetListID(list->GetID());
         recipient->SetAddress(member);
         if (PersistentDistributionListRecipient::SaveObject(recipient))
            saved++;
      }

      LOG_APPLICATION("RestApi: Distribution list '" + list->GetAddress() + "' created with " + StringParser::IntToString(saved) + " member(s).");

      return BuildResponse_(201, ListEntryJson_(list));
   }

   HttpResponse
   RestApiServer::HandleUpdateList_(const String &address, const std::vector<String> &callerDomains, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      static const char *const keys[] =
      {
         "active", "require_auth", "mode", "require_sender_address", "moderator_address", "bounce_address", "membership"
      };
      AnsiString error;
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      std::shared_ptr<DistributionList> list = ListByAddress(address);
      if (!list)
         return BuildResponse_(404, "{\"error\":\"list not found\"}");

      bool active = list->GetActive(), requireAuth = list->GetRequireAuth();
      String modeWord, requireAddress = list->GetRequireAddress();
      String moderatorAddress = list->GetModeratorAddress(), bounceAddress = list->GetBounceAddress();

      // Read from the list, so a body that leaves it out keeps the query the
      // list has - and an empty string in the body is a real change, the one
      // that turns a query list back into a static one.
      String membership = list->GetMembership();

      if (!ReadBool(body, "active", active, error) ||
          !ReadBool(body, "require_auth", requireAuth, error) ||
          !ReadString(body, "mode", modeWord, error) ||
          !ReadString(body, "require_sender_address", requireAddress, error) ||
          !ReadString(body, "moderator_address", moderatorAddress, error) ||
          !ReadString(body, "bounce_address", bounceAddress, error) ||
          !ReadString(body, "membership", membership, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      DistributionList::ListMode mode = list->GetListMode();
      if (!modeWord.IsEmpty() && !ParseListModeWord(modeWord, mode))
         return BuildResponse_(400, ListModeRefusal);

      // Judged only when the body names it. Seeded from the list above, the
      // value is otherwise the query the list already has - and a key confined
      // to this domain would then be refused every change to a list whose query
      // the server administrator set, including switching it off.
      const JsonValue *membershipGiven = body.Get("membership");

      if (membershipGiven && !membershipGiven->IsNull() &&
          MembershipReachesPastCaller(membership, StringParser::ExtractDomain(list->GetAddress()), callerDomains))
         return BuildResponse_(403, MembershipRefusal);

      list->SetActive(active);
      list->SetRequireAuth(requireAuth);
      list->SetListMode(mode);
      list->SetRequireAddress(requireAddress);
      list->SetModeratorAddress(moderatorAddress);
      list->SetBounceAddress(bounceAddress);
      list->SetMembership(membership);

      // SaveObject drops the list from its cache, so the next message to it
      // reads the row just written.
      String saveError;
      if (!PersistentDistributionList::SaveObject(list, saveError, PersistenceModeNormal))
      {
         if (!saveError.IsEmpty())
            return BuildResponse_(400, ErrorBody(quote, saveError));
         return BuildResponse_(500, "{\"error\":\"failed to save the list\"}");
      }

      LOG_APPLICATION("RestApi: Distribution list '" + list->GetAddress() + "' updated.");

      return BuildResponse_(200, ListEntryJson_(list));
   }

   // ------------------------------------------------------------------------
   // IP ranges: one entry as every range route emits it, and the update.
   // ------------------------------------------------------------------------

   AnsiString
   RestApiServer::IpRangeEntryJson_(const std::shared_ptr<SecurityRange> &range)
   {
      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"name\":\"%hs\",\"lower\":\"%hs\",\"upper\":\"%hs\",\"priority\":%d",
         range->GetID(),
         JsonEscape_(Utf8_(range->GetName())).c_str(),
         JsonEscape_(Utf8_(range->GetLowerIPString())).c_str(),
         JsonEscape_(Utf8_(range->GetUpperIPString())).c_str(),
         (int) range->GetPriority());

      auto flag = [&entry](const char *name, bool value)
      {
         entry += ",\"";
         entry += name;
         entry += value ? "\":true" : "\":false";
      };

      flag("allow_smtp", range->GetAllowSMTP());
      flag("allow_imap", range->GetAllowIMAP());
      flag("allow_pop3", range->GetAllowPOP3());
      flag("deliver_local_to_local", range->GetAllowOption(SecurityRange::IPRANGE_RELAY_LOCAL_TO_LOCAL));
      flag("deliver_local_to_remote", range->GetAllowOption(SecurityRange::IPRANGE_RELAY_LOCAL_TO_REMOTE));
      flag("deliver_remote_to_local", range->GetAllowOption(SecurityRange::IPRANGE_RELAY_REMOTE_TO_LOCAL));
      flag("deliver_remote_to_remote", range->GetAllowOption(SecurityRange::IPRANGE_RELAY_REMOTE_TO_REMOTE));
      flag("require_auth_local_to_local", range->GetRequireSMTPAuthLocalToLocal());
      flag("require_auth_local_to_remote", range->GetRequireSMTPAuthLocalToExternal());
      flag("require_auth_remote_to_local", range->GetRequireSMTPAuthExternalToLocal());
      flag("require_auth_remote_to_remote", range->GetRequireSMTPAuthExternalToExternal());
      flag("require_tls_for_auth", range->GetRequireTLSForAuth());
      flag("spam_protection", range->GetSpamProtection());
      flag("virus_protection", range->GetVirusProtection());
      flag("expires", range->GetExpires());
      // The time only when it means something: a range that does not expire
      // carries the placeholder SaveObject writes, which is not a fact about
      // the range.
      entry += ",\"expires_time\":\"";
      if (range->GetExpires())
         entry += JsonEscape_(Utf8_(Time::GetTimeStampFromDateTime(range->GetExpiresTime())));
      entry += "\"}";
      return entry;
   }

   bool
   RestApiServer::ParseExpiryTime_(const String &text, DateTime &out)
   {
      String candidate = text;
      candidate.Trim();
      if (candidate.GetLength() == 10)
         candidate += " 00:00:00";
      if (candidate.GetLength() != 19)
         return false;

      DateTime parsed = Time::GetDateFromSystemDate(candidate);
      if (parsed.GetStatus() != DateTime::valid)
         return false;

      // Round-tripped, so that a month of 13 or a minute of 70 - which the
      // digit reads accept and the date arithmetic may fold into the next
      // month or hour - is refused rather than moved.
      if (Time::GetTimeStampFromDateTime(parsed) != candidate)
         return false;

      out = parsed;
      return true;
   }

   HttpResponse
   RestApiServer::HandleUpdateIpRange_(__int64 rangeId, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      static const char *const keys[] =
      {
         "name", "lower", "upper", "priority", "allow_smtp", "allow_imap", "allow_pop3",
         "deliver_local_to_local", "deliver_local_to_remote", "deliver_remote_to_local", "deliver_remote_to_remote",
         "require_auth_local_to_local", "require_auth_local_to_remote", "require_auth_remote_to_local", "require_auth_remote_to_remote",
         "require_tls_for_auth", "spam_protection", "virus_protection", "expires", "expires_time"
      };
      AnsiString error;
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      SecurityRanges ranges;
      ranges.Refresh();

      std::shared_ptr<SecurityRange> range = ranges.GetItemByDBID(rangeId);
      if (!range)
         return BuildResponse_(404, "{\"error\":\"ip range not found\"}");

      String name = range->GetName(), lowerText = range->GetLowerIPString(), upperText = range->GetUpperIPString();
      if (!ReadString(body, "name", name, error) ||
          !ReadString(body, "lower", lowerText, error) ||
          !ReadString(body, "upper", upperText, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      name.Trim();
      if (name.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name must not be empty\"}");

      IPAddress lower;
      IPAddress upper;
      if (!lower.TryParse(AnsiString(lowerText), false) || !upper.TryParse(AnsiString(upperText), false))
         return BuildResponse_(400, "{\"error\":\"lower and upper must be IP addresses\"}");

      long priority = range->GetPriority();
      if (!ReadInteger(body, "priority", -2000000000L, 2000000000L, priority, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      bool allowSmtp = range->GetAllowSMTP(), allowImap = range->GetAllowIMAP(), allowPop3 = range->GetAllowPOP3();
      bool l2l = range->GetAllowOption(SecurityRange::IPRANGE_RELAY_LOCAL_TO_LOCAL);
      bool l2r = range->GetAllowOption(SecurityRange::IPRANGE_RELAY_LOCAL_TO_REMOTE);
      bool r2l = range->GetAllowOption(SecurityRange::IPRANGE_RELAY_REMOTE_TO_LOCAL);
      bool r2r = range->GetAllowOption(SecurityRange::IPRANGE_RELAY_REMOTE_TO_REMOTE);
      bool authL2l = range->GetRequireSMTPAuthLocalToLocal(), authL2r = range->GetRequireSMTPAuthLocalToExternal();
      bool authR2l = range->GetRequireSMTPAuthExternalToLocal(), authR2r = range->GetRequireSMTPAuthExternalToExternal();
      bool tlsForAuth = range->GetRequireTLSForAuth(), spam = range->GetSpamProtection(), virus = range->GetVirusProtection();

      if (!ReadBool(body, "allow_smtp", allowSmtp, error) ||
          !ReadBool(body, "allow_imap", allowImap, error) ||
          !ReadBool(body, "allow_pop3", allowPop3, error) ||
          !ReadBool(body, "deliver_local_to_local", l2l, error) ||
          !ReadBool(body, "deliver_local_to_remote", l2r, error) ||
          !ReadBool(body, "deliver_remote_to_local", r2l, error) ||
          !ReadBool(body, "deliver_remote_to_remote", r2r, error) ||
          !ReadBool(body, "require_auth_local_to_local", authL2l, error) ||
          !ReadBool(body, "require_auth_local_to_remote", authL2r, error) ||
          !ReadBool(body, "require_auth_remote_to_local", authR2l, error) ||
          !ReadBool(body, "require_auth_remote_to_remote", authR2r, error) ||
          !ReadBool(body, "require_tls_for_auth", tlsForAuth, error) ||
          !ReadBool(body, "spam_protection", spam, error) ||
          !ReadBool(body, "virus_protection", virus, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      // The expiry, as the desktop dialog and the auto-ban set it: the flag,
      // and a time on the server's clock that is taken only with the flag.
      // Turning expiry on needs the time in the same body, because the time a
      // range that never expired carries is SaveObject's placeholder, and a
      // range left to it would be removed by the next sweep.
      bool expires = range->GetExpires();
      String expiresText;
      if (!ReadBool(body, "expires", expires, error) ||
          !ReadString(body, "expires_time", expiresText, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      expiresText.Trim();
      DateTime expiresTime = range->GetExpiresTime();
      if (!expiresText.IsEmpty())
      {
         if (!expires)
            return BuildResponse_(400, "{\"error\":\"expires_time is taken only when expires is true\"}");
         if (!ParseExpiryTime_(expiresText, expiresTime))
            return BuildResponse_(400, "{\"error\":\"expires_time must be a date and time as YYYY-MM-DD HH:MM:SS\"}");
      }
      else if (expires && !range->GetExpires())
         return BuildResponse_(400, "{\"error\":\"expires_time is required when a range is made to expire\"}");

      range->SetName(name);
      range->SetLowerIP(lower);
      range->SetUpperIP(upper);
      range->SetPriority(priority);
      range->SetAllowSMTP(allowSmtp);
      range->SetAllowIMAP(allowImap);
      range->SetAllowPOP3(allowPop3);
      range->SetAllowRelayL2L(l2l);
      range->SetAllowRelayL2R(l2r);
      range->SetAllowRelayR2L(r2l);
      range->SetAllowRelayR2R(r2r);
      range->SetRequireSMTPAuthLocalToLocal(authL2l);
      range->SetRequireSMTPAuthLocalToExternal(authL2r);
      range->SetRequireSMTPAuthExternalToLocal(authR2l);
      range->SetRequireSMTPAuthExternalToExternal(authR2r);
      range->SetRequireTLSForAuth(tlsForAuth);
      range->SetSpamProtection(spam);
      range->SetVirusProtection(virus);
      range->SetExpires(expires);
      if (expires)
         range->SetExpiresTime(expiresTime);

      String result;
      if (!PersistentSecurityRange::SaveObject(range, result, PersistenceModeNormal))
         return BuildResponse_(400, ErrorBody(quote, result));

      LOG_APPLICATION("RestApi: IP range '" + range->GetName() + "' updated.");

      return BuildResponse_(200, IpRangeEntryJson_(range));
   }

   // The session policy in force for a domain's accounts, as one document: what
   // applies (in_force), what the server holds, the built-in ceilings that apply
   // where nothing is set, and the domain's own row. The same JSON
   // Utilities.GetSessionPolicy answers over COM, built by SessionPolicies so that
   // the two cannot say different things.
   HttpResponse
   RestApiServer::HandleDomainSessionPolicy_(const Route &route)
   {
      // The name as the path carried it, as HandleDkim_ takes it: a domain name
      // has nothing in it that needs decoding.
      const SessionPolicy policy = SessionPolicies::ForDomain(String(route.identifier));
      if (!policy.has_domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      return BuildResponse_(200, SessionPolicies::ToJson(policy));
   }

   HttpResponse
   RestApiServer::HandleSessionPolicy_()
   {
      return BuildResponse_(200, SessionPolicies::ToJson(SessionPolicies::ServerWide()));
   }

   AnsiString
   RestApiServer::OpenApiAdministrationPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/session-policy\":{\"get\":{\"summary\":\"The session policy in force for a domain's accounts\","
         "\"description\":\"One document: in_force (idle_seconds, lifetime_seconds, one_per_device, bind_to_address - what a signed-in browser session of this domain's accounts is held to), server (the same four as stored server-wide, 0 and false where unset), defaults (the ceilings that apply where nothing is set: 1800 and 43200 seconds) and domain_policy (the domain's own four fields: 0 takes the server's answer; for the two switches 1 is on and 2 is off). The same document Utilities.GetSessionPolicy answers over COM. To change the domain's row, PUT the four session_* fields of the domain; to change the server's, PUT the four session_* keys of the settings group.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"domain, in_force, server, defaults, domain_policy\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/session-policy\":{\"get\":{\"summary\":\"The session policy in force server-wide\","
         "\"description\":\"The same document as a domain's, without domain_policy: what governs the administrator's own browser session and the accounts of every domain that has set nothing of its own. Server-wide; refused for domain-restricted keys, which read their domain's.\","
         "\"responses\":{\"200\":{\"description\":\"in_force, server, defaults\"}}}}"
         ",\"/api/v1/domains/{domain}/domain-aliases\":{"
         "\"get\":{\"summary\":\"The domain's aliases - other names the domain answers to\",\"description\":\"Each entry: id, name. A key restricted to named domains reaches its own domains only.\",\"responses\":{\"200\":{\"description\":\"Array of domain aliases\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"post\":{\"summary\":\"Add a domain alias\",\"description\":\"Body: name. What DomainAliases.Add and Save do over COM, with the same refusal when the name is already a domain or an alias; in effect for the next RCPT TO.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: id, name\"},\"400\":{\"description\":\"name missing, or refused (error says why)\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domains/{domain}/domain-aliases/{name}\":{\"delete\":{\"summary\":\"Remove a domain alias\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown domain, or no alias of that name in it\"}}}}";

      return AnsiString(paths);
   }
}
