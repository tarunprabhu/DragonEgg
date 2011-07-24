//===----------- Types.cpp - Converting GCC types to LLVM types -----------===//
//
// Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010, 2011  Chris Lattner,
// Duncan Sands et al.
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
// This is the code that converts GCC tree types into LLVM types.
//===----------------------------------------------------------------------===//

// Plugin headers
#include "dragonegg/ABI.h"
#include "dragonegg/Trees.h"
extern "C" {
#include "dragonegg/cache.h"
}

// LLVM headers
#include "llvm/Module.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

// System headers
#include <gmp.h>
#include <map>

// GCC headers
extern "C" {
#include "config.h"
// Stop GCC declaring 'getopt' as it can clash with the system's declaration.
#undef HAVE_DECL_GETOPT
#include "system.h"
#include "coretypes.h"
#include "target.h"
#include "tree.h"
}

static LLVMContext &Context = getGlobalContext();

//===----------------------------------------------------------------------===//
//                   Matching LLVM types with GCC trees
//===----------------------------------------------------------------------===//

// GET_TYPE_LLVM/SET_TYPE_LLVM - Associate an LLVM type with each TREE type.
// These are lazily computed by ConvertType.

Type *llvm_set_type(tree Tr, Type *Ty) {
  assert(TYPE_P(Tr) && "Expected a gcc type!");

  // Check that the LLVM and GCC types have the same size, or, if the type has
  // variable size, that the LLVM type is not bigger than any possible value of
  // the GCC type.
#ifndef NDEBUG
  if (Ty->isSized() && isInt64(TYPE_SIZE(Tr), true)) {
    uint64_t LLVMSize = getTargetData().getTypeAllocSizeInBits(Ty);
    if (getInt64(TYPE_SIZE(Tr), true) != LLVMSize) {
      errs() << "GCC: ";
      debug_tree(Tr);
      errs() << "LLVM: ";
      Ty->print(errs());
      errs() << " (" << LLVMSize << " bits)\n";
      DieAbjectly("LLVM type size doesn't match GCC type size!");
    }
  }
#endif

  return (Type *)llvm_set_cached(Tr, Ty);
}

#define SET_TYPE_LLVM(NODE, TYPE) llvm_set_type(NODE, TYPE)

Type *llvm_get_type(tree Tr) {
  assert(TYPE_P(Tr) && "Expected a gcc type!");
  return (Type *)llvm_get_cached(Tr);
}

#define GET_TYPE_LLVM(NODE) llvm_get_type(NODE)


//===----------------------------------------------------------------------===//
//                   Recursive Type Handling Code and Data
//===----------------------------------------------------------------------===//

// Recursive types are a major pain to handle for a couple of reasons.  Because
// of this, when we start parsing a struct or a union, we globally change how
// POINTER_TYPE and REFERENCE_TYPE are handled.  In particular, instead of
// actually recursing and computing the type they point to, they will return an
// opaque*, and remember that they did this in PointersToReresolve.


/// GetFunctionType - This is just a helper like FunctionType::get but that
/// takes PATypeHolders.
static FunctionType *GetFunctionType(const PATypeHolder &Res,
                                     std::vector<PATypeHolder> &ArgTys,
                                     bool isVarArg) {
  std::vector<Type*> ArgTysP;
  ArgTysP.reserve(ArgTys.size());
  for (unsigned i = 0, e = ArgTys.size(); i != e; ++i)
    ArgTysP.push_back(ArgTys[i]);

  return FunctionType::get(Res, ArgTysP, isVarArg);
}

//===----------------------------------------------------------------------===//
//                       Type Conversion Utilities
//===----------------------------------------------------------------------===//

/// ArrayLengthOf - Returns the length of the given gcc array type, or NO_LENGTH
/// if the array has variable or unknown length.
uint64_t ArrayLengthOf(tree type) {
  assert(TREE_CODE(type) == ARRAY_TYPE && "Only for array types!");
  tree range = array_type_nelts(type); // The number of elements minus one.
  // Bail out if the array has variable or unknown length.
  if (!isInt64(range, false))
    return NO_LENGTH;
  int64_t Range = getInt64(range, false);
  return Range < 0 ? 0 : 1 + (uint64_t)Range;
}

/// getFieldOffsetInBits - Return the bit offset of a FIELD_DECL in a structure.
uint64_t getFieldOffsetInBits(tree field) {
  assert(OffsetIsLLVMCompatible(field) && "Offset is not constant!");
  uint64_t Result = getInt64(DECL_FIELD_BIT_OFFSET(field), true);
  Result += getInt64(DECL_FIELD_OFFSET(field), true) * BITS_PER_UNIT;
  return Result;
}

/// GetUnitType - Returns an integer one address unit wide if 'NumUnits' is 1;
/// otherwise returns an array of such integers with 'NumUnits' elements.  For
/// example, on a machine which has 16 bit bytes returns an i16 or an array of
/// i16.
Type *GetUnitType(LLVMContext &C, unsigned NumUnits) {
  assert(!(BITS_PER_UNIT & 7) && "Unit size not a multiple of 8 bits!");
  Type *UnitTy = IntegerType::get(C, BITS_PER_UNIT);
  if (NumUnits == 1)
    return UnitTy;
  return ArrayType::get(UnitTy, NumUnits);
}

/// GetUnitPointerType - Returns an LLVM pointer type which points to memory one
/// address unit wide.  For example, on a machine which has 16 bit bytes returns
/// an i16*.
Type *GetUnitPointerType(LLVMContext &C, unsigned AddrSpace) {
  return GetUnitType(C)->getPointerTo(AddrSpace);
}

// isPassedByInvisibleReference - Return true if an argument of the specified
// type should be passed in by invisible reference.
//
bool isPassedByInvisibleReference(tree Type) {
  // Don't crash in this case.
  if (Type == error_mark_node)
    return false;

  // FIXME: Search for TREE_ADDRESSABLE in calls.c, and see if there are other
  // cases that make arguments automatically passed in by reference.
  return TREE_ADDRESSABLE(Type) || TYPE_SIZE(Type) == 0 ||
         TREE_CODE(TYPE_SIZE(Type)) != INTEGER_CST;
}

/// isSequentialCompatible - Return true if the specified gcc array, pointer or
/// vector type and the corresponding LLVM SequentialType lay out their elements
/// identically in memory, so doing a GEP accesses the right memory location.
/// We assume that objects without a known size do not.
bool isSequentialCompatible(tree type) {
  assert((TREE_CODE(type) == ARRAY_TYPE ||
          TREE_CODE(type) == POINTER_TYPE ||
          TREE_CODE(type) == VECTOR_TYPE ||
          TREE_CODE(type) == REFERENCE_TYPE) && "not a sequential type!");
  // This relies on gcc types with constant size mapping to LLVM types with the
  // same size.  It is possible for the component type not to have a size:
  // struct foo;  extern foo bar[];
  return isInt64(TYPE_SIZE(TREE_TYPE(type)), true);
}

/// OffsetIsLLVMCompatible - Return true if the given field is offset from the
/// start of the record by a constant amount which is not humongously big.
bool OffsetIsLLVMCompatible(tree field_decl) {
  return isInt64(DECL_FIELD_OFFSET(field_decl), true);
}

/// isBitfield - Returns whether to treat the specified field as a bitfield.
bool isBitfield(tree_node *field_decl) {
  if (!DECL_BIT_FIELD(field_decl))
    return false;

  // A bitfield.  But do we need to treat it as one?

  assert(DECL_FIELD_BIT_OFFSET(field_decl) && "Bitfield with no bit offset!");
  if (TREE_INT_CST_LOW(DECL_FIELD_BIT_OFFSET(field_decl)) & 7)
    // Does not start on a byte boundary - must treat as a bitfield.
    return true;

  if (!isInt64(TYPE_SIZE (TREE_TYPE(field_decl)), true))
    // No size or variable sized - play safe, treat as a bitfield.
    return true;

  uint64_t TypeSizeInBits = getInt64(TYPE_SIZE (TREE_TYPE(field_decl)), true);
  assert(!(TypeSizeInBits & 7) && "A type with a non-byte size!");

  assert(DECL_SIZE(field_decl) && "Bitfield with no bit size!");
  uint64_t FieldSizeInBits = getInt64(DECL_SIZE(field_decl), true);
  if (FieldSizeInBits < TypeSizeInBits)
    // Not wide enough to hold the entire type - treat as a bitfield.
    return true;

  return false;
}


//===----------------------------------------------------------------------===//
//                     Abstract Type Refinement Helpers
//===----------------------------------------------------------------------===//
//
// This code is built to make sure that the TYPE_LLVM field on tree types are
// updated when LLVM types are refined.  This prevents dangling pointers from
// occurring due to type coallescing.
//
namespace {
  class TypeRefinementDatabase : public AbstractTypeUser {
    virtual void refineAbstractType(const DerivedType *OldTy,
                                    Type *NewTy);
    virtual void typeBecameConcrete(const DerivedType *AbsTy);

    // TypeUsers - For each abstract LLVM type, we keep track of all of the GCC
    // types that point to it.
    std::map<Type*, std::vector<tree> > TypeUsers;
  public:
    /// setType - call SET_TYPE_LLVM(type, Ty), associating the type with the
    /// specified tree type.  In addition, if the LLVM type is an abstract type,
    /// we add it to our data structure to track it.
    inline Type *setType(tree type, Type *Ty) {
      if (GET_TYPE_LLVM(type))
        RemoveTypeFromTable(type);

      if (Ty->isAbstract()) {
        std::vector<tree> &Users = TypeUsers[Ty];
        if (Users.empty()) Ty->addAbstractTypeUser(this);
        Users.push_back(type);
      }
      return SET_TYPE_LLVM(type, Ty);
    }

