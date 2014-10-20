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
#include <clang/AST/DeclVisitor.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/RecursiveASTVisitor.h>

static inline Location createLocation(const clang::SourceLocation& loc, ClangIndexerCXX* indexer, bool* blocked = 0)
{
    const clang::SourceManager* sm = indexer->manager();
    const clang::StringRef fn = sm->getFilename(loc);
    const unsigned int l = sm->getSpellingLineNumber(loc);
    const unsigned int c = sm->getSpellingColumnNumber(loc);
    if (fn.empty()) {
        // boo
        if (*blocked)
            *blocked = false;
        return Location();
    }
    const Path path(fn.data(), fn.size());
    return indexer->createLocation(path, l, c, blocked);
}

class RTagsCompilationDatabase : public clang::tooling::CompilationDatabase
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

    virtual std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef file) const
    {
        Path path(file.data(), file.size());
        if (path.isSameFile(mSource.sourceFile()))
            return getAllCompileCommands();
        return std::vector<clang::tooling::CompileCommand>();
    }

    virtual std::vector<std::string> getAllFiles() const
    {
        return std::vector<std::string>(1, mSource.sourceFile());
    }

    virtual std::vector<clang::tooling::CompileCommand> getAllCompileCommands() const
    {
        return std::vector<clang::tooling::CompileCommand>(1, mCommand);
    }
private:
    clang::tooling::CompileCommand mCommand;
    const Source mSource;
    const String mUnsaved;
};

static inline const clang::Decl* definition(const clang::Decl* decl)
{
    if (decl) {
        if (clang::isa<clang::VarDecl>(decl)) {
            const clang::VarDecl* vd = clang::cast<clang::VarDecl>(decl);
            const clang::Decl* def = vd->getDefinition();
            if (def)
                decl = def;
        }
    }
    return decl;
}

