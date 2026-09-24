// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "RemoteDomainPolicy.h"

#include "../Util/Encoding/Base64.h"
#include "../Util/Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   RemoteDomainPolicy::RemoteDomainPolicy() :
      active_(true),
      outbound_tls_(RemoteTlsDefault),
      require_inbound_tls_(false),
      max_message_size_kb_(0),
      max_connections_(0),
      max_messages_per_minute_(0),
      allow_automatic_replies_(true),
      allow_forwarding_(true),
      callout_enabled_(false),
      callout_port_(25),
      callout_timeout_seconds_(10),
      callout_cache_minutes_(60),
      callout_max_per_minute_(10),
      encrypt_(RemoteEncryptNone),
      encrypt_fallback_(RemoteEncryptFallbackRefuse)
   {

   }

   RemoteDomainPolicy::~RemoteDomainPolicy()
   {

   }

   bool
   RemoteDomainPolicy::XMLStore(XNode *parentNode, int options)
   {
      XNode *node = parentNode->AppendChild(_T("RemoteDomainPolicy"));

      node->AppendAttr(_T("Name"), domain_name_);
      node->AppendAttr(_T("Description"), description_);
      node->AppendAttr(_T("Active"), active_ ? _T("1") : _T("0"));

      // The longs are cast to int for IntToString the way Route::XMLStore casts
      // its three: MSVC binds a long to the int overload because long and int are
      // the same 32-bit type on Windows, and clang finds int, unsigned int and
      // __int64 all equally distant and refuses to choose. The cast names the
      // overload the Windows build already selects and cannot lose a value a
      // 32-bit long could hold.
      node->AppendAttr(_T("OutboundTls"), StringParser::IntToString((int) outbound_tls_));
      node->AppendAttr(_T("RequireInboundTls"), require_inbound_tls_ ? _T("1") : _T("0"));
      node->AppendAttr(_T("MaxMessageSizeKB"), StringParser::IntToString((int) max_message_size_kb_));
      node->AppendAttr(_T("MaxConnections"), StringParser::IntToString((int) max_connections_));
      node->AppendAttr(_T("MaxMessagesPerMinute"), StringParser::IntToString((int) max_messages_per_minute_));
      node->AppendAttr(_T("AllowAutomaticReplies"), allow_automatic_replies_ ? _T("1") : _T("0"));
      node->AppendAttr(_T("AllowForwarding"), allow_forwarding_ ? _T("1") : _T("0"));
      node->AppendAttr(_T("CalloutEnabled"), callout_enabled_ ? _T("1") : _T("0"));
      node->AppendAttr(_T("CalloutHost"), callout_host_);
      node->AppendAttr(_T("CalloutPort"), StringParser::IntToString((int) callout_port_));
      node->AppendAttr(_T("CalloutTimeoutSeconds"), StringParser::IntToString((int) callout_timeout_seconds_));
      node->AppendAttr(_T("CalloutCacheMinutes"), StringParser::IntToString((int) callout_cache_minutes_));
      node->AppendAttr(_T("CalloutMaxPerMinute"), StringParser::IntToString((int) callout_max_per_minute_));

      // The partner's key is public and the backup carries it, so that a restored
      // policy still encrypts. Base64 of its UTF-8, because an armoured key and a
      // PEM certificate are many lines and an XML attribute does not keep a line
      // break as it stands.
      AnsiString keyUtf8;
      Unicode::WideToMultiByte(encrypt_key_, keyUtf8);
      const String keyBase64 = String(Base64::Encode(keyUtf8.c_str(), keyUtf8.GetLength()));

      node->AppendAttr(_T("Encrypt"), StringParser::IntToString((int) encrypt_));
      node->AppendAttr(_T("EncryptKey"), keyBase64);
      node->AppendAttr(_T("EncryptFallback"), StringParser::IntToString((int) encrypt_fallback_));

      return true;
   }

   bool
   RemoteDomainPolicy::XMLLoad(XNode *node, int options)
   {
      domain_name_ = node->GetAttrValue(_T("Name"));
      description_ = node->GetAttrValue(_T("Description"));

      // A backup written before this attribute existed has none, and an empty
      // string is not "0" - so an absent Active restores as active, which is
      // what a record in the file was. The two flags that permit something
      // (replies, forwarding) restore the same way, as permitted.
      String activeValue = node->GetAttrValue(_T("Active"));
      active_ = activeValue.IsEmpty() ? true : activeValue == _T("1");

      outbound_tls_ = (RemoteTlsRequirement) _ttoi(node->GetAttrValue(_T("OutboundTls")));
      require_inbound_tls_ = node->GetAttrValue(_T("RequireInboundTls")) == _T("1");
      max_message_size_kb_ = _ttoi(node->GetAttrValue(_T("MaxMessageSizeKB")));
      max_connections_ = _ttoi(node->GetAttrValue(_T("MaxConnections")));
      max_messages_per_minute_ = _ttoi(node->GetAttrValue(_T("MaxMessagesPerMinute")));

      String repliesValue = node->GetAttrValue(_T("AllowAutomaticReplies"));
      allow_automatic_replies_ = repliesValue.IsEmpty() ? true : repliesValue == _T("1");

      String forwardingValue = node->GetAttrValue(_T("AllowForwarding"));
      allow_forwarding_ = forwardingValue.IsEmpty() ? true : forwardingValue == _T("1");

      callout_enabled_ = node->GetAttrValue(_T("CalloutEnabled")) == _T("1");
      callout_host_ = node->GetAttrValue(_T("CalloutHost"));
      callout_port_ = _ttoi(node->GetAttrValue(_T("CalloutPort")));
      callout_timeout_seconds_ = _ttoi(node->GetAttrValue(_T("CalloutTimeoutSeconds")));
      callout_cache_minutes_ = _ttoi(node->GetAttrValue(_T("CalloutCacheMinutes")));
      callout_max_per_minute_ = _ttoi(node->GetAttrValue(_T("CalloutMaxPerMinute")));

      // A restored record with no port or timeout would be a callout to port 0
      // that never answers, so the constructor's values stand in for a zero the
      // file should not have carried.
      if (callout_port_ <= 0)
         callout_port_ = 25;
      if (callout_timeout_seconds_ <= 0)
         callout_timeout_seconds_ = 10;

      // A backup written before gateway encryption existed has none of the three,
      // and restores as a policy that encrypts nothing - which is what it was. A
      // number this version does not know lands on the value that rewrites no
      // message and refuses rather than sends, as the database read does.
      int encrypt = _ttoi(node->GetAttrValue(_T("Encrypt")));
      if (encrypt < RemoteEncryptNone || encrypt > RemoteEncryptSmime)
         encrypt = RemoteEncryptNone;
      encrypt_ = (RemoteEncryption) encrypt;

      AnsiString keyBase64 = AnsiString(node->GetAttrValue(_T("EncryptKey")));
      AnsiString keyUtf8 = keyBase64.IsEmpty() ? AnsiString() : Base64::Decode(keyBase64.c_str(), keyBase64.GetLength());
      String key;
      Unicode::MultiByteToWide(keyUtf8, key);
      encrypt_key_ = key;

      encrypt_fallback_ = _ttoi(node->GetAttrValue(_T("EncryptFallback"))) == RemoteEncryptFallbackSendUnencrypted
         ? RemoteEncryptFallbackSendUnencrypted
         : RemoteEncryptFallbackRefuse;

      return true;
   }
}
