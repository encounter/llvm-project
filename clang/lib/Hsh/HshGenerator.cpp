//===--- HshGenerator.cpp - Codegen for hshgen tool -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Config/config.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_carray_ostream.h"
#include "llvm/Support/raw_comment_ostream.h"
#include "llvm/Support/xxhash.h"

#include "clang/AST/ASTDumper.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/FileManager.h"
#include "clang/Config/config.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Hsh/HshGenerator.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"

#include "dxc/dxcapi.h"

#include "compiler_iface.h"

#define XSTR(X) #X
#define STR(X) XSTR(X)

#define ENABLE_DUMP 0

namespace llvm {

template <> struct DenseMapInfo<APSInt> {
  static APSInt getEmptyKey() {
    return APSInt::get(DenseMapInfo<int64_t>::getEmptyKey());
  }

  static APSInt getTombstoneKey() {
    return APSInt::get(DenseMapInfo<int64_t>::getTombstoneKey());
  }

  static unsigned getHashValue(const APSInt &Val) {
    return DenseMapInfo<int64_t>::getHashValue(Val.getSExtValue());
  }

  static bool isEqual(const APSInt &LHS, const APSInt &RHS) {
    return APSInt::compareValues(LHS, RHS) == 0;
  }
};

template <> struct DenseMapInfo<clang::SourceLocation> {
  static clang::SourceLocation getEmptyKey() { return {}; }

  static clang::SourceLocation getTombstoneKey() {
    return clang::SourceLocation::getFromRawEncoding(~(0u));
  }

  static unsigned getHashValue(const clang::SourceLocation &Val) {
    return DenseMapInfo<unsigned>::getHashValue(Val.getRawEncoding());
  }

  static bool isEqual(const clang::SourceLocation &LHS,
                      const clang::SourceLocation &RHS) {
    return LHS == RHS;
  }
};

template <> struct DenseMapInfo<clang::hshgen::HshTarget> {
  static clang::hshgen::HshTarget getEmptyKey() {
    return clang::hshgen::HshTarget(DenseMapInfo<int>::getEmptyKey());
  }

  static clang::hshgen::HshTarget getTombstoneKey() {
    return clang::hshgen::HshTarget(DenseMapInfo<int>::getTombstoneKey());
  }

  static unsigned getHashValue(const clang::hshgen::HshTarget &Val) {
    return DenseMapInfo<int>::getHashValue(int(Val));
  }

  static bool isEqual(const clang::hshgen::HshTarget &LHS,
                      const clang::hshgen::HshTarget &RHS) {
    return LHS == RHS;
  }
};

} // end namespace llvm

namespace {

using namespace llvm;
using namespace clang;
using namespace clang::hshgen;
using namespace std::literals;

constexpr StringRef operator""_ll(const char *__str, size_t __len) noexcept {
  return StringRef{__str, __len};
}

template <typename T, typename TIter = decltype(std::begin(std::declval<T>())),
          typename = decltype(std::end(std::declval<T>()))>
constexpr auto enumerate(T &&iterable) {
  struct iterator {
    size_t i;
    TIter iter;
    bool operator!=(const iterator &other) const { return iter != other.iter; }
    void operator++() {
      ++i;
      ++iter;
    }
    auto operator*() const { return std::tie(i, *iter); }
  };
  struct iterable_wrapper {
    T iterable;
    auto begin() { return iterator{0, std::begin(iterable)}; }
    auto end() { return iterator{0, std::end(iterable)}; }
  };
  return iterable_wrapper{std::forward<T>(iterable)};
}

#ifdef __EMULATE_UUID
#define HSH_IID_PPV_ARGS(ppType)                                               \
  DxcLibrary::SharedInstance->UUIDs.get<std::decay_t<decltype(**(ppType))>>(), \
      reinterpret_cast<void **>(ppType)
#else // __EMULATE_UUID
#define HSH_IID_PPV_ARGS(ppType)                                               \
  __uuidof(**(ppType)), IID_PPV_ARGS_Helper(ppType)
#endif // __EMULATE_UUID

class DxcLibrary {
  sys::DynamicLibrary Library;
  DxcCreateInstanceProc DxcCreateInstance;

public:
  static llvm::Optional<DxcLibrary> SharedInstance;
  static void EnsureSharedInstance(StringRef ResourceDir,
                                   DiagnosticsEngine &Diags) {
    if (!SharedInstance)
      SharedInstance.emplace(ResourceDir, Diags);
  }

#ifdef __EMULATE_UUID
  struct ImportedUUIDs {
    void *_IUnknown = nullptr;
    void *_IDxcBlob = nullptr;
    void *_IDxcBlobUtf8 = nullptr;
    void *_IDxcResult = nullptr;
    void *_IDxcCompiler3 = nullptr;
    void import(sys::DynamicLibrary &Library) {
      _IUnknown = Library.getAddressOfSymbol("_ZN8IUnknown11IUnknown_IDE");
      _IDxcBlob = Library.getAddressOfSymbol("_ZN8IDxcBlob11IDxcBlob_IDE");
      _IDxcBlobUtf8 =
          Library.getAddressOfSymbol("_ZN12IDxcBlobUtf815IDxcBlobUtf8_IDE");
      _IDxcResult =
          Library.getAddressOfSymbol("_ZN10IDxcResult13IDxcResult_IDE");
      _IDxcCompiler3 =
          Library.getAddressOfSymbol("_ZN13IDxcCompiler316IDxcCompiler3_IDE");
    }
    template <typename T> REFIID get();
  } UUIDs;
#endif

  explicit DxcLibrary(StringRef ResourceDir, DiagnosticsEngine &Diags) {
    std::string Err;
#if LLVM_ON_UNIX
    SmallString<128> LibPath(ResourceDir);
    sys::path::append(LibPath, "libdxcompiler" LTDL_SHLIB_EXT);
    Library = sys::DynamicLibrary::getPermanentLibrary(LibPath.c_str(), &Err);
#else
    Library = sys::DynamicLibrary::getPermanentLibrary("dxcompiler.dll", &Err);
#endif
    if (!Library.isValid()) {
      Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                         "unable to load %0; %1"))
          << LibPath << Err;
      return;
    }
    DxcCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
        Library.getAddressOfSymbol("DxcCreateInstance"));
    if (!DxcCreateInstance) {
      Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                         "unable to find DxcCreateInstance"));
      return;
    }
#ifdef __EMULATE_UUID
    UUIDs.import(Library);
#endif
  }

  CComPtr<IDxcCompiler3> MakeCompiler() const;
};
llvm::Optional<DxcLibrary> DxcLibrary::SharedInstance;

#ifdef __EMULATE_UUID
template <> REFIID DxcLibrary::ImportedUUIDs::get<IUnknown>() {
  return _IUnknown;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcBlob>() {
  return _IDxcBlob;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcBlobUtf8>() {
  return _IDxcBlobUtf8;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcResult>() {
  return _IDxcResult;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcCompiler3>() {
  return _IDxcCompiler3;
}
#endif

CComPtr<IDxcCompiler3> DxcLibrary::MakeCompiler() const {
  CComPtr<IDxcCompiler3> Ret;
  DxcCreateInstance(CLSID_DxcCompiler, HSH_IID_PPV_ARGS(&Ret));
  return Ret;
}

enum HshStage : int {
  HshNoStage = -1,
  HshVertexStage = 0,
  HshControlStage,
  HshEvaluationStage,
  HshGeometryStage,
  HshFragmentStage,
  HshMaxStage
};

constexpr StringRef HshStageToString(HshStage Stage) {
  switch (Stage) {
  case HshVertexStage:
    return "vertex"_ll;
  case HshControlStage:
    return "control"_ll;
  case HshEvaluationStage:
    return "evaluation"_ll;
  case HshGeometryStage:
    return "geometry"_ll;
  case HshFragmentStage:
    return "fragment"_ll;
  default:
    return "none"_ll;
  }
}

enum HshAttributeKind { PerVertex, PerInstance };

enum HshFormat : uint8_t {
  R8_UNORM,
  RG8_UNORM,
  RGBA8_UNORM,
  R16_UNORM,
  RG16_UNORM,
  RGBA16_UNORM,
  R32_UINT,
  RG32_UINT,
  RGB32_UINT,
  RGBA32_UINT,
  R8_SNORM,
  RG8_SNORM,
  RGBA8_SNORM,
  R16_SNORM,
  RG16_SNORM,
  RGBA16_SNORM,
  R32_SINT,
  RG32_SINT,
  RGB32_SINT,
  RGBA32_SINT,
  R32_SFLOAT,
  RG32_SFLOAT,
  RGB32_SFLOAT,
  RGBA32_SFLOAT,
  BC1_UNORM,
  BC2_UNORM,
  BC3_UNORM,
};

enum Topology {
  TPL_Points,
  TPL_Lines,
  TPL_LineStrip,
  TPL_Triangles,
  TPL_TriangleStrip,
  TPL_TriangleFan,
  TPL_Patches
};

enum CullMode { CM_CullNone, CM_CullFront, CM_CullBack, CM_CullFrontAndBack };

enum Compare {
  CMP_Never,
  CMP_Less,
  CMP_Equal,
  CMP_LEqual,
  CMP_Greater,
  CMP_NEqual,
  CMP_GEqual,
  CMP_Always
};

enum BlendFactor {
  BF_Zero,
  BF_One,
  BF_SrcColor,
  BF_InvSrcColor,
  BF_DstColor,
  BF_InvDstColor,
  BF_SrcAlpha,
  BF_InvSrcAlpha,
  BF_DstAlpha,
  BF_InvDstAlpha,
  BF_ConstColor,
  BF_InvConstColor,
  BF_ConstAlpha,
  BF_InvConstAlpha,
  BF_Src1Color,
  BF_InvSrc1Color,
  BF_Src1Alpha,
  BF_InvSrc1Alpha
};

enum BlendOp { BO_Add, BO_Subtract, BO_ReverseSubtract };

enum ColorComponentFlags : unsigned {
  CC_Red = 1,
  CC_Green = 2,
  CC_Blue = 4,
  CC_Alpha = 8
};

struct StageBits {
  unsigned Bits = 0;
  operator unsigned() const { return Bits; }
  StageBits &operator=(unsigned NewBits) {
    Bits = NewBits;
    return *this;
  }
  StageBits &operator|=(unsigned NewBits) {
    Bits |= NewBits;
    return *this;
  }
};

class CommaArgPrinter {
  raw_ostream &OS;
  bool NeedsComma = false;

public:
  explicit CommaArgPrinter(raw_ostream &OS) : OS(OS) {}
  raw_ostream &addArg() {
    if (NeedsComma)
      OS << ", ";
    else
      NeedsComma = true;
    return OS;
  }
};

class GeneratorDumper {
public:
#if ENABLE_DUMP
  PrintingPolicy Policy{LangOptions{}};
  static void PrintStageBits(raw_ostream &OS, StageBits Bits) {
    CommaArgPrinter ArgPrinter(OS);
    for (int i = HshVertexStage; i < HshMaxStage; ++i) {
      if ((1 << i) & Bits) {
        ArgPrinter.addArg() << HshStageToString(HshStage(i));
      }
    }
  }

  template <
      typename T,
      std::enable_if_t<!std::is_base_of_v<Stmt, std::remove_pointer_t<T>> &&
                           !std::is_base_of_v<Decl, std::remove_pointer_t<T>> &&
                           !std::is_same_v<QualType, std::decay_t<T>> &&
                           !std::is_same_v<StageBits, std::decay_t<T>> &&
                           !std::is_same_v<HshStage, std::decay_t<T>>,
                       int> = 0>
  GeneratorDumper &operator<<(const T &Obj) {
    llvm::errs() << Obj;
    return *this;
  }
  GeneratorDumper &operator<<(const Stmt *S) {
    S->printPretty(llvm::errs(), nullptr, Policy);
    return *this;
  }
  GeneratorDumper &operator<<(const Decl *D) {
    D->print(llvm::errs(), Policy);
    return *this;
  }
  GeneratorDumper &operator<<(const QualType T) {
    T.print(llvm::errs(), Policy);
    return *this;
  }
  GeneratorDumper &operator<<(const StageBits B) {
    PrintStageBits(llvm::errs(), B);
    return *this;
  }
  GeneratorDumper &operator<<(const HshStage S) {
    llvm::errs() << HshStageToString(S);
    return *this;
  }
  void setPrintingPolicy(const PrintingPolicy &PP) { Policy = PP; }
#else
  template <typename T> GeneratorDumper &operator<<(const T &Obj) {
    return *this;
  }
  void setPrintingPolicy(const PrintingPolicy &PP) {}
#endif
};

GeneratorDumper &dumper() {
  static GeneratorDumper GD;
  return GD;
}

QualType ResolveParmType(const VarDecl *D) {
  if (D->getType()->isTemplateTypeParmType())
    if (auto *Init = D->getInit())
      return Init->getType();
  return D->getType();
}

template <typename T,
          std::enable_if_t<!std::is_base_of_v<Attr, std::remove_pointer_t<T>>,
                           int> = 0>
std::pair<SourceLocation, SourceRange> GetReportLocation(const T *S) {
  return {S->getBeginLoc(), S->getSourceRange()};
}
template <typename T,
          std::enable_if_t<std::is_base_of_v<Attr, std::remove_pointer_t<T>>,
                           int> = 0>
std::pair<SourceLocation, SourceRange> GetReportLocation(const T *S) {
  return {S->getLocation(), S->getRange()};
}

template <typename T, unsigned N>
DiagnosticBuilder
ReportCustom(const T *S, const ASTContext &Context,
             const char (&FormatString)[N],
             DiagnosticsEngine::Level level = DiagnosticsEngine::Error) {
  auto [Loc, Range] = GetReportLocation(S);
  DiagnosticsEngine &Diags = Context.getDiagnostics();
  return Diags.Report(Loc, Diags.getCustomDiagID(level, FormatString))
         << CharSourceRange(Range, false);
}

void ReportUnsupportedStmt(const Stmt *S, const ASTContext &Context) {
  auto Diag = ReportCustom(
      S, Context,
      "statements of type %0 are not supported in hsh generator lambdas");
  Diag.AddString(S->getStmtClassName());
}

void ReportUnsupportedFunctionCall(const Stmt *S, const ASTContext &Context) {
  ReportCustom(S, Context,
               "function calls are limited to hsh intrinsics and static "
               "constexpr functions");
}

void ReportUnsupportedTypeReference(const Stmt *S, const ASTContext &Context) {
  ReportCustom(S, Context, "references to values are limited to hsh types");
}

void ReportUnsupportedTypeConstruct(const Stmt *S, const ASTContext &Context) {
  ReportCustom(S, Context, "constructors are limited to hsh types");
}

void ReportUnsupportedTypeCast(const Stmt *S, const ASTContext &Context) {
  ReportCustom(S, Context, "type casts are limited to hsh types");
}

void ReportBadTextureReference(const Stmt *S, const ASTContext &Context) {
  ReportCustom(S, Context,
               "texture samples must be performed on lambda parameters");
}

void ReportNonConstexprSampler(const Expr *E, const ASTContext &Context) {
  ReportCustom(E, Context, "sampler arguments must be constexpr");
}

void ReportBadIntegerType(const Decl *D, const ASTContext &Context) {
  ReportCustom(D, Context, "integers must be 32-bits in length");
}

void ReportBadRecordType(const Decl *D, const ASTContext &Context) {
  ReportCustom(D, Context,
               "hsh record fields must be a builtin hsh vector or matrix, "
               "float, double, or 32-bit integer");
}

void ReportConstAssignment(const Expr *AssignExpr, const ASTContext &Context) {
  ReportCustom(AssignExpr, Context, "cannot assign data to previous stages");
}

void ReportOverloadedFunctionUsage(const FunctionDecl *Overloaded,
                                   const FunctionDecl *Prev,
                                   const ASTContext &Context) {
  ReportCustom(Overloaded, Context,
               "overloaded functions may not be used in the same pipeline");
  ReportCustom(Prev, Context, "previously used function is here",
               DiagnosticsEngine::Note);
}

void ReportUndefinedFunctionUsage(const FunctionDecl *FD,
                                  const ASTContext &Context) {
  ReportCustom(FD, Context, "constexpr functions must be fully defined");
}

enum HshBuiltinType {
  HBT_None,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) HBT_##Name,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned) HBT_##Name,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  HBT_##Name,
#define BUILTIN_ENUM_TYPE(Name) HBT_##Name,
#include "BuiltinTypes.def"
  HBT_Max
};

enum HshBuiltinFunction {
  HBF_None,
#define BUILTIN_FUNCTION(Name, Spelling, GLSL, HLSL, Metal, InterpDist, ...)   \
  HBF_##Name,
#include "BuiltinFunctions.def"
  HBF_Max
};

enum HshBuiltinCXXMethod {
  HBM_None,
#define BUILTIN_CXX_METHOD(Name, Spelling, IsSwizzle, Record, ...) HBM_##Name,
#include "BuiltinCXXMethods.def"
  HBM_Max
};

enum HshBuiltinPipelineField {
  HPF_None,
#define PIPELINE_FIELD(Name, Stage) HPF_##Name,
#include "ShaderInterface.def"
  HPF_Max
};

class NonConstExpr {
public:
  enum Kind { NonTypeParm, TypePush, TypePop, Integer };

private:
  enum Kind Kind;
  PointerUnion<Expr *, ClassTemplateSpecializationDecl *, const clang::Type *>
      ExprOrType;
  APSInt Int;
  unsigned Position;

public:
  explicit NonConstExpr(Expr *E, unsigned Position)
      : Kind(NonTypeParm), ExprOrType(E), Position(Position) {}
  explicit NonConstExpr(ClassTemplateSpecializationDecl *D, unsigned Position)
      : Kind(TypePush), ExprOrType(D), Position(Position) {}
  struct Pop {};
  explicit NonConstExpr(Pop)
      : Kind(TypePop), ExprOrType(nullptr), Position(0) {}
  explicit NonConstExpr(const APSInt &Int, QualType Tp)
      : Kind(Integer), ExprOrType(Tp.getTypePtr()), Int(Int) {}

  enum Kind getKind() const { return Kind; }
  Expr *getExpr() const {
    assert(Kind == NonTypeParm);
    return ExprOrType.get<Expr *>();
  }
  unsigned getPosition() const {
    assert(Kind == NonTypeParm || Kind == TypePush);
    return Position;
  }
  ClassTemplateSpecializationDecl *getType() const {
    assert(Kind == TypePush);
    return ExprOrType.get<ClassTemplateSpecializationDecl *>();
  }
  const APSInt &getInt() const {
    assert(Kind == Integer);
    return Int;
  }
  QualType getIntType() const {
    assert(Kind == Integer);
    return QualType{ExprOrType.get<const clang::Type *>(), 0};
  }
};

bool CheckConstexprTemplateSpecialization(
    ASTContext &Context, QualType Tp,
    SmallVectorImpl<NonConstExpr> *NonConstExprs = nullptr,
    unsigned Position = 0);

bool CheckConstexprTemplateSpecialization(
    ASTContext &Context, ClassTemplateSpecializationDecl *Spec,
    SmallVectorImpl<NonConstExpr> *NonConstExprs = nullptr) {
  bool Ret = true;
  unsigned Position = 0;
  for (auto &Arg : Spec->getTemplateArgs().asArray()) {
    switch (Arg.getKind()) {
    case TemplateArgument::Type:
      Ret &= CheckConstexprTemplateSpecialization(Context, Arg.getAsType(),
                                                  NonConstExprs, Position);
      break;
    case TemplateArgument::Expression: {
      APSInt Value;
      if (!Arg.getAsExpr()->isIntegerConstantExpr(Value, Context)) {
        Ret = false;
        if (NonConstExprs)
          NonConstExprs->emplace_back(Arg.getAsExpr(), Position);
      } else if (NonConstExprs) {
        NonConstExprs->emplace_back(Value, Arg.getAsExpr()->getType());
      }
      break;
    }
    case TemplateArgument::Integral:
      if (NonConstExprs)
        NonConstExprs->emplace_back(Arg.getAsIntegral(), Arg.getIntegralType());
      break;
    default:
      break;
    }
    ++Position;
  }
  return Ret;
}

bool CheckConstexprTemplateSpecialization(
    ASTContext &Context, QualType Tp,
    SmallVectorImpl<NonConstExpr> *NonConstExprs, unsigned Position) {
  if (auto *ExpDecl = Tp->getAsCXXRecordDecl()) {
    if (auto *Spec = dyn_cast<ClassTemplateSpecializationDecl>(ExpDecl)) {
      if (NonConstExprs)
        NonConstExprs->emplace_back(Spec, Position);
      bool Ret =
          CheckConstexprTemplateSpecialization(Context, Spec, NonConstExprs);
      if (NonConstExprs)
        NonConstExprs->emplace_back(NonConstExpr::Pop{});
      return Ret;
    }
  }
  return true;
}

template <typename Func>
void TraverseNonConstExprs(ArrayRef<NonConstExpr> NCEs, Func F) {
  std::stack<ClassTemplateSpecializationDecl *,
             SmallVector<ClassTemplateSpecializationDecl *, 4>>
      TypeStack;
  for (auto &Expr : NCEs) {
    switch (Expr.getKind()) {
    case NonConstExpr::NonTypeParm:
      F(cast<NonTypeTemplateParmDecl>(TypeStack.top()
                                          ->getSpecializedTemplateOrPartial()
                                          .get<ClassTemplateDecl *>()
                                          ->getTemplateParameters()
                                          ->getParam(Expr.getPosition())));
      break;
    case NonConstExpr::TypePush:
      TypeStack.push(Expr.getType());
      break;
    case NonConstExpr::TypePop:
      TypeStack.pop();
      break;
    case NonConstExpr::Integer:
      break;
    }
  }
}

template <typename Func, typename PushFunc, typename PopFunc,
          typename IntegerFunc>
void TraverseNonConstExprs(ArrayRef<NonConstExpr> NCEs, Func F, PushFunc Push,
                           PopFunc Pop, IntegerFunc Integer) {
  std::stack<ClassTemplateSpecializationDecl *,
             SmallVector<ClassTemplateSpecializationDecl *, 4>>
      TypeStack;
  for (auto &Expr : NCEs) {
    switch (Expr.getKind()) {
    case NonConstExpr::NonTypeParm:
      F(cast<NonTypeTemplateParmDecl>(TypeStack.top()
                                          ->getSpecializedTemplateOrPartial()
                                          .get<ClassTemplateDecl *>()
                                          ->getTemplateParameters()
                                          ->getParam(Expr.getPosition())));
      break;
    case NonConstExpr::TypePush:
      Push(Expr.getType());
      TypeStack.push(Expr.getType());
      break;
    case NonConstExpr::TypePop:
      Pop();
      TypeStack.pop();
      break;
    case NonConstExpr::Integer:
      Integer(Expr.getInt(), Expr.getIntType());
      break;
    }
  }
}

template <typename Func>
void TraverseNonConstExprs(ArrayRef<NonConstExpr> NCEs,
                           ClassTemplateSpecializationDecl *Spec, Func F) {
  std::stack<ClassTemplateSpecializationDecl *,
             SmallVector<ClassTemplateSpecializationDecl *, 4>>
      TypeStack, SpecStack;
  for (auto &Expr : NCEs) {
    switch (Expr.getKind()) {
    case NonConstExpr::NonTypeParm:
      F(cast<NonTypeTemplateParmDecl>(TypeStack.top()
                                          ->getSpecializedTemplateOrPartial()
                                          .get<ClassTemplateDecl *>()
                                          ->getTemplateParameters()
                                          ->getParam(Expr.getPosition())),
        SpecStack.top()->getTemplateArgs().get(Expr.getPosition()));
      break;
    case NonConstExpr::TypePush:
      TypeStack.push(Expr.getType());
      if (SpecStack.empty())
        SpecStack.push(Spec);
      else
        SpecStack.push(
            cast<ClassTemplateSpecializationDecl>(SpecStack.top()
                                                      ->getTemplateArgs()
                                                      .get(Expr.getPosition())
                                                      .getAsType()
                                                      ->getAsCXXRecordDecl()));
      break;
    case NonConstExpr::TypePop:
      TypeStack.pop();
      SpecStack.pop();
      break;
    case NonConstExpr::Integer:
      break;
    }
  }
}

class HshBuiltins {
public:
  struct Spellings {
    StringRef GLSL, HLSL, Metal;
  };

  class PipelineAttributes : public DeclVisitor<PipelineAttributes, bool> {
    using base = DeclVisitor<PipelineAttributes, bool>;
    ClassTemplateDecl *BaseAttributeDecl = nullptr;
    ClassTemplateDecl *ColorAttachmentDecl = nullptr;
    CXXRecordDecl *DualSourceDecl = nullptr;
    CXXRecordDecl *DirectRenderDecl = nullptr;
    CXXRecordDecl *HighPriorityDecl = nullptr;
    SmallVector<ClassTemplateDecl *, 8> Attributes; // Non-color-attachments
    SmallVector<ClassTemplateDecl *, 1>
        InShaderAttributes; // Just for early depth stencil
    ClassTemplateDecl *PipelineDecl = nullptr;

  public:
    bool VisitDecl(Decl *D) {
      if (auto *DC = dyn_cast<DeclContext>(D))
        for (Decl *Child : DC->decls())
          if (!base::Visit(Child))
            return false;
      return true;
    }

    bool VisitClassTemplateDecl(ClassTemplateDecl *CTD) {
      if (CTD->getName() == "base_attribute") {
        BaseAttributeDecl = CTD;
        return true;
      }
      if (CTD->getName() == "pipeline") {
        PipelineDecl = CTD;
        return true;
      }
      if (BaseAttributeDecl) {
        auto *TemplatedDecl = CTD->getTemplatedDecl();
        if (TemplatedDecl->getNumBases() != 1)
          return true;
        if (auto BaseSpec = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                TemplatedDecl->bases_begin()
                    ->getType()
                    ->getAsCXXRecordDecl())) {
          if (BaseSpec->getSpecializedTemplateOrPartial()
                  .get<ClassTemplateDecl *>() == BaseAttributeDecl) {
            if (BaseSpec->getTemplateArgs()[0].getAsIntegral().getZExtValue()) {
              ColorAttachmentDecl = CTD;
              return true;
            } else if (BaseSpec->getTemplateArgs()[1]
                           .getAsIntegral()
                           .getZExtValue()) {
              InShaderAttributes.push_back(CTD);
              return true;
            } else {
              Attributes.push_back(CTD);
              return true;
            }
          }
        }
      }
      return true;
    }

    bool VisitCXXRecordDecl(CXXRecordDecl *CRD) {
      if (CRD->getName() == "dual_source")
        DualSourceDecl = CRD;
      else if (CRD->getName() == "direct_render")
        DirectRenderDecl = CRD;
      else if (CRD->getName() == "high_priority")
        HighPriorityDecl = CRD;
      return true;
    }

    static void ValidateAttributeDecl(ASTContext &Context,
                                      ClassTemplateDecl *CTD) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      for (const auto *Parm : *CTD->getTemplateParameters()) {
        if (auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Parm)) {
          if (auto *DefArg = NTTP->getDefaultArgument()) {
            if (auto *DRE = dyn_cast<DeclRefExpr>(DefArg)) {
              if (isa<NonTypeTemplateParmDecl>(DRE->getDecl()))
                continue;
            } else if (!DefArg->isValueDependent() &&
                       DefArg->isIntegerConstantExpr(Context, {})) {
              continue;
            }
          }
        }
        Diags.Report(CTD->getBeginLoc(),
                     Diags.getCustomDiagID(
                         DiagnosticsEngine::Error,
                         "all pipeline attributes in hsh.h must contain only "
                         "default-initialized non-type template parameters."))
            << CTD->getSourceRange();
      }
    }

    void findDecls(ASTContext &Context, NamespaceDecl *PipelineNS) {
      Visit(PipelineNS);
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      if (!BaseAttributeDecl) {
        Diags.Report(Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "unable to locate declaration of class "
                                      "template hsh::pipeline::base_attribute; "
                                      "is hsh.h included?"));
      }
      if (!PipelineDecl) {
        Diags.Report(Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "unable to locate declaration of class "
                                      "template hsh::pipeline::pipeline; "
                                      "is hsh.h included?"));
      }
      if (!ColorAttachmentDecl) {
        Diags.Report(
            Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                  "unable to locate declaration of class "
                                  "template hsh::pipeline::color_attachment; "
                                  "is hsh.h included?"));
      } else {
        ValidateAttributeDecl(Context, ColorAttachmentDecl);
      }
      for (auto *CTD : Attributes)
        ValidateAttributeDecl(Context, CTD);
      for (auto *CTD : InShaderAttributes)
        ValidateAttributeDecl(Context, CTD);
    }

    ClassTemplateDecl *getPipelineDecl() const { return PipelineDecl; }

    auto
    getColorAttachmentArgs(const ClassTemplateSpecializationDecl *PipelineSpec,
                           bool &DualSourceAdded) const {
      assert(PipelineSpec->getSpecializedTemplateOrPartial()
                 .get<ClassTemplateDecl *>() == PipelineDecl);
      SmallVector<ArrayRef<TemplateArgument>, 4> Ret;
      for (const auto &Arg :
           PipelineSpec->getTemplateArgs()[0].getPackAsArray()) {
        if (Arg.getKind() == TemplateArgument::Type) {
          if (auto *CTD = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                  Arg.getAsType()->getAsCXXRecordDecl())) {
            if (CTD->getSpecializedTemplateOrPartial()
                    .get<ClassTemplateDecl *>() == ColorAttachmentDecl) {
              Ret.push_back(CTD->getTemplateArgs().asArray());
            }
          } else if (Arg.getAsType()->getAsCXXRecordDecl() == DualSourceDecl) {
            DualSourceAdded = true;
          }
        }
      }
      return Ret;
    }

    auto
    getPipelineArgs(ASTContext &Context,
                    const ClassTemplateSpecializationDecl *PipelineSpec) const {
      assert(PipelineSpec->getSpecializedTemplateOrPartial()
                 .get<ClassTemplateDecl *>() == PipelineDecl);
      SmallVector<TemplateArgument, 8> Ret;
      for (auto *RefCTD : Attributes) {
        bool Handled = false;
        for (const auto &Arg :
             PipelineSpec->getTemplateArgs()[0].getPackAsArray()) {
          if (Arg.getKind() == TemplateArgument::Type) {
            if (auto *CTD = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                    Arg.getAsType()->getAsCXXRecordDecl())) {
              if (CTD->getSpecializedTemplateOrPartial()
                      .get<ClassTemplateDecl *>() == RefCTD) {
                for (const auto &ArgIn : CTD->getTemplateArgs().asArray())
                  Ret.push_back(ArgIn);
                Handled = true;
                break;
              }
            }
          }
        }
        if (!Handled) {
          for (const auto *Parm : *RefCTD->getTemplateParameters()) {
            if (auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Parm)) {
              if (auto *DefArg = NTTP->getDefaultArgument()) {
                Expr::EvalResult Result;
                if (DefArg->EvaluateAsInt(Result, Context)) {
                  Ret.emplace_back(Context, Result.Val.getInt(),
                                   DefArg->getType());
                }
              }
            }
          }
        }
      }
      return Ret;
    }

    auto getInShaderPipelineArgs(
        ASTContext &Context,
        const ClassTemplateSpecializationDecl *PipelineSpec) const {
      assert(PipelineSpec->getSpecializedTemplateOrPartial()
                 .get<ClassTemplateDecl *>() == PipelineDecl);
      SmallVector<std::pair<StringRef, TemplateArgument>, 8> Ret;
      for (auto *RefCTD : InShaderAttributes) {
        bool Handled = false;
        for (const auto &Arg :
             PipelineSpec->getTemplateArgs()[0].getPackAsArray()) {
          if (Arg.getKind() == TemplateArgument::Type) {
            if (auto *CTD = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                    Arg.getAsType()->getAsCXXRecordDecl())) {
              if (CTD->getSpecializedTemplateOrPartial()
                      .get<ClassTemplateDecl *>() == RefCTD) {
                for (const auto &ArgIn : CTD->getTemplateArgs().asArray())
                  Ret.emplace_back(RefCTD->getName(), ArgIn);
                Handled = true;
                break;
              }
            }
          }
        }
        if (!Handled) {
          for (const auto *Parm : *RefCTD->getTemplateParameters()) {
            if (auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Parm)) {
              if (auto *DefArg = NTTP->getDefaultArgument()) {
                Expr::EvalResult Result;
                if (DefArg->EvaluateAsInt(Result, Context)) {
                  Ret.emplace_back(RefCTD->getName(),
                                   TemplateArgument(Context,
                                                    Result.Val.getInt(),
                                                    DefArg->getType()));
                }
              }
            }
          }
        }
      }
      return Ret;
    }

    bool
    isHighPriority(const ClassTemplateSpecializationDecl *PipelineSpec) const {
      for (const auto &Arg :
           PipelineSpec->getTemplateArgs()[0].getPackAsArray()) {
        if (Arg.getKind() == TemplateArgument::Type) {
          if (Arg.getAsType()->getAsCXXRecordDecl() == HighPriorityDecl) {
            return true;
          }
        }
      }
      return false;
    }

    bool
    isDirectRender(const ClassTemplateSpecializationDecl *PipelineSpec) const {
      for (const auto &Arg :
           PipelineSpec->getTemplateArgs()[0].getPackAsArray()) {
        if (Arg.getKind() == TemplateArgument::Type) {
          if (Arg.getAsType()->getAsCXXRecordDecl() == DirectRenderDecl) {
            return true;
          }
        }
      }
      return false;
    }
  };

