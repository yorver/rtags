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

#ifndef CursorInfo_h
#define CursorInfo_h

#include <rct/String.h>
#include "Location.h"
#include <rct/Path.h>
#include <rct/Log.h>
#include <rct/List.h>
#include <clang-c/Index.h>
#include <rct/DB.h>

class CursorInfo;
typedef DB<Location, std::shared_ptr<CursorInfo> > SymbolMap; // duplicated from RTags.h
typedef Map<Location, std::shared_ptr<CursorInfo> > SymbolMapMemory; // duplicated from RTags.h
class CursorInfo
{
public:
    CursorInfo()
        : symbolLength(0), kind(CXCursor_FirstInvalid), type(CXType_Invalid), enumValue(0),
          startLine(-1), startColumn(-1), endLine(-1), endColumn(-1)
    {}

    void clear()
    {
        symbolLength = 0;
        kind = CXCursor_FirstInvalid;
        type = CXType_Invalid;
        enumValue = 0;
        targets.clear();
        references.clear();
        symbolName.clear();
    }

    String kindSpelling() const { return kindSpelling(kind); }
    static String kindSpelling(uint16_t kind);
    bool dirty(const Set<uint32_t> &dirty)
    {
        bool changed = false;
        Set<Location> *locations[] = { &targets, &references };
        for (int i=0; i<2; ++i) {
            Set<Location> &l = *locations[i];
            Set<Location>::iterator it = l.begin();
            while (it != l.end()) {
                if (dirty.contains(it->fileId())) {
                    changed = true;
                    l.erase(it++);
                } else {
                    ++it;
                }
            }
        }
        return changed;
    }

    String displayName() const;

    int targetRank(const std::shared_ptr<CursorInfo> &target) const;

    bool isValid() const
    {
        return !isEmpty();
    }

    bool isNull() const
    {
        return isEmpty();
    }

    template <typename SymbolContainer>
    std::shared_ptr<CursorInfo> bestTarget(const SymbolContainer &map, Location *loc = 0) const;
    template <typename SymbolContainer>
    SymbolMapMemory targetInfos(const SymbolContainer &map) const;
    template <typename SymbolContainer>
    SymbolMapMemory referenceInfos(const SymbolContainer &map) const;
    template <typename SymbolContainer>
    SymbolMapMemory callers(const Location &loc, const SymbolContainer &map) const;
    template <typename SymbolContainer>
    SymbolMapMemory allReferences(const Location &loc, const SymbolContainer &map) const;
    template <typename SymbolContainer>
    SymbolMapMemory virtuals(const Location &loc, const SymbolContainer &map) const;
    template <typename SymbolContainer>
    SymbolMapMemory declarationAndDefinition(const Location &loc, const SymbolContainer &map) const;

    template <typename SymbolContainer>
    static typename SymbolContainer::const_iterator findCursorInfo(const SymbolContainer &map, const Location &location)
    {
        typename SymbolContainer::const_iterator it = map.lower_bound(location);
        if (it != map.end() && it->first == location) {
            return it;
        } else if (it != map.begin()) {
            --it;
            if (it->first.fileId() == location.fileId() && location.line() == it->first.line()) {
                const int off = location.column() - it->first.column();
                if (it->second->symbolLength > off)
                    return it;
            }
        }
        return map.end();
    }

    std::shared_ptr<CursorInfo> copy() const;

    bool isClass() const
    {
        switch (kind) {
        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_StructDecl:
            return true;
        default:
            break;
        }
        return false;
    }

    inline bool isDefinition() const
    {
        return kind == CXCursor_EnumConstantDecl || definition;
    }

    bool isEmpty() const
    {
        return !symbolLength && targets.isEmpty() && references.isEmpty();
    }

