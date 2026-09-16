// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A reader for the MaxMind DB (MMDB) binary format - the file a GeoLite2 or
// GeoIP2 country database comes in.
//
// WHY THE BINARY FORMAT AND NOT THE CSV
//
// The administrator supplies the database; this project ships none and must
// never download one. What they will have is the file MaxMind actually
// distributes, which is GeoLite2-Country.mmdb, and which every other mail
// server, firewall and web server on their machine already reads. The CSV form
// exists but is a second-class artefact: it is two files that have to be joined
// (the network blocks and the locations), it has no build date in it, and a
// server that reads it has to hold a few million parsed ranges in its own
// memory and invent its own index over them. The binary file is one file, it
// carries its own build epoch - which is what "stale" is measured against - and
// its index is part of the file, so a lookup is a walk of 32 or 128 bits with
// no table to build at start-up. Reading it directly is less code than reading
// the CSV honestly would be, and it is what the administrator has.
//
// WHAT THE FORMAT IS, in the order the bytes appear
//
//   [binary search tree][16 zero bytes][data section][marker][metadata]
//
// The METADATA is found by searching backwards from the end of the file for the
// marker "\xAB\xCD\xEFMaxMind.com" and decoding the map that follows it. It
// gives node_count, record_size (24, 28 or 32 bits), ip_version (4 or 6), the
// database_type, and build_epoch.
//
// The SEARCH TREE is node_count nodes of two records each, so it is
// node_count * record_size * 2 / 8 bytes long. A lookup walks the address one
// bit at a time from the most significant: 0 takes the left record, 1 the
// right. A record less than node_count is the next node; equal to node_count
// means the address is not in the database; greater than node_count is a
// pointer into the data section, at (record - node_count - 16) bytes from the
// start of the data section, which itself begins 16 bytes after the tree.
//
// The DATA SECTION holds typed values: a control byte whose top three bits are
// the type (0 meaning "add 7 to the next byte") and whose low five bits are the
// size, with 29, 30 and 31 meaning that the size is in the following one, two
// or three bytes. Strings, maps, arrays, unsigned integers and a pointer type
// that lets a value be shared by many networks - which is the whole reason a
// country database is a few megabytes rather than a few hundred.
//
// WHAT IT IS ASKED FOR
//
// One thing: the ISO 3166-1 alpha-2 code of the country, and its English name.
// That is country.iso_code, falling back to registered_country.iso_code for the
// records (satellite and anonymous-proxy ranges, and some GeoIP2 Enterprise
// rows) that have no represented country. A City database answers the same
// path, so an administrator who already has GeoLite2-City on the machine can
// point at that instead and it works.
//
// SAFETY
//
// The file is administrator-supplied, so it is not hostile in the way a
// connecting client is - but it is read on the accept path of every listener,
// and a malformed file must never do worse than answer "unknown". So every
// offset is bounds-checked against the buffer, pointer chasing is depth-limited
// (a file whose pointers form a loop is a file, not a hang), the tree walk
// cannot leave the tree, and nothing here throws or logs: it returns false.
// Loading validates the whole header before the file is used for anything.

#pragma once

#include <string>
#include <vector>

namespace HM
{
   class IPAddress;

   class GeoIpDatabase
   {
   public:
      GeoIpDatabase();
      ~GeoIpDatabase();

      struct Metadata
      {
         Metadata() :
            node_count(0),
            record_size(0),
            ip_version(0),
            build_epoch(0),
            binary_format_major(0),
            binary_format_minor(0)
         {
         }

         AnsiString database_type;      // "GeoLite2-Country", "GeoIP2-City", ...
         unsigned int node_count;
         unsigned int record_size;      // 24, 28 or 32
         unsigned int ip_version;       // 4 or 6
         unsigned __int64 build_epoch;  // seconds since 1970, when MaxMind built it
         unsigned int binary_format_major;
         unsigned int binary_format_minor;
      };

      // Reads the file into memory and validates its header. False, with a
      // sentence in 'error' fit to put in the application log, when the file is
      // missing, unreadable, too large, or not a MaxMind database. A failed load
      // leaves the object unloaded, so a reload that fails never replaces a
      // database that was working.
      bool LoadFile(const String &path, String &error);

      // The same from bytes already in hand. What the in-process tests use, so
      // that the format is tested without a file on disk.
      bool LoadBytes(const std::vector<unsigned char> &bytes, String &error);

      bool IsLoaded() const { return loaded_; }
      const Metadata &GetMetadata() const { return metadata_; }
      unsigned __int64 GetByteCount() const { return (unsigned __int64) bytes_.size(); }

