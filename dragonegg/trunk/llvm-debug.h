//===---- llvm-debug.h - Interface for generating debug info ----*- C++ -*-===//
//
// Copyright (C) 2006, 2007, 2008, 2009, 2010  Jim Laskey, Duncan Sands et al.
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
// This file declares the debug interfaces shared among the dragonegg files.
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUG_H
#define LLVM_DEBUG_H

// Plugin headers
#include "llvm-internal.h"

// LLVM headers
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ValueHandle.h"

// System headers
#include <map>

namespace llvm {

// Forward declarations
class AllocaInst;
class BasicBlock;
class CallInst;
class Function;
class Module;

/// DebugInfo - This class gathers all debug information during compilation and
/// is responsible for emitting to llvm globals or pass directly to the backend.
class DebugInfo {
private:
  Module *M;                            // The current module.
  DIFactory DebugFactory;
  const char *CurFullPath;              // Previous location file encountered.
  int CurLineNo;                        // Previous location line# encountered.
  const char *PrevFullPath;             // Previous location file encountered.
  int PrevLineNo;                       // Previous location line# encountered.
  BasicBlock *PrevBB;                   // Last basic block encountered.

  DICompileUnit TheCU;                  // The compile unit.

  std::map<tree_node *, WeakVH > TypeCache;
                                        // Cache of previously constructed
                                        // Types.
  std::map<tree_node *, WeakVH > SPCache;
                                        // Cache of previously constructed
                                        // Subprograms.
  std::map<tree_node *, WeakVH> NameSpaceCache;
                                        // Cache of previously constructed name
                                        // spaces.

  SmallVector<WeakVH, 4> RegionStack;
                                        // Stack to track declarative scopes.

  std::map<tree_node *, WeakVH> RegionMap;

  /// FunctionNames - This is a storage for function names that are
  /// constructed on demand. For example, C++ destructors, C++ operators etc..
  llvm::BumpPtrAllocator FunctionNames;

public:
  DebugInfo(Module *m);

  /// Initialize - Initialize debug info by creating compile unit for
  /// main_input_filename. This must be invoked after language dependent
  /// initialization is done.
  void Initialize();

  // Accessors.
  void setLocationFile(const char *FullPath) { CurFullPath = FullPath; }
  void setLocationLine(int LineNo)           { CurLineNo = LineNo; }

  /// EmitFunctionStart - Constructs the debug code for entering a function -
  /// "llvm.dbg.func.start."
  void EmitFunctionStart(tree_node *FnDecl, Function *Fn);

  /// EmitFunctionEnd - Constructs the debug code for exiting a declarative
  /// region - "llvm.dbg.region.end."
  void EmitFunctionEnd(bool EndFunction);

  /// EmitDeclare - Constructs the debug code for allocation of a new variable.
  /// region - "llvm.dbg.declare."
  void EmitDeclare(tree_node *decl, unsigned Tag, const char *Name,
                   tree_node *type, Value *AI, LLVMBuilder &Builder);

  /// EmitStopPoint - Emit a call to llvm.dbg.stoppoint to indicate a change of
  /// source line.
  void EmitStopPoint(BasicBlock *CurBB, LLVMBuilder &Builder);

  /// EmitGlobalVariable - Emit information about a global variable.
  ///
  void EmitGlobalVariable(GlobalVariable *GV, tree_node *decl);

  /// getOrCreateType - Get the type from the cache or create a new type if
  /// necessary.
  DIType getOrCreateType(tree_node *type);

  /// createBasicType - Create BasicType.
  DIType createBasicType(tree_node *type);

  /// createMethodType - Create MethodType.
  DIType createMethodType(tree_node *type);

  /// createPointerType - Create PointerType.
  DIType createPointerType(tree_node *type);

  /// createArrayType - Create ArrayType.
  DIType createArrayType(tree_node *type);

  /// createEnumType - Create EnumType.
  DIType createEnumType(tree_node *type);

  /// createStructType - Create StructType for struct or union or class.
  DIType createStructType(tree_node *type);

  /// createVarinatType - Create variant type or return MainTy.
  DIType createVariantType(tree_node *type, DIType MainTy);

  /// getOrCreateCompileUnit - Create a new compile unit.
  DICompileUnit getOrCreateCompileUnit(const char *FullPath,
                                       bool isMain = false);

  /// getOrCreateFile - Get DIFile descriptor.
  DIFile getOrCreateFile(const char *FullPath);

  /// findRegion - Find tree_node N's region.
  DIDescriptor findRegion(tree_node *n);

  /// getOrCreateNameSpace - Get name space descriptor for the tree node.
  DINameSpace getOrCreateNameSpace(tree_node *Node, DIDescriptor Context);

  /// getFunctionName - Get function name for the given FnDecl. If the
  /// name is constructred on demand (e.g. C++ destructor) then the name
  /// is stored on the side.
  StringRef getFunctionName(tree_node *FnDecl);
};

} // end namespace llvm

#endif /* LLVM_DEBUG_H */
