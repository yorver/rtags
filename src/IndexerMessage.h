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

#ifndef IndexerMessage_h
#define IndexerMessage_h

#include <rct/Message.h>
#include <rct/String.h>
#include "RTagsMessage.h"
#include "IndexData.h"
#include <sys/types.h>

class IndexerMessage : public RTagsMessage
{
public:
    enum { MessageId = IndexerMessageId };

    IndexerMessage()
        : RTagsMessage(MessageId), mShmKey(-1)
    {
    }

    static void encodeData(Serializer &serializer,
                           const Path &project,
                           const std::shared_ptr<IndexData> &data)
    {
        static const bool debugIndexerMessage = getenv("RDM_DEBUG_INDEXERMESSAGE");
        StopWatch sw;
        assert(data);
        serializer << project << data->flags << data->key << data->parseTime;
        CursorInfo::serialize(serializer, data->symbols);
        serializer << data->references << data->symbolNames << data->dependencies
                   << data->usrMap << data->message << data->fixIts
                   << data->xmlDiagnostics << data->visited << data->jobId;
        if (debugIndexerMessage)
            error() << "encoding took" << sw.elapsed() << "for" << Location::path(data->fileId());
    }

    static String encodeData(const Path &project,
                             const std::shared_ptr<IndexData> &data)
    {
        String ret;
        Serializer serializer(ret);
        serializer << static_cast<key_t>(-1);
        encodeData(serializer, project, data);
        return ret;
    }

    void decodeData(Deserializer &deserializer)
    {
        assert(!mData);
        uint32_t flags;
        deserializer >> mProject >> flags;
        mData = std::make_shared<IndexData>(flags);
        deserializer >> mData->key >> mData->parseTime;
        CursorInfo::deserialize(deserializer, mData->symbols);
        deserializer >> mData->references >> mData->symbolNames >> mData->dependencies
                     >> mData->usrMap >> mData->message >> mData->fixIts >> mData->xmlDiagnostics
                     >> mData->visited >> mData->jobId;
    }

    virtual void encode(Serializer &) const
    {
        assert(0 && "This should never happen");
    }

    virtual void decode(Deserializer &deserializer)
    {
        static const bool debugIndexerMessage = getenv("RDM_DEBUG_INDEXERMESSAGE");
        StopWatch sw;
        deserializer >> mShmKey;
        if (mShmKey == -1) {
            decodeData(deserializer);
            if (debugIndexerMessage)
                error() << "decoding took" << sw.elapsed();
        }
    }

    std::shared_ptr<IndexData> data() const { return mData; }
    const Path &project() const { return mProject; }

    key_t sharedMemory() const { return mShmKey; }
private:
    Path mProject;
    std::shared_ptr<IndexData> mData;
    key_t mShmKey;
};

#endif
