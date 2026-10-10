// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The object dialogs' headings say what is edited (review finding F293): they
   /// read "example.com", "user00@example.com", "Sales" and "IP range", the kind
   /// only in a window title the frame does not show. Each is headed by its
   /// title now, the kind and then the name - "Account - user00@example.com" - and
   /// the IP range by its name once it is read, as the TCP/IP port is.
   /// </summary>
   public class DialogHeadingTests
   {
      [Theory]
      [InlineData("AccountDialog.cs", "Title = F(\"Account - {0}\", address);", "UseFrame(Title, body, save, cancel, width: DialogWidth);")]
      [InlineData("DomainDialog.cs", "Title = F(\"Domain - {0}\", domainName);", "UseFrame(Title, body, save, cancel, width: DialogWidth);")]
      [InlineData("DistributionListDialog.cs", "Title = F(\"Distribution list - {0}\", address);", "UseFrame(Title, body, save, cancel, width: 520);")]
      [InlineData("FolderPermissionsDialog.cs", "Title = F(\"Permissions - {0}\", folderName);", "UseFrame(Title, body, null, close, width: 520);")]
      [InlineData("GroupMembersDialog.cs", "Title = F(\"Members - {0}\", groupName_);", "UseFrame(Title, body, null, close,")]
      [InlineData("MailboxMoveDialog.cs", "Title = F(\"Move or rename {0}\", address_);", "UseFrame(Title, body, move_, cancel,")]
      [InlineData("RouteDialog.cs", "Title = F(\"Route - {0}\", domainName);", "UseFrame(Title, body, save, cancel, width: width);")]
      public void TheHeadingIsTheTitle_TheKindThenTheName(string file, string title, string frame)
      {
         string dialog = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));
         int titled = dialog.IndexOf(title, StringComparison.Ordinal);
         int framed = dialog.IndexOf(frame, StringComparison.Ordinal);
         Assert.True(titled > 0, file + " no longer sets its title as " + title);
         Assert.True(framed > titled, file + " is not headed by the title it sets first.");
      }

      [Fact]
      public void TheIpRangeIsHeadedByItsName_OnceItIsRead()
      {
         string dialog = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "IPRangeDialog.cs"));
         Assert.Contains("frame_ = UseFrame(Title, body, save, cancel, width: width);", dialog, StringComparison.Ordinal);
         int read = dialog.IndexOf("name_.Text = (string)r.Name ?? \"\";", StringComparison.Ordinal);
         int titled = dialog.IndexOf("Title = F(\"IP range - {0}\", name_.Text.Trim());", StringComparison.Ordinal);
         int headed = dialog.IndexOf("frame_.Heading = Title;", StringComparison.Ordinal);
         Assert.True(read > 0 && titled > read && headed > titled, "The IP range dialog does not take its heading from the range it read.");
      }
   }
}
