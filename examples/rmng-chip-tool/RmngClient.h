/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>

#include <json/json.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct RmngMatterFabric
{
    std::string groupId;
    uint64_t fabricId = 0;
    std::vector<uint8_t> rootCa;
    std::array<uint8_t, 16> ipk{};
    uint32_t adminCat   = 0;
    uint32_t operateCat = 0;
};

struct RmngNodeAssociation
{
    std::string requestId;
    std::array<uint8_t, 32> challenge{};
};

struct RmngIssuedNoc
{
    std::vector<uint8_t> noc;
    uint64_t matterNodeId = 0;
    std::string nodeId;
};

class RmngClient
{
public:
    static RmngClient & GetInstance();

    CHIP_ERROR InstallDeployment(const char * sourcePath);
    CHIP_ERROR Reload();
    CHIP_ERROR Login(const char * username, const char * password);
    CHIP_ERROR Logout();
    CHIP_ERROR SelectGroup(const char * groupId);
    CHIP_ERROR ListGroups(std::vector<RmngMatterFabric> & groups, Json::Value * rawResponse = nullptr);
    CHIP_ERROR GetSelectedFabric(RmngMatterFabric & fabric);

    CHIP_ERROR IssueCommissionerNoc(const std::string & csrPem, RmngIssuedNoc & result);
    CHIP_ERROR InitiateAssociation(RmngNodeAssociation & association);
    CHIP_ERROR VerifyAssociation(const RmngNodeAssociation & association, chip::ByteSpan csrElements,
                                 chip::ByteSpan attestationChallenge, chip::ByteSpan attestationSignature, RmngIssuedNoc & result);
    CHIP_ERROR ConfirmAssociation(const std::string & requestId);
    CHIP_ERROR PutNodeConfiguration(const std::string & nodeId, const Json::Value & configuration);

    std::string GetUsername() const;
    std::string GetSelectedGroupId() const;
    bool HasDeployment() const;
    bool IsLoggedIn() const;

private:
    struct AwsCredentials
    {
        std::string accessKey;
        std::string secretKey;
        std::string sessionToken;
    };

    RmngClient() = default;

    CHIP_ERROR LoadDeployment();
    CHIP_ERROR LoadAccount();
    CHIP_ERROR SaveAccount() const;
    CHIP_ERROR EnsureAwsCredentials();
    CHIP_ERROR LoadCachedFabric(RmngMatterFabric & fabric) const;
    CHIP_ERROR SaveCachedFabric(const RmngMatterFabric & fabric) const;
    CHIP_ERROR InvalidateCachedFabric() const;
    CHIP_ERROR Request(const char * method, const std::string & url, const Json::Value * body, bool signedRequest,
                       Json::Value & response);
    CHIP_ERROR ParseFabric(const Json::Value & value, RmngMatterFabric & fabric) const;
    CHIP_ERROR ParseIssuedNoc(const Json::Value & value, RmngIssuedNoc & result) const;
    std::string PlatformUrl(const std::string & path) const;
    std::string UserUrl(const std::string & path) const;

    Json::Value mDeployment;
    Json::Value mAccount;
    AwsCredentials mAwsCredentials;
};
