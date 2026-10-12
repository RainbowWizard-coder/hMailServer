// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once


#define STDCALL __stdcall

typedef void hm_MYSQL;
typedef void hm_MYSQL_RES;
typedef char** hm_MYSQL_ROW;

struct hm_st_mysql_field {
   char *name;                 /* Name of column */
   char *org_name;             /* Original column name, if an alias */ 
   char *table;                /* Table of column if column was a field */
   char *org_table;            /* Org table name, if table was an alias */
   char *db;                   /* Database for table */
   char *catalog;       /* Catalog for table */
   char *def;                  /* Default value (set by mysql_list_fields) */
};

// Subset of the "enum mysql_option" values of MariaDB Connector/C - the client
// hMailServer bundles on Windows and the one it loads first on Linux. The plugin
// directory is how the bundled client finds its authentication plugins
// (caching_sha2_password, ed25519, gssapi, ...).
//
// These are MariaDB's numbers and NOT the Oracle client's: from MYSQL_OPT_USE_REMOTE_
// CONNECTION on, MySQL 8 removed options MariaDB kept, so the same name has another
// number there - 22 is MYSQL_OPT_SSL_CAPATH to libmysqlclient, 21 its MYSQL_OPT_SSL_CA.
// Read from the two packages' own mysql.h (libmariadb-dev 3.4.9 and libmysqlclient-dev
// 8.4.11, Ubuntu 26.04) on 25 September 2026. MySQLInterface::IsMariaDbClient says
// which set applies.
#define HM_MYSQL_PLUGIN_DIR    22
#define HM_MYSQL_DEFAULT_AUTH  23
#define HM_MARIADB_OPT_SSL_VERIFY_SERVER_CERT 21   // my_bool: TLS required and the certificate checked, chain and name
#define HM_MARIADB_OPT_SSL_CA  27                   // const char *: a PEM file of CA certificates
#define HM_MYSQL_OPT_SSL_ENFORCE 38                 // my_bool: TLS when the server offers it (MariaDB's meaning)

// The Oracle client's (libmysqlclient 8.0 and 8.4) TLS options and its enum
// mysql_ssl_mode, which the MariaDB client does not have. Only for an 8.x client:
// 5.x numbered its options as MariaDB does up to 38 (35 was
// MYSQL_SERVER_PUBLIC_KEY there), and 9.x's list is not one this was read from.
#define HM_ORACLE_OPT_SSL_CA   21                   // const char *
#define HM_ORACLE_OPT_SSL_MODE 35                   // unsigned int, one of:
#define HM_ORACLE_SSL_MODE_DISABLED 1
#define HM_ORACLE_SSL_MODE_PREFERRED 2
#define HM_ORACLE_SSL_MODE_REQUIRED 3
#define HM_ORACLE_SSL_MODE_VERIFY_IDENTITY 5

// The client error both clients raise when TLS cannot be set up (CR_SSL_CONNECTION_ERROR).
#define HM_MYSQL_CR_SSL_CONNECTION_ERROR 2026u

typedef hm_MYSQL* STDCALL hm_mysql_real_connect(hm_MYSQL *,const char *,const char *,const char *,const char *, unsigned int,const char *,unsigned long);
typedef void STDCALL hm_mysql_close(hm_MYSQL *);
typedef hm_MYSQL* STDCALL hm_mysql_init(hm_MYSQL *);
typedef int STDCALL hm_mysql_options(hm_MYSQL *mysql, int option, const void *arg);
typedef const char * STDCALL hm_mysql_error(hm_MYSQL *);
typedef int STDCALL hm_mysql_query(hm_MYSQL *mysql, const char *q);
typedef hm_MYSQL_RES * STDCALL hm_mysql_store_result(hm_MYSQL *mysql);
typedef void STDCALL hm_mysql_free_result(hm_MYSQL_RES *result);
typedef unsigned long long STDCALL hm_mysql_insert_id(hm_MYSQL *mysql);
typedef unsigned int STDCALL hm_mysql_errno(hm_MYSQL *mysql);
typedef unsigned long long STDCALL hm_mysql_num_rows(hm_MYSQL_RES *res);
typedef hm_MYSQL_ROW STDCALL hm_mysql_fetch_row(hm_MYSQL_RES *result);
typedef unsigned int STDCALL hm_mysql_num_fields(hm_MYSQL_RES *res);
// The byte length of every column in the current row. Needed because a column value is
// not necessarily a C string: strlen stops at an embedded NUL and truncates the value.
typedef unsigned long * STDCALL hm_mysql_fetch_lengths(hm_MYSQL_RES *res);
typedef hm_st_mysql_field *STDCALL hm_mysql_fetch_field_direct(hm_MYSQL_RES *res, unsigned int fieldnr);
typedef unsigned long STDCALL hm_mysql_get_server_version(hm_MYSQL *mysql);
// The cipher of the connection's TLS session, or null when the connection is not
// encrypted. Both clients export it.
typedef const char * STDCALL hm_mysql_get_ssl_cipher(hm_MYSQL *mysql);
// The client library's version: 30411 for MariaDB Connector/C 3.4.11, 80411 for
// Oracle's 8.4.11. Which option numbers an Oracle client takes depends on it.
typedef unsigned long STDCALL hm_mysql_get_client_version(void);

namespace HM
{
   class MySQLInterface : public Singleton<MySQLInterface>
   {
   public:
      MySQLInterface();
      virtual ~MySQLInterface();
      // One per process (Singleton::Instance), holding the client library's handle,
      // which the destructor releases: a copy would release it a second time.
      MySQLInterface(const MySQLInterface &) = delete;
      MySQLInterface &operator=(const MySQLInterface &) = delete;

      bool Load(String &sErrorMessage);
      bool IsLoaded();

      // Directory that libmysql.dll was loaded from (the hMailServer Bin folder).
      String GetLibraryDirectory();

      // Whether the client loaded is MariaDB Connector/C - it exports
      // mariadb_get_infov, which the Oracle client does not - and so which set of
      // option numbers above it takes. Decided by Load.
      bool IsMariaDbClient() const { return is_mariadb_client_; }

      hm_mysql_real_connect *p_mysql_real_connect;
      hm_mysql_close *p_mysql_close;
      hm_mysql_init *p_mysql_init;
      hm_mysql_options *p_mysql_options;
      hm_mysql_error *p_mysql_error;
      hm_mysql_query *p_mysql_query;
      hm_mysql_store_result *p_mysql_store_result;
      hm_mysql_free_result *p_mysql_free_result;
      hm_mysql_insert_id *p_mysql_insert_id;
      hm_mysql_errno *p_mysql_errno;
      hm_mysql_num_rows *p_mysql_num_rows;
      hm_mysql_fetch_row *p_mysql_fetch_row;
      hm_mysql_num_fields *p_mysql_num_fields;
      hm_mysql_fetch_lengths *p_mysql_fetch_lengths;
      hm_mysql_fetch_field_direct *p_mysql_fetch_field_direct;
      hm_mysql_get_server_version *p_mysql_get_server_version;
      hm_mysql_get_ssl_cipher *p_mysql_get_ssl_cipher;
      hm_mysql_get_client_version *p_mysql_get_client_version;
   private:

      String GetLibraryFileName_();

      HINSTANCE library_instance_;
      bool is_mariadb_client_;
   };
}
