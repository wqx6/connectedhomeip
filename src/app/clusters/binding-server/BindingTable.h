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

#pragma once

#include <clusters/Binding/Structs.h>
#include <lib/core/CHIPError.h>
#include <lib/core/CHIPPersistentStorageDelegate.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/Optional.h>
#include <lib/core/TLVCommon.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/PersistentData.h>

#include <cstdint>
#include <optional>

namespace chip {
namespace app {
namespace Clusters {

constexpr size_t kBindingEntryBufferSize =
    TLV::EstimateStructOverhead(sizeof(EndpointId), sizeof(EndpointId), sizeof(ClusterId), sizeof(NodeId));

/** @brief Defines an entry in the binding table.
 *
 * A binding entry specifies a local endpoint, a remote endpoint, a
 * cluster ID and either the destination EUI64 (for unicast bindings) or the
 * 64-bit group address (for multicast bindings).
 */
struct BindingTableEntry : public PersistentData<kBindingEntryBufferSize>
{
    BindingTableEntry() :
        localEndpointId(kInvalidEndpointId), nodeId(std::nullopt), remoteEndpointId(std::nullopt), groupId(std::nullopt),
        clusterId(std::nullopt), fabricIndex(kUndefinedFabricIndex)
    {}
    BindingTableEntry(NodeId node, EndpointId remote, EndpointId local, std::optional<ClusterId> cluster,
                      FabricIndex fabric = kUndefinedFabricIndex) :
        localEndpointId(local), nodeId(std::make_optional(node)), remoteEndpointId(std::make_optional(remote)),
        groupId(std::nullopt), clusterId(cluster), fabricIndex(fabric)
    {}

    BindingTableEntry(GroupId group, EndpointId localEndpoint, std::optional<ClusterId> cluster,
                      FabricIndex fabric = kUndefinedFabricIndex) :
        localEndpointId(localEndpoint), nodeId(std::nullopt), remoteEndpointId(std::nullopt), groupId(std::make_optional(group)),
        clusterId(cluster), fabricIndex(fabric)
    {}

    bool operator==(BindingTableEntry const & other) const
    {
        VerifyOrDie(Verify());
        if (fabricIndex != other.fabricIndex && other.localEndpointId != localEndpointId)
        {
            return false;
        }
        if (remoteEndpointId.has_value())
        {
            return nodeId == other.nodeId && remoteEndpointId == other.remoteEndpointId && clusterId == other.clusterId;
        }
        return groupId == other.groupId && clusterId == other.clusterId;
    }

