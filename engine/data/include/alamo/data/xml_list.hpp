// List XML files: "Data\XML\<Name>Files.xml" holds <File> entries, each naming "Data\XML\<entry>"
// (GameObjectFiles, FactionFiles, CampaignFiles, TradeRouteFiles, HardPointDataFiles, SFXEventFiles,
// TargetingPrioritySetFiles, CommandBarComponentFiles, ...).
//
// cu5 t02: the one place that walks such a list. Free and stateless; every function keeps LIST ORDER and
// DUPLICATES (a file listed twice is visited twice), so a loader written on top of it reads exactly what its
// hand-written loop did. What differs between the loaders (empty entries, missing or unparsable listed files,
// the element name of an entry) is an explicit option, never a silent default change. De-duplication and
// other orders (TradeRouteFiles / CampaignFiles: ascending list_file_crc) stay with the caller.
//
// Diagnostics are the caller's: the helpers return a status and, with visit_missing / visit_unparsable, hand the
// failed file to the callback, so each loader keeps its own warning texts.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::data {

inline constexpr std::string_view kListedXmlDir = "Data\\XML\\";

// One entry of a list file.
struct ListedXmlEntry {
    std::string name;   // the entry's text, surrounding whitespace trimmed, as written
    std::string path;   // kListedXmlDir + name
    uint32_t line = 0;  // line of the entry element in the list file (diagnostics)
};

struct ListedXmlOptions {
    // Element name of an entry (case-insensitive). Empty: every child element of the list's root counts.
    std::string_view entry_tag = "File";
    // An entry with empty text: skipped (default), or kept (its path is the bare "Data\XML\", which no provider
    // opens, so it reaches the callback as a missing file when visit_missing is set).
    bool keep_empty_entries = false;
    // A listed file the provider cannot read: skipped (default), or passed to the callback with no bytes / doc.
    bool visit_missing = false;
    // A listed file that parses to no root element (XmlDocument::ok() false): skipped (default), or passed to the
    // callback (its root() is null).
    bool visit_unparsable = false;
    // Name XmlDocument::parse gets for a listed file (its file() and diagnostics): the path (default) or the entry name.
    bool name_docs_by_entry = false;
};

enum class ListStatus : uint8_t {
    Ok = 0,
    Missing = 1,     // the list file itself could not be read
    Unparsable = 2,  // the list file has no root element
};

// The entries of an already parsed list document, in document order (none when !list.ok()).
std::vector<ListedXmlEntry> listed_xml_entries(const XmlDocument& list, const ListedXmlOptions& options = {});

// Reads and parses `list_path` through `provider`; the entries in document order. `status` (if given) says why a
// list yields nothing.
std::vector<ListedXmlEntry> read_listed_xml_entries(const FileProvider& provider, std::string_view list_path,
                                                    const ListedXmlOptions& options = {}, ListStatus* status = nullptr);

// Raw bytes of each listed file, in list order. `bytes` is null for a missing file (visit_missing only).
using ListedFileFn = std::function<void(const ListedXmlEntry& entry, const std::vector<uint8_t>* bytes)>;
ListStatus for_each_listed_file(const FileProvider& provider, std::string_view list_path, const ListedFileFn& fn,
                                const ListedXmlOptions& options = {});

// One parsed listed file. `doc` is null for a missing file (visit_missing only); the callback may move it out to keep
// the document (and views into it) alive.
struct ListedXmlFile {
    const ListedXmlEntry& entry;
    std::unique_ptr<XmlDocument> doc;
};
using ListedXmlFn = std::function<void(ListedXmlFile& file)>;
ListStatus for_each_listed_xml(const FileProvider& provider, std::string_view list_path, const ListedXmlFn& fn,
                               const ListedXmlOptions& options = {});

}  // namespace alamo::data
