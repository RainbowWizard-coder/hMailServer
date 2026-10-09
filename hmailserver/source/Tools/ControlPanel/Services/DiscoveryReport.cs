// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One eDiscovery case, as GET /api/v1/discovery/cases answers it.</summary>
   public sealed class DiscoveryCase
   {
      public long Id { get; init; }
      public string Name { get; init; } = "";
      public string Description { get; init; } = "";

      /// <summary>open or closed - the server's words, which the page translates.</summary>
      public string State { get; init; } = "";

      public string CreatedBy { get; init; } = "";

      /// <summary>Seconds since 1970, as every time in the discovery routes.</summary>
      public long Created { get; init; }

      public string ClosedBy { get; init; } = "";
      public long Closed { get; init; }
      public long SearchCount { get; init; }
      public long LiveHolds { get; init; }
      public long ExportCount { get; init; }

      /// <summary>
      /// How the audit trail names every row of the case but the one that opened
      /// it: DiscoveryStore::CaseLabel, "case 3: Smith v. Jones". Up to and
      /// including the colon, so that a name the trail's column cut short still
      /// matches, and case 3 is never case 31.
      /// </summary>
      public string AuditPrefix => "case " + Id.ToString(CultureInfo.InvariantCulture) + ":"; // no-loc
   }

   /// <summary>
   /// The eDiscovery page's reading of the REST API: where the listener is, from
   /// its four settings; the cases, from GET /api/v1/discovery/cases; and which
   /// rows of GET /api/v1/audit are a case's. Pure, so the shape the page depends
   /// on is pinned by a test rather than by looking at a screen, and the refusals
   /// are sentences the page shows where the table would be, never exceptions.
   /// </summary>
   public sealed class DiscoveryReport
   {
      public IReadOnlyList<DiscoveryCase> Cases { get; private init; } = Array.Empty<DiscoveryCase>();
      public string Error { get; private init; }

      /// <summary>
      /// The audit trail's object types for the four discovery tables and for the
      /// download, which writes no row: AuditTrail.cpp's AuditedTables and
      /// RestApiDiscovery.cpp's RecordEvent. The audit route filters on one type
      /// at a time, exactly, so the page asks for each.
      /// </summary>
      public static readonly IReadOnlyList<string> AuditObjectTypes =
         new[] { "discovery case", "discovery search", "discovery hold", "discovery export" }; // no-loc

      public static DiscoveryReport Parse(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            var cases = new List<DiscoveryCase>();
            if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("cases", out JsonElement list) && list.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement c in list.EnumerateArray())
               {
                  cases.Add(new DiscoveryCase
                  {
                     Id = Long(c, "id"),
                     Name = Text(c, "name"),
                     Description = Text(c, "description"),
                     State = Text(c, "state"),
                     CreatedBy = Text(c, "created_by"),
                     Created = Long(c, "created"),
                     ClosedBy = Text(c, "closed_by"),
                     Closed = Long(c, "closed"),
                     SearchCount = Long(c, "search_count"),
                     LiveHolds = Long(c, "live_holds"),
                     ExportCount = Long(c, "export_count")
                  });
               }
            }

            return new DiscoveryReport { Cases = cases };
         }
         catch (Exception ex) when (ex is JsonException || ex is InvalidOperationException || ex is FormatException)
         {
            return new DiscoveryReport { Error = ex.Message };
         }
      }

      /// <summary>
      /// A case's rows of the audit trail, newest first. Every row but one is
      /// named by the case's label. The row that opened the case is named by the
      /// case's name alone, as a domain's is, because the insert that writes it has
      /// no id to name it by yet (DiscoveryStore::CreateCase); it is told from
      /// another case of the same name by its time, which is the case's own
      /// creation time to within a few seconds.
      /// </summary>
      public static List<AuditRow> LinesOf(DiscoveryCase theCase, IEnumerable<AuditRow> rows)
      {
         if (theCase == null || rows == null)
            return new List<AuditRow>();

         string prefix = theCase.AuditPrefix;
         string openingName = AuditObjectName(theCase.Name);
         return rows
            .Where(r => r.ObjectName.StartsWith(prefix, StringComparison.Ordinal) ||
                        (r.ObjectType == "discovery case" && r.Action == "created" && // no-loc
                         r.ObjectName == openingName && Math.Abs(r.Time - theCase.Created) <= OpeningSlackSeconds))
            .GroupBy(r => r.Id)
            .Select(g => g.First())
            .OrderByDescending(r => r.Time)
            .ThenByDescending(r => r.Id)
            .ToList();
      }

      /// <summary>How far apart the opening row's time and the case's creation time may be.</summary>
      public const int OpeningSlackSeconds = 5;

      /// <summary>The longest object name the audit trail keeps whole (AuditTrail.cpp's MaxValueLength).</summary>
      public const int AuditNameLength = 120;

      /// <summary>
      /// A name as the audit trail stores it for a row's object: cut at 120
      /// characters with "..." after it, as AuditTrail.cpp's Truncate does, so
      /// that the opening row of a case with a long name (up to 255) is still
      /// found by its name.
      /// </summary>
      public static string AuditObjectName(string name)
      {
         string value = name ?? "";
         return value.Length > AuditNameLength ? value.Substring(0, AuditNameLength) + "..." : value; // no-loc
      }

      /// <summary>The sentence in a refusal's {"error": ...}, or "" when the body carries none.</summary>
      public static string ErrorOf(string body)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(body ?? "");
            return document.RootElement.ValueKind == JsonValueKind.Object ? Text(document.RootElement, "error") : "";
         }
         catch (Exception ex) when (ex is JsonException || ex is InvalidOperationException)
         {
            return "";
         }
      }

      /// <summary>
      /// Where the REST listener is, for a Control Panel on the server or on
      /// another machine, by the server's own rules - Application::StartServers,
      /// then RestApiServer::Start: a port of 0 is off; with RestApiCertificateFile
      /// empty the server takes the ACME certificate instead (fullchain.pem and
      /// privkey.pem in its ACME folder) when both are there; with a certificate
      /// and a key it serves HTTPS on the bind address; with neither it serves
      /// plain HTTP, and only on a loopback address. Null and a sentence when the
      /// listener is off, is not running, or cannot be reached from here.
      ///
      /// acmeCertificate says whether that ACME pair is there: true or false for a
      /// Control Panel on the server, which can look (AcmeCertificateFolder,
      /// AcmeCertificateIn), and null from another machine, which cannot. Not
      /// known, a listener bound beyond loopback with no certificate of its own
      /// can only be running on the ACME one, so HTTPS is asked, and a listener
      /// that is not running answers that it could not be reached.
      ///
      /// The address asked is the one the certificate is likeliest to name: the
      /// host the Control Panel connected to, when that is another machine;
      /// "localhost", when the listener is bound to every address; and the bind
      /// address, when it is bound to one. A certificate that does not name it -
      /// the ACME one, on a loopback bind, names the server's host names - is
      /// checked by the request rather than here: this page sends a password, and
      /// sends it past such a certificate only on the server itself and only when
      /// it is byte for byte the one the listener serves, read from this machine's
      /// disk (RestSettings.TrustsCertificate); anything else is refused.
      /// </summary>
      public static Uri Endpoint(string port, string bindAddress, string certificateFile, string privateKeyFile,
         bool? acmeCertificate, bool localSession, string host, out string refusal)
      {
         refusal = null;

         string portText = (port ?? "").Trim();
         if (!int.TryParse(portText, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) || number <= 0 || number > 65535)
         {
            refusal = F("The REST API is off (its port on the API & monitoring page is {0}), and this page reads the cases over it. Give it a port there, or read the cases in the Control Deck.",
               portText.Length == 0 ? "0" : portText);
            return null;
         }

         string bind = string.IsNullOrWhiteSpace(bindAddress) ? "127.0.0.1" : bindAddress.Trim(); // no-loc
         bool loopback = bind == "127.0.0.1" || bind == "::1" || bind.Equals("localhost", StringComparison.OrdinalIgnoreCase); // no-loc
         bool everyAddress = bind == "0.0.0.0" || bind == "::"; // no-loc

         // The server's pair when it has one; the ACME pair in its place when the
         // certificate is empty and the pair is there - or, from another machine,
         // when the pair cannot be looked for and only it could be serving.
         bool configured = !string.IsNullOrWhiteSpace(certificateFile) && !string.IsNullOrWhiteSpace(privateKeyFile);
         bool acmeFallback = string.IsNullOrWhiteSpace(certificateFile) &&
                             (acmeCertificate == true || (acmeCertificate == null && !loopback));
         bool tls = configured || acmeFallback;

         if (!tls && !loopback)
         {
            refusal = F("The REST API is not running: it serves plain HTTP on 127.0.0.1 or ::1 only, and listening on {0} needs a TLS certificate file and its private key file on the API & monitoring page, or an ACME certificate issued to this server.", bind);
            return null;
         }

         string target;
         if (!localSession)
         {
            if (loopback)
            {
               refusal = F("The REST API listens on {0} only, which a Control Panel on another machine cannot reach. Read the cases in the Control Deck on the server.", bind);
               return null;
            }

            target = string.IsNullOrWhiteSpace(host) ? bind : host.Trim();
         }
         else
         {
            target = everyAddress ? "localhost" : bind; // no-loc
         }

         if (target.Contains(':') && !target.StartsWith("[", StringComparison.Ordinal))
            target = "[" + target + "]";

         string scheme = tls ? "https" : "http"; // no-loc
         return new Uri(scheme + "://" + target + ":" + number.ToString(CultureInfo.InvariantCulture) + "/");
      }

      /// <summary>
      /// Where the server looks for an issued ACME certificate, as
      /// AcmeClient::GetCertificateDirectory does: AcmeCertificateDirectory when it
      /// is set, else the data folder's ACME. Null when neither is known.
      /// </summary>
      public static string AcmeCertificateFolder(string configured, string dataFolder)
      {
         string folder = (configured ?? "").Trim();
         if (folder.Length > 0)
            return folder;

         string data = (dataFolder ?? "").Trim();
         return data.Length > 0 ? System.IO.Path.Combine(data, "ACME") : null; // no-loc
      }

      /// <summary>Whether both halves of the ACME pair the listener falls back to are in the folder.</summary>
      public static bool AcmeCertificateIn(string folder, Func<string, bool> fileExists)
         => !string.IsNullOrEmpty(folder) && fileExists != null &&
            fileExists(System.IO.Path.Combine(folder, "fullchain.pem")) && // no-loc
            fileExists(System.IO.Path.Combine(folder, "privkey.pem")); // no-loc

      private static string Text(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static long Long(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out long number) ? number : 0;
   }
}
