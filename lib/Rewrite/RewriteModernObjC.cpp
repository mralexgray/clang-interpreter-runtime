//===--- RewriteObjC.cpp - Playground for the code rewriter ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Hacks and fun related to the code rewriter.
//
//===----------------------------------------------------------------------===//

#include "clang/Rewrite/ASTConsumers.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ParentMap.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang;
using llvm::utostr;

namespace {
  class RewriteModernObjC : public ASTConsumer {
  protected:
    
    enum {
      BLOCK_FIELD_IS_OBJECT   =  3,  /* id, NSObject, __attribute__((NSObject)),
                                        block, ... */
      BLOCK_FIELD_IS_BLOCK    =  7,  /* a block variable */
      BLOCK_FIELD_IS_BYREF    =  8,  /* the on stack structure holding the 
                                        __block variable */
      BLOCK_FIELD_IS_WEAK     = 16,  /* declared __weak, only used in byref copy
                                        helpers */
      BLOCK_BYREF_CALLER      = 128, /* called from __block (byref) copy/dispose
                                        support routines */
      BLOCK_BYREF_CURRENT_MAX = 256
    };
    
    enum {
      BLOCK_NEEDS_FREE =        (1 << 24),
      BLOCK_HAS_COPY_DISPOSE =  (1 << 25),
      BLOCK_HAS_CXX_OBJ =       (1 << 26),
      BLOCK_IS_GC =             (1 << 27),
      BLOCK_IS_GLOBAL =         (1 << 28),
      BLOCK_HAS_DESCRIPTOR =    (1 << 29)
    };
    static const int OBJC_ABI_VERSION = 7;
    
    Rewriter Rewrite;
    DiagnosticsEngine &Diags;
    const LangOptions &LangOpts;
    ASTContext *Context;
    SourceManager *SM;
    TranslationUnitDecl *TUDecl;
    FileID MainFileID;
    const char *MainFileStart, *MainFileEnd;
    Stmt *CurrentBody;
    ParentMap *PropParentMap; // created lazily.
    std::string InFileName;
    raw_ostream* OutFile;
    std::string Preamble;
    
    TypeDecl *ProtocolTypeDecl;
    VarDecl *GlobalVarDecl;
    unsigned RewriteFailedDiag;
    // ObjC string constant support.
    unsigned NumObjCStringLiterals;
    VarDecl *ConstantStringClassReference;
    RecordDecl *NSStringRecord;

    // ObjC foreach break/continue generation support.
    int BcLabelCount;
    
    unsigned TryFinallyContainsReturnDiag;
    // Needed for super.
    ObjCMethodDecl *CurMethodDef;
    RecordDecl *SuperStructDecl;
    RecordDecl *ConstantStringDecl;
    
    FunctionDecl *MsgSendFunctionDecl;
    FunctionDecl *MsgSendSuperFunctionDecl;
    FunctionDecl *MsgSendStretFunctionDecl;
    FunctionDecl *MsgSendSuperStretFunctionDecl;
    FunctionDecl *MsgSendFpretFunctionDecl;
    FunctionDecl *GetClassFunctionDecl;
    FunctionDecl *GetMetaClassFunctionDecl;
    FunctionDecl *GetSuperClassFunctionDecl;
    FunctionDecl *SelGetUidFunctionDecl;
    FunctionDecl *CFStringFunctionDecl;
    FunctionDecl *SuperContructorFunctionDecl;
    FunctionDecl *CurFunctionDef;
    FunctionDecl *CurFunctionDeclToDeclareForBlock;

    /* Misc. containers needed for meta-data rewrite. */
    SmallVector<ObjCImplementationDecl *, 8> ClassImplementation;
    SmallVector<ObjCCategoryImplDecl *, 8> CategoryImplementation;
    llvm::SmallPtrSet<ObjCInterfaceDecl*, 8> ObjCSynthesizedStructs;
    llvm::SmallPtrSet<ObjCProtocolDecl*, 8> ObjCSynthesizedProtocols;
    llvm::SmallPtrSet<ObjCInterfaceDecl*, 8> ObjCWrittenInterfaces;
    llvm::SmallPtrSet<TagDecl*, 8> TagsDefinedInIvarDecls;
    SmallVector<ObjCInterfaceDecl*, 32> ObjCInterfacesSeen;
    SmallVector<Stmt *, 32> Stmts;
    SmallVector<int, 8> ObjCBcLabelNo;
    // Remember all the @protocol(<expr>) expressions.
    llvm::SmallPtrSet<ObjCProtocolDecl *, 32> ProtocolExprDecls;
    
    llvm::DenseSet<uint64_t> CopyDestroyCache;

    // Block expressions.
    SmallVector<BlockExpr *, 32> Blocks;
    SmallVector<int, 32> InnerDeclRefsCount;
    SmallVector<DeclRefExpr *, 32> InnerDeclRefs;
    
    SmallVector<DeclRefExpr *, 32> BlockDeclRefs;

    // Block related declarations.
    SmallVector<ValueDecl *, 8> BlockByCopyDecls;
    llvm::SmallPtrSet<ValueDecl *, 8> BlockByCopyDeclsPtrSet;
    SmallVector<ValueDecl *, 8> BlockByRefDecls;
    llvm::SmallPtrSet<ValueDecl *, 8> BlockByRefDeclsPtrSet;
    llvm::DenseMap<ValueDecl *, unsigned> BlockByRefDeclNo;
    llvm::SmallPtrSet<ValueDecl *, 8> ImportedBlockDecls;
    llvm::SmallPtrSet<VarDecl *, 8> ImportedLocalExternalDecls;
    
    llvm::DenseMap<BlockExpr *, std::string> RewrittenBlockExprs;
    llvm::DenseMap<ObjCInterfaceDecl *, 
                    llvm::SmallPtrSet<ObjCIvarDecl *, 8> > ReferencedIvars;
    
    // This maps an original source AST to it's rewritten form. This allows
    // us to avoid rewriting the same node twice (which is very uncommon).
    // This is needed to support some of the exotic property rewriting.
    llvm::DenseMap<Stmt *, Stmt *> ReplacedNodes;

    // Needed for header files being rewritten
    bool IsHeader;
    bool SilenceRewriteMacroWarning;
    bool objc_impl_method;
    
    bool DisableReplaceStmt;
    class DisableReplaceStmtScope {
      RewriteModernObjC &R;
      bool SavedValue;
    
    public:
      DisableReplaceStmtScope(RewriteModernObjC &R)
        : R(R), SavedValue(R.DisableReplaceStmt) {
        R.DisableReplaceStmt = true;
      }
      ~DisableReplaceStmtScope() {
        R.DisableReplaceStmt = SavedValue;
      }
    };
    void InitializeCommon(ASTContext &context);

  public:
    llvm::DenseMap<ObjCMethodDecl*, std::string> MethodInternalNames;
    // Top Level Driver code.
    virtual bool HandleTopLevelDecl(DeclGroupRef D) {
      for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
        if (ObjCInterfaceDecl *Class = dyn_cast<ObjCInterfaceDecl>(*I)) {
          if (!Class->isThisDeclarationADefinition()) {
            RewriteForwardClassDecl(D);
            break;
          } else {
            // Keep track of all interface declarations seen.
            ObjCInterfacesSeen.push_back(Class);
            break;
          }
        }

        if (ObjCProtocolDecl *Proto = dyn_cast<ObjCProtocolDecl>(*I)) {
          if (!Proto->isThisDeclarationADefinition()) {
            RewriteForwardProtocolDecl(D);
            break;
          }
        }

        HandleTopLevelSingleDecl(*I);
      }
      return true;
    }
    void HandleTopLevelSingleDecl(Decl *D);
    void HandleDeclInMainFile(Decl *D);
    RewriteModernObjC(std::string inFile, raw_ostream *OS,
                DiagnosticsEngine &D, const LangOptions &LOpts,
                bool silenceMacroWarn);
    
    ~RewriteModernObjC() {}
    
    virtual void HandleTranslationUnit(ASTContext &C);

    void ReplaceStmt(Stmt *Old, Stmt *New) {
      Stmt *ReplacingStmt = ReplacedNodes[Old];

      if (ReplacingStmt)
        return; // We can't rewrite the same node twice.

      if (DisableReplaceStmt)
        return;

      // If replacement succeeded or warning disabled return with no warning.
      if (!Rewrite.ReplaceStmt(Old, New)) {
        ReplacedNodes[Old] = New;
        return;
      }
      if (SilenceRewriteMacroWarning)
        return;
      Diags.Report(Context->getFullLoc(Old->getLocStart()), RewriteFailedDiag)
                   << Old->getSourceRange();
    }

    void ReplaceStmtWithRange(Stmt *Old, Stmt *New, SourceRange SrcRange) {
      if (DisableReplaceStmt)
        return;

      // Measure the old text.
      int Size = Rewrite.getRangeSize(SrcRange);
      if (Size == -1) {
        Diags.Report(Context->getFullLoc(Old->getLocStart()), RewriteFailedDiag)
                     << Old->getSourceRange();
        return;
      }
      // Get the new text.
      std::string SStr;
      llvm::raw_string_ostream S(SStr);
      New->printPretty(S, *Context, 0, PrintingPolicy(LangOpts));
      const std::string &Str = S.str();

      // If replacement succeeded or warning disabled return with no warning.
      if (!Rewrite.ReplaceText(SrcRange.getBegin(), Size, Str)) {
        ReplacedNodes[Old] = New;
        return;
      }
      if (SilenceRewriteMacroWarning)
        return;
      Diags.Report(Context->getFullLoc(Old->getLocStart()), RewriteFailedDiag)
                   << Old->getSourceRange();
    }

    void InsertText(SourceLocation Loc, StringRef Str,
                    bool InsertAfter = true) {
      // If insertion succeeded or warning disabled return with no warning.
      if (!Rewrite.InsertText(Loc, Str, InsertAfter) ||
          SilenceRewriteMacroWarning)
        return;

      Diags.Report(Context->getFullLoc(Loc), RewriteFailedDiag);
    }

    void ReplaceText(SourceLocation Start, unsigned OrigLength,
                     StringRef Str) {
      // If removal succeeded or warning disabled return with no warning.
      if (!Rewrite.ReplaceText(Start, OrigLength, Str) ||
          SilenceRewriteMacroWarning)
        return;

      Diags.Report(Context->getFullLoc(Start), RewriteFailedDiag);
    }

    // Syntactic Rewriting.
    void RewriteRecordBody(RecordDecl *RD);
    void RewriteInclude();
    void RewriteForwardClassDecl(DeclGroupRef D);
    void RewriteForwardClassDecl(const llvm::SmallVector<Decl*, 8> &DG);
    void RewriteForwardClassEpilogue(ObjCInterfaceDecl *ClassDecl, 
                                     const std::string &typedefString);
    void RewriteImplementations();
    void RewritePropertyImplDecl(ObjCPropertyImplDecl *PID,
                                 ObjCImplementationDecl *IMD,
                                 ObjCCategoryImplDecl *CID);
    void RewriteInterfaceDecl(ObjCInterfaceDecl *Dcl);
    void RewriteImplementationDecl(Decl *Dcl);
    void RewriteObjCMethodDecl(const ObjCInterfaceDecl *IDecl,
                               ObjCMethodDecl *MDecl, std::string &ResultStr);
    void RewriteTypeIntoString(QualType T, std::string &ResultStr,
                               const FunctionType *&FPRetType);
    void RewriteByRefString(std::string &ResultStr, const std::string &Name,
                            ValueDecl *VD, bool def=false);
    void RewriteCategoryDecl(ObjCCategoryDecl *Dcl);
    void RewriteProtocolDecl(ObjCProtocolDecl *Dcl);
    void RewriteForwardProtocolDecl(DeclGroupRef D);
    void RewriteForwardProtocolDecl(const llvm::SmallVector<Decl*, 8> &DG);
    void RewriteMethodDeclaration(ObjCMethodDecl *Method);
    void RewriteProperty(ObjCPropertyDecl *prop);
    void RewriteFunctionDecl(FunctionDecl *FD);
    void RewriteBlockPointerType(std::string& Str, QualType Type);
    void RewriteBlockPointerTypeVariable(std::string& Str, ValueDecl *VD);
    void RewriteBlockLiteralFunctionDecl(FunctionDecl *FD);
    void RewriteObjCQualifiedInterfaceTypes(Decl *Dcl);
    void RewriteTypeOfDecl(VarDecl *VD);
    void RewriteObjCQualifiedInterfaceTypes(Expr *E);
  
    // Expression Rewriting.
    Stmt *RewriteFunctionBodyOrGlobalInitializer(Stmt *S);
    Stmt *RewriteAtEncode(ObjCEncodeExpr *Exp);
    Stmt *RewritePropertyOrImplicitGetter(PseudoObjectExpr *Pseudo);
    Stmt *RewritePropertyOrImplicitSetter(PseudoObjectExpr *Pseudo);
    Stmt *RewriteAtSelector(ObjCSelectorExpr *Exp);
    Stmt *RewriteMessageExpr(ObjCMessageExpr *Exp);
    Stmt *RewriteObjCStringLiteral(ObjCStringLiteral *Exp);
    Stmt *RewriteObjCProtocolExpr(ObjCProtocolExpr *Exp);
    void RewriteTryReturnStmts(Stmt *S);
    void RewriteSyncReturnStmts(Stmt *S, std::string buf);
    Stmt *RewriteObjCTryStmt(ObjCAtTryStmt *S);
    Stmt *RewriteObjCSynchronizedStmt(ObjCAtSynchronizedStmt *S);
    Stmt *RewriteObjCThrowStmt(ObjCAtThrowStmt *S);
    Stmt *RewriteObjCForCollectionStmt(ObjCForCollectionStmt *S,
                                       SourceLocation OrigEnd);
    Stmt *RewriteBreakStmt(BreakStmt *S);
    Stmt *RewriteContinueStmt(ContinueStmt *S);
    void RewriteCastExpr(CStyleCastExpr *CE);
    
    // Block rewriting.
    void RewriteBlocksInFunctionProtoType(QualType funcType, NamedDecl *D);
    
    // Block specific rewrite rules.
    void RewriteBlockPointerDecl(NamedDecl *VD);
    void RewriteByRefVar(VarDecl *VD);
    Stmt *RewriteBlockDeclRefExpr(DeclRefExpr *VD);
    Stmt *RewriteLocalVariableExternalStorage(DeclRefExpr *DRE);
    void RewriteBlockPointerFunctionArgs(FunctionDecl *FD);
    
    void RewriteObjCInternalStruct(ObjCInterfaceDecl *CDecl,
                                      std::string &Result);
    
    void RewriteObjCFieldDecl(FieldDecl *fieldDecl, std::string &Result);
    
    bool RewriteObjCFieldDeclType(QualType &Type, std::string &Result);
    
    void RewriteIvarOffsetSymbols(ObjCInterfaceDecl *CDecl,
                                  std::string &Result);
    
    virtual void Initialize(ASTContext &context);
    
    // Misc. AST transformation routines. Somtimes they end up calling
    // rewriting routines on the new ASTs.
    CallExpr *SynthesizeCallToFunctionDecl(FunctionDecl *FD,
                                           Expr **args, unsigned nargs,
                                           SourceLocation StartLoc=SourceLocation(),
                                           SourceLocation EndLoc=SourceLocation());

    Stmt *SynthMessageExpr(ObjCMessageExpr *Exp,
                           SourceLocation StartLoc=SourceLocation(),
                           SourceLocation EndLoc=SourceLocation());
    
    void SynthCountByEnumWithState(std::string &buf);
    void SynthMsgSendFunctionDecl();
    void SynthMsgSendSuperFunctionDecl();
    void SynthMsgSendStretFunctionDecl();
    void SynthMsgSendFpretFunctionDecl();
    void SynthMsgSendSuperStretFunctionDecl();
    void SynthGetClassFunctionDecl();
    void SynthGetMetaClassFunctionDecl();
    void SynthGetSuperClassFunctionDecl();
    void SynthSelGetUidFunctionDecl();
    void SynthSuperContructorFunctionDecl();
    
    // Rewriting metadata
    template<typename MethodIterator>
    void RewriteObjCMethodsMetaData(MethodIterator MethodBegin,
                                    MethodIterator MethodEnd,
                                    bool IsInstanceMethod,
                                    StringRef prefix,
                                    StringRef ClassName,
                                    std::string &Result);
    void RewriteObjCProtocolMetaData(ObjCProtocolDecl *Protocol,
                                     std::string &Result);
    virtual void RewriteObjCProtocolListMetaData(
                   const ObjCList<ObjCProtocolDecl> &Prots,
                   StringRef prefix, StringRef ClassName, std::string &Result);
    virtual void RewriteObjCClassMetaData(ObjCImplementationDecl *IDecl,
                                          std::string &Result);
    virtual void RewriteMetaDataIntoBuffer(std::string &Result);
    virtual void RewriteObjCCategoryImplDecl(ObjCCategoryImplDecl *CDecl,
                                             std::string &Result);
    
    // Rewriting ivar
    virtual void RewriteIvarOffsetComputation(ObjCIvarDecl *ivar,
                                              std::string &Result);
    virtual Stmt *RewriteObjCIvarRefExpr(ObjCIvarRefExpr *IV);

    
    std::string SynthesizeByrefCopyDestroyHelper(VarDecl *VD, int flag);
    std::string SynthesizeBlockHelperFuncs(BlockExpr *CE, int i,
                                      StringRef funcName, std::string Tag);
    std::string SynthesizeBlockFunc(BlockExpr *CE, int i,
                                      StringRef funcName, std::string Tag);
    std::string SynthesizeBlockImpl(BlockExpr *CE, 
                                    std::string Tag, std::string Desc);
    std::string SynthesizeBlockDescriptor(std::string DescTag, 
                                          std::string ImplTag,
                                          int i, StringRef funcName,
                                          unsigned hasCopy);
    Stmt *SynthesizeBlockCall(CallExpr *Exp, const Expr* BlockExp);
    void SynthesizeBlockLiterals(SourceLocation FunLocStart,
                                 StringRef FunName);
    FunctionDecl *SynthBlockInitFunctionDecl(StringRef name);
    Stmt *SynthBlockInitExpr(BlockExpr *Exp,
            const SmallVector<DeclRefExpr *, 8> &InnerBlockDeclRefs);

    // Misc. helper routines.
    QualType getProtocolType();
    void WarnAboutReturnGotoStmts(Stmt *S);
    void HasReturnStmts(Stmt *S, bool &hasReturns);
    void CheckFunctionPointerDecl(QualType dType, NamedDecl *ND);
    void InsertBlockLiteralsWithinFunction(FunctionDecl *FD);
    void InsertBlockLiteralsWithinMethod(ObjCMethodDecl *MD);

    bool IsDeclStmtInForeachHeader(DeclStmt *DS);
    void CollectBlockDeclRefInfo(BlockExpr *Exp);
    void GetBlockDeclRefExprs(Stmt *S);
    void GetInnerBlockDeclRefExprs(Stmt *S, 
                SmallVector<DeclRefExpr *, 8> &InnerBlockDeclRefs,
                llvm::SmallPtrSet<const DeclContext *, 8> &InnerContexts);

    // We avoid calling Type::isBlockPointerType(), since it operates on the
    // canonical type. We only care if the top-level type is a closure pointer.
    bool isTopLevelBlockPointerType(QualType T) {
      return isa<BlockPointerType>(T);
    }

    /// convertBlockPointerToFunctionPointer - Converts a block-pointer type
    /// to a function pointer type and upon success, returns true; false
    /// otherwise.
    bool convertBlockPointerToFunctionPointer(QualType &T) {
      if (isTopLevelBlockPointerType(T)) {
        const BlockPointerType *BPT = T->getAs<BlockPointerType>();
        T = Context->getPointerType(BPT->getPointeeType());
        return true;
      }
      return false;
    }
    
    bool convertObjCTypeToCStyleType(QualType &T);
    
    bool needToScanForQualifiers(QualType T);
    QualType getSuperStructType();
    QualType getConstantStringStructType();
    QualType convertFunctionTypeOfBlocks(const FunctionType *FT);
    bool BufferContainsPPDirectives(const char *startBuf, const char *endBuf);
    
    void convertToUnqualifiedObjCType(QualType &T) {
      if (T->isObjCQualifiedIdType())
        T = Context->getObjCIdType();
      else if (T->isObjCQualifiedClassType())
        T = Context->getObjCClassType();
      else if (T->isObjCObjectPointerType() &&
               T->getPointeeType()->isObjCQualifiedInterfaceType()) {
        if (const ObjCObjectPointerType * OBJPT =
              T->getAsObjCInterfacePointerType()) {
          const ObjCInterfaceType *IFaceT = OBJPT->getInterfaceType();
          T = QualType(IFaceT, 0);
          T = Context->getPointerType(T);
        }
     }
    }
    
    // FIXME: This predicate seems like it would be useful to add to ASTContext.
    bool isObjCType(QualType T) {
      if (!LangOpts.ObjC1 && !LangOpts.ObjC2)
        return false;

      QualType OCT = Context->getCanonicalType(T).getUnqualifiedType();

      if (OCT == Context->getCanonicalType(Context->getObjCIdType()) ||
          OCT == Context->getCanonicalType(Context->getObjCClassType()))
        return true;

      if (const PointerType *PT = OCT->getAs<PointerType>()) {
        if (isa<ObjCInterfaceType>(PT->getPointeeType()) ||
            PT->getPointeeType()->isObjCQualifiedIdType())
          return true;
      }
      return false;
    }
    bool PointerTypeTakesAnyBlockArguments(QualType QT);
    bool PointerTypeTakesAnyObjCQualifiedType(QualType QT);
    void GetExtentOfArgList(const char *Name, const char *&LParen,
                            const char *&RParen);
    
    void QuoteDoublequotes(std::string &From, std::string &To) {
      for (unsigned i = 0; i < From.length(); i++) {
        if (From[i] == '"')
          To += "\\\"";
        else
          To += From[i];
      }
    }

    QualType getSimpleFunctionType(QualType result,
                                   const QualType *args,
                                   unsigned numArgs,
                                   bool variadic = false) {
      if (result == Context->getObjCInstanceType())
        result =  Context->getObjCIdType();
      FunctionProtoType::ExtProtoInfo fpi;
      fpi.Variadic = variadic;
      return Context->getFunctionType(result, args, numArgs, fpi);
    }

    // Helper function: create a CStyleCastExpr with trivial type source info.
    CStyleCastExpr* NoTypeInfoCStyleCastExpr(ASTContext *Ctx, QualType Ty,
                                             CastKind Kind, Expr *E) {
      TypeSourceInfo *TInfo = Ctx->getTrivialTypeSourceInfo(Ty, SourceLocation());
      return CStyleCastExpr::Create(*Ctx, Ty, VK_RValue, Kind, E, 0, TInfo,
                                    SourceLocation(), SourceLocation());
    }
  };
  
}

void RewriteModernObjC::RewriteBlocksInFunctionProtoType(QualType funcType,
                                                   NamedDecl *D) {
  if (const FunctionProtoType *fproto
      = dyn_cast<FunctionProtoType>(funcType.IgnoreParens())) {
    for (FunctionProtoType::arg_type_iterator I = fproto->arg_type_begin(),
         E = fproto->arg_type_end(); I && (I != E); ++I)
      if (isTopLevelBlockPointerType(*I)) {
        // All the args are checked/rewritten. Don't call twice!
        RewriteBlockPointerDecl(D);
        break;
      }
  }
}

void RewriteModernObjC::CheckFunctionPointerDecl(QualType funcType, NamedDecl *ND) {
  const PointerType *PT = funcType->getAs<PointerType>();
  if (PT && PointerTypeTakesAnyBlockArguments(funcType))
    RewriteBlocksInFunctionProtoType(PT->getPointeeType(), ND);
}

static bool IsHeaderFile(const std::string &Filename) {
  std::string::size_type DotPos = Filename.rfind('.');

  if (DotPos == std::string::npos) {
    // no file extension
    return false;
  }

  std::string Ext = std::string(Filename.begin()+DotPos+1, Filename.end());
  // C header: .h
  // C++ header: .hh or .H;
  return Ext == "h" || Ext == "hh" || Ext == "H";
}

RewriteModernObjC::RewriteModernObjC(std::string inFile, raw_ostream* OS,
                         DiagnosticsEngine &D, const LangOptions &LOpts,
                         bool silenceMacroWarn)
      : Diags(D), LangOpts(LOpts), InFileName(inFile), OutFile(OS),
        SilenceRewriteMacroWarning(silenceMacroWarn) {
  IsHeader = IsHeaderFile(inFile);
  RewriteFailedDiag = Diags.getCustomDiagID(DiagnosticsEngine::Warning,
               "rewriting sub-expression within a macro (may not be correct)");
  TryFinallyContainsReturnDiag = Diags.getCustomDiagID(
               DiagnosticsEngine::Warning,
               "rewriter doesn't support user-specified control flow semantics "
               "for @try/@finally (code may not execute properly)");
}

ASTConsumer *clang::CreateModernObjCRewriter(const std::string& InFile,
                                       raw_ostream* OS,
                                       DiagnosticsEngine &Diags,
                                       const LangOptions &LOpts,
                                       bool SilenceRewriteMacroWarning) {
    return new RewriteModernObjC(InFile, OS, Diags, LOpts, SilenceRewriteMacroWarning);
}

void RewriteModernObjC::InitializeCommon(ASTContext &context) {
  Context = &context;
  SM = &Context->getSourceManager();
  TUDecl = Context->getTranslationUnitDecl();
  MsgSendFunctionDecl = 0;
  MsgSendSuperFunctionDecl = 0;
  MsgSendStretFunctionDecl = 0;
  MsgSendSuperStretFunctionDecl = 0;
  MsgSendFpretFunctionDecl = 0;
  GetClassFunctionDecl = 0;
  GetMetaClassFunctionDecl = 0;
  GetSuperClassFunctionDecl = 0;
  SelGetUidFunctionDecl = 0;
  CFStringFunctionDecl = 0;
  ConstantStringClassReference = 0;
  NSStringRecord = 0;
  CurMethodDef = 0;
  CurFunctionDef = 0;
  CurFunctionDeclToDeclareForBlock = 0;
  GlobalVarDecl = 0;
  SuperStructDecl = 0;
  ProtocolTypeDecl = 0;
  ConstantStringDecl = 0;
  BcLabelCount = 0;
  SuperContructorFunctionDecl = 0;
  NumObjCStringLiterals = 0;
  PropParentMap = 0;
  CurrentBody = 0;
  DisableReplaceStmt = false;
  objc_impl_method = false;

  // Get the ID and start/end of the main file.
  MainFileID = SM->getMainFileID();
  const llvm::MemoryBuffer *MainBuf = SM->getBuffer(MainFileID);
  MainFileStart = MainBuf->getBufferStart();
  MainFileEnd = MainBuf->getBufferEnd();

  Rewrite.setSourceMgr(Context->getSourceManager(), Context->getLangOpts());
}

//===----------------------------------------------------------------------===//
// Top Level Driver Code
//===----------------------------------------------------------------------===//

void RewriteModernObjC::HandleTopLevelSingleDecl(Decl *D) {
  if (Diags.hasErrorOccurred())
    return;

  // Two cases: either the decl could be in the main file, or it could be in a
  // #included file.  If the former, rewrite it now.  If the later, check to see
  // if we rewrote the #include/#import.
  SourceLocation Loc = D->getLocation();
  Loc = SM->getExpansionLoc(Loc);

  // If this is for a builtin, ignore it.
  if (Loc.isInvalid()) return;

  // Look for built-in declarations that we need to refer during the rewrite.
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    RewriteFunctionDecl(FD);
  } else if (VarDecl *FVD = dyn_cast<VarDecl>(D)) {
    // declared in <Foundation/NSString.h>
    if (FVD->getName() == "_NSConstantStringClassReference") {
      ConstantStringClassReference = FVD;
      return;
    }
  } else if (ObjCCategoryDecl *CD = dyn_cast<ObjCCategoryDecl>(D)) {
    RewriteCategoryDecl(CD);
  } else if (ObjCProtocolDecl *PD = dyn_cast<ObjCProtocolDecl>(D)) {
    if (PD->isThisDeclarationADefinition())
      RewriteProtocolDecl(PD);
  } else if (LinkageSpecDecl *LSD = dyn_cast<LinkageSpecDecl>(D)) {
    // Recurse into linkage specifications
    for (DeclContext::decl_iterator DI = LSD->decls_begin(),
                                 DIEnd = LSD->decls_end();
         DI != DIEnd; ) {
      if (ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>((*DI))) {
        if (!IFace->isThisDeclarationADefinition()) {
          SmallVector<Decl *, 8> DG;
          SourceLocation StartLoc = IFace->getLocStart();
          do {
            if (isa<ObjCInterfaceDecl>(*DI) &&
                !cast<ObjCInterfaceDecl>(*DI)->isThisDeclarationADefinition() &&
                StartLoc == (*DI)->getLocStart())
              DG.push_back(*DI);
            else
              break;
            
            ++DI;
          } while (DI != DIEnd);
          RewriteForwardClassDecl(DG);
          continue;
        }
      }

      if (ObjCProtocolDecl *Proto = dyn_cast<ObjCProtocolDecl>((*DI))) {
        if (!Proto->isThisDeclarationADefinition()) {
          SmallVector<Decl *, 8> DG;
          SourceLocation StartLoc = Proto->getLocStart();
          do {
            if (isa<ObjCProtocolDecl>(*DI) &&
                !cast<ObjCProtocolDecl>(*DI)->isThisDeclarationADefinition() &&
                StartLoc == (*DI)->getLocStart())
              DG.push_back(*DI);
            else
              break;
            
            ++DI;
          } while (DI != DIEnd);
          RewriteForwardProtocolDecl(DG);
          continue;
        }
      }
      
      HandleTopLevelSingleDecl(*DI);
      ++DI;
    }
  }
  // If we have a decl in the main file, see if we should rewrite it.
  if (SM->isFromMainFile(Loc))
    return HandleDeclInMainFile(D);
}

//===----------------------------------------------------------------------===//
// Syntactic (non-AST) Rewriting Code
//===----------------------------------------------------------------------===//

void RewriteModernObjC::RewriteInclude() {
  SourceLocation LocStart = SM->getLocForStartOfFile(MainFileID);
  StringRef MainBuf = SM->getBufferData(MainFileID);
  const char *MainBufStart = MainBuf.begin();
  const char *MainBufEnd = MainBuf.end();
  size_t ImportLen = strlen("import");

  // Loop over the whole file, looking for includes.
  for (const char *BufPtr = MainBufStart; BufPtr < MainBufEnd; ++BufPtr) {
    if (*BufPtr == '#') {
      if (++BufPtr == MainBufEnd)
        return;
      while (*BufPtr == ' ' || *BufPtr == '\t')
        if (++BufPtr == MainBufEnd)
          return;
      if (!strncmp(BufPtr, "import", ImportLen)) {
        // replace import with include
        SourceLocation ImportLoc =
          LocStart.getLocWithOffset(BufPtr-MainBufStart);
        ReplaceText(ImportLoc, ImportLen, "include");
        BufPtr += ImportLen;
      }
    }
  }
}

static std::string getIvarAccessString(ObjCIvarDecl *OID) {
  const ObjCInterfaceDecl *ClassDecl = OID->getContainingInterface();
  std::string S;
  S = "((struct ";
  S += ClassDecl->getIdentifier()->getName();
  S += "_IMPL *)self)->";
  S += OID->getName();
  return S;
}

void RewriteModernObjC::RewritePropertyImplDecl(ObjCPropertyImplDecl *PID,
                                          ObjCImplementationDecl *IMD,
                                          ObjCCategoryImplDecl *CID) {
  static bool objcGetPropertyDefined = false;
  static bool objcSetPropertyDefined = false;
  SourceLocation startLoc = PID->getLocStart();
  InsertText(startLoc, "// ");
  const char *startBuf = SM->getCharacterData(startLoc);
  assert((*startBuf == '@') && "bogus @synthesize location");
  const char *semiBuf = strchr(startBuf, ';');
  assert((*semiBuf == ';') && "@synthesize: can't find ';'");
  SourceLocation onePastSemiLoc =
    startLoc.getLocWithOffset(semiBuf-startBuf+1);

  if (PID->getPropertyImplementation() == ObjCPropertyImplDecl::Dynamic)
    return; // FIXME: is this correct?

  // Generate the 'getter' function.
  ObjCPropertyDecl *PD = PID->getPropertyDecl();
  ObjCIvarDecl *OID = PID->getPropertyIvarDecl();

  if (!OID)
    return;
  unsigned Attributes = PD->getPropertyAttributes();
  if (!PD->getGetterMethodDecl()->isDefined()) {
    bool GenGetProperty = !(Attributes & ObjCPropertyDecl::OBJC_PR_nonatomic) &&
                          (Attributes & (ObjCPropertyDecl::OBJC_PR_retain | 
                                         ObjCPropertyDecl::OBJC_PR_copy));
    std::string Getr;
    if (GenGetProperty && !objcGetPropertyDefined) {
      objcGetPropertyDefined = true;
      // FIXME. Is this attribute correct in all cases?
      Getr = "\nextern \"C\" __declspec(dllimport) "
            "id objc_getProperty(id, SEL, long, bool);\n";
    }
    RewriteObjCMethodDecl(OID->getContainingInterface(),  
                          PD->getGetterMethodDecl(), Getr);
    Getr += "{ ";
    // Synthesize an explicit cast to gain access to the ivar.
    // See objc-act.c:objc_synthesize_new_getter() for details.
    if (GenGetProperty) {
      // return objc_getProperty(self, _cmd, offsetof(ClassDecl, OID), 1)
      Getr += "typedef ";
      const FunctionType *FPRetType = 0;
      RewriteTypeIntoString(PD->getGetterMethodDecl()->getResultType(), Getr, 
                            FPRetType);
      Getr += " _TYPE";
      if (FPRetType) {
        Getr += ")"; // close the precedence "scope" for "*".
      
        // Now, emit the argument types (if any).
        if (const FunctionProtoType *FT = dyn_cast<FunctionProtoType>(FPRetType)){
          Getr += "(";
          for (unsigned i = 0, e = FT->getNumArgs(); i != e; ++i) {
            if (i) Getr += ", ";
            std::string ParamStr = FT->getArgType(i).getAsString(
                                                          Context->getPrintingPolicy());
            Getr += ParamStr;
          }
          if (FT->isVariadic()) {
            if (FT->getNumArgs()) Getr += ", ";
            Getr += "...";
          }
          Getr += ")";
        } else
          Getr += "()";
      }
      Getr += ";\n";
      Getr += "return (_TYPE)";
      Getr += "objc_getProperty(self, _cmd, ";
      RewriteIvarOffsetComputation(OID, Getr);
      Getr += ", 1)";
    }
    else
      Getr += "return " + getIvarAccessString(OID);
    Getr += "; }";
    InsertText(onePastSemiLoc, Getr);
  }
  
  if (PD->isReadOnly() || PD->getSetterMethodDecl()->isDefined())
    return;

  // Generate the 'setter' function.
  std::string Setr;
  bool GenSetProperty = Attributes & (ObjCPropertyDecl::OBJC_PR_retain | 
                                      ObjCPropertyDecl::OBJC_PR_copy);
  if (GenSetProperty && !objcSetPropertyDefined) {
    objcSetPropertyDefined = true;
    // FIXME. Is this attribute correct in all cases?
    Setr = "\nextern \"C\" __declspec(dllimport) "
    "void objc_setProperty (id, SEL, long, id, bool, bool);\n";
  }
  
  RewriteObjCMethodDecl(OID->getContainingInterface(), 
                        PD->getSetterMethodDecl(), Setr);
  Setr += "{ ";
  // Synthesize an explicit cast to initialize the ivar.
  // See objc-act.c:objc_synthesize_new_setter() for details.
  if (GenSetProperty) {
    Setr += "objc_setProperty (self, _cmd, ";
    RewriteIvarOffsetComputation(OID, Setr);
    Setr += ", (id)";
    Setr += PD->getName();
    Setr += ", ";
    if (Attributes & ObjCPropertyDecl::OBJC_PR_nonatomic)
      Setr += "0, ";
    else
      Setr += "1, ";
    if (Attributes & ObjCPropertyDecl::OBJC_PR_copy)
      Setr += "1)";
    else
      Setr += "0)";
  }
  else {
    Setr += getIvarAccessString(OID) + " = ";
    Setr += PD->getName();
  }
  Setr += "; }";
  InsertText(onePastSemiLoc, Setr);
}

