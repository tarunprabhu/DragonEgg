//===------- Constants.cpp - Converting and working with constants --------===//
//
// Copyright (C) 2011  Duncan Sands
//
// This file is part of DragonEgg.
//
// DragonEgg is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2, or (at your option) any later version.
//
// DragonEgg is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// DragonEgg; see the file COPYING.  If not, write to the Free Software
// Foundation, 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
//
//===----------------------------------------------------------------------===//
// This is the code that converts GCC constants to LLVM.
//===----------------------------------------------------------------------===//

// Plugin headers
#include "Constants.h"
#include "Internals.h"
#include "Trees.h"
#include "ADT/IntervalList.h"
#include "ADT/Range.h"

// LLVM headers
#include "llvm/GlobalVariable.h"
#include "llvm/LLVMContext.h"
#include "llvm/Support/Host.h"
#include "llvm/Target/TargetData.h"

// System headers
#include <gmp.h>

// GCC headers
extern "C" {
#include "config.h"
// Stop GCC declaring 'getopt' as it can clash with the system's declaration.
#undef HAVE_DECL_GETOPT
#include "system.h"
#include "coretypes.h"
#include "target.h"
#include "tree.h"

#include "flags.h"
#include "tm_p.h"
}

static LLVMContext &Context = getGlobalContext();

//===----------------------------------------------------------------------===//
//                           ... InterpretAsType ...
//===----------------------------------------------------------------------===//

typedef Range<int> SignedRange;

/// BitSlice - A contiguous range of bits held in memory.
class BitSlice {
  SignedRange R;
  Constant *Contents; // Null if and only if the range is empty.

  bool contentsValid() const {
    if (empty())
      return !Contents;
    return Contents && isa<IntegerType>(Contents->getType()) &&
      getBitWidth() == Contents->getType()->getPrimitiveSizeInBits();
  }

public:
  /// BitSlice - Default constructor: empty bit range.
  BitSlice() : R(), Contents(0) {}

  /// BitSlice - Constructor for the given range of bits.  The bits themselves
  /// are supplied in 'contents' as a constant of integer type (if the range is
  /// empty then 'contents' must be null).  On little-endian machines the least
  /// significant bit of 'contents' corresponds to the first bit of the range
  /// (aka "First"), while on big-endian machines it corresponds to the last bit
  /// of the range (aka "Last-1").
  BitSlice(SignedRange r, Constant *contents) : R(r), Contents(contents) {
    assert(contentsValid() && "Contents do not match range");
  }

  /// BitSlice - Constructor for the range of bits ['first', 'last').
  BitSlice(int first, int last, Constant *contents)
    : R(first, last), Contents(contents) {
    assert(contentsValid() && "Contents do not match range");
  }

  /// empty - Return whether the bit range is empty.
  bool empty() const {
    return R.empty();
  }

  /// getBitWidth - Return the number of bits in the range.
  unsigned getBitWidth() const {
    return R.getWidth();
  }

  /// getFirst - Return the position of the first bit in the range.
  unsigned getFirst() const {
    return R.getFirst();
  }

  /// getLast - Return the position of the last bit defining the range.
  unsigned getLast() const {
    return R.getLast();
  }

  /// getRange - Return the range of bits in this slice.
  SignedRange getRange() const {
    return R;
  }

  /// Displace - Return the result of sliding all bits by the given offset.
  BitSlice Displace(int Offset) const {
    return BitSlice(R.Displace(Offset), Contents);
  }

  /// getBits - Return the bits in the given range.  The supplied range need not
  /// be contained in the range of the slice, but if not then the bits outside
  /// the slice get an undefined value.  The bits are returned as a constant of
  /// integer type.  On little-endian machine the least significant bit of the
  /// returned value corresponds to the first bit of the range (aka "First"),
  /// while on big-endian machines it corresponds to the last bit of the range
  /// (aka "Last-1").
  Constant *getBits(SignedRange r) const;

  /// ExtendRange - Extend the slice to a wider range.  The value of the added
  /// bits is undefined.
  BitSlice ExtendRange(SignedRange r) const;

  /// ReduceRange - Reduce the slice to a smaller range.
  BitSlice ReduceRange(SignedRange r) const;

  /// Merge - Join the slice with another (which must be disjoint), forming the
  /// convex hull of the ranges.  The bits in the range of one of the slices are
  /// those of that slice.  Any other bits have an undefined value.
  void Merge(const BitSlice &other);
};

/// ExtendRange - Extend the slice to a wider range.  The value of the added
/// bits is undefined.
BitSlice BitSlice::ExtendRange(SignedRange r) const {
  assert(r.contains(R) && "Not an extension!");
  // Quick exit if the range did not actually increase.
  if (R == r)
    return *this;
  assert(!r.empty() && "Empty ranges did not evaluate as equal?");
  const Type *ExtTy = IntegerType::get(Context, r.getWidth());
  // If the slice contains no bits then every bit of the extension is undefined.
  if (empty())
    return BitSlice(r, UndefValue::get(ExtTy));
  // Extend the contents to the new type.
  Constant *C = TheFolder->CreateZExtOrBitCast(Contents, ExtTy);
  // Position the old contents correctly inside the new contents.
  unsigned deltaFirst = R.getFirst() - r.getFirst();
  unsigned deltaLast = r.getLast() - R.getLast();
  if (BYTES_BIG_ENDIAN && deltaLast) {
    Constant *ShiftAmt = ConstantInt::get(C->getType(), deltaLast);
    C = TheFolder->CreateShl(C, ShiftAmt);
  } else if (!BYTES_BIG_ENDIAN && deltaFirst) {
    Constant *ShiftAmt = ConstantInt::get(C->getType(), deltaFirst);
    C = TheFolder->CreateShl(C, ShiftAmt);
  }
  return BitSlice(r, C);
}

/// getBits - Return the bits in the given range.  The supplied range need not
/// be contained in the range of the slice, but if not then the bits outside
/// the slice get an undefined value.  The bits are returned as a constant of
/// integer type.  On little-endian machine the least significant bit of the
/// returned value corresponds to the first bit of the range (aka "First"),
/// while on big-endian machines it corresponds to the last bit of the range
/// (aka "Last-1").
Constant *BitSlice::getBits(SignedRange r) const {
  assert(!r.empty() && "Bit range is empty!");
  // Quick exit if the desired range matches that of the slice.
  if (R == r)
    return Contents;
  const Type *RetTy = IntegerType::get(Context, r.getWidth());
  // If the slice contains no bits then every returned bit is undefined.
  if (empty())
    return UndefValue::get(RetTy);
  // Extend to the convex hull of the two ranges.
  BitSlice Slice = ExtendRange(R.Join(r));
  // Chop the slice down to the requested range.
  Slice = Slice.ReduceRange(r);
  // Now we can just return the bits contained in the slice.
  return Slice.Contents;
}

