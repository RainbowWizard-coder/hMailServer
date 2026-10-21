// The decision on an installation of the original hMailServer's 5.7, which this
// fork's setup refuses before it changes anything (OriginalHMailServerRefusal in
// hMailServerInnoExtension.iss, called first in InitializeSetup). [Code] alone,
// with no section header: hMailServerInnoExtension.iss includes it inside its own
// [Code], and so does build\ci\packages\original-guard-test.iss, the self-test the
// windows-packages jobs compile and run, which is why nothing here reads the
// registry, a file or the service: what is found is passed in, and the one file
// written, the refusal's record, is named by the caller.
//
// Both projects' installers register as the uninstall entry hMailServer_is1 (this
// fork's AppId=hMailServer; the original's AppName=hMailServer, with no AppId), so
// each one's setup takes an installation of the other for its own. Up to schema
// 5708 the two share the database's upgrade chain: every release of the original
// up to 5.6 (5601) and its development builds of 5.7 up to 5708 are upgraded in
// place. From there the original went on by steps of its own - 5709 in its
// pre-release builds 2788 and 2843 of 5.7.0 (August 2026), 5710 and 5711 in 5.7.0
// build 3013, 5.7.1 build 3116 and 5.7.2 build 3124 - and this fork by its own,
// from 5708 to 6001. DBUpdater has no step from any version between the two, and
// stops before it changes anything; but setup would run it only after it had
// replaced the original's programs, and this fork's server refuses such a database
// as too old (HM5011), so the original's 5.7 could not be put back without a
// restore. So it is refused before a file is copied.
//
// What identifies such an installation:
//   * its hMailServer.exe's version, in the folder the earlier installation names:
//     before 6. This fork's programs are 6 and later, from its first release
//     (6.0.0-B1), and are never looked at further. The version does not tell the
//     original's 5.7 from its earlier releases, and is not held to 5.7: the
//     original's build stamps 5.7.0.3013, 5.7.1.3116 and 5.7.2.3124, and 5.7.0.2788
//     and 5.7.0.2843, as FILEVERSION, but every one of its 5.7 tags commits the
//     placeholder 1.0.0 (Version.h, section_setup_64.iss), so that a build of it
//     made from source without its build script reports itself as 1.0.0.0, and
//     its 5.7 development builds up to 5708 are 5.7.0 as well. The two tests below
//     decide; neither refuses a database at 5708 or older, nor a DBScripts folder
//     without one of the original's steps past 5708, so the original's releases up
//     to 5.6 (5601) and its development builds up to 5708 are upgraded as before.
//     No hMailServer.exe, or one with no version, is not looked at further either:
//     nothing then says whose installation it is;
//   * then the database's version, hm_dbversion, which the running service answers
//     without a password (Database.CurrentVersion, the original's as this fork's):
//     past ORIGINAL_LAST_SHARED_SCHEMA and before FORK_FIRST_SCHEMA is refused, and
//     anything else is not - which is what upgrades such a development build;
//   * only where that version cannot be read - the service is not running, or does
//     not answer - first the earlier installation's settings file: a [Database]
//     PasswordEncryption of 6, the database's password kept as a DPAPI envelope, is
//     this fork's alone. This fork writes it, from 6.2.0 on and while
//     ProtectStoredSecretsWithDPAPI is on, its default, whenever its database setup
//     makes a database or connects to one, the built-in one among them; the
//     original writes 1 and reads 0 and 1 alone. Such a database is this
//     fork's own, at 6001 or later, which the original's 5.7 installer, run over an
//     installation of this fork, leaves as it was, with the original's scripts
//     beside this fork's in DBScripts. The original's service cannot read its
//     password, so it never answers the version however often it is started, and
//     the database is not refused: this fork's server reads its own password and
//     upgrades it, as before this test was made;
//   * else the scripts the original's 5.7 installs in its DBScripts folder: an
//     upgrade step from 5708 to a version before 6001 (Upgrade5708to5709*.sql),
//     which no development build up to 5708 and no release of this fork carries.
//     The refusal then says that the files decided, and that an installation whose
//     database is at 5708 or older is upgraded once its service runs.
// build\check-installer-scripts.py holds the two numbers to the upgrade chain in
// hmailserver\source\DBScripts: a step from the one to the other, and no step to
// any version between them.

const
   // The last schema version the two projects share, and the first of this fork's
   // own after it: a database past the one and before the other is one only the
   // original's 5.7 makes.
   ORIGINAL_LAST_SHARED_SCHEMA = 5708;
   FORK_FIRST_SCHEMA = 6001;

   // What OriginalHMailServerKind answers.
   ORIGINAL_NOT_REFUSED = 0;
   ORIGINAL_REFUSED_BY_DATABASE = 1;
   ORIGINAL_REFUSED_BY_FILES = 2;

   // The [Database] PasswordEncryption this fork writes for a password kept as a
   // DPAPI envelope (Crypt::ETDPAPI), which the original never writes and cannot
   // read.
   FORK_PASSWORD_ENCRYPTION = 6;

// Whether a database at this version is one the original's 5.7 made and this
// fork cannot upgrade.
function IsOriginalOnlySchema(Version: Integer): Boolean;
begin
   Result := (Version > ORIGINAL_LAST_SHARED_SCHEMA) and (Version < FORK_FIRST_SCHEMA);
end;

