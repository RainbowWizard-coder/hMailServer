// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One incoming relay as the grid shows it: the relay's database id, which the
   /// page acts on, and its three fields.
   /// </summary>
   internal sealed class IncomingRelayRow
   {
      public int Id { get; init; }
      public string Name { get; init; } = "";
      public string LowerIP { get; init; } = "";
      public string UpperIP { get; init; } = "";

      // The name, then each end of the range with its column's name.
      public override string ToString()
         => RowSpeech.Of(Name, RowSpeech.Labelled(L("Lower IP"), LowerIP), RowSpeech.Labelled(L("Upper IP"), UpperIP));
   }

   /// <summary>
   /// The properties of one incoming relay - its name and the two ends of its
   /// range - saved in place (review finding F221): a wrong address meant
   /// deleting the relay and adding it again. Its own class so that its Alt keys
   /// are its own scope and not the page's. An empty box is said under it; the
   /// server's refusal, and a relay that is not there any more, above the fields.
   /// </summary>
   internal static class IncomingRelayDialog
   {
      /// <summary>The relay's name as saved, or null when nothing was changed.</summary>
      public static string Edit(Window owner, int id)
      {
         var dialog = new FramedDialog { Owner = owner, Title = L("Edit incoming relay") };

         var name = new Wpf.Ui.Controls.TextBox();
         var lower = new Wpf.Ui.Controls.TextBox();
         var upper = new Wpf.Ui.Controls.TextBox();
         var notice = DialogFields.Notice();

         if (!Read_(id, out string readName, out string readLower, out string readUpper, out string failure))
         {
            Dialogs.Show(failure, L("Control Panel"));
            return null;
         }

         name.Text = readName;
         lower.Text = readLower;
         upper.Text = readUpper;

         var body = new StackPanel();
         body.Children.Add(notice);
         FieldRow nameRow = DialogFields.Field(L("_Name"), name);
         FieldRow lowerRow = DialogFields.Field(L("_Lower IP"), lower, L("The first address of the gateway's range."));
         FieldRow upperRow = DialogFields.Field(L("_Upper IP"), upper, L("The last address; the same as the first for one gateway."));
         body.Children.Add(nameRow);
         body.Children.Add(lowerRow);
         body.Children.Add(upperRow);

         string result = null;

         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (_, _) =>
         {
            notice.Hide();
            DialogFields.ClearErrors(nameRow, lowerRow, upperRow);

            // Every empty box is said on this press, the keyboard in the first,
            // in the words the page's add form says them in. The addresses
            // themselves are the server's to judge: its refusal is said above.
            FieldRow first = null;
            foreach ((FieldRow row, Wpf.Ui.Controls.TextBox box, string error) in new[]
            {
               (nameRow, name, L("Enter a name for the relay.")),
               (lowerRow, lower, L("Enter the lower address of the range, such as 10.0.0.0.")),
               (upperRow, upper, L("Enter the upper address of the range, such as 10.0.0.255."))
            })
            {
               if ((box.Text ?? "").Trim().Length > 0)
                  continue;
               row.Error = error;
               first ??= row;
            }

            if (first != null)
            {
               DialogFields.ShowError(first, first.Error);
               return;
            }

            if (Write_(id, name.Text.Trim(), lower.Text.Trim(), upper.Text.Trim(), out string refusal))
            {
               result = name.Text.Trim();
               dialog.DialogResult = true;
               dialog.Close();
               return;
            }

            notice.Show(StatusLevel.Critical, refusal);
         };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => dialog.Close();

         dialog.Frame(L("Edit incoming relay"), body, save, cancel,
            L("Mail that arrives from these addresses is judged by the address before them, so a gateway's own address is not scored or banned as the sender's."),
            width: 460);

         return dialog.ShowDialog() == true ? result : null;
      }

      /// <summary>The relay's three fields, or false and why it could not be read.</summary>
      private static bool Read_(int id, out string name, out string lower, out string upper, out string failure)
      {
         name = lower = upper = failure = null;
         dynamic relays = null;
         dynamic relay = null;
         try
         {
            relays = ServerSession.Current.Application.Settings.IncomingRelays;
            relay = relays.ItemByDBID[id];
            name = (string)relay.Name ?? "";
            lower = (string)relay.LowerIP ?? "";
            upper = (string)relay.UpperIP ?? "";
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failure = F("Could not read the relay: {0}", ServerSession.DescribeComError(ex));
            return false;
         }
         finally
         {
            ServerSession.Release((object)relay);
            ServerSession.Release((object)relays);
         }
      }

      /// <summary>Writes the three fields to the relay with this id; false and the server's refusal when it would not.</summary>
      private static bool Write_(int id, string name, string lower, string upper, out string refusal)
      {
         refusal = null;
         dynamic relays = null;
         dynamic relay = null;
         try
         {
            relays = ServerSession.Current.Application.Settings.IncomingRelays;
            relay = relays.ItemByDBID[id];
            relay.Name = name;
            relay.LowerIP = lower;
            relay.UpperIP = upper;
            relay.Save();
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            refusal = F("Could not save the relay: {0}", ServerSession.DescribeComError(ex));
            return false;
         }
         finally
         {
            ServerSession.Release((object)relay);
            ServerSession.Release((object)relays);
         }
      }
   }
}
