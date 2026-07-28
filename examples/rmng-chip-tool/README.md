# RMNG chip-tool

`rmng-chip-tool` is a Linux commissioner that obtains its Matter fabric and
operational certificates from RMNG. It keeps the deployment output at
`/tmp/chip-rmng` and account tokens and the selected group at
`/tmp/chip_account`. The selected fabric is cached under the `rmng/fabric` key
in chip-tool's existing `/tmp/chip_tool_config.ini`, avoiding a group query
during commissioner startup. Files containing RMNG credentials are restricted
to mode `0600`.

## Build

The example is intentionally standalone and is not added to the central example
target registry. Its command framework and project configuration are local to
this directory; it does not link `examples/chip-tool:chip-tool-utils` or use
`examples/chip-tool` as an include directory. It requires libcurl development
headers.

```sh
scripts/run_in_build_env.sh "gn gen out/rmng-chip-tool --root=examples/rmng-chip-tool"
scripts/run_in_build_env.sh "ninja -C out/rmng-chip-tool"
```

## Configure

```sh
out/rmng-chip-tool/rmng-chip-tool rmng set-deployment /path/to/rmng-outputs.json
out/rmng-chip-tool/rmng-chip-tool rmng login user@example.com
out/rmng-chip-tool/rmng-chip-tool rmng list-groups
out/rmng-chip-tool/rmng-chip-tool rmng select-group GROUP_ID
out/rmng-chip-tool/rmng-chip-tool rmng status
```

The password prompt avoids putting the password in shell history. Normal
`pairing` commands then use the selected RMNG fabric. The commissioner NOC is
issued by the Matter Fabric API, and an end-device NOC is issued through the
node-association initiate, verify, and confirm flow. After commissioning, the
tool reads AttributeList, EventList, AcceptedCommandList, and
GeneratedCommandList (plus the software version) and uploads the resulting
Matter node configuration. Commissioning returns a failure if this upload
fails. It can be retried without recommissioning:

```sh
out/rmng-chip-tool/rmng-chip-tool rmng sync-node MATTER_NODE_ID RMNG_NODE_ID
```

Do not share either `/tmp/chip-rmng` or `/tmp/chip_account`; they contain
deployment and login credentials.
