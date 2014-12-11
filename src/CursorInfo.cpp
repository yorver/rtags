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


#include "CursorInfo.h"
#include "RTagsClang.h"
#include "Project.h"

String CursorInfo::kindSpelling(uint16_t kind)
{
    return RTags::eatString(clang_getCursorKindSpelling(static_cast<CXCursorKind>(kind)));
}

String CursorInfo::toString(unsigned cursorInfoFlags, unsigned keyFlags) const
{
    String ret = String::format<1024>("SymbolName: %s\n"
                                      "Kind: %s\n"
                                      "Type: %s\n" // type
                                      "SymbolLength: %u\n"
                                      "%s" // range
                                      "%s" // enumValue
                                      "%s", // definition
                                      symbolName.constData(),
                                      kindSpelling().constData(),
                                      RTags::eatString(clang_getTypeKindSpelling(type)).constData(),
                                      symbolLength,
                                      startLine != -1 ? String::format<32>("Range: %d:%d-%d:%d\n", startLine, startColumn, endLine, endColumn).constData() : "",
#if CINDEX_VERSION_MINOR > 1
                                      kind == CXCursor_EnumConstantDecl ? String::format<32>("Enum Value: %lld\n", enumValue).constData() :
#endif
                                      "",
                                      isDefinition() ? "Definition\n" : "");

    if (!targets.isEmpty() && !(cursorInfoFlags & IgnoreTargets)) {
        ret.append("Targets:\n");
        for (auto tit = targets.begin(); tit != targets.end(); ++tit) {
            const Location &l = tit->first;
            ret.append(String::format<128>("    %s\n", l.key(keyFlags).constData()));
        }
    }

    if (!references.isEmpty() && !(cursorInfoFlags & IgnoreReferences)) {
        ret.append("References:\n");
        for (auto rit = references.begin(); rit != references.end(); ++rit) {
            const Location &l = *rit;
            ret.append(String::format<128>("    %s\n", l.key(keyFlags).constData()));
        }
    }
    return ret;
}

int CursorInfo::targetRank(CXCursorKind kind)
{
    switch (kind) {
    case CXCursor_Constructor: // this one should be more than class/struct decl
        return 1;
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_ClassTemplate:
        return 0;
    case CXCursor_FieldDecl:
    case CXCursor_VarDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
        // functiondecl and cxx method must be more than cxx
        // CXCursor_FunctionTemplate. Since constructors for templatatized
        // objects seem to come out as function templates
        return 4;
    case CXCursor_MacroDefinition:
        return 5;
    case CXCursor_TypeRef:
        return 3;
    default:
        return 2;
    }
}

String CursorInfo::displayName() const
{
    switch (kind) {
    case CXCursor_FunctionTemplate:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_Constructor: {
        const int end = symbolName.indexOf('(');
        if (end != -1)
            return symbolName.left(end);
        break; }
    case CXCursor_FieldDecl: {
        int colon = symbolName.indexOf(':');
        if (colon != -1) {
            const int end = colon + 2;
            while (colon > 0 && RTags::isSymbol(symbolName.at(colon - 1)))
                --colon;
            return symbolName.left(colon + 1) + symbolName.mid(end);
        }
        break; }
    default:
        break;
    }
    return symbolName;
}

std::shared_ptr<CursorInfo> CursorInfo::copy() const
{
    return std::shared_ptr<CursorInfo>(std::make_shared<CursorInfo>(*this));
}

bool CursorInfo::isReference(unsigned int kind)
{
    return RTags::isReference(kind);
}

std::shared_ptr<CursorInfo> CursorInfo::findCursorInfo(const std::shared_ptr<Project> &project,
                                                       const Location &location,
                                                       std::unique_ptr<SymbolMap::Iterator> *iterator)
{
    auto symbols = project->symbols();
    auto it = symbols->lower_bound(location);
    if (it->isValid()) {
        if (it->key() == location) {
            auto ret = it->value()->populate(location, project);
            if (iterator)
                *iterator = std::move(it);
            return ret;
        }
        // error() << "Found a thing for" << location << it->key();
        it->prev();
    } else {
        it->seekToEnd();
        // error() << "Seeking to end";
    }

    if (!it->isValid()) {
        if (iterator)
            *iterator = std::move(it);
        return std::shared_ptr<CursorInfo>();
    }

    // error() << "Now looking at" << it->key();

    if (it->key().fileId() == location.fileId() && location.line() == it->key().line()) {
        const int off = location.column() - it->key().column();
        // error() << "off" << off << it->value()->symbolLength;
        if (it->value()->symbolLength > off) {
            auto ret = it->value()->populate(it->key(), project);
            if (iterator)
                *iterator = std::move(it);
            return ret;
        }
    }
    if (iterator)
        *iterator = std::move(symbols->createIterator(symbols->Invalid));
    return std::shared_ptr<CursorInfo>();
}

