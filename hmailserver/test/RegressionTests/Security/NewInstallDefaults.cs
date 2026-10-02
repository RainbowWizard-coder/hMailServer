// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    A new installation is secure before anyone configures it
   ///    (docs/SecureDefaults.md): hMailServer.exe --apply-new-install-defaults, the
   ///    verb the Windows installer runs for a database it has just made and the Linux
   ///    unit runs at a first install's first start, driven here without either.
   ///
   ///    The bench is made into what the verb is for: a database the create step has
   ///    just made. Every port is deleted and the four put back without TLS, the two
   ///    IP ranges as the create scripts make them, every certificate deleted, and
   ///    the mark COM's create methods and --create-database write
   ///    (NewInstallDefaultsPending) written into it - the bench's own database was
   ///    upgraded, not created, and has none; the fixture's domain is deleted, since
   ///    a new database has no domain, and made again with its account after the
   ///    verb. So the verb is run against the bench itself, with the
   ///    service stopped, and the server started again to serve what it wrote. The
   ///    teardown puts all of it back, step by step, and restarts the service
   ///    whatever failed, so that no later fixture meets a listener offering
   ///    STARTTLS.
   ///
   ///    What a client from another machine meets is tested from the bench's own
   ///    private address (TestSetup.GetLocalIpAddress), which the "My computer" range
   ///    does not cover and the "Internet" range does.
   /// </summary>
   [TestFixture]
   // It stops the service the whole suite shares: nothing else may run meanwhile.
   // The suite runs one fixture at a time today (TestSetup.cs says why); this says
   // so for this fixture, whatever the assembly comes to declare.
   [NonParallelizable]
   public class NewInstallDefaults : TestFixtureBase
   {
      // NewInstallDefaults::CertificateName() and ::PendingName().
      private const string CertificateName = "Self-signed (made at installation)";
      private const string PendingName = "NewInstallDefaultsPending";
      private const string AccountPassword = "test";

      private const string DeleteMarkSql = "delete from hm_settings where settingname = '" + PendingName + "'";

      /// <summary>
      ///    The mark as NewInstallDefaults::MarkCreated writes it: settinginteger 1, and
      ///    the time it was made in Unix seconds, which the verb holds to a day.
      /// </summary>
      private static string InsertMarkSql(DateTimeOffset made)
      {
         return "insert into hm_settings (settingname, settingstring, settinginteger) values ('" + PendingName + "', '" +
                made.ToUnixTimeSeconds().ToString(System.Globalization.CultureInfo.InvariantCulture) + "', 1)";
      }

      private bool _applied;

      [TearDown]
      public void PutTheBenchBack()
      {
         if (!_applied)
            return;

         _applied = false;

         try
         {
            // A live COM object first, whatever the test left: a service it
            // stopped is started, and one that runs is asked again, since the
            // object the test held may be the dead proxy of a restart that failed.
            Guarded(() =>
            {
               if (ServiceIsRunning())
               {
                  SingletonProvider<TestSetup>.Instance.Authenticate();
                  _application = SingletonProvider<TestSetup>.Instance.GetApp();
                  _settings = _application.Settings;
               }
               else
                  RestartServerAndReacquireCom();
            });

            string dataDirectory = null;
            Guarded(() => dataDirectory = _settings.Directories.DataDirectory);

            // Every port deleted before SetDefault, which returns early - keeping a
            // certificate id - when the four are there without TLS.
            Guarded(() => DeleteEveryPort());
            Guarded(() => _settings.TCPIPPorts.SetDefault());
            Guarded(() => _settings.SecurityRanges.SetDefault());
            Guarded(() => _settings.SSLCertificates.Clear());
            Guarded(() => _application.Database.ExecuteSQL(DeleteMarkSql));

            if (dataDirectory != null)
            {
               string directory = Path.Combine(dataDirectory, "Certificates");
               foreach (string name in new[] { "self-signed-certificate.pem", "self-signed-key.pem" })
               {
                  string file = Path.Combine(directory, name);
                  Guarded(() =>
                  {
                     if (File.Exists(file))
                        File.Delete(file);
                  });
               }

               Guarded(() =>
               {
                  if (Directory.Exists(directory) && Directory.GetFileSystemEntries(directory).Length == 0)
                     Directory.Delete(directory);
               });
            }
         }
         finally
         {
            // The listeners the verb gave STARTTLS are the ones the running server
            // opened; a restart opens the plain ones SetDefault wrote. Whatever
            // failed above, this runs.
            Guarded(() => RestartServerAndReacquireCom());
         }
      }

      [Test]
      [Description("Refused while the service runs (exit 4, nothing changed); with it stopped, applied to the bench made into a database the create step has just made - its mark written, the stock listeners and ranges, no certificate, no account, no domain, no route: a self-signed certificate under the data directory, STARTTLS optional on 25, 587, 110 and 143 with it, port 25's SMTP authentication Honeypot and the other three listeners' On, the Internet range requiring TLS for authentication and My computer unchanged, and the mark gone - a mark two days old, which a run by hand acts on though the Linux unit's start would not. From a non-loopback address a password is refused before STARTTLS on SMTP, IMAP and POP3 and taken after it; from 127.0.0.1 it is taken without TLS, as before. Negative control: a second run, the mark gone, finds a database this installation did not just create and changes nothing (exit 5).")]
      public void ANewInstallationTakesAPasswordFromAnotherMachineOnlyOverTls()
      {
         if (TestSetup.ConsoleServer)
            Assert.Ignore("The server under test is a console server, which the verb's service check cannot see stopped.");

         string dataDirectory = _settings.Directories.DataDirectory;
         string domainName = _domain.Name;
         ServerExecutable();

         // Armed before anything below can change the bench: a verb that was not
         // refused would change it, and the put-back is safe on a bench unchanged.
         _applied = true;

         // The bench as the create step leaves a database. PerformBasicSetup put
         // back the ports, but SetDefault returns early when the four are there
         // without TLS, keeping a certificate id or an address an earlier fixture
         // left; so every port goes first. The bench's database was upgraded, not
         // created, so it has no mark of its own: the fixture writes the one
         // COM's create methods and --create-database write, and the teardown
         // removes what the verb did not.
         DeleteEveryPort();
         _settings.TCPIPPorts.SetDefault();
         _settings.SecurityRanges.SetDefault();
         _settings.SSLCertificates.Clear();

         // No domain, and so no account, and no route: none is in a create script,
         // and the verb leaves a database that has one as a server in service. The
         // fixture's domain goes, and is made again after the verb; PerformBasicSetup
         // removed the routes and makes the domain again for the next fixture.
         SingletonProvider<TestSetup>.Instance.ClearDomains();
         ClassicAssert.AreEqual(0, _application.Domains.Count, "A database the create step has just made has no domain.");
         ClassicAssert.AreEqual(0, _settings.Routes.Count, "A database the create step has just made has no route.");

         // The mark as the create step writes it, made two days ago: the Linux
         // unit's start (--if-marked) would leave such a database alone, but a run
         // by hand - the only kind Windows has, and the kind the product points to
         // wherever the unit does not run - has no such limit.
         _application.Database.ExecuteSQL(DeleteMarkSql);
         _application.Database.ExecuteSQL(InsertMarkSql(DateTimeOffset.UtcNow.AddDays(-2)));

         Verb running = RunVerb("--apply-new-install-defaults");
         ClassicAssert.AreEqual(4, running.ExitCode, running.Output);
         StringAssert.Contains("service is running", running.Output);
         ClassicAssert.AreEqual(0, _settings.SSLCertificates.Count, "A refused run made a certificate.");
         for (int i = 0; i < _settings.TCPIPPorts.Count; i++)
            ClassicAssert.AreEqual(eConnectionSecurity.eCSNone, _settings.TCPIPPorts[i].ConnectionSecurity, "A refused run changed a listener.");

         try
         {
            StopService();

            Verb applied = RunVerb("--apply-new-install-defaults");
            ClassicAssert.AreEqual(0, applied.ExitCode, applied.Output);
            StringAssert.Contains("Made a self-signed certificate", applied.Output);
            StringAssert.Contains("STARTTLS is offered on port 25, 110, 143, 587", applied.Output);
            StringAssert.Contains("Port 25 takes no password: its SMTP authentication is Honeypot", applied.Output);
            StringAssert.Contains("now takes a password only over TLS", applied.Output);

            // The mark went with the decision, so the same database is no longer
            // one this installation has just created.
            Verb again = RunVerb("--apply-new-install-defaults");
            ClassicAssert.AreEqual(5, again.ExitCode, again.Output);
            StringAssert.Contains("was not created by this installation", again.Output);
         }
         finally
         {
            RestartServerAndReacquireCom();
         }

         // What the verb wrote, as the server reads it back.
         ClassicAssert.AreEqual(1, _settings.SSLCertificates.Count, "The second run made another certificate.");
         SSLCertificate certificate = _settings.SSLCertificates[0];
         ClassicAssert.AreEqual(CertificateName, certificate.Name);
         ClassicAssert.IsTrue(certificate.CertificateFile.StartsWith(Path.Combine(dataDirectory.TrimEnd('\\'), "Certificates"), StringComparison.OrdinalIgnoreCase),
            certificate.CertificateFile + " is not under the data directory's Certificates folder, " + dataDirectory);
         ClassicAssert.IsTrue(File.Exists(certificate.CertificateFile), certificate.CertificateFile);
         ClassicAssert.IsTrue(File.Exists(certificate.PrivateKeyFile), certificate.PrivateKeyFile);

         ClassicAssert.AreEqual(4, _settings.TCPIPPorts.Count);
         for (int i = 0; i < _settings.TCPIPPorts.Count; i++)
         {
            TCPIPPort port = _settings.TCPIPPorts[i];
            ClassicAssert.AreEqual(eConnectionSecurity.eCSSTARTTLSOptional, port.ConnectionSecurity, "Port " + port.PortNumber);
            ClassicAssert.AreEqual(certificate.ID, port.SSLCertificateID, "Port " + port.PortNumber);

            // Port 25 a honeypot for AUTH, every other listener On (GitLab work item
            // #54). Through dynamic: the suite's interop comes from the registered
            // type library, which a bench older than the property does not have.
            int expected = port.Protocol == eSessionType.eSTSMTP && port.PortNumber == 25 ? 2 : 0;
            ClassicAssert.AreEqual(expected, (int) ((dynamic) port).SMTPAuthentication, "SMTP authentication of port " + port.PortNumber);
         }

         ClassicAssert.IsTrue(_settings.SecurityRanges.get_ItemByName("Internet").RequireSSLTLSForAuth, "The Internet range requires TLS for authentication.");
         ClassicAssert.IsFalse(_settings.SecurityRanges.get_ItemByName("My computer").RequireSSLTLSForAuth, "My computer is unchanged.");

         // The domain and an account now, after the verb, which applies to a
         // database with neither.
         Domain domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         ClassicAssert.AreEqual(domainName, domain.Name);
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "secure-defaults@" + domainName, AccountPassword);
         string address = account.Address;

         // Every read below fails rather than waits for ever on a server that went
         // silent, so that the teardown runs and puts the bench back.
         int receiveTimeoutBefore = TestPorts.ReceiveTimeoutMilliseconds;
         TestPorts.ReceiveTimeoutMilliseconds = 20000;
         try
         {
            CheckTheProtocols(address);
         }
         finally
         {
            TestPorts.ReceiveTimeoutMilliseconds = receiveTimeoutBefore;
         }
      }

      private static void CheckTheProtocols(string address)
      {
         IPAddress outside = TestSetup.GetLocalIpAddress();
         string user = Convert.ToBase64String(Encoding.ASCII.GetBytes(address));
         string password = Convert.ToBase64String(Encoding.ASCII.GetBytes(AccountPassword));

         // SMTP submission from another machine: no AUTH before STARTTLS, and one sent
         // anyway is refused before the credential crosses the network.
         using (var smtp = new TcpConnection())
         {
            ClassicAssert.IsTrue(smtp.Connect(outside, TestPorts.Submission), "Could not connect to " + outside + ":" + TestPorts.Submission);
            StringAssert.StartsWith("220", smtp.ReadUntil("\r\n"));

            smtp.Send("EHLO example.com\r\n");
            string plainEhlo = smtp.ReadUntil("\r\n250 ");
            StringAssert.Contains("STARTTLS", plainEhlo);
            StringAssert.DoesNotContain("AUTH", plainEhlo, "AUTH is not offered before STARTTLS to an address outside this computer.");

            smtp.Send("AUTH LOGIN\r\n");
            string refused = smtp.ReadUntil("\r\n");
            StringAssert.StartsWith("530", refused);
            StringAssert.Contains("SSL/TLS-connection is required for authentication", refused);

            smtp.Send("STARTTLS\r\n");
            StringAssert.StartsWith("220", smtp.ReadUntil("\r\n"));
            smtp.HandshakeAsClient();
            StringAssert.Contains("self-signed at installation", smtp.RemoteCertificate.Subject, "The listener presents the certificate the verb made.");

            smtp.Send("EHLO example.com\r\n");
            StringAssert.Contains("AUTH LOGIN", smtp.ReadUntil("\r\n250 "));
            smtp.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("334", smtp.ReadUntil("\r\n"));
            smtp.Send(user + "\r\n");
            StringAssert.StartsWith("334", smtp.ReadUntil("\r\n"));
            smtp.Send(password + "\r\n");
            StringAssert.StartsWith("235", smtp.ReadUntil("\r\n"), "After STARTTLS the password is taken.");
         }

         // IMAP from another machine.
         using (var imap = new TcpConnection())
         {
            ClassicAssert.IsTrue(imap.Connect(outside, TestPorts.Imap));
            imap.ReadUntil("\r\n");

            imap.Send("A01 LOGIN \"" + address + "\" \"" + AccountPassword + "\"\r\n");
            StringAssert.Contains("A01 BAD A SSL/TLS-connection is required for authentication.", ReadTagged(imap, "A01"));

            imap.Send("A02 STARTTLS\r\n");
            StringAssert.Contains("A02 OK", ReadTagged(imap, "A02"));
            imap.HandshakeAsClient();

            imap.Send("A03 LOGIN \"" + address + "\" \"" + AccountPassword + "\"\r\n");
            StringAssert.Contains("A03 OK", ReadTagged(imap, "A03"), "After STARTTLS the password is taken.");
         }

         // POP3 from another machine.
         using (var pop3 = new TcpConnection())
         {
            ClassicAssert.IsTrue(pop3.Connect(outside, TestPorts.Pop3));
            StringAssert.StartsWith("+OK", pop3.ReadUntil("\r\n"));

            pop3.Send("USER " + address + "\r\n");
            StringAssert.StartsWith("-ERR A SSL/TLS-connection is required for authentication.", pop3.ReadUntil("\r\n"));

            pop3.Send("STLS\r\n");
            StringAssert.StartsWith("+OK", pop3.ReadUntil("\r\n"));
            pop3.HandshakeAsClient();

            pop3.Send("USER " + address + "\r\n");
            StringAssert.StartsWith("+OK", pop3.ReadUntil("\r\n"));
            pop3.Send("PASS " + AccountPassword + "\r\n");
            StringAssert.StartsWith("+OK", pop3.ReadUntil("\r\n"), "After STLS the password is taken.");
         }

         // Negative control: this computer's own clients are served as they always
         // were - the "My computer" range was not touched - which is also why the
         // suite's thousands of plain sign-ins from 127.0.0.1 are unaffected.
         using (var local = new TcpConnection())
         {
            ClassicAssert.IsTrue(local.Connect(IPAddress.Loopback, TestPorts.Submission));
            local.ReadUntil("\r\n");
            local.Send("EHLO example.com\r\n");
            StringAssert.Contains("AUTH LOGIN", local.ReadUntil("\r\n250 "), "From 127.0.0.1, AUTH is offered without TLS.");
            local.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("334", local.ReadUntil("\r\n"));
            local.Send(user + "\r\n");
            StringAssert.StartsWith("334", local.ReadUntil("\r\n"));
            local.Send(password + "\r\n");
            StringAssert.StartsWith("235", local.ReadUntil("\r\n"), "From 127.0.0.1 the password is taken without TLS.");
         }
      }

      // ---- helpers, as API/RestApiProfiles.cs runs the profile verb ----------------

      /// <summary>
      ///    Every TCP/IP port deleted, each asked once: a delete the database refused
      ///    leaves the port in the collection, and a loop until none is left would
      ///    then never end.
      /// </summary>
      private void DeleteEveryPort()
      {
         var ports = _settings.TCPIPPorts;
         int count = ports.Count;
         for (int i = 0; i < count && ports.Count > 0; i++)
            ports.DeleteByDBID(ports[0].ID);

         if (ports.Count > 0)
            throw new InvalidOperationException(ports.Count + " TCP/IP port(s) could not be deleted.");
      }

      /// <summary>What the server sent up to and including the line tagged with tag.</summary>
      private static string ReadTagged(TcpConnection connection, string tag)
      {
         string text = "";
         DateTime deadline = DateTime.UtcNow.AddSeconds(10);
         while (DateTime.UtcNow < deadline)
         {
            text += connection.Receive();
            int at = text.IndexOf(tag + " ", StringComparison.Ordinal);
            if (at >= 0 && text.IndexOf("\r\n", at, StringComparison.Ordinal) > at)
               return text;
         }

         Assert.Fail("No answer tagged " + tag + ": " + text);
         return text;
      }

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      /// <summary>
      ///    hMailServer.exe with these arguments, with none of the runner's HM_
      ///    variables in its environment - an HM_SETTING_ left there would reach the
      ///    bench's database through the verb's start. One that has not finished in
      ///    three minutes is stopped.
      /// </summary>
      private Verb RunVerb(string arguments)
      {
         var start = new ProcessStartInfo(ServerExecutable(), arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(180 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(arguments + " did not finish within three minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      private string executable_;

      // Asked of the server once, over COM, and kept: a verb run with the service
      // stopped has no server to ask.
      private string ServerExecutable()
      {
         if (executable_ != null)
            return executable_;

         string program = _settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return executable_ = candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      /// <summary>The service stopped, and its process gone, as TestSetup's restart stops it.</summary>
      private static void StopService()
      {
         using (Process process = Process.Start(new ProcessStartInfo("net", "stop hMailServer")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         }))
         {
            process.StandardOutput.ReadToEnd();
            process.StandardError.ReadToEnd();
            process.WaitForExit(90000);
         }

         DateTime deadline = DateTime.UtcNow.AddSeconds(60);
         while (DateTime.UtcNow < deadline && Process.GetProcessesByName("hmailserver").Length > 0)
            Thread.Sleep(250);

         ClassicAssert.IsFalse(ServiceRunning(), "The hMailServer service did not stop.");
      }

      private static bool ServiceRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status != System.ServiceProcess.ServiceControllerStatus.Stopped;
         }
      }

      /// <summary>Running, and not on its way to or from it: what the teardown needs before it asks COM.</summary>
      private static bool ServiceIsRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status == System.ServiceProcess.ServiceControllerStatus.Running;
         }
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("NewInstallDefaults teardown: " + e.Message);
         }
      }
   }
}
