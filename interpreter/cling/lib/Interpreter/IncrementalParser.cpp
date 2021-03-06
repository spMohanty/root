//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// version: $Id$
// author:  Axel Naumann <axel@cern.ch>
//------------------------------------------------------------------------------

#include "IncrementalParser.h"
#include "ASTDumper.h"
#include "ASTNodeEraser.h"
#include "AutoSynthesizer.h"
#include "DeclCollector.h"
#include "DeclExtractor.h"
#include "DynamicLookup.h"
#include "IRDumper.h"
#include "NullDerefProtectionTransformer.h"
#include "ReturnSynthesizer.h"
#include "ValuePrinterSynthesizer.h"
#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/Transaction.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Parse/Parser.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Serialization/ASTWriter.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_os_ostream.h"

#include <iostream>
#include <stdio.h>
#include <sstream>

using namespace clang;

namespace cling {
  IncrementalParser::IncrementalParser(Interpreter* interp,
                                       int argc, const char* const *argv,
                                       const char* llvmdir):
    m_Interpreter(interp), m_Consumer(0) {

    CompilerInstance* CI
      = CIFactory::createCI(0, argc, argv, llvmdir);
    assert(CI && "CompilerInstance is (null)!");

    m_Consumer = dyn_cast<DeclCollector>(&CI->getASTConsumer());
    assert(m_Consumer && "Expected ChainedConsumer!");
    m_Consumer->setInterpreter(interp);

    m_CI.reset(CI);

    if (CI->getFrontendOpts().ProgramAction != clang::frontend::ParseSyntaxOnly){
      m_CodeGen.reset(CreateLLVMCodeGen(CI->getDiagnostics(), "cling input",
                                        CI->getCodeGenOpts(),
                                        CI->getTargetOpts(),
                                        *m_Interpreter->getLLVMContext()
                                        ));
      m_Consumer->setCodeGen(m_CodeGen.get());
    }

    CreateSLocOffsetGenerator();

    // Add transformers to the IncrementalParser, which owns them
    Sema* TheSema = &CI->getSema();
    // Register the AST Transformers
    m_ASTTransformers.push_back(new EvaluateTSynthesizer(TheSema));
    m_ASTTransformers.push_back(new AutoSynthesizer(TheSema));
    m_ASTTransformers.push_back(new ValuePrinterSynthesizer(TheSema, 0));
    m_ASTTransformers.push_back(new ASTDumper());
    m_ASTTransformers.push_back(new DeclExtractor(TheSema));
    m_ASTTransformers.push_back(new ReturnSynthesizer(TheSema));
    
    // Register the IR Transformers
    m_IRTransformers.push_back(new IRDumper());
    m_IRTransformers.push_back(new NullDerefProtectionTransformer());
  }

  void IncrementalParser::Initialize() {
    if (hasCodeGenerator())
      getCodeGenerator()->Initialize(getCI()->getASTContext());

    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = CompilationOptions::VPDisabled;
    CO.CodeGeneration = hasCodeGenerator();

    // pull in PCHs
    const std::string& PCHFileName
      = m_CI->getInvocation ().getPreprocessorOpts().ImplicitPCHInclude;
    if (!PCHFileName.empty()) {
      
      Transaction* CurT = beginTransaction(CO);
      m_CI->createPCHExternalASTSource(PCHFileName,
                                       true /*DisablePCHValidation*/,
                                       true /*AllowPCHWithCompilerErrors*/,
                                       0 /*DeserializationListener*/);
      Transaction* EndedT = endTransaction(CurT);
      commitTransaction(EndedT);
    }

    Transaction* CurT = beginTransaction(CO);
    Sema* TheSema = &m_CI->getSema();
    m_Parser.reset(new Parser(m_CI->getPreprocessor(), *TheSema,
                              false /*skipFuncBodies*/));
    m_CI->getPreprocessor().EnterMainSourceFile();
    // Initialize the parser after we have entered the main source file.
    m_Parser->Initialize();
    // Perform initialization that occurs after the parser has been initialized
    // but before it parses anything. Initializes the consumers too.
    TheSema->Initialize();

    ExternalASTSource *External = TheSema->getASTContext().getExternalSource();
    if (External)
      External->StartTranslationUnit(m_Consumer);

    Transaction* EndedT = endTransaction(CurT);
    commitTransaction(EndedT);
  }

