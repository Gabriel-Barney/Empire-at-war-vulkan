---
description: Build everything with the project-local MSVC toolchain and summarise the results
---
Run `build_msvc.cmd` from the project root (Windows), wait until `logs\msvc_configure.txt` ends with
`BUILD_DONE`, then summarise: configure result, number of build steps, every warning/error with
file:line, and the ctest pass/fail table from `logs\msvc_build.txt`.
