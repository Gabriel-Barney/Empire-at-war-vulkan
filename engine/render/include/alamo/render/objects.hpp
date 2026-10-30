// alamo_render - game object type -> model lookup for environment sky objects (TEE 19/1A name object
// types such as STARS_LOW / DAY_STORM_SKY, not ALO files). A light XML scan over Data\XML\*.XML through
// the VFS: the element whose Name attribute equals the type, then its first *_Model_Name child
// (Space_Model_Name, Land_Model_Name, Galactic_Model_Name). Not a general XML parser; the game
// database module owns real object types.
#pragma once
#include <string>
#include <string_view>

namespace alamo {
class FileProvider;
}

namespace alamo::render {

// "" when the type is not found or has no model name. The result is the model name as written
// (e.g. "W_STARS_LOW.ALO"); pass it to AssetCache / ModelStreamer.
std::string find_object_model(const FileProvider& files, std::string_view object_type);

}  // namespace alamo::render
