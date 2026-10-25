// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Text;
using System.Windows.Forms;

namespace hMailServer.Shared
{
   public partial class formEnterPassword : Form
   {
      private string password;

      public formEnterPassword()
      {
         InitializeComponent();

         password = "";
      }

      public string Password
      {
         get
         {
            return password;
         }

      }

      private void btnOK_Click(object sender, EventArgs e)
      {
         password = textPassword.Text;

         DialogResult = DialogResult.OK;
      }

      private void btnCancel_Click(object sender, EventArgs e)
      {
         DialogResult = DialogResult.Cancel;
      }

      private void formEnterPassword_Load(object sender, EventArgs e)
      {

      }

      private void linkWhatIsThis_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
      {
         // UseShellExecute is required to open a URL in the default browser on .NET Core and later.
         // The project's wiki, where the Control Panel's help links go too: the
         // installer's "Administrator password" step says what this password is.
         // The upstream page this used to open is not this server's documentation.
         Process.Start(new ProcessStartInfo("https://gitlab.com/hmailserver/hmailserver/-/wikis/Installing-hMailServer#the-wizard-page-by-page")
         {
            UseShellExecute = true
         });
      }
   }
}
