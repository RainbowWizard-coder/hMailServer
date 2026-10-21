; https://www.progressiverobot.com
; Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
; SPDX-License-Identifier: AGPL-3.0-or-later
;
; The self-test of the Windows installer's decision on an installation of the
; original hMailServer's 5.7 (hmailserver\installation\OriginalHMailServer.iss,
; which the installer includes as it is). It is a setup that installs nothing: its
; InitializeSetup runs the decision over each case below, writes one line a case to
; the file /result= names, and returns False, so that Setup ends there with exit
; code 1 - before a page, a folder or a file. original-guard-test.ps1 compiles it
; with the ISCC that compiles the installer, runs it silently and reads the file;
; the windows-packages jobs run that before they build the installer.
;
; The cases are the installations the decision must tell apart: this fork's own
; (6.x), the original up to 5.6, the original's 5.7 development builds up to schema
; 5708, which are upgraded, and the original's 5.7 releases and pre-release builds
; at 5709 to 5711, and a build of its 5.7 from source, whose hMailServer.exe is
; 1.0.0.0, which are refused - by the database's version when the service answers
; it, and by the original's own upgrade scripts when it does not.

[Setup]
AppName=hMailServer installer self-test
AppVersion=1
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
OutputBaseFilename=original-guard-test
DisableWelcomePage=yes

[Code]
#include "..\..\..\hmailserver\installation\OriginalHMailServer.iss"

var
   g_szLines: String;
   g_iCases: Integer;
   g_iFailed: Integer;

procedure Report(Passed: Boolean; Description: String);
var
   szLine: String;
begin
   g_iCases := g_iCases + 1;
   if Passed then
      szLine := 'ok    ' + Description
   else
   begin
      szLine := 'FAIL  ' + Description;
      g_iFailed := g_iFailed + 1;
   end;
   Log(szLine);
   g_szLines := g_szLines + szLine + #13#10;
end;

procedure ExpectKind(Description: String; ServerMajor, ServerMinor, DatabaseVersion: Integer; OriginalScripts: Boolean; Expected: Integer);
var
   iKind: Integer;
begin
   iKind := OriginalHMailServerKind(ServerMajor, ServerMinor, DatabaseVersion, OriginalScripts);
   Report(iKind = Expected, Description + ': ' + IntToStr(iKind) + ', expected ' + IntToStr(Expected));
end;

procedure ExpectScript(Name: String; Expected: Boolean);
begin
   if Expected then
      Report(IsOriginalOnlyScript(Name), Name + ' is one of the original''s own steps')
   else
      Report(not IsOriginalOnlyScript(Name), Name + ' is not one of the original''s own steps');
end;

procedure ExpectInMessage(Description: String; Kind: Integer; Text: String; Expected: Boolean);
var
   szMessage: String;
begin
   szMessage := OriginalHMailServerMessage(Kind, '5.7.0 build 3013', 5711, 'C:\Program Files\hMailServer');
   Report((Pos(Text, szMessage) > 0) = Expected, Description);
end;

function InitializeSetup(): Boolean;
var
   szResult: String;
   szRecord: String;
   szRead: AnsiString;
