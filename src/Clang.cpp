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

#include "Clang.h"
#include <clang/Tooling/CompilationDatabase.h>

class RTagsCompilationDatabase : public clang::tooling::CompilationDatabase
{
public:
    RTagsCompilationDatabase(const Source &source, const String &unsaved = String())
        : mSource(source), mUnsaved(unsaved)
    {
        mCommand.Directory = source.pwd;
        const unsigned int commandLineFlags = Source::FilterBlacklist|Source::IncludeDefines|Source::IncludeIncludepaths;
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

bool Clang::index(const Source &source)
{
    RTagsCompilationDatabase compilationDatabase(source);
    // const unsigned int commandLineFlags = Source::FilterBlacklist|Source::IncludeDefines|Source::IncludeIncludepaths;
    // const List<String> args = source.toCommandLine(commandLineFlags);
    // const char *clangArgs[args.size() + 1];
    // clangArgs[args.size()] = 0;
    // for (int i=0; i<args.size(); ++i) {
    //     clangArgs[i] = args[i].constData();
    // }

    // clang::ASTUnit *unit = clang::ASTUnit::LoadFromCommandLine(clangArgs, clangArgs + args.size());
    //     const char **ArgBegin, const char **ArgEnd,
    //     IntrusiveRefCntPtr<DiagnosticsEngine> Diags, StringRef ResourceFilesPath,
    //     bool OnlyLocalDecls = false, bool CaptureDiagnostics = false,
    //     ArrayRef<RemappedFile> RemappedFiles = None,
    //     bool RemappedFilesKeepOriginalName = true,
    //     bool PrecompilePreamble = false, TranslationUnitKind TUKind = TU_Complete,
    //     bool CacheCodeCompletionResults = false,
    //     bool IncludeBriefCommentsInCodeCompletion = false,
    //     bool AllowPCHWithCompilerErrors = false, bool SkipFunctionBodies = false,
    //     bool UserFilesAreVolatile = false, bool ForSerialization = false,
    //     std::unique_ptr<ASTUnit> *ErrAST = nullptr);


}