class RTagsDeclVisitor : public clang::ConstDeclVisitor<RTagsDeclVisitor>,
                         public clang::ConstStmtVisitor<RTagsDeclVisitor>
{
public:
    RTagsDeclVisitor(ClangIndexerCXX* clang)
        : mSourceManager(0), mClang(clang)
    {
    }

    void setSourceManager(const clang::SourceManager* sm)
    {
        mSourceManager = sm;
    }

    void visitDecl(const clang::Decl* d)
    {
        if (d)
            clang::ConstDeclVisitor<RTagsDeclVisitor>::Visit(d);
    }

    void visitStmt(const clang::Stmt* s)
    {
        if (!s)
            return;
        //error() << "stmt" << s->getStmtClassName();
        if (const clang::DeclStmt *DS = clang::dyn_cast<clang::DeclStmt>(s)) {
            VisitDeclStmt(DS);
            return;
        }
        clang::ConstStmtVisitor<RTagsDeclVisitor>::Visit(s);
        for (clang::Stmt::const_child_range CI = s->children(); CI; ++CI) {
            // Stmt::const_child_range Next = CI;
            // ++Next;
            // if (!Next)
            //     lastChild();
            visitStmt(*CI);
        }
    }

    void VisitLabelDecl(const clang::LabelDecl *D)
    {
    }

    void VisitTypedefDecl(const clang::TypedefDecl *D)
    {
    }

    void VisitEnumDecl(const clang::EnumDecl *D)
    {
    }

    void VisitRecordDecl(const clang::RecordDecl *D)
    {
    }

    void VisitEnumConstantDecl(const clang::EnumConstantDecl *D)
    {
        if (const clang::Expr *Init = D->getInitExpr()) {
            visitStmt(Init);
        }
    }

    void VisitIndirectFieldDecl(const clang::IndirectFieldDecl *D)
    {
    }

    void VisitFunctionDecl(const clang::FunctionDecl *D)
    {
        const clang::CXXConstructorDecl *C = clang::dyn_cast<clang::CXXConstructorDecl>(D);
        if (C) {
            for (clang::CXXConstructorDecl::init_const_iterator I = C->init_begin(), E = C->init_end(); I != E; ++I) {
                const clang::CXXCtorInitializer* init = *I;
                if (init->isAnyMemberInitializer()) {
                    visitDecl(init->getAnyMember());
                } else if (init->isBaseInitializer()) {
#warning do stuff with getBaseClass()
                } else if (init->isDelegatingInitializer()) {
#warning do stuff with getTypeSourceInfo()
                }
                visitStmt(init->getInit());
            }
        }
        visitStmt(D->getBody());
    }

    void VisitFieldDecl(const clang::FieldDecl *D)
    {
        if (clang::Expr *Init = D->getInClassInitializer()) {
            visitStmt(Init);
        }
    }

    void VisitVarDecl(const clang::VarDecl *D)
    {
        const clang::QualType t = D->getType();
        if (!t.isNull()) {
            const clang::SplitQualType st = t.getSplitDesugaredType();
            error() << "  " << clang::QualType::getAsString(st);
            if (st.Ty) {
                // error() << "    defined at" << typeLocation(st.Ty, mSourceManager);
            }
        }
        if (D->hasInit()) {
            visitStmt(D->getInit());
        }
    }

    void VisitFileScopeAsmDecl(const clang::FileScopeAsmDecl *D)
    {
    }

    void VisitImportDecl(const clang::ImportDecl *D)
    {
    }

    void VisitNamespaceDecl(const clang::NamespaceDecl *D)
    {
    }

    void VisitUsingDirectiveDecl(const clang::UsingDirectiveDecl *D)
    {
    }

    void VisitNamespaceAliasDecl(const clang::NamespaceAliasDecl *D)
    {
    }

    void VisitTypeAliasDecl(const clang::TypeAliasDecl *D)
    {
    }

    void VisitTypeAliasTemplateDecl(const clang::TypeAliasTemplateDecl *D)
    {
    }

    void VisitCXXRecordDecl(const clang::CXXRecordDecl *D)
    {
    }

    void VisitStaticAssertDecl(const clang::StaticAssertDecl *D)
    {
        visitStmt(D->getAssertExpr());
        visitStmt(D->getMessage());
    }

    void VisitFunctionTemplateDecl(const clang::FunctionTemplateDecl *D)
    {
    }

    void VisitClassTemplateDecl(const clang::ClassTemplateDecl *D)
    {
    }

    void VisitClassTemplateSpecializationDecl(const clang::ClassTemplateSpecializationDecl *D)
    {
    }

    void VisitClassTemplatePartialSpecializationDecl(const clang::ClassTemplatePartialSpecializationDecl *D)
    {
    }

    void VisitClassScopeFunctionSpecializationDecl(const clang::ClassScopeFunctionSpecializationDecl *D)
    {
    }

    void VisitVarTemplateDecl(const clang::VarTemplateDecl *D)
    {
    }

    void VisitVarTemplateSpecializationDecl(const clang::VarTemplateSpecializationDecl *D)
    {
    }

    void VisitVarTemplatePartialSpecializationDecl(const clang::VarTemplatePartialSpecializationDecl *D)
    {
    }

    void VisitTemplateTypeParmDecl(const clang::TemplateTypeParmDecl *D)
    {
    }

    void VisitNonTypeTemplateParmDecl(const clang::NonTypeTemplateParmDecl *D)
    {
    }

    void VisitTemplateTemplateParmDecl(const clang::TemplateTemplateParmDecl *D)
    {
    }

    void VisitUsingDecl(const clang::UsingDecl *D)
    {
    }

    void VisitUnresolvedUsingTypenameDecl(const clang::UnresolvedUsingTypenameDecl *D)
    {
    }

    void VisitUnresolvedUsingValueDecl(const clang::UnresolvedUsingValueDecl *D)
    {
    }

    void VisitUsingShadowDecl(const clang::UsingShadowDecl *D)
    {
    }

    void VisitLinkageSpecDecl(const clang::LinkageSpecDecl *D)
    {
    }

    void VisitAccessSpecDecl(const clang::AccessSpecDecl *D)
    {
    }

    void VisitFriendDecl(const clang::FriendDecl *D)
    {
    }

    void VisitDeclStmt(const clang::DeclStmt *Node)
    {
        for (clang::DeclStmt::const_decl_iterator I = Node->decl_begin(), E = Node->decl_end(); I != E; ++I) {
            // if (I + 1 == E)
            //     lastChild();
            visitDecl(*I);
        }
    }

    void VisitAttributedStmt(const clang::AttributedStmt *Node)
    {
    }

    void VisitLabelStmt(const clang::LabelStmt *Node)
    {
    }

    void VisitGotoStmt(const clang::GotoStmt *Node)
    {
    }

    void VisitCXXCatchStmt(const clang::CXXCatchStmt *Node)
    {
    }

    void VisitCastExpr(const clang::CastExpr *Node)
    {
        //error() << "cast to" << Node->getCastKindName();
    }

    void VisitDeclRefExpr(const clang::DeclRefExpr *Node)
    {
        const clang::Decl* decl = definition(Node->getDecl());
        if (decl) {
            assert(decl);
            // error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
        }
    }

    void VisitPredefinedExpr(const clang::PredefinedExpr *Node)
    {
    }

    void VisitCharacterLiteral(const clang::CharacterLiteral *Node)
    {
    }

    void VisitIntegerLiteral(const clang::IntegerLiteral *Node)
    {
    }

    void VisitFloatingLiteral(const clang::FloatingLiteral *Node)
    {
    }

    void VisitStringLiteral(const clang::StringLiteral *Str)
    {
    }

    void VisitInitListExpr(const clang::InitListExpr *ILE)
    {
    }

    void VisitUnaryOperator(const clang::UnaryOperator *Node)
    {
    }

    void VisitUnaryExprOrTypeTraitExpr(const clang::UnaryExprOrTypeTraitExpr *Node)
    {
    }

    void VisitMemberExpr(const clang::MemberExpr *Node)
    {
        const clang::Decl* decl = definition(Node->getMemberDecl());
        // if (decl)
        //     error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
    }

    void VisitExtVectorElementExpr(const clang::ExtVectorElementExpr *Node)
    {
    }

    void VisitBinaryOperator(const clang::BinaryOperator *Node)
    {
    }

    void VisitCompoundAssignOperator(const clang::CompoundAssignOperator *Node)
    {
    }

    void VisitAddrLabelExpr(const clang::AddrLabelExpr *Node)
    {
    }

    void VisitBlockExpr(const clang::BlockExpr *Node)
    {
    }

    void VisitCallExpr(const clang::CallExpr *Node)
    {
        const clang::Decl* decl = Node->getCalleeDecl();
        // if (decl)
        //     error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
        const unsigned int n = Node->getNumArgs();
        const clang::Expr* const* a = Node->getArgs();
        for (unsigned int i = 0; i < n; ++i) {
            visitStmt(a[i]);
        }
    }

    void VisitOpaqueValueExpr(const clang::OpaqueValueExpr *Node)
    {
    }

    // C++
    void VisitCXXNamedCastExpr(const clang::CXXNamedCastExpr *Node)
    {
    }

    void VisitCXXBoolLiteralExpr(const clang::CXXBoolLiteralExpr *Node)
    {
    }

    void VisitCXXThisExpr(const clang::CXXThisExpr *Node)
    {
    }

    void VisitCXXFunctionalCastExpr(const clang::CXXFunctionalCastExpr *Node)
    {
    }

    void VisitCXXConstructExpr(const clang::CXXConstructExpr *Node)
    {
    }

    void VisitCXXBindTemporaryExpr(const clang::CXXBindTemporaryExpr *Node)
    {
    }

    void VisitMaterializeTemporaryExpr(const clang::MaterializeTemporaryExpr *Node)
    {
    }

    void VisitExprWithCleanups(const clang::ExprWithCleanups *Node)
    {
    }

    void VisitUnresolvedLookupExpr(const clang::UnresolvedLookupExpr *Node)
    {
    }

    void dumpCXXTemporary(const clang::CXXTemporary *Temporary)
    {
    }

    void VisitLambdaExpr(const clang::LambdaExpr *Node)
    {
    }

private:
    const clang::SourceManager* mSourceManager;
    ClangIndexerCXX* mClang;
};

