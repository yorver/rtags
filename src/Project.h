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

#ifndef Project_h
#define Project_h

#include "CursorInfo.h"
#include "IndexerJob.h"
#include "Match.h"
#include "QueryMessage.h"
#include "RTags.h"
#include "RTagsClang.h"
#include <memory>
#include <mutex>
#include <rct/FileSystemWatcher.h>
#include <rct/LinkedList.h>
#include <rct/Path.h>
#include <rct/RegExp.h>
#include <rct/Timer.h>

class IndexData;
class FileManager;
class IndexerJob;
class Connection;
class Dirty;
class Project : public std::enable_shared_from_this<Project>
{
public:
    Project(const Path &path);
    ~Project();

    enum State {
        Unloaded,
        Loaded,
        Syncing
    };
    State state() const { return mState; }

    enum FileManagerMode {
        FileManager_Asynchronous,
        FileManager_Synchronous
    };
    bool load(FileManagerMode mode = FileManager_Asynchronous);
    void unload();

    std::shared_ptr<FileManager> fileManager;

    Path path() const { return mPath; }
    String dbPath() const { return mDBPath; }

    bool match(const Match &match, bool *indexed = 0) const;

    std::shared_ptr<SymbolMap> symbols() { return mSymbols; }
    std::shared_ptr<SymbolNameMap> symbolNames() const { return mSymbolNames; }
    std::shared_ptr<FilesMap> files() const { return mFiles; }
    std::shared_ptr<UsrMap> usrs() const { return mUsr; }
    std::shared_ptr<SourceMap> sources() const { return mSources; }
    std::shared_ptr<DependencyMap> dependencies() const { return mDependencies; }
    std::shared_ptr<TargetsMap> targets() const { return mTargets; }
    std::shared_ptr<ReferencesMap> references() const { return mReferences; }

    Set<Location> locations(const String &symbolName, uint32_t fileId = 0) const;
    SymbolMapMemory symbols(uint32_t fileId) const;
    enum SortFlag {
        Sort_None = 0x0,
        Sort_DeclarationOnly = 0x1,
        Sort_Reverse = 0x2
    };
    List<RTags::SortedCursor> sort(const Set<Location> &locations, unsigned int flags = Sort_None) const;

    const Set<uint32_t> &suspendedFiles() const;
    bool toggleSuspendFile(uint32_t file);
    bool isSuspended(uint32_t file) const;
    void clearSuspendedFiles();

    bool isIndexed(uint32_t fileId) const;

    void index(const std::shared_ptr<IndexerJob> &job);
    List<Source> sources(uint32_t fileId) const;
    enum DependencyMode {
        DependsOnArg,
        ArgDependsOn // slow
    };
    Set<uint32_t> dependencies(uint32_t fileId, DependencyMode mode) const;
    bool isActiveJob(uint64_t key) { return !key || mActiveJobs.contains(key); }
    inline bool visitFile(uint32_t fileId, const Path &path, uint64_t id);
    inline void releaseFileIds(const Set<uint32_t> &fileIds);
    String fixIts(uint32_t fileId) const;
    int reindex(const Match &match, const std::shared_ptr<QueryMessage> &query);
    int remove(const Match &match);
    void onJobFinished(const std::shared_ptr<IndexerJob> &job, const std::shared_ptr<IndexData> &indexData);
    const Set<Path> &watchedPaths() const { return mWatchedPaths; }
    bool isIndexing() const { return !mActiveJobs.isEmpty(); }
    void onFileModifiedOrRemoved(const Path &);
    Hash<uint32_t, Path> visitedFiles() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mVisitedFiles;
    }
    void encodeVisitedFiles(Serializer &serializer)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        serializer << mVisitedFiles;
    }
    enum SyncMode {
        Sync_Asynchronous,
        Sync_Synchronous
    };
    bool startSync(SyncMode mode);
    String sync();
private:
    void watch(const Path &file);
    void reloadFileManager();
    void addDependencies(const DependencyMapMemory &deps, Set<uint32_t> &newFiles);
    void addFixIts(const DependencyMapMemory &visited, const FixItMap &fixIts);
    int startDirtyJobs(Dirty *dirty, const UnsavedFiles &unsavedFiles = UnsavedFiles());
    void onSynced();
    void onDirtyTimeout(Timer *);

    const Path mPath;
    Path mDBPath;
    State mState;

    // these need to be mutable since we don't have a non-const iterator for DB
    mutable std::shared_ptr<DependencyMap> mDependencies;
    mutable std::shared_ptr<SourceMap> mSources;
    mutable std::shared_ptr<SymbolMap> mSymbols;
    mutable std::shared_ptr<SymbolNameMap> mSymbolNames;
    mutable std::shared_ptr<TargetsMap> mTargets;
    mutable std::shared_ptr<ReferencesMap> mReferences;
    std::shared_ptr<UsrMap> mUsr;
    std::shared_ptr<FilesMap> mFiles;
    std::shared_ptr<DB<String, String> > mGeneral;

    Hash<uint32_t, Path> mVisitedFiles;
    Hash<uint64_t, std::pair<std::shared_ptr<IndexerJob>, std::shared_ptr<IndexData> > > mPendingIndexData;

    int mJobCounter;

    // key'ed on Source::key()
    Hash<uint64_t, std::shared_ptr<IndexData> > mIndexData;
    Hash<uint64_t, std::shared_ptr<IndexerJob> > mActiveJobs;
    List<std::shared_ptr<IndexerJob> > mPendingJobs;

    Timer mSyncTimer, mDirtyTimer;
    Set<uint32_t> mDirtyFiles, mPendingDirtyFiles;
    // mDirtyFiles are files that need to be dirtied on the next sync
    // mPendingDirtyFiles are files that get collapsed into a single
    // startDirtyJobs with mDirtyTimer
    StopWatch mTimer;
    FileSystemWatcher mWatcher;
    Set<Path> mWatchedPaths;
    FixItMap mFixIts;

    Set<uint32_t> mSuspendedFiles;

    mutable std::mutex mMutex;

    friend class SyncThread;
};

inline bool Project::visitFile(uint32_t visitFileId, const Path &path, uint64_t key)
{
    std::lock_guard<std::mutex> lock(mMutex);
    assert(visitFileId);
    Path &p = mVisitedFiles[visitFileId];
    if (p.isEmpty()) {
        p = path;
        if (key) {
            assert(mActiveJobs.contains(key));
            std::shared_ptr<IndexerJob> &job = mActiveJobs[key];
            assert(job);
            job->visited.insert(visitFileId);
        }
        return true;
    }
    return false;
}

inline void Project::releaseFileIds(const Set<uint32_t> &fileIds)
{
    if (!fileIds.isEmpty()) {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto &f : fileIds) {
            // error() << "Returning files" << Location::path(f);
            mVisitedFiles.remove(f);
        }
    }
}

#endif
