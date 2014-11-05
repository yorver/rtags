/* This file is part of RTags.

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#define RTAGS_SINGLE_THREAD
#include <rct/SHA256.h>
#include "ClangIndexerCXX.h"
#include "QueryMessage.h"
#include "VisitFileMessage.h"
#include "VisitFileResponseMessage.h"
#include <rct/Connection.h>
#include <rct/EventLoop.h>
#include "RTags.h"
#include "IndexerMessage.h"
#include "IndexData.h"
#include <unistd.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/CommentVisitor.h>
#include <clang/AST/DeclVisitor.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/TypeLocVisitor.h>
#include <clang/AST/TypeVisitor.h>
#include <clang/AST/RecursiveASTVisitor.h>

using namespace clang;

static inline Location createLocation(const SourceLocation& loc, ClangIndexerCXX* indexer, bool* blocked = 0)
{
    const SourceManager* sm = indexer->manager();
    const StringRef fn = sm->getFilename(loc);
    const unsigned int l = sm->getSpellingLineNumber(loc);
    const unsigned int c = sm->getSpellingColumnNumber(loc);
    if (fn.empty()) {
        // boo
        if (blocked)
            *blocked = false;
        return Location();
    }
    const Path path(fn.data(), fn.size());
    return indexer->createLocation(path, l, c, blocked);
}

static inline const Decl* getDeclForType(const Type* type)
{
    if (!type)
        return 0;
    if (const MemberPointerType* t = type->getAs<MemberPointerType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    } else if (const TypedefType* t = type->getAs<TypedefType>()) {
        return t->getDecl();
    } else if (const TagType* t = type->getAs<TagType>()) {
        return t->getDecl();
    } else if (const TemplateTypeParmType* t = type->getAs<TemplateTypeParmType>()) {
        return t->getDecl();
    } else if (const InjectedClassNameType* t = type->getAs<InjectedClassNameType>()) {
        return t->getDecl();
    } else if (const ObjCObjectType* t = type->getAs<ObjCObjectType>()) {
        // ### is this right?
        return t->getInterface();
    } else if (const ReferenceType* t = type->getAs<ReferenceType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    } else if (const ObjCObjectPointerType* t = type->getAs<ObjCObjectPointerType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    } else if (const PointerType* t = type->getAs<PointerType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    } else if (const BlockPointerType* t = type->getAs<BlockPointerType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    } else if (const DecayedType* t = type->getAs<DecayedType>()) {
        return getDeclForType(t->getPointeeType().split().Ty);
    }
}

static inline const Decl* getDeclForSpecifier(const NestedNameSpecifier* specifier)
{
    switch (specifier->getKind()) {
    case NestedNameSpecifier::Identifier:
    case NestedNameSpecifier::Namespace:
        return specifier->getAsNamespace();
    case NestedNameSpecifier::NamespaceAlias:
        return specifier->getAsNamespaceAlias();
    case NestedNameSpecifier::TypeSpec:
    case NestedNameSpecifier::TypeSpecWithTemplate:
        return getDeclForType(specifier->getAsType());
    case NestedNameSpecifier::Global:
        break;
    }
    return 0;
}

static void processNameSpecifier(const NestedNameSpecifier* specifier, ClangIndexerCXX* indexer)
{
    if (!specifier)
        return;
    do {
        switch (specifier->getKind()) {
        case NestedNameSpecifier::Identifier:
            if (IdentifierInfo* info = specifier->getAsIdentifier())
                error() << "  specifier identifier" << info->getNameStart();
            break;
        case NestedNameSpecifier::Namespace:
            if (NamespaceDecl* ns = specifier->getAsNamespace()) {
                error() << "  specifier namespace" << ns->getNameAsString();
            }
            break;
        case NestedNameSpecifier::NamespaceAlias:
            if (NamespaceAliasDecl* ns = specifier->getAsNamespaceAlias())
                error() << "  specifier alias" << ns->getNameAsString();
            break;
        case NestedNameSpecifier::TypeSpec:
        case NestedNameSpecifier::TypeSpecWithTemplate:
            if (const Type* t = specifier->getAsType())
                error() << "  specifier typespec" << QualType::getAsString(t->getCanonicalTypeUnqualified().split());
            break;
        case NestedNameSpecifier::Global:
            break;
        }
        specifier = specifier->getPrefix();
    } while (specifier);
}


class RTagsCompilationDatabase : public tooling::CompilationDatabase
{
public:
    RTagsCompilationDatabase(const Source &source, const String &unsaved = String())
        : mSource(source), mUnsaved(unsaved)
    {
        mCommand.Directory = source.pwd;
        const unsigned int commandLineFlags = (Source::FilterBlacklist
                                               | Source::IncludeDefines
                                               | Source::IncludeIncludepaths
                                               | Source::IncludeSourceFile
                                               | Source::IncludeLibClangOptions);
        const List<String> args = source.toCommandLine(commandLineFlags);
        mCommand.CommandLine.resize(args.size());
        int i = 0;

        for (const auto &str : args) {
            mCommand.CommandLine[i++] = str;
        }
        if (!unsaved.isEmpty()) {
            mCommand.MappedSources.push_back(std::make_pair(source.sourceFile(), unsaved));
        }
    }

    virtual std::vector<tooling::CompileCommand> getCompileCommands(llvm::StringRef file) const
    {
        Path path(file.data(), file.size());
        if (path.isSameFile(mSource.sourceFile()))
            return getAllCompileCommands();
        return std::vector<tooling::CompileCommand>();
    }

    virtual std::vector<std::string> getAllFiles() const
    {
        return std::vector<std::string>(1, mSource.sourceFile());
    }

    virtual std::vector<tooling::CompileCommand> getAllCompileCommands() const
    {
        return std::vector<tooling::CompileCommand>(1, mCommand);
    }
private:
    tooling::CompileCommand mCommand;
    const Source mSource;
    const String mUnsaved;
};

static inline const Decl* definition(const Decl* decl)
{
    if (decl) {
        if (isa<VarDecl>(decl)) {
            const VarDecl* vd = cast<VarDecl>(decl);
            const Decl* def = vd->getDefinition();
            if (def)
                decl = def;
        }
    }
    return decl;
}
class RTagsPPCallbacks : public PPCallbacks
{
public:
    RTagsPPCallbacks(ClangIndexerCXX *clang, const SourceManager& sm)
        : mClang(clang), mSourceManager(sm)
    {
    }

    virtual void InclusionDirective(SourceLocation HashLoc,
                                    const Token& IncludeTok,
                                    StringRef FileName,
                                    bool IsAngled,
                                    CharSourceRange FilenameRange,
                                    const FileEntry* File,
                                    StringRef SearchPath,
                                    StringRef RelativePath,
                                    const Module* Imported)
    {
        mClang->included(FileName.empty() ? Path() : Path(FileName.data(), FileName.size()), createLocation(HashLoc, mClang));
    }

private:
    ClangIndexerCXX* mClang;
    const SourceManager& mSourceManager;
};

class RTagsASTConsumer : public ASTConsumer,
                         public RecursiveASTVisitor<RTagsASTConsumer>//,
                         // public TypeLocVisitor<RTagsASTConsumer, bool>
{
    typedef RecursiveASTVisitor<RTagsASTConsumer> base;
public:
    RTagsASTConsumer(ClangIndexerCXX *clang)
        : mClang(clang), mAborted(false)
    {}

    bool VisitCXXRecordDecl(CXXRecordDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitEnumDecl(EnumDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitEnumConstantDecl(EnumConstantDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitFieldDecl(FieldDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitCXXMethodDecl(CXXMethodDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitCXXConstructorDecl(CXXConstructorDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitCXXDestructorDecl(CXXDestructorDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitParmVarDecl(ParmVarDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitQualifiedTypeLoc(QualifiedTypeLoc TL) {
        error() << "VisitQualifiedTypeLoc";
        return true;
    }

    bool VisitBuiltinTypeLoc(BuiltinTypeLoc TL) {
        error() << "VisitBuiltinTypeLoc";
        return true;
    }

    bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
        error() << "VisitTypedefTypeLoc";
        return true;
    }

    bool VisitUnresolvedUsingTypeLoc(UnresolvedUsingTypeLoc TL) {
        error() << "VisitUnresolvedUsingTypeLoc";
        return true;
    }

    bool VisitTagTypeLoc(TagTypeLoc TL) {
        const Location from = createLocation(TL.getNameLoc());
        const Location to = createLocation(TL.getDecl()->getLocation());
        mClang->insertReference(from, to);
        error() << "VisitTagTypeLoc";
        return true;
    }

    bool VisitTemplateTypeParmTypeLoc(TemplateTypeParmTypeLoc TL) {
        error() << "VisitTemplateTypeParmTypeLoc";
        return true;
    }

    bool VisitObjCInterfaceTypeLoc(ObjCInterfaceTypeLoc TL) {
        error() << "VisitObjCInterfaceTypeLoc";
        return true;
    }

    bool VisitObjCObjectTypeLoc(ObjCObjectTypeLoc TL) {
        error() << "VisitObjCObjectTypeLoc";
        return true;
    }

    bool VisitObjCObjectPointerTypeLoc(ObjCObjectPointerTypeLoc TL) {
        error() << "VisitObjCObjectPointerTypeLoc";
        return true;
    }

    bool VisitParenTypeLoc(ParenTypeLoc TL) {
        error() << "VisitParenTypeLoc";
        return true;
    }

    bool VisitPointerTypeLoc(PointerTypeLoc TL) {
        error() << "VisitPointerTypeLoc";
        return true;
    }

    bool VisitBlockPointerTypeLoc(BlockPointerTypeLoc TL) {
        error() << "VisitBlockPointerTypeLoc";
        return true;
    }

    bool VisitMemberPointerTypeLoc(MemberPointerTypeLoc TL) {
        error() << "VisitMemberPointerTypeLoc";
        return true;
    }

    bool VisitLValueReferenceTypeLoc(LValueReferenceTypeLoc TL) {
        error() << "VisitLValueReferenceTypeLoc";
        return true;
    }

    bool VisitRValueReferenceTypeLoc(RValueReferenceTypeLoc TL) {
        error() << "VisitRValueReferenceTypeLoc";
        return true;
    }

    bool VisitAttributedTypeLoc(AttributedTypeLoc TL) {
        error() << "VisitAttributedTypeLoc";
        return true;
    }

    bool VisitFunctionTypeLoc(FunctionTypeLoc TL) {
        error() << "VisitFunctionTypeLoc";
        return true;
    }

    bool VisitArrayTypeLoc(ArrayTypeLoc TL) {
        error() << "VisitArrayTypeLoc";
        return true;
    }

    bool VisitDecayedTypeLoc(DecayedTypeLoc TL) {
        error() << "VisitDecayedTypeLoc";
        return true;
    }

#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
    bool VisitAdjustedTypeLoc(AdjustedTypeLoc TL) {
        error() << "VisitAdjustedTypeLoc";
        return true;
    }
#endif

    bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc TL) {
        error() << "VisitTemplateSpecializationTypeLoc";
        return true;
    }

    bool VisitTypeOfExprTypeLoc(TypeOfExprTypeLoc TL) {
        error() << "VisitTypeOfExprTypeLoc";
        return true;
    }

    bool VisitTypeOfTypeLoc(TypeOfTypeLoc TL) {
        error() << "VisitTypeOfTypeLoc";
        return true;
    }

    bool VisitUnaryTransformTypeLoc(UnaryTransformTypeLoc TL) {
        error() << "VisitUnaryTransformTypeLoc";
        return true;
    }

    bool VisitDependentNameTypeLoc(DependentNameTypeLoc TL) {
        if (VisitNestedNameSpecifierLoc(TL.getQualifierLoc()))
            return true;
        error() << "VisitDependentNameTypeLoc";
        return true;
    }

    bool VisitTemplateArgumentLoc(const TemplateArgumentLoc &TAL) {
        switch (TAL.getArgument().getKind()) {
        case TemplateArgument::Template:
        case TemplateArgument::TemplateExpansion:
            if (VisitNestedNameSpecifierLoc(TAL.getTemplateQualifierLoc()))
                return true;
        }
    }

    bool VisitDependentTemplateSpecializationTypeLoc(DependentTemplateSpecializationTypeLoc TL) {
        if (TL.getQualifierLoc() &&
            VisitNestedNameSpecifierLoc(TL.getQualifierLoc()))
            return true;
        // Visit the template arguments.
        for (unsigned I = 0, N = TL.getNumArgs(); I != N; ++I)
            if (VisitTemplateArgumentLoc(TL.getArgLoc(I)))
                return true;
        error() << "VisitDependentTemplateSpecializationTypeLoc";
        return true;
    }

    bool VisitNestedNameSpecifierLoc(NestedNameSpecifierLoc TL)
    {
        if (!TL)
            return false;
        //processNameSpecifier(TL.getNestedNameSpecifier(), this);
        error() << "VisitNestedNameSpecifierLoc";
        do {
            if (const Decl* decl = getDeclForSpecifier(TL.getNestedNameSpecifier())) {
                const Location from = createLocation(TL.getLocalBeginLoc());
                const Location to = createLocation(decl->getLocation());
                mClang->insertReference(from, to);
            }
            TL = TL.getPrefix();
        } while (TL);
        return true;
    }

    bool VisitElaboratedTypeLoc(ElaboratedTypeLoc TL) {
        if (VisitNestedNameSpecifierLoc(TL.getQualifierLoc()))
            return true;
        error() << "VisitElaboratedTypeLoc";
        return true;
    }

    bool VisitPackExpansionTypeLoc(PackExpansionTypeLoc TL) {
        error() << "VisitPackExpansionTypeLoc";
        return true;
    }

    bool VisitDecltypeTypeLoc(DecltypeTypeLoc TL) {
        error() << "VisitDecltypeTypeLoc";
        return true;
    }

    bool VisitInjectedClassNameTypeLoc(InjectedClassNameTypeLoc TL) {
        error() << "VisitInjectedClassNameTypeLoc";
        return true;
    }

    bool VisitAtomicTypeLoc(AtomicTypeLoc TL) {
        error() << "VisitAtomicTypeLoc";
        return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr* Expr)
    {
        if (!Expr)
            return true;

        const Location loc = createLocation(Expr->getLocation());
        if (!loc.isValid())
            return false;

        const ValueDecl* value = Expr->getDecl();
        if (value) {
            mClang->insertDeclaration(value);
            const Location ref = createLocation(value->getLocation());
            assert(ref.isValid());
            mClang->insertReference(loc, ref);
        } else {
            const NamedDecl* named = Expr->getFoundDecl();
            assert(named);
            mClang->insertDeclaration(named);
            const Location ref = createLocation(named->getLocation());
            assert(ref.isValid());
            mClang->insertReference(loc, ref);
        }
        return true;
    }

    bool VisitCXXConstructExpr(CXXConstructExpr* Expr)
    {
        if (!Expr)
            return true;

        const Location loc = createLocation(Expr->getLocation());
        if (!loc.isValid())
            return false;
        const CXXConstructorDecl* ctor = Expr->getConstructor();
        assert(ctor);
        mClang->insertDeclaration(ctor);
        const Location ref = createLocation(ctor->getLocation());
        assert(ref.isValid());
        mClang->insertReference(loc, ref);

        return true;
    }

    bool VisitMemberExpr(MemberExpr* Expr)
    {
        if (!Expr)
            return true;

        const Location loc = createLocation(Expr->getMemberLoc());
        if (!loc.isValid())
            return false;
        const ValueDecl* value = Expr->getMemberDecl();
        assert(value);
        mClang->insertDeclaration(value);
        const Location ref = createLocation(value->getLocation());
        assert(ref.isValid());
        mClang->insertReference(loc, ref);

        return true;
    }

    bool VisitVarDecl(VarDecl* Decl)
    {
        // printf("[%s:%d]: bool VisitVarDecl(VarDecl* Decl)\n", __FILE__, __LINE__); fflush(stdout);
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl* Decl)
    {
        // printf("[%s:%d]: bool VisitNamespaceDecl(NamespaceDecl* Decl)\n", __FILE__, __LINE__); fflush(stdout);
        mClang->insertDeclaration(Decl);
        return true;
    }

    bool VisitNamespaceAliasDecl(NamespaceAliasDecl* Decl)
    {
        mClang->insertDeclaration(Decl);
        return true;
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        TranslationUnitDecl *D = Context.getTranslationUnitDecl();
        mSourceManager = &Context.getSourceManager();
        TraverseDecl(D);
    }

    bool shouldWalkTypesOfTypeLocs() const { return true; } // ### ???
    bool shouldVisitTemplateInstantiations() const { return true; }
    bool shouldVisitImplicitCode() const { return true; }

private:
    Location createLocation(const SourceLocation& loc, bool* blocked = 0)
    {
        return ::createLocation(loc, mClang, blocked);
    }

private:
    ClangIndexerCXX *mClang;
    bool mAborted;
    const SourceManager* mSourceManager;
};

class RTagsFrontendAction : public ASTFrontendAction
{
public:
    RTagsFrontendAction(ClangIndexerCXX *clang)
        : mClang(clang)
    {}
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &/*CI*/, StringRef /*InFile*/) override
    {
        return std::unique_ptr<ASTConsumer>(new RTagsASTConsumer(mClang));
    }