    void RemoveTypeFromTable(tree type);
    void dump() const;
  };

  /// TypeDB - The main global type database.
  TypeRefinementDatabase TypeDB;
}

/// RemoveTypeFromTable - We're about to change the LLVM type of 'type'
///
void TypeRefinementDatabase::RemoveTypeFromTable(tree type) {
  Type *Ty = GET_TYPE_LLVM(type);
  if (!Ty->isAbstract()) return;
  std::map<Type*, std::vector<tree> >::iterator I = TypeUsers.find(Ty);
  assert(I != TypeUsers.end() && "Using an abstract type but not in table?");

  bool FoundIt = false;
  for (unsigned i = 0, e = I->second.size(); i != e; ++i)
    if (I->second[i] == type) {
      FoundIt = true;
      std::swap(I->second[i], I->second.back());
      I->second.pop_back();
      break;
    }
  assert(FoundIt && "Using an abstract type but not in table?");

  // If the type plane is now empty, nuke it.
  if (I->second.empty()) {
    TypeUsers.erase(I);
    Ty->removeAbstractTypeUser(this);
  }
}

/// refineAbstractType - The callback method invoked when an abstract type is
/// resolved to another type.  An object must override this method to update
/// its internal state to reference NewType instead of OldType.
///
void TypeRefinementDatabase::refineAbstractType(const DerivedType *OldTy,
                                                Type *NewTy) {
  if (OldTy == NewTy && OldTy->isAbstract()) return; // Nothing to do.

  std::map<Type*, std::vector<tree> >::iterator I = TypeUsers.find(OldTy);
  assert(I != TypeUsers.end() && "Using an abstract type but not in table?");

  if (!NewTy->isAbstract()) {
    // If the type became concrete, update everything pointing to it, and remove
    // all of our entries from the map.
    if (OldTy != NewTy)
      for (unsigned i = 0, e = I->second.size(); i != e; ++i)
        SET_TYPE_LLVM(I->second[i], NewTy);
  } else {
    // Otherwise, it was refined to another instance of an abstract type.  Move
    // everything over and stop monitoring OldTy.
    std::vector<tree> &NewSlot = TypeUsers[NewTy];
    if (NewSlot.empty()) NewTy->addAbstractTypeUser(this);

    for (unsigned i = 0, e = I->second.size(); i != e; ++i) {
      NewSlot.push_back(I->second[i]);
      SET_TYPE_LLVM(I->second[i], NewTy);
    }
  }

  TypeUsers.erase(I);

  // Next, remove OldTy's entry in the TargetData object if it has one.
  if (StructType *STy = dyn_cast<StructType>(OldTy))
    getTargetData().InvalidateStructLayoutInfo(STy);

  OldTy->removeAbstractTypeUser(this);
}

/// The other case which AbstractTypeUsers must be aware of is when a type
/// makes the transition from being abstract (where it has clients on it's
/// AbstractTypeUsers list) to concrete (where it does not).  This method
/// notifies ATU's when this occurs for a type.
///
void TypeRefinementDatabase::typeBecameConcrete(const DerivedType *AbsTy) {
  assert(TypeUsers.count(AbsTy) && "Not using this type!");
  // Remove the type from our collection of tracked types.
  TypeUsers.erase(AbsTy);
  AbsTy->removeAbstractTypeUser(this);
}
void TypeRefinementDatabase::dump() const {
  outs() << "TypeRefinementDatabase\n";
  outs().flush();
}

//===----------------------------------------------------------------------===//
//                              Helper Routines
//===----------------------------------------------------------------------===//

/// GetFieldIndex - Return the index of the field in the given LLVM type that
/// corresponds to the GCC field declaration 'decl'.  This means that the LLVM
/// and GCC fields start in the same byte (if 'decl' is a bitfield, this means
/// that its first bit is within the byte the LLVM field starts at).  Returns
/// INT_MAX if there is no such LLVM field.
int GetFieldIndex(tree decl, Type *Ty) {
  assert(TREE_CODE(decl) == FIELD_DECL && "Expected a FIELD_DECL!");
  assert(Ty == ConvertType(DECL_CONTEXT(decl)) && "Field not for this type!");

  // If we previously cached the field index, return the cached value.
  unsigned Index = (unsigned)get_decl_index(decl);
  if (Index <= INT_MAX)
    return Index;

  // TODO: At this point we could process all fields of DECL_CONTEXT(decl), and
  // incrementally advance over the StructLayout.  This would make indexing be
  // O(N) rather than O(N log N) if all N fields are used.  It's not clear if it
  // would really be a win though.

  StructType *STy = dyn_cast<StructType>(Ty);
  // If this is not a struct type, then for sure there is no corresponding LLVM
  // field (we do not require GCC record types to be converted to LLVM structs).
  if (!STy)
    return set_decl_index(decl, INT_MAX);

  // If the field declaration is at a variable or humongous offset then there
  // can be no corresponding LLVM field.
  if (!OffsetIsLLVMCompatible(decl))
    return set_decl_index(decl, INT_MAX);

  // Find the LLVM field that contains the first bit of the GCC field.
  uint64_t OffsetInBytes = getFieldOffsetInBits(decl) / 8; // Ignore bit in byte
  const StructLayout *SL = getTargetData().getStructLayout(STy);
  Index = SL->getElementContainingOffset(OffsetInBytes);

  // The GCC field must start in the first byte of the LLVM field.
  if (OffsetInBytes != SL->getElementOffset(Index))
    return set_decl_index(decl, INT_MAX);

  // We are not able to cache values bigger than INT_MAX, so bail out if the
  // LLVM field index is that huge.
  if (Index >= INT_MAX)
    return set_decl_index(decl, INT_MAX);

  // Found an appropriate LLVM field - return it.
  return set_decl_index(decl, Index);
}


//===----------------------------------------------------------------------===//
//                      Main Type Conversion Routines
//===----------------------------------------------------------------------===//

/// getRegType - Returns the LLVM type to use for registers that hold a value
/// of the scalar GCC type 'type'.  All of the EmitReg* routines use this to
/// determine the LLVM type to return.
Type *getRegType(tree type) {
  // NOTE: Any changes made here need to be reflected in LoadRegisterFromMemory,
  // StoreRegisterToMemory and ExtractRegisterFromConstant.
  assert(!AGGREGATE_TYPE_P(type) && "Registers must have a scalar type!");
  assert(TREE_CODE(type) != VOID_TYPE && "Registers cannot have void type!");

  switch (TREE_CODE(type)) {

  default:
    DieAbjectly("Unknown register type!", type);

  case BOOLEAN_TYPE:
  case ENUMERAL_TYPE:
  case INTEGER_TYPE:
    // For integral types, convert based on the type precision.  For example,
    // this turns bool into i1 while ConvertType probably turns it into i8 or
    // i32.
    return IntegerType::get(Context, TYPE_PRECISION(type));

  case COMPLEX_TYPE: {
    Type *EltTy = getRegType(TREE_TYPE(type));
    return StructType::get(EltTy, EltTy, NULL);
  }

  case OFFSET_TYPE:
    return getTargetData().getIntPtrType(Context);

  case POINTER_TYPE:
  case REFERENCE_TYPE:
    // void* -> byte*
    return VOID_TYPE_P(TREE_TYPE(type)) ?  GetUnitPointerType(Context) :
      ConvertType(TREE_TYPE(type))->getPointerTo();

  case REAL_TYPE:
    if (TYPE_PRECISION(type) == 32)
      return Type::getFloatTy(Context);
    if (TYPE_PRECISION(type) == 64)
      return Type::getDoubleTy(Context);
    if (TYPE_PRECISION(type) == 80)
      return Type::getX86_FP80Ty(Context);
    if (TYPE_PRECISION(type) == 128)
#ifdef TARGET_POWERPC
      return Type::getPPC_FP128Ty(Context);
#else
      // IEEE quad precision.
      return Type::getFP128Ty(Context);
#endif
      DieAbjectly("Unknown FP type!", type);

  case VECTOR_TYPE: {
    // LLVM does not support vectors of pointers, so turn any pointers into
    // integers.
    Type *EltTy = POINTER_TYPE_P(TREE_TYPE(type)) ?
      getTargetData().getIntPtrType(Context) : getRegType(TREE_TYPE(type));
    return VectorType::get(EltTy, TYPE_VECTOR_SUBPARTS(type));
  }

  }
}

/// getPointerToType - Returns the LLVM register type to use for a pointer to
/// the given GCC type.
Type *getPointerToType(tree type) {
  if (VOID_TYPE_P(type))
    // void* -> byte*
    return GetUnitPointerType(Context);
  // FIXME: Handle address spaces.
  return ConvertType(type)->getPointerTo();
}

