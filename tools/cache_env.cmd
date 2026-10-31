@echo off
rem Called by the build scripts (like test_env.cmd): points sccache at the project-local object cache that every worktree and
rem build tree shares (CMakeLists.txt turns sccache on as the compiler launcher when toolchain\sccache\sccache.exe exists).
rem Measured 2026-10-31 (full MSVC build, 16 cores): cold cache 165 s, second tree in the same checkout 28 s, a worktree at
rem another path 114 s (about half the files hit: the test CHECK macros bake __FILE__ into the object). SCCACHE_BASEDIRS and
rem SCCACHE_NO_DAEMON made no difference, so they are not set.
rem No-op when sccache is not installed (toolchain\10_sccache.cmd). Set ALAMO_COMPILER_CACHE=OFF in the environment or pass
rem -DALAMO_COMPILER_CACHE=OFF to cmake for an uncached build. Use "call tools\cache_env.cmd" (no setlocal: the variables must reach the caller).
if exist "%~dp0..\toolchain\sccache\sccache.exe" (
  if not defined SCCACHE_DIR set "SCCACHE_DIR=%~dp0..\toolchain\sccache\cache"
  if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=20G"
)
exit /b 0