  IncrementalParser::~IncrementalParser() {
    if (hasCodeGenerator()) {
      getCodeGenerator()->ReleaseModule();
    }
    const Transaction* T = getFirstTransaction();
    const Transaction* nextT = 0;
    while (T) {
      assert((T->getState() == Transaction::kCommitted 
              || T->getState() == Transaction::kRolledBack)
             && "Not committed?");
      nextT = T->getNext();
      delete T;
      T = nextT;
    }

    for (size_t i = 0; i < m_ASTTransformers.size(); ++i)
      delete m_ASTTransformers[i];

    for (size_t i = 0; i < m_IRTransformers.size(); ++i)
      delete m_IRTransformers[i];
  }

  Transaction* IncrementalParser::beginTransaction(const CompilationOptions& 
                                                   Opts) {
    Transaction* OldCurT = m_Consumer->getTransaction();
    Transaction* NewCurT = 0;
    // If we are in the middle of transaction and we see another begin 
    // transaction - it must be nested transaction.
    if (OldCurT && OldCurT->getState() <= Transaction::kCommitting) {
      // If the last nested was empty just reuse it.
      Transaction* LastNestedT = OldCurT->getLastNestedTransaction();
      if (LastNestedT && LastNestedT->empty()) {
        assert(LastNestedT->getState() == Transaction::kCommitted && "Broken");
        NewCurT = LastNestedT;
        NewCurT->reset();
        NewCurT->setCompilationOpts(Opts);
      }
      else {
        NewCurT = new Transaction(Opts);
        OldCurT->addNestedTransaction(NewCurT); // takes the ownership
      }
      m_Consumer->setTransaction(NewCurT);
      return NewCurT;
    }

    if (getLastTransaction() && getLastTransaction()->empty()) {
      NewCurT = getLastTransaction();
      NewCurT->reset();
      NewCurT->setCompilationOpts(Opts);
    }
    else
      NewCurT = new Transaction(Opts);

    m_Consumer->setTransaction(NewCurT);

    if (NewCurT != getLastTransaction()) {
      if (getLastTransaction())
        m_Transactions.back()->setNext(NewCurT);
      m_Transactions.push_back(NewCurT);
    }
    return NewCurT;
  }

  Transaction* IncrementalParser::endTransaction(Transaction* T) const {
    assert(T && "Null transaction!?");
    assert(T->getState() == Transaction::kCollecting);
    T->setState(Transaction::kCompleted);
    const DiagnosticsEngine& Diags = getCI()->getSema().getDiagnostics();

    //TODO: Make the enum orable.
    if (Diags.getNumWarnings() > 0)
      T->setIssuedDiags(Transaction::kWarnings);

    if (Diags.hasErrorOccurred() || Diags.hasFatalErrorOccurred())
      T->setIssuedDiags(Transaction::kErrors);

      
    if (T->hasNestedTransactions()) {
      for(Transaction::const_nested_iterator I = T->nested_begin(),
            E = T->nested_end(); I != E; ++I)
        assert((*I)->isCompleted() && "Nested transaction not completed!?");
    }

    return T;
  }

