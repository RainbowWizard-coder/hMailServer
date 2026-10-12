// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "Idna.h"
#include "IdnaTables.h"

#include <algorithm>
#include <utility>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace Idna
   {
      namespace
      {
         const unsigned int lastCodePoint = 0x10FFFF;
         const unsigned int fullStop = 0x2E;
         const unsigned int hyphen = 0x2D;
         const unsigned int zeroWidthNonJoiner = 0x200C;
         const unsigned int zeroWidthJoiner = 0x200D;
         const unsigned int capitalSharpS = 0x1E9E;

         // Canonical_Combining_Class Virama, which RFC 5892's CONTEXTJ rules
         // name.
         const unsigned int viramaClass = 9;

         // UAX #15's Hangul syllables, decomposed and composed by arithmetic.
         const unsigned int hangulSBase = 0xAC00, hangulLBase = 0x1100, hangulVBase = 0x1161, hangulTBase = 0x11A7;
         const unsigned int hangulLCount = 19, hangulVCount = 21, hangulTCount = 28;
         const unsigned int hangulNCount = hangulVCount * hangulTCount;
         const unsigned int hangulSCount = hangulLCount * hangulNCount;

         // RFC 3492's bootstring parameters for Punycode (section 5), and the
         // largest integer it computes with: a signed 32-bit one's, as the RFC's
         // sample code and the URL implementations have it.
         const unsigned int punycodeBase = 36, punycodeTMin = 1, punycodeTMax = 26, punycodeSkew = 38, punycodeDamp = 700;
         const unsigned int punycodeInitialBias = 72, punycodeInitialN = 0x80;
         const unsigned int punycodeMaxInt = 0x7FFFFFFF;

         const IdnaTables::MappingRange disallowedBeyondUnicode = { lastCodePoint + 1, 0xFFFFFFFF, IdnaTables::StatusDisallowed, 0, 0 };

         const IdnaTables::MappingRange &MappingOf_(unsigned int codePoint)
         {
            if (codePoint > lastCodePoint)
               return disallowedBeyondUnicode;

            // The last range that begins at or before the code point; the table
            // covers U+0000..U+10FFFF without a gap.
            size_t low = 0;
            size_t high = IdnaTables::MappingRangeCount;
            while (high - low > 1)
            {
               const size_t middle = low + (high - low) / 2;
               if (IdnaTables::MappingRanges[middle].first <= codePoint)
                  low = middle;
               else
                  high = middle;
            }

            return IdnaTables::MappingRanges[low];
         }

         unsigned int StatusOf_(unsigned int codePoint)
         {
            return MappingOf_(codePoint).status;
         }

         unsigned int ValueOf_(const IdnaTables::ValueRange *ranges, size_t count, unsigned int codePoint)
         {
            size_t low = 0;
            size_t high = count;
            while (low < high)
            {
               const size_t middle = low + (high - low) / 2;
               if (ranges[middle].last < codePoint)
                  low = middle + 1;
               else if (ranges[middle].first > codePoint)
                  high = middle;
               else
                  return ranges[middle].value;
            }

            return 0;
         }

         unsigned int CombiningClass_(unsigned int codePoint)
         {
            if (codePoint < 0x300)
               return 0;
            return ValueOf_(IdnaTables::CombiningClasses, IdnaTables::CombiningClassCount, codePoint);
         }

         bool IsMark_(unsigned int codePoint)
         {
            return ValueOf_(IdnaTables::Marks, IdnaTables::MarkCount, codePoint) != 0;
         }

         unsigned int BidiClass_(unsigned int codePoint)
         {
            return ValueOf_(IdnaTables::BidiClasses, IdnaTables::BidiClassCount, codePoint);
         }

         unsigned int JoiningType_(unsigned int codePoint)
         {
            return ValueOf_(IdnaTables::JoiningTypes, IdnaTables::JoiningTypeCount, codePoint);
         }

         const IdnaTables::Decomposition *DecompositionOf_(unsigned int codePoint)
         {
            const IdnaTables::Decomposition *begin = IdnaTables::Decompositions;
            const IdnaTables::Decomposition *end = begin + IdnaTables::DecompositionCount;
            const IdnaTables::Decomposition *found = std::lower_bound(begin, end, codePoint,
               [](const IdnaTables::Decomposition &entry, unsigned int value) { return entry.codePoint < value; });

            if (found == end || found->codePoint != codePoint)
               return nullptr;
            return found;
         }

         bool ComposePair_(unsigned int first, unsigned int second, unsigned int &composite)
         {
            if (first >= hangulLBase && first < hangulLBase + hangulLCount && second >= hangulVBase && second < hangulVBase + hangulVCount)
            {
               composite = hangulSBase + ((first - hangulLBase) * hangulVCount + (second - hangulVBase)) * hangulTCount;
               return true;
            }

            if (first >= hangulSBase && first < hangulSBase + hangulSCount && (first - hangulSBase) % hangulTCount == 0 &&
                second > hangulTBase && second < hangulTBase + hangulTCount)
            {
               composite = first + (second - hangulTBase);
               return true;
            }

            const IdnaTables::Composition *begin = IdnaTables::Compositions;
            const IdnaTables::Composition *end = begin + IdnaTables::CompositionCount;
            const IdnaTables::Composition *found = std::lower_bound(begin, end, std::make_pair(first, second),
               [](const IdnaTables::Composition &entry, const std::pair<unsigned int, unsigned int> &value)
               {
                  return entry.first < value.first || (entry.first == value.first && entry.second < value.second);
               });

            if (found == end || found->first != first || found->second != second)
               return false;

            composite = found->composite;
            return true;
         }

         // The canonical decomposition of text, in canonical order (UAX #15):
         // every run of non-starters sorted by combining class, stably, in time
         // n log n however long the run.
         void Decompose_(const CodePoints &text, CodePoints &decomposed)
         {
            decomposed.clear();
            decomposed.reserve(text.size());

            for (unsigned int codePoint : text)
            {
               if (codePoint >= hangulSBase && codePoint < hangulSBase + hangulSCount)
               {
                  const unsigned int index = codePoint - hangulSBase;
                  decomposed.push_back(hangulLBase + index / hangulNCount);
                  decomposed.push_back(hangulVBase + (index % hangulNCount) / hangulTCount);
                  if (index % hangulTCount != 0)
                     decomposed.push_back(hangulTBase + index % hangulTCount);
                  continue;
               }

               const IdnaTables::Decomposition *decomposition = DecompositionOf_(codePoint);
               if (decomposition == nullptr)
               {
                  decomposed.push_back(codePoint);
                  continue;
               }

               for (unsigned int i = 0; i < decomposition->length; i++)
                  decomposed.push_back(IdnaTables::DecompositionData[decomposition->offset + i]);
            }

            std::vector<std::pair<unsigned int, unsigned int> > run;
            size_t i = 0;
            while (i < decomposed.size())
            {
               if (CombiningClass_(decomposed[i]) == 0)
               {
                  i++;
                  continue;
               }

               size_t end = i;
               run.clear();
               while (end < decomposed.size())
               {
                  const unsigned int combiningClass = CombiningClass_(decomposed[end]);
                  if (combiningClass == 0)
                     break;
                  run.push_back(std::make_pair(combiningClass, decomposed[end]));
                  end++;
               }

               if (run.size() > 1)
               {
                  std::stable_sort(run.begin(), run.end(),
                     [](const std::pair<unsigned int, unsigned int> &a, const std::pair<unsigned int, unsigned int> &b)
                     {
                        return a.first < b.first;
                     });

                  for (size_t j = 0; j < run.size(); j++)
                     decomposed[i + j] = run[j].second;
               }

               i = end;
            }
         }

         // UAX #15's canonical composition, in place.
         void Compose_(CodePoints &text)
         {
            if (text.empty())
               return;

            size_t starter = 0;
            unsigned int lastClass = CombiningClass_(text[0]);
            if (lastClass != 0)
               lastClass = 256;

            size_t written = 1;
            for (size_t i = 1; i < text.size(); i++)
            {
               const unsigned int codePoint = text[i];
               const unsigned int combiningClass = CombiningClass_(codePoint);

               unsigned int composite = 0;
               if ((lastClass < combiningClass || lastClass == 0) && ComposePair_(text[starter], codePoint, composite))
               {
                  text[starter] = composite;
                  continue;
               }

               if (combiningClass == 0)
                  starter = written;

               lastClass = combiningClass;
               text[written++] = codePoint;
            }

            text.resize(written);
         }

         bool IsAscii_(const CodePoints &text)
         {
            for (unsigned int codePoint : text)
               if (codePoint >= 0x80)
                  return false;
            return true;
         }

         bool StartsWithXn_(const CodePoints &label)
         {
            return label.size() >= 4 && label[0] == 'x' && label[1] == 'n' && label[2] == hyphen && label[3] == hyphen;
         }

         std::string AsciiOf_(const CodePoints &label)
         {
            std::string ascii;
            ascii.reserve(label.size());
            for (unsigned int codePoint : label)
               ascii += static_cast<char>(codePoint);
            return ascii;
         }

         bool IsRightToLeft_(unsigned int bidiClass)
         {
            return bidiClass == IdnaTables::BidiR || bidiClass == IdnaTables::BidiAL || bidiClass == IdnaTables::BidiAN;
         }

         // RFC 5893 section 2, the six conditions, for one label of a bidi
         // domain name.
         bool SatisfiesBidiRule_(const CodePoints &label)
         {
            if (label.empty())
               return true;

            const unsigned int first = BidiClass_(label[0]);
            const bool rightToLeft = first == IdnaTables::BidiR || first == IdnaTables::BidiAL;

            // 1. The first character is L, R or AL.
            if (!rightToLeft && first != IdnaTables::BidiL)
               return false;

            bool hasEuropeanNumber = false;
            bool hasArabicNumber = false;
            unsigned int lastNonMark = first;

            for (unsigned int codePoint : label)
            {
               const unsigned int bidiClass = BidiClass_(codePoint);

               if (rightToLeft)
               {
                  // 2. Only R, AL, AN, EN, ES, CS, ET, ON, BN and NSM.
                  switch (bidiClass)
                  {
                  case IdnaTables::BidiR: case IdnaTables::BidiAL: case IdnaTables::BidiAN: case IdnaTables::BidiEN:
                  case IdnaTables::BidiES: case IdnaTables::BidiCS: case IdnaTables::BidiET: case IdnaTables::BidiON:
                  case IdnaTables::BidiBN: case IdnaTables::BidiNSM:
                     break;
                  default:
                     return false;
                  }
               }
               else
               {
                  // 5. Only L, EN, ES, CS, ET, ON, BN and NSM.
                  switch (bidiClass)
                  {
                  case IdnaTables::BidiL: case IdnaTables::BidiEN: case IdnaTables::BidiES: case IdnaTables::BidiCS:
                  case IdnaTables::BidiET: case IdnaTables::BidiON: case IdnaTables::BidiBN: case IdnaTables::BidiNSM:
                     break;
                  default:
                     return false;
                  }
               }

               if (bidiClass == IdnaTables::BidiEN)
                  hasEuropeanNumber = true;
               if (bidiClass == IdnaTables::BidiAN)
                  hasArabicNumber = true;
               if (bidiClass != IdnaTables::BidiNSM)
                  lastNonMark = bidiClass;
            }

            if (rightToLeft)
            {
               // 3. It ends with R, AL, EN or AN, and any NSM after it; 4. EN
               // and AN not both.
               if (lastNonMark != IdnaTables::BidiR && lastNonMark != IdnaTables::BidiAL && lastNonMark != IdnaTables::BidiEN &&
                   lastNonMark != IdnaTables::BidiAN)
                  return false;
               return !(hasEuropeanNumber && hasArabicNumber);
            }

            // 6. It ends with L or EN, and any NSM after it.
            return lastNonMark == IdnaTables::BidiL || lastNonMark == IdnaTables::BidiEN;
         }

         // RFC 5892 appendix A.1 and A.2, the CONTEXTJ rules.
         bool SatisfiesJoinerRules_(const CodePoints &label)
         {
            for (size_t i = 0; i < label.size(); i++)
            {
               const unsigned int codePoint = label[i];
               if (codePoint != zeroWidthNonJoiner && codePoint != zeroWidthJoiner)
                  continue;

               // After a virama, either is allowed.
               if (i > 0 && CombiningClass_(label[i - 1]) == viramaClass)
                  continue;

               if (codePoint == zeroWidthJoiner)
                  return false;

               // (Joining_Type:{L,D})(Joining_Type:T)*U+200C(Joining_Type:T)*(Joining_Type:{R,D})
               bool before = false;
               for (size_t j = i; j > 0; j--)
               {
                  const unsigned int type = JoiningType_(label[j - 1]);
                  if (type == IdnaTables::JoiningT)
                     continue;
                  before = type == IdnaTables::JoiningL || type == IdnaTables::JoiningD;
                  break;
               }

               bool after = false;
               for (size_t j = i + 1; j < label.size(); j++)
               {
                  const unsigned int type = JoiningType_(label[j]);
                  if (type == IdnaTables::JoiningT)
                     continue;
                  after = type == IdnaTables::JoiningR || type == IdnaTables::JoiningD;
                  break;
               }

               if (!before || !after)
                  return false;
            }

            return true;
         }

         // Section 4.1, the validity criteria, for a non-empty label; the bidi
         // rule is the caller's, since it depends on the whole name.
         unsigned int ValidityErrors_(const CodePoints &label, const Options &options, bool transitional, bool decoded)
         {
            unsigned int errors = 0;

            // 1. In NFC: only a label decoded from Punycode can be otherwise.
            if (decoded && !IsNfc(label))
               errors |= ErrorNotNfc;

            if (options.checkHyphens)
            {
               // 2. Not "--" in the third and fourth positions.
               if (label.size() >= 4 && label[2] == hyphen && label[3] == hyphen)
                  errors |= ErrorHyphens34;

               // 3. Not a hyphen first or last.
               if (label.front() == hyphen || label.back() == hyphen)
                  errors |= ErrorHyphenAtEnd;
            }
            else if (StartsWithXn_(label))
            {
               // 4. Not "xn--" first.
               errors |= ErrorXnPrefix;
            }

            // 6. Not a combining mark first.
            if (IsMark_(label[0]))
               errors |= ErrorLeadingMark;

            for (unsigned int codePoint : label)
            {
               // 5. No full stop.
               if (codePoint == fullStop)
                  errors |= ErrorFullStop;

               // 7. Valid, or under Nontransitional Processing a deviation.
               const unsigned int status = StatusOf_(codePoint);
               if (status != IdnaTables::StatusValid && !(status == IdnaTables::StatusDeviation && !transitional))
                  errors |= ErrorStatus;

               // UseSTD3ASCIIRules: an ASCII code point is a-z, 0-9 or '-'.
               if (options.useStd3AsciiRules && codePoint < 0x80 &&
                   !((codePoint >= 'a' && codePoint <= 'z') || (codePoint >= '0' && codePoint <= '9') || codePoint == hyphen))
                  errors |= ErrorStd3;
            }

            // 8. CONTEXTJ.
            if (options.checkJoiners && !SatisfiesJoinerRules_(label))
               errors |= ErrorJoiner;

            return errors;
         }

         unsigned int PunycodeAdapt_(unsigned int delta, unsigned int points, bool first)
         {
            delta = first ? delta / punycodeDamp : delta / 2;
            delta += delta / points;

            unsigned int k = 0;
            while (delta > ((punycodeBase - punycodeTMin) * punycodeTMax) / 2)
            {
               delta /= punycodeBase - punycodeTMin;
               k += punycodeBase;
            }

            return k + ((punycodeBase - punycodeTMin + 1) * delta) / (delta + punycodeSkew);
         }

         unsigned int PunycodeThreshold_(unsigned int k, unsigned int bias)
         {
            if (k <= bias)
               return punycodeTMin;
            if (k >= bias + punycodeTMax)
               return punycodeTMax;
            return k - bias;
         }

         char PunycodeDigit_(unsigned int digit)
         {
            return static_cast<char>(digit < 26 ? 'a' + digit : '0' + (digit - 26));
         }

         // The value of a Punycode digit, 36 for a character that is none. Upper
         // case is read as lower case, as RFC 3492 says a decoder must.
         unsigned int PunycodeDigitValue_(char character)
         {
            if (character >= 'a' && character <= 'z')
               return static_cast<unsigned int>(character - 'a');
            if (character >= 'A' && character <= 'Z')
               return static_cast<unsigned int>(character - 'A');
            if (character >= '0' && character <= '9')
               return static_cast<unsigned int>(character - '0') + 26;
            return punycodeBase;
         }

         void AppendUtf8_(unsigned int codePoint, std::string &text)
         {
            if (codePoint < 0x80)
            {
               text += static_cast<char>(codePoint);
            }
            else if (codePoint < 0x800)
            {
               text += static_cast<char>(0xC0 | (codePoint >> 6));
               text += static_cast<char>(0x80 | (codePoint & 0x3F));
            }
            else if (codePoint < 0x10000)
            {
               text += static_cast<char>(0xE0 | (codePoint >> 12));
               text += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
               text += static_cast<char>(0x80 | (codePoint & 0x3F));
            }
            else
            {
               text += static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07));
               text += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
               text += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
               text += static_cast<char>(0x80 | (codePoint & 0x3F));
            }
         }
      }

      Options::Options() :
         useStd3AsciiRules(false),
         checkHyphens(false),
         checkBidi(true),
         checkJoiners(true),
         transitionalProcessing(false),
         verifyDnsLength(false),
         longestLabel(0)
      {
      }

      Label::Label() :
         punycode(false),
         errors(0)
      {
      }

      Options
      UrlStandardOptions()
      {
         return Options();
      }

      Options
      ConformanceOptions()
      {
         Options options;
         options.useStd3AsciiRules = true;
         options.checkHyphens = true;
         options.checkBidi = true;
         options.checkJoiners = true;
         options.transitionalProcessing = false;
         options.verifyDnsLength = true;
         return options;
      }

      void
      ToNfc(CodePoints &text)
      {
         // ASCII is in NFC, and nothing composes with it but what follows a
         // starter beyond it: the common case costs a scan.
         if (IsAscii_(text))
            return;

         CodePoints decomposed;
         Decompose_(text, decomposed);
         Compose_(decomposed);
         text.swap(decomposed);
      }

      bool
      IsNfc(const CodePoints &text)
      {
         CodePoints normalised = text;
         ToNfc(normalised);
         return normalised == text;
      }

      bool
      PunycodeEncode(const CodePoints &input, std::string &output)
      {
         // RFC 3492 section 6.3.
         output.clear();

         size_t basic = 0;
         for (unsigned int codePoint : input)
         {
            // A surrogate or a code point past Unicode's last has no Punycode
            // (IdnaTestV2.txt's A3).
            if (codePoint > lastCodePoint || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
               return false;

            if (codePoint < 0x80)
            {
               output += static_cast<char>(codePoint);
               basic++;
            }
         }

         if (input.size() > punycodeMaxInt)
            return false;

         size_t handled = basic;
         if (basic > 0)
            output += '-';

         unsigned int n = punycodeInitialN;
         unsigned int delta = 0;
         unsigned int bias = punycodeInitialBias;

         while (handled < input.size())
         {
            unsigned int next = 0xFFFFFFFF;
            for (unsigned int codePoint : input)
               if (codePoint >= n && codePoint < next)
                  next = codePoint;

            const unsigned int points = static_cast<unsigned int>(handled + 1);
            if (next - n > (punycodeMaxInt - delta) / points)
               return false;

            delta += (next - n) * points;
            n = next;

            for (unsigned int codePoint : input)
            {
               if (codePoint < n)
               {
                  if (delta == punycodeMaxInt)
                     return false;
                  delta++;
               }

               if (codePoint != n)
                  continue;

               unsigned int q = delta;
               for (unsigned int k = punycodeBase;; k += punycodeBase)
               {
                  const unsigned int t = PunycodeThreshold_(k, bias);
                  if (q < t)
                     break;
                  output += PunycodeDigit_(t + (q - t) % (punycodeBase - t));
                  q = (q - t) / (punycodeBase - t);
               }

               output += PunycodeDigit_(q);
               bias = PunycodeAdapt_(delta, static_cast<unsigned int>(handled + 1), handled == basic);
               delta = 0;
               handled++;
            }

            if (delta == punycodeMaxInt)
               return false;
            delta++;
            n++;
         }

         return true;
      }

      bool
      PunycodeDecode(const std::string &input, CodePoints &output)
      {
         // RFC 3492 section 6.2: the code points before the last delimiter are
         // the basic ones, and the delimiter is consumed only when there were
         // some - so "-" alone is no Punycode (IdnaTestV2.txt's xn---).
         output.clear();

         const size_t delimiter = input.rfind('-');
         size_t in = 0;
         if (delimiter != std::string::npos && delimiter > 0)
         {
            for (size_t i = 0; i < delimiter; i++)
            {
               const unsigned char character = static_cast<unsigned char>(input[i]);
               if (character >= 0x80)
                  return false;
               output.push_back(character);
            }
            in = delimiter + 1;
         }

         unsigned int n = punycodeInitialN;
         unsigned int i = 0;
         unsigned int bias = punycodeInitialBias;

         while (in < input.size())
         {
            const unsigned int oldI = i;
            unsigned int w = 1;

            for (unsigned int k = punycodeBase;; k += punycodeBase)
            {
               if (in >= input.size())
                  return false;

               const unsigned int digit = PunycodeDigitValue_(input[in++]);
               if (digit >= punycodeBase)
                  return false;

               if (digit > (punycodeMaxInt - i) / w)
                  return false;
               i += digit * w;

               const unsigned int t = PunycodeThreshold_(k, bias);
               if (digit < t)
                  break;

               if (w > punycodeMaxInt / (punycodeBase - t))
                  return false;
               w *= punycodeBase - t;
            }

            if (output.size() + 1 > punycodeMaxInt)
               return false;

            const unsigned int points = static_cast<unsigned int>(output.size() + 1);
            bias = PunycodeAdapt_(i - oldI, points, oldI == 0);

            if (i / points > punycodeMaxInt - n)
               return false;

            n += i / points;
            i %= points;

            // A basic code point is never encoded, and nothing past Unicode's
            // last is a code point.
            if (n < 0x80 || n > lastCodePoint)
               return false;

            output.insert(output.begin() + i, n);
            i++;
         }

         return true;
      }

      void
      ProcessLabels(const CodePoints &domain, const Options &options, std::vector<Label> &labels)
      {
         labels.clear();

         // 1. Map.
         CodePoints mapped;
         mapped.reserve(domain.size());
         for (unsigned int codePoint : domain)
         {
            const IdnaTables::MappingRange &range = MappingOf_(codePoint);
            switch (range.status)
            {
            case IdnaTables::StatusIgnored:
               break;
            case IdnaTables::StatusMapped:
               if (options.transitionalProcessing && codePoint == capitalSharpS)
               {
                  mapped.push_back('s');
                  mapped.push_back('s');
                  break;
               }
               mapped.insert(mapped.end(), IdnaTables::MappingData + range.offset, IdnaTables::MappingData + range.offset + range.length);
               break;
            case IdnaTables::StatusDeviation:
               if (options.transitionalProcessing)
                  mapped.insert(mapped.end(), IdnaTables::MappingData + range.offset, IdnaTables::MappingData + range.offset + range.length);
               else
                  mapped.push_back(codePoint);
               break;
            default:
               // Valid, and disallowed, which the validity criteria find.
               mapped.push_back(codePoint);
               break;
            }
         }

         // 2. Normalize.
         ToNfc(mapped);

         // 3. Break.
         size_t start = 0;
         for (;;)
         {
            size_t end = start;
            while (end < mapped.size() && mapped[end] != fullStop)
               end++;

            Label label;
            label.unicode.assign(mapped.begin() + static_cast<std::ptrdiff_t>(start), mapped.begin() + static_cast<std::ptrdiff_t>(end));
            label.mapped = label.unicode;
            labels.push_back(std::move(label));

            if (end >= mapped.size())
               break;
            start = end + 1;
         }

         // 4. Convert/Validate.
         bool bidiDomain = false;
         std::vector<bool> checked(labels.size(), false);

         for (size_t index = 0; index < labels.size(); index++)
         {
            Label &label = labels[index];

            if (options.longestLabel > 0 && label.unicode.size() > options.longestLabel)
            {
               label.errors |= ErrorLabelTooLong;
               label.punycode = StartsWithXn_(label.unicode);
               continue;
            }

            if (StartsWithXn_(label.unicode))
            {
               label.punycode = true;

               if (!IsAscii_(label.unicode))
               {
                  label.errors |= ErrorPunycode;
                  continue;
               }

               CodePoints decoded;
               if (!PunycodeDecode(AsciiOf_(CodePoints(label.unicode.begin() + 4, label.unicode.end())), decoded))
               {
                  label.errors |= ErrorPunycode;
                  continue;
               }

               label.unicode.swap(decoded);

               if (label.unicode.empty() || IsAscii_(label.unicode))
               {
                  label.errors |= ErrorPunycode;
                  continue;
               }

               label.errors |= ValidityErrors_(label.unicode, options, false, true);
               checked[index] = true;
               continue;
            }

            if (!label.unicode.empty())
            {
               label.errors |= ValidityErrors_(label.unicode, options, options.transitionalProcessing, false);
               checked[index] = true;
            }
         }

         // 4.1's ninth criterion: in a bidi domain name - one that holds an R,
         // AL or AN anywhere - every label meets RFC 5893's rule.
         if (options.checkBidi)
         {
            for (const Label &label : labels)
            {
               for (unsigned int codePoint : label.unicode)
               {
                  if (IsRightToLeft_(BidiClass_(codePoint)))
                  {
                     bidiDomain = true;
                     break;
                  }
               }

               if (bidiDomain)
                  break;
            }

            if (bidiDomain)
            {
               for (size_t index = 0; index < labels.size(); index++)
                  if (checked[index] && !SatisfiesBidiRule_(labels[index].unicode))
                     labels[index].errors |= ErrorBidi;
            }
         }

         // ToASCII's step 3: each label with non-ASCII in it as "xn--" and its
         // Punycode.
         for (Label &label : labels)
         {
            if (label.errors & ErrorLabelTooLong)
               continue;

            if (IsAscii_(label.unicode))
            {
               label.ascii = AsciiOf_(label.unicode);
               continue;
            }

            std::string encoded;
            if (!PunycodeEncode(label.unicode, encoded))
            {
               label.errors |= ErrorPunycode;
               continue;
            }

            label.ascii = "xn--" + encoded;
         }
      }

      unsigned int
      ToAscii(const CodePoints &domain, const Options &options, std::string &ascii)
      {
         std::vector<Label> labels;
         ProcessLabels(domain, options, labels);

         unsigned int errors = 0;
         ascii.clear();

         for (size_t i = 0; i < labels.size(); i++)
         {
            const Label &label = labels[i];
            errors |= label.errors;

            if (i > 0)
               ascii += '.';

            // A label whose Punycode failed stays as it was, as far as the
            // conversion went.
            if (label.ascii.empty() && !label.unicode.empty())
               ascii += ToUtf8(label.unicode);
            else
               ascii += label.ascii;
         }

         if (options.verifyDnsLength)
         {
            // The name 1 to 253 characters, and each label 1 to 63; the empty
            // root label is not allowed here.
            if (ascii.empty() || ascii.size() > 253)
               errors |= ErrorDomainLength;

            for (const Label &label : labels)
            {
               const size_t length = label.ascii.empty() ? label.unicode.size() : label.ascii.size();
               if (length == 0 || length > 63)
                  errors |= ErrorLabelLength;
            }
         }

         return errors;
      }

      unsigned int
      ToUnicode(const CodePoints &domain, const Options &options, CodePoints &unicode)
      {
         std::vector<Label> labels;
         ProcessLabels(domain, options, labels);

         unsigned int errors = 0;
         unicode.clear();

         for (size_t i = 0; i < labels.size(); i++)
         {
            // Not ToASCII's: a Punycode that would overflow is no error of
            // ToUnicode's, which never encodes.
            unsigned int labelErrors = labels[i].errors;
            if ((labelErrors & ErrorPunycode) && !labels[i].punycode)
               labelErrors &= ~static_cast<unsigned int>(ErrorPunycode);
            errors |= labelErrors;

            if (i > 0)
               unicode.push_back(fullStop);
            unicode.insert(unicode.end(), labels[i].unicode.begin(), labels[i].unicode.end());
         }

         return errors;
      }

      bool
      DomainToAscii(const CodePoints &domain, std::string &ascii)
      {
         // An ASCII domain is lowered and nothing more, whatever ToASCII would
         // make of it: "due to web compatibility", the standard says, an "xn--"
         // label that is not Punycode among them.
         if (IsAscii_(domain))
         {
            ascii.clear();
            for (unsigned int codePoint : domain)
               ascii += static_cast<char>(codePoint >= 'A' && codePoint <= 'Z' ? codePoint + ('a' - 'A') : codePoint);
         }
         else if (ToAscii(domain, UrlStandardOptions(), ascii) != 0)
         {
            return false;
         }

         if (ascii.empty())
            return false;

         // The forbidden domain code points: the forbidden host code points,
         // the C0 controls, '%' and DELETE.
         for (char character : ascii)
         {
            const unsigned char c = static_cast<unsigned char>(character);
            if (c <= 0x20 || c == 0x7F || c == '#' || c == '%' || c == '/' || c == ':' || c == '<' || c == '>' ||
                c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|')
               return false;
         }

         return true;
      }

      std::string
      ToUtf8(const CodePoints &text)
      {
         std::string utf8;
         utf8.reserve(text.size());
         for (unsigned int codePoint : text)
            AppendUtf8_(codePoint, utf8);
         return utf8;
      }

      CodePoints
      FromUtf8(const std::string &text)
      {
         CodePoints codePoints;
         codePoints.reserve(text.size());

         size_t i = 0;
         while (i < text.size())
         {
            const unsigned int lead = static_cast<unsigned char>(text[i]);
            unsigned int length = 0;
            unsigned int codePoint = 0;

            if (lead < 0x80)
            {
               codePoints.push_back(lead);
               i++;
               continue;
            }

            if (lead >= 0xC2 && lead <= 0xDF)
            {
               length = 2;
               codePoint = lead & 0x1F;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
               length = 3;
               codePoint = lead & 0x0F;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
               length = 4;
               codePoint = lead & 0x07;
            }

            bool whole = length > 0 && i + length <= text.size();
            for (unsigned int j = 1; whole && j < length; j++)
            {
               const unsigned int continuation = static_cast<unsigned char>(text[i + j]);
               if ((continuation & 0xC0) != 0x80)
                  whole = false;
               else
                  codePoint = (codePoint << 6) | (continuation & 0x3F);
            }

            // Overlong forms are not read; a surrogate's three bytes are.
            if (whole && ((length == 3 && codePoint < 0x800) || (length == 4 && (codePoint < 0x10000 || codePoint > lastCodePoint))))
               whole = false;

            if (!whole)
            {
               codePoints.push_back(0xFFFD);
               i++;
               continue;
            }

            codePoints.push_back(codePoint);
            i += length;
         }

         return codePoints;
      }
   }
}
