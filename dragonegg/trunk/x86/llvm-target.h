/* Some target-specific hooks for gcc->llvm conversion
Copyright (C) 2007 Free Software Foundation, Inc.
Contributed by Anton Korobeynikov (asl@math.spbu.ru)

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

#ifndef LLVM_TARGET_H
#define LLVM_TARGET_H

/* LLVM specific stuff for supporting calling convention output */
#define TARGET_ADJUST_LLVM_CC(CC, type)                         \
  {                                                             \
    tree type_attributes = TYPE_ATTRIBUTES (type);              \
    if (lookup_attribute ("stdcall", type_attributes)) {        \
      CC = CallingConv::X86_StdCall;                            \
    } else if (lookup_attribute("fastcall", type_attributes)) { \
      CC = CallingConv::X86_FastCall;                           \
    }                                                           \
  }

#define TARGET_ADJUST_LLVM_RETATTR(Rattributes, type)           \
  {                                                             \
    tree type_attributes = TYPE_ATTRIBUTES (type);              \
    if (!TARGET_64BIT && (TARGET_SSEREGPARM ||                  \
               lookup_attribute("sseregparm", type_attributes)))\
      RAttributes |= Attribute::InReg;                          \
  }

/* LLVM specific stuff for converting gcc's `regparm` attribute to LLVM's
   `inreg` parameter attribute */
#define LLVM_TARGET_ENABLE_REGPARM

extern "C" int ix86_regparm;

#define LLVM_TARGET_INIT_REGPARM(local_regparm, local_fp_regparm, type) \
  {                                                             \
    tree attr;                                                  \
    local_regparm = ix86_regparm;                               \
    local_fp_regparm = TARGET_SSEREGPARM ? 3 : 0;               \
    attr = lookup_attribute ("regparm",                         \
                              TYPE_ATTRIBUTES (type));          \
    if (attr) {                                                 \
      local_regparm = TREE_INT_CST_LOW (TREE_VALUE              \
                                        (TREE_VALUE (attr)));   \
    }                                                           \
    attr = lookup_attribute("sseregparm",                       \
                              TYPE_ATTRIBUTES (type));          \
    if (attr)                                                   \
      local_fp_regparm = 3;                                     \
  }

#define LLVM_ADJUST_REGPARM_ATTRIBUTE(PAttribute, Type, Size,   \
                                      local_regparm,            \
                                      local_fp_regparm)         \
  {                                                             \
    if (!TARGET_64BIT) {                                        \
      if (TREE_CODE(Type) == REAL_TYPE &&                       \
          (TYPE_PRECISION(Type)==32 ||                          \
           TYPE_PRECISION(Type)==64)) {                         \
          local_fp_regparm -= 1;                                \
          if (local_fp_regparm >= 0)                            \
            PAttribute |= Attribute::InReg;                     \
          else                                                  \
            local_fp_regparm = 0;                               \
      } else if (INTEGRAL_TYPE_P(Type) ||                       \
                 POINTER_TYPE_P(Type)) {                        \
          int words =                                           \
                  (Size + BITS_PER_WORD - 1) / BITS_PER_WORD;   \
          local_regparm -= words;                               \
          if (local_regparm>=0)                                 \
            PAttribute |= Attribute::InReg;                     \
          else                                                  \
            local_regparm = 0;                                  \
      }                                                         \
    }                                                           \
  }

#define LLVM_SET_RED_ZONE_FLAG(disable_red_zone)                \
  if (TARGET_64BIT && TARGET_NO_RED_ZONE)                       \
    disable_red_zone = 1;

#ifdef LLVM_ABI_H

/* On x86-32 objects containing SSE vectors are 16 byte aligned, everything
   else 4.  On x86-64 vectors are 8-byte aligned, everything else can
   be figured out by the back end. */
#define LLVM_BYVAL_ALIGNMENT(T) \
  (TYPE_ALIGN(T) / 8)

extern tree llvm_x86_should_return_selt_struct_as_scalar(tree);

/* Structs containing a single data field plus zero-length fields are
   considered as if they were the type of the data field.  On x86-64,
   if the element type is an MMX vector, return it as double (which will
   get it into XMM0). */

#define LLVM_SHOULD_RETURN_SELT_STRUCT_AS_SCALAR(X) \
  llvm_x86_should_return_selt_struct_as_scalar((X))

