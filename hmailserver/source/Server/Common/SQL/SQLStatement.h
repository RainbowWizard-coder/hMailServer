// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DatabaseSettings.h"

namespace HM
{
   class DateTime;
   class SQLCommand;

   class  SQLStatement  
   {
   public:

      enum eColType
      {
         ColTypeUnknown = 0,
         ColTypeValue = 1,
         ColTypeString = 2,
         ColTypeRaw = 4
      };

      enum eStatementType
      {
         STUndefined = 0,
         STInsert = 1,
         STUpdate = 2,
         STDelete = 3,
         STSelect = 4
      };


      // Every AddColumn fills in the fields its kind of column uses and copies the
      // rest as they stand, so the rest start at a value: a string column's iInt
      // was copied uninitialised (the security scan of 25 September 2026).
      struct Column
      {
         String sName;
         
         String sString;
         __int64 iInt = 0;

         eColType iType = ColTypeUnknown;
      };

      SQLStatement();
      SQLStatement(eStatementType iType, const String &tableName);
      virtual ~SQLStatement();

      void AddColumn(const String & sName, const String & sValue);
      void AddColumn(const String & sName, const String & sValue, int iMaxLength);

      void AddColumn(const String & sName, long lValue);
      void AddColumnNULL(const String & sName);
      void AddColumnInt64(const String & sName, __int64 lValue);
      void AddColumnDate(const String & sName, const DateTime &dtValue);
      void AddColumn(const String & sName);
      void AddColumnCommand(const String &column, const String &command);

      void AddWhereClauseColumn(const String &sName, const String &sValue);
      void AddWhereClauseColumn(const String &sName, const AnsiString &sValue);

      void SetAdditionalSQL(const String &additionalSQL) {additional_sql_ = additionalSQL; }

      void SetTable(const String & sName) { table_ = sName; }
      void SetWhereClause(const String & sWhere) { where_ = sWhere; }
      void SetTopRows(int rows);

      void SetStatementType(eStatementType iType) { type_ = iType; }
      void SetIdentityColumn(const String &sIdentityColumn) {identity_column_ = sIdentityColumn; }

      SQLCommand GetCommand() const;
      int GetNoOfCols() const;

      String GenerateFromCommand(const SQLCommand &command);

      // The database's clock as local wall time - GETDATE(), NOW() in the session's
      // time zone, current_timestamp stored into a column without a zone - which is
      // what every date column the server writes today holds. It jumps an hour at a
      // daylight-saving change (ActiveActive.md, C16). Without a type, the
      // configured database's ([Database] Type); with one, that engine's, for a
      // tester on a database of its own.
      static String GetCurrentTimestamp();
      static String GetCurrentTimestamp(DatabaseSettings::SQLDBType type);
      static String GetCurrentTimestampPlusMinutes(int iMinutes);

      // The database's clock in UTC, for every lease, heartbeat and expiry two
      // nodes compare (ActiveActive.md, decision D3), into columns that hold UTC
      // and nothing else; GetCurrentTimestamp and the columns it fills keep their
      // local time. Two nodes whose own clocks disagree read the one clock, and no
      // daylight-saving change moves it.
      //
      //   SQL Server           SYSUTCDATETIME(), a datetime2
      //   MySQL and MariaDB    UTC_TIMESTAMP(6), to the microsecond (since MySQL 5.6.4
      //                        and MariaDB 5.3; the server needs 5.7.9 and 10.2.2)
      //   PostgreSQL           (statement_timestamp() at time zone 'utc'), a timestamp
      //                        without a zone. The statement's start and not now(),
      //                        which is the transaction's: inside a transaction now()
      //                        stands still, and a lease taken there would be shorter
      //                        than it says. The other two engines' functions are the
      //                        statement's time already.
      //   SQL Server Compact   has no UTC function (GETUTCDATE, SYSUTCDATETIME,
      //                        SYSDATETIME and CURRENT_TIMESTAMP all refused, measured
      //                        on 4.0.8876.1 on 12 October 2026), and is one node by
      //                        definition (D2): the node's own UTC clock, to the
      //                        millisecond, as a literal - so the statement is to be
      //                        built when it is run, not kept and run later.
      //
      // Without a type, the configured database's; with one, that engine's.
      static String GetCurrentUtcTimestamp();
      static String GetCurrentUtcTimestamp(DatabaseSettings::SQLDBType type);
      static String GetCurrentUtcTimestampPlusSeconds(int seconds);
      static String GetCurrentUtcTimestampPlusSeconds(DatabaseSettings::SQLDBType type, int seconds);
      static String GetCreateDatabase(std::shared_ptr<DatabaseSettings> pSettings, const String &sDatabaseName);
      static String GetLeftFunction(const String &sParamName, int iLength);
      static String GetTopRows(const String &tableName, int rows);

      static String Escape(const String &input);
      
      static String ConvertWildcardToLike(String input);
      static String ConvertLikeToWildcard(String input);

      // Text a LIKE pattern is to find as it is written, for a LIKE that names
      // ESCAPE '/', the escape character ConvertWildcardToLike's patterns use: the
      // same rows on all four backends, whatever the text holds.
      static String EscapeLikeText(const String &text);

   private:

      static bool IsParameterNameCharacter_(wchar_t character);

      eStatementType type_;

      String identity_column_;
      String where_;
      String table_;
      String additional_sql_;

      int top_rows_;

      std::vector<Column> vecColumns;
      std::vector<Column> where_clause_columns_;
   };

   /*
      Tests SQLStatement::GenerateFromCommand. GenerateFromCommand is only
      *used* by MySQLConnection and PGConnection, but it is a plain
      string-to-string function that does not look at the configured backend, so
      it can be exercised whichever backend the bench is running. The one part
      that does look at the backend is Escape(), so the values used here contain
      no backslashes and the expectations hold on all four backends.

      Called from ClassTester::DoTests, i.e. reachable from the regression suite
      through Utilities.RunTestSuite. Signals failure with "throw 0" the way the
      rest of the in-process testers do, which RunTestSuite turns into a COM
      error - never into a terminate.
   */
   class SQLStatementTester
   {
   public:
      void Test();

      // GetCreateDatabase for hostile names in each dialect: the name quoted as
      // the dialect quotes an identifier, its closing quote doubled. Test runs
      // it; connection-string-tests runs it on Linux, where ClassTester does not.
      void TestCreateDatabase();

      // EscapeLikeText: every wildcard any backend reads written after the escape
      // character, which is doubled, and the text read back whole. Test runs it;
      // quarantine-search-tests runs it on Linux.
      void TestEscapeLikeText();

   private:

      void TestNumberedParameterCollision_();
      void TestExistingPrefixCollisions_();
      void TestParameterNameInsideValue_();
      void TestTypeDispatch_();
      void TestUnknownTokensAreLeftAlone_();
      void TestRepeatedAndCaseSensitiveTokens_();
      void TestGeneratedNamesDoNotCollide_();
      void TestColumnStartsEmpty_();

      static void AssertQuery_(const SQLCommand &command, const String &expected);
   };
}