static void RewriteOneForwardClassDecl(ObjCInterfaceDecl *ForwardDecl,
                                       std::string &typedefString) {
  typedefString += "#ifndef _REWRITER_typedef_";
  typedefString += ForwardDecl->getNameAsString();
  typedefString += "\n";
  typedefString += "#define _REWRITER_typedef_";
  typedefString += ForwardDecl->getNameAsString();
  typedefString += "\n";
  typedefString += "typedef struct objc_object ";
  typedefString += ForwardDecl->getNameAsString();
  typedefString += ";\n#endif\n";
}

void RewriteModernObjC::RewriteForwardClassEpilogue(ObjCInterfaceDecl *ClassDecl,
                                              const std::string &typedefString) {
    SourceLocation startLoc = ClassDecl->getLocStart();
    const char *startBuf = SM->getCharacterData(startLoc);
    const char *semiPtr = strchr(startBuf, ';'); 
    // Replace the @class with typedefs corresponding to the classes.
    ReplaceText(startLoc, semiPtr-startBuf+1, typedefString);  
}

void RewriteModernObjC::RewriteForwardClassDecl(DeclGroupRef D) {
  std::string typedefString;
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    ObjCInterfaceDecl *ForwardDecl = cast<ObjCInterfaceDecl>(*I);
    if (I == D.begin()) {
      // Translate to typedef's that forward reference structs with the same name
      // as the class. As a convenience, we include the original declaration
      // as a comment.
      typedefString += "// @class ";
      typedefString += ForwardDecl->getNameAsString();
      typedefString += ";\n";
    }
    RewriteOneForwardClassDecl(ForwardDecl, typedefString);
  }
  DeclGroupRef::iterator I = D.begin();
  RewriteForwardClassEpilogue(cast<ObjCInterfaceDecl>(*I), typedefString);
}

void RewriteModernObjC::RewriteForwardClassDecl(
                                const llvm::SmallVector<Decl*, 8> &D) {
  std::string typedefString;
  for (unsigned i = 0; i < D.size(); i++) {
    ObjCInterfaceDecl *ForwardDecl = cast<ObjCInterfaceDecl>(D[i]);
    if (i == 0) {
      typedefString += "// @class ";
      typedefString += ForwardDecl->getNameAsString();
      typedefString += ";\n";
    }
    RewriteOneForwardClassDecl(ForwardDecl, typedefString);
  }
  RewriteForwardClassEpilogue(cast<ObjCInterfaceDecl>(D[0]), typedefString);
}

void RewriteModernObjC::RewriteMethodDeclaration(ObjCMethodDecl *Method) {
  // When method is a synthesized one, such as a getter/setter there is
  // nothing to rewrite.
  if (Method->isImplicit())
    return;
  SourceLocation LocStart = Method->getLocStart();
  SourceLocation LocEnd = Method->getLocEnd();

  if (SM->getExpansionLineNumber(LocEnd) >
      SM->getExpansionLineNumber(LocStart)) {
    InsertText(LocStart, "#if 0\n");
    ReplaceText(LocEnd, 1, ";\n#endif\n");
  } else {
    InsertText(LocStart, "// ");
  }
}

void RewriteModernObjC::RewriteProperty(ObjCPropertyDecl *prop) {
  SourceLocation Loc = prop->getAtLoc();

  ReplaceText(Loc, 0, "// ");
  // FIXME: handle properties that are declared across multiple lines.
}

void RewriteModernObjC::RewriteCategoryDecl(ObjCCategoryDecl *CatDecl) {
  SourceLocation LocStart = CatDecl->getLocStart();

  // FIXME: handle category headers that are declared across multiple lines.
  ReplaceText(LocStart, 0, "// ");
  if (CatDecl->getIvarLBraceLoc().isValid())
    InsertText(CatDecl->getIvarLBraceLoc(), "// ");
  for (ObjCCategoryDecl::ivar_iterator
       I = CatDecl->ivar_begin(), E = CatDecl->ivar_end(); I != E; ++I) {
    ObjCIvarDecl *Ivar = (*I);
    SourceLocation LocStart = Ivar->getLocStart();
    ReplaceText(LocStart, 0, "// ");
  } 
  if (CatDecl->getIvarRBraceLoc().isValid())
    InsertText(CatDecl->getIvarRBraceLoc(), "// ");
  
  for (ObjCCategoryDecl::prop_iterator I = CatDecl->prop_begin(),
       E = CatDecl->prop_end(); I != E; ++I)
    RewriteProperty(*I);
  
  for (ObjCCategoryDecl::instmeth_iterator
         I = CatDecl->instmeth_begin(), E = CatDecl->instmeth_end();
       I != E; ++I)
    RewriteMethodDeclaration(*I);
  for (ObjCCategoryDecl::classmeth_iterator
         I = CatDecl->classmeth_begin(), E = CatDecl->classmeth_end();
       I != E; ++I)
    RewriteMethodDeclaration(*I);

  // Lastly, comment out the @end.
  ReplaceText(CatDecl->getAtEndRange().getBegin(), 
              strlen("@end"), "/* @end */");
}

void RewriteModernObjC::RewriteProtocolDecl(ObjCProtocolDecl *PDecl) {
  SourceLocation LocStart = PDecl->getLocStart();
  assert(PDecl->isThisDeclarationADefinition());
  
  // FIXME: handle protocol headers that are declared across multiple lines.
  ReplaceText(LocStart, 0, "// ");

  for (ObjCProtocolDecl::instmeth_iterator
         I = PDecl->instmeth_begin(), E = PDecl->instmeth_end();
       I != E; ++I)
    RewriteMethodDeclaration(*I);
  for (ObjCProtocolDecl::classmeth_iterator
         I = PDecl->classmeth_begin(), E = PDecl->classmeth_end();
       I != E; ++I)
    RewriteMethodDeclaration(*I);

  for (ObjCInterfaceDecl::prop_iterator I = PDecl->prop_begin(),
       E = PDecl->prop_end(); I != E; ++I)
    RewriteProperty(*I);
  
  // Lastly, comment out the @end.
  SourceLocation LocEnd = PDecl->getAtEndRange().getBegin();
  ReplaceText(LocEnd, strlen("@end"), "/* @end */");

  // Must comment out @optional/@required
  const char *startBuf = SM->getCharacterData(LocStart);
  const char *endBuf = SM->getCharacterData(LocEnd);
  for (const char *p = startBuf; p < endBuf; p++) {
    if (*p == '@' && !strncmp(p+1, "optional", strlen("optional"))) {
      SourceLocation OptionalLoc = LocStart.getLocWithOffset(p-startBuf);
      ReplaceText(OptionalLoc, strlen("@optional"), "/* @optional */");

    }
    else if (*p == '@' && !strncmp(p+1, "required", strlen("required"))) {
      SourceLocation OptionalLoc = LocStart.getLocWithOffset(p-startBuf);
      ReplaceText(OptionalLoc, strlen("@required"), "/* @required */");

    }
  }
}

void RewriteModernObjC::RewriteForwardProtocolDecl(DeclGroupRef D) {
  SourceLocation LocStart = (*D.begin())->getLocStart();
  if (LocStart.isInvalid())
    llvm_unreachable("Invalid SourceLocation");
  // FIXME: handle forward protocol that are declared across multiple lines.
  ReplaceText(LocStart, 0, "// ");
}

void 
RewriteModernObjC::RewriteForwardProtocolDecl(const llvm::SmallVector<Decl*, 8> &DG) {
  SourceLocation LocStart = DG[0]->getLocStart();
  if (LocStart.isInvalid())
    llvm_unreachable("Invalid SourceLocation");
  // FIXME: handle forward protocol that are declared across multiple lines.
  ReplaceText(LocStart, 0, "// ");
}

void RewriteModernObjC::RewriteTypeIntoString(QualType T, std::string &ResultStr,
                                        const FunctionType *&FPRetType) {
  if (T->isObjCQualifiedIdType())
    ResultStr += "id";
  else if (T->isFunctionPointerType() ||
           T->isBlockPointerType()) {
    // needs special handling, since pointer-to-functions have special
    // syntax (where a decaration models use).
    QualType retType = T;
    QualType PointeeTy;
    if (const PointerType* PT = retType->getAs<PointerType>())
      PointeeTy = PT->getPointeeType();
    else if (const BlockPointerType *BPT = retType->getAs<BlockPointerType>())
      PointeeTy = BPT->getPointeeType();
    if ((FPRetType = PointeeTy->getAs<FunctionType>())) {
      ResultStr += FPRetType->getResultType().getAsString(
        Context->getPrintingPolicy());
      ResultStr += "(*";
    }
  } else
    ResultStr += T.getAsString(Context->getPrintingPolicy());
}

void RewriteModernObjC::RewriteObjCMethodDecl(const ObjCInterfaceDecl *IDecl,
                                        ObjCMethodDecl *OMD,
                                        std::string &ResultStr) {
  //fprintf(stderr,"In RewriteObjCMethodDecl\n");
  const FunctionType *FPRetType = 0;
  ResultStr += "\nstatic ";
  RewriteTypeIntoString(OMD->getResultType(), ResultStr, FPRetType);
  ResultStr += " ";

  // Unique method name
  std::string NameStr;

  if (OMD->isInstanceMethod())
    NameStr += "_I_";
  else
    NameStr += "_C_";

  NameStr += IDecl->getNameAsString();
  NameStr += "_";

  if (ObjCCategoryImplDecl *CID =
      dyn_cast<ObjCCategoryImplDecl>(OMD->getDeclContext())) {
    NameStr += CID->getNameAsString();
    NameStr += "_";
  }
  // Append selector names, replacing ':' with '_'
  {
    std::string selString = OMD->getSelector().getAsString();
    int len = selString.size();
    for (int i = 0; i < len; i++)
      if (selString[i] == ':')
        selString[i] = '_';
    NameStr += selString;
  }
  // Remember this name for metadata emission
  MethodInternalNames[OMD] = NameStr;
  ResultStr += NameStr;

  // Rewrite arguments
  ResultStr += "(";

  // invisible arguments
  if (OMD->isInstanceMethod()) {
    QualType selfTy = Context->getObjCInterfaceType(IDecl);
    selfTy = Context->getPointerType(selfTy);
    if (!LangOpts.MicrosoftExt) {
      if (ObjCSynthesizedStructs.count(const_cast<ObjCInterfaceDecl*>(IDecl)))
        ResultStr += "struct ";
    }
    // When rewriting for Microsoft, explicitly omit the structure name.
    ResultStr += IDecl->getNameAsString();
    ResultStr += " *";
  }
  else
    ResultStr += Context->getObjCClassType().getAsString(
      Context->getPrintingPolicy());

  ResultStr += " self, ";
  ResultStr += Context->getObjCSelType().getAsString(Context->getPrintingPolicy());
  ResultStr += " _cmd";

  // Method arguments.
  for (ObjCMethodDecl::param_iterator PI = OMD->param_begin(),
       E = OMD->param_end(); PI != E; ++PI) {
    ParmVarDecl *PDecl = *PI;
    ResultStr += ", ";
    if (PDecl->getType()->isObjCQualifiedIdType()) {
      ResultStr += "id ";
      ResultStr += PDecl->getNameAsString();
    } else {
      std::string Name = PDecl->getNameAsString();
      QualType QT = PDecl->getType();
      // Make sure we convert "t (^)(...)" to "t (*)(...)".
      if (convertBlockPointerToFunctionPointer(QT))
        QT.getAsStringInternal(Name, Context->getPrintingPolicy());
      else
        PDecl->getType().getAsStringInternal(Name, Context->getPrintingPolicy());
      ResultStr += Name;
    }
  }
  if (OMD->isVariadic())
    ResultStr += ", ...";
  ResultStr += ") ";

  if (FPRetType) {
    ResultStr += ")"; // close the precedence "scope" for "*".

    // Now, emit the argument types (if any).
    if (const FunctionProtoType *FT = dyn_cast<FunctionProtoType>(FPRetType)) {
      ResultStr += "(";
      for (unsigned i = 0, e = FT->getNumArgs(); i != e; ++i) {
        if (i) ResultStr += ", ";
        std::string ParamStr = FT->getArgType(i).getAsString(
          Context->getPrintingPolicy());
        ResultStr += ParamStr;
      }
      if (FT->isVariadic()) {
        if (FT->getNumArgs()) ResultStr += ", ";
        ResultStr += "...";
      }
      ResultStr += ")";
    } else {
      ResultStr += "()";
    }
  }
}
void RewriteModernObjC::RewriteImplementationDecl(Decl *OID) {
  ObjCImplementationDecl *IMD = dyn_cast<ObjCImplementationDecl>(OID);
  ObjCCategoryImplDecl *CID = dyn_cast<ObjCCategoryImplDecl>(OID);

  if (IMD) {
    InsertText(IMD->getLocStart(), "// ");
    if (IMD->getIvarLBraceLoc().isValid())
      InsertText(IMD->getIvarLBraceLoc(), "// ");
    for (ObjCImplementationDecl::ivar_iterator
         I = IMD->ivar_begin(), E = IMD->ivar_end(); I != E; ++I) {
      ObjCIvarDecl *Ivar = (*I);
      SourceLocation LocStart = Ivar->getLocStart();
      ReplaceText(LocStart, 0, "// ");
    }
    if (IMD->getIvarRBraceLoc().isValid())
      InsertText(IMD->getIvarRBraceLoc(), "// ");
  }
  else
    InsertText(CID->getLocStart(), "// ");

  for (ObjCCategoryImplDecl::instmeth_iterator
       I = IMD ? IMD->instmeth_begin() : CID->instmeth_begin(),
       E = IMD ? IMD->instmeth_end() : CID->instmeth_end();
       I != E; ++I) {
    std::string ResultStr;
    ObjCMethodDecl *OMD = *I;
    RewriteObjCMethodDecl(OMD->getClassInterface(), OMD, ResultStr);
    SourceLocation LocStart = OMD->getLocStart();
    SourceLocation LocEnd = OMD->getCompoundBody()->getLocStart();

    const char *startBuf = SM->getCharacterData(LocStart);
    const char *endBuf = SM->getCharacterData(LocEnd);
    ReplaceText(LocStart, endBuf-startBuf, ResultStr);
  }

  for (ObjCCategoryImplDecl::classmeth_iterator
       I = IMD ? IMD->classmeth_begin() : CID->classmeth_begin(),
       E = IMD ? IMD->classmeth_end() : CID->classmeth_end();
       I != E; ++I) {
    std::string ResultStr;
    ObjCMethodDecl *OMD = *I;
    RewriteObjCMethodDecl(OMD->getClassInterface(), OMD, ResultStr);
    SourceLocation LocStart = OMD->getLocStart();
    SourceLocation LocEnd = OMD->getCompoundBody()->getLocStart();

    const char *startBuf = SM->getCharacterData(LocStart);
    const char *endBuf = SM->getCharacterData(LocEnd);
    ReplaceText(LocStart, endBuf-startBuf, ResultStr);
  }
  for (ObjCCategoryImplDecl::propimpl_iterator
       I = IMD ? IMD->propimpl_begin() : CID->propimpl_begin(),
       E = IMD ? IMD->propimpl_end() : CID->propimpl_end();
       I != E; ++I) {
    RewritePropertyImplDecl(*I, IMD, CID);
  }

  InsertText(IMD ? IMD->getLocEnd() : CID->getLocEnd(), "// ");
}

void RewriteModernObjC::RewriteInterfaceDecl(ObjCInterfaceDecl *ClassDecl) {
  // Do not synthesize more than once.
  if (ObjCSynthesizedStructs.count(ClassDecl))
    return;
  // Make sure super class's are written before current class is written.
  ObjCInterfaceDecl *SuperClass = ClassDecl->getSuperClass();
  while (SuperClass) {
    RewriteInterfaceDecl(SuperClass);
    SuperClass = SuperClass->getSuperClass();
  }
  std::string ResultStr;
  if (!ObjCWrittenInterfaces.count(ClassDecl->getCanonicalDecl())) {
    // we haven't seen a forward decl - generate a typedef.
    ResultStr = "#ifndef _REWRITER_typedef_";
    ResultStr += ClassDecl->getNameAsString();
    ResultStr += "\n";
    ResultStr += "#define _REWRITER_typedef_";
    ResultStr += ClassDecl->getNameAsString();
    ResultStr += "\n";
    ResultStr += "typedef struct objc_object ";
    ResultStr += ClassDecl->getNameAsString();
    ResultStr += ";\n#endif\n";
    
    RewriteIvarOffsetSymbols(ClassDecl, ResultStr);
    
    RewriteObjCInternalStruct(ClassDecl, ResultStr);
    // Mark this typedef as having been written into its c++ equivalent.
    ObjCWrittenInterfaces.insert(ClassDecl->getCanonicalDecl());
  
    for (ObjCInterfaceDecl::prop_iterator I = ClassDecl->prop_begin(),
         E = ClassDecl->prop_end(); I != E; ++I)
      RewriteProperty(*I);
    for (ObjCInterfaceDecl::instmeth_iterator
         I = ClassDecl->instmeth_begin(), E = ClassDecl->instmeth_end();
         I != E; ++I)
      RewriteMethodDeclaration(*I);
    for (ObjCInterfaceDecl::classmeth_iterator
         I = ClassDecl->classmeth_begin(), E = ClassDecl->classmeth_end();
         I != E; ++I)
      RewriteMethodDeclaration(*I);

    // Lastly, comment out the @end.
    ReplaceText(ClassDecl->getAtEndRange().getBegin(), strlen("@end"), 
                "/* @end */");
  }
}

Stmt *RewriteModernObjC::RewritePropertyOrImplicitSetter(PseudoObjectExpr *PseudoOp) {
  SourceRange OldRange = PseudoOp->getSourceRange();

  // We just magically know some things about the structure of this
  // expression.
  ObjCMessageExpr *OldMsg =
    cast<ObjCMessageExpr>(PseudoOp->getSemanticExpr(
                            PseudoOp->getNumSemanticExprs() - 1));

  // Because the rewriter doesn't allow us to rewrite rewritten code,
  // we need to suppress rewriting the sub-statements.
  Expr *Base, *RHS;
  {
    DisableReplaceStmtScope S(*this);

    // Rebuild the base expression if we have one.
    Base = 0;
    if (OldMsg->getReceiverKind() == ObjCMessageExpr::Instance) {
      Base = OldMsg->getInstanceReceiver();
      Base = cast<OpaqueValueExpr>(Base)->getSourceExpr();
      Base = cast<Expr>(RewriteFunctionBodyOrGlobalInitializer(Base));
    }

    // Rebuild the RHS.
    RHS = cast<BinaryOperator>(PseudoOp->getSyntacticForm())->getRHS();
    RHS = cast<OpaqueValueExpr>(RHS)->getSourceExpr();
    RHS = cast<Expr>(RewriteFunctionBodyOrGlobalInitializer(RHS));
  }

  // TODO: avoid this copy.
  SmallVector<SourceLocation, 1> SelLocs;
  OldMsg->getSelectorLocs(SelLocs);

  ObjCMessageExpr *NewMsg = 0;
  switch (OldMsg->getReceiverKind()) {
  case ObjCMessageExpr::Class:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     OldMsg->getClassReceiverTypeInfo(),
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     RHS,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;

  case ObjCMessageExpr::Instance:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     Base,
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     RHS,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;

  case ObjCMessageExpr::SuperClass:
  case ObjCMessageExpr::SuperInstance:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     OldMsg->getSuperLoc(),
                 OldMsg->getReceiverKind() == ObjCMessageExpr::SuperInstance,
                                     OldMsg->getSuperType(),
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     RHS,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;
  }

  Stmt *Replacement = SynthMessageExpr(NewMsg);
  ReplaceStmtWithRange(PseudoOp, Replacement, OldRange);
  return Replacement;
}

Stmt *RewriteModernObjC::RewritePropertyOrImplicitGetter(PseudoObjectExpr *PseudoOp) {
  SourceRange OldRange = PseudoOp->getSourceRange();

  // We just magically know some things about the structure of this
  // expression.
  ObjCMessageExpr *OldMsg =
    cast<ObjCMessageExpr>(PseudoOp->getResultExpr()->IgnoreImplicit());

  // Because the rewriter doesn't allow us to rewrite rewritten code,
  // we need to suppress rewriting the sub-statements.
  Expr *Base = 0;
  {
    DisableReplaceStmtScope S(*this);

    // Rebuild the base expression if we have one.
    if (OldMsg->getReceiverKind() == ObjCMessageExpr::Instance) {
      Base = OldMsg->getInstanceReceiver();
      Base = cast<OpaqueValueExpr>(Base)->getSourceExpr();
      Base = cast<Expr>(RewriteFunctionBodyOrGlobalInitializer(Base));
    }
  }

  // Intentionally empty.
  SmallVector<SourceLocation, 1> SelLocs;
  SmallVector<Expr*, 1> Args;

  ObjCMessageExpr *NewMsg = 0;
  switch (OldMsg->getReceiverKind()) {
  case ObjCMessageExpr::Class:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     OldMsg->getClassReceiverTypeInfo(),
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     Args,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;

  case ObjCMessageExpr::Instance:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     Base,
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     Args,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;

  case ObjCMessageExpr::SuperClass:
  case ObjCMessageExpr::SuperInstance:
    NewMsg = ObjCMessageExpr::Create(*Context, OldMsg->getType(),
                                     OldMsg->getValueKind(),
                                     OldMsg->getLeftLoc(),
                                     OldMsg->getSuperLoc(),
                 OldMsg->getReceiverKind() == ObjCMessageExpr::SuperInstance,
                                     OldMsg->getSuperType(),
                                     OldMsg->getSelector(),
                                     SelLocs,
                                     OldMsg->getMethodDecl(),
                                     Args,
                                     OldMsg->getRightLoc(),
                                     OldMsg->isImplicit());
    break;
  }

  Stmt *Replacement = SynthMessageExpr(NewMsg);
  ReplaceStmtWithRange(PseudoOp, Replacement, OldRange);
  return Replacement;
}

/// SynthCountByEnumWithState - To print:
/// ((unsigned int (*)
///  (id, SEL, struct __objcFastEnumerationState *, id *, unsigned int))
///  (void *)objc_msgSend)((id)l_collection,
///                        sel_registerName(
///                          "countByEnumeratingWithState:objects:count:"),
///                        &enumState,
///                        (id *)__rw_items, (unsigned int)16)
///
void RewriteModernObjC::SynthCountByEnumWithState(std::string &buf) {
  buf += "((unsigned int (*) (id, SEL, struct __objcFastEnumerationState *, "
  "id *, unsigned int))(void *)objc_msgSend)";
  buf += "\n\t\t";
  buf += "((id)l_collection,\n\t\t";
  buf += "sel_registerName(\"countByEnumeratingWithState:objects:count:\"),";
  buf += "\n\t\t";
  buf += "&enumState, "
         "(id *)__rw_items, (unsigned int)16)";
}

/// RewriteBreakStmt - Rewrite for a break-stmt inside an ObjC2's foreach
/// statement to exit to its outer synthesized loop.
///
Stmt *RewriteModernObjC::RewriteBreakStmt(BreakStmt *S) {
  if (Stmts.empty() || !isa<ObjCForCollectionStmt>(Stmts.back()))
    return S;
  // replace break with goto __break_label
  std::string buf;

  SourceLocation startLoc = S->getLocStart();
  buf = "goto __break_label_";
  buf += utostr(ObjCBcLabelNo.back());
  ReplaceText(startLoc, strlen("break"), buf);

  return 0;
}

/// RewriteContinueStmt - Rewrite for a continue-stmt inside an ObjC2's foreach
/// statement to continue with its inner synthesized loop.
///
Stmt *RewriteModernObjC::RewriteContinueStmt(ContinueStmt *S) {
  if (Stmts.empty() || !isa<ObjCForCollectionStmt>(Stmts.back()))
    return S;
  // replace continue with goto __continue_label
  std::string buf;

  SourceLocation startLoc = S->getLocStart();
  buf = "goto __continue_label_";
  buf += utostr(ObjCBcLabelNo.back());
  ReplaceText(startLoc, strlen("continue"), buf);

  return 0;
}

/// RewriteObjCForCollectionStmt - Rewriter for ObjC2's foreach statement.
///  It rewrites:
/// for ( type elem in collection) { stmts; }

/// Into:
/// {
///   type elem;
///   struct __objcFastEnumerationState enumState = { 0 };
///   id __rw_items[16];
///   id l_collection = (id)collection;
///   unsigned long limit = [l_collection countByEnumeratingWithState:&enumState
///                                       objects:__rw_items count:16];
/// if (limit) {
///   unsigned long startMutations = *enumState.mutationsPtr;
///   do {
///        unsigned long counter = 0;
///        do {
///             if (startMutations != *enumState.mutationsPtr)
///               objc_enumerationMutation(l_collection);
///             elem = (type)enumState.itemsPtr[counter++];
///             stmts;
///             __continue_label: ;
///        } while (counter < limit);
///   } while (limit = [l_collection countByEnumeratingWithState:&enumState
///                                  objects:__rw_items count:16]);
///   elem = nil;
///   __break_label: ;
///  }
///  else
///       elem = nil;
///  }
///
Stmt *RewriteModernObjC::RewriteObjCForCollectionStmt(ObjCForCollectionStmt *S,
                                                SourceLocation OrigEnd) {
  assert(!Stmts.empty() && "ObjCForCollectionStmt - Statement stack empty");
  assert(isa<ObjCForCollectionStmt>(Stmts.back()) &&
         "ObjCForCollectionStmt Statement stack mismatch");
  assert(!ObjCBcLabelNo.empty() &&
         "ObjCForCollectionStmt - Label No stack empty");

  SourceLocation startLoc = S->getLocStart();
  const char *startBuf = SM->getCharacterData(startLoc);
  StringRef elementName;
  std::string elementTypeAsString;
  std::string buf;
  buf = "\n{\n\t";
  if (DeclStmt *DS = dyn_cast<DeclStmt>(S->getElement())) {
    // type elem;
    NamedDecl* D = cast<NamedDecl>(DS->getSingleDecl());
    QualType ElementType = cast<ValueDecl>(D)->getType();
    if (ElementType->isObjCQualifiedIdType() ||
        ElementType->isObjCQualifiedInterfaceType())
      // Simply use 'id' for all qualified types.
      elementTypeAsString = "id";
    else
      elementTypeAsString = ElementType.getAsString(Context->getPrintingPolicy());
    buf += elementTypeAsString;
    buf += " ";
    elementName = D->getName();
    buf += elementName;
    buf += ";\n\t";
  }
  else {
    DeclRefExpr *DR = cast<DeclRefExpr>(S->getElement());
    elementName = DR->getDecl()->getName();
    ValueDecl *VD = cast<ValueDecl>(DR->getDecl());
    if (VD->getType()->isObjCQualifiedIdType() ||
        VD->getType()->isObjCQualifiedInterfaceType())
      // Simply use 'id' for all qualified types.
      elementTypeAsString = "id";
    else
      elementTypeAsString = VD->getType().getAsString(Context->getPrintingPolicy());
  }

  // struct __objcFastEnumerationState enumState = { 0 };
  buf += "struct __objcFastEnumerationState enumState = { 0 };\n\t";
  // id __rw_items[16];
  buf += "id __rw_items[16];\n\t";
  // id l_collection = (id)
  buf += "id l_collection = (id)";
  // Find start location of 'collection' the hard way!
  const char *startCollectionBuf = startBuf;
  startCollectionBuf += 3;  // skip 'for'
  startCollectionBuf = strchr(startCollectionBuf, '(');
  startCollectionBuf++; // skip '('
  // find 'in' and skip it.
  while (*startCollectionBuf != ' ' ||
         *(startCollectionBuf+1) != 'i' || *(startCollectionBuf+2) != 'n' ||
         (*(startCollectionBuf+3) != ' ' &&
          *(startCollectionBuf+3) != '[' && *(startCollectionBuf+3) != '('))
    startCollectionBuf++;
  startCollectionBuf += 3;

  // Replace: "for (type element in" with string constructed thus far.
  ReplaceText(startLoc, startCollectionBuf - startBuf, buf);
  // Replace ')' in for '(' type elem in collection ')' with ';'
  SourceLocation rightParenLoc = S->getRParenLoc();
  const char *rparenBuf = SM->getCharacterData(rightParenLoc);
  SourceLocation lparenLoc = startLoc.getLocWithOffset(rparenBuf-startBuf);
  buf = ";\n\t";

  // unsigned long limit = [l_collection countByEnumeratingWithState:&enumState
  //                                   objects:__rw_items count:16];
  // which is synthesized into:
  // unsigned int limit =
  // ((unsigned int (*)
  //  (id, SEL, struct __objcFastEnumerationState *, id *, unsigned int))
  //  (void *)objc_msgSend)((id)l_collection,
  //                        sel_registerName(
  //                          "countByEnumeratingWithState:objects:count:"),
  //                        (struct __objcFastEnumerationState *)&state,
  //                        (id *)__rw_items, (unsigned int)16);
  buf += "unsigned long limit =\n\t\t";
  SynthCountByEnumWithState(buf);
  buf += ";\n\t";
  /// if (limit) {
  ///   unsigned long startMutations = *enumState.mutationsPtr;
  ///   do {
  ///        unsigned long counter = 0;
  ///        do {
  ///             if (startMutations != *enumState.mutationsPtr)
  ///               objc_enumerationMutation(l_collection);
  ///             elem = (type)enumState.itemsPtr[counter++];
  buf += "if (limit) {\n\t";
  buf += "unsigned long startMutations = *enumState.mutationsPtr;\n\t";
  buf += "do {\n\t\t";
  buf += "unsigned long counter = 0;\n\t\t";
  buf += "do {\n\t\t\t";
  buf += "if (startMutations != *enumState.mutationsPtr)\n\t\t\t\t";
  buf += "objc_enumerationMutation(l_collection);\n\t\t\t";
  buf += elementName;
  buf += " = (";
  buf += elementTypeAsString;
  buf += ")enumState.itemsPtr[counter++];";
  // Replace ')' in for '(' type elem in collection ')' with all of these.
  ReplaceText(lparenLoc, 1, buf);

  ///            __continue_label: ;
  ///        } while (counter < limit);
  ///   } while (limit = [l_collection countByEnumeratingWithState:&enumState
  ///                                  objects:__rw_items count:16]);
  ///   elem = nil;
  ///   __break_label: ;
  ///  }
  ///  else
  ///       elem = nil;
  ///  }
  ///
  buf = ";\n\t";
  buf += "__continue_label_";
  buf += utostr(ObjCBcLabelNo.back());
  buf += ": ;";
  buf += "\n\t\t";
  buf += "} while (counter < limit);\n\t";
  buf += "} while (limit = ";
  SynthCountByEnumWithState(buf);
  buf += ");\n\t";
  buf += elementName;
  buf += " = ((";
  buf += elementTypeAsString;
  buf += ")0);\n\t";
  buf += "__break_label_";
  buf += utostr(ObjCBcLabelNo.back());
  buf += ": ;\n\t";
  buf += "}\n\t";
  buf += "else\n\t\t";
  buf += elementName;
  buf += " = ((";
  buf += elementTypeAsString;
  buf += ")0);\n\t";
  buf += "}\n";

  // Insert all these *after* the statement body.
  // FIXME: If this should support Obj-C++, support CXXTryStmt
  if (isa<CompoundStmt>(S->getBody())) {
    SourceLocation endBodyLoc = OrigEnd.getLocWithOffset(1);
    InsertText(endBodyLoc, buf);
  } else {
    /* Need to treat single statements specially. For example:
     *
     *     for (A *a in b) if (stuff()) break;
     *     for (A *a in b) xxxyy;
     *
     * The following code simply scans ahead to the semi to find the actual end.
     */
    const char *stmtBuf = SM->getCharacterData(OrigEnd);
    const char *semiBuf = strchr(stmtBuf, ';');
    assert(semiBuf && "Can't find ';'");
    SourceLocation endBodyLoc = OrigEnd.getLocWithOffset(semiBuf-stmtBuf+1);
    InsertText(endBodyLoc, buf);
  }
  Stmts.pop_back();
  ObjCBcLabelNo.pop_back();
  return 0;
}

