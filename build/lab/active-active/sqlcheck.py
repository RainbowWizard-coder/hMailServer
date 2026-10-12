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
           1 with it, as PostgreSQL and SQL Server do.
  claims   two connections race to claim the same rows with a conditional UPDATE
           (... where id = @ID and owner = 0) and count their wins by the row
           count. Every row must be won exactly once.
  lease    a claim held until the database's clock plus two seconds is refused to
           the other connection before then and taken by it after.
  uids     two writers put rows into one "folder", each taking the next number from
           the folder's counter, while a reader does what an IMAP session's
           Messages::Refresh does: reads the rows above the highest number it has
           seen. Done twice: as the server does it today (the counter raised, read
           back and the row inserted, three statements on their own), and in one
           transaction. Counted: numbers given to two rows, and rows the reader
           never saw because they were committed below a number it had already
           passed - a message a client would never fetch.

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


def connect(kind, found_rows=True):
    host = os.environ.get("AA_DB_HOST", kind)
    user = os.environ.get("AA_DB_USER", "hmlab")
    database = os.environ.get("AA_DB_NAME", "hmlab_aa_sqlcheck")
    if kind == "postgresql":
        return PgConnection(host, int(os.environ.get("AA_DB_PORT", "5432")), user, database)
    if kind in ("mysql", "mariadb"):
        return MyConnection(kind, host, int(os.environ.get("AA_DB_PORT", "3306")), user, database, found_rows)
    if kind == "mssql":
        return TdsConnection(host, int(os.environ.get("AA_DB_PORT", "1433")), user, database)
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


def begin(kind):
    return {"postgresql": "BEGIN", "mysql": "START TRANSACTION", "mariadb": "START TRANSACTION"}.get(kind, "BEGIN TRANSACTION")


def commit(kind):
    return "COMMIT TRANSACTION" if kind == "mssql" else "COMMIT"


def create_tables(c, kind):
    stamp = "timestamp" if kind == "postgresql" else "datetime"
    engine = " ENGINE=InnoDB" if kind in ("mysql", "mariadb") else ""
    for table in ("aa_probe", "aa_folder", "aa_msg"):
        try:
            c.execute("drop table %s" % table)
        except Failure:
            pass
    c.execute("create table aa_probe (id int not null primary key, owner int not null, until %s null, "
              "token bigint not null, counter int not null)%s" % (stamp, engine))
    c.execute("create table aa_folder (id int not null primary key, uid int not null)%s" % engine)
    c.execute("create table aa_msg (id int not null primary key, folder int not null, uid int not null, writer int not null)%s" % engine)


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
        want = {"match_none": 0, "match_one_changed": 1, "match_one_unchanged": expected_same if found_rows else 0,
                "match_two": 2, "insert": 1, "delete": 1}
        record("counts", got == want, connection=label, got=got, want=want)
        c.close()


def check_claims(kind, rows=300):
    c = connect(kind)
    c.execute("delete from aa_probe")
    for i in range(1, rows + 1):
        c.execute("insert into aa_probe (id, owner, token, counter) values (%d, 0, 0, 0)" % i)
    c.close()
    wins = {1: 0, 2: 0}
    errors = []

    def claimer(me):
        try:
            conn = connect(kind)
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
    record("claims", ok, rows=rows, wins=wins, owners=by_owner, errors=errors[:3])


def check_lease(kind):
    a = connect(kind)
    b = connect(kind)
    a.execute("delete from aa_probe")
    a.execute("insert into aa_probe (id, owner, token, counter) values (1, 0, 0, 0)")
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


def main():
    if len(sys.argv) < 2:
        print("usage: sqlcheck.py postgresql|mysql|mariadb|mssql [counts|claims|lease|uids|all]")
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
        if which in ("lease", "all"):
            check_lease(kind)
        if which in ("uids", "all"):
            window = check_uids_negative(kind)
            check_uids(kind, transactional=True, window_ms=window)
    except Failure as e:
        record("run", False, error=str(e))
    failed = [r["check"] for r in results if not r["ok"]]
    print("SQLCHECK %s on %s: %d checks, %d failed%s" % ("PASSED" if not failed else "FAILED", kind, len(results),
                                                         len(failed), (" (" + ", ".join(failed) + ")") if failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
