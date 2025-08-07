/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
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

#include <app/InteractionModelEngine.h>
#include <app/clusters/binding-server/BindingCluster.h>
#include <app/clusters/binding-server/BindingManager.h>
#include <app/clusters/binding-server/BindingTable.h>
#include <clusters/Binding/Attributes.h>
#include <clusters/Binding/Metadata.h>
#include <clusters/Binding/Structs.h>
#include <lib/core/CHIPError.h>
#include <lib/core/Optional.h>
#include <lib/support/CodeUtils.h>
#include <platform/PlatformManager.h>
#include <protocols/interaction_model/StatusCode.h>

namespace chip {
namespace app {
namespace Clusters {

using TargetStructType         = Binding::Structs::TargetStruct::Type;
using DecodableBindingListType = Binding::Attributes::Binding::TypeInfo::DecodableType;

namespace {

constexpr DataModel::AttributeEntry kAttributes[] = {
    Binding::Attributes::Binding::kMetadataEntry,
};

bool IsValidBinding(const EndpointId localEndpoint, const TargetStructType & entry)
{
    bool isValid = false;

    // Entry has endpoint, node id and no group id
    if (!entry.group.HasValue() && entry.endpoint.HasValue() && entry.node.HasValue())
    {
        if (entry.cluster.HasValue())
        {
            auto * dataModelProvider = InteractionModelEngine::GetInstance()->GetDataModelProvider();
            ReadOnlyBufferBuilder<ClusterId> clientClusters;
            dataModelProvider->ClientClusters(localEndpoint, clientClusters);
            for (auto clusterId : clientClusters.TakeBuffer())
            {
                if (clusterId == entry.cluster.Value())
                {
                    // Valid node/endpoint/cluster binding
                    isValid = true;
                }
            }
        }
        else
        {
            // Valid node/endpoint (no cluster id) binding
            isValid = true;
        }
    }
    // Entry has group id and no endpoint and node id
    else if (!entry.endpoint.HasValue() && !entry.node.HasValue() && entry.group.HasValue())
    {
        // Valid group binding
        isValid = true;
    }

    return isValid;
}

CHIP_ERROR CheckValidBindingList(const EndpointId localEndpoint, const DecodableBindingListType & bindingList,
                                 FabricIndex accessingFabricIndex)
{
    size_t listSize = 0;
    auto iter       = bindingList.begin();
    while (iter.Next())
    {
        VerifyOrReturnError(IsValidBinding(localEndpoint, iter.GetValue()), CHIP_IM_GLOBAL_STATUS(ConstraintError));
        listSize++;
    }
    ReturnErrorOnFailure(iter.GetStatus());
    size_t endpointSize = 0;
    size_t totalSize    = 0;
    auto & bindingTable = BindingManager::GetInstance().GetBindingTable();
    BindingTableEntry entry(0, kInvalidEndpointId, kInvalidEndpointId, std::nullopt);
    for (size_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; index++)
    {
        if (bindingTable.Get(accessingFabricIndex, index, entry) == CHIP_ERROR_NOT_FOUND)
        {
            break;
        }
        totalSize++;
        if (entry.localEndpointId == localEndpoint)
        {
            endpointSize++;
        }
    }
    VerifyOrReturnError(totalSize + listSize - endpointSize <= BindingTable::kMaxBindingEntriesPerFabric,
                        CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    return CHIP_NO_ERROR;
}

CHIP_ERROR AddBindingEntry(const BindingTableEntry & entry)
{
    ReturnErrorOnFailure(BindingManager::GetInstance().GetBindingTable().Append(entry.fabricIndex, entry));
    if (entry.nodeId.has_value() && entry.remoteEndpointId.has_value())
    {
        CHIP_ERROR err = BindingManager::GetInstance().UnicastBindingCreated(entry.fabricIndex, entry.nodeId.value());
        if (err != CHIP_NO_ERROR)
        {
            // Unicast connection failure can happen if peer is offline. We'll retry connection on-demand.
            ChipLogError(
                Zcl, "Binding: Failed to create session for unicast binding to device " ChipLogFormatX64 ": %" CHIP_ERROR_FORMAT,
                ChipLogValueX64(entry.nodeId.value()), err.Format());
        }
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR CreateBindingEntry(const TargetStructType & entry, EndpointId localEndpoint)
{

    if (entry.group.HasValue())
    {
        BindingTableEntry bindingEntry(entry.group.Value(), localEndpoint, entry.cluster.std_optional(), entry.fabricIndex);
        return AddBindingEntry(bindingEntry);
    }
    if (entry.node.HasValue() && entry.endpoint.HasValue())
    {
        BindingTableEntry bindingEntry(entry.node.Value(), entry.endpoint.Value(), localEndpoint, entry.cluster.std_optional(),
                                       entry.fabricIndex);
        return AddBindingEntry(bindingEntry);
    }
    return CHIP_ERROR_INVALID_ARGUMENT;
}

} // namespace

DataModel::ActionReturnStatus BindingServerCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                                  AttributeValueEncoder & encoder)
{
    switch (request.path.mAttributeId)
    {
    case Binding::Attributes::Binding::Id: {
        return encoder.EncodeList([&](const auto & subEncoder) {
            auto & bindingTable = BindingManager::GetInstance().GetBindingTable();
            BindingTableEntry entry(0, kInvalidEndpointId, kInvalidEndpointId, std::nullopt);
            for (size_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
            {
                if (bindingTable.Get(request.GetAccessingFabricIndex(), index, entry) == CHIP_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (entry.localEndpointId == request.path.mEndpointId && entry.nodeId.has_value() &&
                    entry.remoteEndpointId.has_value())
                {
                    Binding::Structs::TargetStruct::Type value = {
                        .node        = FromStdOptional(entry.nodeId),
                        .group       = NullOptional,
                        .endpoint    = FromStdOptional(entry.remoteEndpointId),
                        .cluster     = FromStdOptional(entry.clusterId),
                        .fabricIndex = entry.fabricIndex,
                    };
                    ReturnErrorOnFailure(subEncoder.Encode(value));
                }
                else if (entry.localEndpointId == request.path.mEndpointId && entry.groupId.has_value())
                {
                    Binding::Structs::TargetStruct::Type value = {
                        .node        = NullOptional,
                        .group       = FromStdOptional(entry.groupId),
                        .endpoint    = NullOptional,
                        .cluster     = FromStdOptional(entry.clusterId),
                        .fabricIndex = entry.fabricIndex,
                    };
                    ReturnErrorOnFailure(subEncoder.Encode(value));
                }
            }
            return CHIP_NO_ERROR;
        });
    }
    case Globals::Attributes::FeatureMap::Id:
        return encoder.Encode((uint32_t)0);
    case Globals::Attributes::ClusterRevision::Id:
        return encoder.Encode(Binding::kRevision);
    default:
        break;
    }
    return Protocols::InteractionModel::Status::UnsupportedAttribute;
}

DataModel::ActionReturnStatus BindingServerCluster::WriteAttribute(const DataModel::WriteAttributeRequest & request,
                                                                   AttributeValueDecoder & decoder)
{
    switch (request.path.mAttributeId)
    {
    case Binding::Attributes::Binding::Id: {
        mAccessingFabricIndex = request.GetAccessingFabricIndex();
        if (!request.path.IsListOperation() || request.path.mListOp == ConcreteDataAttributePath::ListOperation::ReplaceAll)
        {
            DecodableBindingListType newBindingList;

            ReturnErrorOnFailure(decoder.Decode(newBindingList));
            ReturnErrorOnFailure(CheckValidBindingList(request.path.mEndpointId, newBindingList, mAccessingFabricIndex));

            // Clear all entries for the current accessing fabric and endpoint
            auto & bindingTable = BindingManager::GetInstance().GetBindingTable();
            BindingTableEntry entry;
            for (size_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
            {
                CHIP_ERROR err = bindingTable.Get(mAccessingFabricIndex, index, entry);
                if (err == CHIP_ERROR_NOT_FOUND)
                {
                    break;
                }
                ReturnErrorOnFailure(err);
                if (entry.localEndpointId == mPath.mEndpointId)
                {
                    if (entry.nodeId.has_value() && entry.remoteEndpointId.has_value())
                    {
                        BindingManager::GetInstance().UnicastBindingRemoved(mAccessingFabricIndex, index);
                    }
                    ReturnErrorOnFailure(bindingTable.Remove(mAccessingFabricIndex, index));
                }
            }

            // Add new entries
            auto iter      = newBindingList.begin();
            CHIP_ERROR err = CHIP_NO_ERROR;
            while (iter.Next() && err == CHIP_NO_ERROR)
            {
                err = CreateBindingEntry(iter.GetValue(), request.path.mEndpointId);
            }

            // If this was not caused by a list operation, OnListWriteEnd is not going to be triggered
            // so a notification is sent here.
            if (!request.path.IsListOperation())
            {
                DeviceLayer::ChipDeviceEvent event{ .Type            = DeviceLayer::DeviceEventType::kBindingsChangedViaCluster,
                                                    .BindingsChanged = { .fabricIndex = mAccessingFabricIndex } };
                if (chip::DeviceLayer::PlatformMgr().PostEvent(&event) != CHIP_NO_ERROR)
                {
                    ChipLogError(AppServer, "Failed to post BindingsChangedViaCluster event");
                }
            }
            return err;
        }
        if (request.path.mListOp == ConcreteDataAttributePath::ListOperation::AppendItem)
        {
            TargetStructType target;
            ReturnErrorOnFailure(decoder.Decode(target));
            if (!IsValidBinding(request.path.mEndpointId, target))
            {
                return CHIP_IM_GLOBAL_STATUS(ConstraintError);
            }
            return CreateBindingEntry(target, request.path.mEndpointId);
        }
        return CHIP_IM_GLOBAL_STATUS(UnsupportedWrite);
    }
    break;
    default:
        break;
    }
    return Protocols::InteractionModel::Status::UnsupportedWrite;
}

void BindingServerCluster::ListAttributeWriteNotification(const ConcreteAttributePath & path, DataModel::ListWriteOperation opType)
{
    switch (opType)
    {
    case DataModel::ListWriteOperation::kListWriteSuccess: {
        DeviceLayer::ChipDeviceEvent event{ .Type            = DeviceLayer::DeviceEventType::kBindingsChangedViaCluster,
                                            .BindingsChanged = { .fabricIndex = mAccessingFabricIndex } };
        CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().PostEvent(&event);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "Failed to post BindingsChangedViaCluster event");
        }
    }
    default:
        break;
    }
}

CHIP_ERROR BindingServerCluster::Attributes(const ConcreteClusterPath & path,
                                            ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    ReturnErrorOnFailure(builder.ReferenceExisting(kAttributes));
    return builder.AppendElements(DefaultServerCluster::GlobalAttributes());
}

} // namespace Clusters
} // namespace app
} // namespace chip