      // The country of an address. True when the database has an answer, and
      // then isoCode is the upper-case two-letter code; false when it has none,
      // when nothing is loaded, or when the address is not one this database can
      // answer for (an IPv6 address against an IPv4-only file). Never throws.
      bool Lookup(const IPAddress &address, AnsiString &isoCode, AnsiString &countryName) const;

      // The lookup by raw address bytes: 4 for IPv4, 16 for IPv6. Public because
      // it is the seam the format tests drive.
      bool LookupBytes(const unsigned char *address, int byteCount, AnsiString &isoCode, AnsiString &countryName) const;

      // How many whole days ago MaxMind built this file, or -1 when the file
      // carries no build epoch or one in the future. What "stale" is measured
      // against - a country database more than a few weeks old is answering with
      // allocations that have since moved.
      int BuildAgeDays(time_t now) const;

      // The marker that precedes the metadata, as a string of bytes. Public so a
      // test can write a file without it and prove the refusal.
      static const char *MetadataMarker();
      static int MetadataMarkerLength();

      // The largest file that will be read, in bytes. A GeoLite2-City database is
      // about 60 MB; this is generous enough for any real one and small enough
      // that a wrong path at a disk image cannot exhaust memory.
      static const unsigned __int64 MaxFileBytes = 256ull * 1024ull * 1024ull;

   private:
      // The data-section decoder. 'offset' is absolute within bytes_.
      enum FieldType
      {
         TypeExtended = 0,
         TypePointer = 1,
         TypeUtf8String = 2,
         TypeDouble = 3,
         TypeBytes = 4,
         TypeUint16 = 5,
         TypeUint32 = 6,
         TypeMap = 7,
         TypeInt32 = 8,
         TypeUint64 = 9,
         TypeUint128 = 10,
         TypeArray = 11,
         TypeCacheContainer = 12,
         TypeEndMarker = 13,
         TypeBoolean = 14,
         TypeFloat = 15
      };

      // Reads the control byte at 'offset', leaving 'offset' on the payload.
      bool ReadControl_(size_t &offset, size_t limit, int &type, unsigned __int64 &size) const;

      // Follows a pointer field at 'offset' (which must be a pointer), returning
      // the absolute offset it names. 'base' is what its value is relative to.
      bool ReadPointer_(size_t &offset, size_t limit, size_t base, size_t &target) const;

      // Moves 'offset' past the whole value that starts there.
      bool SkipValue_(size_t &offset, size_t limit, size_t base, int depth) const;

      // In the map that starts at 'offset', finds 'key' and leaves 'valueOffset'
      // on its value. False when the field is not a map or the key is absent.
      bool FindInMap_(size_t offset, size_t limit, size_t base, const char *key, size_t &valueOffset, int depth) const;

      // The string value at 'offset', pointers followed.
      bool ReadString_(size_t offset, size_t limit, size_t base, AnsiString &value, int depth) const;

      // The unsigned integer value at 'offset', pointers followed.
      bool ReadUnsigned_(size_t offset, size_t limit, size_t base, unsigned __int64 &value, int depth) const;

      // One named member of a map, by kind. Used for the metadata, whose fields
      // are all strings and unsigned integers.
      bool ReadStringField_(size_t mapOffset, size_t limit, size_t base, const char *key, AnsiString &value) const;
      bool ReadUnsignedField_(size_t mapOffset, size_t limit, size_t base, const char *key, unsigned __int64 &value) const;

      // One record of one node of the search tree.
      bool ReadRecord_(unsigned int node, int bit, unsigned int &value) const;

      // The walk itself, from a starting node, over byteCount * 8 bits.
      bool LookupFrom_(unsigned int startNode, const unsigned char *address, int byteCount,
                       AnsiString &isoCode, AnsiString &countryName) const;

      // country.iso_code of the record at that offset, or registered_country's.
      bool ReadCountry_(size_t recordOffset, AnsiString &isoCode, AnsiString &countryName) const;

      // The node an IPv4 lookup starts at in an IPv6 database: 96 left turns.
      unsigned int Ipv4StartNode_() const;

      bool ParseMetadata_(String &error);

      bool loaded_;
      std::vector<unsigned char> bytes_;
      Metadata metadata_;

      size_t tree_bytes_;          // node_count * record_size * 2 / 8
      size_t data_section_start_;  // tree_bytes_ + 16
      size_t data_section_end_;    // where the metadata marker begins
      unsigned int ipv4_start_node_;
   };

   // The in-process format tests, run by ClassTester::DoTests. They build MMDB
   // files byte by byte - a tree, a data section, a metadata block - and read
   // them back, because an MMDB reader that is only tested against a real
   // MaxMind file is tested against one build of one database and against
   // nothing that is malformed.
   class GeoIpDatabaseTester
   {
   public:
      void Test();
   };
}
