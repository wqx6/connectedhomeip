/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <app/clusters/binding-server/BindingTable.h>
#include <credentials/FabricTable.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/Pool.h>
#include <lib/support/Span.h>

namespace chip {
namespace app {
namespace Clusters {

/**
 * Application callback function when a context used in PendingNotificationEntry will not be needed and should be
 * released.
 */
using PendingNotificationContextReleaseHandler = void (*)(void * context);

class PendingNotificationContext
{
public:
    PendingNotificationContext(void * context, PendingNotificationContextReleaseHandler contextReleaseHandler) :
        mContext(context), mPendingNotificationContextReleaseHandler(contextReleaseHandler)
    {}
    void * GetContext() { return mContext; };
    uint32_t GetConsumersNumber() { return mConsumersNumber; }
    void IncrementConsumersNumber() { mConsumersNumber++; }
    void DecrementConsumersNumber()
    {
        VerifyOrDie(mConsumersNumber > 0);
        if (--mConsumersNumber == 0)
        {
            // Release the context only if there is no pending notification pointing to us.
            if (mPendingNotificationContextReleaseHandler != nullptr)
            {
                mPendingNotificationContextReleaseHandler(mContext);
            }
            Platform::Delete(this);
        }
    }

private:
    void * mContext;
    uint32_t mConsumersNumber = 0;
    PendingNotificationContextReleaseHandler mPendingNotificationContextReleaseHandler;
};

struct PendingNotificationEntry
{
public:
    FabricIndex mFabricIndex              = kUndefinedFabricIndex;
    uint16_t mBindingEntryIndex           = UINT16_MAX;
    PendingNotificationContext * mContext = nullptr;
    PendingNotificationEntry()            = default;
    PendingNotificationEntry(FabricIndex fabricIndex, uint8_t bindingEntryIndex, PendingNotificationContext * context) :
        mFabricIndex(fabricIndex), mBindingEntryIndex(bindingEntryIndex), mContext(context)
    {
        if (mContext)
        {
            mContext->IncrementConsumersNumber();
        }
    }

    ~PendingNotificationEntry()
    {
        if (mContext)
        {
            mContext->DecrementConsumersNumber();
        }
    }
};

// The pool for all the pending comands.
class PendingNotificationMap
{
public:
    static constexpr size_t kMaxPendingNotifications = BindingTable::kMaxBindingEntriesPerFabric * CHIP_CONFIG_MAX_FABRICS;

    friend class Iterator;

    class Iterator
    {
    public:
        Iterator(PendingNotificationMap * map, size_t index) : mMap(map), mIndex(index) {}

        PendingNotificationEntry operator*()
        {
            return mMap->mEntries[mIndex];
        }

        Iterator operator++()
        {
            mIndex++;
            return *this;
        }

        bool operator!=(const Iterator & rhs) const { return mIndex != rhs.mIndex; }

        bool operator==(const Iterator & rhs) const { return mIndex == rhs.mIndex; }

    private:
        PendingNotificationMap * mMap;
        size_t mIndex;
    };

    Iterator begin() { return Iterator(this, 0); }

    Iterator end() { return Iterator(this, mEntryCount); };
    PendingNotificationMap(BindingTable & bindingTable) : mEntryCount(0), mBindingTable(bindingTable) {}

    CHIP_ERROR FindLRUConnectPeer(ScopedNodeId & nodeId);

    CHIP_ERROR AddPendingNotification(FabricIndex fabricIndex, uint16_t bindingEntryIndex, PendingNotificationContext * context);

    void RemoveEntry(FabricIndex fabricIndex, uint16_t bindingEntryIndex);

    void RemoveAllEntriesForNode(const ScopedNodeId & nodeId);

    void RemoveAllEntriesForFabric(FabricIndex fabricIndex);

    void RegisterPendingNotificationContextReleaseHandler(PendingNotificationContextReleaseHandler handler)
    {
        mPendingNotificationContextReleaseHandler = handler;
    }

    PendingNotificationContext * NewPendingNotificationContext(void * context)
    {
        return Platform::New<PendingNotificationContext>(context, mPendingNotificationContextReleaseHandler);
    };

private:
    PendingNotificationContextReleaseHandler mPendingNotificationContextReleaseHandler = nullptr;
    PendingNotificationEntry mEntries[kMaxPendingNotifications];
    size_t mEntryCount;
    BindingTable & mBindingTable;
};
} // namespace Clusters
} // namespace app
} // namespace chip
