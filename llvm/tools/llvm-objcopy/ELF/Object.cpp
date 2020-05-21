//===- Object.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Object.h"
#include "llvm-objcopy.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Path.h"
#include "lld/../../ELF/InputSection.h"
#include "lld/../../ELF/OutputSections.h"
#include "lld/../../ELF/Symbols.h"
#include "lld/../../ELF/Target.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llvm {
namespace objcopy {
namespace elf {

using namespace object;
using namespace ELF;

template <class ELFT> void ELFWriter<ELFT>::writePhdr(const Segment &Seg) {
  uint8_t *B = Buf.getBufferStart() + Obj.ProgramHdrSegment.Offset +
               Seg.Index * sizeof(Elf_Phdr);
  Elf_Phdr &Phdr = *reinterpret_cast<Elf_Phdr *>(B);
  Phdr.p_type = Seg.Type;
  Phdr.p_flags = Seg.Flags;
  Phdr.p_offset = Seg.Offset;
  Phdr.p_vaddr = Seg.VAddr;
  Phdr.p_paddr = Seg.PAddr;
  Phdr.p_filesz = Seg.FileSize;
  Phdr.p_memsz = Seg.MemSize;
  Phdr.p_align = Seg.Align;
}

Error SectionBase::removeSectionReferences(
    bool AllowBrokenLinks,
    function_ref<bool(const SectionBase *)> ToRemove) {
  return Error::success();
}

Error SectionBase::removeSymbols(function_ref<bool(const Symbol &)> ToRemove) {
  return Error::success();
}

void SectionBase::initialize(SectionTableRef SecTable) {}
void SectionBase::finalize() {}
void SectionBase::markSymbols() {}
void SectionBase::replaceSectionReferences(
    const DenseMap<SectionBase *, SectionBase *> &) {}

template <class ELFT> void ELFWriter<ELFT>::writeShdr(const SectionBase &Sec) {
  uint8_t *B = Buf.getBufferStart() + Sec.HeaderOffset;
  Elf_Shdr &Shdr = *reinterpret_cast<Elf_Shdr *>(B);
  Shdr.sh_name = Sec.NameIndex;
  Shdr.sh_type = Sec.Type;
  Shdr.sh_flags = Sec.Flags;
  Shdr.sh_addr = Sec.Addr;
  Shdr.sh_offset = Sec.Offset;
  Shdr.sh_size = Sec.Size;
  Shdr.sh_link = Sec.Link;
  Shdr.sh_info = Sec.Info;
  Shdr.sh_addralign = Sec.Align;
  Shdr.sh_entsize = Sec.EntrySize;
}

template <class ELFT> void ELFSectionSizer<ELFT>::visit(Section &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(OwnedDataSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(StringTableSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(DynamicRelocationSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(SymbolTableSection &Sec) {
  Sec.EntrySize = sizeof(Elf_Sym);
  Sec.Size = Sec.Symbols.size() * Sec.EntrySize;
  // Align to the largest field in Elf_Sym.
  Sec.Align = ELFT::Is64Bits ? sizeof(Elf_Xword) : sizeof(Elf_Word);
}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(RelocationSection &Sec) {
  Sec.EntrySize = Sec.Type == SHT_REL ? sizeof(Elf_Rel) : sizeof(Elf_Rela);
  Sec.Size = Sec.Relocations.size() * Sec.EntrySize;
  // Align to the largest field in Elf_Rel(a).
  Sec.Align = ELFT::Is64Bits ? sizeof(Elf_Xword) : sizeof(Elf_Word);
}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(GnuDebugLinkSection &Sec) {}

template <class ELFT> void ELFSectionSizer<ELFT>::visit(GroupSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(SectionIndexSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(CompressedSection &Sec) {}

template <class ELFT>
void ELFSectionSizer<ELFT>::visit(DecompressedSection &Sec) {}

void BinarySectionWriter::visit(const SectionIndexSection &Sec) {
  error("cannot write symbol section index table '" + Sec.Name + "' ");
}

void BinarySectionWriter::visit(const SymbolTableSection &Sec) {
  error("cannot write symbol table '" + Sec.Name + "' out to binary");
}

void BinarySectionWriter::visit(const RelocationSection &Sec) {
  error("cannot write relocation section '" + Sec.Name + "' out to binary");
}

void BinarySectionWriter::visit(const GnuDebugLinkSection &Sec) {
  error("cannot write '" + Sec.Name + "' out to binary");
}

void BinarySectionWriter::visit(const GroupSection &Sec) {
  error("cannot write '" + Sec.Name + "' out to binary");
}

void SectionWriter::visit(const Section &Sec) {
  if (Sec.Type != SHT_NOBITS)
    llvm::copy(Sec.Contents, Out.getBufferStart() + Sec.Offset);
}

static bool addressOverflows32bit(uint64_t Addr) {
  // Sign extended 32 bit addresses (e.g 0xFFFFFFFF80000000) are ok
  return Addr > UINT32_MAX && Addr + 0x80000000 > UINT32_MAX;
}

template <class T> static T checkedGetHex(StringRef S) {
  T Value;
  bool Fail = S.getAsInteger(16, Value);
  assert(!Fail);
  (void)Fail;
  return Value;
}

// Fills exactly Len bytes of buffer with hexadecimal characters
// representing value 'X'
template <class T, class Iterator>
static Iterator utohexstr(T X, Iterator It, size_t Len) {
  // Fill range with '0'
  std::fill(It, It + Len, '0');

  for (long I = Len - 1; I >= 0; --I) {
    unsigned char Mod = static_cast<unsigned char>(X) & 15;
    *(It + I) = hexdigit(Mod, false);
    X >>= 4;
  }
  assert(X == 0);
  return It + Len;
}

uint8_t IHexRecord::getChecksum(StringRef S) {
  assert((S.size() & 1) == 0);
  uint8_t Checksum = 0;
  while (!S.empty()) {
    Checksum += checkedGetHex<uint8_t>(S.take_front(2));
    S = S.drop_front(2);
  }
  return -Checksum;
}

IHexLineData IHexRecord::getLine(uint8_t Type, uint16_t Addr,
                                 ArrayRef<uint8_t> Data) {
  IHexLineData Line(getLineLength(Data.size()));
  assert(Line.size());
  auto Iter = Line.begin();
  *Iter++ = ':';
  Iter = utohexstr(Data.size(), Iter, 2);
  Iter = utohexstr(Addr, Iter, 4);
  Iter = utohexstr(Type, Iter, 2);
  for (uint8_t X : Data)
    Iter = utohexstr(X, Iter, 2);
  StringRef S(Line.data() + 1, std::distance(Line.begin() + 1, Iter));
  Iter = utohexstr(getChecksum(S), Iter, 2);
  *Iter++ = '\r';
  *Iter++ = '\n';
  assert(Iter == Line.end());
  return Line;
}

static Error checkRecord(const IHexRecord &R) {
  switch (R.Type) {
  case IHexRecord::Data:
    if (R.HexData.size() == 0)
      return createStringError(
          errc::invalid_argument,
          "zero data length is not allowed for data records");
    break;
  case IHexRecord::EndOfFile:
    break;
  case IHexRecord::SegmentAddr:
    // 20-bit segment address. Data length must be 2 bytes
    // (4 bytes in hex)
    if (R.HexData.size() != 4)
      return createStringError(
          errc::invalid_argument,
          "segment address data should be 2 bytes in size");
    break;
  case IHexRecord::StartAddr80x86:
  case IHexRecord::StartAddr:
    if (R.HexData.size() != 8)
      return createStringError(errc::invalid_argument,
                               "start address data should be 4 bytes in size");
    // According to Intel HEX specification '03' record
    // only specifies the code address within the 20-bit
    // segmented address space of the 8086/80186. This
    // means 12 high order bits should be zeroes.
    if (R.Type == IHexRecord::StartAddr80x86 &&
        R.HexData.take_front(3) != "000")
      return createStringError(errc::invalid_argument,
                               "start address exceeds 20 bit for 80x86");
    break;
  case IHexRecord::ExtendedAddr:
    // 16-31 bits of linear base address
    if (R.HexData.size() != 4)
      return createStringError(
          errc::invalid_argument,
          "extended address data should be 2 bytes in size");
    break;
  default:
    // Unknown record type
    return createStringError(errc::invalid_argument, "unknown record type: %u",
                             static_cast<unsigned>(R.Type));
  }
  return Error::success();
}

// Checks that IHEX line contains valid characters.
// This allows converting hexadecimal data to integers
// without extra verification.
static Error checkChars(StringRef Line) {
  assert(!Line.empty());
  if (Line[0] != ':')
    return createStringError(errc::invalid_argument,
                             "missing ':' in the beginning of line.");

  for (size_t Pos = 1; Pos < Line.size(); ++Pos)
    if (hexDigitValue(Line[Pos]) == -1U)
      return createStringError(errc::invalid_argument,
                               "invalid character at position %zu.", Pos + 1);
  return Error::success();
}

Expected<IHexRecord> IHexRecord::parse(StringRef Line) {
  assert(!Line.empty());

  // ':' + Length + Address + Type + Checksum with empty data ':LLAAAATTCC'
  if (Line.size() < 11)
    return createStringError(errc::invalid_argument,
                             "line is too short: %zu chars.", Line.size());

  if (Error E = checkChars(Line))
    return std::move(E);

  IHexRecord Rec;
  size_t DataLen = checkedGetHex<uint8_t>(Line.substr(1, 2));
  if (Line.size() != getLength(DataLen))
    return createStringError(errc::invalid_argument,
                             "invalid line length %zu (should be %zu)",
                             Line.size(), getLength(DataLen));

  Rec.Addr = checkedGetHex<uint16_t>(Line.substr(3, 4));
  Rec.Type = checkedGetHex<uint8_t>(Line.substr(7, 2));
  Rec.HexData = Line.substr(9, DataLen * 2);

  if (getChecksum(Line.drop_front(1)) != 0)
    return createStringError(errc::invalid_argument, "incorrect checksum.");
  if (Error E = checkRecord(Rec))
    return std::move(E);
  return Rec;
}

static uint64_t sectionPhysicalAddr(const SectionBase *Sec) {
  Segment *Seg = Sec->ParentSegment;
  if (Seg && Seg->Type != ELF::PT_LOAD)
    Seg = nullptr;
  return Seg ? Seg->PAddr + Sec->OriginalOffset - Seg->OriginalOffset
             : Sec->Addr;
}

void IHexSectionWriterBase::writeSection(const SectionBase *Sec,
                                         ArrayRef<uint8_t> Data) {
  assert(Data.size() == Sec->Size);
  const uint32_t ChunkSize = 16;
  uint32_t Addr = sectionPhysicalAddr(Sec) & 0xFFFFFFFFU;
  while (!Data.empty()) {
    uint64_t DataSize = std::min<uint64_t>(Data.size(), ChunkSize);
    if (Addr > SegmentAddr + BaseAddr + 0xFFFFU) {
      if (Addr > 0xFFFFFU) {
        // Write extended address record, zeroing segment address
        // if needed.
        if (SegmentAddr != 0)
          SegmentAddr = writeSegmentAddr(0U);
        BaseAddr = writeBaseAddr(Addr);
      } else {
        // We can still remain 16-bit
        SegmentAddr = writeSegmentAddr(Addr);
      }
    }
    uint64_t SegOffset = Addr - BaseAddr - SegmentAddr;
    assert(SegOffset <= 0xFFFFU);
    DataSize = std::min(DataSize, 0x10000U - SegOffset);
    writeData(0, SegOffset, Data.take_front(DataSize));
    Addr += DataSize;
    Data = Data.drop_front(DataSize);
  }
}

uint64_t IHexSectionWriterBase::writeSegmentAddr(uint64_t Addr) {
  assert(Addr <= 0xFFFFFU);
  uint8_t Data[] = {static_cast<uint8_t>((Addr & 0xF0000U) >> 12), 0};
  writeData(2, 0, Data);
  return Addr & 0xF0000U;
}

uint64_t IHexSectionWriterBase::writeBaseAddr(uint64_t Addr) {
  assert(Addr <= 0xFFFFFFFFU);
  uint64_t Base = Addr & 0xFFFF0000U;
  uint8_t Data[] = {static_cast<uint8_t>(Base >> 24),
                    static_cast<uint8_t>((Base >> 16) & 0xFF)};
  writeData(4, 0, Data);
  return Base;
}

void IHexSectionWriterBase::writeData(uint8_t Type, uint16_t Addr,
                                      ArrayRef<uint8_t> Data) {
  Offset += IHexRecord::getLineLength(Data.size());
}

void IHexSectionWriterBase::visit(const Section &Sec) {
  writeSection(&Sec, Sec.Contents);
}

void IHexSectionWriterBase::visit(const OwnedDataSection &Sec) {
  writeSection(&Sec, Sec.Data);
}

void IHexSectionWriterBase::visit(const StringTableSection &Sec) {
  // Check that sizer has already done its work
  assert(Sec.Size == Sec.StrTabBuilder.getSize());
  // We are free to pass an invalid pointer to writeSection as long
  // as we don't actually write any data. The real writer class has
  // to override this method .
  writeSection(&Sec, {nullptr, static_cast<size_t>(Sec.Size)});
}

void IHexSectionWriterBase::visit(const DynamicRelocationSection &Sec) {
  writeSection(&Sec, Sec.Contents);
}

void IHexSectionWriter::writeData(uint8_t Type, uint16_t Addr,
                                  ArrayRef<uint8_t> Data) {
  IHexLineData HexData = IHexRecord::getLine(Type, Addr, Data);
  memcpy(Out.getBufferStart() + Offset, HexData.data(), HexData.size());
  Offset += HexData.size();
}

void IHexSectionWriter::visit(const StringTableSection &Sec) {
  assert(Sec.Size == Sec.StrTabBuilder.getSize());
  std::vector<uint8_t> Data(Sec.Size);
  Sec.StrTabBuilder.write(Data.data());
  writeSection(&Sec, Data);
}

void Section::accept(SectionVisitor &Visitor) const { Visitor.visit(*this); }

void Section::accept(MutableSectionVisitor &Visitor) { Visitor.visit(*this); }

void SectionWriter::visit(const OwnedDataSection &Sec) {
  llvm::copy(Sec.Data, Out.getBufferStart() + Sec.Offset);
}

static constexpr std::array<uint8_t, 4> ZlibGnuMagic = {{'Z', 'L', 'I', 'B'}};

static bool isDataGnuCompressed(ArrayRef<uint8_t> Data) {
  return Data.size() > ZlibGnuMagic.size() &&
         std::equal(ZlibGnuMagic.begin(), ZlibGnuMagic.end(), Data.data());
}

template <class ELFT>
static std::tuple<uint64_t, uint64_t>
getDecompressedSizeAndAlignment(ArrayRef<uint8_t> Data) {
  const bool IsGnuDebug = isDataGnuCompressed(Data);
  const uint64_t DecompressedSize =
      IsGnuDebug
          ? support::endian::read64be(Data.data() + ZlibGnuMagic.size())
          : reinterpret_cast<const Elf_Chdr_Impl<ELFT> *>(Data.data())->ch_size;
  const uint64_t DecompressedAlign =
      IsGnuDebug ? 1
                 : reinterpret_cast<const Elf_Chdr_Impl<ELFT> *>(Data.data())
                       ->ch_addralign;

  return std::make_tuple(DecompressedSize, DecompressedAlign);
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const DecompressedSection &Sec) {
  const size_t DataOffset = isDataGnuCompressed(Sec.OriginalData)
                                ? (ZlibGnuMagic.size() + sizeof(Sec.Size))
                                : sizeof(Elf_Chdr_Impl<ELFT>);

  StringRef CompressedContent(
      reinterpret_cast<const char *>(Sec.OriginalData.data()) + DataOffset,
      Sec.OriginalData.size() - DataOffset);

  SmallVector<char, 128> DecompressedContent;
  if (Error E = zlib::uncompress(CompressedContent, DecompressedContent,
                                 static_cast<size_t>(Sec.Size)))
    reportError(Sec.Name, std::move(E));

  uint8_t *Buf = Out.getBufferStart() + Sec.Offset;
  std::copy(DecompressedContent.begin(), DecompressedContent.end(), Buf);
}

void BinarySectionWriter::visit(const DecompressedSection &Sec) {
  error("cannot write compressed section '" + Sec.Name + "' ");
}

void DecompressedSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void DecompressedSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

void OwnedDataSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void OwnedDataSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

void OwnedDataSection::appendHexData(StringRef HexData) {
  assert((HexData.size() & 1) == 0);
  while (!HexData.empty()) {
    Data.push_back(checkedGetHex<uint8_t>(HexData.take_front(2)));
    HexData = HexData.drop_front(2);
  }
  Size = Data.size();
}

void BinarySectionWriter::visit(const CompressedSection &Sec) {
  error("cannot write compressed section '" + Sec.Name + "' ");
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const CompressedSection &Sec) {
  uint8_t *Buf = Out.getBufferStart() + Sec.Offset;
  if (Sec.CompressionType == DebugCompressionType::None) {
    std::copy(Sec.OriginalData.begin(), Sec.OriginalData.end(), Buf);
    return;
  }

  if (Sec.CompressionType == DebugCompressionType::GNU) {
    const char *Magic = "ZLIB";
    memcpy(Buf, Magic, strlen(Magic));
    Buf += strlen(Magic);
    const uint64_t DecompressedSize =
        support::endian::read64be(&Sec.DecompressedSize);
    memcpy(Buf, &DecompressedSize, sizeof(DecompressedSize));
    Buf += sizeof(DecompressedSize);
  } else {
    Elf_Chdr_Impl<ELFT> Chdr;
    Chdr.ch_type = ELF::ELFCOMPRESS_ZLIB;
    Chdr.ch_size = Sec.DecompressedSize;
    Chdr.ch_addralign = Sec.DecompressedAlign;
    memcpy(Buf, &Chdr, sizeof(Chdr));
    Buf += sizeof(Chdr);
  }

  std::copy(Sec.CompressedData.begin(), Sec.CompressedData.end(), Buf);
}

CompressedSection::CompressedSection(const SectionBase &Sec,
                                     DebugCompressionType CompressionType)
    : SectionBase(Sec), CompressionType(CompressionType),
      DecompressedSize(Sec.OriginalData.size()), DecompressedAlign(Sec.Align) {
  if (Error E = zlib::compress(
          StringRef(reinterpret_cast<const char *>(OriginalData.data()),
                    OriginalData.size()),
          CompressedData))
    reportError(Name, std::move(E));

  size_t ChdrSize;
  if (CompressionType == DebugCompressionType::GNU) {
    Name = ".z" + Sec.Name.substr(1);
    ChdrSize = sizeof("ZLIB") - 1 + sizeof(uint64_t);
  } else {
    Flags |= ELF::SHF_COMPRESSED;
    ChdrSize =
        std::max(std::max(sizeof(object::Elf_Chdr_Impl<object::ELF64LE>),
                          sizeof(object::Elf_Chdr_Impl<object::ELF64BE>)),
                 std::max(sizeof(object::Elf_Chdr_Impl<object::ELF32LE>),
                          sizeof(object::Elf_Chdr_Impl<object::ELF32BE>)));
  }
  Size = ChdrSize + CompressedData.size();
  Align = 8;
}

CompressedSection::CompressedSection(ArrayRef<uint8_t> CompressedData,
                                     uint64_t DecompressedSize,
                                     uint64_t DecompressedAlign)
    : CompressionType(DebugCompressionType::None),
      DecompressedSize(DecompressedSize), DecompressedAlign(DecompressedAlign) {
  OriginalData = CompressedData;
}

void CompressedSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void CompressedSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

void StringTableSection::addString(StringRef Name) { StrTabBuilder.add(Name); }

uint32_t StringTableSection::findIndex(StringRef Name) const {
  return StrTabBuilder.getOffset(Name);
}

void StringTableSection::prepareForLayout() {
  StrTabBuilder.finalize();
  Size = StrTabBuilder.getSize();
}

void SectionWriter::visit(const StringTableSection &Sec) {
  Sec.StrTabBuilder.write(Out.getBufferStart() + Sec.Offset);
}

void StringTableSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void StringTableSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const SectionIndexSection &Sec) {
  uint8_t *Buf = Out.getBufferStart() + Sec.Offset;
  llvm::copy(Sec.Indexes, reinterpret_cast<Elf_Word *>(Buf));
}

void SectionIndexSection::initialize(SectionTableRef SecTable) {
  Size = 0;
  setSymTab(SecTable.getSectionOfType<SymbolTableSection>(
      Link,
      "Link field value " + Twine(Link) + " in section " + Name + " is invalid",
      "Link field value " + Twine(Link) + " in section " + Name +
          " is not a symbol table"));
  Symbols->setShndxTable(this);
}

void SectionIndexSection::finalize() { Link = Symbols->Index; }

void SectionIndexSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void SectionIndexSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

static bool isValidReservedSectionIndex(uint16_t Index, uint16_t Machine) {
  switch (Index) {
  case SHN_ABS:
  case SHN_COMMON:
    return true;
  }

  if (Machine == EM_AMDGPU) {
    return Index == SHN_AMDGPU_LDS;
  }

  if (Machine == EM_HEXAGON) {
    switch (Index) {
    case SHN_HEXAGON_SCOMMON:
    case SHN_HEXAGON_SCOMMON_2:
    case SHN_HEXAGON_SCOMMON_4:
    case SHN_HEXAGON_SCOMMON_8:
      return true;
    }
  }
  return false;
}

// Large indexes force us to clarify exactly what this function should do. This
// function should return the value that will appear in st_shndx when written
// out.
uint16_t Symbol::getShndx() const {
  if (DefinedIn != nullptr) {
    if (DefinedIn->Index >= SHN_LORESERVE)
      return SHN_XINDEX;
    return DefinedIn->Index;
  }

  if (ShndxType == SYMBOL_SIMPLE_INDEX) {
    // This means that we don't have a defined section but we do need to
    // output a legitimate section index.
    return SHN_UNDEF;
  }

  assert(ShndxType == SYMBOL_ABS || ShndxType == SYMBOL_COMMON ||
         (ShndxType >= SYMBOL_LOPROC && ShndxType <= SYMBOL_HIPROC) ||
         (ShndxType >= SYMBOL_LOOS && ShndxType <= SYMBOL_HIOS));
  return static_cast<uint16_t>(ShndxType);
}

bool Symbol::isCommon() const { return getShndx() == SHN_COMMON; }

void SymbolTableSection::assignIndices() {
  uint32_t Index = 0;
  for (auto &Sym : Symbols)
    Sym->Index = Index++;
}

void SymbolTableSection::addSymbol(Twine Name, uint8_t Bind, uint8_t Type,
                                   SectionBase *DefinedIn, uint64_t Value,
                                   uint8_t Visibility, uint16_t Shndx,
                                   uint64_t SymbolSize) {
  Symbol Sym;
  Sym.Name = Name.str();
  Sym.Binding = Bind;
  Sym.Type = Type;
  Sym.DefinedIn = DefinedIn;
  if (DefinedIn != nullptr)
    DefinedIn->HasSymbol = true;
  if (DefinedIn == nullptr) {
    if (Shndx >= SHN_LORESERVE)
      Sym.ShndxType = static_cast<SymbolShndxType>(Shndx);
    else
      Sym.ShndxType = SYMBOL_SIMPLE_INDEX;
  }
  Sym.Value = Value;
  Sym.Visibility = Visibility;
  Sym.Size = SymbolSize;
  Sym.Index = Symbols.size();
  Symbols.emplace_back(std::make_unique<Symbol>(Sym));
  Size += this->EntrySize;
}

Error SymbolTableSection::removeSectionReferences(
    bool AllowBrokenLinks,
    function_ref<bool(const SectionBase *)> ToRemove) {
  if (ToRemove(SectionIndexTable))
    SectionIndexTable = nullptr;
  if (ToRemove(SymbolNames)) {
    if (!AllowBrokenLinks)
      return createStringError(
          llvm::errc::invalid_argument,
          "string table '%s' cannot be removed because it is "
          "referenced by the symbol table '%s'",
          SymbolNames->Name.data(), this->Name.data());
    SymbolNames = nullptr;
  }
  return removeSymbols(
      [ToRemove](const Symbol &Sym) { return ToRemove(Sym.DefinedIn); });
}

void SymbolTableSection::updateSymbols(function_ref<void(Symbol &)> Callable) {
  std::for_each(std::begin(Symbols) + 1, std::end(Symbols),
                [Callable](SymPtr &Sym) { Callable(*Sym); });
  std::stable_partition(
      std::begin(Symbols), std::end(Symbols),
      [](const SymPtr &Sym) { return Sym->Binding == STB_LOCAL; });
  assignIndices();
}

Error SymbolTableSection::removeSymbols(
    function_ref<bool(const Symbol &)> ToRemove) {
  Symbols.erase(
      std::remove_if(std::begin(Symbols) + 1, std::end(Symbols),
                     [ToRemove](const SymPtr &Sym) { return ToRemove(*Sym); }),
      std::end(Symbols));
  Size = Symbols.size() * EntrySize;
  assignIndices();
  return Error::success();
}

void SymbolTableSection::replaceSectionReferences(
    const DenseMap<SectionBase *, SectionBase *> &FromTo) {
  for (std::unique_ptr<Symbol> &Sym : Symbols)
    if (SectionBase *To = FromTo.lookup(Sym->DefinedIn))
      Sym->DefinedIn = To;
}

void SymbolTableSection::initialize(SectionTableRef SecTable) {
  Size = 0;
  setStrTab(SecTable.getSectionOfType<StringTableSection>(
      Link,
      "Symbol table has link index of " + Twine(Link) +
          " which is not a valid index",
      "Symbol table has link index of " + Twine(Link) +
          " which is not a string table"));
}

void SymbolTableSection::finalize() {
  uint32_t MaxLocalIndex = 0;
  for (std::unique_ptr<Symbol> &Sym : Symbols) {
    Sym->NameIndex =
        SymbolNames == nullptr ? 0 : SymbolNames->findIndex(Sym->Name);
    if (Sym->Binding == STB_LOCAL)
      MaxLocalIndex = std::max(MaxLocalIndex, Sym->Index);
  }
  // Now we need to set the Link and Info fields.
  Link = SymbolNames == nullptr ? 0 : SymbolNames->Index;
  Info = MaxLocalIndex + 1;
}

void SymbolTableSection::prepareForLayout() {
  // Reserve proper amount of space in section index table, so we can
  // layout sections correctly. We will fill the table with correct
  // indexes later in fillShdnxTable.
  if (SectionIndexTable)  
    SectionIndexTable->reserve(Symbols.size());

  // Add all of our strings to SymbolNames so that SymbolNames has the right
  // size before layout is decided.
  // If the symbol names section has been removed, don't try to add strings to
  // the table.
  if (SymbolNames != nullptr)
    for (std::unique_ptr<Symbol> &Sym : Symbols)
      SymbolNames->addString(Sym->Name);
}

void SymbolTableSection::fillShndxTable() {
  if (SectionIndexTable == nullptr)
    return;
  // Fill section index table with real section indexes. This function must
  // be called after assignOffsets.
  for (const std::unique_ptr<Symbol> &Sym : Symbols) {
    if (Sym->DefinedIn != nullptr && Sym->DefinedIn->Index >= SHN_LORESERVE)
      SectionIndexTable->addIndex(Sym->DefinedIn->Index);
    else
      SectionIndexTable->addIndex(SHN_UNDEF);
  }
}

const Symbol *SymbolTableSection::getSymbolByIndex(uint32_t Index) const {
  if (Symbols.size() <= Index)
    error("invalid symbol index: " + Twine(Index));
  return Symbols[Index].get();
}

Symbol *SymbolTableSection::getSymbolByIndex(uint32_t Index) {
  return const_cast<Symbol *>(
      static_cast<const SymbolTableSection *>(this)->getSymbolByIndex(Index));
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const SymbolTableSection &Sec) {
  Elf_Sym *Sym = reinterpret_cast<Elf_Sym *>(Out.getBufferStart() + Sec.Offset);
  // Loop though symbols setting each entry of the symbol table.
  for (const std::unique_ptr<Symbol> &Symbol : Sec.Symbols) {
    Sym->st_name = Symbol->NameIndex;
    Sym->st_value = Symbol->Value;
    Sym->st_size = Symbol->Size;
    Sym->st_other = Symbol->Visibility;
    Sym->setBinding(Symbol->Binding);
    Sym->setType(Symbol->Type);
    Sym->st_shndx = Symbol->getShndx();
    ++Sym;
  }
}

void SymbolTableSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void SymbolTableSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

Error RelocationSection::removeSectionReferences(
    bool AllowBrokenLinks,
    function_ref<bool(const SectionBase *)> ToRemove) {
  if (ToRemove(Symbols)) {
    if (!AllowBrokenLinks)
      return createStringError(
          llvm::errc::invalid_argument,
          "symbol table '%s' cannot be removed because it is "
          "referenced by the relocation section '%s'",
          Symbols->Name.data(), this->Name.data());
    Symbols = nullptr;
  }

  for (const Relocation &R : Relocations) {
    if (!R.RelocSymbol || !R.RelocSymbol->DefinedIn ||
        !ToRemove(R.RelocSymbol->DefinedIn))
      continue;
    return createStringError(llvm::errc::invalid_argument,
                             "section '%s' cannot be removed: (%s+0x%" PRIx64
                             ") has relocation against symbol '%s'",
                             R.RelocSymbol->DefinedIn->Name.data(),
                             SecToApplyRel->Name.data(), R.Offset,
                             R.RelocSymbol->Name.c_str());
  }

  return Error::success();
}

template <class SymTabType>
void RelocSectionWithSymtabBase<SymTabType>::initialize(
    SectionTableRef SecTable) {
  if (Link != SHN_UNDEF)
    setSymTab(SecTable.getSectionOfType<SymTabType>(
        Link,
        "Link field value " + Twine(Link) + " in section " + Name +
            " is invalid",
        "Link field value " + Twine(Link) + " in section " + Name +
            " is not a symbol table"));

  if (Info != SHN_UNDEF)
    setSection(SecTable.getSection(Info, "Info field value " + Twine(Info) +
                                             " in section " + Name +
                                             " is invalid"));
  else
    setSection(nullptr);
}

template <class SymTabType>
void RelocSectionWithSymtabBase<SymTabType>::finalize() {
  this->Link = Symbols ? Symbols->Index : 0;

  if (SecToApplyRel != nullptr)
    this->Info = SecToApplyRel->Index;
}

template <class ELFT>
static void setAddend(Elf_Rel_Impl<ELFT, false> &Rel, uint64_t Addend) {}

template <class ELFT>
static void setAddend(Elf_Rel_Impl<ELFT, true> &Rela, uint64_t Addend) {
  Rela.r_addend = Addend;
}

template <class RelRange, class T>
static void writeRel(const RelRange &Relocations, T *Buf) {
  for (const auto &Reloc : Relocations) {
    Buf->r_offset = Reloc.Offset;
    setAddend(*Buf, Reloc.Addend);
    Buf->setSymbolAndType(Reloc.RelocSymbol ? Reloc.RelocSymbol->Index : 0,
                          Reloc.Type, false);
    ++Buf;
  }
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const RelocationSection &Sec) {
  uint8_t *Buf = Out.getBufferStart() + Sec.Offset;
  if (Sec.Type == SHT_REL)
    writeRel(Sec.Relocations, reinterpret_cast<Elf_Rel *>(Buf));
  else
    writeRel(Sec.Relocations, reinterpret_cast<Elf_Rela *>(Buf));
}

void RelocationSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void RelocationSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

Error RelocationSection::removeSymbols(
    function_ref<bool(const Symbol &)> ToRemove) {
  for (const Relocation &Reloc : Relocations)
    if (Reloc.RelocSymbol && ToRemove(*Reloc.RelocSymbol))
      return createStringError(
          llvm::errc::invalid_argument,
          "not stripping symbol '%s' because it is named in a relocation",
          Reloc.RelocSymbol->Name.data());
  return Error::success();
}

void RelocationSection::markSymbols() {
  for (const Relocation &Reloc : Relocations)
    if (Reloc.RelocSymbol)
      Reloc.RelocSymbol->Referenced = true;
}

void RelocationSection::replaceSectionReferences(
    const DenseMap<SectionBase *, SectionBase *> &FromTo) {
  // Update the target section if it was replaced.
  if (SectionBase *To = FromTo.lookup(SecToApplyRel))
    SecToApplyRel = To;
}

void RelocationSection::makeSectionRelative(SectionBase *RefSec) {
  static lld::elf::Configuration LldConfig{};
  LldConfig.wordsize = 8;
  lld::elf::config = &LldConfig;
  lld::elf::target = lld::elf::getX86_64TargetInfo();

  if (auto *ApplySec = dyn_cast<Section>(SecToApplyRel)) {
    lld::elf::InputSection LldSection{nullptr,
                                      ApplySec->Flags,
                                      uint32_t(ApplySec->Type),
                                      uint32_t(ApplySec->Align),
                                      ApplySec->OriginalData,
                                      ApplySec->Name};
    lld::elf::OutputSection LldOut{ApplySec->Name, uint32_t(ApplySec->Type),
                                   ApplySec->Flags};
    LldOut.addr = ApplySec->Addr;
    LldSection.parent = &LldOut;

    std::size_t NumRelocs = 0;
    for (const Relocation &R : Relocations) {
      if (!R.RelocSymbol || !R.RelocSymbol->DefinedIn ||
          R.RelocSymbol->DefinedIn != RefSec)
        continue;
      ++NumRelocs;
    }
    if (NumRelocs == 0)
      return;

    uint8_t *SecData = ApplySec->mutableData();
    LldSection.relocations.reserve(NumRelocs);
    std::vector<lld::elf::Defined> Symbols;
    Symbols.reserve(NumRelocs);
    for (const Relocation &R : Relocations) {
      if (!R.RelocSymbol || !R.RelocSymbol->DefinedIn ||
          R.RelocSymbol->DefinedIn != RefSec)
        continue;
      uint64_t Offset = R.Offset - ApplySec->Offset;
      uint8_t *Data = SecData + Offset;
      lld::elf::Symbol &LldSymbol = Symbols.emplace_back(
          nullptr, "", 0, 0, 0, R.RelocSymbol->Value - RefSec->Addr,
          R.RelocSymbol->Size, nullptr);
      LldSection.relocations.push_back(lld::elf::Relocation{
          lld::elf::target->getRelExpr(R.Type, LldSymbol, Data), R.Type, Offset,
          int64_t(R.Addend), &LldSymbol});
    }

    LldSection.relocateAlloc(SecData, SecData + ApplySec->Size);
  }
}

void SectionWriter::visit(const DynamicRelocationSection &Sec) {
  llvm::copy(Sec.Contents, Out.getBufferStart() + Sec.Offset);
}

void DynamicRelocationSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void DynamicRelocationSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

Error DynamicRelocationSection::removeSectionReferences(
    bool AllowBrokenLinks, function_ref<bool(const SectionBase *)> ToRemove) {
  if (ToRemove(Symbols)) {
    if (!AllowBrokenLinks)
      return createStringError(
          llvm::errc::invalid_argument,
          "symbol table '%s' cannot be removed because it is "
          "referenced by the relocation section '%s'",
          Symbols->Name.data(), this->Name.data());
    Symbols = nullptr;
  }

  // SecToApplyRel contains a section referenced by sh_info field. It keeps
  // a section to which the relocation section applies. When we remove any
  // sections we also remove their relocation sections. Since we do that much
  // earlier, this assert should never be triggered.
  assert(!SecToApplyRel || !ToRemove(SecToApplyRel));
  return Error::success();
}

Error Section::removeSectionReferences(
    bool AllowBrokenDependency,
    function_ref<bool(const SectionBase *)> ToRemove) {
  if (ToRemove(LinkSection)) {
    if (!AllowBrokenDependency)
      return createStringError(llvm::errc::invalid_argument,
                               "section '%s' cannot be removed because it is "
                               "referenced by the section '%s'",
                               LinkSection->Name.data(), this->Name.data());
    LinkSection = nullptr;
  }
  return Error::success();
}

void GroupSection::finalize() {
  this->Info = Sym->Index;
  this->Link = SymTab->Index;
}

Error GroupSection::removeSymbols(function_ref<bool(const Symbol &)> ToRemove) {
  if (ToRemove(*Sym))
    return createStringError(llvm::errc::invalid_argument,
                             "symbol '%s' cannot be removed because it is "
                             "referenced by the section '%s[%d]'",
                             Sym->Name.data(), this->Name.data(), this->Index);
  return Error::success();
}

void GroupSection::markSymbols() {
  if (Sym)
    Sym->Referenced = true;
}

void GroupSection::replaceSectionReferences(
    const DenseMap<SectionBase *, SectionBase *> &FromTo) {
  for (SectionBase *&Sec : GroupMembers)
    if (SectionBase *To = FromTo.lookup(Sec))
      Sec = To;
}

void Section::initialize(SectionTableRef SecTable) {
  if (Link == ELF::SHN_UNDEF)
    return;
  LinkSection =
      SecTable.getSection(Link, "Link field value " + Twine(Link) +
                                    " in section " + Name + " is invalid");
  if (LinkSection->Type == ELF::SHT_SYMTAB)
    LinkSection = nullptr;
}

void Section::finalize() { this->Link = LinkSection ? LinkSection->Index : 0; }

uint8_t *Section::mutableData() {
  if (ParentSegment) {
    if (ParentSegment->OwnedContents.empty() &&
        !ParentSegment->Contents.empty()) {
      ParentSegment->OwnedContents.assign(ParentSegment->Contents.begin(),
                                          ParentSegment->Contents.end());
      ParentSegment->Contents = ParentSegment->OwnedContents;
    }
    return ParentSegment->OwnedContents.data() +
           (Offset - ParentSegment->Offset);
  }

  if (OwnedContents.empty() && !Contents.empty()) {
    OwnedContents.assign(Contents.begin(), Contents.end());
    Contents = OwnedContents;
  }
  return OwnedContents.data();
}

void GnuDebugLinkSection::init(StringRef File) {
  FileName = sys::path::filename(File);
  // The format for the .gnu_debuglink starts with the file name and is
  // followed by a null terminator and then the CRC32 of the file. The CRC32
  // should be 4 byte aligned. So we add the FileName size, a 1 for the null
  // byte, and then finally push the size to alignment and add 4.
  Size = alignTo(FileName.size() + 1, 4) + 4;
  // The CRC32 will only be aligned if we align the whole section.
  Align = 4;
  Type = OriginalType = ELF::SHT_PROGBITS;
  Name = ".gnu_debuglink";
  // For sections not found in segments, OriginalOffset is only used to
  // establish the order that sections should go in. By using the maximum
  // possible offset we cause this section to wind up at the end.
  OriginalOffset = std::numeric_limits<uint64_t>::max();
}

GnuDebugLinkSection::GnuDebugLinkSection(StringRef File,
                                         uint32_t PrecomputedCRC)
    : FileName(File), CRC32(PrecomputedCRC) {
  init(File);
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const GnuDebugLinkSection &Sec) {
  unsigned char *Buf = Out.getBufferStart() + Sec.Offset;
  Elf_Word *CRC =
      reinterpret_cast<Elf_Word *>(Buf + Sec.Size - sizeof(Elf_Word));
  *CRC = Sec.CRC32;
  llvm::copy(Sec.FileName, Buf);
}

void GnuDebugLinkSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void GnuDebugLinkSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

template <class ELFT>
void ELFSectionWriter<ELFT>::visit(const GroupSection &Sec) {
  ELF::Elf32_Word *Buf =
      reinterpret_cast<ELF::Elf32_Word *>(Out.getBufferStart() + Sec.Offset);
  *Buf++ = Sec.FlagWord;
  for (SectionBase *S : Sec.GroupMembers)
    support::endian::write32<ELFT::TargetEndianness>(Buf++, S->Index);
}

void GroupSection::accept(SectionVisitor &Visitor) const {
  Visitor.visit(*this);
}

void GroupSection::accept(MutableSectionVisitor &Visitor) {
  Visitor.visit(*this);
}

// Returns true IFF a section is wholly inside the range of a segment
static bool sectionWithinSegment(const SectionBase &Sec, const Segment &Seg) {
  // If a section is empty it should be treated like it has a size of 1. This is
  // to clarify the case when an empty section lies on a boundary between two
  // segments and ensures that the section "belongs" to the second segment and
  // not the first.
  uint64_t SecSize = Sec.Size ? Sec.Size : 1;

  if (Sec.Type == SHT_NOBITS) {
    if (!(Sec.Flags & SHF_ALLOC))
      return false;

    bool SectionIsTLS = Sec.Flags & SHF_TLS;
    bool SegmentIsTLS = Seg.Type == PT_TLS;
    if (SectionIsTLS != SegmentIsTLS)
      return false;

    return Seg.VAddr <= Sec.Addr &&
           Seg.VAddr + Seg.MemSize >= Sec.Addr + SecSize;
  }

  return Seg.Offset <= Sec.OriginalOffset &&
         Seg.Offset + Seg.FileSize >= Sec.OriginalOffset + SecSize;
}

// Returns true IFF a segment's original offset is inside of another segment's
// range.
static bool segmentOverlapsSegment(const Segment &Child,
                                   const Segment &Parent) {

  return Parent.OriginalOffset <= Child.OriginalOffset &&
         Parent.OriginalOffset + Parent.FileSize > Child.OriginalOffset;
}

static bool compareSegmentsByOffset(const Segment *A, const Segment *B) {
  // Any segment without a parent segment should come before a segment
  // that has a parent segment.
  if (A->OriginalOffset < B->OriginalOffset)
    return true;
  if (A->OriginalOffset > B->OriginalOffset)
    return false;
  return A->Index < B->Index;
}

static bool compareSegmentsByPAddr(const Segment *A, const Segment *B) {
  if (A->PAddr < B->PAddr)
    return true;
  if (A->PAddr > B->PAddr)
    return false;
  return A->Index < B->Index;
}

void BasicELFBuilder::initFileHeader() {
  Obj->Flags = 0x0;
  Obj->Type = ET_REL;
  Obj->OSABI = ELFOSABI_NONE;
  Obj->ABIVersion = 0;
  Obj->Entry = 0x0;
  Obj->Machine = EM_NONE;
  Obj->Version = 1;
}

void BasicELFBuilder::initHeaderSegment() { Obj->ElfHdrSegment.Index = 0; }

StringTableSection *BasicELFBuilder::addStrTab() {
  auto &StrTab = Obj->addSection<StringTableSection>();
  StrTab.Name = ".strtab";

  Obj->SectionNames = &StrTab;
  return &StrTab;
}

SymbolTableSection *BasicELFBuilder::addSymTab(StringTableSection *StrTab) {
  auto &SymTab = Obj->addSection<SymbolTableSection>();

  SymTab.Name = ".symtab";
  SymTab.Link = StrTab->Index;

  // The symbol table always needs a null symbol
  SymTab.addSymbol("", 0, 0, nullptr, 0, 0, 0, 0);

  Obj->SymbolTable = &SymTab;
  return &SymTab;
}

void BasicELFBuilder::initSections() {
  for (SectionBase &Sec : Obj->sections())
    Sec.initialize(Obj->sections());
}

void BinaryELFBuilder::addData(SymbolTableSection *SymTab) {
  auto Data = ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(MemBuf->getBufferStart()),
      MemBuf->getBufferSize());
  auto &DataSection = Obj->addSection<Section>(Data);
  DataSection.Name = ".data";
  DataSection.Type = ELF::SHT_PROGBITS;
  DataSection.Size = Data.size();
  DataSection.Flags = ELF::SHF_ALLOC | ELF::SHF_WRITE;

  std::string SanitizedFilename = MemBuf->getBufferIdentifier().str();
  std::replace_if(std::begin(SanitizedFilename), std::end(SanitizedFilename),
                  [](char C) { return !isalnum(C); }, '_');
  Twine Prefix = Twine("_binary_") + SanitizedFilename;

  SymTab->addSymbol(Prefix + "_start", STB_GLOBAL, STT_NOTYPE, &DataSection,
                    /*Value=*/0, NewSymbolVisibility, 0, 0);
  SymTab->addSymbol(Prefix + "_end", STB_GLOBAL, STT_NOTYPE, &DataSection,
                    /*Value=*/DataSection.Size, NewSymbolVisibility, 0, 0);
  SymTab->addSymbol(Prefix + "_size", STB_GLOBAL, STT_NOTYPE, nullptr,
                    /*Value=*/DataSection.Size, NewSymbolVisibility, SHN_ABS,
                    0);
}

std::unique_ptr<Object> BinaryELFBuilder::build() {
  initFileHeader();
  initHeaderSegment();

  SymbolTableSection *SymTab = addSymTab(addStrTab());
  initSections();
  addData(SymTab);

  return std::move(Obj);
}

// Adds sections from IHEX data file. Data should have been
// fully validated by this time.
void IHexELFBuilder::addDataSections() {
  OwnedDataSection *Section = nullptr;
  uint64_t SegmentAddr = 0, BaseAddr = 0;
  uint32_t SecNo = 1;

  for (const IHexRecord &R : Records) {
    uint64_t RecAddr;
    switch (R.Type) {
    case IHexRecord::Data:
      // Ignore empty data records
      if (R.HexData.empty())
        continue;
      RecAddr = R.Addr + SegmentAddr + BaseAddr;
      if (!Section || Section->Addr + Section->Size != RecAddr)
        // OriginalOffset field is only used to sort section properly, so
        // instead of keeping track of real offset in IHEX file, we use
        // section number.
        Section = &Obj->addSection<OwnedDataSection>(
            ".sec" + std::to_string(SecNo++), RecAddr,
            ELF::SHF_ALLOC | ELF::SHF_WRITE, SecNo);
      Section->appendHexData(R.HexData);
      break;
    case IHexRecord::EndOfFile:
      break;
    case IHexRecord::SegmentAddr:
      // 20-bit segment address.
      SegmentAddr = checkedGetHex<uint16_t>(R.HexData) << 4;
      break;
    case IHexRecord::StartAddr80x86:
    case IHexRecord::StartAddr:
      Obj->Entry = checkedGetHex<uint32_t>(R.HexData);
      assert(Obj->Entry <= 0xFFFFFU);
      break;
    case IHexRecord::ExtendedAddr:
      // 16-31 bits of linear base address
      BaseAddr = checkedGetHex<uint16_t>(R.HexData) << 16;
      break;
    default:
      llvm_unreachable("unknown record type");
    }
  }
}

std::unique_ptr<Object> IHexELFBuilder::build() {
  initFileHeader();
  initHeaderSegment();
  StringTableSection *StrTab = addStrTab();
  addSymTab(StrTab);
  initSections();
  addDataSections();

  return std::move(Obj);
}

template <class ELFT> void ELFBuilder<ELFT>::setParentSegment(Segment &Child) {
  for (Segment &Parent : Obj.segments()) {
    // Every segment will overlap with itself but we don't want a segment to
    // be it's own parent so we avoid that situation.
    if (&Child != &Parent && segmentOverlapsSegment(Child, Parent)) {
      // We want a canonical "most parental" segment but this requires
      // inspecting the ParentSegment.
      if (compareSegmentsByOffset(&Parent, &Child))
        if (Child.ParentSegment == nullptr ||
            compareSegmentsByOffset(&Parent, Child.ParentSegment)) {
          Child.ParentSegment = &Parent;
        }
    }
  }
}

template <class ELFT> void ELFBuilder<ELFT>::findEhdrOffset() {
  if (!ExtractPartition)
    return;

  for (const SectionBase &Sec : Obj.sections()) {
    if (Sec.Type == SHT_LLVM_PART_EHDR && Sec.Name == *ExtractPartition) {
      EhdrOffset = Sec.Offset;
      return;
    }
  }
  error("could not find partition named '" + *ExtractPartition + "'");
}

template <class ELFT>
void ELFBuilder<ELFT>::readProgramHeaders(const ELFFile<ELFT> &HeadersFile) {
  uint32_t Index = 0;
  for (const auto &Phdr : unwrapOrError(HeadersFile.program_headers())) {
    if (Phdr.p_offset + Phdr.p_filesz > HeadersFile.getBufSize())
      error("program header with offset 0x" + Twine::utohexstr(Phdr.p_offset) +
            " and file size 0x" + Twine::utohexstr(Phdr.p_filesz) +
            " goes past the end of the file");

    ArrayRef<uint8_t> Data{HeadersFile.base() + Phdr.p_offset,
                           (size_t)Phdr.p_filesz};
    Segment &Seg = Obj.addSegment(Data);
    Seg.Type = Phdr.p_type;
    Seg.Flags = Phdr.p_flags;
    Seg.OriginalOffset = Phdr.p_offset + EhdrOffset;
    Seg.Offset = Phdr.p_offset + EhdrOffset;
    Seg.VAddr = Phdr.p_vaddr;
    Seg.PAddr = Phdr.p_paddr;
    Seg.FileSize = Phdr.p_filesz;
    Seg.MemSize = Phdr.p_memsz;
    Seg.Align = Phdr.p_align;
    Seg.Index = Index++;
    for (SectionBase &Sec : Obj.sections())
      if (sectionWithinSegment(Sec, Seg)) {
        Seg.addSection(&Sec);
        if (!Sec.ParentSegment || Sec.ParentSegment->Offset > Seg.Offset)
          Sec.ParentSegment = &Seg;
      }
  }

  auto &ElfHdr = Obj.ElfHdrSegment;
  ElfHdr.Index = Index++;
  ElfHdr.OriginalOffset = ElfHdr.Offset = EhdrOffset;

  const auto &Ehdr = *HeadersFile.getHeader();
  auto &PrHdr = Obj.ProgramHdrSegment;
  PrHdr.Type = PT_PHDR;
  PrHdr.Flags = 0;
  // The spec requires us to have p_vaddr % p_align == p_offset % p_align.
  // Whereas this works automatically for ElfHdr, here OriginalOffset is
  // always non-zero and to ensure the equation we assign the same value to
  // VAddr as well.
  PrHdr.OriginalOffset = PrHdr.Offset = PrHdr.VAddr = EhdrOffset + Ehdr.e_phoff;
  PrHdr.PAddr = 0;
  PrHdr.FileSize = PrHdr.MemSize = Ehdr.e_phentsize * Ehdr.e_phnum;
  // The spec requires us to naturally align all the fields.
  PrHdr.Align = sizeof(Elf_Addr);
  PrHdr.Index = Index++;

  // Now we do an O(n^2) loop through the segments in order to match up
  // segments.
  for (Segment &Child : Obj.segments())
    setParentSegment(Child);
  setParentSegment(ElfHdr);
  setParentSegment(PrHdr);
}

template <class ELFT>
void ELFBuilder<ELFT>::initGroupSection(GroupSection *GroupSec) {
  if (GroupSec->Align % sizeof(ELF::Elf32_Word) != 0)
    error("invalid alignment " + Twine(GroupSec->Align) + " of group section '" +
          GroupSec->Name + "'");
  SectionTableRef SecTable = Obj.sections();
  auto SymTab = SecTable.template getSectionOfType<SymbolTableSection>(
      GroupSec->Link,
      "link field value '" + Twine(GroupSec->Link) + "' in section '" +
          GroupSec->Name + "' is invalid",
      "link field value '" + Twine(GroupSec->Link) + "' in section '" +
          GroupSec->Name + "' is not a symbol table");
  Symbol *Sym = SymTab->getSymbolByIndex(GroupSec->Info);
  if (!Sym)
    error("info field value '" + Twine(GroupSec->Info) + "' in section '" +
          GroupSec->Name + "' is not a valid symbol index");
  GroupSec->setSymTab(SymTab);
  GroupSec->setSymbol(Sym);
  if (GroupSec->Contents.size() % sizeof(ELF::Elf32_Word) ||
      GroupSec->Contents.empty())
    error("the content of the section " + GroupSec->Name + " is malformed");
  const ELF::Elf32_Word *Word =
      reinterpret_cast<const ELF::Elf32_Word *>(GroupSec->Contents.data());
  const ELF::Elf32_Word *End =
      Word + GroupSec->Contents.size() / sizeof(ELF::Elf32_Word);
  GroupSec->setFlagWord(*Word++);
  for (; Word != End; ++Word) {
    uint32_t Index = support::endian::read32<ELFT::TargetEndianness>(Word);
    GroupSec->addMember(SecTable.getSection(
        Index, "group member index " + Twine(Index) + " in section '" +
                   GroupSec->Name + "' is invalid"));
  }
}

template <class ELFT>
void ELFBuilder<ELFT>::initSymbolTable(SymbolTableSection *SymTab) {
  const Elf_Shdr &Shdr = *unwrapOrError(ElfFile.getSection(SymTab->Index));
  StringRef StrTabData = unwrapOrError(ElfFile.getStringTableForSymtab(Shdr));
  ArrayRef<Elf_Word> ShndxData;

  auto Symbols = unwrapOrError(ElfFile.symbols(&Shdr));
  for (const auto &Sym : Symbols) {
    SectionBase *DefSection = nullptr;
    StringRef Name = unwrapOrError(Sym.getName(StrTabData));

    if (Sym.st_shndx == SHN_XINDEX) {
      if (SymTab->getShndxTable() == nullptr)
        error("symbol '" + Name +
              "' has index SHN_XINDEX but no SHT_SYMTAB_SHNDX section exists");
      if (ShndxData.data() == nullptr) {
        const Elf_Shdr &ShndxSec =
            *unwrapOrError(ElfFile.getSection(SymTab->getShndxTable()->Index));
        ShndxData = unwrapOrError(
            ElfFile.template getSectionContentsAsArray<Elf_Word>(&ShndxSec));
        if (ShndxData.size() != Symbols.size())
          error("symbol section index table does not have the same number of "
                "entries as the symbol table");
      }
      Elf_Word Index = ShndxData[&Sym - Symbols.begin()];
      DefSection = Obj.sections().getSection(
          Index,
          "symbol '" + Name + "' has invalid section index " + Twine(Index));
    } else if (Sym.st_shndx >= SHN_LORESERVE) {
      if (!isValidReservedSectionIndex(Sym.st_shndx, Obj.Machine)) {
        error(
            "symbol '" + Name +
            "' has unsupported value greater than or equal to SHN_LORESERVE: " +
            Twine(Sym.st_shndx));
      }
    } else if (Sym.st_shndx != SHN_UNDEF) {
      DefSection = Obj.sections().getSection(
          Sym.st_shndx, "symbol '" + Name +
                            "' is defined has invalid section index " +
                            Twine(Sym.st_shndx));
    }

    SymTab->addSymbol(Name, Sym.getBinding(), Sym.getType(), DefSection,
                      Sym.getValue(), Sym.st_other, Sym.st_shndx, Sym.st_size);
  }
}

template <class ELFT>
static void getAddend(uint64_t &ToSet, const Elf_Rel_Impl<ELFT, false> &Rel) {}

template <class ELFT>
static void getAddend(uint64_t &ToSet, const Elf_Rel_Impl<ELFT, true> &Rela) {
  ToSet = Rela.r_addend;
}

template <class T>
static void initRelocations(RelocationSection *Relocs,
                            SymbolTableSection *SymbolTable, T RelRange) {
  for (const auto &Rel : RelRange) {
    Relocation ToAdd;
    ToAdd.Offset = Rel.r_offset;
    getAddend(ToAdd.Addend, Rel);
    ToAdd.Type = Rel.getType(false);

    if (uint32_t Sym = Rel.getSymbol(false)) {
      if (!SymbolTable)
        error("'" + Relocs->Name +
              "': relocation references symbol with index " + Twine(Sym) +
              ", but there is no symbol table");
      ToAdd.RelocSymbol = SymbolTable->getSymbolByIndex(Sym);
    }

    Relocs->addRelocation(ToAdd);
  }
}

SectionBase *SectionTableRef::getSection(uint32_t Index, Twine ErrMsg) {
  if (Index == SHN_UNDEF || Index > Sections.size())
    error(ErrMsg);
  return Sections[Index - 1].get();
}

template <class T>
T *SectionTableRef::getSectionOfType(uint32_t Index, Twine IndexErrMsg,
                                     Twine TypeErrMsg) {
  if (T *Sec = dyn_cast<T>(getSection(Index, IndexErrMsg)))
    return Sec;
  error(TypeErrMsg);
}

template <class ELFT>
SectionBase &ELFBuilder<ELFT>::makeSection(const Elf_Shdr &Shdr) {
  ArrayRef<uint8_t> Data;
  switch (Shdr.sh_type) {
  case SHT_REL:
  case SHT_RELA:
    if (Shdr.sh_flags & SHF_ALLOC) {
      Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
      return Obj.addSection<DynamicRelocationSection>(Data);
    }
    return Obj.addSection<RelocationSection>();
  case SHT_STRTAB:
    // If a string table is allocated we don't want to mess with it. That would
    // mean altering the memory image. There are no special link types or
    // anything so we can just use a Section.
    if (Shdr.sh_flags & SHF_ALLOC) {
      Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
      return Obj.addSection<Section>(Data);
    }
    return Obj.addSection<StringTableSection>();
  case SHT_HASH:
  case SHT_GNU_HASH:
    // Hash tables should refer to SHT_DYNSYM which we're not going to change.
    // Because of this we don't need to mess with the hash tables either.
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
    return Obj.addSection<Section>(Data);
  case SHT_GROUP:
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
    return Obj.addSection<GroupSection>(Data);
  case SHT_DYNSYM:
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
    return Obj.addSection<DynamicSymbolTableSection>(Data);
  case SHT_DYNAMIC:
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));
    return Obj.addSection<DynamicSection>(Data);
  case SHT_SYMTAB: {
    auto &SymTab = Obj.addSection<SymbolTableSection>();
    Obj.SymbolTable = &SymTab;
    return SymTab;
  }
  case SHT_SYMTAB_SHNDX: {
    auto &ShndxSection = Obj.addSection<SectionIndexSection>();
    Obj.SectionIndexTable = &ShndxSection;
    return ShndxSection;
  }
  case SHT_NOBITS:
    return Obj.addSection<Section>(Data);
  default: {
    Data = unwrapOrError(ElfFile.getSectionContents(&Shdr));

    StringRef Name = unwrapOrError(ElfFile.getSectionName(&Shdr));
    if (Name.startswith(".zdebug") || (Shdr.sh_flags & ELF::SHF_COMPRESSED)) {
      uint64_t DecompressedSize, DecompressedAlign;
      std::tie(DecompressedSize, DecompressedAlign) =
          getDecompressedSizeAndAlignment<ELFT>(Data);
      return Obj.addSection<CompressedSection>(Data, DecompressedSize,
                                               DecompressedAlign);
    }

    return Obj.addSection<Section>(Data);
  }
  }
}

template <class ELFT> void ELFBuilder<ELFT>::readSectionHeaders() {
  uint32_t Index = 0;
  for (const auto &Shdr : unwrapOrError(ElfFile.sections())) {
    if (Index == 0) {
      ++Index;
      continue;
    }
    auto &Sec = makeSection(Shdr);
    Sec.Name = std::string(unwrapOrError(ElfFile.getSectionName(&Shdr)));
    Sec.Type = Sec.OriginalType = Shdr.sh_type;
    Sec.Flags = Sec.OriginalFlags = Shdr.sh_flags;
    Sec.Addr = Shdr.sh_addr;
    Sec.Offset = Shdr.sh_offset;
    Sec.OriginalOffset = Shdr.sh_offset;
    Sec.Size = Shdr.sh_size;
    Sec.Link = Shdr.sh_link;
    Sec.Info = Shdr.sh_info;
    Sec.Align = Shdr.sh_addralign;
    Sec.EntrySize = Shdr.sh_entsize;
    Sec.Index = Index++;
    Sec.OriginalData =
        ArrayRef<uint8_t>(ElfFile.base() + Shdr.sh_offset,
                          (Shdr.sh_type == SHT_NOBITS) ? 0 : Shdr.sh_size);
  }
}

template <class ELFT> void ELFBuilder<ELFT>::readSections(bool EnsureSymtab) {
  uint32_t ShstrIndex = ElfFile.getHeader()->e_shstrndx;
  if (ShstrIndex == SHN_XINDEX)
    ShstrIndex = unwrapOrError(ElfFile.getSection(0))->sh_link;

  if (ShstrIndex == SHN_UNDEF)
    Obj.HadShdrs = false;
  else
    Obj.SectionNames =
        Obj.sections().template getSectionOfType<StringTableSection>(
            ShstrIndex,
            "e_shstrndx field value " + Twine(ShstrIndex) + " in elf header " +
                " is invalid",
            "e_shstrndx field value " + Twine(ShstrIndex) + " in elf header " +
                " does not reference a string table");

  // If a section index table exists we'll need to initialize it before we
  // initialize the symbol table because the symbol table might need to
  // reference it.
  if (Obj.SectionIndexTable)
    Obj.SectionIndexTable->initialize(Obj.sections());

  // Now that all of the sections have been added we can fill out some extra
  // details about symbol tables. We need the symbol table filled out before
  // any relocations.
  if (Obj.SymbolTable) {
    Obj.SymbolTable->initialize(Obj.sections());
    initSymbolTable(Obj.SymbolTable);
  } else if (EnsureSymtab) {
    // Reuse an existing SHT_STRTAB section if it exists.
    StringTableSection *StrTab = nullptr;
    for (auto &Sec : Obj.sections()) {
      if (Sec.Type == ELF::SHT_STRTAB && !(Sec.Flags & SHF_ALLOC)) {
        StrTab = static_cast<StringTableSection *>(&Sec);

        // Prefer a string table that is not the section header string table, if
        // such a table exists.
        if (Obj.SectionNames != &Sec)
          break;
      }
    }
    if (!StrTab)
      StrTab = &Obj.addSection<StringTableSection>();

    SymbolTableSection &SymTab = Obj.addSection<SymbolTableSection>();
    SymTab.Name = ".symtab";
    SymTab.Link = StrTab->Index;
    SymTab.initialize(Obj.sections());
    SymTab.addSymbol("", 0, 0, nullptr, 0, 0, 0, 0);
    Obj.SymbolTable = &SymTab;
  }

  // Now that all sections and symbols have been added we can add
  // relocations that reference symbols and set the link and info fields for
  // relocation sections.
  for (auto &Sec : Obj.sections()) {
    if (&Sec == Obj.SymbolTable)
      continue;
    Sec.initialize(Obj.sections());
    if (auto RelSec = dyn_cast<RelocationSection>(&Sec)) {
      auto Shdr = unwrapOrError(ElfFile.sections()).begin() + RelSec->Index;
      if (RelSec->Type == SHT_REL)
        initRelocations(RelSec, Obj.SymbolTable,
                        unwrapOrError(ElfFile.rels(Shdr)));
      else
        initRelocations(RelSec, Obj.SymbolTable,
                        unwrapOrError(ElfFile.relas(Shdr)));
    } else if (auto GroupSec = dyn_cast<GroupSection>(&Sec)) {
      initGroupSection(GroupSec);
    }
  }
}

template <class ELFT> void ELFBuilder<ELFT>::build(bool EnsureSymtab) {
  readSectionHeaders();
  findEhdrOffset();

  // The ELFFile whose ELF headers and program headers are copied into the
  // output file. Normally the same as ElfFile, but if we're extracting a
  // loadable partition it will point to the partition's headers.
  ELFFile<ELFT> HeadersFile = unwrapOrError(ELFFile<ELFT>::create(toStringRef(
      {ElfFile.base() + EhdrOffset, ElfFile.getBufSize() - EhdrOffset})));

  auto &Ehdr = *HeadersFile.getHeader();
  Obj.OSABI = Ehdr.e_ident[EI_OSABI];
  Obj.ABIVersion = Ehdr.e_ident[EI_ABIVERSION];
  Obj.Type = Ehdr.e_type;
  Obj.Machine = Ehdr.e_machine;
  Obj.Version = Ehdr.e_version;
  Obj.Entry = Ehdr.e_entry;
  Obj.Flags = Ehdr.e_flags;

  readSections(EnsureSymtab);
  readProgramHeaders(HeadersFile);
}

Writer::~Writer() {}

Reader::~Reader() {}

std::unique_ptr<Object> BinaryReader::create(bool /*EnsureSymtab*/) const {
  return BinaryELFBuilder(MemBuf, NewSymbolVisibility).build();
}

Expected<std::vector<IHexRecord>> IHexReader::parse() const {
  SmallVector<StringRef, 16> Lines;
  std::vector<IHexRecord> Records;
  bool HasSections = false;

  MemBuf->getBuffer().split(Lines, '\n');
  Records.reserve(Lines.size());
  for (size_t LineNo = 1; LineNo <= Lines.size(); ++LineNo) {
    StringRef Line = Lines[LineNo - 1].trim();
    if (Line.empty())
      continue;

    Expected<IHexRecord> R = IHexRecord::parse(Line);
    if (!R)
      return parseError(LineNo, R.takeError());
    if (R->Type == IHexRecord::EndOfFile)
      break;
    HasSections |= (R->Type == IHexRecord::Data);
    Records.push_back(*R);
  }
  if (!HasSections)
    return parseError(-1U, "no sections");

  return std::move(Records);
}

std::unique_ptr<Object> IHexReader::create(bool /*EnsureSymtab*/) const {
  std::vector<IHexRecord> Records = unwrapOrError(parse());
  return IHexELFBuilder(Records).build();
}

std::unique_ptr<Object> ELFReader::create(bool EnsureSymtab) const {
  auto Obj = std::make_unique<Object>();
  if (auto *O = dyn_cast<ELFObjectFile<ELF32LE>>(Bin)) {
    ELFBuilder<ELF32LE> Builder(*O, *Obj, ExtractPartition);
    Builder.build(EnsureSymtab);
    return Obj;
  } else if (auto *O = dyn_cast<ELFObjectFile<ELF64LE>>(Bin)) {
    ELFBuilder<ELF64LE> Builder(*O, *Obj, ExtractPartition);
    Builder.build(EnsureSymtab);
    return Obj;
  } else if (auto *O = dyn_cast<ELFObjectFile<ELF32BE>>(Bin)) {
    ELFBuilder<ELF32BE> Builder(*O, *Obj, ExtractPartition);
    Builder.build(EnsureSymtab);
    return Obj;
  } else if (auto *O = dyn_cast<ELFObjectFile<ELF64BE>>(Bin)) {
    ELFBuilder<ELF64BE> Builder(*O, *Obj, ExtractPartition);
    Builder.build(EnsureSymtab);
    return Obj;
  }
  error("invalid file type");
}

template <class ELFT> void ELFWriter<ELFT>::writeEhdr() {
  Elf_Ehdr &Ehdr = *reinterpret_cast<Elf_Ehdr *>(Buf.getBufferStart());
  std::fill(Ehdr.e_ident, Ehdr.e_ident + 16, 0);
  Ehdr.e_ident[EI_MAG0] = 0x7f;
  Ehdr.e_ident[EI_MAG1] = 'E';
  Ehdr.e_ident[EI_MAG2] = 'L';
  Ehdr.e_ident[EI_MAG3] = 'F';
  Ehdr.e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  Ehdr.e_ident[EI_DATA] =
      ELFT::TargetEndianness == support::big ? ELFDATA2MSB : ELFDATA2LSB;
  Ehdr.e_ident[EI_VERSION] = EV_CURRENT;
  Ehdr.e_ident[EI_OSABI] = Obj.OSABI;
  Ehdr.e_ident[EI_ABIVERSION] = Obj.ABIVersion;

  Ehdr.e_type = Obj.Type;
  Ehdr.e_machine = Obj.Machine;
  Ehdr.e_version = Obj.Version;
  Ehdr.e_entry = Obj.Entry;
  // We have to use the fully-qualified name llvm::size
  // since some compilers complain on ambiguous resolution.
  Ehdr.e_phnum = llvm::size(Obj.segments());
  Ehdr.e_phoff = (Ehdr.e_phnum != 0) ? Obj.ProgramHdrSegment.Offset : 0;
  Ehdr.e_phentsize = (Ehdr.e_phnum != 0) ? sizeof(Elf_Phdr) : 0;
  Ehdr.e_flags = Obj.Flags;
  Ehdr.e_ehsize = sizeof(Elf_Ehdr);
  if (WriteSectionHeaders && Obj.sections().size() != 0) {
    Ehdr.e_shentsize = sizeof(Elf_Shdr);
    Ehdr.e_shoff = Obj.SHOff;
    // """
    // If the number of sections is greater than or equal to
    // SHN_LORESERVE (0xff00), this member has the value zero and the actual
    // number of section header table entries is contained in the sh_size field
    // of the section header at index 0.
    // """
    auto Shnum = Obj.sections().size() + 1;
    if (Shnum >= SHN_LORESERVE)
      Ehdr.e_shnum = 0;
    else
      Ehdr.e_shnum = Shnum;
    // """
    // If the section name string table section index is greater than or equal
    // to SHN_LORESERVE (0xff00), this member has the value SHN_XINDEX (0xffff)
    // and the actual index of the section name string table section is
    // contained in the sh_link field of the section header at index 0.
    // """
    if (Obj.SectionNames->Index >= SHN_LORESERVE)
      Ehdr.e_shstrndx = SHN_XINDEX;
    else
      Ehdr.e_shstrndx = Obj.SectionNames->Index;
  } else {
    Ehdr.e_shentsize = 0;
    Ehdr.e_shoff = 0;
    Ehdr.e_shnum = 0;
    Ehdr.e_shstrndx = 0;
  }
}

template <class ELFT> void ELFWriter<ELFT>::writePhdrs() {
  for (auto &Seg : Obj.segments())
    writePhdr(Seg);
}

template <class ELFT> void ELFWriter<ELFT>::writeShdrs() {
  // This reference serves to write the dummy section header at the begining
  // of the file. It is not used for anything else
  Elf_Shdr &Shdr =
      *reinterpret_cast<Elf_Shdr *>(Buf.getBufferStart() + Obj.SHOff);
  Shdr.sh_name = 0;
  Shdr.sh_type = SHT_NULL;
  Shdr.sh_flags = 0;
  Shdr.sh_addr = 0;
  Shdr.sh_offset = 0;
  // See writeEhdr for why we do this.
  uint64_t Shnum = Obj.sections().size() + 1;
  if (Shnum >= SHN_LORESERVE)
    Shdr.sh_size = Shnum;
  else
    Shdr.sh_size = 0;
  // See writeEhdr for why we do this.
  if (Obj.SectionNames != nullptr && Obj.SectionNames->Index >= SHN_LORESERVE)
    Shdr.sh_link = Obj.SectionNames->Index;
  else
    Shdr.sh_link = 0;
  Shdr.sh_info = 0;
  Shdr.sh_addralign = 0;
  Shdr.sh_entsize = 0;

  for (SectionBase &Sec : Obj.sections())
    writeShdr(Sec);
}

template <class ELFT> void ELFWriter<ELFT>::writeSectionData() {
  for (SectionBase &Sec : Obj.sections())
    // Segments are responsible for writing their contents, so only write the
    // section data if the section is not in a segment. Note that this renders
    // sections in segments effectively immutable.
    if (Sec.ParentSegment == nullptr)
      Sec.accept(*SecWriter);
}

template <class ELFT> void ELFWriter<ELFT>::writeSegmentData() {
  for (Segment &Seg : Obj.segments()) {
    size_t Size = std::min<size_t>(Seg.FileSize, Seg.getContents().size());
    std::memcpy(Buf.getBufferStart() + Seg.Offset, Seg.getContents().data(),
                Size);
  }

  // Iterate over removed sections and overwrite their old data with zeroes.
  for (auto &Sec : Obj.removedSections()) {
    Segment *Parent = Sec.ParentSegment;
    if (Parent == nullptr || Sec.Type == SHT_NOBITS || Sec.Size == 0)
      continue;
    uint64_t Offset =
        Sec.OriginalOffset - Parent->OriginalOffset + Parent->Offset;
    std::memset(Buf.getBufferStart() + Offset, 0, Sec.Size);
  }
}

template <class ELFT>
ELFWriter<ELFT>::ELFWriter(Object &Obj, Buffer &Buf, bool WSH,
                           bool OnlyKeepDebug)
    : Writer(Obj, Buf), WriteSectionHeaders(WSH && Obj.HadShdrs),
      OnlyKeepDebug(OnlyKeepDebug) {}

Error Object::removeSections(bool AllowBrokenLinks,
    std::function<bool(const SectionBase &)> ToRemove) {

  auto Iter = std::stable_partition(
      std::begin(Sections), std::end(Sections), [=](const SecPtr &Sec) {
        if (ToRemove(*Sec))
          return false;
        if (auto RelSec = dyn_cast<RelocationSectionBase>(Sec.get())) {
          if (auto ToRelSec = RelSec->getSection())
            return !ToRemove(*ToRelSec);
        }
        return true;
      });
  if (SymbolTable != nullptr && ToRemove(*SymbolTable))
    SymbolTable = nullptr;
  if (SectionNames != nullptr && ToRemove(*SectionNames))
    SectionNames = nullptr;
  if (SectionIndexTable != nullptr && ToRemove(*SectionIndexTable))
    SectionIndexTable = nullptr;
  // Now make sure there are no remaining references to the sections that will
  // be removed. Sometimes it is impossible to remove a reference so we emit
  // an error here instead.
  std::unordered_set<const SectionBase *> RemoveSections;
  RemoveSections.reserve(std::distance(Iter, std::end(Sections)));
  for (auto &RemoveSec : make_range(Iter, std::end(Sections))) {
    for (auto &Segment : Segments)
      Segment->removeSection(RemoveSec.get());
    RemoveSections.insert(RemoveSec.get());
  }

  // For each section that remains alive, we want to remove the dead references.
  // This either might update the content of the section (e.g. remove symbols
  // from symbol table that belongs to removed section) or trigger an error if
  // a live section critically depends on a section being removed somehow
  // (e.g. the removed section is referenced by a relocation).
  for (auto &KeepSec : make_range(std::begin(Sections), Iter)) {
    if (Error E = KeepSec->removeSectionReferences(AllowBrokenLinks,
            [&RemoveSections](const SectionBase *Sec) {
              return RemoveSections.find(Sec) != RemoveSections.end();
            }))
      return E;
  }

  // Transfer removed sections into the Object RemovedSections container for use
  // later.
  std::move(Iter, Sections.end(), std::back_inserter(RemovedSections));
  // Now finally get rid of them all together.
  Sections.erase(Iter, std::end(Sections));
  return Error::success();
}

Error Object::removeSymbols(function_ref<bool(const Symbol &)> ToRemove) {
  if (SymbolTable)
    for (const SecPtr &Sec : Sections)
      if (Error E = Sec->removeSymbols(ToRemove))
        return E;
  return Error::success();
}

void Object::sortSections() {
  // Use stable_sort to maintain the original ordering as closely as possible.
  llvm::stable_sort(Sections, [](const SecPtr &A, const SecPtr &B) {
    // Put SHT_GROUP sections first, since group section headers must come
    // before the sections they contain. This also matches what GNU objcopy
    // does.
    if (A->Type != B->Type &&
        (A->Type == ELF::SHT_GROUP || B->Type == ELF::SHT_GROUP))
      return A->Type == ELF::SHT_GROUP;
    // For all other sections, sort by offset order.
    return A->OriginalOffset < B->OriginalOffset;
  });
}

// Orders segments such that if x = y->ParentSegment then y comes before x.
static void orderSegments(std::vector<Segment *> &Segments) {
  llvm::stable_sort(Segments, compareSegmentsByOffset);
}

// This function finds a consistent layout for a list of segments starting from
// an Offset. It assumes that Segments have been sorted by orderSegments and
// returns an Offset one past the end of the last segment.
static uint64_t layoutSegments(std::vector<Segment *> &Segments,
                               uint64_t Offset) {
  assert(llvm::is_sorted(Segments, compareSegmentsByOffset));
  // The only way a segment should move is if a section was between two
  // segments and that section was removed. If that section isn't in a segment
  // then it's acceptable, but not ideal, to simply move it to after the
  // segments. So we can simply layout segments one after the other accounting
  // for alignment.
  for (Segment *Seg : Segments) {
    // We assume that segments have been ordered by OriginalOffset and Index
    // such that a parent segment will always come before a child segment in
    // OrderedSegments. This means that the Offset of the ParentSegment should
    // already be set and we can set our offset relative to it.
    if (Seg->ParentSegment != nullptr) {
      Segment *Parent = Seg->ParentSegment;
      Seg->Offset =
          Parent->Offset + Seg->OriginalOffset - Parent->OriginalOffset;
    } else {
      Seg->Offset =
          alignTo(Offset, std::max<uint64_t>(Seg->Align, 1), Seg->VAddr);
    }
    Offset = std::max(Offset, Seg->Offset + Seg->FileSize);
  }
  return Offset;
}

// This function finds a consistent layout for a list of sections. It assumes
// that the ->ParentSegment of each section has already been laid out. The
// supplied starting Offset is used for the starting offset of any section that
// does not have a ParentSegment. It returns either the offset given if all
// sections had a ParentSegment or an offset one past the last section if there
// was a section that didn't have a ParentSegment.
template <class Range>
static uint64_t layoutSections(Range Sections, uint64_t Offset) {
  // Now the offset of every segment has been set we can assign the offsets
  // of each section. For sections that are covered by a segment we should use
  // the segment's original offset and the section's original offset to compute
  // the offset from the start of the segment. Using the offset from the start
  // of the segment we can assign a new offset to the section. For sections not
  // covered by segments we can just bump Offset to the next valid location.
  uint32_t Index = 1;
  for (auto &Sec : Sections) {
    Sec.Index = Index++;
    if (Sec.ParentSegment != nullptr) {
      auto Segment = *Sec.ParentSegment;
      Sec.Offset =
          Segment.Offset + (Sec.OriginalOffset - Segment.OriginalOffset);
    } else {
      Offset = alignTo(Offset, Sec.Align == 0 ? 1 : Sec.Align);
      Sec.Offset = Offset;
      if (Sec.Type != SHT_NOBITS)
        Offset += Sec.Size;
    }
  }
  return Offset;
}

// Rewrite sh_offset after some sections are changed to SHT_NOBITS and thus
// occupy no space in the file.
static uint64_t layoutSectionsForOnlyKeepDebug(Object &Obj, uint64_t Off) {
  uint32_t Index = 1;
  for (auto &Sec : Obj.sections()) {
    Sec.Index = Index++;

    auto *FirstSec = Sec.ParentSegment && Sec.ParentSegment->Type == PT_LOAD
                         ? Sec.ParentSegment->firstSection()
                         : nullptr;

    // The first section in a PT_LOAD has to have congruent offset and address
    // modulo the alignment, which usually equals the maximum page size.
    if (FirstSec && FirstSec == &Sec)
      Off = alignTo(Off, Sec.ParentSegment->Align, Sec.Addr);

    // sh_offset is not significant for SHT_NOBITS sections, but the congruence
    // rule must be followed if it is the first section in a PT_LOAD. Do not
    // advance Off.
    if (Sec.Type == SHT_NOBITS) {
      Sec.Offset = Off;
      continue;
    }

    if (!FirstSec) {
      // FirstSec being nullptr generally means that Sec does not have the
      // SHF_ALLOC flag.
      Off = Sec.Align ? alignTo(Off, Sec.Align) : Off;
    } else if (FirstSec != &Sec) {
      // The offset is relative to the first section in the PT_LOAD segment. Use
      // sh_offset for non-SHF_ALLOC sections.
      Off = Sec.OriginalOffset - FirstSec->OriginalOffset + FirstSec->Offset;
    }
    Sec.Offset = Off;
    Off += Sec.Size;
  }
  return Off;
}

// Rewrite p_offset and p_filesz of non-empty non-PT_PHDR segments after
// sh_offset values have been updated.
static uint64_t layoutSegmentsForOnlyKeepDebug(std::vector<Segment *> &Segments,
                                               uint64_t HdrEnd) {
  uint64_t MaxOffset = 0;
  for (Segment *Seg : Segments) {
    const SectionBase *FirstSec = Seg->firstSection();
    if (Seg->Type == PT_PHDR || !FirstSec)
      continue;

    uint64_t Offset = FirstSec->Offset;
    uint64_t FileSize = 0;
    for (const SectionBase *Sec : Seg->Sections) {
      uint64_t Size = Sec->Type == SHT_NOBITS ? 0 : Sec->Size;
      if (Sec->Offset + Size > Offset)
        FileSize = std::max(FileSize, Sec->Offset + Size - Offset);
    }

    // If the segment includes EHDR and program headers, don't make it smaller
    // than the headers.
    if (Seg->Offset < HdrEnd && HdrEnd <= Seg->Offset + Seg->FileSize) {
      FileSize += Offset - Seg->Offset;
      Offset = Seg->Offset;
      FileSize = std::max(FileSize, HdrEnd - Offset);
    }

    Seg->Offset = Offset;
    Seg->FileSize = FileSize;
    MaxOffset = std::max(MaxOffset, Offset + FileSize);
  }
  return MaxOffset;
}

template <class ELFT> void ELFWriter<ELFT>::initEhdrSegment() {
  Segment &ElfHdr = Obj.ElfHdrSegment;
  ElfHdr.Type = PT_PHDR;
  ElfHdr.Flags = 0;
  ElfHdr.VAddr = 0;
  ElfHdr.PAddr = 0;
  ElfHdr.FileSize = ElfHdr.MemSize = sizeof(Elf_Ehdr);
  ElfHdr.Align = 0;
}

template <class ELFT> void ELFWriter<ELFT>::assignOffsets() {
  // We need a temporary list of segments that has a special order to it
  // so that we know that anytime ->ParentSegment is set that segment has
  // already had its offset properly set.
  std::vector<Segment *> OrderedSegments;
  for (Segment &Segment : Obj.segments())
    OrderedSegments.push_back(&Segment);
  OrderedSegments.push_back(&Obj.ElfHdrSegment);
  OrderedSegments.push_back(&Obj.ProgramHdrSegment);
  orderSegments(OrderedSegments);

  uint64_t Offset;
  if (OnlyKeepDebug) {
    // For --only-keep-debug, the sections that did not preserve contents were
    // changed to SHT_NOBITS. We now rewrite sh_offset fields of sections, and
    // then rewrite p_offset/p_filesz of program headers.
    uint64_t HdrEnd =
        sizeof(Elf_Ehdr) + llvm::size(Obj.segments()) * sizeof(Elf_Phdr);
    Offset = layoutSectionsForOnlyKeepDebug(Obj, HdrEnd);
    Offset = std::max(Offset,
                      layoutSegmentsForOnlyKeepDebug(OrderedSegments, HdrEnd));
  } else {
    // Offset is used as the start offset of the first segment to be laid out.
    // Since the ELF Header (ElfHdrSegment) must be at the start of the file,
    // we start at offset 0.
    Offset = layoutSegments(OrderedSegments, 0);
    Offset = layoutSections(Obj.sections(), Offset);
  }
  // If we need to write the section header table out then we need to align the
  // Offset so that SHOffset is valid.
  if (WriteSectionHeaders)
    Offset = alignTo(Offset, sizeof(Elf_Addr));
  Obj.SHOff = Offset;
}

template <class ELFT> size_t ELFWriter<ELFT>::totalSize() const {
  // We already have the section header offset so we can calculate the total
  // size by just adding up the size of each section header.
  if (!WriteSectionHeaders)
    return Obj.SHOff;
  size_t ShdrCount = Obj.sections().size() + 1; // Includes null shdr.
  return Obj.SHOff + ShdrCount * sizeof(Elf_Shdr);
}

template <class ELFT> Error ELFWriter<ELFT>::write() {
  // Segment data must be written first, so that the ELF header and program
  // header tables can overwrite it, if covered by a segment.
  writeSegmentData();
  writeEhdr();
  writePhdrs();
  writeSectionData();
  if (WriteSectionHeaders)
    writeShdrs();
  return Buf.commit();
}

static Error removeUnneededSections(Object &Obj) {
  // We can remove an empty symbol table from non-relocatable objects.
  // Relocatable objects typically have relocation sections whose
  // sh_link field points to .symtab, so we can't remove .symtab
  // even if it is empty.
  if (Obj.isRelocatable() || Obj.SymbolTable == nullptr ||
      !Obj.SymbolTable->empty())
    return Error::success();

  // .strtab can be used for section names. In such a case we shouldn't
  // remove it.
  auto *StrTab = Obj.SymbolTable->getStrTab() == Obj.SectionNames
                     ? nullptr
                     : Obj.SymbolTable->getStrTab();
  return Obj.removeSections(false, [&](const SectionBase &Sec) {
    return &Sec == Obj.SymbolTable || &Sec == StrTab;
  });
}

template <class ELFT> Error ELFWriter<ELFT>::finalize() {
  // It could happen that SectionNames has been removed and yet the user wants
  // a section header table output. We need to throw an error if a user tries
  // to do that.
  if (Obj.SectionNames == nullptr && WriteSectionHeaders)
    return createStringError(llvm::errc::invalid_argument,
                             "cannot write section header table because "
                             "section header string table was removed");

  if (Error E = removeUnneededSections(Obj))
    return E;
  Obj.sortSections();

  // We need to assign indexes before we perform layout because we need to know
  // if we need large indexes or not. We can assign indexes first and check as
  // we go to see if we will actully need large indexes.
  bool NeedsLargeIndexes = false;
  if (Obj.sections().size() >= SHN_LORESERVE) {
    SectionTableRef Sections = Obj.sections();
    NeedsLargeIndexes =
        std::any_of(Sections.begin() + SHN_LORESERVE, Sections.end(),
                    [](const SectionBase &Sec) { return Sec.HasSymbol; });
    // TODO: handle case where only one section needs the large index table but
    // only needs it because the large index table hasn't been removed yet.
  }

  if (NeedsLargeIndexes) {
    // This means we definitely need to have a section index table but if we
    // already have one then we should use it instead of making a new one.
    if (Obj.SymbolTable != nullptr && Obj.SectionIndexTable == nullptr) {
      // Addition of a section to the end does not invalidate the indexes of
      // other sections and assigns the correct index to the new section.
      auto &Shndx = Obj.addSection<SectionIndexSection>();
      Obj.SymbolTable->setShndxTable(&Shndx);
      Shndx.setSymTab(Obj.SymbolTable);
    }
  } else {
    // Since we don't need SectionIndexTable we should remove it and all
    // references to it.
    if (Obj.SectionIndexTable != nullptr) {
      // We do not support sections referring to the section index table.
      if (Error E = Obj.removeSections(false /*AllowBrokenLinks*/,
                                       [this](const SectionBase &Sec) {
                                         return &Sec == Obj.SectionIndexTable;
                                       }))
        return E;
    }
  }

  // Make sure we add the names of all the sections. Importantly this must be
  // done after we decide to add or remove SectionIndexes.
  if (Obj.SectionNames != nullptr)
    for (const SectionBase &Sec : Obj.sections())
      Obj.SectionNames->addString(Sec.Name);

  initEhdrSegment();

  // Before we can prepare for layout the indexes need to be finalized.
  // Also, the output arch may not be the same as the input arch, so fix up
  // size-related fields before doing layout calculations.
  uint64_t Index = 0;
  auto SecSizer = std::make_unique<ELFSectionSizer<ELFT>>();
  for (SectionBase &Sec : Obj.sections()) {
    Sec.Index = Index++;
    Sec.accept(*SecSizer);
  }

  // The symbol table does not update all other sections on update. For
  // instance, symbol names are not added as new symbols are added. This means
  // that some sections, like .strtab, don't yet have their final size.
  if (Obj.SymbolTable != nullptr)
    Obj.SymbolTable->prepareForLayout();

  // Now that all strings are added we want to finalize string table builders,
  // because that affects section sizes which in turn affects section offsets.
  for (SectionBase &Sec : Obj.sections())
    if (auto StrTab = dyn_cast<StringTableSection>(&Sec))
      StrTab->prepareForLayout();

  assignOffsets();

  // layoutSections could have modified section indexes, so we need
  // to fill the index table after assignOffsets.
  if (Obj.SymbolTable != nullptr)
    Obj.SymbolTable->fillShndxTable();

  // Finally now that all offsets and indexes have been set we can finalize any
  // remaining issues.
  uint64_t Offset = Obj.SHOff + sizeof(Elf_Shdr);
  for (SectionBase &Sec : Obj.sections()) {
    Sec.HeaderOffset = Offset;
    Offset += sizeof(Elf_Shdr);
    if (WriteSectionHeaders)
      Sec.NameIndex = Obj.SectionNames->findIndex(Sec.Name);
    Sec.finalize();
  }

  if (Error E = Buf.allocate(totalSize()))
    return E;
  SecWriter = std::make_unique<ELFSectionWriter<ELFT>>(Buf);
  return Error::success();
}

Error BinaryWriter::write() {
  for (const SectionBase &Sec : Obj.allocSections())
    Sec.accept(*SecWriter);
  return Buf.commit();
}

Error BinaryWriter::finalize() {
  // We need a temporary list of segments that has a special order to it
  // so that we know that anytime ->ParentSegment is set that segment has
  // already had it's offset properly set. We only want to consider the segments
  // that will affect layout of allocated sections so we only add those.
  std::vector<Segment *> OrderedSegments;
  for (const SectionBase &Sec : Obj.allocSections())
    if (Sec.ParentSegment != nullptr)
      OrderedSegments.push_back(Sec.ParentSegment);

  // For binary output, we're going to use physical addresses instead of
  // virtual addresses, since a binary output is used for cases like ROM
  // loading and physical addresses are intended for ROM loading.
  // However, if no segment has a physical address, we'll fallback to using
  // virtual addresses for all.
  if (all_of(OrderedSegments,
             [](const Segment *Seg) { return Seg->PAddr == 0; }))
    for (Segment *Seg : OrderedSegments)
      Seg->PAddr = Seg->VAddr;

  llvm::stable_sort(OrderedSegments, compareSegmentsByPAddr);

  // Because we add a ParentSegment for each section we might have duplicate
  // segments in OrderedSegments. If there were duplicates then layoutSegments
  // would do very strange things.
  auto End =
      std::unique(std::begin(OrderedSegments), std::end(OrderedSegments));
  OrderedSegments.erase(End, std::end(OrderedSegments));

  // Compute the section LMA based on its sh_offset and the containing segment's
  // p_offset and p_paddr. Also compute the minimum LMA of all sections as
  // MinAddr. In the output, the contents between address 0 and MinAddr will be
  // skipped.
  uint64_t MinAddr = UINT64_MAX;
  for (SectionBase &Sec : Obj.allocSections()) {
    if (Sec.ParentSegment != nullptr)
      Sec.Addr =
          Sec.Offset - Sec.ParentSegment->Offset + Sec.ParentSegment->PAddr;
    MinAddr = std::min(MinAddr, Sec.Addr);
  }

  // Now that every section has been laid out we just need to compute the total
  // file size. This might not be the same as the offset returned by
  // layoutSections, because we want to truncate the last segment to the end of
  // its last section, to match GNU objcopy's behaviour.
  TotalSize = 0;
  for (SectionBase &Sec : Obj.allocSections()) {
    Sec.Offset = Sec.Addr - MinAddr;
    if (Sec.Type != SHT_NOBITS)
      TotalSize = std::max(TotalSize, Sec.Offset + Sec.Size);
  }

  if (Error E = Buf.allocate(TotalSize))
    return E;
  SecWriter = std::make_unique<BinarySectionWriter>(Buf);
  return Error::success();
}

bool IHexWriter::SectionCompare::operator()(const SectionBase *Lhs,
                                            const SectionBase *Rhs) const {
  return (sectionPhysicalAddr(Lhs) & 0xFFFFFFFFU) <
         (sectionPhysicalAddr(Rhs) & 0xFFFFFFFFU);
}

uint64_t IHexWriter::writeEntryPointRecord(uint8_t *Buf) {
  IHexLineData HexData;
  uint8_t Data[4] = {};
  // We don't write entry point record if entry is zero.
  if (Obj.Entry == 0)
    return 0;

  if (Obj.Entry <= 0xFFFFFU) {
    Data[0] = ((Obj.Entry & 0xF0000U) >> 12) & 0xFF;
    support::endian::write(&Data[2], static_cast<uint16_t>(Obj.Entry),
                           support::big);
    HexData = IHexRecord::getLine(IHexRecord::StartAddr80x86, 0, Data);
  } else {
    support::endian::write(Data, static_cast<uint32_t>(Obj.Entry),
                           support::big);
    HexData = IHexRecord::getLine(IHexRecord::StartAddr, 0, Data);
  }
  memcpy(Buf, HexData.data(), HexData.size());
  return HexData.size();
}

uint64_t IHexWriter::writeEndOfFileRecord(uint8_t *Buf) {
  IHexLineData HexData = IHexRecord::getLine(IHexRecord::EndOfFile, 0, {});
  memcpy(Buf, HexData.data(), HexData.size());
  return HexData.size();
}

Error IHexWriter::write() {
  IHexSectionWriter Writer(Buf);
  // Write sections.
  for (const SectionBase *Sec : Sections)
    Sec->accept(Writer);

  uint64_t Offset = Writer.getBufferOffset();
  // Write entry point address.
  Offset += writeEntryPointRecord(Buf.getBufferStart() + Offset);
  // Write EOF.
  Offset += writeEndOfFileRecord(Buf.getBufferStart() + Offset);
  assert(Offset == TotalSize);
  return Buf.commit();
}

Error IHexWriter::checkSection(const SectionBase &Sec) {
  uint64_t Addr = sectionPhysicalAddr(&Sec);
  if (addressOverflows32bit(Addr) || addressOverflows32bit(Addr + Sec.Size - 1))
    return createStringError(
        errc::invalid_argument,
        "Section '%s' address range [0x%llx, 0x%llx] is not 32 bit", Sec.Name.c_str(),
        Addr, Addr + Sec.Size - 1);
  return Error::success();
}

Error IHexWriter::finalize() {
  bool UseSegments = false;
  auto ShouldWrite = [](const SectionBase &Sec) {
    return (Sec.Flags & ELF::SHF_ALLOC) && (Sec.Type != ELF::SHT_NOBITS);
  };
  auto IsInPtLoad = [](const SectionBase &Sec) {
    return Sec.ParentSegment && Sec.ParentSegment->Type == ELF::PT_LOAD;
  };

  // We can't write 64-bit addresses.
  if (addressOverflows32bit(Obj.Entry))
    return createStringError(errc::invalid_argument,
                             "Entry point address 0x%llx overflows 32 bits.",
                             Obj.Entry);

  // If any section we're to write has segment then we
  // switch to using physical addresses. Otherwise we
  // use section virtual address.
  for (const SectionBase &Sec : Obj.sections())
    if (ShouldWrite(Sec) && IsInPtLoad(Sec)) {
      UseSegments = true;
      break;
    }

  for (const SectionBase &Sec : Obj.sections())
    if (ShouldWrite(Sec) && (!UseSegments || IsInPtLoad(Sec))) {
      if (Error E = checkSection(Sec))
        return E;
      Sections.insert(&Sec);
    }

  IHexSectionWriterBase LengthCalc(Buf);
  for (const SectionBase *Sec : Sections)
    Sec->accept(LengthCalc);

  // We need space to write section records + StartAddress record
  // (if start adress is not zero) + EndOfFile record.
  TotalSize = LengthCalc.getBufferOffset() +
              (Obj.Entry ? IHexRecord::getLineLength(4) : 0) +
              IHexRecord::getLineLength(0);
  if (Error E = Buf.allocate(TotalSize))
    return E;
  return Error::success();
}

template class ELFBuilder<ELF64LE>;
template class ELFBuilder<ELF64BE>;
template class ELFBuilder<ELF32LE>;
template class ELFBuilder<ELF32BE>;

template class ELFWriter<ELF64LE>;
template class ELFWriter<ELF64BE>;
template class ELFWriter<ELF32LE>;
template class ELFWriter<ELF32BE>;

} // end namespace elf
} // end namespace objcopy
} // end namespace llvm