Type *TypeConverter::ConvertType(tree type) {
  if (type == error_mark_node) return Type::getInt32Ty(Context);

  // LLVM doesn't care about variants such as const, volatile, or restrict.
  type = TYPE_MAIN_VARIANT(type);
  Type *Ty;

  switch (TREE_CODE(type)) {
  default:
    DieAbjectly("Unknown type to convert!", type);

  case VOID_TYPE:
    Ty = SET_TYPE_LLVM(type, Type::getVoidTy(Context));
    break;

  case RECORD_TYPE:
  case QUAL_UNION_TYPE:
  case UNION_TYPE:
    Ty = ConvertRECORD(type);
    break;

  case ENUMERAL_TYPE:
    // Use of an enum that is implicitly declared?
    if (TYPE_SIZE(type) == 0) {
      // If we already compiled this type, use the old type.
      if ((Ty = GET_TYPE_LLVM(type)))
        return Ty;

      Ty = OpaqueType::get(Context);
      Ty = TypeDB.setType(type, Ty);
      break;
    }
    // FALL THROUGH.
  case BOOLEAN_TYPE:
  case INTEGER_TYPE: {
    if ((Ty = GET_TYPE_LLVM(type))) return Ty;
    uint64_t Size = getInt64(TYPE_SIZE(type), true);
    Ty = SET_TYPE_LLVM(type, IntegerType::get(Context, Size));
    break;
  }

  case REAL_TYPE:
    if ((Ty = GET_TYPE_LLVM(type))) return Ty;
    switch (TYPE_PRECISION(type)) {
    default:
      DieAbjectly("Unknown FP type!", type);
    case 32: Ty = SET_TYPE_LLVM(type, Type::getFloatTy(Context)); break;
    case 64: Ty = SET_TYPE_LLVM(type, Type::getDoubleTy(Context)); break;
    case 80: Ty = SET_TYPE_LLVM(type, Type::getX86_FP80Ty(Context)); break;
    case 128:
#ifdef TARGET_POWERPC
      Ty = SET_TYPE_LLVM(type, Type::getPPC_FP128Ty(Context));
#else
      // IEEE quad precision.
      Ty = SET_TYPE_LLVM(type, Type::getFP128Ty(Context));
#endif
      break;
    }
    break;

  case COMPLEX_TYPE: {
    if ((Ty = GET_TYPE_LLVM(type))) return Ty;
    Ty = ConvertType(TREE_TYPE(type));
    assert(!Ty->isAbstract() && "should use TypeDB.setType()");
    Ty = StructType::get(Ty, Ty, NULL);
    Ty = SET_TYPE_LLVM(type, Ty);
    break;
  }

  case VECTOR_TYPE: {
    if ((Ty = GET_TYPE_LLVM(type))) return Ty;
    // LLVM does not support vectors of pointers, so turn any pointers into
    // integers.
    Ty = POINTER_TYPE_P(TREE_TYPE(type)) ?
      getTargetData().getIntPtrType(Context) : ConvertType(TREE_TYPE(type));
    assert(!Ty->isAbstract() && "should use TypeDB.setType()");
    Ty = VectorType::get(Ty, TYPE_VECTOR_SUBPARTS(type));
    Ty = SET_TYPE_LLVM(type, Ty);
    break;
  }

  case POINTER_TYPE:
  case REFERENCE_TYPE:
    if (PointerType *PTy = cast_or_null<PointerType>(GET_TYPE_LLVM(type))){
      // We already converted this type.  If this isn't a case where we have to
      // reparse it, just return it.
      if (PointersToReresolve.empty() || PointersToReresolve.back() != type ||
          ConvertingStruct)
        return PTy;

      // Okay, we know that we're !ConvertingStruct and that type is on the end
      // of the vector.  Remove this entry from the PointersToReresolve list and
      // get the pointee type.  Note that this order is important in case the
      // pointee type uses this pointer.
      assert(PTy->getElementType()->isOpaqueTy() && "Not a deferred ref!");

      // We are actively resolving this pointer.  We want to pop this value from
      // the stack, as we are no longer resolving it.  However, we don't want to
      // make it look like we are now resolving the previous pointer on the
      // stack, so pop this value and push a null.
      PointersToReresolve.back() = 0;


      // Do not do any nested resolution.  We know that there is a higher-level
      // loop processing deferred pointers, let it handle anything new.
      ConvertingStruct = true;

      // Note that we know that PTy cannot be resolved or invalidated here.
      Type *Actual = ConvertType(TREE_TYPE(type));
      assert(GET_TYPE_LLVM(type) == PTy && "Pointer invalidated!");

      // Restore ConvertingStruct for the caller.
      ConvertingStruct = false;

      if (Actual->isVoidTy())
        Actual = Type::getInt8Ty(Context);  // void* -> sbyte*

      // Update the type, potentially updating TYPE_LLVM(type).
      const OpaqueType *OT = cast<OpaqueType>(PTy->getElementType());
      const_cast<OpaqueType*>(OT)->refineAbstractTypeTo(Actual);
      Ty = GET_TYPE_LLVM(type);
      break;
    } else {
      // If we are converting a struct, and if we haven't converted the pointee
      // type, add this pointer to PointersToReresolve and return an opaque*.
      if (ConvertingStruct) {
        // If the pointee type has not already been converted to LLVM, create
        // a new opaque type and remember it in the database.
        Ty = GET_TYPE_LLVM(TYPE_MAIN_VARIANT(TREE_TYPE(type)));
        if (Ty == 0) {
          PointersToReresolve.push_back(type);
          Ty = TypeDB.setType(type,
                              PointerType::getUnqual(OpaqueType::get(Context)));
          break;
        }

        // A type has already been computed.  However, this may be some sort of
        // recursive struct.  We don't want to call ConvertType on it, because
        // this will try to resolve it, and not adding the type to the
        // PointerToReresolve collection is just an optimization.  Instead,
        // we'll use the type returned by GET_TYPE_LLVM directly, even if this
        // may be resolved further in the future.
      } else {
        // If we're not in a struct, just call ConvertType.  If it has already
        // been converted, this will return the precomputed value, otherwise
        // this will compute and return the new type.
        Ty = ConvertType(TREE_TYPE(type));
      }

      if (Ty->isVoidTy())
        Ty = Type::getInt8Ty(Context);  // void* -> sbyte*
      Ty = TypeDB.setType(type, Ty->getPointerTo());
      break;
    }

  case METHOD_TYPE:
  case FUNCTION_TYPE: {
    if ((Ty = GET_TYPE_LLVM(type)))
      return Ty;

    // No declaration to pass through, passing NULL.
    CallingConv::ID CallingConv;
    AttrListPtr PAL;
    Ty = TypeDB.setType(type, ConvertFunctionType(type, NULL, NULL,
                                                  CallingConv, PAL));
    break;
  }

  case ARRAY_TYPE: {
    if ((Ty = GET_TYPE_LLVM(type)))
      return Ty;

    Type *ElementTy = ConvertType(TREE_TYPE(type));
    uint64_t NumElements = ArrayLengthOf(type);

    if (NumElements == NO_LENGTH) // Variable length array?
      NumElements = 0;

    // Create the array type.
    Ty = ArrayType::get(ElementTy, NumElements);

    // If the user increased the alignment of the array element type, then the
    // size of the array is rounded up by that alignment even though the size
    // of the array element type is not (!).  Correct for this if necessary by
    // adding padding.  May also need padding if the element type has variable
    // size and the array type has variable length, but by a miracle the product
    // gives a constant size.
    if (isInt64(TYPE_SIZE(type), true)) {
      uint64_t PadBits = getInt64(TYPE_SIZE(type), true) -
        getTargetData().getTypeAllocSizeInBits(Ty);
      if (PadBits) {
        Type *Padding = ArrayType::get(Type::getInt8Ty(Context), PadBits / 8);
        Ty = StructType::get(Ty, Padding, NULL);
      }
    }

    Ty = TypeDB.setType(type, Ty);
    break;
  }

  case OFFSET_TYPE:
    // Handle OFFSET_TYPE specially.  This is used for pointers to members,
    // which are really just integer offsets.  As such, return the appropriate
    // integer directly.
    Ty = getTargetData().getIntPtrType(Context);
    break;
  }

  // Try to give the type a helpful name.  There is no point in doing this for
  // array and pointer types since LLVM automatically gives them a useful name
  // based on the element type.
  if (!Ty->isVoidTy() && !isa<SequentialType>(Ty)) {
    const std::string &TypeName = getDescriptiveName(type);
    if (!TypeName.empty())
      TheModule->addTypeName(TypeName, Ty);
  }

  return Ty;
}

//===----------------------------------------------------------------------===//
//                  FUNCTION/METHOD_TYPE Conversion Routines
//===----------------------------------------------------------------------===//

namespace {
  class FunctionTypeConversion : public DefaultABIClient {
    PATypeHolder &RetTy;
    std::vector<PATypeHolder> &ArgTypes;
    CallingConv::ID &CallingConv;
    bool isShadowRet;
    bool KNRPromotion;
    unsigned Offset;
  public:
    FunctionTypeConversion(PATypeHolder &retty, std::vector<PATypeHolder> &AT,
                           CallingConv::ID &CC, bool KNR)
      : RetTy(retty), ArgTypes(AT), CallingConv(CC), KNRPromotion(KNR), Offset(0) {
      CallingConv = CallingConv::C;
      isShadowRet = false;
    }

    /// getCallingConv - This provides the desired CallingConv for the function.
    CallingConv::ID& getCallingConv(void) { return CallingConv; }

    bool isShadowReturn() const { return isShadowRet; }

    /// HandleScalarResult - This callback is invoked if the function returns a
    /// simple scalar result value.
    void HandleScalarResult(Type *RetTy) {
      this->RetTy = RetTy;
    }

    /// HandleAggregateResultAsScalar - This callback is invoked if the function
    /// returns an aggregate value by bit converting it to the specified scalar
    /// type and returning that.
    void HandleAggregateResultAsScalar(Type *ScalarTy, unsigned Offset=0) {
      RetTy = ScalarTy;
      this->Offset = Offset;
    }

    /// HandleAggregateResultAsAggregate - This callback is invoked if the function
    /// returns an aggregate value using multiple return values.
    void HandleAggregateResultAsAggregate(Type *AggrTy) {
      RetTy = AggrTy;
    }