private:
  NamespaceDecl *StdNamespace = nullptr;
  NamespaceDecl *HshNamespace = nullptr;
  NamespaceDecl *HshDetailNamespace = nullptr;
  NamespaceDecl *HshPipelineNamespace = nullptr;
  PipelineAttributes PipelineAttributes;
  CXXRecordDecl *BindingRecordType = nullptr;
  FunctionTemplateDecl *RebindTemplateFunc = nullptr;
  ClassTemplateDecl *UniformBufferType = nullptr;
  ClassTemplateDecl *VertexBufferType = nullptr;
  EnumDecl *EnumTarget = nullptr;
  EnumDecl *EnumStage = nullptr;
  EnumDecl *EnumInputRate = nullptr;
  EnumDecl *EnumFormat = nullptr;
  ClassTemplateDecl *ShaderConstDataTemplateType = nullptr;
  ClassTemplateDecl *ShaderDataTemplateType = nullptr;
  CXXRecordDecl *SamplerRecordType = nullptr;
  CXXRecordDecl *SamplerBindingType = nullptr;
  APSInt MaxUniforms;
  APSInt MaxImages;
  APSInt MaxSamplers;
  std::array<const TagDecl *, HBT_Max> Types{};
  std::array<const TagDecl *, HBT_Max> AlignedTypes{};
  std::array<const FunctionDecl *, HBF_Max> Functions{};
  std::array<const CXXMethodDecl *, HBM_Max> Methods{};
  std::array<std::pair<const FieldDecl *, HshStage>, HPF_Max> PipelineFields{};
  ClassTemplateDecl *StdArrayType = nullptr;
  ClassTemplateDecl *AlignedArrayType = nullptr;

  static constexpr Spellings BuiltinTypeSpellings[] = {
      {{}, {}, {}},
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal)                           \
  {#GLSL##_ll, #HLSL##_ll, #Metal##_ll},
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned)               \
  {#GLSL##_ll, #HLSL##_ll, #Metal##_ll},
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  {#GLSLf##_ll, #HLSLf##_ll, #Metalf##_ll},
#define BUILTIN_ENUM_TYPE(Name) {{}, {}, {}},
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeVector[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) true,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned) false,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  false,
#define BUILTIN_ENUM_TYPE(Name) false,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeMatrix[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned) true,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  false,
#define BUILTIN_ENUM_TYPE(Name) false,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeTexture[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned) false,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  true,
#define BUILTIN_ENUM_TYPE(Name) false,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeEnum[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned) false,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  false,
#define BUILTIN_ENUM_TYPE(Name) true,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinMethodSwizzle[] = {
      false,
#define BUILTIN_CXX_METHOD(Name, Spelling, IsSwizzle, Record, ...) IsSwizzle,
#include "BuiltinCXXMethods.def"
  };

  static constexpr Spellings BuiltinFunctionSpellings[] = {
      {{}, {}, {}},
#define BUILTIN_FUNCTION(Name, Spelling, GLSL, HLSL, Metal, InterpDist, ...)   \
  {#GLSL##_ll, #HLSL##_ll, #Metal##_ll},
#include "BuiltinFunctions.def"
  };

  static constexpr bool BuiltinFunctionInterpDists[] = {
      false,
#define BUILTIN_FUNCTION(Name, Spelling, GLSL, HLSL, Metal, InterpDist, ...)   \
  InterpDist,
#include "BuiltinFunctions.def"
  };

  template <typename T>
  static T *LookupDecl(ASTContext &Context, DeclContext *DC, StringRef Name) {
    auto LookupResult = DC->noload_lookup(&Context.Idents.get(Name));
    if (!LookupResult.empty()) {
      if (T *Found = dyn_cast<T>(LookupResult[0]))
        return Found->getCanonicalDecl();
    }
    return nullptr;
  }

  void addType(ASTContext &Context, DeclContext *DC, HshBuiltinType TypeKind,
               StringRef Name) {
    if (auto *FoundType = LookupDecl<TagDecl>(Context, DC, Name)) {
      Types[TypeKind] = FoundType;
    } else {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "type %0; is hsh.h included?"))
          << Name;
    }
  }

  void addAlignedType(ASTContext &Context, DeclContext *DC,
                      HshBuiltinType TypeKind, StringRef Name) {
    if (auto *FoundType = LookupDecl<TagDecl>(Context, DC, Name)) {
      AlignedTypes[TypeKind] = FoundType;
    } else {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "aligned type %0; is hsh.h included?"))
          << Name;
    }
  }

  void addEnumType(ASTContext &Context, DeclContext *DC,
                   HshBuiltinType TypeKind, StringRef Name) {
    if (auto *FoundEnum = LookupDecl<EnumDecl>(Context, DC, Name)) {
      Types[TypeKind] = FoundEnum;
    } else {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "enum %0; is hsh.h included?"))
          << Name;
    }
  }

  void addFunction(ASTContext &Context, DeclContext *DC,
                   HshBuiltinFunction FuncKind, StringRef Name, StringRef P) {
    SmallVector<StringRef, 8> Params;
    if (P != "void") {
      P.split(Params, ',');
      for (auto &ParamStr : Params)
        ParamStr = ParamStr.trim();
    }
    auto LookupResults = DC->noload_lookup(&Context.Idents.get(Name));
    for (auto *Res : LookupResults) {
      FunctionDecl *Function = dyn_cast<FunctionDecl>(Res);
      if (!Function) {
        if (auto *Template = dyn_cast<FunctionTemplateDecl>(Res))
          Function = dyn_cast<FunctionDecl>(Template->getTemplatedDecl());
      }
      if (Function && Function->getNumParams() == Params.size()) {
        auto *It = Params.begin();
        bool Match = true;
        for (auto *Param : Function->parameters()) {
          if (Param->getType().getAsString() != *It++) {
            Match = false;
            break;
          }
        }
        if (Match) {
          Functions[FuncKind] = Function->getCanonicalDecl();
          return;
        }
      }
    }
    DiagnosticsEngine &Diags = Context.getDiagnostics();
    Diags.Report(Diags.getCustomDiagID(
        DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                  "function %1(%2); is hsh.h included?"))
        << Name << P;
  }

  void addCXXMethod(ASTContext &Context, DeclContext *DC, StringRef R,
                    StringRef Name, StringRef P,
                    HshBuiltinCXXMethod MethodKind) {
    if (auto *FoundRecord = LookupDecl<CXXRecordDecl>(Context, DC, R)) {
      SmallVector<StringRef, 8> Params;
      if (P != "void") {
        P.split(Params, ',');
        for (auto &ParamStr : Params)
          ParamStr = ParamStr.trim();
      }
      auto LookupResults =
          FoundRecord->noload_lookup(&Context.Idents.get(Name));
      for (auto *Res : LookupResults) {
        CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(Res);
        if (!Method) {
          if (auto *Template = dyn_cast<FunctionTemplateDecl>(Res))
            Method = dyn_cast<CXXMethodDecl>(Template->getTemplatedDecl());
        }
        if (Method && Method->getNumParams() == Params.size()) {
          auto *It = Params.begin();
          bool Match = true;
          for (auto *Param : Method->parameters()) {
            if (Param->getType().getAsString() != *It++) {
              Match = false;
              break;
            }
          }
          if (Match) {
            Methods[MethodKind] =
                dyn_cast<CXXMethodDecl>(Method->getCanonicalDecl());
            return;
          }
        }
      }
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "method %0::%1(%2); is hsh.h included?"))
          << R << Name << P;
    } else {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "record %0; is hsh.h included?"))
          << R;
    }
  }

  NamespaceDecl *findNamespace(StringRef Name, DeclContext *DC,
                               ASTContext &Context) const {
    auto *FoundNS = LookupDecl<NamespaceDecl>(Context, DC, Name);
    if (!FoundNS) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "unable to locate declaration of namespace %0; "
                                "is hsh.h included?"))
          << Name;
    }
    return FoundNS;
  }

  EnumDecl *findEnum(StringRef Name, DeclContext *DC,
                     ASTContext &Context) const {
    auto *FoundEnum = LookupDecl<EnumDecl>(Context, DC, Name);
    if (!FoundEnum) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of enum %0; "
                                    "is hsh.h included?"))
          << Name;
    }
    return FoundEnum;
  }

  CXXRecordDecl *findCXXRecord(StringRef Name, DeclContext *DC,
                               ASTContext &Context) const {
    auto *FoundRecord = LookupDecl<CXXRecordDecl>(Context, DC, Name);
    if (!FoundRecord) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "unable to locate declaration of record %0; "
                                "is hsh.h included?"))
          << Name;
    }
    return FoundRecord;
  }

  ClassTemplateDecl *findClassTemplate(StringRef Name, DeclContext *DC,
                                       ASTContext &Context) const {
    auto *FoundTemplate = LookupDecl<ClassTemplateDecl>(Context, DC, Name);
    if (!FoundTemplate) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unable to locate declaration of class template %0; "
          "is hsh.h included?"))
          << Name;
    }
    return FoundTemplate;
  }

  FunctionTemplateDecl *findMethodTemplate(CXXRecordDecl *Class, StringRef Name,
                                           ASTContext &Context) const {
    auto ClassName = Class->getName();
    Class = Class->getDefinition();
    DiagnosticsEngine &Diags = Context.getDiagnostics();
    if (!Class) {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "definition of %0 is not available; is hsh.h included?"))
          << ClassName;
      return nullptr;
    }
    using FuncTemplIt =
        CXXRecordDecl::specific_decl_iterator<FunctionTemplateDecl>;
    FunctionTemplateDecl *Ret = nullptr;
    for (FuncTemplIt TI(Class->decls_begin()), TE(Class->decls_end()); TI != TE;
         ++TI) {
      if (TI->getName() == Name)
        Ret = *TI;
    }
    if (Ret)
      return Ret;
    Diags.Report(Diags.getCustomDiagID(
        DiagnosticsEngine::Error, "unable to locate declaration of "
                                  "method template %0::%1; is hsh.h included?"))
        << Class->getName() << Name;
    return nullptr;
  }

  FunctionTemplateDecl *findMethodTemplate(ClassTemplateDecl *Class,
                                           StringRef Name,
                                           ASTContext &Context) const {
    return findMethodTemplate(Class->getTemplatedDecl(), Name, Context);
  }

  VarDecl *findVar(StringRef Name, DeclContext *DC, ASTContext &Context) const {
    auto *FoundVar = LookupDecl<VarDecl>(Context, DC, Name);
    if (!FoundVar) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      Diags.Report(
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "unable to locate declaration of variable %0; "
                                "is hsh.h included?"))
          << Name;
    }
    return FoundVar;
  }

  APSInt findICEVar(StringRef Name, DeclContext *DC,
                    ASTContext &Context) const {
    if (auto *VD = findVar(Name, DC, Context)) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      if (auto *Val = VD->evaluateValue()) {
        if (Val->isInt())
          return Val->getInt();
        Diags.Report(Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "variable %0 is not integer constexpr"))
            << Name;
        return APSInt{};
      }
      Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                         "variable %0 is not constexpr"))
          << Name;
    }
    return APSInt{};
  }

public:
  void findBuiltinDecls(ASTContext &Context) {
    StdNamespace =
        findNamespace("std"_ll, Context.getTranslationUnitDecl(), Context);
    if (!StdNamespace)
      return;
    HshNamespace =
        findNamespace("hsh"_ll, Context.getTranslationUnitDecl(), Context);
    if (!HshNamespace)
      return;
    HshDetailNamespace = findNamespace("detail"_ll, HshNamespace, Context);
    if (!HshDetailNamespace)
      return;
    HshPipelineNamespace = findNamespace("pipeline"_ll, HshNamespace, Context);
    if (!HshPipelineNamespace)
      return;

    PipelineAttributes.findDecls(Context, HshPipelineNamespace);
    if (auto *PipelineRecordDecl = PipelineAttributes.getPipelineDecl()) {
      auto *Record = PipelineRecordDecl->getTemplatedDecl();
      for (auto *FD : Record->fields()) {
#define PIPELINE_FIELD(Name, Stage)                                            \
  if (FD->getName() == #Name##_ll)                                             \
    PipelineFields[HPF_##Name] = {FD, Stage};
#include "ShaderInterface.def"
      }
      auto ReportMissingPipelineField = [&](StringRef Name) {
        DiagnosticsEngine &Diags = Context.getDiagnostics();
        Diags.Report(Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "unable to locate pipeline field %0; "
                                      "is hsh::pipeline::pipeline invalid?"))
            << Name;
      };
      auto CheckIt = PipelineFields.begin();
#define PIPELINE_FIELD(Name, Stage)                                            \
  if (!(++CheckIt)->first)                                                     \
    ReportMissingPipelineField(#Name##_ll);
#include "ShaderInterface.def"
    }
    BindingRecordType = findCXXRecord("binding"_ll, HshNamespace, Context);
    RebindTemplateFunc =
        findMethodTemplate(BindingRecordType, "_rebind"_ll, Context);

    UniformBufferType =
        findClassTemplate("uniform_buffer"_ll, HshNamespace, Context);
    VertexBufferType =
        findClassTemplate("vertex_buffer"_ll, HshNamespace, Context);

    EnumTarget = findEnum("Target"_ll, HshNamespace, Context);
    EnumStage = findEnum("Stage"_ll, HshNamespace, Context);
    EnumInputRate = findEnum("InputRate"_ll, HshDetailNamespace, Context);
    EnumFormat = findEnum("Format"_ll, HshNamespace, Context);
    ShaderConstDataTemplateType =
        findClassTemplate("ShaderConstData"_ll, HshDetailNamespace, Context);
    ShaderDataTemplateType =
        findClassTemplate("ShaderData"_ll, HshDetailNamespace, Context);
    SamplerRecordType = findCXXRecord("sampler"_ll, HshNamespace, Context);
    SamplerBindingType =
        findCXXRecord("SamplerBinding"_ll, HshDetailNamespace, Context);

    MaxUniforms = findICEVar("MaxUniforms"_ll, HshDetailNamespace, Context);
    MaxImages = findICEVar("MaxImages"_ll, HshDetailNamespace, Context);
    MaxSamplers = findICEVar("MaxSamplers"_ll, HshDetailNamespace, Context);

#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal)                           \
  addType(Context, HshNamespace, HBT_##Name, #Name##_ll);
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal, HasAligned)               \
  addType(Context, HshNamespace, HBT_##Name, #Name##_ll);                      \
  if (HasAligned)                                                              \
    addAlignedType(Context, HshNamespace, HBT_##Name, "aligned_" #Name##_ll);
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  addType(Context, HshNamespace, HBT_##Name, #Name##_ll);
#define BUILTIN_ENUM_TYPE(Name)                                                \
  addEnumType(Context, HshNamespace, HBT_##Name, #Name##_ll);
#include "BuiltinTypes.def"
#define BUILTIN_FUNCTION(Name, Spelling, GLSL, HLSL, Metal, InterpDist, ...)   \
  addFunction(Context, HshNamespace, HBF_##Name, #Spelling##_ll,               \
              #__VA_ARGS__##_ll);
#include "BuiltinFunctions.def"
#define BUILTIN_CXX_METHOD(Name, Spelling, IsSwizzle, Record, ...)             \
  addCXXMethod(Context, HshNamespace, #Record##_ll, #Spelling##_ll,            \
               #__VA_ARGS__##_ll, HBM_##Name);
#include "BuiltinCXXMethods.def"

    StdArrayType = findClassTemplate("array"_ll, StdNamespace, Context);
    AlignedArrayType =
        findClassTemplate("aligned_array"_ll, HshNamespace, Context);
  }

  template <typename T> HshBuiltinType identifyBuiltinType(T Arg) const {
    bool IsAligned;
    return identifyBuiltinType(Arg, IsAligned);
  }

  HshBuiltinType identifyBuiltinType(QualType QT, bool &IsAligned) const {
    return identifyBuiltinType(QT.getNonReferenceType().getTypePtrOrNull(),
                               IsAligned);
  }

  HshBuiltinType identifyBuiltinType(const clang::Type *UT,
                                     bool &IsAligned) const {
    IsAligned = false;
    if (!UT)
      return HBT_None;
    TagDecl *T = UT->getAsTagDecl();
    if (!T)
      return HBT_None;
    T = T->getCanonicalDecl();
    if (!T)
      return HBT_None;
    HshBuiltinType Ret = HBT_None;
    for (const auto *Tp : Types) {
      if (T == Tp)
        return Ret;
      Ret = HshBuiltinType(int(Ret) + 1);
    }
    Ret = HBT_None;
    for (const auto *Tp : AlignedTypes) {
      if (T == Tp) {
        IsAligned = true;
        return Ret;
      }
      Ret = HshBuiltinType(int(Ret) + 1);
    }
    return HBT_None;
  }

  HshBuiltinFunction identifyBuiltinFunction(const FunctionDecl *F) const {
    F = F->getCanonicalDecl();
    if (!F)
      return HBF_None;
    HshBuiltinFunction Ret = HBF_None;
    for (const auto *Func : Functions) {
      if (F == Func)
        return Ret;
      Ret = HshBuiltinFunction(int(Ret) + 1);
    }
    return HBF_None;
  }

  HshBuiltinCXXMethod identifyBuiltinMethod(const CXXMethodDecl *M) const {
    M = dyn_cast_or_null<CXXMethodDecl>(M->getCanonicalDecl());
    if (!M)
      return HBM_None;
    if (FunctionDecl *FD = M->getInstantiatedFromMemberFunction())
      M = dyn_cast<CXXMethodDecl>(FD->getCanonicalDecl());
    if (auto *TD = M->getPrimaryTemplate())
      if (auto TDM = dyn_cast<CXXMethodDecl>(TD->getTemplatedDecl()))
        M = TDM;
    HshBuiltinCXXMethod Ret = HBM_None;
    for (const auto *Method : Methods) {
      if (M == Method)
        return Ret;
      Ret = HshBuiltinCXXMethod(int(Ret) + 1);
    }
    return HBM_None;
  }

  HshBuiltinPipelineField
  identifyBuiltinPipelineField(const FieldDecl *FD) const {
    if (auto *Record =
            dyn_cast<ClassTemplateSpecializationDecl>(FD->getDeclContext())) {
      if (auto *CTD = Record->getSpecializedTemplateOrPartial()
                          .get<ClassTemplateDecl *>()) {
        auto FieldIt = CTD->getTemplatedDecl()->field_begin();
        std::advance(FieldIt, FD->getFieldIndex());
        FD = *FieldIt;
      }
    }

    HshBuiltinPipelineField Ret = HPF_None;
    for (const auto &Field : PipelineFields) {
      if (FD == Field.first)
        return Ret;
      Ret = HshBuiltinPipelineField(int(Ret) + 1);
    }
    return HPF_None;
  }

  HshStage stageOfBuiltinPipelineField(HshBuiltinPipelineField PF) const {
    return PipelineFields[PF].second;
  }

  static const CXXRecordDecl *
  FirstTemplateParamType(ClassTemplateSpecializationDecl *Derived,
                         ClassTemplateDecl *Decl) {
    if (Derived->getSpecializedTemplateOrPartial()
            .get<ClassTemplateDecl *>()
            ->getCanonicalDecl() == Decl) {
      const auto &Arg = Derived->getTemplateArgs()[0];
      if (Arg.getKind() == TemplateArgument::Type)
        return Arg.getAsType()->getAsCXXRecordDecl();
    }
    return nullptr;
  }

  const CXXRecordDecl *getUniformRecord(const ParmVarDecl *PVD) const {
    auto *Derived = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
        PVD->getType()->getAsCXXRecordDecl());
    if (!Derived)
      return nullptr;
    if (auto *Ret = FirstTemplateParamType(Derived, UniformBufferType))
      return Ret;
    return nullptr;
  }

  const CXXRecordDecl *getVertexAttributeRecord(const ParmVarDecl *PVD) const {
    auto *Derived = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
        PVD->getType()->getAsCXXRecordDecl());
    if (!Derived)
      return nullptr;
    if (auto *Ret = FirstTemplateParamType(Derived, VertexBufferType))
      return Ret;
    return nullptr;
  }

  bool checkHshTypeCompatibility(const ASTContext &Context, const ValueDecl *VD,
                                 QualType Tp, bool AllowTextures) const {
    if (auto *Spec = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
            Tp->getAsCXXRecordDecl())) {
      auto *CTD =
          Spec->getSpecializedTemplateOrPartial().get<ClassTemplateDecl *>();
      if (CTD == StdArrayType || CTD == AlignedArrayType) {
        auto &Arg = Spec->getTemplateArgs()[0];
        return checkHshTypeCompatibility(Context, VD, Arg.getAsType(),
                                         AllowTextures);
      }
    }
    HshBuiltinType HBT = identifyBuiltinType(Tp);
    if (HBT != HBT_None &&
        (AllowTextures || !HshBuiltins::isTextureType(HBT))) {
      return true;
    } else if (Tp->isIntegralOrEnumerationType()) {
      if (Context.getIntWidth(Tp) != 32) {
        ReportBadIntegerType(VD, Context);
        return false;
      }
      return true;
    } else if (Tp->isSpecificBuiltinType(BuiltinType::Float) ||
               Tp->isSpecificBuiltinType(BuiltinType::Double)) {
      return true;
    }
    ReportBadRecordType(VD, Context);
    return false;
  }

  bool checkHshTypeCompatibility(const ASTContext &Context, const ValueDecl *VD,
                                 bool AllowTextures) const {
    QualType Tp = VD->getType();
    if (auto *VarD = dyn_cast<VarDecl>(VD))
      Tp = ResolveParmType(VarD);
    else if (auto *FuncD = dyn_cast<FunctionDecl>(VD))
      Tp = FuncD->getReturnType();
    return checkHshTypeCompatibility(Context, VD, Tp, AllowTextures);
  }

  bool checkHshFieldTypeCompatibility(const ASTContext &Context,
                                      const ValueDecl *VD) const {
    return checkHshTypeCompatibility(Context, VD, false);
  }

  bool checkHshParamTypeCompatibility(const ASTContext &Context,
                                      const ValueDecl *VD) const {
    return checkHshTypeCompatibility(Context, VD, true);
  }

  bool checkHshRecordCompatibility(const ASTContext &Context,
                                   const CXXRecordDecl *Record) const {
    bool Ret = true;
    for (const auto *FD : Record->fields())
      if (!checkHshFieldTypeCompatibility(Context, FD))
        Ret = false;
    return Ret;
  }

  bool checkHshFunctionCompatibility(const ASTContext &Context,
                                     const FunctionDecl *FD) const {
    bool Ret = true;
    if (FD->getReturnType() != Context.VoidTy)
      Ret = checkHshParamTypeCompatibility(Context, FD);
    for (const auto *Param : FD->parameters())
      if (!checkHshParamTypeCompatibility(Context, Param))
        Ret = false;
    return Ret;
  }

  HshStage determineVarStage(const VarDecl *VD) const {
    if (auto *PVD = dyn_cast<ParmVarDecl>(VD)) {
      if (getVertexAttributeRecord(PVD))
        return HshVertexStage;
      else if (auto *SA = PVD->getAttr<HshStageAttr>())
        return HshStage(SA->getStageIndex());
      else if (isTextureType(identifyBuiltinType(PVD->getType())))
        return HshFragmentStage;
    } else if (auto *SA = VD->getAttr<HshStageAttr>()) {
      return HshStage(SA->getStageIndex());
    }
    return HshNoStage;
  }

  HshStage determinePipelineFieldStage(const FieldDecl *FD) const {
    auto HPF = identifyBuiltinPipelineField(FD);
    if (HPF == HPF_None)
      return HshNoStage;
    return stageOfBuiltinPipelineField(HPF);
  }

  static constexpr const Spellings &getSpellings(HshBuiltinType Tp) {
    return BuiltinTypeSpellings[Tp];
  }

  template <HshTarget T>
  static constexpr StringRef getSpelling(HshBuiltinType Tp);

  static constexpr const Spellings &getSpellings(HshBuiltinFunction Func) {
    return BuiltinFunctionSpellings[Func];
  }

  template <HshTarget T>
  static constexpr StringRef getSpelling(HshBuiltinFunction Func);

  static constexpr bool isVectorType(HshBuiltinType Tp) {
    return BuiltinTypeVector[Tp];
  }

  static constexpr unsigned getVectorSize(HshBuiltinType Tp) {
    switch (Tp) {
    case HBT_float2:
    case HBT_int2:
    case HBT_uint2:
      return 2;
    case HBT_float3:
    case HBT_int3:
    case HBT_uint3:
      return 3;
    case HBT_float4:
    case HBT_int4:
    case HBT_uint4:
      return 4;
    default:
      return 0;
    }
  }

  static constexpr bool isMatrixType(HshBuiltinType Tp) {
    return BuiltinTypeMatrix[Tp];
  }

  static constexpr unsigned getMatrixColumnCount(HshBuiltinType Tp) {
    switch (Tp) {
    case HBT_float3x3:
      return 3;
    case HBT_float4x4:
      return 4;
    default:
      return 0;
    }
  }

  static constexpr HshBuiltinType getMatrixColumnType(HshBuiltinType Tp) {
    switch (Tp) {
    case HBT_float3x3:
      return HBT_float3;
    case HBT_float4x4:
      return HBT_float4;
    default:
      return HBT_None;
    }
  }

  static constexpr bool isTextureType(HshBuiltinType Tp) {
    return BuiltinTypeTexture[Tp];
  }

  static constexpr bool isEnumType(HshBuiltinType Tp) {
    return BuiltinTypeEnum[Tp];
  }

  static constexpr bool isSwizzleMethod(HshBuiltinCXXMethod M) {
    return BuiltinMethodSwizzle[M];
  }

  static constexpr bool isInterpolationDistributed(HshBuiltinFunction Func) {
    return BuiltinFunctionInterpDists[Func];
  }

  const clang::TagDecl *getTypeDecl(HshBuiltinType Tp) const {
    return Types[Tp];
  }

  QualType getType(HshBuiltinType Tp) const {
    return getTypeDecl(Tp)->getTypeForDecl()->getCanonicalTypeUnqualified();
  }

  const TagDecl *getAlignedAvailable(FieldDecl *FD) const {
    bool IsAligned;
    auto HBT = identifyBuiltinType(FD->getType(), IsAligned);
    return (AlignedTypes[HBT] != nullptr && !IsAligned) ? AlignedTypes[HBT]
                                                        : nullptr;
  }

  static TypeSourceInfo *getFullyQualifiedTemplateSpecializationTypeInfo(
      ASTContext &Context, TemplateDecl *TDecl,
      const TemplateArgumentListInfo &Args) {
    QualType Underlying =
        Context.getTemplateSpecializationType(TemplateName(TDecl), Args);
    Underlying = TypeName::getFullyQualifiedType(Underlying, Context);
    return Context.getTrivialTypeSourceInfo(Underlying);
  }

  QualType getBindingRefType(ASTContext &Context) const {
    return Context.getLValueReferenceType(
        QualType{BindingRecordType->getTypeForDecl(), 0});
  }

  CXXMemberCallExpr *makeSpecializedRebindCall(
      ASTContext &Context, QualType SpecType, ParmVarDecl *BindingParm,
      ArrayRef<QualType> CallArgs, ArrayRef<Expr *> CallExprs) const {
    auto *Args =
        TemplateArgumentList::CreateCopy(Context, TemplateArgument(SpecType));
    void *InsertPos;
    CXXMethodDecl *Method = dyn_cast_or_null<CXXMethodDecl>(
        RebindTemplateFunc->findSpecialization(Args->asArray(), InsertPos));
    if (!Method) {
      Method = CXXMethodDecl::Create(
          Context, BindingRecordType, {},
          {RebindTemplateFunc->getDeclName(), {}},
          Context.getFunctionType(
              Context.VoidTy, CallArgs,
              FunctionProtoType::ExtProtoInfo().withExceptionSpec(
                  EST_BasicNoexcept)),
          nullptr, SC_None, false, CSK_unspecified, {});
      Method->setAccess(AS_public);
      Method->setFunctionTemplateSpecialization(RebindTemplateFunc, Args,
                                                InsertPos);
    }
    auto AValidLocation = Context.getSourceManager().getLocForStartOfFile(
        Context.getSourceManager().getMainFileID());
    TemplateArgumentListInfo TemplateArgs(AValidLocation, AValidLocation);
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument(SpecType),
                            Context.getTrivialTypeSourceInfo(SpecType)));
    return CXXMemberCallExpr::Create(
        Context,
        MemberExpr::Create(
            Context,
            DeclRefExpr::Create(
                Context, {}, {}, BindingParm, false, SourceLocation{},
                BindingParm->getType().getNonReferenceType(), VK_XValue),
            false, SourceLocation{}, {}, SourceLocation{}, Method,
            DeclAccessPair::make(Method, AS_public), {}, &TemplateArgs,
            Method->getType(), VK_XValue, OK_Ordinary, NOUR_None),
        CallExprs, Context.VoidTy, VK_XValue, {});
  }

  const class PipelineAttributes &getPipelineAttributes() const {
    return PipelineAttributes;
  }

  const ClassTemplateDecl *getPipelineRecordDecl() const {
    return PipelineAttributes.getPipelineDecl();
  }

  bool isDerivedFromPipelineDecl(CXXRecordDecl *Decl) const {
    CXXBasePaths Paths(/*FindAmbiguities=*/false, /*RecordPaths=*/false,
                       /*DetectVirtual=*/false);
    return Decl->lookupInBases(
        [this](const CXXBaseSpecifier *Specifier, CXXBasePath &Path) {
          if (const auto *TST =
                  Specifier->getType()->getAs<TemplateSpecializationType>()) {
            if (auto *TD = dyn_cast_or_null<ClassTemplateDecl>(
                    TST->getTemplateName().getAsTemplateDecl()))
              return TD == getPipelineRecordDecl();
          }
          return false;
        },
        Paths, /*LookupInDependent=*/true);
  }

  ClassTemplateSpecializationDecl *
  getDerivedPipelineSpecialization(CXXRecordDecl *Decl) const {
    CXXBasePaths Paths(/*FindAmbiguities=*/false, /*RecordPaths=*/false,
                       /*DetectVirtual=*/false);
    ClassTemplateSpecializationDecl *Ret = nullptr;
    Decl->lookupInBases(
        [this, &Ret](const CXXBaseSpecifier *Specifier, CXXBasePath &Path) {
          if (const auto *TST =
                  Specifier->getType()->getAs<TemplateSpecializationType>()) {
            if (auto *TD = dyn_cast_or_null<ClassTemplateDecl>(
                    TST->getTemplateName().getAsTemplateDecl())) {
              if (TD == getPipelineRecordDecl()) {
                Ret = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                    Specifier->getType()->getAsCXXRecordDecl());
                return true;
              }
            }
          }
          return false;
        },
        Paths, /*LookupInDependent=*/false);
    return Ret;
  }

  const CXXRecordDecl *getSamplerRecordDecl() const {
    return SamplerRecordType;
  }

  ClassTemplateDecl *getStdArrayDecl() const { return StdArrayType; }

  bool isStdArrayType(const CXXRecordDecl *RD) const {
    if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
      return CTSD->getSpecializedTemplateOrPartial()
                 .get<ClassTemplateDecl *>() == StdArrayType;
    return false;
  }

  ClassTemplateDecl *getAlignedArrayDecl() const { return AlignedArrayType; }

  bool isAlignedArrayType(const CXXRecordDecl *RD) const {
    if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
      return CTSD->getSpecializedTemplateOrPartial()
                 .get<ClassTemplateDecl *>() == AlignedArrayType;
    return false;
  }

  bool isSupportedArrayType(const CXXRecordDecl *RD) const {
    return isStdArrayType(RD) || isAlignedArrayType(RD);
  }

  CXXFunctionalCastExpr *makeSamplerBinding(ASTContext &Context,
                                            ParmVarDecl *Tex,
                                            unsigned SamplerIdx,
                                            unsigned TextureIdx) const {
    std::array<Expr *, 3> Args{
        DeclRefExpr::Create(Context, {}, {}, Tex, false, SourceLocation{},
                            Tex->getType(), VK_XValue),
        IntegerLiteral::Create(Context, APInt(32, SamplerIdx), Context.IntTy,
                               {}),
        IntegerLiteral::Create(Context, APInt(32, TextureIdx), Context.IntTy,
                               {})};
    auto *Init = new (Context) InitListExpr(Context, {}, Args, {});
    return CXXFunctionalCastExpr::Create(
        Context, QualType{SamplerBindingType->getTypeForDecl(), 0}, VK_XValue,
        nullptr, CastKind::CK_NoOp, Init, nullptr, {}, {});
  }

  CXXRecordDecl *makeBindingDerivative(ASTContext &Context,
                                       CXXRecordDecl *Source,
                                       StringRef Name) const {
    auto *Record = CXXRecordDecl::Create(Context, TTK_Class,
                                         Context.getTranslationUnitDecl(), {},
                                         {}, &Context.Idents.get(Name));
    Record->HshSourceRecord = Source;
    Record->startDefinition();

    return Record;
  }

  ClassTemplateDecl *
  makeBindingDerivative(ASTContext &Context, Sema &Actions,
                        ClassTemplateDecl *SpecializationSource,
                        StringRef Name) const {
    auto *Record = CXXRecordDecl::Create(Context, TTK_Class,
                                         Context.getTranslationUnitDecl(), {},
                                         {}, &Context.Idents.get(Name));
    Record->startDefinition();
    Record->completeDefinition();

    auto *CTD = ClassTemplateDecl::Create(
        Context, Context.getTranslationUnitDecl(), {}, Record->getIdentifier(),
        SpecializationSource->getTemplateParameters(), Record);

    for (auto *Specialization : SpecializationSource->specializations()) {
      /*
       * Hsh violates C++ within binding macros by permitting non-constexpr
       * template parameters. These non-constexpr specializations shall not be
       * translated.
       */
      if (!CheckConstexprTemplateSpecialization(Context, Specialization))
        continue;

      /*
       * Hsh supports forward-declared specializations for profiling macros.
       * Attempt to instantiate if necessary.
       */
      if (!Specialization->hasDefinition()) {
        if (Actions.InstantiateClassTemplateSpecialization(
                Specialization->getBeginLoc(), Specialization,
                TSK_ExplicitInstantiationDefinition))
          continue;
        Actions.InstantiateClassTemplateSpecializationMembers(
            Specialization->getBeginLoc(), Specialization,
            TSK_ExplicitInstantiationDefinition);
      }
      /*
       * Every specialization must inherit a hsh::pipeline specialization to be
       * eligible for translation.
       */
      if (!isDerivedFromPipelineDecl(Specialization))
        continue;

      auto Args = Specialization->getTemplateArgs().asArray();
      void *InsertPos;
      CTD->findSpecialization(Args, InsertPos);
      auto *Spec = ClassTemplateSpecializationDecl::Create(
          Context, Record->getTagKind(), Record->getDeclContext(), {}, {}, CTD,
          Args, nullptr);
      Spec->HshSourceRecord = Specialization;
      Spec->startDefinition();
      CTD->AddSpecialization(Spec, InsertPos);
    }

    return CTD;
  }

  static void printEnumeratorString(raw_ostream &Out,
                                    const PrintingPolicy &Policy,
                                    const EnumDecl *ED, const APSInt &Val) {
    for (const EnumConstantDecl *ECD : ED->enumerators()) {
      if (llvm::APSInt::isSameValue(ECD->getInitVal(), Val)) {
        ECD->printQualifiedName(Out, Policy);
        return;
      }
    }
  }

  static const EnumConstantDecl *lookupEnumConstantDecl(const EnumDecl *ED,
                                                        const APSInt &Val) {
    for (const EnumConstantDecl *ECD : ED->enumerators())
      if (llvm::APSInt::isSameValue(ECD->getInitVal(), Val))
        return ECD;
    return nullptr;
  }

  VarDecl *getConstDataVar(ASTContext &Context, DeclContext *DC,
                           HshTarget Target, uint32_t NumStages,
                           uint32_t NumBindings, uint32_t NumAttributes,
                           uint32_t NumSamplers,
                           uint32_t NumColorAttachments) const {
    const auto *ECD = lookupEnumConstantDecl(EnumTarget, APSInt::get(Target));
    assert(ECD);

    TemplateArgumentListInfo TemplateArgs;
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{Context, APSInt::get(Target),
                         QualType{EnumTarget->getTypeForDecl(), 0}},
        (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumStages),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumBindings),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{Context, APSInt::get(NumAttributes),
                         Context.UnsignedIntTy},
        (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumSamplers),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{Context, APSInt::get(NumColorAttachments),
                         Context.UnsignedIntTy},
        (Expr *)nullptr));
    TypeSourceInfo *TSI = getFullyQualifiedTemplateSpecializationTypeInfo(
        Context, ShaderConstDataTemplateType, TemplateArgs);

    auto *VD = VarDecl::Create(
        Context, DC, {}, {},
        &Context.Idents.get(Twine("cdata_", ECD->getName()).str()),
        TSI->getType(), nullptr, SC_Static);
    VD->setConstexpr(true);
    VD->setInitStyle(VarDecl::ListInit);
    VD->setInit(new (Context) InitListExpr(Stmt::EmptyShell{}));
    VD->InitHshTarget = Target;
    return VD;
  }

  VarDecl *getDataVar(ASTContext &Context, DeclContext *DC, HshTarget Target,
                      uint32_t NumStages, uint32_t NumSamplers) const {
    const auto *ECD = lookupEnumConstantDecl(EnumTarget, APSInt::get(Target));
    assert(ECD);

    TemplateArgumentListInfo TemplateArgs;
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{Context, APSInt::get(Target),
                         QualType{EnumTarget->getTypeForDecl(), 0}},
        (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumStages),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumSamplers),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TypeSourceInfo *TSI = getFullyQualifiedTemplateSpecializationTypeInfo(
        Context, ShaderDataTemplateType, TemplateArgs);

    return VarDecl::Create(
        Context, DC, {}, {},
        &Context.Idents.get(Twine("data_", ECD->getName()).str()),
        TSI->getType(), nullptr, SC_Static);
  }

  void printBuiltinEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                              HshBuiltinType HBT, const APSInt &Val) const {
    printEnumeratorString(Out, Policy, cast<EnumDecl>(getTypeDecl(HBT)), Val);
  }

  void printBuiltinEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                              HshBuiltinType HBT, int64_t Val) const {
    printBuiltinEnumString(Out, Policy, HBT, APSInt::get(Val));
  }

  void printTargetEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                             HshTarget Target) const {
    printEnumeratorString(Out, Policy, EnumTarget, APSInt::get(Target));
  }

  void printTargetEnumName(raw_ostream &Out, HshTarget Target) const {
    if (const auto *ECD =
            lookupEnumConstantDecl(EnumTarget, APSInt::get(Target)))
      ECD->printName(Out);
  }

  void printStageEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                            HshStage Stage) const {
    printEnumeratorString(Out, Policy, EnumStage, APSInt::get(Stage));
  }

  void printInputRateEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                                HshAttributeKind InputRate) const {
    printEnumeratorString(Out, Policy, EnumInputRate, APSInt::get(InputRate));
  }

  void printFormatEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                             HshFormat Format) const {
    printEnumeratorString(Out, Policy, EnumFormat, APSInt::get(Format));
  }

  void printColorComponentFlagExpr(raw_ostream &Out,
                                   const PrintingPolicy &Policy,
                                   ColorComponentFlags Flags) {
    bool NeedsPipe = false;
    for (int i = 0; i < 4; ++i) {
      if (Flags & (1 << i)) {
        if (NeedsPipe)
          Out << " | ";
        else
          NeedsPipe = true;
        printBuiltinEnumString(Out, Policy, HBT_ColorComponentFlags, 1 << i);
      }
    }
  }

  HshFormat formatOfType(QualType Tp) const {
    if (Tp->isSpecificBuiltinType(BuiltinType::Float)) {
      return R32_SFLOAT;
    } else if (Tp->isSpecificBuiltinType(BuiltinType::Int)) {
      return R32_SINT;
    } else if (Tp->isSpecificBuiltinType(BuiltinType::UInt)) {
      return R32_UINT;
    }
    auto HBT = identifyBuiltinType(Tp);
    switch (HBT) {
    case HBT_float2:
      return RG32_SFLOAT;
    case HBT_float3:
    case HBT_float3x3:
      return RGB32_SFLOAT;
    case HBT_float4:
    case HBT_float4x4:
      return RGBA32_SFLOAT;
    case HBT_int2:
      return RG32_SINT;
    case HBT_int3:
      return RGB32_SINT;
    case HBT_int4:
      return RGBA32_SINT;
    case HBT_uint2:
      return RG32_UINT;
    case HBT_uint3:
      return RGB32_UINT;
    case HBT_uint4:
      return RGBA32_UINT;
    default:
      break;
    }
    llvm_unreachable("Invalid type passed to formatOfType");
  }

  unsigned getMaxUniforms() const { return MaxUniforms.getZExtValue(); }
  unsigned getMaxImages() const { return MaxImages.getZExtValue(); }
  unsigned getMaxSamplers() const { return MaxSamplers.getZExtValue(); }
};