SymbolMapMemory CursorInfo::allReferences() const
{
    assert(project);
    SymbolMapMemory ret;
    Mode mode = NormalRefs;
    switch (kind) {
    case CXCursor_Constructor:
    case CXCursor_Destructor:
        mode = ClassRefs;
        break;
    case CXCursor_CXXMethod:
        mode = VirtualRefs;
        break;
    default:
        mode = isClass() ? ClassRefs : VirtualRefs;
        break;
    }

    allImpl(copy(), ret, mode, kind);
    return ret;
}

SymbolMapMemory CursorInfo::virtuals() const
{
    assert(project);
    SymbolMapMemory ret;
    ret[location] = populate(location, project);
    const SymbolMapMemory s = (kind == CXCursor_CXXMethod ? allReferences() : targetInfos());
    for (auto it = s.begin(); it != s.end(); ++it) {
        if (it->second->kind == kind) {
            assert(it->second->project);
            ret[it->first] = it->second;
        }
    }
    return ret;
}

std::shared_ptr<CursorInfo> CursorInfo::bestTarget() const
{
    assert(project);
    const SymbolMapMemory targets = targetInfos();
    auto best = targets.end();
    int bestRank = -1;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const std::shared_ptr<CursorInfo> &ci = it->second;
        const int r = CursorInfo::targetRank(static_cast<CXCursorKind>(ci->kind));
        if (r > bestRank || (r == bestRank && ci->isDefinition())) {
            bestRank = r;
            best = it;
        }
    }
    if (best != targets.end()) {
        return best->second->populate(best->first, project);
    }
    return std::shared_ptr<CursorInfo>();
}

SymbolMapMemory CursorInfo::targetInfos() const
{
    assert(project);
    SymbolMapMemory ret;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        auto found = CursorInfo::findCursorInfo(project, it->first);
        if (found) {
            ret[it->first] = found;
        } else {
            ret[it->first] = std::make_shared<CursorInfo>();
            // we need this one for inclusion directives which target a
            // non-existing CursorInfo
        }
    }
    return ret;
}

SymbolMapMemory CursorInfo::referenceInfos() const
{
    assert(project);
    SymbolMapMemory ret;
    for (auto it = references.begin(); it != references.end(); ++it) {
        auto found = CursorInfo::findCursorInfo(project, *it);
        if (found)
            ret[*it] = found;
    }
    return ret;
}

SymbolMapMemory CursorInfo::callers() const
{
    assert(project);
    SymbolMapMemory ret;
    const SymbolMapMemory cursors = virtuals();
    const bool isClazz = isClass();
    for (auto c = cursors.begin(); c != cursors.end(); ++c) {
        for (auto it = c->second->references.begin(); it != c->second->references.end(); ++it) {
            const auto found = CursorInfo::findCursorInfo(project, *it);
            if (!found)
                continue;
            if (isClazz && found->kind == CXCursor_CallExpr)
                continue;
            if (CursorInfo::isReference(found->kind)) { // is this always right?
                ret[*it] = found;
            } else if (kind == CXCursor_Constructor && (found->kind == CXCursor_VarDecl || found->kind == CXCursor_FieldDecl)) {
                ret[*it] = found;
            }
        }
    }
    return ret;
}


void CursorInfo::allImpl(const std::shared_ptr<CursorInfo> &info, SymbolMapMemory &out, Mode mode, unsigned kind)
{
    assert(info);
    assert(info->project);
    assert(!info->location.isNull());
    if (out.contains(info->location))
        return;
    out[info->location] = info;
    const SymbolMapMemory targets = info->targetInfos();
    for (auto t = targets.begin(); t != targets.end(); ++t) {
        bool ok = false;
        switch (mode) {
        case VirtualRefs:
        case NormalRefs:
            ok = (t->second->kind == kind);
            break;
        case ClassRefs:
            ok = (t->second->isClass() || t->second->kind == CXCursor_Destructor || t->second->kind == CXCursor_Constructor);
            break;
        }
        if (ok)
            allImpl(t->second, out, mode, kind);
    }
    const SymbolMapMemory refs = info->referenceInfos();
    for (auto r = refs.begin(); r != refs.end(); ++r) {
        switch (mode) {
        case NormalRefs:
            out[r->first] = r->second;
            break;
        case VirtualRefs:
            if (r->second->kind == kind) {
                allImpl(r->second, out, mode, kind);
            } else {
                out[r->first] = r->second;
            }
            break;
        case ClassRefs:
            if (info->isClass()) // for class/struct we want the references inserted directly regardless and also recursed
                out[r->first] = r->second;
            if (r->second->isClass()
                || r->second->kind == CXCursor_Destructor
                || r->second->kind == CXCursor_Constructor) { // if is a constructor/destructor/class reference we want to recurse it
                allImpl(r->second, out, mode, kind);
            }
        }
    }
}

std::shared_ptr<CursorInfo> CursorInfo::populate(const Location &location,
                                                 const std::shared_ptr<Project> &project) const
{
    auto ret = copy();
    ret->project = project;
    ret->location = location;
    ret->targets = project->targets()->value(location);
    ret->references = project->references()->value(location);
    return ret;
}