class RTagsASTConsumer : public clang::ASTConsumer, public clang::RecursiveASTVisitor<RTagsASTConsumer>
{
    typedef clang::RecursiveASTVisitor<RTagsASTConsumer> base;
public:
    RTagsASTConsumer(ClangIndexerCXX *clang)
        : mDeclVisitor(clang), mClang(clang), mAborted(false)
    {}

    void HandleTranslationUnit(clang::ASTContext &Context) override {
        clang::TranslationUnitDecl *D = Context.getTranslationUnitDecl();
        mSourceManager = &Context.getSourceManager();
        mDeclVisitor.setSourceManager(mSourceManager);
        TraverseDecl(D);
    }

    bool shouldWalkTypesOfTypeLocs() const { return true; } // ### ???

    bool TraverseDecl(clang::Decl *d) {
        if (mAborted)
            return true;
        if (d)
            mDeclVisitor.visitDecl(d);
        return base::TraverseDecl(d);
    }

private:
    RTagsDeclVisitor mDeclVisitor;
    ClangIndexerCXX *mClang;
    bool mAborted;
    const clang::SourceManager* mSourceManager;
};

class RTagsPPCallbacks : public clang::PPCallbacks
{
public:
    RTagsPPCallbacks(ClangIndexerCXX *clang, const clang::SourceManager& sm)
        : mClang(clang), mSourceManager(sm)
    {
    }

