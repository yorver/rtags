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

#ifndef ClangThread_h
#define ClangThread_h

#include <clang-c/Index.h>

#include "Project.h"
#include "QueryMessage.h"
#include "rct/Thread.h"
#include "Source.h"

class Connection;
struct Dep;
class ClangThread : public Thread
{
public:
    ClangThread(const std::shared_ptr<QueryMessage> &queryMessage, const Source &source, const std::shared_ptr<Connection> &conn);
    virtual void run() override;
    void abort() { std::unique_lock<std::mutex> lock(mMutex); mAborted = false; }
    bool isAborted() const { std::unique_lock<std::mutex> lock(mMutex); return mAborted; }
private:
    static CXChildVisitResult visitor(CXCursor cursor, CXCursor, CXClientData userData);
    CXChildVisitResult visit(const CXCursor &cursor);
    void checkIncludes(Location location, const CXCursor &cursor);

    void writeToConnetion(const String &message);
    Location createLocation(const CXSourceLocation &loc)
    {
        CXString fileName;
        unsigned int line, col;
        CXFile file;
        clang_getSpellingLocation(loc, &file, &line, &col, 0);
        if (file) {
            fileName = clang_getFileName(file);
        } else {
            return Location();
        }
        const char *fn = clang_getCString(fileName);
        assert(fn);
        if (!*fn || !strcmp("<built-in>", fn) || !strcmp("<command line>", fn)) {
            clang_disposeString(fileName);
            return Location();
        }
        Path path = RTags::eatString(fileName);
        uint32_t fileId = Location::fileId(path);
        if (!fileId) {
            path.resolve();
            fileId = Location::insertFile(path);
        }
        return Location(fileId, line, col);
    }

    Location createLocation(const CXCursor &cursor)
    {
        return createLocation(clang_getCursorLocation(cursor));
    }
    void handleInclude(Location loc, const CXCursor &cursor);
    void handleReference(Location loc, const CXCursor &ref);
    void checkIncludes();

    struct Type;
    struct Cursor {
        Cursor()
            : referenced(0), lexicalParent(0), semanticParent(0),
              canonical(0), definition(0), specializedCursorTemplate(0),
              bitWidth(0), flags(0)
        {}
        Location location, rangeStart, rangeEnd;
        Path includedFile;
        String usr, kind, linkage, availability, spelling, displayName, mangledName, templateKind;
        Cursor *referenced, *lexicalParent, *semanticParent, *canonical, *definition, *specializedCursorTemplate;
        List<Cursor*> overridden, arguments, templateArgumentKinds, overloadedDecls;
        Type *type, *receiverType, *typedefUnderlyingType, *enumDeclIntegerType, *resultType;
        int bitWidth;
        List<Type*> templateArgumentTypes;
        List<long long> templateArgumentValues; // how about unsigned?

        enum Flag {
            None = 0x00,
            BitField = 0x001,
            VirtualBase = 0x002,
            Definition = 0x004,
            DynamicCall = 0x008,
            Variadic = 0x010,
            PureVirtual = 0x020,
            Virtual = 0x040,
            Static = 0x080,
            Const = 0x100
        };

        unsigned flags;
    };
    struct Type {
        Type()
            : flags(0), numElements(-1), sizeOf(-1)
        {}
        String kind, spelling, element, arrayElementType, referenceType;
        Type *underlyingType, *canonicalType, *pointeeType;
        Cursor *typeDeclaration;
        List<String> args, templateArgs;
        enum Flag {
            None = 0x00,
            Const = 0x01,
            Volatile = 0x02,
            Restrict = 0x04,
            Variadic = 0x08
        };
        unsigned flags;
        long long numElements, align, sizeOf;
    };
    Cursor *addCursor(CXCursor cursor, Location location = Location());
    Type *addType(CXType type);

    Hash<String, Cursor*> mCursorsByUsr;
    Map<Location, List<std::shared_ptr<Cursor> > > mCursors;
    Hash<String, std::shared_ptr<Type> > mTypes;

    List<std::pair<Location, Location> > mSkippedRanges;
    // ### diagnostics?

    const std::shared_ptr<QueryMessage> mQueryMessage;
    const Source mSource;
    std::shared_ptr<Connection> mConnection;
    int mIndentLevel;
    mutable std::mutex mMutex;
    Hash<uint32_t, Dep*> mDependencies;
    Hash<Path, String> mContextCache;
    bool mAborted;
};

#endif