template <>
constexpr StringRef HshBuiltins::getSpelling<HT_GLSL>(HshBuiltinType Tp) {
  return getSpellings(Tp).GLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_HLSL>(HshBuiltinType Tp) {
  return getSpellings(Tp).HLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_METAL>(HshBuiltinType Tp) {
  return getSpellings(Tp).Metal;
}

template <>
constexpr StringRef HshBuiltins::getSpelling<HT_GLSL>(HshBuiltinFunction Func) {
  return getSpellings(Func).GLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_HLSL>(HshBuiltinFunction Func) {
  return getSpellings(Func).HLSL;
}
template <>
constexpr StringRef
HshBuiltins::getSpelling<HT_METAL>(HshBuiltinFunction Func) {
  return getSpellings(Func).Metal;
}

struct SamplerRecord {
  APValue Config;
};

struct SamplerBinding {
  unsigned RecordIdx;
  unsigned TextureIdx;
  ParmVarDecl *TextureDecl;
  unsigned UseStages;
};

struct UniformRecord {
  StringRef Name;
  const CXXRecordDecl *Record;
  unsigned UseStages;
};

struct AttributeRecord {
  StringRef Name;
  const CXXRecordDecl *Record;
  HshAttributeKind Kind;
};

struct FunctionRecord {
  const IdentifierInfo *Identifier;
  const FunctionDecl *Function;
  unsigned UseStages;
};

enum HshTextureKind {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  HTK_##Name,
#include "BuiltinTypes.def"
};

constexpr HshTextureKind KindOfTextureType(HshBuiltinType Type) {
  switch (Type) {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  case HBT_##Name:                                                             \
    return HTK_##Name;
#include "BuiltinTypes.def"
  default:
    llvm_unreachable("invalid texture kind");
  }
}

constexpr HshBuiltinType BuiltinTypeOfTexture(HshTextureKind Kind) {
  switch (Kind) {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  case HTK_##Name:                                                             \
    return HBT_##Name;
#include "BuiltinTypes.def"
  }
}

struct TextureRecord {
  const ParmVarDecl *TexParm;
  HshTextureKind Kind;
  unsigned UseStages;
};

struct VertexBinding {
  uint32_t Binding;
  uint32_t Stride;
  HshAttributeKind InputRate;
};

struct VertexAttribute {
  uint32_t Offset;
  uint32_t Binding;
  HshFormat Format;
};

struct SampleCall {
  const CXXMemberCallExpr *Expr;
  const ParmVarDecl *Decl;
  unsigned SamplerIndex;
};

struct HostPrintingPolicy final : PrintingCallbacks, PrintingPolicy {
  explicit HostPrintingPolicy(const PrintingPolicy &Policy)
      : PrintingPolicy(Policy) {
    Callbacks = this;
    Indentation = 1;
    SuppressImplicitBase = true;
    SilentNullStatement = true;
    NeverSuppressScope = true;
    UseStdOffsetOf = true;
  }

  mutable llvm::unique_function<bool(VarDecl *, raw_ostream &)> VarInitPrint;
  void setVarInitPrint(
      llvm::unique_function<bool(VarDecl *, raw_ostream &)> &&Func) {
    VarInitPrint = std::move(Func);
  }
  void resetVarInitPrint() { VarInitPrint = decltype(VarInitPrint){}; }
  bool overrideVarInitPrint(VarDecl *D, raw_ostream &OS) const override {
    return VarInitPrint(D, OS);
  }
};

struct ShaderPrintingPolicyBase : PrintingPolicy {
  HshTarget Target;
  virtual ~ShaderPrintingPolicyBase() = default;
  virtual void printStage(raw_ostream &OS, ASTContext &Context,
                          ArrayRef<FunctionRecord> FunctionRecords,
                          ArrayRef<UniformRecord> UniformRecords,
                          CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                          ArrayRef<AttributeRecord> Attributes,
                          ArrayRef<TextureRecord> Textures,
                          ArrayRef<SamplerBinding> Samplers,
                          unsigned NumColorAttachments, CompoundStmt *Stmts,
                          HshStage Stage, HshStage From, HshStage To,
                          ArrayRef<SampleCall> SampleCalls) = 0;
  explicit ShaderPrintingPolicyBase(HshTarget Target)
      : PrintingPolicy(LangOptions()), Target(Target) {}
};

using InShaderPipelineArgsType =
    ArrayRef<std::pair<StringRef, TemplateArgument>>;

template <typename ImplClass>
struct ShaderPrintingPolicy : PrintingCallbacks, ShaderPrintingPolicyBase {
  HshBuiltins &Builtins;
  bool EarlyDepthStencil = false;
  explicit ShaderPrintingPolicy(HshBuiltins &Builtins, HshTarget Target,
                                InShaderPipelineArgsType InShaderPipelineArgs)
      : ShaderPrintingPolicyBase(Target), Builtins(Builtins) {
    Callbacks = this;
    Indentation = 1;
    IncludeTagDefinition = false;
    SuppressTagKeyword = true;
    SuppressScope = true;
    AnonymousTagLocations = false;
    SuppressImplicitBase = true;
    PolishForDeclaration = true;
    PrintCanonicalTypes = true;

    SuppressNestedQualifiers = true;
    SuppressListInitialization = true;
    SeparateConditionVarDecls = true;
    ConstantExprsAsInt = true;
    SilentNullStatement = true;

    for (const auto &[Name, Arg] : InShaderPipelineArgs) {
      if (Name == "early_depth_stencil")
        EarlyDepthStencil = Arg.getAsIntegral().getZExtValue();
    }
  }

  static void PrintNZeros(raw_ostream &OS, unsigned N) {
    CommaArgPrinter ArgPrinter(OS);
    for (unsigned i = 0; i < N; ++i) {
      ArgPrinter.addArg() << '0';
    }
  }

  static void PrintNExprs(raw_ostream &OS,
                          const std::function<void(Expr *)> &ExprArg,
                          unsigned N, Expr *E) {
    CommaArgPrinter ArgPrinter(OS);
    for (unsigned i = 0; i < N; ++i) {
      ArgPrinter.addArg();
      ExprArg(E);
    }
  }

  StringRef overrideBuiltinTypeName(const BuiltinType *T) const override {
    if (T->isSignedIntegerOrEnumerationType()) {
      return ImplClass::SignedInt32Spelling;
    } else if (T->isUnsignedIntegerOrEnumerationType()) {
      return ImplClass::UnsignedInt32Spelling;
    } else if (T->isSpecificBuiltinType(BuiltinType::Float)) {
      return ImplClass::Float32Spelling;
    } else if (T->isSpecificBuiltinType(BuiltinType::Double)) {
      return ImplClass::Float64Spelling;
    }
    return {};
  }

  StringRef overrideTagDeclIdentifier(TagDecl *D) const override {
    const auto *Tp = D->getTypeForDecl();
    auto HBT = Builtins.identifyBuiltinType(Tp);
    if (HBT == HBT_None) {
      if (Tp->isSignedIntegerOrEnumerationType())
        return ImplClass::SignedInt32Spelling;
      if (Tp->isUnsignedIntegerOrEnumerationType())
        return ImplClass::UnsignedInt32Spelling;
      return {};
    }
    return HshBuiltins::getSpelling<ImplClass::SourceTarget>(HBT);
  }

  StringRef overrideBuiltinFunctionIdentifier(CallExpr *C) const override {
    if (auto *MemberCall = dyn_cast<CXXMemberCallExpr>(C)) {
      auto HBM = Builtins.identifyBuiltinMethod(MemberCall->getMethodDecl());
      if (HBM == HBM_None)
        return {};
      return static_cast<const ImplClass &>(*this).identifierOfCXXMethod(
          HBM, MemberCall);
    }
    if (auto *DeclRef =
            dyn_cast<DeclRefExpr>(C->getCallee()->IgnoreParenImpCasts())) {
      if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
        auto HBF = Builtins.identifyBuiltinFunction(FD);
        if (HBF == HBF_None)
          return {};
        return HshBuiltins::getSpelling<ImplClass::SourceTarget>(HBF);
      }
    }
    return {};
  }

  bool overrideCallArguments(
      CallExpr *C, const std::function<void(StringRef)> &StringArg,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (auto *MemberCall = dyn_cast<CXXMemberCallExpr>(C)) {
      auto HBM = Builtins.identifyBuiltinMethod(MemberCall->getMethodDecl());
      if (HBM == HBM_None)
        return {};
      return static_cast<const ImplClass &>(*this).overrideCXXMethodArguments(
          HBM, MemberCall, StringArg, ExprArg);
    }
    return false;
  }

  mutable std::string EnumValStr;
  StringRef overrideDeclRefIdentifier(DeclRefExpr *DR) const override {
    if (auto *ECD = dyn_cast<EnumConstantDecl>(DR->getDecl())) {
      EnumValStr.clear();
      raw_string_ostream OS(EnumValStr);
      OS << ECD->getInitVal();
      return OS.str();
    }
    return {};
  }

  StringRef overrideMemberExpr(MemberExpr *ME) const override {
    if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      auto HPF = Builtins.identifyBuiltinPipelineField(FD);
      switch (HPF) {
      case HPF_position:
        return static_cast<const ImplClass &>(*this).identifierOfVertexPosition(
            FD);
      case HPF_color_out:
        return static_cast<const ImplClass &>(*this)
            .identifierOfColorAttachment(FD);
      default:
        break;
      }
    }
    return {};
  }

  static DeclRefExpr *GetMemberExprBase(MemberExpr *ME) {
    Expr *E = ME->getBase()->IgnoreParenImpCasts();
    if (auto *OCE = dyn_cast<CXXOperatorCallExpr>(E)) {
      if (OCE->getOperator() == OO_Arrow)
        E = OCE->getArg(0)->IgnoreParenImpCasts();
    }
    return dyn_cast<DeclRefExpr>(E);
  }

  StringRef prependMemberExprBase(MemberExpr *ME,
                                  bool &ReplaceBase) const override {
    if (auto *DRE = GetMemberExprBase(ME)) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl())) {
        if (Builtins.getVertexAttributeRecord(PVD))
          return ImplClass::VertexBufferBase;
        if (Builtins.getUniformRecord(PVD)) {
          ReplaceBase = true;
          return PVD->getName();
        }
      }
      if (ImplClass::NoUniformVarDecl &&
          DRE->getDecl()->getName() == "_from_host"_ll)
        ReplaceBase = true;
    }
    return {};
  }

  bool shouldPrintMemberExprUnderscore(MemberExpr *ME) const override {
    if (auto *DRE = GetMemberExprBase(ME)) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl())) {
        if (Builtins.getVertexAttributeRecord(PVD) ||
            Builtins.getUniformRecord(PVD))
          return true;
      }
    }
    return false;
  }

  SmallVector<const CXXRecordDecl *, 8> NestedRecords;

  void PrintNestedStructs(raw_ostream &OS, ASTContext &Context) {
    unsigned Idx = 0;
    for (const auto *Record : NestedRecords) {
      OS << "struct __Struct" << Idx++ << " {\n";
      for (auto *FD : Record->fields()) {
        PrintStructField(OS, Context, FD->getType(), FD->getName(),
                         ArrayWaitType::NoArray, 1);
        OS << ";\n";
      }
      OS << "};\n";
    }
  }

  enum class ArrayWaitType { NoArray, StdArray, AlignedArray };

  ArrayWaitType getArrayWaitType(const CXXRecordDecl *RD) const {
    if (Builtins.isStdArrayType(RD))
      return ArrayWaitType::StdArray;
    if (Builtins.isAlignedArrayType(RD))
      return ArrayWaitType::AlignedArray;
    return ArrayWaitType::NoArray;
  }

  void GatherNestedStructField(ASTContext &Context, QualType Tp,
                               ArrayWaitType WaitingForArray) {
    if (Builtins.identifyBuiltinType(Tp) == HBT_None) {
      if (auto *SubRecord = Tp->getAsCXXRecordDecl()) {
        GatherNestedStructFields(Context, SubRecord, WaitingForArray);
        return;
      }
    }

    if (const auto *AT =
            dyn_cast_or_null<ConstantArrayType>(Tp->getAsArrayTypeUnsafe())) {
      QualType ElemType = AT->getElementType();
      if (WaitingForArray == ArrayWaitType::AlignedArray) {
        ElemType = ElemType->getAsCXXRecordDecl()->field_begin()->getType();
      }

      if (Builtins.identifyBuiltinType(ElemType) == HBT_None) {
        if (auto *SubRecord = ElemType->getAsCXXRecordDecl()) {
          GatherNestedStructFields(Context, SubRecord, ArrayWaitType::NoArray);
          return;
        }
      }

      GatherNestedStructField(Context, ElemType, ArrayWaitType::NoArray);
      return;
    }
  }

  void GatherNestedStructFields(ASTContext &Context,
                                const CXXRecordDecl *Record,
                                ArrayWaitType WaitingForArray) {
    if (WaitingForArray == ArrayWaitType::NoArray)
      WaitingForArray = getArrayWaitType(Record);
    if (WaitingForArray != ArrayWaitType::NoArray) {
      for (auto *FD : Record->fields()) {
        GatherNestedStructField(Context, FD->getType(), WaitingForArray);
      }
    } else {
      for (auto *FD : Record->fields()) {
        GatherNestedStructField(Context, FD->getType(), ArrayWaitType::NoArray);
      }
      if (std::find(NestedRecords.begin(), NestedRecords.end(), Record) ==
          NestedRecords.end())
        NestedRecords.push_back(Record);
    }
  }

  void GatherNestedPackoffsetFields(ASTContext &Context,
                                    const CXXRecordDecl *Record,
                                    CharUnits BaseOffset = {}) {
    ArrayWaitType WaitingForArray = getArrayWaitType(Record);

    for (auto *FD : Record->fields()) {
      GatherNestedStructField(Context, FD->getType(), WaitingForArray);
    }
  }

  static constexpr std::array<char, 4> VectorComponents{'x', 'y', 'z', 'w'};

  void PrintStructField(raw_ostream &OS, ASTContext &Context, QualType Tp,
                        const Twine &FieldName, ArrayWaitType WaitingForArray,
                        unsigned Indent) {
    if (Builtins.identifyBuiltinType(Tp) == HBT_None) {
      if (auto *SubRecord = Tp->getAsCXXRecordDecl()) {
        PrintStructFields(OS, Context, SubRecord, FieldName, WaitingForArray,
                          Indent);
        return;
      }
    }

    if (const auto *AT =
            dyn_cast_or_null<ConstantArrayType>(Tp->getAsArrayTypeUnsafe())) {
      QualType ElemType = AT->getElementType();
      if (WaitingForArray == ArrayWaitType::AlignedArray) {
        ElemType = ElemType->getAsCXXRecordDecl()->field_begin()->getType();
      }

      unsigned Size = AT->getSize().getZExtValue();
      Twine Tw1 = Twine('[') + Twine(Size);
      Twine Tw2 = Tw1 + Twine(']');
      Twine ArrayFieldName = FieldName + Tw2;

      if (Builtins.identifyBuiltinType(ElemType) == HBT_None) {
        if (auto *SubRecord = ElemType->getAsCXXRecordDecl()) {
          PrintStructFields(OS, Context, SubRecord, ArrayFieldName,
                            ArrayWaitType::NoArray, Indent);
          return;
        }
      }

      PrintStructField(OS, Context, ElemType, ArrayFieldName,
                       ArrayWaitType::NoArray, Indent);
      return;
    }

    if (WaitingForArray == ArrayWaitType::NoArray) {
      OS.indent(Indent * 2);
      Tp.print(OS, *this, FieldName, Indent);
    }
  }

  void PrintStructFields(raw_ostream &OS, ASTContext &Context,
                         const CXXRecordDecl *Record, const Twine &FieldName,
                         ArrayWaitType WaitingForArray, unsigned Indent) {
    if (WaitingForArray == ArrayWaitType::NoArray)
      WaitingForArray = getArrayWaitType(Record);
    if (WaitingForArray != ArrayWaitType::NoArray) {
      for (auto *FD : Record->fields()) {
        PrintStructField(OS, Context, FD->getType(), FieldName, WaitingForArray,
                         Indent);
      }
    } else {
      auto *Search =
          std::find(NestedRecords.begin(), NestedRecords.end(), Record);
      assert(Search != NestedRecords.end());
      OS.indent(Indent * 2)
          << "__Struct" << Search - NestedRecords.begin() << ' ' << FieldName;
    }
  }

  void PrintPackoffsetField(raw_ostream &OS, ASTContext &Context, QualType Tp,
                            const Twine &FieldName,
                            ArrayWaitType WaitingForArray, CharUnits Offset) {
    assert(Offset.getQuantity() % 4 == 0);
    ImplClass::PrintBeforePackoffset(OS, Offset);
    PrintStructField(OS, Context, Tp, FieldName, WaitingForArray, 1);
    ImplClass::PrintAfterPackoffset(OS, Offset);
  }

  void PrintPackoffsetFields(raw_ostream &OS, ASTContext &Context,
                             const CXXRecordDecl *Record,
                             const Twine &PrefixName,
                             CharUnits BaseOffset = {}) {
    ArrayWaitType WaitingForArray = getArrayWaitType(Record);

    const ASTRecordLayout &RL = Context.getASTRecordLayout(Record);
    for (auto *FD : Record->fields()) {
      auto Offset = BaseOffset + Context.toCharUnitsFromBits(
                                     RL.getFieldOffset(FD->getFieldIndex()));
      Twine Tw1 = PrefixName + Twine('_');
      PrintPackoffsetField(OS, Context, FD->getType(),
                           WaitingForArray != ArrayWaitType::NoArray
                               ? PrefixName
                               : Tw1 + FD->getName(),
                           WaitingForArray, Offset);
    }
  }

  void PrintAttributeField(raw_ostream &OS, ASTContext &Context, QualType Tp,
                           const Twine &FieldName,
                           ArrayWaitType WaitingForArray, unsigned Indent,
                           unsigned &Location, unsigned ArraySize) {
    if (Builtins.identifyBuiltinType(Tp) == HBT_None) {
      if (auto *SubRecord = Tp->getAsCXXRecordDecl()) {
        PrintAttributeFields(OS, Context, SubRecord, FieldName, WaitingForArray,
                             Indent, Location);
        return;
      }
    }

    if (const auto *AT =
            dyn_cast_or_null<ConstantArrayType>(Tp->getAsArrayTypeUnsafe())) {
      QualType ElemType = AT->getElementType();
      if (WaitingForArray == ArrayWaitType::AlignedArray) {
        ElemType = ElemType->getAsCXXRecordDecl()->field_begin()->getType();
      }

      unsigned Size = AT->getSize().getZExtValue();
      Twine Tw1 = Twine('[') + Twine(Size);
      Twine Tw2 = Tw1 + Twine(']');
      Twine ArrayFieldName = FieldName + Tw2;

      if (Builtins.identifyBuiltinType(ElemType) == HBT_None) {
        if (auto *SubRecord = ElemType->getAsCXXRecordDecl()) {
          PrintAttributeFields(OS, Context, SubRecord, ArrayFieldName,
                               ArrayWaitType::NoArray, Indent, Location);
          return;
        }
      }

      PrintAttributeField(OS, Context, ElemType, ArrayFieldName,
                          ArrayWaitType::NoArray, Indent, Location, Size);
      return;
    }

    if (WaitingForArray == ArrayWaitType::NoArray) {
      HshBuiltinType HBT = Builtins.identifyBuiltinType(Tp);
      if (HshBuiltins::isMatrixType(HBT)) {
        switch (HBT) {
        case HBT_float3x3:
          static_cast<const ImplClass &>(*this).PrintAttributeFieldSpelling(
              OS, Tp, FieldName, Location, Indent);
          Location += 3 * ArraySize;
          break;
        case HBT_float4x4:
          static_cast<const ImplClass &>(*this).PrintAttributeFieldSpelling(
              OS, Tp, FieldName, Location, Indent);
          Location += 4 * ArraySize;
          break;
        default:
          llvm_unreachable("Unhandled matrix type");
        }
      } else {
        static_cast<const ImplClass &>(*this).PrintAttributeFieldSpelling(
            OS, Tp, FieldName, Location, Indent);
        Location += ArraySize;
      }
    }
  }

  void PrintAttributeFields(raw_ostream &OS, ASTContext &Context,
                            const CXXRecordDecl *Record, const Twine &FieldName,
                            ArrayWaitType WaitingForArray, unsigned Indent,
                            unsigned &Location) {
    if (WaitingForArray == ArrayWaitType::NoArray)
      WaitingForArray = getArrayWaitType(Record);
    if (WaitingForArray != ArrayWaitType::NoArray) {
      for (auto *FD : Record->fields()) {
        PrintAttributeField(OS, Context, FD->getType(), FieldName,
                            WaitingForArray, Indent, Location, 1);
      }
    }
  }
};

