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

#include "Project.h"
#include "FileManager.h"
#include "Diagnostic.h"
#include "IndexerJob.h"
#include "RTags.h"
#include "Server.h"
#include "Server.h"
#include "JobScheduler.h"
#include "IndexData.h"
#include <math.h>
#include <fnmatch.h>
#include <rct/Log.h>
#include <rct/MemoryMonitor.h>
#include <rct/Path.h>
#include <rct/Rct.h>
#include <rct/ReadLocker.h>
#include <rct/RegExp.h>
#include <rct/Thread.h>
#include <memory>

enum {
    SyncTimeout = 500,
    DirtyTimeout = 100
};


template <typename T>
static inline bool openDB(std::shared_ptr<T> &db, const Path &dbPath, const char *name, std::function<int(const char *l, int ll, const char *r, int rl)> cmp = 0)
{
    if (!db)
        db.reset(new T);
    if (db->path().isEmpty()) {
        warning() << "Opening" << (dbPath + name);
        String err;
        if (!db->open(dbPath + name, 0, cmp, &err)) {
            error() << "Failed to open database" << dbPath + name << err;
            return false;
        }
    }
    return true;
}

class SyncThread : public Thread
{
public:
    SyncThread(const std::shared_ptr<Project> &project)
        : mProject(project)
    {
        setAutoDelete(true);
    }

    virtual void run()
    {
        if (std::shared_ptr<Project> project = mProject.lock()) {
            const String msg = project->sync();
            EventLoop::mainEventLoop()->callLater([project, msg]() {
                    if (!msg.isEmpty())
                        error() << msg;
                    project->onSynced();
                });
        }
    }

    std::weak_ptr<Project> mProject;
};

class Dirty
{
public:
    virtual ~Dirty() {}
    virtual Set<uint32_t> dirtied() const = 0;
    virtual bool isDirty(const Source &source) = 0;
};

class SimpleDirty : public Dirty
{
public:
    SimpleDirty()
        : Dirty()
    {}
    void init(const Set<uint32_t> &dirty, const std::shared_ptr<DependencyMap> &dependencies)
    {
        for (auto fileId : dirty) {
            mDirty.insert(fileId);
            mDirty += dependencies->value(fileId);
        }
    }

    virtual Set<uint32_t> dirtied() const
    {
        return mDirty;
    }

    virtual bool isDirty(const Source &source)
    {
        return mDirty.contains(source.fileId);
    }

    Set<uint32_t> mDirty;
};

class ComplexDirty : public Dirty
{
public:
    ComplexDirty()
        : Dirty()
    {}
    virtual Set<uint32_t> dirtied() const
    {
        return mDirty;
    }
    void insertDirtyFile(uint32_t fileId)
    {
        mDirty.insert(fileId);
    }
    inline uint64_t lastModified(uint32_t fileId)
    {
        uint64_t &time = mLastModified[fileId];
        if (!time) {
            time = Location::path(fileId).lastModifiedMs();
        }
        return time;
    }

    Hash<uint32_t, uint64_t> mLastModified;
    Set<uint32_t> mDirty;
};

class SuspendedDirty : public ComplexDirty
{
public:
    SuspendedDirty()
        : ComplexDirty()
    {}
    bool isDirty(const Source &)
    {
        return false;
    }
};

class IfModifiedDirty : public ComplexDirty
{
public:
    IfModifiedDirty(const std::shared_ptr<DependencyMap> &dependencies, const Match &match = Match())
        : ComplexDirty(), mMatch(match)
    {
        for (auto it = dependencies->createIterator(); it->isValid(); it->next()) {
            const uint32_t dependee = it->key();
            const Set<uint32_t> &dependents = it->value();
            for (auto dependent : dependents) {
                mReversedDependencies[dependent].insert(dependee);
            }
        }
        // mReversedDependencies are in the form of:
        //   Path.cpp: Path.h, String.h ...
        // mDependencies are like this:
        //   Path.h: Path.cpp, Server.cpp ...
    }

    virtual bool isDirty(const Source &source)
    {
        bool ret = false;

        if (mMatch.isEmpty() || mMatch.match(source.sourceFile())) {
            for (auto it : mReversedDependencies[source.fileId]) {
                const uint64_t depLastModified = lastModified(it);
                if (!depLastModified || depLastModified > source.parsed) {
                    // dependency is gone
                    ret = true;
                    insertDirtyFile(it);
                }
            }
            if (ret)
                insertDirtyFile(source.fileId);

            assert(!ret || mDirty.contains(source.fileId));
        }
        return ret;
    }

    DependencyMapMemory mReversedDependencies;
    Match mMatch;
};


class WatcherDirty : public ComplexDirty
{
public:
    WatcherDirty(const std::shared_ptr<DependencyMap> &dependencies, const Set<uint32_t> &modified)
    {
        for (auto it : modified) {
            mModified[it] = dependencies->value(it);
        }
    }

    virtual bool isDirty(const Source &source)
    {
        bool ret = false;

        for (auto it : mModified) {
            const auto &deps = it.second;
            if (deps.contains(source.fileId)) {
                const uint64_t depLastModified = lastModified(it.first);
                if (!depLastModified || depLastModified > source.parsed) {
                    // dependency is gone
                    ret = true;
                    insertDirtyFile(it.first);
                }
            }
        }

        if (ret)
            insertDirtyFile(source.fileId);
        return ret;
    }