    /// HandleShadowResult - Handle an aggregate or scalar shadow argument.
    void HandleShadowResult(PointerType *PtrArgTy, bool RetPtr) {
      // This function either returns void or the shadow argument,
      // depending on the target.
      RetTy = RetPtr ? PtrArgTy : Type::getVoidTy(Context);

      // In any case, there is a dummy shadow argument though!
      ArgTypes.push_back(PtrArgTy);

      // Also, note the use of a shadow argument.
      isShadowRet = true;
    }

    /// HandleAggregateShadowResult - This callback is invoked if the function
    /// returns an aggregate value by using a "shadow" first parameter, which is
    /// a pointer to the aggregate, of type PtrArgTy.  If RetPtr is set to true,
    /// the pointer argument itself is returned from the function.
    void HandleAggregateShadowResult(PointerType *PtrArgTy,
                                       bool RetPtr) {
      HandleShadowResult(PtrArgTy, RetPtr);
    }

    /// HandleScalarShadowResult - This callback is invoked if the function
    /// returns a scalar value by using a "shadow" first parameter, which is a
    /// pointer to the scalar, of type PtrArgTy.  If RetPtr is set to true,
    /// the pointer argument itself is returned from the function.
    void HandleScalarShadowResult(PointerType *PtrArgTy, bool RetPtr) {
      HandleShadowResult(PtrArgTy, RetPtr);
    }

    void HandlePad(llvm::Type *LLVMTy) {
      HandleScalarArgument(LLVMTy, 0, 0);
    }

    void HandleScalarArgument(llvm::Type *LLVMTy, tree type,
                              unsigned /*RealSize*/ = 0) {
      if (KNRPromotion) {
        if (type == float_type_node)
          LLVMTy = ConvertType(double_type_node);
        else if (LLVMTy->isIntegerTy(16) || LLVMTy->isIntegerTy(8) ||
                 LLVMTy->isIntegerTy(1))
          LLVMTy = Type::getInt32Ty(Context);
      }
      ArgTypes.push_back(LLVMTy);
    }

    /// HandleByInvisibleReferenceArgument - This callback is invoked if a pointer
    /// (of type PtrTy) to the argument is passed rather than the argument itself.
    void HandleByInvisibleReferenceArgument(llvm::Type *PtrTy,
                                            tree /*type*/) {
      ArgTypes.push_back(PtrTy);
    }

    /// HandleByValArgument - This callback is invoked if the aggregate function
    /// argument is passed by value. It is lowered to a parameter passed by
    /// reference with an additional parameter attribute "ByVal".
    void HandleByValArgument(llvm::Type *LLVMTy, tree type) {
      HandleScalarArgument(LLVMTy->getPointerTo(), type);
    }

    /// HandleFCAArgument - This callback is invoked if the aggregate function
    /// argument is a first class aggregate passed by value.
    void HandleFCAArgument(llvm::Type *LLVMTy, tree /*type*/) {
      ArgTypes.push_back(LLVMTy);
    }
  };
}


static Attributes HandleArgumentExtension(tree ArgTy) {
  if (TREE_CODE(ArgTy) == BOOLEAN_TYPE) {
    if (TREE_INT_CST_LOW(TYPE_SIZE(ArgTy)) < INT_TYPE_SIZE)
      return Attribute::ZExt;
  } else if (TREE_CODE(ArgTy) == INTEGER_TYPE &&
             TREE_INT_CST_LOW(TYPE_SIZE(ArgTy)) < INT_TYPE_SIZE) {
    if (TYPE_UNSIGNED(ArgTy))
      return Attribute::ZExt;
    else
      return Attribute::SExt;
  }

  return Attribute::None;
}

/// ConvertParamListToLLVMSignature - This method is used to build the argument
/// type list for K&R prototyped functions.  In this case, we have to figure out
/// the type list (to build a FunctionType) from the actual DECL_ARGUMENTS list
/// for the function.  This method takes the DECL_ARGUMENTS list (Args), and
/// fills in Result with the argument types for the function.  It returns the
/// specified result type for the function.
FunctionType *TypeConverter::
ConvertArgListToFnType(tree type, tree Args, tree static_chain,
                       CallingConv::ID &CallingConv, AttrListPtr &PAL) {
  tree ReturnType = TREE_TYPE(type);
  std::vector<PATypeHolder> ArgTys;
  PATypeHolder RetTy(Type::getVoidTy(Context));

  FunctionTypeConversion Client(RetTy, ArgTys, CallingConv, true /*K&R*/);
  DefaultABI ABIConverter(Client);

#ifdef TARGET_ADJUST_LLVM_CC
  TARGET_ADJUST_LLVM_CC(CallingConv, type);
#endif

  // Builtins are always prototyped, so this isn't one.
  ABIConverter.HandleReturnType(ReturnType, current_function_decl, false);

  SmallVector<AttributeWithIndex, 8> Attrs;

  // Compute whether the result needs to be zext or sext'd.
  Attributes RAttributes = HandleArgumentExtension(ReturnType);

  // Allow the target to change the attributes.
#ifdef TARGET_ADJUST_LLVM_RETATTR
  TARGET_ADJUST_LLVM_RETATTR(RAttributes, type);
#endif

  if (RAttributes != Attribute::None)
    Attrs.push_back(AttributeWithIndex::get(0, RAttributes));

  // If this function returns via a shadow argument, the dest loc is passed
  // in as a pointer.  Mark that pointer as struct-ret and noalias.
  if (ABIConverter.isShadowReturn())
    Attrs.push_back(AttributeWithIndex::get(ArgTys.size(),
                                    Attribute::StructRet | Attribute::NoAlias));

  std::vector<Type*> ScalarArgs;
  if (static_chain) {
    // Pass the static chain as the first parameter.
    ABIConverter.HandleArgument(TREE_TYPE(static_chain), ScalarArgs);
    // Mark it as the chain argument.
    Attrs.push_back(AttributeWithIndex::get(ArgTys.size(),
                                             Attribute::Nest));
  }

  for (; Args && TREE_TYPE(Args) != void_type_node; Args = TREE_CHAIN(Args)) {
    tree ArgTy = TREE_TYPE(Args);

    // Determine if there are any attributes for this param.
    Attributes PAttributes = Attribute::None;

    ABIConverter.HandleArgument(ArgTy, ScalarArgs, &PAttributes);

    // Compute zext/sext attributes.
    PAttributes |= HandleArgumentExtension(ArgTy);

    if (PAttributes != Attribute::None)
      Attrs.push_back(AttributeWithIndex::get(ArgTys.size(), PAttributes));
  }

  PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());
  return GetFunctionType(RetTy, ArgTys, false);
}

