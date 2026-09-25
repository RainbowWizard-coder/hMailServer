[UninstallRun]
; The service is stopped, and its process waited for, before these entries run:
; CurUninstallStepChanged in hMailServerInnoExtension.iss does both at usUninstall,
; which Inno reaches before [UninstallRun] and before any file is deleted. A net
; stop is not enough on its own, although this file used to say it was: net.exe
; returns when the service reports STOPPED, 63 to 114 ms after it is asked, and the
; process lives about 1.1 seconds longer, while the uninstaller began deleting 0.7
; seconds after net stop returned. The installer smoke test found hMailServer.exe
; and six DLLs left in Bin that way on 25 September 2026 ("Failed to delete the
; file; it may be in use (5)", "Removed all? No"). The two net stop entries stay:
; an uninstall log written by an earlier installer holds them anyway, and they cost
; nothing when the service has already stopped.
;
; Order matters, and it used to be the wrong way round: /Unregister came first and
; called DeleteService on a *running* service, which only marks it for deletion and
; leaves the process running - so the uninstaller then began deleting {app} with
; hMailServer.exe still holding its own files open, and COM was unregistered from
; under a live server. Stopping first, and waiting for the process, means it is
; gone before anything is removed.
;
; RunOnceId on every entry. Inno records these entries in the uninstall log at every
; installation, so after an upgrade the log held each of them twice and the
; uninstall ran each twice. The second /Unregister found nothing registered, wrote
; the COM AppID again and left it behind (hMailServer.cpp registered the AppID before
; it read its command line; it no longer does). With an id an entry runs once,
; however many installations recorded it. Entries an earlier installer recorded
; carry no id and still run once more each, which is why /Unregister has to be safe
; to run twice, and now is: it creates nothing.
;
; skipifdoesntexist on every {app} entry. Without it Inno raises a "cannot execute"
; error for each missing file, and the entries below for the bundled MySQL and for
; hSMTPServer/hPOP3Server refer to files that have not shipped since hMailServer 4
; and 3 respectively: on a current installation that was three error dialogs on
; every single uninstall, and they are kept only so that an uninstall of a very old
; installation still tidies up after itself.
Filename: "{sys}\net.exe"; Parameters: "STOP hMailServer"; Flags: runhidden; RunOnceId: "StopHMailServer"
Filename: "{sys}\net.exe"; Parameters: "STOP hMailServerMySQL"; Flags: runhidden; RunOnceId: "StopHMailServerMySQL"
Filename: "{app}\Bin\hMailServer.exe"; Parameters: "/Unregister"; Flags: runhidden skipifdoesntexist; RunOnceId: "UnregisterHMailServer"
Filename: "{app}\MySQL\Bin\mysqld-nt.exe"; Parameters: "--remove hMailServerMySQL"; Flags: runhidden skipifdoesntexist; RunOnceId: "RemoveHMailServerMySQL"
Filename: "{app}\Bin\hSMTPServer.exe"; Parameters: "unregister"; Flags: runhidden skipifdoesntexist; RunOnceId: "UnregisterHSMTPServer"
Filename: "{app}\Bin\hPOP3Server.exe"; Parameters: "unregister"; Flags: runhidden skipifdoesntexist; RunOnceId: "UnregisterHPOP3Server"