/// RewriteObjCSynchronizedStmt -
/// This routine rewrites @synchronized(expr) stmt;
/// into:
/// objc_sync_enter(expr);
/// @try stmt @finally { objc_sync_exit(expr); }
///
Stmt *RewriteModernObjC::RewriteObjCSynchronizedStmt(ObjCAtSynchronizedStmt *S) {
  // Get the start location and compute the semi location.
  SourceLocation startLoc = S->getLocStart();
  const char *startBuf = SM->getCharacterData(startLoc);

  assert((*startBuf == '@') && "bogus @synchronized location");

  std::string buf;
  buf = "objc_sync_enter((id)";
  const char *lparenBuf = startBuf;
  while (*lparenBuf != '(') lparenBuf++;
  ReplaceText(startLoc, lparenBuf-startBuf+1, buf);
  // We can't use S->getSynchExpr()->getLocEnd() to find the end location, since
  // the sync expression is typically a message expression that's already
  // been rewritten! (which implies the SourceLocation's are invalid).
  SourceLocation endLoc = S->getSynchBody()->getLocStart();
  const char *endBuf = SM->getCharacterData(endLoc);
  while (*endBuf != ')') endBuf--;
  SourceLocation rparenLoc = startLoc.getLocWithOffset(endBuf-startBuf);
  buf = ");\n";
  // declare a new scope with two variables, _stack and _rethrow.
  buf += "/* @try scope begin */ \n{ struct _objc_exception_data {\n";
  buf += "int buf[18/*32-bit i386*/];\n";
  buf += "char *pointers[4];} _stack;\n";
  buf += "id volatile _rethrow = 0;\n";
  buf += "objc_exception_try_enter(&_stack);\n";
  buf += "if (!_setjmp(_stack.buf)) /* @try block continue */\n";
  ReplaceText(rparenLoc, 1, buf);
  startLoc = S->getSynchBody()->getLocEnd();
  startBuf = SM->getCharacterData(startLoc);

  assert((*startBuf == '}') && "bogus @synchronized block");
  SourceLocation lastCurlyLoc = startLoc;
  buf = "}\nelse {\n";
  buf += "  _rethrow = objc_exception_extract(&_stack);\n";
  buf += "}\n";
  buf += "{ /* implicit finally clause */\n";
  buf += "  if (!_rethrow) objc_exception_try_exit(&_stack);\n";
  
  std::string syncBuf;
  syncBuf += " objc_sync_exit(";

  Expr *syncExpr = S->getSynchExpr();
  CastKind CK = syncExpr->getType()->isObjCObjectPointerType()
                  ? CK_BitCast :
                syncExpr->getType()->isBlockPointerType()
                  ? CK_BlockPointerToObjCPointerCast
                  : CK_CPointerToObjCPointerCast;
  syncExpr = NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                                      CK, syncExpr);
  std::string syncExprBufS;
  llvm::raw_string_ostream syncExprBuf(syncExprBufS);
  syncExpr->printPretty(syncExprBuf, *Context, 0,
                        PrintingPolicy(LangOpts));
  syncBuf += syncExprBuf.str();
  syncBuf += ");";
  
  buf += syncBuf;
  buf += "\n  if (_rethrow) objc_exception_throw(_rethrow);\n";
  buf += "}\n";
  buf += "}";

  ReplaceText(lastCurlyLoc, 1, buf);

  bool hasReturns = false;
  HasReturnStmts(S->getSynchBody(), hasReturns);
  if (hasReturns)
    RewriteSyncReturnStmts(S->getSynchBody(), syncBuf);

  return 0;
}

void RewriteModernObjC::WarnAboutReturnGotoStmts(Stmt *S)
{
  // Perform a bottom up traversal of all children.
  for (Stmt::child_range CI = S->children(); CI; ++CI)
    if (*CI)
      WarnAboutReturnGotoStmts(*CI);

  if (isa<ReturnStmt>(S) || isa<GotoStmt>(S)) {
    Diags.Report(Context->getFullLoc(S->getLocStart()),
                 TryFinallyContainsReturnDiag);
  }
  return;
}

void RewriteModernObjC::HasReturnStmts(Stmt *S, bool &hasReturns) 
{  
  // Perform a bottom up traversal of all children.
  for (Stmt::child_range CI = S->children(); CI; ++CI)
   if (*CI)
     HasReturnStmts(*CI, hasReturns);

 if (isa<ReturnStmt>(S))
   hasReturns = true;
 return;
}

void RewriteModernObjC::RewriteTryReturnStmts(Stmt *S) {
 // Perform a bottom up traversal of all children.
 for (Stmt::child_range CI = S->children(); CI; ++CI)
   if (*CI) {
     RewriteTryReturnStmts(*CI);
   }
 if (isa<ReturnStmt>(S)) {
   SourceLocation startLoc = S->getLocStart();
   const char *startBuf = SM->getCharacterData(startLoc);

   const char *semiBuf = strchr(startBuf, ';');
   assert((*semiBuf == ';') && "RewriteTryReturnStmts: can't find ';'");
   SourceLocation onePastSemiLoc = startLoc.getLocWithOffset(semiBuf-startBuf+1);

   std::string buf;
   buf = "{ objc_exception_try_exit(&_stack); return";
   
   ReplaceText(startLoc, 6, buf);
   InsertText(onePastSemiLoc, "}");
 }
 return;
}

void RewriteModernObjC::RewriteSyncReturnStmts(Stmt *S, std::string syncExitBuf) {
  // Perform a bottom up traversal of all children.
  for (Stmt::child_range CI = S->children(); CI; ++CI)
    if (*CI) {
      RewriteSyncReturnStmts(*CI, syncExitBuf);
    }
  if (isa<ReturnStmt>(S)) {
    SourceLocation startLoc = S->getLocStart();
    const char *startBuf = SM->getCharacterData(startLoc);

    const char *semiBuf = strchr(startBuf, ';');
    assert((*semiBuf == ';') && "RewriteSyncReturnStmts: can't find ';'");
    SourceLocation onePastSemiLoc = startLoc.getLocWithOffset(semiBuf-startBuf+1);

    std::string buf;
    buf = "{ objc_exception_try_exit(&_stack);";
    buf += syncExitBuf;
    buf += " return";
    
    ReplaceText(startLoc, 6, buf);
    InsertText(onePastSemiLoc, "}");
  }
  return;
}

Stmt *RewriteModernObjC::RewriteObjCTryStmt(ObjCAtTryStmt *S) {
  // Get the start location and compute the semi location.
  SourceLocation startLoc = S->getLocStart();
  const char *startBuf = SM->getCharacterData(startLoc);

  assert((*startBuf == '@') && "bogus @try location");

  std::string buf;
  // declare a new scope with two variables, _stack and _rethrow.
  buf = "/* @try scope begin */ { struct _objc_exception_data {\n";
  buf += "int buf[18/*32-bit i386*/];\n";
  buf += "char *pointers[4];} _stack;\n";
  buf += "id volatile _rethrow = 0;\n";
  buf += "objc_exception_try_enter(&_stack);\n";
  buf += "if (!_setjmp(_stack.buf)) /* @try block continue */\n";

  ReplaceText(startLoc, 4, buf);

  startLoc = S->getTryBody()->getLocEnd();
  startBuf = SM->getCharacterData(startLoc);

  assert((*startBuf == '}') && "bogus @try block");

  SourceLocation lastCurlyLoc = startLoc;
  if (S->getNumCatchStmts()) {
    startLoc = startLoc.getLocWithOffset(1);
    buf = " /* @catch begin */ else {\n";
    buf += " id _caught = objc_exception_extract(&_stack);\n";
    buf += " objc_exception_try_enter (&_stack);\n";
    buf += " if (_setjmp(_stack.buf))\n";
    buf += "   _rethrow = objc_exception_extract(&_stack);\n";
    buf += " else { /* @catch continue */";

    InsertText(startLoc, buf);
  } else { /* no catch list */
    buf = "}\nelse {\n";
    buf += "  _rethrow = objc_exception_extract(&_stack);\n";
    buf += "}";
    ReplaceText(lastCurlyLoc, 1, buf);
  }
  Stmt *lastCatchBody = 0;
  for (unsigned I = 0, N = S->getNumCatchStmts(); I != N; ++I) {
    ObjCAtCatchStmt *Catch = S->getCatchStmt(I);
    VarDecl *catchDecl = Catch->getCatchParamDecl();

    if (I == 0)
      buf = "if ("; // we are generating code for the first catch clause
    else
      buf = "else if (";
    startLoc = Catch->getLocStart();
    startBuf = SM->getCharacterData(startLoc);

    assert((*startBuf == '@') && "bogus @catch location");

    const char *lParenLoc = strchr(startBuf, '(');

    if (Catch->hasEllipsis()) {
      // Now rewrite the body...
      lastCatchBody = Catch->getCatchBody();
      SourceLocation bodyLoc = lastCatchBody->getLocStart();
      const char *bodyBuf = SM->getCharacterData(bodyLoc);
      assert(*SM->getCharacterData(Catch->getRParenLoc()) == ')' &&
             "bogus @catch paren location");
      assert((*bodyBuf == '{') && "bogus @catch body location");

      buf += "1) { id _tmp = _caught;";
      Rewrite.ReplaceText(startLoc, bodyBuf-startBuf+1, buf);
    } else if (catchDecl) {
      QualType t = catchDecl->getType();
      if (t == Context->getObjCIdType()) {
        buf += "1) { ";
        ReplaceText(startLoc, lParenLoc-startBuf+1, buf);
      } else if (const ObjCObjectPointerType *Ptr =
                   t->getAs<ObjCObjectPointerType>()) {
        // Should be a pointer to a class.
        ObjCInterfaceDecl *IDecl = Ptr->getObjectType()->getInterface();
        if (IDecl) {
          buf += "objc_exception_match((struct objc_class *)objc_getClass(\"";
          buf += IDecl->getNameAsString();
          buf += "\"), (struct objc_object *)_caught)) { ";
          ReplaceText(startLoc, lParenLoc-startBuf+1, buf);
        }
      }
      // Now rewrite the body...
      lastCatchBody = Catch->getCatchBody();
      SourceLocation rParenLoc = Catch->getRParenLoc();
      SourceLocation bodyLoc = lastCatchBody->getLocStart();
      const char *bodyBuf = SM->getCharacterData(bodyLoc);
      const char *rParenBuf = SM->getCharacterData(rParenLoc);
      assert((*rParenBuf == ')') && "bogus @catch paren location");
      assert((*bodyBuf == '{') && "bogus @catch body location");

      // Here we replace ") {" with "= _caught;" (which initializes and
      // declares the @catch parameter).
      ReplaceText(rParenLoc, bodyBuf-rParenBuf+1, " = _caught;");
    } else {
      llvm_unreachable("@catch rewrite bug");
    }
  }
  // Complete the catch list...
  if (lastCatchBody) {
    SourceLocation bodyLoc = lastCatchBody->getLocEnd();
    assert(*SM->getCharacterData(bodyLoc) == '}' &&
           "bogus @catch body location");

    // Insert the last (implicit) else clause *before* the right curly brace.
    bodyLoc = bodyLoc.getLocWithOffset(-1);
    buf = "} /* last catch end */\n";
    buf += "else {\n";
    buf += " _rethrow = _caught;\n";
    buf += " objc_exception_try_exit(&_stack);\n";
    buf += "} } /* @catch end */\n";
    if (!S->getFinallyStmt())
      buf += "}\n";
    InsertText(bodyLoc, buf);

    // Set lastCurlyLoc
    lastCurlyLoc = lastCatchBody->getLocEnd();
  }
  if (ObjCAtFinallyStmt *finalStmt = S->getFinallyStmt()) {
    startLoc = finalStmt->getLocStart();
    startBuf = SM->getCharacterData(startLoc);
    assert((*startBuf == '@') && "bogus @finally start");

    ReplaceText(startLoc, 8, "/* @finally */");

    Stmt *body = finalStmt->getFinallyBody();
    SourceLocation startLoc = body->getLocStart();
    SourceLocation endLoc = body->getLocEnd();
    assert(*SM->getCharacterData(startLoc) == '{' &&
           "bogus @finally body location");
    assert(*SM->getCharacterData(endLoc) == '}' &&
           "bogus @finally body location");

    startLoc = startLoc.getLocWithOffset(1);
    InsertText(startLoc, " if (!_rethrow) objc_exception_try_exit(&_stack);\n");
    endLoc = endLoc.getLocWithOffset(-1);
    InsertText(endLoc, " if (_rethrow) objc_exception_throw(_rethrow);\n");

    // Set lastCurlyLoc
    lastCurlyLoc = body->getLocEnd();

    // Now check for any return/continue/go statements within the @try.
    WarnAboutReturnGotoStmts(S->getTryBody());
  } else { /* no finally clause - make sure we synthesize an implicit one */
    buf = "{ /* implicit finally clause */\n";
    buf += " if (!_rethrow) objc_exception_try_exit(&_stack);\n";
    buf += " if (_rethrow) objc_exception_throw(_rethrow);\n";
    buf += "}";
    ReplaceText(lastCurlyLoc, 1, buf);
    
    // Now check for any return/continue/go statements within the @try.
    // The implicit finally clause won't called if the @try contains any
    // jump statements.
    bool hasReturns = false;
    HasReturnStmts(S->getTryBody(), hasReturns);
    if (hasReturns)
      RewriteTryReturnStmts(S->getTryBody());
  }
  // Now emit the final closing curly brace...
  lastCurlyLoc = lastCurlyLoc.getLocWithOffset(1);
  InsertText(lastCurlyLoc, " } /* @try scope end */\n");
  return 0;
}

// This can't be done with ReplaceStmt(S, ThrowExpr), since
// the throw expression is typically a message expression that's already
// been rewritten! (which implies the SourceLocation's are invalid).
Stmt *RewriteModernObjC::RewriteObjCThrowStmt(ObjCAtThrowStmt *S) {
  // Get the start location and compute the semi location.
  SourceLocation startLoc = S->getLocStart();
  const char *startBuf = SM->getCharacterData(startLoc);

  assert((*startBuf == '@') && "bogus @throw location");

  std::string buf;
  /* void objc_exception_throw(id) __attribute__((noreturn)); */
  if (S->getThrowExpr())
    buf = "objc_exception_throw(";
  else // add an implicit argument
    buf = "objc_exception_throw(_caught";

  // handle "@  throw" correctly.
  const char *wBuf = strchr(startBuf, 'w');
  assert((*wBuf == 'w') && "@throw: can't find 'w'");
  ReplaceText(startLoc, wBuf-startBuf+1, buf);

  const char *semiBuf = strchr(startBuf, ';');
  assert((*semiBuf == ';') && "@throw: can't find ';'");
  SourceLocation semiLoc = startLoc.getLocWithOffset(semiBuf-startBuf);
  ReplaceText(semiLoc, 1, ");");
  return 0;
}

Stmt *RewriteModernObjC::RewriteAtEncode(ObjCEncodeExpr *Exp) {
  // Create a new string expression.
  QualType StrType = Context->getPointerType(Context->CharTy);
  std::string StrEncoding;
  Context->getObjCEncodingForType(Exp->getEncodedType(), StrEncoding);
  Expr *Replacement = StringLiteral::Create(*Context, StrEncoding,
                                            StringLiteral::Ascii, false,
                                            StrType, SourceLocation());
  ReplaceStmt(Exp, Replacement);

  // Replace this subexpr in the parent.
  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return Replacement;
}

Stmt *RewriteModernObjC::RewriteAtSelector(ObjCSelectorExpr *Exp) {
  if (!SelGetUidFunctionDecl)
    SynthSelGetUidFunctionDecl();
  assert(SelGetUidFunctionDecl && "Can't find sel_registerName() decl");
  // Create a call to sel_registerName("selName").
  SmallVector<Expr*, 8> SelExprs;
  QualType argType = Context->getPointerType(Context->CharTy);
  SelExprs.push_back(StringLiteral::Create(*Context,
                                           Exp->getSelector().getAsString(),
                                           StringLiteral::Ascii, false,
                                           argType, SourceLocation()));
  CallExpr *SelExp = SynthesizeCallToFunctionDecl(SelGetUidFunctionDecl,
                                                 &SelExprs[0], SelExprs.size());
  ReplaceStmt(Exp, SelExp);
  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return SelExp;
}

CallExpr *RewriteModernObjC::SynthesizeCallToFunctionDecl(
  FunctionDecl *FD, Expr **args, unsigned nargs, SourceLocation StartLoc,
                                                    SourceLocation EndLoc) {
  // Get the type, we will need to reference it in a couple spots.
  QualType msgSendType = FD->getType();

  // Create a reference to the objc_msgSend() declaration.
  DeclRefExpr *DRE =
    new (Context) DeclRefExpr(FD, false, msgSendType, VK_LValue, SourceLocation());

  // Now, we cast the reference to a pointer to the objc_msgSend type.
  QualType pToFunc = Context->getPointerType(msgSendType);
  ImplicitCastExpr *ICE = 
    ImplicitCastExpr::Create(*Context, pToFunc, CK_FunctionToPointerDecay,
                             DRE, 0, VK_RValue);

  const FunctionType *FT = msgSendType->getAs<FunctionType>();

  CallExpr *Exp =  
    new (Context) CallExpr(*Context, ICE, args, nargs, 
                           FT->getCallResultType(*Context),
                           VK_RValue, EndLoc);
  return Exp;
}

static bool scanForProtocolRefs(const char *startBuf, const char *endBuf,
                                const char *&startRef, const char *&endRef) {
  while (startBuf < endBuf) {
    if (*startBuf == '<')
      startRef = startBuf; // mark the start.
    if (*startBuf == '>') {
      if (startRef && *startRef == '<') {
        endRef = startBuf; // mark the end.
        return true;
      }
      return false;
    }
    startBuf++;
  }
  return false;
}

static void scanToNextArgument(const char *&argRef) {
  int angle = 0;
  while (*argRef != ')' && (*argRef != ',' || angle > 0)) {
    if (*argRef == '<')
      angle++;
    else if (*argRef == '>')
      angle--;
    argRef++;
  }
  assert(angle == 0 && "scanToNextArgument - bad protocol type syntax");
}

bool RewriteModernObjC::needToScanForQualifiers(QualType T) {
  if (T->isObjCQualifiedIdType())
    return true;
  if (const PointerType *PT = T->getAs<PointerType>()) {
    if (PT->getPointeeType()->isObjCQualifiedIdType())
      return true;
  }
  if (T->isObjCObjectPointerType()) {
    T = T->getPointeeType();
    return T->isObjCQualifiedInterfaceType();
  }
  if (T->isArrayType()) {
    QualType ElemTy = Context->getBaseElementType(T);
    return needToScanForQualifiers(ElemTy);
  }
  return false;
}

void RewriteModernObjC::RewriteObjCQualifiedInterfaceTypes(Expr *E) {
  QualType Type = E->getType();
  if (needToScanForQualifiers(Type)) {
    SourceLocation Loc, EndLoc;

    if (const CStyleCastExpr *ECE = dyn_cast<CStyleCastExpr>(E)) {
      Loc = ECE->getLParenLoc();
      EndLoc = ECE->getRParenLoc();
    } else {
      Loc = E->getLocStart();
      EndLoc = E->getLocEnd();
    }
    // This will defend against trying to rewrite synthesized expressions.
    if (Loc.isInvalid() || EndLoc.isInvalid())
      return;

    const char *startBuf = SM->getCharacterData(Loc);
    const char *endBuf = SM->getCharacterData(EndLoc);
    const char *startRef = 0, *endRef = 0;
    if (scanForProtocolRefs(startBuf, endBuf, startRef, endRef)) {
      // Get the locations of the startRef, endRef.
      SourceLocation LessLoc = Loc.getLocWithOffset(startRef-startBuf);
      SourceLocation GreaterLoc = Loc.getLocWithOffset(endRef-startBuf+1);
      // Comment out the protocol references.
      InsertText(LessLoc, "/*");
      InsertText(GreaterLoc, "*/");
    }
  }
}

void RewriteModernObjC::RewriteObjCQualifiedInterfaceTypes(Decl *Dcl) {
  SourceLocation Loc;
  QualType Type;
  const FunctionProtoType *proto = 0;
  if (VarDecl *VD = dyn_cast<VarDecl>(Dcl)) {
    Loc = VD->getLocation();
    Type = VD->getType();
  }
  else if (FunctionDecl *FD = dyn_cast<FunctionDecl>(Dcl)) {
    Loc = FD->getLocation();
    // Check for ObjC 'id' and class types that have been adorned with protocol
    // information (id<p>, C<p>*). The protocol references need to be rewritten!
    const FunctionType *funcType = FD->getType()->getAs<FunctionType>();
    assert(funcType && "missing function type");
    proto = dyn_cast<FunctionProtoType>(funcType);
    if (!proto)
      return;
    Type = proto->getResultType();
  }
  else if (FieldDecl *FD = dyn_cast<FieldDecl>(Dcl)) {
    Loc = FD->getLocation();
    Type = FD->getType();
  }
  else
    return;

  if (needToScanForQualifiers(Type)) {
    // Since types are unique, we need to scan the buffer.

    const char *endBuf = SM->getCharacterData(Loc);
    const char *startBuf = endBuf;
    while (*startBuf != ';' && *startBuf != '<' && startBuf != MainFileStart)
      startBuf--; // scan backward (from the decl location) for return type.
    const char *startRef = 0, *endRef = 0;
    if (scanForProtocolRefs(startBuf, endBuf, startRef, endRef)) {
      // Get the locations of the startRef, endRef.
      SourceLocation LessLoc = Loc.getLocWithOffset(startRef-endBuf);
      SourceLocation GreaterLoc = Loc.getLocWithOffset(endRef-endBuf+1);
      // Comment out the protocol references.
      InsertText(LessLoc, "/*");
      InsertText(GreaterLoc, "*/");
    }
  }
  if (!proto)
      return; // most likely, was a variable
  // Now check arguments.
  const char *startBuf = SM->getCharacterData(Loc);
  const char *startFuncBuf = startBuf;
  for (unsigned i = 0; i < proto->getNumArgs(); i++) {
    if (needToScanForQualifiers(proto->getArgType(i))) {
      // Since types are unique, we need to scan the buffer.

      const char *endBuf = startBuf;
      // scan forward (from the decl location) for argument types.
      scanToNextArgument(endBuf);
      const char *startRef = 0, *endRef = 0;
      if (scanForProtocolRefs(startBuf, endBuf, startRef, endRef)) {
        // Get the locations of the startRef, endRef.
        SourceLocation LessLoc =
          Loc.getLocWithOffset(startRef-startFuncBuf);
        SourceLocation GreaterLoc =
          Loc.getLocWithOffset(endRef-startFuncBuf+1);
        // Comment out the protocol references.
        InsertText(LessLoc, "/*");
        InsertText(GreaterLoc, "*/");
      }
      startBuf = ++endBuf;
    }
    else {
      // If the function name is derived from a macro expansion, then the
      // argument buffer will not follow the name. Need to speak with Chris.
      while (*startBuf && *startBuf != ')' && *startBuf != ',')
        startBuf++; // scan forward (from the decl location) for argument types.
      startBuf++;
    }
  }
}

void RewriteModernObjC::RewriteTypeOfDecl(VarDecl *ND) {
  QualType QT = ND->getType();
  const Type* TypePtr = QT->getAs<Type>();
  if (!isa<TypeOfExprType>(TypePtr))
    return;
  while (isa<TypeOfExprType>(TypePtr)) {
    const TypeOfExprType *TypeOfExprTypePtr = cast<TypeOfExprType>(TypePtr);
    QT = TypeOfExprTypePtr->getUnderlyingExpr()->getType();
    TypePtr = QT->getAs<Type>();
  }
  // FIXME. This will not work for multiple declarators; as in:
  // __typeof__(a) b,c,d;
  std::string TypeAsString(QT.getAsString(Context->getPrintingPolicy()));
  SourceLocation DeclLoc = ND->getTypeSpecStartLoc();
  const char *startBuf = SM->getCharacterData(DeclLoc);
  if (ND->getInit()) {
    std::string Name(ND->getNameAsString());
    TypeAsString += " " + Name + " = ";
    Expr *E = ND->getInit();
    SourceLocation startLoc;
    if (const CStyleCastExpr *ECE = dyn_cast<CStyleCastExpr>(E))
      startLoc = ECE->getLParenLoc();
    else
      startLoc = E->getLocStart();
    startLoc = SM->getExpansionLoc(startLoc);
    const char *endBuf = SM->getCharacterData(startLoc);
    ReplaceText(DeclLoc, endBuf-startBuf-1, TypeAsString);
  }
  else {
    SourceLocation X = ND->getLocEnd();
    X = SM->getExpansionLoc(X);
    const char *endBuf = SM->getCharacterData(X);
    ReplaceText(DeclLoc, endBuf-startBuf-1, TypeAsString);
  }
}

// SynthSelGetUidFunctionDecl - SEL sel_registerName(const char *str);
void RewriteModernObjC::SynthSelGetUidFunctionDecl() {
  IdentifierInfo *SelGetUidIdent = &Context->Idents.get("sel_registerName");
  SmallVector<QualType, 16> ArgTys;
  ArgTys.push_back(Context->getPointerType(Context->CharTy.withConst()));
  QualType getFuncType =
    getSimpleFunctionType(Context->getObjCSelType(), &ArgTys[0], ArgTys.size());
  SelGetUidFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                           SourceLocation(),
                                           SourceLocation(),
                                           SelGetUidIdent, getFuncType, 0,
                                           SC_Extern,
                                           SC_None, false);
}

void RewriteModernObjC::RewriteFunctionDecl(FunctionDecl *FD) {
  // declared in <objc/objc.h>
  if (FD->getIdentifier() &&
      FD->getName() == "sel_registerName") {
    SelGetUidFunctionDecl = FD;
    return;
  }
  RewriteObjCQualifiedInterfaceTypes(FD);
}

void RewriteModernObjC::RewriteBlockPointerType(std::string& Str, QualType Type) {
  std::string TypeString(Type.getAsString(Context->getPrintingPolicy()));
  const char *argPtr = TypeString.c_str();
  if (!strchr(argPtr, '^')) {
    Str += TypeString;
    return;
  }
  while (*argPtr) {
    Str += (*argPtr == '^' ? '*' : *argPtr);
    argPtr++;
  }
}

// FIXME. Consolidate this routine with RewriteBlockPointerType.
void RewriteModernObjC::RewriteBlockPointerTypeVariable(std::string& Str,
                                                  ValueDecl *VD) {
  QualType Type = VD->getType();
  std::string TypeString(Type.getAsString(Context->getPrintingPolicy()));
  const char *argPtr = TypeString.c_str();
  int paren = 0;
  while (*argPtr) {
    switch (*argPtr) {
      case '(':
        Str += *argPtr;
        paren++;
        break;
      case ')':
        Str += *argPtr;
        paren--;
        break;
      case '^':
        Str += '*';
        if (paren == 1)
          Str += VD->getNameAsString();
        break;
      default:
        Str += *argPtr;
        break;
    }
    argPtr++;
  }
}


void RewriteModernObjC::RewriteBlockLiteralFunctionDecl(FunctionDecl *FD) {
  SourceLocation FunLocStart = FD->getTypeSpecStartLoc();
  const FunctionType *funcType = FD->getType()->getAs<FunctionType>();
  const FunctionProtoType *proto = dyn_cast<FunctionProtoType>(funcType);
  if (!proto)
    return;
  QualType Type = proto->getResultType();
  std::string FdStr = Type.getAsString(Context->getPrintingPolicy());
  FdStr += " ";
  FdStr += FD->getName();
  FdStr +=  "(";
  unsigned numArgs = proto->getNumArgs();
  for (unsigned i = 0; i < numArgs; i++) {
    QualType ArgType = proto->getArgType(i);
    RewriteBlockPointerType(FdStr, ArgType);
    if (i+1 < numArgs)
      FdStr += ", ";
  }
  FdStr +=  ");\n";
  InsertText(FunLocStart, FdStr);
  CurFunctionDeclToDeclareForBlock = 0;
}

// SynthSuperContructorFunctionDecl - id objc_super(id obj, id super);
void RewriteModernObjC::SynthSuperContructorFunctionDecl() {
  if (SuperContructorFunctionDecl)
    return;
  IdentifierInfo *msgSendIdent = &Context->Idents.get("__rw_objc_super");
  SmallVector<QualType, 16> ArgTys;
  QualType argT = Context->getObjCIdType();
  assert(!argT.isNull() && "Can't find 'id' type");
  ArgTys.push_back(argT);
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->getObjCIdType(),
                                               &ArgTys[0], ArgTys.size());
  SuperContructorFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                         SourceLocation(),
                                         SourceLocation(),
                                         msgSendIdent, msgSendType, 0,
                                         SC_Extern,
                                         SC_None, false);
}

// SynthMsgSendFunctionDecl - id objc_msgSend(id self, SEL op, ...);
void RewriteModernObjC::SynthMsgSendFunctionDecl() {
  IdentifierInfo *msgSendIdent = &Context->Idents.get("objc_msgSend");
  SmallVector<QualType, 16> ArgTys;
  QualType argT = Context->getObjCIdType();
  assert(!argT.isNull() && "Can't find 'id' type");
  ArgTys.push_back(argT);
  argT = Context->getObjCSelType();
  assert(!argT.isNull() && "Can't find 'SEL' type");
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->getObjCIdType(),
                                               &ArgTys[0], ArgTys.size(),
                                               true /*isVariadic*/);
  MsgSendFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                         SourceLocation(),
                                         SourceLocation(),
                                         msgSendIdent, msgSendType, 0,
                                         SC_Extern,
                                         SC_None, false);
}

// SynthMsgSendSuperFunctionDecl - id objc_msgSendSuper(struct objc_super *, SEL op, ...);
void RewriteModernObjC::SynthMsgSendSuperFunctionDecl() {
  IdentifierInfo *msgSendIdent = &Context->Idents.get("objc_msgSendSuper");
  SmallVector<QualType, 16> ArgTys;
  RecordDecl *RD = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                      SourceLocation(), SourceLocation(),
                                      &Context->Idents.get("objc_super"));
  QualType argT = Context->getPointerType(Context->getTagDeclType(RD));
  assert(!argT.isNull() && "Can't build 'struct objc_super *' type");
  ArgTys.push_back(argT);
  argT = Context->getObjCSelType();
  assert(!argT.isNull() && "Can't find 'SEL' type");
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->getObjCIdType(),
                                               &ArgTys[0], ArgTys.size(),
                                               true /*isVariadic*/);
  MsgSendSuperFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                              SourceLocation(),
                                              SourceLocation(),
                                              msgSendIdent, msgSendType, 0,
                                              SC_Extern,
                                              SC_None, false);
}

// SynthMsgSendStretFunctionDecl - id objc_msgSend_stret(id self, SEL op, ...);
void RewriteModernObjC::SynthMsgSendStretFunctionDecl() {
  IdentifierInfo *msgSendIdent = &Context->Idents.get("objc_msgSend_stret");
  SmallVector<QualType, 16> ArgTys;
  QualType argT = Context->getObjCIdType();
  assert(!argT.isNull() && "Can't find 'id' type");
  ArgTys.push_back(argT);
  argT = Context->getObjCSelType();
  assert(!argT.isNull() && "Can't find 'SEL' type");
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->getObjCIdType(),
                                               &ArgTys[0], ArgTys.size(),
                                               true /*isVariadic*/);
  MsgSendStretFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                         SourceLocation(),
                                         SourceLocation(),
                                         msgSendIdent, msgSendType, 0,
                                         SC_Extern,
                                         SC_None, false);
}

// SynthMsgSendSuperStretFunctionDecl -
// id objc_msgSendSuper_stret(struct objc_super *, SEL op, ...);
void RewriteModernObjC::SynthMsgSendSuperStretFunctionDecl() {
  IdentifierInfo *msgSendIdent =
    &Context->Idents.get("objc_msgSendSuper_stret");
  SmallVector<QualType, 16> ArgTys;
  RecordDecl *RD = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                      SourceLocation(), SourceLocation(),
                                      &Context->Idents.get("objc_super"));
  QualType argT = Context->getPointerType(Context->getTagDeclType(RD));
  assert(!argT.isNull() && "Can't build 'struct objc_super *' type");
  ArgTys.push_back(argT);
  argT = Context->getObjCSelType();
  assert(!argT.isNull() && "Can't find 'SEL' type");
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->getObjCIdType(),
                                               &ArgTys[0], ArgTys.size(),
                                               true /*isVariadic*/);
  MsgSendSuperStretFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                                       SourceLocation(),
                                                       SourceLocation(),
                                              msgSendIdent, msgSendType, 0,
                                              SC_Extern,
                                              SC_None, false);
}

// SynthMsgSendFpretFunctionDecl - double objc_msgSend_fpret(id self, SEL op, ...);
void RewriteModernObjC::SynthMsgSendFpretFunctionDecl() {
  IdentifierInfo *msgSendIdent = &Context->Idents.get("objc_msgSend_fpret");
  SmallVector<QualType, 16> ArgTys;
  QualType argT = Context->getObjCIdType();
  assert(!argT.isNull() && "Can't find 'id' type");
  ArgTys.push_back(argT);
  argT = Context->getObjCSelType();
  assert(!argT.isNull() && "Can't find 'SEL' type");
  ArgTys.push_back(argT);
  QualType msgSendType = getSimpleFunctionType(Context->DoubleTy,
                                               &ArgTys[0], ArgTys.size(),
                                               true /*isVariadic*/);
  MsgSendFpretFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                              SourceLocation(),
                                              SourceLocation(),
                                              msgSendIdent, msgSendType, 0,
                                              SC_Extern,
                                              SC_None, false);
}

