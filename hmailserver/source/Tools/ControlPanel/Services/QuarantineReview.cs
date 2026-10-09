// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>How the Quarantine page folds its rows.</summary>
   public enum QuarantineGrouping
   {
      SenderDomain,
      HeloDomain,
      None
   }

   /// <summary>The columns a reviewer can sort the held messages by.</summary>
   public enum QuarantineColumn
   {
      Held,
      Helo,
      Sender,
      Recipients,
      Score,
      Subject
   }

   /// <summary>One held message, copied out of the COM object so that it can be released at once.</summary>
   public sealed class HeldMessage
   {
      public int Id { get; init; }
      public string CreatedTime { get; init; } = "";
      public string Helo { get; init; } = "";
      public string Sender { get; init; } = "";
      public string Recipients { get; init; } = "";
      public int Score { get; init; }
      public string Subject { get; init; } = "";
      public string Reason { get; init; } = "";

      /// <summary>The organizations' domains the server computed (the Public Suffix List's registrable domain), which the groups are keyed by.</summary>
      public string SenderDomain { get; init; } = "";
      public string HeloDomain { get; init; } = "";
   }

   /// <summary>
   /// A row of the Quarantine page's grid: a group's header, or one held message.
   /// Only the check state changes in place - a toggle must not rebuild the grid
   /// under the pointer and throw away the scroll position - so that is the one
   /// property that notifies; everything else is read once when the rows are built.
   /// </summary>
   public sealed class QuarantineRow : INotifyPropertyChanged
   {
      private bool? isChecked_;

      internal QuarantineRow(HeldMessage message, bool indented, DateTime? agesFrom = null)
      {
         Message = message;
         Indented = indented;
         agesFrom_ = agesFrom;
      }

      // The time a held message's age is counted from: this computer's clock where
      // it is the server's, which wrote CreatedTime; null where it is not.
      private readonly DateTime? agesFrom_;

      internal QuarantineRow(string key, IReadOnlyList<HeldMessage> members, bool expanded)
      {
         IsGroup = true;
         GroupKey = key ?? "";
         Members = members;
         IsExpanded = expanded;
      }

      public event PropertyChangedEventHandler PropertyChanged;

      public bool IsGroup { get; }
      public bool IsMessage => !IsGroup;

      /// <summary>The message a message row stands for; null on a group's row.</summary>
      public HeldMessage Message { get; }

      /// <summary>A message row under a group's header, drawn a step in.</summary>
      public bool Indented { get; }

      public string GroupKey { get; } = "";
      public IReadOnlyList<HeldMessage> Members { get; } = Array.Empty<HeldMessage>();
      public bool IsExpanded { get; }

      /// <summary>True, false, or null for a group some of whose messages are checked.</summary>
      public bool? IsChecked
      {
         get => isChecked_;
         internal set
         {
            if (isChecked_ == value)
               return;

            isChecked_ = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsChecked)));
         }
      }

      public int Id => Message?.Id ?? 0;

      public string GroupTitle => IsGroup ? (GroupKey.Length == 0 ? L("(no domain)") : GroupKey) : "";
      public string GroupCount => !IsGroup ? "" : Plural(Members.Count, "{0} message", "{0} messages");

      /// <summary>
      /// When the message was held: how long ago, where the server's clock is this
      /// computer's (review finding F247's rule), and the reader's short date and time
      /// where it is not - the database's raw string, the same in every language, it
      /// was. The tool tip is the whole time, marked as the server's.
      /// </summary>
      public string Held => Message == null ? "" : QuarantineReview.HeldText(Message.CreatedTime, agesFrom_);
      public string HeldTip => Message == null ? null : QuarantineReview.HeldTip(Message.CreatedTime);
      public string Helo => Message?.Helo ?? "";
      public string HeloTip => Message == null ? null : Message.Helo.Length == 0 ? L("No HELO name was recorded") : Message.Helo;

      /// <summary>The null sender is shown as the SMTP conversation wrote it.</summary>
      public string Sender => Message == null ? "" : Message.Sender.Length == 0 ? "<>" : Message.Sender;
      public string SenderTip => Message == null ? null : Sender;

      public string Recipients => Message == null ? "" : QuarantineReview.SummariseRecipients(Message.Recipients);
      public string RecipientsTip => Message == null ? null : string.Join(Environment.NewLine, QuarantineReview.SplitRecipients(Message.Recipients));

      public string Score => Message?.Score.ToString(CultureInfo.CurrentCulture) ?? "";
      public string Subject => Message == null ? "" : Message.Subject.Length == 0 ? L("(no subject)") : Message.Subject;
      public string SubjectTip => Message == null ? null : Subject;

      /// <summary>Why it was held, as the row's tooltip; a group's row has none.</summary>
      public string HeldBecause => Message == null ? null : F("Held because: {0}", Message.Reason);

      public string ReleaseTip => IsGroup ? L("Release this group") : L("Release");
      public string DeleteTip => IsGroup ? L("Delete this group") : L("Delete");
      public string AllowTip => IsGroup ? L("Allow this group's senders, then release") : L("Allow the sender, then release");
      public string BlockTip => IsGroup ? L("Block this group's senders, then delete") : L("Block the sender, then delete");
      public string CheckTip => IsGroup ? F("Check every message from {0}", GroupTitle) : L("Check this message");
      public string FoldTip => IsGroup ? (IsExpanded ? L("Fold this group") : L("Unfold this group")) : null;
      public string HeadersTip => IsGroup ? null : L("Show the headers");
      public string PreviewTip => IsGroup ? null : L("Preview the text");

      // The same tips with the key that does the same from the keyboard, for the
      // tool tips: the rows are one tab stop, and the selected row answers Space,
      // R, Delete, P and H (QuarantineView.WireRowKeys_). The accessible names
      // stay the tips alone; the key is the buttons' AcceleratorKey.
      public string CheckKeyTip => F("{0} ({1})", CheckTip, L(SpaceKey));
      public string ReleaseKeyTip => F("{0} ({1})", ReleaseTip, ReleaseKey);
      public string DeleteKeyTip => F("{0} ({1})", DeleteTip, L(DeleteKey));
      public string PreviewKeyTip => IsGroup ? null : F("{0} ({1})", PreviewTip, PreviewKey);
      public string HeadersKeyTip => IsGroup ? null : F("{0} ({1})", HeadersTip, HeadersKey);

      /// <summary>The keys the quarantine's rows answer, as a keyboard prints them; the two words translated where they are shown.</summary>
      public static readonly string SpaceKey = N("Space");
      public static readonly string DeleteKey = N("Del");
      public const string ReleaseKey = "R"; // no-loc: a key's name
      public const string PreviewKey = "P"; // no-loc: a key's name
      public const string HeadersKey = "H"; // no-loc: a key's name

      /// <summary>
      /// What a screen reader says for the row: the group and its size, or who sent
      /// what and why it was held - the question the page exists to answer, which
      /// was in the row's tool tip alone, where no keyboard or screen reader reaches
      /// (review finding F261).
      /// </summary>
      public string AccessibleName => IsGroup
         ? GroupTitle + ", " + GroupCount
         : Message.Reason.Length == 0
         ? F("{0}, from {1}, score {2}", Subject, Sender, Score)
         : F("{0}, from {1}, score {2}, held because: {3}", Subject, Sender, Score, Message.Reason);

      /// <summary>The same, wherever the row is shown without the grid's row style.</summary>
      public override string ToString() => AccessibleName;
   }

   /// <summary>
   /// What the Quarantine page shows and what its selection is: the held messages,
   /// grouped by the organization's domain of the sender or of the HELO name or not
   /// at all, each group foldable; sorted by a column inside each group; and a set of
   /// checked messages the header's four actions act on. No WPF, so the tests can
   /// hold the grouping, the folding, the three-state checks and the requests it
   /// builds without a window.
   ///
   /// Grouping is not sorting. The groups keep one order - the group holding the most
   /// recent message first, as the list is newest first - and a column sort orders the
   /// messages inside each group. A folded group keeps its checks: checking a group,
   /// or everything, checks the messages it hides too, and the actions act on them.
   /// </summary>
   public sealed class QuarantineReview
   {
      private readonly List<HeldMessage> messages_ = new();
      private readonly HashSet<int> checked_ = new();
      private readonly HashSet<string> folded_ = new(StringComparer.OrdinalIgnoreCase);
      private List<QuarantineRow> rows_ = new();

      public QuarantineGrouping Grouping { get; private set; } = QuarantineGrouping.SenderDomain;

      /// <summary>The column the messages are sorted by, or null for the server's order, newest first.</summary>
      public QuarantineColumn? SortColumn { get; private set; }
      public bool SortDescending { get; private set; }

      public IReadOnlyList<HeldMessage> Messages => messages_;
      public IReadOnlyList<QuarantineRow> Rows => rows_;

      /// <summary>
      /// What the search box holds: a message is shown while its sender, its
      /// recipients, its subject or why it was held contains it, in any case.
      /// Empty shows every message (review finding F261: there was no search).
      /// </summary>
      public string Filter { get; private set; } = "";

      /// <summary>The messages the filter shows, in the server's order.</summary>
      public IReadOnlyList<HeldMessage> Shown => string.IsNullOrWhiteSpace(Filter) ? messages_ : messages_.Where(m => Matches(m, Filter)).ToList();

      /// <summary>The time held messages' ages are counted from, or null where they are not shown (QuarantineRow.Held).</summary>
      public DateTime? AgesFrom { get; set; }

      public void SetFilter(string filter)
      {
         Filter = (filter ?? "").Trim();
         Rebuild();
      }

      /// <summary>Whether a message's sender, recipients, subject or reason contains the text, in any case.</summary>
      public static bool Matches(HeldMessage message, string filter)
      {
         if (message == null)
            return false;

         string wanted = (filter ?? "").Trim();
         if (wanted.Length == 0)
            return true;

         return (message.Sender ?? "").Contains(wanted, StringComparison.OrdinalIgnoreCase)
            || (message.Recipients ?? "").Contains(wanted, StringComparison.OrdinalIgnoreCase)
            || (message.Subject ?? "").Contains(wanted, StringComparison.OrdinalIgnoreCase)
            || (message.Reason ?? "").Contains(wanted, StringComparison.OrdinalIgnoreCase);
      }

      /// <summary>
      /// A held time as the Held column shows it: its age where
      /// <paramref name="agesFrom"/> is the server's clock, which is short enough to
      /// leave the addresses and the subject their room, and the reader's short date
      /// and time where it is not; the text as it came where it is not a time. The
      /// whole time is the tool tip (HeldTip).
      /// </summary>
      public static string HeldText(string createdTime, DateTime? agesFrom)
      {
         DateTime held = HeldAt_(createdTime);
         if (held == DateTime.MinValue)
            return createdTime ?? "";

         return agesFrom is DateTime now ? AgeWords.Ago(now - held) : held.ToString("g", CultureInfo.CurrentCulture);
      }

      /// <summary>The whole held time, marked as the server's.</summary>
      public static string HeldTip(string createdTime)
      {
         DateTime held = HeldAt_(createdTime);
         return held == DateTime.MinValue
            ? createdTime ?? ""
            : F("{0} (server time)", held.ToString("G", CultureInfo.CurrentCulture));
      }

      /// <summary>
      /// The held messages, as the server listed them. Checks on messages that are
      /// still held are kept - a refresh after acting on some must not drop the
      /// reviewer's selection of the rest - and the ones that are gone are dropped.
      /// </summary>
      public void Load(IEnumerable<HeldMessage> messages)
      {
         messages_.Clear();
         messages_.AddRange(messages ?? Enumerable.Empty<HeldMessage>());

         var present = new HashSet<int>(messages_.Select(m => m.Id));
         checked_.RemoveWhere(id => !present.Contains(id));

         Rebuild();
      }

      public void SetGrouping(QuarantineGrouping grouping)
      {
         Grouping = grouping;
         Rebuild();
      }

      /// <summary>
      /// Sorts by a column: the same column again turns the order round. Held and
      /// Score start with the largest, which is what anybody sorting by them wants
      /// first; the text columns start at A.
      /// </summary>
      public void SortBy(QuarantineColumn column)
      {
         if (SortColumn == column)
            SortDescending = !SortDescending;
         else
         {
            SortColumn = column;
            SortDescending = column == QuarantineColumn.Held || column == QuarantineColumn.Score;
         }

         Rebuild();
      }

      public void ToggleFold(QuarantineRow group)
      {
         if (group == null || !group.IsGroup)
            return;

         if (!folded_.Remove(group.GroupKey))
            folded_.Add(group.GroupKey);

         Rebuild();
      }

      /// <summary>
      /// A message's check flips. A group's check sets every one of its messages:
      /// on when any was off, off when all were on - so a group some of whose
      /// messages are checked is checked whole by one click.
      /// </summary>
      public void Toggle(QuarantineRow row)
      {
         if (row == null)
            return;

         if (row.IsGroup)
         {
            bool all = row.Members.Count > 0 && row.Members.All(m => checked_.Contains(m.Id));
            foreach (HeldMessage member in row.Members)
            {
               if (all)
                  checked_.Remove(member.Id);
               else
                  checked_.Add(member.Id);
            }
         }
         else if (!checked_.Remove(row.Id))
         {
            checked_.Add(row.Id);
         }

         RefreshChecks_();
      }

      /// <summary>The header's box: every message on when any was off, all off when all were on.</summary>
      public void ToggleAll()
      {
         IReadOnlyList<HeldMessage> shown = Shown;
         bool all = shown.Count > 0 && shown.All(m => checked_.Contains(m.Id));
         checked_.Clear();

         if (!all)
         {
            foreach (HeldMessage message in shown)
               checked_.Add(message.Id);
         }

         RefreshChecks_();
      }

      /// <summary>The header box's state: true, false, or null when some are checked.</summary>
      public bool? AllChecked => ThreeState_(Shown);

      /// <summary>The checked messages' ids, in the order the page shows them.</summary>
      public IReadOnlyList<int> CheckedIds => Ordered_().Where(m => checked_.Contains(m.Id)).Select(m => m.Id).ToList();

      /// <summary>What a row's own icons act on: the message, or every message of the group.</summary>
      public static IReadOnlyList<int> IdsOf(QuarantineRow row)
      {
         if (row == null)
            return Array.Empty<int>();

         return row.IsGroup ? row.Members.Select(m => m.Id).ToList() : new[] { row.Id };
      }

      public IReadOnlyList<HeldMessage> MessagesWith(IEnumerable<int> ids)
      {
         var wanted = new HashSet<int>(ids ?? Enumerable.Empty<int>());
         return messages_.Where(m => wanted.Contains(m.Id)).ToList();
      }

      /// <summary>The key a message is grouped under: the organization's domain the server gave, lower-cased; empty when it has none.</summary>
      public static string GroupKeyOf(HeldMessage message, QuarantineGrouping grouping)
      {
         if (message == null)
            return "";

         string key = grouping switch
         {
            QuarantineGrouping.SenderDomain => message.SenderDomain,
            QuarantineGrouping.HeloDomain => message.HeloDomain,
            _ => ""
         };

         return (key ?? "").Trim().ToLowerInvariant();
      }

      /// <summary>The envelope recipients, as the store records them: comma separated.</summary>
      public static IReadOnlyList<string> SplitRecipients(string recipients)
      {
         return (recipients ?? "")
            .Split(',')
            .Select(r => r.Trim())
            .Where(r => r.Length > 0)
            .ToList();
      }

      /// <summary>The first recipient, and how many more there are.</summary>
      public static string SummariseRecipients(string recipients)
      {
         IReadOnlyList<string> all = SplitRecipients(recipients);

         if (all.Count == 0)
            return "";

         return all.Count == 1 ? all[0] : F("{0} + {1} more", all[0], all.Count - 1);
      }

      /// <summary>
      /// The body of Quarantine.Bulk and POST /api/v1/quarantine/bulk. action is
      /// release, delete, allow or block; list is address or domain, and domainFrom
      /// sender or helo, for allow and block only.
      /// </summary>
      public static string BulkRequest(IEnumerable<int> ids, string action, string list = null, string domainFrom = null,
                                       bool dryRun = false, bool confirmPublicSuffix = false)
      {
         var body = new Dictionary<string, object>
         {
            ["ids"] = (ids ?? Enumerable.Empty<int>()).Distinct().ToArray(),
            ["action"] = action
         };

         if (!string.IsNullOrEmpty(list))
            body["list"] = list;

         if (!string.IsNullOrEmpty(domainFrom))
            body["domain_from"] = domainFrom;

         if (dryRun)
            body["dry_run"] = true;

         if (confirmPublicSuffix)
            body["confirm_public_suffix"] = true;

         return JsonSerializer.Serialize(body);
      }

      /// <summary>The answer of Quarantine.Bulk, or null when it is not one.</summary>
      public static BulkAnswer ReadBulkAnswer(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;

            if (root.ValueKind != JsonValueKind.Object)
               return null;

            var answer = new BulkAnswer
            {
               DryRun = Bool_(root, "dry_run"),
               Succeeded = Int_(root, "succeeded"),
               Failed = Int_(root, "failed")
            };

            if (root.TryGetProperty("entries", out JsonElement entries) && entries.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement entry in entries.EnumerateArray())
               {
                  answer.Entries.Add(new BulkEntry
                  {
                     Value = Text_(entry, "value"),
                     List = Text_(entry, "list"),
                     MatchOn = Text_(entry, "match_on"),
                     PublicSuffix = Bool_(entry, "public_suffix"),
                     Outcome = Text_(entry, "outcome"),
                     Error = Text_(entry, "error"),
                     Ids = entry.TryGetProperty("ids", out JsonElement ids) && ids.ValueKind == JsonValueKind.Array
                        ? ids.EnumerateArray().Where(i => i.ValueKind == JsonValueKind.Number).Select(i => i.GetInt32()).ToList()
                        : new List<int>()
                  });
               }
            }

            if (root.TryGetProperty("results", out JsonElement results) && results.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement result in results.EnumerateArray())
               {
                  answer.Results.Add(new BulkResult
                  {
                     Id = Int_(result, "id"),
                     Ok = Bool_(result, "ok"),
                     Outcome = Text_(result, "outcome"),
                     Error = Text_(result, "error")
                  });
               }
            }

            return answer;
         }
         catch (JsonException)
         {
            return null;
         }
      }

      private static string Text_(JsonElement element, string name)
      {
         return element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
      }

      private static bool Bool_(JsonElement element, string name)
      {
         return element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.True;
      }

      private static int Int_(JsonElement element, string name)
      {
         return element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) ? number : 0;
      }

      private bool? ThreeState_(IEnumerable<HeldMessage> members)
      {
         int count = 0;
         int on = 0;

         foreach (HeldMessage member in members)
         {
            count++;
            if (checked_.Contains(member.Id))
               on++;
         }

         if (count == 0 || on == 0)
            return false;

         return on == count ? true : null;
      }

      private void RefreshChecks_()
      {
         foreach (QuarantineRow row in rows_)
            row.IsChecked = row.IsGroup ? ThreeState_(row.Members) : checked_.Contains(row.Id);
      }

      /// <summary>The messages sorted as the page shows them, groups apart.</summary>
      private IEnumerable<HeldMessage> Sorted_(IEnumerable<HeldMessage> messages)
      {
         if (SortColumn == null)
            return messages;

         Func<HeldMessage, IComparable> key = SortColumn switch
         {
            QuarantineColumn.Held => m => HeldAt_(m.CreatedTime),
            QuarantineColumn.Helo => m => (m.Helo ?? "").ToLowerInvariant(),
            QuarantineColumn.Sender => m => (m.Sender ?? "").ToLowerInvariant(),
            QuarantineColumn.Recipients => m => (m.Recipients ?? "").ToLowerInvariant(),
            QuarantineColumn.Score => m => m.Score,
            _ => m => (m.Subject ?? "").ToLowerInvariant()
         };

         // Stable, with the id as the last word, so two equal keys never swap places
         // between one rebuild and the next.
         return SortDescending
            ? messages.OrderByDescending(key).ThenByDescending(m => m.Id)
            : messages.OrderBy(key).ThenBy(m => m.Id);
      }

      /// <summary>
      /// When a message was held, as a time: the text is what the database driver
      /// wrote, which is YYYY-MM-DD HH:MM:SS on some engines and the machine's own
      /// date format on others, and neither sorts as text across a month.
      /// </summary>
      private static DateTime HeldAt_(string text)
      {
         // The machine's own format first, since that is the one a driver writes
         // when it writes a local one: 03/04/2026 is April in London.
         if (DateTime.TryParse(text ?? "", CultureInfo.CurrentCulture, DateTimeStyles.AllowWhiteSpaces, out DateTime local))
            return local;

         if (DateTime.TryParse(text ?? "", CultureInfo.InvariantCulture, DateTimeStyles.AllowWhiteSpaces, out DateTime invariant))
            return invariant;

         return DateTime.MinValue;
      }

      private IEnumerable<HeldMessage> Ordered_()
      {
         if (Grouping == QuarantineGrouping.None)
            return Sorted_(Shown);

         return Groups_().SelectMany(g => Sorted_(g.members));
      }

      /// <summary>The groups in the order they are shown: the one whose newest message is newest first.</summary>
      private List<(string key, List<HeldMessage> members)> Groups_()
      {
         var order = new List<string>();
         var members = new Dictionary<string, List<HeldMessage>>(StringComparer.OrdinalIgnoreCase);

         // The server lists newest first, so the order in which the keys are first
         // met is the order of their newest messages.
         foreach (HeldMessage message in Shown)
         {
            string key = GroupKeyOf(message, Grouping);

            if (!members.TryGetValue(key, out List<HeldMessage> list))
            {
               list = new List<HeldMessage>();
               members[key] = list;
               order.Add(key);
            }

            list.Add(message);
         }

         return order.Select(k => (k, members[k])).ToList();
      }

      private void Rebuild()
      {
         var rows = new List<QuarantineRow>();

         if (Grouping == QuarantineGrouping.None)
         {
            rows.AddRange(Sorted_(Shown).Select(m => new QuarantineRow(m, false, AgesFrom)));
         }
         else
         {
            foreach ((string key, List<HeldMessage> members) in Groups_())
            {
               bool expanded = !folded_.Contains(key);
               rows.Add(new QuarantineRow(key, members, expanded));

               if (expanded)
                  rows.AddRange(Sorted_(members).Select(m => new QuarantineRow(m, true, AgesFrom)));
            }
         }

         rows_ = rows;
         RefreshChecks_();
      }
   }

   /// <summary>What Quarantine.Bulk answered.</summary>
   public sealed class BulkAnswer
   {
      public bool DryRun { get; init; }
      public List<BulkEntry> Entries { get; } = new();
      public List<BulkResult> Results { get; } = new();
      public int Succeeded { get; init; }
      public int Failed { get; init; }
   }

   /// <summary>One list entry the request makes, and whether it was written.</summary>
   public sealed class BulkEntry
   {
      public string Value { get; init; } = "";
      public string List { get; init; } = "";
      public string MatchOn { get; init; } = "";
      public bool PublicSuffix { get; init; }
      public string Outcome { get; init; } = "";
      public string Error { get; init; } = "";
      public List<int> Ids { get; init; } = new();
   }

   /// <summary>What became of one message.</summary>
   public sealed class BulkResult
   {
      public int Id { get; init; }
      public bool Ok { get; init; }
      public string Outcome { get; init; } = "";
      public string Error { get; init; } = "";
   }
}
