// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// This Control Panel's session with the server's REST API, for the settings the
   /// server keeps and answers over REST alone (Services/RestSettings). One per
   /// signed-in server: made on the first page that needs it, and replaced when the
   /// Control Panel signs in again.
   ///
   /// It signs in once, with the user name and password the Control Panel signed in
   /// with, at POST /api/v1/session, and uses the session's cookie from then on - the
   /// same session the Control Deck makes. That is what lets an administrator with a
   /// second factor use it: the REST API asks for a one-time code with a password,
   /// takes each code once, and asks for none with the cookie, so the code is asked
   /// for once, when the session is made, and not with every request. A request that
   /// changes something carries X-Requested-With: hMailServer, which the server asks
   /// of every write made on a session.
   ///
   /// Synchronous on purpose: the settings pages read and save their COM values
   /// synchronously too, and a page built before its REST values had arrived would
   /// show defaults as if the server held them. The requests run on a pool thread,
   /// so the dispatcher's context is never waited on by the task it is running.
   /// </summary>
   internal sealed class RestSettingsClient
   {
      /// <summary>One answer: the status (0 when there was none), the body, and whether a one-time code was asked for.</summary>
      public sealed record Reply(int Status, string Body, bool SecondFactorRequired, string Failure)
      {
         public bool Ok => Status >= 200 && Status < 300;
      }

      private static readonly object gate_ = new();
      private static RestSettingsClient current_;

      private readonly ServerSession session_;
      private readonly CookieContainer cookies_ = new();
      private readonly HttpClient http_;
      private Uri endpoint_;
      private bool signedIn_;

      // Set when the administrator was asked for a code and gave none: the pages then
      // say so rather than asking again on every page opened. Reload asks again.
      private bool declined_;

      private RestSettingsClient(ServerSession session)
      {
         session_ = session;

         // Redirects are not followed: a password is not sent to wherever a
         // listener points it.
         http_ = new HttpClient(new HttpClientHandler
         {
            AllowAutoRedirect = false,
            UseCookies = true,
            CookieContainer = cookies_
         })
         {
            Timeout = TimeSpan.FromSeconds(15)
         };
         http_.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent", "hMailServer-ControlPanel"); // no-loc
      }

      /// <summary>The client for the server this Control Panel is signed in to.</summary>
      public static RestSettingsClient For(ServerSession session)
      {
         lock (gate_)
         {
            if (current_ == null || !ReferenceEquals(current_.session_, session))
               current_ = new RestSettingsClient(session);
            return current_;
         }
      }

      /// <summary>Where the listener was found, for a sentence; null before Open.</summary>
      public Uri Endpoint => endpoint_;

      /// <summary>
      /// Signs in when there is no session yet. False and the reason when the REST API
      /// cannot be used from here. askForCode is asked for the one-time code when the
      /// administrator has a second factor, and answers null when none was given.
      /// </summary>
      public bool Open(Func<string> askForCode, bool askAgain, out string refusal)
      {
         refusal = null;

         if (askAgain)
            declined_ = false;

         if (signedIn_ && endpoint_ != null)
            return true;

         string authorization = session_?.RestAuthorization();
         if (authorization == null)
         {
            refusal = L("Sign in to a server first.");
            return false;
         }

         endpoint_ = ResolveEndpoint_(out refusal);
         if (endpoint_ == null)
            return false;

         if (declined_)
         {
            refusal = L("The administrator has a second factor, and the REST API asks for its one-time code before it answers. Open the page again to be asked for the code.");
            return false;
         }

         Reply reply = Send_(HttpMethod.Post, "api/v1/session", null, authorization, null); // no-loc
         if (reply.Status == 401 && reply.SecondFactorRequired)
         {
            string code = askForCode?.Invoke();
            if (string.IsNullOrWhiteSpace(code))
            {
               declined_ = true;
               refusal = L("The administrator has a second factor, and the REST API asks for its one-time code before it answers. Open the page again to be asked for the code.");
               return false;
            }

            reply = Send_(HttpMethod.Post, "api/v1/session", null, authorization, code.Trim()); // no-loc
         }

         if (!reply.Ok)
         {
            refusal = RefusalFor(reply);
            return false;
         }

         signedIn_ = true;
         return true;
      }

      /// <summary>GET a settings group. A session the server has ended is made again once.</summary>
      public Reply Get(string path, Func<string> askForCode) => Call_(HttpMethod.Get, path, null, askForCode);

      /// <summary>PUT a settings group's changed keys.</summary>
      public Reply Put(string path, string json, Func<string> askForCode) => Call_(HttpMethod.Put, path, json, askForCode);

      /// <summary>The sentence for a refused answer.</summary>
      public string RefusalFor(Reply reply)
         => reply.Status == 0 && endpoint_ != null
            ? F("The REST API at {0} could not be reached: {1}", endpoint_, reply.Failure ?? "")
            : RestSettings.Refusal(reply.Status, reply.Body, reply.SecondFactorRequired, reply.Failure);

      private Reply Call_(HttpMethod method, string path, string json, Func<string> askForCode)
      {
         string relative = (path ?? "").TrimStart('/');
         Reply reply = Send_(method, relative, json, null, null);

         // The session ended - its idle time, its lifetime, the administrator's
         // password changed, the service restarted: sign in again and ask once more.
         if (reply.Status == 401)
         {
            signedIn_ = false;
            if (!Open(askForCode, false, out string refusal))
               return new Reply(401, reply.Body, reply.SecondFactorRequired, refusal);

            reply = Send_(method, relative, json, null, null);
         }

         // No answer at all: the listener may have moved or stopped, so the next
         // page finds it again rather than asking the old address.
         if (reply.Status == 0)
            signedIn_ = false;

         return reply;
      }

      private Reply Send_(HttpMethod method, string relative, string json, string authorization, string code)
      {
         Uri target = new Uri(endpoint_, relative);

         try
         {
            return Task.Run(async () =>
            {
               using var request = new HttpRequestMessage(method, target);
               request.Headers.TryAddWithoutValidation("X-Requested-With", "hMailServer"); // no-loc
               if (authorization != null)
                  request.Headers.TryAddWithoutValidation("Authorization", authorization); // no-loc
               if (code != null)
                  request.Headers.TryAddWithoutValidation("X-hMailServer-OTP", code); // no-loc
               if (json != null)
                  request.Content = new StringContent(json, Encoding.UTF8, "application/json"); // no-loc

               using HttpResponseMessage response = await http_.SendAsync(request).ConfigureAwait(false);
               string body = await response.Content.ReadAsStringAsync().ConfigureAwait(false);

               // RestApiServer's 401 for an administrator whose second factor is
               // enrolled says so in this header (BuildUnauthorizedResponse_).
               bool secondFactor = response.Headers.TryGetValues("X-hMailServer-OTP", out IEnumerable<string> values) && // no-loc
                                   values.Any(v => string.Equals(v.Trim(), "required", StringComparison.OrdinalIgnoreCase)); // no-loc

               return new Reply((int)response.StatusCode, body, secondFactor, null);
            }).GetAwaiter().GetResult();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // A refused certificate, a listener that is not there, a timeout: the
            // innermost message is the one that names the cause.
            Exception real = ex;
            while (real.InnerException != null)
               real = real.InnerException;

            return new Reply(0, "", false, real.Message);
         }
      }

      /// <summary>
      /// The listener, from its four settings and - on the server itself - whether the
      /// ACME certificate it falls back to is there, as the Discovery page finds it.
      /// </summary>
      private Uri ResolveEndpoint_(out string refusal)
      {
         var ini = new IniFeatureStore();

         bool? acmeCertificate = null;
         if (ServerSession.IsLocalSession)
         {
            string dataFolder = ini.IniPath == null ? "" : ProfileApi.ReadString("Directories", "DataFolder", "", ini.IniPath, 4096); // no-loc
            string acmeFolder = DiscoveryReport.AcmeCertificateFolder(ini.Read("AcmeCertificateDirectory", ""), dataFolder); // no-loc
            acmeCertificate = DiscoveryReport.AcmeCertificateIn(acmeFolder, System.IO.File.Exists);
         }

         return RestSettings.Endpoint(
            ini.Read("RestApiPort", "0"), ini.Read("RestApiBindAddress", "127.0.0.1"), // no-loc
            ini.Read("RestApiCertificateFile", ""), ini.Read("RestApiPrivateKeyFile", ""), // no-loc
            acmeCertificate, ServerSession.IsLocalSession, session_.Host, out refusal);
      }
   }
}
