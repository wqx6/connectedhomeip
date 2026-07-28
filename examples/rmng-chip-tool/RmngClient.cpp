/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RmngClient.h"

#include <controller/ExamplePersistentStorage.h>
#include <lib/support/Base64.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <curl/curl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr char kDeploymentPath[]     = "/tmp/chip-rmng";
constexpr char kAccountPath[]        = "/tmp/chip_account";
constexpr char kChipToolConfigPath[] = "/tmp/chip_tool_config.ini";
constexpr char kLegacyFabricPath[]   = "/tmp/chip_fabric";
constexpr char kFabricStorageKey[]   = "rmng/fabric";

CHIP_ERROR ReadFile(const char * path, std::string & contents)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    VerifyOrReturnError(fd >= 0, errno == ENOENT ? CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND : CHIP_ERROR_OPEN_FAILED);

    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
    {
        contents.append(buffer, static_cast<size_t>(count));
    }
    int savedErrno = errno;
    close(fd);
    VerifyOrReturnError(count == 0, CHIP_ERROR_READ_FAILED);
    static_cast<void>(savedErrno);
    return CHIP_NO_ERROR;
}

CHIP_ERROR WritePrivateFile(const char * path, const std::string & contents)
{
    std::string temporary = std::string(path) + ".new";
    int fd                = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    VerifyOrReturnError(fd >= 0, CHIP_ERROR_OPEN_FAILED);
    VerifyOrReturnError(fchmod(fd, S_IRUSR | S_IWUSR) == 0, CHIP_ERROR_WRITE_FAILED, close(fd));

    size_t offset = 0;
    while (offset < contents.size())
    {
        ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count <= 0)
        {
            close(fd);
            return CHIP_ERROR_WRITE_FAILED;
        }
        offset += static_cast<size_t>(count);
    }
    VerifyOrReturnError(fsync(fd) == 0, CHIP_ERROR_WRITE_FAILED, close(fd));
    VerifyOrReturnError(close(fd) == 0, CHIP_ERROR_WRITE_FAILED);
    VerifyOrReturnError(rename(temporary.c_str(), path) == 0, CHIP_ERROR_WRITE_FAILED);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ParseJson(const std::string & text, Json::Value & value)
{
    Json::CharReaderBuilder builder;
    std::istringstream input(text);
    std::string errors;
    VerifyOrReturnError(Json::parseFromStream(builder, input, &value, &errors), CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

const Json::Value * FindString(const Json::Value & value, const char * key)
{
    if (value.isObject())
    {
        if (value.isMember(key) && value[key].isString())
        {
            return &value[key];
        }
        for (const auto & name : value.getMemberNames())
        {
            if (const Json::Value * found = FindString(value[name], key))
            {
                return found;
            }
        }
    }
    else if (value.isArray())
    {
        for (const auto & item : value)
        {
            if (const Json::Value * found = FindString(item, key))
            {
                return found;
            }
        }
    }
    return nullptr;
}

std::string GetDeploymentString(const Json::Value & deployment, const char * key)
{
    const Json::Value * value = FindString(deployment, key);
    return value == nullptr ? std::string() : value->asString();
}

std::string JoinUrl(const std::string & base, const std::string & path)
{
    if (!base.empty() && base.back() == '/' && !path.empty() && path.front() == '/')
    {
        return base.substr(0, base.size() - 1) + path;
    }
    return base + path;
}

size_t CurlWrite(char * data, size_t size, size_t count, void * context)
{
    auto * output = static_cast<std::string *>(context);
    size_t bytes  = size * count;
    output->append(data, bytes);
    return bytes;
}

CHIP_ERROR EnsureCurlInitialized()
{
    static CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    return result == CURLE_OK ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

int HexNibble(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

CHIP_ERROR DecodeHex(const std::string & text, chip::MutableByteSpan output)
{
    VerifyOrReturnError(text.size() == output.size() * 2, CHIP_ERROR_INVALID_ARGUMENT);
    for (size_t index = 0; index < output.size(); ++index)
    {
        int high = HexNibble(text[index * 2]);
        int low  = HexNibble(text[index * 2 + 1]);
        VerifyOrReturnError(high >= 0 && low >= 0, CHIP_ERROR_INVALID_ARGUMENT);
        output[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodeHex(const std::string & text, std::vector<uint8_t> & output)
{
    VerifyOrReturnError(text.size() % 2 == 0, CHIP_ERROR_INVALID_ARGUMENT);
    output.resize(text.size() / 2);
    chip::MutableByteSpan span(output.data(), output.size());
    return DecodeHex(text, span);
}

CHIP_ERROR ParseHex64(const std::string & text, uint64_t & output)
{
    VerifyOrReturnError(!text.empty() && text.size() <= 16, CHIP_ERROR_INVALID_ARGUMENT);
    output = 0;
    for (char character : text)
    {
        int nibble = HexNibble(character);
        VerifyOrReturnError(nibble >= 0, CHIP_ERROR_INVALID_ARGUMENT);
        output = (output << 4) | static_cast<uint64_t>(nibble);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodePemCertificate(const std::string & pem, std::vector<uint8_t> & der)
{
    constexpr char begin[] = "-----BEGIN CERTIFICATE-----";
    constexpr char end[]   = "-----END CERTIFICATE-----";
    size_t first           = pem.find(begin);
    size_t last            = pem.find(end);
    VerifyOrReturnError(first != std::string::npos && last != std::string::npos && last > first, CHIP_ERROR_INVALID_ARGUMENT);
    first += strlen(begin);

    std::string base64;
    for (size_t index = first; index < last; ++index)
    {
        if (pem[index] != '\r' && pem[index] != '\n' && pem[index] != ' ' && pem[index] != '\t')
        {
            base64.push_back(pem[index]);
        }
    }
    VerifyOrReturnError(base64.size() <= UINT16_MAX, CHIP_ERROR_INVALID_ARGUMENT);
    der.resize((base64.size() * 3) / 4 + 3);
    uint16_t decoded = chip::Base64Decode(base64.data(), static_cast<uint16_t>(base64.size()), der.data());
    VerifyOrReturnError(decoded != UINT16_MAX, CHIP_ERROR_INVALID_ARGUMENT);
    der.resize(decoded);
    return CHIP_NO_ERROR;
}

std::string HexEncode(chip::ByteSpan value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (size_t index = 0; index < value.size(); ++index)
    {
        result[index * 2]     = digits[value[index] >> 4];
        result[index * 2 + 1] = digits[value[index] & 0x0f];
    }
    return result;
}

} // namespace

RmngClient & RmngClient::GetInstance()
{
    static RmngClient instance;
    return instance;
}

CHIP_ERROR RmngClient::Reload()
{
    CHIP_ERROR deploymentError = LoadDeployment();
    CHIP_ERROR accountError    = LoadAccount();
    if (deploymentError != CHIP_NO_ERROR && deploymentError != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
    {
        return deploymentError;
    }
    if (accountError != CHIP_NO_ERROR && accountError != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
    {
        return accountError;
    }
    return (deploymentError == CHIP_NO_ERROR || accountError == CHIP_NO_ERROR) ? CHIP_NO_ERROR
                                                                               : CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
}

CHIP_ERROR RmngClient::LoadDeployment()
{
    std::string text;
    ReturnErrorOnFailure(ReadFile(kDeploymentPath, text));
    ReturnErrorOnFailure(ParseJson(text, mDeployment));
    VerifyOrReturnError(!GetDeploymentString(mDeployment, "ApiGatewayUrl").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!GetDeploymentString(mDeployment, "EspUserApiUrl").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!GetDeploymentString(mDeployment, "StackRegion").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::LoadAccount()
{
    std::string text;
    CHIP_ERROR err = ReadFile(kAccountPath, text);
    if (err == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
    {
        mAccount = Json::Value(Json::objectValue);
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(err);
    return ParseJson(text, mAccount);
}

CHIP_ERROR RmngClient::SaveAccount() const
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return WritePrivateFile(kAccountPath, Json::writeString(builder, mAccount));
}

CHIP_ERROR RmngClient::InstallDeployment(const char * sourcePath)
{
    VerifyOrReturnError(sourcePath != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    std::string text;
    Json::Value deployment;
    ReturnErrorOnFailure(ReadFile(sourcePath, text));
    ReturnErrorOnFailure(ParseJson(text, deployment));
    VerifyOrReturnError(!GetDeploymentString(deployment, "ApiGatewayUrl").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!GetDeploymentString(deployment, "EspUserApiUrl").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!GetDeploymentString(deployment, "StackRegion").empty(), CHIP_ERROR_INVALID_ARGUMENT);
    ReturnErrorOnFailure(WritePrivateFile(kDeploymentPath, text));
    mDeployment = std::move(deployment);
    return InvalidateCachedFabric();
}

CHIP_ERROR RmngClient::Request(const char * method, const std::string & url, const Json::Value * body, bool signedRequest,
                               Json::Value & response)
{
    ReturnErrorOnFailure(EnsureCurlInitialized());
    CURL * curl = curl_easy_init();
    VerifyOrReturnError(curl != nullptr, CHIP_ERROR_NO_MEMORY);

    struct curl_slist * headers = nullptr;
    headers                     = curl_slist_append(headers, "Accept: application/json");
    std::string requestBody;
    if (body != nullptr)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        requestBody            = Json::writeString(builder, *body);
        headers                = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    }

    if (signedRequest)
    {
        std::string region = GetDeploymentString(mDeployment, "StackRegion");
        std::string sigv4  = "aws:amz:" + region + ":execute-api";
        std::string user   = mAwsCredentials.accessKey + ":" + mAwsCredentials.secretKey;
        std::string token  = "X-Amz-Security-Token: " + mAwsCredentials.sessionToken;
        headers            = curl_slist_append(headers, token.c_str());
        curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, sigv4.c_str());
        curl_easy_setopt(curl, CURLOPT_USERPWD, user.c_str());
    }

    std::string output;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);

    CURLcode curlResult = curl_easy_perform(curl);
    long status         = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    VerifyOrReturnError(curlResult == CURLE_OK, CHIP_ERROR_CONNECTION_ABORTED);
    if (!output.empty())
    {
        ReturnErrorOnFailure(ParseJson(output, response));
    }
    else
    {
        response = Json::Value(Json::objectValue);
    }
    if (status < 200 || status >= 300)
    {
        ChipLogError(chipTool, "RMNG request failed: HTTP %ld", status);
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

std::string RmngClient::PlatformUrl(const std::string & path) const
{
    return JoinUrl(GetDeploymentString(mDeployment, "ApiGatewayUrl"), path);
}

std::string RmngClient::UserUrl(const std::string & path) const
{
    return JoinUrl(GetDeploymentString(mDeployment, "EspUserApiUrl"), path);
}

CHIP_ERROR RmngClient::Login(const char * username, const char * password)
{
    VerifyOrReturnError(username != nullptr && password != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    ReturnErrorOnFailure(LoadDeployment());
    Json::Value body(Json::objectValue);
    body["username"] = username;
    body["password"] = password;
    Json::Value response;
    ReturnErrorOnFailure(Request("POST", UserUrl("/v1/user/auth/token"), &body, false, response));
    VerifyOrReturnError(response["access_token"].isString(), CHIP_ERROR_INVALID_ARGUMENT);
    mAccount                   = Json::Value(Json::objectValue);
    mAccount["username"]       = username;
    mAccount["access_token"]   = response["access_token"];
    mAccount["refresh_token"]  = response["refresh_token"];
    mAccount["id_token"]       = response["id_token"];
    mAccount["selected_group"] = "";
    mAwsCredentials            = {};
    ReturnErrorOnFailure(InvalidateCachedFabric());
    return SaveAccount();
}

CHIP_ERROR RmngClient::Logout()
{
    mAccount        = Json::Value(Json::objectValue);
    mAwsCredentials = {};
    ReturnErrorOnFailure(InvalidateCachedFabric());
    return unlink(kAccountPath) == 0 || errno == ENOENT ? CHIP_NO_ERROR : CHIP_ERROR_WRITE_FAILED;
}

CHIP_ERROR RmngClient::InvalidateCachedFabric() const
{
    VerifyOrReturnError(unlink(kLegacyFabricPath) == 0 || errno == ENOENT, CHIP_ERROR_WRITE_FAILED);
    PersistentStorage storage;
    ReturnErrorOnFailure(storage.Init());
    if (!storage.SyncDoesKeyExist(kFabricStorageKey))
    {
        return CHIP_NO_ERROR;
    }
    return storage.SyncDeleteKeyValue(kFabricStorageKey);
}

CHIP_ERROR RmngClient::SaveCachedFabric(const RmngMatterFabric & fabric) const
{
    Json::Value value(Json::objectValue);
    value["group_id"]    = fabric.groupId;
    value["fabric_id"]   = Json::UInt64(fabric.fabricId);
    value["root_ca_der"] = HexEncode(chip::ByteSpan(fabric.rootCa.data(), fabric.rootCa.size()));
    value["ipk"]         = HexEncode(chip::ByteSpan(fabric.ipk));
    value["admin_cat"]   = fabric.adminCat;
    value["operate_cat"] = fabric.operateCat;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::string serialized = Json::writeString(builder, value);
    VerifyOrReturnError(serialized.size() <= UINT16_MAX, CHIP_ERROR_MESSAGE_TOO_LONG);
    PersistentStorage storage;
    ReturnErrorOnFailure(storage.Init());
    ReturnErrorOnFailure(storage.SyncSetKeyValue(kFabricStorageKey, serialized.data(), static_cast<uint16_t>(serialized.size())));
    VerifyOrReturnError(chmod(kChipToolConfigPath, S_IRUSR | S_IWUSR) == 0, CHIP_ERROR_WRITE_FAILED);
    VerifyOrReturnError(unlink(kLegacyFabricPath) == 0 || errno == ENOENT, CHIP_ERROR_WRITE_FAILED);
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::LoadCachedFabric(RmngMatterFabric & fabric) const
{
    PersistentStorage storage;
    ReturnErrorOnFailure(storage.Init());
    uint8_t buffer[4096];
    uint16_t size = sizeof(buffer);
    ReturnErrorOnFailure(storage.SyncGetKeyValue(kFabricStorageKey, buffer, size));
    std::string text(reinterpret_cast<char *>(buffer), size);
    Json::Value value;
    ReturnErrorOnFailure(ParseJson(text, value));
    VerifyOrReturnError(value["group_id"].isString() && value["fabric_id"].isUInt64() && value["root_ca_der"].isString() &&
                            value["ipk"].isString() && value["admin_cat"].isUInt() && value["operate_cat"].isUInt(),
                        CHIP_ERROR_INVALID_ARGUMENT);
    fabric.groupId    = value["group_id"].asString();
    fabric.fabricId   = value["fabric_id"].asUInt64();
    fabric.adminCat   = value["admin_cat"].asUInt();
    fabric.operateCat = value["operate_cat"].asUInt();
    ReturnErrorOnFailure(DecodeHex(value["root_ca_der"].asString(), fabric.rootCa));
    chip::MutableByteSpan ipk(fabric.ipk);
    return DecodeHex(value["ipk"].asString(), ipk);
}

CHIP_ERROR RmngClient::EnsureAwsCredentials()
{
    ReturnErrorOnFailure(LoadDeployment());
    ReturnErrorOnFailure(LoadAccount());
    VerifyOrReturnError(mAccount["id_token"].isString(), CHIP_ERROR_INCORRECT_STATE);

    ReturnErrorOnFailure(EnsureCurlInitialized());
    Json::Value response;
    CURL * curl = curl_easy_init();
    VerifyOrReturnError(curl != nullptr, CHIP_ERROR_NO_MEMORY);
    struct curl_slist * headers = nullptr;
    // This API Gateway Cognito authorizer expects the ID token. The access
    // token returned by the same login endpoint is rejected with HTTP 401.
    std::string token = "Authorization: " + mAccount["id_token"].asString();
    headers           = curl_slist_append(headers, token.c_str());
    std::string output;
    std::string credentialsUrl = PlatformUrl("/v1/user/credentials");
    curl_easy_setopt(curl, CURLOPT_URL, credentialsUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    long status         = 0;
    CURLcode curlResult = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (curlResult != CURLE_OK)
    {
        ChipLogError(chipTool, "RMNG credentials request failed: %s", curl_easy_strerror(curlResult));
        return CHIP_ERROR_CONNECTION_ABORTED;
    }
    if (status < 200 || status >= 300)
    {
        ChipLogError(chipTool, "RMNG credentials request failed: HTTP %ld", status);
        return CHIP_ERROR_ACCESS_DENIED;
    }
    ReturnErrorOnFailure(ParseJson(output, response));
    VerifyOrReturnError(response["access_key_id"].isString() && response["secret_access_key"].isString() &&
                            response["session_token"].isString(),
                        CHIP_ERROR_INVALID_ARGUMENT);
    mAwsCredentials.accessKey    = response["access_key_id"].asString();
    mAwsCredentials.secretKey    = response["secret_access_key"].asString();
    mAwsCredentials.sessionToken = response["session_token"].asString();
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::ParseFabric(const Json::Value & value, RmngMatterFabric & fabric) const
{
    const Json::Value & matter = value["matter"];
    VerifyOrReturnError(matter.isObject(), CHIP_ERROR_INVALID_ARGUMENT);
    fabric.groupId = value.get("group_id", value.get("id", "")).asString();
    ReturnErrorOnFailure(ParseHex64(matter["fabric_id"].asString(), fabric.fabricId));
    ReturnErrorOnFailure(DecodePemCertificate(matter["root_ca"].asString(), fabric.rootCa));
    chip::MutableByteSpan ipk(fabric.ipk);
    ReturnErrorOnFailure(DecodeHex(matter["ipk"].asString(), ipk));
    uint64_t adminCat   = 0;
    uint64_t operateCat = 0;
    ReturnErrorOnFailure(ParseHex64(matter["group_cat_id_admin"].asString(), adminCat));
    ReturnErrorOnFailure(ParseHex64(matter["group_cat_id_operate"].asString(), operateCat));
    VerifyOrReturnError(adminCat <= UINT32_MAX && operateCat <= UINT32_MAX, CHIP_ERROR_INVALID_ARGUMENT);
    fabric.adminCat   = static_cast<uint32_t>(adminCat);
    fabric.operateCat = static_cast<uint32_t>(operateCat);
    VerifyOrReturnError(!fabric.groupId.empty(), CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::ListGroups(std::vector<RmngMatterFabric> & groups, Json::Value * rawResponse)
{
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value response;
    ReturnErrorOnFailure(Request("GET", PlatformUrl("/v1/groups"), nullptr, true, response));
    if (rawResponse != nullptr)
    {
        *rawResponse = response;
    }
    const Json::Value & values = response.isArray() ? response : response["groups"];
    VerifyOrReturnError(values.isArray(), CHIP_ERROR_INVALID_ARGUMENT);
    groups.clear();
    std::string selected = mAccount.get("selected_group", "").asString();
    for (const auto & value : values)
    {
        if (!value["matter"].isObject())
        {
            continue;
        }
        RmngMatterFabric fabric;
        ReturnErrorOnFailure(ParseFabric(value, fabric));
        if (fabric.groupId == selected)
        {
            ReturnErrorOnFailure(SaveCachedFabric(fabric));
        }
        groups.push_back(std::move(fabric));
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::SelectGroup(const char * groupId)
{
    VerifyOrReturnError(groupId != nullptr && groupId[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    std::vector<RmngMatterFabric> groups;
    ReturnErrorOnFailure(ListGroups(groups));
    for (const auto & group : groups)
    {
        if (group.groupId == groupId)
        {
            mAccount["selected_group"] = groupId;
            ReturnErrorOnFailure(SaveCachedFabric(group));
            return SaveAccount();
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR RmngClient::GetSelectedFabric(RmngMatterFabric & fabric)
{
    ReturnErrorOnFailure(LoadAccount());
    std::string selected = mAccount.get("selected_group", "").asString();
    VerifyOrReturnError(!selected.empty(), CHIP_ERROR_INCORRECT_STATE);
    CHIP_ERROR cacheError = LoadCachedFabric(fabric);
    if (cacheError == CHIP_NO_ERROR && fabric.groupId == selected)
    {
        return CHIP_NO_ERROR;
    }
    std::vector<RmngMatterFabric> groups;
    ReturnErrorOnFailure(ListGroups(groups));
    for (auto & group : groups)
    {
        if (group.groupId == selected)
        {
            fabric = std::move(group);
            return SaveCachedFabric(fabric);
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR RmngClient::ParseIssuedNoc(const Json::Value & value, RmngIssuedNoc & result) const
{
    ReturnErrorOnFailure(DecodePemCertificate(value["noc"].asString(), result.noc));
    ReturnErrorOnFailure(ParseHex64(value["matter_node_id"].asString(), result.matterNodeId));
    result.nodeId = value.get("node_id", "").asString();
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::IssueCommissionerNoc(const std::string & csrPem, RmngIssuedNoc & result)
{
    RmngMatterFabric fabric;
    ReturnErrorOnFailure(GetSelectedFabric(fabric));
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value body(Json::objectValue);
    body["csr"] = csrPem;
    Json::Value response;
    ReturnErrorOnFailure(Request("POST", PlatformUrl("/v1/groups/" + fabric.groupId + "/matter-nocs"), &body, true, response));
    return ParseIssuedNoc(response, result);
}

CHIP_ERROR RmngClient::InitiateAssociation(RmngNodeAssociation & association)
{
    RmngMatterFabric fabric;
    ReturnErrorOnFailure(GetSelectedFabric(fabric));
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value body(Json::objectValue);
    Json::Value response;
    ReturnErrorOnFailure(
        Request("POST", PlatformUrl("/v1/groups/" + fabric.groupId + "/node-assoc-requests"), &body, true, response));
    association.requestId = response["request_id"].asString();
    chip::MutableByteSpan challenge(association.challenge);
    ReturnErrorOnFailure(DecodeHex(response["challenge"].asString(), challenge));
    VerifyOrReturnError(!association.requestId.empty(), CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR RmngClient::VerifyAssociation(const RmngNodeAssociation & association, chip::ByteSpan csrElements,
                                         chip::ByteSpan attestationChallenge, chip::ByteSpan attestationSignature,
                                         RmngIssuedNoc & result)
{
    RmngMatterFabric fabric;
    ReturnErrorOnFailure(GetSelectedFabric(fabric));
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value body(Json::objectValue);
    body["nocsr_elements"]        = HexEncode(csrElements);
    body["attestation_challenge"] = HexEncode(attestationChallenge);
    body["attestation_signature"] = HexEncode(attestationSignature);
    Json::Value response;
    std::string path = "/v1/groups/" + fabric.groupId + "/node-assoc-requests/" + association.requestId + "/verify";
    ReturnErrorOnFailure(Request("POST", PlatformUrl(path), &body, true, response));
    return ParseIssuedNoc(response, result);
}

CHIP_ERROR RmngClient::ConfirmAssociation(const std::string & requestId)
{
    RmngMatterFabric fabric;
    ReturnErrorOnFailure(GetSelectedFabric(fabric));
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value body(Json::objectValue);
    Json::Value response;
    std::string path = "/v1/groups/" + fabric.groupId + "/node-assoc-requests/" + requestId + "/confirm";
    return Request("POST", PlatformUrl(path), &body, true, response);
}

CHIP_ERROR RmngClient::PutNodeConfiguration(const std::string & nodeId, const Json::Value & configuration)
{
    RmngMatterFabric fabric;
    ReturnErrorOnFailure(GetSelectedFabric(fabric));
    ReturnErrorOnFailure(EnsureAwsCredentials());
    Json::Value response;
    std::string path = "/v1/groups/" + fabric.groupId + "/nodes/" + nodeId + "/config";
    return Request("PUT", PlatformUrl(path), &configuration, true, response);
}

std::string RmngClient::GetUsername() const
{
    return mAccount.get("username", "").asString();
}

std::string RmngClient::GetSelectedGroupId() const
{
    return mAccount.get("selected_group", "").asString();
}

bool RmngClient::HasDeployment() const
{
    struct stat info;
    return stat(kDeploymentPath, &info) == 0;
}

bool RmngClient::IsLoggedIn() const
{
    return !GetUsername().empty() && mAccount["access_token"].isString();
}
