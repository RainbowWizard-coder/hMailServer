// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "GeoIpDatabase.h"

#include "FileUtilities.h"
#include "../TCPIP/IPAddress.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // "\xAB\xCD\xEF" then "MaxMind.com". Written as separate characters rather
      // than as one string literal because "\xABM" is read by the compiler as a
      // hex escape of more than two digits.
      const char MARKER[] = { '\xAB', '\xCD', '\xEF', 'M', 'a', 'x', 'M', 'i', 'n', 'd', '.', 'c', 'o', 'm' };
      const int MARKER_LENGTH = (int) sizeof(MARKER);

      // The number of times a value may be a pointer to a pointer before the file
      // is declared circular. Real files never nest at all.
      const int MAX_DEPTH = 16;

      // 16 zero bytes stand between the search tree and the data section.
      const size_t DATA_SECTION_SEPARATOR = 16;
   }

   GeoIpDatabase::GeoIpDatabase() :
      loaded_(false),
      tree_bytes_(0),
      data_section_start_(0),
      data_section_end_(0),
      ipv4_start_node_(0)
   {
   }

   GeoIpDatabase::~GeoIpDatabase()
   {
   }

   const char *
   GeoIpDatabase::MetadataMarker()
   {
      return MARKER;
   }

   int
   GeoIpDatabase::MetadataMarkerLength()
   {
      return MARKER_LENGTH;
   }

   bool
   GeoIpDatabase::LoadFile(const String &path, String &error)
   {
      error.Empty();

      if (path.IsEmpty())
      {
         error = _T("No database file is configured.");
         return false;
      }

      if (!FileUtilities::Exists(path))
      {
         error = Formatter::Format(_T("{0} does not exist."), path);
         return false;
      }

      unsigned __int64 size = 0;
      if (!FileUtilities::FileSize64(path, size))
      {
         error = Formatter::Format(_T("The size of {0} could not be read."), path);
         return false;
      }

      if (size == 0)
      {
         error = Formatter::Format(_T("{0} is empty."), path);
         return false;
      }

      if (size > MaxFileBytes)
      {
         error = Formatter::Format(_T("{0} is larger than the 256 MB a geographic database may be."), path);
         return false;
      }

#ifdef HM_PLATFORM_POSIX
      // std::ifstream's constructor from a wide C string is a Microsoft
      // extension; the standard one takes a narrow path, which is what this
      // filesystem's names are. The same narrowing SigstoreVerifier::Sha256File
      // does, for the same reason.
      const AnsiString narrow_path = path;
      std::ifstream file(narrow_path.c_str(), std::ios::binary);
#else
      std::ifstream file(path.c_str(), std::ios::binary);
#endif

      if (!file)
      {
         error = Formatter::Format(_T("{0} could not be opened for reading."), path);
         return false;
      }

      std::vector<unsigned char> bytes;

      try
      {
         bytes.resize((size_t) size);
      }
      catch (...)
      {
         error = Formatter::Format(_T("There was not enough memory to read {0}."), path);
         return false;
      }

      file.read((char *) &bytes[0], (std::streamsize) size);

      if ((unsigned __int64) file.gcount() != size)
      {
         error = Formatter::Format(_T("{0} could not be read to its end."), path);
         return false;
      }

      return LoadBytes(bytes, error);
   }

   bool
   GeoIpDatabase::LoadBytes(const std::vector<unsigned char> &bytes, String &error)
   {
      error.Empty();

      // A failed load must not disturb a database that is already working, so
      // nothing here is written into the object until everything has been
      // validated.
      GeoIpDatabase candidate;
      candidate.bytes_ = bytes;

      if (!candidate.ParseMetadata_(error))
         return false;

      loaded_ = false;
      bytes_.swap(candidate.bytes_);
      metadata_ = candidate.metadata_;
      tree_bytes_ = candidate.tree_bytes_;
      data_section_start_ = candidate.data_section_start_;
      data_section_end_ = candidate.data_section_end_;
      ipv4_start_node_ = candidate.ipv4_start_node_;
      loaded_ = true;

      return true;
   }

   bool
   GeoIpDatabase::ParseMetadata_(String &error)
   {
      if (bytes_.size() < (size_t) MARKER_LENGTH + 1)
      {
         error = _T("The file is too short to be a MaxMind database.");
         return false;
      }

      // The LAST occurrence of the marker, because the marker's bytes may appear
      // in the data section by chance and the metadata is what follows the final
      // one. MaxMind's own readers search backwards for the same reason.
      size_t markerAt = 0;
      bool found = false;

      for (size_t candidate = bytes_.size() - MARKER_LENGTH + 1; candidate > 0; candidate--)
      {
         const size_t at = candidate - 1;

         if (memcmp(&bytes_[at], MARKER, MARKER_LENGTH) == 0)
         {
            markerAt = at;
            found = true;
            break;
         }
      }

      if (!found)
      {
         error = _T("The file does not carry a MaxMind database marker, so it is not a MaxMind database (.mmdb) file.");
         return false;
      }

      const size_t metadataStart = markerAt + MARKER_LENGTH;
      const size_t metadataLimit = bytes_.size();

      if (metadataStart >= metadataLimit)
      {
         error = _T("The file's metadata is missing: it ends at the marker.");
         return false;
      }

      // Pointers inside the metadata are relative to the start of the metadata,
      // not to the data section.
      const size_t base = metadataStart;

      AnsiString databaseType;
      if (!ReadStringField_(metadataStart, metadataLimit, base, "database_type", databaseType))
      {
         error = _T("The file's metadata has no database_type, so it is not a readable MaxMind database.");
         return false;
      }

      unsigned __int64 nodeCount = 0;
      unsigned __int64 recordSize = 0;
      unsigned __int64 ipVersion = 0;

      if (!ReadUnsignedField_(metadataStart, metadataLimit, base, "node_count", nodeCount) ||
          !ReadUnsignedField_(metadataStart, metadataLimit, base, "record_size", recordSize) ||
          !ReadUnsignedField_(metadataStart, metadataLimit, base, "ip_version", ipVersion))
      {
         error = _T("The file's metadata is missing node_count, record_size or ip_version.");
         return false;
      }

      if (recordSize != 24 && recordSize != 28 && recordSize != 32)
      {
         error = Formatter::Format(_T("The database uses a record size of {0} bits, and only 24, 28 and 32 are defined."),
            StringParser::IntToString((int) recordSize));
         return false;
      }

      if (ipVersion != 4 && ipVersion != 6)
      {
         error = Formatter::Format(_T("The database declares IP version {0}, and only 4 and 6 exist."),
            StringParser::IntToString((int) ipVersion));
         return false;
      }

      if (nodeCount == 0)
      {
         error = _T("The database has an empty search tree, so it can answer nothing.");
         return false;
      }

      // The tree's size in bytes. Computed in 64 bits and compared against the
      // file, because a node_count and record_size that multiply out beyond the
      // file are exactly what a truncated or corrupt download looks like.
      const unsigned __int64 treeBytes = nodeCount * recordSize * 2 / 8;

      if (treeBytes + DATA_SECTION_SEPARATOR > (unsigned __int64) markerAt)
      {
         error = _T("The database's search tree is longer than the file that holds it, so the file is truncated or corrupt.");
         return false;
      }

      metadata_.database_type = databaseType;
      metadata_.node_count = (unsigned int) nodeCount;
      metadata_.record_size = (unsigned int) recordSize;
      metadata_.ip_version = (unsigned int) ipVersion;

      unsigned __int64 value = 0;
      if (ReadUnsignedField_(metadataStart, metadataLimit, base, "build_epoch", value))
         metadata_.build_epoch = value;

      if (ReadUnsignedField_(metadataStart, metadataLimit, base, "binary_format_major_version", value))
         metadata_.binary_format_major = (unsigned int) value;

      if (ReadUnsignedField_(metadataStart, metadataLimit, base, "binary_format_minor_version", value))
         metadata_.binary_format_minor = (unsigned int) value;

      if (metadata_.binary_format_major != 0 && metadata_.binary_format_major != 2)
      {
         error = Formatter::Format(_T("The database is in MaxMind binary format version {0}, and this server reads version 2."),
            StringParser::IntToString((int) metadata_.binary_format_major));
         return false;
      }

      tree_bytes_ = (size_t) treeBytes;
      data_section_start_ = tree_bytes_ + DATA_SECTION_SEPARATOR;
      data_section_end_ = markerAt;

      // Walks the tree, so it needs the three fields just set and nothing else;
      // it does not ask whether the database is loaded.
      ipv4_start_node_ = Ipv4StartNode_();

      return true;
   }

   bool
   GeoIpDatabase::ReadStringField_(size_t mapOffset, size_t limit, size_t base, const char *key, AnsiString &value) const
   {
      size_t at = 0;

      if (!FindInMap_(mapOffset, limit, base, key, at, 0))
         return false;

      return ReadString_(at, limit, base, value, 0);
   }

   bool
   GeoIpDatabase::ReadUnsignedField_(size_t mapOffset, size_t limit, size_t base, const char *key, unsigned __int64 &value) const
   {
      size_t at = 0;

      if (!FindInMap_(mapOffset, limit, base, key, at, 0))
         return false;

      return ReadUnsigned_(at, limit, base, value, 0);
   }

   bool
   GeoIpDatabase::ReadControl_(size_t &offset, size_t limit, int &type, unsigned __int64 &size) const
   {
      if (offset >= limit || offset >= bytes_.size())
         return false;

      const unsigned char control = bytes_[offset++];

      type = (control >> 5) & 0x07;

      if (type == TypeExtended)
      {
         if (offset >= limit)
            return false;

         type = (int) bytes_[offset++] + 7;
      }

      size = control & 0x1F;

      if (size < 29)
         return true;

      // A pointer's low five bits are not a size at all - they carry the pointer
      // itself - so the caller handles that type before asking for a size.
      if (type == TypePointer)
         return true;

      const int extra = (int) (size - 28);        // 29 -> 1, 30 -> 2, 31 -> 3

      if (offset + extra > limit)
         return false;

      unsigned __int64 more = 0;
      for (int i = 0; i < extra; i++)
         more = (more << 8) | bytes_[offset++];

      if (extra == 1)
         size = 29 + more;
      else if (extra == 2)
         size = 285 + more;
      else
         size = 65821 + more;

      return true;
   }

   bool
   GeoIpDatabase::ReadPointer_(size_t &offset, size_t limit, size_t base, size_t &target) const
   {
      if (offset >= limit || offset >= bytes_.size())
         return false;

      const unsigned char control = bytes_[offset++];
      const int size = (control >> 3) & 0x03;     // the two "ss" bits
      const unsigned __int64 value = control & 0x07;

      const int extra = size + 1;

      if (offset + extra > limit)
         return false;

      unsigned __int64 pointer = (size == 3) ? 0 : value;

      for (int i = 0; i < extra; i++)
         pointer = (pointer << 8) | bytes_[offset++];

      if (size == 1)
         pointer += 2048;
      else if (size == 2)
         pointer += 526336;

      const unsigned __int64 absolute = (unsigned __int64) base + pointer;

      if (absolute >= (unsigned __int64) limit)
         return false;

      target = (size_t) absolute;
      return true;
   }

   bool
   GeoIpDatabase::SkipValue_(size_t &offset, size_t limit, size_t base, int depth) const
   {
      if (depth > MAX_DEPTH)
         return false;

      if (offset >= limit)
         return false;

      // A pointer is one field however large the value it names is.
      if (((bytes_[offset] >> 5) & 0x07) == TypePointer)
      {
         size_t ignored = 0;
         return ReadPointer_(offset, limit, base, ignored);
      }

      int type = 0;
      unsigned __int64 size = 0;

      if (!ReadControl_(offset, limit, type, size))
         return false;

      if (type == TypeMap)
      {
         for (unsigned __int64 i = 0; i < size; i++)
         {
            if (!SkipValue_(offset, limit, base, depth + 1))     // the key
               return false;
            if (!SkipValue_(offset, limit, base, depth + 1))     // the value
               return false;
         }

         return true;
      }

      if (type == TypeArray)
      {
         for (unsigned __int64 i = 0; i < size; i++)
         {
            if (!SkipValue_(offset, limit, base, depth + 1))
               return false;
         }

         return true;
      }

      if (type == TypeBoolean)
         return true;                                           // the size IS the value

      if (offset + size > (unsigned __int64) limit)
         return false;

      offset += (size_t) size;
      return true;
   }

   bool
   GeoIpDatabase::FindInMap_(size_t offset, size_t limit, size_t base, const char *key, size_t &valueOffset, int depth) const
   {
      if (depth > MAX_DEPTH || offset >= limit)
         return false;

      if (((bytes_[offset] >> 5) & 0x07) == TypePointer)
      {
         size_t target = 0;
         size_t at = offset;

         if (!ReadPointer_(at, limit, base, target))
            return false;

         return FindInMap_(target, limit, base, key, valueOffset, depth + 1);
      }

      int type = 0;
      unsigned __int64 size = 0;
      size_t at = offset;

      if (!ReadControl_(at, limit, type, size))
         return false;

      if (type != TypeMap)
         return false;

      const AnsiString wanted = key;

      for (unsigned __int64 i = 0; i < size; i++)
      {
         AnsiString name;

         if (!ReadString_(at, limit, base, name, depth + 1))
            return false;

         if (!SkipValue_(at, limit, base, depth + 1))            // past the key
            return false;

         if (name == wanted)
         {
            valueOffset = at;
            return true;
         }

         if (!SkipValue_(at, limit, base, depth + 1))            // past the value
            return false;
      }

      return false;
   }

   bool
   GeoIpDatabase::ReadString_(size_t offset, size_t limit, size_t base, AnsiString &value, int depth) const
   {
      if (depth > MAX_DEPTH || offset >= limit)
         return false;

      if (((bytes_[offset] >> 5) & 0x07) == TypePointer)
      {
         size_t target = 0;
         size_t at = offset;

         if (!ReadPointer_(at, limit, base, target))
            return false;

         return ReadString_(target, limit, base, value, depth + 1);
      }

      int type = 0;
      unsigned __int64 size = 0;
      size_t at = offset;

      if (!ReadControl_(at, limit, type, size))
         return false;

      if (type != TypeUtf8String && type != TypeBytes)
         return false;

      if (at + size > (unsigned __int64) limit)
         return false;

      value.assign((const char *) &bytes_[0] + at, (size_t) size);
      return true;
   }

   bool
   GeoIpDatabase::ReadUnsigned_(size_t offset, size_t limit, size_t base, unsigned __int64 &value, int depth) const
   {
      if (depth > MAX_DEPTH || offset >= limit)
         return false;

      if (((bytes_[offset] >> 5) & 0x07) == TypePointer)
      {
         size_t target = 0;
         size_t at = offset;

         if (!ReadPointer_(at, limit, base, target))
            return false;

         return ReadUnsigned_(target, limit, base, value, depth + 1);
      }

      int type = 0;
      unsigned __int64 size = 0;
      size_t at = offset;

      if (!ReadControl_(at, limit, type, size))
         return false;

      if (type != TypeUint16 && type != TypeUint32 && type != TypeUint64)
         return false;

      if (size > 8 || at + size > (unsigned __int64) limit)
         return false;

      value = 0;
      for (unsigned __int64 i = 0; i < size; i++)
         value = (value << 8) | bytes_[at++];

      return true;
   }

   bool
   GeoIpDatabase::ReadRecord_(unsigned int node, int bit, unsigned int &value) const
   {
      const unsigned int recordSize = metadata_.record_size;
      const size_t nodeBytes = (size_t) (recordSize * 2 / 8);
      const size_t at = (size_t) node * nodeBytes;

      if (at + nodeBytes > tree_bytes_ || at + nodeBytes > bytes_.size())
         return false;

      const unsigned char *n = &bytes_[at];

      if (recordSize == 24)
      {
         const int o = bit * 3;
         value = ((unsigned int) n[o] << 16) | ((unsigned int) n[o + 1] << 8) | n[o + 2];
         return true;
      }

      if (recordSize == 28)
      {
         // The middle byte carries the top nibble of each record: the high nibble
         // belongs to the left record, the low nibble to the right.
         if (bit == 0)
            value = (((unsigned int) (n[3] & 0xF0)) << 20) | ((unsigned int) n[0] << 16) | ((unsigned int) n[1] << 8) | n[2];
         else
            value = (((unsigned int) (n[3] & 0x0F)) << 24) | ((unsigned int) n[4] << 16) | ((unsigned int) n[5] << 8) | n[6];

         return true;
      }

      const int o = bit * 4;
      value = ((unsigned int) n[o] << 24) | ((unsigned int) n[o + 1] << 16) | ((unsigned int) n[o + 2] << 8) | n[o + 3];
      return true;
   }

   unsigned int
   GeoIpDatabase::Ipv4StartNode_() const
   {
      if (metadata_.ip_version == 4)
         return 0;

      // In an IPv6 database the IPv4 space lives under ::/96, so an IPv4 lookup
      // starts at the node 96 left turns from the root. MaxMind's own files also
      // alias ::ffff:0:0/96 onto the same subtree, which is why LookupBytes falls
      // back to the mapped form when this path answers nothing.
      unsigned int node = 0;

      for (int i = 0; i < 96 && node < metadata_.node_count; i++)
      {
         unsigned int next = 0;

         if (!ReadRecord_(node, 0, next))
            return 0;

         node = next;
      }

      return node;
   }

   bool
   GeoIpDatabase::Lookup(const IPAddress &address, AnsiString &isoCode, AnsiString &countryName) const
   {
      isoCode.Empty();
      countryName.Empty();

      if (!loaded_)
         return false;

      try
      {
         const boost::asio::ip::address asio = address.GetAddress();

         if (asio.is_v4())
         {
            boost::asio::ip::address_v4::bytes_type raw = asio.to_v4().to_bytes();
            return LookupBytes(&raw[0], 4, isoCode, countryName);
         }

         if (asio.is_v6())
         {
            boost::asio::ip::address_v6 v6 = asio.to_v6();

            // A dual-stack listener hands an IPv4 client over as ::ffff:a.b.c.d.
            // The answer must be the same one the same client would get on an
            // IPv4 listener, so it is unmapped before the walk rather than looked
            // up as an IPv6 address that happens to encode one.
            if (v6.is_v4_mapped())
            {
               boost::asio::ip::address_v6::bytes_type raw = v6.to_bytes();
               return LookupBytes(&raw[0] + 12, 4, isoCode, countryName);
            }

            boost::asio::ip::address_v6::bytes_type raw = v6.to_bytes();
            return LookupBytes(&raw[0], 16, isoCode, countryName);
         }
      }
      catch (...)
      {
         // An address that cannot be turned into bytes has no country, and this
         // runs on an accept path: no country is the answer, not an exception.
      }

      return false;
   }

   bool
   GeoIpDatabase::LookupBytes(const unsigned char *address, int byteCount, AnsiString &isoCode, AnsiString &countryName) const
   {
      isoCode.Empty();
      countryName.Empty();

      if (!loaded_ || address == nullptr)
         return false;

      if (byteCount != 4 && byteCount != 16)
         return false;

      // An IPv4-only database cannot answer for an IPv6 address. Saying so is
      // what makes the policy able to treat it as "unknown" rather than as a
      // country it happens to have guessed.
      if (byteCount == 16 && metadata_.ip_version == 4)
         return false;

      if (LookupFrom_(byteCount == 4 ? ipv4_start_node_ : 0, address, byteCount, isoCode, countryName))
         return true;

      // The second way an IPv6 file holds IPv4 data: as the aliased
      // ::ffff:0:0/96 subtree rather than as ::/96. A file written either way
      // answers.
      if (byteCount == 4 && metadata_.ip_version == 6)
      {
         unsigned char mapped[16];
         memset(mapped, 0, sizeof(mapped));
         mapped[10] = 0xFF;
         mapped[11] = 0xFF;
         memcpy(mapped + 12, address, 4);

         return LookupFrom_(0, mapped, 16, isoCode, countryName);
      }

      return false;
   }

   bool
   GeoIpDatabase::LookupFrom_(unsigned int startNode, const unsigned char *address, int byteCount,
                              AnsiString &isoCode, AnsiString &countryName) const
   {
      unsigned int node = startNode;
      const int bits = byteCount * 8;

      for (int i = 0; i < bits && node < metadata_.node_count; i++)
      {
         const int bit = (address[i / 8] >> (7 - (i % 8))) & 1;

         unsigned int next = 0;

         if (!ReadRecord_(node, bit, next))
            return false;

         node = next;
      }

      // Equal to the node count means the walk ended in an empty record: the
      // database has nothing for this address, which is not an error. Anything
      // between that and node_count + 16 is a pointer that would land inside the
      // separator, which no writer emits and which must not be read as a
      // negative offset.
      if (node < metadata_.node_count + DATA_SECTION_SEPARATOR)
         return false;

      const unsigned __int64 offset = (unsigned __int64) node - metadata_.node_count - DATA_SECTION_SEPARATOR;
      const unsigned __int64 absolute = (unsigned __int64) data_section_start_ + offset;

      if (absolute >= (unsigned __int64) data_section_end_)
         return false;

      return ReadCountry_((size_t) absolute, isoCode, countryName);
   }

   bool
   GeoIpDatabase::ReadCountry_(size_t recordOffset, AnsiString &isoCode, AnsiString &countryName) const
   {
      const size_t limit = data_section_end_;
      const size_t base = data_section_start_;

      // country first, then registered_country: the satellite, anonymous-proxy
      // and some Enterprise rows have no represented country and only a
      // registered one, and answering "unknown" for them would quietly exempt
      // exactly the ranges an administrator most wants a policy over.
      static const char *SECTIONS[] = { "country", "registered_country" };

      for (int s = 0; s < 2; s++)
      {
         size_t countryAt = 0;

         if (!FindInMap_(recordOffset, limit, base, SECTIONS[s], countryAt, 0))
            continue;

         size_t isoAt = 0;

         if (!FindInMap_(countryAt, limit, base, "iso_code", isoAt, 0))
            continue;

         AnsiString code;

         if (!ReadString_(isoAt, limit, base, code, 0))
            continue;

         code.Trim();

         if (code.IsEmpty())
            continue;

         code.MakeUpper();
         isoCode = code;

         // The English name, when the file carries one. A Country database always
         // does; nothing depends on it, so a file without names still works.
         size_t namesAt = 0;
         size_t englishAt = 0;

         if (FindInMap_(countryAt, limit, base, "names", namesAt, 0) &&
             FindInMap_(namesAt, limit, base, "en", englishAt, 0))
         {
            AnsiString name;

            if (ReadString_(englishAt, limit, base, name, 0))
               countryName = name;
         }

         return true;
      }

      return false;
   }

   int
   GeoIpDatabase::BuildAgeDays(time_t now) const
   {
      if (!loaded_ || metadata_.build_epoch == 0)
         return -1;

      const __int64 built = (__int64) metadata_.build_epoch;
      const __int64 current = (__int64) now;

      if (current <= built)
         return 0;

      return (int) ((current - built) / (24 * 60 * 60));
   }
}
