// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    TLS to the database, [Database] TlsMode and TlsCaFile (docs/DatabaseTls.md),
   ///    against the real servers of the test lab on linix: MySQL 8.4, MariaDB 11.4,
   ///    PostgreSQL 17 and SQL Server 2022 (docs/TestLab.md). The bench's own database
   ///    is SQL Server Compact and is never touched: each test writes a configuration
   ///    file of its own naming one lab database, and runs the server's own
   ///    `hMailServer.exe --check-database --config &lt;file&gt;` beside the service - one
   ///    connection through the same DALConnection code the service opens its pool
   ///    with, and a line saying how it is protected - and reads what it printed.
   ///
   ///    The fixture is told where the lab is, and skips, saying which is missing,
   ///    without it:
   ///
   ///       HMAILSERVER_DBTLS_LAB        the lab host, 192.168.11.154
   ///       HMAILSERVER_DBTLS_PASSWORD   the password of the lab's application user,
   ///                                    HMLAB_DB_PASSWORD in ~/hmlab/secrets.env on linix
   ///
   ///    and, with the lab's defaults, HMAILSERVER_DBTLS_USER (hmlab) and
   ///    HMAILSERVER_DBTLS_MYSQL_PORT (10310), _MARIADB_PORT (10311),
   ///    _POSTGRESQL_PORT (10312) and _MSSQL_PORT (10313). A database that does not
   ///    answer is skipped with the command that starts it (bash build/lab/lab.sh up
   ///    mysql, and so on). Each test connects to a database every account may open
   ///    - information_schema, postgres, master - so nothing is created.
   ///
   ///    What is proved, per backend: the default encrypts when the server offers TLS
   ///    and connects without it when the server does not; disable does not encrypt
   ///    (the negative control for the default); require encrypts or refuses; and
   ///    verify-full refuses the lab servers' own certificates when TlsCaFile names a
   ///    CA that did not issue them - a CA this fixture makes for the purpose. A
   ///    verify-full that SUCCEEDS needs a server certificate from a CA the fixture
   ///    knows, which the lab's databases do not have; the row's lab run proves that
   ///    half on linix with throwaway servers (the commit message has it).
   /// </summary>
   [TestFixture]
   public class DatabaseTlsLab : TestFixtureBase
   {
      private string labHost_;
      private string password_;
      private string user_;
      private string workDirectory_;
      private string unknownCaFile_;

      [OneTimeSetUp]
      public void ReachTheLab()
      {
         labHost_ = Environment.GetEnvironmentVariable("HMAILSERVER_DBTLS_LAB");
         password_ = Environment.GetEnvironmentVariable("HMAILSERVER_DBTLS_PASSWORD");

         if (string.IsNullOrWhiteSpace(labHost_))
            Assert.Ignore("HMAILSERVER_DBTLS_LAB is not set: it names the host the test lab's databases run on (docs/TestLab.md).");
         if (string.IsNullOrEmpty(password_))
            Assert.Ignore("HMAILSERVER_DBTLS_PASSWORD is not set: it is the lab's application user's password, HMLAB_DB_PASSWORD in ~/hmlab/secrets.env on the lab host.");

         labHost_ = labHost_.Trim();
         user_ = Text("HMAILSERVER_DBTLS_USER", "hmlab");

         workDirectory_ = Path.Combine(Path.GetTempPath(), "hm-dbtls-" + Guid.NewGuid().ToString("N").Substring(0, 12));
         Directory.CreateDirectory(workDirectory_);
         Directory.CreateDirectory(Path.Combine(workDirectory_, "Logs"));

         unknownCaFile_ = Path.Combine(workDirectory_, "unknown-ca.pem");
         // settings-store-exempt: a PEM file of a CA's certificate, not the server's configuration.
         File.WriteAllText(unknownCaFile_, UnknownAuthorityPem(), new UTF8Encoding(false));
      }

      [OneTimeTearDown]
      public void RemoveTheFiles()
      {
         if (!string.IsNullOrEmpty(workDirectory_) && Directory.Exists(workDirectory_))
         {
            try
            {
               Directory.Delete(workDirectory_, true);
            }
            catch (IOException)
            {
               // A log file the child still held; the temporary directory keeps it.
            }
         }
      }

      // ---- MySQL and MariaDB ---------------------------------------------------------

      [Test]
      [Description("MySQL 8.4 and MariaDB 11.4 offer TLS, and with TlsMode not set the connection takes it - the gap the lab found was that both clients connected in plaintext when asked nothing.")]
      public void TheDefaultEncryptsToMySqlAndMariaDb()
      {
         foreach (var server in MySqlFamily())
         {
            Check check = Run(server, null, null);

            Assert.AreEqual(0, check.ExitCode, server.Name + ": " + check.Output);
            StringAssert.Contains("Connected. MySQL/MariaDB: encrypted with TLS", check.Output, server.Name);
            StringAssert.Contains("the server's certificate not checked (TlsMode prefer, the default)", check.Output, server.Name);
         }
      }

      [Test]
      [Description("The negative control for the default: TlsMode=disable connects to the same servers without TLS.")]
      public void DisableConnectsWithoutTls()
      {
         foreach (var server in MySqlFamily())
         {
            Check check = Run(server, "disable", null);

            Assert.AreEqual(0, check.ExitCode, server.Name + ": " + check.Output);
            StringAssert.Contains("Connected. MySQL/MariaDB: NOT encrypted: TLS is switched off (TlsMode disable)", check.Output, server.Name);
         }
      }

      [Test]
      [Description("TlsMode=require encrypts, without checking the servers' own certificates.")]
      public void RequireEncryptsToMySqlAndMariaDb()
      {
         foreach (var server in MySqlFamily())
         {
            Check check = Run(server, "require", null);

            Assert.AreEqual(0, check.ExitCode, server.Name + ": " + check.Output);
            StringAssert.Contains("Connected. MySQL/MariaDB: encrypted with TLS", check.Output, server.Name);
            StringAssert.Contains("(TlsMode require)", check.Output, server.Name);
         }
      }

      [Test]
      [Description("TlsMode=verify-full with a TlsCaFile that did not issue the server's certificate is refused before anything crosses the connection, and says which setting to change.")]
      public void VerifyFullRefusesACertificateFromAnUnknownCa()
      {
         foreach (var server in MySqlFamily())
         {
            Check check = Run(server, "verify-full", unknownCaFile_);

            Assert.AreEqual(1, check.ExitCode, server.Name + ": " + check.Output);
            StringAssert.DoesNotContain("Connected. ", check.Output, server.Name);

            // What follows "Not connected: " - the client's own reason and the
            // guidance - and not the "TLS: TlsMode verify-full" line before it.
            int refusedAt = check.Output.IndexOf("Not connected: ", StringComparison.Ordinal);
            ClassicAssert.GreaterOrEqual(refusedAt, 0, server.Name + ": " + check.Output);
            string refusal = check.Output.Substring(refusedAt);
            StringAssert.Contains("TLS/SSL error", refusal, server.Name + ": the client's own reason (CR_SSL_CONNECTION_ERROR)");
            StringAssert.Contains("TlsMode=verify-full under [Database]", refusal, server.Name + ": and the guidance names the setting");
         }
      }

      [Test]
      [Description("A TlsMode this server does not know is refused without an attempt - verify-ca included, which two of the three clients cannot honour.")]
      public void AnUnknownModeIsRefusedWithoutAnAttempt()
      {
         var server = MySqlFamily()[0];
         Check check = Run(server, "verify-ca", null);

         Assert.AreEqual(1, check.ExitCode, check.Output);
         StringAssert.Contains("TlsMode under [Database] in hMailServer.ini is 'verify-ca'", check.Output);
         StringAssert.Contains("The connection was not attempted.", check.Output);
      }

      // ---- PostgreSQL ----------------------------------------------------------------

      [Test]
      [Description("PostgreSQL with TlsMode not set connects whether the server offers TLS or not (libpq's prefer), and require then agrees with what the default found: encrypted if the server offers TLS, refused if it does not.")]
      public void PostgreSqlDefaultAndRequireAgree()
      {
         var server = Server("PostgreSQL", "POSTGRESQL", 10312, "PostgreSQL", "postgres");

         Check byDefault = Run(server, null, null);
         Assert.AreEqual(0, byDefault.ExitCode, byDefault.Output);
         StringAssert.Contains("Connected. PostgreSQL: ", byDefault.Output);
         StringAssert.Contains("TLS: libpq's own default", byDefault.Output);

         bool offersTls = byDefault.Output.Contains("PostgreSQL: encrypted with TLS");

         Check required = Run(server, "require", null);
         if (offersTls)
         {
            Assert.AreEqual(0, required.ExitCode, required.Output);
            StringAssert.Contains("Connected. PostgreSQL: encrypted with TLS", required.Output);
         }
         else
         {
            StringAssert.Contains("NOT encrypted: the server offered no TLS", byDefault.Output);
            Assert.AreEqual(1, required.ExitCode, required.Output);
            StringAssert.Contains("Not connected: ", required.Output);
            StringAssert.Contains("SSL", required.Output, "libpq's own reason: the server does not support SSL.");
         }

         Check disabled = Run(server, "disable", null);
         Assert.AreEqual(0, disabled.ExitCode, disabled.Output);
         StringAssert.Contains("Connected. PostgreSQL: NOT encrypted: TLS is switched off (TlsMode disable)", disabled.Output);
      }

      // ---- SQL Server ----------------------------------------------------------------

      [Test]
      [Description("SQL Server: the default encrypts through a provider that can do so without checking the certificate (MSOLEDBSQL), and says why not through SQLOLEDB; verify-full refuses the server's own certificate, which no root in the Windows store issued.")]
      public void SqlServerDefaultAndVerifyFull()
      {
         var server = Server("SQL Server", "MSSQL", 10313, "MSSQL", "master");

         Check byDefault = Run(server, null, null);
         Assert.AreEqual(0, byDefault.ExitCode, byDefault.Output);
         StringAssert.Contains("Connected. SQL Server: ", byDefault.Output);

         bool legacyProvider = byDefault.Output.Contains("SQLOLEDB encrypts only the login");
         if (legacyProvider)
            StringAssert.Contains("SQL Server: NOT encrypted", byDefault.Output);
         else
            StringAssert.Contains("SQL Server: encrypted with TLS (provider ", byDefault.Output);

         Check verified = Run(server, "verify-full", null);
         Assert.AreEqual(1, verified.ExitCode, verified.Output);
         StringAssert.Contains("Not connected: ", verified.Output);
         StringAssert.Contains("Trusted Root Certification Authorities", verified.Output, "The guidance says where the provider looks.");

         Check disabled = Run(server, "disable", null);
         Assert.AreEqual(0, disabled.ExitCode, disabled.Output);
         StringAssert.Contains("SQL Server: NOT encrypted: TLS is switched off", disabled.Output);
      }

      // ---- plumbing ------------------------------------------------------------------

      private sealed class LabServer
      {
         public string Name;
         public string Type;
         public string Host;
         public int Port;
         public string Database;
      }

      private sealed class Check
      {
         public int ExitCode;
         public string Output;
      }

      private List<LabServer> MySqlFamily()
      {
         return new List<LabServer>
         {
            Server("MySQL 8.4", "MYSQL", 10310, "MySQL", "information_schema"),
            Server("MariaDB 11.4", "MARIADB", 10311, "MySQL", "information_schema"),
         };
      }

      // One lab database, or the test skipped with the command that starts it.
      private LabServer Server(string name, string variable, int defaultPort, string type, string database)
      {
         int port = Number("HMAILSERVER_DBTLS_" + variable + "_PORT", defaultPort);

         if (!Answers(labHost_, port))
         {
            Assert.Ignore(name + " does not answer at " + labHost_ + ":" + port + " - on the lab host: bash build/lab/lab.sh up " +
                          (variable == "MSSQL" ? "mssql" : variable.ToLowerInvariant()));
         }

         return new LabServer { Name = name, Type = type, Host = labHost_, Port = port, Database = database };
      }

      // hMailServer.exe --check-database --config <a file of this test's own>.
      private Check Run(LabServer server, string tlsMode, string tlsCaFile)
      {
         string configuration = Path.Combine(workDirectory_, "check-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".ini");

         var text = new StringBuilder();
         text.Append("[Directories]\r\n");
         text.Append("ProgramFolder=" + _settings.Directories.ProgramDirectory + "\r\n");
         text.Append("DataFolder=" + workDirectory_ + "\r\n");
         text.Append("TempFolder=" + workDirectory_ + "\r\n");
         text.Append("LogFolder=" + Path.Combine(workDirectory_, "Logs") + "\r\n");
         text.Append("[Database]\r\n");
         text.Append("Type=" + server.Type + "\r\n");
         // SQL Server's provider takes the port after a comma in the server's name.
         text.Append("Server=" + (server.Type == "MSSQL" ? server.Host + "," + server.Port : server.Host) + "\r\n");
         text.Append("Port=" + (server.Type == "MSSQL" ? 0 : server.Port) + "\r\n");
         text.Append("Database=" + server.Database + "\r\n");
         text.Append("Username=" + user_ + "\r\n");
         text.Append("Password=" + password_ + "\r\n");
         text.Append("Passwordencryption=0\r\n");
         if (tlsMode != null)
            text.Append("TlsMode=" + tlsMode + "\r\n");
         if (tlsCaFile != null)
            text.Append("TlsCaFile=" + tlsCaFile + "\r\n");

         // settings-store-exempt: a configuration file of the test's own for a child process's --check-database, never the service's hMailServer.ini.
         File.WriteAllText(configuration, text.ToString(), Encoding.ASCII);

         try
         {
            var start = new ProcessStartInfo(ServerExecutable(), "--check-database --config \"" + configuration + "\"")
            {
               UseShellExecute = false,
               CreateNoWindow = true,
               RedirectStandardOutput = true,
               RedirectStandardError = true,
               StandardOutputEncoding = Encoding.UTF8,
               StandardErrorEncoding = Encoding.UTF8,
            };

            using (Process process = Process.Start(start))
            {
               Task<string> output = process.StandardOutput.ReadToEndAsync();
               Task<string> error = process.StandardError.ReadToEndAsync();
               if (!process.WaitForExit(90 * 1000))
               {
                  try
                  {
                     process.Kill();
                  }
                  catch (InvalidOperationException)
                  {
                     // It finished between the wait and the kill.
                  }

                  Assert.Fail("--check-database against " + server.Name + " did not finish within 90 seconds, and was stopped.");
               }
               process.WaitForExit();

               var check = new Check { ExitCode = process.ExitCode, Output = output.Result + error.Result };
               TestContext.Out.WriteLine(server.Name + ", TlsMode " + (tlsMode ?? "(not set)") + ", exit " + check.ExitCode + ":\r\n" + check.Output);
               return check;
            }
         }
         finally
         {
            // The file holds the lab's password.
            File.Delete(configuration);
         }
      }

      private string ServerExecutable()
      {
         string program = _settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so --check-database cannot be run here.");
         return null;
      }

      private static bool Answers(string host, int port)
      {
         try
         {
            using (var client = new TcpClient())
            {
               Task connect = client.ConnectAsync(host, port);
               return connect.Wait(3000) && client.Connected;
            }
         }
         catch (Exception)
         {
            return false;
         }
      }

      // A CA made for this fixture and nothing else: it issued no certificate any lab
      // database presents, which is what verify-full has to notice.
      private static string UnknownAuthorityPem()
      {
         using (var key = RSA.Create(2048))
         {
            var request = new CertificateRequest("CN=hMailServer regression unknown database CA", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
            request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign, true));

            using (var authority = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(30)))
            {
               return "-----BEGIN CERTIFICATE-----\n" +
                      Convert.ToBase64String(authority.Export(X509ContentType.Cert), Base64FormattingOptions.InsertLineBreaks).Replace("\r\n", "\n") +
                      "\n-----END CERTIFICATE-----\n";
            }
         }
      }

      private static string Text(string variable, string fallback)
      {
         string value = Environment.GetEnvironmentVariable(variable);
         return string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
      }

      private static int Number(string variable, int fallback)
      {
         return int.TryParse(Environment.GetEnvironmentVariable(variable), out int value) ? value : fallback;
      }
   }
}
