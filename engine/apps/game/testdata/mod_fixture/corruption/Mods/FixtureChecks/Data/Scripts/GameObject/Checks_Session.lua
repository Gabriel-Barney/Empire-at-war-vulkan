-- Main chunk reads a GlobalValue that Checks_Scoring sets in the session: fails here only for want of session state.
local id = GlobalValue.Get("CHECKS_MOD_ID")
Checks_Path = "checks-" .. string.lower(id)
