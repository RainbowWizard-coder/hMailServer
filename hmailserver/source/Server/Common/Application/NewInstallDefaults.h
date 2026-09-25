// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class DALConnection;

   /// <summary>
   /// What a new installation starts with, so that it is secure before anyone
   /// configures it (docs/SecureDefaults.md).
   ///
   /// THE THREE CHANGES. A self-signed certificate made for this machine - an RSA
   /// 2048 key written readable by the server's account and the administrators
   /// only, a certificate for 825 days naming the host - under the data
   /// directory's Certificates folder, and a certificate record naming it
   /// (CertificateName). STARTTLS offered with it on the four listeners a new
   /// database has - 25, 587, 110 and 143 - and optional on every one of them:
   /// required on 25 it would refuse mail from a server that cannot do TLS, and
   /// required on the other three it would refuse this computer's own clients,
   /// which the next change leaves alone. And the Internet range requiring TLS
   /// for authentication, so that a password from any address but this
   /// computer's is refused until TLS is up; the My computer range is left as it
   /// is.
   ///
   /// ONLY A DATABASE THIS INSTALLATION CREATED. Never one that existed before,
   /// whatever its size: a small server in service with the stock listeners and
   /// ranges looks, from its configuration alone, exactly like a new one, and
   /// requiring TLS for its sign-ins would lock out every client that has never
   /// used TLS. So the create path marks the database it makes - COM's
   /// CreateInternalDatabase and CreateExternalDatabase (DBSetupQuick, and DBSetup
   /// when it creates) and hmailserver --create-database when it runs the create
   /// script - with the hm_settings row PendingName(), and nothing else writes it:
   /// not SetDefaultDatabase, which attaches a database that exists, not an
   /// upgrade, not --create-database finding a schema already there. The verb
   /// requires the mark and clears it once it has decided, so it is answered
   /// once. The mark carries the time it was made, and the Linux unit's start
   /// does not act on one made more than a day from its clock, either way - its
   /// server has run without the defaults since, or the clock cannot say - and
   /// keeps it for a run by hand, which has no such limit. And it still asks the
   /// shape a new database has: the four listeners
   /// with no TLS and no certificate, the two IP ranges with the options
   /// CreateTables*.sql gives them, no certificate at all, no account, no domain
   /// (the sample domain and its accounts not counted) and no route. Anything else
   /// is left as it is, exit 5. A database that cannot be read well enough to
   /// tell is not judged at all, and neither is one whose listeners and ranges
   /// cannot be read again just before they are changed: exit 2, the mark kept,
   /// and the next run asks again. The range is set only once all four
   /// listeners offer STARTTLS.
   ///
   /// WHERE IT RUNS. Nothing calls this at a server's start. The Windows installer
   /// runs it after the database step of a new installation, and the Linux unit
   /// before a start when the file FirstStartMarkerFile() is there, which
   /// --create-database leaves when it made the database. The create scripts, and
   /// so the regression benches, are what they always were; a bench made by the
   /// create path carries the mark, and nothing runs the verb against it but the
   /// fixture that writes and clears the mark itself.
   ///
   /// WHAT IS SAID. One line per change on standard output, a refusal on standard
   /// error, and the audit trail under the command line's name, as the profile
   /// verb writes it. A change that could not be written is also an error in the
   /// error log (HM7511), so that an installation that finished without it does
   /// not pass for one that did not.
   /// </summary>
   class NewInstallDefaults
   {
   public:

      /// <summary>
      /// The name of the certificate record the defaults make. The setup checklist
      /// reads it as the self-signed certificate to replace, and the ACME client
      /// takes over the listeners that hold it when it has a certificate of its
      /// own.
      /// </summary>
      static const TCHAR *CertificateName();

      /// <summary>Whether a certificate record is the one the defaults made, by its name.</summary>
      static bool IsInstallationCertificate(const String &name);

      /// <summary>
      /// Where the pair is written: the Certificates folder of the data directory.
      /// The key in it has a protected ACL (ProtectedFile) and inherits nothing from
      /// the data directory, so whatever grants a service account its folders must
      /// grant this one by name as well.
      /// </summary>
      static String CertificateDirectory();

      /// <summary>
      /// The hm_settings row that says this database was made by the create path
      /// and its defaults are still to be decided (settinginteger 1). A record of
      /// this server's history, not a setting: in no create script, no default, left
      /// out of the XML backup.
      /// </summary>
      static const TCHAR *PendingName();

      /// <summary>
      /// Writes that row into a database the caller has just created, on the
      /// caller's own connection to it. False, with a sentence, when it could not;
      /// the create still stands, and the defaults are then not applied to it.
      /// </summary>
      static bool MarkCreated(std::shared_ptr<DALConnection> connection, String &error);

      /// <summary>
      /// The file --create-database leaves beside the data when it made the
      /// database, which the Linux unit's first start answers (--if-marked):
      /// .new-installation in the data directory the configuration names, read
      /// from the file alone (IniFileSettings::ReadDataDirectory), so that the
      /// answer is the same whether or not the configuration has been loaded.
      /// </summary>
      static String FirstStartMarkerFile();

      /// <summary>
      /// hMailServer.exe --apply-new-install-defaults on Windows, and
      /// hmailserver --apply-new-install-defaults [--if-marked [FILE]] on Linux:
      /// the configuration and the database opened as the service opens them, the
      /// server judged, the changes made, and one line for each. With ifMarked,
      /// nothing at all is done - not even the database opened - unless the marker
      /// exists, the database is reached at the first attempt, and the marker is
      /// removed once the question has been answered. The marker is markerFile
      /// when one is named, and otherwise FirstStartMarkerFile() of the
      /// configuration this reads - where --create-database wrote it with the same
      /// configuration - found from the configuration alone. On Linux it is
      /// refused as root, whose key the service could not read.
      ///
      /// 0 applied (or no marker); 1 something could not be made or written, and
      /// the lines say what was; 2 no database, it could not be opened or read well
      /// enough to judge, or run as root; 5 not a database this installation
      /// created, not as the create script leaves it, or - with ifMarked - marked
      /// more than a day from now, so nothing was changed. 4,
      /// the service is running, is the Windows entry point's, which asks the
      /// service controller first.
      /// </summary>
      static int RunCommand(bool ifMarked, const String &markerFile);

   private:

      static void Print_(bool error, const String &line);
      static int OpenForCommand_();

      // True when the database is a new installation's. False with why when it
      // is not, or - unknown set - when a read failed and it could not be told.
      // With ifMarked - the unit's start - a mark made more than a day from now
      // either way is not acted on: false with tooOld set, the mark to be kept
      // for a run by hand, which has no such limit.
      static bool IsNewInstallation_(bool ifMarked, String &why, bool &marked, bool &unknown, bool &tooOld);
      static bool ClearMark_();
      static int Apply_();
   };
}
