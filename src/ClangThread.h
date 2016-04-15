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
#include "rct/Value.h"
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
    struct Cursor;
    Cursor *visitAST(const CXCursor &cursor, Location location = Location());
    struct Type;
    Type *createType(const CXType &type);
    void dumpJSON(CXTranslationUnit unit);

    void writeToConnetion(const String &message);
    static Location createLocation(CXSourceLocation loc)
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

    static Location createLocation(const CXCursor &cursor)
    {
        return createLocation(clang_getCursorLocation(cursor));
    }
    void handleInclude(Location loc, const CXCursor &cursor);
    void handleReference(Location loc, const CXCursor &ref);
    void checkIncludes();

    struct Link {
        Link()
            : id(0)
        {}
        virtual ~Link()
        {}

        uint32_t id;
    };
    struct Cursor : public Link {
        Cursor()
            : referenced(0), lexicalParent(0), semanticParent(0),
              canonical(0), definition(0), specializedCursorTemplate(0),
              bitFieldWidth(-1), flags(0)
        {}
        Location location, rangeStart, rangeEnd;
        Path includedFile;
        String usr, kind, linkage, availability, spelling, displayName, mangledName, templateCursorKind;
        Cursor *referenced, *lexicalParent, *semanticParent, *canonical, *definition, *specializedCursorTemplate;
        List<Cursor*> overridden, arguments, overloadedDecls;
        int bitFieldWidth;
        struct TemplateArgument {
            String kind;
            long long value;
            unsigned long long unsignedValue;
            Type *type;
        };
        List<TemplateArgument> templateArguments;
        Type *type, *receiverType, *typedefUnderlyingType, *enumDeclIntegerType, *resultType;

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

        Value toValue() const;
    };
    List<Cursor*> mCursors;
    Hash<String, Cursor*> mCursorsByUsr;

    struct Type : public Link {
        Type()
            : canonicalType(0), pointeeType(0), resultType(0), elementType(0),
              arrayElementType(0), classType(0), typeDeclaration(0), flags(0),
              numElements(-1), arraySize(-1), align(-1), sizeOf(-1)
        {}
        String spelling, kind, element, referenceType, callingConvention;
        Type *canonicalType, *pointeeType, *resultType, *elementType, *arrayElementType, *classType;
        List<Type*> arguments, templateArguments;
        Cursor *typeDeclaration;
        enum Flag {
            None = 0x00,
            ConstQualified = 0x01,
            VolatileQualified = 0x02,
            RestrictQualified = 0x04,
            Variadic = 0x08,
            RValue = 0x10,
            LValue = 0x20,
            POD = 0x40
        };
        Value toValue() const;
        unsigned flags;
        long long numElements, arraySize, align, sizeOf;
    };

    static Value diagnosticToValue(CXDiagnostic diagnostics);
    static Value locationToValue(Location location);
    static Value rangeToValue(CXSourceRange range);
    static Value rangeToValue(Location start, Location end);

    List<std::shared_ptr<Type> > mTypes;
    Hash<String, Type*> mTypesBySpelling;

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
