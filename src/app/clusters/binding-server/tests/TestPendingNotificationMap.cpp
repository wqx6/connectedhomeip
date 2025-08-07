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

#include <app/clusters/binding-server/BindingTable.h>
#include <app/clusters/binding-server/PendingNotificationMap.h>
#include <lib/core/StringBuilderAdapters.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <pw_unit_test/framework.h>

#include <optional>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {

static BindingTable sBindingTable;
FabricIndex sTestFabrics[CHIP_CONFIG_MAX_FABRICS];

class TestPendingNotificationMap : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        static chip::TestPersistentStorageDelegate storage;
        sBindingTable.SetPersistentStorage(&storage);
        FabricIndex fabricIndex = 1;
        for (auto & testFabric : sTestFabrics)
        {
            testFabric = fabricIndex++;
        }
    }
};

void ClearBindingTable(BindingTable & table)
{
    for (auto & fabricIndex : sTestFabrics)
    {
        EXPECT_EQ(table.RemoveAll(fabricIndex), CHIP_NO_ERROR);
    }
}

void CreateDefaultFullBindingTable(BindingTable & table)
{
    for (auto & fabricIndex : sTestFabrics)
    {
        for (uint16_t i = 0; i < BindingTable::kMaxBindingEntriesPerFabric; i++)
        {
            EXPECT_EQ(table.Append(fabricIndex, BindingTableEntry((i % 3) + 1, 1, 1, std::make_optional(i))), CHIP_NO_ERROR);
        }
    }
}

TEST_F(TestPendingNotificationMap, TestEmptyMap)
{
    PendingNotificationMap pendingMap(sBindingTable);
    EXPECT_EQ(pendingMap.begin(), pendingMap.end());
    chip::ScopedNodeId peer;
    EXPECT_EQ(pendingMap.FindLRUConnectPeer(peer), CHIP_ERROR_NOT_FOUND);
}

TEST_F(TestPendingNotificationMap, TestAddRemove)
{
    PendingNotificationMap pendingMap(sBindingTable);
    ClearBindingTable(sBindingTable);
    CreateDefaultFullBindingTable(sBindingTable);
    for (auto & fabricIndex : sTestFabrics)
    {
        for (uint16_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
        {
            EXPECT_EQ(pendingMap.AddPendingNotification(fabricIndex, index, nullptr), CHIP_NO_ERROR);
        }
    }
    // Confirm adding in one more element fails
    for (auto & fabricIndex : sTestFabrics)
    {
        EXPECT_EQ(pendingMap.AddPendingNotification(fabricIndex, BindingTable::kMaxBindingEntriesPerFabric, nullptr),
                  CHIP_ERROR_NO_MEMORY);
    }
    // Check the fabric index and binding entries index in the PendingNotificationEntries
    auto iter = pendingMap.begin();
    for (auto & fabricIndex : sTestFabrics)
    {
        for (uint16_t i = 0; i < BindingTable::kMaxBindingEntriesPerFabric; ++i)
        {
            PendingNotificationEntry entry = *iter;
            EXPECT_EQ(entry.mFabricIndex, fabricIndex);
            EXPECT_EQ(entry.mBindingEntryIndex, i);
            ++iter;
        }
    }
    EXPECT_EQ(iter, pendingMap.end());
    // Remove entries for specific node
    pendingMap.RemoveAllEntriesForNode(chip::ScopedNodeId(1, 1));
    for (iter = pendingMap.begin(); iter != pendingMap.end(); ++iter)
    {
        PendingNotificationEntry entry = *iter;
        if (entry.mFabricIndex == 1)
        {
            BindingTableEntry bindingEntry;
            EXPECT_EQ(sBindingTable.Get(entry.mFabricIndex, entry.mBindingEntryIndex, bindingEntry), CHIP_NO_ERROR);
            EXPECT_NE(bindingEntry.nodeId, std::make_optional(1));
        }
    }
    // Remove all entries on pending map for Fabric 2
    pendingMap.RemoveAllEntriesForFabric(2);
    for (iter = pendingMap.begin(); iter != pendingMap.end(); ++iter)
    {
        PendingNotificationEntry entry = *iter;
        EXPECT_NE(entry.mFabricIndex, 2);
    }
    // Remove all entries on all the fabcics, then the pending map should be empty.
    for (auto fabric : sTestFabrics)
    {
        pendingMap.RemoveAllEntriesForFabric(fabric);
    }
    EXPECT_EQ(pendingMap.begin(), pendingMap.end());
}

TEST_F(TestPendingNotificationMap, TestLRUEntry)
{
    PendingNotificationMap pendingMap(sBindingTable);
    ClearBindingTable(sBindingTable);
    CreateDefaultFullBindingTable(sBindingTable);
    EXPECT_EQ(pendingMap.AddPendingNotification(1, 0, nullptr), CHIP_NO_ERROR); // Add Pending Notification for ScopedNodeId(1, 1)
    EXPECT_EQ(pendingMap.AddPendingNotification(1, 1, nullptr), CHIP_NO_ERROR); // Add Pending Notification for ScopedNodeId(2, 1)
    EXPECT_EQ(pendingMap.AddPendingNotification(1, 3, nullptr), CHIP_NO_ERROR); // Add Pending Notification for ScopedNodeId(1, 1)
    EXPECT_EQ(pendingMap.AddPendingNotification(1, 2, nullptr), CHIP_NO_ERROR); // Add Pending Notification for ScopedNodeId(3, 1)
    EXPECT_EQ(pendingMap.AddPendingNotification(2, 1, nullptr), CHIP_NO_ERROR); // Add Pending Notification for ScopedNodeId(2, 2)

    chip::ScopedNodeId node;
    
    EXPECT_EQ(pendingMap.FindLRUConnectPeer(node), CHIP_NO_ERROR);
    EXPECT_EQ(node.GetNodeId(), 2u);
    EXPECT_EQ(node.GetFabricIndex(), 1u);

    pendingMap.RemoveEntry(1, 1);
    EXPECT_EQ(pendingMap.FindLRUConnectPeer(node), CHIP_NO_ERROR);
    EXPECT_EQ(node.GetNodeId(), 1u);
    EXPECT_EQ(node.GetFabricIndex(), 1u);

    pendingMap.RemoveAllEntriesForFabric(1);
    EXPECT_EQ(pendingMap.FindLRUConnectPeer(node), CHIP_NO_ERROR);
    EXPECT_EQ(node.GetNodeId(), 2u);
    EXPECT_EQ(node.GetFabricIndex(), 2u);
}

} // namespace
