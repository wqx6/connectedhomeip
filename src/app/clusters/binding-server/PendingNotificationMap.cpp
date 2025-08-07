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

#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/CodeUtils.h>

#include <app/clusters/binding-server/BindingTable.h>
#include <app/clusters/binding-server/PendingNotificationMap.h>
#include <optional>

namespace chip {
namespace app {
namespace Clusters {

CHIP_ERROR PendingNotificationMap::FindLRUConnectPeer(ScopedNodeId & nodeId)
{
    // When entries are added to PendingNotificationMap, they are appended to the end.
    // To find the LRU peer, we need to find the peer whose last entry in the map is closer
    // to the start of the list than the last entry of any other peer.
    ScopedNodeId peers[kMaxPendingNotifications];
    bool foundLRUPeer = false;
    for (size_t i = 0; i < mEntryCount; ++i)
    {
        size_t currentIndex = mEntryCount - i - 1;
        BindingTableEntry bindingEntry;
        ReturnErrorOnFailure(mBindingTable.Get(mEntries[currentIndex].mFabricIndex, mEntries[currentIndex].mBindingEntryIndex, bindingEntry));
        VerifyOrReturnError(bindingEntry.nodeId.has_value() && bindingEntry.remoteEndpointId.has_value(), CHIP_ERROR_INTERNAL);
        ScopedNodeId currentNode(bindingEntry.nodeId.value(), mEntries[currentIndex].mFabricIndex);
        peers[currentIndex] = currentNode;
        bool recorded = false;
        for (size_t index = currentIndex + 1; index < mEntryCount; ++index)
        {
            if (peers[index] == currentNode)
            {
                recorded = true;
                break;
            }
        }
        if (!recorded)
        {
            nodeId = currentNode;
            foundLRUPeer = true;
        }
    }
    return foundLRUPeer ? CHIP_NO_ERROR : CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR PendingNotificationMap::AddPendingNotification(FabricIndex fabricIndex, uint16_t bindingEntryIndex,
                                                          PendingNotificationContext * context)
{
    RemoveEntry(fabricIndex, bindingEntryIndex);
    if (mEntryCount == kMaxPendingNotifications)
    {
        return CHIP_ERROR_NO_MEMORY;
    }
    mEntries[mEntryCount] = PendingNotificationEntry(fabricIndex, bindingEntryIndex, context);
    if (context)
    {
        context->IncrementConsumersNumber();
    }
    mEntryCount++;
    return CHIP_NO_ERROR;
}

void PendingNotificationMap::RemoveEntry(FabricIndex fabricIndex, uint16_t bindingEntryIndex)
{
    uint8_t newEntryCount = 0;
    for (size_t index = 0; index < mEntryCount; ++index)
    {
        if (mEntries[index].mFabricIndex != fabricIndex || mEntries[index].mBindingEntryIndex != bindingEntryIndex)
        {
            mEntries[newEntryCount] = mEntries[index];
            newEntryCount++;
        }
        else if (mEntries[index].mContext)
        {
            mEntries[index].mContext->DecrementConsumersNumber();
        }
    }
    mEntryCount = newEntryCount;
}

void PendingNotificationMap::RemoveAllEntriesForNode(const ScopedNodeId & nodeId)
{
    uint8_t newEntryCount = 0;
    for (size_t index = 0; index < mEntryCount; ++index)
    {
        BindingTableEntry bindingEntry;
        if (mBindingTable.Get(mEntries[index].mFabricIndex, mEntries[index].mBindingEntryIndex, bindingEntry) != CHIP_NO_ERROR ||
            !bindingEntry.nodeId.has_value() || !bindingEntry.remoteEndpointId.has_value())
        {
            // Invalid entry as we cannot find binding entry from binding table
            if (mEntries[index].mContext) {
                mEntries[index].mContext->DecrementConsumersNumber();
            }
            continue;
        }
        if (bindingEntry.fabricIndex != nodeId.GetFabricIndex() || bindingEntry.nodeId.value() != nodeId.GetNodeId())
        {
            mEntries[newEntryCount] = mEntries[index];
            newEntryCount++;
        }
        else if (mEntries[index].mContext)
        {
            mEntries[index].mContext->DecrementConsumersNumber();
        }
    }
    mEntryCount = newEntryCount;
}

void PendingNotificationMap::RemoveAllEntriesForFabric(FabricIndex fabric)
{
    uint8_t newEntryCount = 0;
    for (size_t index = 0; index < mEntryCount; ++index)
    {
        if (mEntries[index].mFabricIndex != fabric)
        {
            mEntries[newEntryCount] = mEntries[index];
            newEntryCount++;
        }
        else if (mEntries[index].mContext)
        {
            mEntries[index].mContext->DecrementConsumersNumber();
        }
    }
    mEntryCount = newEntryCount;
}

} // namespace Clusters
} // namespace app
} // namespace chip
