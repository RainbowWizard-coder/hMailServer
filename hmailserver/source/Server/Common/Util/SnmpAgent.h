// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The SNMP agent: a read-only answer to the monitoring system a bank already
// owns, beside the Prometheus endpoint and the OTLP exporter rather than instead
// of them. hmailserver/docs/Snmp.md is the operator's document; SnmpEngine.h
// says what a datagram gets back and the bounds it keeps.
//
// WHAT IT SERVES, AND FROM WHERE
//
// The same counters and gauges /metrics serves (ServerStatus, the session
// counts, the delivery queue's depth and oldest message), in the shape of the
// mail MIB (RFC 2789 MTA-MIB) and of a few values under an enterprise arc, and
// the system, snmp, snmpEngine and usmStats groups the agent keeps itself.
// There is no second set of counters: a value that /metrics has no counter for
// is answered noSuchObject, and docs/Snmp.md names each one and why.
//
// WHO IT ANSWERS
//
// Nobody until SnmpAllowedAddresses names a manager. A datagram from an address
// outside the list is dropped before it is decoded - it moves no counter, costs
// no cryptography and is never answered - so turning the agent on does nothing
// until the operator has said who it is for. Then SNMPv3 at authPriv for the one
// user SnmpV3User names, and SNMPv2c only while SnmpV2cEnabled is on as well.
//
// THE LIFECYCLE is the metrics listener's: Application::StartServers makes and
// starts one when SnmpEnabled is on, StopServers stops it, and a Reinitialize
// is both. Every setting is read at Start; a change is saved at once and applied
// at the next start or Reinitialize.
//
// THE THREADS. One receives and answers, in a Boost.Asio io_context of its own;
// one refreshes the delivery queue's two figures from the database, on demand,
// at most every ten seconds, so that the thread that answers never waits on the
// database - the rule MetricsServer.h's rule 5 states and this follows.
//
// THE ENGINE ID AND THE BOOTS. The engine ID is RFC 3411's enterprise form:
// 0x80 and the enterprise number (the documentation number 32473, until this
// project has one of its own), format 5 (octets), and twelve octets of
// SHA-256 over a random seed made once per installation and this machine's
// name - so two servers sharing one database are two engines, as SNMP needs
// them to be. The seed is made by the first start that finds none and written
// only where there is none yet, so that two nodes starting together agree on
// one. snmpEngineBoots is kept per engine, in a row named for the engine
// (EngineBootsName), read from the database and not from the settings the
// process loaded, and written one higher before the socket opens; an agent that
// cannot read it, finds it damaged or cannot write it does not start, since a
// boots value used twice lets a message recorded in one run be replayed in the
// next. The rows are the server's own records, not settings: no interface sets
// them, the list of what is changed from stock leaves them out, and a backup
// does not carry them (IsEngineRecord says which they are).

#pragma once

#include "SnmpEngine.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace HM
{
   class SnmpAgent : public SnmpMibSource
   {
   public:
      SnmpAgent();
      virtual ~SnmpAgent();

      // Reads the settings, records the boot, prepares the keys and binds the
      // socket. False, said in the application log or the error log, when the
      // agent did not start.
      bool Start();
      void Stop();

      // The mail MIB and the enterprise arc, now. Called on the agent's thread,
      // once for each request that is answered.
      virtual void Collect(std::vector<SnmpBinding> &bindings);

      // The records in hm_settings (see above). They are written only when the
      // agent starts - a server that never switches it on has none - and read
      // and written in the database directly, never through the loaded settings.
      // A boots row's name is the prefix and twenty hex digits of the engine ID's
      // own octets, which fits the thirty characters hm_settings.settingname
      // holds. IsEngineRecord is true for the seed's row, for a boots row, and for
      // the single SnmpEngineBoots row of the row's first commit, so that a
      // backup and the changed-from-stock list leave all of them out.
      static const TCHAR *EngineSeedName();
      static const TCHAR *EngineBootsPrefix();
      static String EngineBootsName(const SnmpBytes &engineId);
      static bool IsEngineRecord(const String &name);

      // The refusals every surface that sets the SNMP settings gives, each one
      // sentence and empty when the value is acceptable.
      static String BindAddressProblem(const String &value);
      static String AllowedAddressesProblem(const String &value);
      static String UserProblem(const String &value);
      static String PasswordProblem(const String &value, const String &settingName);
      static String CommunityProblem(const String &value);
      static String TextProblem(const String &value, const String &settingName);

      // The placeholder enterprise arc (1.3.6.1.4.1.32473.1): RFC 5612's number
      // for documentation, not registered to this project. docs/Snmp.md says so.
      static SnmpOid EnterpriseOid();

   private:
      SnmpAgent(const SnmpAgent &);
      SnmpAgent &operator=(const SnmpAgent &);

      // Everything Boost.Asio: the io_context, the socket, the datagram buffer
      // and the parsed allow list. Defined in the .cpp so that asio stays out of
      // the headers that include this one.
      struct UdpListener;

      bool PrepareEngine_(SnmpEngineSettings &settings);

      // The seed, made if there is none, and the boots recorded one higher. Read
      // and written in the database, not the loaded settings. False, reported as
      // 7421, when either cannot be.
      static bool ReadOrMakeSeed_(SnmpBytes &seed);
      static bool RecordBoot_(const SnmpBytes &engineId, long long &boots);
      bool OpenSocket_(const String &bindAddress, int port);

      void Run_();
      void Receive_();
      void Answer_(size_t length);
      bool IsAllowed_() const;

      void Run_Refresher_();
      void RefreshDeliveryQueue_();

      std::unique_ptr<UdpListener> transport_;
      std::unique_ptr<SnmpEngine> engine_;

      std::thread worker_;
      std::thread refresher_;

      std::atomic<bool> running_;
      ULONGLONG start_tick_;

      // Which of the three protocol groups the MIB carries, read at Start: the
      // servers that run.
      bool group_smtp_;
      bool group_pop3_;
      bool group_imap_;

      // The refresher's figures and its wake-up, under cache_mutex_, which is
      // never held across a database call.
      std::mutex cache_mutex_;
      std::condition_variable refresher_wakeup_;
      bool refresher_stop_;
      bool queue_refresh_requested_;
      ULONGLONG queue_refresh_tick_;
      long long queue_depth_;
      long long queue_oldest_age_seconds_;
   };
}
