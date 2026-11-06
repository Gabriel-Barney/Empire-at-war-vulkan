-- Sets a GlobalValue only when the session calls Base_Definitions (as a game scoring script does at startup): its main
-- chunk sets nothing.
function Base_Definitions()
    GlobalValue.Set("CHECKS_MOD_ID", "Checks")
end