// SynthGetClassFunctionDecl - id objc_getClass(const char *name);
void RewriteModernObjC::SynthGetClassFunctionDecl() {
  IdentifierInfo *getClassIdent = &Context->Idents.get("objc_getClass");
  SmallVector<QualType, 16> ArgTys;
  ArgTys.push_back(Context->getPointerType(Context->CharTy.withConst()));
  QualType getClassType = getSimpleFunctionType(Context->getObjCIdType(),
                                                &ArgTys[0], ArgTys.size());
  GetClassFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                          SourceLocation(),
                                          SourceLocation(),
                                          getClassIdent, getClassType, 0,
                                          SC_Extern,
                                          SC_None, false);
}

// SynthGetSuperClassFunctionDecl - Class class_getSuperclass(Class cls);
void RewriteModernObjC::SynthGetSuperClassFunctionDecl() {
  IdentifierInfo *getSuperClassIdent = 
    &Context->Idents.get("class_getSuperclass");
  SmallVector<QualType, 16> ArgTys;
  ArgTys.push_back(Context->getObjCClassType());
  QualType getClassType = getSimpleFunctionType(Context->getObjCClassType(),
                                                &ArgTys[0], ArgTys.size());
  GetSuperClassFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                                   SourceLocation(),
                                                   SourceLocation(),
                                                   getSuperClassIdent,
                                                   getClassType, 0,
                                                   SC_Extern,
                                                   SC_None,
                                                   false);
}

// SynthGetMetaClassFunctionDecl - id objc_getMetaClass(const char *name);
void RewriteModernObjC::SynthGetMetaClassFunctionDecl() {
  IdentifierInfo *getClassIdent = &Context->Idents.get("objc_getMetaClass");
  SmallVector<QualType, 16> ArgTys;
  ArgTys.push_back(Context->getPointerType(Context->CharTy.withConst()));
  QualType getClassType = getSimpleFunctionType(Context->getObjCIdType(),
                                                &ArgTys[0], ArgTys.size());
  GetMetaClassFunctionDecl = FunctionDecl::Create(*Context, TUDecl,
                                              SourceLocation(),
                                              SourceLocation(),
                                              getClassIdent, getClassType, 0,
                                              SC_Extern,
                                              SC_None, false);
}

Stmt *RewriteModernObjC::RewriteObjCStringLiteral(ObjCStringLiteral *Exp) {
  QualType strType = getConstantStringStructType();

  std::string S = "__NSConstantStringImpl_";

  std::string tmpName = InFileName;
  unsigned i;
  for (i=0; i < tmpName.length(); i++) {
    char c = tmpName.at(i);
    // replace any non alphanumeric characters with '_'.
    if (!isalpha(c) && (c < '0' || c > '9'))
      tmpName[i] = '_';
  }
  S += tmpName;
  S += "_";
  S += utostr(NumObjCStringLiterals++);

  Preamble += "static __NSConstantStringImpl " + S;
  Preamble += " __attribute__ ((section (\"__DATA, __cfstring\"))) = {__CFConstantStringClassReference,";
  Preamble += "0x000007c8,"; // utf8_str
  // The pretty printer for StringLiteral handles escape characters properly.
  std::string prettyBufS;
  llvm::raw_string_ostream prettyBuf(prettyBufS);
  Exp->getString()->printPretty(prettyBuf, *Context, 0,
                                PrintingPolicy(LangOpts));
  Preamble += prettyBuf.str();
  Preamble += ",";
  Preamble += utostr(Exp->getString()->getByteLength()) + "};\n";

  VarDecl *NewVD = VarDecl::Create(*Context, TUDecl, SourceLocation(),
                                   SourceLocation(), &Context->Idents.get(S),
                                   strType, 0, SC_Static, SC_None);
  DeclRefExpr *DRE = new (Context) DeclRefExpr(NewVD, false, strType, VK_LValue,
                                               SourceLocation());
  Expr *Unop = new (Context) UnaryOperator(DRE, UO_AddrOf,
                                 Context->getPointerType(DRE->getType()),
                                           VK_RValue, OK_Ordinary,
                                           SourceLocation());
  // cast to NSConstantString *
  CastExpr *cast = NoTypeInfoCStyleCastExpr(Context, Exp->getType(),
                                            CK_CPointerToObjCPointerCast, Unop);
  ReplaceStmt(Exp, cast);
  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return cast;
}

// struct objc_super { struct objc_object *receiver; struct objc_class *super; };
QualType RewriteModernObjC::getSuperStructType() {
  if (!SuperStructDecl) {
    SuperStructDecl = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                         SourceLocation(), SourceLocation(),
                                         &Context->Idents.get("objc_super"));
    QualType FieldTypes[2];

    // struct objc_object *receiver;
    FieldTypes[0] = Context->getObjCIdType();
    // struct objc_class *super;
    FieldTypes[1] = Context->getObjCClassType();

    // Create fields
    for (unsigned i = 0; i < 2; ++i) {
      SuperStructDecl->addDecl(FieldDecl::Create(*Context, SuperStructDecl,
                                                 SourceLocation(),
                                                 SourceLocation(), 0,
                                                 FieldTypes[i], 0,
                                                 /*BitWidth=*/0,
                                                 /*Mutable=*/false,
                                                 /*HasInit=*/false));
    }

    SuperStructDecl->completeDefinition();
  }
  return Context->getTagDeclType(SuperStructDecl);
}

QualType RewriteModernObjC::getConstantStringStructType() {
  if (!ConstantStringDecl) {
    ConstantStringDecl = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                            SourceLocation(), SourceLocation(),
                         &Context->Idents.get("__NSConstantStringImpl"));
    QualType FieldTypes[4];

    // struct objc_object *receiver;
    FieldTypes[0] = Context->getObjCIdType();
    // int flags;
    FieldTypes[1] = Context->IntTy;
    // char *str;
    FieldTypes[2] = Context->getPointerType(Context->CharTy);
    // long length;
    FieldTypes[3] = Context->LongTy;

    // Create fields
    for (unsigned i = 0; i < 4; ++i) {
      ConstantStringDecl->addDecl(FieldDecl::Create(*Context,
                                                    ConstantStringDecl,
                                                    SourceLocation(),
                                                    SourceLocation(), 0,
                                                    FieldTypes[i], 0,
                                                    /*BitWidth=*/0,
                                                    /*Mutable=*/true,
                                                    /*HasInit=*/false));
    }

    ConstantStringDecl->completeDefinition();
  }
  return Context->getTagDeclType(ConstantStringDecl);
}

Stmt *RewriteModernObjC::SynthMessageExpr(ObjCMessageExpr *Exp,
                                    SourceLocation StartLoc,
                                    SourceLocation EndLoc) {
  if (!SelGetUidFunctionDecl)
    SynthSelGetUidFunctionDecl();
  if (!MsgSendFunctionDecl)
    SynthMsgSendFunctionDecl();
  if (!MsgSendSuperFunctionDecl)
    SynthMsgSendSuperFunctionDecl();
  if (!MsgSendStretFunctionDecl)
    SynthMsgSendStretFunctionDecl();
  if (!MsgSendSuperStretFunctionDecl)
    SynthMsgSendSuperStretFunctionDecl();
  if (!MsgSendFpretFunctionDecl)
    SynthMsgSendFpretFunctionDecl();
  if (!GetClassFunctionDecl)
    SynthGetClassFunctionDecl();
  if (!GetSuperClassFunctionDecl)
    SynthGetSuperClassFunctionDecl();
  if (!GetMetaClassFunctionDecl)
    SynthGetMetaClassFunctionDecl();

  // default to objc_msgSend().
  FunctionDecl *MsgSendFlavor = MsgSendFunctionDecl;
  // May need to use objc_msgSend_stret() as well.
  FunctionDecl *MsgSendStretFlavor = 0;
  if (ObjCMethodDecl *mDecl = Exp->getMethodDecl()) {
    QualType resultType = mDecl->getResultType();
    if (resultType->isRecordType())
      MsgSendStretFlavor = MsgSendStretFunctionDecl;
    else if (resultType->isRealFloatingType())
      MsgSendFlavor = MsgSendFpretFunctionDecl;
  }

  // Synthesize a call to objc_msgSend().
  SmallVector<Expr*, 8> MsgExprs;
  switch (Exp->getReceiverKind()) {
  case ObjCMessageExpr::SuperClass: {
    MsgSendFlavor = MsgSendSuperFunctionDecl;
    if (MsgSendStretFlavor)
      MsgSendStretFlavor = MsgSendSuperStretFunctionDecl;
    assert(MsgSendFlavor && "MsgSendFlavor is NULL!");

    ObjCInterfaceDecl *ClassDecl = CurMethodDef->getClassInterface();

    SmallVector<Expr*, 4> InitExprs;

    // set the receiver to self, the first argument to all methods.
    InitExprs.push_back(
      NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                               CK_BitCast,
                   new (Context) DeclRefExpr(CurMethodDef->getSelfDecl(),
                                             false,
                                             Context->getObjCIdType(),
                                             VK_RValue,
                                             SourceLocation()))
                        ); // set the 'receiver'.

    // (id)class_getSuperclass((Class)objc_getClass("CurrentClass"))
    SmallVector<Expr*, 8> ClsExprs;
    QualType argType = Context->getPointerType(Context->CharTy);
    ClsExprs.push_back(StringLiteral::Create(*Context,
                                   ClassDecl->getIdentifier()->getName(),
                                   StringLiteral::Ascii, false,
                                   argType, SourceLocation()));
    CallExpr *Cls = SynthesizeCallToFunctionDecl(GetMetaClassFunctionDecl,
                                                 &ClsExprs[0],
                                                 ClsExprs.size(),
                                                 StartLoc,
                                                 EndLoc);
    // (Class)objc_getClass("CurrentClass")
    CastExpr *ArgExpr = NoTypeInfoCStyleCastExpr(Context,
                                             Context->getObjCClassType(),
                                             CK_BitCast, Cls);
    ClsExprs.clear();
    ClsExprs.push_back(ArgExpr);
    Cls = SynthesizeCallToFunctionDecl(GetSuperClassFunctionDecl,
                                       &ClsExprs[0], ClsExprs.size(),
                                       StartLoc, EndLoc);
    
    // (id)class_getSuperclass((Class)objc_getClass("CurrentClass"))
    // To turn off a warning, type-cast to 'id'
    InitExprs.push_back( // set 'super class', using class_getSuperclass().
                        NoTypeInfoCStyleCastExpr(Context,
                                                 Context->getObjCIdType(),
                                                 CK_BitCast, Cls));
    // struct objc_super
    QualType superType = getSuperStructType();
    Expr *SuperRep;

    if (LangOpts.MicrosoftExt) {
      SynthSuperContructorFunctionDecl();
      // Simulate a contructor call...
      DeclRefExpr *DRE = new (Context) DeclRefExpr(SuperContructorFunctionDecl,
                                                   false, superType, VK_LValue,
                                                   SourceLocation());
      SuperRep = new (Context) CallExpr(*Context, DRE, &InitExprs[0],
                                        InitExprs.size(),
                                        superType, VK_LValue,
                                        SourceLocation());
      // The code for super is a little tricky to prevent collision with
      // the structure definition in the header. The rewriter has it's own
      // internal definition (__rw_objc_super) that is uses. This is why
      // we need the cast below. For example:
      // (struct objc_super *)&__rw_objc_super((id)self, (id)objc_getClass("SUPER"))
      //
      SuperRep = new (Context) UnaryOperator(SuperRep, UO_AddrOf,
                               Context->getPointerType(SuperRep->getType()),
                                             VK_RValue, OK_Ordinary,
                                             SourceLocation());
      SuperRep = NoTypeInfoCStyleCastExpr(Context,
                                          Context->getPointerType(superType),
                                          CK_BitCast, SuperRep);
    } else {
      // (struct objc_super) { <exprs from above> }
      InitListExpr *ILE =
        new (Context) InitListExpr(*Context, SourceLocation(),
                                   &InitExprs[0], InitExprs.size(),
                                   SourceLocation());
      TypeSourceInfo *superTInfo
        = Context->getTrivialTypeSourceInfo(superType);
      SuperRep = new (Context) CompoundLiteralExpr(SourceLocation(), superTInfo,
                                                   superType, VK_LValue,
                                                   ILE, false);
      // struct objc_super *
      SuperRep = new (Context) UnaryOperator(SuperRep, UO_AddrOf,
                               Context->getPointerType(SuperRep->getType()),
                                             VK_RValue, OK_Ordinary,
                                             SourceLocation());
    }
    MsgExprs.push_back(SuperRep);
    break;
  }

  case ObjCMessageExpr::Class: {
    SmallVector<Expr*, 8> ClsExprs;
    QualType argType = Context->getPointerType(Context->CharTy);
    ObjCInterfaceDecl *Class
      = Exp->getClassReceiver()->getAs<ObjCObjectType>()->getInterface();
    IdentifierInfo *clsName = Class->getIdentifier();
    ClsExprs.push_back(StringLiteral::Create(*Context,
                                             clsName->getName(),
                                             StringLiteral::Ascii, false,
                                             argType, SourceLocation()));
    CallExpr *Cls = SynthesizeCallToFunctionDecl(GetClassFunctionDecl,
                                                 &ClsExprs[0],
                                                 ClsExprs.size(), 
                                                 StartLoc, EndLoc);
    MsgExprs.push_back(Cls);
    break;
  }

  case ObjCMessageExpr::SuperInstance:{
    MsgSendFlavor = MsgSendSuperFunctionDecl;
    if (MsgSendStretFlavor)
      MsgSendStretFlavor = MsgSendSuperStretFunctionDecl;
    assert(MsgSendFlavor && "MsgSendFlavor is NULL!");
    ObjCInterfaceDecl *ClassDecl = CurMethodDef->getClassInterface();
    SmallVector<Expr*, 4> InitExprs;

    InitExprs.push_back(
      NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                               CK_BitCast,
                   new (Context) DeclRefExpr(CurMethodDef->getSelfDecl(),
                                             false,
                                             Context->getObjCIdType(),
                                             VK_RValue, SourceLocation()))
                        ); // set the 'receiver'.
    
    // (id)class_getSuperclass((Class)objc_getClass("CurrentClass"))
    SmallVector<Expr*, 8> ClsExprs;
    QualType argType = Context->getPointerType(Context->CharTy);
    ClsExprs.push_back(StringLiteral::Create(*Context,
                                   ClassDecl->getIdentifier()->getName(),
                                   StringLiteral::Ascii, false, argType,
                                   SourceLocation()));
    CallExpr *Cls = SynthesizeCallToFunctionDecl(GetClassFunctionDecl,
                                                 &ClsExprs[0],
                                                 ClsExprs.size(), 
                                                 StartLoc, EndLoc);
    // (Class)objc_getClass("CurrentClass")
    CastExpr *ArgExpr = NoTypeInfoCStyleCastExpr(Context,
                                                 Context->getObjCClassType(),
                                                 CK_BitCast, Cls);
    ClsExprs.clear();
    ClsExprs.push_back(ArgExpr);
    Cls = SynthesizeCallToFunctionDecl(GetSuperClassFunctionDecl,
                                       &ClsExprs[0], ClsExprs.size(),
                                       StartLoc, EndLoc);
    
    // (id)class_getSuperclass((Class)objc_getClass("CurrentClass"))
    // To turn off a warning, type-cast to 'id'
    InitExprs.push_back(
      // set 'super class', using class_getSuperclass().
      NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                               CK_BitCast, Cls));
    // struct objc_super
    QualType superType = getSuperStructType();
    Expr *SuperRep;

    if (LangOpts.MicrosoftExt) {
      SynthSuperContructorFunctionDecl();
      // Simulate a contructor call...
      DeclRefExpr *DRE = new (Context) DeclRefExpr(SuperContructorFunctionDecl,
                                                   false, superType, VK_LValue,
                                                   SourceLocation());
      SuperRep = new (Context) CallExpr(*Context, DRE, &InitExprs[0],
                                        InitExprs.size(),
                                        superType, VK_LValue, SourceLocation());
      // The code for super is a little tricky to prevent collision with
      // the structure definition in the header. The rewriter has it's own
      // internal definition (__rw_objc_super) that is uses. This is why
      // we need the cast below. For example:
      // (struct objc_super *)&__rw_objc_super((id)self, (id)objc_getClass("SUPER"))
      //
      SuperRep = new (Context) UnaryOperator(SuperRep, UO_AddrOf,
                               Context->getPointerType(SuperRep->getType()),
                               VK_RValue, OK_Ordinary,
                               SourceLocation());
      SuperRep = NoTypeInfoCStyleCastExpr(Context,
                               Context->getPointerType(superType),
                               CK_BitCast, SuperRep);
    } else {
      // (struct objc_super) { <exprs from above> }
      InitListExpr *ILE =
        new (Context) InitListExpr(*Context, SourceLocation(),
                                   &InitExprs[0], InitExprs.size(),
                                   SourceLocation());
      TypeSourceInfo *superTInfo
        = Context->getTrivialTypeSourceInfo(superType);
      SuperRep = new (Context) CompoundLiteralExpr(SourceLocation(), superTInfo,
                                                   superType, VK_RValue, ILE,
                                                   false);
    }
    MsgExprs.push_back(SuperRep);
    break;
  }

  case ObjCMessageExpr::Instance: {
    // Remove all type-casts because it may contain objc-style types; e.g.
    // Foo<Proto> *.
    Expr *recExpr = Exp->getInstanceReceiver();
    while (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(recExpr))
      recExpr = CE->getSubExpr();
    CastKind CK = recExpr->getType()->isObjCObjectPointerType()
                    ? CK_BitCast : recExpr->getType()->isBlockPointerType()
                                     ? CK_BlockPointerToObjCPointerCast
                                     : CK_CPointerToObjCPointerCast;

    recExpr = NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                                       CK, recExpr);
    MsgExprs.push_back(recExpr);
    break;
  }
  }

  // Create a call to sel_registerName("selName"), it will be the 2nd argument.
  SmallVector<Expr*, 8> SelExprs;
  QualType argType = Context->getPointerType(Context->CharTy);
  SelExprs.push_back(StringLiteral::Create(*Context,
                                       Exp->getSelector().getAsString(),
                                       StringLiteral::Ascii, false,
                                       argType, SourceLocation()));
  CallExpr *SelExp = SynthesizeCallToFunctionDecl(SelGetUidFunctionDecl,
                                                 &SelExprs[0], SelExprs.size(),
                                                  StartLoc,
                                                  EndLoc);
  MsgExprs.push_back(SelExp);

  // Now push any user supplied arguments.
  for (unsigned i = 0; i < Exp->getNumArgs(); i++) {
    Expr *userExpr = Exp->getArg(i);
    // Make all implicit casts explicit...ICE comes in handy:-)
    if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(userExpr)) {
      // Reuse the ICE type, it is exactly what the doctor ordered.
      QualType type = ICE->getType();
      if (needToScanForQualifiers(type))
        type = Context->getObjCIdType();
      // Make sure we convert "type (^)(...)" to "type (*)(...)".
      (void)convertBlockPointerToFunctionPointer(type);
      const Expr *SubExpr = ICE->IgnoreParenImpCasts();
      CastKind CK;
      if (SubExpr->getType()->isIntegralType(*Context) && 
          type->isBooleanType()) {
        CK = CK_IntegralToBoolean;
      } else if (type->isObjCObjectPointerType()) {
        if (SubExpr->getType()->isBlockPointerType()) {
          CK = CK_BlockPointerToObjCPointerCast;
        } else if (SubExpr->getType()->isPointerType()) {
          CK = CK_CPointerToObjCPointerCast;
        } else {
          CK = CK_BitCast;
        }
      } else {
        CK = CK_BitCast;
      }

      userExpr = NoTypeInfoCStyleCastExpr(Context, type, CK, userExpr);
    }
    // Make id<P...> cast into an 'id' cast.
    else if (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(userExpr)) {
      if (CE->getType()->isObjCQualifiedIdType()) {
        while ((CE = dyn_cast<CStyleCastExpr>(userExpr)))
          userExpr = CE->getSubExpr();
        CastKind CK;
        if (userExpr->getType()->isIntegralType(*Context)) {
          CK = CK_IntegralToPointer;
        } else if (userExpr->getType()->isBlockPointerType()) {
          CK = CK_BlockPointerToObjCPointerCast;
        } else if (userExpr->getType()->isPointerType()) {
          CK = CK_CPointerToObjCPointerCast;
        } else {
          CK = CK_BitCast;
        }
        userExpr = NoTypeInfoCStyleCastExpr(Context, Context->getObjCIdType(),
                                            CK, userExpr);
      }
    }
    MsgExprs.push_back(userExpr);
    // We've transferred the ownership to MsgExprs. For now, we *don't* null
    // out the argument in the original expression (since we aren't deleting
    // the ObjCMessageExpr). See RewritePropertyOrImplicitSetter() usage for more info.
    //Exp->setArg(i, 0);
  }
  // Generate the funky cast.
  CastExpr *cast;
  SmallVector<QualType, 8> ArgTypes;
  QualType returnType;

  // Push 'id' and 'SEL', the 2 implicit arguments.
  if (MsgSendFlavor == MsgSendSuperFunctionDecl)
    ArgTypes.push_back(Context->getPointerType(getSuperStructType()));
  else
    ArgTypes.push_back(Context->getObjCIdType());
  ArgTypes.push_back(Context->getObjCSelType());
  if (ObjCMethodDecl *OMD = Exp->getMethodDecl()) {
    // Push any user argument types.
    for (ObjCMethodDecl::param_iterator PI = OMD->param_begin(),
         E = OMD->param_end(); PI != E; ++PI) {
      QualType t = (*PI)->getType()->isObjCQualifiedIdType()
                     ? Context->getObjCIdType()
                     : (*PI)->getType();
      // Make sure we convert "t (^)(...)" to "t (*)(...)".
      (void)convertBlockPointerToFunctionPointer(t);
      ArgTypes.push_back(t);
    }
    returnType = Exp->getType();
    convertToUnqualifiedObjCType(returnType);
    (void)convertBlockPointerToFunctionPointer(returnType);
  } else {
    returnType = Context->getObjCIdType();
  }
  // Get the type, we will need to reference it in a couple spots.
  QualType msgSendType = MsgSendFlavor->getType();

  // Create a reference to the objc_msgSend() declaration.
  DeclRefExpr *DRE = new (Context) DeclRefExpr(MsgSendFlavor, false, msgSendType,
                                               VK_LValue, SourceLocation());

  // Need to cast objc_msgSend to "void *" (to workaround a GCC bandaid).
  // If we don't do this cast, we get the following bizarre warning/note:
  // xx.m:13: warning: function called through a non-compatible type
  // xx.m:13: note: if this code is reached, the program will abort
  cast = NoTypeInfoCStyleCastExpr(Context,
                                  Context->getPointerType(Context->VoidTy),
                                  CK_BitCast, DRE);

  // Now do the "normal" pointer to function cast.
  QualType castType =
    getSimpleFunctionType(returnType, &ArgTypes[0], ArgTypes.size(),
      // If we don't have a method decl, force a variadic cast.
      Exp->getMethodDecl() ? Exp->getMethodDecl()->isVariadic() : true);
  castType = Context->getPointerType(castType);
  cast = NoTypeInfoCStyleCastExpr(Context, castType, CK_BitCast,
                                  cast);

  // Don't forget the parens to enforce the proper binding.
  ParenExpr *PE = new (Context) ParenExpr(StartLoc, EndLoc, cast);

  const FunctionType *FT = msgSendType->getAs<FunctionType>();
  CallExpr *CE = new (Context) CallExpr(*Context, PE, &MsgExprs[0],
                                        MsgExprs.size(),
                                        FT->getResultType(), VK_RValue,
                                        EndLoc);
  Stmt *ReplacingStmt = CE;
  if (MsgSendStretFlavor) {
    // We have the method which returns a struct/union. Must also generate
    // call to objc_msgSend_stret and hang both varieties on a conditional
    // expression which dictate which one to envoke depending on size of
    // method's return type.

    // Create a reference to the objc_msgSend_stret() declaration.
    DeclRefExpr *STDRE = new (Context) DeclRefExpr(MsgSendStretFlavor,
                                                   false, msgSendType,
                                                   VK_LValue, SourceLocation());
    // Need to cast objc_msgSend_stret to "void *" (see above comment).
    cast = NoTypeInfoCStyleCastExpr(Context,
                                    Context->getPointerType(Context->VoidTy),
                                    CK_BitCast, STDRE);
    // Now do the "normal" pointer to function cast.
    castType = getSimpleFunctionType(returnType, &ArgTypes[0], ArgTypes.size(),
      Exp->getMethodDecl() ? Exp->getMethodDecl()->isVariadic() : false);
    castType = Context->getPointerType(castType);
    cast = NoTypeInfoCStyleCastExpr(Context, castType, CK_BitCast,
                                    cast);

    // Don't forget the parens to enforce the proper binding.
    PE = new (Context) ParenExpr(SourceLocation(), SourceLocation(), cast);

    FT = msgSendType->getAs<FunctionType>();
    CallExpr *STCE = new (Context) CallExpr(*Context, PE, &MsgExprs[0],
                                            MsgExprs.size(),
                                            FT->getResultType(), VK_RValue,
                                            SourceLocation());

    // Build sizeof(returnType)
    UnaryExprOrTypeTraitExpr *sizeofExpr =
       new (Context) UnaryExprOrTypeTraitExpr(UETT_SizeOf,
                                 Context->getTrivialTypeSourceInfo(returnType),
                                 Context->getSizeType(), SourceLocation(),
                                 SourceLocation());
    // (sizeof(returnType) <= 8 ? objc_msgSend(...) : objc_msgSend_stret(...))
    // FIXME: Value of 8 is base on ppc32/x86 ABI for the most common cases.
    // For X86 it is more complicated and some kind of target specific routine
    // is needed to decide what to do.
    unsigned IntSize =
      static_cast<unsigned>(Context->getTypeSize(Context->IntTy));
    IntegerLiteral *limit = IntegerLiteral::Create(*Context,
                                                   llvm::APInt(IntSize, 8),
                                                   Context->IntTy,
                                                   SourceLocation());
    BinaryOperator *lessThanExpr = 
      new (Context) BinaryOperator(sizeofExpr, limit, BO_LE, Context->IntTy,
                                   VK_RValue, OK_Ordinary, SourceLocation());
    // (sizeof(returnType) <= 8 ? objc_msgSend(...) : objc_msgSend_stret(...))
    ConditionalOperator *CondExpr =
      new (Context) ConditionalOperator(lessThanExpr,
                                        SourceLocation(), CE,
                                        SourceLocation(), STCE,
                                        returnType, VK_RValue, OK_Ordinary);
    ReplacingStmt = new (Context) ParenExpr(SourceLocation(), SourceLocation(), 
                                            CondExpr);
  }
  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return ReplacingStmt;
}

Stmt *RewriteModernObjC::RewriteMessageExpr(ObjCMessageExpr *Exp) {
  Stmt *ReplacingStmt = SynthMessageExpr(Exp, Exp->getLocStart(),
                                         Exp->getLocEnd());

  // Now do the actual rewrite.
  ReplaceStmt(Exp, ReplacingStmt);

  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return ReplacingStmt;
}

// typedef struct objc_object Protocol;
QualType RewriteModernObjC::getProtocolType() {
  if (!ProtocolTypeDecl) {
    TypeSourceInfo *TInfo
      = Context->getTrivialTypeSourceInfo(Context->getObjCIdType());
    ProtocolTypeDecl = TypedefDecl::Create(*Context, TUDecl,
                                           SourceLocation(), SourceLocation(),
                                           &Context->Idents.get("Protocol"),
                                           TInfo);
  }
  return Context->getTypeDeclType(ProtocolTypeDecl);
}

/// RewriteObjCProtocolExpr - Rewrite a protocol expression into
/// a synthesized/forward data reference (to the protocol's metadata).
/// The forward references (and metadata) are generated in
/// RewriteModernObjC::HandleTranslationUnit().
Stmt *RewriteModernObjC::RewriteObjCProtocolExpr(ObjCProtocolExpr *Exp) {
  std::string Name = "_OBJC_PROTOCOL_" + Exp->getProtocol()->getNameAsString();
  IdentifierInfo *ID = &Context->Idents.get(Name);
  VarDecl *VD = VarDecl::Create(*Context, TUDecl, SourceLocation(),
                                SourceLocation(), ID, getProtocolType(), 0,
                                SC_Extern, SC_None);
  DeclRefExpr *DRE = new (Context) DeclRefExpr(VD, false, getProtocolType(),
                                               VK_LValue, SourceLocation());
  Expr *DerefExpr = new (Context) UnaryOperator(DRE, UO_AddrOf,
                             Context->getPointerType(DRE->getType()),
                             VK_RValue, OK_Ordinary, SourceLocation());
  CastExpr *castExpr = NoTypeInfoCStyleCastExpr(Context, DerefExpr->getType(),
                                                CK_BitCast,
                                                DerefExpr);
  ReplaceStmt(Exp, castExpr);
  ProtocolExprDecls.insert(Exp->getProtocol()->getCanonicalDecl());
  // delete Exp; leak for now, see RewritePropertyOrImplicitSetter() usage for more info.
  return castExpr;

}

bool RewriteModernObjC::BufferContainsPPDirectives(const char *startBuf,
                                             const char *endBuf) {
  while (startBuf < endBuf) {
    if (*startBuf == '#') {
      // Skip whitespace.
      for (++startBuf; startBuf[0] == ' ' || startBuf[0] == '\t'; ++startBuf)
        ;
      if (!strncmp(startBuf, "if", strlen("if")) ||
          !strncmp(startBuf, "ifdef", strlen("ifdef")) ||
          !strncmp(startBuf, "ifndef", strlen("ifndef")) ||
          !strncmp(startBuf, "define", strlen("define")) ||
          !strncmp(startBuf, "undef", strlen("undef")) ||
          !strncmp(startBuf, "else", strlen("else")) ||
          !strncmp(startBuf, "elif", strlen("elif")) ||
          !strncmp(startBuf, "endif", strlen("endif")) ||
          !strncmp(startBuf, "pragma", strlen("pragma")) ||
          !strncmp(startBuf, "include", strlen("include")) ||
          !strncmp(startBuf, "import", strlen("import")) ||
          !strncmp(startBuf, "include_next", strlen("include_next")))
        return true;
    }
    startBuf++;
  }
  return false;
}

/// RewriteObjCFieldDeclType - This routine rewrites a type into the buffer.
/// It handles elaborated types, as well as enum types in the process.
bool RewriteModernObjC::RewriteObjCFieldDeclType(QualType &Type, 
                                                 std::string &Result) {
  if (Type->isArrayType()) {
    QualType ElemTy = Context->getBaseElementType(Type);
    return RewriteObjCFieldDeclType(ElemTy, Result);
  }
  else if (Type->isRecordType()) {
    RecordDecl *RD = Type->getAs<RecordType>()->getDecl();
    if (RD->isCompleteDefinition()) {
      if (RD->isStruct())
        Result += "\n\tstruct ";
      else if (RD->isUnion())
        Result += "\n\tunion ";
      else
        assert(false && "class not allowed as an ivar type");
      
      Result += RD->getName();
      if (TagsDefinedInIvarDecls.count(RD)) {
        // This struct is already defined. Do not write its definition again.
        Result += " ";
        return true;
      }
      TagsDefinedInIvarDecls.insert(RD);
      Result += " {\n";
      for (RecordDecl::field_iterator i = RD->field_begin(), 
           e = RD->field_end(); i != e; ++i) {
        FieldDecl *FD = *i;
        RewriteObjCFieldDecl(FD, Result);
      }
      Result += "\t} "; 
      return true;
    }
  }
  else if (Type->isEnumeralType()) {
    EnumDecl *ED = Type->getAs<EnumType>()->getDecl();
    if (ED->isCompleteDefinition()) {
      Result += "\n\tenum ";
      Result += ED->getName();
      if (TagsDefinedInIvarDecls.count(ED)) {
        // This enum is already defined. Do not write its definition again.
        Result += " ";
        return true;
      }
      TagsDefinedInIvarDecls.insert(ED);
      
      Result += " {\n";
      for (EnumDecl::enumerator_iterator EC = ED->enumerator_begin(),
           ECEnd = ED->enumerator_end(); EC != ECEnd; ++EC) {
        Result += "\t"; Result += EC->getName(); Result += " = ";
        llvm::APSInt Val = EC->getInitVal();
        Result += Val.toString(10);
        Result += ",\n";
      }
      Result += "\t} "; 
      return true;
    }
  }
  
  Result += "\t";
  convertObjCTypeToCStyleType(Type);
  return false;
}


/// RewriteObjCFieldDecl - This routine rewrites a field into the buffer.
/// It handles elaborated types, as well as enum types in the process.
void RewriteModernObjC::RewriteObjCFieldDecl(FieldDecl *fieldDecl, 
                                             std::string &Result) {
  QualType Type = fieldDecl->getType();
  std::string Name = fieldDecl->getNameAsString();
  
  bool EleboratedType = RewriteObjCFieldDeclType(Type, Result); 
  if (!EleboratedType)
    Type.getAsStringInternal(Name, Context->getPrintingPolicy());
  Result += Name;
  if (fieldDecl->isBitField()) {
    Result += " : "; Result += utostr(fieldDecl->getBitWidthValue(*Context));
  }
  else if (EleboratedType && Type->isArrayType()) {
    CanQualType CType = Context->getCanonicalType(Type);
    while (isa<ArrayType>(CType)) {
      if (const ConstantArrayType *CAT = Context->getAsConstantArrayType(CType)) {
        Result += "[";
        llvm::APInt Dim = CAT->getSize();
        Result += utostr(Dim.getZExtValue());
        Result += "]";
      }
      CType = CType->getAs<ArrayType>()->getElementType();
    }
  }
  
  Result += ";\n";
}