extern bool llvm_x86_should_pass_aggregate_in_integer_regs(tree,
                                                          unsigned*, bool*);

/* LLVM_SHOULD_PASS_AGGREGATE_IN_INTEGER_REGS - Return true if this aggregate
   value should be passed in integer registers.  This differs from the usual
   handling in that x86-64 passes 128-bit structs and unions which only
   contain data in the first 64 bits, as 64-bit objects.  (These can be
   created by abusing __attribute__((aligned)).  */
#define LLVM_SHOULD_PASS_AGGREGATE_IN_INTEGER_REGS(X, Y, Z)             \
  llvm_x86_should_pass_aggregate_in_integer_regs((X), (Y), (Z))

extern const Type *llvm_x86_scalar_type_for_struct_return(tree type,
                                                          unsigned *Offset);

/* LLVM_SCALAR_TYPE_FOR_STRUCT_RETURN - Return LLVM Type if X can be
   returned as a scalar, otherwise return NULL. */
#define LLVM_SCALAR_TYPE_FOR_STRUCT_RETURN(X, Y) \
  llvm_x86_scalar_type_for_struct_return((X), (Y))

extern const Type *llvm_x86_aggr_type_for_struct_return(tree type);

/* LLVM_AGGR_TYPE_FOR_STRUCT_RETURN - Return LLVM Type if X can be
   returned as an aggregate, otherwise return NULL. */
#define LLVM_AGGR_TYPE_FOR_STRUCT_RETURN(X, CC) \
  llvm_x86_aggr_type_for_struct_return(X)

extern void llvm_x86_extract_multiple_return_value(Value *Src, Value *Dest,
                                                   bool isVolatile,
                                                   LLVMBuilder &B);

/* LLVM_EXTRACT_MULTIPLE_RETURN_VALUE - Extract multiple return value from
   SRC and assign it to DEST. */
#define LLVM_EXTRACT_MULTIPLE_RETURN_VALUE(Src,Dest,V,B)       \
  llvm_x86_extract_multiple_return_value((Src),(Dest),(V),(B))

extern bool llvm_x86_should_pass_vector_using_byval_attr(tree);

/* On x86-64, vectors which are not MMX nor SSE should be passed byval. */
#define LLVM_SHOULD_PASS_VECTOR_USING_BYVAL_ATTR(X)      \
  llvm_x86_should_pass_vector_using_byval_attr((X))

extern bool llvm_x86_should_pass_vector_in_integer_regs(tree);

/* On x86-32, vectors which are not MMX nor SSE should be passed as integers. */
#define LLVM_SHOULD_PASS_VECTOR_IN_INTEGER_REGS(X)      \
  llvm_x86_should_pass_vector_in_integer_regs((X))

extern tree llvm_x86_should_return_vector_as_scalar(tree, bool);

/* The MMX vector v1i64 is returned in EAX and EDX on Darwin.  Communicate
    this by returning i64 here.  Likewise, (generic) vectors such as v2i16
    are returned in EAX.
    On Darwin x86-64, MMX vectors are returned in XMM0.  Communicate this by
    returning f64.  */
#define LLVM_SHOULD_RETURN_VECTOR_AS_SCALAR(X,isBuiltin)\
  llvm_x86_should_return_vector_as_scalar((X), (isBuiltin))

extern bool llvm_x86_should_return_vector_as_shadow(tree, bool);

/* MMX vectors v2i32, v4i16, v8i8, v2f32 are returned using sret on Darwin
   32-bit.  Vectors bigger than 128 are returned using sret.  */
#define LLVM_SHOULD_RETURN_VECTOR_AS_SHADOW(X,isBuiltin)\
  llvm_x86_should_return_vector_as_shadow((X),(isBuiltin))

extern bool
llvm_x86_should_not_return_complex_in_memory(tree type);

/* LLVM_SHOULD_NOT_RETURN_COMPLEX_IN_MEMORY - A hook to allow
   special _Complex handling. Return true if X should be returned using
   multiple value return instruction.  */
#define LLVM_SHOULD_NOT_RETURN_COMPLEX_IN_MEMORY(X) \
  llvm_x86_should_not_return_complex_in_memory((X))

extern bool
llvm_x86_should_pass_aggregate_as_fca(tree type, const Type *);