begin
   Result := False;
   g_szLines := '';
   g_iCases := 0;
   g_iFailed := 0;

   // This fork's own installations: never refused, whatever else is found.
   ExpectKind('this fork 6.3, database 6108', 6, 3, 6108, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 6.0, database unread', 6, 0, -1, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 6.3, database unread, the original''s scripts left in DBScripts', 6, 3, -1, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 6.3, a database at 5711', 6, 3, 5711, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 6.7, a database at 5711, the original''s scripts left in DBScripts', 6, 7, 5711, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 6.9, database unread, the original''s scripts left in DBScripts', 6, 9, -1, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('this fork 7.0, database unread, the original''s scripts left in DBScripts', 7, 0, -1, True, ORIGINAL_NOT_REFUSED);

   // The original up to 5.6, upgraded in place. Its programs are asked as the 5.7's
   // are, and the two tests take neither its databases nor its DBScripts folders;
   // a folder holding the original's steps past 5708 with no database read is one
   // this fork cannot upgrade whatever hMailServer.exe is in it.
   ExpectKind('the original 5.6, database 5601', 5, 6, 5601, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('the original 5.6, database 5601, the 5.7 scripts left in DBScripts', 5, 6, 5601, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('the original 5.6, database unread, the 5.7 scripts left in DBScripts', 5, 6, -1, True, ORIGINAL_REFUSED_BY_FILES);
   ExpectKind('the original 5.4, database unread', 5, 4, -1, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('the original 4.4, database unread', 4, 4, -1, False, ORIGINAL_NOT_REFUSED);

   // The original's 5.7 built from source without its build script: every 5.7 tag
   // commits the placeholder 1.0.0, so its hMailServer.exe is 1.0.0.0.
   ExpectKind('the original 5.7 built from source (1.0.0.0), database 5711', 1, 0, 5711, True, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7 built from source (1.0.0.0), database unread, its scripts in DBScripts', 1, 0, -1, True, ORIGINAL_REFUSED_BY_FILES);
   ExpectKind('a build from source (1.0.0.0), database unread, no script past 5708', 1, 0, -1, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('a build from source (1.0.0.0), database 5708', 1, 0, 5708, True, ORIGINAL_NOT_REFUSED);

   // The original's 5.7 development builds up to 5708, upgraded in place.
   ExpectKind('a 5.7 development build, database 5708', 5, 7, 5708, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('a 5.7 development build, database 5705', 5, 7, 5705, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('a 5.7 development build, database 5700', 5, 7, 5700, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('a 5.7 development build, database 5708, the 5.7 scripts left in DBScripts', 5, 7, 5708, True, ORIGINAL_NOT_REFUSED);
   ExpectKind('a 5.7 development build, database unread, no script past 5708', 5, 7, -1, False, ORIGINAL_NOT_REFUSED);
   ExpectKind('a 5.7 development build, the service answering 0, no script past 5708', 5, 7, 0, False, ORIGINAL_NOT_REFUSED);

   // The original's 5.7 at 5709 to 5711, refused by the database's version.
   ExpectKind('the original 5.7.0 build 2788 or 2843, database 5709', 5, 7, 5709, True, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7.0 build 3013, database 5711', 5, 7, 5711, True, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7.2, database 5711, no scripts found', 5, 7, 5711, False, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7, database 5710', 5, 7, 5710, True, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('a later original 5.8, database 5800', 5, 8, 5800, True, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7, database 6000, the last before this fork''s first', 5, 7, 6000, False, ORIGINAL_REFUSED_BY_DATABASE);
   ExpectKind('the original 5.7''s programs, database 6001, this fork''s first', 5, 7, 6001, False, ORIGINAL_NOT_REFUSED);

   // The original's 5.7 with its database's version unread: its scripts decide.
   ExpectKind('the original 5.7, service not running, its scripts in DBScripts', 5, 7, -1, True, ORIGINAL_REFUSED_BY_FILES);
   ExpectKind('the original 5.7, the service answering 0, its scripts in DBScripts', 5, 7, 0, True, ORIGINAL_REFUSED_BY_FILES);

   // No hMailServer.exe, or one whose version could not be read.
   ExpectKind('no hMailServer.exe, a database at 5711', -1, -1, 5711, True, ORIGINAL_NOT_REFUSED);

   // The scripts: the original's own steps past 5708, and nothing else.
   ExpectScript('Upgrade5708to5709MSSQL.sql', True);
   ExpectScript('Upgrade5708to5709MSSQLCE.sql', True);
   ExpectScript('Upgrade5708to5709MySQL.sql', True);
   ExpectScript('upgrade5708to5709pgsql.SQL', True);
   ExpectScript('C:\Program Files\hMailServer\DBScripts\Upgrade5708to5711MySQL.sql', True);
   ExpectScript('Upgrade5708to6001MSSQL.sql', False);
   ExpectScript('Upgrade5705to5708MSSQL.sql', False);
   ExpectScript('Upgrade5709to5710MSSQL.sql', False);
   ExpectScript('Upgrade5708to5709MSSQL.txt', False);
   ExpectScript('Upgrade5708to57.sql', False);
   ExpectScript('CreateTablesMSSQL.sql', False);

   // The words: why, that nothing was changed, and the way that works instead.
   Report(OriginalHMailServerMessage(ORIGINAL_NOT_REFUSED, '6.3.7 build 0', 6108, 'C:\Program Files\hMailServer') = '', 'no words when not refused');
   ExpectInMessage('the refusal names the database''s version', ORIGINAL_REFUSED_BY_DATABASE, 'database is at version 5711', True);
   ExpectInMessage('the refusal names the server''s version', ORIGINAL_REFUSED_BY_DATABASE, 'the original hMailServer 5.7.0 build 3013', True);
   ExpectInMessage('the refusal says this is Progressive Robot''s fork', ORIGINAL_REFUSED_BY_DATABASE, 'Progressive Robot''s fork of hMailServer', True);
   ExpectInMessage('the refusal says nothing has been changed', ORIGINAL_REFUSED_BY_DATABASE, 'Nothing has been changed.', True);
   ExpectInMessage('the refusal names the IMAP route', ORIGINAL_REFUSED_BY_DATABASE, 'over IMAP (Migration.md in the documentation, "From the original hMailServer")', True);
   ExpectInMessage('the refusal by database says nothing of starting the service', ORIGINAL_REFUSED_BY_DATABASE, 'start the hMailServer service', False);
   ExpectInMessage('the refusal by files says the version was not read', ORIGINAL_REFUSED_BY_FILES, 'could not be read', True);
   ExpectInMessage('the refusal by files says how a development build gets past it', ORIGINAL_REFUSED_BY_FILES, 'start the hMailServer service and run this installer again', True);
   ExpectInMessage('the refusal by files names the IMAP route', ORIGINAL_REFUSED_BY_FILES, 'Migration.md', True);

   // The record a silent setup run without /LOG leaves: written to the file named,
   // here beside the result, read back and removed.
   szResult := ExpandConstant('{param:result|}');
   if szResult <> '' then
   begin
      szRecord := ExtractFilePath(szResult) + 'refusal-record.txt';
      Report(WriteOriginalHMailServerRefusal(szRecord, OriginalHMailServerMessage(ORIGINAL_REFUSED_BY_DATABASE, '5.7.0 build 3013', 5711, 'C:\Program Files\hMailServer')),
             'the refusal is written to the file named');
      Report(LoadStringFromFile(szRecord, szRead) and (Pos('refused to install, and changed nothing:', szRead) > 0) and
             (Pos('its database is at version 5711.', szRead) > 0) and (Pos('Migration.md', szRead) > 0),
             'the file says that setup refused and changed nothing, and holds the refusal''s words');
      DeleteFile(szRecord);
   end;

   g_szLines := g_szLines + 'CASES ' + IntToStr(g_iCases) + ' FAILED ' + IntToStr(g_iFailed) + #13#10;
   szResult := ExpandConstant('{param:result|}');
   if szResult <> '' then
      SaveStringToFile(szResult, g_szLines, False);
end;
