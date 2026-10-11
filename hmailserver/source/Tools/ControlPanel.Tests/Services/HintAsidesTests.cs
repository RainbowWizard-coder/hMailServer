// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A hint says what its setting does, not how its writer feels about it or what the
   /// project did before (review finding F144): asides and history are words seventeen
   /// translators have to recreate, and they tell the administrator nothing. Directory
   /// authentication's test card began its last sentence "One honest caveat:",
   /// Directory synchronisation called one domain at a time "the sane way to do this the
   /// first time", and the archive's retention said that "until now nothing ever removed
   /// anything from it" and that the administrator who found out was "the one whose disk
   /// filled up"; the Auto-ban tarpit's history of TarpitDelay went with T2.9.
   /// </summary>
   public class HintAsidesTests
   {
      [Theory]
      [InlineData("One honest caveat")]
      [InlineData("the sane way")]
      [InlineData("until now nothing ever removed")]
      [InlineData("the one whose disk filled up")]
      [InlineData("was a stub for a decade")]
      [InlineData("self-inflicted outage")]
      public void NoEnglishText_CarriesTheAside(string aside)
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         var offenders = XDocument.Load(path).Root.Elements("data").Select(d => (string)d.Attribute("name"))
            .Where(text => text.Contains(aside, StringComparison.OrdinalIgnoreCase)).ToList();
         Assert.True(offenders.Count == 0, "These carry an aside or the project's history:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void TheThreeHints_SayWhatTheAsideLeftToSay()
      {
         string ldap = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));
         string sync = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         string server = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));

         Assert.Contains("Note: the test runs from this computer, so a firewall that treats this machine and the hMailServer service differently can make the two disagree.", ldap, StringComparison.Ordinal);
         Assert.Contains("that field is what links a mail domain to a directory. The first time, bring one domain over at a time.", sync, StringComparison.Ordinal);
         // The archive's blurb keeps two sentences (t31c, F311), its history in docs/Protocols.md.
         Assert.Contains("Blurb = L(\"0 keeps everything, and that is the default on purpose:", server, StringComparison.Ordinal);
         Assert.DoesNotContain("until now nothing ever removed anything", server, StringComparison.Ordinal);
      }
   }
}