    virtual void InclusionDirective(clang::SourceLocation HashLoc,
                                    const clang::Token& IncludeTok,
                                    clang::StringRef FileName,
                                    bool IsAngled,
                                    clang::CharSourceRange FilenameRange,
                                    const clang::FileEntry* File,
                                    clang::StringRef SearchPath,
                                    clang::StringRef RelativePath,
                                    const clang::Module* Imported)
    {
        mClang->included(FileName.empty() ? Path() : Path(FileName.data(), FileName.size()), createLocation(HashLoc, mClang));
    }

private:
    ClangIndexerCXX* mClang;
    const clang::SourceManager& mSourceManager;
};

class RTagsFrontendAction : public clang::ASTFrontendAction
{
public:
    RTagsFrontendAction(ClangIndexerCXX *clang)
        : mClang(clang)
    {}
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &/*CI*/, clang::StringRef /*InFile*/) override
    {
        return std::unique_ptr<clang::ASTConsumer>(new RTagsASTConsumer(mClang));
    }
#else
    clang::ASTConsumer *CreateASTConsumer(clang::CompilerInstance &/*CI*/, clang::StringRef /*InFile*/) override
    {
        return new RTagsASTConsumer(mClang);
    }
#endif
    void ExecuteAction() override
    {
        clang::Preprocessor& pre = getCompilerInstance().getPreprocessor();
        const clang::SourceManager& manager = pre.getSourceManager();
        mClang->setManager(manager);
        pre.addPPCallbacks(std::unique_ptr<RTagsPPCallbacks>(new RTagsPPCallbacks(mClang, manager)));
        clang::ASTFrontendAction::ExecuteAction();
    }
private:
    ClangIndexerCXX *mClang;
};

class RTagsFrontendActionFactory : public clang::tooling::FrontendActionFactory
{
public:
    RTagsFrontendActionFactory(ClangIndexerCXX *clang)
        : mClang(clang)
    {}
    virtual clang::FrontendAction *create()
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
    clang::tooling::ClangTool tool(compilationDatabase, compilationDatabase.getAllFiles());
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
    Deserializer deserializer(data);
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
