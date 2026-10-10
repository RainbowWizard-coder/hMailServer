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
   /// One SSL certificate's name and its two files, saved in place (review finding
   /// F235): the page edited only the passphrase, so a certificate whose file had
   /// moved - "File missing" - was put right by deleting it and adding it again,
   /// which gave it a new id and left every TCP/IP port that presented the old one
   /// with no certificate, so that it did not start. Saved here, the certificate
   /// keeps its id and every port keeps it. Its own class so that its Alt keys are
   /// its own scope and not the page's, as the incoming relay's Edit is; an empty box
   /// is said under it, the server's refusal and a certificate that is not there any
   /// more above the fields.
   /// </summary>
   internal static class CertificateDialog
   {
      /// <summary>The certificate's name as saved, or null when nothing was changed.</summary>
      public static string Edit(Window owner, int id)
      {
         if (!Read_(id, out string readName, out string readCertificate, out string readKey, out string failure))
         {
            Dialogs.Show(failure, L("Control Panel"));
            return null;
         }

         var dialog = new FramedDialog { Owner = owner, Title = L("Edit certificate") };

         var name = new Wpf.Ui.Controls.TextBox { Text = readName };
         var certificate = new Wpf.Ui.Controls.TextBox { Text = readCertificate };
         var key = new Wpf.Ui.Controls.TextBox { Text = readKey };
         var notice = DialogFields.Notice();

         var body = new StackPanel();
         body.Children.Add(notice);
         FieldRow nameRow = DialogFields.Field(L("_Name"), name);
         FieldRow certificateRow = DialogFields.Field(L("_Certificate file (PEM)"), WithBrowse_(certificate, L("Browse for the certificate file")),
            PfxHint);
         FieldRow keyRow = DialogFields.Field(L("Private key _file (PEM)"), WithBrowse_(key, L("Browse for the private key file")));
         body.Children.Add(nameRow);
         body.Children.Add(certificateRow);
         body.Children.Add(keyRow);

         string result = null;

         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (_, _) =>
         {
            notice.Hide();
            DialogFields.ClearErrors(nameRow, certificateRow, keyRow);

            // Every empty box is said on this press, the keyboard in the first, in
            // the words the page's add form says them in. Whether the files are
            // there is the page's to say after the save, as it says it of every
            // certificate: they may be paths on the server, not on this computer.
            if ((name.Text ?? "").Trim().Length == 0)
               nameRow.Error = L("Enter a name for the certificate.");
            if ((certificate.Text ?? "").Trim().Length == 0)
               certificateRow.Error = L("Enter the path of the certificate file, or choose the file with the … button beside the box.");
            if ((key.Text ?? "").Trim().Length == 0)
               keyRow.Error = L("Enter the path of the private key file, or choose the file with the … button beside the box.");
            if (DialogFields.ShowFirstError(nameRow, certificateRow, keyRow))
               return;

            if (Write_(id, name.Text.Trim(), certificate.Text.Trim(), key.Text.Trim(), out string refusal))
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

         dialog.Frame(L("Edit certificate"), body, save, cancel,
            L("The certificate keeps its place on every TCP/IP port that presents it. Those ports read the files again when the server is next reinitialised."),
            width: 560);
         // Room for the three boxes' messages, which one press can say together,
         // so that Save and Cancel stay where they were pressed (review finding F213).
         dialog.KeepRoomForFieldErrors(3);

         return dialog.ShowDialog() == true ? result : null;
      }

      /// <summary>
      /// What the server reads, and how a certificate in another form is made into
      /// it: the Control Panel's search finds this page for "PFX", and the form said
      /// nothing of one (review finding F235).
      /// </summary>
      public static string PfxHint
         => L("The server reads PEM files. Convert a .pfx or .p12 file first, for example with openssl pkcs12 -in cert.pfx -nokeys -out cert.pem and openssl pkcs12 -in cert.pfx -nocerts -nodes -out key.pem.");

      /// <summary>A path box with the … button beside it that chooses the file, as the page's add form has.</summary>
      private static FrameworkElement WithBrowse_(TextBox box, string browseName)
      {
         var grid = new Grid();
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         grid.Children.Add(box);

         var browse = new Wpf.Ui.Controls.Button { Content = "…", Margin = new Thickness(DesignTokens.Space.Xs, 0, 0, 0) }; // no-loc: the glyph; the name says what it does
         System.Windows.Automation.AutomationProperties.SetName(browse, browseName);
         browse.Click += (_, _) =>
         {
            var picker = new Microsoft.Win32.OpenFileDialog { Filter = L("PEM files (*.pem;*.crt;*.key)|*.pem;*.crt;*.key|All files (*.*)|*.*") };
            if (picker.ShowDialog() == true)
               box.Text = picker.FileName;
         };
         Grid.SetColumn(browse, 1);
         grid.Children.Add(browse);
         return grid;
      }

      /// <summary>The certificate's name and files, or false and why it could not be read.</summary>
      private static bool Read_(int id, out string name, out string certificate, out string key, out string failure)
      {
         name = certificate = key = failure = null;
         bool found = false;
         dynamic certs = null;
         try
         {
            certs = ServerSession.Current.Application.Settings.SSLCertificates;
            int count = (int)certs.Count;
            for (int i = 0; i < count && !found; i++)
            {
               dynamic cert = certs.Item[i];
               try
               {
                  if ((int)cert.ID != id)
                     continue;
                  name = (string)cert.Name ?? "";
                  certificate = (string)cert.CertificateFile ?? "";
                  key = (string)cert.PrivateKeyFile ?? "";
                  found = true;
               }
               finally
               {
                  ServerSession.Release((object)cert);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failure = F("Could not read the certificate: {0}", ServerSession.DescribeComError(ex));
            return false;
         }
         finally
         {
            ServerSession.Release((object)certs);
         }

         if (!found)
            failure = L("The certificate no longer exists - it was deleted after this list was read.");
         return found;
      }

      /// <summary>
      /// Writes the name and the two files to the certificate with this id, found by
      /// its id as the passphrase is, never by its name; false and the server's
      /// refusal when it would not, or when the certificate is not there any more.
      /// </summary>
      private static bool Write_(int id, string name, string certificate, string key, out string refusal)
      {
         refusal = null;
         dynamic certs = null;
         try
         {
            certs = ServerSession.Current.Application.Settings.SSLCertificates;
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic cert = certs.Item[i];
               try
               {
                  if ((int)cert.ID != id)
                     continue;
                  cert.Name = name;
                  cert.CertificateFile = certificate;
                  cert.PrivateKeyFile = key;
                  cert.Save();
                  return true;
               }
               finally
               {
                  ServerSession.Release((object)cert);
               }
            }

            refusal = L("The certificate no longer exists - it was deleted after this list was read.");
            return false;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            refusal = F("Could not save the certificate: {0}", ServerSession.DescribeComError(ex));
            return false;
         }
         finally
         {
            ServerSession.Release((object)certs);
         }
      }
   }
}