FunctionType *TypeConverter::
ConvertFunctionType(tree type, tree decl, tree static_chain,
                    CallingConv::ID &CallingConv, AttrListPtr &PAL) {
  PATypeHolder RetTy = Type::getVoidTy(Context);
  std::vector<PATypeHolder> ArgTypes;
  bool isVarArg = false;
  FunctionTypeConversion Client(RetTy, ArgTypes, CallingConv, false/*not K&R*/);
  DefaultABI ABIConverter(Client);

  // Allow the target to set the CC for things like fastcall etc.
#ifdef TARGET_ADJUST_LLVM_CC
  TARGET_ADJUST_LLVM_CC(CallingConv, type);
#endif

  ABIConverter.HandleReturnType(TREE_TYPE(type), current_function_decl,
                                decl ? DECL_BUILT_IN(decl) : false);

  // Compute attributes for return type (and function attributes).
  SmallVector<AttributeWithIndex, 8> Attrs;
  Attributes FnAttributes = Attribute::None;

  int flags = flags_from_decl_or_type(decl ? decl : type);

  // Check for 'noreturn' function attribute.
  if (flags & ECF_NORETURN)
    FnAttributes |= Attribute::NoReturn;

  // Check for 'nounwind' function attribute.
  if (flags & ECF_NOTHROW)
    FnAttributes |= Attribute::NoUnwind;

  // Check for 'readnone' function attribute.
  // Both PURE and CONST will be set if the user applied
  // __attribute__((const)) to a function the compiler
  // knows to be pure, such as log.  A user or (more
  // likely) libm implementor might know their local log
  // is in fact const, so this should be valid (and gcc
  // accepts it).  But llvm IR does not allow both, so
  // set only ReadNone.
  if (flags & ECF_CONST)
    FnAttributes |= Attribute::ReadNone;

  // Check for 'readonly' function attribute.
  if (flags & ECF_PURE && !(flags & ECF_CONST))
    FnAttributes |= Attribute::ReadOnly;

  // Since they write the return value through a pointer,
  // 'sret' functions cannot be 'readnone' or 'readonly'.
  if (ABIConverter.isShadowReturn())
    FnAttributes &= ~(Attribute::ReadNone|Attribute::ReadOnly);

  // Demote 'readnone' nested functions to 'readonly' since
  // they may need to read through the static chain.
  if (static_chain && (FnAttributes & Attribute::ReadNone)) {
    FnAttributes &= ~Attribute::ReadNone;
    FnAttributes |= Attribute::ReadOnly;
  }

  // Compute whether the result needs to be zext or sext'd.
  Attributes RAttributes = Attribute::None;
  RAttributes |= HandleArgumentExtension(TREE_TYPE(type));

  // Allow the target to change the attributes.
#ifdef TARGET_ADJUST_LLVM_RETATTR
  TARGET_ADJUST_LLVM_RETATTR(RAttributes, type);
#endif

  // The value returned by a 'malloc' function does not alias anything.
  if (flags & ECF_MALLOC)
    RAttributes |= Attribute::NoAlias;

  if (RAttributes != Attribute::None)
    Attrs.push_back(AttributeWithIndex::get(0, RAttributes));

  // If this function returns via a shadow argument, the dest loc is passed
  // in as a pointer.  Mark that pointer as struct-ret and noalias.
  if (ABIConverter.isShadowReturn())
    Attrs.push_back(AttributeWithIndex::get(ArgTypes.size(),
                                    Attribute::StructRet | Attribute::NoAlias));

  std::vector<Type*> ScalarArgs;
  if (static_chain) {
    // Pass the static chain as the first parameter.
    ABIConverter.HandleArgument(TREE_TYPE(static_chain), ScalarArgs);
    // Mark it as the chain argument.
    Attrs.push_back(AttributeWithIndex::get(ArgTypes.size(),
                                             Attribute::Nest));
  }

  // If the target has regparam parameters, allow it to inspect the function
  // type.
  int local_regparam = 0;
  int local_fp_regparam = 0;
#ifdef LLVM_TARGET_ENABLE_REGPARM
  LLVM_TARGET_INIT_REGPARM(local_regparam, local_fp_regparam, type);
#endif // LLVM_TARGET_ENABLE_REGPARM

  // Keep track of whether we see a byval argument.
  bool HasByVal = false;

  // Check if we have a corresponding decl to inspect.
  tree DeclArgs = (decl) ? DECL_ARGUMENTS(decl) : NULL;
  // Loop over all of the arguments, adding them as we go.
  tree Args = TYPE_ARG_TYPES(type);
  for (; Args && TREE_VALUE(Args) != void_type_node; Args = TREE_CHAIN(Args)){
    tree ArgTy = TREE_VALUE(Args);
    if (!isPassedByInvisibleReference(ArgTy) &&
        ConvertType(ArgTy)->isOpaqueTy()) {
      // If we are passing an opaque struct by value, we don't know how many
      // arguments it will turn into.  Because we can't handle this yet,
      // codegen the prototype as (...).
      if (CallingConv == CallingConv::C)
        ArgTypes.clear();
      else
        // Don't nuke last argument.
        ArgTypes.erase(ArgTypes.begin()+1, ArgTypes.end());
      Args = 0;
      break;
    }

    // Determine if there are any attributes for this param.
    Attributes PAttributes = Attribute::None;

    unsigned OldSize = ArgTypes.size();

    ABIConverter.HandleArgument(ArgTy, ScalarArgs, &PAttributes);

    // Compute zext/sext attributes.
    PAttributes |= HandleArgumentExtension(ArgTy);

    // Compute noalias attributes. If we have a decl for the function
    // inspect it for restrict qualifiers, otherwise try the argument
    // types.
    tree RestrictArgTy = (DeclArgs) ? TREE_TYPE(DeclArgs) : ArgTy;
    if (TREE_CODE(RestrictArgTy) == POINTER_TYPE ||
        TREE_CODE(RestrictArgTy) == REFERENCE_TYPE) {
      if (TYPE_RESTRICT(RestrictArgTy))
        PAttributes |= Attribute::NoAlias;
    }

#ifdef LLVM_TARGET_ENABLE_REGPARM
    // Allow the target to mark this as inreg.
    if (INTEGRAL_TYPE_P(ArgTy) || POINTER_TYPE_P(ArgTy) ||
        SCALAR_FLOAT_TYPE_P(ArgTy))
      LLVM_ADJUST_REGPARM_ATTRIBUTE(PAttributes, ArgTy,
                                    TREE_INT_CST_LOW(TYPE_SIZE(ArgTy)),
                                    local_regparam, local_fp_regparam);
#endif // LLVM_TARGET_ENABLE_REGPARM

    if (PAttributes != Attribute::None) {
      HasByVal |= PAttributes & Attribute::ByVal;

      // If the argument is split into multiple scalars, assign the
      // attributes to all scalars of the aggregate.
      for (unsigned i = OldSize + 1; i <= ArgTypes.size(); ++i) {
        Attrs.push_back(AttributeWithIndex::get(i, PAttributes));
      }
    }

    if (DeclArgs)
      DeclArgs = TREE_CHAIN(DeclArgs);
  }

  // If there is a byval argument then it is not safe to mark the function
  // 'readnone' or 'readonly': gcc permits a 'const' or 'pure' function to
  // write to struct arguments passed by value, but in LLVM this becomes a
  // write through the byval pointer argument, which LLVM does not allow for
  // readonly/readnone functions.
  if (HasByVal)
    FnAttributes &= ~(Attribute::ReadNone | Attribute::ReadOnly);

  if (flag_force_vararg_prototypes)
    // If forcing prototypes to be varargs, make all function types varargs
    // except those for builtin functions.
    isVarArg = decl ? !DECL_BUILT_IN(decl) : true;
  else
    // If the argument list ends with a void type node, it isn't vararg.
    isVarArg = (Args == 0);
  assert(RetTy && "Return type not specified!");

  if (FnAttributes != Attribute::None)
    Attrs.push_back(AttributeWithIndex::get(~0, FnAttributes));

  // Finally, make the function type and result attributes.
  PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());
  return GetFunctionType(RetTy, ArgTypes, isVarArg);
}

//===----------------------------------------------------------------------===//
//                      RECORD/Struct Conversion Routines
//===----------------------------------------------------------------------===//

/// StructTypeConversionInfo - A temporary structure that is used when
/// translating a RECORD_TYPE to an LLVM type.
struct StructTypeConversionInfo {
  std::vector<Type*> Elements;
  std::vector<uint64_t> ElementOffsetInBytes;
  std::vector<uint64_t> ElementSizeInBytes;
  std::vector<bool> PaddingElement; // True if field is used for padding
  const TargetData &TD;
  unsigned GCCStructAlignmentInBytes;
  bool Packed; // True if struct is packed
  bool AllBitFields; // True if all struct fields are bit fields
  bool LastFieldStartsAtNonByteBoundry;
  unsigned ExtraBitsAvailable; // Non-zero if last field is bit field and it
                               // does not use all allocated bits

  StructTypeConversionInfo(TargetMachine &TM, unsigned GCCAlign, bool P)
    : TD(*TM.getTargetData()), GCCStructAlignmentInBytes(GCCAlign),
      Packed(P), AllBitFields(true), LastFieldStartsAtNonByteBoundry(false),
      ExtraBitsAvailable(0) {}

  void lastFieldStartsAtNonByteBoundry(bool value) {
    LastFieldStartsAtNonByteBoundry = value;
  }

  void extraBitsAvailable (unsigned E) {
    ExtraBitsAvailable = E;
  }

  bool isPacked() { return Packed; }

  void markAsPacked() {
    Packed = true;
  }

  void allFieldsAreNotBitFields() {
    AllBitFields = false;
    // Next field is not a bitfield.
    LastFieldStartsAtNonByteBoundry = false;
  }

  unsigned getGCCStructAlignmentInBytes() const {
    return GCCStructAlignmentInBytes;
  }

  /// getTypeAlignment - Return the alignment of the specified type in bytes.
  ///
  unsigned getTypeAlignment(Type *Ty) const {
    return Packed ? 1 : TD.getABITypeAlignment(Ty);
  }

  /// getTypeSize - Return the size of the specified type in bytes.
  ///
  uint64_t getTypeSize(Type *Ty) const {
    return TD.getTypeAllocSize(Ty);
  }

  /// getLLVMType - Return the LLVM type for the specified object.
  ///
  Type *getLLVMType() const {
    // Use Packed type if Packed is set or all struct fields are bitfields.
    // Empty struct is not packed unless packed is set.
    return StructType::get(Context, Elements,
                           Packed || (!Elements.empty() && AllBitFields));
  }

  /// getAlignmentAsLLVMStruct - Return the alignment of this struct if it were
  /// converted to an LLVM type.
  uint64_t getAlignmentAsLLVMStruct() const {
    if (Packed || AllBitFields) return 1;
    unsigned MaxAlign = 1;
    for (unsigned i = 0, e = Elements.size(); i != e; ++i)
      MaxAlign = std::max(MaxAlign, getTypeAlignment(Elements[i]));
    return MaxAlign;
  }

  /// getSizeAsLLVMStruct - Return the size of this struct if it were converted
  /// to an LLVM type.  This is the end of last element push an alignment pad at
  /// the end.
  uint64_t getSizeAsLLVMStruct() const {
    if (Elements.empty()) return 0;
    unsigned MaxAlign = getAlignmentAsLLVMStruct();
    uint64_t Size = ElementOffsetInBytes.back()+ElementSizeInBytes.back();
    return (Size+MaxAlign-1) & ~(MaxAlign-1);
  }

  // If this is a Packed struct and ExtraBitsAvailable is not zero then
  // remove Extra bytes if ExtraBitsAvailable > 8.
  void RemoveExtraBytes () {

    unsigned NoOfBytesToRemove = ExtraBitsAvailable/8;

    if (!Packed && !AllBitFields)
      return;

    if (NoOfBytesToRemove == 0)
      return;

    Type *LastType = Elements.back();
    unsigned PadBytes = 0;

    if (LastType->isIntegerTy(8))
      PadBytes = 1 - NoOfBytesToRemove;
    else if (LastType->isIntegerTy(16))
      PadBytes = 2 - NoOfBytesToRemove;
    else if (LastType->isIntegerTy(32))
      PadBytes = 4 - NoOfBytesToRemove;
    else if (LastType->isIntegerTy(64))
      PadBytes = 8 - NoOfBytesToRemove;
    else
      return;

    assert (PadBytes > 0 && "Unable to remove extra bytes");

    // Update last element type and size, element offset is unchanged.
    Type *Pad =  ArrayType::get(Type::getInt8Ty(Context), PadBytes);
    unsigned OriginalSize = ElementSizeInBytes.back();
    Elements.pop_back();
    Elements.push_back(Pad);

    ElementSizeInBytes.pop_back();
    ElementSizeInBytes.push_back(OriginalSize - NoOfBytesToRemove);
  }