/// Merge - Join the slice with another (which must be disjoint), forming the
/// convex hull of the ranges.  The bits in the range of one of the slices are
/// those of that slice.  Any other bits have an undefined value.
void BitSlice::Merge(const BitSlice &other) {
  // If the other slice is empty, the result is this slice.
  if (other.empty())
    return;
  // If this slice is empty, the result is the other slice.
  if (empty()) {
    *this = other;
    return;
  }
  assert(!R.intersects(other.getRange()) && "Slices overlap!");

  // Extend each slice to the convex hull of the ranges.
  SignedRange Hull = R.Join(other.getRange());
  BitSlice ExtThis = ExtendRange(Hull);
  BitSlice ExtOther = other.ExtendRange(Hull);

  // The extra bits added when extending a slice may contain anything.  In each
  // extended slice clear the bits corresponding to the other slice.
  int HullFirst = Hull.getFirst(), HullLast = Hull.getLast();
  unsigned HullWidth = Hull.getWidth();
  // Compute masks with the bits for each slice set to 1.
  APInt ThisBits, OtherBits;
  if (BYTES_BIG_ENDIAN) {
    ThisBits = APInt::getBitsSet(HullWidth, HullLast - getLast(),
                                 HullLast - getFirst());
    OtherBits = APInt::getBitsSet(HullWidth, HullLast - other.getLast(),
                                  HullLast - other.getFirst());
  } else {
    ThisBits = APInt::getBitsSet(HullWidth, getFirst() - HullFirst,
                                 getLast() - HullFirst);
    OtherBits = APInt::getBitsSet(HullWidth, other.getFirst() - HullFirst,
                                  other.getLast() - HullFirst);
  }
  // Clear bits which correspond to the other slice.
  ConstantInt *ClearThis = ConstantInt::get(Context, ~ThisBits);
  ConstantInt *ClearOther = ConstantInt::get(Context, ~OtherBits);
  Constant *ThisPart = TheFolder->CreateAnd(ExtThis.Contents, ClearOther);
  Constant *OtherPart = TheFolder->CreateAnd(ExtOther.Contents, ClearThis);

  // The slices can now be joined via a simple 'or'.
  *this = BitSlice(Hull, TheFolder->CreateOr(ThisPart, OtherPart));
}

/// ReduceRange - Reduce the slice to a smaller range.
BitSlice BitSlice::ReduceRange(SignedRange r) const {
  assert(R.contains(r) && "Not a reduction!");
  // Quick exit if the range did not actually decrease.
  if (R == r)
    return *this;
  // The trivial case of reducing to an empty range.
  if (r.empty())
    return BitSlice();
  assert(!R.empty() && "Empty ranges did not evaluate as equal?");
  // Move the least-significant bit to the correct position.
  Constant *C = Contents;
  unsigned deltaFirst = r.getFirst() - R.getFirst();
  unsigned deltaLast = R.getLast() - r.getLast();
  if (BYTES_BIG_ENDIAN && deltaLast) {
    Constant *ShiftAmt = ConstantInt::get(C->getType(), deltaLast);
    C = TheFolder->CreateLShr(C, ShiftAmt);
  } else if (!BYTES_BIG_ENDIAN && deltaFirst) {
    Constant *ShiftAmt = ConstantInt::get(C->getType(), deltaFirst);
    C = TheFolder->CreateLShr(C, ShiftAmt);
  }
  // Truncate to the new type.
  const Type *RedTy = IntegerType::get(Context, r.getWidth());
  C = TheFolder->CreateTruncOrBitCast(C, RedTy);
  return BitSlice(r, C);
}

/// ViewAsBits - View the given constant as a bunch of bits, i.e. as one big
/// integer.  Only the bits in the given range are needed, so there is no need
/// to supply bits outside this range though it is harmless to do so.  There is
/// also no need to supply undefined bits inside the range.
static BitSlice ViewAsBits(Constant *C, SignedRange R) {
  if (R.empty())
    return BitSlice();

  // Sanitize the range to make life easier in what follows.
  const Type *Ty = C->getType();
  int StoreSize = getTargetData().getTypeStoreSizeInBits(Ty);
  R = R.Meet(SignedRange(0, StoreSize));

  // Quick exit if it is clear that there are no bits in the range.
  if (R.empty())
    return BitSlice();
  assert(StoreSize > 0 && "Empty range not eliminated?");

  switch (Ty->getTypeID()) {
  default:
    DieAbjectly("Unsupported type!");
  case Type::PointerTyID: {
    // Cast to an integer with the same number of bits and return that.
    const IntegerType *IntTy = getTargetData().getIntPtrType(Context);
    return BitSlice(0, StoreSize, TheFolder->CreatePtrToInt(C, IntTy));
  }
  case Type::DoubleTyID:
  case Type::FloatTyID:
  case Type::FP128TyID:
  case Type::IntegerTyID:
  case Type::PPC_FP128TyID:
  case Type::X86_FP80TyID:
  case Type::X86_MMXTyID: {
    // Bitcast to an integer with the same number of bits and return that.
    unsigned BitWidth = Ty->getPrimitiveSizeInBits();
    const IntegerType *IntTy = IntegerType::get(Context, BitWidth);
    C = TheFolder->CreateBitCast(C, IntTy);
    // Be careful about where the bits are placed in case this is a funky type
    // like i1.  If the width is a multiple of the address unit then there is
    // nothing to worry about: the bits occupy the range [0, StoreSize).  But
    // if not then endianness matters: on big-endian machines there are padding
    // bits at the start, while on little-endian machines they are at the end.
    return BYTES_BIG_ENDIAN ?
      BitSlice(StoreSize - BitWidth, StoreSize, C) : BitSlice(0, BitWidth, C);
  }

  case Type::ArrayTyID: {
    const ArrayType *ATy = cast<ArrayType>(Ty);
    const Type *EltTy = ATy->getElementType();
    const unsigned NumElts = ATy->getNumElements();
    const unsigned Stride = getTargetData().getTypeAllocSizeInBits(EltTy);
    assert(Stride > 0 && "Store size smaller than alloc size?");
    // Elements with indices in [FirstElt, LastElt) overlap the range.
    unsigned FirstElt = R.getFirst() / Stride;
    unsigned LastElt = (R.getLast() + Stride - 1) / Stride;
    assert(LastElt <= NumElts && "Store size bigger than array?");
    // Visit all elements that overlap the requested range, accumulating their
    // bits in Bits.
    BitSlice Bits;
    SignedRange StrideRange(0, Stride);
    for (unsigned i = FirstElt; i < LastElt; ++i) {
      // Extract the element.
      Constant *Elt = TheFolder->CreateExtractValue(C, &i, 1);
      // View it as a bunch of bits.
      BitSlice EltBits = ViewAsBits(Elt, StrideRange);
      // Add to the already known bits.
      Bits.Merge(EltBits.Displace(i * Stride));
    }
    return Bits;
  }

  case Type::StructTyID: {
    const StructType *STy = cast<StructType>(Ty);
    const StructLayout *SL = getTargetData().getStructLayout(STy);
    // Fields with indices in [FirstIdx, LastIdx) overlap the range.
    unsigned FirstIdx = SL->getElementContainingOffset((R.getFirst()+7)/8);
    unsigned LastIdx = 1 + SL->getElementContainingOffset((R.getLast()+6)/8);
    // Visit all fields that overlap the requested range, accumulating their
    // bits in Bits.
    BitSlice Bits;
    for (unsigned i = FirstIdx; i < LastIdx; ++i) {
      // Extract the field.
      Constant *Field = TheFolder->CreateExtractValue(C, &i, 1);
      // View it as a bunch of bits.
      const Type *FieldTy = Field->getType();
      unsigned FieldStoreSize = getTargetData().getTypeStoreSizeInBits(FieldTy);
      BitSlice FieldBits = ViewAsBits(Field, SignedRange(0, FieldStoreSize));
      // Add to the already known bits.
      Bits.Merge(FieldBits.Displace(SL->getElementOffset(i)*8));
    }
    return Bits;
  }

  case Type::VectorTyID: {
    const VectorType *VTy = cast<VectorType>(Ty);
    const Type *EltTy = VTy->getElementType();
    const unsigned NumElts = VTy->getNumElements();
    const unsigned Stride = getTargetData().getTypeAllocSizeInBits(EltTy);
    assert(Stride > 0 && "Store size smaller than alloc size?");
    // Elements with indices in [FirstElt, LastElt) overlap the range.
    unsigned FirstElt = R.getFirst() / Stride;
    unsigned LastElt = (R.getLast() + Stride - 1) / Stride;
    assert(LastElt <= NumElts && "Store size bigger than vector?");
    // Visit all elements that overlap the requested range, accumulating their
    // bits in Bits.
    BitSlice Bits;
    SignedRange StrideRange(0, Stride);
    for (unsigned i = FirstElt; i < LastElt; ++i) {
      // Extract the element.
      ConstantInt *Idx = ConstantInt::get(Type::getInt32Ty(Context), i);
      Constant *Elt = TheFolder->CreateExtractElement(C, Idx);
      // View it as a bunch of bits.
      BitSlice EltBits = ViewAsBits(Elt, StrideRange);
      // Add to the already known bits.
      Bits.Merge(EltBits.Displace(i * Stride));
    }
    return Bits;
  }
  }
}

