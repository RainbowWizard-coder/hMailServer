// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    A minimal SCRAM-SHA-256 (RFC 5802 / RFC 7677) client used to exercise the
   ///    server's AUTH SCRAM-SHA-256 implementation over SMTP (RFC 4954) on a raw
   ///    connection that has already completed EHLO.
   /// </summary>
   internal static class SmtpScramTestClient
   {
      /// <summary>
      ///    Runs a full SCRAM-SHA-256 exchange (no SASL-IR). Returns the server's final
      ///    reply line: the 235 on success, or the 5xx rejection on a bad proof.
      /// </summary>
      public static string Authenticate(TcpConnection con, string username, string password)
      {
         var nonceBytes = new byte[18];
         using (var rng = RandomNumberGenerator.Create())
            rng.GetBytes(nonceBytes);
         string clientNonce = Convert.ToBase64String(nonceBytes);

         string clientFirstBare = "n=" + SaslName(username) + ",r=" + clientNonce;
         string clientFirst = "n,," + clientFirstBare;

         // Mechanism selection -> empty 334 challenge.
         con.Send("AUTH SCRAM-SHA-256\r\n");
         string challenge = con.Receive();
         Assert.IsTrue(challenge.StartsWith("334"),
            "Expected an empty 334 challenge. Got: " + challenge);

         // client-first -> server-first.
         con.Send(Base64(clientFirst) + "\r\n");
         string serverFirstLine = con.Receive();
         if (!serverFirstLine.StartsWith("334"))
            return serverFirstLine; // protocol error
         string serverFirst = DecodeChallenge(serverFirstLine);

         string combinedNonce = Attribute(serverFirst, "r");
         byte[] salt = Convert.FromBase64String(Attribute(serverFirst, "s"));
         int iterations = int.Parse(Attribute(serverFirst, "i"));
         Assert.IsTrue(combinedNonce.StartsWith(clientNonce),
            "Server nonce must start with the client nonce. Got: " + combinedNonce);

         byte[] saltedPassword;
         using (var pbkdf2 = new Rfc2898DeriveBytes(password, salt, iterations, HashAlgorithmName.SHA256))
            saltedPassword = pbkdf2.GetBytes(32);

         byte[] clientKey = Hmac(saltedPassword, "Client Key");
         byte[] storedKey = Sha256(clientKey);

         string clientFinalWithoutProof = "c=biws,r=" + combinedNonce;
         string authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;
         byte[] clientSignature = Hmac(storedKey, authMessage);
         byte[] clientProof = Xor(clientKey, clientSignature);

         string clientFinal = clientFinalWithoutProof + ",p=" + Convert.ToBase64String(clientProof);
         con.Send(Base64(clientFinal) + "\r\n");

         string afterFinal = con.Receive();
         if (!afterFinal.StartsWith("334"))
            return afterFinal; // rejected proof (5xx)

         // Verify the server proved knowledge of the key (ServerSignature).
         string serverFinal = DecodeChallenge(afterFinal);
         byte[] serverKey = Hmac(saltedPassword, "Server Key");
         byte[] serverSignature = Hmac(serverKey, authMessage);
         Assert.AreEqual("v=" + Convert.ToBase64String(serverSignature), serverFinal,
            "Server signature (v=) did not verify.");

         // Empty client response acknowledges the server-final; server completes auth.
         con.Send("\r\n");
         return con.Receive();
      }

      /// <summary>
      ///    Runs a full SCRAM-SHA-256-PLUS exchange with tls-server-end-point channel
      ///    binding (no SASL-IR) over an already-EHLO'd TLS connection. Returns the
      ///    server's final reply line: the 235 on success, or the 5xx rejection on a
      ///    bad proof / channel-binding mismatch.
      /// </summary>
      public static string AuthenticatePlus(TcpConnection con, string username, string password, byte[] channelBindingData)
      {
         var nonceBytes = new byte[18];
         using (var rng = RandomNumberGenerator.Create())
            rng.GetBytes(nonceBytes);
         string clientNonce = Convert.ToBase64String(nonceBytes);

         const string gs2Header = "p=tls-server-end-point,,";
         string clientFirstBare = "n=" + SaslName(username) + ",r=" + clientNonce;
         string clientFirst = gs2Header + clientFirstBare;

         // Mechanism selection -> empty 334 challenge.
         con.Send("AUTH SCRAM-SHA-256-PLUS\r\n");
         string challenge = con.Receive();
         if (!challenge.StartsWith("334"))
            return challenge; // rejected (e.g. not on TLS)

         // client-first -> server-first.
         con.Send(Base64(clientFirst) + "\r\n");
         string serverFirstLine = con.Receive();
         if (!serverFirstLine.StartsWith("334"))
            return serverFirstLine; // protocol error
         string serverFirst = DecodeChallenge(serverFirstLine);

         string combinedNonce = Attribute(serverFirst, "r");
         byte[] salt = Convert.FromBase64String(Attribute(serverFirst, "s"));
         int iterations = int.Parse(Attribute(serverFirst, "i"));
         Assert.IsTrue(combinedNonce.StartsWith(clientNonce),
            "Server nonce must start with the client nonce. Got: " + combinedNonce);

         byte[] saltedPassword;
         using (var pbkdf2 = new Rfc2898DeriveBytes(password, salt, iterations, HashAlgorithmName.SHA256))
            saltedPassword = pbkdf2.GetBytes(32);

         byte[] clientKey = Hmac(saltedPassword, "Client Key");
         byte[] storedKey = Sha256(clientKey);

         // c= is base64( gs2-header-bytes || channel-binding-data ).
         byte[] gs2Bytes = Encoding.UTF8.GetBytes(gs2Header);
         var cbindInput = new byte[gs2Bytes.Length + channelBindingData.Length];
         Buffer.BlockCopy(gs2Bytes, 0, cbindInput, 0, gs2Bytes.Length);
         Buffer.BlockCopy(channelBindingData, 0, cbindInput, gs2Bytes.Length, channelBindingData.Length);
         string cbind = Convert.ToBase64String(cbindInput);

         string clientFinalWithoutProof = "c=" + cbind + ",r=" + combinedNonce;
         string authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;
         byte[] clientSignature = Hmac(storedKey, authMessage);
         byte[] clientProof = Xor(clientKey, clientSignature);

         string clientFinal = clientFinalWithoutProof + ",p=" + Convert.ToBase64String(clientProof);
         con.Send(Base64(clientFinal) + "\r\n");

         string afterFinal = con.Receive();
         if (!afterFinal.StartsWith("334"))
            return afterFinal; // rejected proof / channel-binding mismatch (5xx)

         // Verify the server proved knowledge of the key (ServerSignature).
         string serverFinal = DecodeChallenge(afterFinal);
         byte[] serverKey = Hmac(saltedPassword, "Server Key");
         byte[] serverSignature = Hmac(serverKey, authMessage);
         Assert.AreEqual("v=" + Convert.ToBase64String(serverSignature), serverFinal,
            "Server signature (v=) did not verify.");

         // Empty client response acknowledges the server-final; server completes auth.
         con.Send("\r\n");
         return con.Receive();
      }

      private static string SaslName(string name)
      {
         // saslname: '=' must be escaped before ',' so an escaped '=' is not re-encoded.
         return name.Replace("=", "=3D").Replace(",", "=2C");
      }

      private static string Base64(string s)
      {
         return Convert.ToBase64String(Encoding.UTF8.GetBytes(s));
      }

      private static string DecodeChallenge(string line)
      {
         string t = line.Trim();
         int sp = t.IndexOf(' ');
         string b64 = sp >= 0 ? t.Substring(sp + 1).Trim() : "";
         if (b64.Length == 0)
            return "";
         return Encoding.UTF8.GetString(Convert.FromBase64String(b64));
      }

      private static string Attribute(string message, string key)
      {
         string part = message.Split(',').FirstOrDefault(candidate => candidate.StartsWith(key + "="));
         return part == null ? "" : part.Substring(key.Length + 1);
      }

      private static byte[] Hmac(byte[] key, string data)
      {
         using (var h = new HMACSHA256(key))
            return h.ComputeHash(Encoding.ASCII.GetBytes(data));
      }

      private static byte[] Sha256(byte[] data)
      {
         using (var sha = SHA256.Create())
            return sha.ComputeHash(data);
      }

      private static byte[] Xor(byte[] a, byte[] b)
      {
         var r = new byte[a.Length];
         for (int i = 0; i < a.Length; i++)
            r[i] = (byte) (a[i] ^ b[i]);
         return r;
      }
   }
}
