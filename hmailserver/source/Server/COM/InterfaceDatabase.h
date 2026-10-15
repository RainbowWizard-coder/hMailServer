// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"

namespace HM
{
   class IniFileSettings;
}

// InterfaceDatabase

class ATL_NO_VTABLE InterfaceDatabase : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceDatabase, &CLSID_Database>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceDatabase, &IID_IInterfaceDatabase, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceDatabase();
   ~InterfaceDatabase();

   bool LoadSettings();

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEDATABASE)


BEGIN_COM_MAP(InterfaceDatabase)
   COM_INTERFACE_ENTRY(IInterfaceDatabase)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()


   DECLARE_PROTECT_FINAL_CONSTRUCT()

   HRESULT FinalConstruct()
   {
      return S_OK;
   }
   
   void FinalRelease() 
   {
   }

public:

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

   STDMETHOD(ExecuteSQL)(BSTR sSQLStatement);
   STDMETHOD(ExecuteSQLWithReturn)(BSTR sSQLStatement, long *uniqueID);
   STDMETHOD(ExecuteSQLScript)(BSTR sFilename);

   STDMETHOD(BeginTransaction)();
   STDMETHOD(CommitTransaction)();
   STDMETHOD(RollbackTransaction)();

   STDMETHOD(get_DatabaseType)(/*[out, retval]*/ eDBtype *pVal);
   STDMETHOD(get_CurrentVersion)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_RequiredVersion)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_RequiresUpgrade)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_IsConnected)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_DatabaseExists)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_ServerName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_DatabaseName)(/*[out, retval]*/ BSTR *pVal);

   STDMETHOD(UtilGetFileNameByMessageID)(hyper lMessageID, BSTR *Output);
   
   STDMETHOD(CreateExternalDatabase)(eDBtype ServerType, BSTR ServerName, long lPort, BSTR DatabaseName, BSTR Username, BSTR Password);
   STDMETHOD(CreateInternalDatabase)();
   STDMETHOD(SetDefaultDatabase)(eDBtype ServerType, BSTR ServerName, long lPort, BSTR DatabaseName, BSTR Username, BSTR Password);

   STDMETHOD(EnsurePrerequisites)(long DBVersion);
   
private:
   HRESULT EnsureDatabaseConnectivity_();

   // Whether the database has ended the script's transaction and gone on in
   // autocommit: so recorded, or so the connection says now
   // (DALConnection::IsTransactionFailed, but for PostgreSQL's, which is aborted
   // and open), which is then recorded with failure, what the statement that
   // ended it failed with.
   bool TransactionEnded_(const HM::String &failure);

   // What a statement of a transaction the database has ended is refused with,
   // and what is added to the error of the statement that ended it.
   HM::String EndedTransactionRefusal_() const;
   static HM::String EndedTransactionAdvice_();

   // The script's transaction given back to the pool, its record cleared.
   std::shared_ptr<HM::DALConnection> TakeTransaction_();

   HM::Configuration *config_;
   HM::IniFileSettings *ini_file_settings_;
   std::shared_ptr<HM::DatabaseConnectionManager> db_manager_;

   // The script's transaction, from BeginTransaction to CommitTransaction or
   // RollbackTransaction: the pool's connection it runs on, and - once the
   // database has ended it at a statement in it that failed - that it has, and
   // what that statement failed with. MySQL, MariaDB and SQL Server then go on in
   // autocommit, so that a statement run after it would be committed on its own:
   // every statement of an ended transaction is refused without being sent, and
   // CommitTransaction rolls back and fails, as DatabaseTransaction refuses
   // everything after a failure. PostgreSQL refuses them by itself
   // (TransactionEnded_).
   std::shared_ptr<HM::DALConnection> conn_;
   bool transaction_ended_;
   HM::String transaction_ended_by_;
};

OBJECT_ENTRY_AUTO(__uuidof(Database), InterfaceDatabase)