    DependencyMapMemory mModified;
};

Project::Project(const Path &path)
    : mPath(path), mDBPath(path), mState(Unloaded), mJobCounter(0)
{
    Path p = mPath;
    RTags::encodePath(p);
    mDBPath = Server::instance()->options().dataDir + p + "/";

    const auto &options = Server::instance()->options();

    if (!(options.options & Server::NoFileSystemWatch)) {
        mWatcher.modified().connect(std::bind(&Project::onFileModifiedOrRemoved, this, std::placeholders::_1));
        mWatcher.removed().connect(std::bind(&Project::onFileModifiedOrRemoved, this, std::placeholders::_1));
    }
    if (!(options.options & Server::NoFileManagerWatch)) {
        mWatcher.removed().connect(std::bind(&Project::reloadFileManager, this));
        mWatcher.added().connect(std::bind(&Project::reloadFileManager, this));
    }
    mSyncTimer.timeout().connect([this](Timer *) { this->startSync(Sync_Asynchronous); });
    mDirtyTimer.timeout().connect(std::bind(&Project::onDirtyTimeout, this, std::placeholders::_1));
}

Project::~Project()
{
    assert(EventLoop::isMainThread());
    assert(mActiveJobs.isEmpty());
}

bool Project::load(FileManagerMode mode)
{
    switch (mState) {
    case Syncing:
    case Loaded:
        return true;
    case Unloaded:
        fileManager.reset(new FileManager);
        fileManager->init(shared_from_this(),
                          mode == FileManager_Asynchronous ? FileManager::Asynchronous : FileManager::Synchronous);
        break;
    }

    auto uint64Compare = [](const char *a, int , const char *b, int) {
        const uint64_t aval = *reinterpret_cast<const uint64_t*>(a);
        const uint64_t bval = *reinterpret_cast<const uint64_t*>(b);
        if (aval < bval)
            return -1;
        return aval == bval ? 0 : 1;
    };

    auto uint32Compare = [](const char *a, int , const char *b, int) {
        const uint32_t aval = *reinterpret_cast<const uint32_t*>(a);
        const uint32_t bval = *reinterpret_cast<const uint32_t*>(b);
        if (aval < bval)
            return -1;
        return aval == bval ? 0 : 1;
    };

    mFiles.reset(new FilesMap);

    Path::mkdir(mDBPath, Path::Recursive);
    if (!openDB(mSymbols, mDBPath, "symbols", uint64Compare)
        || !openDB(mSymbolNames, mDBPath, "symbolnames")
        || !openDB(mUsr, mDBPath, "usr")
        || !openDB(mDependencies, mDBPath, "dependencies", uint32Compare)
        || !openDB(mSources, mDBPath, "sources", uint64Compare)
        || !openDB(mReferences, mDBPath, "references", uint64Compare)
        || !openDB(mTargets, mDBPath, "targets", uint64Compare)
        || !openDB(mGeneral, mDBPath, "db")) {
        return false;
    }

    mState = Loaded;
    std::unique_ptr<ComplexDirty> dirty;
    const String visited = mGeneral->value("visitedFiles");
    if (!visited.isEmpty()) {
        Deserializer deserializer(visited);
        deserializer >> mVisitedFiles;
    }

    for (auto dep = mDependencies->createIterator(); dep->isValid(); dep->next()) {
        watch(Location::path(dep->key()));
    }

    if (Server::instance()->suspended()) {
        dirty.reset(new SuspendedDirty);
    } else {
        dirty.reset(new IfModifiedDirty(mDependencies));
    }

    {
        std::shared_ptr<DependencyMap::WriteScope> dependenciesWriteScope;
        auto it = mDependencies->createIterator();

        while (it->isValid()) {
            const Path path = Location::path(it->key());
            if (!path.isFile()) {
                error() << path << "seems to have disappeared";
                dirty.get()->insertDirtyFile(it->key());

                const Set<uint32_t> &dependents = it->value();
                for (auto dependent : dependents) {
                    // we don't have a file to compare with to
                    // know whether the source is parsed after the
                    // file was removed... so, force sources
                    // dirty.
                    dirty.get()->insertDirtyFile(dependent);
                }

                if (!dependenciesWriteScope)
                    dependenciesWriteScope = mDependencies->createWriteScope(1024 * 8);

                it->erase();
            } else {
                it->next();
            }
        }
    }

    {
        std::shared_ptr<SourceMap::WriteScope> sourcesWriteScope;
        auto it = mSources->createIterator();
        while (it->isValid()) {
            const Source &source = it->value();
            if (!source.sourceFile().isFile()) {
                error() << source.sourceFile() << "seems to have disappeared";
                dirty.get()->insertDirtyFile(source.fileId);
                if (!sourcesWriteScope)
                    sourcesWriteScope = mSources->createWriteScope(1024 * 8);
                it->erase();
            } else {
                it->next();
            }
        }
    }
    if (dirty)
        startDirtyJobs(dirty.get());
    return true;
}