/* LLVM_SHOULD_PASS_AGGREGATE_AS_FCA - Return true if an aggregate of the
   specified type should be passed as a first-class aggregate. */
#ifndef LLVM_SHOULD_PASS_AGGREGATE_AS_FCA
#define LLVM_SHOULD_PASS_AGGREGATE_AS_FCA(X, TY) \
  llvm_x86_should_pass_aggregate_as_fca(X, TY)
#endif

extern bool llvm_x86_should_pass_aggregate_in_memory(tree, const Type *);

#define LLVM_SHOULD_PASS_AGGREGATE_USING_BYVAL_ATTR(X, TY)      \
  llvm_x86_should_pass_aggregate_in_memory(X, TY)


extern bool
llvm_x86_64_should_pass_aggregate_in_mixed_regs(tree, const Type *Ty,
                                                std::vector<const Type*>&);
extern bool
llvm_x86_32_should_pass_aggregate_in_mixed_regs(tree, const Type *Ty,
                                                std::vector<const Type*>&);

#define LLVM_SHOULD_PASS_AGGREGATE_IN_MIXED_REGS(T, TY, CC, E)       \
  (TARGET_64BIT ?                                                    \
   llvm_x86_64_should_pass_aggregate_in_mixed_regs((T), (TY), (E)) : \
   llvm_x86_32_should_pass_aggregate_in_mixed_regs((T), (TY), (E)))

extern
bool llvm_x86_64_aggregate_partially_passed_in_regs(std::vector<const Type*>&,
                                                    std::vector<const Type*>&,
                                                    bool);

#define LLVM_AGGREGATE_PARTIALLY_PASSED_IN_REGS(E, SE, ISR, CC)       \
  (TARGET_64BIT ?                                                     \
   llvm_x86_64_aggregate_partially_passed_in_regs((E), (SE), (ISR)) : \
   false)

#endif /* LLVM_ABI_H */

/* Register class used for passing given 64bit part of the argument.
   These represent classes as documented by the PS ABI, with the exception
   of SSESF, SSEDF classes, that are basically SSE class, just gcc will
   use SF or DFmode move instead of DImode to avoid reformatting penalties.

   Similarly we play games with INTEGERSI_CLASS to use cheaper SImode moves
   whenever possible (upper half does contain padding).
 */
enum x86_64_reg_class
  {
    X86_64_NO_CLASS,
    X86_64_INTEGER_CLASS,
    X86_64_INTEGERSI_CLASS,
    X86_64_SSE_CLASS,
    X86_64_SSESF_CLASS,
    X86_64_SSEDF_CLASS,
    X86_64_SSEUP_CLASS,
    X86_64_X87_CLASS,
    X86_64_X87UP_CLASS,
    X86_64_COMPLEX_X87_CLASS,
    X86_64_MEMORY_CLASS
  };

/* LLVM_TARGET_INTRINSIC_PREFIX - Specify what prefix this target uses for its
 * intrinsics.
 */
#define LLVM_TARGET_INTRINSIC_PREFIX "x86"

/* LLVM_TARGET_NAME - This specifies the name of the target, which correlates to
 * the llvm::InitializeXXXTarget() function.
 */
#define LLVM_TARGET_NAME X86

/* Turn -march=xx into a CPU type.
 */