    CHIP_ERROR UpdateKey(StorageKeyName & skey) const override
    {
        if (fabricIndex == kUndefinedFabricIndex || index >= CHIP_CONFIG_MAX_BINDING_ENTRIES_PER_FABRIC)
        {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        skey = DefaultStorageKeyAllocator::BindingTableEntry(fabricIndex, index);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Serialize(TLV::TLVWriter & writer) const override;
    CHIP_ERROR Deserialize(TLV::TLVReader & reader) override;
    void Clear() override
    {
        nodeId.reset();
        remoteEndpointId.reset();
        groupId.reset();
        clusterId.reset();
        localEndpointId = kInvalidEndpointId;
        fabricIndex     = kUndefinedFabricIndex;
    }

    bool Verify() const
    {
        if (remoteEndpointId.has_value())
        {
            return nodeId.has_value() && !groupId.has_value();
        }
        return !nodeId.has_value() && groupId.has_value();
    }

    EndpointId localEndpointId = kInvalidEndpointId;
    std::optional<NodeId> nodeId;
    std::optional<EndpointId> remoteEndpointId;
    std::optional<GroupId> groupId;
    std::optional<ClusterId> clusterId;
    uint16_t index;
    FabricIndex fabricIndex;
};

class BindingTable
{
public:
    static constexpr size_t kMaxBindingEntriesPerFabric = CHIP_CONFIG_MAX_BINDING_ENTRIES_PER_FABRIC;

    BindingTable() {}

    /**
     * @brief Returns the Binding entry at the given position for specific fabric.
     * @param fabricIndex FabricIndex.
     * @param index Zero-based position within the Bindings table.
     * @param entry On success, contains the Binding matching the given index.
     * @return CHIP_NO_ERROR on success,
     *         CHIP_ERROR_NOT_FOUND if index is greater than the index of the last entry on the table.
     */
    CHIP_ERROR Get(FabricIndex fabricIndex, uint16_t index, BindingTableEntry & entry) const;

    /**
     * @brief Stores the Binding entry at the given position for specific fabric,
     *        overwriting any existing entry.
     * @param fabricIndex FabricIndex.
     * @param index Zero-based position within the Bindings table.
     * @param entry Binding entry to set.
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR Set(FabricIndex fabricIndex, uint16_t index, const BindingTableEntry & entry);

    /**
     * @brief Append the Binding entry for specific fabric.
     * @param fabricIndex FabricIndex.
     * @param entry Binding entry to append.
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR Append(FabricIndex fabricIndex, const BindingTableEntry & entry);

    /**
     * @brief Search the Binding entry.
     * @param fabricIndex    FabricIndex to match.
     * @param localEndpoint  LocalEndpointID to match
     * @param nodeId         NodeID to match.
     * @param remoteEndpoint EndpointID to match.
     * @param ClusterID      Optional ClusterID to match.
     * @param index On success, contains the position of the entry matching the given parameters in the table.
     *  If found, index contains the position of the entry in the table.
     *  If CHIP_ERROR_NOT_FOUND is returned, index contains the total number of entries in the table.
     * @return CHIP_NO_ERROR if found, CHIP_ERROR_NOT_FOUND if no entry matches the provided parameters.
     */
    CHIP_ERROR Find(FabricIndex fabricIndex, EndpointId localEndpoint, NodeId nodeId, EndpointId remoteEndpoint,
                    std::optional<ClusterId> cluster, uint16_t & index);

    /**
     * @brief Search the multicast Binding entry with FabricIndex, GroupID, and optional ClusterID.
     * @param fabricIndex    FabricIndex to match.
     * @param localEndpoint  LocalEndpointID to match
     * @param groupId        GroupID to match.
     * @param ClusterID      Optional ClusterID to match.
     * @param index On success, contains the position of the entry matching the given parameters in the table.
     *  If found, index contains the position of the entry in the table.
     *  If CHIP_ERROR_NOT_FOUND is returned, index contains the total number of entries in the table.
     * @return CHIP_NO_ERROR if found, CHIP_ERROR_NOT_FOUND if no no entry matches the provided parameters.
     */
    CHIP_ERROR Find(FabricIndex fabricIndex, EndpointId localEndpoint, GroupId groupId, std::optional<ClusterId> cluster,
                    uint16_t & index);

    /**
     * @brief Removes the Binding entry at the given position for the fabricIndex,
     *        shifting down the upper entries.
     * @param fabricIndex FabricIndex.
     * @param index Zero-based position within the Bindings table.
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR Remove(FabricIndex fabricIndex, uint16_t index);

    /**
     * @brief Removes all the entries for the fabricIndex.
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR RemoveAll(FabricIndex fabricIndex);

    /**
     * @brief Check if the table is empty for specific fabric
     * @return True when there is no entry in the table. False if there is at least one
     */
    bool IsEmpty(FabricIndex fabricIndex);

    /**
     * @brief Set the PersistentStorageDelegate for this Binding Table
     */
    void SetPersistentStorage(PersistentStorageDelegate * delegate) { mStorage = delegate; }

private:
    CHIP_ERROR Find(const BindingTableEntry & entry, uint16_t & index);

    PersistentStorageDelegate * mStorage = nullptr;
};

} // namespace Clusters
} // namespace app
} // namespace chip
