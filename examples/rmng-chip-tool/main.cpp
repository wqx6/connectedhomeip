/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RmngCommands.h"
#include "RmngCredentialIssuerCommands.h"

#include <commands/clusters/SubscriptionsCommands.h>
#include <commands/common/Commands.h>
#include <commands/dcl/Commands.h>
#include <commands/delay/Commands.h>
#include <commands/discover/Commands.h>
#include <commands/group/Commands.h>
#include <commands/icd/ICDCommand.h>
#include <commands/interactive/Commands.h>
#include <commands/pairing/Commands.h>
#include <commands/payload/Commands.h>
#include <commands/session-management/Commands.h>
#include <commands/storage/Commands.h>
#include <zap-generated/cluster/Commands.h>

int main(int argc, char * argv[])
{
    RmngCredentialIssuerCommands credentialIssuer;
    Commands commands;
    RegisterRmngCommands(commands, credentialIssuer);
    registerCommandsDCL(commands);
    registerCommandsDelay(commands, &credentialIssuer);
    registerCommandsDiscover(commands, &credentialIssuer);
    registerCommandsICD(commands, &credentialIssuer);
    registerCommandsInteractive(commands, &credentialIssuer);
    registerCommandsPayload(commands);
    registerCommandsPairing(commands, &credentialIssuer);
    registerCommandsGroup(commands, &credentialIssuer);
    registerClusters(commands, &credentialIssuer);
    registerCommandsSubscriptions(commands, &credentialIssuer);
    registerCommandsStorage(commands);
    registerCommandsSessionManagement(commands, &credentialIssuer);
    return commands.Run(argc, argv);
}
