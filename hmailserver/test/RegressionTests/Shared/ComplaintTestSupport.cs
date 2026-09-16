// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    What the two fixtures for complaints read - the feedback loops - share: the
   ///    four settings put back, an RFC 5965 abuse report built the way a provider
   ///    builds one, and the late-bound way to the COM members.
   ///
   ///    The documents are read as text with BounceTestSupport's readers, the way
   ///    every REST fixture in the suite reads its answers: every item the routes
   ///    write is one flat object with no braces in it, so "the object around this
   ///    address" is a substring and no JSON library is needed on either bench.
   /// </summary>
   public static class ComplaintTestSupport
   {
      /// <summary>
      ///    Every complaint setting back to what the server ships, whatever it holds
      ///    now. Unconditional, for a TearDown: a test that failed half way may have
      ///    changed any of them.
      /// </summary>
      public static void RestoreDefaults(Settings settings)
      {
         ComSetComplaintProcessingEnabled(settings, false);
         ComSetComplaintMailbox(settings, "");
         ComSetComplaintSuppressionThreshold(settings, 0);
         ComSetComplaintRetentionDays(settings, 180);
      }

      /// <summary>
      ///    A report in the shape a provider's feedback loop sends one: the human
      ///    readable part, the message/feedback-report part of fields, and the message
      ///    complained about as message/rfc822. Any field given as null is left out,
      ///    which is how a provider that redacts the reader sends one.
      ///
      ///    originalRcptTo null redacts the reader COMPLETELY: no Original-Rcpt-To
      ///    field AND no To: line in the wrapped message, because the server falls
      ///    back to the second when the first is missing. A report built with the To:
      ///    line still in it is not a redacted report, and a test that called it one
      ///    was asserting about an address the report never named.
      /// </summary>
      public static string ReportText(string reportTo, string feedbackType, string originalMailFrom, string originalRcptTo,
                                      string originalMessageId, string subject = "The original", string arrivalDate = "Tue, 15 Sep 2026 09:00:00 GMT",
                                      string sourceIp = "192.0.2.1", string reportedDomain = null)
      {
         const string Boundary = "feedback-boundary-9C3E1D";

         string fields =
            "Feedback-Type: " + feedbackType + "\r\n" +
            "User-Agent: SomeGenerator/1.0\r\n" +
            "Version: 1\r\n";

         if (originalMailFrom != null)
            fields += "Original-Mail-From: <" + originalMailFrom + ">\r\n";
         if (originalRcptTo != null)
            fields += "Original-Rcpt-To: <" + originalRcptTo + ">\r\n";
         if (arrivalDate != null)
            fields += "Arrival-Date: " + arrivalDate + "\r\n";
         if (sourceIp != null)
            fields += "Source-IP: " + sourceIp + "\r\n";
         if (reportedDomain != null)
            fields += "Reported-Domain: " + reportedDomain + "\r\n";

         return
            "From: Feedback Loop <fbl@provider.test>\r\n" +
            "To: " + reportTo + "\r\n" +
            "Subject: FW: " + subject + "\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/report; report-type=feedback-report;\r\n" +
            "\tboundary=\"" + Boundary + "\"\r\n" +
            "\r\n" +
            "This is an email abuse report for an email message received from IP " + (sourceIp ?? "192.0.2.1") + ".\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "This is an email abuse report for an email message received from IP " + (sourceIp ?? "192.0.2.1") + ".\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: message/feedback-report\r\n" +
            "\r\n" +
            fields +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: message/rfc822\r\n" +
            "\r\n" +
            "From: " + (originalMailFrom ?? "somebody@example.test") + "\r\n" +
            (originalRcptTo == null ? "" : "To: " + originalRcptTo + "\r\n") +
            "Subject: " + subject + "\r\n" +
            "Message-ID: <" + originalMessageId + ">\r\n" +
            "\r\n" +
            "Body text.\r\n" +
            "\r\n" +
            "--" + Boundary + "--\r\n";
      }

      /// <summary>
      ///    A report with no Original-Rcpt-To field but with the wrapped message's
      ///    To: line still in it - which several providers send, and which the server
      ///    falls back to. Built by hand rather than through ReportText, so that the
      ///    two redactions are told apart by the fixture instead of by accident.
      /// </summary>
      public static string ReportWithRcptToOnlyInTheWrappedMessage(string reportTo, string originalMailFrom,
                                                                  string wrappedTo, string originalMessageId)
      {
         string report = ReportText(reportTo, "abuse", originalMailFrom, null, originalMessageId);

         return report.Replace("From: " + originalMailFrom + "\r\n",
                               "From: " + originalMailFrom + "\r\nTo: " + wrappedTo + "\r\n");
      }

      /// <summary>
      ///    A message to the complaint mailbox that announces itself as a feedback
      ///    report and is not one: the report-type is right and the
      ///    message/feedback-report part is missing. What a provider sends after
      ///    changing its format, and what somebody probing the mailbox sends.
      /// </summary>
      public static string MalformedReportText(string reportTo)
      {
         const string Boundary = "feedback-boundary-BROKEN";

         return
            "From: Feedback Loop <fbl@provider.test>\r\n" +
            "To: " + reportTo + "\r\n" +
            "Subject: FW: something a reader did not want\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/report; report-type=feedback-report;\r\n" +
            "\tboundary=\"" + Boundary + "\"\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "Somebody complained, and this report does not say who or about what.\r\n" +
            "\r\n" +
            "--" + Boundary + "--\r\n";
      }

      // The complaint members of Settings and Utilities are newer than the type
      // library build-tests.ps1 compiles against until an elevated
      // build/register-com.ps1 has run, so every fixture reaches them through these,
      // which bind late as BounceTestSupport's and Roles.cs's helpers do; a refusal
      // still arrives as the COMException each assertion expects.
      public static bool ComComplaintProcessingEnabled(Settings settings) => (bool) ((dynamic) settings).ComplaintProcessingEnabled;

      public static void ComSetComplaintProcessingEnabled(Settings settings, bool value) => ((dynamic) settings).ComplaintProcessingEnabled = value;

      public static string ComComplaintMailbox(Settings settings) => (string) ((dynamic) settings).ComplaintMailbox;

      public static void ComSetComplaintMailbox(Settings settings, string value) => ((dynamic) settings).ComplaintMailbox = value;

      public static int ComComplaintSuppressionThreshold(Settings settings) => (int) ((dynamic) settings).ComplaintSuppressionThreshold;

      public static void ComSetComplaintSuppressionThreshold(Settings settings, int value) => ((dynamic) settings).ComplaintSuppressionThreshold = value;

      public static int ComComplaintRetentionDays(Settings settings) => (int) ((dynamic) settings).ComplaintRetentionDays;

      public static void ComSetComplaintRetentionDays(Settings settings, int value) => ((dynamic) settings).ComplaintRetentionDays = value;

      public static string ComGetComplaints(Utilities utilities, string domainName, string search, int maxRows) =>
         (string) ((dynamic) utilities).GetComplaints(domainName, search, maxRows);

      public static string ComGetComplaintRates(Utilities utilities) => (string) ((dynamic) utilities).GetComplaintRates();
   }
}
