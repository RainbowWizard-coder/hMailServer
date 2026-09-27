// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The settings pages read and write the database settings no COM property
   /// carries through GET and PUT of RestApiSettings.cpp's settings groups. These
   /// pin the shape GroupJson answers and the body ApplyGroup takes: a key's value
   /// read as the editor takes it, a masked value read as the server masks it, and a
   /// save that names only the keys that changed, typed as the server reads them.
   /// </summary>
   public class RestSettingsTests
   {
      // The shape GroupJson writes: a value per key, an _environment mark beside a
      // key the environment sets, a text value a file supplied answered as (set),
      // and a number one supplied answered as null.
      private const string Group =
         "{\"passkeys_enabled\":true,\"oidc_auto_provision\":false,\"recovery_reset_link_minutes\":60," +
         "\"oidc_issuer\":\"https://login.example.com/\",\"oidc_client_secret_environment\":\"file\"," +
         "\"saml_idp_entity_id\":\"(set)\",\"saml_idp_entity_id_environment\":\"file\"," +
         "\"snmp_port\":null,\"snmp_port_environment\":\"file\",\"compromise_detection_mode\":\"watch\"}";

      [Fact]
      public void ParseGroup_ReadsEveryMember_AndValueOf_TypesThemAsTheEditorsTakeThem()
      {
         IReadOnlyDictionary<string, JsonElement> group = RestSettings.ParseGroup(Group, out string error);

         Assert.Null(error);
         Assert.Equal(true, RestSettings.ValueOf(group, "passkeys_enabled"));
         Assert.Equal(false, RestSettings.ValueOf(group, "oidc_auto_provision"));
         Assert.Equal(60L, RestSettings.ValueOf(group, "recovery_reset_link_minutes"));
         Assert.Equal("https://login.example.com/", RestSettings.ValueOf(group, "oidc_issuer"));
         Assert.Equal("watch", RestSettings.ValueOf(group, "compromise_detection_mode"));

         // A write-only secret has no value, only its environment mark.
         Assert.Null(RestSettings.ValueOf(group, "oidc_client_secret"));
         Assert.Equal("file", RestSettings.ValueOf(group, "oidc_client_secret_environment"));

         // A masked value reads as the server masks it, and the mark says why.
         Assert.Equal("(set)", RestSettings.ValueOf(group, "saml_idp_entity_id"));
         Assert.Null(RestSettings.ValueOf(group, "snmp_port"));
         Assert.Equal(EnvironmentSettings.File, RestSettings.ValueOf(group, "snmp_port_environment"));

         // A key the group does not carry - an older server - is null, not an error.
         Assert.Null(RestSettings.ValueOf(group, "a_key_no_server_has"));
         Assert.Null(RestSettings.ValueOf(null, "passkeys_enabled"));
      }

      [Fact]
      public void ParseGroup_OfWhatIsNotAGroup_IsAnErrorAndNoValues()
      {
         Assert.Null(RestSettings.ParseGroup("[1,2]", out string array));
         Assert.NotNull(array);

         Assert.Null(RestSettings.ParseGroup("<html>", out string html));
         Assert.NotNull(html);

         Assert.Null(RestSettings.ParseGroup(null, out string nothing));
         Assert.NotNull(nothing);
      }

      [Fact]
      public void Body_NamesOnlyTheKeysGiven_TypedAsTheServerReadsThem()
      {
         string body = RestSettings.Body(new[]
         {
            new KeyValuePair<string, object>("passkeys_enabled", false),
            new KeyValuePair<string, object>("recovery_reset_link_minutes", 45),
            new KeyValuePair<string, object>("message_store_object_cache_mb", 4096L),
            new KeyValuePair<string, object>("oidc_issuer", "https://login.example.com/\"quoted\""),
            new KeyValuePair<string, object>("oidc_administrators", null)
         });

         using JsonDocument document = JsonDocument.Parse(body);
         JsonElement root = document.RootElement;

         Assert.Equal(5, CountMembers(root));
         Assert.Equal(JsonValueKind.False, root.GetProperty("passkeys_enabled").ValueKind);
         Assert.Equal(45, root.GetProperty("recovery_reset_link_minutes").GetInt32());
         Assert.Equal(4096, root.GetProperty("message_store_object_cache_mb").GetInt64());
         Assert.Equal("https://login.example.com/\"quoted\"", root.GetProperty("oidc_issuer").GetString());
         Assert.Equal("", root.GetProperty("oidc_administrators").GetString());

         Assert.Equal("{}", RestSettings.Body(null));
      }

      [Fact]
      public void Endpoint_FollowsTheListenersOwnRules_WithTheSettingsPagesSentences()
      {
         // Off: the sentence names the setting and the page it is set on.
         Assert.Null(RestSettings.Endpoint("0", "127.0.0.1", "", "", false, true, "localhost", out string off));
         Assert.Contains("RestApiPort", off);
         Assert.Contains("API & monitoring", off);
         Assert.Null(RestSettings.Endpoint("", "", "", "", false, true, "localhost", out _));
         Assert.Null(RestSettings.Endpoint("70000", "", "", "", false, true, "localhost", out _));

         // Plain HTTP on a loopback address, from this machine.
         Assert.Equal("http://127.0.0.1:8045/", RestSettings.Endpoint("8045", "", "", "", false, true, "localhost", out string none).ToString());
         Assert.Null(none);
         Assert.Equal("http://[::1]:8045/", RestSettings.Endpoint("8045", "::1", "", "", false, true, "localhost", out _).ToString());

         // Plain HTTP anywhere else is a listener the server does not start.
         Assert.Null(RestSettings.Endpoint("8045", "0.0.0.0", "", "", false, true, "localhost", out string notRunning));
         Assert.Contains("RestApiCertificateFile", notRunning);

         // A loopback listener, from another machine.
         Assert.Null(RestSettings.Endpoint("8045", "127.0.0.1", "", "", null, false, "mail.example.com", out string far));
         Assert.Contains("127.0.0.1", far);
         Assert.Contains("Control Deck", far);

         // HTTPS: the host connected to from another machine, "localhost" when
         // bound to every address, the bind address otherwise, and the ACME pair
         // in place of a certificate of the listener's own.
         Assert.Equal("https://mail.example.com:8045/",
            RestSettings.Endpoint("8045", "0.0.0.0", "cert.pem", "key.pem", null, false, "mail.example.com", out _).ToString());
         Assert.Equal("https://localhost:8045/",
            RestSettings.Endpoint("8045", "0.0.0.0", "cert.pem", "key.pem", false, true, "localhost", out _).ToString());
         Assert.Equal("https://192.0.2.10:8045/",
            RestSettings.Endpoint("8045", "192.0.2.10", "cert.pem", "key.pem", false, true, "localhost", out _).ToString());
         Assert.Equal("https://127.0.0.1:8045/",
            RestSettings.Endpoint("8045", "127.0.0.1", "", "", true, true, "localhost", out _).ToString());
      }

      [Fact]
      public void Refusal_SaysWhyInTheServersWordsWhenItGaveSome()
      {
         Assert.Contains("connection refused", RestSettings.Refusal(0, null, false, "connection refused"));
         Assert.Contains("no answer", RestSettings.Refusal(0, null, false));

         Assert.Contains("one-time code", RestSettings.Refusal(401, "{\"error\":\"unauthorized\"}", true));
         Assert.Contains("credential", RestSettings.Refusal(401, "{\"error\":\"unauthorized\"}", false));

         string refused = RestSettings.Refusal(400, "{\"error\":\"snmp_port: 1 to 65535\"}", false);
         Assert.Contains("400", refused);
         Assert.Contains("snmp_port: 1 to 65535", refused);

         Assert.Contains("503", RestSettings.Refusal(503, "not json", false));
      }

      [Fact]
      public void TheGroupsAreTheServersRoutes()
      {
         Assert.Equal("/api/v1/settings", RestSettings.ServerGroup);
         Assert.Equal("/api/v1/settings/openpgp", RestSettings.OpenPgpGroup);
         Assert.Equal("/api/v1/settings/push", RestSettings.PushGroup);
      }

      private static int CountMembers(JsonElement element)
      {
         int count = 0;
         foreach (JsonProperty unused in element.EnumerateObject())
            count++;
         return count;
      }
   }
}