/// InterpretAsType - Interpret the bits of the given constant (starting from
/// StartingBit) as representing a constant of type 'Ty'.  This results in the
/// same constant as you would get by storing the bits of 'C' to memory (with
/// the first bit stored being 'StartingBit') and then loading out a (constant)
/// value of type 'Ty' from the stored to memory location.
Constant *InterpretAsType(Constant *C, const Type* Ty, int StartingBit) {
  if (C->getType() == Ty)
    return C;

  switch (Ty->getTypeID()) {
  default:
    DieAbjectly("Unsupported type!");
  case Type::IntegerTyID: {
    unsigned BitWidth = Ty->getPrimitiveSizeInBits();
    unsigned StoreSize = getTargetData().getTypeStoreSizeInBits(Ty);
    // Convert the constant into a bunch of bits.  Only the bits to be "loaded"
    // out are needed, so rather than converting the entire constant this only
    // converts enough to get all of the required bits.
    BitSlice Bits = ViewAsBits(C, SignedRange(StartingBit,
                                              StartingBit + StoreSize));
    // Extract the bits used by the integer.  If the integer width is a multiple
    // of the address unit then the endianness of the target doesn't matter.  If
    // not then the padding bits come at the start on big-endian machines and at
    // the end on little-endian machines.
    Bits = Bits.Displace(-StartingBit);
    return BYTES_BIG_ENDIAN ?
      Bits.getBits(SignedRange(StoreSize - BitWidth, StoreSize)) :
      Bits.getBits(SignedRange(0, BitWidth));
  }

  case Type::PointerTyID: {
    // Interpret as an integer with the same number of bits then cast back to
    // the original type.
    const IntegerType *IntTy = getTargetData().getIntPtrType(Context);
    C = InterpretAsType(C, IntTy, StartingBit);
    return TheFolder->CreateIntToPtr(C, Ty);
  }
  case Type::DoubleTyID:
  case Type::FloatTyID:
  case Type::FP128TyID:
  case Type::PPC_FP128TyID:
  case Type::X86_FP80TyID:
  case Type::X86_MMXTyID: {
    // Interpret as an integer with the same number of bits then cast back to
    // the original type.
    unsigned BitWidth = Ty->getPrimitiveSizeInBits();
    const IntegerType *IntTy = IntegerType::get(Context, BitWidth);
    return TheFolder->CreateBitCast(InterpretAsType(C, IntTy, StartingBit), Ty);
  }

  case Type::ArrayTyID: {
    // Interpret each array element in turn.
    const ArrayType *ATy = cast<ArrayType>(Ty);
    const Type *EltTy = ATy->getElementType();
    const unsigned Stride = getTargetData().getTypeAllocSizeInBits(EltTy);
    const unsigned NumElts = ATy->getNumElements();
    std::vector<Constant*> Vals(NumElts);
    for (unsigned i = 0; i != NumElts; ++i)
      Vals[i] = InterpretAsType(C, EltTy, StartingBit + i*Stride);
    return ConstantArray::get(ATy, Vals); // TODO: Use ArrayRef constructor.
  }

  case Type::StructTyID: {
    // Interpret each struct field in turn.
    const StructType *STy = cast<StructType>(Ty);
    const StructLayout *SL = getTargetData().getStructLayout(STy);
    unsigned NumElts = STy->getNumElements();
    std::vector<Constant*> Vals(NumElts);
    for (unsigned i = 0; i != NumElts; ++i)
      Vals[i] = InterpretAsType(C, STy->getElementType(i),
                                StartingBit + SL->getElementOffsetInBits(i));
    return ConstantStruct::get(STy, Vals); // TODO: Use ArrayRef constructor.
  }

  case Type::VectorTyID: {
    // Interpret each vector element in turn.
    const VectorType *VTy = cast<VectorType>(Ty);
    const Type *EltTy = VTy->getElementType();
    const unsigned Stride = getTargetData().getTypeAllocSizeInBits(EltTy);
    const unsigned NumElts = VTy->getNumElements();
    SmallVector<Constant*, 16> Vals(NumElts);
    for (unsigned i = 0; i != NumElts; ++i)
      Vals[i] = InterpretAsType(C, EltTy, StartingBit + i*Stride);
    return ConstantVector::get(Vals);
  }
  }
}


//===----------------------------------------------------------------------===//
//                       ... ConvertInitializer ...
//===----------------------------------------------------------------------===//

/// ConvertInitializerWithCast - Convert the initial value for a global variable
/// to an equivalent LLVM constant then cast to the given type if both the type
/// and the initializer are scalar.  This is convenient for making explicit the
/// implicit scalar casts that GCC allows in "assignments" such as initializing
/// a record field.
static Constant *ConvertInitializerWithCast(tree exp, tree type) {
  // Convert the initializer.
  Constant *C = ConvertInitializer(exp);

  // If no cast is needed, or it would not be a scalar cast, then just return
  // the initializer as is.
  if (type == TREE_TYPE(exp) || AGGREGATE_TYPE_P(TREE_TYPE(exp)) ||
      AGGREGATE_TYPE_P(type))
    return C;
  const Type *SrcTy = ConvertType(TREE_TYPE(exp));
  const Type *DestTy = ConvertType(type);
  // LLVM types are often the same even when the GCC types differ.
  if (SrcTy == DestTy)
    return C;

  // First ensure that the initializer has a sensible type.  Note that it would
  // be wrong to interpret the constant as being of type DestTy here since that
  // would not perform a value extension (adding extra zeros or sign bits when
  // casting to a larger integer type for example): any extra bits would get an
  // undefined value instead.
  C = InterpretAsType(C, SrcTy, 0);
  // Now cast to the desired type.
  bool SrcIsSigned = !TYPE_UNSIGNED(TREE_TYPE(exp));
  bool DestIsSigned = !TYPE_UNSIGNED(type);
  Instruction::CastOps opcode = CastInst::getCastOpcode(C, SrcIsSigned, DestTy,
                                                        DestIsSigned);
  return TheFolder->CreateCast(opcode, C, DestTy);
}