  /// ResizeLastElementIfOverlapsWith - If the last element in the struct
  /// includes the specified byte, remove it. Return true struct
  /// layout is sized properly. Return false if unable to handle ByteOffset.
  /// In this case caller should redo this struct as a packed structure.
  bool ResizeLastElementIfOverlapsWith(uint64_t ByteOffset, tree /*Field*/,
                                       Type *Ty) {
    Type *SavedTy = NULL;

    if (!Elements.empty()) {
      assert(ElementOffsetInBytes.back() <= ByteOffset &&
             "Cannot go backwards in struct");

      SavedTy = Elements.back();
      if (ElementOffsetInBytes.back()+ElementSizeInBytes.back() > ByteOffset) {
        // The last element overlapped with this one, remove it.
        uint64_t PoppedOffset = ElementOffsetInBytes.back();
        Elements.pop_back();
        ElementOffsetInBytes.pop_back();
        ElementSizeInBytes.pop_back();
        PaddingElement.pop_back();
        uint64_t EndOffset = getNewElementByteOffset(1);
        if (EndOffset < PoppedOffset) {
          // Make sure that some field starts at the position of the
          // field we just popped.  Otherwise we might end up with a
          // gcc non-bitfield being mapped to an LLVM field with a
          // different offset.
          Type *Pad = Type::getInt8Ty(Context);
          if (PoppedOffset != EndOffset + 1)
            Pad = ArrayType::get(Pad, PoppedOffset - EndOffset);
          addElement(Pad, EndOffset, PoppedOffset - EndOffset);
        }
      }
    }

    // Get the LLVM type for the field.  If this field is a bitfield, use the
    // declared type, not the shrunk-to-fit type that GCC gives us in TREE_TYPE.
    unsigned ByteAlignment = getTypeAlignment(Ty);
    uint64_t NextByteOffset = getNewElementByteOffset(ByteAlignment);
    if (NextByteOffset > ByteOffset ||
        ByteAlignment > getGCCStructAlignmentInBytes()) {
      // LLVM disagrees as to where this field should go in the natural field
      // ordering.  Therefore convert to a packed struct and try again.
      return false;
    }

    // If alignment won't round us up to the right boundary, insert explicit
    // padding.
    if (NextByteOffset < ByteOffset) {
      uint64_t CurOffset = getNewElementByteOffset(1);
      Type *Pad = Type::getInt8Ty(Context);
      if (SavedTy && LastFieldStartsAtNonByteBoundry)
        // We want to reuse SavedType to access this bit field.
        // e.g. struct __attribute__((packed)) {
        //  unsigned int A,
        //  unsigned short B : 6,
        //                 C : 15;
        //  char D; };
        //  In this example, previous field is C and D is current field.
        addElement(SavedTy, CurOffset, ByteOffset - CurOffset);
      else if (ByteOffset - CurOffset != 1)
        Pad = ArrayType::get(Pad, ByteOffset - CurOffset);
      addElement(Pad, CurOffset, ByteOffset - CurOffset);
    }
    return true;
  }

  /// FieldNo - Remove the specified field and all of the fields that come after
  /// it.
  void RemoveFieldsAfter(unsigned FieldNo) {
    Elements.erase(Elements.begin()+FieldNo, Elements.end());
    ElementOffsetInBytes.erase(ElementOffsetInBytes.begin()+FieldNo,
                               ElementOffsetInBytes.end());
    ElementSizeInBytes.erase(ElementSizeInBytes.begin()+FieldNo,
                             ElementSizeInBytes.end());
    PaddingElement.erase(PaddingElement.begin()+FieldNo,
                         PaddingElement.end());
  }

  /// getNewElementByteOffset - If we add a new element with the specified
  /// alignment, what byte offset will it land at?
  uint64_t getNewElementByteOffset(unsigned ByteAlignment) {
    if (Elements.empty()) return 0;
    uint64_t LastElementEnd =
      ElementOffsetInBytes.back() + ElementSizeInBytes.back();

    return (LastElementEnd+ByteAlignment-1) & ~(ByteAlignment-1);
  }

  /// addElement - Add an element to the structure with the specified type,
  /// offset and size.
  void addElement(Type *Ty, uint64_t Offset, uint64_t Size,
                  bool ExtraPadding = false) {
    Elements.push_back(Ty);
    ElementOffsetInBytes.push_back(Offset);
    ElementSizeInBytes.push_back(Size);
    PaddingElement.push_back(ExtraPadding);
    lastFieldStartsAtNonByteBoundry(false);
    ExtraBitsAvailable = 0;
  }

  /// getFieldEndOffsetInBytes - Return the byte offset of the byte immediately
  /// after the specified field.  For example, if FieldNo is 0 and the field
  /// is 4 bytes in size, this will return 4.
  uint64_t getFieldEndOffsetInBytes(unsigned FieldNo) const {
    assert(FieldNo < ElementOffsetInBytes.size() && "Invalid field #!");
    return ElementOffsetInBytes[FieldNo]+ElementSizeInBytes[FieldNo];
  }

  /// getEndUnallocatedByte - Return the first byte that isn't allocated at the
  /// end of a structure.  For example, for {}, it's 0, for {int} it is 4, for
  /// {int,short}, it is 6.
  uint64_t getEndUnallocatedByte() const {
    if (ElementOffsetInBytes.empty()) return 0;
    return getFieldEndOffsetInBytes(ElementOffsetInBytes.size()-1);
  }

  void addNewBitField(uint64_t Size, uint64_t Extra,
                      uint64_t FirstUnallocatedByte);

  void dump() const;
};

// Add new element which is a bit field. Size is not the size of bit field,
// but size of bits required to determine type of new Field which will be
// used to access this bit field.
// If possible, allocate a field with room for Size+Extra bits.
void StructTypeConversionInfo::addNewBitField(uint64_t Size, uint64_t Extra,
                                              uint64_t FirstUnallocatedByte) {

  // Figure out the LLVM type that we will use for the new field.
  // Note, Size is not necessarily size of the new field. It indicates
  // additional bits required after FirstunallocatedByte to cover new field.
  Type *NewFieldTy = 0;

  // First try an ABI-aligned field including (some of) the Extra bits.
  // This field must satisfy Size <= w && w <= XSize.
  uint64_t XSize = Size + Extra;
  for (unsigned w = NextPowerOf2(std::min(UINT64_C(64), XSize))/2;
       w >= Size && w >= 8; w /= 2) {
    if (TD.isIllegalInteger(w))
      continue;
    // Would a w-sized integer field be aligned here?
    const unsigned a = TD.getABIIntegerTypeAlignment(w);
    if (FirstUnallocatedByte & (a-1) || a > getGCCStructAlignmentInBytes())
      continue;
    // OK, use w-sized integer.
    NewFieldTy = IntegerType::get(Context, w);
    break;
  }

  // Try an integer field that holds Size bits.
  if (!NewFieldTy) {
    if (Size <= 8)
      NewFieldTy = Type::getInt8Ty(Context);
    else if (Size <= 16)
      NewFieldTy = Type::getInt16Ty(Context);
    else if (Size <= 32)
      NewFieldTy = Type::getInt32Ty(Context);
    else {
      assert(Size <= 64 && "Bitfield too large!");
      NewFieldTy = Type::getInt64Ty(Context);
    }
  }

  // Check that the alignment of NewFieldTy won't cause a gap in the structure!
  unsigned ByteAlignment = getTypeAlignment(NewFieldTy);
  if (FirstUnallocatedByte & (ByteAlignment-1) ||
      ByteAlignment > getGCCStructAlignmentInBytes()) {
    // Instead of inserting a nice whole field, insert a small array of ubytes.
    NewFieldTy = ArrayType::get(Type::getInt8Ty(Context), (Size+7)/8);
  }

  // Finally, add the new field.
  addElement(NewFieldTy, FirstUnallocatedByte, getTypeSize(NewFieldTy));
  ExtraBitsAvailable = NewFieldTy->getPrimitiveSizeInBits() - Size;
}

void StructTypeConversionInfo::dump() const {
  raw_ostream &OS = outs();
  OS << "Info has " << Elements.size() << " fields:\n";
  for (unsigned i = 0, e = Elements.size(); i != e; ++i) {
    OS << "  Offset = " << ElementOffsetInBytes[i]
       << " Size = " << ElementSizeInBytes[i]
       << " Type = ";
    WriteTypeSymbolic(OS, Elements[i], TheModule);
    OS << "\n";
  }
  OS.flush();
}

