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

#include <app-common/zap-generated/callback.h>
#include <app/PluginApplicationCallbacks.h>
#include <app/util/attribute-storage.h>
#include <app/clusters/binding-server/BindingCluster.h>
#include <app/static-cluster-config/Binding.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <data-model-providers/codegen/ServerClusterInterfaceRegistry.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {
static constexpr size_t kBindingFixedClusterCount = Binding::StaticApplicationConfig::kFixedClusterConfig.size();

LazyRegisteredServerCluster<BindingServerCluster> gServers[kBindingFixedClusterCount];
} // namespace

void emberAfBindingClusterInitCallback(EndpointId endpoint)
{
    uint16_t endpointIndex = emberAfGetClusterServerEndpointIndex(endpoint, Binding::Id, kBindingFixedClusterCount);
    if (endpointIndex == kEmberInvalidEndpointIndex)
    {
        return;
    }
    gServers[endpointIndex].Create(endpoint);
    CHIP_ERROR err = CodegenDataModelProvider::Instance().Registry().Register(gServers[endpointIndex].Registration());
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Binding register error: endpoint %u, %" CHIP_ERROR_FORMAT, endpoint, err.Format());
    }
}

void emberAfBindingClusterShutdownCallback(EndpointId endpoint)
{
    uint16_t endpointIndex = emberAfGetClusterServerEndpointIndex(endpoint, Binding::Id, kBindingFixedClusterCount);
    if (endpointIndex == kEmberInvalidEndpointIndex)
    {
        return;
    }
    CHIP_ERROR err = CodegenDataModelProvider::Instance().Registry().Unregister(&gServers[endpointIndex].Cluster());
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Binding unregister error: endpoint %u, %" CHIP_ERROR_FORMAT, endpoint, err.Format());
    }
    gServers[endpointIndex].Destroy();
}

void MatterBindingPluginServerInitCallback() {}
void MatterBindingPluginServerShutdownCallback() {}
