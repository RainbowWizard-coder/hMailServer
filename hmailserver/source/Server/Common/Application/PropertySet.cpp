// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "PropertySet.h"
#include "Property.h"
#include "../Util/Crypt.h"
#include "../Util/AuditTrail.h"
#include "EnvironmentSettings.h"
#include "NewInstallDefaults.h"
#include "Profiles.h"
#include "../Util/SnmpAgent.h"
#include "../Util/StoredSecrets.h"

#include <algorithm>


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif


using namespace std;


namespace HM
{
   thread_local PropertySet::ReadRecorder *PropertySet::recorder_ = nullptr;

   PropertySet::ReadRecorder::ReadRecorder() :
      previous_(PropertySet::recorder_)
   {
      PropertySet::recorder_ = this;
   }

   PropertySet::ReadRecorder::~ReadRecorder()
   {
      PropertySet::recorder_ = previous_;
   }

   PropertySet::PropertySet(void)
   {
   }

   PropertySet::~PropertySet(void)
   {
      
   }

   void 
   PropertySet::Refresh()
   {
      SQLCommand command("select * from hm_settings");
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
   
      if (!pRS)
         return;

      std::map<String, std::shared_ptr<Property> > tmpMap;

      while (!pRS->IsEOF())
      {
         String sPropertyName = pRS->GetStringValue("settingname");
         long lPropertyLong = pRS->GetLongValue("settinginteger");
         String sPropertyString = pRS->GetStringValue("settingstring");

         bool bIsCrypted = false;
         if (IsCryptedProperty_(sPropertyName))
         {
            // De-crypt the string after load from DB.
            sPropertyString = Crypt::Instance()->UnprotectSecret(sPropertyString);
            bIsCrypted = true;
         }
      
         std::shared_ptr<Property> oProperty = std::shared_ptr<Property> (new Property(sPropertyName, lPropertyLong, sPropertyString));
         if (bIsCrypted)
            oProperty->SetIsCrypted();

         tmpMap[sPropertyName] = oProperty;

         pRS->MoveNext();
      }



      items_ = tmpMap;

      auto iter = items_.begin();
      auto iterEnd = items_.end();
      for (; iter != iterEnd; iter++)
      {
         // Trigger an change-event for all options.
         OnPropertyChanged_((*iter).second);
      }
   }

   bool
   PropertySet::Exists(const String &sPropertyName) const
   {
      return items_.find(sPropertyName) != items_.end();
   }

   std::shared_ptr<Property>
   PropertySet::GetProperty_(const String & sPropertyName)
   {
      // A reader asking which property it holds (ReadRecorder). The environment's
      // records and a profile's are the server's bookkeeping, read beside a setting
      // rather than as one, and are never a name a row holds.
      if (recorder_ && sPropertyName != EnvironmentSettings::MarkerName() && sPropertyName != EnvironmentSettings::SecretMarkerName() &&
          sPropertyName != Profiles::SnapshotName() && sPropertyName != Profiles::AppliedName() &&
          sPropertyName != StoredSecrets::KeyIdRowName() && sPropertyName != StoredSecrets::ExportRowName())
      {
         if (std::find(recorder_->names_.begin(), recorder_->names_.end(), sPropertyName) == recorder_->names_.end())
            recorder_->names_.push_back(sPropertyName);
      }

      auto iterProperty = items_.find(sPropertyName);
   
      if (iterProperty != items_.end())
         return (*iterProperty).second;

      String sErrorMessage;
      sErrorMessage.Format(_T("The property %s could not be found."), sPropertyName.c_str());
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5015, "PropertySet::GetProperty_()", sErrorMessage);

      // The property row is missing from the database. Return a NAMED in-memory
      // property: reads get the default value, and a write persists the row via
      // the insert fallback in Property, self-healing the database. (A nameless
      // property here used to make writes silently update zero rows.)
      std::shared_ptr<Property> oProperty = std::shared_ptr<Property>(new Property(sPropertyName, 0, ""));

      if (IsCryptedProperty_(sPropertyName))
         oProperty->SetIsCrypted();