struct GLSLPrintingPolicy : ShaderPrintingPolicy<GLSLPrintingPolicy> {
  static constexpr HshTarget SourceTarget = HT_GLSL;
  static constexpr bool NoUniformVarDecl = true;
  static constexpr llvm::StringLiteral SignedInt32Spelling{"int"};
  static constexpr llvm::StringLiteral UnsignedInt32Spelling{"uint"};
  static constexpr llvm::StringLiteral Float32Spelling{"float"};
  static constexpr llvm::StringLiteral Float64Spelling{"double"};
  static constexpr llvm::StringLiteral VertexBufferBase{""};

  static constexpr StringRef identifierOfVertexPosition(FieldDecl *FD) {
    return "gl_Position"_ll;
  }

  static constexpr StringRef identifierOfColorAttachment(FieldDecl *FD) {
    return "_color_out"_ll;
  }

  static constexpr StringRef identifierOfCXXMethod(HshBuiltinCXXMethod HBM,
                                                   CXXMemberCallExpr *C) {
    switch (HBM) {
    case HBM_sample2d:
    case HBM_render_sample2d:
    case HBM_sample_bias2d:
      return "texture"_ll;
    default:
      return {};
    }
  }

  static constexpr bool
  overrideCXXMethodArguments(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C,
                             const std::function<void(StringRef)> &StringArg,
                             const std::function<void(Expr *)> &ExprArg) {
    switch (HBM) {
    case HBM_sample2d:
    case HBM_render_sample2d:
    case HBM_sample_bias2d: {
      ExprArg(C->getImplicitObjectArgument()->IgnoreParenImpCasts());
      ExprArg(C->getArg(0));
      if (HBM == HBM_sample_bias2d)
        ExprArg(C->getArg(1));
      return true;
    }
    default:
      return false;
    }
  }

  bool overrideCXXOperatorCall(
      CXXOperatorCallExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (C->getNumArgs() == 1) {
      if (C->getOperator() == OO_Star) {
        /* Ignore derefs */
        ExprArg(C->getArg(0));
        return true;
      }
    }
    return false;
  }

  bool overrideCXXTemporaryObjectExpr(
      CXXTemporaryObjectExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    auto DTp = Builtins.identifyBuiltinType(C->getType());
    if (HshBuiltins::isVectorType(DTp)) {
      if (C->getNumArgs() == 0) {
        /* Implicit zero vector construction */
        OS << HshBuiltins::getSpelling<SourceTarget>(DTp) << '(';
        PrintNZeros(OS, HshBuiltins::getVectorSize(DTp));
        OS << ')';
        return true;
      } else if (C->getNumArgs() == 1 &&
                 !HshBuiltins::isVectorType(
                     Builtins.identifyBuiltinType(C->getArg(0)->getType()))) {
        /* Implicit scalar-to-vector conversion */
        OS << HshBuiltins::getSpelling<SourceTarget>(DTp) << '(';
        PrintNExprs(OS, ExprArg, HshBuiltins::getVectorSize(DTp), C->getArg(0));
        OS << ')';
        return true;
      }
    }
    return false;
  }

  static void PrintBeforePackoffset(raw_ostream &OS, CharUnits Offset) {
    OS.indent(2) << "layout(offset = " << Offset.getQuantity() << ")\n";
  }

  static void PrintAfterPackoffset(raw_ostream &OS, CharUnits Offset) {
    OS << ";\n";
  }

  void PrintAttributeFieldSpelling(raw_ostream &OS, QualType Tp,
                                   const Twine &FieldName, unsigned Location,
                                   unsigned Indent) const {
    OS << "layout(location = " << Location << ") in ";
    Tp.print(OS, *this);
    OS << " " << FieldName << ";\n";
  }

  static constexpr llvm::StringLiteral GLSLRuntimeSupport{
      R"(#version 450 core
float saturate(float val) {
  return clamp(val, 0.0, 1.0);
}
vec2 saturate(vec2 val) {
  return clamp(val, vec2(0.0, 0.0), vec2(1.0, 1.0));
}
vec3 saturate(vec3 val) {
  return clamp(val, vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));
}
vec4 saturate(vec4 val) {
  return clamp(val, vec4(0.0, 0.0, 0.0, 0.0), vec4(1.0, 1.0, 1.0, 1.0));
}
)"};

  void printStage(raw_ostream &OS, ASTContext &Context,
                  ArrayRef<FunctionRecord> FunctionRecords,
                  ArrayRef<UniformRecord> UniformRecords,
                  CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                  ArrayRef<AttributeRecord> Attributes,
                  ArrayRef<TextureRecord> Textures,
                  ArrayRef<SamplerBinding> Samplers,
                  unsigned NumColorAttachments, CompoundStmt *Stmts,
                  HshStage Stage, HshStage From, HshStage To,
                  ArrayRef<SampleCall> SampleCalls) override {
    OS << GLSLRuntimeSupport;

    auto PrintFunction = [&](const FunctionDecl *FD) {
      SuppressSpecifiers = false;
      FD->getReturnType().print(OS, *this);
      OS << ' ';
      SuppressSpecifiers = true;
      FD->print(OS, *this);
    };
    bool OldTerseOutput = TerseOutput;
    TerseOutput = true;
    bool OldSuppressSpecifiers = SuppressSpecifiers;
    for (auto &Function : FunctionRecords) {
      if ((1u << Stage) & Function.UseStages) {
        PrintFunction(Function.Function);
        OS << ";\n";
      }
    }
    TerseOutput = false;
    for (auto &Function : FunctionRecords) {
      if ((1u << Stage) & Function.UseStages) {
        PrintFunction(Function.Function);
      }
    }
    TerseOutput = OldTerseOutput;
    SuppressSpecifiers = OldSuppressSpecifiers;

    NestedRecords.clear();
    for (auto &Record : UniformRecords) {
      if ((1u << Stage) & Record.UseStages)
        GatherNestedPackoffsetFields(Context, Record.Record);
    }

    PrintNestedStructs(OS, Context);

    unsigned Binding = 0;
    for (auto &Record : UniformRecords) {
      if ((1u << Stage) & Record.UseStages) {
        OS << "layout(std140, binding = " << Binding << ") uniform b" << Binding
           << '_' << Record.Record->getName() << " {\n";
        PrintPackoffsetFields(OS, Context, Record.Record, Record.Name);
        OS << "};\n";
      }
      ++Binding;
    }

    if (FromRecord && !FromRecord->fields().empty()) {
      OS << "in " << HshStageToString(From) << "_to_" << HshStageToString(Stage)
         << " {\n";
      for (auto *FD : FromRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "} _from_" << HshStageToString(From) << ";\n";
    }

    if (ToRecord && !ToRecord->fields().empty()) {
      OS << "out " << HshStageToString(Stage) << "_to_" << HshStageToString(To)
         << " {\n";
      for (auto *FD : ToRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "} _to_" << HshStageToString(To) << ";\n";
    }

    if (Stage == HshVertexStage) {
      uint32_t Location = 0;
      for (const auto &Attribute : Attributes) {
        for (const auto *FD : Attribute.Record->fields()) {
          QualType Tp = FD->getType().getUnqualifiedType();
          Twine Tw1 = Twine(Attribute.Name) + Twine('_');
          PrintAttributeField(OS, Context, Tp, Tw1 + FD->getName(),
                              ArrayWaitType::NoArray, 0, Location, 1);
        }
      }
    }

    uint32_t TexBinding = 0;
    for (const auto &Tex : Textures) {
      if ((1u << Stage) & Tex.UseStages)
        OS << "layout(binding = " << TexBinding << ") uniform "
           << HshBuiltins::getSpelling<SourceTarget>(
                  BuiltinTypeOfTexture(Tex.Kind))
           << " " << Tex.TexParm->getName() << ";\n";
      ++TexBinding;
    }

    if (Stage == HshFragmentStage) {
      OS << "layout(location = 0) out vec4 _color_out[" << NumColorAttachments
         << "];\n";
      if (EarlyDepthStencil)
        OS << "layout(early_fragment_tests) in;\n";
    }

    OS << "void main() ";
    Stmts->printPretty(OS, nullptr, *this);
  }

  using ShaderPrintingPolicy<GLSLPrintingPolicy>::ShaderPrintingPolicy;
};

struct HLSLPrintingPolicy : ShaderPrintingPolicy<HLSLPrintingPolicy> {
  HLSLPrintingPolicy(HshBuiltins &Builtins, HshTarget Target,
                     InShaderPipelineArgsType InShaderPipelineArgs)
      : ShaderPrintingPolicy(Builtins, Target, InShaderPipelineArgs) {
    NoLoopInitVar = true;
  }

  static constexpr HshTarget SourceTarget = HT_HLSL;
  static constexpr bool NoUniformVarDecl = true;
  static constexpr llvm::StringLiteral SignedInt32Spelling{"int"};
  static constexpr llvm::StringLiteral UnsignedInt32Spelling{"uint"};
  static constexpr llvm::StringLiteral Float32Spelling{"float"};
  static constexpr llvm::StringLiteral Float64Spelling{"double"};
  static constexpr llvm::StringLiteral VertexBufferBase{"_vert_data."};

  std::string VertexPositionIdentifier;
  StringRef identifierOfVertexPosition(FieldDecl *FD) const {
    return VertexPositionIdentifier;
  }

  static constexpr StringRef identifierOfColorAttachment(FieldDecl *FD) {
    return "_targets_out._color_out"_ll;
  }

  mutable std::string CXXMethodIdentifier;
  StringRef identifierOfCXXMethod(HshBuiltinCXXMethod HBM,
                                  CXXMemberCallExpr *C) const {
    switch (HBM) {
    case HBM_sample2d:
    case HBM_render_sample2d:
    case HBM_sample_bias2d: {
      CXXMethodIdentifier.clear();
      raw_string_ostream OS(CXXMethodIdentifier);
      C->getImplicitObjectArgument()->printPretty(OS, nullptr, *this);
      OS << (HBM == HBM_sample_bias2d ? ".SampleBias" : ".Sample");
      return OS.str();
    }
    default:
      return {};
    }
  }

  ArrayRef<SampleCall> ThisSampleCalls;
  bool
  overrideCXXMethodArguments(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C,
                             const std::function<void(StringRef)> &StringArg,
                             const std::function<void(Expr *)> &ExprArg) const {
    switch (HBM) {
    case HBM_sample2d:
    case HBM_render_sample2d:
    case HBM_sample_bias2d: {
      auto Search =
          std::find_if(ThisSampleCalls.begin(), ThisSampleCalls.end(),
                       [&](const auto &Other) { return C == Other.Expr; });
      assert(Search != ThisSampleCalls.end() && "sample call must exist");
      std::string SamplerArg{"_sampler"};
      raw_string_ostream OS(SamplerArg);
      OS << Search->SamplerIndex;
      StringArg(OS.str());
      ExprArg(C->getArg(0));
      if (HBM == HBM_sample_bias2d)
        ExprArg(C->getArg(1));
      return true;
    }
    default:
      return false;
    }
  }

  bool overrideCXXOperatorCall(
      CXXOperatorCallExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (C->getNumArgs() == 1) {
      if (C->getOperator() == OO_Star) {
        /* Ignore derefs */
        ExprArg(C->getArg(0));
        return true;
      }
    } else if (C->getNumArgs() == 2) {
      if (C->getOperator() == OO_Star) {
        if (HshBuiltins::isMatrixType(
                Builtins.identifyBuiltinType(C->getArg(0)->getType())) ||
            HshBuiltins::isMatrixType(
                Builtins.identifyBuiltinType(C->getArg(1)->getType()))) {
          /* HLSL matrix math operation */
          OS << "mul(";
          ExprArg(C->getArg(0));
          OS << ", ";
          ExprArg(C->getArg(1));
          OS << ")";
          return true;
        }
      }
    }
    return false;
  }

  bool overrideCXXTemporaryObjectExpr(
      CXXTemporaryObjectExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    auto DTp = Builtins.identifyBuiltinType(C->getType());
    if (C->getNumArgs() == 1) {
      auto STp = Builtins.identifyBuiltinType(C->getArg(0)->getType());
      switch (DTp) {
      case HBT_float3x3:
        switch (STp) {
        case HBT_float4x4:
          OS << "float4x4_to_float3x3(";
          ExprArg(C->getArg(0));
          OS << ")";
          return true;
        default:
          break;
        }
        break;
      default:
        break;
      }
    }
    if (HshBuiltins::isVectorType(DTp)) {
      if (C->getNumArgs() == 0) {
        /* Implicit zero vector construction */
        OS << HshBuiltins::getSpelling<SourceTarget>(DTp) << '(';
        PrintNZeros(OS, HshBuiltins::getVectorSize(DTp));
        OS << ')';
        return true;
      } else if (C->getNumArgs() == 1 &&
                 !HshBuiltins::isVectorType(
                     Builtins.identifyBuiltinType(C->getArg(0)->getType()))) {
        /* Implicit scalar-to-vector conversion */
        OS << HshBuiltins::getSpelling<SourceTarget>(DTp) << '(';
        PrintNExprs(OS, ExprArg, HshBuiltins::getVectorSize(DTp), C->getArg(0));
        OS << ')';
        return true;
      }
    }
    return false;
  }

  CompoundStmt *ThisStmts = nullptr;
  std::string BeforeStatements;
  void
  printCompoundStatementBefore(const std::function<raw_ostream &()> &Indent,
                               CompoundStmt *CS) const override {
    if (CS == ThisStmts)
      Indent() << BeforeStatements;
  }

  std::string AfterStatements;
  void printCompoundStatementAfter(const std::function<raw_ostream &()> &Indent,
                                   CompoundStmt *CS) const override {
    if (CS == ThisStmts)
      Indent() << AfterStatements;
  }

  static void PrintBeforePackoffset(raw_ostream &OS, CharUnits Offset) {}

  static void PrintAfterPackoffset(raw_ostream &OS, CharUnits Offset) {
    auto Words = Offset.getQuantity() / 4;
    auto Vectors = Words / 4;
    auto Rem = Words % 4;
    OS << " : packoffset(c" << Vectors << '.' << VectorComponents[Rem]
       << ");\n";
  }

  void PrintAttributeFieldSpelling(raw_ostream &OS, QualType Tp,
                                   const Twine &FieldName, unsigned Location,
                                   unsigned Indent) const {
    if (Target == HT_VULKAN_SPIRV)
      OS.indent(Indent * 2) << "[[vk::location(" << Location << ")]] ";
    else
      OS.indent(Indent * 2);
    Tp.print(OS, *this);
    OS << " " << FieldName << " : ATTR" << Location << ";\n";
  }

  static constexpr llvm::StringLiteral HLSLRuntimeSupport{
      R"(static float3x3 float4x4_to_float3x3(float4x4 mtx) {
  return float3x3(mtx[0].xyz, mtx[1].xyz, mtx[2].xyz);
}
)"};

  void printStage(raw_ostream &OS, ASTContext &Context,
                  ArrayRef<FunctionRecord> FunctionRecords,
                  ArrayRef<UniformRecord> UniformRecords,
                  CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                  ArrayRef<AttributeRecord> Attributes,
                  ArrayRef<TextureRecord> Textures,
                  ArrayRef<SamplerBinding> Samplers,
                  unsigned NumColorAttachments, CompoundStmt *Stmts,
                  HshStage Stage, HshStage From, HshStage To,
                  ArrayRef<SampleCall> SampleCalls) override {
    OS << HLSLRuntimeSupport;
    ThisStmts = Stmts;
    ThisSampleCalls = SampleCalls;

    auto PrintFunction = [&](const FunctionDecl *FD) {
      OS << "static ";
      SuppressSpecifiers = false;
      FD->getReturnType().print(OS, *this);
      OS << ' ';
      SuppressSpecifiers = true;
      FD->print(OS, *this);
    };
    bool OldTerseOutput = TerseOutput;
    TerseOutput = true;
    bool OldSuppressSpecifiers = SuppressSpecifiers;
    for (auto &Function : FunctionRecords) {
      if ((1u << Stage) & Function.UseStages) {
        PrintFunction(Function.Function);
        OS << ";\n";
      }
    }
    TerseOutput = false;
    for (auto &Function : FunctionRecords) {
      if ((1u << Stage) & Function.UseStages) {
        PrintFunction(Function.Function);
      }
    }
    TerseOutput = OldTerseOutput;
    SuppressSpecifiers = OldSuppressSpecifiers;

    NestedRecords.clear();
    for (auto &Record : UniformRecords) {
      if ((1u << Stage) & Record.UseStages)
        GatherNestedPackoffsetFields(Context, Record.Record);
    }

    PrintNestedStructs(OS, Context);

    unsigned Binding = 0;
    for (auto &Record : UniformRecords) {
      if ((1u << Stage) & Record.UseStages) {
        OS << "cbuffer b" << Binding << '_' << Record.Record->getName()
           << " : register(b" << Binding << ") {\n";
        PrintPackoffsetFields(OS, Context, Record.Record, Record.Name);
        OS << "};\n";
      }
      ++Binding;
    }

    if (FromRecord) {
      OS << "struct " << HshStageToString(From) << "_to_"
         << HshStageToString(Stage) << " {\n";
      uint32_t VarIdx = 0;
      for (auto *FD : FromRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << " : VAR" << VarIdx++ << ";\n";
      }
      OS << "};\n";
    }

    if (ToRecord) {
      OS << "struct " << HshStageToString(Stage) << "_to_"
         << HshStageToString(To) << " {\n"
         << "  float4 _position : SV_Position;\n";
      uint32_t VarIdx = 0;
      for (auto *FD : ToRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << " : VAR" << VarIdx++ << ";\n";
      }
      OS << "};\n";
    }

    if (Stage == HshVertexStage) {
      OS << "struct host_vert_data {\n";
      unsigned Location = 0;
      for (const auto &Attribute : Attributes) {
        for (const auto *FD : Attribute.Record->fields()) {
          QualType Tp = FD->getType().getUnqualifiedType();
          Twine Tw1 = Twine(Attribute.Name) + Twine('_');
          PrintAttributeField(OS, Context, Tp, Tw1 + FD->getName(),
                              ArrayWaitType::NoArray, 1, Location, 1);
        }
      }
      OS << "};\n";
    }

    uint32_t TexBinding = 0;
    for (const auto &Tex : Textures) {
      if ((1u << Stage) & Tex.UseStages)
        OS << HshBuiltins::getSpelling<SourceTarget>(
                  BuiltinTypeOfTexture(Tex.Kind))
           << " " << Tex.TexParm->getName() << " : register(t" << TexBinding
           << ");\n";
      ++TexBinding;
    }

    uint32_t SamplerBinding = 0;
    for (const auto &Samp : Samplers) {
      if ((1u << Stage) & Samp.UseStages)
        OS << "SamplerState _sampler" << SamplerBinding << " : register(s"
           << SamplerBinding << ");\n";
      ++SamplerBinding;
    }

    if (Stage == HshFragmentStage) {
      OS << "struct color_targets_out {\n"
            "  float4 _color_out["
         << NumColorAttachments << "] : SV_Target"
         << ";\n"
            "};\n";
    }

    if (Stage == HshFragmentStage) {
      if (EarlyDepthStencil)
        OS << "[earlydepthstencil]\n";
      OS << "color_targets_out main(";
      BeforeStatements = "color_targets_out _targets_out;\n";
      AfterStatements = "return _targets_out;\n";
    } else if (ToRecord) {
      VertexPositionIdentifier.clear();
      raw_string_ostream PIO(VertexPositionIdentifier);
      PIO << "_to_" << HshStageToString(To) << "._position";
      OS << HshStageToString(Stage) << "_to_" << HshStageToString(To)
         << " main(";
      BeforeStatements.clear();
      raw_string_ostream BO(BeforeStatements);
      BO << HshStageToString(Stage) << "_to_" << HshStageToString(To) << " _to_"
         << HshStageToString(To) << ";\n";
      AfterStatements.clear();
      raw_string_ostream AO(AfterStatements);
      AO << "return _to_" << HshStageToString(To) << ";\n";
    }
    if (Stage == HshVertexStage)
      OS << "in host_vert_data _vert_data";
    else if (FromRecord)
      OS << "in " << HshStageToString(From) << "_to_" << HshStageToString(Stage)
         << " _from_" << HshStageToString(From);
    OS << ") ";
    Stmts->printPretty(OS, nullptr, *this);
  }
};

std::unique_ptr<ShaderPrintingPolicyBase>
MakePrintingPolicy(HshBuiltins &Builtins, HshTarget Target,
                   InShaderPipelineArgsType InShaderPipelineArgs) {
  switch (Target) {
  default:
  case HT_GLSL:
  case HT_DEKO3D:
    return std::make_unique<GLSLPrintingPolicy>(Builtins, Target,
                                                InShaderPipelineArgs);
  case HT_HLSL:
  case HT_DXBC:
  case HT_DXIL:
  case HT_VULKAN_SPIRV:
  case HT_METAL:
  case HT_METAL_BIN_MAC:
  case HT_METAL_BIN_IOS:
  case HT_METAL_BIN_TVOS:
    return std::make_unique<HLSLPrintingPolicy>(Builtins, Target,
                                                InShaderPipelineArgs);
  }
}

using StageSources = std::array<std::string, HshMaxStage>;

class StagesBuilder {
  ASTContext &Context;
  HshBuiltins &Builtins;
  DeclContext *BindingDeclContext;
  unsigned UseStages = 0;

  static IdentifierInfo &getToIdent(ASTContext &Context, HshStage Stage) {
    std::string VarName;
    raw_string_ostream VNS(VarName);
    VNS << "_to_" << HshStageToString(Stage);
    return Context.Idents.get(VNS.str());
  }

  static IdentifierInfo &getFromIdent(ASTContext &Context, HshStage Stage) {
    std::string VarName;
    raw_string_ostream VNS(VarName);
    VNS << "_from_" << HshStageToString(Stage);
    return Context.Idents.get(VNS.str());
  }

  static IdentifierInfo &getFromToIdent(ASTContext &Context, HshStage From,
                                        HshStage To) {
    std::string RecordName;
    raw_string_ostream RNS(RecordName);
    RNS << HshStageToString(From) << "_to_" << HshStageToString(To);
    return Context.Idents.get(RNS.str());
  }

  class InterfaceRecord {
    CXXRecordDecl *Record = nullptr;
    SmallVector<std::pair<Expr *, FieldDecl *>, 8> Fields;
    VarDecl *Producer = nullptr;
    VarDecl *Consumer = nullptr;
    HshStage SStage = HshNoStage, DStage = HshNoStage;

    MemberExpr *createFieldReference(ASTContext &Context, Expr *E, VarDecl *VD,
                                     bool Producer) {
      FieldDecl *Field = getFieldForExpr(Context, E, Producer);
      if (!Field)
        return nullptr;
      QualType Tp = Field->getType().getLocalUnqualifiedType();
      if (!Producer)
        Tp = Tp.withConst();
      return MemberExpr::CreateImplicit(
          Context,
          DeclRefExpr::Create(Context, {}, {}, VD, false, SourceLocation{},
                              VD->getType(), VK_XValue),
          false, Field, Tp, VK_XValue, OK_Ordinary);
    }

  public:
    void initializeRecord(ASTContext &Context, DeclContext *BindingDeclContext,
                          HshStage S, HshStage D) {
      Record = CXXRecordDecl::Create(Context, TTK_Struct, BindingDeclContext,
                                     {}, {}, &getFromToIdent(Context, S, D));
      Record->startDefinition();

      CanQualType CDType =
          Record->getTypeForDecl()->getCanonicalTypeUnqualified();

      VarDecl *PVD =
          VarDecl::Create(Context, BindingDeclContext, {}, {},
                          &getToIdent(Context, D), CDType, nullptr, SC_None);
      Producer = PVD;

      VarDecl *CVD =
          VarDecl::Create(Context, BindingDeclContext, {}, {},
                          &getFromIdent(Context, S), CDType, nullptr, SC_None);
      Consumer = CVD;

      SStage = S;
      DStage = D;
    }

    static bool isSameComparisonOperand(Expr *E1, Expr *E2) {
      if (E1 == E2)
        return true;
      E1->setValueKind(VK_RValue);
      E2->setValueKind(VK_RValue);
      return Expr::isSameComparisonOperand(E1, E2);
    }

    FieldDecl *getFieldForExpr(ASTContext &Context, Expr *E,
                               bool IgnoreExisting) {
      assert(Record && "Invalid InterfaceRecord requested from");
      for (auto &P : Fields) {
        if (isSameComparisonOperand(P.first, E))
          return IgnoreExisting ? nullptr : P.second;
      }
      std::string FieldName;
      raw_string_ostream FNS(FieldName);
      FNS << '_' << HshStageToString(SStage)[0] << HshStageToString(DStage)[0]
          << Fields.size();
      FieldDecl *FD = FieldDecl::Create(
          Context, Record, {}, {}, &Context.Idents.get(FNS.str()),
          E->getType().getUnqualifiedType(), {}, {}, false, ICIS_NoInit);
      FD->setAccess(AS_public);
      Fields.push_back(std::make_pair(E, FD));
      return FD;
    }

    MemberExpr *createProducerFieldReference(ASTContext &Context, Expr *E) {
      return createFieldReference(Context, E, Producer, true);
    }

    MemberExpr *createConsumerFieldReference(ASTContext &Context, Expr *E) {
      return createFieldReference(Context, E, Consumer, false);
    }

    void finalizeRecord(ASTContext &Context, HshBuiltins &Builtins) {
      std::stable_sort(
          Fields.begin(), Fields.end(), [&](const auto &a, const auto &b) {
            return Context.getTypeSizeInChars(a.second->getType()) >
                   Context.getTypeSizeInChars(b.second->getType());
          });

      for (auto &P : Fields)
        Record->addDecl(P.second);
      Record->completeDefinition();
    }

    CXXRecordDecl *getRecord() const { return Record; }
  };

  struct StageStmtList {
    SmallVector<Stmt *, 16> Stmts;
    CompoundStmt *CStmts = nullptr;
  };

  std::array<InterfaceRecord, HshMaxStage>
      InterStageRecords; /* Indexed by consumer stage */
  std::array<StageStmtList, HshMaxStage> StageStmts;
  std::array<SmallVector<SampleCall, 4>, HshMaxStage> SampleCalls;
  SmallVector<FunctionRecord, 4> FunctionRecords;
  SmallVector<UniformRecord, 4> UniformRecords;
  SmallVector<AttributeRecord, 4> AttributeRecords;
  SmallVector<TextureRecord, 8> Textures;
  SmallVector<SamplerRecord, 8> Samplers;
  SmallVector<SamplerBinding, 8> SamplerBindings;
  unsigned NumColorAttachments = 0;
  unsigned FinalStageCount = 0;
  SmallVector<VertexBinding, 4> VertexBindings;
  SmallVector<VertexAttribute, 4> VertexAttributes;
  DenseMap<ParmVarDecl *, unsigned> UseParmVarDecls;

public:
  StagesBuilder(ASTContext &Context, HshBuiltins &Builtins,
                DeclContext *BindingDeclContext, unsigned NumColorAttachments)
      : Context(Context), Builtins(Builtins),
        BindingDeclContext(BindingDeclContext),
        NumColorAttachments(NumColorAttachments) {}

