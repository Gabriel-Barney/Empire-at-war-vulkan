---
name: build
description: Full MSVC build of every module with the project-local toolchain (build_msvc.cmd) plus ctest, then a summary of warnings, errors and test results. Use when asked to build everything, run the full build, or check that the whole tree is green; for quick iteration on a few modules use build_agent.cmd instead (docs/BUILD.md).
---
Run `build_msvc.cmd` from the project root (Windows), wait until `logs\msvc_configure.txt` ends with
`BUILD_DONE`, then summarise: configure result, number of build steps, every warning/error with
file:line, and the ctest pass/fail table from `logs\msvc_build.txt`.