void Project::unload()
{
    switch (mState) {
    case Unloaded:
        return;
    case Syncing: {
        std::weak_ptr<Project> weak = shared_from_this();
        EventLoop::eventLoop()->registerTimer([weak](int) {
                if (std::shared_ptr<Project> project = weak.lock())
                    project->unload(); },
            1000, Timer::SingleShot);
        return; }
    default:
        break;
    }
    for (const auto &job : mActiveJobs) {
        assert(job.second);
        Server::instance()->jobScheduler()->abort(job.second);
    }

    const String msg = sync();
    if (!msg.isEmpty())
        error() << msg;

    mActiveJobs.clear();
    fileManager.reset();

    mSymbols.reset();
    mSymbolNames.reset();
    mDependencies.reset();
    mUsr.reset();
    mSources.reset();

    mFiles.reset();
    mVisitedFiles.clear();
    mState = Unloaded;
    mSyncTimer.stop();
    mDirtyTimer.stop();
}

bool Project::match(const Match &p, bool *indexed) const
{
    Path paths[] = { p.pattern(), p.pattern() };
    paths[1].resolve();
    const int count = paths[1].compare(paths[0]) ? 2 : 1;
    bool ret = false;
    const Path resolvedPath = mPath.resolved();
    for (int i=0; i<count; ++i) {
        const Path &path = paths[i];
        const uint32_t id = Location::fileId(path);
        if (id && isIndexed(id)) {
            if (indexed)
                *indexed = true;
            return true;
        } else if ((mFiles && mFiles->contains(path)) || p.match(mPath) || p.match(resolvedPath)) {
            if (!indexed)
                return true;
            ret = true;
        }
    }
    if (indexed)
        *indexed = false;
    return ret;
}

void Project::onJobFinished(const std::shared_ptr<IndexerJob> &job, const std::shared_ptr<IndexData> &indexData)
{
    mSyncTimer.stop();
    if (mState == Syncing) {
        mPendingIndexData[indexData->key] = std::make_pair(job, indexData);
        return;
    } else if (mState != Loaded) {
        return;
    }
    assert(indexData);
    std::shared_ptr<IndexerJob> restart;
    const uint32_t fileId = indexData->fileId();
    auto j = mActiveJobs.take(indexData->key);
    if (!j) {
        error() << "Couldn't find JobData for" << Location::path(fileId);
        return;
    } else if (j != job) {
        error() << "Wrong IndexerJob for for" << Location::path(fileId);
        return;
    }

    const bool success = job->flags & IndexerJob::Complete;
    assert(!(job->flags & IndexerJob::Aborted));
    assert(((job->flags & (IndexerJob::Complete|IndexerJob::Crashed)) == IndexerJob::Complete)
           || ((job->flags & (IndexerJob::Complete|IndexerJob::Crashed)) == IndexerJob::Crashed));
    const auto &options = Server::instance()->options();
    if (!success) {
        releaseFileIds(job->visited);
    }

    auto src = mSources->find(indexData->key);
    if (!src->isValid()) {
        error() << "Can't find source for" << Location::path(fileId);
        return;
    }

    const int idx = mJobCounter - mActiveJobs.size();
    if (testLog(RTags::CompilationErrorXml)) {
        logDirect(RTags::CompilationErrorXml, Diagnostic::format(indexData->diagnostics));
        if (!(options.options & Server::NoProgress)) {
            log(RTags::CompilationErrorXml,
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<progress index=\"%d\" total=\"%d\"></progress>",
                idx, mJobCounter);
        }
    }

    mIndexData[indexData->key] = indexData;
    if (success) {
        auto sourcesWriteScope = mSources->createWriteScope(1024 * 8);
        Source s = src->value();
        s.parsed = indexData->parseTime;
        src->setValue(s);
        error("[%3d%%] %d/%d %s %s.",
              static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
              String::formatTime(time(0), String::Time).constData(),
              indexData->message.constData());
    } else {
        assert(indexData->flags & IndexerJob::Crashed);
        error("[%3d%%] %d/%d %s %s indexing crashed.",
              static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
              String::formatTime(time(0), String::Time).constData(),
              Location::path(fileId).toTilde().constData());
    }

    if (options.syncThreshold && mIndexData.size() >= options.syncThreshold) {
        startSync(Sync_Asynchronous);
    } else if (mActiveJobs.isEmpty()) {
        mSyncTimer.restart(indexData->flags & IndexerJob::Dirty ? 0 : SyncTimeout, Timer::SingleShot);
    }
}

static inline void markActive(const std::unique_ptr<SourceMap::Iterator> &start, uint32_t buildId)
{
    const uint32_t fileId = start->value().fileId;
    while (start->isValid()) {
        uint32_t f, b;
        Source::decodeKey(start->key(), f, b);
        if (f != fileId)
            break;

        Source source = start->value();
        unsigned int flags = source.flags;
        if (b == buildId) {
            flags |= Source::Active;
        } else {
            flags &= ~Source::Active;
        }
        if (source.flags != flags) {
            start->setValue(source);
        }
        start->next();
    }
}