#define LLVM_SET_SUBTARGET_FEATURES(F) \
  { if (TARGET_MACHO && ! strcmp (ix86_arch_string, "apple")) \
      F.setCPU(TARGET_64BIT ? "core2" : "yonah");             \
    else                                                      \
      F.setCPU(ix86_arch_string);                             \
                                                              \
    if (TARGET_64BIT)                                         \
      F.AddFeature("64bit");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_64BIT)   \
      F.AddFeature("64bit", false);                           \
                                                              \
    if (TARGET_MMX)                                           \
      F.AddFeature("mmx");                                    \
    else if (target_flags_explicit & OPTION_MASK_ISA_MMX)     \
      F.AddFeature("mmx", false);                             \
                                                              \
    if (TARGET_3DNOW)                                         \
      F.AddFeature("3dnow");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_3DNOW)   \
      F.AddFeature("3dnow", false);                           \
                                                              \
    if (TARGET_3DNOW_A)                                       \
      F.AddFeature("3dnowa");                                 \
    else if (target_flags_explicit & OPTION_MASK_ISA_3DNOW_A) \
      F.AddFeature("3dnowa", false);                          \
                                                              \
    if (TARGET_SSE)                                           \
      F.AddFeature("sse");                                    \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE)     \
      F.AddFeature("sse", false);                             \
                                                              \
    if (TARGET_SSE2)                                          \
      F.AddFeature("sse2");                                   \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE2)    \
      F.AddFeature("sse2", false);                            \
                                                              \
    if (TARGET_SSE3)                                          \
      F.AddFeature("sse3");                                   \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE3)    \
      F.AddFeature("sse3", false);                            \
                                                              \
    if (TARGET_SSSE3)                                         \
      F.AddFeature("ssse3");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSSE3)   \
      F.AddFeature("ssse3", false);                           \
                                                              \
    if (TARGET_SSE4_1)                                        \
      F.AddFeature("sse41");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE4_1)  \
      F.AddFeature("sse41", false);                           \
                                                              \
    if (TARGET_SSE4_2)                                        \
      F.AddFeature("sse42");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE4_2)  \
      F.AddFeature("sse42", false);                           \
                                                              \
    if (TARGET_AVX)                                           \
      F.AddFeature("avx");                                    \
    else if (target_flags_explicit & OPTION_MASK_ISA_AVX)     \
      F.AddFeature("avx", false);                             \
                                                              \
    if (TARGET_FMA)                                           \
      F.AddFeature("fma3");                                   \
    else if (target_flags_explicit & OPTION_MASK_ISA_FMA)     \
      F.AddFeature("fma3", false);                            \
                                                              \
    if (TARGET_SSE4A)                                         \
      F.AddFeature("sse4a");                                  \
    else if (target_flags_explicit & OPTION_MASK_ISA_SSE4A)   \
      F.AddFeature("sse4a", false);                           \
  }

#define LLVM_SET_IMPLICIT_FLOAT(flag_no_implicit_float)       \
  if (!TARGET_80387)                                          \
    flag_no_implicit_float = 1;                               \
  else                                                        \
    flag_no_implicit_float = 0;

/* LLVM ABI definition macros. */

/* When -m64 is specified, set the architecture to x86_64-os-blah even if the
 * compiler was configured for i[3456]86-os-blah.
 */
#define LLVM_OVERRIDE_TARGET_ARCH() \
  (TARGET_64BIT ? "x86_64" : "i386")

/* LLVM_TARGET_INTRINSIC_LOWER - To handle builtins, we want to expand the
 * invocation into normal LLVM code.  If the target can handle the builtin, this
 * macro should call the target TreeToLLVM::TargetIntrinsicLower method and
 *  return true.This macro is invoked from a method in the TreeToLLVM class.
 */
#define LLVM_TARGET_INTRINSIC_LOWER(STMT, FNDECL, DESTLOC, RESULT, DESTTY, OPS) \
        TargetIntrinsicLower(STMT, FNDECL, DESTLOC, RESULT, DESTTY, OPS);

/* LLVM_GET_REG_NAME - When extracting a register name for a constraint, use
   the string extracted from the magic symbol built for that register, rather
   than reg_names.  The latter maps both AH and AL to the same thing, which
   means we can't distinguish them. */
#define LLVM_GET_REG_NAME(REG_NAME, REG_NUM) __extension__ \
  ({ const char *nm = (REG_NAME);                          \
     if (*nm == '%' || *nm == '#') ++nm;                   \
     (ISDIGIT (*nm) ? reg_names[REG_NUM] : nm); })

/* Propagate code model setting to backend */
#define LLVM_SET_MACHINE_OPTIONS(argvec)           \
  switch (ix86_cmodel) {                           \
  default:                                         \
    sorry ("code model %<%s%> not supported yet", ix86_cmodel_string);  \
    break;                                         \
  case CM_SMALL:                                   \
  case CM_SMALL_PIC:                               \
    argvec.push_back("--code-model=small");        \
    break;                                         \
  case CM_KERNEL:                                  \
    argvec.push_back("--code-model=kernel");       \
    break;                                         \
  case CM_MEDIUM:                                  \
  case CM_MEDIUM_PIC:                              \
    argvec.push_back("--code-model=medium");       \
    break;                                         \
  case CM_32:                                      \
    argvec.push_back("--code-model=default");      \
    break;                                         \
  }

#endif /* LLVM_TARGET_H */
