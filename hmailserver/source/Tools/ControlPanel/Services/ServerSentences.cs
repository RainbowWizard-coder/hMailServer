// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Reflection;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Sentences the SERVER writes that the Control Panel shows in the reader's
   /// language.
   ///
   /// A refusal from the server arrives as the text of a COM error, in English,
   /// because the server has no idea what language the program on the other end is
   /// in. These are marked here, word for word as the server writes them, so that
   /// they are catalogue keys - and <c>L(message)</c> at the place a refusal is
   /// shown then answers the translation when the message is one of them and the
   /// English message, unchanged, when it is anything else. ServerSentencesTests
   /// holds each to the server source, so a sentence changed there fails the test
   /// rather than quietly falling back to English here.
   /// </summary>
   public static class ServerSentences
   {
      /// <summary>PasswordBreachCheck::BreachedSentence.</summary>
      public static readonly string PasswordBreached =
         N("This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.");

      /// <summary>PasswordBreachCheck::UnavailableSentence.</summary>
      public static readonly string PasswordBreachCheckUnavailable =
         N("The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.");

      /// <summary>
      /// The server's words out of an exception from a COM call made by reflection,
      /// which wraps them in a TargetInvocationException whose own message says only
      /// that an invocation failed.
      /// </summary>
      public static string MessageOf(Exception ex)
      {
         Exception real = ex;
         while (real is TargetInvocationException && real.InnerException != null)
            real = real.InnerException;
         return real?.Message ?? "";
      }
   }
}
