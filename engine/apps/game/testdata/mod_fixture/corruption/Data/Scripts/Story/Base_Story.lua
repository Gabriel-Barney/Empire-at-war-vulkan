-- Base story script: loads the library through require (the script roots, through the VFS).
require("FixtureLib")
if Fixture_Lib_Version == nil then
    error("FixtureLib did not load")
end
function Base_Story_Value()
    return Fixture_Lib_Version
end
