// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading.Tasks;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// This Control Panel's session with the server's REST API, for the settings the
   /// server keeps and answers over REST alone (Services/RestSettings). One per
   /// signed-in server: made on the first page that needs it, and replaced when the
   /// Control Panel signs in again (RestSettingsClient.Session.cs, which ties it to
   /// the server signed in to; this half knows nothing of that, so a test can drive
   /// it against a listener of its own).
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
   /// so the dispatcher's context is never waited on by the task it is running. The
   /// page does wait, so a listener that does not answer is given a few seconds and
   /// no more (ConnectTimeout, RequestTimeout), and a page asks no further group of a
   /// listener that has not answered one.
   /// </summary>
   internal sealed partial class RestSettingsClient
   {
      /// <summary>
      /// One answer: the status (0 when there was none), the body, whether a one-time
      /// code was asked for, and why there was no answer. Refusal, when it is set, is
      /// the whole sentence for a request that was not sent, or not answered, because
      /// the session could not be made - the listener is off, or moved, or the code
      /// was not given - and RefusalFor says it as it is.
      /// </summary>
      public sealed record Reply(int Status, string Body, bool SecondFactorRequired, string Failure, string Refusal = null)
      {
         public bool Ok => Status >= 200 && Status < 300;
      }

      /// <summary>
      /// Where the listener is - or null, and why not - and, on the server itself, the
      /// certificate file it serves (RestSettings.ServedCertificateFile), which a
      /// certificate it presents may be held to.
      /// </summary>
      public sealed record Listener(Uri Endpoint, string Refusal, string ServedCertificateFile);

      /// <summary>
      /// How long a connection, TCP and TLS, may take. A port a firewall drops - which
      /// Windows Firewall does to inbound traffic by default - never refuses, and the
      /// page waits for this on the dispatcher, so it is short: a listener on this
      /// machine or the next one connects in far less.
      /// </summary>
      public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(3);

      /// <summary>
      /// How long a request may take in all. A settings group is read or written in
      /// well under a second; a listener that took the connection and says nothing is
      /// waited for this long, once per page (see the class).
      /// </summary>
      public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

      private readonly Func<string> authorization_;
      private readonly Func<Listener> find_;
      private readonly CookieContainer cookies_ = new();
      private readonly HttpClient http_;
      private Uri endpoint_;
      private volatile string servedCertificateFile_;
      private bool signedIn_;

      // Set when the administrator was asked for a code and gave none: the pages then
      // say so rather than asking again on every page opened. Reload asks again.
      private bool declined_;

      /// <summary>
      /// A client that signs in with what authorization answers (null: not signed in to
      /// a server) at the listener find answers.
      /// </summary>
      internal RestSettingsClient(Func<string> authorization, Func<Listener> find,
         TimeSpan? connectTimeout = null, TimeSpan? requestTimeout = null)
      {
         authorization_ = authorization;
         find_ = find;

         // Redirects are not followed: a password is not sent to wherever a
         // listener points it.
         http_ = new HttpClient(new SocketsHttpHandler
         {
            AllowAutoRedirect = false,
            UseCookies = true,
            CookieContainer = cookies_,
            ConnectTimeout = connectTimeout ?? ConnectTimeout,
            SslOptions = new SslClientAuthenticationOptions
            {
               RemoteCertificateValidationCallback = (sender, certificate, chain, errors)
                  => TrustsServedCertificate(servedCertificateFile_, certificate, errors)
            }
         })
         {
            Timeout = requestTimeout ?? RequestTimeout
         };
         http_.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent", "hMailServer-ControlPanel"); // no-loc
      }

      /// <summary>Where the listener was found, for a sentence; null before Open.</summary>
      public Uri Endpoint => endpoint_;

      /// <summary>
      /// Whether the administrator was asked for the one-time code and gave none,
      /// since Open last asked again: a page whose act asks again (askAgain) can
      /// then say that in its own words, where the sentence Open gives - "Open the
      /// page again to be asked for the code." - is the settings pages'.
      /// </summary>
      public bool CodeDeclined => declined_;

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

         // A session that holds no credential the API takes. "Sign in to a server
         // first." was said here under a Connected pill, on every card that held a
         // REST setting: the shell had a session, and the sentence denied it. The
         // page that has no session at all says that sentence itself.
         string authorization = authorization_?.Invoke();
         if (authorization == null)
         {
            refusal = L("This session holds no password the REST API can sign in with. Sign in again with the administrator's user name and password.");
            return false;
         }

         Listener listener = find_?.Invoke();
         endpoint_ = listener?.Endpoint;
         servedCertificateFile_ = listener?.ServedCertificateFile;
         if (endpoint_ == null)
         {
            refusal = listener?.Refusal ?? RestSettings.Refusal(0, null, false);
            return false;
         }

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
      {
         if (!string.IsNullOrEmpty(reply?.Refusal))
            return reply.Refusal;

         if (reply == null)
            return RestSettings.Refusal(0, null, false);

         return reply.Status == 0 && endpoint_ != null
            ? F("The REST API at {0} could not be reached: {1}", endpoint_, reply.Failure ?? "")
            : RestSettings.Refusal(reply.Status, reply.Body, reply.SecondFactorRequired, reply.Failure);
      }

      /// <summary>
      /// Whether a password may be sent past the certificate a listener presented: one
      /// the platform accepts, or the very certificate in servedFile, read from this
      /// machine's disk (RestSettings.TrustsCertificate). Shared with the Discovery
      /// page, which asks the same listener.
      /// </summary>
      internal static bool TrustsServedCertificate(string servedFile, X509Certificate certificate, SslPolicyErrors errors)
      {
         if (errors == SslPolicyErrors.None)
            return true;

         string served = null;
         if (!string.IsNullOrEmpty(servedFile))
         {
            try
            {
               served = File.ReadAllText(servedFile);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // Not readable from here: nothing to hold the certificate to, so it is
               // refused as the platform refused it.
               served = null;
            }
         }

         return RestSettings.TrustsCertificate(errors, certificate?.GetRawCertData(), served);
      }

      /// <summary>
      /// Why a request had no answer, from what it threw: the innermost message, which
      /// names the cause - a refused certificate, a listener that is not there - unless
      /// the request timed out, when the innermost is the socket's "The I/O operation
      /// has been aborted", which does not say so. A timeout is said in the words that
      /// say it: the TimeoutException's own ("A connection could not be established
      /// within the configured ConnectTimeout"), or, where it only repeats the
      /// cancellation it wraps - HttpClient's own timeout does that - the words of the
      /// exception around it ("... the configured HttpClient.Timeout of 5 seconds
      /// elapsing").
      /// </summary>
      internal static string FailureOf(Exception ex)
      {
         Exception outer = null;
         for (Exception at = ex; at != null; outer = at, at = at.InnerException)
         {
            if (at is TimeoutException)
            {
               bool repeats = at.InnerException != null && string.Equals(at.Message, at.InnerException.Message, StringComparison.Ordinal);
               return repeats && outer != null ? outer.Message : at.Message;
            }
         }

         Exception real = ex;
         while (real.InnerException != null)
            real = real.InnerException;

         return real.Message;
      }

      private Reply Call_(HttpMethod method, string path, string json, Func<string> askForCode)
      {
         // No session - none was made, or the last one ended and could not be made
         // again because the listener had gone or the code was not given: made now,
         // or the reason is the answer. Nothing is sent to no listener at all.
         if (!Open(askForCode, false, out string unopened))
            return new Reply(0, "", false, null, unopened);

         string relative = (path ?? "").TrimStart('/');
         Reply reply = Send_(method, relative, json, null, null);

         // The session ended - its idle time, its lifetime, the administrator's
         // password changed, the service restarted: sign in again and ask once more.
         if (reply.Status == 401)
         {
            signedIn_ = false;
            if (!Open(askForCode, false, out string refusal))
               return new Reply(401, reply.Body, reply.SecondFactorRequired, null, refusal);

            reply = Send_(method, relative, json, null, null);
         }

         // No answer at all: the listener may have moved or stopped, so the next
         // request finds it again rather than asking the old address.
         if (reply.Status == 0)
            signedIn_ = false;

         return reply;
      }

      private Reply Send_(HttpMethod method, string relative, string json, string authorization, string code)
      {
         try
         {
            // Inside the try with the rest: an address that cannot be made is a
            // request with no answer, never an exception out of a page's save.
            Uri target = new Uri(endpoint_, relative);

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
            return new Reply(0, "", false, FailureOf(ex));
         }
      }
   }
}