/// ConvertCST - Return the given simple constant as an array of bytes.  For the
/// moment only INTEGER_CST, REAL_CST, COMPLEX_CST and VECTOR_CST are supported.
static Constant *ConvertCST(tree exp) {
  const tree type = TREE_TYPE(exp);
  unsigned SizeInChars = (TREE_INT_CST_LOW(TYPE_SIZE(type)) + CHAR_BIT - 1) /
    CHAR_BIT;
  // Encode the constant in Buffer in target format.
  std::vector<unsigned char> Buffer(SizeInChars);
  unsigned CharsWritten = native_encode_expr(exp, &Buffer[0], SizeInChars);
  assert(CharsWritten == SizeInChars && "Failed to fully encode expression!");
  // Turn it into an LLVM byte array.
  return ConstantArray::get(Context, StringRef((char *)&Buffer[0], SizeInChars),
                            /*AddNull*/false);
}

static Constant *ConvertSTRING_CST(tree exp) {
  const ArrayType *StrTy = cast<ArrayType>(ConvertType(TREE_TYPE(exp)));
  const Type *ElTy = StrTy->getElementType();

  unsigned Len = (unsigned)TREE_STRING_LENGTH(exp);

  std::vector<Constant*> Elts;
  if (ElTy->isIntegerTy(8)) {
    const unsigned char *InStr =(const unsigned char *)TREE_STRING_POINTER(exp);
    for (unsigned i = 0; i != Len; ++i)
      Elts.push_back(ConstantInt::get(Type::getInt8Ty(Context), InStr[i]));
  } else if (ElTy->isIntegerTy(16)) {
    assert((Len&1) == 0 &&
           "Length in bytes should be a multiple of element size");
    const uint16_t *InStr =
      (const unsigned short *)TREE_STRING_POINTER(exp);
    for (unsigned i = 0; i != Len/2; ++i) {
      // gcc has constructed the initializer elements in the target endianness,
      // but we're going to treat them as ordinary shorts from here, with
      // host endianness.  Adjust if necessary.
      if (llvm::sys::isBigEndianHost() == BYTES_BIG_ENDIAN)
        Elts.push_back(ConstantInt::get(Type::getInt16Ty(Context), InStr[i]));
      else
        Elts.push_back(ConstantInt::get(Type::getInt16Ty(Context),
                                        ByteSwap_16(InStr[i])));
    }
  } else if (ElTy->isIntegerTy(32)) {
    assert((Len&3) == 0 &&
           "Length in bytes should be a multiple of element size");
    const uint32_t *InStr = (const uint32_t *)TREE_STRING_POINTER(exp);
    for (unsigned i = 0; i != Len/4; ++i) {
      // gcc has constructed the initializer elements in the target endianness,
      // but we're going to treat them as ordinary ints from here, with
      // host endianness.  Adjust if necessary.
      if (llvm::sys::isBigEndianHost() == BYTES_BIG_ENDIAN)
        Elts.push_back(ConstantInt::get(Type::getInt32Ty(Context), InStr[i]));
      else
        Elts.push_back(ConstantInt::get(Type::getInt32Ty(Context),
                                        ByteSwap_32(InStr[i])));
    }
  } else {
    assert(0 && "Unknown character type!");
  }

  unsigned LenInElts = Len /
          TREE_INT_CST_LOW(TYPE_SIZE_UNIT(TREE_TYPE(TREE_TYPE(exp))));
  unsigned ConstantSize = StrTy->getNumElements();

  if (LenInElts != ConstantSize) {
    // If this is a variable sized array type, set the length to LenInElts.
    if (ConstantSize == 0) {
      tree Domain = TYPE_DOMAIN(TREE_TYPE(exp));
      if (!Domain || !TYPE_MAX_VALUE(Domain)) {
        ConstantSize = LenInElts;
        StrTy = ArrayType::get(ElTy, LenInElts);
      }
    }

    if (ConstantSize < LenInElts) {
      // Only some chars are being used, truncate the string: char X[2] = "foo";
      Elts.resize(ConstantSize);
    } else {
      // Fill the end of the string with nulls.
      Constant *C = Constant::getNullValue(ElTy);
      for (; LenInElts != ConstantSize; ++LenInElts)
        Elts.push_back(C);
    }
  }
  return ConstantArray::get(StrTy, Elts);
}

static Constant *ConvertADDR_EXPR(tree exp) {
  return AddressOf(TREE_OPERAND(exp, 0));
}

static Constant *ConvertArrayCONSTRUCTOR(tree exp) {
  // Vectors are like arrays, but the domain is stored via an array
  // type indirectly.

  // If we have a lower bound for the range of the type, get it.
  tree init_type = TREE_TYPE(exp);
  tree elt_type = TREE_TYPE(init_type);

  tree min_element = size_zero_node;
  std::vector<Constant*> ResultElts;

  if (TREE_CODE(init_type) == VECTOR_TYPE) {
    ResultElts.resize(TYPE_VECTOR_SUBPARTS(init_type));
  } else {
    assert(TREE_CODE(init_type) == ARRAY_TYPE && "Unknown type for init");
    tree Domain = TYPE_DOMAIN(init_type);
    if (Domain && TYPE_MIN_VALUE(Domain))
      min_element = fold_convert(sizetype, TYPE_MIN_VALUE(Domain));

    if (Domain && TYPE_MAX_VALUE(Domain)) {
      tree max_element = fold_convert(sizetype, TYPE_MAX_VALUE(Domain));
      tree size = size_binop (MINUS_EXPR, max_element, min_element);
      size = size_binop (PLUS_EXPR, size, size_one_node);

      if (host_integerp(size, 1))
        ResultElts.resize(tree_low_cst(size, 1));
    }
  }

  unsigned NextFieldToFill = 0;
  unsigned HOST_WIDE_INT ix;
  tree elt_index, elt_value;
  Constant *SomeVal = 0;
  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (exp), ix, elt_index, elt_value) {
    // Find and decode the constructor's value.
    Constant *Val = ConvertInitializerWithCast(elt_value, elt_type);
    SomeVal = Val;

    // Get the index position of the element within the array.  Note that this
    // can be NULL_TREE, which means that it belongs in the next available slot.
    tree index = elt_index;

    // The first and last field to fill in, inclusive.
    unsigned FieldOffset, FieldLastOffset;
    if (index && TREE_CODE(index) == RANGE_EXPR) {
      tree first = fold_convert (sizetype, TREE_OPERAND(index, 0));
      tree last  = fold_convert (sizetype, TREE_OPERAND(index, 1));

      first = size_binop (MINUS_EXPR, first, min_element);
      last  = size_binop (MINUS_EXPR, last, min_element);

      assert(host_integerp(first, 1) && host_integerp(last, 1) &&
             "Unknown range_expr!");
      FieldOffset     = tree_low_cst(first, 1);
      FieldLastOffset = tree_low_cst(last, 1);
    } else if (index) {
      index = size_binop (MINUS_EXPR, fold_convert (sizetype, index),
                          min_element);
      assert(host_integerp(index, 1));
      FieldOffset = tree_low_cst(index, 1);
      FieldLastOffset = FieldOffset;
    } else {
      FieldOffset = NextFieldToFill;
      FieldLastOffset = FieldOffset;
    }

    // Process all of the elements in the range.
    for (--FieldOffset; FieldOffset != FieldLastOffset; ) {
      ++FieldOffset;
      if (FieldOffset == ResultElts.size())
        ResultElts.push_back(Val);
      else {
        if (FieldOffset >= ResultElts.size())
          ResultElts.resize(FieldOffset+1);
        ResultElts[FieldOffset] = Val;
      }

      NextFieldToFill = FieldOffset+1;
    }
  }

  // Zero length array.
  if (ResultElts.empty())
    return Constant::getNullValue(ConvertType(TREE_TYPE(exp)));
  assert(SomeVal && "If we had some initializer, we should have some value!");

  // Do a post-pass over all of the elements.  We're taking care of two things
  // here:
  //   #1. If any elements did not have initializers specified, provide them
  //       with a null init.
  //   #2. If any of the elements have different types, return a struct instead
  //       of an array.  This can occur in cases where we have an array of
  //       unions, and the various unions had different pieces init'd.
  const Type *ElTy = SomeVal->getType();
  Constant *Filler = Constant::getNullValue(ElTy);
  bool AllEltsSameType = true;
  for (unsigned i = 0, e = ResultElts.size(); i != e; ++i) {
    if (ResultElts[i] == 0)
      ResultElts[i] = Filler;
    else if (ResultElts[i]->getType() != ElTy)
      AllEltsSameType = false;
  }

  if (TREE_CODE(init_type) == VECTOR_TYPE) {
    assert(AllEltsSameType && "Vector of heterogeneous element types?");
    return ConstantVector::get(ResultElts);
  }

  Constant *Res = AllEltsSameType ?
    ConstantArray::get(ArrayType::get(ElTy, ResultElts.size()), ResultElts) :
    ConstantStruct::get(Context, ResultElts, false);

  // If the array does not require extra padding, return it.
  const Type *InitType = ConvertType(init_type);
  uint64_t ExpectedBits = getTargetData().getTypeAllocSizeInBits(InitType);
  uint64_t FoundBits = getTargetData().getTypeAllocSizeInBits(Res->getType());
  // The initializer may be bigger than the type if init_type is variable sized
  // or has no size (in which case the size is determined by the initial value).
  if (ExpectedBits <= FoundBits)
    return Res;

  // Wrap the array in a struct with padding at the end.
  Constant *PadElts[2];
  PadElts[0] = Res;
  PadElts[1] = UndefValue::get(ArrayType::get(Type::getInt8Ty(Context),
                                              (ExpectedBits - FoundBits) / 8));
  return ConstantStruct::get(Context, PadElts, 2, false);
}

