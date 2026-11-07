// Tolerant XML DOM for the game's data files.
//
// - One flat node array + one flat attribute array per document; every string is a
//   std::string_view into the document's own buffer (entities are decoded in place).
// - Accepts what the shipped files contain: UTF-8 BOM, <?xml ?> (also a malformed "<?xml ...>"
//   without the closing '?'), comments, <!DOCTYPE>, CDATA, CRLF, Latin-1 bytes (passed through
//   untouched), unquoted attribute values, raw '&' / '<' that do not start markup.
// - Recovery rules (each one adds a warning with file + line, parsing continues):
//     * a close tag whose name differs from the open element closes the open element
//       (the EaW AI/GUI files contain e.g. <Top_Color>..</Bottom_Color>);
//     * a stray close tag with nothing open is ignored;
//     * elements still open at end of file are closed;
//     * an unterminated comment / CDATA / tag ends at end of file.
//   Only "no root element" (empty file, UTF-16 text) is fatal: ok() == false.
// - Lookups by element / attribute name are ASCII case-insensitive, like the engine's.
// - Numeric character references (&#N; / &#xH;) are written as UTF-8.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::data {

struct XmlDiagnostic {
    std::string file;
    uint32_t line = 0;  // 1-based; 0 = whole file
    bool fatal = false;
    std::string message;
    std::string to_string() const;  // "file(line): message"
};

struct XmlAttribute {
    std::string_view name;
    std::string_view value;
};

class XmlDocument;
struct XmlChildRange;

// Lightweight handle (document pointer + index). Default-constructed / failed lookups are
// "null": operator bool() is false and every accessor returns an empty value.
class XmlNode {
public:
    XmlNode() = default;
    XmlNode(const XmlDocument* doc, uint32_t index) : doc_(doc), index_(index) {}

    explicit operator bool() const { return doc_ != nullptr; }
    const XmlDocument* document() const { return doc_; }
    uint32_t index() const { return index_; }

    std::string_view name() const;
    std::string_view text() const;          // decoded, untrimmed character data
    std::string_view text_trimmed() const;  // text() without surrounding whitespace
    uint32_t line() const;                  // 1-based line of the start tag

    XmlNode parent() const;
    XmlNode first_child() const;
    XmlNode next_sibling() const;
    XmlNode child(std::string_view name) const;               // first child with this name
    XmlNode next_sibling(std::string_view name) const;        // next sibling with this name
    std::string_view child_text(std::string_view name) const;  // trimmed text of child(name)
    size_t child_count() const;

    const XmlAttribute* attributes_begin() const;
    const XmlAttribute* attributes_end() const;
    size_t attribute_count() const;
    // Value of the first attribute with this name (case-insensitive), or `fallback`.
    std::string_view attribute(std::string_view name, std::string_view fallback = {}) const;

    // Range over children, optionally filtered by name: for (XmlNode c : n.children("File")).
    XmlChildRange children(std::string_view name = {}) const;

    friend bool operator==(const XmlNode& a, const XmlNode& b) { return a.doc_ == b.doc_ && a.index_ == b.index_; }

private:
    const XmlDocument* doc_ = nullptr;
    uint32_t index_ = 0;
};

class XmlChildIterator {
public:
    XmlChildIterator() = default;
    XmlChildIterator(XmlNode node, std::string_view filter) : node_(node), filter_(filter) {}
    XmlNode operator*() const { return node_; }
    XmlChildIterator& operator++() {
        node_ = filter_.empty() ? node_.next_sibling() : node_.next_sibling(filter_);
        return *this;
    }
    bool operator==(const XmlChildIterator& o) const { return node_ == o.node_; }
    bool operator!=(const XmlChildIterator& o) const { return !(node_ == o.node_); }

private:
    XmlNode node_;
    std::string_view filter_;
};

struct XmlChildRange {
    XmlChildIterator b, e;
    XmlChildIterator begin() const { return b; }
    XmlChildIterator end() const { return e; }
};

class XmlDocument {
public:
    XmlDocument() = default;
    XmlDocument(const XmlDocument&) = delete;
    XmlDocument& operator=(const XmlDocument&) = delete;
    XmlDocument(XmlDocument&&) = default;
    XmlDocument& operator=(XmlDocument&&) = default;

    // Parses a copy of `bytes`. `file` is only used for diagnostics. Never throws on bad input.
    static XmlDocument parse(std::string_view bytes, std::string file = {});
    static XmlDocument parse(const std::vector<uint8_t>& bytes, std::string file = {});

    bool ok() const { return ok_; }
    const std::string& file() const { return file_; }
    // Non-fatal recoveries and (if !ok()) the fatal error, in source order.
    const std::vector<XmlDiagnostic>& diagnostics() const { return diags_; }

    XmlNode root() const;  // first top-level element (null if !ok())
    size_t node_count() const { return nodes_.empty() ? 0 : nodes_.size() - 1; }  // excludes the document node
    uint32_t line_of_offset(uint32_t offset) const;

private:
    friend class XmlNode;
    friend class XmlParser;
    struct Node {
        std::string_view name;
        std::string_view text;
        uint32_t parent = 0;
        uint32_t first_child = 0;  // 0 = none (index 0 is the document node, never a child)
        uint32_t last_child = 0;
        uint32_t next_sibling = 0;
        uint32_t attr_begin = 0;
        uint32_t attr_count = 0;
        uint32_t offset = 0;      // byte offset of '<'
        bool text_is_space = true;
    };

    std::string file_;
    std::vector<char> buffer_;  // owned bytes; vector move keeps data() stable
    std::vector<Node> nodes_;   // [0] = document node
    std::vector<XmlAttribute> attrs_;
    std::vector<uint32_t> line_starts_;
    std::deque<std::string> extra_text_;  // concatenated text for elements with several text runs
    std::vector<XmlDiagnostic> diags_;
    bool ok_ = false;
};

}  // namespace alamo::data
