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

#include <lib/core/StringBuilderAdapters.h>
#include <pw_unit_test/framework.h>

#include <app/clusters/binding-server/BindingTable.h>
#include <lib/support/DefaultStorageKeyAllocator.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <optional>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {

constexpr FabricIndex kTestFabricAlphaIndex = 1;
constexpr FabricIndex kTestFabricBetaIndex  = 2;

void VerifyRestored(chip::TestPersistentStorageDelegate & storage,
                    const std::map<FabricIndex, std::vector<BindingTableEntry>> & expect)
{
    BindingTable restoreTable;
    restoreTable.SetPersistentStorage(&storage);
    BindingTableEntry entry;
    for (auto & kv : expect)
    {
        if (kv.second.size() == 0)
        {
            EXPECT_TRUE(restoreTable.IsEmpty(kv.first));
        }
        else
        {
            uint16_t index = 0;
            for (auto & expectEntry : kv.second)
            {
                EXPECT_EQ(restoreTable.Get(kv.first, index, entry), CHIP_NO_ERROR);
                EXPECT_EQ(entry, expectEntry);
                index++;
            }
        }
    }
}

TEST(TestBindingTable, TestEmptyBindingTable)
{
    BindingTable table;
    chip::TestPersistentStorageDelegate testStorage;
    table.SetPersistentStorage(&testStorage);
    EXPECT_TRUE(table.IsEmpty(kTestFabricAlphaIndex));
    EXPECT_TRUE(table.IsEmpty(kTestFabricBetaIndex));
}