/// FieldContents - A constant restricted to a range of bits.  Any part of the
/// constant outside of the range is discarded.  The range may be bigger than
/// the constant in which case any extra bits have an undefined value.
class FieldContents {
  SignedRange R; // The range of bits occupied by the constant.
  Constant *C;   // The constant.  May be null if the range is empty.
  int Starts;    // The first bit of the constant is positioned at this offset.

  FieldContents(SignedRange r, Constant *c, int starts)
    : R(r), C(c), Starts(starts) {
    assert((R.empty() || C) && "Need constant when range not empty!");
  }

  /// getAsBits - Return the bits in the range as an integer (or null if the
  /// range is empty).
  Constant *getAsBits() const {
    if (R.empty())
      return 0;
    const Type *IntTy = IntegerType::get(Context, R.getWidth());
    return InterpretAsType(C, IntTy, R.getFirst() - Starts);
  }

  /// isSafeToReturnContentsDirectly - Return whether the current value for the
  /// constant properly represents the bits in the range and so can be handed to
  /// the user as is.
  bool isSafeToReturnContentsDirectly(const TargetData &TD) const {
    // If there is no constant (allowed when the range is empty) then one needs
    // to be created.
    if (!C)
      return false;
    // If the first bit of the constant is not the first bit of the range then
    // it needs to be displaced before being passed to the user.
    if (!R.empty() && R.getFirst() != Starts)
      return false;
    // If the constant is wider than the range then it needs to be truncated
    // before being passed to the user.
    const Type *Ty = C->getType();
    unsigned AllocBits = TD.getTypeAllocSizeInBits(Ty);
    return AllocBits <= (unsigned)R.getWidth();
  }

public:
  /// FieldContents - Default constructor: empty bit range.
  FieldContents() : R(), C(0), Starts(0) {}

  /// get - Fill the range [first, last) with the given constant.
  static FieldContents get(int first, int last, Constant *c) {
    return FieldContents(SignedRange(first, last), c, first);
  }

  /// getRange - Return the range occupied by this field.
  SignedRange getRange() const { return R; }

  /// ChangeRangeTo - Change the range occupied by this field.
  void ChangeRangeTo(SignedRange r) { R = r; }

  /// JoinWith - Form the union of this field with another field (which must be
  /// disjoint from this one).  After this the range will be the convex hull of
  /// the ranges of the two fields.
  void JoinWith(const FieldContents &S);

  /// extractContents - Return the contained bits as a constant which contains
  /// every defined bit in the range, yet is guaranteed to have alloc size no
  /// larger than the width of the range.  Unlike the other methods for this
  /// class, this one requires that the width of the range be a multiple of an
  /// address unit, which usually means a multiple of 8.
  Constant *extractContents(const TargetData &TD) {
    /// If the current value for the constant can be used to represent the bits
    /// in the range then just return it.
    if (isSafeToReturnContentsDirectly(TD))
      return C;
    // If the range is empty then return a constant with zero size.
    if (R.empty()) {
      // Return an empty array.  Remember the returned value as an optimization
      // in case we are called again.
      C = Constant::getNullValue(GetUnitType(Context, 0));
      assert(isSafeToReturnContentsDirectly(TD) && "Unit over aligned?");
      return C;
    }
    assert(R.getWidth() % BITS_PER_UNIT == 0 && "Boundaries not aligned?");
    unsigned Units = R.getWidth() / BITS_PER_UNIT;
    // Turn the contents into a bunch of bits.  Remember the returned value as
    // an optimization in case we are called again.
    // TODO: If the contents only need to be truncated and have struct or array
    // type then we could try to do the truncation by dropping or modifying the
    // last elements of the constant, maybe yielding something less horrible.
    C = getAsBits();
    Starts = R.getFirst();
    if (isSafeToReturnContentsDirectly(TD))
      return C;
    // The integer type used to hold the bits was too big (for example an i24
    // typically occupies 32 bits so is too big for a range of 24 bits).  Turn
    // it into an array of bytes instead.
    C = InterpretAsType(C, GetUnitType(Context, Units), 0);
    assert(isSafeToReturnContentsDirectly(TD) && "Unit over aligned?");
    return C;
  }
};

/// JoinWith - Form the union of this field with another field (which must be
/// disjoint from this one).  After this the range will be the convex hull of
/// the ranges of the two fields.
void FieldContents::JoinWith(const FieldContents &S) {
  // Consider the contents of the fields to be bunches of bits and paste them
  // together.  This can result in a nasty integer constant expression, but as
  // we only get here for bitfields that's mostly harmless.
  BitSlice Bits(R, getAsBits());
  Bits.Merge (BitSlice(S.R, S.getAsBits()));
  R = Bits.getRange();
  C = Bits.getBits(R);
  Starts = R.empty() ? 0 : R.getFirst();
}

