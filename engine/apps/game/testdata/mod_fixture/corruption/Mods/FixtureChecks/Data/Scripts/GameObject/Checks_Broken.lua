-- Main chunk reads a GlobalValue no script sets: a real load error.
local id = GlobalValue.Get("CHECKS_NEVER_SET")
Checks_Path = "checks-" .. string.lower(id)
