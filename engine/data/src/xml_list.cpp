#include "alamo/data/xml_list.hpp"

namespace alamo::data {

std::vector<ListedXmlEntry> listed_xml_entries(const XmlDocument& list, const ListedXmlOptions& options) {
    std::vector<ListedXmlEntry> out;
    if (!list.ok()) return out;
    for (XmlNode e : list.root().children(options.entry_tag)) {
        const std::string_view name = e.text_trimmed();
        if (name.empty() && !options.keep_empty_entries) continue;
        ListedXmlEntry en;
        en.name = std::string(name);
        en.path = std::string(kListedXmlDir);
        en.path += name;
        en.line = e.line();
        out.push_back(std::move(en));
    }
    return out;
}

std::vector<ListedXmlEntry> read_listed_xml_entries(const FileProvider& provider, std::string_view list_path,
                                                    const ListedXmlOptions& options, ListStatus* status) {
    std::vector<uint8_t> bytes;
    if (!provider.read(list_path, bytes)) {
        if (status) *status = ListStatus::Missing;
        return {};
    }
    const XmlDocument list = XmlDocument::parse(bytes, std::string(list_path));
    if (status) *status = list.ok() ? ListStatus::Ok : ListStatus::Unparsable;
    return listed_xml_entries(list, options);
}

ListStatus for_each_listed_file(const FileProvider& provider, std::string_view list_path, const ListedFileFn& fn,
                                const ListedXmlOptions& options) {
    ListStatus status = ListStatus::Ok;
    const std::vector<ListedXmlEntry> entries = read_listed_xml_entries(provider, list_path, options, &status);
    std::vector<uint8_t> bytes;
    for (const ListedXmlEntry& en : entries) {
        bytes.clear();
        if (!provider.read(en.path, bytes)) {
            if (options.visit_missing) fn(en, nullptr);
            continue;
        }
        fn(en, &bytes);
    }
    return status;
}

ListStatus for_each_listed_xml(const FileProvider& provider, std::string_view list_path, const ListedXmlFn& fn,
                               const ListedXmlOptions& options) {
    return for_each_listed_file(
        provider, list_path,
        [&](const ListedXmlEntry& en, const std::vector<uint8_t>* bytes) {
            ListedXmlFile f{en, nullptr};
            if (bytes) {
                f.doc = std::make_unique<XmlDocument>(
                    XmlDocument::parse(*bytes, options.name_docs_by_entry ? en.name : en.path));
                if (!f.doc->ok() && !options.visit_unparsable) return;
            }
            fn(f);
        },
        options);
}

}  // namespace alamo::data