static Constant *ConvertRecordCONSTRUCTOR(tree exp) {
  // FIXME: This new logic, especially the handling of bitfields, is untested
  // and probably wrong on big-endian machines.
  IntervalList<FieldContents, int, 8> Layout;
  const TargetData &TD = getTargetData();
  uint64_t TypeSize = TD.getTypeAllocSizeInBits(ConvertType(TREE_TYPE(exp)));

  // Ensure that fields without an initial value are default initialized by
  // explicitly setting the starting value for all fields to be zero.  If an
  // initial value is supplied for a field then the value will overwrite and
  // replace the zero starting value later.
  if (flag_default_initialize_globals) {
    for (tree field = TYPE_FIELDS(TREE_TYPE(exp)); field;
         field = TREE_CHAIN(field)) {
      // Skip contained methods, types etc.
      if (TREE_CODE(field) != FIELD_DECL)
        continue;
      // If the field has variable or unknown position then it cannot be default
      // initialized - skip it.
      if (!OffsetIsLLVMCompatible(field))
        continue;
      uint64_t FirstBit = getFieldOffsetInBits(field);
      assert(FirstBit <= TypeSize && "Field off end of type!");
      // Determine the width of the field.
      uint64_t BitWidth;
      const Type *FieldTy = ConvertType(TREE_TYPE(field));
      if (isInt64(DECL_SIZE(field), true)) {
        // The field has a size and it is a constant, so use it.  Note that
        // this size may be smaller than the type size.  For example, if the
        // next field starts inside alignment padding at the end of this one
        // then DECL_SIZE will be the size with the padding used by the next
        // field not included.
        BitWidth = getInt64(DECL_SIZE(field), true);
      } else {
        // If the field has variable or unknown size then use the size of the
        // LLVM type instead as it gives the minimum size the field may have.
        if (!FieldTy->isSized())
          // An incomplete type - this field cannot be default initialized.
          continue;
        BitWidth = TD.getTypeAllocSizeInBits(FieldTy);
        if (FirstBit + BitWidth > TypeSize)
          BitWidth = TypeSize - FirstBit;
      }
      uint64_t LastBit = FirstBit + BitWidth;

      // Zero the bits occupied by the field.  It is safe to use FieldTy here as
      // it is guaranteed to cover all parts of the GCC type that can be default
      // initialized.  This makes for nicer IR than just using a bunch of bytes.
      Constant *Zero = Constant::getNullValue(FieldTy);
      Layout.AddInterval(FieldContents::get(FirstBit, LastBit, Zero));
    }
  }

  // For each field for which an initial value was specified, set the bits
  // occupied by the field to that value.
  unsigned HOST_WIDE_INT idx;
  tree field, next_field, value;
  next_field = TYPE_FIELDS(TREE_TYPE(exp));
  FOR_EACH_CONSTRUCTOR_ELT(CONSTRUCTOR_ELTS(exp), idx, field, value) {
    if (!field) {
      // Move on to the next FIELD_DECL, skipping contained methods, types etc.
      field = next_field;
      while (1) {
        assert(field && "Fell off end of record!");
        if (TREE_CODE(field) == FIELD_DECL) break;
        field = TREE_CHAIN(field);
      }
    }
    next_field = TREE_CHAIN(field);

    assert(TREE_CODE(field) == FIELD_DECL && "Initial value not for a field!");
    assert(OffsetIsLLVMCompatible(field) && "Field position not known!");
    // Turn the initial value for this field into an LLVM constant.
    Constant *Init = ConvertInitializerWithCast(value, TREE_TYPE(field));
    // Work out the range of bits occupied by the field.
    uint64_t FirstBit = getFieldOffsetInBits(field);
    assert(FirstBit <= TypeSize && "Field off end of type!");
    // If a size was specified for the field then use it.  Otherwise take the
    // size from the initial value.
    uint64_t BitWidth = isInt64(DECL_SIZE(field), true) ?
      getInt64(DECL_SIZE(field), true) :
      TD.getTypeAllocSizeInBits(Init->getType());
    uint64_t LastBit = FirstBit + BitWidth;

    // Set the bits occupied by the field to the initial value.
    Layout.AddInterval(FieldContents::get(FirstBit, LastBit, Init));
  }

  // Force all fields to begin and end on a byte boundary.  This automagically
  // takes care of bitfields.
  Layout.AlignBoundaries(BITS_PER_UNIT);

  // Determine whether to return a packed struct.  If returning an ordinary
  // struct would result in an initializer that is more aligned than its GCC
  // type then return a packed struct instead.  If a field's alignment would
  // make it start after its desired position then also use a packed struct.
  bool Pack = false;
  unsigned MaxAlign = TYPE_ALIGN(TREE_TYPE(exp));
  for (unsigned i = 0, e = Layout.getNumIntervals(); i != e; ++i) {
    FieldContents F = Layout.getInterval(i);
    unsigned First = F.getRange().getFirst();
    Constant *Val = F.extractContents(TD);
    unsigned Alignment = TD.getABITypeAlignment(Val->getType()) * 8;
    if (Alignment > MaxAlign || First % Alignment) {
      Pack = true;
      break;
    }
  }

  // Create the elements that will make up the struct.  As well as the fields
  // themselves there may also be padding elements.
  std::vector<Constant*> Elts;
  Elts.reserve(Layout.getNumIntervals());
  unsigned EndOfPrevious = 0; // Offset of first bit after previous element.
  for (unsigned i = 0, e = Layout.getNumIntervals(); i != e; ++i) {
    FieldContents F = Layout.getInterval(i);
    unsigned First = F.getRange().getFirst();
    Constant *Val = F.extractContents(TD);
    assert(EndOfPrevious <= First && "Previous field too big!");

    // If there is a gap then we may need to fill it with padding.
    if (First > EndOfPrevious) {
      // There is a gap between the end of the previous field and the start of
      // this one.  The alignment of the field contents may mean that it will
      // start at the right offset anyway, but if not then insert padding.
      bool NeedPadding = true;
      if (!Pack) {
        // If the field's alignment will take care of the gap then there is no
        // need for padding.
        unsigned Alignment = TD.getABITypeAlignment(Val->getType()) * 8;
        if (First == (EndOfPrevious + Alignment - 1) / Alignment * Alignment)
          NeedPadding = false;
      }
      if (NeedPadding) {
        // Fill the gap with undefined bytes.
        assert((First - EndOfPrevious) % BITS_PER_UNIT == 0 &&
               "Non-unit field boundaries!");
        unsigned Units = (First - EndOfPrevious) / BITS_PER_UNIT;
        Elts.push_back(UndefValue::get(GetUnitType(Context, Units)));
      }
    }

    // Append the field.
    Elts.push_back(Val);
    EndOfPrevious = First + TD.getTypeAllocSizeInBits(Val->getType());
  }

  // We guarantee that initializers are always at least as big as the LLVM type
  // for the initializer.  If needed, append padding to ensure this.
  if (EndOfPrevious < TypeSize) {
    assert((TypeSize - EndOfPrevious) % BITS_PER_UNIT == 0 &&
           "Non-unit type size?");
    unsigned Units = (TypeSize - EndOfPrevious) / BITS_PER_UNIT;
    Elts.push_back(UndefValue::get(GetUnitType(Context, Units)));
  }

  // Okay, we're done, return the computed elements.
  return ConstantStruct::get(Context, Elts, Pack);
}