  void updateUseStages() {
    for (int D = HshControlStage, S = HshVertexStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D))) {
        InterStageRecords[D].initializeRecord(Context, BindingDeclContext,
                                              HshStage(S), HshStage(D));
        S = D;
      }
    }
  }

  Expr *createInterStageReferenceExpr(Expr *E, HshStage From, HshStage To) {
    if (From == To || From == HshNoStage || To == HshNoStage)
      return E;
    assert(To > From && "cannot create backwards stage references");
    /* Create intermediate inter-stage assignments */
    for (int D = From + 1, S = From; D <= To; ++D) {
      if (UseStages & (1u << unsigned(D))) {
        InterfaceRecord &SRecord = InterStageRecords[S];
        InterfaceRecord &DRecord = InterStageRecords[D];
        if (MemberExpr *Producer =
                DRecord.createProducerFieldReference(Context, E)) {
          auto *AssignOp = BinaryOperator::Create(
              Context, Producer,
              S == From ? E : SRecord.createConsumerFieldReference(Context, E),
              BO_Assign, E->getType(), VK_XValue, OK_Ordinary, {}, {});
          addStageStmt(AssignOp, HshStage(S));
        }
        S = D;
      }
    }
    return InterStageRecords[To].createConsumerFieldReference(Context, E);
  }

  void addStageStmt(Stmt *S, HshStage Stage) {
    StageStmts[Stage].Stmts.push_back(S);
  }

  static bool CheckSamplersEqual(const APValue &A, const APValue &B) {
    if (!A.isStruct() || !B.isStruct())
      return false;
    unsigned NumFields = A.getStructNumFields();
    if (NumFields != B.getStructNumFields())
      return false;
    for (unsigned i = 0; i < NumFields; ++i) {
      const auto &AF = A.getStructField(i);
      const auto &BF = A.getStructField(i);
      if (AF.isInt() && BF.isInt()) {
        if (APSInt::compareValues(AF.getInt(), BF.getInt()))
          return false;
      } else if (AF.isFloat() && BF.isFloat()) {
        if (AF.getFloat().compare(BF.getFloat()) != APFloat::cmpEqual)
          return false;
      } else {
        return false;
      }
    }
    return true;
  }

  void registerSampleCall(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C,
                          HshStage Stage) {
    if (auto *DR = dyn_cast<DeclRefExpr>(
            C->getImplicitObjectArgument()->IgnoreParenImpCasts())) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DR->getDecl())) {
        auto &StageCalls = SampleCalls[Stage];
        for (const auto &Call : StageCalls)
          if (Call.Expr == C)
            return;
        APValue Res;
        Expr *SamplerArg = C->getArg(1);
        if (!SamplerArg->isCXX11ConstantExpr(Context, &Res)) {
          ReportNonConstexprSampler(SamplerArg, Context);
          return;
        }
        auto Search =
            std::find_if(Samplers.begin(), Samplers.end(), [&](const auto &S) {
              return CheckSamplersEqual(S.Config, Res);
            });
        if (Search == Samplers.end()) {
          Search =
              Samplers.insert(Samplers.end(), SamplerRecord{std::move(Res)});
        }
        unsigned RecordIdx = Search - Samplers.begin();
        auto BindingSearch = std::find_if(
            SamplerBindings.begin(), SamplerBindings.end(), [&](const auto &B) {
              return B.RecordIdx == RecordIdx && B.TextureDecl == PVD;
            });
        if (BindingSearch == SamplerBindings.end()) {
          auto TextureSearch =
              std::find_if(Textures.begin(), Textures.end(),
                           [&](const auto &Tex) { return Tex.TexParm == PVD; });
          assert(TextureSearch != Textures.end());
          BindingSearch = SamplerBindings.insert(
              SamplerBindings.end(),
              SamplerBinding{RecordIdx, TextureSearch - Textures.begin(), PVD,
                             1u << Stage});
        } else {
          BindingSearch->UseStages |= 1u << Stage;
        }
        StageCalls.push_back(
            {C, PVD, unsigned(BindingSearch - SamplerBindings.begin())});
      }
    }
  }

  void registerAttributeField(QualType FieldType, CharUnits Offset,
                              unsigned Binding) {
    auto HBT = Builtins.identifyBuiltinType(FieldType);

    if (HBT == HBT_None) {
      if (auto *SubRecord = FieldType->getAsCXXRecordDecl()) {
        registerAttributeFields(SubRecord, Binding, Offset);
        return;
      }
    }

    if (const auto *AT = dyn_cast_or_null<ConstantArrayType>(
            FieldType->getAsArrayTypeUnsafe())) {
      auto ElemSize = Context.getTypeSizeInChars(AT->getElementType());
      auto ElemAlign = Context.getTypeAlignInChars(AT->getElementType());
      unsigned Size = AT->getSize().getZExtValue();

      if (Builtins.identifyBuiltinType(AT->getElementType()) == HBT_None) {
        if (auto *SubRecord = AT->getElementType()->getAsCXXRecordDecl()) {
          for (unsigned i = 0; i < Size; ++i) {
            Offset = Offset.alignTo(ElemAlign);
            registerAttributeFields(SubRecord, Binding, Offset);
            Offset += ElemSize;
          }
          return;
        }
      }

      for (unsigned i = 0; i < Size; ++i) {
        Offset = Offset.alignTo(ElemAlign);
        registerAttributeField(AT->getElementType(), Offset, Binding);
        Offset += ElemSize;
      }
      return;
    }

    auto FieldSize = Context.getTypeSizeInChars(FieldType);
    auto FieldAlign = Context.getTypeAlignInChars(FieldType);
    auto Format = Builtins.formatOfType(FieldType);
    auto ProcessField = [&]() {
      Offset = Offset.alignTo(FieldAlign);
      VertexAttributes.push_back(
          VertexAttribute{Offset.getQuantity(), Binding, Format});
      Offset += FieldSize;
    };
    if (HshBuiltins::isMatrixType(HBT)) {
      auto ColType = Builtins.getType(HshBuiltins::getMatrixColumnType(HBT));
      FieldSize = Context.getTypeSizeInChars(ColType);
      FieldAlign = Context.getTypeSizeInChars(ColType);
      Format = Builtins.formatOfType(ColType);
      auto ColumnCount = HshBuiltins::getMatrixColumnCount(HBT);
      for (unsigned i = 0; i < ColumnCount; ++i)
        ProcessField();
    } else {
      ProcessField();
    }
  }

  void registerAttributeFields(const CXXRecordDecl *Record, unsigned Binding,
                               CharUnits BaseOffset = {}) {
    const ASTRecordLayout &RL = Context.getASTRecordLayout(Record);
    for (const auto *Field : Record->fields()) {
      auto Offset = BaseOffset + Context.toCharUnitsFromBits(
                                     RL.getFieldOffset(Field->getFieldIndex()));
      registerAttributeField(Field->getType(), Offset, Binding);
    }
  }

  void registerAttributeRecord(AttributeRecord Attribute) {
    auto Search =
        std::find_if(AttributeRecords.begin(), AttributeRecords.end(),
                     [&](const auto &A) { return A.Name == Attribute.Name; });
    if (Search != AttributeRecords.end())
      return;
    unsigned Binding = AttributeRecords.size();
    AttributeRecords.push_back(Attribute);

    auto *Type = Attribute.Record->getTypeForDecl();
    auto Size = Context.getTypeSizeInChars(Type);
    auto Align = Context.getTypeAlignInChars(Type);
    auto SizeOf = Size.alignTo(Align).getQuantity();
    VertexBindings.push_back(VertexBinding{Binding, SizeOf, Attribute.Kind});
    registerAttributeFields(Attribute.Record, Binding);
  }

  static StaticAssertDecl *
  CreateEnumSizeAssert(ASTContext &Context, DeclContext *DC, FieldDecl *FD) {
    return StaticAssertDecl::Create(
        Context, DC, {},
        BinaryOperator::Create(
            Context,
            new (Context) UnaryExprOrTypeTraitExpr(
                UETT_SizeOf,
                new (Context)
                    ParenExpr({}, {},
                              DeclRefExpr::Create(Context, {}, {}, FD, false,
                                                  SourceLocation{},
                                                  FD->getType(), VK_XValue)),
                Context.IntTy, {}, {}),
            IntegerLiteral::Create(Context, APInt(32, 4), Context.IntTy, {}),
            BO_EQ, Context.BoolTy, VK_XValue, OK_Ordinary, {}, {}),
        Context.getPredefinedStringLiteralFromCache(
            "underlying enum type must be 32-bits wide"_ll),
        {}, false);
  }

  static StaticAssertDecl *CreateOffsetAssert(ASTContext &Context,
                                              DeclContext *DC, FieldDecl *FD,
                                              CharUnits Offset) {
    return StaticAssertDecl::Create(
        Context, DC, {},
        BinaryOperator::Create(
            Context,
            OffsetOfExpr::Create(Context, Context.IntTy, {},
                                 Context.getTrivialTypeSourceInfo(QualType{
                                     FD->getParent()->getTypeForDecl(), 0}),
                                 OffsetOfNode{{}, FD, {}}, {}, {}),
            IntegerLiteral::Create(Context, APInt(32, Offset.getQuantity()),
                                   Context.IntTy, {}),
            BO_EQ, Context.BoolTy, VK_XValue, OK_Ordinary, {}, {}),
        Context.getPredefinedStringLiteralFromCache(
            "compiler does not align field correctly"_ll),
        {}, false);
  }

  class UniformFieldValidator {
    ASTContext &Context;
    HshBuiltins &Builtins;
    DenseSet<const CXXRecordDecl *> Records;
    const FieldDecl *ArrayField = nullptr;
    bool InAlignedArray = false;

  public:
    UniformFieldValidator(ASTContext &Context, HshBuiltins &Builtins)
        : Context(Context), Builtins(Builtins) {}

    void Visit(const CXXRecordDecl *Record) {
      if (!Records.insert(Record).second)
        return;

      auto &Diags = Context.getDiagnostics();

      for (auto *Field : Record->fields()) {
        auto HBT = Builtins.identifyBuiltinType(Field->getType());
        if (HBT == HBT_None) {
          if (Field->getType()->isArrayType()) {
            auto *ArrayElemType =
                Field->getType()->getPointeeOrArrayElementType();
            if (!InAlignedArray &&
                Context.getTypeSizeInChars(ArrayElemType).getQuantity() % 16) {
              auto *ActiveField = ArrayField ? ArrayField : Field;
              SourceRange Range = ActiveField->getSourceRange();
              if (auto *TSI = ActiveField->getTypeSourceInfo()) {
                if (auto TSTL =
                        TSI->getTypeLoc()
                            .getAsAdjusted<TemplateSpecializationTypeLoc>()) {
                  Range = SourceRange(Range.getBegin(),
                                      TSTL.getLAngleLoc().getLocWithOffset(-1));
                } else if (auto TSTL = TSI->getTypeLoc()
                                           .getAsAdjusted<TypeSpecTypeLoc>()) {
                  Range = TSTL.getLocalSourceRange();
                }
              }
              Diags.Report(Range.getBegin(),
                           Diags.getCustomDiagID(
                               DiagnosticsEngine::Error,
                               "use aligned array to ensure each element "
                               "is stored in a 16 byte register"))
                  << Range
                  << FixItHint::CreateReplacement(Range, "hsh::aligned_array");
            }

            if (auto *Child = ArrayElemType->getAsCXXRecordDecl()) {
              SaveAndRestore<const FieldDecl *> SavedArrayField(ArrayField,
                                                                nullptr);
              SaveAndRestore<bool> SavedAlignedArrayField(InAlignedArray,
                                                          false);
              Visit(Child);
            }
          } else if (auto *Child = Field->getType()->getAsCXXRecordDecl()) {
            if (Builtins.isStdArrayType(Child)) {
              SaveAndRestore<const FieldDecl *> SavedArrayField(ArrayField,
                                                                Field);
              Visit(Child);
            } else if (Builtins.isAlignedArrayType(Child)) {
              SaveAndRestore<bool> SavedAlignedArrayField(InAlignedArray, true);
              Visit(Child);
            } else {
              Visit(Child);
            }
          }
        } else if (HshBuiltins::isMatrixType(HBT)) {
          if (auto *AlignedType = Builtins.getAlignedAvailable(Field)) {
            SourceRange Range = Field->getSourceRange();
            if (auto *TSI = Field->getTypeSourceInfo())
              Range = TSI->getTypeLoc()
                          .getAsAdjusted<TypeSpecTypeLoc>()
                          .getLocalSourceRange();
            Diags.Report(Range.getBegin(),
                         Diags.getCustomDiagID(
                             DiagnosticsEngine::Error,
                             "use aligned matrix to ensure each column "
                             "is stored in a 16 byte register"))
                << Range
                << FixItHint::CreateReplacement(Range, AlignedType->getName());
          }
        }
      }
    }
  };

  void registerUniform(StringRef Name, const CXXRecordDecl *Record,
                       unsigned Stages) {
    auto &Diags = Context.getDiagnostics();

    auto Search = std::find_if(UniformRecords.begin(), UniformRecords.end(),
                               [&](const auto &T) { return T.Name == Name; });
    if (Search != UniformRecords.end()) {
      Search->UseStages |= Stages;
      return;
    }

    UniformFieldValidator Validator(Context, Builtins);
    Validator.Visit(Record);

    const auto &RL = Context.getASTRecordLayout(Record);

    CharUnits HLSLOffset, CXXOffset;
    for (auto *Field : Record->fields()) {
      auto CXXFieldSize = Context.getTypeSizeInChars(Field->getType());
      auto HLSLFieldSize = CXXFieldSize.alignTo(CharUnits::fromQuantity(4));
      auto CXXFieldAlign = Context.getTypeAlignInChars(Field->getType());
      auto HLSLFieldAlign = CXXFieldAlign.alignTo(CharUnits::fromQuantity(4));

      HLSLOffset = HLSLOffset.alignTo(HLSLFieldAlign);
      CXXOffset = Context.toCharUnitsFromBits(
          RL.getFieldOffset(Field->getFieldIndex()));
      CharUnits HLSLEndOffset = HLSLOffset + HLSLFieldSize;
      bool FieldStraddled;
      if ((FieldStraddled = (HLSLEndOffset.getQuantity() - 1) / 16 >
                            HLSLOffset.getQuantity() / 16))
        HLSLOffset = HLSLOffset.alignTo(CharUnits::fromQuantity(16));
      CharUnits ThisOffset = HLSLOffset;
      if (HLSLOffset != CXXOffset) {
        unsigned FixAlign = 16;
        if (!FieldStraddled && HLSLFieldSize.getQuantity() < 16) {
          for (unsigned Align : {4, 8, 16}) {
            FixAlign = Align;
            CharUnits FixOffset =
                CXXOffset.alignTo(CharUnits::fromQuantity(Align));
            if (HLSLOffset == FixOffset)
              break;
          }
        }
        std::string AlignAsStr;
        raw_string_ostream OS(AlignAsStr);
        OS << "alignas(" << FixAlign << ") ";
        Diags.Report(
            Field->getBeginLoc(),
            Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                  "uniform field violates DirectX packing "
                                  "rules; align or pad by %0 bytes"))
            << int((HLSLOffset - CXXOffset).getQuantity())
            << Field->getSourceRange()
            << FixItHint::CreateInsertion(Field->getBeginLoc(), OS.str());
        return;
      }
      HLSLOffset += HLSLFieldSize;

      if (Field->getType()->isEnumeralType())
        BindingDeclContext->addDecl(
            CreateEnumSizeAssert(Context, BindingDeclContext, Field));

      if (!Builtins.isSupportedArrayType(Record))
        BindingDeclContext->addDecl(
            CreateOffsetAssert(Context, BindingDeclContext, Field, ThisOffset));
    }

    UniformRecords.push_back(UniformRecord{Name, Record, Stages});
  }

  void registerTexture(const ParmVarDecl *TexParm, HshTextureKind Kind,
                       unsigned Stages) {
    auto Search =
        std::find_if(Textures.begin(), Textures.end(),
                     [&](const auto &T) { return T.TexParm == TexParm; });
    if (Search != Textures.end()) {
      Search->UseStages |= Stages;
      return;
    }
    Textures.push_back(TextureRecord{TexParm, Kind, Stages});
  }

  void registerParmVarRef(ParmVarDecl *PVD, HshStage Stage) {
    UseParmVarDecls[PVD] |= 1u << unsigned(Stage);
  }

  void registerFunctionDecl(FunctionDecl *FD, HshStage Stage) {
    if (auto *FDDecl = FD->getDefinition())
      FD = FDDecl;
    else
      ReportUndefinedFunctionUsage(FD, Context);
    auto Search = std::find_if(
        FunctionRecords.begin(), FunctionRecords.end(),
        [&](const auto &T) { return T.Identifier == FD->getIdentifier(); });
    if (Search == FunctionRecords.end()) {
      FunctionRecords.push_back(
          FunctionRecord{FD->getIdentifier(), FD, 1u << unsigned(Stage)});
    } else if (Search->Function == FD) {
      Search->UseStages |= 1u << unsigned(Stage);
    } else {
      ReportOverloadedFunctionUsage(FD, Search->Function, Context);
    }
  }

  void prepare(CXXConstructorDecl *Ctor) {
    for (auto *Param : Ctor->parameters()) {
      auto HBT = Builtins.identifyBuiltinType(Param->getType());
      if (HshBuiltins::isTextureType(HBT)) {
        registerTexture(Param, KindOfTextureType(HBT), 0);
      }
    }
  }

  void finalizeResults(CXXConstructorDecl *Ctor) {
    for (int D = HshControlStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D)))
        InterStageRecords[D].finalizeRecord(Context, Builtins);
    }

    FinalStageCount = 0;
    for (int S = HshVertexStage; S < HshMaxStage; ++S) {
      if (UseStages & (1u << unsigned(S))) {
        ++FinalStageCount;
        auto &Stmts = StageStmts[S];
        Stmts.CStmts = CompoundStmt::Create(Context, Stmts.Stmts, {}, {});
      }
    }

    for (auto *Param : Ctor->parameters()) {
      unsigned Stages = 0;
      auto Search = UseParmVarDecls.find(Param);
      if (Search != UseParmVarDecls.end())
        Stages = Search->second;
      auto HBT = Builtins.identifyBuiltinType(Param->getType());
      if (HshBuiltins::isTextureType(HBT)) {
        registerTexture(Param, KindOfTextureType(HBT), Stages);
      } else if (auto *UR = Builtins.getUniformRecord(Param)) {
        registerUniform(Param->getName(), UR, Stages);
      } else if (auto *AR = Builtins.getVertexAttributeRecord(Param)) {
        registerAttributeRecord(AttributeRecord{
            Param->getName(), AR,
            Param->hasAttr<HshInstanceAttr>() ? PerInstance : PerVertex});
      }
    }
  }

  HshStage previousUsedStage(HshStage S) const {
    for (int D = S - 1; D >= HshVertexStage; --D) {
      if (UseStages & (1u << unsigned(D)))
        return HshStage(D);
    }
    return HshNoStage;
  }

  HshStage nextUsedStage(HshStage S) const {
    for (int D = S + 1; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D)))
        return HshStage(D);
    }
    return HshNoStage;
  }

  bool isStageUsed(HshStage S) const { return UseStages & (1u << S); }

  void setStageUsed(HshStage S) {
    if (S == HshNoStage)
      return;
    UseStages |= 1 << S;
  }

  StageSources printResults(ShaderPrintingPolicyBase &Policy) {
    StageSources Sources;

    for (int S = HshVertexStage; S < HshMaxStage; ++S) {
      if (UseStages & (1u << unsigned(S))) {
        raw_string_ostream OS(Sources[S]);
        HshStage NextStage = nextUsedStage(HshStage(S));
        Policy.printStage(
            OS, Context, FunctionRecords, UniformRecords,
            InterStageRecords[S].getRecord(),
            NextStage != HshNoStage ? InterStageRecords[NextStage].getRecord()
                                    : nullptr,
            AttributeRecords, Textures, SamplerBindings, NumColorAttachments,
            StageStmts[S].CStmts, HshStage(S), previousUsedStage(HshStage(S)),
            NextStage, SampleCalls[S]);
      }
    }

    return Sources;
  }

  unsigned getNumStages() const { return FinalStageCount; }
  unsigned getNumBindings() const { return VertexBindings.size(); }
  unsigned getNumAttributes() const { return VertexAttributes.size(); }
  unsigned getNumSamplers() const { return Samplers.size(); }
  unsigned getNumSamplerBindings() const { return SamplerBindings.size(); }
  ArrayRef<VertexBinding> getBindings() const { return VertexBindings; }
  ArrayRef<VertexAttribute> getAttributes() const { return VertexAttributes; }
  ArrayRef<SamplerRecord> getSamplers() const { return Samplers; }
  ArrayRef<SamplerBinding> getSamplerBindings() const {
    return SamplerBindings;
  }
};

struct StageBinaries
    : std::array<std::pair<std::vector<uint8_t>, uint64_t>, HshMaxStage> {
  void updateHashes() {
    for (auto &Binary : *this)
      if (!Binary.first.empty())
        Binary.second = xxHash64(Binary.first);
  }
};

class StagesCompilerBase {
protected:
  HshTarget Target;
  virtual StageBinaries doCompile(ArrayRef<std::string> Sources) const = 0;

public:
  explicit StagesCompilerBase(HshTarget Target) : Target(Target) {}
  virtual ~StagesCompilerBase() = default;
  StageBinaries compile(ArrayRef<std::string> Sources) const {
    auto Binaries = doCompile(Sources);
    Binaries.updateHashes();
    return Binaries;
  }
};

class StagesCompilerText : public StagesCompilerBase {
protected:
  StageBinaries doCompile(ArrayRef<std::string> Sources) const override {
    StageBinaries Binaries;
    auto OutIt = Binaries.begin();
    for (const auto &Stage : Sources) {
      auto &Out = OutIt++->first;
      if (Stage.empty())
        continue;
      Out.resize(Stage.size() + 1);
      std::memcpy(&Out[0], Stage.data(), Stage.size());
    }
    return Binaries;
  }

public:
  using StagesCompilerBase::StagesCompilerBase;
};

class StagesCompilerDxc : public StagesCompilerBase {
  DiagnosticsEngine &Diags;
  bool DebugInfo;
  WCHAR TShiftArg[4];
  WCHAR SShiftArg[4];
  CComPtr<IDxcCompiler3> Compiler;

  static constexpr std::array<LPCWSTR, 6> ShaderProfiles{
      L"vs_6_0", L"hs_6_0", L"ds_6_0", L"gs_6_0", L"ps_6_0"};

protected:
  StageBinaries doCompile(ArrayRef<std::string> Sources) const override {
    StageBinaries Binaries;
    auto OutIt = Binaries.begin();
    auto ProfileIt = ShaderProfiles.begin();
    int StageIt = 0;
    for (const auto &Stage : Sources) {
      auto &Out = OutIt++->first;
      const LPCWSTR Profile = *ProfileIt++;
      const auto HStage = HshStage(StageIt++);
      if (Stage.empty())
        continue;
      DxcText SourceBuf{Stage.data(), Stage.size(), 0};
      LPCWSTR DxArgs[] = {L"-T", Profile, DebugInfo ? L"-Zi" : L""};
      LPCWSTR VkArgs[] = {L"-T",
                          Profile,
                          DebugInfo ? L"-Zi" : L"",
                          L"-spirv",
                          L"-fspv-target-env=vulkan1.1",
                          L"-fvk-use-dx-layout",
                          L"-fvk-t-shift",
                          TShiftArg,
                          L"0",
                          L"-fvk-s-shift",
                          SShiftArg,
                          L"0"};
      LPCWSTR *Args = Target == HT_VULKAN_SPIRV ? VkArgs : DxArgs;
      UINT32 ArgCount = Target == HT_VULKAN_SPIRV
                            ? std::extent_v<decltype(VkArgs)>
                            : std::extent_v<decltype(DxArgs)>;
      CComPtr<IDxcResult> Result;
      HRESULT HResult = Compiler->Compile(&SourceBuf, Args, ArgCount, nullptr,
                                          HSH_IID_PPV_ARGS(&Result));
      if (!Result) {
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "no result from dxcompiler"));
        continue;
      }
      bool HasObj = Result->HasOutput(DXC_OUT_OBJECT);
      if (HasObj) {
        CComPtr<IDxcBlob> ObjBlob;
        Result->GetOutput(DXC_OUT_OBJECT, HSH_IID_PPV_ARGS(&ObjBlob), nullptr);
        if (auto Size = ObjBlob->GetBufferSize()) {
          Out.resize(Size);
          std::memcpy(&Out[0], ObjBlob->GetBufferPointer(), Size);
        } else {
          HasObj = false;
        }
      }
      if (Result->HasOutput(DXC_OUT_ERRORS)) {
        CComPtr<IDxcBlobUtf8> ErrBlob;
        Result->GetOutput(DXC_OUT_ERRORS, HSH_IID_PPV_ARGS(&ErrBlob), nullptr);
        if (ErrBlob->GetBufferSize()) {
          if (!HasObj)
            llvm::errs() << Stage << '\n';
          StringRef ErrStr((char *)ErrBlob->GetBufferPointer());
          Diags.Report(Diags.getCustomDiagID(HasObj ? DiagnosticsEngine::Warning
                                                    : DiagnosticsEngine::Error,
                                             "%0 problem from dxcompiler: %1"))
              << HshStageToString(HStage) << ErrStr.rtrim();
        }
      }
      if (HResult != ERROR_SUCCESS) {
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "%0 problem from dxcompiler: %1"))
            << HshStageToString(HStage) << HResult;
      }
    }
    return Binaries;
  }

public:
  explicit StagesCompilerDxc(HshTarget Target, bool DebugInfo,
                             StringRef ResourceDir, DiagnosticsEngine &Diags,
                             HshBuiltins &Builtins)
      : StagesCompilerBase(Target), Diags(Diags), DebugInfo(DebugInfo) {
    DxcLibrary::EnsureSharedInstance(ResourceDir, Diags);
    Compiler = DxcLibrary::SharedInstance->MakeCompiler();

    int res = std::swprintf(TShiftArg, 4, L"%u", Builtins.getMaxUniforms());
    assert(res >= 0);
    res = std::swprintf(SShiftArg, 4, L"%u",
                        Builtins.getMaxUniforms() + Builtins.getMaxImages());
    assert(res >= 0);
  }
};

class StagesCompilerDeko : public StagesCompilerBase {
  DiagnosticsEngine &Diags;

  static constexpr std::array<pipeline_stage, 6> ShaderProfiles{
      pipeline_stage_vertex, pipeline_stage_tess_ctrl, pipeline_stage_tess_eval,
      pipeline_stage_geometry, pipeline_stage_fragment};

  template <typename T> static constexpr T Align256(T x) {
    return (x + 0xFF) & ~0xFF;
  }
  static constexpr unsigned s_shaderStartOffset = 0x80 - sizeof(NvShaderHeader);

protected:
  StageBinaries doCompile(ArrayRef<std::string> Sources) const override {
    StageBinaries Binaries;
    auto OutIt = Binaries.begin();
    auto ProfileIt = ShaderProfiles.begin();
    int StageIt = 0;
    for (const auto &Stage : Sources) {
      auto &Out = OutIt++->first;
      const pipeline_stage Profile = *ProfileIt++;
      const auto HStage = HshStage(StageIt++);
      if (Stage.empty())
        continue;

      DekoCompiler Compiler(ShaderProfiles[Profile]);

      if (!Compiler.CompileGlsl(Stage.data())) {
        llvm::errs() << Stage << '\n';
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "%0 problem from deko"))
            << HshStageToString(HStage);
        continue;
      }

      auto Write = [&](void *data, size_t size) {
        std::memcpy(&*Out.insert(Out.end(), size, 0), data, size);
      };
      auto FileAlign256 = [&]() {
        Out.insert(Out.end(), Align256(Out.size()) - Out.size(), 0);
      };

      DkshHeader hdr = {};
      hdr.magic = DKSH_MAGIC;
      hdr.header_sz = sizeof(DkshHeader);
      hdr.control_sz = Align256(sizeof(DkshHeader) + sizeof(DkshProgramHeader));
      hdr.code_sz = Align256((Profile != pipeline_stage_compute ? 0x80 : 0x00) +
                             Compiler.m_codeSize) +
                    Align256(Compiler.m_dataSize);
      hdr.programs_off = sizeof(DkshHeader);
      hdr.num_programs = 1;

      Write(&hdr, sizeof(hdr));
      Write(&Compiler.m_dkph, sizeof(Compiler.m_dkph));
      FileAlign256();

      if (Profile != pipeline_stage_compute) {
        static const char s_padding[s_shaderStartOffset] =
            "lol nvidia why did you make us waste space here";
        Write((void *)s_padding, sizeof(s_padding));
        Write(&Compiler.m_nvsh, sizeof(Compiler.m_nvsh));
      }

      Write(Compiler.m_code, Compiler.m_codeSize);
      FileAlign256();

      if (Compiler.m_dataSize) {
        Write(Compiler.m_data, Compiler.m_dataSize);
        FileAlign256();
      }
    }
    return Binaries;
  }

public:
  explicit StagesCompilerDeko(HshTarget Target, DiagnosticsEngine &Diags)
      : StagesCompilerBase(Target), Diags(Diags) {}
};

std::unique_ptr<StagesCompilerBase>
MakeCompiler(HshTarget Target, bool DebugInfo, StringRef ResourceDir,
             DiagnosticsEngine &Diags, HshBuiltins &Builtins) {
  switch (Target) {
  default:
  case HT_GLSL:
  case HT_HLSL:
    return std::make_unique<StagesCompilerText>(Target);
  case HT_DXBC:
  case HT_DXIL:
  case HT_VULKAN_SPIRV:
    return std::make_unique<StagesCompilerDxc>(Target, DebugInfo, ResourceDir,
                                               Diags, Builtins);
  case HT_METAL:
  case HT_METAL_BIN_MAC:
  case HT_METAL_BIN_IOS:
  case HT_METAL_BIN_TVOS:
    return std::make_unique<StagesCompilerText>(Target);
  case HT_DEKO3D:
    return std::make_unique<StagesCompilerDeko>(Target, Diags);
  }
}

class StageStmtPartitioner {
  /*
   * Per-statement stage dependencies are centrally tracked here.
   * The first DependencyPass populates the stage bits of each statement in
   * post-order graph traversal. Statements that depend on the output of each
   * statement are also collected here for later lifting and pruning.
   */
  struct StmtDepInfo {
    struct StageBits StageBits;
    DenseSet<const Stmt *> Dependents;
    void setPrimaryStage(HshStage Stage) {
      if (Stage == HshNoStage)
        StageBits = 0;
      else
        StageBits = 1 << Stage;
    }
    HshStage getMaxStage() const {
      for (int i = HshMaxStage - 1; i >= HshVertexStage; --i) {
        if ((1 << i) & StageBits)
          return HshStage(i);
      }
      return HshVertexStage;
    }
    HshStage getMinStage() const {
      for (int i = HshVertexStage; i < HshMaxStage; ++i) {
        if ((1 << i) & StageBits)
          return HshStage(i);
      }
      return HshVertexStage;
    }
    HshStage getLeqStage(HshStage RefStage) const {
      for (int i = HshMaxStage - 1; i >= HshVertexStage; --i) {
        if (HshStage(i) > RefStage)
          continue;
        if ((1 << i) & StageBits)
          return HshStage(i);
      }
      return HshVertexStage;
    }
    bool hasStage(HshStage Stage) const { return (1 << Stage) & StageBits; }
  };
  using StmtMapType = DenseMap<const Stmt *, StmtDepInfo>;
  StmtMapType StmtMap;
  std::vector<const Stmt *> OrderedStmts;
  std::array<DenseSet<const VarDecl *>, HshMaxStage> UsedDecls;

  void UpdateDeclRefExprStages(const DeclStmt *DS, unsigned StageBits) {
    for (auto *Decl : DS->decls()) {
      if (auto *VD = dyn_cast<VarDecl>(Decl)) {
        for (auto &P : StmtMap) {
          if (auto *DRE = dyn_cast<DeclRefExpr>(P.first)) {
            if (DRE->getDecl() == VD)
              P.second.StageBits = StageBits;
          }
        }
      }
    }
  }

  /*
   * Assignments into declarations will mutate their stage dependency.
   * This is information is stored according to the parallel block scope
   * of nested flow control statements.
   */
  struct DeclDepInfo {
    HshStage Stage = HshNoStage;
    /* Mutator statements of decl so far, starting with original DeclStmt */
    DenseSet<const Stmt *> MutatorStmts;
  };
  using DeclMapType = DenseMap<const Decl *, DeclDepInfo>;
  DeclMapType FinalDeclMap;

  struct DependencyPass : ConstStmtVisitor<DependencyPass, HshStage> {
    StageStmtPartitioner &Partitioner;
    StagesBuilder &Builder;
    explicit DependencyPass(StageStmtPartitioner &Partitioner,
                            StagesBuilder &Builder)
        : Partitioner(Partitioner), Builder(Builder) {}

    /*
     * Statement stage dependencies include condition variables that decide
     * if their containing block is reached. The BlockScopeStack follows the
     * block scope of supported flow control statements. Parallel branches
     * (i.e. if-else, switch) are kept separate so they have fresh declaration
     * context from the point of flow control entry.
     */
    struct BlockScopeStack {
      struct StackEntry {
        const CFGBlock *OrigSucc;
        HshStage Stage;
        SmallVector<DeclMapType, 8> ParallelDeclMaps{DeclMapType{}};
        explicit StackEntry(const CFGBlock *OrigSucc, HshStage Stage)
            : OrigSucc(OrigSucc), Stage(Stage) {}
        void popMerge(const StackEntry &Other) {
          /*
           * Only declaration dependencies are merged since they represent
           * forward mutations. The stack entry stage follows the context of
           * condition expressions, therefore not merged.
           */
          auto &NewDeclMap = ParallelDeclMaps.back();
          for (const auto &OldDeclMap : Other.ParallelDeclMaps) {
            for (const auto &[OldDecl, OldMapEntry] : OldDeclMap) {
              auto &NewMapEntry = NewDeclMap[OldDecl];
              NewMapEntry.Stage =
                  std::max(NewMapEntry.Stage, OldMapEntry.Stage);
            }
          }
        }
      };
      SmallVector<StackEntry, 8> Stack;

      void push(const CFGBlock *OrigSucc, HshStage Stage) {
        Stack.emplace_back(OrigSucc, Stage);
      }

