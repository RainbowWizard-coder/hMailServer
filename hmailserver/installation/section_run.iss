[Run]
; Install the bundled .NET {#DOTNET_MAJOR} Desktop Runtime, but only when it is
; missing. This entry is the FIRST attempt, for every component: Inno processes a
; [Run] entry without the postinstall flag before ssPostInstall, so it runs before
; RunPostInstallTasks and the database tools, and InstallDotNetRuntime() there is
; the second attempt, for when this one failed (it says so; this entry does not).
; Until 25 September 2026 this comment and the [Code] said the reverse; the smoke
; test's install log shows the order. Note that this double-installed the runtime
; on every install for as long as DotNetDesktopMissing was broken - the Check could
; never go false.
Filename: "{tmp}\{#DOTNET_RUNTIME_FILE}"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing .NET {#DOTNET_MAJOR} Desktop Runtime..."; Check: DotNetDesktopMissing; Components: server controlpanel;
Filename: "{app}\ControlPanel\hMailCP.exe";  Flags: skipifsilent postinstall nowait; Description: Run hMailServer Control Panel; Components: controlpanel;
