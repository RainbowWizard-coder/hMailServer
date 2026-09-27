// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The database settings the server answers over its REST API's settings groups
   /// and over no COM property - the webmail's sign-in, the alerts, the SNMP agent,
   /// the object store and the rest (build/check-db-settings-coverage.py lists which
   /// are reached this way) - as the settings pages read and write them.
   ///
   /// Pure, so a test pins the shape RestApiSettings.cpp answers and takes: where
   /// the listener is, by the server's own rules; one group's answer, read into
   /// values; and the body of a save, which carries only the keys that changed, as
   /// the server asks of a client that reads a group and writes it back. The network
   /// half, which signs in and sends, is RestSettingsClient in the application.
   /// </summary>
   public static class RestSettings
   {
      /// <summary>GET and PUT /api/v1/settings: the server-wide settings group.</summary>
      public const string ServerGroup = "/api/v1/settings"; // no-loc

      /// <summary>GET and PUT /api/v1/settings/openpgp.</summary>
      public const string OpenPgpGroup = "/api/v1/settings/openpgp"; // no-loc

      /// <summary>GET and PUT /api/v1/settings/push: Web Push.</summary>
      public const string PushGroup = "/api/v1/settings/push"; // no-loc

      /// <summary>
      /// Where the REST listener is, by the rules DiscoveryReport.Endpoint states
      /// (Application::StartServers, then RestApiServer::Start), with the sentences a
      /// settings page needs: null and the reason when the listener is off, is not
      /// running, or cannot be reached from this machine.
      /// </summary>
      public static Uri Endpoint(string port, string bindAddress, string certificateFile, string privateKeyFile,
         bool? acmeCertificate, bool localSession, string host, out string refusal)
      {
         refusal = null;

         string portText = (port ?? "").Trim();
         if (!int.TryParse(portText, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) || number <= 0 || number > 65535)
         {
            refusal = F("The REST API is off (RestApiPort is {0}), and these settings are reached over it: the server keeps them, but no COM property carries them. Give the REST API a port on the API & monitoring page, reinitialize the server, and open this page again.",
               portText.Length == 0 ? "0" : portText);
            return null;
         }

         string bind = string.IsNullOrWhiteSpace(bindAddress) ? "127.0.0.1" : bindAddress.Trim(); // no-loc
         bool loopback = bind == "127.0.0.1" || bind == "::1" || bind.Equals("localhost", StringComparison.OrdinalIgnoreCase); // no-loc
         bool everyAddress = bind == "0.0.0.0" || bind == "::"; // no-loc

         bool configured = !string.IsNullOrWhiteSpace(certificateFile) && !string.IsNullOrWhiteSpace(privateKeyFile);
         bool acmeFallback = string.IsNullOrWhiteSpace(certificateFile) &&
                             (acmeCertificate == true || (acmeCertificate == null && !loopback));
         bool tls = configured || acmeFallback;

         if (!tls && !loopback)
         {
            refusal = F("The REST API is not running: it serves plain HTTP on 127.0.0.1 or ::1 only, and listening on {0} needs RestApiCertificateFile and RestApiPrivateKeyFile, or an ACME certificate issued to this server.", bind);
            return null;
         }

         string target;
         if (!localSession)
         {
            if (loopback)
            {
               refusal = F("The REST API listens on {0} only, which a Control Panel on another machine cannot reach. Change these settings in the Control Deck on the server, or from a Control Panel running there.", bind);
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
      /// One group's GET answer as its keys and values, or null and the reason. The
      /// answer is one flat object (RestApiSettings.cpp's GroupJson): a value per
      /// key, with &lt;key&gt;_environment and &lt;key&gt;_levels marks beside some, which
      /// are kept here like any other member and never sent back.
      /// </summary>
      public static IReadOnlyDictionary<string, JsonElement> ParseGroup(string body, out string error)
      {
         error = null;
         try
         {
            using JsonDocument document = JsonDocument.Parse(body ?? "");
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
               error = L("The server's answer is not a settings group.");
               return null;
            }

            var values = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
            foreach (JsonProperty member in document.RootElement.EnumerateObject())
               values[member.Name] = member.Value.Clone();
            return values;
         }
         catch (JsonException ex)
         {
            error = F("The server's answer could not be read: {0}", ex.Message);
            return null;
         }
      }

      /// <summary>
      /// A key's value as an editor takes it: a bool, a whole number (long) or a
      /// string - and null when the group does not carry the key, or carries null,
      /// which is how the server answers a value a file of its environment supplied.
      /// </summary>
      public static object ValueOf(IReadOnlyDictionary<string, JsonElement> group, string key)
      {
         if (group == null || key == null || !group.TryGetValue(key, out JsonElement value))
            return null;

         return value.ValueKind switch
         {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number => value.TryGetInt64(out long number) ? number : null,
            JsonValueKind.String => value.GetString(),
            _ => null
         };
      }

      /// <summary>
      /// The body of a PUT: every key given, typed as the server reads it - a bool
      /// as true or false, a whole number as a number, anything else as a string.
      /// Only the keys that changed are given: the server applies what a body names,
      /// and a key sent back unchanged is one a value the environment supplied could
      /// be overwritten by.
      /// </summary>
      public static string Body(IEnumerable<KeyValuePair<string, object>> changes)
      {
         using var stream = new MemoryStream();
         using (var writer = new Utf8JsonWriter(stream))
         {
            writer.WriteStartObject();
            foreach (KeyValuePair<string, object> change in changes ?? Array.Empty<KeyValuePair<string, object>>())
            {
               switch (change.Value)
               {
                  case bool flag:
                     writer.WriteBoolean(change.Key, flag);
                     break;
                  case int whole:
                     writer.WriteNumber(change.Key, whole);
                     break;
                  case long wide:
                     writer.WriteNumber(change.Key, wide);
                     break;
                  case null:
                     writer.WriteString(change.Key, "");
                     break;
                  default:
                     writer.WriteString(change.Key, Convert.ToString(change.Value, CultureInfo.InvariantCulture));
                     break;
               }
            }
            writer.WriteEndObject();
         }

         return Encoding.UTF8.GetString(stream.ToArray());
      }

      /// <summary>
      /// What a refused request says, in the server's words when it gave some: a
      /// status of 0 is a request that never had an answer, and message is then why.
      /// </summary>
      public static string Refusal(int status, string body, bool secondFactorRequired, string message = null)
      {
         if (status == 0)
            return F("The REST API could not be reached: {0}", string.IsNullOrWhiteSpace(message) ? L("no answer") : message);

         string sentence = DiscoveryReport.ErrorOf(body);

         if (status == 401 && secondFactorRequired)
            return L("The administrator has a second factor, and the REST API asks for its one-time code before it answers. Open the page again to be asked for the code.");

         if (status == 401)
            return L("The REST API refused the credential this Control Panel signed in with.") + (sentence.Length == 0 ? "" : " " + sentence);

         return sentence.Length == 0
            ? F("The REST API answered {0}.", status)
            : F("The REST API answered {0}: {1}", status, sentence);
      }
   }
}