#else
    ASTConsumer *CreateASTConsumer(CompilerInstance &/*CI*/, StringRef /*InFile*/) override
    {
        return new RTagsASTConsumer(mClang);
    }
#endif
    void ExecuteAction() override
    {
        Preprocessor& pre = getCompilerInstance().getPreprocessor();
        const SourceManager& manager = pre.getSourceManager();
        mClang->setManager(manager);
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
        pre.addPPCallbacks(std::unique_ptr<RTagsPPCallbacks>(new RTagsPPCallbacks(mClang, manager)));
#else
        pre.addPPCallbacks(new RTagsPPCallbacks(mClang, manager));
#endif
        ASTFrontendAction::ExecuteAction();
    }
private:
    ClangIndexerCXX *mClang;
};

class RTagsFrontendActionFactory : public tooling::FrontendActionFactory
{
public:
    RTagsFrontendActionFactory(ClangIndexerCXX *clang)
        : mClang(clang)
    {}
    virtual FrontendAction *create()
    {
        return new RTagsFrontendAction(mClang);
    }
private:
    ClangIndexerCXX *mClang;
};

ClangIndexerCXX::ClangIndexerCXX()
    : mLoadedFromCache(false), mVisitFileResponseMessageFileId(0),
      mVisitFileResponseMessageVisit(0), mParseDuration(0), mVisitDuration(0),
      mBlocked(0), mAllowed(0), mIndexed(1), mVisitFileTimeout(0),
      mIndexerMessageTimeout(0), mFileIdsQueried(0), mLogFile(0)
{
    mConnection.newMessage().connect(std::bind(&ClangIndexerCXX::onMessage, this,
                                               std::placeholders::_1, std::placeholders::_2));
}

