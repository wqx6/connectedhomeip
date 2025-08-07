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

#include <app/clusters/binding-server/BindingTable.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/Optional.h>
#include <lib/core/TLVWriter.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/DefaultStorageKeyAllocator.h>

namespace chip {
namespace app {
namespace Clusters {

enum class Fields : uint8_t
{
    kNodeId           = 1,
    kRemoteEndpointId = 2,
    kGroupId          = 3,
    kClusterId        = 4,
    klocalEndpointId  = 5,
};

CHIP_ERROR BindingTableEntry::Serialize(TLV::TLVWriter & writer) const
{
    VerifyOrReturnError(Verify(), CHIP_ERROR_INVALID_ARGUMENT);
    TLV::TLVType outer;
    ReturnErrorOnFailure(writer.StartContainer(TLV::AnonymousTag(), TLV::kTLVType_Structure, outer));
    if (nodeId.has_value() && remoteEndpointId.has_value())
    {
        ReturnErrorOnFailure(writer.Put(TLV::ContextTag(Fields::kNodeId), nodeId.value()));
        ReturnErrorOnFailure(writer.Put(TLV::ContextTag(Fields::kRemoteEndpointId), remoteEndpointId.value()));
    }
    else
    {
        ReturnErrorOnFailure(writer.Put(TLV::ContextTag(Fields::kGroupId), groupId.value()));
    }
    if (clusterId.has_value())
    {
        ReturnErrorOnFailure(writer.Put(TLV::ContextTag(Fields::kClusterId), clusterId.value()));
    }
    ReturnErrorOnFailure(writer.Put(TLV::ContextTag(Fields::klocalEndpointId), localEndpointId));
    ReturnErrorOnFailure(writer.EndContainer(outer));
    ReturnErrorOnFailure(writer.Finalize());
    return CHIP_NO_ERROR;
}

CHIP_ERROR BindingTableEntry::Deserialize(TLV::TLVReader & reader)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    TLV::TLVType outer;

    ReturnErrorOnFailure(reader.Next(TLV::AnonymousTag()));
    VerifyOrReturnError(TLV::kTLVType_Structure == reader.GetType(), CHIP_ERROR_WRONG_TLV_TYPE);
    ReturnErrorOnFailure(reader.EnterContainer(outer));
    while ((err = reader.Next()) == CHIP_NO_ERROR)
    {
        if (TLV::IsContextTag(reader.GetTag()))
        {
            switch (TLV::TagNumFromTag(reader.GetTag()))
            {
            case to_underlying(Fields::kNodeId): {
                NodeId node;
                ReturnErrorOnFailure(reader.Get(node));
                nodeId.emplace(node);
            }
            break;
            case to_underlying(Fields::kRemoteEndpointId): {
                EndpointId remoteEndpoint;
                ReturnErrorOnFailure(reader.Get(remoteEndpoint));
                remoteEndpointId.emplace(remoteEndpoint);
            }
            break;
            case to_underlying(Fields::kGroupId): {
                GroupId group;
                ReturnErrorOnFailure(reader.Get(group));
                groupId.emplace(group);
            }
            break;
            case to_underlying(Fields::kClusterId): {
                ClusterId cluster;
                ReturnErrorOnFailure(reader.Get(cluster));
                clusterId.emplace(cluster);
            }
            break;
            case to_underlying(Fields::klocalEndpointId):
                ReturnErrorOnFailure(reader.Get(localEndpointId));
                break;
            default:
                break;
            }
        }
    }

    VerifyOrReturnError(err == CHIP_END_OF_TLV, err);
    ReturnErrorOnFailure(reader.ExitContainer(outer));
    VerifyOrReturnError(Verify(), CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BindingTable::Get(FabricIndex fabricIndex, uint16_t index, BindingTableEntry & entry) const
{
    entry.fabricIndex = fabricIndex;
    entry.index       = index;
    ReturnErrorOnFailure(entry.Load(mStorage));
    entry.fabricIndex = fabricIndex;
    VerifyOrReturnError(entry.Verify(), CHIP_ERROR_INTERNAL);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BindingTable::Set(FabricIndex fabricIndex, uint16_t index, const BindingTableEntry & entry)
{
    VerifyOrReturnError(index < kMaxBindingEntriesPerFabric, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(entry.Verify(), CHIP_ERROR_INVALID_ARGUMENT);
    BindingTableEntry e(entry);
    e.index       = index;
    e.fabricIndex = fabricIndex;
    return e.Save(mStorage);
}

CHIP_ERROR BindingTable::Append(FabricIndex fabricIndex, const BindingTableEntry & entry)
{

    bool entryAdded = false;
    BindingTableEntry emptyEntry;
    CHIP_ERROR err = CHIP_NO_ERROR;
    for (size_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; index++)
    {
        if ((err = Get(fabricIndex, index, emptyEntry)) == CHIP_ERROR_NOT_FOUND)
        {
            ReturnErrorOnFailure(Set(fabricIndex, index, entry));
            entryAdded = true;
            break;
        }
        ReturnErrorOnFailure(err);
    }
    return entryAdded ? CHIP_NO_ERROR: CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
}


CHIP_ERROR BindingTable::Find(FabricIndex fabricIndex, EndpointId localEndpointId, NodeId nodeId, EndpointId remoteEndpoint,
                              std::optional<ClusterId> cluster, uint16_t & index)
{
    BindingTableEntry entry(nodeId, remoteEndpoint, localEndpointId, cluster, fabricIndex);
    return Find(entry, index);
}

CHIP_ERROR BindingTable::Find(FabricIndex fabricIndex, EndpointId localEndpointId, GroupId groupId,
                              std::optional<ClusterId> cluster, uint16_t & index)
{
    BindingTableEntry entry(groupId, localEndpointId, cluster, fabricIndex);
    return Find(entry, index);
}

CHIP_ERROR BindingTable::Find(const BindingTableEntry & entry, uint16_t & index)
{
    BindingTableEntry tempEntry;
    for (index = 0; index < kMaxBindingEntriesPerFabric; index++)
    {
        if (Get(entry.fabricIndex, index, tempEntry) != CHIP_NO_ERROR)
        {
            break;
        }
        if (entry == tempEntry)
        {
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR BindingTable::Remove(FabricIndex fabricIndex, uint16_t index)
{
    BindingTableEntry entry;
    ReturnErrorOnFailure(Get(fabricIndex, index, entry));
    // Shift remaining entries down one position
    while (CHIP_NO_ERROR == this->Get(fabricIndex, static_cast<uint16_t>(index + 1), entry))
    {
        ReturnErrorOnFailure(this->Set(fabricIndex, index++, entry));
    }
    // Remove last entry
    entry.fabricIndex = fabricIndex;
    entry.index       = index;
    return entry.Delete(mStorage);
}

CHIP_ERROR BindingTable::RemoveAll(FabricIndex fabricIndex)
{
    BindingTableEntry entry;
    uint16_t index = 0;
    while (index < kMaxBindingEntriesPerFabric)
    {
        CHIP_ERROR err = Get(fabricIndex, index++, entry);
        if (err == CHIP_ERROR_NOT_FOUND)
        {
            break;
        }
        ReturnErrorOnFailure(err);
        ReturnErrorOnFailure(entry.Delete(mStorage));
    }
    return CHIP_NO_ERROR;
}

bool BindingTable::IsEmpty(FabricIndex fabricIndex)
{
    BindingTableEntry entry;
    return (Get(fabricIndex, 0, entry) == CHIP_ERROR_NOT_FOUND);
}

} // namespace Clusters
} // namespace app
} // namespace chip