// Whether a file in the installation's DBScripts folder is one of the original's
// own steps past 5708: 'Upgrade5708to<version><dialect>.sql', <version> four
// digits that IsOriginalOnlySchema takes. This fork's own step from 5708,
// Upgrade5708to6001, is not.
function IsOriginalOnlyScript(Name: String): Boolean;
var
   szName: String;
   szPrefix: String;
   szRest: String;
   iVersion: Integer;
begin
   Result := False;
   szPrefix := 'upgrade' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) + 'to';
   szName := Lowercase(ExtractFileName(Name));
   if (Copy(szName, 1, Length(szPrefix)) <> szPrefix) or (ExtractFileExt(szName) <> '.sql') then
      Exit;
   szRest := Copy(szName, Length(szPrefix) + 1, 4);
   if Length(szRest) <> 4 then
      Exit;
   iVersion := StrToIntDef(szRest, -1);
   Result := IsOriginalOnlySchema(iVersion);
end;

// Whether the installed hMailServer.exe's version is before 6, that is, not one
// of this fork's: the original's, of any release or build, its 5.7 among them,
// whose version is not held to 5.7 (a build of it from source is 1.0.0.0). -1
// for a version that could not be read, which is not taken. The minor version
// does not decide.
function IsServerBeforeThisFork(ServerMajor, ServerMinor: Integer): Boolean;
begin
   Result := (ServerMajor >= 0) and (ServerMajor < 6);
end;

// Whether a settings file's [Database] PasswordEncryption is this fork's alone.
function IsForkOnlyPasswordEncryption(Value: Integer): Boolean;
begin
   Result := Value = FORK_PASSWORD_ENCRYPTION;
end;

// The decision. ServerMajor and ServerMinor: the installed hMailServer.exe's
// version, -1 when there is none or it could not be read. DatabaseVersion: the
// database's version as the running service answers it, -1 (or 0) when it could
// not be read. OriginalScripts: whether the installation's DBScripts folder holds
// a script IsOriginalOnlyScript takes. ForkSecret: whether the installation's
// settings file keeps the database's password as only this fork does
// (IsForkOnlyPasswordEncryption), which makes the database this fork's own.
function OriginalHMailServerKind(ServerMajor, ServerMinor, DatabaseVersion: Integer; OriginalScripts, ForkSecret: Boolean): Integer;
begin
   Result := ORIGINAL_NOT_REFUSED;
   if not IsServerBeforeThisFork(ServerMajor, ServerMinor) then
      Exit;
   if DatabaseVersion > 0 then
   begin
      if IsOriginalOnlySchema(DatabaseVersion) then
         Result := ORIGINAL_REFUSED_BY_DATABASE;
   end
   else if OriginalScripts and not ForkSecret then
      Result := ORIGINAL_REFUSED_BY_FILES;
end;

// The refusal's words, for the message box and the setup log: what was found, why
// it is refused, that nothing has been changed, and the way that works instead.
// Empty for ORIGINAL_NOT_REFUSED.
function OriginalHMailServerMessage(Kind: Integer; ServerVersion: String; DatabaseVersion: Integer; Folder: String): String;
var
   szFound: String;
begin
   Result := '';
   if Kind = ORIGINAL_REFUSED_BY_DATABASE then
      szFound := 'This computer has the original hMailServer ' + ServerVersion + ' installed, in ' + Folder +
                 ', and its database is at version ' + IntToStr(DatabaseVersion) + '.'
   else if Kind = ORIGINAL_REFUSED_BY_FILES then
      szFound := 'This computer has the original hMailServer ' + ServerVersion + ' installed, in ' + Folder +
                 ', with the original''s own database upgrade steps past version ' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) +
                 ' in its DBScripts folder. Its database''s version could not be read, because the hMailServer service is not running or did not answer.'
   else
      Exit;

   Result := szFound + #13#10#13#10 +
             'This installer is Progressive Robot''s fork of hMailServer, which cannot upgrade a database of the original''s 5.7 ' +
             '(version ' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA + 1) + ' and later). Installed over it, setup would replace the original''s ' +
             'programs with a server that refuses that database, and the original''s 5.7 could not be put back without restoring a backup. ' +
             'Nothing has been changed.' + #13#10#13#10 +
             'To move to this fork, install it on another computer, make the domains and accounts there, and copy each mailbox''s mail ' +
             'across over IMAP (Migration.md in the documentation, "From the original hMailServer").';
   if Kind = ORIGINAL_REFUSED_BY_FILES then
      Result := Result + #13#10#13#10 +
                'If its database is at version ' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) +
                ' or older - a release of the original up to 5.6, or one of its 5.7 development builds - ' +
                'start the hMailServer service and run this installer again: setup then reads the database''s version, and upgrades such a database.';
end;

// Writes the refusal to FileName, in UTF-8, after a line that says when and which
// setup refused, so that a silent setup run without /LOG, whose message box is
// suppressed, leaves the reason where it can be found (InitializeSetup passes the
// file in the temporary folder, RefusalFileName). True when it was written.
function WriteOriginalHMailServerRefusal(FileName, Refusal: String): Boolean;
var
   Lines: TArrayOfString;
begin
   SetArrayLength(Lines, 3);
   Lines[0] := GetDateTimeString('yyyy-mm-dd hh:nn:ss', '-', ':') + ' ' + ExtractFileName(ExpandConstant('{srcexe}')) +
               ' refused to install, and changed nothing:';
   Lines[1] := '';
   Lines[2] := Refusal;
   Result := SaveStringsToUTF8File(FileName, Lines, False);
end;