    bool unite(const std::shared_ptr<CursorInfo> &other)
    {
        bool changed = false;
        if (targets.isEmpty() && !other->targets.isEmpty()) {
            targets = other->targets;
            changed = true;
        } else if (!other->targets.isEmpty()) {
            int count = 0;
            targets.unite(other->targets, &count);
            if (count)
                changed = true;
        }

        if (startLine == -1 && other->startLine != -1) {
            startLine = other->startLine;
            startColumn = other->startColumn;
            endLine = other->endLine;
            endColumn = other->endColumn;
            changed = true;
        }

        if (!symbolLength && other->symbolLength) {
            symbolLength = other->symbolLength;
            kind = other->kind;
            enumValue = other->enumValue;
            type = other->type;
            symbolName = other->symbolName;
            changed = true;
        }
        const int oldSize = references.size();
        if (!oldSize) {
            references = other->references;
            if (!references.isEmpty())
                changed = true;
        } else {
            int inserted = 0;
            references.unite(other->references, &inserted);
            if (inserted)
                changed = true;
        }

        return changed;
    }

    template <typename T>
    static inline void serialize(T &s, const SymbolMapMemory &t);
    template <typename T>
    static inline void deserialize(T &s, SymbolMapMemory &t);

    enum Flag {
        IgnoreTargets = 0x1,
        IgnoreReferences = 0x2,
        DefaultFlags = 0x0
    };
    String toString(unsigned cursorInfoFlags = DefaultFlags, unsigned keyFlags = 0) const;
    uint16_t symbolLength; // this is just the symbol name length e.g. foo => 3
    String symbolName; // this is fully qualified Foobar::Barfoo::foo
    uint16_t kind;
    CXTypeKind type;
    union {
        bool definition;
        int64_t enumValue; // only used if type == CXCursor_EnumConstantDecl
    };
    Set<Location> targets, references;
    int startLine, startColumn, endLine, endColumn;
private:
    enum Mode {
        ClassRefs,
        VirtualRefs,
        NormalRefs
    };
    template <typename SymbolContainer>
    static void allImpl(const SymbolContainer &map, const Location &loc, const std::shared_ptr<CursorInfo> &info, SymbolMapMemory &out, Mode mode, unsigned kind);
    static bool isReference(unsigned int kind);
};

template <> inline Serializer &operator<<(Serializer &s, const CursorInfo &t)
{
    s << t.symbolLength << t.symbolName << static_cast<int>(t.kind)
      << static_cast<int>(t.type) << t.enumValue << t.targets << t.references
      << t.startLine << t.startColumn << t.endLine << t.endColumn;
    return s;
}

template <> inline Deserializer &operator>>(Deserializer &s, CursorInfo &t)
{
    int kind, type;
    s >> t.symbolLength >> t.symbolName >> kind >> type
      >> t.enumValue >> t.targets >> t.references
      >> t.startLine >> t.startColumn >> t.endLine >> t.endColumn;
    t.kind = static_cast<CXCursorKind>(kind);
    t.type = static_cast<CXTypeKind>(type);
    return s;
}

template <typename T>
inline void CursorInfo::serialize(T &s, const SymbolMapMemory &t)
{
    const uint32_t size = t.size();
    s << size;
    for (const auto &it : t)
        s << it.first << *it.second;
}

template <typename T>
inline void CursorInfo::deserialize(T &s, SymbolMapMemory &t)
{
    uint32_t size;
    s >> size;
    t.clear();
    while (size--) {
        Location location;
        s >> location;
        std::shared_ptr<CursorInfo> &ci = t[location];
        ci = std::make_shared<CursorInfo>();
        s >> *ci;
    }
}

