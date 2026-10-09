// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The questions the Control Panel asks before it deletes, removes, replaces or
   /// releases something, where the question has more to say than its page's one
   /// line: each is one whole sentence of the catalogue per case, which names the
   /// thing acted on and says what follows, and Views/Dialogs.Confirm asks it with a
   /// button named for the act (review findings F185 and F065). A count that changes the
   /// sentence is one sentence per number, never a noun inflected by the code, so
   /// that each language counts its own way.
   /// </summary>
   public static class Confirmations
   {
      /// <summary>
      /// Whether Enter declines a confirmation rather than choosing its action: as the
      /// caller says, or - when it says nothing - whenever the action is destructive,
      /// so that the button that deletes, revokes or replaces something is never the
      /// one a reflexive Enter presses.
      /// </summary>
      public static bool CancelTakesEnter(bool destructive, bool? cancelByDefault)
         => cancelByDefault ?? destructive;

      // ---- Domains & accounts, public folders (F065) ---------------------------------

      public static string DeleteAccount(string address)
         => F("Delete the account {0}? Its mailbox and every message in it are deleted. There is no undo.", address);

      public static string DeleteAlias(string alias)
         => F("Delete the alias {0}? Mail to it is no longer delivered.", alias);

      public static string DeleteDistributionList(string address)
         => F("Delete the distribution list {0}? Mail to it is no longer delivered; the members' mailboxes are not affected.", address);

      /// <summary>
      /// The domain's question, one whole sentence by how many accounts go with it,
      /// where the count was a translated fragment ("its 1 account") put into it.
      /// </summary>
      public static string DeleteDomain(string name, int accounts)
      {
         if (accounts <= 0)
            return F("Delete the domain {0}? It has no accounts; its aliases and distribution lists are deleted with it. There is no undo.", name);

         if (accounts == 1)
            return F("Delete the domain {0}? Its one account, every message stored in it, and the domain's aliases and distribution lists are deleted with it. There is no undo.", name);

         return F("Delete the domain {0}? Its {1} accounts, every message stored in them, and the domain's aliases and distribution lists are deleted with it. There is no undo.", name, accounts);
      }

      public static string DeletePublicFolder(string name)
         => F("Delete the public folder {0} and every message in it?", name);

      // ---- Delivery queue, REST API keys, restarts (F185) -----------------------------

      public static string RemoveQueuedMessage(string id, string from, string recipients)
         => F("Remove message {0} ({1} to {2}) from the queue? It is deleted without a bounce, so its sender is not told.", id, from, recipients);

      public static string RevokeApiKey(string name, bool expired)
         => expired
            ? F("Revoke the API key {0}? It is already expired, so nothing is using it successfully; revoking removes it from this list.", name)
            : F("Revoke the API key {0}? Anything using it stops working on its very next request, and the key cannot be restored - only replaced with a new one.", name);

      /// <summary>
      /// After a save whose settings wait for the service to restart: how many wait,
      /// when the server could say (null when it could not), and that a restart drops
      /// every open session - which is why Later, not Restart now, takes Enter.
      /// </summary>
      public static string RestartAfterSave(int? waiting)
      {
         if (waiting == null)
            return L("Settings saved. They take effect when the hMailServer service restarts, and a restart drops every open session.");

         if (waiting.Value == 1)
            return L("Settings saved. One of them takes effect only when the hMailServer service restarts, and a restart drops every open session.");

         return F("Settings saved. {0} of them take effect only when the hMailServer service restarts, and a restart drops every open session.", waiting.Value);
      }
   }
}