  void IncrementalParser::commitTransaction(Transaction* T) {
    //Transaction* CurT = m_Consumer->getTransaction();
    assert(T->isCompleted() && "Transaction not ended!?");
    assert(T->getState() != Transaction::kCommitted
           && "Committing an already committed transaction.");

    // If committing a nested transaction the active one should be its parent
    // from now on.
    if (T->isNestedTransaction())
      m_Consumer->setTransaction(T->getParent());

    // Check for errors...
    if (T->getIssuedDiags() == Transaction::kErrors) {
      rollbackTransaction(T);
      return;
    }

    if (T->hasNestedTransactions()) {
      for(Transaction::const_nested_iterator I = T->nested_begin(),
            E = T->nested_end(); I != E; ++I)
        if ((*I)->getState() != Transaction::kCommitted)
          commitTransaction(*I);
    }

    if (!transformTransactionAST(T))
      return;

    // Here we expect a template instantiation. We need to open the transaction
    // that we are currently work with.
    Transaction::State oldState = T->getState();
    T->setState(Transaction::kCollecting);
    // Pull all template instantiations in that came from the consumers.
    getCI()->getSema().PerformPendingInstantiations();
    T->setState(oldState);

    m_Consumer->HandleTranslationUnit(getCI()->getASTContext());

    if (T->getCompilationOpts().CodeGeneration && hasCodeGenerator()) {
      codeGenTransaction(T);
      transformTransactionIR(T);
    }

    // The static initializers might run anything and can thus cause more
    // decls that need to end up in a transaction. But this one is done
    // with CodeGen...
    T->setState(Transaction::kCommitted);

    if (T->getCompilationOpts().CodeGeneration && hasCodeGenerator()) {
      runStaticInitOnTransaction(T);
    }

    InterpreterCallbacks* callbacks = m_Interpreter->getCallbacks();

    if (callbacks)
      callbacks->TransactionCommitted(*T);

    // If the transaction is empty do nothing.
    // Except it was nested transaction and we want to reuse it later on.
    if (T->empty() && T->isNestedTransaction()) {
      // We need to remove the marker from its parent.
      Transaction* ParentT = T->getParent();
      for (size_t i = 0; i < ParentT->size(); ++i)
        if ((*ParentT)[i].m_DGR.isNull())
          ParentT->erase(i);
    }
  }


  void IncrementalParser::markWholeTransactionAsUsed(Transaction* T) const {
    for (size_t Idx = 0; Idx < T->size() /*can change in the loop!*/; ++Idx) {
      Transaction::DelayCallInfo I = (*T)[Idx];
      // FIXME: implement for multiple decls in a DGR.
      assert(I.m_DGR.isSingleDecl());
      Decl* D = I.m_DGR.getSingleDecl();
      if (!D->hasAttr<clang::UsedAttr>())
        D->addAttr(::new (D->getASTContext())
                   clang::UsedAttr(D->getSourceRange(), D->getASTContext(),
                                   0/*AttributeSpellingListIndex*/));
    }
  }


  void IncrementalParser::codeGenTransaction(Transaction* T) {
    // codegen the transaction
    assert(T->getCompilationOpts().CodeGeneration && "CodeGen turned off");
    assert(hasCodeGenerator() && "No CodeGen");

    T->setModule(getCodeGenerator()->GetModule());

    for (size_t Idx = 0; Idx < T->size() /*can change in the loop!*/; ++Idx) {
      // Copy DCI; it might get relocated below.
      Transaction::DelayCallInfo I = (*T)[Idx];

      if (I.m_Call == Transaction::kCCIHandleTopLevelDecl)
        getCodeGenerator()->HandleTopLevelDecl(I.m_DGR);
      else if (I.m_Call == Transaction::kCCIHandleInterestingDecl) {
        // Usually through BackendConsumer which doesn't implement
        // HandleInterestingDecl() and thus calls
        // ASTConsumer::HandleInterestingDecl()
        getCodeGenerator()->HandleTopLevelDecl(I.m_DGR);
      } else if(I.m_Call == Transaction::kCCIHandleTagDeclDefinition) {
        TagDecl* TD = cast<TagDecl>(I.m_DGR.getSingleDecl());
        getCodeGenerator()->HandleTagDeclDefinition(TD);
      }
      else if (I.m_Call == Transaction::kCCIHandleVTable) {
        CXXRecordDecl* CXXRD = cast<CXXRecordDecl>(I.m_DGR.getSingleDecl());
        getCodeGenerator()->HandleVTable(CXXRD, /*isRequired*/true);
      }
      else if (I.m_Call
               == Transaction::kCCIHandleCXXImplicitFunctionInstantiation) {
        FunctionDecl* FD = cast<FunctionDecl>(I.m_DGR.getSingleDecl());
        getCodeGenerator()->HandleCXXImplicitFunctionInstantiation(FD);
      }
      else if (I.m_Call
               == Transaction::kCCIHandleCXXStaticMemberVarInstantiation) {
        VarDecl* VD = cast<VarDecl>(I.m_DGR.getSingleDecl());
        getCodeGenerator()->HandleCXXStaticMemberVarInstantiation(VD);
      }
      else if (I.m_Call == Transaction::kCCINone)
        ; // We use that internally as delimiter in the Transaction.
      else
        llvm_unreachable("We shouldn't have decl without call info.");
    }

    getCodeGenerator()->HandleTranslationUnit(getCI()->getASTContext());
  }