      void pop(const CFGBlock *ToSucc) {
        while (Stack.back().OrigSucc == ToSucc) {
          assert(Stack.size() >= 2 && "stack underflow");
          auto It = Stack.rbegin();
          auto &OldTop = *It++;
          auto &NewTop = *It;
          NewTop.popMerge(OldTop);
          Stack.pop_back();
        }
      }

      size_t size() const { return Stack.size(); }

      void newParallelBranch() { Stack.back().ParallelDeclMaps.emplace_back(); }

      DeclDepInfo &getDeclDepInfo(const Decl *D) {
        for (auto I = Stack.rbegin(), E = Stack.rend(); I != E; ++I) {
          auto &DeclMap = I->ParallelDeclMaps.back();
          auto Search = DeclMap.find(D);
          if (Search != DeclMap.end())
            return Search->second;
        }
        return Stack.back().ParallelDeclMaps.back()[D];
      }

      HshStage getContextStage() const { return Stack.back().Stage; }
    };
    BlockScopeStack ScopeStack;

    HshStage VisitDeclStmt(const DeclStmt *DS) {
      HshStage MaxStage = ScopeStack.getContextStage();
      for (const auto *D : DS->decls()) {
        if (auto *VD = dyn_cast<VarDecl>(D)) {
          if (const Expr *Init = VD->getInit()) {
            Partitioner.StmtMap[Init].Dependents.insert(DS);
            auto &DepInfo = ScopeStack.getDeclDepInfo(D);
            DepInfo.Stage = Visit(Init);
            DepInfo.MutatorStmts.insert(DS);
            MaxStage = std::max(MaxStage, DepInfo.Stage);
          }
        }
      }
      Partitioner.StmtMap[DS].setPrimaryStage(MaxStage);
      return MaxStage;
    }

    Expr *AssignMutator = nullptr;
    HshStage VisitDeclRefExpr(const DeclRefExpr *DRE) {
      auto &DepInfo = ScopeStack.getDeclDepInfo(DRE->getDecl());
      if (AssignMutator) {
        DepInfo.MutatorStmts.insert(AssignMutator);
        DepInfo.Stage = std::max(
            DepInfo.Stage, Partitioner.StmtMap[AssignMutator].getMaxStage());
      }
      for (auto *MS : DepInfo.MutatorStmts)
        Partitioner.StmtMap[MS].Dependents.insert(DRE);
      if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        DepInfo.Stage =
            std::max(DepInfo.Stage, Partitioner.Builtins.determineVarStage(VD));
      }
      Partitioner.StmtMap[DRE].setPrimaryStage(DepInfo.Stage);
      return DepInfo.Stage;
    }

    HshStage VisitMemberExpr(const MemberExpr *ME) {
      if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
        auto Stage = Partitioner.Builtins.determinePipelineFieldStage(FD);
        if (Stage != HshNoStage) {
          if (AssignMutator)
            Builder.setStageUsed(Stage);
          return Stage;
        }
      }
      return VisitStmt(ME);
    }

    HshStage VisitBinaryOperator(const BinaryOperator *BO) {
      if (BO->isAssignmentOp()) {
        HshStage MaxStage = ScopeStack.getContextStage();
        {
          auto *RHS = BO->getRHS();
          Partitioner.StmtMap[RHS].Dependents.insert(BO);
          MaxStage = std::max(MaxStage, Visit(RHS));
        }
        Partitioner.StmtMap[BO].setPrimaryStage(MaxStage);
        {
          SaveAndRestore<Expr *> SavedAssignMutator(AssignMutator, (Expr *)BO);
          auto *LHS = BO->getLHS();
          Partitioner.StmtMap[LHS].Dependents.insert(BO);
          MaxStage = std::max(MaxStage, Visit(LHS));
        }
        Partitioner.StmtMap[BO].setPrimaryStage(MaxStage);
        return MaxStage;
      }
      return VisitStmt(BO);
    }

    HshStage VisitCXXOperatorCallExpr(const CXXOperatorCallExpr *OC) {
      if (OC->isAssignmentOp()) {
        HshStage MaxStage = ScopeStack.getContextStage();
        {
          auto *RHS = OC->getArg(1);
          Partitioner.StmtMap[RHS].Dependents.insert(OC);
          MaxStage = std::max(MaxStage, Visit(RHS));
        }
        Partitioner.StmtMap[OC].setPrimaryStage(MaxStage);
        {
          SaveAndRestore<Expr *> SavedAssignMutator(AssignMutator, (Expr *)OC);
          auto *LHS = OC->getArg(0);
          Partitioner.StmtMap[LHS].Dependents.insert(OC);
          MaxStage = std::max(MaxStage, Visit(LHS));
        }
        Partitioner.StmtMap[OC].setPrimaryStage(MaxStage);
        return MaxStage;
      }
      return VisitStmt(OC);
    }

    HshStage VisitStmt(const Stmt *S) {
      HshStage MaxStage = ScopeStack.getContextStage();
      for (auto *Child : S->children()) {
        Partitioner.StmtMap[Child].Dependents.insert(S);
        MaxStage = std::max(MaxStage, Visit(Child));
      }
      Partitioner.StmtMap[S].setPrimaryStage(MaxStage);
      return MaxStage;
    }

    void run(AnalysisDeclContext &AD) {
      auto *CFG = AD.getCFG();
      size_t EstStmtCount = 0;
      for (auto *B : *CFG)
        EstStmtCount += B->size();
      Partitioner.StmtMap.reserve(EstStmtCount);
      Partitioner.OrderedStmts.reserve(EstStmtCount);
      ScopeStack.push(nullptr, HshNoStage);

      struct SuccStack {
        using PopReturn = PointerIntPair<const CFGBlock *, 2>;
        enum : unsigned { ePoppedExit = 1, ePoppedDo = 2 };
        struct Entry {
          CFGBlock::succ_const_range Succs;
          PopReturn Exit;
          Entry(CFGBlock::succ_const_range Succs, const CFGBlock *Exit,
                bool ExitRoot)
              : Succs(Succs), Exit(Exit, ExitRoot ? ePoppedExit : 0) {}
        };
        SmallVector<Entry, 8> Stack;
        SmallVector<const CFGBlock *, 8> PreStack;
        BitVector ClosedSet;
        explicit SuccStack(class CFG *CFG) : ClosedSet(CFG->getNumBlockIDs()) {
          push(CFG->getEntry().succs(), &CFG->getExit(), true);
        }
        void push(CFGBlock::succ_const_range Succs, const CFGBlock *Exit,
                  bool ExitRoot) {
          Stack.emplace_back(Succs, Exit, ExitRoot);
        }
        PopReturn _pop() {
          assert(!Stack.empty() && "popping empty stack");
          auto &StackTop = Stack.back();
          while (!StackTop.Succs.empty()) {
            const auto *Ret = StackTop.Succs.begin()->getReachableBlock();
            StackTop.Succs =
                make_range(StackTop.Succs.begin() + 1, StackTop.Succs.end());
            if (Ret)
              return PopReturn{Ret, 0};
          }
          auto Ret = StackTop.Exit;
          Stack.pop_back();
          return Ret;
        }
        PopReturn pop() {
          while (true) {
            auto Ret = _pop();
            if (Stack.empty())
              return Ret;
            const auto *Block = Ret.getPointer();
            bool PoppedExit = Ret.getInt() & ePoppedExit;
            const auto *Exit = Stack.back().Exit.getPointer();
            if (!PoppedExit && Block->getBlockID() <= Exit->getBlockID())
              continue;
            if (ClosedSet.test(Block->getBlockID()))
              continue;
            ClosedSet.set(Block->getBlockID());
            if (!PreStack.empty() &&
                PreStack.back()->getBlockID() == Block->getBlockID()) {
              PreStack.pop_back();
              Ret.setInt(Ret.getInt() | ePoppedDo);
            }
            if (const auto *DoLoopTarget = Block->getDoLoopTarget()) {
              PreStack.emplace_back(DoLoopTarget);
            }
            const auto *OrigSucc = Block->getTerminator().getOrigSucc();
            push(Block->succs(), OrigSucc ? OrigSucc : Exit, OrigSucc);
            return Ret;
          }
        }
      } SuccStack{CFG};

      bool NeedsParallelBranch = false;
      while (true) {
        auto Ret = SuccStack.pop();
        const auto *Block = Ret.getPointer();
        bool PoppedExit = Ret.getInt() & SuccStack::ePoppedExit;
        bool PoppedDo = Ret.getInt() & SuccStack::ePoppedDo;
        bool AtExit = Block->getBlockID() == CFG->getExit().getBlockID();

        /*
         * The AtExit check is required for the case where a function ends
         * with a DoStmt. The CFG inserts an empty loopback statement that
         * would trigger a second pop and underflow the scope stack.
         */
        if (PoppedExit && !(AtExit && ScopeStack.size() == 1)) {
          dumper() << ScopeStack.size();
          ScopeStack.pop(Block);
          NeedsParallelBranch = false;
          dumper() << " Popped Succ\n";
        }

        /*
         * Exit occurs here to ensure the scope stack is in the correct
         * exit state.
         */
        if (AtExit) {
          assert(ScopeStack.size() == 1 && "unbalanced scope stack");
          break;
        }

        /*
         * DoStmt body scopes are handled in a somewhat reversed fashion.
         * They are not opened by a condition terminator, but hsh still
         * wants to treat their contents as dependents of the while condition.
         */
        if (const auto *DoLoopTarget = Block->getDoLoopTarget()) {
          ScopeStack.push(DoLoopTarget,
                          Visit(DoLoopTarget->getTerminatorCondition()));
          NeedsParallelBranch = false;
          dumper() << ScopeStack.size() << " Pushed Do Succ\n";
        }

        /*
         * If no push-pop operation occurs between blocks, this is a parallel
         * block (i.e. else parallel with if)
         */
        if (NeedsParallelBranch) {
          ScopeStack.newParallelBranch();
          dumper() << "New parallel branch\n";
        }
        NeedsParallelBranch = true;

        /*
         * Scope stack is in the correct state for processing the block at
         * this point.
         */
        dumper() << "visit B" << Block->getBlockID() << '\n';
        for (const auto &Elem : *Block) {
          if (auto Stmt = Elem.getAs<CFGStmt>()) {
            dumper() << Stmt->getStmt() << " ";
            dumper() << Visit(Stmt->getStmt()) << "\n";
            Partitioner.OrderedStmts.push_back(Stmt->getStmt());
          }
        }

        if (PoppedDo) {
          dumper() << ScopeStack.size();
          ScopeStack.pop(Block);
          NeedsParallelBranch = false;
          dumper() << " Popped Do Succ\n";
        }
        if (const auto *OrigSucc = Block->getTerminator().getOrigSucc()) {
          ScopeStack.push(OrigSucc, Visit(Block->getTerminatorCondition()));
          NeedsParallelBranch = false;
          dumper() << ScopeStack.size() << " Pushed Succ\n";
        }
      }

      Partitioner.FinalDeclMap =
          std::move(ScopeStack.Stack[0].ParallelDeclMaps[0]);
    }
  };

  struct LiftPass : ConstStmtVisitor<LiftPass, bool> {
    StageStmtPartitioner &Partitioner;
    explicit LiftPass(StageStmtPartitioner &Partitioner)
        : Partitioner(Partitioner) {}

    static bool VisitStmt(const Stmt *) { return false; }

    static bool VisitDeclRefExpr(const DeclRefExpr *) { return true; }
    static bool VisitIntegerLiteral(const IntegerLiteral *) { return true; }
    static bool VisitFloatingLiteral(const FloatingLiteral *) { return true; }
    static bool VisitCXXBoolLiteralExpr(const CXXBoolLiteralExpr *) {
      return true;
    }

    bool VisitCXXConstructExpr(const CXXConstructExpr *CE) {
      for (auto *Arg : CE->arguments())
        if (!CanLift(Arg))
          return false;
      return true;
    }

    bool VisitDeclStmt(const DeclStmt *DS) {
      for (auto *Decl : DS->decls()) {
        if (auto *VD = dyn_cast<VarDecl>(Decl)) {
          auto Search = Partitioner.FinalDeclMap.find(Decl);
          if (Search != Partitioner.FinalDeclMap.end()) {
            for (auto *S : Search->second.MutatorStmts)
              if (S != DS && !CanLift(S))
                return false;
          }
          if (auto *Init = VD->getInit()) {
            if (!CanLift(Init))
              return false;
          }
        }
      }
      return true;
    }

    bool VisitCallExpr(const CallExpr *C) {
      if (auto *DeclRef =
              dyn_cast<DeclRefExpr>(C->getCallee()->IgnoreParenImpCasts())) {
        if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
          auto HBF = Partitioner.Builtins.identifyBuiltinFunction(FD);
          if (HBF != HBF_None && !HshBuiltins::isInterpolationDistributed(HBF))
            return true;
        }
      }
      return false;
    }

    bool CanLift(const Stmt *S) {
      if (auto *E = dyn_cast<Expr>(S))
        S = E->IgnoreParenImpCasts();
      return Visit(S);
    }

    void LiftToDependents(const Stmt *S) {
      dumper() << "Can lift " << S;
      if (!CanLift(S)) {
        dumper() << " No\n";
        return;
      }
      dumper() << " Yes\n";
      auto Search = Partitioner.StmtMap.find(S);
      if (Search != Partitioner.StmtMap.end()) {
        StageBits DepStageBits;
        for (const auto *Dep : Search->second.Dependents) {
          dumper() << "  Checking dep " << Dep;
          auto DepSearch = Partitioner.StmtMap.find(Dep);
          if (DepSearch != Partitioner.StmtMap.end()) {
            DepStageBits |= DepSearch->second.StageBits;
            dumper() << " " << DepSearch->second.StageBits;
          }
          dumper() << "\n";
        }
        if (Search->second.StageBits != DepStageBits) {
          dumper() << "  Lifted From " << Search->second.StageBits << " To "
                   << DepStageBits << "\n";
          Search->second.StageBits = DepStageBits;

          if (auto *DS = dyn_cast<DeclStmt>(S))
            Partitioner.UpdateDeclRefExprStages(DS, DepStageBits);
        }
      }
    }

    unsigned PassthroughDependents() {
      unsigned LiftCount = 0;
      for (auto &Stmt : Partitioner.StmtMap) {
        auto &StmtDeps = Stmt.second.Dependents;
        if (StmtDeps.empty())
          continue;
        DenseSet<const class Stmt *> NewStmtDeps;
        NewStmtDeps.reserve(StmtDeps.size());
        for (auto *Dep : StmtDeps) {
          if (isa<CastExpr>(Dep) || isa<DeclStmt>(Dep) ||
              isa<DeclRefExpr>(Dep) || isa<CXXConstructExpr>(Dep)) {
            auto Search = Partitioner.StmtMap.find(Dep);
            if (Search != Partitioner.StmtMap.end()) {
              auto &DREDeps = Search->second.Dependents;
              dumper() << "passing " << Dep->getStmtClassName()
                       << " dependent to " << Stmt.first->getStmtClassName()
                       << " " << Stmt.first << "\n";
              for (const auto *Deps : DREDeps)
                dumper() << "  " << Deps << "\n";
              NewStmtDeps.insert(DREDeps.begin(), DREDeps.end());
              ++LiftCount;
              continue;
            }
          }
          NewStmtDeps.insert(Dep);
        }
        StmtDeps = std::move(NewStmtDeps);
      }
      return LiftCount;
    }

    void run(AnalysisDeclContext &AD) {
      while (PassthroughDependents()) {
      }

      for (auto I = Partitioner.OrderedStmts.rbegin(),
                E = Partitioner.OrderedStmts.rend();
           I != E; ++I) {
        LiftToDependents(*I);
      }
    }
  };

  struct BlockDependencyPass : ConstStmtVisitor<BlockDependencyPass, unsigned> {
    StageStmtPartitioner &Partitioner;
    explicit BlockDependencyPass(StageStmtPartitioner &Partitioner)
        : Partitioner(Partitioner) {}

    unsigned VisitStmt(const Stmt *S) {
      if (auto *E = dyn_cast<Expr>(S))
        S = E->IgnoreParenImpCasts();
      auto Search = Partitioner.StmtMap.find(S);
      if (Search != Partitioner.StmtMap.end())
        return Search->second.StageBits;
      return 0;
    }

    unsigned VisitCompoundStmt(const CompoundStmt *CS) {
      StageBits NewStageBits;
      for (auto *Child : CS->body())
        NewStageBits |= Visit(Child);
      auto &DepInfo = Partitioner.StmtMap[CS];
      DepInfo.StageBits |= NewStageBits;
      return DepInfo.StageBits;
    }

    unsigned VisitIfStmt(const IfStmt *S) {
      StageBits NewStageBits;
      if (auto *Then = S->getThen())
        NewStageBits |= Visit(Then);
      if (auto *Else = S->getElse())
        NewStageBits |= Visit(Else);
      auto &DepInfo = Partitioner.StmtMap[S];
      DepInfo.StageBits |= NewStageBits;
      return DepInfo.StageBits;
    }

    template <typename T> unsigned VisitBody(const T *S) {
      StageBits NewStageBits;
      if (auto *Body = S->getBody())
        NewStageBits |= Visit(Body);
      auto &DepInfo = Partitioner.StmtMap[S];
      DepInfo.StageBits |= NewStageBits;
      return DepInfo.StageBits;
    }
    unsigned VisitForStmt(const ForStmt *S) { return VisitBody(S); }
    unsigned VisitWhileStmt(const WhileStmt *S) { return VisitBody(S); }
    unsigned VisitDoStmt(const DoStmt *S) { return VisitBody(S); }
    unsigned VisitSwitchStmt(const SwitchStmt *S) { return VisitBody(S); }

    template <typename T> unsigned VisitSubStmt(const T *S) {
      StageBits NewStageBits;
      if (auto *Sub = S->getSubStmt())
        NewStageBits |= Visit(Sub);
      auto &DepInfo = Partitioner.StmtMap[S];
      DepInfo.StageBits |= NewStageBits;
      return DepInfo.StageBits;
    }
    unsigned VisitCaseStmt(const CaseStmt *S) { return VisitSubStmt(S); }
    unsigned VisitDefaultStmt(const DefaultStmt *S) { return VisitSubStmt(S); }

    void run(AnalysisDeclContext &AD) { Visit(AD.getBody()); }
  };

  struct DeclUsagePass : StmtVisitor<DeclUsagePass, void, HshStage> {
    StageStmtPartitioner &Partitioner;
    StagesBuilder &Builder;
    explicit DeclUsagePass(StageStmtPartitioner &Partitioner,
                           StagesBuilder &Builder)
        : Partitioner(Partitioner), Builder(Builder) {}

    void InterStageReferenceExpr(Expr *E, HshStage ToStage) {
      if (E->isIntegerConstantExpr(Partitioner.Context))
        return;
      if (auto *DRE = dyn_cast<DeclRefExpr>(E))
        if (isa<EnumConstantDecl>(DRE->getDecl()))
          return;
      auto Search = Partitioner.StmtMap.find(E);
      if (Search != Partitioner.StmtMap.end()) {
        auto &DepInfo = Search->second;
        if (DepInfo.StageBits) {
          Visit(E, DepInfo.getMaxStage());
          return;
        }
      }
      Visit(E, ToStage);
    }

    void DoVisit(Stmt *S, HshStage Stage, bool ScopeBody = false) {
      dumper() << "Used Visiting for " << Stage << " " << S << "("
               << S->getStmtClassName() << ")\n";
      if (auto *E = dyn_cast<Expr>(S))
        S = E->IgnoreParenImpCasts();
      if (isa<DeclStmt>(S) || isa<CaseStmt>(S) || isa<DefaultStmt>(S)) {
        /* DeclStmts and switch components passthrough unconditionally */
        Visit(S, Stage);
        return;
      } else if (isa<IntegerLiteral>(S) || isa<FloatingLiteral>(S) ||
                 isa<CXXBoolLiteralExpr>(S) || isa<BreakStmt>(S) ||
                 isa<ContinueStmt>(S) || isa<CXXThisExpr>(S) ||
                 isa<CXXDefaultArgExpr>(S)) {
        /* Literals, flow control leaves, and this can go right where they
         * are used
         */
        return;
      } else if (ScopeBody) {
        /* "root" statement if immediate child of a scope body */
      } else if (auto *E = dyn_cast<Expr>(S)) {
        /* Trace expression tree and establish inter-stage references */
        InterStageReferenceExpr(E, Stage);
        return;
      }
      /* "Root" statements of bodies are conditionally emitted based on stage
       */
      auto Search = Partitioner.StmtMap.find(S);
      if (Search != Partitioner.StmtMap.end() && Search->second.hasStage(Stage))
        Visit(S, Stage);
      /* Prune this statement */
    }

    template <typename T> void DoVisitExprRange(T Range, HshStage Stage) {
      for (Expr *E : Range)
        DoVisit(E, Stage);
    }

    /* Begin ignores */
    void VisitValueStmt(ValueStmt *ValueStmt, HshStage Stage) {
      DoVisit(ValueStmt->getExprStmt(), Stage);
    }

    void VisitConstantExpr(ConstantExpr *CE, HshStage Stage) {
      DoVisit(CE->getSubExpr(), Stage);
    }

    void VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *MTE,
                                       HshStage Stage) {
      DoVisit(MTE->getSubExpr(), Stage);
    }

    void VisitSubstNonTypeTemplateParmExpr(SubstNonTypeTemplateParmExpr *NTTP,
                                           HshStage Stage) {
      DoVisit(NTTP->getReplacement(), Stage);
    }
    /* End ignores */

    static void VisitStmt(Stmt *S, HshStage) {}

    void VisitDeclStmt(DeclStmt *DeclStmt, HshStage Stage) {
      for (Decl *D : DeclStmt->decls())
        if (auto *VD = dyn_cast<VarDecl>(D))
          if (Expr *Init = VD->getInit())
            DoVisit(Init, Stage);
    }

    void VisitUnaryOperator(UnaryOperator *UnOp, HshStage Stage) {
      DoVisit(UnOp->getSubExpr(), Stage);
    }

    void VisitCallExpr(CallExpr *CallExpr, HshStage Stage) {
      if (auto *DeclRef = dyn_cast<DeclRefExpr>(
              CallExpr->getCallee()->IgnoreParenImpCasts())) {
        if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
          HshBuiltinFunction Func =
              Partitioner.Builtins.identifyBuiltinFunction(FD);
          bool CompatibleFD =
              FD->isConstexpr() &&
              Partitioner.Builtins.checkHshFunctionCompatibility(
                  Partitioner.Context, FD);
          if (CompatibleFD || Func != HBF_None)
            DoVisitExprRange(CallExpr->arguments(), Stage);
        }
      }
    }

    void VisitCXXMemberCallExpr(CXXMemberCallExpr *CallExpr, HshStage Stage) {
      CXXMethodDecl *MD = CallExpr->getMethodDecl();
      Expr *ObjArg =
          CallExpr->getImplicitObjectArgument()->IgnoreParenImpCasts();
      HshBuiltinCXXMethod Method =
          Partitioner.Builtins.identifyBuiltinMethod(MD);
      if (HshBuiltins::isSwizzleMethod(Method)) {
        DoVisit(ObjArg, Stage);
      }
      switch (Method) {
      case HBM_sample2d:
      case HBM_render_sample2d:
      case HBM_sample_bias2d:
        DoVisit(CallExpr->getArg(0), Stage);
        if (Method == HBM_sample_bias2d)
          DoVisit(CallExpr->getArg(1), Stage);
        break;
      default:
        break;
      }
    }

    void VisitCastExpr(CastExpr *CastExpr, HshStage Stage) {
      if (Partitioner.Builtins.identifyBuiltinType(CastExpr->getType()) ==
          HBT_None)
        return;
      DoVisit(CastExpr->getSubExpr(), Stage);
    }

    void VisitCXXConstructExpr(CXXConstructExpr *ConstructExpr,
                               HshStage Stage) {
      if (Partitioner.Builtins.identifyBuiltinType(ConstructExpr->getType()) ==
          HBT_None)
        return;
      DoVisitExprRange(ConstructExpr->arguments(), Stage);
    }

    void VisitCXXOperatorCallExpr(CXXOperatorCallExpr *CallExpr,
                                  HshStage Stage) {
      DoVisitExprRange(CallExpr->arguments(), Stage);
    }

    void VisitBinaryOperator(BinaryOperator *BinOp, HshStage Stage) {
      DoVisit(BinOp->getLHS(), Stage);
      DoVisit(BinOp->getRHS(), Stage);
    }

    bool InMemberExpr = false;

    void VisitDeclRefExpr(DeclRefExpr *DeclRef, HshStage Stage) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DeclRef->getDecl())) {
      } else if (auto *VD = dyn_cast<VarDecl>(DeclRef->getDecl())) {
        dumper() << VD << " Used in " << Stage << "\n";
        Partitioner.UsedDecls[Stage].insert(VD);
      }
    }

    void VisitInitListExpr(InitListExpr *InitList, HshStage Stage) {
      DoVisitExprRange(InitList->inits(), Stage);
    }

    void VisitMemberExpr(MemberExpr *MemberExpr, HshStage Stage) {
      if (!InMemberExpr &&
          !Partitioner.Builtins.checkHshFieldTypeCompatibility(
              Partitioner.Context, MemberExpr->getMemberDecl()))
        return;
      SaveAndRestore<bool> SavedInMemberExpr(InMemberExpr, true);
      DoVisit(MemberExpr->getBase(), Stage);
    }

    void VisitDoStmt(DoStmt *S, HshStage Stage) {
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *Body = S->getBody())
        DoVisit(Body, Stage, true);
    }

    void VisitForStmt(ForStmt *S, HshStage Stage) {
      if (auto *Init = S->getInit())
        DoVisit(Init, Stage);
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *Inc = S->getInc())
        DoVisit(Inc, Stage);
      if (auto *Body = S->getBody())
        DoVisit(Body, Stage, true);
    }

    void VisitIfStmt(IfStmt *S, HshStage Stage) {
      if (auto *Init = S->getInit())
        DoVisit(Init, Stage);
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *Then = S->getThen())
        DoVisit(Then, Stage, true);
      if (auto *Else = S->getElse())
        DoVisit(Else, Stage, true);
    }

    void VisitConditionalOperator(ConditionalOperator *S, HshStage Stage) {
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *True = S->getTrueExpr())
        DoVisit(True, Stage);
      if (auto *False = S->getFalseExpr())
        DoVisit(False, Stage);
    }

    void VisitCaseStmt(CaseStmt *S, HshStage Stage) {
      if (Stmt *St = S->getSubStmt())
        DoVisit(St, Stage, true);
    }

    void VisitDefaultStmt(DefaultStmt *S, HshStage Stage) {
      if (Stmt *St = S->getSubStmt())
        DoVisit(St, Stage, true);
    }

    void VisitSwitchStmt(SwitchStmt *S, HshStage Stage) {
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *Body = S->getBody())
        DoVisit(Body, Stage);
    }

    void VisitWhileStmt(WhileStmt *S, HshStage Stage) {
      if (auto *Cond = S->getCond())
        DoVisit(Cond, Stage);
      if (auto *Body = S->getBody())
        DoVisit(Body, Stage, true);
    }

    void VisitCompoundStmt(CompoundStmt *S, HshStage Stage) {
      for (auto *CS : S->body())
        DoVisit(CS, Stage, true);
    }

    void run(AnalysisDeclContext &AD) {
      for (int i = HshVertexStage; i < HshMaxStage; ++i) {
        auto Stage = HshStage(i);
        if (!Builder.isStageUsed(Stage))
          continue;
        if (auto *Body = dyn_cast<CompoundStmt>(AD.getBody())) {
          for (auto *Stmt : Body->body())
            DoVisit(Stmt, Stage, true);
        } else {
          DoVisit(AD.getBody(), Stage, true);
        }
      }
    }
  };

  struct BuildPass : StmtVisitor<BuildPass, Stmt *, HshStage> {
    StageStmtPartitioner &Partitioner;
    StagesBuilder &Builder;
    explicit BuildPass(StageStmtPartitioner &Partitioner,
                       StagesBuilder &Builder)
        : Partitioner(Partitioner), Builder(Builder) {}

    bool hasErrorOccurred() const {
      return Partitioner.Context.getDiagnostics().hasErrorOccurred();
    }

    Expr *InterStageReferenceExpr(Expr *E, HshStage ToStage) {
      llvm::APSInt ConstVal;
      if (E->isIntegerConstantExpr(ConstVal, Partitioner.Context)) {
        if (E->isKnownToHaveBooleanValue()) {
          return new (Partitioner.Context)
              CXXBoolLiteralExpr(!!ConstVal, Partitioner.Context.BoolTy, {});
        } else {
          return IntegerLiteral::Create(Partitioner.Context,
                                        ConstVal.extOrTrunc(32),
                                        Partitioner.Context.IntTy, {});
        }
      }
      if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
        if (isa<EnumConstantDecl>(DRE->getDecl()))
          return E;
      }
      auto Search = Partitioner.StmtMap.find(E);
      if (Search != Partitioner.StmtMap.end()) {
        auto &DepInfo = Search->second;
        if (!DepInfo.StageBits)
          return cast_or_null<Expr>(Visit(E, ToStage));
        E = cast_or_null<Expr>(Visit(E, DepInfo.getMaxStage()));
        if (!E)
          return nullptr;
        return Builder.createInterStageReferenceExpr(
            E, DepInfo.getLeqStage(ToStage), ToStage);
      }
      return cast_or_null<Expr>(Visit(E, ToStage));
    }

    Stmt *DoVisit(Stmt *S, HshStage Stage, bool ScopeBody = false) {
      dumper() << "Visiting for " << Stage << " " << S << "\n";
      /* For building purposes, ParenExpr passes through unconditionally */
      if (auto *P = dyn_cast<ParenExpr>(S))
        return Visit(P, Stage);
      if (auto *E = dyn_cast<Expr>(S))
        S = E->IgnoreParenImpCasts();
      if (isa<DeclStmt>(S) || isa<CaseStmt>(S) || isa<DefaultStmt>(S)) {
        /* DeclStmts and switch components passthrough unconditionally */
        return Visit(S, Stage);
      } else if (isa<IntegerLiteral>(S) || isa<FloatingLiteral>(S) ||
                 isa<CXXBoolLiteralExpr>(S) || isa<BreakStmt>(S) ||
                 isa<ContinueStmt>(S) || isa<CXXThisExpr>(S) ||
                 isa<CXXDefaultArgExpr>(S)) {
        /* Literals, flow control leaves, and this can go right where they are
         * used
         */
        return S;
      } else if (ScopeBody) {
        /* "root" statement if immediate child of a scope body */
      } else if (auto *E = dyn_cast<Expr>(S)) {
        /* Trace expression tree and establish inter-stage references */
        return InterStageReferenceExpr(E, Stage);
      }
      /* "Root" statements of bodies are conditionally emitted based on stage
       */
      auto Search = Partitioner.StmtMap.find(S);
      if (Search != Partitioner.StmtMap.end() && Search->second.hasStage(Stage))
        return Visit(S, Stage);
      /* Prune this statement */
      return nullptr;
    }

    using ExprRangeRet = SmallVector<Expr *, 4>;
    template <typename T>
    Optional<ExprRangeRet> DoVisitExprRange(T Range, HshStage Stage) {
      ExprRangeRet Res;
      for (Expr *E : Range) {
        Stmt *ExprStmt = DoVisit(E, Stage);
        if (!ExprStmt)
          return {};
        Res.push_back(cast<Expr>(ExprStmt));
      }
      return {Res};
    }

    /* Begin ignores */
    Stmt *VisitValueStmt(ValueStmt *ValueStmt, HshStage Stage) {
      return DoVisit(ValueStmt->getExprStmt(), Stage);
    }

    Stmt *VisitConstantExpr(ConstantExpr *CE, HshStage Stage) {
      return DoVisit(CE->getSubExpr(), Stage);
    }

    Stmt *VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *MTE,
                                        HshStage Stage) {
      return DoVisit(MTE->getSubExpr(), Stage);
    }

    Stmt *VisitSubstNonTypeTemplateParmExpr(SubstNonTypeTemplateParmExpr *NTTP,
                                            HshStage Stage) {
      return DoVisit(NTTP->getReplacement(), Stage);
    }
    /* End ignores */

    Stmt *VisitStmt(Stmt *S, HshStage) {
      ReportUnsupportedStmt(S, Partitioner.Context);
      return nullptr;
    }

    Stmt *VisitExpr(Expr *E, HshStage) {
      ReportUnsupportedStmt(E, Partitioner.Context);
      return nullptr;
    }

    Stmt *VisitParenExpr(ParenExpr *P, HshStage Stage) {
      Stmt *ExprStmt = DoVisit(P->getSubExpr(), Stage);
      if (!ExprStmt)
        return {};
      return new (Partitioner.Context) ParenExpr({}, {}, cast<Expr>(ExprStmt));
    }

    Stmt *VisitUnaryOperator(UnaryOperator *UnOp, HshStage Stage) {
      Stmt *ExprStmt = DoVisit(UnOp->getSubExpr(), Stage);
      if (!ExprStmt)
        return {};
      return new (Partitioner.Context)
          UnaryOperator(cast<Expr>(ExprStmt), UnOp->getOpcode(),
                        UnOp->getType(), VK_XValue, OK_Ordinary, {}, false);
    }

    Stmt *VisitDeclStmt(DeclStmt *DS, HshStage Stage) {
      SmallVector<Decl *, 4> NewDecls;
      for (auto *Decl : DS->decls()) {
        if (auto *VD = dyn_cast<VarDecl>(Decl)) {
          if (Partitioner.UsedDecls[Stage].find(VD) !=
              Partitioner.UsedDecls[Stage].end()) {
            auto *NewVD =
                VarDecl::Create(Partitioner.Context, VD->getDeclContext(), {},
                                {}, VD->getIdentifier(), VD->getType(),
                                VD->getTypeSourceInfo(), VD->getStorageClass());
            if (Expr *Init = VD->getInit()) {
              auto *InitStmt = DoVisit(Init, Stage);
              if (!InitStmt)
                return nullptr;
              NewVD->setInit(cast<Expr>(InitStmt));
            }
            NewDecls.push_back(NewVD);
          }
        } else {
          ReportUnsupportedTypeReference(DS, Partitioner.Context);
          return nullptr;
        }
      }
      if (!NewDecls.empty()) {
        return new (Partitioner.Context)
            DeclStmt(DeclGroupRef::Create(Partitioner.Context, NewDecls.data(),
                                          NewDecls.size()),
                     {}, {});
      }
      return nullptr;
    }

    static Stmt *VisitNullStmt(NullStmt *NS, HshStage) { return NS; }

    Stmt *VisitCallExpr(CallExpr *CallExpr, HshStage Stage) {
      if (auto *DeclRef = dyn_cast<DeclRefExpr>(
              CallExpr->getCallee()->IgnoreParenImpCasts())) {
        if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
          HshBuiltinFunction Func =
              Partitioner.Builtins.identifyBuiltinFunction(FD);
          bool CompatibleFD =
              FD->isConstexpr() &&
              Partitioner.Builtins.checkHshFunctionCompatibility(
                  Partitioner.Context, FD);
          if (CompatibleFD || Func != HBF_None) {
            auto Arguments = DoVisitExprRange(CallExpr->arguments(), Stage);
            if (!Arguments)
              return nullptr;
            if (Func == HBF_None) {
              Partitioner.Builtins.identifyBuiltinFunction(FD);
              Builder.registerFunctionDecl(FD, Stage);
            }
            return CallExpr::Create(Partitioner.Context, CallExpr->getCallee(),
                                    *Arguments, CallExpr->getType(), VK_XValue,
                                    {});
          }
        }
      }
      ReportUnsupportedFunctionCall(CallExpr, Partitioner.Context);
      return nullptr;
    }

    Stmt *VisitCXXMemberCallExpr(CXXMemberCallExpr *CallExpr, HshStage Stage) {
      CXXMethodDecl *MD = CallExpr->getMethodDecl();
      Expr *ObjArg =
          CallExpr->getImplicitObjectArgument()->IgnoreParenImpCasts();
      HshBuiltinCXXMethod Method =
          Partitioner.Builtins.identifyBuiltinMethod(MD);
      if (HshBuiltins::isSwizzleMethod(Method)) {
        auto *BaseStmt = DoVisit(ObjArg, Stage);
        auto *ParenBase =
            new (Partitioner.Context) ParenExpr({}, {}, cast<Expr>(BaseStmt));
        return MemberExpr::CreateImplicit(Partitioner.Context, ParenBase, false,
                                          MD, MD->getReturnType(), VK_XValue,
                                          OK_Ordinary);
      }
      switch (Method) {
      case HBM_sample2d:
      case HBM_render_sample2d:
      case HBM_sample_bias2d: {
        ParmVarDecl *PVD = nullptr;
        if (auto *TexRef = dyn_cast<DeclRefExpr>(ObjArg))
          PVD = dyn_cast<ParmVarDecl>(TexRef->getDecl());
        if (PVD)
          Builder.registerParmVarRef(PVD, Stage);
        else
          ReportBadTextureReference(CallExpr, Partitioner.Context);
        auto *UVStmt = DoVisit(CallExpr->getArg(0), Stage);
        if (!UVStmt)
          return nullptr;
        if (Method == HBM_sample_bias2d) {
          auto *BiasStmt = DoVisit(CallExpr->getArg(1), Stage);
          if (!BiasStmt)
            return nullptr;
          std::array<Expr *, 3> NewArgs{
              cast<Expr>(UVStmt), cast<Expr>(BiasStmt), CallExpr->getArg(2)};
          auto *NMCE = CXXMemberCallExpr::Create(
              Partitioner.Context, CallExpr->getCallee(), NewArgs,
              CallExpr->getType(), VK_XValue, {});
          Builder.registerSampleCall(Method, NMCE, Stage);
          return NMCE;
        } else {
          std::array<Expr *, 2> NewArgs{cast<Expr>(UVStmt),
                                        CallExpr->getArg(1)};
          auto *NMCE = CXXMemberCallExpr::Create(
              Partitioner.Context, CallExpr->getCallee(), NewArgs,
              CallExpr->getType(), VK_XValue, {});
          Builder.registerSampleCall(Method, NMCE, Stage);
          return NMCE;
        }
      }
      default:
        ReportUnsupportedFunctionCall(CallExpr, Partitioner.Context);
        break;
      }
      return nullptr;
    }

    Stmt *VisitCastExpr(CastExpr *CastExpr, HshStage Stage) {
      if (Partitioner.Builtins.identifyBuiltinType(CastExpr->getType()) ==
          HBT_None) {
        ReportUnsupportedTypeCast(CastExpr, Partitioner.Context);
        return nullptr;
      }
      return DoVisit(CastExpr->getSubExpr(), Stage);
    }

    Stmt *VisitCXXConstructExpr(CXXConstructExpr *ConstructExpr,
                                HshStage Stage) {
      if (Partitioner.Builtins.identifyBuiltinType(ConstructExpr->getType()) ==
          HBT_None) {
        ReportUnsupportedTypeConstruct(ConstructExpr, Partitioner.Context);
        return nullptr;
      }

      auto Arguments = DoVisitExprRange(ConstructExpr->arguments(), Stage);
      if (!Arguments)
        return nullptr;
      return CXXTemporaryObjectExpr::Create(
          Partitioner.Context, ConstructExpr->getConstructor(),
          ConstructExpr->getType(),
          Partitioner.Context.getTrivialTypeSourceInfo(
              ConstructExpr->getType()),
          *Arguments, {}, ConstructExpr->hadMultipleCandidates(),
          ConstructExpr->isListInitialization(),
          ConstructExpr->isStdInitListInitialization(),
          ConstructExpr->requiresZeroInitialization());
    }

    Stmt *VisitCXXOperatorCallExpr(CXXOperatorCallExpr *CallExpr,
                                   HshStage Stage) {
      auto Arguments = DoVisitExprRange(CallExpr->arguments(), Stage);
      if (!Arguments)
        return nullptr;
      if (CallExpr->isAssignmentOp()) {
        Expr *LHS = (*Arguments)[0];
        if (LHS->getType().isConstQualified())
          ReportConstAssignment(CallExpr, Partitioner.Context);
      }
      return CXXOperatorCallExpr::Create(
          Partitioner.Context, CallExpr->getOperator(), CallExpr->getCallee(),
          *Arguments, CallExpr->getType(), VK_XValue, {}, {});
    }

    Stmt *VisitBinaryOperator(BinaryOperator *BinOp, HshStage Stage) {
      auto *LStmt = DoVisit(BinOp->getLHS(), Stage);
      if (!LStmt)
        return nullptr;
      auto *RStmt = DoVisit(BinOp->getRHS(), Stage);
      if (!RStmt)
        return nullptr;
      if (BinOp->isAssignmentOp()) {
        if (cast<Expr>(LStmt)->getType().isConstQualified())
          ReportConstAssignment(BinOp, Partitioner.Context);
      }
      return BinaryOperator::Create(
          Partitioner.Context, cast<Expr>(LStmt), cast<Expr>(RStmt),
          BinOp->getOpcode(), BinOp->getType(), VK_XValue, OK_Ordinary, {}, {});
    }

    bool InMemberExpr = false;

    Stmt *VisitDeclRefExpr(DeclRefExpr *DeclRef, HshStage Stage) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DeclRef->getDecl())) {
        Builder.registerParmVarRef(PVD, Stage);
        return DeclRef;
      } else if (auto *VD = dyn_cast<VarDecl>(DeclRef->getDecl())) {
        if (!InMemberExpr &&
            !Partitioner.Builtins.checkHshFieldTypeCompatibility(
                Partitioner.Context, VD)) {
          ReportUnsupportedTypeReference(DeclRef, Partitioner.Context);
          return nullptr;
        }
        auto Search = Partitioner.StmtMap.find(DeclRef);
        if (Search != Partitioner.StmtMap.end()) {
          auto &DepInfo = Search->second;
          if (DepInfo.StageBits)
            return Builder.createInterStageReferenceExpr(
                DeclRef, DepInfo.getLeqStage(Stage), Stage);
        }
        return DeclRef;
      } else if (isa<EnumConstantDecl>(DeclRef->getDecl())) {
        return DeclRef;
      } else {
        ReportUnsupportedTypeReference(DeclRef, Partitioner.Context);
        return nullptr;
      }
    }

    Stmt *VisitInitListExpr(InitListExpr *InitList, HshStage Stage) {
      auto Exprs = DoVisitExprRange(InitList->inits(), Stage);
      if (!Exprs)
        return nullptr;
      return new (Partitioner.Context)
          InitListExpr(Partitioner.Context, {}, *Exprs, {});
    }

    Stmt *VisitMemberExpr(MemberExpr *MemberExpr, HshStage Stage) {
      if (!InMemberExpr &&
          !Partitioner.Builtins.checkHshFieldTypeCompatibility(
              Partitioner.Context, MemberExpr->getMemberDecl())) {
        ReportUnsupportedTypeReference(MemberExpr, Partitioner.Context);
        return nullptr;
      }
      SaveAndRestore<bool> SavedInMemberExpr(InMemberExpr, true);
      auto *BaseStmt = DoVisit(MemberExpr->getBase(), Stage);
      return MemberExpr::CreateImplicit(
          Partitioner.Context, cast<Expr>(BaseStmt), false,
          MemberExpr->getMemberDecl(), MemberExpr->getType(), VK_XValue,
          OK_Ordinary);
    }

    Stmt *StmtOrNull(Stmt *S) {
      if (S)
        return S;
      return new (Partitioner.Context) NullStmt({}, false);
    };

    Stmt *VisitDoStmt(DoStmt *S, HshStage Stage) {
      dumper() << "Do ";
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << ":\n";
      Stmt *NewBody = nullptr;
      if (auto *Body = S->getBody()) {
        NewBody = StmtOrNull(DoVisit(Body, Stage, true));
        dumper() << Body;
      }
      if (hasErrorOccurred())
        return nullptr;
      return new (Partitioner.Context) DoStmt(NewBody, NewCond, {}, {}, {});
    }

    Stmt *VisitForStmt(ForStmt *S, HshStage Stage) {
      dumper() << "For ";
      Stmt *NewInit = nullptr;
      if (auto *Init = S->getInit()) {
        NewInit = DoVisit(Init, Stage);
        dumper() << Init;
      }
      dumper() << "; ";
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << "; ";
      Expr *NewInc = nullptr;
      if (auto *Inc = S->getInc()) {
        NewInc = cast_or_null<Expr>(DoVisit(Inc, Stage));
        dumper() << Inc;
      }
      dumper() << ":\n";
      Stmt *NewBody = nullptr;
      if (auto *Body = S->getBody()) {
        NewBody = StmtOrNull(DoVisit(Body, Stage, true));
        dumper() << Body;
      }
      if (hasErrorOccurred())
        return nullptr;
      return new (Partitioner.Context)
          ForStmt(Partitioner.Context, NewInit, NewCond,
                  S->getConditionVariable(), NewInc, NewBody, {}, {}, {});
    }

    Stmt *VisitIfStmt(IfStmt *S, HshStage Stage) {
      dumper() << "If ";
      Stmt *NewInit = nullptr;
      if (auto *Init = S->getInit()) {
        NewInit = DoVisit(Init, Stage);
      }
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << ":\n";
      Stmt *NewThen = nullptr;
      if (auto *Then = S->getThen()) {
        NewThen = StmtOrNull(DoVisit(Then, Stage, true));
        dumper() << Then;
      }
      Stmt *NewElse = nullptr;
      if (auto *Else = S->getElse()) {
        NewElse = DoVisit(Else, Stage, true);
        dumper() << "Else:\n" << Else;
      }
      if (hasErrorOccurred())
        return nullptr;
      return IfStmt::Create(Partitioner.Context, {}, S->isConstexpr(), NewInit,
                            S->getConditionVariable(), NewCond, NewThen,
                            SourceLocation{}, NewElse);
    }

    Stmt *VisitConditionalOperator(ConditionalOperator *S, HshStage Stage) {
      if (auto *Cond = S->getCond()) {
        llvm::APSInt ConstVal;
        if (Cond->isIntegerConstantExpr(ConstVal, Partitioner.Context)) {
          dumper() << "Constant Cond ";
          dumper() << S->getCond();
          if (!!ConstVal) {
            if (auto *True = S->getTrueExpr()) {
              dumper() << "True:\n" << True;
              return cast_or_null<Expr>(DoVisit(True, Stage));
            }
          } else {
            if (auto *False = S->getFalseExpr()) {
              dumper() << "Else:\n" << False;
              return cast_or_null<Expr>(DoVisit(False, Stage));
            }
          }
          return nullptr;
        }
      }

      dumper() << "Cond ";
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << ":\n";
      Expr *NewTrue = nullptr;
      if (auto *True = S->getTrueExpr()) {
        NewTrue = cast_or_null<Expr>(DoVisit(True, Stage));
        dumper() << "True:\n" << True;
      }
      Expr *NewFalse = nullptr;
      if (auto *False = S->getFalseExpr()) {
        NewFalse = cast_or_null<Expr>(DoVisit(False, Stage));
        dumper() << "Else:\n" << False;
      }
      if (hasErrorOccurred())
        return nullptr;
      return new (Partitioner.Context)
          ConditionalOperator(NewCond, {}, NewTrue, {}, NewFalse, S->getType(),
                              VK_XValue, OK_Ordinary);
    }

    Stmt *VisitCaseStmt(CaseStmt *S, HshStage Stage) {
      dumper() << "case " << S->getLHS() << ":\n";
      Stmt *NewSubStmt = nullptr;
      if (Stmt *St = S->getSubStmt())
        NewSubStmt = DoVisit(St, Stage, true);
      if (hasErrorOccurred())
        return nullptr;
      auto *NewCase = CaseStmt::Create(Partitioner.Context, S->getLHS(),
                                       nullptr, {}, {}, {});
      NewCase->setSubStmt(NewSubStmt);
      dumper() << "\n";
      return NewCase;
    }

    Stmt *VisitDefaultStmt(DefaultStmt *S, HshStage Stage) {
      dumper() << "default:\n";
      Stmt *NewSubStmt = nullptr;
      if (Stmt *St = S->getSubStmt())
        NewSubStmt = DoVisit(St, Stage, true);
      if (hasErrorOccurred())
        return nullptr;
      auto *NewDefault =
          new (Partitioner.Context) DefaultStmt({}, {}, NewSubStmt);
      dumper() << "\n";
      return NewDefault;
    }

    static Stmt *VisitBreakStmt(BreakStmt *S, HshStage Stage) {
      dumper() << S;
      return S;
    }

    static Stmt *VisitContinueStmt(ContinueStmt *S, HshStage Stage) {
      dumper() << S;
      return S;
    }

    Stmt *VisitSwitchStmt(SwitchStmt *S, HshStage Stage) {
      dumper() << "Switch ";
      if (Stmt *Init = S->getInit()) {
        auto &Diags = Partitioner.Context.getDiagnostics();
        Diags.Report(
            Init->getBeginLoc(),
            Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                  "C++17 switch init statements not supported"))
            << Init->getSourceRange();
      }
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << ":\n";
      Stmt *NewBody = nullptr;
      if (auto *Body = S->getBody())
        NewBody = DoVisit(Body, Stage);
      if (hasErrorOccurred())
        return nullptr;
      auto *NewSwitch = SwitchStmt::Create(Partitioner.Context, nullptr,
                                           S->getConditionVariable(), NewCond);
      NewSwitch->setBody(NewBody);
      return NewSwitch;
    }

    Stmt *VisitWhileStmt(WhileStmt *S, HshStage Stage) {
      dumper() << "While ";
      Expr *NewCond = nullptr;
      if (auto *Cond = S->getCond()) {
        NewCond = cast_or_null<Expr>(DoVisit(Cond, Stage));
        dumper() << Cond;
      }
      dumper() << ":\n";
      Stmt *NewBody = nullptr;
      if (auto *Body = S->getBody())
        NewBody = DoVisit(Body, Stage, true);
      if (hasErrorOccurred())
        return nullptr;
      return WhileStmt::Create(Partitioner.Context, S->getConditionVariable(),
                               NewCond, NewBody, {});
    }

    Stmt *VisitCompoundStmt(CompoundStmt *S, HshStage Stage) {
      dumper() << "{\n";
      SmallVector<Stmt *, 16> Stmts;
      Stmts.reserve(S->size());
      for (auto *CS : S->body())
        if (auto *NewStmt = DoVisit(CS, Stage, true))
          Stmts.push_back(NewStmt);
      dumper() << "}\n";
      if (hasErrorOccurred())
        return nullptr;
      return CompoundStmt::Create(Partitioner.Context, Stmts, {}, {});
    }

    void run(AnalysisDeclContext &AD) {
      for (int i = HshVertexStage; i < HshMaxStage; ++i) {
        auto Stage = HshStage(i);
        if (!Builder.isStageUsed(Stage))
          continue;
        dumper() << "Statements for " << Stage << ":\n";
        if (auto *Body = dyn_cast<CompoundStmt>(AD.getBody())) {
          dumper() << "{\n";
          for (auto *Stmt : Body->body()) {
            if (auto *NewStmt = DoVisit(Stmt, Stage, true)) {
              dumper() << NewStmt << "\n";
              Builder.addStageStmt(NewStmt, Stage);
            }
            if (hasErrorOccurred())
              return;
          }
          dumper() << "}";
        } else {
          if (auto *NewStmt = DoVisit(AD.getBody(), Stage, true)) {
            dumper() << NewStmt << "\n";
            Builder.addStageStmt(NewStmt, Stage);
          }
          if (hasErrorOccurred())
            return;
        }
        dumper() << "\n";
      }
    }
  };

  ASTContext &Context;
  HshBuiltins &Builtins;

