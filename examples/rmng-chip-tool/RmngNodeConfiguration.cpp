/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RmngNodeConfiguration.h"

#include <app-common/zap-generated/ids/Clusters.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadPrepareParams.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

constexpr chip::AttributeId kGeneratedCommandList  = 0xFFF8;
constexpr chip::AttributeId kAcceptedCommandList   = 0xFFF9;
constexpr chip::AttributeId kEventList             = 0xFFFA;
constexpr chip::AttributeId kAttributeList         = 0xFFFB;
constexpr chip::AttributeId kSoftwareVersionString = 0x0009;

std::string HexId(uint32_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << value;
    return output.str();
}

void AppendUnique(Json::Value & values, const std::string & value)
{
    for (const auto & current : values)
    {
        if (current.asString() == value)
        {
            return;
        }
    }
    values.append(value);
}

} // namespace

RmngNodeConfiguration::RmngNodeConfiguration() :
    mConnectedCallback(OnConnected, this), mConnectionFailureCallback(OnConnectionFailure, this)
{}

CHIP_ERROR RmngNodeConfiguration::Start(chip::Controller::DeviceCommissioner & commissioner, chip::NodeId matterNodeId,
                                        const std::string & rmngNodeId, Completion completion)
{
    VerifyOrReturnError(!rmngNodeId.empty() && completion, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!mCompletion, CHIP_ERROR_BUSY);
    mConfiguration                       = Json::Value(Json::objectValue);
    mConfiguration["data_model"]         = "matter";
    mConfiguration["info"]["fw_version"] = "unknown";
    mConfiguration["endpoints"]          = Json::Value(Json::objectValue);
    mRmngNodeId                          = rmngNodeId;
    mCompletion                          = std::move(completion);
    mError                               = CHIP_NO_ERROR;
    CHIP_ERROR err = commissioner.GetConnectedDevice(matterNodeId, &mConnectedCallback, &mConnectionFailureCallback,
                                                     chip::TransportPayloadCapability::kMRPPayload);
    if (err != CHIP_NO_ERROR)
    {
        Complete(err);
    }
    return CHIP_NO_ERROR;
}

void RmngNodeConfiguration::OnConnected(void * context, chip::Messaging::ExchangeManager & exchangeManager,
                                        const chip::SessionHandle & sessionHandle)
{
    static_cast<RmngNodeConfiguration *>(context)->SendRead(exchangeManager, sessionHandle);
}

void RmngNodeConfiguration::OnConnectionFailure(void * context, const chip::ScopedNodeId & peerId, CHIP_ERROR error)
{
    static_cast<void>(peerId);
    static_cast<RmngNodeConfiguration *>(context)->Complete(error);
}

void RmngNodeConfiguration::SendRead(chip::Messaging::ExchangeManager & exchangeManager, const chip::SessionHandle & sessionHandle)
{
    mReadClient = std::make_unique<chip::app::ReadClient>(chip::app::InteractionModelEngine::GetInstance(), &exchangeManager, *this,
                                                          chip::app::ReadClient::InteractionType::Read);
    chip::app::AttributePathParams paths[] = {
        chip::app::AttributePathParams(chip::kInvalidEndpointId, chip::kInvalidClusterId, kAttributeList),
        chip::app::AttributePathParams(chip::kInvalidEndpointId, chip::kInvalidClusterId, kEventList),
        chip::app::AttributePathParams(chip::kInvalidEndpointId, chip::kInvalidClusterId, kAcceptedCommandList),
        chip::app::AttributePathParams(chip::kInvalidEndpointId, chip::kInvalidClusterId, kGeneratedCommandList),
        chip::app::AttributePathParams(chip::kRootEndpointId, chip::app::Clusters::BasicInformation::Id, kSoftwareVersionString),
    };
    chip::app::ReadPrepareParams parameters(sessionHandle);
    parameters.mpAttributePathParamsList    = paths;
    parameters.mAttributePathParamsListSize = MATTER_ARRAY_SIZE(paths);
    parameters.mTimeout                     = chip::System::Clock::Seconds16(45);
    CHIP_ERROR err                          = mReadClient->SendRequest(parameters);
    if (err != CHIP_NO_ERROR)
    {
        Complete(err);
    }
}

void RmngNodeConfiguration::AddIds(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader & reader,
                                   const char * direction, const char * listName)
{
    Json::Value & values = mConfiguration["endpoints"][HexId(path.mEndpointId)]["c"][direction][HexId(path.mClusterId)][listName];
    if (!values.isArray())
    {
        values = Json::Value(Json::arrayValue);
    }

    auto addCurrentValue = [&values, listName](chip::TLV::TLVReader & valueReader) {
        uint32_t value = 0;
        CHIP_ERROR err = valueReader.Get(value);
        if (err == CHIP_NO_ERROR && !(strcmp(listName, "a") == 0 && value >= kGeneratedCommandList))
        {
            AppendUnique(values, HexId(value));
        }
    };

    if (reader.GetType() == chip::TLV::kTLVType_Array || reader.GetType() == chip::TLV::kTLVType_List)
    {
        chip::TLV::TLVType container;
        if (reader.EnterContainer(container) != CHIP_NO_ERROR)
        {
            return;
        }
        while (reader.Next() == CHIP_NO_ERROR)
        {
            addCurrentValue(reader);
        }
        static_cast<void>(reader.ExitContainer(container));
        return;
    }
    addCurrentValue(reader);
}

void RmngNodeConfiguration::OnAttributeData(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader * data,
                                            const chip::app::StatusIB & status)
{
    if (!status.IsSuccess() || data == nullptr)
    {
        if (mError == CHIP_NO_ERROR)
        {
            mError = status.IsSuccess() ? CHIP_ERROR_INVALID_ARGUMENT : status.ToChipError();
        }
        return;
    }

    if (path.mEndpointId == chip::kRootEndpointId && path.mClusterId == chip::app::Clusters::BasicInformation::Id &&
        path.mAttributeId == kSoftwareVersionString)
    {
        chip::CharSpan version;
        if (data->Get(version) == CHIP_NO_ERROR)
        {
            mConfiguration["info"]["fw_version"] = std::string(version.data(), version.size());
        }
        return;
    }

    switch (path.mAttributeId)
    {
    case kAttributeList:
        AddIds(path, *data, "s", "a");
        break;
    case kEventList:
        AddIds(path, *data, "s", "e");
        break;
    case kAcceptedCommandList:
        AddIds(path, *data, "s", "c");
        break;
    case kGeneratedCommandList:
        AddIds(path, *data, "c", "c");
        break;
    default:
        break;
    }
}

void RmngNodeConfiguration::OnError(CHIP_ERROR error)
{
    mError = error;
}

void RmngNodeConfiguration::OnDone(chip::app::ReadClient * client)
{
    static_cast<void>(client);
    CHIP_ERROR err = mError;
    if (err == CHIP_NO_ERROR)
    {
        err = RmngClient::GetInstance().PutNodeConfiguration(mRmngNodeId, mConfiguration);
    }
    Complete(err);
}

void RmngNodeConfiguration::Complete(CHIP_ERROR error)
{
    if (!mCompletion)
    {
        return;
    }
    Completion completion = std::move(mCompletion);
    mCompletion           = nullptr;
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(chipTool, "Failed to synchronize RMNG node configuration: %" CHIP_ERROR_FORMAT, error.Format());
    }
    completion(error);
}