/// DecodeStructFields - This method decodes the specified field, if it is a
/// FIELD_DECL, adding or updating the specified StructTypeConversionInfo to
/// reflect it.  Return true if field is decoded correctly. Otherwise return
/// false.
bool TypeConverter::DecodeStructFields(tree Field,
                                       StructTypeConversionInfo &Info) {
  // Handle bit-fields specially.
  if (isBitfield(Field)) {
    // If this field is forcing packed llvm struct then retry entire struct
    // layout.
    if (!Info.isPacked()) {
      // Unnamed bitfield type does not contribute in struct alignment
      // computations. Use packed llvm structure in such cases.
      if (!DECL_NAME(Field))
        return false;
      // If this field is packed then the struct may need padding fields
      // before this field.
      if (DECL_PACKED(Field))
        return false;
      // If Field has user defined alignment and it does not match Ty alignment
      // then convert to a packed struct and try again.
      if (TYPE_USER_ALIGN(TREE_TYPE(Field))) {
        Type *Ty = ConvertType(TREE_TYPE(Field));
        if (TYPE_ALIGN(TREE_TYPE(Field)) !=
            8 * Info.getTypeAlignment(Ty))
          return false;
      }
    }
    DecodeStructBitField(Field, Info);
    return true;
  }

  Info.allFieldsAreNotBitFields();

  // Get the starting offset in the record.
  uint64_t StartOffsetInBits = getFieldOffsetInBits(Field);
  assert((StartOffsetInBits & 7) == 0 && "Non-bit-field has non-byte offset!");
  uint64_t StartOffsetInBytes = StartOffsetInBits/8;

  Type *Ty = ConvertType(TREE_TYPE(Field));

  // If this field is packed then the struct may need padding fields
  // before this field.
  if (DECL_PACKED(Field) && !Info.isPacked())
    return false;
  // Pop any previous elements out of the struct if they overlap with this one.
  // This can happen when the C++ front-end overlaps fields with tail padding in
  // C++ classes.
  else if (!Info.ResizeLastElementIfOverlapsWith(StartOffsetInBytes, Field, Ty)) {
    // LLVM disagrees as to where this field should go in the natural field
    // ordering.  Therefore convert to a packed struct and try again.
    return false;
  }
  else if (TYPE_USER_ALIGN(TREE_TYPE(Field))
           && (unsigned)DECL_ALIGN(Field) != 8 * Info.getTypeAlignment(Ty)
           && !Info.isPacked()) {
    // If Field has user defined alignment and it does not match Ty alignment
    // then convert to a packed struct and try again.
    return false;
  } else
    // At this point, we know that adding the element will happen at the right
    // offset.  Add it.
    Info.addElement(Ty, StartOffsetInBytes, Info.getTypeSize(Ty));
  return true;
}

/// DecodeStructBitField - This method decodes the specified bit-field, adding
/// or updating the specified StructTypeConversionInfo to reflect it.
///
/// Note that in general, we cannot produce a good covering of struct fields for
/// bitfields.  As such, we only make sure that all bits in a struct that
/// correspond to a bitfield are represented in the LLVM struct with
/// (potentially multiple) integer fields of integer type.  This ensures that
/// initialized globals with bitfields can have the initializers for the
/// bitfields specified.
void TypeConverter::DecodeStructBitField(tree_node *Field,
                                         StructTypeConversionInfo &Info) {
  unsigned FieldSizeInBits = TREE_INT_CST_LOW(DECL_SIZE(Field));

  if (FieldSizeInBits == 0)   // Ignore 'int:0', which just affects layout.
    return;

  // Get the starting offset in the record.
  uint64_t StartOffsetInBits = getFieldOffsetInBits(Field);
  uint64_t EndBitOffset    = FieldSizeInBits+StartOffsetInBits;

  // If the last inserted LLVM field completely contains this bitfield, just
  // ignore this field.
  if (!Info.Elements.empty()) {
    uint64_t LastFieldBitOffset = Info.ElementOffsetInBytes.back()*8;
    unsigned LastFieldBitSize   = Info.ElementSizeInBytes.back()*8;
    assert(LastFieldBitOffset <= StartOffsetInBits &&
           "This bitfield isn't part of the last field!");
    if (EndBitOffset <= LastFieldBitOffset+LastFieldBitSize &&
        LastFieldBitOffset+LastFieldBitSize >= StartOffsetInBits) {
      // Already contained in previous field. Update remaining extra bits that
      // are available.
      Info.extraBitsAvailable(Info.getEndUnallocatedByte()*8 - EndBitOffset);
      return;
    }
  }

  // Otherwise, this bitfield lives (potentially) partially in the preceding
  // field and in fields that exist after it.  Add integer-typed fields to the
  // LLVM struct such that there are no holes in the struct where the bitfield
  // is: these holes would make it impossible to statically initialize a global
  // of this type that has an initializer for the bitfield.

  // We want the integer-typed fields as large as possible up to the machine
  // word size. If there are more bitfields following this one, try to include
  // them in the same field.

  // Calculate the total number of bits in the continuous group of bitfields
  // following this one. This is the number of bits that addNewBitField should
  // try to include.
  unsigned ExtraSizeInBits = 0;
  tree LastBitField = 0;
  for (tree f = TREE_CHAIN(Field); f; f = TREE_CHAIN(f)) {
    assert(TREE_CODE(Field) == FIELD_DECL && "Lang data not freed?");
    if (TREE_CODE(DECL_FIELD_OFFSET(f)) != INTEGER_CST)
      break;
    if (isBitfield(f))
      LastBitField = f;
    else {
      // We can use all this bits up to the next non-bitfield.
      LastBitField = 0;
      ExtraSizeInBits = getFieldOffsetInBits(f) - EndBitOffset;
      break;
    }
  }
  // Record ended in a bitfield? Use all of the last byte.
  if (LastBitField)
    ExtraSizeInBits = RoundUpToAlignment(getFieldOffsetInBits(LastBitField) +
      TREE_INT_CST_LOW(DECL_SIZE(LastBitField)), 8) - EndBitOffset;

  // Compute the number of bits that we need to add to this struct to cover
  // this field.
  uint64_t FirstUnallocatedByte = Info.getEndUnallocatedByte();
  uint64_t StartOffsetFromByteBoundry = StartOffsetInBits & 7;

  if (StartOffsetInBits < FirstUnallocatedByte*8) {

    uint64_t AvailableBits = FirstUnallocatedByte * 8 - StartOffsetInBits;
    // This field's starting point is already allocated.
    if (StartOffsetFromByteBoundry == 0) {
      // This field starts at byte boundary. Need to allocate space
      // for additional bytes not yet allocated.
      unsigned NumBitsToAdd = FieldSizeInBits - AvailableBits;
      Info.addNewBitField(NumBitsToAdd, ExtraSizeInBits, FirstUnallocatedByte);
      return;
    }

    // Otherwise, this field's starting point is inside previously used byte.
    // This happens with Packed bit fields. In this case one LLVM Field is
    // used to access previous field and current field.
    unsigned prevFieldTypeSizeInBits =
      Info.ElementSizeInBytes[Info.Elements.size() - 1] * 8;

    unsigned NumBitsRequired = prevFieldTypeSizeInBits
      + (FieldSizeInBits - AvailableBits);

    if (NumBitsRequired > 64) {
      // Use bits from previous field.
      NumBitsRequired = FieldSizeInBits - AvailableBits;
    } else {
      // If type used to access previous field is not large enough then
      // remove previous field and insert new field that is large enough to
      // hold both fields.
      Info.RemoveFieldsAfter(Info.Elements.size() - 1);
      for (unsigned idx = 0; idx < (prevFieldTypeSizeInBits/8); ++idx)
        FirstUnallocatedByte--;
    }
    Info.addNewBitField(NumBitsRequired, ExtraSizeInBits, FirstUnallocatedByte);
    // Do this after adding Field.
    Info.lastFieldStartsAtNonByteBoundry(true);
    return;
  }

  if (StartOffsetInBits > FirstUnallocatedByte*8) {
    // If there is padding between the last field and the struct, insert
    // explicit bytes into the field to represent it.
    unsigned PadBytes = 0;
    unsigned PadBits = 0;
    if (StartOffsetFromByteBoundry != 0) {
      // New field does not start at byte boundary.
      PadBits = StartOffsetInBits - (FirstUnallocatedByte*8);
      PadBytes = PadBits/8;
      PadBits = PadBits - PadBytes*8;
    } else
      PadBytes = StartOffsetInBits/8-FirstUnallocatedByte;

    if (PadBytes) {
      Type *Pad = Type::getInt8Ty(Context);
      if (PadBytes != 1)
        Pad = ArrayType::get(Pad, PadBytes);
      Info.addElement(Pad, FirstUnallocatedByte, PadBytes);
    }

    FirstUnallocatedByte = StartOffsetInBits/8;
    // This field will use some of the bits from this PadBytes, if
    // starting offset is not at byte boundary.
    if (StartOffsetFromByteBoundry != 0)
      FieldSizeInBits += PadBits;
  }

  // Now, Field starts at FirstUnallocatedByte and everything is aligned.
  Info.addNewBitField(FieldSizeInBits, ExtraSizeInBits, FirstUnallocatedByte);
}

/// UnionHasOnlyZeroOffsets - Check if a union type has only members with
/// offsets that are zero, e.g., no Fortran equivalences.
static bool UnionHasOnlyZeroOffsets(tree type) {
  for (tree Field = TYPE_FIELDS(type); Field; Field = TREE_CHAIN(Field)) {
    assert(TREE_CODE(Field) == FIELD_DECL && "Lang data not freed?");
    if (!OffsetIsLLVMCompatible(Field))
      return false;
    if (getFieldOffsetInBits(Field) != 0)
      return false;
  }
  return true;
}

