// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>How much attention a value is asking for.</summary>
   public enum StatusLevel
   {
      /// <summary>Nothing to say. Renders with no badge at all.</summary>
      Normal,
      Good,
      Information,
      Warning,
      Critical
   }

   /// <summary>
   /// The three things a status has to carry so that none of them is load-bearing
   /// on its own: a colour token, a shape, and a word.
   /// </summary>
   public sealed class StatusPresentation
   {
      internal StatusPresentation(StatusLevel level, string severityWord, ShapeMark shape, string brushKey,
         string fillBrushKey, string fillTextBrushKey)
      {
         Level = level;
         severityWord_ = severityWord;
         Shape = shape;
         BrushKey = brushKey;
         FillBrushKey = fillBrushKey;
         FillTextBrushKey = fillTextBrushKey;
      }

      // The word in English, the catalogue's key: translated each time it is
      // read (review finding F006). The presentations are made once, in a
      // static initialiser, and a word translated there kept the language in
      // force when the type was first touched - the render harness touched it
      // before it put German in force and every notice read "Warning" before a
      // German sentence; nothing in the product guaranteed the order either.
      private readonly string severityWord_;

      public StatusLevel Level { get; }

      /// <summary>
      /// "Warning", "Critical", ... in the language in force when it is read -
      /// prefixed to the caller's own text in the accessible name, so a screen
      /// reader hears the severity even though the visible badge only shows the
      /// shape and the caller's wording.
      /// </summary>
      public string SeverityWord => L(severityWord_);

      /// <summary>The severity word in English, whatever the language in force: the catalogue's key.</summary>
      public string SeverityWordEnglish => severityWord_;

      /// <summary>
      /// The shape drawn beside the value. This is the channel that survives when
      /// the colour does not: printed in greyscale, viewed by a reader with
      /// deuteranopia, or squashed onto the two or three colours a High Contrast
      /// theme allows.
      /// </summary>
      public ShapeMark Shape { get; }

      /// <summary>
      /// Application resource key of the brush, e.g. "AppWarningBrush". A key
      /// rather than a brush because ThemeTokens republishes these on every theme
      /// change, so anything holding a resolved brush would keep the old colour.
      /// </summary>
      public string BrushKey { get; }

      /// <summary>
      /// Application resource key of the fill a status pill draws its box with,
      /// e.g. "AppWarningFillBrush" (<see cref="DesignTokens.StatusFill"/>).
      /// Normal takes the neutral fill.
      /// </summary>
      public string FillBrushKey { get; }

      /// <summary>
      /// Application resource key of the colour of the word and the mark on
      /// that fill, e.g. "AppWarningFillTextBrush" - a shade of the level's
      /// colour chosen to clear 4.5:1 against the fill.
      /// </summary>
      public string FillTextBrushKey { get; }
   }

   /// <summary>
   /// Maps a status level onto the colour token, the shape and the word that
   /// present it.
   ///
   /// The dashboard used to turn the queue counter amber above a hundred messages
   /// and leave it at that: the entire message was the colour, so it did not exist
   /// for anyone who could not see the colour, and the threshold was a bare
   /// literal in the middle of a refresh method. Both problems are fixed by having
   /// exactly one place that decides what a level looks like and what counts as
   /// which level.
   ///
   /// Every level gets a distinct shape - that invariant is what a test can hold
   /// on to, and it is the whole reason the shape is worth having.
   /// </summary>
   public static class StatusSemantics
   {
      /// <summary>
      /// Queue length at which the dashboard starts calling the queue a backlog.
      /// The number is the one the dashboard has always used; naming it here is
      /// what makes it testable and what stops the next reader guessing whether
      /// the comparison was inclusive.
      /// </summary>
      public const int QueueBacklogThreshold = 100;

      /// <summary>
      /// Days before expiry at which a certificate starts reading as a warning.
      /// Thirty, because that is the point at which ACME clients (including this
      /// server's own AcmeRenewalTask) renew: a manually managed certificate inside
      /// that window is later than the automation would be.
      ///
      /// It lives here rather than with the certificate code because two pages now
      /// decide what to say about an expiry date - the certificates page and the
      /// transport encryption overview - and two pages disagreeing about when a
      /// certificate becomes urgent is worse than neither of them saying so.
      /// <see cref="CertificateInspector.ExpiryWarningDays"/> is this number; so is
      /// TlsPosture.ExpiryWarningDays. This is also the only one of the three that a
      /// test can reach without dragging COM and System.ServiceProcess into the test
      /// assembly, which is the practical reason it moved.
      /// </summary>
      public const int CertificateExpiryWarningDays = 30;

      private static readonly Dictionary<StatusLevel, StatusPresentation> Map = new()
      {
         [StatusLevel.Normal] =
            new StatusPresentation(StatusLevel.Normal, N("Normal"), ShapeMark.Circle, "TextFillColorPrimaryBrush",
               "AppNeutralFillBrush", "AppNeutralFillTextBrush"),
         [StatusLevel.Good] =
            new StatusPresentation(StatusLevel.Good, N("OK"), ShapeMark.Square, "AppSuccessBrush",
               "AppSuccessFillBrush", "AppSuccessFillTextBrush"),
         [StatusLevel.Information] =
            new StatusPresentation(StatusLevel.Information, N("Information"), ShapeMark.Diamond, "AppInfoBrush",
               "AppInfoFillBrush", "AppInfoFillTextBrush"),
         [StatusLevel.Warning] =
            new StatusPresentation(StatusLevel.Warning, N("Warning"), ShapeMark.Triangle, "AppWarningBrush",
               "AppWarningFillBrush", "AppWarningFillTextBrush"),
         [StatusLevel.Critical] =
            new StatusPresentation(StatusLevel.Critical, N("Critical"), ShapeMark.Cross, "AppDangerBrush",
               "AppDangerFillBrush", "AppDangerFillTextBrush")
      };

      /// <summary>Never returns null; an unknown level is treated as Normal.</summary>
      public static StatusPresentation For(StatusLevel level)
      {
         return Map.TryGetValue(level, out StatusPresentation presentation) ? presentation : Map[StatusLevel.Normal];
      }

      /// <summary>Every level, in increasing severity. Only used by the tests today.</summary>
      public static IReadOnlyList<StatusLevel> AllLevels { get; } = new[]
      {
         StatusLevel.Normal,
         StatusLevel.Good,
         StatusLevel.Information,
         StatusLevel.Warning,
         StatusLevel.Critical
      };

      /// <summary>
      /// Whether a queue of this length is a backlog. Strictly greater than the
      /// threshold, matching the behaviour the dashboard has always had: a queue
      /// sitting on exactly a hundred messages is busy, not broken.
      /// </summary>
      public static StatusLevel ForQueueLength(int queueLength, int threshold = QueueBacklogThreshold)
      {
         return queueLength > threshold ? StatusLevel.Warning : StatusLevel.Normal;
      }
   }
}