void Project::index(const std::shared_ptr<IndexerJob> &job)
{
    const Path sourceFile = job->sourceFile;
    static const char *fileFilter = getenv("RTAGS_FILE_FILTER");
    if (fileFilter && !strstr(job->sourceFile.constData(), fileFilter)) {
        error() << "Not indexing" << job->sourceFile.constData() << "because of file filter"
                << fileFilter;
        return;
    }

    if (mState != Loaded) {
        assert(mState == Syncing);
        mPendingJobs.append(job);
        return;
    }
    const uint64_t key = job->source.key();
    if (Server::instance()->suspended() && mSources->contains(key) && (job->flags & IndexerJob::Compile)) {
        return;
    }

    auto writeScope = mSources->createWriteScope(1024 * 8);
    if (job->flags & IndexerJob::Compile) {
        const auto &options = Server::instance()->options();
        if (options.options & Server::NoFileSystemWatch) {
            auto it = mSources->lower_bound(Source::key(job->source.fileId, 0));
            if (it->isValid()) {
                uint32_t f, b;
                Source::decodeKey(it->key(), f, b);
                if (f == job->source.fileId) {
                    // When we're not watching the file system, we ignore
                    // updating compiles. This means that you always have to
                    // do check-reindex to build existing files!
                    return;
                }
            }
        } else {
            auto cur = mSources->find(key);
            if (cur->isValid()) {
                if (!(cur->value().flags & Source::Active))
                    markActive(mSources->lower_bound(Source::key(job->source.fileId, 0)), cur->value().buildRootId);
                if (cur->value().compareArguments(job->source)) {
                    // no updates
                    return;
                }
            } else {
                auto it = mSources->lower_bound(Source::key(job->source.fileId, 0));
                if (it->isValid()) {
                    const auto start = mSources->find(it->key()); // there's no way to duplicate an iterator in RocksDB
                    const bool disallowMultiple = options.options & Server::DisallowMultipleSources;
                    bool unsetActive = false;
                    while (it->isValid()) {
                        uint32_t f, b;
                        Source::decodeKey(it->key(), f, b);
                        if (f != job->source.fileId)
                            break;

                        if (it->value().compareArguments(job->source)) {
                            markActive(start, b);
                            // no updates
                            return;
                        } else if (disallowMultiple) {
                            it->erase();
                            continue;
                        }
                        unsetActive = true;
                        it->next();
                    }
                    if (unsetActive) {
                        assert(!disallowMultiple);
                        markActive(start, 0);
                    }
                }
            }
        }
    }

    Source source = job->source;
    source.flags |= Source::Active;
    mSources->set(key, source);

    String err;
    if (!writeScope->flush(&err)) {
        error() << "Failed to write to sources" << mSources->size() << err;
    }

    std::shared_ptr<IndexerJob> &ref = mActiveJobs[key];
    if (ref) {
        releaseFileIds(ref->visited);
        Server::instance()->jobScheduler()->abort(ref);
        --mJobCounter;
    }
    ref = job;

    if (mIndexData.remove(key))
        --mJobCounter;

    if (!mJobCounter++)
        mTimer.start();

    mSyncTimer.stop();
    Server::instance()->jobScheduler()->add(job);
}

void Project::onFileModifiedOrRemoved(const Path &file)
{
    const uint32_t fileId = Location::fileId(file);
    debug() << file << "was modified" << fileId;
    if (!fileId)
        return;
    if (Server::instance()->suspended() || mSuspendedFiles.contains(fileId)) {
        warning() << file << "is suspended. Ignoring modification";
        return;
    }
    if (mPendingDirtyFiles.insert(fileId)) {
        mDirtyTimer.restart(DirtyTimeout, Timer::SingleShot);
    }
}

void Project::onDirtyTimeout(Timer *)
{
    Set<uint32_t> dirtyFiles = std::move(mPendingDirtyFiles);
    WatcherDirty dirty(mDependencies, dirtyFiles);
    startDirtyJobs(&dirty);
}

List<Source> Project::sources(uint32_t fileId) const
{
    List<Source> ret;
    if (fileId) {
        auto it = mSources->lower_bound(Source::key(fileId, 0));
        while (it->isValid()) {
            uint32_t f, b;
            Source::decodeKey(it->key(), f, b);
            if (f != fileId)
                break;
            ret.append(it->value());
            it->next();
        }
    }
    return ret;
}

template <typename T>
static inline int uniteSet(const Set<T> &original, const Set<T> &newValues, Set<T> &result)
{
    assert(!newValues.isEmpty());
    if (original.isEmpty()) {
        result = newValues;
        return newValues.size();
    }

    int ret = 0;
    for (const T &t : newValues) {
        if (ret) {
            if (result.insert(t))
                ++ret;
        } else if (!original.contains(t)) {
            result = original;
            result.insert(t);
            assert(!ret);
            ++ret;
        }
    }
    return ret;
}

template <typename T, typename K>
static inline int uniteMap(const Map<T, K> &original, const Map<T, K> &newValues, Map<T, K> &result)
{
    assert(!newValues.isEmpty());
    if (original.isEmpty()) {
        result = newValues;
        return newValues.size();
    }

    int ret = 0;
    for (const auto &v : newValues) {
        if (ret) {
            if (result.insert(v.first, v.second))
                ++ret;
        } else if (!original.contains(v.first)) {
            result = original;
            result[v.first] = v.second;
            assert(!ret);
            ++ret;
        }
    }
    return ret;
}

