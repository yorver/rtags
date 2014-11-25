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

#include "CursorInfoJob.h"
#include "RTags.h"
#include "Server.h"
#include "CursorInfo.h"
#include "Project.h"
#include "QueryMessage.h"

CursorInfoJob::CursorInfoJob(const Location &loc, const std::shared_ptr<QueryMessage> &query, const std::shared_ptr<Project> &proj)
    : QueryJob(query, 0, proj), location(loc)
{
}

int CursorInfoJob::execute()
{
    std::shared_ptr<SymbolMap> map = project()->symbols();
    if (map->isEmpty())
        return 1;
    auto it = CursorInfo::findCursorInfo(map, location);

    unsigned ciFlags = 0;
    if (!(queryFlags() & QueryMessage::CursorInfoIncludeTargets))
        ciFlags |= CursorInfo::IgnoreTargets;
    if (!(queryFlags() & QueryMessage::CursorInfoIncludeReferences))
        ciFlags |= CursorInfo::IgnoreReferences;
    if (!it->isValid())
        return 1;
    write(it->key());
    write(it->value(), ciFlags);
    if (queryFlags() & QueryMessage::CursorInfoIncludeParents) {
        ciFlags |= CursorInfo::IgnoreTargets|CursorInfo::IgnoreReferences;
        const uint32_t fileId = location.fileId();
        const unsigned int line = location.line();
        const unsigned int column = location.column();
        while (true) {
            it->prev();
            if (it->key().fileId() != fileId)
                break;
            if (it->value()->isDefinition()
                && RTags::isContainer(it->value()->kind)
                && comparePosition(line, column, it->value()->startLine, it->value()->startColumn) >= 0
                && comparePosition(line, column, it->value()->endLine, it->value()->endColumn) <= 0) {
                write("====================");
                write(it->key());
                write(it->value(), ciFlags);
                break;
            }
        }
    }
    return 0;
}
