// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Discovery page reads GET /api/v1/discovery/cases, which
   /// RestApiDiscovery.cpp's CaseJson writes, and picks a case's rows out of GET
   /// /api/v1/audit. These pin both: a key renamed on the server fails here rather
   /// than drawing an empty column, a case's rows are its own and no other case's,
   /// and the listener's address is found by the rules RestApiServer::Start keeps.
   /// </summary>
   public class DiscoveryReportTests
   {
      // Byte-for-byte the shape CaseJson writes for the listing (whole = false).
      private const string Cases =
         "{\"cases\":[" +
         "{\"id\":31,\"name\":\"Northwind v. Example\",\"description\":\"The supplier dispute\",\"state\":\"open\"," +
         "\"created_by\":\"Administrator\",\"created\":1758060000,\"closed_by\":\"\",\"closed\":0," +
         "\"search_count\":2,\"live_holds\":1,\"export_count\":1}," +
         "{\"id\":3,\"name\":\"Northwind v. Example\",\"description\":\"\",\"state\":\"closed\"," +
         "\"created_by\":\"audra@example.com\",\"created\":1757000000,\"closed_by\":\"Administrator\",\"closed\":1757100000," +
         "\"search_count\":0,\"live_holds\":0,\"export_count\":0}" +
         "]}";

      [Fact]
      public void Parse_ReadsEveryFieldOfEveryCase()
      {
         DiscoveryReport report = DiscoveryReport.Parse(Cases);

         Assert.Null(report.Error);
         Assert.Equal(2, report.Cases.Count);

         DiscoveryCase first = report.Cases[0];
         Assert.Equal(31, first.Id);
         Assert.Equal("Northwind v. Example", first.Name);
         Assert.Equal("The supplier dispute", first.Description);
         Assert.Equal("open", first.State);
         Assert.Equal("Administrator", first.CreatedBy);
         Assert.Equal(1758060000, first.Created);
         Assert.Equal(2, first.SearchCount);
         Assert.Equal(1, first.LiveHolds);
         Assert.Equal(1, first.ExportCount);

         DiscoveryCase second = report.Cases[1];
         Assert.Equal("closed", second.State);
         Assert.Equal("Administrator", second.ClosedBy);
         Assert.Equal(1757100000, second.Closed);
      }

      [Fact]
      public void Parse_OfWhatIsNotTheDocument_IsAnErrorAndNoCases()
      {
         DiscoveryReport broken = DiscoveryReport.Parse("{\"cases\":[");
         Assert.NotNull(broken.Error);
         Assert.Empty(broken.Cases);

         Assert.Empty(DiscoveryReport.Parse("{\"error\":\"refused\"}").Cases);
      }

      [Fact]
      public void LinesOf_TakesTheCasesRowsAndTheOneThatOpenedIt_AndNoOtherCases()
      {
         DiscoveryReport report = DiscoveryReport.Parse(Cases);
         DiscoveryCase open = report.Cases[0];
         DiscoveryCase older = report.Cases[1];

         var rows = new List<AuditRow>
         {
            // The opening row is named by the name alone, and the two cases share it.
            new() { Id = 1, Time = 1757000001, ObjectType = "discovery case", ObjectName = "Northwind v. Example", Action = "created" },
            new() { Id = 2, Time = 1758060001, ObjectType = "discovery case", ObjectName = "Northwind v. Example", Action = "created" },
            new() { Id = 3, Time = 1758060100, ObjectType = "discovery search", ObjectName = "case 31: Northwind v. Example", Action = "created", Detail = "query={...}" },
            new() { Id = 4, Time = 1757050000, ObjectType = "discovery search", ObjectName = "case 3: Northwind v. Example", Action = "created" },
            new() { Id = 5, Time = 1758060300, ObjectType = "discovery export", ObjectName = "case 31: Northwind v. Examp", Action = "downloaded" },
            new() { Id = 6, Time = 1758060200, ObjectType = "discovery hold", ObjectName = "case 31: Northwind v. Example", Action = "created" }
         };

         List<AuditRow> lines = DiscoveryReport.LinesOf(open, rows);
         Assert.Equal(new long[] { 5, 6, 3, 2 }, lines.Select(r => r.Id).ToArray());

         List<AuditRow> olderLines = DiscoveryReport.LinesOf(older, rows);
         Assert.Equal(new long[] { 4, 1 }, olderLines.Select(r => r.Id).ToArray());

         Assert.Empty(DiscoveryReport.LinesOf(null, rows));
      }

      [Fact]
      public void ErrorOf_ReadsTheServersSentence()
      {
         Assert.Equal("There is no such case.", DiscoveryReport.ErrorOf("{\"error\":\"There is no such case.\"}"));
         Assert.Equal("", DiscoveryReport.ErrorOf("not json"));
         Assert.Equal("", DiscoveryReport.ErrorOf("[]"));
      }

      [Fact]
      public void LinesOf_FindsTheOpeningRowOfACaseWhoseNameTheTrailCutShort()
      {
         // The trail keeps 120 characters of an object's name and marks the cut,
         // and a case's name may be 255: its opening row is named by the cut name.
         string longName = new string('N', 200);
         var theCase = new DiscoveryCase { Id = 7, Name = longName, Created = 1758060000 };
         string stored = longName.Substring(0, 120) + "...";
         Assert.Equal(stored, DiscoveryReport.AuditObjectName(longName));
         Assert.Equal("Short", DiscoveryReport.AuditObjectName("Short"));

         var rows = new List<AuditRow>
         {
            new() { Id = 1, Time = 1758060001, ObjectType = "discovery case", ObjectName = stored, Action = "created" },
            new() { Id = 2, Time = 1758060001, ObjectType = "discovery case", ObjectName = longName, Action = "created" }
         };

         Assert.Equal(new long[] { 1 }, DiscoveryReport.LinesOf(theCase, rows).Select(r => r.Id).ToArray());
      }

      [Fact]
      public void Endpoint_FollowsTheListenersOwnRules()
      {
         // Off.
         Assert.Null(DiscoveryReport.Endpoint("0", "127.0.0.1", "", "", false, true, "localhost", out string off));
         Assert.Contains("RestApiPort", off);
         Assert.Null(DiscoveryReport.Endpoint("", "", "", "", false, true, "localhost", out _));

         // Plain HTTP on a loopback address, from this machine, with no ACME certificate.
         Uri local = DiscoveryReport.Endpoint("8045", "", "", "", false, true, "localhost", out string none);
         Assert.Null(none);
         Assert.Equal("http://127.0.0.1:8045/", local.ToString());
         Assert.Equal("http://[::1]:8045/", DiscoveryReport.Endpoint("8045", "::1", "", "", false, true, "localhost", out _).ToString());

         // Plain HTTP anywhere else is a listener the server does not start.
         Assert.Null(DiscoveryReport.Endpoint("8045", "0.0.0.0", "", "", false, true, "localhost", out string notRunning));
         Assert.Contains("RestApiCertificateFile", notRunning);
         Assert.Contains("ACME", notRunning);

         // A loopback listener, from another machine.
         Assert.Null(DiscoveryReport.Endpoint("8045", "127.0.0.1", "", "", null, false, "mail.example.com", out string far));
         Assert.Contains("127.0.0.1", far);

         // HTTPS: the host connected to from another machine, "localhost" when
         // bound to every address, and the bind address otherwise.
         Assert.Equal("https://mail.example.com:8045/",
            DiscoveryReport.Endpoint("8045", "0.0.0.0", "cert.pem", "key.pem", null, false, "mail.example.com", out _).ToString());
         Assert.Equal("https://localhost:8045/",
            DiscoveryReport.Endpoint("8045", "0.0.0.0", "cert.pem", "key.pem", false, true, "localhost", out _).ToString());
         Assert.Equal("https://192.0.2.10:8045/",
            DiscoveryReport.Endpoint("8045", "192.0.2.10", "cert.pem", "key.pem", false, true, "localhost", out _).ToString());
      }

      [Fact]
      public void Endpoint_FollowsTheListenersFallBackToTheAcmeCertificate()
      {
         // No certificate of the listener's own and an issued ACME pair: HTTPS,
         // on every address and on a loopback one alike.
         Assert.Equal("https://localhost:8045/",
            DiscoveryReport.Endpoint("8045", "0.0.0.0", "", "", true, true, "localhost", out string everywhere).ToString());
         Assert.Null(everywhere);
         Assert.Equal("https://127.0.0.1:8045/",
            DiscoveryReport.Endpoint("8045", "127.0.0.1", "", "", true, true, "localhost", out _).ToString());

         // A key without a certificate is still the ACME pair's to replace.
         Assert.Equal("https://localhost:8045/",
            DiscoveryReport.Endpoint("8045", "0.0.0.0", "", "key.pem", true, true, "localhost", out _).ToString());

         // A certificate without a key is not replaced, and serves nothing beyond loopback.
         Assert.Null(DiscoveryReport.Endpoint("8045", "0.0.0.0", "cert.pem", "", true, true, "localhost", out string half));
         Assert.Contains("RestApiCertificateFile", half);

         // From another machine the ACME folder cannot be looked in: a listener
         // bound beyond loopback with no certificate of its own can only be
         // serving on the ACME one, so HTTPS is asked of the host connected to.
         Assert.Equal("https://mail.example.com:8045/",
            DiscoveryReport.Endpoint("8045", "0.0.0.0", "", "", null, false, "mail.example.com", out string unknown).ToString());
         Assert.Null(unknown);

         // Where the server looks: the configured folder, else the data folder's ACME.
         Assert.Equal(@"D:\Certs", DiscoveryReport.AcmeCertificateFolder(@" D:\Certs ", @"C:\Data"));
         Assert.Equal(System.IO.Path.Combine(@"C:\Data", "ACME"), DiscoveryReport.AcmeCertificateFolder("", @"C:\Data"));
         Assert.Null(DiscoveryReport.AcmeCertificateFolder("", ""));

         // Both halves, or it is not the pair.
         var present = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
         {
            System.IO.Path.Combine(@"C:\Data\ACME", "fullchain.pem"),
            System.IO.Path.Combine(@"C:\Data\ACME", "privkey.pem")
         };
         Assert.True(DiscoveryReport.AcmeCertificateIn(@"C:\Data\ACME", present.Contains));
         present.Remove(System.IO.Path.Combine(@"C:\Data\ACME", "privkey.pem"));
         Assert.False(DiscoveryReport.AcmeCertificateIn(@"C:\Data\ACME", present.Contains));
         Assert.False(DiscoveryReport.AcmeCertificateIn(null, present.Contains));
      }
   }
}
