$ErrorActionPreference = "Stop"
cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -DSATPRED_CUDA=OFF @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$PSScriptRoot/build" --config Release
exit $LASTEXITCODE
