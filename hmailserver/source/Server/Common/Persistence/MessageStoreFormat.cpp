// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStoreFormat.h"

#include "../Util/Assert.h"
#include "../Util/MailboxCrypto.h"
#include "../Util/MailboxKeys.h"

// zlib: on Windows the copy vendored under Server/zlib, which hMailServer.exe
// compiles in; on POSIX the distribution's, whose library CMakeLists.txt links,
// so the header and the library are one version (and the source tarball can
// leave Server/zlib out: build/source-exclusions.txt).
#ifdef HM_PLATFORM_POSIX
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif

#include <openssl/sha.h>

#include <algorithm>
#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const unsigned char MagicBytes[8] = { 0x89, 'H', 'M', 'S', 0x0D, 0x0A, 0x1A, 0x0A };

      // A header can be no longer than this: 1024 references and eight stages
      // come to well under 64 KB, and a length past it is damage, not a header.
      const unsigned int LongestHeader = (unsigned int) MessageStoreFormat::MaximumHeaderLength;

      // MimeBody's own limit on how deep a multipart may nest.
      const int DeepestNesting = 20;

      void PutU16(std::string &out, unsigned int value)
      {
         out.push_back((char) (value & 0xFF));
         out.push_back((char) ((value >> 8) & 0xFF));
      }

      void PutU32(std::string &out, unsigned int value)
      {
         for (int i = 0; i < 4; i++)
            out.push_back((char) ((value >> (8 * i)) & 0xFF));
      }

      void PutU64(std::string &out, unsigned __int64 value)
      {
         for (int i = 0; i < 8; i++)
            out.push_back((char) ((value >> (8 * i)) & 0xFF));
      }

      unsigned int GetU32(const std::string &in, size_t at)
      {
         unsigned int value = 0;
         for (int i = 3; i >= 0; i--)
            value = (value << 8) | (unsigned char) in[at + i];
         return value;
      }

      unsigned __int64 GetU64(const std::string &in, size_t at)
      {
         unsigned __int64 value = 0;
         for (int i = 7; i >= 0; i--)
            value = (value << 8) | (unsigned char) in[at + i];
         return value;
      }

      bool IsHex(const AnsiString &text)
      {
         if (text.size() != 64)
            return false;

         for (size_t i = 0; i < text.size(); i++)
         {
            const char c = text.c_str()[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }

         return true;
      }

      std::string FromHex(const AnsiString &hex)
      {
         std::string raw;
         for (size_t i = 0; i + 1 < hex.size(); i += 2)
         {
            const char high = hex.c_str()[i];
            const char low = hex.c_str()[i + 1];
            const int h = high <= '9' ? high - '0' : high - 'a' + 10;
            const int l = low <= '9' ? low - '0' : low - 'a' + 10;
            raw.push_back((char) ((h << 4) | l));
         }
         return raw;
      }

      bool Deflate(const std::string &in, int level, std::string &out, String &problem)
      {
         z_stream stream;
         memset(&stream, 0, sizeof(stream));

         if (deflateInit(&stream, level) != Z_OK)
         {
            problem = _T("zlib could not start a deflate stream");
            return false;
         }

         const uLong bound = deflateBound(&stream, (uLong) in.size());
         out.assign((size_t) bound + 1, '\0');

         stream.next_in = (Bytef *) in.data();
         stream.avail_in = (uInt) in.size();
         stream.next_out = (Bytef *) &out[0];
         stream.avail_out = (uInt) out.size();

         const int result = deflate(&stream, Z_FINISH);
         const uLong written = stream.total_out;
         deflateEnd(&stream);

         if (result != Z_STREAM_END)
         {
            problem.Format(_T("zlib's deflate answered %d"), result);
            return false;
         }

         out.resize((size_t) written);
         return true;
      }

      // The whole stream, which must give exactly expected bytes and end there.
      bool Inflate(const std::string &in, size_t expected, std::string &out, String &problem)
      {
         z_stream stream;
         memset(&stream, 0, sizeof(stream));

         if (inflateInit(&stream) != Z_OK)
         {
            problem = _T("zlib could not start an inflate stream");
            return false;
         }

         // One byte more than expected, so that a stream that would give more
         // is caught rather than cut.
         out.assign(expected + 1, '\0');

         stream.next_in = (Bytef *) in.data();
         stream.avail_in = (uInt) in.size();
         stream.next_out = (Bytef *) &out[0];
         stream.avail_out = (uInt) out.size();

         const int result = inflate(&stream, Z_FINISH);
         const uLong written = stream.total_out;
         const uInt left = stream.avail_in;
         inflateEnd(&stream);

         if (result != Z_STREAM_END)
         {
            problem.Format(_T("the deflated body is damaged or cut short (zlib answered %d)"), result);
            return false;
         }

         if ((size_t) written != expected || left != 0)
         {
            problem.Format(_T("the deflated body gives %I64u bytes where the header says %I64u"), (unsigned __int64) written, (unsigned __int64) expected);
            return false;
         }

         out.resize(expected);
         return true;
      }

      // The whole stream inflated and checked - it must give exactly expected
      // bytes, end there with its Adler-32 right and leave nothing over - with
      // only the first maximum bytes kept: what a read of the header needs,
      // without holding the rest. A chunk at a time, so a stream that would
      // give more than it should is stopped as soon as it has.
      bool InflateCheckedStart(const std::string &in, size_t expected, size_t maximum, std::string &out, String &problem)
      {
         out.clear();

         z_stream stream;
         memset(&stream, 0, sizeof(stream));

         if (inflateInit(&stream) != Z_OK)
         {
            problem = _T("zlib could not start an inflate stream");
            return false;
         }

         std::vector<unsigned char> chunk(65536);

         stream.next_in = (Bytef *) in.data();
         stream.avail_in = (uInt) in.size();

         int result = Z_OK;
         bool tooMuch = false;
         for (;;)
         {
            stream.next_out = (Bytef *) &chunk[0];
            stream.avail_out = (uInt) chunk.size();

            // Z_BUF_ERROR here is a stream that ran out of input before its end:
            // cut short, and refused below with the rest.
            result = inflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END)
               break;

            const size_t produced = chunk.size() - (size_t) stream.avail_out;
            if (produced > 0 && out.size() < maximum)
               out.append((const char *) &chunk[0], std::min(produced, maximum - out.size()));

            if ((unsigned __int64) stream.total_out > (unsigned __int64) expected)
            {
               tooMuch = true;
               break;
            }

            if (result == Z_STREAM_END)
               break;
         }

         const unsigned __int64 written = (unsigned __int64) stream.total_out;
         const uInt left = stream.avail_in;
         inflateEnd(&stream);

         if (tooMuch || (result == Z_STREAM_END && (written != (unsigned __int64) expected || left != 0)))
         {
            out.clear();
            problem.Format(_T("the deflated body gives %I64u bytes where the header says %I64u"), written, (unsigned __int64) expected);
            return false;
         }

         if (result != Z_STREAM_END)
         {
            out.clear();
            problem.Format(_T("the deflated body is damaged or cut short (zlib answered %d)"), result);
            return false;
         }

         return true;
      }

      // ---- The encryption stage ---------------------------------------------------

      // What the encryption stage binds besides the two public keys: the
      // header's own facts about the message before the stage - the magic,
      // the version, the flags, the original's size - and every stage under
      // it with its parameters. Built the same way when a container is
      // written and when it is read, from the fields and not from the header's
      // bytes, since the header's length and the stage's own parameters are
      // not known until the stage has run.
      std::string StageContext(unsigned __int64 originalSize, unsigned char flags,
                               const std::vector<std::pair<unsigned char, std::string> > &innerStages)
      {
         std::string context("HMS stage 3 context");
         context.push_back('\0');
         context.append(reinterpret_cast<const char *>(MagicBytes), MessageStoreFormat::MagicLength);
         context.push_back((char) MessageStoreFormat::Version);
         context.push_back((char) flags);
         PutU64(context, originalSize);
         context.push_back((char) innerStages.size());
         for (size_t i = 0; i < innerStages.size(); i++)
         {
            context.push_back((char) innerStages[i].first);
            context.push_back((char) innerStages[i].second.size());
            context += innerStages[i].second;
         }
         return context;
      }

      // The server's own open keys, when a caller gives no finder of its own.
      bool FindOpenKey(const std::string &recipientPublic, std::string &privateKey, String &problem)
      {
         return MailboxKeys::FindOpenKey(recipientPublic, privateKey, problem);
      }

      // The encryption stage at index undone: the body decrypted with the key
      // that opens it, the tag checked.
      bool OpenKeyStage(const MessageStoreHeader &header, size_t index, const std::string &body,
                        const MailboxCrypto::KeyFinder &keys, std::string &out, String &problem)
      {
         const std::vector<std::pair<unsigned char, std::string> > inner(header.stages.begin(), header.stages.begin() + index);
         const std::string context = StageContext(header.original_size, header.flags, inner);

         if (keys)
            return MailboxCrypto::OpenStage(header.stages[index].second, context, body, keys, out, problem);

         const MailboxCrypto::KeyFinder server = FindOpenKey;
         return MailboxCrypto::OpenStage(header.stages[index].second, context, body, server, out, problem);
      }

      // ---- The MIME walk --------------------------------------------------------

      bool StartsWithNoCase(const std::string &text, size_t at, const char *prefix)
      {
         const size_t length = strlen(prefix);
         if (at + length > text.size())
            return false;

         for (size_t i = 0; i < length; i++)
         {
            char c = text[at + i];
            if (c >= 'A' && c <= 'Z')
               c = (char) (c - 'A' + 'a');
            if (c != prefix[i])
               return false;
         }

         return true;
      }

      // The value of one header field in a header block, unfolded, or empty.
      std::string FieldValue(const std::string &header, const char *lowerName)
      {
         size_t at = 0;
         while (at < header.size())
         {
            size_t lineEnd = header.find('\n', at);
            if (lineEnd == std::string::npos)
               lineEnd = header.size();

            if (StartsWithNoCase(header, at, lowerName) && at + strlen(lowerName) < header.size() && header[at + strlen(lowerName)] == ':')
            {
               std::string value = header.substr(at + strlen(lowerName) + 1, lineEnd - at - strlen(lowerName) - 1);

               // Continuation lines: the next line begins with a space or a tab.
               size_t next = lineEnd + 1;
               while (next < header.size() && (header[next] == ' ' || header[next] == '\t'))
               {
                  size_t nextEnd = header.find('\n', next);
                  if (nextEnd == std::string::npos)
                     nextEnd = header.size();
                  value += " " + header.substr(next, nextEnd - next);
                  next = nextEnd + 1;
               }

               std::string cleaned;
               for (size_t i = 0; i < value.size(); i++)
               {
                  if (value[i] != '\r' && value[i] != '\n')
                     cleaned.push_back(value[i]);
               }
               return cleaned;
            }

            at = lineEnd + 1;
         }

         return std::string();
      }

      std::string Trimmed(const std::string &text)
      {
         size_t begin = 0;
         size_t end = text.size();
         while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
            begin++;
         while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            end--;
         return text.substr(begin, end - begin);
      }

      std::string Lowered(const std::string &text)
      {
         std::string out = text;
         for (size_t i = 0; i < out.size(); i++)
         {
            if (out[i] >= 'A' && out[i] <= 'Z')
               out[i] = (char) (out[i] - 'A' + 'a');
         }
         return out;
      }

      // The media type of a Content-Type value, lower case, and its boundary.
      void ReadContentType(const std::string &value, std::string &type, std::string &boundary)
      {
         const size_t semicolon = value.find(';');
         type = Lowered(Trimmed(value.substr(0, semicolon)));
         boundary.clear();

         const std::string lower = Lowered(value);
         size_t at = lower.find("boundary=");
         while (at != std::string::npos && at > 0 && lower[at - 1] != ';' && lower[at - 1] != ' ' && lower[at - 1] != '\t')
            at = lower.find("boundary=", at + 1);

         if (at == std::string::npos)
            return;

         at += 9;
         if (at < value.size() && value[at] == '"')
         {
            const size_t close = value.find('"', at + 1);
            if (close != std::string::npos)
               boundary = value.substr(at + 1, close - at - 1);
            return;
         }

         size_t end = at;
         while (end < value.size() && value[end] != ';' && value[end] != ' ' && value[end] != '\t')
            end++;
         boundary = value.substr(at, end - at);
      }

      // Where the header block of an entity ends, and its body begins: after the
      // first empty line. False for an entity with no empty line - no body.
      bool FindBody(const std::string &m, size_t start, size_t end, size_t &headerEnd, size_t &bodyStart)
      {
         size_t at = start;
         while (at < end)
         {
            // An empty line at the very start of the entity is an empty header.
            if (m[at] == '\n')
            {
               headerEnd = at;
               bodyStart = at + 1;
               return true;
            }
            if (m[at] == '\r' && at + 1 < end && m[at + 1] == '\n')
            {
               headerEnd = at;
               bodyStart = at + 2;
               return true;
            }

            const size_t lineEnd = m.find('\n', at);
            if (lineEnd == std::string::npos || lineEnd >= end)
               return false;

            at = lineEnd + 1;
         }

         return false;
      }

      // The next delimiter line "--boundary" at the start of a line in
      // [from, end), or npos. RFC 2046 5.1.1: the line may carry transport
      // padding (spaces and tabs) and the close delimiter adds "--".
      size_t FindDelimiter(const std::string &m, size_t from, size_t end, const std::string &delimiter)
      {
         size_t at = from;
         while (at < end)
         {
            const size_t found = m.find(delimiter, at);
            if (found == std::string::npos || found + delimiter.size() > end)
               return std::string::npos;

            const bool lineStart = found == 0 || m[found - 1] == '\n';
            const size_t after = found + delimiter.size();
            const bool lineEnds = after >= end || m[after] == '\r' || m[after] == '\n' || m[after] == ' ' || m[after] == '\t' || m[after] == '-';

            if (lineStart && lineEnds)
               return found;

            at = found + 1;
         }

         return std::string::npos;
      }

      unsigned __int64 Base64DecodedLength(const std::string &m, size_t start, size_t end)
      {
         unsigned __int64 characters = 0;
         for (size_t i = start; i < end; i++)
         {
            const char c = m[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')
               characters++;
         }
         return characters * 3 / 4;
      }

      void Walk(const std::string &m, size_t start, size_t end, int depth, unsigned __int64 minimum, std::vector<MessageStoreReference> &parts)
      {
         size_t headerEnd = 0;
         size_t bodyStart = 0;
         if (!FindBody(m, start, end, headerEnd, bodyStart) || bodyStart >= end)
            return;

         const std::string header = m.substr(start, headerEnd - start);

         std::string type;
         std::string boundary;
         ReadContentType(FieldValue(header, "content-type"), type, boundary);

         if (type.compare(0, 10, "multipart/") == 0)
         {
            // Deeper than MimeBody parses, or no boundary to split on: the
            // whole is left as it is.
            if (boundary.empty() || depth >= DeepestNesting)
               return;

            const std::string delimiter = "--" + boundary;
            size_t at = FindDelimiter(m, bodyStart, end, delimiter);

            while (at != std::string::npos)
            {
               const size_t after = at + delimiter.size();
               if (after + 1 < end && m[after] == '-' && m[after + 1] == '-')
                  break;   // the close delimiter

               const size_t lineEnd = m.find('\n', after);
               if (lineEnd == std::string::npos || lineEnd >= end)
                  break;

               const size_t partStart = lineEnd + 1;
               const size_t next = FindDelimiter(m, partStart, end, delimiter);
               if (next == std::string::npos)
                  break;   // no delimiter after the part: not a part this can place

               // The line break before a delimiter belongs to the delimiter.
               size_t partEnd = next;
               if (partEnd >= partStart + 2 && m[partEnd - 2] == '\r' && m[partEnd - 1] == '\n')
                  partEnd -= 2;
               else if (partEnd >= partStart + 1 && m[partEnd - 1] == '\n')
                  partEnd -= 1;

               if (partEnd > partStart)
                  Walk(m, partStart, partEnd, depth + 1, minimum, parts);

               at = next;
            }

            return;
         }

         // A leaf: its body, as it is on the wire.
         const size_t length = end - bodyStart;
         const std::string encoding = Lowered(Trimmed(FieldValue(header, "content-transfer-encoding")));
         const unsigned __int64 decoded = encoding == "base64" ? Base64DecodedLength(m, bodyStart, end) : (unsigned __int64) length;

         if (length > 0 && decoded >= minimum && parts.size() < (size_t) MessageStoreFormat::MaximumReferences)
         {
            MessageStoreReference part;
            part.offset = bodyStart;
            part.length = length;
            part.sha256 = MessageStoreFormat::Hex(MessageStoreFormat::Sha256(m.data() + bodyStart, length));
            parts.push_back(part);
         }
      }
   }

   bool
   MessageStoreFormat::HasMagic(const char *bytes, size_t length)
   {
      return length >= MagicLength && memcmp(bytes, MagicBytes, MagicLength) == 0;
   }

   unsigned int
   MessageStoreFormat::HeaderLengthOf(const char *bytes, size_t length)
   {
      if (length < 16 || !HasMagic(bytes, length))
         return 0;

      unsigned int value = 0;
      for (int i = 3; i >= 0; i--)
         value = (value << 8) | (unsigned char) bytes[12 + i];

      return value;
   }

   std::string
   MessageStoreFormat::Sha256(const char *bytes, size_t length)
   {
      unsigned char digest[SHA256_DIGEST_LENGTH] = {};
      SHA256(reinterpret_cast<const unsigned char *>(bytes), length, digest);
      return std::string(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
   }

   AnsiString
   MessageStoreFormat::Hex(const std::string &raw)
   {
      static const char *digits = "0123456789abcdef";
      AnsiString hex;
      for (size_t i = 0; i < raw.size(); i++)
      {
         const unsigned char byte = (unsigned char) raw[i];
         hex += digits[byte >> 4];
         hex += digits[byte & 0x0F];
      }
      return hex;
   }

   bool
   MessageStoreFormat::ParseHeader(const std::string &bytes, MessageStoreHeader &header, String &problem)
   {
      header = MessageStoreHeader();

      if (!HasMagic(bytes.data(), bytes.size()))
      {
         problem = _T("it does not begin with the container's magic");
         return false;
      }

      if (bytes.size() < (size_t) FixedHeaderLength)
      {
         problem = _T("it is cut short inside the header");
         return false;
      }

      header.version = (unsigned char) bytes[8];
      header.flags = (unsigned char) bytes[9];
      header.header_length = GetU32(bytes, 12);
      header.original_size = GetU64(bytes, 16);
      header.sha256 = bytes.substr(24, 32);

      if (header.version != Version)
      {
         problem.Format(_T("it is version %d of the format, and this server reads version %d"), (int) header.version, (int) Version);
         return false;
      }

      if (header.header_length < (unsigned int) FixedHeaderLength + 4 || header.header_length > LongestHeader)
      {
         problem.Format(_T("its header claims a length of %u bytes"), header.header_length);
         return false;
      }

      if (bytes.size() < header.header_length)
      {
         problem = _T("it is cut short inside the header");
         return false;
      }

      if (header.original_size > MaximumOriginalBytes)
      {
         problem.Format(_T("it claims to hold %I64u bytes, more than a container ever holds"), header.original_size);
         return false;
      }

      size_t at = 56;
      const unsigned int stageCount = (unsigned char) bytes[at++];
      if (stageCount > (unsigned int) MaximumStages)
      {
         problem.Format(_T("it names %u stages"), stageCount);
         return false;
      }

      bool deflated = false;
      bool encrypted = false;

      for (unsigned int i = 0; i < stageCount; i++)
      {
         if (at + 2 > header.header_length)
         {
            problem = _T("its list of stages runs past its header");
            return false;
         }

         const unsigned char id = (unsigned char) bytes[at];
         const unsigned int parameterLength = (unsigned char) bytes[at + 1];
         at += 2;

         if (at + parameterLength > header.header_length)
         {
            problem = _T("its list of stages runs past its header");
            return false;
         }

         // The encryption to the account's key is always the last stage: a
         // stage after it would be one run over ciphertext, which nothing here
         // writes.
         if (encrypted)
         {
            problem = _T("it names a stage after its encryption stage, which is always the last");
            return false;
         }

         if (id == StageDeflate)
         {
            if (deflated)
            {
               problem = _T("it names deflate twice");
               return false;
            }
            deflated = true;
         }
         else if (id == StageAccountKey)
         {
            const std::string parameters = bytes.substr(at, parameterLength);
            if (MailboxCrypto::StageRecipient(parameters).empty())
            {
               problem = _T("its encryption stage's parameters are not ones this server reads");
               return false;
            }
            encrypted = true;
         }
         else
         {
            problem.Format(_T("it names stage %d, which this server does not know"), (int) id);
            return false;
         }

         header.stages.push_back(std::make_pair(id, bytes.substr(at, parameterLength)));
         at += parameterLength;
      }

      if (at + 4 > header.header_length)
      {
         problem = _T("its list of references runs past its header");
         return false;
      }

      const unsigned int referenceCount = GetU32(bytes, at);
      at += 4;

      if (referenceCount > (unsigned int) MaximumReferences ||
          at + (size_t) referenceCount * ReferenceLength > header.header_length)
      {
         problem.Format(_T("it names %u references, more than its header holds"), referenceCount);
         return false;
      }

      if (((header.flags & FlagReferences) != 0) != (referenceCount > 0))
      {
         problem = _T("its flags and its list of references disagree");
         return false;
      }

      unsigned __int64 previousEnd = 0;
      for (unsigned int i = 0; i < referenceCount; i++)
      {
         MessageStoreReference reference;
         reference.offset = GetU64(bytes, at);
         reference.length = GetU64(bytes, at + 8);
         reference.sha256 = Hex(bytes.substr(at + 16, 32));
         at += ReferenceLength;

         if (reference.length == 0 || reference.offset < previousEnd ||
             reference.offset > header.original_size || reference.length > header.original_size - reference.offset)
         {
            problem.Format(_T("its reference %u lies outside the message, or out of order"), i + 1);
            return false;
         }

         previousEnd = reference.offset + reference.length;
         header.references.push_back(reference);
      }

      // An encrypted container holds its whole message, under its account's
      // key, and carries no digest of it in the clear (the header comment says
      // why): one that names a part held once, or a digest, is not one this
      // server wrote.
      if (encrypted)
      {
         if (!header.references.empty())
         {
            problem = _T("it is encrypted and names a part held once, which an encrypted container never does");
            return false;
         }

         if (header.sha256 != std::string(32, '\0'))
         {
            problem = _T("it is encrypted and carries a digest of its message, which an encrypted container never does");
            return false;
         }
      }

      return true;
   }

   bool
   MessageStoreFormat::IsEncrypted(const MessageStoreHeader &header)
   {
      return !header.stages.empty() && header.stages.back().first == StageAccountKey;
   }

   std::string
   MessageStoreFormat::EncryptedTo(const MessageStoreHeader &header)
   {
      if (!IsEncrypted(header))
         return std::string();

      return MailboxCrypto::StageRecipient(header.stages.back().second);
   }

   bool
   MessageStoreFormat::ApplyStage(unsigned char id, const std::string &parameters, const std::string &in, std::string &out, String &problem)
   {
      if (id == StageDeflate)
      {
         const int level = parameters.empty() ? (int) DeflateLevel : (int) (unsigned char) parameters[0];
         return Deflate(in, level, out, problem);
      }

      // The encryption stage needs a key and the header it binds, which a
      // stage alone is not given: EncodeEncrypted applies it.
      if (id == StageAccountKey)
      {
         problem = _T("the encryption stage is applied with its account's key, by EncodeEncrypted");
         return false;
      }

      problem.Format(_T("stage %d is not one this server can apply"), (int) id);
      return false;
   }

   bool
   MessageStoreFormat::UndoStage(unsigned char id, const std::string &parameters, const std::string &in, size_t expected, std::string &out, String &problem)
   {
      if (id == StageDeflate)
         return Inflate(in, expected, out, problem);

      // As above: Decode and DecodeStart undo it, with the header.
      if (id == StageAccountKey)
      {
         problem = _T("the encryption stage is undone with its account's key, by Decode");
         return false;
      }

      problem.Format(_T("stage %d is not one this server can undo"), (int) id);
      return false;
   }

   bool
   MessageStoreFormat::Encode(const std::string &original, const std::vector<MessageStoreReference> &references, bool compress,
                              std::string &container, String &problem)
   {
      container.clear();

      if ((unsigned __int64) original.size() > MaximumOriginalBytes)
      {
         problem = _T("the message is larger than a container holds");
         return false;
      }

      if (references.size() > (size_t) MaximumReferences)
      {
         problem = _T("the message has more parts to hold once than a container names");
         return false;
      }

      // What is left once the referenced ranges are cut out.
      std::string rest;
      rest.reserve(original.size());
      unsigned __int64 at = 0;
      for (size_t i = 0; i < references.size(); i++)
      {
         const MessageStoreReference &reference = references[i];
         if (reference.offset < at || reference.length == 0 || reference.offset + reference.length > original.size() || !IsHex(reference.sha256))
         {
            problem = _T("a part to hold once lies outside the message, or out of order");
            return false;
         }

         rest.append(original, (size_t) at, (size_t) (reference.offset - at));
         at = reference.offset + reference.length;
      }
      rest.append(original, (size_t) at, std::string::npos);

      std::vector<std::pair<unsigned char, std::string> > stages;
      std::string body = rest;

      if (compress)
      {
         const std::string parameters(1, (char) DeflateLevel);
         std::string deflated;
         if (!ApplyStage(StageDeflate, parameters, body, deflated, problem))
            return false;

         stages.push_back(std::make_pair((unsigned char) StageDeflate, parameters));
         body.swap(deflated);
      }

      std::string header(reinterpret_cast<const char *>(MagicBytes), MagicLength);
      header.push_back((char) Version);
      header.push_back((char) (references.empty() ? 0 : FlagReferences));
      PutU16(header, 0);
      PutU32(header, 0);   // the header's length, filled in below
      PutU64(header, (unsigned __int64) original.size());
      header += Sha256(original.data(), original.size());

      header.push_back((char) stages.size());
      for (size_t i = 0; i < stages.size(); i++)
      {
         header.push_back((char) stages[i].first);
         header.push_back((char) stages[i].second.size());
         header += stages[i].second;
      }

      PutU32(header, (unsigned int) references.size());
      for (size_t i = 0; i < references.size(); i++)
      {
         PutU64(header, references[i].offset);
         PutU64(header, references[i].length);
         header += FromHex(references[i].sha256);
      }

      const unsigned int length = (unsigned int) header.size();
      for (int i = 0; i < 4; i++)
         header[12 + i] = (char) ((length >> (8 * i)) & 0xFF);

      container = header + body;
      return true;
   }

   bool
   MessageStoreFormat::EncodeEncrypted(const std::string &original, bool compress, const std::string &recipientPublic,
                                       std::string &container, String &problem)
   {
      container.clear();

      if ((unsigned __int64) original.size() > MaximumOriginalBytes)
      {
         problem = _T("the message is larger than a container holds");
         return false;
      }

      // Compression first: ciphertext does not compress.
      std::vector<std::pair<unsigned char, std::string> > stages;
      std::string body = original;

      if (compress)
      {
         const std::string parameters(1, (char) DeflateLevel);
         std::string deflated;
         if (!ApplyStage(StageDeflate, parameters, body, deflated, problem))
            return false;

         stages.push_back(std::make_pair((unsigned char) StageDeflate, parameters));
         body.swap(deflated);
      }

      // No references, so no flag; the context binds the size and the stage
      // under this one.
      const unsigned char flags = 0;
      const std::string context = StageContext((unsigned __int64) original.size(), flags, stages);

      std::string parameters;
      std::string cipher;
      if (!MailboxCrypto::SealStage(recipientPublic, context, body, parameters, cipher, problem))
         return false;

      MailboxCrypto::Forget(body);
      stages.push_back(std::make_pair((unsigned char) StageAccountKey, parameters));

      std::string header(reinterpret_cast<const char *>(MagicBytes), MagicLength);
      header.push_back((char) Version);
      header.push_back((char) flags);
      PutU16(header, 0);
      PutU32(header, 0);   // the header's length, filled in below
      PutU64(header, (unsigned __int64) original.size());
      header += std::string(32, '\0');   // no digest of the message in the clear

      header.push_back((char) stages.size());
      for (size_t i = 0; i < stages.size(); i++)
      {
         header.push_back((char) stages[i].first);
         header.push_back((char) stages[i].second.size());
         header += stages[i].second;
      }

      PutU32(header, 0);   // no references

      const unsigned int length = (unsigned int) header.size();
      for (int i = 0; i < 4; i++)
         header[12 + i] = (char) ((length >> (8 * i)) & 0xFF);

      container = header + cipher;
      return true;
   }

   bool
   MessageStoreFormat::Decode(const std::string &container, const BlobReader &blob, std::string &original,
                              MessageStoreHeader &header, String &problem, const MailboxCrypto::KeyFinder &keys)
   {
      original.clear();

      if (!ParseHeader(container, header, problem))
         return false;

      unsigned __int64 referenced = 0;
      for (size_t i = 0; i < header.references.size(); i++)
         referenced += header.references[i].length;

      const size_t restLength = (size_t) (header.original_size - referenced);

      // The stages undone in the reverse order: the encryption, which is
      // always the last, first - its output is the body under it, as long as
      // its input - and then deflate, whose output is the rest.
      std::string body = container.substr(header.header_length);
      for (size_t i = header.stages.size(); i > 0; i--)
      {
         std::string undone;

         if (header.stages[i - 1].first == StageAccountKey)
         {
            if (!OpenKeyStage(header, i - 1, body, keys, undone, problem))
               return false;
            body.swap(undone);
            continue;
         }

         if (!UndoStage(header.stages[i - 1].first, header.stages[i - 1].second, body, restLength, undone, problem))
            return false;
         body.swap(undone);
      }

      if (body.size() != restLength)
      {
         problem.Format(_T("its body gives %I64u bytes where %I64u are left once the referenced parts are taken away"),
            (unsigned __int64) body.size(), (unsigned __int64) restLength);
         return false;
      }

      // The referenced parts put back where they were.
      original.reserve((size_t) header.original_size);
      size_t restAt = 0;
      unsigned __int64 originalAt = 0;
      for (size_t i = 0; i < header.references.size(); i++)
      {
         const MessageStoreReference &reference = header.references[i];
         const size_t before = (size_t) (reference.offset - originalAt);
         original.append(body, restAt, before);
         restAt += before;

         std::string part;
         if (!blob(reference, part, problem))
            return false;

         if ((unsigned __int64) part.size() != reference.length)
         {
            problem.Format(_T("the blob %s is %I64u bytes where the reference names %I64u"),
               String(reference.sha256).c_str(), (unsigned __int64) part.size(), reference.length);
            return false;
         }

         original += part;
         originalAt = reference.offset + reference.length;
      }
      original.append(body, restAt, std::string::npos);

      if ((unsigned __int64) original.size() != header.original_size)
      {
         problem.Format(_T("it gives %I64u bytes where the header says %I64u"), (unsigned __int64) original.size(), header.original_size);
         return false;
      }

      // An encrypted container carries no digest: its tag, checked above,
      // refused every changed byte of what it holds, and its size is checked
      // just before this.
      if (!IsEncrypted(header) && Sha256(original.data(), original.size()) != header.sha256)
      {
         problem = _T("what it gives back does not match the SHA-256 its header carries");
         return false;
      }

      return true;
   }

   bool
   MessageStoreFormat::DecodeStart(const std::string &container, size_t maximum, std::string &start, String &problem,
                                   const MailboxCrypto::KeyFinder &keys)
   {
      start.clear();

      MessageStoreHeader header;
      if (!ParseHeader(container, header, problem))
         return false;

      // What the container holds itself: the original less the referenced
      // parts, which ParseHeader has found in order and inside the original.
      unsigned __int64 referenced = 0;
      for (size_t i = 0; i < header.references.size(); i++)
         referenced += header.references[i].length;

      const size_t restLength = (size_t) (header.original_size - referenced);

      // The original's first bytes are the rest's until the first referenced part.
      size_t wanted = maximum;
      if (!header.references.empty() && header.references[0].offset < (unsigned __int64) wanted)
         wanted = (size_t) header.references[0].offset;
      if ((unsigned __int64) wanted > header.original_size)
         wanted = (size_t) header.original_size;

      std::string body = container.substr(header.header_length);

      // The encryption, always the last stage, undone whole first: its tag
      // covers the whole body, so it cannot be read a part at a time, and
      // the body under it is then read as an unencrypted container's is.
      std::vector<std::pair<unsigned char, std::string> > stages = header.stages;
      if (IsEncrypted(header))
      {
         std::string opened;
         if (!OpenKeyStage(header, stages.size() - 1, body, keys, opened, problem))
            return false;

         body.swap(opened);
         stages.pop_back();
      }

      if (stages.empty())
      {
         if (body.size() != restLength)
         {
            problem.Format(_T("its body holds %I64u bytes where %I64u are left once the referenced parts are taken away"),
               (unsigned __int64) body.size(), (unsigned __int64) restLength);
            return false;
         }

         start = body.substr(0, wanted);
         return true;
      }

      // Deflate, the one stage under the encryption: its stream read to its
      // end and checked, only the start kept.
      if (stages.size() != 1 || stages[0].first != StageDeflate)
      {
         problem = _T("its stages cannot be read a part at a time");
         return false;
      }

      return InflateCheckedStart(body, restLength, wanted, start, problem);
   }

   void
   MessageStoreFormat::FindLargeParts(const std::string &message, unsigned __int64 minimumBytes, std::vector<MessageStoreReference> &parts)
   {
      parts.clear();

      if (minimumBytes == 0 || message.empty())
         return;

      Walk(message, 0, message.size(), 0, minimumBytes, parts);
   }

   void
   MessageStoreFormat::Test()
   {
      // A message with a large base64 part, held once and put back byte for byte.
      std::string attachment;
      for (int i = 0; i < 6000; i++)
         attachment += "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ejAx\r\n";

      const std::string message =
         "From: a@example.test\r\nTo: b@example.test\r\nSubject: Test\r\nMIME-Version: 1.0\r\n"
         "Content-Type: multipart/mixed; boundary=\"frontier\"\r\n\r\n"
         "--frontier\r\nContent-Type: text/plain\r\n\r\nHello.\r\n"
         "--frontier\r\nContent-Type: application/octet-stream\r\nContent-Transfer-Encoding: base64\r\n\r\n" + attachment +
         "\r\n--frontier--\r\n";

      std::vector<MessageStoreReference> parts;
      FindLargeParts(message, 256 * 1024, parts);
      Assert::IsTrue(parts.size() == 1);
      Assert::IsTrue(message.substr((size_t) parts[0].offset, (size_t) parts[0].length) == attachment);

      std::string container;
      String problem;
      Assert::IsTrue(Encode(message, parts, true, container, problem));
      Assert::IsTrue(HasMagic(container.data(), container.size()));

      MessageStoreHeader header;
      std::string decoded;
      const BlobReader blob = [&attachment] (const MessageStoreReference &, std::string &bytes, String &) { bytes = attachment; return true; };
      Assert::IsTrue(Decode(container, blob, decoded, header, problem));
      Assert::IsTrue(decoded == message);

      std::string start;
      Assert::IsTrue(DecodeStart(container, 60, start, problem));
      Assert::IsTrue(start == message.substr(0, 60));

      // Damage refused: one byte of the body changed, and the file cut short -
      // by a read of the whole, and by a read of the start alone, which reads
      // the deflate stream to its end and its Adler-32.
      std::string damaged = container;
      damaged[damaged.size() - 3] = (char) (damaged[damaged.size() - 3] ^ 0x55);
      Assert::IsFalse(Decode(damaged, blob, decoded, header, problem));
      Assert::IsFalse(Decode(container.substr(0, container.size() / 2), blob, decoded, header, problem));
      Assert::IsFalse(DecodeStart(damaged, 60, start, problem));
      Assert::IsFalse(DecodeStart(container.substr(0, container.size() - 5), 60, start, problem));

      // A message that begins with the magic is a container with no stages.
      const std::string odd = std::string(reinterpret_cast<const char *>(MagicBytes), MagicLength) + "not a header\r\n\r\nbody";
      Assert::IsTrue(Encode(odd, std::vector<MessageStoreReference>(), false, container, problem));
      Assert::IsTrue(Decode(container, blob, decoded, header, problem));
      Assert::IsTrue(decoded == odd && header.stages.empty());

      // Encrypted to an account's key (stage 3), deflated first and not: the
      // container names no reference and carries no digest, holds no byte of
      // the message in the clear, and gives the message back byte for byte -
      // whole and its start - only to the account's key.
      std::string accountPrivate;
      std::string accountPublic;
      Assert::IsTrue(MailboxCrypto::GenerateKeyPair(accountPrivate, accountPublic));
      std::string otherPrivate;
      std::string otherPublic;
      Assert::IsTrue(MailboxCrypto::GenerateKeyPair(otherPrivate, otherPublic));

      const MailboxCrypto::KeyFinder accountKey = [&accountPrivate, &accountPublic] (const std::string &recipient, std::string &key, String &) {
         if (recipient != accountPublic) return false;
         key = accountPrivate;
         return true;
      };
      const MailboxCrypto::KeyFinder otherKey = [&otherPrivate] (const std::string &, std::string &key, String &) {
         key = otherPrivate;
         return true;
      };
      const MailboxCrypto::KeyFinder noKey = [] (const std::string &, std::string &, String &why) {
         why = _T("the key is not open");
         return false;
      };

      // A container rebuilt from an encrypted one with its header changed - the
      // stages, the references and the digest given; the size and the body
      // kept - for ParseHeader's refusals below.
      const auto rebuilt = [] (const std::string &from, const std::vector<std::pair<unsigned char, std::string> > &stages,
                               unsigned int referenceCount, const std::string &digest) -> std::string
      {
         MessageStoreHeader parsed;
         String why;
         if (!ParseHeader(from, parsed, why))
            return std::string();

         std::string header(reinterpret_cast<const char *>(MagicBytes), MagicLength);
         header.push_back((char) Version);
         header.push_back((char) (referenceCount > 0 ? FlagReferences : 0));
         PutU16(header, 0);
         PutU32(header, 0);
         PutU64(header, parsed.original_size);
         header += digest;

         header.push_back((char) stages.size());
         for (size_t i = 0; i < stages.size(); i++)
         {
            header.push_back((char) stages[i].first);
            header.push_back((char) stages[i].second.size());
            header += stages[i].second;
         }

         // A reference that lies inside the message, so that the one refusal
         // meant is the one met.
         PutU32(header, referenceCount);
         for (unsigned int i = 0; i < referenceCount; i++)
         {
            PutU64(header, 0);
            PutU64(header, 1);
            header += std::string(32, '\x01');
         }

         const unsigned int length = (unsigned int) header.size();
         for (int i = 0; i < 4; i++)
            header[12 + i] = (char) ((length >> (8 * i)) & 0xFF);

         return header + from.substr(parsed.header_length);
      };

      for (int compressed = 0; compressed < 2; compressed++)
      {
         std::string sealed;
         Assert::IsTrue(EncodeEncrypted(message, compressed == 1, accountPublic, sealed, problem));
         Assert::IsTrue(HasMagic(sealed.data(), sealed.size()));
         Assert::IsTrue(sealed.find("Hello.") == std::string::npos && sealed.find("QUJDREVG") == std::string::npos);

         MessageStoreHeader sealedHeader;
         Assert::IsTrue(ParseHeader(sealed, sealedHeader, problem));
         Assert::IsTrue(IsEncrypted(sealedHeader) && EncryptedTo(sealedHeader) == accountPublic);
         Assert::IsTrue(sealedHeader.references.empty() && sealedHeader.sha256 == std::string(32, '\0'));
         Assert::IsTrue(sealedHeader.stages.size() == (size_t) (compressed == 1 ? 2 : 1));

         Assert::IsTrue(Decode(sealed, blob, decoded, header, problem, accountKey));
         Assert::IsTrue(decoded == message);
         Assert::IsTrue(DecodeStart(sealed, 60, start, problem, accountKey));
         Assert::IsTrue(start == message.substr(0, 60));

         // Another key, no key, and a changed byte of the body or of the size
         // the header claims are each refused.
         Assert::IsFalse(Decode(sealed, blob, decoded, header, problem, otherKey));
         Assert::IsFalse(Decode(sealed, blob, decoded, header, problem, noKey));
         Assert::IsFalse(DecodeStart(sealed, 60, start, problem, noKey));

         std::string changed = sealed;
         changed[changed.size() - 10] = (char) (changed[changed.size() - 10] ^ 0x20);
         Assert::IsFalse(Decode(changed, blob, decoded, header, problem, accountKey));
         Assert::IsFalse(DecodeStart(changed, 60, start, problem, accountKey));

         std::string resized = sealed;
         resized[16] = (char) (resized[16] ^ 0x01);
         Assert::IsFalse(Decode(resized, blob, decoded, header, problem, accountKey));

         // A byte only the tag's associated data protects: the deflate stage's
         // level (bytes 56 to 59 are the count, the id, the length and the
         // level), which neither ParseHeader nor Inflate reads - so only the
         // binding of the stages under the encryption refuses it.
         if (compressed == 1)
         {
            std::string relevelled = sealed;
            Assert::IsTrue((unsigned char) relevelled[57] == (unsigned char) StageDeflate && relevelled[58] == 1);
            relevelled[59] = (char) (relevelled[59] ^ 0x01);
            Assert::IsTrue(ParseHeader(relevelled, header, problem));
            Assert::IsFalse(Decode(relevelled, blob, decoded, header, problem, accountKey));
            Assert::IsFalse(DecodeStart(relevelled, 60, start, problem, accountKey));
         }

         // ParseHeader's refusals of an encrypted container this server never
         // writes: a stage after the encryption, the encryption twice, deflate
         // twice, one that names a part held once, and one carrying a digest.
         // The control: the same header rebuilt unchanged parses and opens.
         const std::vector<std::pair<unsigned char, std::string> > sealedStages = sealedHeader.stages;
         const std::pair<unsigned char, std::string> encryption = sealedStages.back();
         const std::pair<unsigned char, std::string> deflate((unsigned char) StageDeflate, std::string(1, (char) DeflateLevel));
         const std::string noDigest(32, '\0');

         const std::string unchanged = rebuilt(sealed, sealedStages, 0, noDigest);
         Assert::IsTrue(unchanged == sealed);
         Assert::IsTrue(Decode(unchanged, blob, decoded, header, problem, accountKey) && decoded == message);

         std::vector<std::pair<unsigned char, std::string> > stages = sealedStages;
         stages.push_back(deflate);
         Assert::IsFalse(ParseHeader(rebuilt(sealed, stages, 0, noDigest), header, problem));
         Assert::IsTrue(problem.Find(_T("after its encryption stage")) >= 0);

         stages = sealedStages;
         stages.push_back(encryption);
         Assert::IsFalse(ParseHeader(rebuilt(sealed, stages, 0, noDigest), header, problem));
         Assert::IsTrue(problem.Find(_T("after its encryption stage")) >= 0);

         if (compressed == 1)
         {
            stages = sealedStages;
            stages.insert(stages.begin(), deflate);
            Assert::IsFalse(ParseHeader(rebuilt(sealed, stages, 0, noDigest), header, problem));
            Assert::IsTrue(problem.Find(_T("deflate twice")) >= 0);
         }

         Assert::IsFalse(ParseHeader(rebuilt(sealed, sealedStages, 1, noDigest), header, problem));
         Assert::IsTrue(problem.Find(_T("names a part held once")) >= 0);

         std::string digest = noDigest;
         digest[0] = (char) 1;
         Assert::IsFalse(ParseHeader(rebuilt(sealed, sealedStages, 0, digest), header, problem));
         Assert::IsTrue(problem.Find(_T("carries a digest")) >= 0);
      }

      MailboxCrypto::Forget(accountPrivate);
      MailboxCrypto::Forget(otherPrivate);
   }
}
