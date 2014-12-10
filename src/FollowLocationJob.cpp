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

#include "FollowLocationJob.h"
#include "RTags.h"
#include "Server.h"
#include "CursorInfo.h"
#include "Project.h"

FollowLocationJob::FollowLocationJob(const Location &loc, const std::shared_ptr<QueryMessage> &query, const std::shared_ptr<Project> &project)
    : QueryJob(query, 0, project), location(loc)
{
}

int FollowLocationJob::execute()
{
    auto cursorInfo = CursorInfo::findCursorInfo(project(), location);

    if (!cursorInfo) {
        return 1;
    }

    if (cursorInfo->isClass() && cursorInfo->isDefinition()) {
        return 2;
    }

    SymbolMapMemory targets;
    if (cursorInfo->kind == CXCursor_ObjCMessageExpr) {
        auto symbols = project()->symbols();
        for (const auto &loc : cursorInfo->targets) {
            const auto target = symbols->value(loc.first);
            if (target)
                targets[loc.first] = target;

        }
    } else {
        std::shared_ptr<CursorInfo> target = cursorInfo->bestTarget();

        if (target) {
            targets[target->location] = target;
        }
    }
    int ret = 1;
    for (const auto &t : targets) {
        auto target = t.second;
        Location loc = t.first;
        if (cursorInfo->kind != target->kind) {
            if (!target->isDefinition() && !target->targets.isEmpty()) {
                switch (target->kind) {
                case CXCursor_ClassDecl:
                case CXCursor_ClassTemplate:
                case CXCursor_StructDecl:
                case CXCursor_FunctionDecl:
                case CXCursor_CXXMethod:
                case CXCursor_Destructor:
                case CXCursor_Constructor:
                case CXCursor_FunctionTemplate:
                    target = target->bestTarget();
                    if (target)
                        loc = target->location;
                    break;
                default:
                    break;
                }
            }
        }
        if (!loc.isNull()) {
            if (queryFlags() & QueryMessage::DeclarationOnly && target->isDefinition()) {
                const std::shared_ptr<CursorInfo> decl = target->bestTarget();
                if (decl) {
                    write(decl->location);
                    ret = 0;
                }
            } else {
                write(loc);
                ret = 0;
            }
        }
    }
    return ret;
}
