//===--- SourceLoader.cpp - Import .swift files as modules ------*- c++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file A simple module loader that loads .swift source files as
/// TranslationUnit modules.
///
//===----------------------------------------------------------------------===//

#include "swift/Sema/SourceLoader.h"
#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/Diagnostics.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/system_error.h"

using namespace swift;

static llvm::error_code findModule(ASTContext &ctx, StringRef moduleID,
                                   SourceLoc importLoc,
                                   llvm::OwningPtr<llvm::MemoryBuffer> &buffer){
  llvm::SmallString<64> moduleFilename(moduleID);
  moduleFilename += ".swift";

  llvm::SmallString<128> inputFilename;

  // First, search in the directory corresponding to the import location.
  // FIXME: This screams for a proper FileManager abstraction.
  if (importLoc.isValid()) {
    unsigned currentBufferID =
        ctx.SourceMgr.findBufferContainingLoc(importLoc);
    const llvm::MemoryBuffer *importingBuffer
      = ctx.SourceMgr->getMemoryBuffer(currentBufferID);
    StringRef currentDirectory
      = llvm::sys::path::parent_path(importingBuffer->getBufferIdentifier());
    if (!currentDirectory.empty()) {
      inputFilename = currentDirectory;
      llvm::sys::path::append(inputFilename, moduleFilename.str());
      llvm::error_code err = llvm::MemoryBuffer::getFile(inputFilename.str(),
                                                         buffer);
      if (!err)
        return err;
    }
  }

  // Second, search in the current directory.
  llvm::error_code err = llvm::MemoryBuffer::getFile(moduleFilename.str(),
                                                     buffer);
  if (!err)
    return err;

  // If we fail, search each import search path.
  for (auto Path : ctx.ImportSearchPaths) {
    inputFilename = Path;
    llvm::sys::path::append(inputFilename, moduleFilename.str());
    err = llvm::MemoryBuffer::getFile(inputFilename.str(), buffer);
    if (!err)
      return err;
  }

  return err;
}

namespace {

/// Don't parse any function bodies except those that are transparent.
class SkipNonTransparentFunctions : public DelayedParsingCallbacks {
  bool shouldDelayFunctionBodyParsing(Parser &TheParser,
                                      AbstractFunctionDecl *AFD,
                                      const DeclAttributes &Attrs,
                                      SourceRange BodyRange) override {
    return Attrs.isTransparent();
  }
};

} // unnamed namespace

Module *SourceLoader::loadModule(SourceLoc importLoc,
                             ArrayRef<std::pair<Identifier, SourceLoc>> path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  llvm::OwningPtr<llvm::MemoryBuffer> inputFile;
  if (llvm::error_code err = findModule(Ctx, moduleID.first.str(),
                                        moduleID.second, inputFile)) {
    if (err.value() != llvm::errc::no_such_file_or_directory) {
      Ctx.Diags.diagnose(moduleID.second, diag::sema_opening_import,
                         moduleID.first.str(), err.message());
    }

    return nullptr;
  }

  // Turn off debugging while parsing other modules.
  llvm::SaveAndRestore<bool> turnOffDebug(Ctx.LangOpts.DebugConstraintSolver,
                                          false);

  unsigned bufferID = Ctx.SourceMgr.addNewSourceBuffer(inputFile.take(),
                                                       moduleID.second);

  auto *importTU = new (Ctx) TranslationUnit(moduleID.first, Ctx);
  Ctx.LoadedModules[moduleID.first.str()] = importTU;

  auto *importFile = new (Ctx) SourceFile(*importTU, SourceFile::Library,
                                          bufferID);
  importTU->addSourceFile(*importFile);

  bool done;
  PersistentParserState persistentState;
  SkipNonTransparentFunctions delayCallbacks;
  parseIntoTranslationUnit(*importFile, bufferID, &done, nullptr,
                           &persistentState,
                           SkipBodies ? &delayCallbacks : nullptr);
  assert(done && "Parser returned early?");
  (void)done;
  
  if (SkipBodies)
    performDelayedParsing(importTU, persistentState, nullptr);

  // FIXME: Support recursive definitions in immediate modes by making type
  // checking even lazier.
  if (SkipBodies)
    performNameBinding(*importFile);
  else
    performTypeChecking(*importFile);

  return importTU;
}

void SourceLoader::loadExtensions(NominalTypeDecl *nominal,
                                  unsigned previousGeneration) {
  // Type-checking the source automatically loads all extensions; there's
  // nothing to do here.
}
