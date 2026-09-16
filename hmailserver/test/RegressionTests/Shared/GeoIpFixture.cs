// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    Writes a MaxMind database (.mmdb) with known ranges in it, so that the
   ///    country blocking tests have a geographic database without one being
   ///    committed to this repository.
   ///
   ///    Nothing here is a copy of anything MaxMind distributes: it is the FORMAT,
   ///    written from its specification, filled with networks a test chose. That
   ///    matters twice over - the licence of a real GeoLite2 file has no business
   ///    in a source tree, and a test that needed one could not say what the
   ///    answer for an address ought to be.
   ///
   ///    The shape it writes, which is the shape GeoLite2-Country has: an IP
   ///    version 6 database with 28-bit records, holding the IPv4 space under
   ///    ::/96 where MaxMind's own writer puts it. The server's own tests
   ///    (GeoIpDatabaseTester, run through Utilities.RunTestSuite) cover the other
   ///    two record widths, the pointer forms and every refusal; this one exists
   ///    so that a test can point the running server at a file and connect to it.
   ///
   ///    The format, in the order the bytes come out:
   ///       [search tree][16 zero bytes][data section][marker][metadata]
   ///    A record in the tree is the next node when it is below node_count, "not
   ///    found" when it equals it, and a data-section offset of
   ///    (record - node_count - 16) when it is above it.
   /// </summary>
   public static class GeoIpFixture
   {
      /// <summary>"\xAB\xCD\xEF" then "MaxMind.com", the bytes the metadata follows.</summary>
      private static readonly byte[] Marker =
      {
         0xAB, 0xCD, 0xEF, (byte) 'M', (byte) 'a', (byte) 'x', (byte) 'M', (byte) 'i',
         (byte) 'n', (byte) 'd', (byte) '.', (byte) 'c', (byte) 'o', (byte) 'm'
      };

      /// <summary>
      ///    Writes a country database holding the given networks and nothing else.
      ///    Each network is "a.b.c.d/prefix" against a two-letter country code;
      ///    the English name written into the record is the code twice over, which
      ///    is enough for a test to tell the name apart from the code.
      /// </summary>
      public static void WriteCountryDatabase(string path, IDictionary<string, string> networks)
      {
         WriteCountryDatabase(path, networks, DateTimeOffset.UtcNow.ToUnixTimeSeconds());
      }

      /// <summary>
      ///    The same, with the build date the file declares. A test that wants a
      ///    stale database passes an epoch far enough back.
      /// </summary>
      public static void WriteCountryDatabase(string path, IDictionary<string, string> networks, long buildEpoch)
      {
         var builder = new Builder(28, 6);
         var records = new Dictionary<string, int>();

         foreach (KeyValuePair<string, string> network in networks)
         {
            string country = network.Value.ToUpperInvariant();

            if (!records.ContainsKey(country))
               records[country] = builder.AddCountry(country, "Country " + country);

            builder.AddIpv4Network(network.Key, records[country]);
         }

         string directory = Path.GetDirectoryName(path);
         if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
            Directory.CreateDirectory(directory);

         File.WriteAllBytes(path, builder.Build("GeoLite2-Country", buildEpoch));
      }

      /// <summary>One node of the tree being built: each record is a node, a data offset, or nothing.</summary>
      private sealed class Node
      {
         public readonly int[] Child = { -1, -1 };
         public readonly int[] Data = { -1, -1 };
      }

      public sealed class Builder
      {
         private readonly int _recordSize;
         private readonly int _ipVersion;
         private readonly List<Node> _nodes = new List<Node>();
         private readonly List<byte> _data = new List<byte>();

         public Builder(int recordSize, int ipVersion)
         {
            if (recordSize != 24 && recordSize != 28 && recordSize != 32)
               throw new ArgumentException("A record is 24, 28 or 32 bits.", nameof(recordSize));

            _recordSize = recordSize;
            _ipVersion = ipVersion;
            _nodes.Add(new Node());
         }

         /// <summary>A country record, and its offset in the data section.</summary>
         public int AddCountry(string isoCode, string name)
         {
            int at = _data.Count;

            PutControl(_data, 7, 1);               // { "country": ... }
            PutString(_data, "country");
            PutControl(_data, 7, 2);               // { "iso_code": ..., "names": ... }
            PutString(_data, "iso_code");
            PutString(_data, isoCode);
            PutString(_data, "names");
            PutControl(_data, 7, 1);               // { "en": ... }
            PutString(_data, "en");
            PutString(_data, name);

            return at;
         }

         /// <summary>"a.b.c.d/prefix", placed where this database's version keeps IPv4.</summary>
         public void AddIpv4Network(string cidr, int dataOffset)
         {
            int slash = cidr.IndexOf('/');
            if (slash < 0)
               throw new ArgumentException("A network is written as a.b.c.d/prefix.", nameof(cidr));

            string[] parts = cidr.Substring(0, slash).Split('.');
            if (parts.Length != 4)
               throw new ArgumentException("A network is written as a.b.c.d/prefix.", nameof(cidr));

            int prefix = int.Parse(cidr.Substring(slash + 1));
            if (prefix < 1 || prefix > 32)
               throw new ArgumentException("An IPv4 prefix is 1 to 32 bits.", nameof(cidr));

            var four = new byte[4];
            for (int i = 0; i < 4; i++)
               four[i] = byte.Parse(parts[i]);

            if (_ipVersion == 4)
            {
               AddNetwork(four, prefix, dataOffset);
               return;
            }

            // Under ::/96, which is where MaxMind's writer puts the IPv4 space in
            // an IP version 6 database.
            var mapped = new byte[16];
            Array.Copy(four, 0, mapped, 12, 4);

            AddNetwork(mapped, 96 + prefix, dataOffset);
         }

         public void AddNetwork(byte[] address, int prefixLength, int dataOffset)
         {
            int node = 0;

            for (int i = 0; i < prefixLength; i++)
            {
               int bit = (address[i / 8] >> (7 - (i % 8))) & 1;

               if (i == prefixLength - 1)
               {
                  _nodes[node].Data[bit] = dataOffset;
                  return;
               }

               if (_nodes[node].Child[bit] < 0)
               {
                  _nodes.Add(new Node());
                  _nodes[node].Child[bit] = _nodes.Count - 1;
               }

               node = _nodes[node].Child[bit];
            }
         }

         public byte[] Build(string databaseType, long buildEpoch)
         {
            int nodeCount = _nodes.Count;
            var output = new List<byte>();

            foreach (Node node in _nodes)
            {
               var record = new uint[2];

               for (int side = 0; side < 2; side++)
               {
                  if (node.Data[side] >= 0)
                     record[side] = (uint) (nodeCount + 16 + node.Data[side]);
                  else if (node.Child[side] >= 0)
                     record[side] = (uint) node.Child[side];
                  else
                     record[side] = (uint) nodeCount;
               }

               WriteNode(output, record[0], record[1]);
            }

            for (int i = 0; i < 16; i++)
               output.Add(0);

            output.AddRange(_data);
            output.AddRange(Marker);

            var metadata = new List<byte>();
            PutControl(metadata, 7, 8);
            PutString(metadata, "node_count");
            PutUnsigned(metadata, 6, (ulong) nodeCount);
            PutString(metadata, "record_size");
            PutUnsigned(metadata, 5, (ulong) _recordSize);
            PutString(metadata, "ip_version");
            PutUnsigned(metadata, 5, (ulong) _ipVersion);
            PutString(metadata, "database_type");
            PutString(metadata, databaseType);
            PutString(metadata, "languages");
            PutControl(metadata, 11, 1);
            PutString(metadata, "en");
            PutString(metadata, "binary_format_major_version");
            PutUnsigned(metadata, 5, 2);
            PutString(metadata, "binary_format_minor_version");
            PutUnsigned(metadata, 5, 0);
            PutString(metadata, "build_epoch");
            PutUnsigned(metadata, 9, (ulong) buildEpoch);

            output.AddRange(metadata);

            return output.ToArray();
         }

         private void WriteNode(List<byte> output, uint left, uint right)
         {
            if (_recordSize == 24)
            {
               output.Add((byte) ((left >> 16) & 0xFF));
               output.Add((byte) ((left >> 8) & 0xFF));
               output.Add((byte) (left & 0xFF));
               output.Add((byte) ((right >> 16) & 0xFF));
               output.Add((byte) ((right >> 8) & 0xFF));
               output.Add((byte) (right & 0xFF));
               return;
            }

            if (_recordSize == 28)
            {
               // The middle byte carries the top nibble of each record: the high
               // nibble is the left record's, the low nibble the right record's.
               output.Add((byte) ((left >> 16) & 0xFF));
               output.Add((byte) ((left >> 8) & 0xFF));
               output.Add((byte) (left & 0xFF));
               output.Add((byte) ((((left >> 24) & 0x0F) << 4) | ((right >> 24) & 0x0F)));
               output.Add((byte) ((right >> 16) & 0xFF));
               output.Add((byte) ((right >> 8) & 0xFF));
               output.Add((byte) (right & 0xFF));
               return;
            }

            output.Add((byte) ((left >> 24) & 0xFF));
            output.Add((byte) ((left >> 16) & 0xFF));
            output.Add((byte) ((left >> 8) & 0xFF));
            output.Add((byte) (left & 0xFF));
            output.Add((byte) ((right >> 24) & 0xFF));
            output.Add((byte) ((right >> 16) & 0xFF));
            output.Add((byte) ((right >> 8) & 0xFF));
            output.Add((byte) (right & 0xFF));
         }

         // The control byte: the type in the top three bits (0 meaning "add 7 to
         // the next byte"), the size in the low five, with 29 and 30 meaning that
         // the size is in the bytes that follow.
         private static void PutControl(List<byte> output, int type, int size)
         {
            byte first = type < 8 ? (byte) (type << 5) : (byte) 0;

            if (size < 29)
               first |= (byte) size;
            else if (size < 285)
               first |= 29;
            else
               first |= 30;

            output.Add(first);

            if (type >= 8)
               output.Add((byte) (type - 7));

            if (size >= 29 && size < 285)
            {
               output.Add((byte) (size - 29));
            }
            else if (size >= 285)
            {
               int value = size - 285;
               output.Add((byte) ((value >> 8) & 0xFF));
               output.Add((byte) (value & 0xFF));
            }
         }

         private static void PutString(List<byte> output, string text)
         {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            PutControl(output, 2, bytes.Length);
            output.AddRange(bytes);
         }

         // Big-endian with the leading zero bytes left off, which is how the
         // format stores every unsigned; the type says which unsigned it is and
         // the control byte's size says how many bytes came.
         private static void PutUnsigned(List<byte> output, int type, ulong value)
         {
            var raw = new List<byte>();

            for (int shift = 56; shift >= 0; shift -= 8)
            {
               byte b = (byte) ((value >> shift) & 0xFF);

               if (raw.Count == 0 && b == 0)
                  continue;

               raw.Add(b);
            }

            PutControl(output, type, raw.Count);
            output.AddRange(raw);
         }
      }
   }
}