void Project::addDependencies(const DependencyMapMemory &deps, Set<uint32_t> &newFiles)
{
    auto scope = mDependencies->createWriteScope(1024 * 1024);
    StopWatch timer;

    const auto end = deps.end();
    for (auto it = deps.begin(); it != end; ++it) {
        auto cur = mDependencies->find(it->first);
        if (!cur->isValid()) {
            mDependencies->set(it->first, it->second);
        } else {
            Set<uint32_t> merged;
            if (uniteSet(cur->value(), it->second, merged)) {
                cur->setValue(merged);
            }
        }
        if (newFiles.isEmpty()) {
            newFiles = it->second;
        } else {
            newFiles.unite(it->second);
        }
        newFiles.insert(it->first);
    }
}

Set<uint32_t> Project::dependencies(uint32_t fileId, DependencyMode mode) const
{
    if (mode == DependsOnArg)
        return mDependencies->value(fileId);

    Set<uint32_t> ret;
    for (auto it = mDependencies->createIterator(); it->isValid(); it->next()) {
        if (it->value().contains(fileId))
            ret.insert(it->key());
    }
    return ret;
}

int Project::reindex(const Match &match, const std::shared_ptr<QueryMessage> &query)
{
    if (query->type() == QueryMessage::Reindex) {
        Set<uint32_t> dirtyFiles;

        for (auto it = mDependencies->createIterator(); it->isValid(); it->next()) {
            if (!dirtyFiles.contains(it->key()) && (match.isEmpty() || match.match(Location::path(it->key())))) {
                dirtyFiles.insert(it->key());
            }
        }
        if (dirtyFiles.isEmpty())
            return 0;
        SimpleDirty dirty;
        dirty.init(dirtyFiles, mDependencies);
        return startDirtyJobs(&dirty, query->unsavedFiles());
    } else {
        assert(query->type() == QueryMessage::CheckReindex);
        IfModifiedDirty dirty(mDependencies, match);
        return startDirtyJobs(&dirty, query->unsavedFiles());
    }
}

int Project::remove(const Match &match)
{
    int count = 0;
    Set<uint32_t> dirty;
    auto it = mSources->createIterator();
    while (it->isValid()) {
        if (match.match(it->value().sourceFile())) {
            const uint32_t fileId = it->value().fileId;
            it->erase();
            std::shared_ptr<IndexerJob> job = mActiveJobs.take(fileId);
            if (job) {
                releaseFileIds(job->visited);
                Server::instance()->jobScheduler()->abort(job);
            }
            mIndexData.remove(fileId);
            dirty.insert(fileId);
            ++count;
        } else {
            it->next();
        }
    }
    if (count) {
        auto symbolsWriteScope = mSymbols->createWriteScope(1024);
        auto referencesWriteScope = mReferences->createWriteScope(1024);
        auto targetsWriteScope = mTargets->createWriteScope(1024);
        auto symbolNameWriteScope = mSymbolNames->createWriteScope(1024);
        auto usrScope = mUsr->createWriteScope(1024);

        RTags::dirtySymbols(mSymbols, dirty);
        RTags::dirtyReferences(mReferences, dirty);
        RTags::dirtyTargets(mTargets, dirty);
        RTags::dirtySymbolNames(mSymbolNames, dirty);
        RTags::dirtyUsr(mUsr, dirty);
    }
    return count;
}

int Project::startDirtyJobs(Dirty *dirty, const UnsavedFiles &unsavedFiles)
{
    List<Source> toIndex;
    for (auto source = mSources->createIterator(); source->isValid(); source->next()) {
        if (source->value().flags & Source::Active && dirty->isDirty(source->value())) {
            toIndex << source->value();
        }
    }
    const Set<uint32_t> dirtyFiles = dirty->dirtied();

    for (const auto &fileId : dirtyFiles) {
        mVisitedFiles.remove(fileId);
    }

    for (const auto &source : toIndex) {
        index(std::shared_ptr<IndexerJob>(new IndexerJob(source, IndexerJob::Dirty, mPath, unsavedFiles)));
    }

    if (!toIndex.size() && !dirtyFiles.isEmpty()) {
        // this is for the case where we've removed a file
        auto symbolsWriteScope = mSymbols->createWriteScope(1024);
        auto referencesWriteScope = mReferences->createWriteScope(1024);
        auto targetsWriteScope = mTargets->createWriteScope(1024);
        auto symbolNameWriteScope = mSymbolNames->createWriteScope(1024);
        auto usrScope = mUsr->createWriteScope(1024);

        RTags::dirtySymbols(mSymbols, dirtyFiles);
        RTags::dirtyReferences(mReferences, dirtyFiles);
        RTags::dirtyTargets(mTargets, dirtyFiles);
        RTags::dirtySymbolNames(mSymbolNames, dirtyFiles);
        RTags::dirtyUsr(mUsr, dirtyFiles);
    } else {
        mDirtyFiles += dirtyFiles;
    }
    return toIndex.size();
}

static inline int writeSymbolNames(const SymbolNameMapMemory &symbolNames, const std::shared_ptr<SymbolNameMap> &current)
{
    auto scope = current->createWriteScope(1024 * 1024);

    int ret = 0;
    auto it = symbolNames.begin();
    const auto end = symbolNames.end();
    while (it != end) {
        auto cur = current->find(it->first);
        if (!cur->isValid()) {
            current->set(it->first, it->second);
        } else {
            Set<Location> merged;
            const int count = uniteSet(cur->value(), it->second, merged);
            if (count) {
                ret += count;
                cur->setValue(merged);
            }
        }
        ++it;
    }
    return ret;
}


