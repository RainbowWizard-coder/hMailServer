// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The stored-secrets key, and the secrets it seals (docs/SecretsAtRest.md).
//
// An account's second factor and the [Settings] secrets live in the DATABASE,
// which more than one machine may use: a warm standby shares it, a move to new
// hardware restores it, a failback returns to it. A machine-bound key - DPAPI, or
// the Linux key file of one data directory - seals a value that only the machine
// that wrote it can open. So these values are sealed under a key of the
// installation's instead: 32 random bytes, kept in each node's hMailServer.ini
// ([Security] StoredSecretsKey) under that node's own machine store, never in the
// database, and carried to every other node by an administrator -
// --export-stored-secrets-key FILE on one, --import-stored-secrets-key FILE on
// the next - as the password pepper is carried.
//
// WHO MAKES THE KEY. Never the service, and nothing unelevated: the service may
// run as an account with no write on hMailServer.ini (NT SERVICE\hMailServer, the
// packaged Linux user), and a key made by whichever node happened to start first
// is how two nodes came to seal one database under two keys. A key is made only
// by an elevated step - the Windows installer on a new installation, after the
// database is created; hMailServer.exe or `sudo hmailserver`
// --create-stored-secrets-key; the Linux --create-database when it creates the
// schema - and never on an upgrade, where the setup checklist and the upgrade
// notes say how to turn sealing on. Without a key the server behaves as before
// this row: values stay as they are.
//
// THE DATABASE IS THE ARBITER. Making the key records its id in the database, in
// the same step (the hm_settings row StoredSecretsKeyId): the first key recorded
// is the installation's, a second is refused with the import named, and an import
// is checked against the record. A node whose key is not the recorded one seals
// nothing (HM7495, at each start). The id reveals nothing of the key.
//
// THE KEY'S FATE IS EXPLICIT. It exists on the machines that hold it and in the
// files exported from them, and nowhere else: a database-level backup restored
// without it brings every sealed value back unopenable. So making it says to
// export it at once, the checklist stays open until an export has run (the row
// StoredSecretsKeyExported), and when it is lost after all - no node holds it,
// and no export was kept - --forget-sealed-secrets KEYID clears what was sealed
// under that key alone - second factors to re-enrol, settings to enter again -
// so that a new key can be made.
//
// The sealed form is one line of text:
//
//   HMK1:<key id>:<base64 of nonce || ciphertext || tag>
//
// AES-256-GCM (MailboxCrypto's), a fresh 12-byte nonce each time, the 16-byte
// tag, and as associated data the format, the key id and what the value is -
// the setting's name, or the second factor's column and account id - so that one
// value copied into another's place does not open there. Anyone who can write the
// database can still store a second factor as generated, which is read as it is:
// the binding keeps an envelope where it was made, it is not an authorisation.
#pragma once

#include <set>
#include <vector>

namespace HM
{
   class StoredSecrets
   {
   public:
      enum KeyState
      {
         // No [Security] StoredSecretsKey in this node's file.
         KeyNone = 0,
         // One, opened.
         KeyHeld = 1,
         // One that does not open here: the line was copied from another
         // machine's file, or the Linux key file it was sealed under is gone.
         KeyUnopenable = 2
      };

      // Whether a stored value is a sealed one ("HMK1:").
      static bool IsEnvelope(const String &stored);

      // The key id an envelope names; empty when it is not one.
      static AnsiString EnvelopeKeyId(const String &stored);

      // What a value is, for the associated data: an account's second factor by
      // the account's id, and a [Settings] key by its name (matched without case,
      // as the file matches keys).
      static String TotpPurpose(__int64 accountId);
      static String SettingPurpose(const String &name);

      // The sealed form of plain, under the key this node holds. False - and
      // nothing to store - when there is no key, when it is not the key the
      // database records (MaySeal), when that could not be checked, or when
      // OpenSSL fails. A process that did not run EnsureAtStart - a verb such as
      // --set-setting or --apply-environment - checks the database's record the
      // first time it seals, and seals nothing, said once (HM7496), when its key
      // is not the database's or the database cannot be read.
      static bool Seal(const String &plain, const String &purpose, String &envelope);

      // A stored value as it is used. Anything that is not an envelope is
      // returned as it is, with opened true: a value stored as written before this
      // row, while protection was off, or while this installation had no key. An
      // envelope that does not open here - no key, another key, a changed byte,
      // another purpose - answers empty with opened false, and the caller says
      // what that costs.
      static String Open(const String &stored, const String &purpose, bool &opened);

      // This node's key, as [Security] StoredSecretsKey stands in the settings
      // IniFileSettings last loaded; read again whenever that line changes, and at
      // every start and reinitialise, so that a Linux key file put back is taken.
      static KeyState State();

      // This node's key id; empty when it holds none.
      static AnsiString HeldKeyId();

      // Whether this node seals new values: it holds a key, and that key is the
      // one the database records (or, on a database from before the record, the
      // one its envelopes were made under).
      static bool MaySeal();

      // The two hm_settings rows: the installation's key id, and the key id and
      // time of the last export. The server's bookkeeping, as a profile's records
      // are: read and written by this unit alone, never a property, and never in
      // a backup - a restore onto another installation must not carry this one's
      // record there.
      static const TCHAR *KeyIdRowName();
      static const TCHAR *ExportRowName();

