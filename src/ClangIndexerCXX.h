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

#ifndef ClangIndexerCXX_h
#define ClangIndexerCXX_h

#include <rct/StopWatch.h>
#include <rct/Hash.h>
#include <rct/Serializer.h>
#include <rct/Path.h>
#include <rct/Connection.h>
#include <sys/stat.h>
#include "IndexerJob.h"
#include "RTagsClang.h"
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

struct Unit;
class IndexData;
class ClangIndexerCXX
{
public:
    ClangIndexerCXX();
    ~ClangIndexerCXX();

    bool exec(const String &data);
private:
    bool parse();
    bool visit();
    bool diagnose();
    String shaFile(const Path &path) const;

    void addFileSymbol(uint32_t file);
    inline Location createLocation(const clang::SourceLocation &location, bool *blocked = 0)
    {
        bool invalid = false;
        clang::StringRef fileName = mManager->getFilename(location);
        unsigned int line = mManager->getSpellingLineNumber(location, &invalid);
        if (invalid) {
            if (blocked)
                *blocked = false;
            return Location();
        }
        unsigned int col = mManager->getSpellingColumnNumber(location, &invalid);
        if (invalid) {
            if (blocked)
                *blocked = false;
            return Location();
        }
        const Path fn = Path::resolved(Path(fileName.data(), fileName.size()));
        if (fn.isEmpty() || fn == "<built-in>" || fn == "<command line>") {
            if (blocked)
                *blocked = false;
            return Location();
        }
        if (fn == mLastFile) {
            if (mLastBlocked && blocked) {
                *blocked = true;
                return Location();
            } else if (blocked) {
                *blocked = false;
            }

            return Location(mLastFileId, line, col);
        }
        const Location ret = createLocation(fn, line, col, blocked);
        if (blocked) {
            mLastBlocked = *blocked;
            mLastFileId = ret.fileId();
            mLastFile = fn;
        }
        return ret;
    }
    Location createLocation(const clang::StringRef& file, unsigned line, unsigned col, bool *blocked = 0)
    {
        if (blocked)
            *blocked = false;
        if (file.empty())
            return Location();
        const Path p = Path::resolved(Path(file.data(), file.size()));
        return createLocation(p, line, col, blocked);
    }
    // inline Location createLocation(const CXCursor &cursor, bool *blocked = 0)
    // {
    //     const CXSourceRange range = clang_Cursor_getSpellingNameRange(cursor, 0, 0);
    //     if (clang_Range_isNull(range))
    //         return Location();
    //     return createLocation(clang_getRangeStart(range), blocked);
    // }
    Location createLocation(const Path &file, unsigned int line, unsigned int col, bool *blocked = 0);
    // String addNamePermutations(const CXCursor &cursor, const Location &location);

    void onMessage(const std::shared_ptr<Message> &msg, Connection *conn);

    clang::SourceManager* mManager;

    Path mProject;
    Source mSource;
    Path mSourceFile;
    std::shared_ptr<IndexData> mData;
    bool mLoadedFromCache;
    String mClangLine;
    uint32_t mVisitFileResponseMessageFileId;
    bool mVisitFileResponseMessageVisit;
    Path mSocketFile, mASTCacheDir;
    StopWatch mTimer;
    int mParseDuration, mVisitDuration, mBlocked, mAllowed,
        mIndexed, mVisitFileTimeout, mIndexerMessageTimeout, mFileIdsQueried;
    UnsavedFiles mUnsavedFiles;
    Connection mConnection;
    FILE *mLogFile;
    uint32_t mLastFileId;
    bool mLastBlocked;
    Path mLastFile;
};

#endif