public:
  StageStmtPartitioner(ASTContext &Context, HshBuiltins &Builtins)
      : Context(Context), Builtins(Builtins) {}

  void run(AnalysisDeclContext &AD, StagesBuilder &Builder) {
    DependencyPass(*this, Builder).run(AD);
    Builder.updateUseStages();
    LiftPass(*this).run(AD);
    BlockDependencyPass(*this).run(AD);
    for (auto &P : StmtMap)
      dumper() << "Stmt " << P.first << " " << P.second.StageBits << "\n";
    DeclUsagePass(*this, Builder).run(AD);
    BuildPass(*this, Builder).run(AD);
  }
};

class GenerateConsumer : public ASTConsumer {
  HshBuiltins Builtins;
  CompilerInstance &CI;
  ASTContext &Context;
  HostPrintingPolicy HostPolicy;
  AnalysisDeclContextManager AnalysisMgr;
  Preprocessor &PP;
  ArrayRef<HshTarget> Targets;
  bool DebugInfo, SourceDump;
  SmallString<256> ProfilePath;
  std::unique_ptr<raw_pwrite_stream> OS;
  llvm::DenseSet<uint64_t> SeenHashes;
  llvm::DenseSet<uint64_t> SeenSamplerHashes;
  std::string AnonNSString;
  raw_string_ostream AnonOS{AnonNSString};
  std::string CoordinatorSpecString;
  raw_string_ostream CoordinatorSpecOS{CoordinatorSpecString};
  std::string HighCoordinatorSpecString;
  raw_string_ostream HighCoordinatorSpecOS{HighCoordinatorSpecString};
  Optional<std::pair<SourceLocation, std::string>> HeadInclude;
  struct HshExpansion {
    SourceRange Range;
    SmallString<32> Name;
    clang::Expr *Construct;
  };
  DenseMap<SourceLocation, HshExpansion> SeenHshExpansions;

  std::array<std::unique_ptr<StagesCompilerBase>, HT_MAX> Compilers;
  StagesCompilerBase &getCompiler(HshTarget Target) {
    auto &Compiler = Compilers[Target];
    if (!Compiler)
      Compiler =
          MakeCompiler(Target, DebugInfo, CI.getHeaderSearchOpts().ResourceDir,
                       Context.getDiagnostics(), Builtins);
    return *Compiler;
  }

  bool NeedsCoordinatorComma = false;
  void addCoordinatorType(QualType T) {
    if (NeedsCoordinatorComma)
      CoordinatorSpecOS << ",\n";
    else
      NeedsCoordinatorComma = true;
    T.print(CoordinatorSpecOS, HostPolicy);
  }

  bool NeedsHighCoordinatorComma = false;
  void addHighCoordinatorType(QualType T) {
    if (NeedsHighCoordinatorComma)
      HighCoordinatorSpecOS << ",\n";
    else
      NeedsHighCoordinatorComma = true;
    T.print(HighCoordinatorSpecOS, HostPolicy);
  }

  class LocationNamespaceSearch
      : public RecursiveASTVisitor<LocationNamespaceSearch> {
    ASTContext &Context;
    SourceLocation L;
    NamespaceDecl *InNS = nullptr;

  public:
    explicit LocationNamespaceSearch(ASTContext &Context) : Context(Context) {}

    bool VisitNamespaceDecl(NamespaceDecl *NS) {
      auto Range = NS->getSourceRange();
      if (Range.getBegin() < L && L < Range.getEnd()) {
        InNS = NS;
        return false;
      }
      return true;
    }

    NamespaceDecl *findNamespace(SourceLocation Location) {
      L = Location;
      InNS = nullptr;
      TraverseAST(Context);
      return InNS;
    }
  };

  class PipelineDerivativeSearch
      : public RecursiveASTVisitor<PipelineDerivativeSearch> {
    using FuncTp = llvm::unique_function<bool(NamedDecl *)>;
    ASTContext &Context;
    HshBuiltins &Builtins;
    FuncTp Func;

  public:
    explicit PipelineDerivativeSearch(ASTContext &Context,
                                      HshBuiltins &Builtins)
        : Context(Context), Builtins(Builtins) {}

    bool VisitCXXRecordDecl(CXXRecordDecl *Decl) {
      if (!Decl->isThisDeclarationADefinition() ||
          Decl->getDescribedClassTemplate() ||
          isa<ClassTemplateSpecializationDecl>(Decl))
        return true;
      if (Builtins.isDerivedFromPipelineDecl(Decl))
        return Func(Decl);
      return true;
    }

    bool VisitClassTemplateDecl(ClassTemplateDecl *Decl) {
      if (!Decl->isThisDeclarationADefinition())
        return true;
      if (Builtins.isDerivedFromPipelineDecl(Decl->getTemplatedDecl()))
        return Func(Decl);
      return true;
    }

    void search(llvm::unique_function<bool(NamedDecl *)> f) {
      Func = std::move(f);
      TraverseAST(Context);
    }
  };

public:
  explicit GenerateConsumer(CompilerInstance &CI, ArrayRef<HshTarget> Targets,
                            bool DebugInfo, bool SourceDump,
                            StringRef ProfilePath)
      : CI(CI), Context(CI.getASTContext()),
        HostPolicy(Context.getPrintingPolicy()), AnalysisMgr(Context),
        PP(CI.getPreprocessor()), Targets(Targets), DebugInfo(DebugInfo),
        SourceDump(SourceDump), ProfilePath(ProfilePath) {
    AnalysisMgr.getCFGBuildOptions().OmitLogicalBinaryOperators = true;
  }

  static std::string MakeHashString(uint64_t Hash) {
    std::string HashStr;
    raw_string_ostream HexOS(HashStr);
    llvm::write_hex(HexOS, Hash, HexPrintStyle::Upper, {16});
    return HashStr;
  }

  bool handlePipelineDerivative(NamedDecl *Decl) {
    auto &Diags = Context.getDiagnostics();
    auto *CD = dyn_cast<CXXRecordDecl>(Decl);
    auto *CTD = dyn_cast<ClassTemplateDecl>(Decl);
    assert((CTD || CD) && "Bad decl type");
    if (CTD)
      CD = CTD->getTemplatedDecl();

    auto *RedeclContext = Decl->getDeclContext()->getRedeclContext();
    if (!RedeclContext->isFileContext()) {
      Diags.Report(
          Decl->getBeginLoc(),
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "hsh::pipeline derivatives must be declared in "
                                "file or namespace scope"));
      return false;
    }

    SmallString<32> BindingName("hshbinding_"_ll);
    BindingName += Decl->getName();

    CXXRecordDecl *BindingCD = nullptr;
    ClassTemplateDecl *BindingCTD = nullptr;
    if (CTD) {
      BindingCTD = Builtins.makeBindingDerivative(Context, CI.getSema(), CTD,
                                                  BindingName);
      BindingCTD->print(AnonOS, HostPolicy);
      AnonOS << ";\n";
    } else {
      BindingCD = Builtins.makeBindingDerivative(Context, CD, BindingName);
    }

    if (Context.getDiagnostics().hasErrorOccurred())
      return false;

    auto ProcessSpecialization = [&](CXXRecordDecl *Specialization) {
      QualType T{Specialization->getTypeForDecl(), 0};

      // HshBuiltins::makeBindingDerivative sets this
      CXXRecordDecl *PipelineSource = Specialization->HshSourceRecord;
      assert(PipelineSource);

      // Validate constructor of this specialization source
      if (!PipelineSource->hasUserDeclaredConstructor()) {
        // Consider lack of user-defined constructor as an abstract pipeline
        return;
      }
      CXXConstructorDecl *PrevCtor = nullptr;
      bool MultiCtorReport = false;
      for (auto *Ctor : PipelineSource->ctors()) {
        if (Ctor->isCopyOrMoveConstructor())
          continue;
        if (!MultiCtorReport && PrevCtor) {
          Diags.Report(
              PrevCtor->getBeginLoc(),
              Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                    "hsh::pipeline derivatives may not "
                                    "have multiple constructors"));
          MultiCtorReport = true;
        }
        if (MultiCtorReport) {
          Diags.Report(Ctor->getBeginLoc(),
                       Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                             "additional constructor here"));
        }
        PrevCtor = Ctor;
      }
      if (MultiCtorReport)
        return;

      // Extract template arguments for constructing color attachments and
      // logical pipeline constants
      auto *PipelineSpec =
          Builtins.getDerivedPipelineSpecialization(PipelineSource);
      const auto &PipelineAttributes = Builtins.getPipelineAttributes();
      bool HasDualSource = false;
      auto ColorAttachmentArgs = PipelineAttributes.getColorAttachmentArgs(
          PipelineSpec, HasDualSource);
      auto PipelineArgs =
          PipelineAttributes.getPipelineArgs(Context, PipelineSpec);
      auto InShaderPipelineArgs =
          PipelineAttributes.getInShaderPipelineArgs(Context, PipelineSpec);
      bool IsDirectRender = PipelineAttributes.isDirectRender(PipelineSpec);

      if (PipelineAttributes.isHighPriority(PipelineSpec))
        addHighCoordinatorType(T);
      else
        addCoordinatorType(T);

      StagesBuilder Builder(Context, Builtins, Specialization,
                            ColorAttachmentArgs.size() +
                                (HasDualSource ? 1 : 0));

      CXXConstructorDecl *Constructor = nullptr;
      for (auto *Ctor : PipelineSource->ctors()) {
        if (Ctor->hasBody()) {
          Constructor = Ctor;
          break;
        }
      }
      assert(Constructor);

#if ENABLE_DUMP
      ASTDumper Dumper(llvm::errs(), nullptr, &Context.getSourceManager());
      Dumper.Visit(Constructor->getBody());
      llvm::errs() << '\n';
#endif

      auto *CallCtx = AnalysisMgr.getContext(Constructor);
#if ENABLE_DUMP
      CallCtx->dumpCFG(true);