static Constant *ConvertCONSTRUCTOR(tree exp) {
  // Please note, that we can have empty ctor, even if array is non-trivial (has
  // nonzero number of entries). This situation is typical for static ctors,
  // when array is filled during program initialization.
  if (CONSTRUCTOR_ELTS(exp) == 0 ||
      VEC_length(constructor_elt, CONSTRUCTOR_ELTS(exp)) == 0)  // All zeros?
    return Constant::getNullValue(ConvertType(TREE_TYPE(exp)));

  switch (TREE_CODE(TREE_TYPE(exp))) {
  default:
    debug_tree(exp);
    assert(0 && "Unknown ctor!");
  case VECTOR_TYPE:
  case ARRAY_TYPE:  return ConvertArrayCONSTRUCTOR(exp);
  case QUAL_UNION_TYPE:
  case RECORD_TYPE:
  case UNION_TYPE: return ConvertRecordCONSTRUCTOR(exp);
  }
}

static Constant *ConvertBinOp_CST(tree exp) {
  Constant *LHS = ConvertInitializer(TREE_OPERAND(exp, 0));
  bool LHSIsSigned = !TYPE_UNSIGNED(TREE_TYPE(TREE_OPERAND(exp,0)));
  Constant *RHS = ConvertInitializer(TREE_OPERAND(exp, 1));
  bool RHSIsSigned = !TYPE_UNSIGNED(TREE_TYPE(TREE_OPERAND(exp,1)));
  Instruction::CastOps opcode;
  if (LHS->getType()->isPointerTy()) {
    const Type *IntPtrTy = getTargetData().getIntPtrType(Context);
    opcode = CastInst::getCastOpcode(LHS, LHSIsSigned, IntPtrTy, false);
    LHS = TheFolder->CreateCast(opcode, LHS, IntPtrTy);
    opcode = CastInst::getCastOpcode(RHS, RHSIsSigned, IntPtrTy, false);
    RHS = TheFolder->CreateCast(opcode, RHS, IntPtrTy);
  }

  Constant *Result;
  switch (TREE_CODE(exp)) {
  default: assert(0 && "Unexpected case!");
  case PLUS_EXPR:   Result = TheFolder->CreateAdd(LHS, RHS); break;
  case MINUS_EXPR:  Result = TheFolder->CreateSub(LHS, RHS); break;
  }

  const Type *Ty = ConvertType(TREE_TYPE(exp));
  bool TyIsSigned = !TYPE_UNSIGNED(TREE_TYPE(exp));
  opcode = CastInst::getCastOpcode(Result, LHSIsSigned, Ty, TyIsSigned);
  return TheFolder->CreateCast(opcode, Result, Ty);
}

static Constant *ConvertPOINTER_PLUS_EXPR(tree exp) {
  Constant *Ptr = ConvertInitializer(TREE_OPERAND(exp, 0)); // The pointer.
  Constant *Idx = ConvertInitializer(TREE_OPERAND(exp, 1)); // Offset in units.

  // Convert the pointer into an i8* and add the offset to it.
  Ptr = TheFolder->CreateBitCast(Ptr, GetUnitPointerType(Context));
  Constant *GEP = POINTER_TYPE_OVERFLOW_UNDEFINED ?
    TheFolder->CreateInBoundsGetElementPtr(Ptr, &Idx, 1) :
    TheFolder->CreateGetElementPtr(Ptr, &Idx, 1);

  // The result may be of a different pointer type.
  return TheFolder->CreateBitCast(GEP, ConvertType(TREE_TYPE(exp)));
}

static Constant *ConvertVIEW_CONVERT_EXPR(tree exp) {
  // Does not change the bits, only the type they are considered to be.
  return ConvertInitializer(TREE_OPERAND(exp, 0));
}

/// ConvertInitializer - Convert the initial value for a global variable to an
/// equivalent LLVM constant.  Also handles constant constructors.  The type of
/// the returned value may be pretty much anything.  All that is guaranteed is
/// that its alloc size is equal to the size of the initial value and that its
/// alignment is less than or equal to the initial value's GCC type alignment.
/// Note that the GCC type may have variable size or no size, in which case the
/// size is determined by the initial value.  When this happens the size of the
/// initial value may exceed the alloc size of the LLVM memory type generated
/// for the GCC type (see ConvertType); it is never smaller than the alloc size.
Constant *ConvertInitializer(tree exp) {
  Constant *Init;
  switch (TREE_CODE(exp)) {
  default:
    DieAbjectly("Unknown constant to convert!", exp);
  case COMPLEX_CST:
  case INTEGER_CST:
  case REAL_CST:
  case VECTOR_CST:
    // Make the IR easier to read by converting the bunch of bytes returned by
    // ConvertCST into a less surprising type.
    Init = InterpretAsType(ConvertCST(exp), ConvertType(TREE_TYPE(exp)), 0);
    break;
  case STRING_CST:
    Init = ConvertSTRING_CST(exp);
    break;
  case ADDR_EXPR:
    Init = ConvertADDR_EXPR(exp);
    break;
  case CONSTRUCTOR:
    Init = ConvertCONSTRUCTOR(exp);
    break;
  case CONVERT_EXPR:
  case NOP_EXPR:
    Init = ConvertInitializerWithCast(TREE_OPERAND(exp, 0), TREE_TYPE(exp));
    break;
  case MINUS_EXPR:
  case PLUS_EXPR:
    Init = ConvertBinOp_CST(exp);
    break;
  case POINTER_PLUS_EXPR:
    Init = ConvertPOINTER_PLUS_EXPR(exp);
    break;
  case VIEW_CONVERT_EXPR:
    Init = ConvertVIEW_CONVERT_EXPR(exp);
    break;
  }

#ifndef NDEBUG
  // Check that the guarantees we make about the returned value actually hold.
  // The initializer should always be at least as big as the constructor's type,
  // and except in the cases of incomplete types or types with variable size the
  // sizes should be the same.
  const Type *Ty = ConvertType(TREE_TYPE(exp));
  if (Ty->isSized()) {
    uint64_t InitSize = getTargetData().getTypeAllocSizeInBits(Init->getType());
    uint64_t TypeSize = getTargetData().getTypeAllocSizeInBits(Ty);
    if (InitSize < TypeSize)
      DieAbjectly("Constant too small for type!", exp);
    if (isInt64(TREE_TYPE(exp), true) && InitSize != TypeSize)
      DieAbjectly("Constant too big for type!", exp);
  }
// FIXME: This check fails when building libdecnumber (self-host build).
//  if (getTargetData().getABITypeAlignment(Init->getType()) * 8 >
//      TYPE_ALIGN(TREE_TYPE(exp)))
//    DieAbjectly("Constant over aligned!", exp);
#endif

  return Init;
}


//===----------------------------------------------------------------------===//
//                            ... AddressOf ...
//===----------------------------------------------------------------------===//

/// getAsInteger - Given a constant of integer type, return its value as an LLVM
/// integer constant.
static Constant *getAsInteger(tree exp) {
  tree type = TREE_TYPE(exp);
  assert(INTEGRAL_TYPE_P(type) && "Constant does not have integer type!");
  Constant *C = ConvertInitializer(exp);
  const Type *IntTy = IntegerType::get(Context, TYPE_PRECISION(type));
  return InterpretAsType(C, IntTy, 0);
}

/// AddressOfCST - Return the address of a simple constant, eg a of number.
static Constant *AddressOfCST(tree exp) {
  Constant *Init = ConvertInitializer(exp);

  // Cache the constants to avoid making obvious duplicates that have to be
  // folded by the optimizer.
  static DenseMap<Constant*, GlobalVariable*> CSTCache;
  GlobalVariable *&Slot = CSTCache[Init];
  if (Slot)
    return Slot;

  // Create a new global variable.
  Slot = new GlobalVariable(*TheModule, Init->getType(), true,
                            GlobalVariable::PrivateLinkage, Init, ".cst");
  unsigned align = TYPE_ALIGN (TREE_TYPE (exp));
#ifdef CONSTANT_ALIGNMENT
  align = CONSTANT_ALIGNMENT (exp, align);
#endif
  Slot->setAlignment(align);

  return Slot;
}