static inline void joinCursors(TargetsMapMemory &targets, const Map<Location, uint16_t> &locations)
{
    for (const auto &location : locations) {
        Map<Location, uint16_t> &t = targets[location.first];
        for (const auto &innerLoc : locations) {
            if (location.first != innerLoc.first) {
                t[innerLoc.first] = innerLoc.second;
            }
            // ### this is filthy, we could likely think of something better
        }
    }
}

static inline void writeUsr(const UsrMapMemory &usr,
                            const std::shared_ptr<UsrMap> &current,
                            TargetsMapMemory &targets)
{
    auto usrScope = current->createWriteScope(1024 * 1024);

    // error() << "Writing usr" << usr.size();
    for (const auto &it : usr) {
        // error() << "usr" << it.first << it.second;
        auto cur = current->find(it.first);
        if (!cur->isValid()) {
            current->set(it.first, it.second);
            if (it.second.size() > 1)
                joinCursors(targets, it.second);
        } else {
            Map<Location, uint16_t> merged;
            if (uniteMap(cur->value(), it.second, merged)) {
                cur->setValue(merged);
                if (merged.size() > 1)
                    joinCursors(targets, merged);
            }
        }
    }
}

static inline void resolvePendingReferences(const std::shared_ptr<SymbolMap> &symbols,
                                            const std::shared_ptr<UsrMap> &usrs,
                                            const UsrMapMemory &pendingRefs,
                                            TargetsMapMemory &allTargets,
                                            ReferencesMapMemory &allReferences)
{
    for (const auto &ref : pendingRefs) {
        assert(!ref.second.isEmpty());
        // find the declaration
        List<String> refUsrs;
        {
            String refUsr = ref.first;
            refUsrs.append(refUsr);
            // assume this is an implicit instance method for a property, replace the last (im) with (py)
            const int lastIm = refUsr.lastIndexOf("(im)");
            if (lastIm != -1) {
                refUsr.replace(lastIm, 4, "(py)");
                refUsrs.append(refUsr);
            }
        }
        SymbolMapMemory targets;
        for (const String &refUsr : refUsrs) {
            const auto usr = usrs->find(refUsr);
            if (usr->isValid()) {
                for (const auto &usrLoc : usr->value()) {
                    auto symbol = symbols->value(usrLoc.first);
                    assert(symbol);
                    if (RTags::isCursor(symbol->kind))
                        targets[usrLoc.first] = symbol;
                }
            }
        }
        if (!targets.isEmpty()) {
            for (const auto &r : ref.second) {
                Map<Location, uint16_t> &subTargets = allTargets[r.first];
                for (const auto &t : targets) {
                    subTargets[t.first] = t.second->kind;
                    allReferences[t.first].insert(r.first);
                }
            }
        }
    }
}

static inline int writeSymbols(const SymbolMapMemory &symbols, const std::shared_ptr<SymbolMap> &current)
{
    int ret = 0;
    // const bool wasEmpty = current->isEmpty();
    auto it = symbols.begin();
    const auto end = symbols.end();
    while (it != end) {
        current->set(it->first, it->second);
        ++ret;
        ++it;
    }
    return ret;
}

template <typename Container>
static inline void uniteUnite(Container &current, const Container &newValues)
{
    const bool wasEmpty = current.isEmpty();
    for (const auto &it : newValues) {
        auto &value = current[it.first];
        if (wasEmpty) {
            current[it.first] = it.second;
        } else {
            value.unite(it.second);
        }
    }
}

template <typename Memory, typename DB>
static inline int writeReferencesOrTargets(const Memory &m, const std::shared_ptr<DB> &db)
{
    int ret = 0;
    const bool wasEmpty = db->isEmpty();
    for (const auto &val : m) {
        if (wasEmpty) {
            db->set(val.first, val.second);
            ++ret;
        } else {
            auto cur = db->find(val.first);
            if (!cur->isValid()) {
                db->set(val.first, val.second);
                ++ret;
            } else {
                auto vals = cur->value();
                int count = 0;
                vals.unite(val.second, &count);
                if (count) {
                    db->set(val.first, val.second);
                    ++ret;
                }
            }
        }
    }
    return ret;

}

bool Project::isIndexed(uint32_t fileId) const
{
    if (mVisitedFiles.contains(fileId))
        return true;

    if (mSources) {
        const uint64_t key = Source::key(fileId, 0);
        auto it = mSources->lower_bound(key);
        if (it->isValid()) {
            uint32_t f, b;
            Source::decodeKey(it->key(), f, b);
            if (f == fileId)
                return true;
        }
    }
    return false;
}

const Set<uint32_t> &Project::suspendedFiles() const
{
    return mSuspendedFiles;
}

void Project::clearSuspendedFiles()
{
    mSuspendedFiles.clear();
}

bool Project::toggleSuspendFile(uint32_t file)
{
    if (!mSuspendedFiles.insert(file)) {
        mSuspendedFiles.remove(file);
        return false;
    }
    return true;
}

