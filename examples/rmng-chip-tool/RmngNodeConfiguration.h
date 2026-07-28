/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include "RmngClient.h"

#include <app/ReadClient.h>
#include <controller/CHIPDeviceController.h>

#include <functional>
#include <memory>
#include <string>

class RmngNodeConfiguration : public chip::app::ReadClient::Callback
{
public:
    using Completion = std::function<void(CHIP_ERROR)>;

    RmngNodeConfiguration();
    CHIP_ERROR Start(chip::Controller::DeviceCommissioner & commissioner, chip::NodeId matterNodeId, const std::string & rmngNodeId,
                     Completion completion);

    void OnAttributeData(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader * data,
                         const chip::app::StatusIB & status) override;
    void OnError(CHIP_ERROR error) override;
    void OnDone(chip::app::ReadClient * client) override;

private:
    static void OnConnected(void * context, chip::Messaging::ExchangeManager & exchangeManager,
                            const chip::SessionHandle & sessionHandle);
    static void OnConnectionFailure(void * context, const chip::ScopedNodeId & peerId, CHIP_ERROR error);
    void SendRead(chip::Messaging::ExchangeManager & exchangeManager, const chip::SessionHandle & sessionHandle);
    void AddIds(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader & reader, const char * direction,
                const char * listName);
    void Complete(CHIP_ERROR error);

    chip::Callback::Callback<chip::OnDeviceConnected> mConnectedCallback;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mConnectionFailureCallback;
    std::unique_ptr<chip::app::ReadClient> mReadClient;
    Json::Value mConfiguration;
    std::string mRmngNodeId;
    Completion mCompletion;
    CHIP_ERROR mError = CHIP_NO_ERROR;
};