      // The key id the database records; empty when it records none. False when
      // the database could not be read.
      static bool ReadDatabaseKeyId(AnsiString &keyId);

      // The key ids of every envelope the database holds - the [Settings] rows and
      // the accounts' second factors. False when the database could not be read.
      static bool CollectDatabaseKeyIds(std::set<AnsiString> &keyIds);

      // The last export the database records: the key id it was of, and when
      // (seconds since the epoch, UTC). False when there is none or it could not
      // be read.
      static bool ReadExportRecord(AnsiString &keyId, __int64 &when);

      // At every start and reinitialise, with the database open and before the
      // settings store is reconciled. Writes nothing into hMailServer.ini, ever:
      // it judges this node's key against the database's record and says what is
      // wrong at each start (HM7495) - no key where the database records one, a
      // key that does not open, a key that is not the database's, values sealed
      // under a key no node here holds - naming the step that mends it. A node
      // holding a key, on a database that records none and holds nothing sealed
      // under another, records its key there. Without a key and with nothing to
      // open, sealing is off: one application-log line says so.
      static void EnsureAtStart(bool protectionOn);

      // A line for the application log from the start: kept until the logger has
      // been configured (LogNotes, after Configuration::Load), which at a cold
      // start is after the settings are reconciled - a line logged before it would
      // be dropped whatever logging is set to. Logged at once outside a start.
      static void Note(const String &line);
      static void LogNotes();

      // The elevated verbs, each with the database open.
      //
      // CreateKey: a new key, written into this node's file and recorded as the
      // database's in the same step - the file first, flushed to the disk, then
      // the record, and the line taken back out if the record could not be made
      // or names another key. Refused where the database records a key or holds
      // values sealed under one: that key is imported, never replaced. A node that
      // already holds the database's key succeeds with alreadyHeld true.
      static bool CreateKey(AnsiString &keyId, bool &alreadyHeld, String &problem);

      // The key file an administrator carries to another node: three lines, the
      // format, the id and the key in base64. Refused when this node holds none,
      // and when the database records another.
      static bool ExportText(AnsiString &text, AnsiString &keyId, String &problem);

      // --export-stored-secrets-key FILE: ExportText's text written to path by
      // ProtectedFile::WriteNew - a new file, never over one - and the text
      // written over (OPENSSL_cleanse) however this ends. False with problem
      // when ExportText refused, nothing written; false with writeError when
      // the file could not be written.
      static bool ExportFile(const String &path, AnsiString &keyId, String &problem, String &writeError);

      // The export recorded in the database, for the setup checklist.
      static bool RecordExport(const AnsiString &keyId);

      // A key file ExportText wrote, made this node's key. Refused when the text is
      // not one, when the database records another key, and when this node holds
      // another key that opens - replacing that is removing its line first,
      // deliberately. Importing the key this node already holds changes nothing
      // and succeeds. On a database that records no key it is recorded.
      static bool ImportText(const AnsiString &text, AnsiString &keyId, String &problem);

      // --import-stored-secrets-key FILE: the key file at path read and given to
      // ImportText. It is read by the operating system's own reads - ReadFile,
      // read() - straight into one buffer, and no further than KeyFileMaxBytes,
      // so that no C runtime buffer, no ByteBuffer and no wide String holds a
      // copy of the key; that buffer, and every copy ImportText makes, is
      // written over (OPENSSL_cleanse) before it goes back to the heap, however
      // this ends - an exception included. The one copy kept, the key this
      // process holds once it has read it from its line, is written over as
      // the process ends. A file with a UTF-16LE byte order mark - what Windows
      // PowerShell 5's Out-File and > write - is narrowed in the same buffer; a
      // UTF-8 one is stepped over by ImportText. Refused as ImportText refuses,
      // and when the file cannot be read or is larger than a key file can be.
      static bool ImportFile(const String &path, AnsiString &keyId, String &problem);

      // The most of a key file ImportFile reads: an exported one is about a
      // hundred bytes, twice that in UTF-16.
      static const size_t KeyFileMaxBytes = 65536;

      // --forget-sealed-secrets KEYID, for a key nobody holds any more: every
      // envelope that names that key is cleared - an account's second factor
      // removed, so that it enrols again; a [Settings] secret emptied, in the row
      // and the file, so that it is entered again - and the database's record, and
      // the export's, only where they name it. Nothing sealed under another key is
      // touched, and no other key is recorded. Refused without a key id, where this
      // node's own key line does not open (it may be the key named), and where this
      // node holds the key named (nothing under it is lost). One line per item in
      // forgotten, all of them logged as HM7497.
      static bool ForgetLostKey(const AnsiString &lostKeyId, std::vector<String> &forgotten, String &problem);

      // The verbs as a person types them on this platform, for the sentences
      // that name them.
      static String CreateVerbText();
      static String ExportVerbText();
      static String ImportVerbText();
      static String ForgetVerbText();

      // Seal and Open round-tripped under a key made for the test, with their
      // refusals: another purpose, another account, a changed byte, another key
      // id, not an envelope.
      static void Test();
   };
}