bool Project::isSuspended(uint32_t file) const
{
    return mSuspendedFiles.contains(file);
}

void Project::addFixIts(const DependencyMapMemory &visited, const FixItMap &fixIts)
{
    for (auto it = visited.begin(); it != visited.end(); ++it) {
        const auto fit = fixIts.find(it->first);
        if (fit == fixIts.end()) {
            mFixIts.remove(it->first);
        } else {
            mFixIts[it->first] = fit->second;
        }
    }
}

String Project::fixIts(uint32_t fileId) const
{
    const auto it = mFixIts.find(fileId);
    String out;
    if (it != mFixIts.end()) {
        const Set<FixIt> &fixIts = it->second;
        if (!fixIts.isEmpty()) {
            auto f = fixIts.end();
            do {
                --f;
                if (!out.isEmpty())
                    out.append('\n');
                out.append(String::format<32>("%d:%d %d %s", f->line, f->column, f->length, f->text.constData()));

            } while (f != fixIts.begin());
        }
    }
    return out;
}

bool Project::startSync(SyncMode mode)
{
    if (!Server::instance()->options().tests.isEmpty())
        mode = Sync_Synchronous;
    if (mState != Loaded) {
        if (mode == Sync_Asynchronous)
            mSyncTimer.restart(SyncTimeout, Timer::SingleShot);
        return false;
    }
    assert(mState == Loaded);
    mState = Syncing;
    mSyncTimer.stop();
    if (mode == Sync_Synchronous) {
        const String msg = sync();
        if (!msg.isEmpty())
            error() << msg;
        onSynced();
    } else {
        SyncThread *thread = new SyncThread(shared_from_this());
        thread->start();
    }
    return true;
}

void Project::reloadFileManager()
{
    fileManager->reload(FileManager::Asynchronous);
}

enum MatchSymbolNameMode {
    MaybeFunction,
    NonFunction
};

static inline MatchSymbolNameMode checkFunction(unsigned int kind)
{
    switch (kind) {
    case CXCursor_VarDecl:
    case CXCursor_ParmDecl:
        return MaybeFunction;
    default:
        break;
    }
    return NonFunction;
}

static inline bool matchSymbolName(const String &needle, const String &haystack, MatchSymbolNameMode checkFunction)
{
    int start = 0;
    if (checkFunction == MaybeFunction) {
        // we generate symbols for arguments and local variables in functions
        // . E.g. there's a symbol with the symbolName:
        // bool matchSymbolName(String &, String &, bool)::checkFunction
        // we don't want to match when we're searching for "matchSymbolName" so
        // we start searching at the index of ):: if we're a function. That is
        // unless you really sent in an exact match. In that case you deserve a
        // hit.
        if (needle == haystack)
            return true;

        start = haystack.indexOf(")::");
        if (start != -1) {
            start += 2;
        } else {
            start = 0;
        }
    }
    // We automagically generate symbols with stripped argument lists
    if (!strncmp(needle.constData(), haystack.constData() + start, needle.size())
        && (haystack.size() - start == needle.size() || haystack.at(start + needle.size()) == '(')) {
        return true;
    }
    return false;
}

Set<Location> Project::locations(const String &symbolName, uint32_t fileId) const
{
    Set<Location> ret;
    if (fileId) {
        const SymbolMapMemory s = symbols(fileId);
        for (auto it = s.begin(); it != s.end(); ++it) {
            if (!RTags::isReference(it->second->kind)
                && (symbolName.isEmpty() || matchSymbolName(symbolName, it->second->symbolName, checkFunction(it->second->kind)))) {
                ret.insert(it->first);
            }
        }
    } else if (symbolName.isEmpty()) {
        for (auto it = mSymbols->createIterator(); it->isValid(); it->next()) {
            if (!RTags::isReference(it->value()->kind))
                ret.insert(it->key());
        }
    } else {
        auto it = mSymbolNames->lower_bound(symbolName);
        while (it->isValid() && it->key().startsWith(symbolName)) {
            if (matchSymbolName(symbolName, it->key(), MaybeFunction)) // assume function
                ret.unite(it->value());
            it->next();
        }
    }
    return ret;
}

List<RTags::SortedCursor> Project::sort(const Set<Location> &locations, unsigned int flags) const
{
    List<RTags::SortedCursor> sorted;
    sorted.reserve(locations.size());
    for (auto it = locations.begin(); it != locations.end(); ++it) {
        RTags::SortedCursor node(*it);
        const auto found = mSymbols->find(*it);
        if (found->isValid()) {
            node.isDefinition = found->value()->isDefinition();
            if (flags & Sort_DeclarationOnly && node.isDefinition) {
                const std::shared_ptr<CursorInfo> decl = found->value()->bestTarget();
                if (decl && !decl->isNull())
                    continue;
            }
            node.kind = found->value()->kind;
        }
        sorted.push_back(node);
    }

    if (flags & Sort_Reverse) {
        std::sort(sorted.begin(), sorted.end(), std::greater<RTags::SortedCursor>());
    } else {
        std::sort(sorted.begin(), sorted.end());
    }
    return sorted;
}

SymbolMapMemory Project::symbols(uint32_t fileId) const
{
    SymbolMapMemory ret;
    if (fileId) {
        for (auto it = mSymbols->lower_bound(Location(fileId, 1, 0));
             it->isValid() && it->key().fileId() == fileId; it->next()) {
            ret[it->key()] = it->value();
        }
    }
    return ret;
}