/// RewriteObjCInternalStruct - Rewrite one internal struct corresponding to
/// an objective-c class with ivars.
void RewriteModernObjC::RewriteObjCInternalStruct(ObjCInterfaceDecl *CDecl,
                                               std::string &Result) {
  assert(CDecl && "Class missing in SynthesizeObjCInternalStruct");
  assert(CDecl->getName() != "" &&
         "Name missing in SynthesizeObjCInternalStruct");
  ObjCInterfaceDecl *RCDecl = CDecl->getSuperClass();
  SmallVector<ObjCIvarDecl *, 8> IVars;
  for (ObjCIvarDecl *IVD = CDecl->all_declared_ivar_begin();
       IVD; IVD = IVD->getNextIvar())
    IVars.push_back(IVD);
  
  SourceLocation LocStart = CDecl->getLocStart();
  SourceLocation LocEnd = CDecl->getEndOfDefinitionLoc();
  
  const char *startBuf = SM->getCharacterData(LocStart);
  const char *endBuf = SM->getCharacterData(LocEnd);
  
  // If no ivars and no root or if its root, directly or indirectly,
  // have no ivars (thus not synthesized) then no need to synthesize this class.
  if ((!CDecl->isThisDeclarationADefinition() || IVars.size() == 0) &&
      (!RCDecl || !ObjCSynthesizedStructs.count(RCDecl))) {
    endBuf += Lexer::MeasureTokenLength(LocEnd, *SM, LangOpts);
    ReplaceText(LocStart, endBuf-startBuf, Result);
    return;
  }
  
  Result += "\nstruct ";
  Result += CDecl->getNameAsString();
  Result += "_IMPL {\n";
  
  if (RCDecl && ObjCSynthesizedStructs.count(RCDecl)) {
    Result += "\tstruct "; Result += RCDecl->getNameAsString();
    Result += "_IMPL "; Result += RCDecl->getNameAsString();
    Result += "_IVARS;\n";
  }
  TagsDefinedInIvarDecls.clear();
  for (unsigned i = 0, e = IVars.size(); i < e; i++)
    RewriteObjCFieldDecl(IVars[i], Result);

  Result += "};\n";
  endBuf += Lexer::MeasureTokenLength(LocEnd, *SM, LangOpts);
  ReplaceText(LocStart, endBuf-startBuf, Result);
  // Mark this struct as having been generated.
  if (!ObjCSynthesizedStructs.insert(CDecl))
    llvm_unreachable("struct already synthesize- RewriteObjCInternalStruct");
}

/// RewriteIvarOffsetSymbols - Rewrite ivar offset symbols of those ivars which
/// have been referenced in an ivar access expression.
void RewriteModernObjC::RewriteIvarOffsetSymbols(ObjCInterfaceDecl *CDecl,
                                                  std::string &Result) {
  // write out ivar offset symbols which have been referenced in an ivar
  // access expression.
  llvm::SmallPtrSet<ObjCIvarDecl *, 8> Ivars = ReferencedIvars[CDecl];
  if (Ivars.empty())
    return;
  for (llvm::SmallPtrSet<ObjCIvarDecl *, 8>::iterator i = Ivars.begin(),
       e = Ivars.end(); i != e; i++) {
    ObjCIvarDecl *IvarDecl = (*i);
    Result += "\nextern unsigned long OBJC_IVAR_$_";
    Result += CDecl->getName(); Result += "_";
    Result += IvarDecl->getName(); Result += ";";
  }
}

//===----------------------------------------------------------------------===//
// Meta Data Emission
//===----------------------------------------------------------------------===//


/// RewriteImplementations - This routine rewrites all method implementations
/// and emits meta-data.

void RewriteModernObjC::RewriteImplementations() {
  int ClsDefCount = ClassImplementation.size();
  int CatDefCount = CategoryImplementation.size();

  // Rewrite implemented methods
  for (int i = 0; i < ClsDefCount; i++) {
    ObjCImplementationDecl *OIMP = ClassImplementation[i];
    ObjCInterfaceDecl *CDecl = OIMP->getClassInterface();
    if (CDecl->isImplicitInterfaceDecl())
      assert(false &&
             "Legacy implicit interface rewriting not supported in moder abi");
    RewriteImplementationDecl(OIMP);
  }

  for (int i = 0; i < CatDefCount; i++) {
    ObjCCategoryImplDecl *CIMP = CategoryImplementation[i];
    ObjCInterfaceDecl *CDecl = CIMP->getClassInterface();
    if (CDecl->isImplicitInterfaceDecl())
      assert(false &&
             "Legacy implicit interface rewriting not supported in moder abi");
    RewriteImplementationDecl(CIMP);
  }
}

void RewriteModernObjC::RewriteByRefString(std::string &ResultStr, 
                                     const std::string &Name,
                                     ValueDecl *VD, bool def) {
  assert(BlockByRefDeclNo.count(VD) && 
         "RewriteByRefString: ByRef decl missing");
  if (def)
    ResultStr += "struct ";
  ResultStr += "__Block_byref_" + Name + 
    "_" + utostr(BlockByRefDeclNo[VD]) ;
}

static bool HasLocalVariableExternalStorage(ValueDecl *VD) {
  if (VarDecl *Var = dyn_cast<VarDecl>(VD))
    return (Var->isFunctionOrMethodVarDecl() && !Var->hasLocalStorage());
  return false;
}

std::string RewriteModernObjC::SynthesizeBlockFunc(BlockExpr *CE, int i,
                                                   StringRef funcName,
                                                   std::string Tag) {
  const FunctionType *AFT = CE->getFunctionType();
  QualType RT = AFT->getResultType();
  std::string StructRef = "struct " + Tag;
  std::string S = "static " + RT.getAsString(Context->getPrintingPolicy()) + " __" +
                  funcName.str() + "_" + "block_func_" + utostr(i);

  BlockDecl *BD = CE->getBlockDecl();

  if (isa<FunctionNoProtoType>(AFT)) {
    // No user-supplied arguments. Still need to pass in a pointer to the
    // block (to reference imported block decl refs).
    S += "(" + StructRef + " *__cself)";
  } else if (BD->param_empty()) {
    S += "(" + StructRef + " *__cself)";
  } else {
    const FunctionProtoType *FT = cast<FunctionProtoType>(AFT);
    assert(FT && "SynthesizeBlockFunc: No function proto");
    S += '(';
    // first add the implicit argument.
    S += StructRef + " *__cself, ";
    std::string ParamStr;
    for (BlockDecl::param_iterator AI = BD->param_begin(),
         E = BD->param_end(); AI != E; ++AI) {
      if (AI != BD->param_begin()) S += ", ";
      ParamStr = (*AI)->getNameAsString();
      QualType QT = (*AI)->getType();
      if (convertBlockPointerToFunctionPointer(QT))
        QT.getAsStringInternal(ParamStr, Context->getPrintingPolicy());
      else
        QT.getAsStringInternal(ParamStr, Context->getPrintingPolicy());      
      S += ParamStr;
    }
    if (FT->isVariadic()) {
      if (!BD->param_empty()) S += ", ";
      S += "...";
    }
    S += ')';
  }
  S += " {\n";

  // Create local declarations to avoid rewriting all closure decl ref exprs.
  // First, emit a declaration for all "by ref" decls.
  for (SmallVector<ValueDecl*,8>::iterator I = BlockByRefDecls.begin(),
       E = BlockByRefDecls.end(); I != E; ++I) {
    S += "  ";
    std::string Name = (*I)->getNameAsString();
    std::string TypeString;
    RewriteByRefString(TypeString, Name, (*I));
    TypeString += " *";
    Name = TypeString + Name;
    S += Name + " = __cself->" + (*I)->getNameAsString() + "; // bound by ref\n";
  }
  // Next, emit a declaration for all "by copy" declarations.
  for (SmallVector<ValueDecl*,8>::iterator I = BlockByCopyDecls.begin(),
       E = BlockByCopyDecls.end(); I != E; ++I) {
    S += "  ";
    // Handle nested closure invocation. For example:
    //
    //   void (^myImportedClosure)(void);
    //   myImportedClosure  = ^(void) { setGlobalInt(x + y); };
    //
    //   void (^anotherClosure)(void);
    //   anotherClosure = ^(void) {
    //     myImportedClosure(); // import and invoke the closure
    //   };
    //
    if (isTopLevelBlockPointerType((*I)->getType())) {
      RewriteBlockPointerTypeVariable(S, (*I));
      S += " = (";
      RewriteBlockPointerType(S, (*I)->getType());
      S += ")";
      S += "__cself->" + (*I)->getNameAsString() + "; // bound by copy\n";
    }
    else {
      std::string Name = (*I)->getNameAsString();
      QualType QT = (*I)->getType();
      if (HasLocalVariableExternalStorage(*I))
        QT = Context->getPointerType(QT);
      QT.getAsStringInternal(Name, Context->getPrintingPolicy());
      S += Name + " = __cself->" + 
                              (*I)->getNameAsString() + "; // bound by copy\n";
    }
  }
  std::string RewrittenStr = RewrittenBlockExprs[CE];
  const char *cstr = RewrittenStr.c_str();
  while (*cstr++ != '{') ;
  S += cstr;
  S += "\n";
  return S;
}

std::string RewriteModernObjC::SynthesizeBlockHelperFuncs(BlockExpr *CE, int i,
                                                   StringRef funcName,
                                                   std::string Tag) {
  std::string StructRef = "struct " + Tag;
  std::string S = "static void __";

  S += funcName;
  S += "_block_copy_" + utostr(i);
  S += "(" + StructRef;
  S += "*dst, " + StructRef;
  S += "*src) {";
  for (llvm::SmallPtrSet<ValueDecl*,8>::iterator I = ImportedBlockDecls.begin(),
      E = ImportedBlockDecls.end(); I != E; ++I) {
    ValueDecl *VD = (*I);
    S += "_Block_object_assign((void*)&dst->";
    S += (*I)->getNameAsString();
    S += ", (void*)src->";
    S += (*I)->getNameAsString();
    if (BlockByRefDeclsPtrSet.count((*I)))
      S += ", " + utostr(BLOCK_FIELD_IS_BYREF) + "/*BLOCK_FIELD_IS_BYREF*/);";
    else if (VD->getType()->isBlockPointerType())
      S += ", " + utostr(BLOCK_FIELD_IS_BLOCK) + "/*BLOCK_FIELD_IS_BLOCK*/);";
    else
      S += ", " + utostr(BLOCK_FIELD_IS_OBJECT) + "/*BLOCK_FIELD_IS_OBJECT*/);";
  }
  S += "}\n";
  
  S += "\nstatic void __";
  S += funcName;
  S += "_block_dispose_" + utostr(i);
  S += "(" + StructRef;
  S += "*src) {";
  for (llvm::SmallPtrSet<ValueDecl*,8>::iterator I = ImportedBlockDecls.begin(),
      E = ImportedBlockDecls.end(); I != E; ++I) {
    ValueDecl *VD = (*I);
    S += "_Block_object_dispose((void*)src->";
    S += (*I)->getNameAsString();
    if (BlockByRefDeclsPtrSet.count((*I)))
      S += ", " + utostr(BLOCK_FIELD_IS_BYREF) + "/*BLOCK_FIELD_IS_BYREF*/);";
    else if (VD->getType()->isBlockPointerType())
      S += ", " + utostr(BLOCK_FIELD_IS_BLOCK) + "/*BLOCK_FIELD_IS_BLOCK*/);";
    else
      S += ", " + utostr(BLOCK_FIELD_IS_OBJECT) + "/*BLOCK_FIELD_IS_OBJECT*/);";
  }
  S += "}\n";
  return S;
}

std::string RewriteModernObjC::SynthesizeBlockImpl(BlockExpr *CE, std::string Tag, 
                                             std::string Desc) {
  std::string S = "\nstruct " + Tag;
  std::string Constructor = "  " + Tag;

  S += " {\n  struct __block_impl impl;\n";
  S += "  struct " + Desc;
  S += "* Desc;\n";

  Constructor += "(void *fp, "; // Invoke function pointer.
  Constructor += "struct " + Desc; // Descriptor pointer.
  Constructor += " *desc";

  if (BlockDeclRefs.size()) {
    // Output all "by copy" declarations.
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByCopyDecls.begin(),
         E = BlockByCopyDecls.end(); I != E; ++I) {
      S += "  ";
      std::string FieldName = (*I)->getNameAsString();
      std::string ArgName = "_" + FieldName;
      // Handle nested closure invocation. For example:
      //
      //   void (^myImportedBlock)(void);
      //   myImportedBlock  = ^(void) { setGlobalInt(x + y); };
      //
      //   void (^anotherBlock)(void);
      //   anotherBlock = ^(void) {
      //     myImportedBlock(); // import and invoke the closure
      //   };
      //
      if (isTopLevelBlockPointerType((*I)->getType())) {
        S += "struct __block_impl *";
        Constructor += ", void *" + ArgName;
      } else {
        QualType QT = (*I)->getType();
        if (HasLocalVariableExternalStorage(*I))
          QT = Context->getPointerType(QT);
        QT.getAsStringInternal(FieldName, Context->getPrintingPolicy());
        QT.getAsStringInternal(ArgName, Context->getPrintingPolicy());
        Constructor += ", " + ArgName;
      }
      S += FieldName + ";\n";
    }
    // Output all "by ref" declarations.
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByRefDecls.begin(),
         E = BlockByRefDecls.end(); I != E; ++I) {
      S += "  ";
      std::string FieldName = (*I)->getNameAsString();
      std::string ArgName = "_" + FieldName;
      {
        std::string TypeString;
        RewriteByRefString(TypeString, FieldName, (*I));
        TypeString += " *";
        FieldName = TypeString + FieldName;
        ArgName = TypeString + ArgName;
        Constructor += ", " + ArgName;
      }
      S += FieldName + "; // by ref\n";
    }
    // Finish writing the constructor.
    Constructor += ", int flags=0)";
    // Initialize all "by copy" arguments.
    bool firsTime = true;
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByCopyDecls.begin(),
         E = BlockByCopyDecls.end(); I != E; ++I) {
      std::string Name = (*I)->getNameAsString();
        if (firsTime) {
          Constructor += " : ";
          firsTime = false;
        }
        else
          Constructor += ", ";
        if (isTopLevelBlockPointerType((*I)->getType()))
          Constructor += Name + "((struct __block_impl *)_" + Name + ")";
        else
          Constructor += Name + "(_" + Name + ")";
    }
    // Initialize all "by ref" arguments.
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByRefDecls.begin(),
         E = BlockByRefDecls.end(); I != E; ++I) {
      std::string Name = (*I)->getNameAsString();
      if (firsTime) {
        Constructor += " : ";
        firsTime = false;
      }
      else
        Constructor += ", ";
      Constructor += Name + "(_" + Name + "->__forwarding)";
    }
    
    Constructor += " {\n";
    if (GlobalVarDecl)
      Constructor += "    impl.isa = &_NSConcreteGlobalBlock;\n";
    else
      Constructor += "    impl.isa = &_NSConcreteStackBlock;\n";
    Constructor += "    impl.Flags = flags;\n    impl.FuncPtr = fp;\n";

    Constructor += "    Desc = desc;\n";
  } else {
    // Finish writing the constructor.
    Constructor += ", int flags=0) {\n";
    if (GlobalVarDecl)
      Constructor += "    impl.isa = &_NSConcreteGlobalBlock;\n";
    else
      Constructor += "    impl.isa = &_NSConcreteStackBlock;\n";
    Constructor += "    impl.Flags = flags;\n    impl.FuncPtr = fp;\n";
    Constructor += "    Desc = desc;\n";
  }
  Constructor += "  ";
  Constructor += "}\n";
  S += Constructor;
  S += "};\n";
  return S;
}

std::string RewriteModernObjC::SynthesizeBlockDescriptor(std::string DescTag, 
                                                   std::string ImplTag, int i,
                                                   StringRef FunName,
                                                   unsigned hasCopy) {
  std::string S = "\nstatic struct " + DescTag;
  
  S += " {\n  unsigned long reserved;\n";
  S += "  unsigned long Block_size;\n";
  if (hasCopy) {
    S += "  void (*copy)(struct ";
    S += ImplTag; S += "*, struct ";
    S += ImplTag; S += "*);\n";
    
    S += "  void (*dispose)(struct ";
    S += ImplTag; S += "*);\n";
  }
  S += "} ";

  S += DescTag + "_DATA = { 0, sizeof(struct ";
  S += ImplTag + ")";
  if (hasCopy) {
    S += ", __" + FunName.str() + "_block_copy_" + utostr(i);
    S += ", __" + FunName.str() + "_block_dispose_" + utostr(i);
  }
  S += "};\n";
  return S;
}

void RewriteModernObjC::SynthesizeBlockLiterals(SourceLocation FunLocStart,
                                          StringRef FunName) {
  // Insert declaration for the function in which block literal is used.
  if (CurFunctionDeclToDeclareForBlock && !Blocks.empty())
    RewriteBlockLiteralFunctionDecl(CurFunctionDeclToDeclareForBlock);
  bool RewriteSC = (GlobalVarDecl &&
                    !Blocks.empty() &&
                    GlobalVarDecl->getStorageClass() == SC_Static &&
                    GlobalVarDecl->getType().getCVRQualifiers());
  if (RewriteSC) {
    std::string SC(" void __");
    SC += GlobalVarDecl->getNameAsString();
    SC += "() {}";
    InsertText(FunLocStart, SC);
  }
  
  // Insert closures that were part of the function.
  for (unsigned i = 0, count=0; i < Blocks.size(); i++) {
    CollectBlockDeclRefInfo(Blocks[i]);
    // Need to copy-in the inner copied-in variables not actually used in this
    // block.
    for (int j = 0; j < InnerDeclRefsCount[i]; j++) {
      DeclRefExpr *Exp = InnerDeclRefs[count++];
      ValueDecl *VD = Exp->getDecl();
      BlockDeclRefs.push_back(Exp);
      if (!VD->hasAttr<BlocksAttr>()) {
        if (!BlockByCopyDeclsPtrSet.count(VD)) {
          BlockByCopyDeclsPtrSet.insert(VD);
          BlockByCopyDecls.push_back(VD);
        }
        continue;
      }

      if (!BlockByRefDeclsPtrSet.count(VD)) {
        BlockByRefDeclsPtrSet.insert(VD);
        BlockByRefDecls.push_back(VD);
      }

      // imported objects in the inner blocks not used in the outer
      // blocks must be copied/disposed in the outer block as well.
      if (VD->getType()->isObjCObjectPointerType() || 
          VD->getType()->isBlockPointerType())
        ImportedBlockDecls.insert(VD);
    }

    std::string ImplTag = "__" + FunName.str() + "_block_impl_" + utostr(i);
    std::string DescTag = "__" + FunName.str() + "_block_desc_" + utostr(i);

    std::string CI = SynthesizeBlockImpl(Blocks[i], ImplTag, DescTag);

    InsertText(FunLocStart, CI);

    std::string CF = SynthesizeBlockFunc(Blocks[i], i, FunName, ImplTag);

    InsertText(FunLocStart, CF);

    if (ImportedBlockDecls.size()) {
      std::string HF = SynthesizeBlockHelperFuncs(Blocks[i], i, FunName, ImplTag);
      InsertText(FunLocStart, HF);
    }
    std::string BD = SynthesizeBlockDescriptor(DescTag, ImplTag, i, FunName,
                                               ImportedBlockDecls.size() > 0);
    InsertText(FunLocStart, BD);

    BlockDeclRefs.clear();
    BlockByRefDecls.clear();
    BlockByRefDeclsPtrSet.clear();
    BlockByCopyDecls.clear();
    BlockByCopyDeclsPtrSet.clear();
    ImportedBlockDecls.clear();
  }
  if (RewriteSC) {
    // Must insert any 'const/volatile/static here. Since it has been
    // removed as result of rewriting of block literals.
    std::string SC;
    if (GlobalVarDecl->getStorageClass() == SC_Static)
      SC = "static ";
    if (GlobalVarDecl->getType().isConstQualified())
      SC += "const ";
    if (GlobalVarDecl->getType().isVolatileQualified())
      SC += "volatile ";
    if (GlobalVarDecl->getType().isRestrictQualified())
      SC += "restrict ";
    InsertText(FunLocStart, SC);
  }
  
  Blocks.clear();
  InnerDeclRefsCount.clear();
  InnerDeclRefs.clear();
  RewrittenBlockExprs.clear();
}

void RewriteModernObjC::InsertBlockLiteralsWithinFunction(FunctionDecl *FD) {
  SourceLocation FunLocStart = FD->getTypeSpecStartLoc();
  StringRef FuncName = FD->getName();

  SynthesizeBlockLiterals(FunLocStart, FuncName);
}

static void BuildUniqueMethodName(std::string &Name,
                                  ObjCMethodDecl *MD) {
  ObjCInterfaceDecl *IFace = MD->getClassInterface();
  Name = IFace->getName();
  Name += "__" + MD->getSelector().getAsString();
  // Convert colons to underscores.
  std::string::size_type loc = 0;
  while ((loc = Name.find(":", loc)) != std::string::npos)
    Name.replace(loc, 1, "_");
}

void RewriteModernObjC::InsertBlockLiteralsWithinMethod(ObjCMethodDecl *MD) {
  //fprintf(stderr,"In InsertBlockLiteralsWitinMethod\n");
  //SourceLocation FunLocStart = MD->getLocStart();
  SourceLocation FunLocStart = MD->getLocStart();
  std::string FuncName;
  BuildUniqueMethodName(FuncName, MD);
  SynthesizeBlockLiterals(FunLocStart, FuncName);
}

void RewriteModernObjC::GetBlockDeclRefExprs(Stmt *S) {
  for (Stmt::child_range CI = S->children(); CI; ++CI)
    if (*CI) {
      if (BlockExpr *CBE = dyn_cast<BlockExpr>(*CI))
        GetBlockDeclRefExprs(CBE->getBody());
      else
        GetBlockDeclRefExprs(*CI);
    }
  // Handle specific things.
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S))
    if (DRE->refersToEnclosingLocal() &&
        HasLocalVariableExternalStorage(DRE->getDecl())) {
      BlockDeclRefs.push_back(DRE);
    }
  
  return;
}

void RewriteModernObjC::GetInnerBlockDeclRefExprs(Stmt *S, 
                SmallVector<DeclRefExpr *, 8> &InnerBlockDeclRefs,
                llvm::SmallPtrSet<const DeclContext *, 8> &InnerContexts) {
  for (Stmt::child_range CI = S->children(); CI; ++CI)
    if (*CI) {
      if (BlockExpr *CBE = dyn_cast<BlockExpr>(*CI)) {
        InnerContexts.insert(cast<DeclContext>(CBE->getBlockDecl()));
        GetInnerBlockDeclRefExprs(CBE->getBody(),
                                  InnerBlockDeclRefs,
                                  InnerContexts);
      }
      else
        GetInnerBlockDeclRefExprs(*CI,
                                  InnerBlockDeclRefs,
                                  InnerContexts);

    }
  // Handle specific things.
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (DRE->refersToEnclosingLocal()) {
      if (!isa<FunctionDecl>(DRE->getDecl()) &&
          !InnerContexts.count(DRE->getDecl()->getDeclContext()))
        InnerBlockDeclRefs.push_back(DRE);
      if (VarDecl *Var = dyn_cast<VarDecl>(DRE->getDecl()))
        if (Var->isFunctionOrMethodVarDecl())
          ImportedLocalExternalDecls.insert(Var);
    }
  }
  
  return;
}

/// convertObjCTypeToCStyleType - This routine converts such objc types
/// as qualified objects, and blocks to their closest c/c++ types that
/// it can. It returns true if input type was modified.
bool RewriteModernObjC::convertObjCTypeToCStyleType(QualType &T) {
  QualType oldT = T;
  convertBlockPointerToFunctionPointer(T);
  if (T->isFunctionPointerType()) {
    QualType PointeeTy;
    if (const PointerType* PT = T->getAs<PointerType>()) {
      PointeeTy = PT->getPointeeType();
      if (const FunctionType *FT = PointeeTy->getAs<FunctionType>()) {
        T = convertFunctionTypeOfBlocks(FT);
        T = Context->getPointerType(T);
      }
    }
  }
  
  convertToUnqualifiedObjCType(T);
  return T != oldT;
}

/// convertFunctionTypeOfBlocks - This routine converts a function type
/// whose result type may be a block pointer or whose argument type(s)
/// might be block pointers to an equivalent function type replacing
/// all block pointers to function pointers.
QualType RewriteModernObjC::convertFunctionTypeOfBlocks(const FunctionType *FT) {
  const FunctionProtoType *FTP = dyn_cast<FunctionProtoType>(FT);
  // FTP will be null for closures that don't take arguments.
  // Generate a funky cast.
  SmallVector<QualType, 8> ArgTypes;
  QualType Res = FT->getResultType();
  bool modified = convertObjCTypeToCStyleType(Res);
  
  if (FTP) {
    for (FunctionProtoType::arg_type_iterator I = FTP->arg_type_begin(),
         E = FTP->arg_type_end(); I && (I != E); ++I) {
      QualType t = *I;
      // Make sure we convert "t (^)(...)" to "t (*)(...)".
      if (convertObjCTypeToCStyleType(t))
        modified = true;
      ArgTypes.push_back(t);
    }
  }
  QualType FuncType;
  if (modified)
    FuncType = getSimpleFunctionType(Res, &ArgTypes[0], ArgTypes.size());
  else FuncType = QualType(FT, 0);
  return FuncType;
}

Stmt *RewriteModernObjC::SynthesizeBlockCall(CallExpr *Exp, const Expr *BlockExp) {
  // Navigate to relevant type information.
  const BlockPointerType *CPT = 0;

  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(BlockExp)) {
    CPT = DRE->getType()->getAs<BlockPointerType>();
  } else if (const MemberExpr *MExpr = dyn_cast<MemberExpr>(BlockExp)) {
    CPT = MExpr->getType()->getAs<BlockPointerType>();
  } 
  else if (const ParenExpr *PRE = dyn_cast<ParenExpr>(BlockExp)) {
    return SynthesizeBlockCall(Exp, PRE->getSubExpr());
  }
  else if (const ImplicitCastExpr *IEXPR = dyn_cast<ImplicitCastExpr>(BlockExp)) 
    CPT = IEXPR->getType()->getAs<BlockPointerType>();
  else if (const ConditionalOperator *CEXPR = 
            dyn_cast<ConditionalOperator>(BlockExp)) {
    Expr *LHSExp = CEXPR->getLHS();
    Stmt *LHSStmt = SynthesizeBlockCall(Exp, LHSExp);
    Expr *RHSExp = CEXPR->getRHS();
    Stmt *RHSStmt = SynthesizeBlockCall(Exp, RHSExp);
    Expr *CONDExp = CEXPR->getCond();
    ConditionalOperator *CondExpr =
      new (Context) ConditionalOperator(CONDExp,
                                      SourceLocation(), cast<Expr>(LHSStmt),
                                      SourceLocation(), cast<Expr>(RHSStmt),
                                      Exp->getType(), VK_RValue, OK_Ordinary);
    return CondExpr;
  } else if (const ObjCIvarRefExpr *IRE = dyn_cast<ObjCIvarRefExpr>(BlockExp)) {
    CPT = IRE->getType()->getAs<BlockPointerType>();
  } else if (const PseudoObjectExpr *POE
               = dyn_cast<PseudoObjectExpr>(BlockExp)) {
    CPT = POE->getType()->castAs<BlockPointerType>();
  } else {
    assert(1 && "RewriteBlockClass: Bad type");
  }
  assert(CPT && "RewriteBlockClass: Bad type");
  const FunctionType *FT = CPT->getPointeeType()->getAs<FunctionType>();
  assert(FT && "RewriteBlockClass: Bad type");
  const FunctionProtoType *FTP = dyn_cast<FunctionProtoType>(FT);
  // FTP will be null for closures that don't take arguments.

  RecordDecl *RD = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                      SourceLocation(), SourceLocation(),
                                      &Context->Idents.get("__block_impl"));
  QualType PtrBlock = Context->getPointerType(Context->getTagDeclType(RD));

  // Generate a funky cast.
  SmallVector<QualType, 8> ArgTypes;

  // Push the block argument type.
  ArgTypes.push_back(PtrBlock);
  if (FTP) {
    for (FunctionProtoType::arg_type_iterator I = FTP->arg_type_begin(),
         E = FTP->arg_type_end(); I && (I != E); ++I) {
      QualType t = *I;
      // Make sure we convert "t (^)(...)" to "t (*)(...)".
      if (!convertBlockPointerToFunctionPointer(t))
        convertToUnqualifiedObjCType(t);
      ArgTypes.push_back(t);
    }
  }
  // Now do the pointer to function cast.
  QualType PtrToFuncCastType
    = getSimpleFunctionType(Exp->getType(), &ArgTypes[0], ArgTypes.size());

  PtrToFuncCastType = Context->getPointerType(PtrToFuncCastType);

  CastExpr *BlkCast = NoTypeInfoCStyleCastExpr(Context, PtrBlock,
                                               CK_BitCast,
                                               const_cast<Expr*>(BlockExp));
  // Don't forget the parens to enforce the proper binding.
  ParenExpr *PE = new (Context) ParenExpr(SourceLocation(), SourceLocation(),
                                          BlkCast);
  //PE->dump();

  FieldDecl *FD = FieldDecl::Create(*Context, 0, SourceLocation(),
                                    SourceLocation(),
                                    &Context->Idents.get("FuncPtr"),
                                    Context->VoidPtrTy, 0,
                                    /*BitWidth=*/0, /*Mutable=*/true,
                                    /*HasInit=*/false);
  MemberExpr *ME = new (Context) MemberExpr(PE, true, FD, SourceLocation(),
                                            FD->getType(), VK_LValue,
                                            OK_Ordinary);

  
  CastExpr *FunkCast = NoTypeInfoCStyleCastExpr(Context, PtrToFuncCastType,
                                                CK_BitCast, ME);
  PE = new (Context) ParenExpr(SourceLocation(), SourceLocation(), FunkCast);

  SmallVector<Expr*, 8> BlkExprs;
  // Add the implicit argument.
  BlkExprs.push_back(BlkCast);
  // Add the user arguments.
  for (CallExpr::arg_iterator I = Exp->arg_begin(),
       E = Exp->arg_end(); I != E; ++I) {
    BlkExprs.push_back(*I);
  }
  CallExpr *CE = new (Context) CallExpr(*Context, PE, &BlkExprs[0],
                                        BlkExprs.size(),
                                        Exp->getType(), VK_RValue,
                                        SourceLocation());
  return CE;
}

// We need to return the rewritten expression to handle cases where the
// DeclRefExpr is embedded in another expression being rewritten.
// For example:
//
// int main() {
//    __block Foo *f;
//    __block int i;
//
//    void (^myblock)() = ^() {
//        [f test]; // f is a DeclRefExpr embedded in a message (which is being rewritten).
//        i = 77;
//    };
//}
Stmt *RewriteModernObjC::RewriteBlockDeclRefExpr(DeclRefExpr *DeclRefExp) {
  // Rewrite the byref variable into BYREFVAR->__forwarding->BYREFVAR 
  // for each DeclRefExp where BYREFVAR is name of the variable.
  ValueDecl *VD = DeclRefExp->getDecl();
  bool isArrow = DeclRefExp->refersToEnclosingLocal();
  
  FieldDecl *FD = FieldDecl::Create(*Context, 0, SourceLocation(),
                                    SourceLocation(),
                                    &Context->Idents.get("__forwarding"), 
                                    Context->VoidPtrTy, 0,
                                    /*BitWidth=*/0, /*Mutable=*/true,
                                    /*HasInit=*/false);
  MemberExpr *ME = new (Context) MemberExpr(DeclRefExp, isArrow,
                                            FD, SourceLocation(),
                                            FD->getType(), VK_LValue,
                                            OK_Ordinary);

  StringRef Name = VD->getName();
  FD = FieldDecl::Create(*Context, 0, SourceLocation(), SourceLocation(),
                         &Context->Idents.get(Name), 
                         Context->VoidPtrTy, 0,
                         /*BitWidth=*/0, /*Mutable=*/true,
                         /*HasInit=*/false);
  ME = new (Context) MemberExpr(ME, true, FD, SourceLocation(),
                                DeclRefExp->getType(), VK_LValue, OK_Ordinary);
  
  
  
  // Need parens to enforce precedence.
  ParenExpr *PE = new (Context) ParenExpr(DeclRefExp->getExprLoc(), 
                                          DeclRefExp->getExprLoc(), 
                                          ME);
  ReplaceStmt(DeclRefExp, PE);
  return PE;
}

// Rewrites the imported local variable V with external storage 
// (static, extern, etc.) as *V
//
Stmt *RewriteModernObjC::RewriteLocalVariableExternalStorage(DeclRefExpr *DRE) {
  ValueDecl *VD = DRE->getDecl();
  if (VarDecl *Var = dyn_cast<VarDecl>(VD))
    if (!ImportedLocalExternalDecls.count(Var))
      return DRE;
  Expr *Exp = new (Context) UnaryOperator(DRE, UO_Deref, DRE->getType(),
                                          VK_LValue, OK_Ordinary,
                                          DRE->getLocation());
  // Need parens to enforce precedence.
  ParenExpr *PE = new (Context) ParenExpr(SourceLocation(), SourceLocation(), 
                                          Exp);
  ReplaceStmt(DRE, PE);
  return PE;
}

void RewriteModernObjC::RewriteCastExpr(CStyleCastExpr *CE) {
  SourceLocation LocStart = CE->getLParenLoc();
  SourceLocation LocEnd = CE->getRParenLoc();

  // Need to avoid trying to rewrite synthesized casts.
  if (LocStart.isInvalid())
    return;
  // Need to avoid trying to rewrite casts contained in macros.
  if (!Rewriter::isRewritable(LocStart) || !Rewriter::isRewritable(LocEnd))
    return;

  const char *startBuf = SM->getCharacterData(LocStart);
  const char *endBuf = SM->getCharacterData(LocEnd);
  QualType QT = CE->getType();
  const Type* TypePtr = QT->getAs<Type>();
  if (isa<TypeOfExprType>(TypePtr)) {
    const TypeOfExprType *TypeOfExprTypePtr = cast<TypeOfExprType>(TypePtr);
    QT = TypeOfExprTypePtr->getUnderlyingExpr()->getType();
    std::string TypeAsString = "(";
    RewriteBlockPointerType(TypeAsString, QT);
    TypeAsString += ")";
    ReplaceText(LocStart, endBuf-startBuf+1, TypeAsString);
    return;
  }
  // advance the location to startArgList.
  const char *argPtr = startBuf;

  while (*argPtr++ && (argPtr < endBuf)) {
    switch (*argPtr) {
    case '^':
      // Replace the '^' with '*'.
      LocStart = LocStart.getLocWithOffset(argPtr-startBuf);
      ReplaceText(LocStart, 1, "*");
      break;
    }
  }
  return;
}