/// AddressOfARRAY_REF - Return the address of an array element or slice.
static Constant *AddressOfARRAY_REF(tree exp) {
  tree array = TREE_OPERAND(exp, 0);
  tree index = TREE_OPERAND(exp, 1);
  tree index_type = TREE_TYPE(index);
  assert(TREE_CODE(TREE_TYPE(array)) == ARRAY_TYPE && "Unknown ARRAY_REF!");

  // Check for variable sized reference.
  assert(isSequentialCompatible(TREE_TYPE(array)) &&
         "Global with variable size?");

  // Get the index into the array as an LLVM integer constant.
  Constant *IndexVal = getAsInteger(index);

  // Subtract off the lower bound, if any.
  tree lower_bound = array_ref_low_bound(exp);
  if (!integer_zerop(lower_bound)) {
    // Get the lower bound as an LLVM integer constant.
    Constant *LowerBoundVal = getAsInteger(lower_bound);
    IndexVal = TheFolder->CreateSub(IndexVal, LowerBoundVal, hasNUW(index_type),
                                    hasNSW(index_type));
  }

  // Avoid any assumptions about how the array type is represented in LLVM by
  // doing the GEP on a pointer to the first array element.
  Constant *ArrayAddr = AddressOf(array);
  const Type *EltTy = ConvertType(TREE_TYPE(TREE_TYPE(array)));
  ArrayAddr = TheFolder->CreateBitCast(ArrayAddr, EltTy->getPointerTo());

  return POINTER_TYPE_OVERFLOW_UNDEFINED ?
    TheFolder->CreateInBoundsGetElementPtr(ArrayAddr, &IndexVal, 1) :
    TheFolder->CreateGetElementPtr(ArrayAddr, &IndexVal, 1);
}

/// AddressOfCOMPONENT_REF - Return the address of a field in a record.
static Constant *AddressOfCOMPONENT_REF(tree exp) {
  tree field_decl = TREE_OPERAND(exp, 1);

  // Compute the field offset in units from the start of the record.
  Constant *Offset;
  if (TREE_OPERAND(exp, 2)) {
    Offset = getAsInteger(TREE_OPERAND(exp, 2));
    // At this point the offset is measured in units divided by (exactly)
    // (DECL_OFFSET_ALIGN / BITS_PER_UNIT).  Convert to units.
    unsigned factor = DECL_OFFSET_ALIGN(field_decl) / BITS_PER_UNIT;
    if (factor != 1)
      Offset = TheFolder->CreateMul(Offset,
                                    ConstantInt::get(Offset->getType(),
                                                     factor));
  } else {
    assert(DECL_FIELD_OFFSET(field_decl) && "Field offset not available!");
    Offset = getAsInteger(DECL_FIELD_OFFSET(field_decl));
  }

  // Here BitStart gives the offset of the field in bits from Offset.
  uint64_t BitStart = getInt64(DECL_FIELD_BIT_OFFSET(field_decl), true);
  // Incorporate as much of it as possible into the pointer computation.
  uint64_t Units = BitStart / BITS_PER_UNIT;
  if (Units > 0) {
    Offset = TheFolder->CreateAdd(Offset,
                                  ConstantInt::get(Offset->getType(),
                                                   Units));
    BitStart -= Units * BITS_PER_UNIT;
  }
  assert(BitStart == 0 &&
         "It's a bitfield reference or we didn't get to the field!");

  const Type *UnitPtrTy = GetUnitPointerType(Context);
  Constant *StructAddr = AddressOf(TREE_OPERAND(exp, 0));
  Constant *FieldPtr = TheFolder->CreateBitCast(StructAddr, UnitPtrTy);
  FieldPtr = TheFolder->CreateInBoundsGetElementPtr(FieldPtr, &Offset, 1);

  return FieldPtr;
}

/// AddressOfDecl - Return the address of a global.
static Constant *AddressOfDecl(tree exp) {
  return cast<GlobalValue>(DEFINITION_LLVM(exp));
}

/// AddressOfINDIRECT_REF - Return the address of a dereference.
static Constant *AddressOfINDIRECT_REF(tree exp) {
  // The address is just the dereferenced operand.  Get it as an LLVM constant.
  Constant *C = ConvertInitializer(TREE_OPERAND(exp, 0));
  // Make no assumptions about the type of the constant.
  return InterpretAsType(C, ConvertType(TREE_TYPE(TREE_OPERAND(exp, 0))), 0);
}

/// AddressOfLABEL_DECL - Return the address of a label.
static Constant *AddressOfLABEL_DECL(tree exp) {
  extern TreeToLLVM *TheTreeToLLVM;

  assert(TheTreeToLLVM &&
         "taking the address of a label while not compiling the function!");

  // Figure out which function this is for, verify it's the one we're compiling.
  if (DECL_CONTEXT(exp)) {
    assert(TREE_CODE(DECL_CONTEXT(exp)) == FUNCTION_DECL &&
           "Address of label in nested function?");
    assert(TheTreeToLLVM->getFUNCTION_DECL() == DECL_CONTEXT(exp) &&
           "Taking the address of a label that isn't in the current fn!?");
  }

  return TheTreeToLLVM->AddressOfLABEL_DECL(exp);
}

/// AddressOf - Given an expression with a constant address such as a constant,
/// a global variable or a label, returns the address.  The type of the returned
/// is always a pointer type and, as long as 'exp' does not have void type, the
/// type of the pointee is the memory type that corresponds to the type of exp
/// (see ConvertType).
Constant *AddressOf(tree exp) {
  Constant *Addr;

  switch (TREE_CODE(exp)) {
  default:
    DieAbjectly("Unknown constant to take the address of!", exp);
  case COMPLEX_CST:
  case FIXED_CST:
  case INTEGER_CST:
  case REAL_CST:
  case STRING_CST:
  case VECTOR_CST:
    Addr = AddressOfCST(exp);
    break;
  case ARRAY_RANGE_REF:
  case ARRAY_REF:
    Addr = AddressOfARRAY_REF(exp);
    break;
  case COMPONENT_REF:
    Addr = AddressOfCOMPONENT_REF(exp);
    break;
  case COMPOUND_LITERAL_EXPR: // FIXME: not gimple - defined by C front-end
    Addr = AddressOf(DECL_EXPR_DECL (TREE_OPERAND (exp, 0)));
    break;
  case CONST_DECL:
  case FUNCTION_DECL:
  case VAR_DECL:
    Addr = AddressOfDecl(exp);
    break;
  case INDIRECT_REF:
    Addr = AddressOfINDIRECT_REF(exp);
    break;
  case LABEL_DECL:
    Addr = AddressOfLABEL_DECL(exp);
    break;
  }

  // Ensure that the address has the expected type.  It is simpler to do this
  // once here rather than in every AddressOf helper.
  const Type *Ty;
  if (VOID_TYPE_P(TREE_TYPE(exp)))
    Ty = GetUnitPointerType(Context); // void* -> i8*.
  else
    Ty = ConvertType(TREE_TYPE(exp))->getPointerTo();

  return TheFolder->CreateBitCast(Addr, Ty);
}
