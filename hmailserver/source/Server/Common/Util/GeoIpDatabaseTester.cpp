// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The in-process tests of the MMDB reader, and the little MMDB WRITER they need.
//
// The writer is here and not in GeoIpDatabase.cpp on purpose: the server reads
// these files and never writes one, so a writer in the file the accept path
// includes would be dead code in every shipped build. It lives beside the tests
// that are its only caller.
//
// Why a writer at all. A reader tested against one real GeoLite2 file is tested
// against one build of one database: every record has a country, every network
// is where MaxMind put it, and nothing is malformed. What actually has to be
// right is the format - the three record widths, the pointer forms, the
// separator arithmetic, the walk that has to stop, and every refusal - and the
// only way to reach those is to write the bytes. So this builds whole databases
// byte by byte and reads them back.
//
// It is also the shape the regression suite's C# fixture writes (Shared/
// GeoIpFixture.cs), so a file the tests here accept is a file that fixture can
// produce, and the two halves of the feature are tested against the same format
// rather than against each other.

#include "StdAfx.h"
#include "GeoIpDatabase.h"

#include <stdexcept>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef std::vector<unsigned char> Bytes;

      void PutControl(Bytes &out, int type, size_t size)
      {
         unsigned char first = (type < 8) ? (unsigned char) (type << 5) : 0;

         if (size < 29)
            first |= (unsigned char) size;
         else if (size < 285)
            first |= 29;
         else
            first |= 30;

         out.push_back(first);

         if (type >= 8)
            out.push_back((unsigned char) (type - 7));

         if (size >= 29 && size < 285)
         {
            out.push_back((unsigned char) (size - 29));
         }
         else if (size >= 285)
         {
            const size_t value = size - 285;
            out.push_back((unsigned char) ((value >> 8) & 0xFF));
            out.push_back((unsigned char) (value & 0xFF));
         }
      }

      void PutString(Bytes &out, const AnsiString &text)
      {
         PutControl(out, 2, text.size());
         out.insert(out.end(), text.begin(), text.end());
      }

      // A big-endian unsigned with its leading zero bytes left off, which is how
      // the format stores every unsigned. type is 5 (uint16), 6 (uint32) or 9
      // (uint64), and the reader takes the size from the control byte, so the
      // three differ only in the type they declare.
      void PutUnsigned(Bytes &out, int type, unsigned __int64 value)
      {
         unsigned char raw[8];
         int count = 0;

         for (int shift = 56; shift >= 0; shift -= 8)
         {
            const unsigned char byte = (unsigned char) ((value >> shift) & 0xFF);

            if (count == 0 && byte == 0)
               continue;

            raw[count++] = byte;
         }

         PutControl(out, type, (size_t) count);

         for (int i = 0; i < count; i++)
            out.push_back(raw[i]);
      }

      void PutMapHeader(Bytes &out, size_t pairs)
      {
         PutControl(out, 7, pairs);
      }

      void PutArrayHeader(Bytes &out, size_t items)
      {
         PutControl(out, 11, items);
      }

      // The smallest pointer form that holds the offset. The four forms exist so
      // that the common case costs two bytes, and a reader that only ever meets
      // the four-byte one has not been tested.
      void PutPointer(Bytes &out, size_t offset)
      {
         if (offset < 2048)
         {
            out.push_back((unsigned char) (0x20 | ((offset >> 8) & 0x07)));
            out.push_back((unsigned char) (offset & 0xFF));
         }
         else if (offset < 526336)
         {
            const size_t value = offset - 2048;
            out.push_back((unsigned char) (0x20 | 0x08 | ((value >> 16) & 0x07)));
            out.push_back((unsigned char) ((value >> 8) & 0xFF));
            out.push_back((unsigned char) (value & 0xFF));
         }
         else
         {
            out.push_back((unsigned char) (0x20 | 0x18));
            out.push_back((unsigned char) ((offset >> 24) & 0xFF));
            out.push_back((unsigned char) ((offset >> 16) & 0xFF));
            out.push_back((unsigned char) ((offset >> 8) & 0xFF));
            out.push_back((unsigned char) (offset & 0xFF));
         }
      }

      // One node of the tree being built. A record is one of three things - the
      // index of another node, an offset in the data section, or nothing - which
      // is exactly what the format's three record cases are.
      struct BuildNode
      {
         BuildNode()
         {
            for (int i = 0; i < 2; i++)
            {
               child[i] = -1;
               data[i] = -1;
            }
         }

         int child[2];
         __int64 data[2];
      };

      class MmdbBuilder
      {
      public:
         MmdbBuilder(unsigned int recordSize, unsigned int ipVersion) :
            record_size_(recordSize),
            ip_version_(ipVersion)
         {
            nodes_.push_back(BuildNode());
         }

         // A country record, as a GeoLite2-Country file holds one. Returns its
         // offset in the data section, which is what a network is pointed at.
         size_t AddCountry(const AnsiString &iso, const AnsiString &name, const char *section = "country")
         {
            const size_t at = data_.size();

            PutMapHeader(data_, 1);
            PutString(data_, section);
            PutMapHeader(data_, 2);
            PutString(data_, "iso_code");
            PutString(data_, iso);
            PutString(data_, "names");
            PutMapHeader(data_, 1);
            PutString(data_, "en");
            PutString(data_, name);

            return at;
         }

         // A value that is a pointer to another value. Two networks that share a
         // country in a real file share the record itself; this is the other
         // thing a real file does, and the only way the pointer decoder is
         // exercised at all.
         size_t AddPointerTo(size_t target)
         {
            const size_t at = data_.size();
            PutPointer(data_, target);
            return at;
         }

         size_t AddRaw(const Bytes &raw)
         {
            const size_t at = data_.size();
            data_.insert(data_.end(), raw.begin(), raw.end());
            return at;
         }

         void AddNetwork(const unsigned char *address, int byteCount, int prefixLength, size_t dataOffset)
         {
            int node = 0;

            for (int i = 0; i < prefixLength; i++)
            {
               const int bit = (address[i / 8] >> (7 - (i % 8))) & 1;

               if (i == prefixLength - 1)
               {
                  nodes_[node].data[bit] = (__int64) dataOffset;
                  return;
               }

               if (nodes_[node].child[bit] < 0)
               {
                  nodes_.push_back(BuildNode());
                  nodes_[node].child[bit] = (int) nodes_.size() - 1;
               }

               node = nodes_[node].child[bit];
            }
         }

         // An IPv4 network, placed where the database's own version puts it: at
         // the root of a version 4 file, and under ::/96 in a version 6 one,
         // which is where MaxMind's writer puts it.
         void AddIpv4Network(unsigned char a, unsigned char b, unsigned char c, unsigned char d, int prefixLength, size_t dataOffset)
         {
            if (ip_version_ == 4)
            {
               unsigned char raw[4] = { a, b, c, d };
               AddNetwork(raw, 4, prefixLength, dataOffset);
               return;
            }

            unsigned char raw[16];
            memset(raw, 0, sizeof(raw));
            raw[12] = a;
            raw[13] = b;
            raw[14] = c;
            raw[15] = d;

            AddNetwork(raw, 16, 96 + prefixLength, dataOffset);
         }

         Bytes Build(const AnsiString &databaseType, unsigned __int64 buildEpoch) const
         {
            const unsigned int nodeCount = (unsigned int) nodes_.size();
            Bytes out;

            for (size_t i = 0; i < nodes_.size(); i++)
            {
               unsigned int record[2];

               for (int side = 0; side < 2; side++)
               {
                  if (nodes_[i].data[side] >= 0)
                     record[side] = (unsigned int) (nodeCount + 16 + nodes_[i].data[side]);
                  else if (nodes_[i].child[side] >= 0)
                     record[side] = (unsigned int) nodes_[i].child[side];
                  else
                     record[side] = nodeCount;
               }

               WriteNode(out, record[0], record[1]);
            }

            for (int i = 0; i < 16; i++)
               out.push_back(0);

            out.insert(out.end(), data_.begin(), data_.end());

            const char *marker = GeoIpDatabase::MetadataMarker();
            for (int i = 0; i < GeoIpDatabase::MetadataMarkerLength(); i++)
               out.push_back((unsigned char) marker[i]);

            Bytes metadata;
            PutMapHeader(metadata, 8);
            PutString(metadata, "node_count");
            PutUnsigned(metadata, 6, nodeCount);
            PutString(metadata, "record_size");
            PutUnsigned(metadata, 5, record_size_);
            PutString(metadata, "ip_version");
            PutUnsigned(metadata, 5, ip_version_);
            PutString(metadata, "database_type");
            PutString(metadata, databaseType);
            PutString(metadata, "languages");
            PutArrayHeader(metadata, 1);
            PutString(metadata, "en");
            PutString(metadata, "binary_format_major_version");
            PutUnsigned(metadata, 5, 2);
            PutString(metadata, "binary_format_minor_version");
            PutUnsigned(metadata, 5, 0);
            PutString(metadata, "build_epoch");
            PutUnsigned(metadata, 9, buildEpoch);

            out.insert(out.end(), metadata.begin(), metadata.end());

            return out;
         }

      private:
         void WriteNode(Bytes &out, unsigned int left, unsigned int right) const
         {
            if (record_size_ == 24)
            {
               out.push_back((unsigned char) ((left >> 16) & 0xFF));
               out.push_back((unsigned char) ((left >> 8) & 0xFF));
               out.push_back((unsigned char) (left & 0xFF));
               out.push_back((unsigned char) ((right >> 16) & 0xFF));
               out.push_back((unsigned char) ((right >> 8) & 0xFF));
               out.push_back((unsigned char) (right & 0xFF));
               return;
            }

            if (record_size_ == 28)
            {
               out.push_back((unsigned char) ((left >> 16) & 0xFF));
               out.push_back((unsigned char) ((left >> 8) & 0xFF));
               out.push_back((unsigned char) (left & 0xFF));
               out.push_back((unsigned char) ((((left >> 24) & 0x0F) << 4) | ((right >> 24) & 0x0F)));
               out.push_back((unsigned char) ((right >> 16) & 0xFF));
               out.push_back((unsigned char) ((right >> 8) & 0xFF));
               out.push_back((unsigned char) (right & 0xFF));
               return;
            }

            out.push_back((unsigned char) ((left >> 24) & 0xFF));
            out.push_back((unsigned char) ((left >> 16) & 0xFF));
            out.push_back((unsigned char) ((left >> 8) & 0xFF));
            out.push_back((unsigned char) (left & 0xFF));
            out.push_back((unsigned char) ((right >> 24) & 0xFF));
            out.push_back((unsigned char) ((right >> 16) & 0xFF));
            out.push_back((unsigned char) ((right >> 8) & 0xFF));
            out.push_back((unsigned char) (right & 0xFF));
         }

         unsigned int record_size_;
         unsigned int ip_version_;
         std::vector<BuildNode> nodes_;
         Bytes data_;
      };

      void Fail(const char *what)
      {
         throw std::logic_error(what);
      }

      // A failure whose message carries the reader's own sentence, so that a
      // test that stops says why rather than only where.
      void FailWith(const char *what, const String &reason)
      {
         AnsiString message = what;
         message += AnsiString(reason);

         throw std::logic_error(message.c_str());
      }

      // The country of a dotted-quad, as the database answers it.
      AnsiString CountryOf(const GeoIpDatabase &database, unsigned char a, unsigned char b, unsigned char c, unsigned char d)
      {
         unsigned char raw[4] = { a, b, c, d };
         AnsiString iso;
         AnsiString name;

         if (!database.LookupBytes(raw, 4, iso, name))
            return "";

         return iso;
      }
   }

   void
   GeoIpDatabaseTester::Test()
   {
      // ------------------------------------------------------------------
      // A version 4 database with 24-bit records: the smallest real shape.
      // ------------------------------------------------------------------
      {
         MmdbBuilder builder(24, 4);

         const size_t australia = builder.AddCountry("AU", "Australia");
         const size_t unitedStates = builder.AddCountry("US", "United States");
         const size_t france = builder.AddCountry("FR", "France");

         builder.AddIpv4Network(1, 0, 0, 0, 8, australia);
         builder.AddIpv4Network(8, 8, 8, 0, 24, unitedStates);
         builder.AddIpv4Network(2, 2, 0, 0, 16, france);

         // Two networks sharing one record, as a real file does for every
         // allocation of the same country.
         builder.AddIpv4Network(3, 3, 0, 0, 16, france);

         const std::vector<unsigned char> bytes = builder.Build("GeoLite2-Country", 1750000000);

         GeoIpDatabase database;
         String error;

         if (!database.LoadBytes(bytes, error))
            FailWith("A database this test wrote was refused: ", error);

         if (database.GetMetadata().database_type != "GeoLite2-Country")
            Fail("The database type was not read back from the metadata.");

         if (database.GetMetadata().record_size != 24 || database.GetMetadata().ip_version != 4)
            Fail("The record size or IP version was not read back from the metadata.");

         if (database.GetMetadata().build_epoch != 1750000000)
            Fail("The build epoch was not read back from the metadata.");

         if (CountryOf(database, 1, 2, 3, 4) != "AU")
            Fail("An address inside 1.0.0.0/8 did not answer AU.");

         if (CountryOf(database, 8, 8, 8, 8) != "US")
            Fail("8.8.8.8 did not answer US.");

         if (CountryOf(database, 8, 8, 9, 1) != "")
            Fail("An address outside 8.8.8.0/24 answered as though it were inside it.");

         if (CountryOf(database, 2, 2, 250, 1) != "FR")
            Fail("An address inside 2.2.0.0/16 did not answer FR.");

         if (CountryOf(database, 3, 3, 1, 1) != "FR")
            Fail("A second network sharing one record did not answer the shared country.");

         if (CountryOf(database, 9, 9, 9, 9) != "")
            Fail("An address in no network answered a country.");

         AnsiString iso;
         AnsiString name;
         unsigned char google[4] = { 8, 8, 8, 8 };

         if (!database.LookupBytes(google, 4, iso, name) || name != "United States")
            Fail("The English country name was not read back.");

         // An IPv6 address against an IPv4-only file is unanswerable, and saying
         // so is what lets the policy treat it as unknown.
         unsigned char v6[16];
         memset(v6, 0, sizeof(v6));
         v6[0] = 0x20;
         v6[1] = 0x01;

         if (database.LookupBytes(v6, 16, iso, name))
            Fail("An IPv6 address was answered by a database that declares IP version 4.");

         if (database.BuildAgeDays(1750000000 + 10 * 24 * 60 * 60) != 10)
            Fail("The build age in days was not computed from the build epoch.");

         if (database.BuildAgeDays(1750000000 - 60) != 0)
            Fail("A build epoch in the future did not read as an age of zero.");
      }

      // ------------------------------------------------------------------
      // A version 6 database with 28-bit records - the width GeoLite2 ships -
      // holding both IPv4 (under ::/96) and IPv6 networks.
      // ------------------------------------------------------------------
      {
         MmdbBuilder builder(28, 6);

         const size_t germany = builder.AddCountry("DE", "Germany");
         const size_t unitedStates = builder.AddCountry("US", "United States");

         // A record reached through a pointer rather than directly: the form a
         // real file uses everywhere, and the one an untested reader gets wrong.
         const size_t pointerToGermany = builder.AddPointerTo(germany);

         builder.AddIpv4Network(8, 8, 8, 0, 24, unitedStates);
         builder.AddIpv4Network(5, 0, 0, 0, 8, pointerToGermany);

         unsigned char documentation[16];
         memset(documentation, 0, sizeof(documentation));
         documentation[0] = 0x20;
         documentation[1] = 0x01;
         documentation[2] = 0x0D;
         documentation[3] = 0xB8;

         builder.AddNetwork(documentation, 16, 32, germany);

         const std::vector<unsigned char> bytes = builder.Build("GeoLite2-Country", 1750000000);

         GeoIpDatabase database;
         String error;

         if (!database.LoadBytes(bytes, error))
            FailWith("A version 6 database this test wrote was refused: ", error);

         if (CountryOf(database, 8, 8, 8, 8) != "US")
            Fail("An IPv4 address was not found under ::/96 in a version 6 database.");

         if (CountryOf(database, 5, 1, 2, 3) != "DE")
            Fail("A record reached through a pointer did not answer its country.");

         AnsiString iso;
         AnsiString name;

         unsigned char address[16];
         memcpy(address, documentation, 16);
         address[15] = 1;

         if (!database.LookupBytes(address, 16, iso, name) || iso != "DE")
            Fail("An IPv6 address inside 2001:db8::/32 did not answer DE.");

         address[3] = 0xB9;

         if (database.LookupBytes(address, 16, iso, name))
            Fail("An IPv6 address outside every network answered a country.");
      }

      // ------------------------------------------------------------------
      // 32-bit records, and a record that has only a registered country.
      // ------------------------------------------------------------------
      {
         MmdbBuilder builder(32, 4);

         const size_t registered = builder.AddCountry("SE", "Sweden", "registered_country");

         builder.AddIpv4Network(203, 0, 113, 0, 24, registered);

         const std::vector<unsigned char> bytes = builder.Build("GeoIP2-Country", 1750000000);

         GeoIpDatabase database;
         String error;

         if (!database.LoadBytes(bytes, error))
            FailWith("A 32-bit-record database was refused: ", error);

         if (CountryOf(database, 203, 0, 113, 9) != "SE")
            Fail("A record carrying only registered_country did not fall back to it.");
      }

      // ------------------------------------------------------------------
      // Every refusal. A file that is not a database, one whose tree does not
      // fit in it, one whose record size is not a record size - each is a
      // sentence in the application log and never an exception on an accept
      // path.
      // ------------------------------------------------------------------
      {
         GeoIpDatabase database;
         String error;

         std::vector<unsigned char> empty;

         if (database.LoadBytes(empty, error))
            Fail("An empty file was accepted as a database.");

         if (error.IsEmpty())
            Fail("An empty file was refused without a reason.");

         std::vector<unsigned char> text(200, 'x');

         if (database.LoadBytes(text, error))
            Fail("A file with no MaxMind marker was accepted as a database.");

         if (error.find(_T("marker")) == String::npos)
            Fail("A file with no marker was refused without saying so.");
      }

      {
         MmdbBuilder builder(24, 4);
         const size_t australia = builder.AddCountry("AU", "Australia");
         builder.AddIpv4Network(1, 0, 0, 0, 8, australia);

         std::vector<unsigned char> bytes = builder.Build("GeoLite2-Country", 1750000000);

         // The front of the file removed, with the marker and the metadata left
         // where they are: the tree the metadata describes is now longer than
         // the file that is supposed to hold it, which is what a download that
         // stopped part way looks like from the reader's side.
         std::vector<unsigned char> truncated(bytes.begin() + 40, bytes.end());

         GeoIpDatabase database;
         String error;

         if (database.LoadBytes(truncated, error))
            Fail("A database whose search tree is longer than the file was accepted.");

         if (error.find(_T("search tree")) == String::npos)
            Fail("A database whose search tree is longer than the file was refused without saying so.");

         // And the same database, whole, still loads - so the test above proves
         // the truncation was refused rather than that the writer is broken.
         if (!database.LoadBytes(bytes, error))
            FailWith("The whole database was refused: ", error);

         // A loaded database that is then handed a bad one keeps working: a
         // reload that fails must never take the policy's data away.
         String second;
         std::vector<unsigned char> rubbish(300, 'z');

         if (database.LoadBytes(rubbish, second))
            Fail("Rubbish was accepted as a database.");

         if (!database.IsLoaded() || CountryOf(database, 1, 2, 3, 4) != "AU")
            Fail("A failed reload discarded the database that was already loaded.");
      }

      // ------------------------------------------------------------------
      // A record pointer that lands past the end of the data section. Nothing
      // writes such a file on purpose; a partially-downloaded one is exactly
      // this, and it must answer "no country" rather than read past the buffer.
      // ------------------------------------------------------------------
      {
         MmdbBuilder builder(24, 4);
         const size_t australia = builder.AddCountry("AU", "Australia");
         builder.AddIpv4Network(1, 0, 0, 0, 8, australia);

         std::vector<unsigned char> bytes = builder.Build("GeoLite2-Country", 1750000000);

         // The root node's left record is the first three bytes of the file.
         bytes[0] = 0xFF;
         bytes[1] = 0xFF;
         bytes[2] = 0xFF;

         GeoIpDatabase database;
         String error;

         if (!database.LoadBytes(bytes, error))
            FailWith("A database with one bad record was refused at load: ", error);

         if (CountryOf(database, 1, 2, 3, 4) != "")
            Fail("A record pointing past the data section answered a country.");
      }
   }
}