  bool IncrementalParser::transformTransactionAST(Transaction* T) const {
    bool success = true;
    // We are sure it's safe to pipe it through the transformers
    for (size_t i = 0; success && i < m_ASTTransformers.size(); ++i)
      success = m_ASTTransformers[i]->TransformTransaction(*T);

    m_CI->getDiagnostics().Reset(); // FIXME: Should be in rollback transaction.

    if (!success)
      rollbackTransaction(T);

    return success;
  }

  bool IncrementalParser::transformTransactionIR(Transaction* T) const {
    // Transform IR
    bool success = true;
    for (size_t i = 0; success && i < m_IRTransformers.size(); ++i)
      success = m_IRTransformers[i]->TransformTransaction(*T);
    if (!success)
      rollbackTransaction(T);
    return success;
  }

  bool IncrementalParser::runStaticInitOnTransaction(Transaction* T) const {
    // run the static initializers that came from codegenning
    bool success =
      m_Interpreter->runStaticInitializersOnce() < Interpreter::kExeFirstError;
    if (!success)
      rollbackTransaction(T);
    return success;
  }

  void IncrementalParser::rollbackTransaction(Transaction* T) const {
    ASTNodeEraser NodeEraser(&getCI()->getSema());

    if (NodeEraser.RevertTransaction(T))
      T->setState(Transaction::kRolledBack);
    else
      T->setState(Transaction::kRolledBackWithErrors);
  }

  std::vector<const Transaction*> IncrementalParser::getAllTransactions() {
    std::vector<const Transaction*> result;
    const cling::Transaction* T = getFirstTransaction();
    while (T) {
      result.push_back(T);
      T = T->getNext();
    }
    return result;
  }

  // Each input line is contained in separate memory buffer. The SourceManager
  // assigns sort-of invalid FileID for each buffer, i.e there is no FileEntry
  // for the MemoryBuffer's FileID. That in turn is problem because invalid
  // SourceLocations are given to the diagnostics. Thus the diagnostics cannot
  // order the overloads, for example
  //
  // Our work-around is creating a virtual file, which doesn't exist on the disk
  // with enormous size (no allocation is done). That file has valid FileEntry
  // and so on... We use it for generating valid SourceLocations with valid
  // offsets so that it doesn't cause any troubles to the diagnostics.
  //
  // +---------------------+
  // | Main memory buffer  |
  // +---------------------+
  // |  Virtual file SLoc  |
  // |    address space    |<-----------------+
  // |         ...         |<------------+    |
  // |         ...         |             |    |
  // |         ...         |<----+       |    |
  // |         ...         |     |       |    |
  // +~~~~~~~~~~~~~~~~~~~~~+     |       |    |
  // |     input_line_1    | ....+.......+..--+
  // +---------------------+     |       |
  // |     input_line_2    | ....+.....--+
  // +---------------------+     |
  // |          ...        |     |
  // +---------------------+     |
  // |     input_line_N    | ..--+
  // +---------------------+
  //
  void IncrementalParser::CreateSLocOffsetGenerator() {
    SourceManager& SM = getCI()->getSourceManager();
    m_VirtualFileID = SM.getMainFileID();
    assert(!m_VirtualFileID.isInvalid() && "No VirtualFileID created?");
  }

  Transaction* IncrementalParser::Compile(llvm::StringRef input,
                                          const CompilationOptions& Opts) {

    Transaction* CurT = beginTransaction(Opts);
    EParseResult ParseRes = ParseInternal(input);

    if (ParseRes == kSuccessWithWarnings)
      CurT->setIssuedDiags(Transaction::kWarnings);
    else if (ParseRes == kFailed)
      CurT->setIssuedDiags(Transaction::kErrors);

    const Transaction* EndedT = endTransaction(CurT);
    assert(EndedT == CurT && "Not ending the expected transaction.");
    commitTransaction(CurT);

    return CurT;
  }