void RewriteModernObjC::RewriteBlockPointerFunctionArgs(FunctionDecl *FD) {
  SourceLocation DeclLoc = FD->getLocation();
  unsigned parenCount = 0;

  // We have 1 or more arguments that have closure pointers.
  const char *startBuf = SM->getCharacterData(DeclLoc);
  const char *startArgList = strchr(startBuf, '(');

  assert((*startArgList == '(') && "Rewriter fuzzy parser confused");

  parenCount++;
  // advance the location to startArgList.
  DeclLoc = DeclLoc.getLocWithOffset(startArgList-startBuf);
  assert((DeclLoc.isValid()) && "Invalid DeclLoc");

  const char *argPtr = startArgList;

  while (*argPtr++ && parenCount) {
    switch (*argPtr) {
    case '^':
      // Replace the '^' with '*'.
      DeclLoc = DeclLoc.getLocWithOffset(argPtr-startArgList);
      ReplaceText(DeclLoc, 1, "*");
      break;
    case '(':
      parenCount++;
      break;
    case ')':
      parenCount--;
      break;
    }
  }
  return;
}

bool RewriteModernObjC::PointerTypeTakesAnyBlockArguments(QualType QT) {
  const FunctionProtoType *FTP;
  const PointerType *PT = QT->getAs<PointerType>();
  if (PT) {
    FTP = PT->getPointeeType()->getAs<FunctionProtoType>();
  } else {
    const BlockPointerType *BPT = QT->getAs<BlockPointerType>();
    assert(BPT && "BlockPointerTypeTakeAnyBlockArguments(): not a block pointer type");
    FTP = BPT->getPointeeType()->getAs<FunctionProtoType>();
  }
  if (FTP) {
    for (FunctionProtoType::arg_type_iterator I = FTP->arg_type_begin(),
         E = FTP->arg_type_end(); I != E; ++I)
      if (isTopLevelBlockPointerType(*I))
        return true;
  }
  return false;
}

bool RewriteModernObjC::PointerTypeTakesAnyObjCQualifiedType(QualType QT) {
  const FunctionProtoType *FTP;
  const PointerType *PT = QT->getAs<PointerType>();
  if (PT) {
    FTP = PT->getPointeeType()->getAs<FunctionProtoType>();
  } else {
    const BlockPointerType *BPT = QT->getAs<BlockPointerType>();
    assert(BPT && "BlockPointerTypeTakeAnyBlockArguments(): not a block pointer type");
    FTP = BPT->getPointeeType()->getAs<FunctionProtoType>();
  }
  if (FTP) {
    for (FunctionProtoType::arg_type_iterator I = FTP->arg_type_begin(),
         E = FTP->arg_type_end(); I != E; ++I) {
      if ((*I)->isObjCQualifiedIdType())
        return true;
      if ((*I)->isObjCObjectPointerType() &&
          (*I)->getPointeeType()->isObjCQualifiedInterfaceType())
        return true;
    }
        
  }
  return false;
}

void RewriteModernObjC::GetExtentOfArgList(const char *Name, const char *&LParen,
                                     const char *&RParen) {
  const char *argPtr = strchr(Name, '(');
  assert((*argPtr == '(') && "Rewriter fuzzy parser confused");

  LParen = argPtr; // output the start.
  argPtr++; // skip past the left paren.
  unsigned parenCount = 1;

  while (*argPtr && parenCount) {
    switch (*argPtr) {
    case '(': parenCount++; break;
    case ')': parenCount--; break;
    default: break;
    }
    if (parenCount) argPtr++;
  }
  assert((*argPtr == ')') && "Rewriter fuzzy parser confused");
  RParen = argPtr; // output the end
}

void RewriteModernObjC::RewriteBlockPointerDecl(NamedDecl *ND) {
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(ND)) {
    RewriteBlockPointerFunctionArgs(FD);
    return;
  }
  // Handle Variables and Typedefs.
  SourceLocation DeclLoc = ND->getLocation();
  QualType DeclT;
  if (VarDecl *VD = dyn_cast<VarDecl>(ND))
    DeclT = VD->getType();
  else if (TypedefNameDecl *TDD = dyn_cast<TypedefNameDecl>(ND))
    DeclT = TDD->getUnderlyingType();
  else if (FieldDecl *FD = dyn_cast<FieldDecl>(ND))
    DeclT = FD->getType();
  else
    llvm_unreachable("RewriteBlockPointerDecl(): Decl type not yet handled");

  const char *startBuf = SM->getCharacterData(DeclLoc);
  const char *endBuf = startBuf;
  // scan backward (from the decl location) for the end of the previous decl.
  while (*startBuf != '^' && *startBuf != ';' && startBuf != MainFileStart)
    startBuf--;
  SourceLocation Start = DeclLoc.getLocWithOffset(startBuf-endBuf);
  std::string buf;
  unsigned OrigLength=0;
  // *startBuf != '^' if we are dealing with a pointer to function that
  // may take block argument types (which will be handled below).
  if (*startBuf == '^') {
    // Replace the '^' with '*', computing a negative offset.
    buf = '*';
    startBuf++;
    OrigLength++;
  }
  while (*startBuf != ')') {
    buf += *startBuf;
    startBuf++;
    OrigLength++;
  }
  buf += ')';
  OrigLength++;
  
  if (PointerTypeTakesAnyBlockArguments(DeclT) ||
      PointerTypeTakesAnyObjCQualifiedType(DeclT)) {
    // Replace the '^' with '*' for arguments.
    // Replace id<P> with id/*<>*/
    DeclLoc = ND->getLocation();
    startBuf = SM->getCharacterData(DeclLoc);
    const char *argListBegin, *argListEnd;
    GetExtentOfArgList(startBuf, argListBegin, argListEnd);
    while (argListBegin < argListEnd) {
      if (*argListBegin == '^')
        buf += '*';
      else if (*argListBegin ==  '<') {
        buf += "/*"; 
        buf += *argListBegin++;
        OrigLength++;;
        while (*argListBegin != '>') {
          buf += *argListBegin++;
          OrigLength++;
        }
        buf += *argListBegin;
        buf += "*/";
      }
      else
        buf += *argListBegin;
      argListBegin++;
      OrigLength++;
    }
    buf += ')';
    OrigLength++;
  }
  ReplaceText(Start, OrigLength, buf);
  
  return;
}


/// SynthesizeByrefCopyDestroyHelper - This routine synthesizes:
/// void __Block_byref_id_object_copy(struct Block_byref_id_object *dst,
///                    struct Block_byref_id_object *src) {
///  _Block_object_assign (&_dest->object, _src->object, 
///                        BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT
///                        [|BLOCK_FIELD_IS_WEAK]) // object
///  _Block_object_assign(&_dest->object, _src->object, 
///                       BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK
///                       [|BLOCK_FIELD_IS_WEAK]) // block
/// }
/// And:
/// void __Block_byref_id_object_dispose(struct Block_byref_id_object *_src) {
///  _Block_object_dispose(_src->object, 
///                        BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_OBJECT
///                        [|BLOCK_FIELD_IS_WEAK]) // object
///  _Block_object_dispose(_src->object, 
///                         BLOCK_BYREF_CALLER | BLOCK_FIELD_IS_BLOCK
///                         [|BLOCK_FIELD_IS_WEAK]) // block
/// }

std::string RewriteModernObjC::SynthesizeByrefCopyDestroyHelper(VarDecl *VD,
                                                          int flag) {
  std::string S;
  if (CopyDestroyCache.count(flag))
    return S;
  CopyDestroyCache.insert(flag);
  S = "static void __Block_byref_id_object_copy_";
  S += utostr(flag);
  S += "(void *dst, void *src) {\n";
  
  // offset into the object pointer is computed as:
  // void * + void* + int + int + void* + void *
  unsigned IntSize = 
  static_cast<unsigned>(Context->getTypeSize(Context->IntTy));
  unsigned VoidPtrSize = 
  static_cast<unsigned>(Context->getTypeSize(Context->VoidPtrTy));
  
  unsigned offset = (VoidPtrSize*4 + IntSize + IntSize)/Context->getCharWidth();
  S += " _Block_object_assign((char*)dst + ";
  S += utostr(offset);
  S += ", *(void * *) ((char*)src + ";
  S += utostr(offset);
  S += "), ";
  S += utostr(flag);
  S += ");\n}\n";
  
  S += "static void __Block_byref_id_object_dispose_";
  S += utostr(flag);
  S += "(void *src) {\n";
  S += " _Block_object_dispose(*(void * *) ((char*)src + ";
  S += utostr(offset);
  S += "), ";
  S += utostr(flag);
  S += ");\n}\n";
  return S;
}

/// RewriteByRefVar - For each __block typex ND variable this routine transforms
/// the declaration into:
/// struct __Block_byref_ND {
/// void *__isa;                  // NULL for everything except __weak pointers
/// struct __Block_byref_ND *__forwarding;
/// int32_t __flags;
/// int32_t __size;
/// void *__Block_byref_id_object_copy; // If variable is __block ObjC object
/// void *__Block_byref_id_object_dispose; // If variable is __block ObjC object
/// typex ND;
/// };
///
/// It then replaces declaration of ND variable with:
/// struct __Block_byref_ND ND = {__isa=0B, __forwarding=&ND, __flags=some_flag, 
///                               __size=sizeof(struct __Block_byref_ND), 
///                               ND=initializer-if-any};
///
///
void RewriteModernObjC::RewriteByRefVar(VarDecl *ND) {
  // Insert declaration for the function in which block literal is
  // used.
  if (CurFunctionDeclToDeclareForBlock)
    RewriteBlockLiteralFunctionDecl(CurFunctionDeclToDeclareForBlock);
  int flag = 0;
  int isa = 0;
  SourceLocation DeclLoc = ND->getTypeSpecStartLoc();
  if (DeclLoc.isInvalid())
    // If type location is missing, it is because of missing type (a warning).
    // Use variable's location which is good for this case.
    DeclLoc = ND->getLocation();
  const char *startBuf = SM->getCharacterData(DeclLoc);
  SourceLocation X = ND->getLocEnd();
  X = SM->getExpansionLoc(X);
  const char *endBuf = SM->getCharacterData(X);
  std::string Name(ND->getNameAsString());
  std::string ByrefType;
  RewriteByRefString(ByrefType, Name, ND, true);
  ByrefType += " {\n";
  ByrefType += "  void *__isa;\n";
  RewriteByRefString(ByrefType, Name, ND);
  ByrefType += " *__forwarding;\n";
  ByrefType += " int __flags;\n";
  ByrefType += " int __size;\n";
  // Add void *__Block_byref_id_object_copy; 
  // void *__Block_byref_id_object_dispose; if needed.
  QualType Ty = ND->getType();
  bool HasCopyAndDispose = Context->BlockRequiresCopying(Ty);
  if (HasCopyAndDispose) {
    ByrefType += " void (*__Block_byref_id_object_copy)(void*, void*);\n";
    ByrefType += " void (*__Block_byref_id_object_dispose)(void*);\n";
  }

  QualType T = Ty;
  (void)convertBlockPointerToFunctionPointer(T);
  T.getAsStringInternal(Name, Context->getPrintingPolicy());
    
  ByrefType += " " + Name + ";\n";
  ByrefType += "};\n";
  // Insert this type in global scope. It is needed by helper function.
  SourceLocation FunLocStart;
  if (CurFunctionDef)
     FunLocStart = CurFunctionDef->getTypeSpecStartLoc();
  else {
    assert(CurMethodDef && "RewriteByRefVar - CurMethodDef is null");
    FunLocStart = CurMethodDef->getLocStart();
  }
  InsertText(FunLocStart, ByrefType);
  if (Ty.isObjCGCWeak()) {
    flag |= BLOCK_FIELD_IS_WEAK;
    isa = 1;
  }
  
  if (HasCopyAndDispose) {
    flag = BLOCK_BYREF_CALLER;
    QualType Ty = ND->getType();
    // FIXME. Handle __weak variable (BLOCK_FIELD_IS_WEAK) as well.
    if (Ty->isBlockPointerType())
      flag |= BLOCK_FIELD_IS_BLOCK;
    else
      flag |= BLOCK_FIELD_IS_OBJECT;
    std::string HF = SynthesizeByrefCopyDestroyHelper(ND, flag);
    if (!HF.empty())
      InsertText(FunLocStart, HF);
  }
  
  // struct __Block_byref_ND ND = 
  // {0, &ND, some_flag, __size=sizeof(struct __Block_byref_ND), 
  //  initializer-if-any};
  bool hasInit = (ND->getInit() != 0);
  unsigned flags = 0;
  if (HasCopyAndDispose)
    flags |= BLOCK_HAS_COPY_DISPOSE;
  Name = ND->getNameAsString();
  ByrefType.clear();
  RewriteByRefString(ByrefType, Name, ND);
  std::string ForwardingCastType("(");
  ForwardingCastType += ByrefType + " *)";
  if (!hasInit) {
    ByrefType += " " + Name + " = {(void*)";
    ByrefType += utostr(isa);
    ByrefType += "," +  ForwardingCastType + "&" + Name + ", ";
    ByrefType += utostr(flags);
    ByrefType += ", ";
    ByrefType += "sizeof(";
    RewriteByRefString(ByrefType, Name, ND);
    ByrefType += ")";
    if (HasCopyAndDispose) {
      ByrefType += ", __Block_byref_id_object_copy_";
      ByrefType += utostr(flag);
      ByrefType += ", __Block_byref_id_object_dispose_";
      ByrefType += utostr(flag);
    }
    ByrefType += "};\n";
    unsigned nameSize = Name.size();
    // for block or function pointer declaration. Name is aleady
    // part of the declaration.
    if (Ty->isBlockPointerType() || Ty->isFunctionPointerType())
      nameSize = 1;
    ReplaceText(DeclLoc, endBuf-startBuf+nameSize, ByrefType);
  }
  else {
    SourceLocation startLoc;
    Expr *E = ND->getInit();
    if (const CStyleCastExpr *ECE = dyn_cast<CStyleCastExpr>(E))
      startLoc = ECE->getLParenLoc();
    else
      startLoc = E->getLocStart();
    startLoc = SM->getExpansionLoc(startLoc);
    endBuf = SM->getCharacterData(startLoc);
    ByrefType += " " + Name;
    ByrefType += " = {(void*)";
    ByrefType += utostr(isa);
    ByrefType += "," +  ForwardingCastType + "&" + Name + ", ";
    ByrefType += utostr(flags);
    ByrefType += ", ";
    ByrefType += "sizeof(";
    RewriteByRefString(ByrefType, Name, ND);
    ByrefType += "), ";
    if (HasCopyAndDispose) {
      ByrefType += "__Block_byref_id_object_copy_";
      ByrefType += utostr(flag);
      ByrefType += ", __Block_byref_id_object_dispose_";
      ByrefType += utostr(flag);
      ByrefType += ", ";
    }
    ReplaceText(DeclLoc, endBuf-startBuf, ByrefType);
    
    // Complete the newly synthesized compound expression by inserting a right
    // curly brace before the end of the declaration.
    // FIXME: This approach avoids rewriting the initializer expression. It
    // also assumes there is only one declarator. For example, the following
    // isn't currently supported by this routine (in general):
    // 
    // double __block BYREFVAR = 1.34, BYREFVAR2 = 1.37;
    //
    const char *startInitializerBuf = SM->getCharacterData(startLoc);
    const char *semiBuf = strchr(startInitializerBuf, ';');
    assert((*semiBuf == ';') && "RewriteByRefVar: can't find ';'");
    SourceLocation semiLoc =
      startLoc.getLocWithOffset(semiBuf-startInitializerBuf);

    InsertText(semiLoc, "}");
  }
  return;
}

void RewriteModernObjC::CollectBlockDeclRefInfo(BlockExpr *Exp) {
  // Add initializers for any closure decl refs.
  GetBlockDeclRefExprs(Exp->getBody());
  if (BlockDeclRefs.size()) {
    // Unique all "by copy" declarations.
    for (unsigned i = 0; i < BlockDeclRefs.size(); i++)
      if (!BlockDeclRefs[i]->getDecl()->hasAttr<BlocksAttr>()) {
        if (!BlockByCopyDeclsPtrSet.count(BlockDeclRefs[i]->getDecl())) {
          BlockByCopyDeclsPtrSet.insert(BlockDeclRefs[i]->getDecl());
          BlockByCopyDecls.push_back(BlockDeclRefs[i]->getDecl());
        }
      }
    // Unique all "by ref" declarations.
    for (unsigned i = 0; i < BlockDeclRefs.size(); i++)
      if (BlockDeclRefs[i]->getDecl()->hasAttr<BlocksAttr>()) {
        if (!BlockByRefDeclsPtrSet.count(BlockDeclRefs[i]->getDecl())) {
          BlockByRefDeclsPtrSet.insert(BlockDeclRefs[i]->getDecl());
          BlockByRefDecls.push_back(BlockDeclRefs[i]->getDecl());
        }
      }
    // Find any imported blocks...they will need special attention.
    for (unsigned i = 0; i < BlockDeclRefs.size(); i++)
      if (BlockDeclRefs[i]->getDecl()->hasAttr<BlocksAttr>() ||
          BlockDeclRefs[i]->getType()->isObjCObjectPointerType() || 
          BlockDeclRefs[i]->getType()->isBlockPointerType())
        ImportedBlockDecls.insert(BlockDeclRefs[i]->getDecl());
  }
}

FunctionDecl *RewriteModernObjC::SynthBlockInitFunctionDecl(StringRef name) {
  IdentifierInfo *ID = &Context->Idents.get(name);
  QualType FType = Context->getFunctionNoProtoType(Context->VoidPtrTy);
  return FunctionDecl::Create(*Context, TUDecl, SourceLocation(),
                              SourceLocation(), ID, FType, 0, SC_Extern,
                              SC_None, false, false);
}

Stmt *RewriteModernObjC::SynthBlockInitExpr(BlockExpr *Exp,
          const SmallVector<DeclRefExpr *, 8> &InnerBlockDeclRefs) {
  const BlockDecl *block = Exp->getBlockDecl();
  Blocks.push_back(Exp);

  CollectBlockDeclRefInfo(Exp);
  
  // Add inner imported variables now used in current block.
 int countOfInnerDecls = 0;
  if (!InnerBlockDeclRefs.empty()) {
    for (unsigned i = 0; i < InnerBlockDeclRefs.size(); i++) {
      DeclRefExpr *Exp = InnerBlockDeclRefs[i];
      ValueDecl *VD = Exp->getDecl();
      if (!VD->hasAttr<BlocksAttr>() && !BlockByCopyDeclsPtrSet.count(VD)) {
      // We need to save the copied-in variables in nested
      // blocks because it is needed at the end for some of the API generations.
      // See SynthesizeBlockLiterals routine.
        InnerDeclRefs.push_back(Exp); countOfInnerDecls++;
        BlockDeclRefs.push_back(Exp);
        BlockByCopyDeclsPtrSet.insert(VD);
        BlockByCopyDecls.push_back(VD);
      }
      if (VD->hasAttr<BlocksAttr>() && !BlockByRefDeclsPtrSet.count(VD)) {
        InnerDeclRefs.push_back(Exp); countOfInnerDecls++;
        BlockDeclRefs.push_back(Exp);
        BlockByRefDeclsPtrSet.insert(VD);
        BlockByRefDecls.push_back(VD);
      }
    }
    // Find any imported blocks...they will need special attention.
    for (unsigned i = 0; i < InnerBlockDeclRefs.size(); i++)
      if (InnerBlockDeclRefs[i]->getDecl()->hasAttr<BlocksAttr>() ||
          InnerBlockDeclRefs[i]->getType()->isObjCObjectPointerType() || 
          InnerBlockDeclRefs[i]->getType()->isBlockPointerType())
        ImportedBlockDecls.insert(InnerBlockDeclRefs[i]->getDecl());
  }
  InnerDeclRefsCount.push_back(countOfInnerDecls);
  
  std::string FuncName;

  if (CurFunctionDef)
    FuncName = CurFunctionDef->getNameAsString();
  else if (CurMethodDef)
    BuildUniqueMethodName(FuncName, CurMethodDef);
  else if (GlobalVarDecl)
    FuncName = std::string(GlobalVarDecl->getNameAsString());

  std::string BlockNumber = utostr(Blocks.size()-1);

  std::string Tag = "__" + FuncName + "_block_impl_" + BlockNumber;
  std::string Func = "__" + FuncName + "_block_func_" + BlockNumber;

  // Get a pointer to the function type so we can cast appropriately.
  QualType BFT = convertFunctionTypeOfBlocks(Exp->getFunctionType());
  QualType FType = Context->getPointerType(BFT);

  FunctionDecl *FD;
  Expr *NewRep;

  // Simulate a contructor call...
  FD = SynthBlockInitFunctionDecl(Tag);
  DeclRefExpr *DRE = new (Context) DeclRefExpr(FD, false, FType, VK_RValue,
                                               SourceLocation());

  SmallVector<Expr*, 4> InitExprs;

  // Initialize the block function.
  FD = SynthBlockInitFunctionDecl(Func);
  DeclRefExpr *Arg = new (Context) DeclRefExpr(FD, false, FD->getType(),
                                               VK_LValue, SourceLocation());
  CastExpr *castExpr = NoTypeInfoCStyleCastExpr(Context, Context->VoidPtrTy,
                                                CK_BitCast, Arg);
  InitExprs.push_back(castExpr);

  // Initialize the block descriptor.
  std::string DescData = "__" + FuncName + "_block_desc_" + BlockNumber + "_DATA";

  VarDecl *NewVD = VarDecl::Create(*Context, TUDecl,
                                   SourceLocation(), SourceLocation(),
                                   &Context->Idents.get(DescData.c_str()),
                                   Context->VoidPtrTy, 0,
                                   SC_Static, SC_None);
  UnaryOperator *DescRefExpr =
    new (Context) UnaryOperator(new (Context) DeclRefExpr(NewVD, false,
                                                          Context->VoidPtrTy,
                                                          VK_LValue,
                                                          SourceLocation()), 
                                UO_AddrOf,
                                Context->getPointerType(Context->VoidPtrTy), 
                                VK_RValue, OK_Ordinary,
                                SourceLocation());
  InitExprs.push_back(DescRefExpr); 
  
  // Add initializers for any closure decl refs.
  if (BlockDeclRefs.size()) {
    Expr *Exp;
    // Output all "by copy" declarations.
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByCopyDecls.begin(),
         E = BlockByCopyDecls.end(); I != E; ++I) {
      if (isObjCType((*I)->getType())) {
        // FIXME: Conform to ABI ([[obj retain] autorelease]).
        FD = SynthBlockInitFunctionDecl((*I)->getName());
        Exp = new (Context) DeclRefExpr(FD, false, FD->getType(),
                                        VK_LValue, SourceLocation());
        if (HasLocalVariableExternalStorage(*I)) {
          QualType QT = (*I)->getType();
          QT = Context->getPointerType(QT);
          Exp = new (Context) UnaryOperator(Exp, UO_AddrOf, QT, VK_RValue,
                                            OK_Ordinary, SourceLocation());
        }
      } else if (isTopLevelBlockPointerType((*I)->getType())) {
        FD = SynthBlockInitFunctionDecl((*I)->getName());
        Arg = new (Context) DeclRefExpr(FD, false, FD->getType(),
                                        VK_LValue, SourceLocation());
        Exp = NoTypeInfoCStyleCastExpr(Context, Context->VoidPtrTy,
                                       CK_BitCast, Arg);
      } else {
        FD = SynthBlockInitFunctionDecl((*I)->getName());
        Exp = new (Context) DeclRefExpr(FD, false, FD->getType(),
                                        VK_LValue, SourceLocation());
        if (HasLocalVariableExternalStorage(*I)) {
          QualType QT = (*I)->getType();
          QT = Context->getPointerType(QT);
          Exp = new (Context) UnaryOperator(Exp, UO_AddrOf, QT, VK_RValue,
                                            OK_Ordinary, SourceLocation());
        }
        
      }
      InitExprs.push_back(Exp);
    }
    // Output all "by ref" declarations.
    for (SmallVector<ValueDecl*,8>::iterator I = BlockByRefDecls.begin(),
         E = BlockByRefDecls.end(); I != E; ++I) {
      ValueDecl *ND = (*I);
      std::string Name(ND->getNameAsString());
      std::string RecName;
      RewriteByRefString(RecName, Name, ND, true);
      IdentifierInfo *II = &Context->Idents.get(RecName.c_str() 
                                                + sizeof("struct"));
      RecordDecl *RD = RecordDecl::Create(*Context, TTK_Struct, TUDecl,
                                          SourceLocation(), SourceLocation(),
                                          II);
      assert(RD && "SynthBlockInitExpr(): Can't find RecordDecl");
      QualType castT = Context->getPointerType(Context->getTagDeclType(RD));
      
      FD = SynthBlockInitFunctionDecl((*I)->getName());
      Exp = new (Context) DeclRefExpr(FD, false, FD->getType(), VK_LValue,
                                      SourceLocation());
      bool isNestedCapturedVar = false;
      if (block)
        for (BlockDecl::capture_const_iterator ci = block->capture_begin(),
             ce = block->capture_end(); ci != ce; ++ci) {
          const VarDecl *variable = ci->getVariable();
          if (variable == ND && ci->isNested()) {
            assert (ci->isByRef() && 
                    "SynthBlockInitExpr - captured block variable is not byref");
            isNestedCapturedVar = true;
            break;
          }
        }
      // captured nested byref variable has its address passed. Do not take
      // its address again.
      if (!isNestedCapturedVar)
          Exp = new (Context) UnaryOperator(Exp, UO_AddrOf,
                                     Context->getPointerType(Exp->getType()),
                                     VK_RValue, OK_Ordinary, SourceLocation());
      Exp = NoTypeInfoCStyleCastExpr(Context, castT, CK_BitCast, Exp);
      InitExprs.push_back(Exp);
    }
  }
  if (ImportedBlockDecls.size()) {
    // generate BLOCK_HAS_COPY_DISPOSE(have helper funcs) | BLOCK_HAS_DESCRIPTOR
    int flag = (BLOCK_HAS_COPY_DISPOSE | BLOCK_HAS_DESCRIPTOR);
    unsigned IntSize = 
      static_cast<unsigned>(Context->getTypeSize(Context->IntTy));
    Expr *FlagExp = IntegerLiteral::Create(*Context, llvm::APInt(IntSize, flag), 
                                           Context->IntTy, SourceLocation());
    InitExprs.push_back(FlagExp);
  }
  NewRep = new (Context) CallExpr(*Context, DRE, &InitExprs[0], InitExprs.size(),
                                  FType, VK_LValue, SourceLocation());
  NewRep = new (Context) UnaryOperator(NewRep, UO_AddrOf,
                             Context->getPointerType(NewRep->getType()),
                             VK_RValue, OK_Ordinary, SourceLocation());
  NewRep = NoTypeInfoCStyleCastExpr(Context, FType, CK_BitCast,
                                    NewRep);
  BlockDeclRefs.clear();
  BlockByRefDecls.clear();
  BlockByRefDeclsPtrSet.clear();
  BlockByCopyDecls.clear();
  BlockByCopyDeclsPtrSet.clear();
  ImportedBlockDecls.clear();
  return NewRep;
}

bool RewriteModernObjC::IsDeclStmtInForeachHeader(DeclStmt *DS) {
  if (const ObjCForCollectionStmt * CS = 
      dyn_cast<ObjCForCollectionStmt>(Stmts.back()))
        return CS->getElement() == DS;
  return false;
}

//===----------------------------------------------------------------------===//
// Function Body / Expression rewriting
//===----------------------------------------------------------------------===//

Stmt *RewriteModernObjC::RewriteFunctionBodyOrGlobalInitializer(Stmt *S) {
  if (isa<SwitchStmt>(S) || isa<WhileStmt>(S) ||
      isa<DoStmt>(S) || isa<ForStmt>(S))
    Stmts.push_back(S);
  else if (isa<ObjCForCollectionStmt>(S)) {
    Stmts.push_back(S);
    ObjCBcLabelNo.push_back(++BcLabelCount);
  }

  // Pseudo-object operations and ivar references need special
  // treatment because we're going to recursively rewrite them.
  if (PseudoObjectExpr *PseudoOp = dyn_cast<PseudoObjectExpr>(S)) {
    if (isa<BinaryOperator>(PseudoOp->getSyntacticForm())) {
      return RewritePropertyOrImplicitSetter(PseudoOp);
    } else {
      return RewritePropertyOrImplicitGetter(PseudoOp);
    }
  } else if (ObjCIvarRefExpr *IvarRefExpr = dyn_cast<ObjCIvarRefExpr>(S)) {
    return RewriteObjCIvarRefExpr(IvarRefExpr);
  }

  SourceRange OrigStmtRange = S->getSourceRange();

  // Perform a bottom up rewrite of all children.
  for (Stmt::child_range CI = S->children(); CI; ++CI)
    if (*CI) {
      Stmt *childStmt = (*CI);
      Stmt *newStmt = RewriteFunctionBodyOrGlobalInitializer(childStmt);
      if (newStmt) {
        *CI = newStmt;
      }
    }

  if (BlockExpr *BE = dyn_cast<BlockExpr>(S)) {
    SmallVector<DeclRefExpr *, 8> InnerBlockDeclRefs;
    llvm::SmallPtrSet<const DeclContext *, 8> InnerContexts;
    InnerContexts.insert(BE->getBlockDecl());
    ImportedLocalExternalDecls.clear();
    GetInnerBlockDeclRefExprs(BE->getBody(),
                              InnerBlockDeclRefs, InnerContexts);
    // Rewrite the block body in place.
    Stmt *SaveCurrentBody = CurrentBody;
    CurrentBody = BE->getBody();
    PropParentMap = 0;
    // block literal on rhs of a property-dot-sytax assignment
    // must be replaced by its synthesize ast so getRewrittenText
    // works as expected. In this case, what actually ends up on RHS
    // is the blockTranscribed which is the helper function for the
    // block literal; as in: self.c = ^() {[ace ARR];};
    bool saveDisableReplaceStmt = DisableReplaceStmt;
    DisableReplaceStmt = false;
    RewriteFunctionBodyOrGlobalInitializer(BE->getBody());
    DisableReplaceStmt = saveDisableReplaceStmt;
    CurrentBody = SaveCurrentBody;
    PropParentMap = 0;
    ImportedLocalExternalDecls.clear();
    // Now we snarf the rewritten text and stash it away for later use.
    std::string Str = Rewrite.getRewrittenText(BE->getSourceRange());
    RewrittenBlockExprs[BE] = Str;

    Stmt *blockTranscribed = SynthBlockInitExpr(BE, InnerBlockDeclRefs);
                            
    //blockTranscribed->dump();
    ReplaceStmt(S, blockTranscribed);
    return blockTranscribed;
  }
  // Handle specific things.
  if (ObjCEncodeExpr *AtEncode = dyn_cast<ObjCEncodeExpr>(S))
    return RewriteAtEncode(AtEncode);

  if (ObjCSelectorExpr *AtSelector = dyn_cast<ObjCSelectorExpr>(S))
    return RewriteAtSelector(AtSelector);

  if (ObjCStringLiteral *AtString = dyn_cast<ObjCStringLiteral>(S))
    return RewriteObjCStringLiteral(AtString);

  if (ObjCMessageExpr *MessExpr = dyn_cast<ObjCMessageExpr>(S)) {
#if 0
    // Before we rewrite it, put the original message expression in a comment.
    SourceLocation startLoc = MessExpr->getLocStart();
    SourceLocation endLoc = MessExpr->getLocEnd();

    const char *startBuf = SM->getCharacterData(startLoc);
    const char *endBuf = SM->getCharacterData(endLoc);

    std::string messString;
    messString += "// ";
    messString.append(startBuf, endBuf-startBuf+1);
    messString += "\n";

    // FIXME: Missing definition of
    // InsertText(clang::SourceLocation, char const*, unsigned int).
    // InsertText(startLoc, messString.c_str(), messString.size());
    // Tried this, but it didn't work either...
    // ReplaceText(startLoc, 0, messString.c_str(), messString.size());
#endif
    return RewriteMessageExpr(MessExpr);
  }

  if (ObjCAtTryStmt *StmtTry = dyn_cast<ObjCAtTryStmt>(S))
    return RewriteObjCTryStmt(StmtTry);

  if (ObjCAtSynchronizedStmt *StmtTry = dyn_cast<ObjCAtSynchronizedStmt>(S))
    return RewriteObjCSynchronizedStmt(StmtTry);

  if (ObjCAtThrowStmt *StmtThrow = dyn_cast<ObjCAtThrowStmt>(S))
    return RewriteObjCThrowStmt(StmtThrow);

  if (ObjCProtocolExpr *ProtocolExp = dyn_cast<ObjCProtocolExpr>(S))
    return RewriteObjCProtocolExpr(ProtocolExp);

  if (ObjCForCollectionStmt *StmtForCollection =
        dyn_cast<ObjCForCollectionStmt>(S))
    return RewriteObjCForCollectionStmt(StmtForCollection,
                                        OrigStmtRange.getEnd());
  if (BreakStmt *StmtBreakStmt =
      dyn_cast<BreakStmt>(S))
    return RewriteBreakStmt(StmtBreakStmt);
  if (ContinueStmt *StmtContinueStmt =
      dyn_cast<ContinueStmt>(S))
    return RewriteContinueStmt(StmtContinueStmt);

  // Need to check for protocol refs (id <P>, Foo <P> *) in variable decls
  // and cast exprs.
  if (DeclStmt *DS = dyn_cast<DeclStmt>(S)) {
    // FIXME: What we're doing here is modifying the type-specifier that
    // precedes the first Decl.  In the future the DeclGroup should have
    // a separate type-specifier that we can rewrite.
    // NOTE: We need to avoid rewriting the DeclStmt if it is within
    // the context of an ObjCForCollectionStmt. For example:
    //   NSArray *someArray;
    //   for (id <FooProtocol> index in someArray) ;
    // This is because RewriteObjCForCollectionStmt() does textual rewriting 
    // and it depends on the original text locations/positions.
    if (Stmts.empty() || !IsDeclStmtInForeachHeader(DS))
      RewriteObjCQualifiedInterfaceTypes(*DS->decl_begin());

    // Blocks rewrite rules.
    for (DeclStmt::decl_iterator DI = DS->decl_begin(), DE = DS->decl_end();
         DI != DE; ++DI) {
      Decl *SD = *DI;
      if (ValueDecl *ND = dyn_cast<ValueDecl>(SD)) {
        if (isTopLevelBlockPointerType(ND->getType()))
          RewriteBlockPointerDecl(ND);
        else if (ND->getType()->isFunctionPointerType())
          CheckFunctionPointerDecl(ND->getType(), ND);
        if (VarDecl *VD = dyn_cast<VarDecl>(SD)) {
          if (VD->hasAttr<BlocksAttr>()) {
            static unsigned uniqueByrefDeclCount = 0;
            assert(!BlockByRefDeclNo.count(ND) &&
              "RewriteFunctionBodyOrGlobalInitializer: Duplicate byref decl");
            BlockByRefDeclNo[ND] = uniqueByrefDeclCount++;
            RewriteByRefVar(VD);
          }
          else           
            RewriteTypeOfDecl(VD);
        }
      }
      if (TypedefNameDecl *TD = dyn_cast<TypedefNameDecl>(SD)) {
        if (isTopLevelBlockPointerType(TD->getUnderlyingType()))
          RewriteBlockPointerDecl(TD);
        else if (TD->getUnderlyingType()->isFunctionPointerType())
          CheckFunctionPointerDecl(TD->getUnderlyingType(), TD);
      }
    }
  }

  if (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(S))
    RewriteObjCQualifiedInterfaceTypes(CE);

  if (isa<SwitchStmt>(S) || isa<WhileStmt>(S) ||
      isa<DoStmt>(S) || isa<ForStmt>(S)) {
    assert(!Stmts.empty() && "Statement stack is empty");
    assert ((isa<SwitchStmt>(Stmts.back()) || isa<WhileStmt>(Stmts.back()) ||
             isa<DoStmt>(Stmts.back()) || isa<ForStmt>(Stmts.back()))
            && "Statement stack mismatch");
    Stmts.pop_back();
  }
  // Handle blocks rewriting.
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
    ValueDecl *VD = DRE->getDecl(); 
    if (VD->hasAttr<BlocksAttr>())
      return RewriteBlockDeclRefExpr(DRE);
    if (HasLocalVariableExternalStorage(VD))
      return RewriteLocalVariableExternalStorage(DRE);
  }
  
  if (CallExpr *CE = dyn_cast<CallExpr>(S)) {
    if (CE->getCallee()->getType()->isBlockPointerType()) {
      Stmt *BlockCall = SynthesizeBlockCall(CE, CE->getCallee());
      ReplaceStmt(S, BlockCall);
      return BlockCall;
    }
  }
  if (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(S)) {
    RewriteCastExpr(CE);
  }
