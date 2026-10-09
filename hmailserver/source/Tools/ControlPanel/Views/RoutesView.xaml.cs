// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class RoutesView : UserControl, IPageLifecycle, IRefreshablePage
   {
      public class RouteRow
      {
         public string DomainName { get; set; }
         public string TargetHost { get; set; }
         public int TargetPort { get; set; }
         public int Retries { get; set; }
         public string Auth { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => DomainName + ", " + TargetHost + ":" + TargetPort;
      }

      public RoutesView()
      {
         InitializeComponent();
         hMailServer.ControlPanel.Services.SelectionGate.Bind(RouteGrid, PropertiesButton, DeleteButton);

         // Enter and F2 open the selected route, Delete deletes it after asking,
         // Insert goes to the add form, and Enter in the form adds the route;
         // Escape empties its two boxes and leaves the port (Views/GridKeys, FormSubmit).
         GridKeys.Wire(RouteGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteButton), GridKeys.Focusing(NewDomain));
         FormSubmit.Bind((UIElement)AddRouteButton.Parent, AddRouteButton, NewDomain, NewHost);
      }

      public void OnEnter() => Reload();

      void IRefreshablePage.Refresh() => Reload();

      public void OnLeave()
      {
      }

      private void Reload()
      {
         var rows = new List<RouteRow>();
         try
         {
            dynamic routes = ServerSession.Current.Application.Settings.Routes;
            try
            {
               int count = (int)routes.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic route = routes.Item[i];
                  rows.Add(new RouteRow
                  {
                     DomainName = (string)route.DomainName,
                     TargetHost = (string)route.TargetSMTPHost,
                     TargetPort = (int)route.TargetSMTPPort,
                     Retries = (int)route.NumberOfTries,
                     Auth = (bool)route.RelayerRequiresAuth ? L("Yes") : L("No")
                  });
                  ServerSession.Release(route);
               }
            }
            finally
            {
               ServerSession.Release(routes);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "No routes defined yet." is a
            // fact this page has now: both go, the notice says what happened, with
            // Try again, and Add waits for a read that works (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, RouteGrid, L("Could not read the routes: "), ex, Reload, AddRouteButton);
            return;
         }

         ReadFailure.Clear(Notice, AddRouteButton);
         RouteGrid.ItemsSource = rows;
         ListSearch.Apply(RouteGrid, SearchBar.SearchText);
         StatusText.Show(EmptyStatus, null, rows.Count, null, L("No routes defined yet."));
      }

      private void Search_TextChanged(object sender, EventArgs e)
         => ListSearch.Apply(RouteGrid, SearchBar.SearchText);

      private void Add_Click(object sender, RoutedEventArgs e)
      {
         string domain = NewDomain.Text.Trim();
         string host = NewHost.Text.Trim();

         if (domain.Length == 0 || host.Length == 0)
         {
            MessageBox.Show(L("Domain and target host are required."), L("Control Panel"));
            return;
         }

         if (!int.TryParse(NewPort.Text.Trim(), out int port))
            port = 25;

         dynamic routes = ServerSession.Current.Application.Settings.Routes;
         try
         {
            dynamic route = routes.Add();
            route.DomainName = domain;
            route.TargetSMTPHost = host;
            route.TargetSMTPPort = port;
            route.NumberOfTries = 3;
            route.MinutesBetweenTry = 10;
            route.Save();
            ServerSession.Release(route);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not add the route: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(routes);
         }

         NewDomain.Text = NewHost.Text = "";
         Reload();
      }

      private void Edit_Click(object sender, RoutedEventArgs e)
      {
         if (RouteGrid.SelectedItem is not RouteRow row)
         {
            MessageBox.Show(L("Select a route first."), L("Control Panel"));
            return;
         }

         new RouteDialog(Window.GetWindow(this), row.DomainName).ShowDialog();
         Reload();
      }

      private void Delete_Click(object sender, RoutedEventArgs e)
      {
         if (RouteGrid.SelectedItem is not RouteRow row)
            return;

         if (!MessageBox.Confirm(F("Delete the route for {0}?", row.DomainName), L("Control Panel"), L("_Delete route"), destructive: true))
            return;

         dynamic routes = ServerSession.Current.Application.Settings.Routes;
         try
         {
            int count = (int)routes.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic route = routes.Item[i];
               if ((string)route.DomainName == row.DomainName)
               {
                  route.Delete();
                  ServerSession.Release(route);
                  break;
               }
               ServerSession.Release(route);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the route: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(routes);
         }

         Reload();
      }
   }
}