  Transaction* IncrementalParser::Parse(llvm::StringRef input,
                                        const CompilationOptions& Opts) {
    Transaction* CurT = beginTransaction(Opts);
    ParseInternal(input);
    Transaction* EndedT = endTransaction(CurT);
    assert(EndedT == CurT && "Not ending the expected transaction.");
    return EndedT;
  }

  // Add the input to the memory buffer, parse it, and add it to the AST.
  IncrementalParser::EParseResult
  IncrementalParser::ParseInternal(llvm::StringRef input) {
    if (input.empty()) return IncrementalParser::kSuccess;

    Sema& S = getCI()->getSema();

    assert(!(S.getLangOpts().Modules
             && m_Consumer->getTransaction()->getCompilationOpts()
              .CodeGenerationForModule)
           && "CodeGenerationForModule should be removed once modules are available!");

    // Recover resources if we crash before exiting this method.
    llvm::CrashRecoveryContextCleanupRegistrar<Sema> CleanupSema(&S);

    Preprocessor& PP = m_CI->getPreprocessor();
    if (!PP.getCurrentLexer()) {
       PP.EnterSourceFile(m_CI->getSourceManager().getMainFileID(),
                          0, SourceLocation());
    }
    assert(PP.isIncrementalProcessingEnabled() && "Not in incremental mode!?");
    PP.enableIncrementalProcessing();

    std::ostringstream source_name;
    source_name << "input_line_" << (m_MemoryBuffers.size() + 1);

    // Create an uninitialized memory buffer, copy code in and append "\n"
    size_t InputSize = input.size(); // don't include trailing 0
    // MemBuffer size should *not* include terminating zero
    llvm::MemoryBuffer* MB
      = llvm::MemoryBuffer::getNewUninitMemBuffer(InputSize + 1,
                                                  source_name.str());
    char* MBStart = const_cast<char*>(MB->getBufferStart());
    memcpy(MBStart, input.data(), InputSize);
    memcpy(MBStart + InputSize, "\n", 2);

    m_MemoryBuffers.push_back(MB);
    SourceManager& SM = getCI()->getSourceManager();

    // Create SourceLocation, which will allow clang to order the overload
    // candidates for example
    SourceLocation NewLoc = SM.getLocForStartOfFile(m_VirtualFileID);
    NewLoc = NewLoc.getLocWithOffset(m_MemoryBuffers.size() + 1);

    // Create FileID for the current buffer
    FileID FID = SM.createFileIDForMemBuffer(m_MemoryBuffers.back(),
                                             SrcMgr::C_User,
                                             /*LoadedID*/0,
                                             /*LoadedOffset*/0, NewLoc);

    PP.EnterSourceFile(FID, /*DirLookup*/0, NewLoc);

    Parser::DeclGroupPtrTy ADecl;

    while (!m_Parser->ParseTopLevelDecl(ADecl)) {
      // If we got a null return and something *was* parsed, ignore it.  This
      // is due to a top-level semicolon, an action override, or a parse error
      // skipping something.
      if (ADecl)
        m_Consumer->HandleTopLevelDecl(ADecl.getAsVal<DeclGroupRef>());
    };

    // Process any TopLevelDecls generated by #pragma weak.
    for (llvm::SmallVector<Decl*,2>::iterator I = S.WeakTopLevelDecls().begin(),
           E = S.WeakTopLevelDecls().end(); I != E; ++I) {
      m_Consumer->HandleTopLevelDecl(DeclGroupRef(*I));
    }

    DiagnosticsEngine& Diag = S.getDiagnostics();
    if (Diag.hasErrorOccurred())
      return IncrementalParser::kFailed;
    else if (Diag.getNumWarnings())
      return IncrementalParser::kSuccessWithWarnings;

    return IncrementalParser::kSuccess;
  }

  void IncrementalParser::unloadTransaction(Transaction* T) {
    if (!T)
      T = getLastTransaction();

    assert(T->getState() == Transaction::kCommitted && 
           "Unloading not commited transaction?");
    assert(T->getModule() && 
           "Trying to uncodegen transaction taken in syntax only mode. ");

    ASTNodeEraser NodeEraser(&getCI()->getSema());
    NodeEraser.RevertTransaction(T);

    InterpreterCallbacks* callbacks = m_Interpreter->getCallbacks();
    if (callbacks)
      callbacks->TransactionUnloaded(*T);
  }

} // namespace cling