#if 0
  if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(S)) {
    CastExpr *Replacement = new (Context) CastExpr(ICE->getType(),
                                                   ICE->getSubExpr(),
                                                   SourceLocation());
    // Get the new text.
    std::string SStr;
    llvm::raw_string_ostream Buf(SStr);
    Replacement->printPretty(Buf, *Context);
    const std::string &Str = Buf.str();

    printf("CAST = %s\n", &Str[0]);
    InsertText(ICE->getSubExpr()->getLocStart(), &Str[0], Str.size());
    delete S;
    return Replacement;
  }
#endif
  // Return this stmt unmodified.
  return S;
}

void RewriteModernObjC::RewriteRecordBody(RecordDecl *RD) {
  for (RecordDecl::field_iterator i = RD->field_begin(), 
                                  e = RD->field_end(); i != e; ++i) {
    FieldDecl *FD = *i;
    if (isTopLevelBlockPointerType(FD->getType()))
      RewriteBlockPointerDecl(FD);
    if (FD->getType()->isObjCQualifiedIdType() ||
        FD->getType()->isObjCQualifiedInterfaceType())
      RewriteObjCQualifiedInterfaceTypes(FD);
  }
}

/// HandleDeclInMainFile - This is called for each top-level decl defined in the
/// main file of the input.
void RewriteModernObjC::HandleDeclInMainFile(Decl *D) {
  switch (D->getKind()) {
    case Decl::Function: {
      FunctionDecl *FD = cast<FunctionDecl>(D);
      if (FD->isOverloadedOperator())
        return;

      // Since function prototypes don't have ParmDecl's, we check the function
      // prototype. This enables us to rewrite function declarations and
      // definitions using the same code.
      RewriteBlocksInFunctionProtoType(FD->getType(), FD);

      if (!FD->isThisDeclarationADefinition())
        break;

      // FIXME: If this should support Obj-C++, support CXXTryStmt
      if (CompoundStmt *Body = dyn_cast_or_null<CompoundStmt>(FD->getBody())) {
        CurFunctionDef = FD;
        CurFunctionDeclToDeclareForBlock = FD;
        CurrentBody = Body;
        Body =
        cast_or_null<CompoundStmt>(RewriteFunctionBodyOrGlobalInitializer(Body));
        FD->setBody(Body);
        CurrentBody = 0;
        if (PropParentMap) {
          delete PropParentMap;
          PropParentMap = 0;
        }
        // This synthesizes and inserts the block "impl" struct, invoke function,
        // and any copy/dispose helper functions.
        InsertBlockLiteralsWithinFunction(FD);
        CurFunctionDef = 0;
        CurFunctionDeclToDeclareForBlock = 0;
      }
      break;
    }
    case Decl::ObjCMethod: {
      ObjCMethodDecl *MD = cast<ObjCMethodDecl>(D);
      if (CompoundStmt *Body = MD->getCompoundBody()) {
        CurMethodDef = MD;
        CurrentBody = Body;
        Body =
          cast_or_null<CompoundStmt>(RewriteFunctionBodyOrGlobalInitializer(Body));
        MD->setBody(Body);
        CurrentBody = 0;
        if (PropParentMap) {
          delete PropParentMap;
          PropParentMap = 0;
        }
        InsertBlockLiteralsWithinMethod(MD);
        CurMethodDef = 0;
      }
      break;
    }
    case Decl::ObjCImplementation: {
      ObjCImplementationDecl *CI = cast<ObjCImplementationDecl>(D);
      ClassImplementation.push_back(CI);
      break;
    }
    case Decl::ObjCCategoryImpl: {
      ObjCCategoryImplDecl *CI = cast<ObjCCategoryImplDecl>(D);
      CategoryImplementation.push_back(CI);
      break;
    }
    case Decl::Var: {
      VarDecl *VD = cast<VarDecl>(D);
      RewriteObjCQualifiedInterfaceTypes(VD);
      if (isTopLevelBlockPointerType(VD->getType()))
        RewriteBlockPointerDecl(VD);
      else if (VD->getType()->isFunctionPointerType()) {
        CheckFunctionPointerDecl(VD->getType(), VD);
        if (VD->getInit()) {
          if (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(VD->getInit())) {
            RewriteCastExpr(CE);
          }
        }
      } else if (VD->getType()->isRecordType()) {
        RecordDecl *RD = VD->getType()->getAs<RecordType>()->getDecl();
        if (RD->isCompleteDefinition())
          RewriteRecordBody(RD);
      }
      if (VD->getInit()) {
        GlobalVarDecl = VD;
        CurrentBody = VD->getInit();
        RewriteFunctionBodyOrGlobalInitializer(VD->getInit());
        CurrentBody = 0;
        if (PropParentMap) {
          delete PropParentMap;
          PropParentMap = 0;
        }
        SynthesizeBlockLiterals(VD->getTypeSpecStartLoc(), VD->getName());
        GlobalVarDecl = 0;
          
        // This is needed for blocks.
        if (CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(VD->getInit())) {
            RewriteCastExpr(CE);
        }
      }
      break;
    }
    case Decl::TypeAlias:
    case Decl::Typedef: {
      if (TypedefNameDecl *TD = dyn_cast<TypedefNameDecl>(D)) {
        if (isTopLevelBlockPointerType(TD->getUnderlyingType()))
          RewriteBlockPointerDecl(TD);
        else if (TD->getUnderlyingType()->isFunctionPointerType())
          CheckFunctionPointerDecl(TD->getUnderlyingType(), TD);
      }
      break;
    }
    case Decl::CXXRecord:
    case Decl::Record: {
      RecordDecl *RD = cast<RecordDecl>(D);
      if (RD->isCompleteDefinition()) 
        RewriteRecordBody(RD);
      break;
    }
    default:
      break;
  }
  // Nothing yet.
}

void RewriteModernObjC::HandleTranslationUnit(ASTContext &C) {
  if (Diags.hasErrorOccurred())
    return;

  RewriteInclude();

  // Here's a great place to add any extra declarations that may be needed.
  // Write out meta data for each @protocol(<expr>).
  for (llvm::SmallPtrSet<ObjCProtocolDecl *,8>::iterator I = ProtocolExprDecls.begin(),
       E = ProtocolExprDecls.end(); I != E; ++I)
    RewriteObjCProtocolMetaData(*I, Preamble);

  InsertText(SM->getLocForStartOfFile(MainFileID), Preamble, false);
  for (unsigned i = 0, e = ObjCInterfacesSeen.size(); i < e; i++) {
    ObjCInterfaceDecl *CDecl = ObjCInterfacesSeen[i];
    // Write struct declaration for the class matching its ivar declarations.
    // Note that for modern abi, this is postponed until the end of TU
    // because class extensions and the implementation might declare their own
    // private ivars.
    RewriteInterfaceDecl(CDecl);
  }
  
  if (ClassImplementation.size() || CategoryImplementation.size())
    RewriteImplementations();

  // Get the buffer corresponding to MainFileID.  If we haven't changed it, then
  // we are done.
  if (const RewriteBuffer *RewriteBuf =
      Rewrite.getRewriteBufferFor(MainFileID)) {
    //printf("Changed:\n");
    *OutFile << std::string(RewriteBuf->begin(), RewriteBuf->end());
  } else {
    llvm::errs() << "No changes\n";
  }

  if (ClassImplementation.size() || CategoryImplementation.size() ||
      ProtocolExprDecls.size()) {
    // Rewrite Objective-c meta data*
    std::string ResultStr;
    RewriteMetaDataIntoBuffer(ResultStr);
    // Emit metadata.
    *OutFile << ResultStr;
  }
  OutFile->flush();
}

void RewriteModernObjC::Initialize(ASTContext &context) {
  InitializeCommon(context);
  
  Preamble += "#ifndef __OBJC2__\n";
  Preamble += "#define __OBJC2__\n";
  Preamble += "#endif\n";

  // declaring objc_selector outside the parameter list removes a silly
  // scope related warning...
  if (IsHeader)
    Preamble = "#pragma once\n";
  Preamble += "struct objc_selector; struct objc_class;\n";
  Preamble += "struct __rw_objc_super { struct objc_object *object; ";
  Preamble += "struct objc_object *superClass; ";
  if (LangOpts.MicrosoftExt) {
    // Add a constructor for creating temporary objects.
    Preamble += "__rw_objc_super(struct objc_object *o, struct objc_object *s) "
    ": ";
    Preamble += "object(o), superClass(s) {} ";
  }
  Preamble += "};\n";
  Preamble += "#ifndef _REWRITER_typedef_Protocol\n";
  Preamble += "typedef struct objc_object Protocol;\n";
  Preamble += "#define _REWRITER_typedef_Protocol\n";
  Preamble += "#endif\n";
  if (LangOpts.MicrosoftExt) {
    Preamble += "#define __OBJC_RW_DLLIMPORT extern \"C\" __declspec(dllimport)\n";
    Preamble += "#define __OBJC_RW_STATICIMPORT extern \"C\"\n";
  } else
    Preamble += "#define __OBJC_RW_DLLIMPORT extern\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object *objc_msgSend";
  Preamble += "(struct objc_object *, struct objc_selector *, ...);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object *objc_msgSendSuper";
  Preamble += "(struct objc_super *, struct objc_selector *, ...);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object* objc_msgSend_stret";
  Preamble += "(struct objc_object *, struct objc_selector *, ...);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object* objc_msgSendSuper_stret";
  Preamble += "(struct objc_super *, struct objc_selector *, ...);\n";
  Preamble += "__OBJC_RW_DLLIMPORT double objc_msgSend_fpret";
  Preamble += "(struct objc_object *, struct objc_selector *, ...);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object *objc_getClass";
  Preamble += "(const char *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_class *class_getSuperclass";
  Preamble += "(struct objc_class *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object *objc_getMetaClass";
  Preamble += "(const char *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void objc_exception_throw(struct objc_object *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void objc_exception_try_enter(void *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void objc_exception_try_exit(void *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT struct objc_object *objc_exception_extract(void *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT int objc_exception_match";
  Preamble += "(struct objc_class *, struct objc_object *);\n";
  // @synchronized hooks.
  Preamble += "__OBJC_RW_DLLIMPORT void objc_sync_enter(struct objc_object *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void objc_sync_exit(struct objc_object *);\n";
  Preamble += "__OBJC_RW_DLLIMPORT Protocol *objc_getProtocol(const char *);\n";
  Preamble += "#ifndef __FASTENUMERATIONSTATE\n";
  Preamble += "struct __objcFastEnumerationState {\n\t";
  Preamble += "unsigned long state;\n\t";
  Preamble += "void **itemsPtr;\n\t";
  Preamble += "unsigned long *mutationsPtr;\n\t";
  Preamble += "unsigned long extra[5];\n};\n";
  Preamble += "__OBJC_RW_DLLIMPORT void objc_enumerationMutation(struct objc_object *);\n";
  Preamble += "#define __FASTENUMERATIONSTATE\n";
  Preamble += "#endif\n";
  Preamble += "#ifndef __NSCONSTANTSTRINGIMPL\n";
  Preamble += "struct __NSConstantStringImpl {\n";
  Preamble += "  int *isa;\n";
  Preamble += "  int flags;\n";
  Preamble += "  char *str;\n";
  Preamble += "  long length;\n";
  Preamble += "};\n";
  Preamble += "#ifdef CF_EXPORT_CONSTANT_STRING\n";
  Preamble += "extern \"C\" __declspec(dllexport) int __CFConstantStringClassReference[];\n";
  Preamble += "#else\n";
  Preamble += "__OBJC_RW_DLLIMPORT int __CFConstantStringClassReference[];\n";
  Preamble += "#endif\n";
  Preamble += "#define __NSCONSTANTSTRINGIMPL\n";
  Preamble += "#endif\n";
  // Blocks preamble.
  Preamble += "#ifndef BLOCK_IMPL\n";
  Preamble += "#define BLOCK_IMPL\n";
  Preamble += "struct __block_impl {\n";
  Preamble += "  void *isa;\n";
  Preamble += "  int Flags;\n";
  Preamble += "  int Reserved;\n";
  Preamble += "  void *FuncPtr;\n";
  Preamble += "};\n";
  Preamble += "// Runtime copy/destroy helper functions (from Block_private.h)\n";
  Preamble += "#ifdef __OBJC_EXPORT_BLOCKS\n";
  Preamble += "extern \"C\" __declspec(dllexport) "
  "void _Block_object_assign(void *, const void *, const int);\n";
  Preamble += "extern \"C\" __declspec(dllexport) void _Block_object_dispose(const void *, const int);\n";
  Preamble += "extern \"C\" __declspec(dllexport) void *_NSConcreteGlobalBlock[32];\n";
  Preamble += "extern \"C\" __declspec(dllexport) void *_NSConcreteStackBlock[32];\n";
  Preamble += "#else\n";
  Preamble += "__OBJC_RW_DLLIMPORT void _Block_object_assign(void *, const void *, const int);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void _Block_object_dispose(const void *, const int);\n";
  Preamble += "__OBJC_RW_DLLIMPORT void *_NSConcreteGlobalBlock[32];\n";
  Preamble += "__OBJC_RW_DLLIMPORT void *_NSConcreteStackBlock[32];\n";
  Preamble += "#endif\n";
  Preamble += "#endif\n";
  if (LangOpts.MicrosoftExt) {
    Preamble += "#undef __OBJC_RW_DLLIMPORT\n";
    Preamble += "#undef __OBJC_RW_STATICIMPORT\n";
    Preamble += "#ifndef KEEP_ATTRIBUTES\n";  // We use this for clang tests.
    Preamble += "#define __attribute__(X)\n";
    Preamble += "#endif\n";
    Preamble += "#define __weak\n";
  }
  else {
    Preamble += "#define __block\n";
    Preamble += "#define __weak\n";
  }
  // NOTE! Windows uses LLP64 for 64bit mode. So, cast pointer to long long
  // as this avoids warning in any 64bit/32bit compilation model.
  Preamble += "\n#define __OFFSETOFIVAR__(TYPE, MEMBER) ((long long) &((TYPE *)0)->MEMBER)\n";
}

/// RewriteIvarOffsetComputation - This rutine synthesizes computation of
/// ivar offset.
void RewriteModernObjC::RewriteIvarOffsetComputation(ObjCIvarDecl *ivar,
                                                         std::string &Result) {
  if (ivar->isBitField()) {
    // FIXME: The hack below doesn't work for bitfields. For now, we simply
    // place all bitfields at offset 0.
    Result += "0";
  } else {
    Result += "__OFFSETOFIVAR__(struct ";
    Result += ivar->getContainingInterface()->getNameAsString();
    if (LangOpts.MicrosoftExt)
      Result += "_IMPL";
    Result += ", ";
    Result += ivar->getNameAsString();
    Result += ")";
  }
}

/// WriteModernMetadataDeclarations - Writes out metadata declarations for modern ABI.
/// struct _prop_t {
///   const char *name;
///   char *attributes;
/// }

/// struct _prop_list_t {
///   uint32_t entsize;      // sizeof(struct _prop_t)
///   uint32_t count_of_properties;
///   struct _prop_t prop_list[count_of_properties];
/// }

/// struct _protocol_t;

/// struct _protocol_list_t {
///   long protocol_count;   // Note, this is 32/64 bit
///   struct _protocol_t * protocol_list[protocol_count];
/// }

/// struct _objc_method {
///   SEL _cmd;
///   const char *method_type;
///   char *_imp;
/// }

/// struct _method_list_t {
///   uint32_t entsize;  // sizeof(struct _objc_method)
///   uint32_t method_count;
///   struct _objc_method method_list[method_count];
/// }

/// struct _protocol_t {
///   id isa;  // NULL
///   const char * const protocol_name;
///   const struct _protocol_list_t * protocol_list; // super protocols
///   const struct method_list_t * const instance_methods;
///   const struct method_list_t * const class_methods;
///   const struct method_list_t *optionalInstanceMethods;
///   const struct method_list_t *optionalClassMethods;
///   const struct _prop_list_t * properties;
///   const uint32_t size;  // sizeof(struct _protocol_t)
///   const uint32_t flags;  // = 0
///   const char ** extendedMethodTypes;
/// }

/// struct _ivar_t {
///   unsigned long int *offset;  // pointer to ivar offset location
///   const char *name;
///   const char *type;
///   uint32_t alignment;
///   uint32_t size;
/// }

/// struct _ivar_list_t {
///   uint32 entsize;  // sizeof(struct _ivar_t)
///   uint32 count;
///   struct _ivar_t list[count];
/// }

/// struct _class_ro_t {
///   uint32_t const flags;
///   uint32_t const instanceStart;
///   uint32_t const instanceSize;
///   uint32_t const reserved;  // only when building for 64bit targets
///   const uint8_t * const ivarLayout;
///   const char *const name;
///   const struct _method_list_t * const baseMethods;
///   const struct _protocol_list_t *const baseProtocols;
///   const struct _ivar_list_t *const ivars;
///   const uint8_t * const weakIvarLayout;
///   const struct _prop_list_t * const properties;
/// }

/// struct _class_t {
///   struct _class_t *isa;
///   struct _class_t * const superclass;
///   void *cache;
///   IMP *vtable;
///   struct _class_ro_t *ro;
/// }

/// struct _category_t {
///   const char * const name;
///   struct _class_t *const cls;
///   const struct _method_list_t * const instance_methods;
///   const struct _method_list_t * const class_methods;
///   const struct _protocol_list_t * const protocols;
///   const struct _prop_list_t * const properties;
/// }

/// MessageRefTy - LLVM for:
/// struct _message_ref_t {
///   IMP messenger;
///   SEL name;
/// };

/// SuperMessageRefTy - LLVM for:
/// struct _super_message_ref_t {
///   SUPER_IMP messenger;
///   SEL name;
/// };

static void WriteModernMetadataDeclarations(std::string &Result) {
  static bool meta_data_declared = false;
  if (meta_data_declared)
    return;
  
  Result += "\nstruct _prop_t {\n";
  Result += "\tconst char *name;\n";
  Result += "\tconst char *attributes;\n";
  Result += "};\n";
  
  Result += "\nstruct _protocol_t;\n";
  
  Result += "\nstruct _objc_method {\n";
  Result += "\tstruct objc_selector * _cmd;\n";
  Result += "\tconst char *method_type;\n";
  Result += "\tvoid  *_imp;\n";
  Result += "};\n";
  
  Result += "\nstruct _protocol_t {\n";
  Result += "\tvoid * isa;  // NULL\n";
  Result += "\tconst char * const protocol_name;\n";
  Result += "\tconst struct _protocol_list_t * protocol_list; // super protocols\n";
  Result += "\tconst struct method_list_t * const instance_methods;\n";
  Result += "\tconst struct method_list_t * const class_methods;\n";
  Result += "\tconst struct method_list_t *optionalInstanceMethods;\n";
  Result += "\tconst struct method_list_t *optionalClassMethods;\n";
  Result += "\tconst struct _prop_list_t * properties;\n";
  Result += "\tconst unsigned int size;  // sizeof(struct _protocol_t)\n";
  Result += "\tconst unsigned int flags;  // = 0\n";
  Result += "\tconst char ** extendedMethodTypes;\n";
  Result += "};\n";
  
  Result += "\nstruct _ivar_t {\n";
  Result += "\tunsigned long int *offset;  // pointer to ivar offset location\n";
  Result += "\tconst char *name;\n";
  Result += "\tconst char *type;\n";
  Result += "\tunsigned int alignment;\n";
  Result += "\tunsigned int  size;\n";
  Result += "};\n";
  
  Result += "\nstruct _class_ro_t {\n";
  Result += "\tunsigned int const flags;\n";
  Result += "\tunsigned int instanceStart;\n";
  Result += "\tunsigned int const instanceSize;\n";
  Result += "\tunsigned int const reserved;  // only when building for 64bit targets\n";
  Result += "\tconst unsigned char * const ivarLayout;\n";
  Result += "\tconst char *const name;\n";
  Result += "\tconst struct _method_list_t * const baseMethods;\n";
  Result += "\tconst struct _objc_protocol_list *const baseProtocols;\n";
  Result += "\tconst struct _ivar_list_t *const ivars;\n";
  Result += "\tconst unsigned char *const weakIvarLayout;\n";
  Result += "\tconst struct _prop_list_t *const properties;\n";
  Result += "};\n";
  
  Result += "\nstruct _class_t {\n";
  Result += "\tstruct _class_t *isa;\n";
  Result += "\tstruct _class_t *const superclass;\n";
  Result += "\tvoid *cache;\n";
  Result += "\tvoid *vtable;\n";
  Result += "\tstruct _class_ro_t *ro;\n";
  Result += "};\n";
  
  Result += "\nstruct _category_t {\n";
  Result += "\tconst char * const name;\n";
  Result += "\tstruct _class_t *const cls;\n";
  Result += "\tconst struct _method_list_t *const instance_methods;\n";
  Result += "\tconst struct _method_list_t *const class_methods;\n";
  Result += "\tconst struct _protocol_list_t *const protocols;\n";
  Result += "\tconst struct _prop_list_t *const properties;\n";
  Result += "};\n";
  
  Result += "extern void *_objc_empty_cache;\n";
  Result += "extern void *_objc_empty_vtable;\n";
  
  meta_data_declared = true;
}

static void Write_protocol_list_t_TypeDecl(std::string &Result,
                                           long super_protocol_count) {
  Result += "struct /*_protocol_list_t*/"; Result += " {\n";
  Result += "\tlong protocol_count;  // Note, this is 32/64 bit\n";
  Result += "\tstruct _protocol_t *super_protocols[";
  Result += utostr(super_protocol_count); Result += "];\n";
  Result += "}";
}

static void Write_method_list_t_TypeDecl(std::string &Result,
                                         unsigned int method_count) {
  Result += "struct /*_method_list_t*/"; Result += " {\n";
  Result += "\tunsigned int entsize;  // sizeof(struct _objc_method)\n";
  Result += "\tunsigned int method_count;\n";
  Result += "\tstruct _objc_method method_list[";
  Result += utostr(method_count); Result += "];\n";
  Result += "}";
}

static void Write__prop_list_t_TypeDecl(std::string &Result,
                                        unsigned int property_count) {
  Result += "struct /*_prop_list_t*/"; Result += " {\n";
  Result += "\tunsigned int entsize;  // sizeof(struct _prop_t)\n";
  Result += "\tunsigned int count_of_properties;\n";
  Result += "\tstruct _prop_t prop_list[";
  Result += utostr(property_count); Result += "];\n";
  Result += "}";
}

static void Write__ivar_list_t_TypeDecl(std::string &Result,
                                        unsigned int ivar_count) {
  Result += "struct /*_ivar_list_t*/"; Result += " {\n";
  Result += "\tunsigned int entsize;  // sizeof(struct _prop_t)\n";
  Result += "\tunsigned int count;\n";
  Result += "\tstruct _ivar_t ivar_list[";
  Result += utostr(ivar_count); Result += "];\n";
  Result += "}";
}

