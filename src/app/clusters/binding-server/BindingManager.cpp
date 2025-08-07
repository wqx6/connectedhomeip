/*
 *
 *    Copyright (c) 2022-2025 Project CHIP Authors
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

#include <app/clusters/binding-server/BindingManager.h>
#include <app/clusters/binding-server/BindingTable.h>
#include <credentials/FabricTable.h>
#include <lib/core/CHIPConfig.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/ScopedNodeId.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>

namespace chip {
namespace app {
namespace Clusters {

CHIP_ERROR BindingManager::UnicastBindingCreated(FabricIndex fabricIndex, NodeId nodeId)
{
    return EstablishConnection(ScopedNodeId(nodeId, fabricIndex));
}

CHIP_ERROR BindingManager::UnicastBindingRemoved(FabricIndex fabricIndex, uint8_t bindingEntryIndex)
{
    mPendingNotificationMap.RemoveEntry(fabricIndex, bindingEntryIndex);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BindingManager::Init(const BindingManagerInitParams & params)
{
    VerifyOrReturnError(params.mCASESessionManager != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(params.mFabricTable != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(params.mStorage != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    mBindingTable.SetPersistentStorage(params.mStorage);
    mInitParams = params;
    return CHIP_NO_ERROR;
}

CHIP_ERROR BindingManager::EstablishConnection(const ScopedNodeId & nodeId)
{
    VerifyOrReturnError(mInitParams.mCASESessionManager != nullptr, CHIP_ERROR_INCORRECT_STATE);

    mLastSessionEstablishmentError = CHIP_NO_ERROR;
    auto * connectionCallback      = Platform::New<ConnectionCallback>(*this);
    VerifyOrReturnError(connectionCallback != nullptr, CHIP_ERROR_NO_MEMORY);

    mInitParams.mCASESessionManager->FindOrEstablishSession(nodeId, connectionCallback->GetOnDeviceConnected(),
                                                            connectionCallback->GetOnDeviceConnectionFailure());
    if (mLastSessionEstablishmentError == CHIP_ERROR_NO_MEMORY)
    {
        // Release the least recently used entry
        ScopedNodeId peerToRemove;
        if (mPendingNotificationMap.FindLRUConnectPeer(peerToRemove) == CHIP_NO_ERROR)
        {
            mPendingNotificationMap.RemoveAllEntriesForNode(peerToRemove);

            // Now retry
            mLastSessionEstablishmentError = CHIP_NO_ERROR;
            // At this point connectionCallback is null since it deletes itself when the callback is called.
            connectionCallback = Platform::New<ConnectionCallback>(*this);
            mInitParams.mCASESessionManager->FindOrEstablishSession(nodeId, connectionCallback->GetOnDeviceConnected(),
                                                                    connectionCallback->GetOnDeviceConnectionFailure());
        }
    }
    return mLastSessionEstablishmentError;
}

void BindingManager::HandleDeviceConnected(Messaging::ExchangeManager & exchangeMgr, const SessionHandle & sessionHandle)
{
    FabricIndex fabricToRemove = kUndefinedFabricIndex;
    NodeId nodeToRemove        = kUndefinedNodeId;
    for (auto pendingNotification : mPendingNotificationMap)
    {
        BindingTableEntry bindingEntry;
        if (mBindingTable.Get(pendingNotification.mFabricIndex, pendingNotification.mBindingEntryIndex, bindingEntry) !=
                CHIP_NO_ERROR ||
            !bindingEntry.nodeId.has_value() || !bindingEntry.remoteEndpointId.has_value())
        {
            continue;
        }
        if (sessionHandle->GetPeer() == ScopedNodeId(bindingEntry.nodeId.value(), bindingEntry.fabricIndex))
        {
            fabricToRemove = bindingEntry.fabricIndex;
            nodeToRemove   = bindingEntry.nodeId.value();
            OperationalDeviceProxy device(&exchangeMgr, sessionHandle);
            mBoundDeviceChangedHandler(bindingEntry, &device, pendingNotification.mContext->GetContext());
        }
    }
    mPendingNotificationMap.RemoveAllEntriesForNode(ScopedNodeId(nodeToRemove, fabricToRemove));
}

void BindingManager::HandleDeviceConnectionFailure(const ScopedNodeId & peerId, CHIP_ERROR error)
{
    // Simply release the entry, the connection will be re-established as needed.
    ChipLogError(AppServer, "Failed to establish connection to node 0x" ChipLogFormatX64, ChipLogValueX64(peerId.GetNodeId()));
    mLastSessionEstablishmentError = error;
    // We don't release the entry when connection fails, because inside
    // BindingManager::EstablishConnection we may try again the connection.
    // TODO(#22173): The logic in there doesn't actually make any sense with how
    // mPendingNotificationMap and CASESessionManager are implemented today.
}

void BindingManager::FabricRemoved(FabricIndex fabricIndex)
{
    mPendingNotificationMap.RemoveAllEntriesForFabric(fabricIndex);
}

CHIP_ERROR BindingManager::NotifyBoundClusterChanged(EndpointId endpoint, ClusterId cluster, void * context)
{
    VerifyOrReturnError(mInitParams.mFabricTable != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBoundDeviceChangedHandler != nullptr, CHIP_ERROR_HANDLER_NOT_SET);

    CHIP_ERROR error      = CHIP_NO_ERROR;
    auto * bindingContext = mPendingNotificationMap.NewPendingNotificationContext(context);
    VerifyOrReturnError(bindingContext != nullptr, CHIP_ERROR_NO_MEMORY);

    bindingContext->IncrementConsumersNumber();

    const auto & fabricTable = Server::GetInstance().GetFabricTable();
    for (auto & fabric : fabricTable)
    {
        FabricIndex fabricIndex = fabric.GetFabricIndex();
        for (size_t bindingIndex = 0; bindingIndex < BindingTable::kMaxBindingEntriesPerFabric; ++bindingIndex)
        {
            BindingTableEntry entry;
            if (mBindingTable.Get(fabricIndex, bindingIndex, entry) == CHIP_ERROR_NOT_FOUND)
            {
                break;
            }
            if (entry.localEndpointId == endpoint && (entry.clusterId.value_or(cluster) == cluster))
            {
                if (entry.nodeId.has_value() && entry.remoteEndpointId.has_value())
                {
                    error = mPendingNotificationMap.AddPendingNotification(entry.fabricIndex, entry.index, bindingContext);
                    SuccessOrExit(error);
                    error = EstablishConnection(ScopedNodeId(entry.nodeId.value(), entry.fabricIndex));
                    SuccessOrExit(error);
                }
                else if (entry.groupId.has_value())
                {
                    mBoundDeviceChangedHandler(entry, nullptr, bindingContext->GetContext());
                }
            }
        }
    }
exit:
    bindingContext->DecrementConsumersNumber();

    return error;
}
} // namespace Clusters
} // namespace app
} // namespace chip