void Project::watch(const Path &file)
{
    Path dir = file.parentDir();
    if (dir.isEmpty()) {
        error() << "Got empty parent dir for" << file;
    } else {
        if (mWatchedPaths.contains(dir))
            return;
        dir.resolve();
        if (((Server::instance()->options().options & Server::WatchSystemPaths) || !dir.isSystem())
            && mWatchedPaths.insert(dir)) {
            mWatcher.watch(dir);
        }
    }
}

void Project::onSynced()
{
    assert(mState == Syncing);
    mState = Loaded;
    for (const auto &it : mPendingIndexData) {
        onJobFinished(it.second.first, it.second.second);
    }
    mPendingIndexData.clear();
    for (const auto &job : mPendingJobs) {
        index(job);
    }
    mPendingJobs.clear();
}

String Project::sync()
{
    mJobCounter = mActiveJobs.size();
    StopWatch sw;
    if (mDirtyFiles.isEmpty() && mIndexData.isEmpty()) {
        return String();
    }


    if (!mDirtyFiles.isEmpty()) {
        auto symbolsWriteScope = mSymbols->createWriteScope(1024 * 1024);
        auto symbolNameWriteScope = mSymbolNames->createWriteScope(1024 * 1024);
        auto usrScope = mUsr->createWriteScope(1024 * 1024);
        auto referencesWriteScope = mReferences->createWriteScope(1024 * 1024);
        auto targetsWriteScope = mTargets->createWriteScope(1024 * 1024);

        RTags::dirtySymbols(mSymbols, mDirtyFiles);
        RTags::dirtyReferences(mReferences, mDirtyFiles);
        RTags::dirtyTargets(mTargets, mDirtyFiles);
        RTags::dirtySymbolNames(mSymbolNames, mDirtyFiles);
        RTags::dirtyUsr(mUsr, mDirtyFiles);
        mDirtyFiles.clear();
    }
    const int dirtyTime = sw.restart();

    Set<uint32_t> newFiles;
    List<const UsrMapMemory *> pendingReferences;
    int symbols = 0;
    int symbolNames = 0;
    int references = 0;
    int targets = 0;
    ReferencesMapMemory allReferences;
    TargetsMapMemory allTargets;
    auto it = mIndexData.begin();
    auto symbolsScope = mSymbols->createWriteScope(1024 * 1024 * 4);
    UsrMapMemory allUsrs;
    while (true) {
        const std::shared_ptr<IndexData> &data = it->second;
        addDependencies(data->dependencies, newFiles);
        addFixIts(data->dependencies, data->fixIts);
        uniteUnite(allUsrs, data->usrs);
        symbols += writeSymbols(data->symbols, mSymbols);
        symbolNames += writeSymbolNames(data->symbolNames, mSymbolNames);
        // error() << data->references << allReferences.size() << Location::path(data->fileId());
        uniteUnite(allReferences, data->references);
        uniteUnite(allTargets, data->targets);

        if (!data->pendingReferenceMap.isEmpty())
            pendingReferences.append(&data->pendingReferenceMap);
        if (++it == mIndexData.end()) {
            symbolsScope.reset();
            writeUsr(allUsrs, mUsr, allTargets);
            auto referencesWriteScope = mReferences->createWriteScope(1024 * 1024);
            auto targetsWriteScope = mTargets->createWriteScope(1024 * 1024);
            auto usrScope = mUsr->createWriteScope(1024 * 1024);
            for (const UsrMapMemory *map : pendingReferences)
                resolvePendingReferences(mSymbols, mUsr, *map, allTargets, allReferences);
            references = writeReferencesOrTargets(allReferences, mReferences);
            targets = writeReferencesOrTargets(allTargets, mTargets);
            break;
        }
    }

    for (auto it = newFiles.constBegin(); it != newFiles.constEnd(); ++it) {
        watch(Location::path(*it));
    }
    const int syncTime = sw.restart();
    for (int i=0; i<3 && !Server::instance()->saveFileIds(); ++i) {
        // this has to work or we're screwed
        usleep(1000);
    }

    {
        String visited;
        Serializer serialize(visited);
        serialize << mVisitedFiles;

        auto writeScope = mGeneral->createWriteScope(1024 * 256);
        mGeneral->set("visitedFiles", visited);
    }

    const int saveTime = sw.elapsed();
    double timerElapsed = (mTimer.elapsed() / 1000.0);
    const double averageJobTime = timerElapsed / mIndexData.size();
    const String msg = String::format<1024>("Jobs took %.2fs, %sdirtying took %.2fs, "
                                            "syncing took %.2fs, saving took %.2fs. We're using %lldmb of memory. "
                                            "%d symbols, %d targets, %d references, %d symbolNames", timerElapsed,
                                            mIndexData.size() > 1 ? String::format("(avg %.2fs), ", averageJobTime).constData() : "",
                                            dirtyTime / 1000.0, syncTime / 1000.0, saveTime / 1000.0, MemoryMonitor::usage() / (1024 * 1024),
                                            symbols, targets, references, symbolNames);
    mIndexData.clear();
    mTimer.start();
    return msg;
};
