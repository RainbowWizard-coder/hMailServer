#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The engine checks of ../sqlcheck.py against SQL Server, in sessions set up as the server's own.

ADOConnection::Connect opens every session of the server with SET DATEFORMAT ymd and
SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED, where sqlcheck.py's SQL Server leg
ran at the engine's default, READ COMMITTED (BUILD-PLAN.md C15). This runs the same
checks - row counts, claims, a lease on the database's clock, UIDs in three
statements and in one transaction - in sessions set up that way, and adds the one
READ UNCOMMITTED makes necessary:

  uids_rollback  two writers number rows in transactions, as AA-04 will, and roll
                 back every third (as a deadlock's retry or a failed insert does),
                 while two readers poll "uid > last" as Messages::Refresh does: one
                 plain, at the session's READ UNCOMMITTED, and one with the
                 READCOMMITTED table hint D19 gives the nodes' reads. For each
                 reader: rows it saw that were rolled back (a UID shown to a client
                 that no message keeps), and committed rows it never saw (a number
                 rolled back is taken again by the next writer, below the reader's
                 mark). The plain reader is the negative control: it passes when it
                 shows the defect. The hinted reader must see neither.

Run by mssql.sh sqlcheck in a container on server2, with ../sqlcheck.py beside it
(the active-active directory mounted at /aa) and python-tds on the path. The same
environment as sqlcheck.py: AA_DB_HOST, AA_DB_PORT, AA_DB_USER, AA_DB_NAME and the
password in AA_DB_PASSWORD, never on the command line. Prints one JSON line a check
and a summary; exits 0 when every expectation held, 1 otherwise, 2 when SQL Server
could not be reached.
"""

import json
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import sqlcheck  # noqa: E402  the Linux lab's checks, beside this directory


class ServerSessionTds(sqlcheck.TdsConnection):
    """A TDS session set up as ADOConnection::Connect sets up the server's."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.execute("SET DATEFORMAT ymd")
        self.execute("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED")


# sqlcheck.connect() looks the class up by name when it is called.
sqlcheck.TdsConnection = ServerSessionTds


def session_facts():
    c = sqlcheck.connect("mssql")
    rows = c.query("select transaction_isolation_level, convert(varchar(33), sysdatetimeoffset(), 126), "
                   "convert(varchar(23), getdate(), 126), convert(varchar(27), sysutcdatetime(), 126), "
                   "cast(serverproperty('ProductVersion') as varchar(32)), cast(serverproperty('ProductUpdateLevel') as varchar(16)), "
                   "(select is_read_committed_snapshot_on from sys.databases where name = db_name()) "
                   "from sys.dm_exec_sessions where session_id = @@spid")
    c.close()
    level, offset_now, local_now, utc_now, version, update_level, rcsi = rows[0]
    names = {"0": "unspecified", "1": "READ UNCOMMITTED", "2": "READ COMMITTED", "3": "REPEATABLE READ",
             "4": "SERIALIZABLE", "5": "SNAPSHOT"}
    return {"isolation": names.get(level, level), "sysdatetimeoffset": offset_now, "getdate": local_now,
            "sysutcdatetime": utc_now, "version": version, "update_level": update_level,
            "read_committed_snapshot": rcsi}


def check_uids_rollback(per_writer=300, rollback_every=3, window_ms=2.0, report=True):
    kind = "mssql"
    c = sqlcheck.connect(kind)
    c.execute("delete from aa_msg")
    c.execute("delete from aa_folder")
    c.execute("insert into aa_folder (id, uid) values (1, 0)")
    c.close()
    errors = []
    done = threading.Event()
    committed = set()
    rolled_back = set()
    lock = threading.Lock()
    readers = {"read_uncommitted": {"hint": "", "seen": {}}, "readcommitted_hint": {"hint": " with (readcommitted)", "seen": {}}}

    def writer(me):
        try:
            conn = sqlcheck.connect(kind)
            for k in range(per_writer):
                row_id = me * 1000000 + k + 1
                conn.execute("BEGIN TRANSACTION")
                conn.execute("update aa_folder set uid = uid + 1 where id = 1")
                time.sleep(random.random() * window_ms / 1000.0)
                uid = int(conn.query("select uid from aa_folder where id = 1")[0][0])
                conn.execute("insert into aa_msg (id, folder, uid, writer) values (%d, 1, %d, %d)" % (row_id, uid, me))
                time.sleep(random.random() * window_ms / 1000.0)
                if k % rollback_every == rollback_every - 1:
                    conn.execute("ROLLBACK TRANSACTION")
                    with lock:
                        rolled_back.add(row_id)
                else:
                    conn.execute("COMMIT TRANSACTION")
                    with lock:
                        committed.add(row_id)
            conn.close()
        except sqlcheck.Failure as e:
            errors.append("writer %d: %s" % (me, e))

    def reader(name):
        state = readers[name]
        try:
            conn = sqlcheck.connect(kind)
            last = 0
            while True:
                finished = done.is_set()
                for row_id, uid in conn.query("select id, uid from aa_msg%s where folder = 1 and uid > %d order by uid"
                                              % (state["hint"], last)):
                    state["seen"][int(row_id)] = int(uid)
                    last = max(last, int(uid))
                if finished:
                    break
                time.sleep(0.002)
            conn.close()
        except sqlcheck.Failure as e:
            errors.append("reader %s: %s" % (name, e))

    watchers = [threading.Thread(target=reader, args=(n,)) for n in readers]
    writers = [threading.Thread(target=writer, args=(m,)) for m in (1, 2)]
    for t in watchers:
        t.start()
    for t in writers:
        t.start()
    for t in writers:
        t.join()
    done.set()
    for t in watchers:
        t.join()
    c = sqlcheck.connect(kind)
    rows = [(int(i), int(u)) for i, u in c.query("select id, uid from aa_msg with (readcommitted) where folder = 1")]
    c.close()
    uids = [u for _, u in rows]
    stored_ids = {i for i, _ in rows}
    result = {"rows_committed": len(rows), "rows_rolled_back": len(rolled_back), "window_ms": window_ms,
              "rollback_every": rollback_every, "committed_numbers_given_twice": len(uids) - len(set(uids)),
              "stored_rows_as_committed": stored_ids == committed, "errors": errors[:3], "readers": {}}
    for name, state in readers.items():
        seen = state["seen"]
        result["readers"][name] = {
            "rows_seen_then_rolled_back": len(set(seen) & rolled_back),
            "committed_rows_never_seen": len(committed - set(seen)),
        }
    plain = result["readers"]["read_uncommitted"]
    hinted = result["readers"]["readcommitted_hint"]
    defect_shown = plain["rows_seen_then_rolled_back"] > 0 or plain["committed_rows_never_seen"] > 0
    hint_clean = hinted["rows_seen_then_rolled_back"] == 0 and hinted["committed_rows_never_seen"] == 0
    ok = (not errors and result["committed_numbers_given_twice"] == 0 and result["stored_rows_as_committed"]
          and hint_clean and defect_shown)
    result["defect_shown_at_read_uncommitted"] = defect_shown
    if report:
        sqlcheck.record("uids_rollback", ok, **result)
    return ok, defect_shown, result


def check_uids_rollback_widening():
    # As sqlcheck.check_uids_negative widens its window: the plain reader's defect
    # needs a writer's row to be read in the moment between its insert and its
    # rollback, which a wider window makes likelier. The hinted reader is held to
    # seeing nothing wrong at every width tried.
    tried = []
    for window in (2.0, 10.0, 25.0):
        ok, shown, result = check_uids_rollback(window_ms=window, report=False)
        if shown or window == 25.0:
            sqlcheck.record("uids_rollback", ok, windows_tried_before=tried, **result)
            return
        hinted = result["readers"]["readcommitted_hint"]
        tried.append({"window_ms": window, "hinted_reader": hinted})


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    try:
        facts = session_facts()
        c = sqlcheck.connect("mssql")
        sqlcheck.create_tables(c, "mssql")
        c.close()
    except sqlcheck.Failure as e:
        print("REFUSED: %s" % e)
        return 2
    except Exception as e:  # pytds raises its own errors for a refused login or no route
        print("REFUSED: %s" % e)
        return 2
    print(json.dumps({"engine": "mssql", "session": facts}), flush=True)
    sqlcheck.record("session_as_the_server_sets_it", facts["isolation"] == "READ UNCOMMITTED", isolation=facts["isolation"])
    try:
        if which in ("counts", "all"):
            sqlcheck.check_counts("mssql")
        if which in ("claims", "all"):
            sqlcheck.check_claims("mssql")
        if which in ("lease", "all"):
            sqlcheck.check_lease("mssql")
        if which in ("uids", "all"):
            window = sqlcheck.check_uids_negative("mssql")
            sqlcheck.check_uids("mssql", transactional=True, window_ms=window)
        if which in ("rollback", "all"):
            check_uids_rollback_widening()
    except sqlcheck.Failure as e:
        sqlcheck.record("run", False, error=str(e))
    failed = [r["check"] for r in sqlcheck.results if not r["ok"]]
    print("SQLCHECK %s on mssql at READ UNCOMMITTED: %d checks, %d failed%s"
          % ("PASSED" if not failed else "FAILED", len(sqlcheck.results), len(failed),
             (" (" + ", ".join(failed) + ")") if failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
