/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RmngCredentialIssuerCommands.h"

#include <credentials/CHIPCert.h>
#include <credentials/GroupDataProvider.h>
#include <lib/support/CodeUtils.h>

#include <cstring>

namespace {

constexpr char kCommissionerKeyStorageKey[]   = "rmng-commissioner-key";
constexpr char kCommissionerGroupStorageKey[] = "rmng-commissioner-group";
constexpr char kCommissionerNocStorageKey[]   = "rmng-commissioner-noc";
constexpr char kCommissionerRootStorageKey[]  = "rmng-commissioner-root";

CHIP_ERROR CopyCertificate(chip::ByteSpan source, chip::MutableByteSpan & destination)
{
    VerifyOrReturnError(destination.size() >= source.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(destination.data(), source.data(), source.size());
    destination.reduce_size(source.size());
    return CHIP_NO_ERROR;
}

chip::ByteSpan AsSpan(const std::vector<uint8_t> & value)
{
    return chip::ByteSpan(value.data(), value.size());
}

std::string EncodeCsrPem(chip::ByteSpan csr)
{
    chip::Crypto::PemEncoder encoder("CERTIFICATE REQUEST", csr);
    std::string pem;
    while (const char * line = encoder.NextLine())
    {
        pem.append(line);
        pem.push_back('\n');
    }
    return pem;
}

} // namespace

CHIP_ERROR RmngOperationalCredentialsIssuer::Prepare()
{
    mIssuedNoc = {};
    return RmngClient::GetInstance().InitiateAssociation(mAssociation);
}

CHIP_ERROR RmngOperationalCredentialsIssuer::ObtainCsrNonce(chip::MutableByteSpan & csrNonce)
{
    VerifyOrReturnError(csrNonce.size() == mAssociation.challenge.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(csrNonce.data(), mAssociation.challenge.data(), mAssociation.challenge.size());
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngOperationalCredentialsIssuer::GenerateNOCChain(
    const chip::ByteSpan & csrElements, const chip::ByteSpan & csrNonce, const chip::ByteSpan & attestationSignature,
    const chip::ByteSpan & attestationChallenge, const chip::ByteSpan & dac, const chip::ByteSpan & pai,
    chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> * onCompletion)
{
    static_cast<void>(csrNonce);
    static_cast<void>(dac);
    static_cast<void>(pai);
    ReturnErrorOnFailure(RmngClient::GetInstance().GetSelectedFabric(mFabric));
    ReturnErrorOnFailure(RmngClient::GetInstance().VerifyAssociation(mAssociation, csrElements, attestationChallenge,
                                                                     attestationSignature, mIssuedNoc));

    chip::Crypto::IdentityProtectionKeySpan ipk(mFabric.ipk.data());
    chip::NodeId adminSubject = chip::NodeIdFromCASEAuthTag(mFabric.adminCat);
    onCompletion->mCall(onCompletion->mContext, CHIP_NO_ERROR, AsSpan(mIssuedNoc.noc), chip::ByteSpan(), AsSpan(mFabric.rootCa),
                        chip::MakeOptional(ipk), chip::MakeOptional(adminSubject));
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngOperationalCredentialsIssuer::Confirm()
{
    return RmngClient::GetInstance().ConfirmAssociation(mAssociation.requestId);
}

CHIP_ERROR RmngCredentialIssuerCommands::InitializeCredentialsIssuer(chip::PersistentStorageDelegate & storage)
{
    mStorage = &storage;
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngCredentialIssuerCommands::LoadOrCreateCommissionerKey(chip::Crypto::P256Keypair & keypair)
{
    VerifyOrReturnError(mStorage != nullptr, CHIP_ERROR_INCORRECT_STATE);
    chip::Crypto::P256SerializedKeypair serialized;
    uint16_t serializedSize = static_cast<uint16_t>(serialized.Capacity());
    CHIP_ERROR err          = mStorage->SyncGetKeyValue(kCommissionerKeyStorageKey, serialized.Bytes(), serializedSize);
    if (err == CHIP_NO_ERROR)
    {
        VerifyOrReturnError(serializedSize == serialized.Capacity(), CHIP_ERROR_INVALID_ARGUMENT);
        ReturnErrorOnFailure(serialized.SetLength(serializedSize));
        keypair.Clear();
        return keypair.Deserialize(serialized);
    }
    VerifyOrReturnError(err == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND, err);
    ReturnErrorOnFailure(keypair.Serialize(serialized));
    return mStorage->SyncSetKeyValue(kCommissionerKeyStorageKey, serialized.ConstBytes(),
                                     static_cast<uint16_t>(serialized.Length()));
}

CHIP_ERROR RmngCredentialIssuerCommands::GenerateControllerNOCChain(chip::NodeId nodeId, chip::FabricId fabricId,
                                                                    const chip::CATValues & cats,
                                                                    chip::Crypto::P256Keypair & keypair,
                                                                    chip::MutableByteSpan & rcac, chip::MutableByteSpan & icac,
                                                                    chip::MutableByteSpan & noc)
{
    static_cast<void>(nodeId);
    static_cast<void>(fabricId);
    static_cast<void>(cats);
    ReturnErrorOnFailure(RmngClient::GetInstance().GetSelectedFabric(mFabric));
    ReturnErrorOnFailure(LoadOrCreateCommissionerKey(keypair));

    char cachedGroup[128];
    uint16_t cachedGroupSize = sizeof(cachedGroup);
    CHIP_ERROR cacheError    = mStorage->SyncGetKeyValue(kCommissionerGroupStorageKey, cachedGroup, cachedGroupSize);
    if (cacheError == CHIP_NO_ERROR && cachedGroupSize == mFabric.groupId.size() &&
        memcmp(cachedGroup, mFabric.groupId.data(), cachedGroupSize) == 0)
    {
        uint8_t cachedNoc[chip::Controller::kMaxCHIPDERCertLength];
        uint8_t cachedRoot[chip::Controller::kMaxCHIPDERCertLength];
        uint16_t nocSize     = sizeof(cachedNoc);
        uint16_t rootSize    = sizeof(cachedRoot);
        CHIP_ERROR nocError  = mStorage->SyncGetKeyValue(kCommissionerNocStorageKey, cachedNoc, nocSize);
        CHIP_ERROR rootError = mStorage->SyncGetKeyValue(kCommissionerRootStorageKey, cachedRoot, rootSize);
        if (nocError == CHIP_NO_ERROR && rootError == CHIP_NO_ERROR && rootSize == mFabric.rootCa.size() &&
            memcmp(cachedRoot, mFabric.rootCa.data(), rootSize) == 0)
        {
            ReturnErrorOnFailure(CopyCertificate(chip::ByteSpan(cachedNoc, nocSize), noc));
            ReturnErrorOnFailure(CopyCertificate(chip::ByteSpan(cachedRoot, rootSize), rcac));
            icac.reduce_size(0);
            return CHIP_NO_ERROR;
        }
    }

    uint8_t csrBuffer[chip::Crypto::kMIN_CSR_Buffer_Size];
    size_t csrLength = sizeof(csrBuffer);
    ReturnErrorOnFailure(keypair.NewCertificateSigningRequest(csrBuffer, csrLength));
    RmngIssuedNoc issued;
    ReturnErrorOnFailure(
        RmngClient::GetInstance().IssueCommissionerNoc(EncodeCsrPem(chip::ByteSpan(csrBuffer, csrLength)), issued));

    uint8_t chipNocBuffer[chip::Controller::kMaxCHIPDERCertLength];
    chip::MutableByteSpan chipNoc(chipNocBuffer);
    ReturnErrorOnFailure(chip::Credentials::ConvertX509CertToChipCert(AsSpan(issued.noc), chipNoc));
    chip::NodeId issuedNodeId;
    chip::FabricId issuedFabricId;
    ReturnErrorOnFailure(chip::Credentials::ExtractNodeIdFabricIdFromOpCert(chipNoc, &issuedNodeId, &issuedFabricId));
    VerifyOrReturnError(issuedFabricId == mFabric.fabricId && issuedNodeId == issued.matterNodeId, CHIP_ERROR_INVALID_ARGUMENT);

    ReturnErrorOnFailure(CopyCertificate(AsSpan(mFabric.rootCa), rcac));
    icac.reduce_size(0);
    ReturnErrorOnFailure(CopyCertificate(AsSpan(issued.noc), noc));
    VerifyOrReturnError(mFabric.groupId.size() <= UINT16_MAX && noc.size() <= UINT16_MAX && rcac.size() <= UINT16_MAX,
                        CHIP_ERROR_INTERNAL);
    ReturnErrorOnFailure(mStorage->SyncSetKeyValue(kCommissionerGroupStorageKey, mFabric.groupId.data(),
                                                   static_cast<uint16_t>(mFabric.groupId.size())));
    ReturnErrorOnFailure(mStorage->SyncSetKeyValue(kCommissionerNocStorageKey, noc.data(), static_cast<uint16_t>(noc.size())));
    return mStorage->SyncSetKeyValue(kCommissionerRootStorageKey, rcac.data(), static_cast<uint16_t>(rcac.size()));
}

CHIP_ERROR RmngCredentialIssuerCommands::ConfigureGroupData(chip::Credentials::GroupDataProvider * provider,
                                                            chip::FabricIndex fabricIndex, chip::ByteSpan compressedFabricId)
{
    ReturnErrorOnFailure(RmngClient::GetInstance().GetSelectedFabric(mFabric));
    return chip::Credentials::SetSingleIpkEpochKey(provider, fabricIndex, chip::ByteSpan(mFabric.ipk), compressedFabricId);
}

CHIP_ERROR RmngCredentialIssuerCommands::PrepareCommissioning(chip::NodeId nodeId, CommissioningCallback completion)
{
    static_cast<void>(nodeId);
    CHIP_ERROR err = mOperationalIssuer.Prepare();
    completion(err);
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngCredentialIssuerCommands::FinishCommissioning(chip::Controller::DeviceCommissioner & commissioner,
                                                             chip::NodeId nodeId, CommissioningCallback completion)
{
    CHIP_ERROR err = mOperationalIssuer.Confirm();
    if (err != CHIP_NO_ERROR)
    {
        completion(err);
        return CHIP_NO_ERROR;
    }
    return mNodeConfiguration.Start(commissioner, nodeId, mOperationalIssuer.GetNodeId(), std::move(completion));
}

CHIP_ERROR RmngCredentialIssuerCommands::SynchronizeNode(chip::Controller::DeviceCommissioner & commissioner,
                                                         chip::NodeId matterNodeId, const std::string & rmngNodeId,
                                                         CommissioningCallback completion)
{
    return mNodeConfiguration.Start(commissioner, matterNodeId, rmngNodeId, std::move(completion));
}
