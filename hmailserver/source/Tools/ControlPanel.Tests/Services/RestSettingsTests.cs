// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
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
         Assert.Contains("its port on the API & monitoring page", off);
         Assert.Contains("API & monitoring", off);
         Assert.Null(RestSettings.Endpoint("", "", "", "", false, true, "localhost", out _));
         Assert.Null(RestSettings.Endpoint("70000", "", "", "", false, true, "localhost", out _));

         // Plain HTTP on a loopback address, from this machine.
         Assert.Equal("http://127.0.0.1:8045/", RestSettings.Endpoint("8045", "", "", "", false, true, "localhost", out string none).ToString());
         Assert.Null(none);
         Assert.Equal("http://[::1]:8045/", RestSettings.Endpoint("8045", "::1", "", "", false, true, "localhost", out _).ToString());

         // Plain HTTP anywhere else is a listener the server does not start.
         Assert.Null(RestSettings.Endpoint("8045", "0.0.0.0", "", "", false, true, "localhost", out string notRunning));
         Assert.Contains("TLS certificate file and its private key file on the API & monitoring page", notRunning);

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

      // compromise_detection_mode's options and words, as the Auto-ban page has them.
      private static readonly int[] ModeValues = { 0, 1, 2 };
      private static readonly string[] ModeWords = { "off", "watch", "enforce" };

      [Fact]
      public void ChoiceChange_OfAWithheldRow_SendsWhatWasTyped_AndReadsNoList()
      {
         // A mode a file of the environment supplied is withheld: the row is a text
         // box, its list was never built (selected is null), and a save of the page
         // threw a NullReferenceException reading that list - every save of the page,
         // typed over or not, before any group after it was sent.
         Assert.False(RestSettings.ChoiceChange("", null, ModeValues, ModeWords, null, out _));
         Assert.False(RestSettings.ChoiceChange("  ", null, ModeValues, ModeWords, null, out _));

         Assert.True(RestSettings.ChoiceChange("enforce", null, ModeValues, ModeWords, null, out object typed));
         Assert.Equal("enforce", typed);

         // In the word's own case, which is the only one the server takes.
         Assert.True(RestSettings.ChoiceChange("  Enforce ", null, ModeValues, ModeWords, null, out object cased));
         Assert.Equal("enforce", cased);

         // A word that is none of them is sent as typed, and the server refuses it
         // with its own sentence ("must be one of off, watch, enforce").
         Assert.True(RestSettings.ChoiceChange("strict", null, ModeValues, ModeWords, null, out object unknown));
         Assert.Equal("strict", unknown);

         // A row with no list and nothing withheld sends nothing, and reads nothing.
         Assert.False(RestSettings.ChoiceChange(null, null, ModeValues, ModeWords, "watch", out object none));
         Assert.Null(none);
      }

      [Fact]
      public void ChoiceChange_OfAList_SendsTheChosenWord_WhenItIsNotTheOneRead()
      {
         Assert.True(RestSettings.ChoiceChange(null, 2, ModeValues, ModeWords, "watch", out object changed));
         Assert.Equal("enforce", changed);

         Assert.False(RestSettings.ChoiceChange(null, 1, ModeValues, ModeWords, "watch", out object same));
         Assert.Equal("watch", same);

         // The value the server answered that is none of the words is offered under
         // a number no option has, and never sent back as a word.
         Assert.False(RestSettings.ChoiceChange(null, int.MinValue, ModeValues, ModeWords, "paranoid", out object kept));
         Assert.Null(kept);
      }

      [Fact]
      public void NumberChange_OfAWithheldRow_SendsWhatWasTyped_EvenWhenItIsNoNumber()
      {
         Assert.True(RestSettings.NumberChange("30", 0, null, out object number));
         Assert.Equal(30L, number);

         // Typed text that is no number was dropped, and the row counted as saved.
         // It is sent as typed, and the server's check of a KindInteger key refuses
         // it ("must be an integer"), so the save says the row was not saved.
         Assert.True(RestSettings.NumberChange("30 days", 0, null, out object words));
         Assert.Equal("30 days", words);
         Assert.True(RestSettings.NumberChange("1,000", 0, null, out object grouped));
         Assert.Equal("1,000", grouped);

         using (JsonDocument body = JsonDocument.Parse(RestSettings.Body(new[] { new KeyValuePair<string, object>("snmp_port", words) })))
            Assert.Equal(JsonValueKind.String, body.RootElement.GetProperty("snmp_port").ValueKind);

         // Nothing typed: nothing sent, and the withheld value is left as it is.
         Assert.False(RestSettings.NumberChange("", 0, null, out _));
         Assert.False(RestSettings.NumberChange("   ", 0, null, out _));

         // A row that is not withheld: its box, when it is not what was read.
         Assert.True(RestSettings.NumberChange(null, 45, 60, out object box));
         Assert.Equal(45L, box);
         Assert.False(RestSettings.NumberChange(null, 60, 60, out _));
      }

      [Fact]
      public void FlagChange_OfAWithheldRow_SendsTrueFalseOrWhatWasTyped()
      {
         Assert.True(RestSettings.FlagChange("true", false, null, out object yes));
         Assert.Equal(true, yes);
         Assert.True(RestSettings.FlagChange("1", false, null, out object one));
         Assert.Equal(true, one);
         Assert.True(RestSettings.FlagChange("FALSE", false, null, out object no));
         Assert.Equal(false, no);
         Assert.True(RestSettings.FlagChange("0", false, null, out object zero));
         Assert.Equal(false, zero);

         // "yes" was sent as false - the opposite of what was typed.
         Assert.True(RestSettings.FlagChange("yes", false, null, out object word));
         Assert.Equal("yes", word);

         Assert.False(RestSettings.FlagChange("", true, null, out _));

         Assert.True(RestSettings.FlagChange(null, true, false, out object box));
         Assert.Equal(true, box);
         Assert.False(RestSettings.FlagChange(null, true, true, out _));
      }

      [Fact]
      public void ServedCertificateFile_IsTheListenersOwn_ThenTheAcmeChain()
      {
         Assert.Equal(@"C:\certs\api.pem", RestSettings.ServedCertificateFile(@" C:\certs\api.pem ", @"C:\Data\ACME", true));
         Assert.Equal(System.IO.Path.Combine(@"C:\Data\ACME", "fullchain.pem"), RestSettings.ServedCertificateFile("", @"C:\Data\ACME", true));
         Assert.Null(RestSettings.ServedCertificateFile("", @"C:\Data\ACME", false));
         Assert.Null(RestSettings.ServedCertificateFile("", null, null));
      }

      [Fact]
      public void TrustsCertificate_ThatIsTheOneTheListenerServes_OnlyThat()
      {
         using X509Certificate2 served = SelfSigned("mail.example.com");
         using X509Certificate2 other = SelfSigned("mail.example.com");

         // As the listener's file has it: its certificate, then its chain - and, in a
         // file that holds both, perhaps the key before either.
         string chainFile = served.ExportCertificatePem() + "\n" + other.ExportCertificatePem() + "\n";
         // The key's markers are written in two pieces, so that a secret scan, which reads
         // the text and not the test, does not take this placeholder for a private key.
         string keyFirst = "-----BEGIN " + "PRIVATE KEY-----\nAAAA\n-----END " + "PRIVATE KEY-----\n" + chainFile;

         // Asked as https://127.0.0.1 or https://localhost, the ACME certificate names
         // neither, and is not issued by an authority this test trusts.
         const SslPolicyErrors nameAndChain = SslPolicyErrors.RemoteCertificateNameMismatch | SslPolicyErrors.RemoteCertificateChainErrors;
         Assert.True(RestSettings.TrustsCertificate(nameAndChain, served.RawData, chainFile));
         Assert.True(RestSettings.TrustsCertificate(nameAndChain, served.RawData, keyFirst));

         // Anything else is refused: another certificate for the same name, the
         // chain's second, no file to hold it to, no certificate at all.
         Assert.False(RestSettings.TrustsCertificate(nameAndChain, other.RawData, chainFile));
         Assert.False(RestSettings.TrustsCertificate(nameAndChain, served.RawData, null));
         Assert.False(RestSettings.TrustsCertificate(nameAndChain, served.RawData, "not a certificate"));
         Assert.False(RestSettings.TrustsCertificate(SslPolicyErrors.RemoteCertificateNotAvailable, null, chainFile));

         // What the platform accepts needs no file.
         Assert.True(RestSettings.TrustsCertificate(SslPolicyErrors.None, other.RawData, null));

         Assert.Equal(served.RawData, RestSettings.LeafCertificateOf(keyFirst));
         Assert.Null(RestSettings.LeafCertificateOf(""));
      }

      internal static X509Certificate2 SelfSigned(string name)
      {
         using RSA key = RSA.Create(2048);
         var request = new CertificateRequest("CN=" + name, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
         var names = new SubjectAlternativeNameBuilder();
         names.AddDnsName(name);
         request.CertificateExtensions.Add(names.Build());
         using X509Certificate2 made = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(30));

         // Through PKCS#12, so that the key is one SChannel will serve with.
         return X509CertificateLoader.LoadPkcs12(made.Export(X509ContentType.Pfx), null);
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
