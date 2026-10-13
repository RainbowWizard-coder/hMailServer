#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What the database engines do with the statements docs/ActiveActive.md builds on.

Run INSIDE the active-active lab's node image (aa-lab.sh sqlcheck <db>), on the lab's
back network, against one engine at a time. It speaks to each engine through the
same client library the server uses and in the same calls:

  postgresql     libpq.so.5: PQexec, then PQcmdTuples - PGConnection's calls
  mysql/mariadb  libmariadb.so.3: mysql_options (TLS enforced, the certificate not
                 checked, as TlsMode=prefer sets them), mysql_real_connect,
                 mysql_query, then mysql_affected_rows - MySQLConnection's calls
  mssql          python-tds over TDS: the row count is the DONE token's count,
                 which is what ADO hands back as Command.Execute's RecordsAffected
                 (the server's ADOConnection is Windows-only and cannot run here)

What it checks, each against what the document says:

  counts   an UPDATE that matches no row, one row, one row whose value it does not
           change, two rows; an INSERT; a DELETE. The count of the one-row update
           that changes nothing is the difference between the engines: MySQL and
           MariaDB answer 0 unless the connection asked for CLIENT_FOUND_ROWS, and
           1 with it, as PostgreSQL and SQL Server do. And a claim that recognises
           itself (... and (owner = 0 or token = @epoch)) run a second time, as the
           database layer runs a statement again after a reconnect: 1 - and 0 on
           MySQL without CLIENT_FOUND_ROWS, where it changes nothing the second
           time - while the same claim without its own epoch counts 0 and another
           epoch 0. On SQL Server also NOCOUNT: no count under SET NOCOUNT ON, which
           is why the server's sessions turn it off.
  claims   two connections race to claim the same rows with a conditional UPDATE
           (... where id = @ID and owner = 0) and count their wins by the row
           count. Every row must be won exactly once. On SQL Server, again with both
           sessions at READ UNCOMMITTED, as the server's run.
  lease    a claim held until the database's clock plus two seconds is refused to
           the other connection before then and taken by it after; and the same on
           the database's UTC clock.
  named    two connections race to take named leases whose rows do not exist yet,
           by insert-or-claim: the claim, which counts 0 while there is no row,
           then the insert, which the unique name lets one of them win. Every name
           won exactly once. On SQL Server at READ UNCOMMITTED as well.
  uids     two writers put rows into one "folder", each taking the next number from
           the folder's counter, while a reader does what an IMAP session's
           Messages::Refresh does: reads the rows above the highest number it has
           seen. Done twice: as the server does it today (the counter raised, read
           back and the row inserted, three statements on their own), and in one
           transaction. Counted: numbers given to two rows, and rows the reader
           never saw because they were committed below a number it had already
           passed - a message a client would never fetch.
  rollback the transaction again, with every fourth one rolled back, so that a
           number is given back and given to the next row. Counted: rows the
           reader saw that were never committed, numbers it saw on one row that
           ended on another, and rows it never saw. At each engine's own isolation
           none of the three. On SQL Server the reader also at READ UNCOMMITTED, as
           the server's sessions run: the negative control, which sees rows that
           are rolled back; and at READ UNCOMMITTED with the READCOMMITTED table
           hint, which sees none (ActiveActive.md, decision D19).
  dst      a lease on the database's local clock and one on its UTC clock across a
           daylight-saving change: only against a database whose zone has one and
           whose clock libfaketime has put a minute before it (aa-lab.sh dst, which
           says which change in AA_DST_SEASON, autumn or spring). The UTC lease is
           held for its length and no longer; the local one is taken an hour early
           in spring and held an hour late in autumn (the negative control, C16).

Credentials come from the environment by name, never from the command line:
AA_DB_PASSWORD. Prints one JSON line per check and a summary; exits 0 when every
expectation held, 1 otherwise, 2 when the engine could not be reached.
"""

import ctypes
import json
import os
import random
import sys
import threading
import time


class Failure(Exception):
    pass


# ------------------------------------------------------------------ the clients

class PgConnection:
    dialect = "postgresql"

    def __init__(self, host, port, user, database):
        self.lib = ctypes.CDLL("libpq.so.5")
        lib = self.lib
        lib.PQconnectdb.restype = ctypes.c_void_p
        lib.PQconnectdb.argtypes = [ctypes.c_char_p]
        lib.PQstatus.restype = ctypes.c_int
        lib.PQstatus.argtypes = [ctypes.c_void_p]
        lib.PQerrorMessage.restype = ctypes.c_char_p
        lib.PQerrorMessage.argtypes = [ctypes.c_void_p]
        lib.PQexec.restype = ctypes.c_void_p
        lib.PQexec.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.PQresultStatus.restype = ctypes.c_int
        lib.PQresultStatus.argtypes = [ctypes.c_void_p]
        lib.PQresultErrorMessage.restype = ctypes.c_char_p
        lib.PQresultErrorMessage.argtypes = [ctypes.c_void_p]
        lib.PQcmdTuples.restype = ctypes.c_char_p
        lib.PQcmdTuples.argtypes = [ctypes.c_void_p]
        lib.PQntuples.restype = ctypes.c_int
        lib.PQntuples.argtypes = [ctypes.c_void_p]
        lib.PQnfields.restype = ctypes.c_int
        lib.PQnfields.argtypes = [ctypes.c_void_p]
        lib.PQgetvalue.restype = ctypes.c_char_p
        lib.PQgetvalue.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.PQclear.argtypes = [ctypes.c_void_p]
        lib.PQfinish.argtypes = [ctypes.c_void_p]
        # The password is read by libpq from PGPASSWORD, so it is in no string here.
        os.environ["PGPASSWORD"] = os.environ.get("AA_DB_PASSWORD", "")
        info = "host=%s port=%d user=%s dbname=%s sslmode=prefer connect_timeout=10" % (host, port, user, database)
        self.conn = lib.PQconnectdb(info.encode())
        if not self.conn or lib.PQstatus(self.conn) != 0:
            raise Failure("PostgreSQL: " + (lib.PQerrorMessage(self.conn) or b"no connection").decode(errors="replace").strip())

    def _run(self, sql):
        res = self.lib.PQexec(self.conn, sql.encode())
        if not res:
            raise Failure("PQexec returned nothing: " + self.lib.PQerrorMessage(self.conn).decode(errors="replace"))
        status = self.lib.PQresultStatus(res)
        if status not in (1, 2):  # PGRES_COMMAND_OK, PGRES_TUPLES_OK
            message = self.lib.PQresultErrorMessage(res).decode(errors="replace").strip()
            self.lib.PQclear(res)
            raise Failure("%s: %s" % (sql[:80], message))
        return res, status

    def execute(self, sql):
        res, _ = self._run(sql)
        text = self.lib.PQcmdTuples(res).decode()
        self.lib.PQclear(res)
        return int(text) if text else None

    def query(self, sql):
        res, _ = self._run(sql)
        rows = []
        for r in range(self.lib.PQntuples(res)):
            rows.append(tuple((self.lib.PQgetvalue(res, r, c) or b"").decode() for c in range(self.lib.PQnfields(res))))
        self.lib.PQclear(res)
        return rows

    def close(self):
        self.lib.PQfinish(self.conn)


class MyConnection:
    CLIENT_FOUND_ROWS = 2
    OPT_SSL_VERIFY_SERVER_CERT = 21
    OPT_PLUGIN_DIR = 22
    OPT_SSL_ENFORCE = 38

    def __init__(self, dialect, host, port, user, database, found_rows):
        self.dialect = dialect
        self.lib = ctypes.CDLL("libmariadb.so.3")
        lib = self.lib
        lib.mysql_init.restype = ctypes.c_void_p
        lib.mysql_init.argtypes = [ctypes.c_void_p]
        lib.mysql_options.restype = ctypes.c_int
        lib.mysql_options.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        lib.mysql_real_connect.restype = ctypes.c_void_p
        lib.mysql_real_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                           ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_ulong]
        lib.mysql_query.restype = ctypes.c_int
        lib.mysql_query.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.mysql_affected_rows.restype = ctypes.c_ulonglong
        lib.mysql_affected_rows.argtypes = [ctypes.c_void_p]
        lib.mysql_store_result.restype = ctypes.c_void_p
        lib.mysql_store_result.argtypes = [ctypes.c_void_p]
        lib.mysql_free_result.argtypes = [ctypes.c_void_p]
        lib.mysql_num_fields.restype = ctypes.c_uint
        lib.mysql_num_fields.argtypes = [ctypes.c_void_p]
        lib.mysql_fetch_row.restype = ctypes.POINTER(ctypes.c_char_p)
        lib.mysql_fetch_row.argtypes = [ctypes.c_void_p]
        lib.mysql_error.restype = ctypes.c_char_p
        lib.mysql_error.argtypes = [ctypes.c_void_p]
        lib.mysql_close.argtypes = [ctypes.c_void_p]
        self.conn = lib.mysql_init(None)
        enforce = ctypes.c_char(1)
        verify = ctypes.c_char(0)
        lib.mysql_options(self.conn, self.OPT_SSL_ENFORCE, ctypes.byref(enforce))
        lib.mysql_options(self.conn, self.OPT_SSL_VERIFY_SERVER_CERT, ctypes.byref(verify))
        plugins = os.environ.get("AA_MARIADB_PLUGIN_DIR", "/usr/lib/x86_64-linux-gnu/libmariadb3/plugin")
        lib.mysql_options(self.conn, self.OPT_PLUGIN_DIR, ctypes.c_char_p(plugins.encode()))
        flags = self.CLIENT_FOUND_ROWS if found_rows else 0
        password = os.environ.get("AA_DB_PASSWORD", "").encode()
        if not lib.mysql_real_connect(self.conn, host.encode(), user.encode(), password, database.encode(), port, None, flags):
            raise Failure("%s: %s" % (dialect, lib.mysql_error(self.conn).decode(errors="replace")))

    def _query(self, sql):
        if self.lib.mysql_query(self.conn, sql.encode()) != 0:
            raise Failure("%s: %s" % (sql[:80], self.lib.mysql_error(self.conn).decode(errors="replace")))

    def execute(self, sql):
        self._query(sql)
        # Read before mysql_store_result, which MySQLConnection::TryExecute calls next.
        count = self.lib.mysql_affected_rows(self.conn)
        res = self.lib.mysql_store_result(self.conn)
        if res:
            self.lib.mysql_free_result(res)
        if count == 0xFFFFFFFFFFFFFFFF:
            return None
        return int(count)

    def query(self, sql):
        self._query(sql)
        res = self.lib.mysql_store_result(self.conn)
        rows = []
        if not res:
            return rows
        fields = self.lib.mysql_num_fields(res)
        while True:
            row = self.lib.mysql_fetch_row(res)
            if not row:
                break
            rows.append(tuple((row[i] or b"").decode() for i in range(fields)))
        self.lib.mysql_free_result(res)
        return rows

    def close(self):
        self.lib.mysql_close(self.conn)


class TdsConnection:
    dialect = "mssql"

    def __init__(self, host, port, user, database):
        import pytds  # installed into the lab's node image by aa-lab.sh image
        self.conn = pytds.connect(server=host, port=port, database=database, user=user,
                                  password=os.environ.get("AA_DB_PASSWORD", ""), autocommit=True,
                                  login_timeout=15, timeout=60)

    def execute(self, sql):
        import pytds
        try:
            with self.conn.cursor() as cursor:
                cursor.execute(sql)
                return cursor.rowcount if cursor.rowcount is not None and cursor.rowcount >= 0 else None
        except pytds.Error as e:
            raise Failure("%s: %s" % (sql[:80], e))

    def query(self, sql):
        import pytds
        try:
            with self.conn.cursor() as cursor:
                cursor.execute(sql)
                return [tuple("" if v is None else str(v) for v in row) for row in cursor.fetchall()]
        except pytds.Error as e:
            raise Failure("%s: %s" % (sql[:80], e))

    def close(self):
        self.conn.close()


def connect(kind, found_rows=True, uncommitted=False):
    """A connection as the server's own would be; uncommitted: on SQL Server, the
    session put at READ UNCOMMITTED, as ADOConnection::Connect puts every session of
    the server's (the other engines have no such level in use)."""
    host = os.environ.get("AA_DB_HOST", kind)
    user = os.environ.get("AA_DB_USER", "hmlab")
    database = os.environ.get("AA_DB_NAME", "hmlab_aa_sqlcheck")
    if kind == "postgresql":
        return PgConnection(host, int(os.environ.get("AA_DB_PORT", "5432")), user, database)
    if kind in ("mysql", "mariadb"):
        return MyConnection(kind, host, int(os.environ.get("AA_DB_PORT", "3306")), user, database, found_rows)
    if kind == "mssql":
        c = TdsConnection(host, int(os.environ.get("AA_DB_PORT", "1433")), user, database)
        if uncommitted:
            c.execute("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED")
        return c
    raise Failure("no engine '%s' (postgresql, mysql, mariadb, mssql)" % kind)


# ------------------------------------------------------------------ the dialects

def now_plus(kind, seconds):
    if kind == "postgresql":
        return "current_timestamp + interval '%d seconds'" % seconds
    if kind in ("mysql", "mariadb"):
        return "DATE_ADD(NOW(), INTERVAL %d SECOND)" % seconds
    return "DATEADD(second, %d, GETDATE())" % seconds


def now(kind):
    return {"postgresql": "current_timestamp", "mysql": "NOW()", "mariadb": "NOW()"}.get(kind, "GETDATE()")


# The database's clock in UTC, as SQLStatement::GetCurrentUtcTimestamp writes it
# (ActiveActive.md, decision D3): the statement's time, never the transaction's.
def utc(kind):
    return {"postgresql": "(statement_timestamp() at time zone 'utc')", "mysql": "UTC_TIMESTAMP(6)",
            "mariadb": "UTC_TIMESTAMP(6)"}.get(kind, "SYSUTCDATETIME()")


def utc_plus(kind, seconds):
    if kind == "postgresql":
        return "((statement_timestamp() at time zone 'utc') + interval '%d seconds')" % seconds
    if kind in ("mysql", "mariadb"):
        return "DATE_ADD(UTC_TIMESTAMP(6), INTERVAL %d SECOND)" % seconds
    return "DATEADD(second, %d, SYSUTCDATETIME())" % seconds


# The database's local wall clock to the microsecond, in the session's zone: what
# GetCurrentTimestamp's functions give, read without the zone.
def local_clock(kind):
    return {"postgresql": "localtimestamp", "mysql": "NOW(6)", "mariadb": "NOW(6)"}.get(kind, "SYSDATETIME()")


def seconds_between(kind, earlier, later):
    if kind == "postgresql":
        return "cast(round(extract(epoch from (%s - %s))) as bigint)" % (later, earlier)
    if kind in ("mysql", "mariadb"):
        return "timestampdiff(second, %s, %s)" % (earlier, later)
    return "datediff(second, %s, %s)" % (earlier, later)


def begin(kind):
    return {"postgresql": "BEGIN", "mysql": "START TRANSACTION", "mariadb": "START TRANSACTION"}.get(kind, "BEGIN TRANSACTION")


def commit(kind):
    return "COMMIT TRANSACTION" if kind == "mssql" else "COMMIT"


def create_tables(c, kind):
    stamp = "timestamp" if kind == "postgresql" else "datetime"
    # The columns the leases on the UTC clock go into, as section 7 of the build plan
    # gives them: to the microsecond where the engine keeps it.
    fine = {"postgresql": "timestamp", "mysql": "datetime(6)", "mariadb": "datetime(6)"}.get(kind, "datetime2")
    name = "nvarchar(100)" if kind == "mssql" else "varchar(100)"
    engine = " ENGINE=InnoDB" if kind in ("mysql", "mariadb") else ""
    for table in ("aa_probe", "aa_folder", "aa_msg", "aa_lock", "aa_lease"):
        try:
            c.execute("drop table %s" % table)
        except Failure:
            pass
    c.execute("create table aa_probe (id int not null primary key, owner int not null, until %s null, "
              "token bigint not null, counter int not null)%s" % (stamp, engine))
    c.execute("create table aa_folder (id int not null primary key, uid int not null)%s" % engine)
    c.execute("create table aa_msg (id int not null primary key, folder int not null, uid int not null, writer int not null)%s" % engine)
    c.execute("create table aa_lock (name %s not null primary key, node int not null, until %s not null, epoch bigint not null)%s"
              % (name, fine, engine))
    c.execute("create table aa_lease (id int not null primary key, owner int not null, until %s null)%s" % (fine, engine))


# ------------------------------------------------------------------ the checks

results = []


def record(name, ok, **detail):
    line = {"check": name, "ok": bool(ok)}
    line.update(detail)
    results.append(line)
    print(json.dumps(line), flush=True)


def check_counts(kind):
    expected_same = 1
    variants = [("found_rows", True)]
    if kind in ("mysql", "mariadb"):
        variants.append(("default_flags", False))
    for label, found_rows in variants:
        c = connect(kind, found_rows)
        c.execute("delete from aa_probe")
        c.execute("insert into aa_probe (id, owner, token, counter) values (1, 0, 0, 0)")
        c.execute("insert into aa_probe (id, owner, token, counter) values (2, 0, 0, 0)")
        got = {
            "match_none": c.execute("update aa_probe set counter = counter + 1 where id = 99"),
            "match_one_changed": c.execute("update aa_probe set counter = counter + 1 where id = 1"),
            "match_one_unchanged": c.execute("update aa_probe set owner = 0 where id = 1"),
            "match_two": c.execute("update aa_probe set counter = counter + 1 where id in (1, 2)"),
            "insert": c.execute("insert into aa_probe (id, owner, token, counter) values (3, 0, 0, 0)"),
            "delete": c.execute("delete from aa_probe where id = 3"),
        }
        # A claim that recognises itself, run a second time as DALConnection::Execute
        # runs a statement again after a reconnect (ActiveActive.md, D1 and C17).
        epoch = random.randint(1, 2 ** 62)
        other = epoch + 1
        claim = "update aa_probe set owner = 7, token = %d where id = 2 and (owner = 0 or token = %d)"
        got["claim"] = c.execute(claim % (epoch, epoch))
        got["claim_run_again"] = c.execute(claim % (epoch, epoch))
        got["claim_run_again_without_its_epoch"] = c.execute("update aa_probe set owner = 7, token = %d where id = 2 and owner = 0" % epoch)
        got["claim_another_epoch"] = c.execute(claim % (other, other))
        want = {"match_none": 0, "match_one_changed": 1, "match_one_unchanged": expected_same if found_rows else 0,
                "match_two": 2, "insert": 1, "delete": 1,
                "claim": 1, "claim_run_again": 1 if found_rows else 0,
                "claim_run_again_without_its_epoch": 0, "claim_another_epoch": 0}
        if kind == "mssql":
            # NOCOUNT leaves the DONE token without its count, which ADO hands back as
            # RecordsAffected -1: why ADOConnection::Connect turns it off.
            c.execute("SET NOCOUNT ON")
            got["under_nocount"] = c.execute("update aa_probe set counter = counter + 1 where id = 1")
            c.execute("SET NOCOUNT OFF")
            got["after_nocount_off"] = c.execute("update aa_probe set counter = counter + 1 where id = 1")
            want["under_nocount"] = None
            want["after_nocount_off"] = 1
        record("counts", got == want, connection=label, got=got, want=want)
        c.close()


def check_claims(kind, rows=300, uncommitted=False):
    c = connect(kind)
    c.execute("delete from aa_probe")
    for i in range(1, rows + 1):
        c.execute("insert into aa_probe (id, owner, token, counter) values (%d, 0, 0, 0)" % i)
    c.close()
    wins = {1: 0, 2: 0}
    errors = []

    def claimer(me):
        try:
            conn = connect(kind, uncommitted=uncommitted)
            order = list(range(1, rows + 1))
            random.shuffle(order)
            for i in order:
                token = random.randint(1, 2 ** 62)
                n = conn.execute("update aa_probe set owner = %d, token = %d, until = %s where id = %d and owner = 0"
                                 % (me, token, now_plus(kind, 300), i))
                if n == 1:
                    wins[me] += 1
                elif n is None:
                    errors.append("count unknown")
            conn.close()
        except Failure as e:
            errors.append(str(e))

    threads = [threading.Thread(target=claimer, args=(m,)) for m in (1, 2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    c = connect(kind)
    owned = c.query("select owner, count(*) from aa_probe group by owner order by owner")
    c.close()
    by_owner = {int(o): int(n) for o, n in owned}
    ok = not errors and wins[1] + wins[2] == rows and by_owner.get(1, 0) == wins[1] and by_owner.get(2, 0) == wins[2]
    record("claims_read_uncommitted" if uncommitted else "claims", ok, rows=rows, wins=wins, owners=by_owner, errors=errors[:3])


def check_lease(kind, on_utc=False):
    a = connect(kind)
    b = connect(kind)
    a.execute("delete from aa_probe")
    a.execute("insert into aa_probe (id, owner, token, counter) values (1, 0, 0, 0)")
    if on_utc:
        a.execute("delete from aa_lease")
        a.execute("insert into aa_lease (id, owner, until) values (1, 0, null)")
        claim = "update aa_lease set owner = %%d, until = %s where id = 1 and (owner = 0 or until < %s)" % (utc_plus(kind, 2), utc(kind))
        first = a.execute(claim % 1)
        early = b.execute(claim % 2)
        time.sleep(3.5)
        late = b.execute(claim % 2)
        holder = a.query("select owner from aa_lease where id = 1")[0][0]
        a.close()
        b.close()
        record("lease_on_utc", first == 1 and early == 0 and late == 1 and holder == "2",
               first=first, before_expiry=early, after_expiry=late, holder_after=holder)
        return
    claim = "update aa_probe set owner = %d, token = %d, until = %s where id = 1 and (owner = 0 or until < %s)"
    first = a.execute(claim % (1, 111, now_plus(kind, 2), now(kind)))
    early = b.execute(claim % (2, 222, now_plus(kind, 2), now(kind)))
    time.sleep(3.5)
    late = b.execute(claim % (2, 222, now_plus(kind, 2), now(kind)))
    token = a.query("select token from aa_probe where id = 1")[0][0]
    a.close()
    b.close()
    record("lease", first == 1 and early == 0 and late == 1 and token == "222",
           first=first, before_expiry=early, after_expiry=late, token_after=token)


def check_named(kind, names=100, uncommitted=False):
    """Named leases whose rows do not exist yet, taken by insert-or-claim by two
    connections at once (ActiveActive.md, D10): the claim first, which counts 0 while
    the name has no row, then the insert, which the unique name lets one win. The
    loser's refused insert is the engine's unique-key error, which the server's
    database layer reads as DALErrorInSQL and a caller may ignore."""
    c = connect(kind)
    c.execute("delete from aa_lock")
    c.close()
    wins = {1: 0, 2: 0}
    refused = {1: 0, 2: 0}
    errors = []

    def taker(me):
        try:
            conn = connect(kind, uncommitted=uncommitted)
            order = list(range(1, names + 1))
            random.shuffle(order)
            for n in order:
                epoch = random.randint(1, 2 ** 62)
                name = "lease-%d" % n
                got = conn.execute("update aa_lock set node = %d, until = %s, epoch = %d where name = '%s' and (until < %s or epoch = %d)"
                                   % (me, utc_plus(kind, 600), epoch, name, utc(kind), epoch))
                if got == 0:
                    try:
                        got = conn.execute("insert into aa_lock (name, node, until, epoch) values ('%s', %d, %s, %d)"
                                           % (name, me, utc_plus(kind, 600), epoch))
                    except Failure as e:
                        text = str(e).lower()
                        if "duplicate" in text or "unique" in text or "primary key" in text:
                            refused[me] += 1
                            got = 0
                        else:
                            raise
                if got == 1:
                    wins[me] += 1
                elif got is None:
                    errors.append("count unknown")
            conn.close()
        except Failure as e:
            errors.append(str(e))

    threads = [threading.Thread(target=taker, args=(m,)) for m in (1, 2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    c = connect(kind)
    held = {int(o): int(n) for o, n in c.query("select node, count(*) from aa_lock group by node order by node")}
    c.close()
    # Each name: one connection's insert takes it, and the other's - made after a
    # claim that found no row, or found the row and a lease not expired - is refused.
    ok = (not errors and wins[1] + wins[2] == names and held.get(1, 0) == wins[1] and held.get(2, 0) == wins[2]
          and refused[1] + refused[2] == names)
    record("named_read_uncommitted" if uncommitted else "named", ok, names=names, wins=wins, inserts_refused=refused,
           held=held, errors=errors[:3])


def check_uids(kind, transactional, per_writer=400, window_ms=2.0, report=True):
    c = connect(kind)
    c.execute("delete from aa_msg")
    c.execute("delete from aa_folder")
    c.execute("insert into aa_folder (id, uid) values (1, 0)")
    c.close()
    errors = []
    done = threading.Event()
    seen = set()
    ids = {1: 0, 2: 0}

    def writer(me):
        try:
            conn = connect(kind)
            for k in range(per_writer):
                if transactional:
                    conn.execute(begin(kind))
                conn.execute("update aa_folder set uid = uid + 1 where id = 1")
                time.sleep(random.random() * window_ms / 1000.0)
                uid = int(conn.query("select uid from aa_folder where id = 1")[0][0])
                time.sleep(random.random() * window_ms / 1000.0)
                ids[me] += 1
                conn.execute("insert into aa_msg (id, folder, uid, writer) values (%d, 1, %d, %d)"
                             % (me * 1000000 + ids[me], uid, me))
                if transactional:
                    conn.execute(commit(kind))
            conn.close()
        except Failure as e:
            errors.append("writer %d: %s" % (me, e))

    def reader():
        try:
            conn = connect(kind)
            last = 0
            while True:
                finished = done.is_set()
                for (uid,) in conn.query("select uid from aa_msg where folder = 1 and uid > %d order by uid" % last):
                    seen.add(int(uid))
                    last = max(last, int(uid))
                if finished:
                    break
                time.sleep(0.003)
            conn.close()
        except Failure as e:
            errors.append("reader: %s" % e)

    writers = [threading.Thread(target=writer, args=(m,)) for m in (1, 2)]
    observer = threading.Thread(target=reader)
    observer.start()
    for t in writers:
        t.start()
    for t in writers:
        t.join()
    done.set()
    observer.join()
    c = connect(kind)
    uids = [int(u) for (u,) in c.query("select uid from aa_msg where folder = 1")]
    c.close()
    duplicates = len(uids) - len(set(uids))
    missed = len(set(uids) - seen)
    name = "uids_transaction" if transactional else "uids_three_statements"
    if transactional:
        ok = not errors and duplicates == 0 and missed == 0 and len(uids) == 2 * per_writer
    else:
        # The negative control: it passes when it shows the defect the transaction
        # removes - a duplicated number or a row the reader never saw.
        ok = not errors and (duplicates > 0 or missed > 0)
    detail = {"rows": len(uids), "window_ms": window_ms, "duplicated_numbers": duplicates,
              "never_seen_by_reader": missed, "errors": errors[:3]}
    if report:
        record(name, ok, **detail)
    return ok, detail


def check_uids_negative(kind):
    # The pause between the counter's update and its read-back stands for the time
    # between the server's two statements, a round trip at least. An engine whose
    # commit is slow (MySQL 8.4 with its binary log on, where the second writer's
    # update is not visible until its commit has been flushed) shows the race only
    # with a wider window: the window is widened until the defect is seen, and the
    # attempt that showed it - or the last - is the one recorded, with the windows
    # tried before it. The transaction is then run at that window.
    tried = []
    for window in (2.0, 10.0, 25.0):
        ok, detail = check_uids(kind, transactional=False, window_ms=window, report=False)
        if ok or window == 25.0:
            record("uids_three_statements", ok, windows_tried_before=tried, **detail)
            return window
        tried.append(window)
    return 25.0


def check_rollback(kind, reader_mode="default", per_writer=300, rollback_every=4):
    """The UIDs in one transaction, with every rollback_every-th rolled back: its
    number is given back and given to the next row. reader_mode: "default", the
    engine's own isolation (READ COMMITTED on SQL Server's own default); and on SQL
    Server "uncommitted", the session at READ UNCOMMITTED as the server's run, and
    "hinted", that session reading with the READCOMMITTED table hint (D19)."""
    c = connect(kind)
    c.execute("delete from aa_msg")
    c.execute("delete from aa_folder")
    c.execute("insert into aa_folder (id, uid) values (1, 0)")
    c.close()
    errors = []
    done = threading.Event()
    seen = {}
    rolled_back = set()

    def writer(me):
        try:
            conn = connect(kind)
            for k in range(per_writer):
                conn.execute(begin(kind))
                conn.execute("update aa_folder set uid = uid + 1 where id = 1")
                uid = int(conn.query("select uid from aa_folder where id = 1")[0][0])
                row = me * 1000000 + k
                conn.execute("insert into aa_msg (id, folder, uid, writer) values (%d, 1, %d, %d)" % (row, uid, me))
                # The row in the open transaction for a few milliseconds, as a
                # delivery's is between its insert and its commit.
                time.sleep(random.random() * 0.004)
                if k % rollback_every == rollback_every - 1:
                    conn.execute("ROLLBACK TRANSACTION" if kind == "mssql" else "ROLLBACK")
                    rolled_back.add(row)
                else:
                    conn.execute(commit(kind))
            conn.close()
        except Failure as e:
            errors.append("writer %d: %s" % (me, e))

    def reader():
        try:
            conn = connect(kind, uncommitted=reader_mode in ("uncommitted", "hinted"))
            hint = " with (READCOMMITTED)" if reader_mode == "hinted" else ""
            last = 0
            while True:
                finished = done.is_set()
                for (row, uid) in conn.query("select id, uid from aa_msg%s where folder = 1 and uid > %d order by uid" % (hint, last)):
                    seen[int(row)] = int(uid)
                    last = max(last, int(uid))
                if finished:
                    break
                time.sleep(0.002)
            conn.close()
        except Failure as e:
            errors.append("reader: %s" % e)

    writers = [threading.Thread(target=writer, args=(m,)) for m in (1, 2)]
    observer = threading.Thread(target=reader)
    observer.start()
    for t in writers:
        t.start()
    for t in writers:
        t.join()
    done.set()
    observer.join()
    c = connect(kind)
    final = {int(row): int(uid) for row, uid in c.query("select id, uid from aa_msg where folder = 1")}
    c.close()
    by_uid = {}
    for row, uid in final.items():
        by_uid.setdefault(uid, []).append(row)
    duplicates = sum(len(rows) - 1 for rows in by_uid.values())
    never_committed = len([row for row in seen if row not in final])
    moved = len([row for row, uid in seen.items() if row not in final and uid in by_uid])
    missed = len([row for row in final if row not in seen])
    detail = {"reader": reader_mode, "rows_committed": len(final), "rows_rolled_back": len(rolled_back),
              "duplicated_numbers": duplicates, "seen_never_committed": never_committed,
              "numbers_seen_then_given_to_another_row": moved, "never_seen_by_reader": missed, "errors": errors[:3]}
    if reader_mode == "uncommitted":
        # The negative control: a reader at READ UNCOMMITTED sees rows that are then
        # rolled back, and a number it saw on one is given to another.
        ok = not errors and duplicates == 0 and never_committed > 0
    else:
        ok = not errors and duplicates == 0 and never_committed == 0 and missed == 0 and len(final) + len(rolled_back) == 2 * per_writer
    record("rollback_%s" % reader_mode, ok, **detail)


def dst_change(season):
    """The daylight-saving change in Europe/London the faked clock is put before, as
    the UNIX time of its instant: 01:00 UTC on the last Sunday of October 2026, or of
    March 2027."""
    import calendar
    if season == "autumn":
        return calendar.timegm((2026, 10, 25, 1, 0, 0))
    if season == "spring":
        return calendar.timegm((2027, 3, 28, 1, 0, 0))
    raise Failure("no season '%s' (autumn, spring)" % season)


def check_dst(kind, season, lease_seconds=120):
    """A lease on the database's local clock and one on its UTC clock, taken a minute
    before a daylight-saving change and looked at on the database's clock 30 seconds
    after it, and again after the lease's length."""
    change = dst_change(season)
    c = connect(kind)
    # Where the database's clock is: the UTC clock as UNIX time, read by the engine.
    if kind == "postgresql":
        epoch_now = "cast(extract(epoch from statement_timestamp()) as bigint)"
    elif kind in ("mysql", "mariadb"):
        epoch_now = "UNIX_TIMESTAMP()"
    else:
        raise Failure("the daylight-saving check runs on PostgreSQL, MySQL and MariaDB (libfaketime)")

    def clocks():
        db_now, offset = c.query("select %s, %s" % (epoch_now, seconds_between(kind, utc(kind), local_clock(kind))))[0]
        return int(db_now), int(offset)

    start, offset_before = clocks()
    if not (change - 300 <= start <= change - 20):
        c.close()
        raise Failure("the database's clock reads %s, not the minute before the %s change at %s: is libfaketime set (aa-lab.sh dst)?"
                      % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(start)), season, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(change))))
    c.execute("delete from aa_lease")
    c.execute("insert into aa_lease (id, owner, until) values (1, 1, %s)" % (
        "%s + interval '%d seconds'" % (local_clock(kind), lease_seconds) if kind == "postgresql"
        else "DATE_ADD(%s, INTERVAL %d SECOND)" % (local_clock(kind), lease_seconds)))
    c.execute("insert into aa_lease (id, owner, until) values (2, 1, %s)" % utc_plus(kind, lease_seconds))

    def expired():
        local, utc_lease = c.query("select (select case when until < %s then 1 else 0 end from aa_lease where id = 1), "
                                   "(select case when until < %s then 1 else 0 end from aa_lease where id = 2)"
                                   % (local_clock(kind), utc(kind)))[0]
        return int(local), int(utc_lease)

    def wait_until(when):
        while True:
            db_now, _ = clocks()
            if db_now >= when:
                return
            time.sleep(min(5, max(0.5, when - db_now)))

    wait_until(change + 30)
    _, offset_after = clocks()
    local_early, utc_early = expired()
    wait_until(start + lease_seconds + 30)
    local_late, utc_late = expired()
    c.close()
    utc_right = utc_early == 0 and utc_late == 1
    if season == "autumn":
        # The local clock goes back an hour: a lease measured on it lasts an hour more.
        local_wrong = local_late == 0
        clock_moved = offset_before == 3600 and offset_after == 0
    else:
        # The local clock goes forward an hour: a lease measured on it ends at once.
        local_wrong = local_early == 1
        clock_moved = offset_before == 0 and offset_after == 3600
    record("dst_%s" % season, utc_right and local_wrong and clock_moved,
           change=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(change)), lease_seconds=lease_seconds,
           local_minus_utc_before=offset_before, local_minus_utc_after=offset_after,
           thirty_seconds_after_the_change={"local_lease_expired": local_early, "utc_lease_expired": utc_early},
           after_the_lease={"local_lease_expired": local_late, "utc_lease_expired": utc_late})


def main():
    if len(sys.argv) < 2:
        print("usage: sqlcheck.py postgresql|mysql|mariadb|mssql [counts|claims|lease|named|uids|rollback|all|dst]")
        return 2
    kind = sys.argv[1]
    which = sys.argv[2] if len(sys.argv) > 2 else "all"
    try:
        c = connect(kind)
        version = c.query("select version()" if kind != "mssql" else "select @@version")[0][0].split("\n")[0]
        create_tables(c, kind)
        c.close()
    except Failure as e:
        print("REFUSED: %s" % e)
        return 2
    print(json.dumps({"engine": kind, "version": version}), flush=True)
    try:
        if which in ("counts", "all"):
            check_counts(kind)
        if which in ("claims", "all"):
            check_claims(kind)
            if kind == "mssql":
                check_claims(kind, uncommitted=True)
        if which in ("lease", "all"):
            check_lease(kind)
            check_lease(kind, on_utc=True)
        if which in ("named", "all"):
            check_named(kind)
            if kind == "mssql":
                check_named(kind, uncommitted=True)
        if which in ("uids", "all"):
            window = check_uids_negative(kind)
            check_uids(kind, transactional=True, window_ms=window)
        if which in ("rollback", "all"):
            check_rollback(kind)
            if kind == "mssql":
                check_rollback(kind, reader_mode="uncommitted")
                check_rollback(kind, reader_mode="hinted")
        if which == "dst":
            check_dst(kind, os.environ.get("AA_DST_SEASON", ""))
    except Failure as e:
        record("run", False, error=str(e))
    failed = [r["check"] for r in results if not r["ok"]]
    print("SQLCHECK %s on %s: %d checks, %d failed%s" % ("PASSED" if not failed else "FAILED", kind, len(results),
                                                         len(failed), (" (" + ", ".join(failed) + ")") if failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
