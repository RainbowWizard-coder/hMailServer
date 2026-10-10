// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How the Rules page writes a rule's criterion, its action and the two
   /// together, in one pattern each (review finding F309): an action is its verb
   /// and what it acts on - "Forward to someone@example.com", "Move to folder
   /// 'Invoices'", "Set header X-Rule to (empty)" - where the page wrote
   /// "Forward e-mail -> someone@example.com", "Move to IMAP folder 'Invoices'",
   /// "Set header value X-Rule=" and "Reply (subject '...')", three ways for one
   /// list; a criterion is the field, the comparison and the value quoted - "From
   /// contains 'invoice'" - one whole text a language can order; and a rule's
   /// summary is its first criterion and its first action, the column that says
   /// which rule forwards without opening each one.
   /// </summary>
   public static class RulePartText
   {
      // The action types of hMailServer's eRuleActionType, numbered as the server numbers them.
      public const int Forward = 2, Reply = 3, MoveToFolder = 4, RunFunction = 5, SetHeader = 7, SendUsingRoute = 8, BindToAddress = 10;

      /// <summary>A criterion: its field (already in the reader's language), its comparison and the value.</summary>
      public static string Criterion(string field, string match, string value)
         => F("{0} {1} '{2}'", field, match, value ?? "");

      /// <summary>
      /// An action: <paramref name="name"/> is the action's name in the reader's
      /// language, said alone where the action acts on nothing named; the other
      /// arguments are what the action of <paramref name="type"/> acts on.
      /// </summary>
      public static string Action(int type, string name, string to = null, string subject = null, string folder = null,
         string function = null, string header = null, string value = null, int route = 0)
      {
         switch (type)
         {
            case Forward: return F("Forward to {0}", to ?? "");
            case Reply: return F("Reply with the subject '{0}'", subject ?? "");
            case MoveToFolder: return F("Move to folder '{0}'", folder ?? "");
            case RunFunction: return F("Run the script function {0}", function ?? "");
            case SetHeader:
               return string.IsNullOrEmpty(value)
                  ? F("Set header {0} to (empty)", header ?? "")
                  : F("Set header {0} to '{1}'", header ?? "", value);
            case SendUsingRoute: return F("Send using route #{0}", route);
            case BindToAddress: return F("Bind to address {0}", value ?? "");
            default: return name;
         }
      }

      /// <summary>A rule in one line: its first criterion and its first action, either alone where the rule has only it.</summary>
      public static string Summary(string firstCriterion, string firstAction)
      {
         if (string.IsNullOrEmpty(firstCriterion))
            return firstAction ?? "";
         if (string.IsNullOrEmpty(firstAction))
            return firstCriterion;
         return F("{0} → {1}", firstCriterion, firstAction);
      }
   }
}
