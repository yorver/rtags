/* This file is part of RTags (http://rtags.net).

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

#include "ProcThread.h"

ProcThread::ProcThread(int interval)
    : Thread(), mInterval(interval)
{
#ifdef RTAGS_HAS_PROC
    mPath = "/proc/";
#endif
}

ProcThread::~ProcThread()
{
}

void ProcThread::run()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (!mInterval)
                break;
            if (mCond.wait_for(lock, std::chrono::milliseconds(mInterval)) != std::cv_status::timeout)
                break;
        }
        readProc();
    }
}

void ProcThread::stop()
{
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mInterval = 0;
        mCond.notify_one();
    }
    join();
}

void ProcThread::readProc()
{
    for (auto &pair : mSeen) {
        assert(!pair.second.marked);
        pair.second.marked = true;
    }
#ifdef RTAGS_HAS_PROC
    mPath.visit([](const Path &path) -> Path::VisitResult {
            bool ok;
            unsigned long long pid = path.toULongLong(&ok);
            if (ok) {
                char path[4096];
                snprintf(path, sizeof(path), "%scmdline", path.constData());
                FILE *f = fopen(path, "r");
                if (f) {
                    char buf[4096];
                    size_t read = fread(buf, 1, sizeof(buf), f);

                    fclose(f);
                }
            }
            return Path::Continue;
        });
#endif
}
