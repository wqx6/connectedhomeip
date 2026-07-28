/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RmngCommands.h"

#include "RmngClient.h"
#include "RmngCredentialIssuerCommands.h"

#include <commands/common/CHIPCommand.h>
#include <commands/common/Command.h>
#include <lib/support/logging/CHIPLogging.h>

#include <cstdio>
#include <memory>
#include <unistd.h>

namespace {

class SetDeploymentCommand : public Command
{
public:
    SetDeploymentCommand() : Command("set-deployment", "Install an rmng-outputs JSON file into /tmp/chip-rmng")
    {
        AddArgument("rmng-outputs", &mPath);
    }
    CHIP_ERROR Run() override { return RmngClient::GetInstance().InstallDeployment(mPath); }

private:
    char * mPath = nullptr;
};

class LoginCommand : public Command
{
public:
    LoginCommand() : Command("login", "Authenticate an RMNG account; the password is prompted when omitted")
    {
        AddArgument("username", &mUsername);
        AddArgument("password", &mPassword, "RMNG password");
    }

    CHIP_ERROR Run() override
    {
        const char * password = nullptr;
        if (mPassword.HasValue())
        {
            password = mPassword.Value();
        }
        else
        {
            password = getpass("RMNG password: ");
        }
        VerifyOrReturnError(password != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
        return RmngClient::GetInstance().Login(mUsername, password);
    }

private:
    char * mUsername = nullptr;
    chip::Optional<char *> mPassword;
};

class LogoutCommand : public Command
{
public:
    LogoutCommand() : Command("logout", "Remove cached RMNG account tokens") {}
    CHIP_ERROR Run() override { return RmngClient::GetInstance().Logout(); }
};

class ListGroupsCommand : public Command
{
public:
    ListGroupsCommand() : Command("list-groups", "List Matter fabrics available to the logged-in account") {}
    CHIP_ERROR Run() override
    {
        std::vector<RmngMatterFabric> groups;
        Json::Value response;
        ReturnErrorOnFailure(RmngClient::GetInstance().ListGroups(groups, &response));
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        fputs(Json::writeString(writer, response).c_str(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return CHIP_NO_ERROR;
    }
};

class SelectGroupCommand : public Command
{
public:
    SelectGroupCommand() : Command("select-group", "Select the RMNG group used for commissioning")
    {
        AddArgument("group-id", &mGroupId);
    }
    CHIP_ERROR Run() override { return RmngClient::GetInstance().SelectGroup(mGroupId); }

private:
    char * mGroupId = nullptr;
};

class StatusCommand : public Command
{
public:
    StatusCommand() : Command("status", "Show local RMNG deployment and account state") {}
    CHIP_ERROR Run() override
    {
        RmngClient & client = RmngClient::GetInstance();
        CHIP_ERROR err      = client.Reload();
        if (err != CHIP_NO_ERROR && err != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
        {
            return err;
        }
        ChipLogProgress(chipTool, "deployment=%s logged-in=%s username=%s selected-group=%s", client.HasDeployment() ? "yes" : "no",
                        client.IsLoggedIn() ? "yes" : "no", client.GetUsername().empty() ? "-" : client.GetUsername().c_str(),
                        client.GetSelectedGroupId().empty() ? "-" : client.GetSelectedGroupId().c_str());
        return CHIP_NO_ERROR;
    }
};

class SynchronizeNodeCommand : public CHIPCommand
{
public:
    explicit SynchronizeNodeCommand(RmngCredentialIssuerCommands & credentialIssuer) :
        CHIPCommand("sync-node", &credentialIssuer, "Retry uploading a commissioned node's Matter configuration"),
        mCredentialIssuer(credentialIssuer)
    {
        AddArgument("matter-node-id", 1, UINT64_MAX, &mMatterNodeId);
        AddArgument("rmng-node-id", &mRmngNodeId);
    }

protected:
    chip::System::Clock::Timeout GetWaitDuration() const override { return chip::System::Clock::Seconds16(90); }

    CHIP_ERROR RunCommand() override
    {
        return mCredentialIssuer.SynchronizeNode(CurrentCommissioner(), mMatterNodeId, mRmngNodeId,
                                                 [this](CHIP_ERROR error) { SetCommandExitStatus(error); });
    }

private:
    RmngCredentialIssuerCommands & mCredentialIssuer;
    chip::NodeId mMatterNodeId = chip::kUndefinedNodeId;
    char * mRmngNodeId         = nullptr;
};

} // namespace

void RegisterRmngCommands(Commands & commands, RmngCredentialIssuerCommands & credentialIssuer)
{
    commands.RegisterCommandSet("rmng",
                                { make_unique<SetDeploymentCommand>(), make_unique<LoginCommand>(), make_unique<LogoutCommand>(),
                                  make_unique<ListGroupsCommand>(), make_unique<SelectGroupCommand>(), make_unique<StatusCommand>(),
                                  make_unique<SynchronizeNodeCommand>(credentialIssuer) },
                                "Configure the RMNG account and select the Matter fabric used by rmng-chip-tool.");
}