ClangIndexerCXX::~ClangIndexerCXX()
{
    if (mLogFile)
        fclose(mLogFile);
    // if (mClangUnit)
    //     clang_disposeTranslationUnit(mClangUnit);
    // if (mIndex)
    //     clang_disposeIndex(mIndex);
}

bool ClangIndexerCXX::parse()
{
    return true;
}

bool ClangIndexerCXX::visit()
{
    RTagsCompilationDatabase compilationDatabase(mSource);
    tooling::ClangTool tool(compilationDatabase, compilationDatabase.getAllFiles());
    RTagsFrontendActionFactory factory(this);
    tool.run(&factory);
    return true;
}

bool ClangIndexerCXX::diagnose()
{
    return true;
}

bool ClangIndexerCXX::exec(const String &data)
{
    Deserializer deserializer(data.constData() + 1, data.size() - 1);
    uint16_t protocolVersion;
    deserializer >> protocolVersion;
    if (protocolVersion != RTags::DatabaseVersion) {
        error("Wrong protocol %d vs %d", protocolVersion, RTags::DatabaseVersion);
        return false;
    }
    uint64_t id;
    String serverFile;
    uint32_t flags;
    uint32_t connectTimeout;
    int32_t niceValue;
    extern bool suspendOnSigSegv;
    Hash<uint32_t, Path> blockedFiles;

    deserializer >> id;
    deserializer >> serverFile;
    deserializer >> mASTCacheDir;
    deserializer >> mProject;
    deserializer >> mSource;
    deserializer >> mSourceFile;
    deserializer >> flags;
    deserializer >> mVisitFileTimeout;
    deserializer >> mIndexerMessageTimeout;
    deserializer >> connectTimeout;
    deserializer >> niceValue;
    deserializer >> suspendOnSigSegv;
    deserializer >> mUnsavedFiles;

#if 0
    while (true) {
        FILE *f = fopen((String("/tmp/stop_") + mSourceFile.fileName()).constData(), "r+");
        if (f) {
            fseek(f, 0, SEEK_END);
            fprintf(f, "Waiting ... %d\n", getpid());
            fclose(f);
            sleep(1);
        } else {
            break;
        }
    }
#endif

    uint32_t dirtySize;
    deserializer >> dirtySize;
    const uint64_t parseTime = Rct::currentTimeMs();

    while (dirtySize-- > 0) {
        Path dirty;
        deserializer >> dirty;
        if (!mUnsavedFiles.contains(dirty)) {
            mUnsavedFiles[dirty] = dirty.readAll();
        }
    }

    deserializer >> blockedFiles;

    if (niceValue != INT_MIN) {
        errno = 0;
        if (nice(niceValue) == -1) {
            error() << "Failed to nice rp" << strerror(errno);
        }
    }

    if (mSourceFile.isEmpty()) {
        error("No sourcefile");
        return false;
    }
    if (!mSource.fileId) {
        error("Bad fileId");
        return false;
    }

    if (mProject.isEmpty()) {
        error("No project");
        return false;
    }

    Location::init(blockedFiles);
    Location::set(mSourceFile, mSource.fileId);
    if (!mConnection.connectUnix(serverFile, connectTimeout)) {
        error("Failed to connect to rdm on %s (%dms timeout)", serverFile.constData(), connectTimeout);
        return false;
    }
    // mLogFile = fopen(String::format("/tmp/%s", mSourceFile.fileName()).constData(), "w");
    mData.reset(new IndexData(flags));
    mData->parseTime = parseTime;
    mData->key = mSource.key();
    mData->id = id;

    assert(mConnection.isConnected());
    mData->visited[mSource.fileId] = true;
    parse() && visit() && diagnose();
    mData->message = mSourceFile.toTilde();
    // if (!mClangUnit)
    //     mData->message += " error";
    mData->message += String::format<16>(" in %lldms%s ", mTimer.elapsed(), (mLoadedFromCache ? " (cached)." : "."));
#warning fix me
    if (true /*mClangUnit*/) {
        const char *format = "(%d syms, %d symNames, %d deps, %d of %d files, cursors: %d of %d, %d queried) (%d/%dms)";
        mData->message += String::format<128>(format, mData->symbols.size(), mData->symbolNames.size(),
                                              mData->dependencies.size(), mIndexed, mData->visited.size(), mAllowed,
                                              mAllowed + mBlocked, mFileIdsQueried,
                                              mParseDuration, mVisitDuration);
    } else if (mData->dependencies.size()) {
        mData->message += String::format<16>("(%d deps)", mData->dependencies.size());
    }
    if (mData->flags & IndexerJob::Dirty)
        mData->message += " (dirty)";
    const IndexerMessage msg(mProject, mData);
    ++mFileIdsQueried;

    StopWatch sw;
    if (!mConnection.send(msg)) {
        error() << "Couldn't send IndexerMessage" << mSourceFile;
        return false;
    }
    mConnection.finished().connect(std::bind(&EventLoop::quit, EventLoop::eventLoop()));
    if (EventLoop::eventLoop()->exec(mIndexerMessageTimeout) == EventLoop::Timeout) {
        error() << "Timed out sending IndexerMessage" << mSourceFile;
        return false;
    }
    if (getenv("RDM_DEBUG_INDEXERMESSAGE"))
        error() << "Send took" << sw.elapsed() << "for" << mSourceFile;

#warning fix me
    if (!mLoadedFromCache && true/*mClangUnit*/ && !mASTCacheDir.isEmpty() && mUnsavedFiles.isEmpty()
        && Path::mkdir(mASTCacheDir, Path::Recursive)) {
        Path outFile = mSourceFile;
        RTags::encodePath(outFile);
        outFile.prepend(mASTCacheDir);
        warning() << "About to save" << outFile << mSourceFile;
#warning fix me
        // if (clang_saveTranslationUnit(mClangUnit, outFile.constData(), clang_defaultSaveOptions(mClangUnit)) != CXSaveError_None) {
        //     error() << "Failed to save translation unit to" << outFile;
        //     return true;
        // }
        FILE *manifest = fopen((outFile + ".manifest").constData(), "w");
        if (!manifest) {
            error() << "Failed to write manifest" << errno << strerror(errno);
            Path::rm(outFile);
            return true;
        }

        Serializer serializer(manifest);
        const Set<uint32_t> &deps = mData->dependencies[mSource.fileId];
        assert(deps.contains(mSource.fileId));
        serializer << static_cast<uint8_t>(RTags::ASTManifestVersion) << mSource << deps.size();
        auto serialize = [this, &serializer](uint32_t file) {
            const Path path = Location::path(file);
            const String sha = shaFile(path);
            if (sha.isEmpty())
                return false;
            serializer << path << path.lastModified() << sha;
            return true;
        };

        bool ok = serialize(mSource.fileId);
        if (ok) {
            for (uint32_t dep : deps) {
                if (!serialize(dep)) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            fclose(manifest);
        }
    }

    return true;
}

void ClangIndexerCXX::included(const Path& file, const Location& from)
{
    const Location refLoc = createLocation(file, 1, 1);
    if (!refLoc.isNull()) {
        {
            String include = "#include ";
            const Path path = refLoc.path();
            assert(mSource.fileId);
            mData->dependencies[refLoc.fileId()].insert(mSource.fileId);
            mData->symbolNames[(include + path)].insert(from);
            mData->symbolNames[(include + path.fileName())].insert(from);
        }
        std::shared_ptr<CursorInfo> &info = mData->symbols[from];
        if (!info)
            info = std::make_shared<CursorInfo>();
        info->targets.insert(refLoc);
#warning fixme
        //info->kind = cursor.kind;
        info->definition = false;
        info->symbolName = "#include " + String(file.fileName());
        info->symbolLength = info->symbolName.size() + 2;
        // this fails for things like:
        // # include    <foobar.h>
    }
}

void ClangIndexerCXX::onMessage(const std::shared_ptr<Message> &msg, Connection */*conn*/)
{
    assert(msg->messageId() == VisitFileResponseMessage::MessageId);
    const std::shared_ptr<VisitFileResponseMessage> vm = std::static_pointer_cast<VisitFileResponseMessage>(msg);
    mVisitFileResponseMessageVisit = vm->visit();
    mVisitFileResponseMessageFileId = vm->fileId();
    assert(EventLoop::eventLoop());
    EventLoop::eventLoop()->quit();
}

String ClangIndexerCXX::insertNamePermutations(const NamedDecl* decl, const Location& location)
{
    const DeclContext *Ctx = decl->getDeclContext();

    typedef SmallVector<const DeclContext *, 8> ContextsTy;
    ContextsTy Contexts;

    // Collect contexts.
    while (Ctx && isa<NamedDecl>(Ctx)) {
        Contexts.push_back(Ctx);
        Ctx = Ctx->getParent();
    }

#warning need to handle template arguments and function arguments here

    const String ret = decl->getNameAsString();

    String name = ret;
    mData->symbolNames[name].insert(location);

    for (ContextsTy::iterator I = Contexts.begin(), E = Contexts.end();
         I != E; ++I) {
        name.prepend(cast<NamedDecl>(*I)->getNameAsString() + "::");
        mData->symbolNames[name].insert(location);
    }

    return ret;
}

void ClangIndexerCXX::insertDeclaration(const NamedDecl* decl)
{
    if (!decl)
        return;

    const Location loc = createLocation(decl->getLocation());
    if (!loc.isValid())
        return;

    std::shared_ptr<CursorInfo> &info = mData->symbols[loc];
    if (!info)
        info = std::make_shared<CursorInfo>();

    if (!info->symbolLength) {
        info->symbolName = insertNamePermutations(decl, loc); //decl->getNameAsString();
        info->symbolLength = info->symbolName.size();
        // traverse the decl, add names
        if (isa<TypeDecl>(decl)) {
            if (const Type* t = cast<TypeDecl>(decl)->getTypeForDecl()) {
                // error() << "declaration type (TypeDecl)" << QualType::getAsString(t->getCanonicalTypeUnqualified().split());
            }
        }
        // if (isa<TagDecl>(decl)) {
        //     const TagDecl* tag = cast<TagDecl>(decl);
        //     error() << "  tag qualifier" << tag->getQualifier();
        //     if (NestedNameSpecifier* specifier = tag->getQualifier())
        //         processNameSpecifier(specifier);
        // }
    }
}

void ClangIndexerCXX::insertReference(const Location& from, const Location& to)
{
    error() << "reference from" << from << "to" << to;

    std::shared_ptr<CursorInfo> &refInfo = mData->symbols[to];
    if (!refInfo) {
        error() << "but no decl";
        return;
    }

    refInfo->references.insert(from);

    std::shared_ptr<CursorInfo> &info = mData->symbols[from];
    if (!info)
        info = std::make_shared<CursorInfo>();
    info->targets.insert(to);
    if (!info->symbolLength) {
        info->symbolLength = refInfo->symbolLength;
        info->symbolName = refInfo->symbolName;
    }
}

Location ClangIndexerCXX::createLocation(const Path &sourceFile, unsigned line, unsigned col, bool *blockedPtr)
{
    uint32_t id = Location::fileId(sourceFile);
    Path resolved;
    if (!id) {
        resolved = sourceFile.resolved();
        id = Location::fileId(resolved);
        if (id)
            Location::set(sourceFile, id);
    }

    if (id) {
        if (blockedPtr) {
            Hash<uint32_t, bool>::iterator it = mData->visited.find(id);
            if (it == mData->visited.end()) {
                // the only reason we already have an id for a file that isn't
                // in the mData->visited is that it's blocked from the outset.
                // The assumption is that we never will go and fetch a file id
                // for a location without passing blockedPtr since any reference
                // to a symbol in another file should have been preceded by that
                // header in which case we would have to make a decision on
                // whether or not to index it. This is a little hairy but we
                // have to try to optimize this process.
#ifndef NDEBUG
                if (resolved.isEmpty())
                    resolved = sourceFile.resolved();
#endif
                mData->visited[id] = false;
                *blockedPtr = true;
                return Location();
            } else if (!it->second) {
                *blockedPtr = true;
                return Location();
            }
        }
        return Location(id, line, col);
    }

    ++mFileIdsQueried;
    VisitFileMessage msg(resolved, mProject, mData->key);

    mVisitFileResponseMessageFileId = UINT_MAX;
    mVisitFileResponseMessageVisit = false;
    mConnection.send(msg);
    StopWatch sw;
    EventLoop::eventLoop()->exec(mVisitFileTimeout);
    switch (mVisitFileResponseMessageFileId) {
    case 0:
        return Location();
    case UINT_MAX:
        // timed out.
        if (mVisitFileResponseMessageFileId == UINT_MAX) {
            error() << "Error getting fileId for" << resolved
                    << sw.elapsed() << mVisitFileTimeout;
        }
        exit(1);
    default:
        id = mVisitFileResponseMessageFileId;
        break;
    }
    mData->visited[id] = mVisitFileResponseMessageVisit;
    if (mVisitFileResponseMessageVisit)
        ++mIndexed;
    // fprintf(mLogFile, "%s %s\n", file.second ? "WON" : "LOST", resolved.constData());

    Location::set(resolved, id);
    if (resolved != sourceFile)
        Location::set(sourceFile, id);

    if (blockedPtr && !mVisitFileResponseMessageVisit) {
        *blockedPtr = true;
        return Location();
    }
    return Location(id, line, col);
}

struct XmlEntry
{
    enum Type { None, Warning, Error, Fixit };

    XmlEntry(Type t = None, const String &m = String(), int l = -1)
        : type(t), message(m), length(l)
    {
    }

    Type type;
    String message;
    int length;
};

static inline String xmlEscape(const String& xml)
{
    if (xml.isEmpty())
        return xml;

    std::ostringstream strm;
    const char* ch = xml.constData();
    bool done = false;
    while (true) {
        switch (*ch) {
        case '\0':
            done = true;
            break;
        case '"':
            strm << "\\\"";
            break;
        case '<':
            strm << "&lt;";
            break;
        case '>':
            strm << "&gt;";
            break;
        case '&':
            strm << "&amp;";
            break;
        default:
            strm << *ch;
            break;
        }
        if (done)
            break;
        ++ch;
    }
    return strm.str();
}

String ClangIndexerCXX::shaFile(const Path &path) const
{
    FILE *f = fopen(path.constData(), "r");
    if (!f) {
        error() << "Failed to serialize" << path;
        return String();
    }
    SHA256 sha256;
    char buf[16384];
    while (true) {
        const int r = fread(buf, sizeof(char), sizeof(buf), f);
        if (r == -1) {
            error() << "Failed to serialize" << path;
            fclose(f);
            return String();
        } else if (r > 0) {
            sha256.update(buf, r);
        }
        if (r < static_cast<int>(sizeof(buf)))
            break;
    }

    fclose(f);
    return sha256.hash();
}