inline Log operator<<(Log log, const CursorInfo &info)
{
    log << info.toString();
    return log;
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::allReferences(const Location &loc, const SymbolContainer &map) const
{
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

    allImpl(map, loc, copy(), ret, mode, kind);
    return ret;
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::virtuals(const Location &loc, const SymbolContainer &map) const
{
    SymbolMapMemory ret;
    ret[loc] = copy();
    const SymbolMapMemory s = (kind == CXCursor_CXXMethod ? allReferences(loc, map) : targetInfos(map));
    for (auto it = s.begin(); it != s.end(); ++it) {
        if (it->second->kind == kind)
            ret[it->first] = it->second;
    }
    return ret;
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::declarationAndDefinition(const Location &loc, const SymbolContainer &map) const
{
    SymbolMapMemory cursors;
    cursors[loc] = copy();

    Location l;
    const std::shared_ptr<CursorInfo> t = bestTarget(map, &l);

    if (t->kind == kind)
        cursors[l] = t;
    return cursors;
}

template <typename SymbolContainer>
inline std::shared_ptr<CursorInfo> CursorInfo::bestTarget(const SymbolContainer &map, Location *loc) const
{
    const SymbolMapMemory targets = targetInfos(map);

    auto best = targets.end();
    int bestRank = -1;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const std::shared_ptr<CursorInfo> &ci = it->second;
        const int r = targetRank(ci);
        if (r > bestRank || (r == bestRank && ci->isDefinition())) {
            bestRank = r;
            best = it;
        }
    }
    if (best != targets.end()) {
        if (loc)
            *loc = best->first;
        return best->second;
    }
    return std::shared_ptr<CursorInfo>();
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::targetInfos(const SymbolContainer &map) const
{
    SymbolMapMemory ret;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        auto found = CursorInfo::findCursorInfo(map, *it);
        if (found != map.end()) {
            ret[*it] = found->second;
        } else {
            ret[*it] = std::make_shared<CursorInfo>();
            // we need this one for inclusion directives which target a
            // non-existing CursorInfo
        }
    }
    return ret;
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::referenceInfos(const SymbolContainer &map) const
{
    SymbolMapMemory ret;
    for (auto it = references.begin(); it != references.end(); ++it) {
        auto found = CursorInfo::findCursorInfo(map, *it);
        if (found != map.end()) {
            ret[*it] = found->second;
        }
    }
    return ret;
}

template <typename SymbolContainer>
inline SymbolMapMemory CursorInfo::callers(const Location &loc, const SymbolContainer &map) const
{
    SymbolMapMemory ret;
    const SymbolMapMemory cursors = virtuals(loc, map);
    const bool isClazz = isClass();
    for (auto c = cursors.begin(); c != cursors.end(); ++c) {
        for (auto it = c->second->references.begin(); it != c->second->references.end(); ++it) {
            const auto found = CursorInfo::findCursorInfo(map, *it);
            if (found == map.end())
                continue;
            if (isClazz && found->second->kind == CXCursor_CallExpr)
                continue;
            if (CursorInfo::isReference(found->second->kind)) { // is this always right?
                ret[*it] = found->second;
            } else if (kind == CXCursor_Constructor && (found->second->kind == CXCursor_VarDecl || found->second->kind == CXCursor_FieldDecl)) {
                ret[*it] = found->second;
            }
        }
    }
    return ret;
}


template <typename SymbolContainer>
inline void CursorInfo::allImpl(const SymbolContainer &map, const Location &loc, const std::shared_ptr<CursorInfo> &info,
                                SymbolMapMemory &out, Mode mode, unsigned kind)
{
    if (out.contains(loc))
        return;
    out[loc] = info;
    const SymbolMapMemory targets = info->targetInfos(map);
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
            allImpl(map, t->first, t->second, out, mode, kind);
    }
    const SymbolMapMemory refs = info->referenceInfos(map);
    for (auto r = refs.begin(); r != refs.end(); ++r) {
        switch (mode) {
        case NormalRefs:
            out[r->first] = r->second;
            break;
        case VirtualRefs:
            if (r->second->kind == kind) {
                allImpl(map, r->first, r->second, out, mode, kind);
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
                allImpl(map, r->first, r->second, out, mode, kind);
            }
        }
    }
}

#endif