/// SelectUnionMember - Find the union member with the largest aligment.  If
/// there are multiple types with the same alignment, select the one with
/// the largest size. If the type with max. align is smaller than other types,
/// then we will add padding later on anyway to match union size.
void TypeConverter::SelectUnionMember(tree type,
                                      StructTypeConversionInfo &Info) {
  bool FindBiggest = TREE_CODE(type) != QUAL_UNION_TYPE;

  Type *UnionTy = 0;
  tree GccUnionTy = 0;
  tree UnionField = 0;
  unsigned MinAlign = ~0U;
  uint64_t BestSize = FindBiggest ? 0 : ~(uint64_t)0;
  for (tree Field = TYPE_FIELDS(type); Field; Field = TREE_CHAIN(Field)) {
    assert(TREE_CODE(Field) == FIELD_DECL && "Lang data not freed?");
    assert(DECL_FIELD_OFFSET(Field) && integer_zerop(DECL_FIELD_OFFSET(Field))
           && "Union with non-zero offset?");

    // Skip fields that are known not to be present.
    if (TREE_CODE(type) == QUAL_UNION_TYPE &&
        integer_zerop(DECL_QUALIFIER(Field)))
      continue;

    tree TheGccTy = TREE_TYPE(Field);

    // Skip zero-length bitfields.  These are only used for setting the
    // alignment.
    if (DECL_BIT_FIELD(Field) && DECL_SIZE(Field) &&
        integer_zerop(DECL_SIZE(Field)))
      continue;

    Type *TheTy = ConvertType(TheGccTy);
    unsigned Align = Info.getTypeAlignment(TheTy);
    uint64_t Size  = Info.getTypeSize(TheTy);

    // Select TheTy as union type if it is the biggest/smallest field (depending
    // on the value of FindBiggest).  If more than one field achieves this size
    // then choose the least aligned.
    if ((Size == BestSize && Align < MinAlign) ||
        (FindBiggest && Size > BestSize) ||
        (!FindBiggest && Size < BestSize)) {
      UnionTy = TheTy;
      UnionField = Field;
      GccUnionTy = TheGccTy;
      BestSize = Size;
      MinAlign = Align;
    }

    // Skip remaining fields if this one is known to be present.
    if (TREE_CODE(type) == QUAL_UNION_TYPE &&
        integer_onep(DECL_QUALIFIER(Field)))
      break;
  }

  if (UnionTy) {            // Not an empty union.
    if (8 * Info.getTypeAlignment(UnionTy) > TYPE_ALIGN(type))
      Info.markAsPacked();

    if (isBitfield(UnionField)) {
      unsigned FieldSizeInBits = TREE_INT_CST_LOW(DECL_SIZE(UnionField));
      Info.addNewBitField(FieldSizeInBits, 0, 0);
    } else {
      Info.allFieldsAreNotBitFields();
      Info.addElement(UnionTy, 0, Info.getTypeSize(UnionTy));
    }
  }
}

/// ConvertRECORD - Convert a RECORD_TYPE, UNION_TYPE or QUAL_UNION_TYPE to
/// an LLVM type.
// A note on C++ virtual base class layout.  Consider the following example:
// class A { public: int i0; };
// class B : public virtual A { public: int i1; };
// class C : public virtual A { public: int i2; };
// class D : public virtual B, public virtual C { public: int i3; };
//
// The TYPE nodes gcc builds for classes represent that class as it looks
// standing alone.  Thus B is size 12 and looks like { vptr; i2; baseclass A; }
// However, this is not the layout used when that class is a base class for
// some other class, yet the same TYPE node is still used.  D in the above has
// both a BINFO list entry and a FIELD that reference type B, but the virtual
// base class A within B is not allocated in that case; B-within-D is only
// size 8.  The correct size is in the FIELD node (does not match the size
// in its child TYPE node.)  The fields to be omitted from the child TYPE,
// as far as I can tell, are always the last ones; but also, there is a
// TYPE_DECL node sitting in the middle of the FIELD list separating virtual
// base classes from everything else.
//
// Similarly, a nonvirtual base class which has virtual base classes might
// not contain those virtual base classes when used as a nonvirtual base class.
// There is seemingly no way to detect this except for the size differential.
//
// For LLVM purposes, we build a new type for B-within-D that
// has the correct size and layout for that usage.

Type *TypeConverter::ConvertRECORD(tree type) {
  if (Type *Ty = GET_TYPE_LLVM(type)) {
    // If we already compiled this type, and if it was not a forward
    // definition that is now defined, use the old type.
    if (!Ty->isOpaqueTy() || TYPE_SIZE(type) == 0)
      return Ty;
  }

  if (TYPE_SIZE(type) == 0) {   // Forward declaration?
    Type *Ty = OpaqueType::get(Context);
    return TypeDB.setType(type, Ty);
  }

  // Note that we are compiling a struct now.
  bool OldConvertingStruct = ConvertingStruct;
  ConvertingStruct = true;

  // Record those fields which will be converted to LLVM fields.
  SmallVector<std::pair<tree, uint64_t>, 32> Fields;
  for (tree Field = TYPE_FIELDS(type); Field; Field = TREE_CHAIN(Field)) {
    assert(TREE_CODE(Field) == FIELD_DECL && "Lang data not freed?");
    if (OffsetIsLLVMCompatible(Field))
      Fields.push_back(std::make_pair(Field, getFieldOffsetInBits(Field)));
  }

  // The fields are almost always sorted, but occasionally not.  Sort them by
  // field offset.
  for (unsigned i = 1, e = Fields.size(); i < e; i++)
    for (unsigned j = i; j && Fields[j].second < Fields[j-1].second; j--)
      std::swap(Fields[j], Fields[j-1]);

  StructTypeConversionInfo *Info =
    new StructTypeConversionInfo(*TheTarget, TYPE_ALIGN(type) / 8,
                                 TYPE_PACKED(type));

  // Convert over all of the elements of the struct.
  // Workaround to get Fortran EQUIVALENCE working.
  // TODO: Unify record and union logic and handle this optimally.
  bool HasOnlyZeroOffsets = TREE_CODE(type) != RECORD_TYPE &&
    UnionHasOnlyZeroOffsets(type);
  if (HasOnlyZeroOffsets) {
    SelectUnionMember(type, *Info);
  } else {
    // Convert over all of the elements of the struct.
    bool retryAsPackedStruct = false;
    for (unsigned i = 0, e = Fields.size(); i < e; i++)
      if (DecodeStructFields(Fields[i].first, *Info) == false) {
        retryAsPackedStruct = true;
        break;
      }

    if (retryAsPackedStruct) {
      delete Info;
      Info = new StructTypeConversionInfo(*TheTarget, TYPE_ALIGN(type) / 8,
                                          true);
      for (unsigned i = 0, e = Fields.size(); i < e; i++)
        if (DecodeStructFields(Fields[i].first, *Info) == false) {
          assert(0 && "Unable to decode struct fields.");
        }
    }
  }

  // Insert tail padding if the LLVM struct requires explicit tail padding to
  // be the same size as the GCC struct or union.  This handles, e.g., "{}" in
  // C++, and cases where a union has larger alignment than the largest member
  // does.
  if (TYPE_SIZE(type) && TREE_CODE(TYPE_SIZE(type)) == INTEGER_CST) {
    uint64_t GCCTypeSize = getInt64(TYPE_SIZE_UNIT(type), true);
    uint64_t LLVMStructSize = Info->getSizeAsLLVMStruct();

    if (LLVMStructSize > GCCTypeSize) {
      Info->RemoveExtraBytes();
      LLVMStructSize = Info->getSizeAsLLVMStruct();
    }

    if (LLVMStructSize != GCCTypeSize) {
      assert(LLVMStructSize < GCCTypeSize &&
             "LLVM type size doesn't match GCC type size!");
      uint64_t LLVMLastElementEnd = Info->getNewElementByteOffset(1);

      // If only one byte is needed then insert i8.
      if (GCCTypeSize-LLVMLastElementEnd == 1)
        Info->addElement(Type::getInt8Ty(Context), 1, 1);
      else {
        if (((GCCTypeSize-LLVMStructSize) % 4) == 0 &&
            (Info->getAlignmentAsLLVMStruct() %
             Info->getTypeAlignment(Type::getInt32Ty(Context))) == 0) {
          // Insert array of i32.
          unsigned Int32ArraySize = (GCCTypeSize-LLVMStructSize) / 4;
          Type *PadTy =
            ArrayType::get(Type::getInt32Ty(Context), Int32ArraySize);
          Info->addElement(PadTy, GCCTypeSize - LLVMLastElementEnd,
                           Int32ArraySize, true /* Padding Element */);
        } else {
          Type *PadTy = ArrayType::get(Type::getInt8Ty(Context),
                                             GCCTypeSize-LLVMStructSize);
          Info->addElement(PadTy, GCCTypeSize - LLVMLastElementEnd,
                           GCCTypeSize - LLVMLastElementEnd,
                           true /* Padding Element */);
        }
      }
    }
  } else
    Info->RemoveExtraBytes();

  Type *ResultTy = Info->getLLVMType();

  const OpaqueType *OldTy = cast_or_null<OpaqueType>(GET_TYPE_LLVM(type));
  TypeDB.setType(type, ResultTy);

  // If there was a forward declaration for this type that is now resolved,
  // refine anything that used it to the new type.
  if (OldTy)
    const_cast<OpaqueType*>(OldTy)->refineAbstractTypeTo(ResultTy);

  // We have finished converting this struct.  See if the is the outer-most
  // struct or union being converted by ConvertType.
  ConvertingStruct = OldConvertingStruct;
  if (!ConvertingStruct) {

    // If this is the outer-most level of structness, resolve any pointers
    // that were deferred.
    while (!PointersToReresolve.empty()) {
      if (tree PtrTy = PointersToReresolve.back()) {
        ConvertType(PtrTy);   // Reresolve this pointer type.
        assert((PointersToReresolve.empty() ||
                PointersToReresolve.back() != PtrTy) &&
               "Something went wrong with pointer resolution!");
      } else {
        // Null marker element.
        PointersToReresolve.pop_back();
      }
    }
  }

  return GET_TYPE_LLVM(type);
}