static void Write_protocol_list_initializer(ASTContext *Context, std::string &Result,
                                            ArrayRef<ObjCProtocolDecl *> SuperProtocols,
                                            StringRef VarName,
                                            StringRef ProtocolName) {
  if (SuperProtocols.size() > 0) {
    Result += "\nstatic ";
    Write_protocol_list_t_TypeDecl(Result, SuperProtocols.size());
    Result += " "; Result += VarName;
    Result += ProtocolName; 
    Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = {\n";
    Result += "\t"; Result += utostr(SuperProtocols.size()); Result += ",\n";
    for (unsigned i = 0, e = SuperProtocols.size(); i < e; i++) {
      ObjCProtocolDecl *SuperPD = SuperProtocols[i];
      Result += "\t&"; Result += "_OBJC_PROTOCOL_"; 
      Result += SuperPD->getNameAsString();
      if (i == e-1)
        Result += "\n};\n";
      else
        Result += ",\n";
    }
  }
}

static void Write_method_list_t_initializer(RewriteModernObjC &RewriteObj,
                                            ASTContext *Context, std::string &Result,
                                            ArrayRef<ObjCMethodDecl *> Methods,
                                            StringRef VarName,
                                            StringRef TopLevelDeclName,
                                            bool MethodImpl) {
  if (Methods.size() > 0) {
    Result += "\nstatic ";
    Write_method_list_t_TypeDecl(Result, Methods.size());
    Result += " "; Result += VarName;
    Result += TopLevelDeclName; 
    Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = {\n";
    Result += "\t"; Result += "sizeof(_objc_method)"; Result += ",\n";
    Result += "\t"; Result += utostr(Methods.size()); Result += ",\n";
    for (unsigned i = 0, e = Methods.size(); i < e; i++) {
      ObjCMethodDecl *MD = Methods[i];
      if (i == 0)
        Result += "\t{{(struct objc_selector *)\"";
      else
        Result += "\t{(struct objc_selector *)\"";
      Result += (MD)->getSelector().getAsString(); Result += "\"";
      Result += ", ";
      std::string MethodTypeString;
      Context->getObjCEncodingForMethodDecl(MD, MethodTypeString);
      Result += "\""; Result += MethodTypeString; Result += "\"";
      Result += ", ";
      if (!MethodImpl)
        Result += "0";
      else {
        Result += "(void *)";
        Result += RewriteObj.MethodInternalNames[MD];
      }
      if (i  == e-1)
        Result += "}}\n";
      else
        Result += "},\n";
    }
    Result += "};\n";
  }
}

static void Write_prop_list_t_initializer(RewriteModernObjC &RewriteObj,
                                           ASTContext *Context, std::string &Result,
                                           ArrayRef<ObjCPropertyDecl *> Properties,
                                           const Decl *Container,
                                           StringRef VarName,
                                           StringRef ProtocolName) {
  if (Properties.size() > 0) {
    Result += "\nstatic ";
    Write__prop_list_t_TypeDecl(Result, Properties.size());
    Result += " "; Result += VarName;
    Result += ProtocolName; 
    Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = {\n";
    Result += "\t"; Result += "sizeof(_prop_t)"; Result += ",\n";
    Result += "\t"; Result += utostr(Properties.size()); Result += ",\n";
    for (unsigned i = 0, e = Properties.size(); i < e; i++) {
      ObjCPropertyDecl *PropDecl = Properties[i];
      if (i == 0)
        Result += "\t{{\"";
      else
        Result += "\t{\"";
      Result += PropDecl->getName(); Result += "\",";
      std::string PropertyTypeString, QuotePropertyTypeString;
      Context->getObjCEncodingForPropertyDecl(PropDecl, Container, PropertyTypeString);
      RewriteObj.QuoteDoublequotes(PropertyTypeString, QuotePropertyTypeString);
      Result += "\""; Result += QuotePropertyTypeString; Result += "\"";
      if (i  == e-1)
        Result += "}}\n";
      else
        Result += "},\n";
    }
    Result += "};\n";
  }
}

// Metadata flags
enum MetaDataDlags {
  CLS = 0x0,
  CLS_META = 0x1,
  CLS_ROOT = 0x2,
  OBJC2_CLS_HIDDEN = 0x10,
  CLS_EXCEPTION = 0x20,
  
  /// (Obsolete) ARC-specific: this class has a .release_ivars method
  CLS_HAS_IVAR_RELEASER = 0x40,
  /// class was compiled with -fobjc-arr
  CLS_COMPILED_BY_ARC = 0x80  // (1<<7)
};

static void Write__class_ro_t_initializer(ASTContext *Context, std::string &Result, 
                                          unsigned int flags, 
                                          const std::string &InstanceStart, 
                                          const std::string &InstanceSize,
                                          ArrayRef<ObjCMethodDecl *>baseMethods,
                                          ArrayRef<ObjCProtocolDecl *>baseProtocols,
                                          ArrayRef<ObjCIvarDecl *>ivars,
                                          ArrayRef<ObjCPropertyDecl *>Properties,
                                          StringRef VarName,
                                          StringRef ClassName) {
  Result += "\nstatic struct _class_ro_t ";
  Result += VarName; Result += ClassName;
  Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = {\n";
  Result += "\t"; 
  Result += llvm::utostr(flags); Result += ", "; 
  Result += InstanceStart; Result += ", ";
  Result += InstanceSize; Result += ", \n";
  Result += "\t";
  // uint32_t const reserved; // only when building for 64bit targets
  Result += "(unsigned int)0, \n\t";
  // const uint8_t * const ivarLayout;
  Result += "0, \n\t";
  Result += "\""; Result += ClassName; Result += "\",\n\t";
  bool metaclass = ((flags & CLS_META) != 0);
  if (baseMethods.size() > 0) {
    Result += "(const struct _method_list_t *)&";
    if (metaclass)
      Result += "_OBJC_$_CLASS_METHODS_";
    else
      Result += "_OBJC_$_INSTANCE_METHODS_";
    Result += ClassName;
    Result += ",\n\t";
  }
  else
    Result += "0, \n\t";

  if (!metaclass && baseProtocols.size() > 0) {
    Result += "(const struct _objc_protocol_list *)&";
    Result += "_OBJC_CLASS_PROTOCOLS_$_"; Result += ClassName;
    Result += ",\n\t";
  }
  else
    Result += "0, \n\t";

  if (!metaclass && ivars.size() > 0) {
    Result += "(const struct _ivar_list_t *)&";
    Result += "_OBJC_$_INSTANCE_VARIABLES_"; Result += ClassName;
    Result += ",\n\t";
  }
  else
    Result += "0, \n\t";

  // weakIvarLayout
  Result += "0, \n\t";
  if (!metaclass && Properties.size() > 0) {
    Result += "(const struct _prop_list_t *)&";
    Result += "_OBJC_$_PROP_LIST_"; Result += ClassName;
    Result += ",\n";
  }
  else
    Result += "0, \n";

  Result += "};\n";
}

static void Write_class_t(ASTContext *Context, std::string &Result,
                          StringRef VarName,
                          const ObjCInterfaceDecl *CDecl, bool metadata) {
  
  if (metadata && !CDecl->getSuperClass()) {
    // Need to handle a case of use of forward declaration.
    Result += "\n";
    if (CDecl->getImplementation())
      Result += "__declspec(dllexport) ";
    Result += "extern struct _class_t OBJC_CLASS_$_";
    Result += CDecl->getNameAsString();
    Result += ";\n";
  }
  // Also, for possibility of 'super' metadata class not having been defined yet.
  if (CDecl->getSuperClass()) {
    Result += "\n";
    if (CDecl->getSuperClass()->getImplementation())
      Result += "__declspec(dllexport) ";
    Result += "extern struct _class_t "; 
    Result += VarName;
    Result += CDecl->getSuperClass()->getNameAsString();
    Result += ";\n";
  }
  
  Result += "\n__declspec(dllexport) struct _class_t "; Result += VarName; Result += CDecl->getNameAsString();
  Result += " __attribute__ ((used, section (\"__DATA,__objc_data\"))) = {\n";
  Result += "\t";
  if (metadata) {
    if (CDecl->getSuperClass()) {
      Result += "&"; Result += VarName;
      Result += CDecl->getSuperClass()->getNameAsString();
      Result += ",\n\t";
      Result += "&"; Result += VarName;
      Result += CDecl->getSuperClass()->getNameAsString();
      Result += ",\n\t";
    }
    else {
      Result += "&"; Result += VarName; 
      Result += CDecl->getNameAsString();
      Result += ",\n\t";
      Result += "&OBJC_CLASS_$_"; Result += CDecl->getNameAsString();
      Result += ",\n\t";
    }
  }
  else {
    Result += "&OBJC_METACLASS_$_"; 
    Result += CDecl->getNameAsString();
    Result += ",\n\t";
    if (CDecl->getSuperClass()) {
      Result += "&"; Result += VarName;
      Result += CDecl->getSuperClass()->getNameAsString();
      Result += ",\n\t";
    }
    else 
      Result += "0,\n\t";
  }
  Result += "(void *)&_objc_empty_cache,\n\t";
  Result += "(void *)&_objc_empty_vtable,\n\t";
  if (metadata)
    Result += "&_OBJC_METACLASS_RO_$_";
  else
    Result += "&_OBJC_CLASS_RO_$_";
  Result += CDecl->getNameAsString();
  Result += ",\n};\n";
}

static void Write_category_t(RewriteModernObjC &RewriteObj, ASTContext *Context, 
                             std::string &Result,
                             StringRef CatName,
                             StringRef ClassName,
                             ArrayRef<ObjCMethodDecl *> InstanceMethods,
                             ArrayRef<ObjCMethodDecl *> ClassMethods,
                             ArrayRef<ObjCProtocolDecl *> RefedProtocols,
                             ArrayRef<ObjCPropertyDecl *> ClassProperties) {
  // must declare an extern class object in case this class is not implemented 
  // in this TU.
  Result += "\nextern struct _class_t ";
  Result += "OBJC_CLASS_$_"; Result += ClassName;
  Result += ";\n";
  
  Result += "\nstatic struct _category_t ";
  Result += "_OBJC_$_CATEGORY_";
  Result += ClassName; Result += "_$_"; Result += CatName;
  Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = \n";
  Result += "{\n";
  Result += "\t\""; Result += ClassName; Result += "\",\n";
  Result += "\t&"; Result += "OBJC_CLASS_$_"; Result += ClassName;
  Result += ",\n";
  if (InstanceMethods.size() > 0) {
    Result += "\t(const struct _method_list_t *)&";  
    Result += "_OBJC_$_CATEGORY_INSTANCE_METHODS_";
    Result += ClassName; Result += "_$_"; Result += CatName;
    Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (ClassMethods.size() > 0) {
    Result += "\t(const struct _method_list_t *)&";  
    Result += "_OBJC_$_CATEGORY_CLASS_METHODS_";
    Result += ClassName; Result += "_$_"; Result += CatName;
    Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (RefedProtocols.size() > 0) {
    Result += "\t(const struct _protocol_list_t *)&";  
    Result += "_OBJC_CATEGORY_PROTOCOLS_$_";
    Result += ClassName; Result += "_$_"; Result += CatName;
    Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (ClassProperties.size() > 0) {
    Result += "\t(const struct _prop_list_t *)&";  Result += "_OBJC_$_PROP_LIST_";
    Result += ClassName; Result += "_$_"; Result += CatName;
    Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  Result += "};\n";
}

static void Write__extendedMethodTypes_initializer(RewriteModernObjC &RewriteObj,
                                           ASTContext *Context, std::string &Result,
                                           ArrayRef<ObjCMethodDecl *> Methods,
                                           StringRef VarName,
                                           StringRef ProtocolName) {
  if (Methods.size() == 0)
    return;
  
  Result += "\nstatic const char *";
  Result += VarName; Result += ProtocolName;
  Result += " [] __attribute__ ((used, section (\"__DATA,__objc_const\"))) = \n";
  Result += "{\n";
  for (unsigned i = 0, e = Methods.size(); i < e; i++) {
    ObjCMethodDecl *MD = Methods[i];
    std::string MethodTypeString, QuoteMethodTypeString;
    Context->getObjCEncodingForMethodDecl(MD, MethodTypeString, true);
    RewriteObj.QuoteDoublequotes(MethodTypeString, QuoteMethodTypeString);
    Result += "\t\""; Result += QuoteMethodTypeString; Result += "\"";
    if (i == e-1)
      Result += "\n};\n";
    else {
      Result += ",\n";
    }
  }
}

static void Write_IvarOffsetVar(std::string &Result, 
                                ArrayRef<ObjCIvarDecl *> Ivars, 
                                StringRef VarName, 
                                StringRef ClassName) {
  // FIXME. visibilty of offset symbols may have to be set; for Darwin
  // this is what happens:
  /**
   if (Ivar->getAccessControl() == ObjCIvarDecl::Private ||
       Ivar->getAccessControl() == ObjCIvarDecl::Package ||
       Class->getVisibility() == HiddenVisibility)
     Visibility shoud be: HiddenVisibility;
   else
     Visibility shoud be: DefaultVisibility;
  */
  
  Result += "\n";
  for (unsigned i =0, e = Ivars.size(); i < e; i++) {
    ObjCIvarDecl *IvarDecl = Ivars[i];
    if (IvarDecl->getAccessControl() == ObjCIvarDecl::Private ||
        IvarDecl->getAccessControl() == ObjCIvarDecl::Package)
      Result += "unsigned long int "; 
    else
      Result += "__declspec(dllexport) unsigned long int ";
    
    Result += VarName;
    Result += ClassName; Result += "_";
    Result += IvarDecl->getName(); 
    Result += " __attribute__ ((used, section (\"__DATA,__objc_ivar\")))";
    Result += " = ";
    if (IvarDecl->isBitField()) {
      // FIXME: The hack below doesn't work for bitfields. For now, we simply
      // place all bitfields at offset 0.
      Result += "0;\n";
    }
    else {
      Result += "__OFFSETOFIVAR__(struct ";
      Result += ClassName;
      Result += "_IMPL, "; 
      Result += IvarDecl->getName(); Result += ");\n";
    }
  }
}

static void Write__ivar_list_t_initializer(RewriteModernObjC &RewriteObj,
                                           ASTContext *Context, std::string &Result,
                                           ArrayRef<ObjCIvarDecl *> Ivars,
                                           StringRef VarName,
                                           StringRef ClassName) {
  if (Ivars.size() > 0) {
    Write_IvarOffsetVar(Result, Ivars, "OBJC_IVAR_$_", ClassName);
    
    Result += "\nstatic ";
    Write__ivar_list_t_TypeDecl(Result, Ivars.size());
    Result += " "; Result += VarName;
    Result += ClassName;
    Result += " __attribute__ ((used, section (\"__DATA,__objc_const\"))) = {\n";
    Result += "\t"; Result += "sizeof(_ivar_t)"; Result += ",\n";
    Result += "\t"; Result += utostr(Ivars.size()); Result += ",\n";
    for (unsigned i =0, e = Ivars.size(); i < e; i++) {
      ObjCIvarDecl *IvarDecl = Ivars[i];
      if (i == 0)
        Result += "\t{{";
      else
        Result += "\t {";
      
      Result += "(unsigned long int *)&OBJC_IVAR_$_";
      Result += ClassName; Result += "_"; Result += IvarDecl->getName();
      Result += ", ";
      
      Result += "\""; Result += IvarDecl->getName(); Result += "\", ";
      std::string IvarTypeString, QuoteIvarTypeString;
      Context->getObjCEncodingForType(IvarDecl->getType(), IvarTypeString,
                                      IvarDecl);
      RewriteObj.QuoteDoublequotes(IvarTypeString, QuoteIvarTypeString);
      Result += "\""; Result += QuoteIvarTypeString; Result += "\", ";
      
      // FIXME. this alignment represents the host alignment and need be changed to
      // represent the target alignment.
      unsigned Align = Context->getTypeAlign(IvarDecl->getType())/8;
      Align = llvm::Log2_32(Align);
      Result += llvm::utostr(Align); Result += ", ";
      CharUnits Size = Context->getTypeSizeInChars(IvarDecl->getType());
      Result += llvm::utostr(Size.getQuantity());
      if (i  == e-1)
        Result += "}}\n";
      else
        Result += "},\n";
    }
    Result += "};\n";
  }
}

/// RewriteObjCProtocolMetaData - Rewrite protocols meta-data.
void RewriteModernObjC::RewriteObjCProtocolMetaData(ObjCProtocolDecl *PDecl, 
                                                    std::string &Result) {
  
  // Do not synthesize the protocol more than once.
  if (ObjCSynthesizedProtocols.count(PDecl->getCanonicalDecl()))
    return;
  WriteModernMetadataDeclarations(Result);
  
  if (ObjCProtocolDecl *Def = PDecl->getDefinition())
    PDecl = Def;
  // Must write out all protocol definitions in current qualifier list,
  // and in their nested qualifiers before writing out current definition.
  for (ObjCProtocolDecl::protocol_iterator I = PDecl->protocol_begin(),
       E = PDecl->protocol_end(); I != E; ++I)
    RewriteObjCProtocolMetaData(*I, Result);
  
  // Construct method lists.
  std::vector<ObjCMethodDecl *> InstanceMethods, ClassMethods;
  std::vector<ObjCMethodDecl *> OptInstanceMethods, OptClassMethods;
  for (ObjCProtocolDecl::instmeth_iterator
       I = PDecl->instmeth_begin(), E = PDecl->instmeth_end();
       I != E; ++I) {
    ObjCMethodDecl *MD = *I;
    if (MD->getImplementationControl() == ObjCMethodDecl::Optional) {
      OptInstanceMethods.push_back(MD);
    } else {
      InstanceMethods.push_back(MD);
    }
  }
  
  for (ObjCProtocolDecl::classmeth_iterator
       I = PDecl->classmeth_begin(), E = PDecl->classmeth_end();
       I != E; ++I) {
    ObjCMethodDecl *MD = *I;
    if (MD->getImplementationControl() == ObjCMethodDecl::Optional) {
      OptClassMethods.push_back(MD);
    } else {
      ClassMethods.push_back(MD);
    }
  }
  std::vector<ObjCMethodDecl *> AllMethods;
  for (unsigned i = 0, e = InstanceMethods.size(); i < e; i++)
    AllMethods.push_back(InstanceMethods[i]);
  for (unsigned i = 0, e = ClassMethods.size(); i < e; i++)
    AllMethods.push_back(ClassMethods[i]);
  for (unsigned i = 0, e = OptInstanceMethods.size(); i < e; i++)
    AllMethods.push_back(OptInstanceMethods[i]);
  for (unsigned i = 0, e = OptClassMethods.size(); i < e; i++)
    AllMethods.push_back(OptClassMethods[i]);

  Write__extendedMethodTypes_initializer(*this, Context, Result,
                                         AllMethods,
                                         "_OBJC_PROTOCOL_METHOD_TYPES_",
                                         PDecl->getNameAsString());
  // Protocol's super protocol list
  std::vector<ObjCProtocolDecl *> SuperProtocols;
  for (ObjCProtocolDecl::protocol_iterator I = PDecl->protocol_begin(),
       E = PDecl->protocol_end(); I != E; ++I)
    SuperProtocols.push_back(*I);
  
  Write_protocol_list_initializer(Context, Result, SuperProtocols,
                                  "_OBJC_PROTOCOL_REFS_",
                                  PDecl->getNameAsString());
  
  Write_method_list_t_initializer(*this, Context, Result, InstanceMethods, 
                                  "_OBJC_PROTOCOL_INSTANCE_METHODS_",
                                  PDecl->getNameAsString(), false);
  
  Write_method_list_t_initializer(*this, Context, Result, ClassMethods, 
                                  "_OBJC_PROTOCOL_CLASS_METHODS_",
                                  PDecl->getNameAsString(), false);

  Write_method_list_t_initializer(*this, Context, Result, OptInstanceMethods, 
                                  "_OBJC_PROTOCOL_OPT_INSTANCE_METHODS_",
                                  PDecl->getNameAsString(), false);
  
  Write_method_list_t_initializer(*this, Context, Result, OptClassMethods, 
                                  "_OBJC_PROTOCOL_OPT_CLASS_METHODS_",
                                  PDecl->getNameAsString(), false);
  
  // Protocol's property metadata.
  std::vector<ObjCPropertyDecl *> ProtocolProperties;
  for (ObjCContainerDecl::prop_iterator I = PDecl->prop_begin(),
       E = PDecl->prop_end(); I != E; ++I)
    ProtocolProperties.push_back(*I);
  
  Write_prop_list_t_initializer(*this, Context, Result, ProtocolProperties,
                                 /* Container */0,
                                 "_OBJC_PROTOCOL_PROPERTIES_",
                                 PDecl->getNameAsString());
  
  // Writer out root metadata for current protocol: struct _protocol_t
  Result += "\nstatic struct _protocol_t _OBJC_PROTOCOL_";
  Result += PDecl->getNameAsString();
  Result += " __attribute__ ((used, section (\"__DATA,__datacoal_nt,coalesced\"))) = {\n";
  Result += "\t0,\n"; // id is; is null
  Result += "\t\""; Result += PDecl->getNameAsString(); Result += "\",\n";
  if (SuperProtocols.size() > 0) {
    Result += "\t(const struct _protocol_list_t *)&"; Result += "_OBJC_PROTOCOL_REFS_";
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";
  if (InstanceMethods.size() > 0) {
    Result += "\t(const struct method_list_t *)&_OBJC_PROTOCOL_INSTANCE_METHODS_"; 
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";

  if (ClassMethods.size() > 0) {
    Result += "\t(const struct method_list_t *)&_OBJC_PROTOCOL_CLASS_METHODS_"; 
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (OptInstanceMethods.size() > 0) {
    Result += "\t(const struct method_list_t *)&_OBJC_PROTOCOL_OPT_INSTANCE_METHODS_"; 
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (OptClassMethods.size() > 0) {
    Result += "\t(const struct method_list_t *)&_OBJC_PROTOCOL_OPT_CLASS_METHODS_"; 
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  if (ProtocolProperties.size() > 0) {
    Result += "\t(const struct _prop_list_t *)&_OBJC_PROTOCOL_PROPERTIES_"; 
    Result += PDecl->getNameAsString(); Result += ",\n";
  }
  else
    Result += "\t0,\n";
  
  Result += "\t"; Result += "sizeof(_protocol_t)"; Result += ",\n";
  Result += "\t0,\n";
  
  if (AllMethods.size() > 0) {
    Result += "\t(const char **)&"; Result += "_OBJC_PROTOCOL_METHOD_TYPES_";
    Result += PDecl->getNameAsString();
    Result += "\n};\n";
  }
  else
    Result += "\t0\n};\n";
    
  // Mark this protocol as having been generated.
  if (!ObjCSynthesizedProtocols.insert(PDecl->getCanonicalDecl()))
    llvm_unreachable("protocol already synthesized");
  
}

void RewriteModernObjC::RewriteObjCProtocolListMetaData(
                                const ObjCList<ObjCProtocolDecl> &Protocols,
                                StringRef prefix, StringRef ClassName,
                                std::string &Result) {
  if (Protocols.empty()) return;
  
  for (unsigned i = 0; i != Protocols.size(); i++)
    RewriteObjCProtocolMetaData(Protocols[i], Result);
  
  // Output the top lovel protocol meta-data for the class.
  /* struct _objc_protocol_list {
   struct _objc_protocol_list *next;
   int    protocol_count;
   struct _objc_protocol *class_protocols[];
   }
   */
  Result += "\nstatic struct {\n";
  Result += "\tstruct _objc_protocol_list *next;\n";
  Result += "\tint    protocol_count;\n";
  Result += "\tstruct _objc_protocol *class_protocols[";
  Result += utostr(Protocols.size());
  Result += "];\n} _OBJC_";
  Result += prefix;
  Result += "_PROTOCOLS_";
  Result += ClassName;
  Result += " __attribute__ ((used, section (\"__OBJC, __cat_cls_meth\")))= "
  "{\n\t0, ";
  Result += utostr(Protocols.size());
  Result += "\n";
  
  Result += "\t,{&_OBJC_PROTOCOL_";
  Result += Protocols[0]->getNameAsString();
  Result += " \n";
  
  for (unsigned i = 1; i != Protocols.size(); i++) {
    Result += "\t ,&_OBJC_PROTOCOL_";
    Result += Protocols[i]->getNameAsString();
    Result += "\n";
  }
  Result += "\t }\n};\n";
}

/// hasObjCExceptionAttribute - Return true if this class or any super
/// class has the __objc_exception__ attribute.
/// FIXME. Move this to ASTContext.cpp as it is also used for IRGen.
static bool hasObjCExceptionAttribute(ASTContext &Context,
                                      const ObjCInterfaceDecl *OID) {
  if (OID->hasAttr<ObjCExceptionAttr>())
    return true;
  if (const ObjCInterfaceDecl *Super = OID->getSuperClass())
    return hasObjCExceptionAttribute(Context, Super);
  return false;
}

void RewriteModernObjC::RewriteObjCClassMetaData(ObjCImplementationDecl *IDecl,
                                           std::string &Result) {
  ObjCInterfaceDecl *CDecl = IDecl->getClassInterface();
  
  // Explicitly declared @interface's are already synthesized.
  if (CDecl->isImplicitInterfaceDecl())
    assert(false && 
           "Legacy implicit interface rewriting not supported in moder abi");
  
  WriteModernMetadataDeclarations(Result);
  SmallVector<ObjCIvarDecl *, 8> IVars;
  
  for (ObjCIvarDecl *IVD = CDecl->all_declared_ivar_begin();
      IVD; IVD = IVD->getNextIvar()) {
    // Ignore unnamed bit-fields.
    if (!IVD->getDeclName())
      continue;
    IVars.push_back(IVD);
  }
  
  Write__ivar_list_t_initializer(*this, Context, Result, IVars, 
                                 "_OBJC_$_INSTANCE_VARIABLES_",
                                 CDecl->getNameAsString());
  
  // Build _objc_method_list for class's instance methods if needed
  SmallVector<ObjCMethodDecl *, 32>
    InstanceMethods(IDecl->instmeth_begin(), IDecl->instmeth_end());
  
  // If any of our property implementations have associated getters or
  // setters, produce metadata for them as well.
  for (ObjCImplDecl::propimpl_iterator Prop = IDecl->propimpl_begin(),
       PropEnd = IDecl->propimpl_end();
       Prop != PropEnd; ++Prop) {
    if ((*Prop)->getPropertyImplementation() == ObjCPropertyImplDecl::Dynamic)
      continue;
    if (!(*Prop)->getPropertyIvarDecl())
      continue;
    ObjCPropertyDecl *PD = (*Prop)->getPropertyDecl();
    if (!PD)
      continue;
    if (ObjCMethodDecl *Getter = PD->getGetterMethodDecl())
      if (!Getter->isDefined())
        InstanceMethods.push_back(Getter);
    if (PD->isReadOnly())
      continue;
    if (ObjCMethodDecl *Setter = PD->getSetterMethodDecl())
      if (!Setter->isDefined())
        InstanceMethods.push_back(Setter);
  }
  
  Write_method_list_t_initializer(*this, Context, Result, InstanceMethods,
                                  "_OBJC_$_INSTANCE_METHODS_",
                                  IDecl->getNameAsString(), true);
  
  SmallVector<ObjCMethodDecl *, 32>
    ClassMethods(IDecl->classmeth_begin(), IDecl->classmeth_end());
  
  Write_method_list_t_initializer(*this, Context, Result, ClassMethods,
                                  "_OBJC_$_CLASS_METHODS_",
                                  IDecl->getNameAsString(), true);
  
  // Protocols referenced in class declaration?
  // Protocol's super protocol list
  std::vector<ObjCProtocolDecl *> RefedProtocols;
  const ObjCList<ObjCProtocolDecl> &Protocols = CDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
       E = Protocols.end();
       I != E; ++I) {
    RefedProtocols.push_back(*I);
    // Must write out all protocol definitions in current qualifier list,
    // and in their nested qualifiers before writing out current definition.
    RewriteObjCProtocolMetaData(*I, Result);
  }
  
  Write_protocol_list_initializer(Context, Result, 
                                  RefedProtocols,
                                  "_OBJC_CLASS_PROTOCOLS_$_",
                                  IDecl->getNameAsString());
  
  // Protocol's property metadata.
  std::vector<ObjCPropertyDecl *> ClassProperties;
  for (ObjCContainerDecl::prop_iterator I = CDecl->prop_begin(),
       E = CDecl->prop_end(); I != E; ++I)
    ClassProperties.push_back(*I);
  
  Write_prop_list_t_initializer(*this, Context, Result, ClassProperties,
                                 /* Container */0,
                                 "_OBJC_$_PROP_LIST_",
                                 CDecl->getNameAsString());

  
  // Data for initializing _class_ro_t  metaclass meta-data
  uint32_t flags = CLS_META;
  std::string InstanceSize;
  std::string InstanceStart;
  
  
  bool classIsHidden = CDecl->getVisibility() == HiddenVisibility;
  if (classIsHidden)
    flags |= OBJC2_CLS_HIDDEN;
  
  if (!CDecl->getSuperClass())
    // class is root
    flags |= CLS_ROOT;
  InstanceSize = "sizeof(struct _class_t)";
  InstanceStart = InstanceSize;
  Write__class_ro_t_initializer(Context, Result, flags, 
                                InstanceStart, InstanceSize,
                                ClassMethods,
                                0,
                                0,
                                0,
                                "_OBJC_METACLASS_RO_$_",
                                CDecl->getNameAsString());

  
  // Data for initializing _class_ro_t meta-data
  flags = CLS;
  if (classIsHidden)
    flags |= OBJC2_CLS_HIDDEN;
  
  if (hasObjCExceptionAttribute(*Context, CDecl))
    flags |= CLS_EXCEPTION;

  if (!CDecl->getSuperClass())
    // class is root
    flags |= CLS_ROOT;
  
  InstanceSize.clear();
  InstanceStart.clear();
  if (!ObjCSynthesizedStructs.count(CDecl)) {
    InstanceSize = "0";
    InstanceStart = "0";
  }
  else {
    InstanceSize = "sizeof(struct ";
    InstanceSize += CDecl->getNameAsString();
    InstanceSize += "_IMPL)";
    
    ObjCIvarDecl *IVD = CDecl->all_declared_ivar_begin();
    if (IVD) {
      InstanceStart += "__OFFSETOFIVAR__(struct ";
      InstanceStart += CDecl->getNameAsString();
      InstanceStart += "_IMPL, ";
      InstanceStart += IVD->getNameAsString();
      InstanceStart += ")";
    }
    else 
      InstanceStart = InstanceSize;
  }
  Write__class_ro_t_initializer(Context, Result, flags, 
                                InstanceStart, InstanceSize,
                                InstanceMethods,
                                RefedProtocols,
                                IVars,
                                ClassProperties,
                                "_OBJC_CLASS_RO_$_",
                                CDecl->getNameAsString());
  
  Write_class_t(Context, Result,
                "OBJC_METACLASS_$_",
                CDecl, /*metaclass*/true);
  
  Write_class_t(Context, Result,
                "OBJC_CLASS_$_",
                CDecl, /*metaclass*/false);
                
}

void RewriteModernObjC::RewriteMetaDataIntoBuffer(std::string &Result) {
  int ClsDefCount = ClassImplementation.size();
  int CatDefCount = CategoryImplementation.size();
  
  // For each implemented class, write out all its meta data.
  for (int i = 0; i < ClsDefCount; i++)
    RewriteObjCClassMetaData(ClassImplementation[i], Result);
  
  // For each implemented category, write out all its meta data.
  for (int i = 0; i < CatDefCount; i++)
    RewriteObjCCategoryImplDecl(CategoryImplementation[i], Result);
  
  if (ClsDefCount > 0) {
    Result += "static struct _class_t *L_OBJC_LABEL_CLASS_$ [";
    Result += llvm::utostr(ClsDefCount); Result += "]";
    Result += 
      " __attribute__((used, section (\"__DATA, __objc_classlist,"
      "regular,no_dead_strip\")))= {\n";
    for (int i = 0; i < ClsDefCount; i++) {
      Result += "\t&OBJC_CLASS_$_";
      Result += ClassImplementation[i]->getNameAsString();
      Result += ",\n";
    }
    Result += "};\n";
  }
  
  if (CatDefCount > 0) {
    Result += "static struct _category_t *L_OBJC_LABEL_CATEGORY_$ [";
    Result += llvm::utostr(CatDefCount); Result += "]";
    Result += 
    " __attribute__((used, section (\"__DATA, __objc_catlist,"
    "regular,no_dead_strip\")))= {\n";
    for (int i = 0; i < CatDefCount; i++) {
      Result += "\t&_OBJC_$_CATEGORY_";
      Result += 
        CategoryImplementation[i]->getClassInterface()->getNameAsString(); 
      Result += "_$_";
      Result += CategoryImplementation[i]->getNameAsString();
      Result += ",\n";
    }
    Result += "};\n";
  }
}

/// RewriteObjCCategoryImplDecl - Rewrite metadata for each category
/// implementation.
void RewriteModernObjC::RewriteObjCCategoryImplDecl(ObjCCategoryImplDecl *IDecl,
                                              std::string &Result) {
  WriteModernMetadataDeclarations(Result);
  ObjCInterfaceDecl *ClassDecl = IDecl->getClassInterface();
  // Find category declaration for this implementation.
  ObjCCategoryDecl *CDecl=0;
  for (CDecl = ClassDecl->getCategoryList(); CDecl;
       CDecl = CDecl->getNextClassCategory())
    if (CDecl->getIdentifier() == IDecl->getIdentifier())
      break;
  
  std::string FullCategoryName = ClassDecl->getNameAsString();
  FullCategoryName += "_$_";
  FullCategoryName += CDecl->getNameAsString();
  
  // Build _objc_method_list for class's instance methods if needed
  SmallVector<ObjCMethodDecl *, 32>
  InstanceMethods(IDecl->instmeth_begin(), IDecl->instmeth_end());
  
  // If any of our property implementations have associated getters or
  // setters, produce metadata for them as well.
  for (ObjCImplDecl::propimpl_iterator Prop = IDecl->propimpl_begin(),
       PropEnd = IDecl->propimpl_end();
       Prop != PropEnd; ++Prop) {
    if ((*Prop)->getPropertyImplementation() == ObjCPropertyImplDecl::Dynamic)
      continue;
    if (!(*Prop)->getPropertyIvarDecl())
      continue;
    ObjCPropertyDecl *PD = (*Prop)->getPropertyDecl();
    if (!PD)
      continue;
    if (ObjCMethodDecl *Getter = PD->getGetterMethodDecl())
      InstanceMethods.push_back(Getter);
    if (PD->isReadOnly())
      continue;
    if (ObjCMethodDecl *Setter = PD->getSetterMethodDecl())
      InstanceMethods.push_back(Setter);
  }
  
  Write_method_list_t_initializer(*this, Context, Result, InstanceMethods,
                                  "_OBJC_$_CATEGORY_INSTANCE_METHODS_",
                                  FullCategoryName, true);
  
  SmallVector<ObjCMethodDecl *, 32>
    ClassMethods(IDecl->classmeth_begin(), IDecl->classmeth_end());
  
  Write_method_list_t_initializer(*this, Context, Result, ClassMethods,
                                  "_OBJC_$_CATEGORY_CLASS_METHODS_",
                                  FullCategoryName, true);
  
  // Protocols referenced in class declaration?
  // Protocol's super protocol list
  std::vector<ObjCProtocolDecl *> RefedProtocols;
  const ObjCList<ObjCProtocolDecl> &Protocols = CDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
       E = Protocols.end();
       I != E; ++I) {
    RefedProtocols.push_back(*I);
    // Must write out all protocol definitions in current qualifier list,
    // and in their nested qualifiers before writing out current definition.
    RewriteObjCProtocolMetaData(*I, Result);
  }
  
  Write_protocol_list_initializer(Context, Result, 
                                  RefedProtocols,
                                  "_OBJC_CATEGORY_PROTOCOLS_$_",
                                  FullCategoryName);
  
  // Protocol's property metadata.
  std::vector<ObjCPropertyDecl *> ClassProperties;
  for (ObjCContainerDecl::prop_iterator I = CDecl->prop_begin(),
       E = CDecl->prop_end(); I != E; ++I)
    ClassProperties.push_back(*I);
  
  Write_prop_list_t_initializer(*this, Context, Result, ClassProperties,
                                /* Container */0,
                                "_OBJC_$_PROP_LIST_",
                                FullCategoryName);
  
  Write_category_t(*this, Context, Result,
                   CDecl->getNameAsString(),
                   ClassDecl->getNameAsString(),
                   InstanceMethods,
                   ClassMethods,
                   RefedProtocols,
                   ClassProperties);
  
}

// RewriteObjCMethodsMetaData - Rewrite methods metadata for instance or
/// class methods.
template<typename MethodIterator>
void RewriteModernObjC::RewriteObjCMethodsMetaData(MethodIterator MethodBegin,
                                             MethodIterator MethodEnd,
                                             bool IsInstanceMethod,
                                             StringRef prefix,
                                             StringRef ClassName,
                                             std::string &Result) {
  if (MethodBegin == MethodEnd) return;
  
  if (!objc_impl_method) {
    /* struct _objc_method {
     SEL _cmd;
     char *method_types;
     void *_imp;
     }
     */
    Result += "\nstruct _objc_method {\n";
    Result += "\tSEL _cmd;\n";
    Result += "\tchar *method_types;\n";
    Result += "\tvoid *_imp;\n";
    Result += "};\n";
    
    objc_impl_method = true;
  }
  
  // Build _objc_method_list for class's methods if needed
  
  /* struct  {
   struct _objc_method_list *next_method;
   int method_count;
   struct _objc_method method_list[];
   }
   */
  unsigned NumMethods = std::distance(MethodBegin, MethodEnd);
  Result += "\nstatic struct {\n";
  Result += "\tstruct _objc_method_list *next_method;\n";
  Result += "\tint method_count;\n";
  Result += "\tstruct _objc_method method_list[";
  Result += utostr(NumMethods);
  Result += "];\n} _OBJC_";
  Result += prefix;
  Result += IsInstanceMethod ? "INSTANCE" : "CLASS";
  Result += "_METHODS_";
  Result += ClassName;
  Result += " __attribute__ ((used, section (\"__OBJC, __";
  Result += IsInstanceMethod ? "inst" : "cls";
  Result += "_meth\")))= ";
  Result += "{\n\t0, " + utostr(NumMethods) + "\n";
  
  Result += "\t,{{(SEL)\"";
  Result += (*MethodBegin)->getSelector().getAsString().c_str();
  std::string MethodTypeString;
  Context->getObjCEncodingForMethodDecl(*MethodBegin, MethodTypeString);
  Result += "\", \"";
  Result += MethodTypeString;
  Result += "\", (void *)";
  Result += MethodInternalNames[*MethodBegin];
  Result += "}\n";
  for (++MethodBegin; MethodBegin != MethodEnd; ++MethodBegin) {
    Result += "\t  ,{(SEL)\"";
    Result += (*MethodBegin)->getSelector().getAsString().c_str();
    std::string MethodTypeString;
    Context->getObjCEncodingForMethodDecl(*MethodBegin, MethodTypeString);
    Result += "\", \"";
    Result += MethodTypeString;
    Result += "\", (void *)";
    Result += MethodInternalNames[*MethodBegin];
    Result += "}\n";
  }
  Result += "\t }\n};\n";
}

Stmt *RewriteModernObjC::RewriteObjCIvarRefExpr(ObjCIvarRefExpr *IV) {
  SourceRange OldRange = IV->getSourceRange();
  Expr *BaseExpr = IV->getBase();
  
  // Rewrite the base, but without actually doing replaces.
  {
    DisableReplaceStmtScope S(*this);
    BaseExpr = cast<Expr>(RewriteFunctionBodyOrGlobalInitializer(BaseExpr));
    IV->setBase(BaseExpr);
  }
  
  ObjCIvarDecl *D = IV->getDecl();
  
  Expr *Replacement = IV;
  
    if (BaseExpr->getType()->isObjCObjectPointerType()) {
      const ObjCInterfaceType *iFaceDecl =
      dyn_cast<ObjCInterfaceType>(BaseExpr->getType()->getPointeeType());
      assert(iFaceDecl && "RewriteObjCIvarRefExpr - iFaceDecl is null");
      // lookup which class implements the instance variable.
      ObjCInterfaceDecl *clsDeclared = 0;
      iFaceDecl->getDecl()->lookupInstanceVariable(D->getIdentifier(),
                                                   clsDeclared);
      assert(clsDeclared && "RewriteObjCIvarRefExpr(): Can't find class");
      
      // Build name of symbol holding ivar offset.
      std::string IvarOffsetName = "OBJC_IVAR_$_";
      IvarOffsetName += clsDeclared->getIdentifier()->getName();
      IvarOffsetName += "_";
      IvarOffsetName += D->getName();
      ReferencedIvars[clsDeclared].insert(D);
      
      // cast offset to "char *".
      CastExpr *castExpr = NoTypeInfoCStyleCastExpr(Context, 
                                                    Context->getPointerType(Context->CharTy),
                                                    CK_BitCast,
                                                    BaseExpr);
      VarDecl *NewVD = VarDecl::Create(*Context, TUDecl, SourceLocation(),
                                       SourceLocation(), &Context->Idents.get(IvarOffsetName),
                                       Context->UnsignedLongTy, 0, SC_Extern, SC_None);
      DeclRefExpr *DRE = new (Context) DeclRefExpr(NewVD, false,
                                                   Context->UnsignedLongTy, VK_LValue,
                                                   SourceLocation());
      BinaryOperator *addExpr = 
        new (Context) BinaryOperator(castExpr, DRE, BO_Add, 
                                     Context->getPointerType(Context->CharTy),
                                     VK_RValue, OK_Ordinary, SourceLocation());
      // Don't forget the parens to enforce the proper binding.
      ParenExpr *PE = new (Context) ParenExpr(SourceLocation(),
                                              SourceLocation(),
                                              addExpr);
      QualType IvarT = D->getType();
      convertObjCTypeToCStyleType(IvarT);
      QualType castT = Context->getPointerType(IvarT);
      
      castExpr = NoTypeInfoCStyleCastExpr(Context, 
                                          castT,
                                          CK_BitCast,
                                          PE);
      Expr *Exp = new (Context) UnaryOperator(castExpr, UO_Deref, IvarT,
                                              VK_LValue, OK_Ordinary,
                                              SourceLocation());
      PE = new (Context) ParenExpr(OldRange.getBegin(),
                                   OldRange.getEnd(),
                                   Exp);

      Replacement = PE;
    }
  
    ReplaceStmtWithRange(IV, Replacement, OldRange);
    return Replacement;  
}

