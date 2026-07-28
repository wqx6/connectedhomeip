/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include "RmngClient.h"
#include "RmngNodeConfiguration.h"

#include <commands/example/ExampleCredentialIssuerCommands.h>
#include <controller/OperationalCredentialsDelegate.h>

class RmngOperationalCredentialsIssuer : public chip::Controller::OperationalCredentialsDelegate
{
public:
    CHIP_ERROR ObtainCsrNonce(chip::MutableByteSpan & csrNonce) override;
    CHIP_ERROR GenerateNOCChain(const chip::ByteSpan & csrElements, const chip::ByteSpan & csrNonce,
                                const chip::ByteSpan & attestationSignature, const chip::ByteSpan & attestationChallenge,
                                const chip::ByteSpan & dac, const chip::ByteSpan & pai,
                                chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> * onCompletion) override;

    CHIP_ERROR Prepare();
    CHIP_ERROR Confirm();
    const std::string & GetNodeId() const { return mIssuedNoc.nodeId; }

private:
    RmngNodeAssociation mAssociation;
    RmngIssuedNoc mIssuedNoc;
    RmngMatterFabric mFabric;
};

class RmngCredentialIssuerCommands : public ExampleCredentialIssuerCommands
{
public:
    CHIP_ERROR InitializeCredentialsIssuer(chip::PersistentStorageDelegate & storage) override;
    chip::Controller::OperationalCredentialsDelegate * GetCredentialIssuer() override { return &mOperationalIssuer; }
    void SetCredentialIssuerCATValues(chip::CATValues cats) override { static_cast<void>(cats); }
    CHIP_ERROR GenerateControllerNOCChain(chip::NodeId nodeId, chip::FabricId fabricId, const chip::CATValues & cats,
                                          chip::Crypto::P256Keypair & keypair, chip::MutableByteSpan & rcac,
                                          chip::MutableByteSpan & icac, chip::MutableByteSpan & noc) override;
    CHIP_ERROR ConfigureGroupData(chip::Credentials::GroupDataProvider * provider, chip::FabricIndex fabricIndex,
                                  chip::ByteSpan compressedFabricId) override;
    CHIP_ERROR PrepareCommissioning(chip::NodeId nodeId, CommissioningCallback completion) override;
    CHIP_ERROR FinishCommissioning(chip::Controller::DeviceCommissioner & commissioner, chip::NodeId nodeId,
                                   CommissioningCallback completion) override;
    CHIP_ERROR SynchronizeNode(chip::Controller::DeviceCommissioner & commissioner, chip::NodeId matterNodeId,
                               const std::string & rmngNodeId, CommissioningCallback completion);

private:
    CHIP_ERROR LoadOrCreateCommissionerKey(chip::Crypto::P256Keypair & keypair);

    chip::PersistentStorageDelegate * mStorage = nullptr;
    RmngOperationalCredentialsIssuer mOperationalIssuer;
    RmngNodeConfiguration mNodeConfiguration;
    RmngMatterFabric mFabric;
};
