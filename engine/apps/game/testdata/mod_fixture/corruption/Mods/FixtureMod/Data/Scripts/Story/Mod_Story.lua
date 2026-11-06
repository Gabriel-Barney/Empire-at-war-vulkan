-- Mod story script: errors at load unless require found the mod's FixtureLib.
require("FixtureLib")
if Fixture_Lib_Version ~= 2 then
    error("the base FixtureLib loaded, not the mod's")
end