TEST(TestBindingTable, TestAppendThenSet)
{
    BindingTable table;
    chip::TestPersistentStorageDelegate testStorage;
    table.SetPersistentStorage(&testStorage);
    // Appending an invalid entry will return CHIP_ERROR_INVALID_ARGUMENT
    BindingTableEntry invalidEntry;
    EXPECT_EQ(table.Append(kTestFabricAlphaIndex, invalidEntry), CHIP_ERROR_INVALID_ARGUMENT);
    invalidEntry.nodeId.emplace(1);
    invalidEntry.remoteEndpointId.emplace(0);
    invalidEntry.groupId.emplace(1);
    EXPECT_EQ(table.Append(kTestFabricAlphaIndex, invalidEntry), CHIP_ERROR_INVALID_ARGUMENT);
    // Append Binding Table to kMaxBindingEntriesPerFabric
    for (uint16_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
    {
        EXPECT_EQ(table.Append(kTestFabricAlphaIndex, BindingTableEntry(0, index, 1, std::nullopt)), CHIP_NO_ERROR);
    }
    EXPECT_EQ(table.Append(kTestFabricAlphaIndex, BindingTableEntry(1, 0, 1, std::nullopt)),
              CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    EXPECT_EQ(table.Append(kTestFabricBetaIndex, BindingTableEntry(1, 0, 1, std::nullopt)), CHIP_NO_ERROR);
    // Check the entries in the Binding Table
    BindingTableEntry entry;
    for (uint16_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
    {
        EXPECT_EQ(table.Get(kTestFabricAlphaIndex, index, entry), CHIP_NO_ERROR);
        EXPECT_EQ(entry, BindingTableEntry(0, index, 1, std::nullopt, kTestFabricAlphaIndex));
    }
    EXPECT_EQ(table.Get(kTestFabricAlphaIndex, BindingTable::kMaxBindingEntriesPerFabric, entry), CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(table.Set(kTestFabricAlphaIndex, 0, BindingTableEntry(1, 0, 1, std::nullopt)), CHIP_NO_ERROR);
    EXPECT_EQ(table.Get(kTestFabricAlphaIndex, 0, entry), CHIP_NO_ERROR);
    EXPECT_EQ(entry, BindingTableEntry(1, 0, 1, std::nullopt, kTestFabricAlphaIndex));
}

TEST(TestBindingTable, TestRemoveThenAdd)
{
    BindingTable table;
    chip::TestPersistentStorageDelegate testStorage;
    table.SetPersistentStorage(&testStorage);
    // Remove entry when table is empty
    EXPECT_EQ(table.Remove(kTestFabricAlphaIndex, 0), CHIP_ERROR_NOT_FOUND);
    // Append Binding Table to kMaxBindingEntriesPerFabric
    for (uint16_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric; ++index)
    {
        EXPECT_EQ(table.Append(kTestFabricAlphaIndex, BindingTableEntry(0, index, 1, std::nullopt)), CHIP_NO_ERROR);
    }

    // Remove entry with invalid index
    EXPECT_EQ(table.Remove(kTestFabricAlphaIndex, BindingTable::kMaxBindingEntriesPerFabric), CHIP_ERROR_INVALID_ARGUMENT);
    // Remove entry with valid index
    EXPECT_EQ(table.Remove(kTestFabricAlphaIndex, 2), CHIP_NO_ERROR);
    BindingTableEntry entry;
    EXPECT_EQ(table.Get(kTestFabricAlphaIndex, BindingTable::kMaxBindingEntriesPerFabric - 1, entry), CHIP_ERROR_NOT_FOUND);
    for (uint16_t index = 0; index < BindingTable::kMaxBindingEntriesPerFabric - 1; ++index)
    {
        EndpointId expectRemoteEndpoint = index >= 2 ? index + 1 : index;
        EXPECT_EQ(table.Get(kTestFabricAlphaIndex, index, entry), CHIP_NO_ERROR);
        EXPECT_EQ(entry, BindingTableEntry(0, expectRemoteEndpoint, 1, std::nullopt, kTestFabricAlphaIndex));
    }
    // Append entry after removing one
    EXPECT_EQ(table.Append(kTestFabricAlphaIndex, BindingTableEntry(1, 0, 1, std::nullopt)), CHIP_NO_ERROR);
    EXPECT_EQ(table.Get(kTestFabricAlphaIndex, BindingTable::kMaxBindingEntriesPerFabric - 1, entry), CHIP_NO_ERROR);
    EXPECT_EQ(entry, BindingTableEntry(1, 0, 1, std::nullopt, kTestFabricAlphaIndex));
    // Remove all the entries for FabricAlpha
    EXPECT_EQ(table.RemoveAll(kTestFabricAlphaIndex), CHIP_NO_ERROR);
    EXPECT_TRUE(table.IsEmpty(kTestFabricAlphaIndex));
}

TEST(TestBindingTable, TestPersistentStorage)
{
    chip::TestPersistentStorageDelegate testStorage;
    BindingTable table;
    chip::Optional<chip::ClusterId> cluster = chip::MakeOptional<chip::ClusterId>(static_cast<chip::ClusterId>(UINT16_MAX + 6));
    std::map<FabricIndex, std::vector<BindingTableEntry>> expected = {
        { kTestFabricAlphaIndex,
          {
              BindingTableEntry(0, 0, 0, std::nullopt, kTestFabricAlphaIndex),
              BindingTableEntry(1, 1, 0, cluster.std_optional(), kTestFabricAlphaIndex),
          } },
        { kTestFabricBetaIndex,
          {
              BindingTableEntry(2, 0, std::nullopt, kTestFabricBetaIndex),
              BindingTableEntry(3, 0, cluster.std_optional(), kTestFabricBetaIndex),

          } },
    };
    EXPECT_EQ(table.Append(kTestFabricAlphaIndex, expected[kTestFabricAlphaIndex][0]), CHIP_ERROR_INVALID_ARGUMENT);
    table.SetPersistentStorage(&testStorage);
    for (auto & kv : expected)
    {
        for (auto & entry : kv.second)
        {
            EXPECT_EQ(table.Append(kv.first, entry), CHIP_NO_ERROR);
        }
    }
    VerifyRestored(testStorage, expected);

    // Verify storage untouched if add fails
    testStorage.AddPoisonKey(chip::DefaultStorageKeyAllocator::BindingTableEntry(kTestFabricAlphaIndex, 2).KeyName());
    EXPECT_NE(table.Append(kTestFabricAlphaIndex, BindingTableEntry(4, 4, 0, std::nullopt)), CHIP_NO_ERROR);
    VerifyRestored(testStorage, expected);
    testStorage.ClearPoisonKeys();

    // Verify removing head
    EXPECT_EQ(table.Remove(kTestFabricAlphaIndex, 0), CHIP_NO_ERROR);
    VerifyRestored(testStorage,
                   { {
                         kTestFabricAlphaIndex,
                         {
                             expected[kTestFabricAlphaIndex][1],
                         },
                     },
                     { kTestFabricBetaIndex, expected[kTestFabricBetaIndex] } });

    // Verify removing another node
    EXPECT_EQ(table.Remove(kTestFabricBetaIndex, 1), CHIP_NO_ERROR);
    VerifyRestored(testStorage,
                   { {
                         kTestFabricAlphaIndex,
                         {
                             expected[kTestFabricAlphaIndex][1],
                         },
                     },
                     {
                         kTestFabricBetaIndex,
                         {
                             expected[kTestFabricBetaIndex][0],
                         },
                     } });
}

} // namespace
