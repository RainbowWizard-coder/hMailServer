// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The reports our own domains receive, read (DomainReports.h): a domain names
   ///    a report mailbox, a DMARC aggregate report or a TLS-RPT report delivered to
   ///    that address over the suite's SMTP is parsed and recorded, and the summary,
   ///    the listing, one report's records and the TLS rows come back over REST -
   ///    the documents the Deck's DMARC page and hmctl dmarc draw.
   ///
   ///    What is held beside the round trips is what must NOT be read, each beside
   ///    a genuine report that is: a report to an ordinary mailbox, one about a
   ///    domain this server does not host, one about a hosted domain that names no
   ///    mailbox, one that is not a report at all, and the same report a second
   ///    time. Every message, read or not, is delivered to the mailbox unchanged.
   ///    The mailbox itself is refused unless it is an address this server hosts,
   ///    over REST and over COM; a key confined to one domain reads that domain's
   ///    reports and gets 403 on another's and on the sweep.
   /// </summary>
   [TestFixture]
   public class RestApiDomainReports : TestFixtureBase
   {
      private static int RestPort = 9626;

      private const string Password = "test";
      private const string Reporter = "noreply-dmarc-support@provider.test";
      private const string ExpirePath = "/api/v1/domain-reports/expire";

      private static int sequence_;

      private Account mailbox_;

      private string Mailbox
      {
         get { return mailbox_.Address; }
      }

      private string DomainPath
      {
         get { return "/api/v1/domains/" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApiAndNameAMailbox()
      {
         _settings.SetAdministratorPassword(BounceTestSupport.AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         mailbox_ = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dmarc@" + _domain.Name, Password);

         (int status, string body) named = Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"" + Mailbox + "\"}");
         Assert.AreEqual(200, named.status, named.body);
      }

      [TearDown]
      public void RestoreAndStopRestApi()
      {
         try
         {
            // Nothing left behind for a later fixture: no mailbox named, no rows
            // for any domain, and the retention back at what the server ships.
            Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"\"}");
            Http("POST", ExpirePath, "{\"older_than_days\":0}");
            Http("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":365}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A gzip aggregate report delivered to the report mailbox is recorded with every field the row asks for, the summary adds its records up as the chart draws them, the listing and the report by id answer it, and the message is delivered unchanged.")]
      public void AGzipReportToTheMailboxIsRecordedAndCharted()
      {
         string reportId = NewReportId();
         long end = Now() - 1;
         long begin = end - 86399;

         string delivered = Inject(GzipMessage(Mailbox, AggregateReportXml(_domain.Name, reportId, begin, end), "provider.test!" + _domain.Name + "!" + begin + "!" + end + ".xml.gz"));
         StringAssert.Contains("Content-Type: application/gzip", delivered, "The report was not delivered to the mailbox unchanged.");

         string summary = Read(DomainPath + "/dmarc-reports/summary?days=30");
         Assert.AreEqual(_domain.Name, BounceTestSupport.Text(summary, "domain"), summary);
         Assert.IsTrue(BounceTestSupport.Flag(summary, "mailbox_configured"), summary);
         Assert.AreEqual(Mailbox, BounceTestSupport.Text(summary, "report_mailbox"), summary);
         Assert.AreEqual(1, BounceTestSupport.Number(summary, "reports"), summary);
         Assert.AreEqual(15, BounceTestSupport.Number(summary, "messages"), summary);
         Assert.AreEqual(12, BounceTestSupport.Number(summary, "passed"), summary);
         Assert.AreEqual(3, BounceTestSupport.Number(summary, "failed"), summary);
         Assert.AreEqual(12, BounceTestSupport.Number(summary, "dkim_passed"), summary);
         Assert.AreEqual(12, BounceTestSupport.Number(summary, "spf_passed"), summary);
         Assert.AreEqual(3, BounceTestSupport.Number(summary, "quarantined"), summary);
         Assert.AreEqual(0, BounceTestSupport.Number(summary, "rejected"), summary);
         Assert.AreEqual(2, BounceTestSupport.Number(summary, "sources_total"), summary);

         string source = ItemWith(summary, "ip", "192.0.2.10");
         Assert.AreEqual(12, BounceTestSupport.Number(source, "messages"), source);
         Assert.AreEqual(12, BounceTestSupport.Number(source, "passed"), source);
         Assert.AreEqual(_domain.Name.ToLowerInvariant(), BounceTestSupport.Text(source, "header_from"), source);
         Assert.AreEqual(1, BounceTestSupport.Number(source, "reporters"), source);

         string forger = ItemWith(summary, "ip", "198.51.100.7");
         Assert.AreEqual(3, BounceTestSupport.Number(forger, "failed"), forger);
         Assert.AreEqual(3, BounceTestSupport.Number(forger, "quarantined"), forger);

         string reporter = ItemWith(summary, "org", "provider.test");
         Assert.AreEqual(1, BounceTestSupport.Number(reporter, "reports"), reporter);
         Assert.AreEqual(15, BounceTestSupport.Number(reporter, "messages"), reporter);

         // The day the report begins is a bar of the window.
         StringAssert.Contains("\"day\":\"" + DayOf(begin) + "\",\"messages\":15", summary, "The report's records were not charted on the day it begins.");

         string list = Read(DomainPath + "/dmarc-reports?limit=10&offset=0");
         Assert.AreEqual(1, BounceTestSupport.Number(list, "total"), list);
         string item = ItemWith(list, "report_id", reportId);
         Assert.AreEqual("provider.test", BounceTestSupport.Text(item, "org"), item);
         Assert.AreEqual(Reporter, BounceTestSupport.Text(item, "email"), item);
         Assert.AreEqual(begin, BounceTestSupport.Number(item, "begin"), item);
         Assert.AreEqual(end, BounceTestSupport.Number(item, "end"), item);
         Assert.AreEqual(_domain.Name.ToLowerInvariant(), BounceTestSupport.Text(item, "policy_domain"), item);
         Assert.AreEqual("quarantine", BounceTestSupport.Text(item, "p"), item);
         Assert.AreEqual("reject", BounceTestSupport.Text(item, "sp"), item);
         Assert.AreEqual("r", BounceTestSupport.Text(item, "adkim"), item);
         Assert.AreEqual(100, BounceTestSupport.Number(item, "pct"), item);
         Assert.AreEqual(15, BounceTestSupport.Number(item, "messages"), item);
         Assert.AreEqual(12, BounceTestSupport.Number(item, "passed"), item);
         Assert.AreEqual(2, BounceTestSupport.Number(item, "records"), item);
         StringAssert.Contains(".xml.gz", BounceTestSupport.Text(item, "source"), item);

         // What the inbound path made of the carrying message is on the item -
         // empty here, since the suite's server writes no Authentication-Results
         // field - and the reporter's entry counts the reports it was checked on.
         StringAssert.Contains("\"message_dmarc\":\"", item, item);
         StringAssert.Contains("\"message_from\":\"", item, item);
         StringAssert.Contains("\"checked\":", reporter, reporter);
         StringAssert.Contains("\"authenticated\":", reporter, reporter);

         long id = BounceTestSupport.Number(item, "id");
         string report = Read(DomainPath + "/dmarc-reports/" + id);
         Assert.AreEqual(reportId, BounceTestSupport.Text(report, "report_id"), report);

         string first = ItemWith(report, "source_ip", "192.0.2.10");
         Assert.AreEqual(12, BounceTestSupport.Number(first, "count"), first);
         Assert.AreEqual("none", BounceTestSupport.Text(first, "disposition"), first);
         Assert.AreEqual("pass", BounceTestSupport.Text(first, "dkim"), first);
         Assert.AreEqual("pass", BounceTestSupport.Text(first, "spf"), first);
         Assert.AreEqual("d=" + _domain.Name.ToLowerInvariant() + " s=s1 result=pass", BounceTestSupport.Text(first, "dkim_auth"), first);
         Assert.AreEqual("d=" + _domain.Name.ToLowerInvariant() + " scope=mfrom result=pass", BounceTestSupport.Text(first, "spf_auth"), first);

         string second = ItemWith(report, "source_ip", "198.51.100.7");
         Assert.AreEqual("quarantine", BounceTestSupport.Text(second, "disposition"), second);
         Assert.AreEqual("fail", BounceTestSupport.Text(second, "dkim"), second);
         Assert.AreEqual("d=forger.test scope=mfrom result=pass", BounceTestSupport.Text(second, "spf_auth"), second);

         Assert.AreEqual(404, Http("GET", DomainPath + "/dmarc-reports/" + (id + 1000000)).status, "A report id that is not this domain's answered.");
         Assert.AreEqual(404, Http("GET", "/api/v1/domains/no-such-domain.test/dmarc-reports/summary").status);
      }

      [Test]
      [Description("A zip attachment is read through its central directory, and the same report delivered a second time - a retry, or two nodes reading one mailbox - is one row.")]
      public void AZipReportIsReadAndTheSameReportTwiceCountsOnce()
      {
         string reportId = NewReportId();
         long end = Now() - 1;
         long begin = end - 86399;
         string xml = AggregateReportXml(_domain.Name, reportId, begin, end);
         string name = "provider.test!" + _domain.Name + "!" + begin + "!" + end + ".zip";

         Inject(AttachmentMessage(Mailbox, Zip(name.Replace(".zip", ".xml"), xml), "application/zip", name));
         Inject(AttachmentMessage(Mailbox, Zip(name.Replace(".zip", ".xml"), xml), "application/zip", name));

         string list = Read(DomainPath + "/dmarc-reports");
         Assert.AreEqual(1, BounceTestSupport.Number(list, "total"), "The same report delivered twice made two rows: " + list);
         string item = ItemWith(list, "report_id", reportId);
         StringAssert.Contains(".zip", BounceTestSupport.Text(item, "source"), item);

         string summary = Read(DomainPath + "/dmarc-reports/summary");
         Assert.AreEqual(1, BounceTestSupport.Number(summary, "reports"), summary);
         Assert.AreEqual(15, BounceTestSupport.Number(summary, "messages"), "A repeat was counted twice: " + summary);
      }

      [Test]
      [Description("A report to an ordinary mailbox, one about a domain this server does not host, one about a hosted domain that names no report mailbox, and a message that is not a report record nothing and are delivered unchanged - and the genuine one after them is recorded, so the test can fail.")]
      public void WhatIsNotReadIsNotRead()
      {
         Account ordinary = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ordinary-" + Interlocked.Increment(ref sequence_) + "@" + _domain.Name, Password);
         long end = Now() - 1;
         long begin = end - 86399;

         // To an ordinary mailbox: the address is not an intake, whatever the attachment is.
         string delivered = Inject(GzipMessage(ordinary.Address, AggregateReportXml(_domain.Name, NewReportId(), begin, end), "report.xml.gz"), ordinary.Address);
         StringAssert.Contains("Content-Type: application/gzip", delivered, "The report to the ordinary mailbox was not delivered unchanged.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "A report delivered to an ordinary mailbox was read.");

         // About a domain this server does not host.
         Inject(GzipMessage(Mailbox, AggregateReportXml("elsewhere.test", NewReportId(), begin, end), "report.xml.gz"));
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "A report about another domain was recorded against this one.");
         BounceTestSupport.AssertLogContains("is about elsewhere.test, which is not a domain this server hosts with a report mailbox",
            "The server did not say why the report about elsewhere.test was not recorded.");

         // About a hosted domain that names no mailbox: hosted is not enough.
         const string Sister = "dmarc-sister.test";
         Domain sister = SingletonProvider<TestSetup>.Instance.AddDomain(Sister);
         try
         {
            Inject(GzipMessage(Mailbox, AggregateReportXml(Sister, NewReportId(), begin, end), "report.xml.gz"));
            Assert.AreEqual(0, BounceTestSupport.Number(Read("/api/v1/domains/" + Sister + "/dmarc-reports"), "total"),
               "A report about a hosted domain that names no report mailbox was recorded.");
            Assert.IsFalse(BounceTestSupport.Flag(Read("/api/v1/domains/" + Sister + "/dmarc-reports/summary"), "mailbox_configured"));
         }
         finally
         {
            sister.Delete();
         }

         // About something that is not a domain name at all: refused before any
         // lookup, since the domain cache asserts on an address where a name should be.
         Inject(GzipMessage(Mailbox, AggregateReportXml("dmarc@" + _domain.Name, NewReportId(), begin, end), "report.xml.gz"));
         BounceTestSupport.AssertLogContains("policy_published names something that is not a host name",
            "The server did not refuse a policy domain that is an address.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "A report about an address was recorded.");

         // Not a report: the root element is wrong, and a message with no attachment at all.
         Inject(GzipMessage(Mailbox, "<?xml version=\"1.0\"?><report><nothing/></report>", "report.xml.gz"));
         BounceTestSupport.AssertLogContains("is not a DMARC aggregate report this server can read: the root element is not feedback",
            "The server did not say that the attachment was not a readable report.");
         delivered = Inject("From: " + Reporter + "\r\nTo: " + Mailbox + "\r\nSubject: Just a note\r\n\r\nNo report here.\r\n");
         StringAssert.Contains("No report here.", delivered, "The plain message to the report mailbox was not delivered unchanged.");
         BounceTestSupport.AssertLogContains("carries no DMARC aggregate or TLS-RPT report this server can read",
            "The server did not say that the plain message carried no report.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "Something that was not a report was recorded.");

         // The negative control: the genuine one, to the same mailbox, is recorded.
         Inject(GzipMessage(Mailbox, AggregateReportXml(_domain.Name, NewReportId(), begin, end), "report.xml.gz"));
         Assert.AreEqual(1, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"),
            "The genuine report after the refused ones was not recorded either, so this test could not have failed.");
      }

      [Test]
      [Description("A TLS-RPT report - JSON, gzip - delivered to the report mailbox is recorded per policy with its session counts and its failure details, and the window's totals add the rows up.")]
      public void ATlsRptReportIsRecorded()
      {
         string reportId = "tls-" + Guid.NewGuid().ToString("N");
         long end = Now() - 1;
         long begin = end - 86399;

         string json =
            "{\"organization-name\":\"Company-X\"," +
            "\"date-range\":{\"start-datetime\":\"" + Rfc3339(begin) + "\",\"end-datetime\":\"" + Rfc3339(end) + "\"}," +
            "\"contact-info\":\"sts-reporting@company-x.example\",\"report-id\":\"" + reportId + "\"," +
            "\"policies\":[{\"policy\":{\"policy-type\":\"sts\",\"policy-string\":[\"version: STSv1\",\"mode: enforce\"],\"policy-domain\":\"" + _domain.Name + "\",\"mx-host\":[\"*.mail." + _domain.Name + "\"]}," +
            "\"summary\":{\"total-successful-session-count\":5326,\"total-failure-session-count\":303}," +
            "\"failure-details\":[{\"result-type\":\"certificate-expired\",\"sending-mta-ip\":\"2001:db8:abcd:12::1\",\"receiving-mx-hostname\":\"mx1.mail." + _domain.Name + "\",\"receiving-ip\":\"203.0.113.6\",\"failed-session-count\":100}]}]}";

         string delivered = Inject(AttachmentMessage(Mailbox, Gzip(json), "application/tlsrpt+gzip", "company-x.example!" + _domain.Name + "!" + begin + "!" + end + "!001.json.gz"));
         StringAssert.Contains("application/tlsrpt+gzip", delivered, "The TLS report was not delivered to the mailbox unchanged.");

         string tls = Read(DomainPath + "/tls-reports?days=30");
         Assert.AreEqual(1, BounceTestSupport.Number(tls, "total"), tls);
         Assert.AreEqual(5326, BounceTestSupport.Number(tls, "sessions_succeeded"), tls);
         Assert.AreEqual(303, BounceTestSupport.Number(tls, "sessions_failed"), tls);

         string item = ItemWith(tls, "report_id", reportId);
         Assert.AreEqual("Company-X", BounceTestSupport.Text(item, "org"), item);
         Assert.AreEqual("sts-reporting@company-x.example", BounceTestSupport.Text(item, "contact"), item);
         Assert.AreEqual("sts", BounceTestSupport.Text(item, "policy_type"), item);
         Assert.AreEqual(_domain.Name.ToLowerInvariant(), BounceTestSupport.Text(item, "policy_domain"), item);
         Assert.AreEqual(begin, BounceTestSupport.Number(item, "begin"), item);
         Assert.AreEqual(end, BounceTestSupport.Number(item, "end"), item);
         Assert.AreEqual(5326, BounceTestSupport.Number(item, "successful"), item);
         Assert.AreEqual(303, BounceTestSupport.Number(item, "failed"), item);
         StringAssert.Contains("\"result_type\":\"certificate-expired\"", tls, tls);
         StringAssert.Contains("\"receiving_mx_hostname\":\"mx1.mail." + _domain.Name.ToLowerInvariant() + "\"", tls, tls);
         StringAssert.Contains("\"count\":100", tls, tls);

         // The DMARC side is untouched by a TLS report.
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"));
      }

      [Test]
      [Description("The report mailbox has to be an address this server hosts, over REST and over COM in the same sentence; an empty value is the default and reads nothing; and the COM property round-trips with the domain's save.")]
      public void TheMailboxMustBeAnAddressThisServerHosts()
      {
         (int status, string body) outside = Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"dmarc@elsewhere.test\"}");
         Assert.AreEqual(400, outside.status, outside.body);
         StringAssert.Contains("hosts", outside.body, outside.body);
         StringAssert.Contains("report_mailbox", outside.body, outside.body);
         Assert.AreEqual(Mailbox, BounceTestSupport.Text(Read(DomainPath + "/dmarc-reports"), "report_mailbox"), "A refused mailbox was stored anyway.");

         (int status, string body) nonsense = Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"not an address\"}");
         Assert.AreEqual(400, nonsense.status, nonsense.body);

         Assert.AreEqual(200, Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"\"}").status, "An empty mailbox is the default and is accepted.");
         string summary = Read(DomainPath + "/dmarc-reports/summary");
         Assert.IsFalse(BounceTestSupport.Flag(summary, "mailbox_configured"), summary);
         Assert.AreEqual("", BounceTestSupport.Text(summary, "report_mailbox"), summary);

         // Over COM: the same refusals, and a value that round-trips through Save.
         // Late-bound, because the type library build-tests.ps1 compiles against
         // is older than the property until an elevated build/register-com.ps1 has run.
         Domain domain = _application.Domains.get_ItemByName(_domain.Name);
         Assert.Throws<COMException>(() => { ((dynamic) domain).ReportMailbox = "dmarc@elsewhere.test"; });
         Assert.Throws<COMException>(() => { ((dynamic) domain).ReportMailbox = "not an address"; });

         ((dynamic) domain).ReportMailbox = " " + Mailbox.ToUpperInvariant() + " ";
         domain.Save();

         Assert.AreEqual(Mailbox.ToLowerInvariant(), (string) ((dynamic) _application.Domains.get_ItemByName(_domain.Name)).ReportMailbox,
            "The mailbox written over COM did not come back trimmed and lower-cased.");
         Assert.AreEqual(Mailbox.ToLowerInvariant(), BounceTestSupport.Text(Read(DomainPath), "report_mailbox"), "The mailbox written over COM was not what REST read.");

         // A mailbox in another hosted domain is fine: one mailbox serves an organisation.
         const string Sister = "dmarc-sister.test";
         Domain sister = SingletonProvider<TestSetup>.Instance.AddDomain(Sister);
         try
         {
            Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + Sister, "{\"active\":true,\"report_mailbox\":\"" + Mailbox + "\"}").status);
            Assert.AreEqual(Mailbox.ToLowerInvariant(), BounceTestSupport.Text(Read("/api/v1/domains/" + Sister), "report_mailbox"));
         }
         finally
         {
            sister.Delete();
         }
      }

      [Test]
      [Description("The retention sweep removes reports read more than the setting's days ago and leaves the rest; a body's older_than_days is used instead, 0 removing every one; the setting refuses a value out of range; and with the setting at 0 and no body nothing is removed.")]
      public void TheSweepRemovesOldReportsAndTheSettingIsRanged()
      {
         long end = Now() - 1;
         long begin = end - 86399;
         Inject(GzipMessage(Mailbox, AggregateReportXml(_domain.Name, NewReportId(), begin, end), "report.xml.gz"));
         Assert.AreEqual(1, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"));

         (int status, string body) tooMany = Http("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":3651}");
         Assert.AreEqual(400, tooMany.status, tooMany.body);
         StringAssert.Contains("domain_report_retention_days", tooMany.body, tooMany.body);
         Assert.AreEqual(365, BounceTestSupport.Number(Read("/api/v1/settings"), "domain_report_retention_days"), "The shipped retention is not 365 days.");

         // The task's own sweep, at the shipped retention: a report read today stays.
         _application.Utilities.RunMessageRetention();
         Assert.AreEqual(1, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "The sweep removed a report read today.");

         // The route with the setting: the same answer.
         (int status, string body) kept = Http("POST", ExpirePath);
         Assert.AreEqual(200, kept.status, kept.body);
         Assert.AreEqual(0, BounceTestSupport.Number(kept.body, "dmarc_reports_pruned"), kept.body);
         Assert.AreEqual(365, BounceTestSupport.Number(kept.body, "older_than_days"), kept.body);

         // With the setting at 0 nothing is ever removed, and the route says so.
         Assert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":0}").status);
         (int status, string body) forEver = Http("POST", ExpirePath);
         Assert.AreEqual(200, forEver.status, forEver.body);
         Assert.IsTrue(BounceTestSupport.Flag(forEver.body, "kept_for_ever"), forEver.body);
         _application.Utilities.RunMessageRetention();
         Assert.AreEqual(1, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"));

         (int status, string body) wrong = Http("POST", ExpirePath, "{\"older_than_days\":3651}");
         Assert.AreEqual(400, wrong.status, wrong.body);

         // A body's 0 removes every report, whatever the setting, and the answer
         // says the sweep finished rather than stopped at a pass.
         (int status, string body) all = Http("POST", ExpirePath, "{\"older_than_days\":0}");
         Assert.AreEqual(200, all.status, all.body);
         Assert.AreEqual(1, BounceTestSupport.Number(all.body, "dmarc_reports_pruned"), all.body);
         Assert.IsTrue(BounceTestSupport.Flag(all.body, "complete"), all.body);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports"), "total"), "The sweep did not remove the report.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/dmarc-reports/summary"), "reports"));
      }

      [Test]
      [Description("A key issued for one domain reads that domain's reports and is refused another's and the sweep; a read-only key reads and cannot sweep; and a key confined to a domain may not point that domain's report mailbox at another tenant's address, which would make the server open every attachment delivered there.")]
      public void AKeyIsConfinedToItsDomainAndItsScope()
      {
         const string OtherDomain = "dmarc-other.test";
         Domain other = null;
         (string id, string token) scoped = (null, null);
         (string id, string token) readOnly = (null, null);

         try
         {
            // Everything that has to be cleaned up is made inside the try, so a
            // failure making the second leaves nothing behind for a later fixture.
            other = SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
            scoped = CreateKey("regression - domain reports domain scoped", "full", _domain.Name);
            readOnly = CreateKey("regression - domain reports read-only", "readonly", null);

            Assert.AreEqual(200, Bearer("GET", DomainPath + "/dmarc-reports/summary", scoped.token).status);
            Assert.AreEqual(200, Bearer("GET", DomainPath + "/dmarc-reports", scoped.token).status);
            Assert.AreEqual(200, Bearer("GET", DomainPath + "/tls-reports", scoped.token).status);

            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/dmarc-reports/summary", scoped.token).status,
               "A key for one domain read another domain's summary.");
            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/dmarc-reports", scoped.token).status);
            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/tls-reports", scoped.token).status);
            Assert.AreEqual(403, Bearer("POST", ExpirePath, scoped.token, "{}").status, "The sweep prunes every domain's rows.");

            Assert.AreEqual(200, Bearer("GET", DomainPath + "/dmarc-reports/summary", readOnly.token).status);
            Assert.AreEqual(403, Bearer("POST", ExpirePath, readOnly.token, "{}").status);

            // The confined key may name a mailbox in its own domain and not in
            // another tenant's; the administrator may name either.
            (int status, string body) elsewhere = Bearer("PUT", DomainPath, scoped.token, "{\"active\":true,\"report_mailbox\":\"reports@" + OtherDomain + "\"}");
            Assert.AreEqual(403, elsewhere.status, elsewhere.body);
            StringAssert.Contains("does not administer", elsewhere.body, elsewhere.body);
            Assert.AreEqual(Mailbox, BounceTestSupport.Text(Read(DomainPath), "report_mailbox"), "The refused mailbox was stored anyway.");

            Assert.AreEqual(200, Bearer("PUT", DomainPath, scoped.token, "{\"active\":true,\"report_mailbox\":\"" + Mailbox + "\"}").status,
               "A confined key could not name a mailbox in its own domain.");
            Assert.AreEqual(200, Bearer("PUT", DomainPath, scoped.token, "{\"active\":true}").status,
               "A confined key could not save the domain without touching the mailbox.");
            Assert.AreEqual(200, Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"reports@" + OtherDomain + "\"}").status,
               "The administrator could not name a mailbox in another hosted domain.");
            Assert.AreEqual(200, Http("PUT", DomainPath, "{\"active\":true,\"report_mailbox\":\"" + Mailbox + "\"}").status);
         }
         finally
         {
            if (scoped.id != null)
               Http("DELETE", "/api/v1/apikeys/" + scoped.id);
            if (readOnly.id != null)
               Http("DELETE", "/api/v1/apikeys/" + readOnly.id);
            if (other != null)
               other.Delete();
         }
      }

      [Test]
      [Description("The report mailbox follows a rename of the domain that hosts it and is cleared when that domain is deleted, on the renamed domain itself and on a sister domain that shares the mailbox; a later save that does not touch the mailbox is not refused for a value the caller never wrote; and a report delivered to the address under its new name is read.")]
      public void TheMailboxFollowsARenameAndGoesWithTheDomainThatHostsIt()
      {
         const string Before = "dmarc-rename.test";
         const string After = "dmarc-renamed.test";
         const string Sister = "dmarc-sister.test";

         try
         {
            Domain renamed = SingletonProvider<TestSetup>.Instance.AddDomain(Before);
            SingletonProvider<TestSetup>.Instance.AddAccount(renamed, "dmarc@" + Before, Password);
            Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + Before, "{\"active\":true,\"report_mailbox\":\"dmarc@" + Before + "\"}").status);

            // The sister names the same mailbox, as an organisation does.
            SingletonProvider<TestSetup>.Instance.AddDomain(Sister);
            Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + Sister, "{\"active\":true,\"report_mailbox\":\"dmarc@" + Before + "\"}").status);

            // The rename: the mailbox follows on both rows, and the answer to the
            // rename already says so.
            (int status, string body) rename = Http("PUT", "/api/v1/domains/" + Before, "{\"active\":true,\"name\":\"" + After + "\"}");
            Assert.AreEqual(200, rename.status, rename.body);
            Assert.AreEqual("dmarc@" + After, BounceTestSupport.Text(rename.body, "report_mailbox"), rename.body);
            Assert.AreEqual("dmarc@" + After, BounceTestSupport.Text(Read("/api/v1/domains/" + After), "report_mailbox"), "The renamed domain's own mailbox did not follow the rename.");
            Assert.AreEqual("dmarc@" + After, BounceTestSupport.Text(Read("/api/v1/domains/" + Sister), "report_mailbox"), "The sister's mailbox did not follow the rename of the domain that hosts it.");
            BounceTestSupport.AssertLogContains("The report mailbox of " + Sister + " is now dmarc@" + After, "The server did not say that the sister's mailbox was rewritten.");

            // The save that used to be refused: a field that is not the mailbox.
            (int status, string body) later = Http("PUT", "/api/v1/domains/" + After, "{\"active\":true,\"postmaster\":\"postmaster@" + After + "\"}");
            Assert.AreEqual(200, later.status, "A save that did not touch the mailbox was refused: " + later.body);
            Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + Sister, "{\"active\":true}").status, "The sister could not be saved after the rename.");

            // Over COM as the Control Panel saves: every field written back, the
            // mailbox among them, and accepted.
            Domain fetched = _application.Domains.get_ItemByName(After);
            ((dynamic) fetched).ReportMailbox = (string) ((dynamic) fetched).ReportMailbox;
            fetched.Save();

            // A report to the address under its new name is read.
            long end = Now() - 1;
            long begin = end - 86399;
            Inject(GzipMessage("dmarc@" + After, AggregateReportXml(After, NewReportId(), begin, end), "report.xml.gz"), "dmarc@" + After);
            Assert.AreEqual(1, BounceTestSupport.Number(Read("/api/v1/domains/" + After + "/dmarc-reports"), "total"), "A report to the mailbox under its new name was not read.");

            // Deleting the domain that hosts the shared mailbox clears the
            // sister's, says so, and leaves the sister saveable.
            fetched.Delete();

            Assert.AreEqual("", BounceTestSupport.Text(Read("/api/v1/domains/" + Sister), "report_mailbox"), "The sister's mailbox was not cleared when the domain hosting it went.");
            BounceTestSupport.AssertLogContains("The report mailbox of " + Sister + " was cleared", "The server did not say that the sister's mailbox was cleared.");
            Assert.IsFalse(BounceTestSupport.Flag(Read("/api/v1/domains/" + Sister + "/dmarc-reports/summary"), "mailbox_configured"));
            Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + Sister, "{\"active\":true}").status, "The sister could not be saved after its mailbox's domain was deleted.");
         }
         finally
         {
            // Whichever of the three exist at this point, whatever failed.
            foreach (string name in new[] { Sister, After, Before })
            {
               Domain leftover = DomainOrNull(name);
               if (leftover != null)
                  leftover.Delete();
            }
         }
      }

      /// <summary>The domain by name, or null: ItemByName raises for a name the server does not have.</summary>
      private Domain DomainOrNull(string name)
      {
         try
         {
            return _application.Domains.get_ItemByName(name);
         }
         catch (COMException)
         {
            return null;
         }
      }

      [Test]
      [Description("The OpenAPI document describes the five routes and the domain's report_mailbox, which is what the Deck draws its page and its form from.")]
      public void TheOpenApiDocumentDescribesTheRoutes()
      {
         string document = Read("/api/v1/openapi.json");

         foreach (string path in new[]
                  {
                     "\"/api/v1/domains/{domain}/dmarc-reports/summary\"",
                     "\"/api/v1/domains/{domain}/dmarc-reports\"",
                     "\"/api/v1/domains/{domain}/dmarc-reports/{id}\"",
                     "\"/api/v1/domains/{domain}/tls-reports\"",
                     "\"/api/v1/domain-reports/expire\""
                  })
         {
            StringAssert.Contains(path, document, "The OpenAPI document does not describe " + path + ".");
         }

         StringAssert.Contains("report_mailbox", document, "The domain's report_mailbox is not in the OpenAPI document.");
         StringAssert.Contains("domain_report_retention_days", document, "The retention setting is not in the OpenAPI document.");
      }

      // ---- the reports, as a receiver builds them

      private static string AggregateReportXml(string domain, string reportId, long begin, long end)
      {
         return
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" +
            "<feedback>\n" +
            "  <report_metadata>\n" +
            "    <org_name>provider.test</org_name>\n" +
            "    <email>" + Reporter + "</email>\n" +
            "    <report_id>" + reportId + "</report_id>\n" +
            "    <date_range><begin>" + begin + "</begin><end>" + end + "</end></date_range>\n" +
            "  </report_metadata>\n" +
            "  <policy_published>\n" +
            "    <domain>" + domain + "</domain>\n" +
            "    <adkim>r</adkim><aspf>r</aspf><p>quarantine</p><sp>reject</sp><pct>100</pct>\n" +
            "  </policy_published>\n" +
            "  <record>\n" +
            "    <row><source_ip>192.0.2.10</source_ip><count>12</count>\n" +
            "      <policy_evaluated><disposition>none</disposition><dkim>pass</dkim><spf>pass</spf></policy_evaluated></row>\n" +
            "    <identifiers><header_from>" + domain + "</header_from><envelope_from>" + domain + "</envelope_from></identifiers>\n" +
            "    <auth_results><dkim><domain>" + domain + "</domain><selector>s1</selector><result>pass</result></dkim>\n" +
            "      <spf><domain>" + domain + "</domain><scope>mfrom</scope><result>pass</result></spf></auth_results>\n" +
            "  </record>\n" +
            "  <record>\n" +
            "    <row><source_ip>198.51.100.7</source_ip><count>3</count>\n" +
            "      <policy_evaluated><disposition>quarantine</disposition><dkim>fail</dkim><spf>fail</spf></policy_evaluated></row>\n" +
            "    <identifiers><header_from>" + domain + "</header_from></identifiers>\n" +
            "    <auth_results><spf><domain>forger.test</domain><scope>mfrom</scope><result>pass</result></spf></auth_results>\n" +
            "  </record>\n" +
            "</feedback>\n";
      }

      private static byte[] Gzip(string text)
      {
         using (var output = new MemoryStream())
         {
            using (var gzip = new GZipStream(output, CompressionMode.Compress, true))
            {
               byte[] bytes = Encoding.UTF8.GetBytes(text);
               gzip.Write(bytes, 0, bytes.Length);
            }

            return output.ToArray();
         }
      }

      private static byte[] Zip(string entryName, string text)
      {
         using (var output = new MemoryStream())
         {
            using (var archive = new ZipArchive(output, ZipArchiveMode.Create, true))
            {
               ZipArchiveEntry entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
               using (Stream stream = entry.Open())
               {
                  byte[] bytes = Encoding.UTF8.GetBytes(text);
                  stream.Write(bytes, 0, bytes.Length);
               }
            }

            return output.ToArray();
         }
      }

      private static string GzipMessage(string to, string xml, string attachmentName)
      {
         return AttachmentMessage(to, Gzip(xml), "application/gzip", attachmentName);
      }

      /// <summary>
      ///    A message as a receiver sends a report: a line of text and the report
      ///    as a base64 attachment, in a multipart/mixed.
      /// </summary>
      private static string AttachmentMessage(string to, byte[] attachment, string contentType, string attachmentName)
      {
         const string Boundary = "report-boundary-7A1C2E";

         return
            "From: " + Reporter + "\r\n" +
            "To: " + to + "\r\n" +
            "Subject: Report Domain: example Submitter: provider.test\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/mixed; boundary=\"" + Boundary + "\"\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "This is an aggregate report.\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: " + contentType + "; name=\"" + attachmentName + "\"\r\n" +
            "Content-Disposition: attachment; filename=\"" + attachmentName + "\"\r\n" +
            "Content-Transfer-Encoding: base64\r\n" +
            "\r\n" +
            Convert.ToBase64String(attachment, Base64FormattingOptions.InsertLineBreaks) + "\r\n" +
            "\r\n" +
            "--" + Boundary + "--\r\n";
      }

      /// <summary>
      ///    Delivers the message over SMTP, waits for the queue to empty and reads it
      ///    back out of the mailbox over POP3 - the proof that it was delivered
      ///    unchanged, read or not.
      /// </summary>
      private string Inject(string text, string to = null)
      {
         string recipient = to ?? Mailbox;

         SmtpClientSimulator.StaticSendRaw(Reporter, recipient, text);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         return Pop3ClientSimulator.AssertGetFirstMessageText(recipient, Password);
      }

      private static string NewReportId()
      {
         return "report-" + Guid.NewGuid().ToString("N");
      }

      private static long Now()
      {
         return DateTimeOffset.UtcNow.ToUnixTimeSeconds();
      }

      private static string DayOf(long seconds)
      {
         return DateTimeOffset.FromUnixTimeSeconds(seconds).UtcDateTime.ToString("yyyy-MM-dd");
      }

      private static string Rfc3339(long seconds)
      {
         return DateTimeOffset.FromUnixTimeSeconds(seconds).UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'");
      }

      /// <summary>The flat object in a document holding this name and value, or a failed assertion.</summary>
      private static string ItemWith(string document, string name, string value)
      {
         int at = document.IndexOf("\"" + name + "\":\"" + value + "\"", StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No item with " + name + " " + value + " in: " + document);

         int start = document.LastIndexOf('{', at);
         int end = document.IndexOf('}', at);
         Assert.IsTrue(start >= 0 && end > start, "No object around " + name + " " + value + " in: " + document);

         return document.Substring(start, end - start + 1);
      }

      private static (string id, string token) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";

         (int status, string answer) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + answer);

         return (BounceTestSupport.Text(answer, "id"), BounceTestSupport.Text(answer, "key"));
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         return BounceTestSupport.Http(RestPort, method, path, requestBody);
      }

      private static string Read(string path)
      {
         return BounceTestSupport.Read(RestPort, path);
      }
   }
}
