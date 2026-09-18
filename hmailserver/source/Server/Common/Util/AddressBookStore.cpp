// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See AddressBookStore.h. Reads and writes go through the same SQLCommand and
// SQLStatement path as every other table, parameterised. The one write that has
// to be atomic against itself - making an account's default book - is taken
// under a process-wide mutex and then reads back whatever won, so two requests
// arriving together for an account that has never been looked at leave one book
// rather than a refusal.

#include "StdAfx.h"
#include "AddressBookStore.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Application/Application.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const char *AddressBookStore::DefaultName = "Contacts";
   const int AddressBookStore::MaximumBooks;
   const int AddressBookStore::MaximumShares;
   const int AddressBookStore::MaximumNameLength;

   namespace
   {
      boost::recursive_mutex book_write_mutex;

      const char *BookColumns = "bookid, bookaccountid, bookname, bookcreated";

      const char *ShareColumns = "shareid, sharebookid, shareaccountid, sharedomainid, sharewrite, sharecreated";

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      __int64 Now()
      {
         return static_cast<__int64>(time(nullptr));
      }

      void ReadBook(std::shared_ptr<DALRecordset> recordset, AddressBookRecord &book)
      {
         book.id = recordset->GetInt64Value("bookid");
         book.accountId = recordset->GetInt64Value("bookaccountid");
         book.name = recordset->GetStringValue("bookname");
         book.created = recordset->GetInt64Value("bookcreated");
      }

      void ReadShare(std::shared_ptr<DALRecordset> recordset, AddressBookShareRecord &share)
      {
         share.id = recordset->GetInt64Value("shareid");
         share.bookId = recordset->GetInt64Value("sharebookid");
         share.accountId = recordset->GetInt64Value("shareaccountid");
         share.domainId = recordset->GetInt64Value("sharedomainid");
         share.write = recordset->GetLongValue("sharewrite") != 0;
         share.created = recordset->GetInt64Value("sharecreated");
      }

      // The account's books as the table holds them, lowest id first - which is
      // the order the default book comes first in. False when the table could
      // not be read, which is not the same as an account with no books.
      bool ReadBooks(__int64 accountId, std::vector<AddressBookRecord> &books)
      {
         books.clear();

         SQLCommand command(String(AnsiString("select ") + BookColumns + " from hm_addressbooks where bookaccountid = @ACCOUNTID order by bookid asc"));
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            AddressBookRecord book;
            ReadBook(recordset, book);
            books.push_back(book);
            recordset->MoveNext();
         }

         return true;
      }

      // A value that no row's account id or domain id can be, for the half of a
      // share lookup that does not apply: sharedomainid is 0 on every account
      // share, so asking for domain 0 would otherwise answer all of them.
      __int64 NoTarget(__int64 value)
      {
         return value > 0 ? value : -1;
      }
   }

   bool
   AddressBookStore::EnsureDefault(__int64 accountId, AddressBookRecord &book)
   {
      std::vector<AddressBookRecord> books;
      if (!ReadBooks(accountId, books))
         return false;

      if (!books.empty())
      {
         book = books[0];
         return true;
      }

      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      // Another thread may have made it between the read above and the lock.
      if (!ReadBooks(accountId, books))
         return false;
      if (!books.empty())
      {
         book = books[0];
         return true;
      }

      AddressBookRecord made;
      made.accountId = accountId;
      made.name = String(DefaultName);
      made.created = Now();

      SQLStatement statement;
      statement.SetTable("hm_addressbooks");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("bookid");
      statement.AddColumnInt64("bookaccountid", accountId);
      statement.AddColumn("bookname", made.name);
      statement.AddColumnInt64("bookcreated", made.created);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
      {
         // The unique index refused it because somebody else's insert landed
         // first. Whatever is there is the default book.
         if (ReadBooks(accountId, books) && !books.empty())
         {
            book = books[0];
            return true;
         }
         return false;
      }

      made.id = id;

      // Every contact the account had before schema 6079, and any row written
      // with no book, is in this one. This is the whole of the migration the
      // upgrade script deliberately does not do, and it runs once per account,
      // the first time anybody asks that account for its books.
      SQLCommand stamp("update hm_contacts set contactbookid = @BOOKID where contactaccountid = @ACCOUNTID and contactbookid = 0");
      stamp.AddParameter("@BOOKID", made.id);
      stamp.AddParameter("@ACCOUNTID", accountId);
      Application::Instance()->GetDBManager()->Execute(stamp);

      book = made;
      return true;
   }

   __int64
   AddressBookStore::DefaultBookId(__int64 accountId)
   {
      AddressBookRecord book;
      if (!EnsureDefault(accountId, book))
         return 0;
      return book.id;
   }

   bool
   AddressBookStore::IsDefault(__int64 accountId, __int64 bookId)
   {
      std::vector<AddressBookRecord> books;
      if (!ReadBooks(accountId, books) || books.empty())
         return false;
      return books[0].id == bookId;
   }

   bool
   AddressBookStore::Get(__int64 accountId, __int64 bookId, AddressBookRecord &book)
   {
      SQLCommand command(String(AnsiString("select ") + BookColumns + " from hm_addressbooks where bookid = @ID and bookaccountid = @ACCOUNTID"));
      command.AddParameter("@ID", bookId);
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadBook(recordset, book);
      return true;
   }

   bool
   AddressBookStore::GetAny(__int64 bookId, AddressBookRecord &book)
   {
      SQLCommand command(String(AnsiString("select ") + BookColumns + " from hm_addressbooks where bookid = @ID"));
      command.AddParameter("@ID", bookId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadBook(recordset, book);
      return true;
   }

   bool
   AddressBookStore::ListBooks(__int64 accountId, std::vector<AddressBookRecord> &books)
   {
      books.clear();

      AddressBookRecord first;
      if (!EnsureDefault(accountId, first))
         return false;

      return ReadBooks(accountId, books);
   }

   bool
   AddressBookStore::ListBooksReadOnly(__int64 accountId, std::vector<AddressBookRecord> &books)
   {
      books.clear();
      return ReadBooks(accountId, books);
   }

   bool
   AddressBookStore::FindByName(__int64 accountId, const String &name, AddressBookRecord &book)
   {
      if (name.IsEmpty() || name.GetLength() > MaximumNameLength)
         return false;

      SQLCommand command(String(AnsiString("select ") + BookColumns + " from hm_addressbooks where bookaccountid = @ACCOUNTID and bookname = @NAME"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NAME", name);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadBook(recordset, book);
      return true;
   }

   bool
   AddressBookStore::IsUsableName(const String &name)
   {
      if (name.IsEmpty() || name.GetLength() > MaximumNameLength)
         return false;

      for (int i = 0; i < name.GetLength(); i++)
      {
         wchar_t c = name.c_str()[i];
         if (c < 0x20 || c == 0x7f)
            return false;
      }

      return true;
   }

   bool
   AddressBookStore::CreateBook(__int64 accountId, const String &name, AddressBookRecord &created)
   {
      if (!IsUsableName(name))
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      // The default book exists before any other, so that the book made here is
      // never the account's lowest id and so never becomes its default.
      AddressBookRecord first;
      if (!EnsureDefault(accountId, first))
         return false;

      std::vector<AddressBookRecord> books;
      if (!ReadBooks(accountId, books))
         return false;
      if ((int) books.size() >= MaximumBooks)
         return false;

      AddressBookRecord taken;
      if (FindByName(accountId, name, taken))
         return false;

      created = AddressBookRecord();
      created.accountId = accountId;
      created.name = name;
      created.created = Now();

      SQLStatement statement;
      statement.SetTable("hm_addressbooks");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("bookid");
      statement.AddColumnInt64("bookaccountid", accountId);
      statement.AddColumn("bookname", created.name);
      statement.AddColumnInt64("bookcreated", created.created);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         return false;

      created.id = id;
      return true;
   }

   bool
   AddressBookStore::RenameBook(__int64 accountId, __int64 bookId, const String &name)
   {
      if (!IsUsableName(name))
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      AddressBookRecord book;
      if (!Get(accountId, bookId, book))
         return false;

      AddressBookRecord taken;
      if (FindByName(accountId, name, taken) && taken.id != bookId)
         return false;

      SQLStatement statement;
      statement.SetTable("hm_addressbooks");
      statement.SetStatementType(SQLStatement::STUpdate);
      statement.AddColumn("bookname", name);
      statement.SetWhereClause("bookid = " + Int64Text(bookId) + " and bookaccountid = " + Int64Text(accountId));

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   AddressBookStore::DeleteBook(__int64 accountId, __int64 bookId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      AddressBookRecord book;
      if (!Get(accountId, bookId, book))
         return false;

      // Whether this is the default book is asked of the read itself rather
      // than of IsDefault, which answers false both for "not the lowest id"
      // and for "the table could not be read": the second of those would
      // delete the default book and the contacts in it on a query that failed
      // between the Get above and here.
      std::vector<AddressBookRecord> books;
      if (!ReadBooks(accountId, books) || books.empty())
         return false;
      if (books[0].id == bookId)
         return false;

      // The contacts in it, then the shares of it, then the book. The foreign
      // key cascades the shares on every backend, and the contacts have no key
      // to cascade through - contactbookid is 0 on an untouched row, and a
      // column that may be 0 cannot reference anything - so both are done here.
      SQLCommand contacts("delete from hm_contacts where contactaccountid = @ACCOUNTID and contactbookid = @BOOKID");
      contacts.AddParameter("@ACCOUNTID", accountId);
      contacts.AddParameter("@BOOKID", bookId);
      if (!Application::Instance()->GetDBManager()->Execute(contacts))
         return false;

      SQLCommand shares("delete from hm_addressbookshares where sharebookid = @BOOKID");
      shares.AddParameter("@BOOKID", bookId);
      if (!Application::Instance()->GetDBManager()->Execute(shares))
         return false;

      SQLCommand row("delete from hm_addressbooks where bookid = @BOOKID and bookaccountid = @ACCOUNTID");
      row.AddParameter("@BOOKID", bookId);
      row.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(row);
   }

   bool
   AddressBookStore::ListShares(__int64 bookId, std::vector<AddressBookShareRecord> &shares)
   {
      shares.clear();

      // Two queries rather than one: the account shares name an address in
      // hm_accounts and the domain shares a name in hm_domains, and an outer
      // join onto both is spelled differently on the four backends.
      SQLCommand accounts(String(AnsiString("select ") + ShareColumns +
         ", accountaddress from hm_addressbookshares inner join hm_accounts on accountid = shareaccountid where sharebookid = @ID order by accountaddress"));
      accounts.AddParameter("@ID", bookId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(accounts);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         AddressBookShareRecord share;
         ReadShare(recordset, share);
         share.target = recordset->GetStringValue("accountaddress");
         shares.push_back(share);
         recordset->MoveNext();
      }

      SQLCommand domains(String(AnsiString("select ") + ShareColumns +
         ", domainname from hm_addressbookshares inner join hm_domains on domainid = sharedomainid where sharebookid = @ID order by domainname"));
      domains.AddParameter("@ID", bookId);

      recordset = Application::Instance()->GetDBManager()->OpenRecordset(domains);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         AddressBookShareRecord share;
         ReadShare(recordset, share);
         share.target = recordset->GetStringValue("domainname");
         shares.push_back(share);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   AddressBookStore::ListSharedWith(__int64 accountId, __int64 domainId,
                                    std::vector<AddressBookRecord> &books,
                                    std::vector<AddressBookShareRecord> &shares)
   {
      books.clear();
      shares.clear();

      SQLCommand command(String(AnsiString("select ") + ShareColumns + ", " + BookColumns +
         " from hm_addressbookshares inner join hm_addressbooks on bookid = sharebookid"
         " where (shareaccountid = @ACCOUNTID or sharedomainid = @DOMAINID) and bookaccountid <> @OWNER"
         " order by bookname asc, bookid asc"));
      command.AddParameter("@ACCOUNTID", NoTarget(accountId));
      command.AddParameter("@DOMAINID", NoTarget(domainId));
      command.AddParameter("@OWNER", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         AddressBookRecord book;
         ReadBook(recordset, book);
         AddressBookShareRecord share;
         ReadShare(recordset, share);

         // A book shared with this account by name AND through its domain is
         // one book, under the wider of the two rights: the person is meant to
         // have been given more, not to be shown the book twice.
         bool merged = false;
         for (size_t i = 0; i < books.size() && !merged; i++)
         {
            if (books[i].id != book.id)
               continue;
            merged = true;
            if (share.write && !shares[i].write)
               shares[i] = share;
         }

         if (!merged)
         {
            books.push_back(book);
            shares.push_back(share);
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   AddressBookStore::ShareOf(__int64 bookId, __int64 accountId, __int64 domainId, AddressBookShareRecord &share)
   {
      SQLCommand command(String(AnsiString("select ") + ShareColumns +
         " from hm_addressbookshares where sharebookid = @ID and shareaccountid = @ACCOUNTID and sharedomainid = @DOMAINID"));
      command.AddParameter("@ID", bookId);
      command.AddParameter("@ACCOUNTID", accountId > 0 ? accountId : 0);
      command.AddParameter("@DOMAINID", domainId > 0 ? domainId : 0);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadShare(recordset, share);
      return true;
   }

   bool
   AddressBookStore::Share(__int64 bookId, __int64 accountId, __int64 domainId, bool write)
   {
      if ((accountId > 0) == (domainId > 0))
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      AddressBookShareRecord existing;
      if (ShareOf(bookId, accountId, domainId, existing))
      {
         SQLStatement statement;
         statement.SetTable("hm_addressbookshares");
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.AddColumnInt64("sharewrite", write ? 1 : 0);
         statement.SetWhereClause("shareid = " + Int64Text(existing.id));
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      std::vector<AddressBookShareRecord> shares;
      if (!ListShares(bookId, shares))
         return false;
      if ((int) shares.size() >= MaximumShares)
         return false;

      SQLStatement statement;
      statement.SetTable("hm_addressbookshares");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("shareid");
      statement.AddColumnInt64("sharebookid", bookId);
      statement.AddColumnInt64("shareaccountid", accountId > 0 ? accountId : 0);
      statement.AddColumnInt64("sharedomainid", domainId > 0 ? domainId : 0);
      statement.AddColumnInt64("sharewrite", write ? 1 : 0);
      statement.AddColumnInt64("sharecreated", Now());

      __int64 id = 0;
      return Application::Instance()->GetDBManager()->Execute(statement, &id) && id > 0;
   }

   bool
   AddressBookStore::Unshare(__int64 bookId, __int64 accountId, __int64 domainId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(book_write_mutex);

      SQLCommand command("delete from hm_addressbookshares where sharebookid = @ID and shareaccountid = @ACCOUNTID and sharedomainid = @DOMAINID");
      command.AddParameter("@ID", bookId);
      command.AddParameter("@ACCOUNTID", accountId > 0 ? accountId : 0);
      command.AddParameter("@DOMAINID", domainId > 0 ? domainId : 0);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   AddressBookStore::UnshareAllForAccount(__int64 accountId)
   {
      SQLCommand command("delete from hm_addressbookshares where shareaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", NoTarget(accountId));
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   AddressBookStore::UnshareAllForDomain(__int64 domainId)
   {
      SQLCommand command("delete from hm_addressbookshares where sharedomainid = @DOMAINID");
      command.AddParameter("@DOMAINID", NoTarget(domainId));
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   AddressBookStore::NameOf(__int64 accountId, __int64 bookId, String &name)
   {
      name.Empty();

      if (bookId <= 0)
         return false;

      AddressBookRecord book;
      if (!Get(accountId, bookId, book))
         return false;

      name = book.name;
      return !name.IsEmpty();
   }

   __int64
   AddressBookStore::IdOfName(__int64 accountId, const String &name)
   {
      AddressBookRecord book;
      if (!FindByName(accountId, name, book))
         return 0;
      return book.id;
   }
}