#endif

      // Prepare for partitioning
      Builder.prepare(Constructor);

      // GO!
      StageStmtPartitioner(Context, Builtins).run(*CallCtx, Builder);
      if (Context.getDiagnostics().hasErrorOccurred())
        return;

      // Finalize expressions and add host to stage records
      Builder.finalizeResults(Constructor);

      // Dump sources directly for -source-dump
      if (SourceDump) {
        for (auto Target : Targets) {
          auto Policy =
              MakePrintingPolicy(Builtins, Target, InShaderPipelineArgs);
          auto Sources = Builder.printResults(*Policy);
          for (auto &S : Sources) {
            if (!S.empty())
              *OS << S;
          }
        }
        return;
      }

      // Set public access
      Specialization->addDecl(
          AccessSpecDecl::Create(Context, AS_public, Specialization, {}, {}));

      // Make _rebind
      {
        SmallVector<QualType, 16> RebindArgs;
        SmallVector<ParmVarDecl *, 16> RebindParms;
        SmallVector<QualType, 16> RebindCallArgs;
        SmallVector<Expr *, 32> RebindCallExprs;
        RebindArgs.reserve(Constructor->getNumParams() + 1);
        RebindParms.reserve(Constructor->getNumParams() + 1);
        RebindCallArgs.reserve(Constructor->getNumParams() +
                               Builder.getNumSamplerBindings());
        RebindCallExprs.reserve(RebindCallArgs.size());

        RebindArgs.push_back(TypeName::getFullyQualifiedType(
            Builtins.getBindingRefType(Context), Context));
        RebindParms.push_back(ParmVarDecl::Create(
            Context, Specialization, {}, {}, &Context.Idents.get("__binding"),
            RebindArgs.back(), {}, SC_None, nullptr));
        for (const auto *Param : Constructor->parameters()) {
          RebindArgs.push_back(
              TypeName::getFullyQualifiedType(Param->getType(), Context));
          RebindParms.push_back(ParmVarDecl::Create(
              Context, Specialization, {}, {}, Param->getIdentifier(),
              RebindArgs.back(), {}, SC_None, nullptr));
          RebindCallArgs.push_back(RebindArgs.back());
          RebindCallExprs.push_back(DeclRefExpr::Create(
              Context, {}, {}, RebindParms.back(), false, SourceLocation{},
              RebindCallArgs.back(), VK_XValue));
        }

        for (const auto &SamplerBinding : Builder.getSamplerBindings()) {
          RebindCallExprs.push_back(Builtins.makeSamplerBinding(
              Context, SamplerBinding.TextureDecl, SamplerBinding.RecordIdx,
              SamplerBinding.TextureIdx));
          RebindCallArgs.push_back(TypeName::getFullyQualifiedType(
              RebindCallExprs.back()->getType(), Context));
        }

        CanQualType CDType =
            Specialization->getTypeForDecl()->getCanonicalTypeUnqualified();

        CXXMethodDecl *RebindMethod = CXXMethodDecl::Create(
            Context, Specialization, {},
            {Context.DeclarationNames.getIdentifier(
                 &Context.Idents.get("_rebind")),
             {}},
            Context.getFunctionType(
                Context.VoidTy, RebindArgs,
                FunctionProtoType::ExtProtoInfo().withExceptionSpec(
                    EST_BasicNoexcept)),
            {}, SC_Static, false, CSK_unspecified, {});
        RebindMethod->setParams(RebindParms);
        RebindMethod->setAccess(AS_public);
        RebindMethod->setBody(CompoundStmt::Create(
            Context,
            Builtins.makeSpecializedRebindCall(Context, CDType, RebindParms[0],
                                               RebindCallArgs, RebindCallExprs),
            {}, {}));
        Specialization->addDecl(RebindMethod);
      }

      // Add per-target shader data vars
      for (auto Target : Targets) {
        Specialization->addDecl(Builtins.getConstDataVar(
            Context, Specialization, Target, Builder.getNumStages(),
            Builder.getNumBindings(), Builder.getNumAttributes(),
            Builder.getNumSamplers(), ColorAttachmentArgs.size()));
        Specialization->addDecl(Builtins.getDataVar(
            Context, Specialization, Target, Builder.getNumStages(),
            Builder.getNumSamplers()));
      }

      Specialization->completeDefinition();

      SmallVector<uint64_t, 8> SamplerHashes;
      DenseMap<HshTarget, StageBinaries> BinaryMap;
      BinaryMap.reserve(Targets.size());

      // Emit shader record while interjecting with data initializers
      HostPolicy.setVarInitPrint([&](VarDecl *D, raw_ostream &InitOS) {
        if (D->InitHshTarget == -1)
          return false;
        auto Target = HshTarget(D->InitHshTarget);

        auto Policy =
            MakePrintingPolicy(Builtins, Target, InShaderPipelineArgs);
        auto Sources = Builder.printResults(*Policy);
        auto &Compiler = getCompiler(Target);
        if (Context.getDiagnostics().hasErrorOccurred())
          return true;
        auto &Binaries =
            BinaryMap.insert(std::make_pair(Target, Compiler.compile(Sources)))
                .first->second;
        auto *SourceIt = Sources.begin();
        int StageIt = HshVertexStage;

        InitOS << "{\n    {\n";

        for (auto &[Data, Hash] : Binaries) {
          auto &Source = *SourceIt++;
          auto Stage = HshStage(StageIt++);
          if (Data.empty())
            continue;
          InitOS << "      hsh::detail::ShaderCode<";
          Builtins.printTargetEnumString(InitOS, HostPolicy, Target);
          InitOS << ">{";
          Builtins.printStageEnumString(InitOS, HostPolicy, Stage);
          std::string ControlHashStr;
          if (Target == HT_DEKO3D) {
            /* Additional shader parameters for deko */
            auto ControlHash =
                xxHash64(ArrayRef<uint8_t>{Data.data() + 24, 64});
            ControlHashStr = MakeHashString(ControlHash);
            InitOS << ", _dekoc_" << ControlHashStr;
          }
          auto HashStr = MakeHashString(Hash);
          InitOS << ", {_hshs_" << HashStr << ", 0x" << HashStr << "}},\n";
          if (!SeenHashes.insert(Hash).second)
            continue;
          {
            raw_comment_ostream CommentOut(*OS);
            CommentOut << "// " << HshStageToString(Stage)
                       << " source targeting " << HshTargetToString(Target)
                       << "\n\n";
            CommentOut << Source;
          }
          *OS << "inline ";
          if (Target == HT_VULKAN_SPIRV) {
            raw_carray32_ostream DataOut(*OS, "_hshs_"s + HashStr);
            DataOut.write((const uint32_t *)Data.data(), Data.size() / 4);
          } else if (Target == HT_DEKO3D) {
            /* Dksh headers are packed together by the linker */
            {
              raw_carray_ostream ControlOut(
                  *OS, "_dekoc_"s + ControlHashStr,
                  "__attribute__((section(\".deko.control\"), aligned(64)))");
              ControlOut.write((const char *)Data.data() + 24, 64);
            }
            {
              *OS << "\ninline ";
              raw_carray_ostream DataOut(
                  *OS, "_hshs_"s + HashStr,
                  "__attribute__((section(\".deko.shader\"), "
                  "aligned(DK_SHADER_CODE_ALIGNMENT)))");
              DataOut.write((const char *)Data.data() + 256, Data.size() - 256);
            }
          } else {
            raw_carray_ostream DataOut(*OS, "_hshs_"s + HashStr);
            DataOut.write((const char *)Data.data(), Data.size());
          }
          *OS << "\ninline hsh::detail::ShaderObject<";
          Builtins.printTargetEnumString(*OS, HostPolicy, Target);
          *OS << "> _hsho_" << HashStr << ";\n\n";
        }

        InitOS << "    },\n    {\n";

        for (const auto &Binding : Builder.getBindings()) {
          InitOS << "      hsh::detail::VertexBinding{" << Binding.Binding
                 << ", " << Binding.Stride << ", ";
          Builtins.printInputRateEnumString(InitOS, HostPolicy,
                                            Binding.InputRate);
          InitOS << "},\n";
        }

        InitOS << "    },\n    {\n";

        for (const auto &Attribute : Builder.getAttributes()) {
          InitOS << "      hsh::detail::VertexAttribute{" << Attribute.Binding
                 << ", ";
          Builtins.printFormatEnumString(InitOS, HostPolicy, Attribute.Format);
          InitOS << ", " << Attribute.Offset << "},\n";
        }

        InitOS << "    },\n    {\n";

        if (SamplerHashes.empty()) {
          SamplerHashes.reserve(Builder.getNumSamplers());
          for (const auto &Sampler : Builder.getSamplers()) {
            InitOS << "      hsh::sampler{";
            std::string SamplerParams;
            raw_string_ostream SPO(SamplerParams);
            unsigned FieldIdx = 0;
            CommaArgPrinter ArgPrinter(SPO);
            for (auto *Field : Builtins.getSamplerRecordDecl()->fields()) {
              ArgPrinter.addArg();
              const auto &FieldVal = Sampler.Config.getStructField(FieldIdx++);
              if (FieldVal.isInt()) {
                TemplateArgument(Context, FieldVal.getInt(), Field->getType())
                    .print(HostPolicy, SPO);
              } else if (FieldVal.isFloat()) {
                SmallVector<char, 16> Buffer;
                FieldVal.getFloat().toString(Buffer);
                SPO << Buffer;
                if (StringRef(Buffer.data(), Buffer.size())
                        .find_first_not_of("-0123456789") == StringRef::npos)
                  SPO << '.';
                SPO << 'F';
              }
            }
            SamplerHashes.push_back(xxHash64(SPO.str()));
            InitOS << SPO.str() << "},\n";
          }
        }

        InitOS << "    },\n    {\n";

        auto PrintArguments = [&](const auto &Args) {
          CommaArgPrinter ArgPrinter(InitOS);
          for (const auto &Arg : Args) {
            ArgPrinter.addArg();
            if (Arg.getKind() == TemplateArgument::Integral &&
                Builtins.identifyBuiltinType(Arg.getIntegralType()) ==
                    HBT_ColorComponentFlags) {
              InitOS << "hsh::ColorComponentFlags(";
              Builtins.printColorComponentFlagExpr(
                  InitOS, HostPolicy,
                  ColorComponentFlags(Arg.getAsIntegral().getZExtValue()));
              InitOS << ")";
            } else {
              Arg.print(HostPolicy, InitOS);
            }
          }
        };

        for (const auto &Attachment : ColorAttachmentArgs) {
          InitOS << "      hsh::detail::ColorAttachment{";
          PrintArguments(Attachment);
          InitOS << "},\n";
        }

        InitOS << "    },\n";

        InitOS << "    hsh::detail::PipelineInfo{";
        PrintArguments(PipelineArgs);
        InitOS << ", " << (IsDirectRender ? "true" : "false");
        InitOS << "}\n";

        InitOS << "  }";
        return true;
      });
      Specialization->print(AnonOS, HostPolicy);
      HostPolicy.resetVarInitPrint();
      AnonOS << ";\n";

      // Emit shader data
      for (auto Target : Targets) {
        AnonOS << "hsh::detail::ShaderData<";
        Builtins.printTargetEnumString(AnonOS, HostPolicy, Target);
        AnonOS << ", " << Builder.getNumStages() << ", "
               << Builder.getNumSamplers() << "> ";
        T.print(AnonOS, HostPolicy);
        AnonOS << "::data_";
        Builtins.printTargetEnumName(AnonOS, Target);
        AnonOS << "{\n  {\n";

        for (auto &[Data, Hash] : BinaryMap[Target]) {
          if (Data.empty())
            continue;
          AnonOS << "    _hsho_" << MakeHashString(Hash) << ",\n";
        }

        AnonOS << "  },\n  {\n";

        for (auto SamplerHash : SamplerHashes)
          AnonOS << "    _hshsamp_" << MakeHashString(SamplerHash) << ",\n";

        AnonOS << "  }\n};\n";
      }

      for (auto SamplerHash : SamplerHashes) {
        if (SeenSamplerHashes.find(SamplerHash) != SeenSamplerHashes.end())
          continue;
        SeenSamplerHashes.insert(SamplerHash);
        for (auto Target : Targets) {
          *OS << "inline hsh::detail::SamplerObject<";
          Builtins.printTargetEnumString(*OS, HostPolicy, Target);
          *OS << "> _hshsamp_" << MakeHashString(SamplerHash) << ";\n";
        }
      }
      *OS << "\n";
    };
    if (BindingCTD) {
      for (auto *Specialization : BindingCTD->specializations())
        ProcessSpecialization(Specialization);
    } else {
      ProcessSpecialization(BindingCD);
    }

    AnonOS << "\n\n";
    return true;
  }

  void handleHshExpansion(const HshExpansion &Expansion,
                          const DenseSet<NamedDecl *> &SeenDecls,
                          StringRef AbsProfFile) {
    auto &Diags = Context.getDiagnostics();
    auto *Decl = Expansion.Construct->getType()->getAsCXXRecordDecl();
    NamedDecl *UseDecl = Decl;
    if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(Decl))
      UseDecl =
          CTSD->getSpecializedTemplateOrPartial().get<ClassTemplateDecl *>();
    if (SeenDecls.find(UseDecl) == SeenDecls.end()) {
      auto ReportInvalid = [&](auto *Construct, const TypeSourceInfo *TSI) {
        Diags.Report(
            Construct->getBeginLoc(),
            Diags.getCustomDiagID(
                DiagnosticsEngine::Error,
                "binding constructor does not construct a valid pipeline"))
            << TSI->getTypeLoc().getSourceRange();
      };
      if (auto *CTOE = dyn_cast<CXXTemporaryObjectExpr>(Expansion.Construct))
        ReportInvalid(CTOE, CTOE->getTypeSourceInfo());
      else if (auto *CUCE =
                   dyn_cast<CXXUnresolvedConstructExpr>(Expansion.Construct))
        ReportInvalid(CUCE, CUCE->getTypeSourceInfo());
      else if (auto *CFCE =
                   dyn_cast<CXXFunctionalCastExpr>(Expansion.Construct))
        ReportInvalid(CFCE, CFCE->getTypeInfoAsWritten());
      return;
    }
    SmallString<32> BindingName("hshbinding_"_ll);
    BindingName += UseDecl->getName();

    /* Determine if construction expression has all constant template parms */
    CommaArgPrinter MacroPrinter(*OS);
    *OS << "struct " << Expansion.Name
        << " {\n"
           "template <typename... Res>\n"
           "static void Bind";
    SmallVector<NonConstExpr, 8> NonConstExprs;
    if (CheckConstexprTemplateSpecialization(
            Context, Expansion.Construct->getType(), &NonConstExprs)) {
      *OS << "(hsh::binding &__binding, Res... Resources) noexcept {\n"
             "  ::"
          << BindingName
          << "::_rebind(__binding, Resources...);\n"
             "}\n};\n"
             "#define "
          << Expansion.Name << "(...) _bind<::" << Expansion.Name << ">(";
    } else {
      *OS << "(hsh::binding &__binding, ";
      TraverseNonConstExprs(NonConstExprs, [&](NonTypeTemplateParmDecl *NTTP) {
        NTTP->getType().print(*OS, HostPolicy, NTTP->getName());
        *OS << ", ";
      });
      *OS << "Res... Resources) noexcept {\n"
             "#if HSH_PROFILE_MODE\n";
      if (!AbsProfFile.empty()) {
        *OS << "hsh::profile_context::instance\n"
               ".get(\""
            << AbsProfFile << "\",\n\"" << Expansion.Name << "\", \"";
        auto PrintFullyQualType = [&](TypeDecl *Decl) {
          if (auto *TD = dyn_cast<TagDecl>(Decl))
            *OS << TD->getKindName() << ' ';
          if (auto *ET = TypeName::getFullyQualifiedType(
                             QualType{Decl->getTypeForDecl(), 0}, Context)
                             ->getAsAdjusted<ElaboratedType>()) {
            if (auto *NNS = ET->getQualifier())
              NNS->print(*OS, HostPolicy);
          }
          Decl->printName(*OS);
        };
        PrintFullyQualType(Decl);
        *OS << "\")\n.add(";
        CommaArgPrinter ArgPrinter(*OS);
        unsigned PushCount = 0;
        TraverseNonConstExprs(
            NonConstExprs,
            [&](NonTypeTemplateParmDecl *NTTP) {
              ArgPrinter.addArg();
              if (auto *EnumTp = NTTP->getType()->getAs<EnumType>()) {
                *OS << "hsh::profiler::cast{\"";
                PrintFullyQualType(EnumTp->getDecl());
                *OS << "\", ";
                NTTP->printName(*OS);
                *OS << '}';
              } else {
                NTTP->printName(*OS);
              }
            },
            [&](ClassTemplateSpecializationDecl *Spec) {
              ++PushCount;
              if (PushCount == 1)
                return;
              ArgPrinter.addArg() << "hsh::profiler::push{\"";
              auto *CTD = Spec->getSpecializedTemplateOrPartial()
                              .get<ClassTemplateDecl *>();
              PrintFullyQualType(CTD->getTemplatedDecl());
              *OS << "\"}";
            },
            [&]() {
              --PushCount;
              if (PushCount == 0)
                return;
              ArgPrinter.addArg() << "hsh::profiler::pop{}";
            },
            [&](const APSInt &Int, QualType IntType) {
              ArgPrinter.addArg() << '\"';
              TemplateArgument(Context, Int, IntType).print(HostPolicy, *OS);
              *OS << '\"';
            });
        *OS << ");\n";
      }
      *OS << "#else\n"
             "#pragma GCC diagnostic push\n"
             "#pragma GCC diagnostic ignored \"-Wcovered-switch-default\"\n";

      struct SpecializationTree {
        static raw_ostream &indent(raw_ostream &OS, unsigned Indentation) {
          for (unsigned i = 0; i < Indentation; ++i)
            OS << "  ";
          return OS;
        }

        struct Node {
          DenseMap<APSInt, Node> Children;
          ClassTemplateSpecializationDecl *Leaf = nullptr;
          StringRef Name;
          bool IntCast = false;

          Node *getChild(const APSInt &Int) { return &Children[Int]; }

          void print(raw_ostream &OS, const PrintingPolicy &Policy,
                     StringRef BindingName, unsigned Indentation = 0) const {
            if (Leaf) {
              indent(OS, Indentation) << "::" << BindingName << '<';
              CommaArgPrinter ArgPrinter(OS);
              for (auto &Arg : Leaf->getTemplateArgs().asArray()) {
                Arg.print(Policy, ArgPrinter.addArg());
              }
              OS << ">::_rebind(__binding, Resources...); return;\n";
            } else if (!Name.empty()) {
              if (IntCast)
                indent(OS, Indentation) << "switch (int(" << Name << ")) {\n";
              else
                indent(OS, Indentation) << "switch (" << Name << ") {\n";
              for (auto &[Case, Child] : Children) {
                indent(OS, Indentation) << "case " << Case << ":\n";
                Child.print(OS, Policy, BindingName, Indentation + 1);
              }
              indent(OS, Indentation) << "default:\n";
              indent(OS, Indentation + 1)
                  << "assert(false && \"Unimplemented shader "
                     "specialization\"); return;\n";
              indent(OS, Indentation) << "}\n";
            } else {
              indent(OS, Indentation) << "assert(false && \"Unimplemented "
                                         "shader specialization\");\n";
            }
          }
        };
        Node Root;

        SpecializationTree(ASTContext &Context, ClassTemplateDecl *CTD,
                           ArrayRef<NonConstExpr> NonConstExprs) {
          for (auto *Specialization : CTD->specializations()) {
            if (!CheckConstexprTemplateSpecialization(Context, Specialization))
              continue;
            SpecializationTree::Node *SpecLeaf = &Root;
            TraverseNonConstExprs(NonConstExprs, Specialization,
                                  [&](NonTypeTemplateParmDecl *NTTP,
                                      const TemplateArgument &Arg) {
                                    if (NTTP->getType()->isBooleanType())
                                      SpecLeaf->IntCast = true;
                                    SpecLeaf->Name = NTTP->getName();
                                    SpecLeaf =
                                        SpecLeaf->getChild(Arg.getAsIntegral());
                                  });
            SpecLeaf->Leaf = Specialization;
          }
        }

        void print(raw_ostream &OS, const PrintingPolicy &Policy,
                   StringRef BindingName, unsigned Indentation = 0) {
          Root.print(OS, Policy, BindingName, Indentation);
        }
      } SpecTree{Context, cast<ClassTemplateDecl>(UseDecl), NonConstExprs};
      SpecTree.print(*OS, HostPolicy, BindingName);

      *OS << "#pragma GCC diagnostic pop\n"
             "#endif\n"
             "}\n};\n"
             "#define "
          << Expansion.Name << "(...) _bind<::" << Expansion.Name << ">(";
      for (auto &NCE : NonConstExprs) {
        if (NCE.getKind() != NonConstExpr::NonTypeParm)
          continue;
        NCE.getExpr()->printPretty(MacroPrinter.addArg(), nullptr, HostPolicy);
      }
    }
    auto PrintArgs = [&](auto *Construct) {
      for (auto *Arg : Construct->arguments()) {
        Arg->printPretty(MacroPrinter.addArg(), nullptr, HostPolicy);
      }
    };
    if (auto *CTOE = dyn_cast<CXXTemporaryObjectExpr>(Expansion.Construct))
      PrintArgs(CTOE);
    else if (auto *CUCE =
                 dyn_cast<CXXUnresolvedConstructExpr>(Expansion.Construct))
      PrintArgs(CUCE);
    else if (auto *CFCE =
                 dyn_cast<CXXFunctionalCastExpr>(Expansion.Construct)) {
      CFCE->getSubExpr()->printPretty(MacroPrinter.addArg(), nullptr,
                                      HostPolicy);
    }
    *OS << ")\n";
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    DiagnosticsEngine &Diags = Context.getDiagnostics();
    if (Diags.hasErrorOccurred())
      return;

    const unsigned IncludeDiagID =
        Diags.getCustomDiagID(DiagnosticsEngine::Error,
                              "hshhead include in must appear in global scope");
    if (!HeadInclude) {
      std::string Insertion;
      raw_string_ostream InsertionOS(Insertion);
      InsertionOS << "#include \""
                  << sys::path::filename(CI.getFrontendOpts().OutputFile)
                  << '\"';
      Diags.Report(IncludeDiagID) << FixItHint::CreateInsertion(
          Context.getSourceManager().getLocForStartOfFile(
              Context.getSourceManager().getMainFileID()),
          InsertionOS.str());
      return;
    }
    if (NamespaceDecl *NS = LocationNamespaceSearch(Context).findNamespace(
            HeadInclude->first)) {
      Diags.Report(HeadInclude->first, IncludeDiagID);
      Diags.Report(NS->getLocation(),
                   Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                         "included in namespace"));
      return;
    }

    Builtins.findBuiltinDecls(Context);
    if (Context.getDiagnostics().hasErrorOccurred())
      return;

    OS = CI.createDefaultOutputFile(false);

    if (!SourceDump) {
      SourceManager &SM = Context.getSourceManager();
      StringRef MainName = SM.getFileEntryForID(SM.getMainFileID())->getName();
      *OS << "/* Auto-generated hshhead for " << MainName
          << " */\n"
             "#include <hsh/hsh.h>\n\n";

      AnonOS << "namespace {\n\n";
      CoordinatorSpecOS << "hsh::detail::PipelineCoordinator<false,\n";
      HighCoordinatorSpecOS << "hsh::detail::PipelineCoordinator<true,\n";
    }

    /*
     * Process all hsh::pipeline derivatives
     */
    DenseSet<NamedDecl *> SeenDecls;
    PipelineDerivativeSearch(Context, Builtins)
        .search([this, &SeenDecls](NamedDecl *Decl) {
          if (handlePipelineDerivative(Decl))
            SeenDecls.insert(Decl);
          return true;
        });

    if (!SourceDump) {
      AnonOS << "}\n\n";

      *OS << "namespace hsh::detail {\n"
             "template struct "
             "ValidateBuiltTargets<std::integer_sequence<hsh::Target";
      for (auto Target : Targets) {
        *OS << ", ";
        Builtins.printTargetEnumString(*OS, HostPolicy, Target);
      }
      *OS << ">>;\n"
             "}\n\n";

      *OS << AnonOS.str();

      if (NeedsCoordinatorComma) {
        *OS << "template <> hsh::detail::PipelineCoordinatorNode<false,\n"
            << CoordinatorSpecOS.str() << ">::Impl>\n"
            << CoordinatorSpecOS.str() << ">::global{};\n\n";
      }

      if (NeedsHighCoordinatorComma) {
        *OS << "template <> hsh::detail::PipelineCoordinatorNode<true,\n"
            << HighCoordinatorSpecOS.str() << ">::Impl>\n"
            << HighCoordinatorSpecOS.str() << ">::global{};\n\n";
      }

      /*
       * Emit binding macro functions
       */
      *OS << "namespace {\n";
      for (auto &Exp : SeenHshExpansions)
        handleHshExpansion(Exp.second, SeenDecls, ProfilePath);
      *OS << "}\n";
    }

    // DxcLibrary::SharedInstance.reset();
  }

  void registerHshHeadInclude(SourceLocation HashLoc,
                              CharSourceRange FilenameRange,
                              StringRef RelativePath) {
    if (Context.getSourceManager().isWrittenInMainFile(HashLoc)) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      if (HeadInclude) {
        Diags.Report(HashLoc, Diags.getCustomDiagID(
                                  DiagnosticsEngine::Error,
                                  "multiple hshhead includes in one file"));
        Diags.Report(HeadInclude->first,
                     Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                           "previous include was here"));
        return;
      } else {
        if (!SourceDump) {
          auto ExpectedName =
              sys::path::filename(CI.getFrontendOpts().OutputFile);
          if (ExpectedName != RelativePath) {
            std::string Replacement;
            raw_string_ostream ReplacementOS(Replacement);
            ReplacementOS << '\"' << ExpectedName << '\"';
            Diags.Report(
                FilenameRange.getBegin(),
                Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                      "hshhead include must match the output "
                                      "filename"))
                << FixItHint::CreateReplacement(FilenameRange,
                                                ReplacementOS.str());
            return;
          }
        }
        HeadInclude.emplace(HashLoc, RelativePath);
      }
    }
  }

  void registerHshExpansion(SourceRange Range, StringRef Name,
                            ExprResult Expr) {
    if (Context.getSourceManager().isWrittenInMainFile(Range.getBegin())) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      for (auto &Exps : SeenHshExpansions) {
        if (Exps.second.Name == Name) {
          Diags.Report(
              Range.getBegin(),
              Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                    "hsh_* macro must be suffixed with "
                                    "identifier unique to the file"))
              << CharSourceRange(Range, false);
          Diags.Report(Exps.first, Diags.getCustomDiagID(
                                       DiagnosticsEngine::Note,
                                       "previous identifier usage is here"))
              << CharSourceRange(Exps.second.Range, false);
          return;
        }
      }
      if (!Expr.isUsable()) {
        Diags.Report(Range.getBegin(),
                     Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "hsh_* argument does not contain a "
                                           "usable construct expression"))
            << CharSourceRange(Range, false);
        return;
      }
      clang::Expr *Construct = dyn_cast<CXXTemporaryObjectExpr>(Expr.get());
      if (!Construct)
        Construct = dyn_cast<CXXUnresolvedConstructExpr>(Expr.get());
      if (!Construct)
        Construct = dyn_cast<CXXFunctionalCastExpr>(Expr.get());
      if (!Construct) {
        Diags.Report(Range.getBegin(),
                     Diags.getCustomDiagID(
                         DiagnosticsEngine::Error,
                         "expected construct expression as hsh_* argument"))
            << CharSourceRange(Range, false);
        return;
      }
      SeenHshExpansions[Range.getBegin()] = {Range, Name, Construct};
    }
  }

  class PPCallbacks : public clang::PPCallbacks {
    GenerateConsumer &Consumer;
    Preprocessor &PP;
    FileManager &FM;
    SourceManager &SM;

    static constexpr llvm::StringLiteral DummyInclude{"#include <hsh/hsh.h>\n"};

  public:
    explicit PPCallbacks(GenerateConsumer &Consumer, Preprocessor &PP,
                         FileManager &FM, SourceManager &SM)
        : Consumer(Consumer), PP(PP), FM(FM), SM(SM) {
      PP.setTokenWatcher([this](const clang::Token &T) { TokenWatcher(T); });
    }

    bool FileNotFound(StringRef FileName,
                      SmallVectorImpl<char> &RecoveryPath) override {
      if (FileName.endswith_lower(".hshhead"_ll)) {
        SmallString<1024> VirtualFilePath("./"_ll);
        VirtualFilePath += FileName;
        FM.getVirtualFile(VirtualFilePath, DummyInclude.size(),
                          std::time(nullptr));
        RecoveryPath.push_back('.');
        return true;
      }
      return false;
    }

    void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                            StringRef FileName, bool IsAngled,
                            CharSourceRange FilenameRange,
                            const FileEntry *File, StringRef SearchPath,
                            StringRef RelativePath,
                            const clang::Module *Imported,
                            SrcMgr::CharacteristicKind FileType) override {
      if (FileName.endswith_lower(".hshhead"_ll)) {
        assert(File && "File must exist at this point");
        SM.overrideFileContents(File,
                                llvm::MemoryBuffer::getMemBuffer(DummyInclude));
        Consumer.registerHshHeadInclude(HashLoc, FilenameRange, RelativePath);
      }
    }

    SmallVector<llvm::unique_function<void(const clang::Token &)>, 4>
        TokenWatcherStack;
    void TokenWatcher(const clang::Token &T) {
      if (!TokenWatcherStack.empty())
        TokenWatcherStack.back()(T);
    }

    void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                      SourceRange Range, const MacroArgs *Args) override {
      if (MacroNameTok.is(tok::identifier)) {
        StringRef Name = MacroNameTok.getIdentifierInfo()->getName();
        if (Name.startswith("hsh_"_ll) && Args) {
          /*
           * Defer a side-channel parsing action once the preprocessor has
           * finished lexing the expression containing the hsh_ macro expansion.
           */
          auto SrcTokens = Args->getUnexpArguments();
          TokenWatcherStack.emplace_back(
              [this,
               PassTokens =
                   std::vector<Token>(SrcTokens.begin(), SrcTokens.end()),
               PassRange = Range, PassName = Name](const clang::Token &T) {
                /*
                 * popping token watcher will delete storage of captured values;
                 * move them here before calling it.
                 */
                PPCallbacks *CB = this;
                auto Tokens(std::move(PassTokens));
                SourceRange Range = PassRange;
                StringRef Name = PassName;
                auto *P =
                    static_cast<Parser *>(CB->PP.getCodeCompletionHandler());
                TokenWatcherStack.pop_back();
                CB->PP.EnterTokenStream(Tokens, false, false);
                {
                  /*
                   * Parse the contents of the hsh_ macro, which should result
                   * in a CXXTemporaryObjectExpr. The parsing checks are relaxed
                   * to permit non ICE expressions within template parameters.
                   */
                  Parser::RevertingTentativeParsingAction PA(*P, true);
                  P->getActions().InHshBindingMacro = true;
                  P->ConsumeToken();
                  ExprResult Res;
                  if (P->getCurToken().isOneOf(tok::identifier,
                                               tok::coloncolon))
                    Res = P->ParseExpression();
                  P->getActions().InHshBindingMacro = false;
                  CB->Consumer.registerHshExpansion(Range, Name, Res);
                }
                CB->PP.RemoveTopOfLexerStack();
              });
        }
      }
    }

    bool DidPostdefines = false;
    bool RequestPostdefines() override {
      if (DidPostdefines)
        return false;

      if (!Consumer.ProfilePath.empty()) {
        if (auto FE = FM.getFile(Consumer.ProfilePath)) {
          auto FID = SM.createFileID(*FE, {}, SrcMgr::C_User);
          PP.EnterSourceFile(FID, nullptr, {});
          PP.getDiagnostics().setSuppressFID(FID);
          DidPostdefines = true;
          return true;
        }
      }

      return false;
    }
  };
};

} // namespace

namespace clang::hshgen {

std::unique_ptr<ASTConsumer>
GenerateAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  dumper().setPrintingPolicy(CI.getASTContext().getPrintingPolicy());
  auto Consumer = std::make_unique<GenerateConsumer>(CI, Targets, DebugInfo,
                                                     SourceDump, ProfilePath);
  CI.getPreprocessor().addPPCallbacks(
      std::make_unique<GenerateConsumer::PPCallbacks>(
          *Consumer, CI.getPreprocessor(), CI.getFileManager(),
          CI.getSourceManager()));
  return Consumer;
}

} // namespace clang::hshgen
