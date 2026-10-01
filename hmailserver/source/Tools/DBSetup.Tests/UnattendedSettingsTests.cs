// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using Xunit;

namespace DBSetup.Tests
{
   /// <summary>
   ///    How DBSetup /unattended reads the HM_DB_ variables: what it takes and passes on
   ///    as given, and what it refuses before anything is changed. Up to 6.3.5 it refused a
   ///    single quote or a backslash for PostgreSQL, and for SQL Server a semicolon or a
   ///    quote as a value's first character, because the server wrote the values into its
   ///    connection string as they were. From 6.3.6 the server quotes and escapes them
   ///    (PGConnection::MakeConninfo, ADOConnection::MakeConnectionString), so the setup
   ///    takes them; what it still refuses is what the server or hMailServer.ini cannot
   ///    carry. Negative control: Unattended.cs as 8503a01fe has it refuses every case in
   ///    <see cref="TakenAsGiven" />.
   /// </summary>
   public sealed class UnattendedSettingsTests
   {
      private static UnattendedSettings Read(string type, string host, string user, string password, out string problem,
                                             string name = "hMailServer", string create = "1")
      {
         var variables = new Dictionary<string, string>
         {
            { UnattendedSettings.TypeVariable, type },
            { UnattendedSettings.HostVariable, host },
            { UnattendedSettings.UserVariable, user },
            { UnattendedSettings.PasswordVariable, password },
            { UnattendedSettings.NameVariable, name },
            { UnattendedSettings.CreateVariable, create },
         };
         return UnattendedSettings.Read(variable => variables.TryGetValue(variable, out string value) ? value : null, out problem);
      }

      [Theory]
      [InlineData("postgresql", "db.example", "hm", "it's a back\\slash")]
      [InlineData("postgresql", "db.example", "hm", "x' host='elsewhere.example' password='y")]
      [InlineData("postgresql", "db.example", "o'brien", "p")]
      [InlineData("postgresql", "db\\x", "hm", "p")]
      [InlineData("postgresql", "db.example", "hm", "\\")]
      [InlineData("mssql", "db.example", "sa", "a;Data Source=elsewhere;")]
      [InlineData("mssql", "db.example", "sa", "\"quoted first")]
      [InlineData("mssql", "db.example", "sa", "'quoted first")]
      [InlineData("mssql", "db.example", "sa", "'in quotes'")]
      [InlineData("mssql", "db.example", "'sa", "p")]
      [InlineData("mssql", "db;x", "sa", "p")]
      [InlineData("mssql", "HOST\\INSTANCE", "sa", "pässwörd 日本")]
      [InlineData("mysql", "db.example", "hm", "it's; a \"back\\slash\"")]
      public void TakenAsGiven(string type, string host, string user, string password)
      {
         UnattendedSettings settings = Read(type, host, user, password, out string problem);

         Assert.True(settings != null, "refused: " + problem);
         Assert.Equal(host, settings.Host);
         Assert.Equal(user, settings.User);
         Assert.Equal(password, settings.Password);
      }

      [Theory]
      [InlineData("postgresql", "db.example", "hm", "a\nb", "control character")]
      [InlineData("postgresql", "db.example", "hm", "a\tb", "control character")]
      [InlineData("postgresql", "db.example", "h\rm", "p", "control character")]
      [InlineData("postgresql", "db.example", "'hm'", "p", "same quotation mark")]
      [InlineData("mysql", "\"db\"", "hm", "p", "same quotation mark")]
      [InlineData("mssql", "a\";b", "sa", "p", "double quote beside a single quote or a semicolon")]
      [InlineData("mssql", "a\"'b", "sa", "p", "double quote beside a single quote or a semicolon")]
      public void RefusedBeforeAnythingChanges(string type, string host, string user, string password, string reason)
      {
         UnattendedSettings settings = Read(type, host, user, password, out string problem);

         Assert.Null(settings);
         Assert.Contains(reason, problem);
         // A refusal names the variable, and never repeats a password.
         if (password.Length > 1)
            Assert.DoesNotContain(password, problem, StringComparison.Ordinal);
      }

      [Fact]
      public void ADoubleQuoteInASqlServerHostIsTakenWithoutASingleQuoteOrASemicolon()
      {
         // The server writes it in single quotes, a form ADO reads (ADOConnection::QuoteValue).
         UnattendedSettings settings = Read("mssql", "a\"b", "sa", "p", out string problem);

         Assert.True(settings != null, "refused: " + problem);
         Assert.Equal("a\"b", settings.Host);
      }

      [Fact]
      public void AQuotedHostIsTakenForPostgreSqlOnlyWhenTheQuotesAreNotAPair()
      {
         // One quotation mark is the value's; a pair around it, hMailServer.ini would take off.
         Assert.NotNull(Read("postgresql", "'db", "hm", "p", out _));
         Assert.NotNull(Read("postgresql", "db'", "hm", "p", out _));
         Assert.Null(Read("postgresql", "'db'", "hm", "p", out _));
      }
   }
}