      return oProperty;
   }

   long
   PropertySet::GetLong(const String &sPropertyName)
   {
      return GetProperty_(sPropertyName)->GetLongValue();
   }

   bool
   PropertySet::GetBool(const String &sPropertyName)
   {
      return GetProperty_(sPropertyName)->GetBoolValue();
   }

   String
   PropertySet::GetString(const String &sPropertyName)
   {
      return GetProperty_(sPropertyName)->GetStringValue();
   }

   /*
      The audit trail's one chokepoint for settings. Every server setting, from
      every interface, is written through the three functions below, and this is
      the only place where the value the setting HAD is still readable - which is
      why hm_settings is not one of the tables AuditTrail records from the
      statement chokepoint. A setting whose value is a secret is recorded as
      changed and never as a value; AuditTrail::IsSecretName decides, by name.

      Recorded only when the value really changed, which is the same condition
      OnPropertyChanged_ already uses: the Control Panel's settings pages write
      every field on the page when one of them is edited, and an audit trail that
      recorded forty unchanged settings per save would bury the one that moved.
   */
   void 
   PropertySet::SetLong(const String &sPropertyName, long lValue)
   {
      std::shared_ptr<Property> pProperty = GetProperty_(sPropertyName);
      long previousValue = pProperty->GetLongValue();
      bool bChanged = lValue != previousValue;
      bool withheld = false;
      if (bChanged)
      {
         RefuseIfEnvironmentManaged_(sPropertyName);
         withheld = IsWithheld_(sPropertyName);
      }
      pProperty->SetLongValue(lValue);

      if (bChanged)
      {
         AuditTrail::Instance()->RecordSettingChange(sPropertyName,
            StringParser::IntToString((int) previousValue), StringParser::IntToString((int) lValue), _T("updated"), withheld);

         ForgetIfWithheld_(sPropertyName, withheld);
         OnPropertyChanged_(pProperty);
      }
   }

   void 
   PropertySet::SetBool(const String &sPropertyName, bool bValue)
   {
      std::shared_ptr<Property> pProperty = GetProperty_(sPropertyName);
      bool previousValue = pProperty->GetBoolValue();
      bool bChanged = bValue != previousValue;
      bool withheld = false;
      if (bChanged)
      {
         RefuseIfEnvironmentManaged_(sPropertyName);
         withheld = IsWithheld_(sPropertyName);
      }
      pProperty->SetBoolValue(bValue);

      if (bChanged)
      {
         AuditTrail::Instance()->RecordSettingChange(sPropertyName,
            previousValue ? _T("true") : _T("false"), bValue ? _T("true") : _T("false"), _T("updated"), withheld);

         ForgetIfWithheld_(sPropertyName, withheld);
         OnPropertyChanged_(pProperty);
      }
   }

   void 
   PropertySet::SetString(const String &sPropertyName, const String &sValue)
   {
      std::shared_ptr<Property> pProperty = GetProperty_(sPropertyName);
      String previousValue = pProperty->GetStringValue();
      bool bChanged = sValue != previousValue;
      bool withheld = false;
      if (bChanged)
      {
         RefuseIfEnvironmentManaged_(sPropertyName);
         withheld = IsWithheld_(sPropertyName);
      }
      pProperty->SetStringValue(sValue);

      if (bChanged)
      {
         AuditTrail::Instance()->RecordSettingChange(sPropertyName, previousValue, sValue, _T("updated"), withheld);

         ForgetIfWithheld_(sPropertyName, withheld);
         OnPropertyChanged_(pProperty);
      }
   }

   void
   PropertySet::SetBookkeeping(const String &sPropertyName, const String &sValue)
   {
      std::shared_ptr<Property> pProperty = GetProperty_(sPropertyName);
      if (pProperty->GetStringValue() != sValue)
         pProperty->SetStringValue(sValue);
   }

   bool
   PropertySet::WriteBookkeeping(const String &sPropertyName, const String &sValue)
   {
      if (!Contains(sPropertyName))
      {
         std::shared_ptr<Property> oProperty = std::shared_ptr<Property>(new Property(sPropertyName, 0, String()));
         if (!oProperty->TrySetStringValue(sValue))
            return false;

         items_[sPropertyName] = oProperty;
         return true;
      }

      std::shared_ptr<Property> pProperty = GetProperty_(sPropertyName);
      return pProperty->TrySetStringValue(sValue);
   }

   bool
   PropertySet::Contains(const String &sPropertyName) const
   {
      return items_.find(sPropertyName) != items_.end();
   }

   void
   PropertySet::GetAll(std::map<String, std::pair<long, String> > &values) const
   {
      values.clear();

      for (auto iterProperty = items_.begin(); iterProperty != items_.end(); iterProperty++)
      {
         std::shared_ptr<Property> oProperty = (*iterProperty).second;
         values[(*iterProperty).first] = std::make_pair(oProperty->GetLongValue(), oProperty->GetStringValue());
      }
   }

   void
   PropertySet::EnsureLong(const String &sPropertyName, long lValue)
   {
      if (Contains(sPropertyName))
         return;

      std::shared_ptr<Property> oProperty = std::shared_ptr<Property>(new Property(sPropertyName, lValue, ""));

      // Writes the row. Property inserts when the row is missing, which it is -
      // that is the whole reason this function was reached.
      oProperty->SetLongValue(lValue);

      items_[sPropertyName] = oProperty;

      // No change event: the value that has just been stored is the default, so
      // nothing about the server's behaviour has changed and nothing needs to be
      // told. Refresh() raises one for every property that WAS in the database.
   }

   void
   PropertySet::EnsureString(const String &sPropertyName, const String &sValue)
   {
      if (Contains(sPropertyName))
         return;

      std::shared_ptr<Property> oProperty = std::shared_ptr<Property>(new Property(sPropertyName, 0, sValue));

      if (IsCryptedProperty_(sPropertyName))
         oProperty->SetIsCrypted();

      oProperty->SetStringValue(sValue);

      items_[sPropertyName] = oProperty;
   }

   void
   PropertySet::OnPropertyChanged_(std::shared_ptr<Property> pProperty)
   {
      // Notify configuration that a setting has changed.
      Configuration::Instance()->OnPropertyChanged(pProperty);
   }

   bool 
   PropertySet::IsCryptedProperty_(const String &sPropertyName)
   {
      if (sPropertyName == PROPERTY_SMTPRELAYER_PASSWORD)
         return true;

      // The VAPID private key. Protected at rest for the same reason the relayer
      // password is: a database dump carried off the machine must not carry with
      // it the key that lets whoever has it send a notification to every phone
      // subscribed to this server.
      if (sPropertyName == PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY)
         return true;

      // The backup's own passphrase. Stored the way the relayer's password is
      // stored, and left out of the backup by XMLStore below.
      if (sPropertyName == PROPERTY_BACKUPENCRYPTIONPASSPHRASE)
         return true;

      // The client secrets this server signs readers in to Gmail and Outlook.com
      // with: the relayer password's reason again - a database dump must not
      // carry the credential that lets whoever has it act as this server.
      if (sPropertyName == PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET ||
          sPropertyName == PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET)
         return true;

      // The client secret this server signs its own users in with at the
      // organisation's identity provider. Same reason again: whoever holds it
      // can ask that provider for tokens as this server.
      if (sPropertyName == PROPERTY_OIDC_CLIENT_SECRET)
         return true;

      // The bearer token this server submits attachments to the sandbox with
      // (AntiVirus/Sandbox.h). The same reason once more: whoever holds it can
      // submit files to that service, and read its verdicts, as this server.
      if (sPropertyName == PROPERTY_SANDBOX_TOKEN)
         return true;

      // The SNMP agent's two passwords and its community (Util/SnmpAgent.h).
      // Whoever holds the passwords can read everything the agent serves as its
      // user; whoever holds the community, while SNMPv2c is on, can read it too.
      if (sPropertyName == PROPERTY_SNMP_V3_AUTH_PASSWORD ||
          sPropertyName == PROPERTY_SNMP_V3_PRIV_PASSWORD ||
          sPropertyName == PROPERTY_SNMP_V2C_COMMUNITY)
         return true;

      // The object store's secret access key, account key and SAS
      // (docs/ObjectStorage.md). Whoever holds them reads and writes every
      // message this server keeps there.
      if (sPropertyName == PROPERTY_MESSAGE_STORE_OBJECT_SECRET ||
          sPropertyName == PROPERTY_MESSAGE_STORE_OBJECT_SAS)
         return true;

      return false;
   }

   bool
   PropertySet::IsWithheld_(const String &sPropertyName)
   {
      // Asked only where the answer is used: an administrator's change, which the
      // audit trail records. Nothing else records, and nothing else forgets.
      if (!AuditTrail::HasActor())
         return false;

      return EnvironmentSettings::IsWithheld(EnvironmentSettings::StoreDatabase, sPropertyName);
   }

   void
   PropertySet::ForgetIfWithheld_(const String &sPropertyName, bool withheld)
   {
      // The value an administrator has just written is theirs, not the file's, and
      // is shown as any other is from now on.
      if (withheld)
         EnvironmentSettings::ForgetSecretOrigin(EnvironmentSettings::StoreDatabase, sPropertyName);
   }

   void
   PropertySet::RefuseIfEnvironmentManaged_(const String &sPropertyName)
   {
      // Throws EnvironmentSettings::Refusal for an administrator's change, before
      // anything is written; lets everything else through - --apply-environment
      // itself, a restore, the server's own bookkeeping - since none of those has
      // an audit actor on the thread.
      EnvironmentSettings::RefuseAdministratorChange(EnvironmentSettings::StoreDatabase, sPropertyName);
   }

   bool
   PropertySet::XMLStore(XNode *pBackupNode)
   {
      XNode *pPropertiesNode = pBackupNode->AppendChild(_T("Properties"));
      auto iterProperty = items_.begin();

      while (iterProperty != items_.end())
      {
         std::shared_ptr<Property> oProperty = (*iterProperty).second;

         // THE ONE SETTING THAT IS NEVER IN THE BACKUP.
         //
         // This is the passphrase the archive being written is encrypted with, and
         // the archive is the place it must never be: a key kept inside the lock it
         // opens is not a key. It is left out of the settings section as
         // PasswordPepper is left out of the stored [Settings] (IniSettingStore),
         // and for the same reason - so a restore also never overwrites the
         // passphrase this server is using with one from an archive, which would
         // leave the next backup encrypted under a passphrase nobody was told
         // about. Whoever restores enters the passphrase they hold.
         if (String(oProperty->GetName()).CompareNoCase(PROPERTY_BACKUPENCRYPTIONPASSPHRASE) == 0)
         {
            iterProperty++;
            continue;
         }

         // Nor the sandbox's bearer token (AntiVirus/Sandbox.h). It is a
         // credential for a service outside this server that no interface ever
         // reads back, and an archive written in the clear would be the one
         // place it could be read; after a restore it is entered again
         // (docs/Sandbox.md). Left out, a restore also leaves the token this
         // server holds as it is.
         if (String(oProperty->GetName()).CompareNoCase(PROPERTY_SANDBOX_TOKEN) == 0)
         {
            iterProperty++;
            continue;
         }

         // Nor the SNMP agent's (Util/SnmpAgent.h): its two passwords and its
         // community, which are credentials no interface reads back, on the
         // sandbox token's terms; and its engine seed and every engine's boot
         // count (SnmpAgent::IsEngineRecord: the seed's name, a boots row's name,
         // and the single boots row of the agent's first, unreleased commit),
         // which are this installation's engines. A restore that brought
         // an older boot count back would let a message recorded since be accepted
         // again, and one onto another server would give two servers one engine.
         // After a restore the passwords are entered again (docs/Snmp.md).
         // Nor the object store's secret key and SAS token (docs/ObjectStorage.md):
         // each reads and writes every message in the bucket, this server's
         // future mail among them, and the archive would carry them in the
         // clear. After a restore they are entered again; left out, a restore
         // leaves the ones this server holds as they are.
         if (String(oProperty->GetName()).CompareNoCase(PROPERTY_SNMP_V3_AUTH_PASSWORD) == 0 ||
             String(oProperty->GetName()).CompareNoCase(PROPERTY_SNMP_V3_PRIV_PASSWORD) == 0 ||
             String(oProperty->GetName()).CompareNoCase(PROPERTY_SNMP_V2C_COMMUNITY) == 0 ||
             SnmpAgent::IsEngineRecord(String(oProperty->GetName())) ||
             String(oProperty->GetName()).CompareNoCase(PROPERTY_MESSAGE_STORE_OBJECT_SECRET) == 0 ||
             String(oProperty->GetName()).CompareNoCase(PROPERTY_MESSAGE_STORE_OBJECT_SAS) == 0)
         {
            iterProperty++;
            continue;
         }

         // Nor the records of which settings the environment manages and which of
         // their values a file supplied: they describe the deployment this database
         // runs under, which a restore onto another server does not bring with it,
         // and a server that never runs --apply-environment would otherwise refuse
         // changes to those settings for ever (docs/EnvironmentConfiguration.md).
         // And a profile's way back and the record of which profile was applied
         // (docs/Profiles.md): this server's history with a profile, not the
         // configuration an archive carries - a snapshot restored onto the values it
         // was not taken of would put back values that were never there. And the
         // mark that this database was made by a new installation's create step and
         // its defaults are still to be decided (Application/NewInstallDefaults.h):
         // restored onto another database it would claim that one was new.
         // Nor the record of the stored-secrets key and of its last export
         // (Util/StoredSecrets.h): the archive carries the sealed values opened, and a
         // restore seals them under the restoring installation's key - carried there,
         // this installation's record would stop that one sealing anything.
         if (String(oProperty->GetName()).CompareNoCase(EnvironmentSettings::MarkerName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(EnvironmentSettings::SecretMarkerName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(Profiles::SnapshotName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(Profiles::AppliedName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(NewInstallDefaults::PendingName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(StoredSecrets::KeyIdRowName()) == 0 ||
             String(oProperty->GetName()).CompareNoCase(StoredSecrets::ExportRowName()) == 0)
         {
            iterProperty++;
            continue;
         }

         XNode *pNode = pPropertiesNode->AppendChild(String(oProperty->GetName()));

         pNode->AppendAttr(_T("LongValue"), StringParser::IntToString(oProperty->GetLongValue()));
         pNode->AppendAttr(_T("StringValue"), oProperty->GetStringValue());

         iterProperty++;
      }      

      return true;
   }

   bool
   PropertySet::XMLLoad(XNode *pBackupNode)
   {
      XNode *pPropertiesNode = pBackupNode->GetChild(_T("Properties"));
      if (!pPropertiesNode)
         return true;

      for (int i = 0; i < pPropertiesNode->GetChildCount(); i++)
      {
         XNode *pPropertyNode = pPropertiesNode->GetChild(i);
         String sName = pPropertyNode->name;

         // XMLStore never writes it; an archive that carries one anyway was
         // written by something else, and it does not get to set what this
         // server's backups are encrypted with.
         if (sName.CompareNoCase(PROPERTY_BACKUPENCRYPTIONPASSPHRASE) == 0)
            continue;

         // Nor the sandbox's token, which XMLStore leaves out too: the token
         // this server holds stays until an administrator enters another.
         if (sName.CompareNoCase(PROPERTY_SANDBOX_TOKEN) == 0)
            continue;

         // Nor the SNMP agent's secrets and records, which XMLStore leaves out:
         // what this server holds stays.
         // Nor the object store's secret key and SAS token, which XMLStore
         // leaves out too.
         if (sName.CompareNoCase(PROPERTY_SNMP_V3_AUTH_PASSWORD) == 0 ||
             sName.CompareNoCase(PROPERTY_SNMP_V3_PRIV_PASSWORD) == 0 ||
             sName.CompareNoCase(PROPERTY_SNMP_V2C_COMMUNITY) == 0 ||
             SnmpAgent::IsEngineRecord(sName) ||
             sName.CompareNoCase(PROPERTY_MESSAGE_STORE_OBJECT_SECRET) == 0 ||
             sName.CompareNoCase(PROPERTY_MESSAGE_STORE_OBJECT_SAS) == 0)
            continue;

         // The environment's records are this server's deployment, not the archive's,
         // a profile's records are this server's history, as is the mark of a
         // new installation's database, and the stored-secrets key's record is
         // this installation's key, not the archive's.
         if (sName.CompareNoCase(EnvironmentSettings::MarkerName()) == 0 ||
             sName.CompareNoCase(EnvironmentSettings::SecretMarkerName()) == 0 ||
             sName.CompareNoCase(Profiles::SnapshotName()) == 0 ||
             sName.CompareNoCase(Profiles::AppliedName()) == 0 ||
             sName.CompareNoCase(NewInstallDefaults::PendingName()) == 0 ||
             sName.CompareNoCase(StoredSecrets::KeyIdRowName()) == 0 ||
             sName.CompareNoCase(StoredSecrets::ExportRowName()) == 0)
            continue;

         String sStringValue = pPropertyNode->GetAttrValue(_T("StringValue"));
         int iLongValue = _ttoi(pPropertyNode->GetAttrValue(_T("LongValue")));

         std::shared_ptr<Property> pProperty = GetProperty_(sName);
         if (pProperty)
         {
            pProperty->SetStringValue(sStringValue);
            pProperty->SetLongValue(iLongValue);
         }
      }

      return true;
   }
}
