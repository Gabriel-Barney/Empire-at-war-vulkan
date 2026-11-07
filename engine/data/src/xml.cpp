#include "alamo/data/xml.hpp"

#include <algorithm>
#include <cstring>

#include "alamo/core/hash.hpp"

namespace alamo::data {

namespace {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

// Unlike XML 1.0, a digit may start an element name: the original loads `<80s_Visor_Man>` / `<181st>` root elements
// and the types inside them (modc t14 probe, docs/re/MODC_LOADER.md section 3). So "<5" is markup, "< 5" stays text.
inline bool is_name_start(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == ':' ||
           u >= 0x80;
}

inline bool is_name_char(char c) {
    return !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '"' && c != '\'';
}

std::string_view trim_view(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool all_space(std::string_view s) {
    for (char c : s)
        if (!is_space(c)) return false;
    return true;
}

// Appends the UTF-8 encoding of cp at w; returns bytes written (0 if cp is invalid).
size_t encode_utf8(uint32_t cp, char* w) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    if (cp < 0x80) {
        w[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        w[0] = static_cast<char>(0xC0 | (cp >> 6));
        w[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        w[0] = static_cast<char>(0xE0 | (cp >> 12));
        w[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        w[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    w[0] = static_cast<char>(0xF0 | (cp >> 18));
    w[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    w[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    w[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// Decodes entities in [b, e) in place. Unknown / malformed references are kept literally.
// The decoded form is never longer than the source. Returns the new end.
char* decode_entities(char* b, char* e) {
    char* r = static_cast<char*>(std::memchr(b, '&', static_cast<size_t>(e - b)));
    if (!r) return e;
    char* w = r;
    while (r < e) {
        if (*r != '&') {
            *w++ = *r++;
            continue;
        }
        char* semi = r + 1;
        const char* limit = (e - r > 12) ? r + 12 : e;  // longest valid reference: &#x10FFFF;
        while (semi < limit && *semi != ';' && *semi != '&' && *semi != '<') ++semi;
        if (semi >= limit || *semi != ';') {
            *w++ = *r++;
            continue;
        }
        const std::string_view ent(r + 1, static_cast<size_t>(semi - r - 1));
        char rep = 0;
        if (ent == "amp") rep = '&';
        else if (ent == "lt") rep = '<';
        else if (ent == "gt") rep = '>';
        else if (ent == "quot") rep = '"';
        else if (ent == "apos") rep = '\'';
        if (rep) {
            *w++ = rep;
            r = semi + 1;
            continue;
        }
        if (ent.size() >= 2 && ent[0] == '#') {
            uint32_t cp = 0;
            bool okd = true;
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const size_t start = hex ? 2 : 1;
            if (start >= ent.size()) okd = false;
            for (size_t i = start; okd && i < ent.size(); ++i) {
                const char c = ent[i];
                uint32_t d = 0;
                if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
                else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
                else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
                else okd = false;
                cp = cp * (hex ? 16u : 10u) + d;
                if (cp > 0x10FFFF) okd = false;
            }
            char tmp[4];
            const size_t n = okd ? encode_utf8(cp, tmp) : 0;
            if (n) {
                for (size_t i = 0; i < n; ++i) *w++ = tmp[i];
                r = semi + 1;
                continue;
            }
        }
        *w++ = *r++;
    }
    return w;
}

}  // namespace

std::string XmlDiagnostic::to_string() const {
    std::string s = file.empty() ? std::string("<memory>") : file;
    if (line) s += "(" + std::to_string(line) + ")";
    s += fatal ? ": error: " : ": warning: ";
    s += message;
    return s;
}

// ------------------------------------------------------------------------------------------
// Parser

class XmlParser {
public:
    explicit XmlParser(XmlDocument& d) : doc_(d) {}

    void run() {
        auto& buf = doc_.buffer_;
        begin_ = buf.data();
        end_ = begin_ + buf.size();
        build_line_table();
        p_ = begin_;
        const size_t n = buf.size();
        if (n >= 3 && static_cast<unsigned char>(p_[0]) == 0xEF && static_cast<unsigned char>(p_[1]) == 0xBB &&
            static_cast<unsigned char>(p_[2]) == 0xBF)
            p_ += 3;
        else if (n >= 2 && ((static_cast<unsigned char>(p_[0]) == 0xFF && static_cast<unsigned char>(p_[1]) == 0xFE) ||
                            (static_cast<unsigned char>(p_[0]) == 0xFE && static_cast<unsigned char>(p_[1]) == 0xFF))) {
            fatal(0, "UTF-16 text is not supported");
            return;
        }
        doc_.nodes_.reserve(buf.size() / 48 + 8);
        doc_.nodes_.push_back({});  // document node
        stack_.push_back(0);

        while (p_ < end_) {
            if (*p_ != '<') {
                parse_text();
                continue;
            }
            const size_t left = static_cast<size_t>(end_ - p_);
            if (left >= 4 && std::memcmp(p_, "<!--", 4) == 0) {
                skip_comment();
            } else if (left >= 9 && std::memcmp(p_, "<![CDATA[", 9) == 0) {
                parse_cdata();
            } else if (left >= 2 && (p_[1] == '!' || p_[1] == '?')) {
                skip_declaration();
            } else if (left >= 2 && p_[1] == '/') {
                parse_close_tag();
            } else if (left >= 2 && is_name_start(p_[1])) {
                parse_open_tag();
            } else {
                warn(p_, "'<' does not start markup; kept as text");
                parse_text(/*force_first=*/true);
            }
        }
        while (stack_.size() > 1) {
            const uint32_t open = stack_.back();
            warn(begin_ + doc_.nodes_[open].offset,
                 "element <" + std::string(doc_.nodes_[open].name) + "> is not closed before end of file");
            stack_.pop_back();
        }
        if (doc_.nodes_[0].first_child == 0) {
            fatal(0, "no root element");
            return;
        }
        uint32_t roots = 0;
        for (uint32_t c = doc_.nodes_[0].first_child; c; c = doc_.nodes_[c].next_sibling) ++roots;
        if (roots > 1) warn(begin_ + doc_.nodes_[doc_.nodes_[0].last_child].offset, "more than one top-level element");
        doc_.ok_ = true;
    }

private:
    void build_line_table() {
        auto& ls = doc_.line_starts_;
        ls.clear();
        ls.push_back(0);
        const char* q = begin_;
        while (q < end_) {
            const void* nl = std::memchr(q, '\n', static_cast<size_t>(end_ - q));
            if (!nl) break;
            q = static_cast<const char*>(nl) + 1;
            ls.push_back(static_cast<uint32_t>(q - begin_));
        }
    }

    uint32_t off(const char* q) const { return static_cast<uint32_t>(q - begin_); }

    void warn(const char* at, std::string msg) {
        doc_.diags_.push_back({doc_.file_, doc_.line_of_offset(off(at)), false, std::move(msg)});
    }
    void fatal(uint32_t line, std::string msg) {
        doc_.ok_ = false;
        doc_.diags_.push_back({doc_.file_, line, true, std::move(msg)});
    }

    const char* find(const char* from, const char* pat, size_t len) const {
        const char* q = from;
        while (q + len <= end_) {
            const void* hit = std::memchr(q, pat[0], static_cast<size_t>(end_ - q));
            if (!hit) return nullptr;
            q = static_cast<const char*>(hit);
            if (q + len <= end_ && std::memcmp(q, pat, len) == 0) return q;
            ++q;
        }
        return nullptr;
    }

    void add_text(char* b, char* e) {
        Node& n = doc_.nodes_[stack_.back()];
        if (b == e) return;
        const std::string_view seg(b, static_cast<size_t>(e - b));
        const bool seg_space = all_space(seg);
        if (stack_.back() == 0) {
            if (!seg_space) warn(b, "text outside the root element ignored");
            return;
        }
        if (n.text.empty()) {
            n.text = seg;
            n.text_is_space = seg_space;
        } else if (seg_space) {
            // whitespace between children / around comments: irrelevant once real text exists
            if (n.text_is_space) n.text = seg;
        } else if (n.text_is_space) {
            n.text = seg;
            n.text_is_space = false;
        } else {
            std::string joined(n.text);
            joined.append(seg);
            doc_.extra_text_.push_back(std::move(joined));
            n.text = doc_.extra_text_.back();
        }
    }

    using Node = XmlDocument::Node;

    void parse_text(bool force_first = false) {
        char* b = p_;
        const char* from = force_first ? p_ + 1 : p_;
        const void* lt = std::memchr(from, '<', static_cast<size_t>(end_ - from));
        char* e = lt ? const_cast<char*>(static_cast<const char*>(lt)) : end_;
        // A '<' that cannot start markup ("a < b") stays part of the text.
        while (e < end_ && e + 1 < end_ && !is_name_start(e[1]) && e[1] != '/' && e[1] != '!' && e[1] != '?') {
            warn(e, "'<' does not start markup; kept as text");
            const void* nx = std::memchr(e + 1, '<', static_cast<size_t>(end_ - e - 1));
            e = nx ? const_cast<char*>(static_cast<const char*>(nx)) : end_;
        }
        if (e + 1 == end_ && e < end_ && *e == '<') e = end_;  // lone '<' at EOF
        p_ = e;
        add_text(b, decode_entities(b, e));
    }

    void skip_comment() {
        const char* close = find(p_ + 4, "-->", 3);
        if (!close) {
            warn(p_, "unterminated comment runs to end of file");
            p_ = end_;
            return;
        }
        p_ = const_cast<char*>(close) + 3;
    }

    void parse_cdata() {
        char* b = p_ + 9;
        const char* close = find(b, "]]>", 3);
        char* e = close ? const_cast<char*>(close) : end_;
        if (!close) warn(p_, "unterminated CDATA section runs to end of file");
        p_ = close ? e + 3 : end_;
        add_text(b, e);
    }

    // <?xml ... ?>, <?pi ?>, <!DOCTYPE ...>. Ends at the first '>' (accepts "<?xml version="1.0">").
    void skip_declaration() {
        const void* gt = std::memchr(p_, '>', static_cast<size_t>(end_ - p_));
        if (!gt) {
            warn(p_, "unterminated declaration runs to end of file");
            p_ = end_;
            return;
        }
        const char* g = static_cast<const char*>(gt);
        if (p_[1] == '?' && g[-1] != '?') warn(p_, "processing instruction not closed with '?>'");
        p_ = const_cast<char*>(g) + 1;
    }

    void parse_close_tag() {
        char* start = p_;
        char* q = p_ + 2;
        while (q < end_ && is_space(*q)) ++q;
        char* nb = q;
        while (q < end_ && is_name_char(*q)) ++q;
        const std::string_view name(nb, static_cast<size_t>(q - nb));
        const void* gt = std::memchr(q, '>', static_cast<size_t>(end_ - q));
        if (!gt) {
            warn(start, "unterminated close tag runs to end of file");
            p_ = end_;
        } else {
            p_ = const_cast<char*>(static_cast<const char*>(gt)) + 1;
        }
        if (stack_.size() <= 1) {
            warn(start, "close tag </" + std::string(name) + "> without an open element ignored");
            return;
        }
        const Node& cur = doc_.nodes_[stack_.back()];
        if (!iequals(cur.name, name))
            warn(start, "close tag </" + std::string(name) + "> closes open element <" + std::string(cur.name) + ">");
        stack_.pop_back();
    }

    void parse_open_tag() {
        char* start = p_;
        char* q = p_ + 1;
        char* nb = q;
        while (q < end_ && is_name_char(*q)) ++q;

        const uint32_t idx = static_cast<uint32_t>(doc_.nodes_.size());
        const uint32_t parent = stack_.back();
        {
            Node n;
            n.name = std::string_view(nb, static_cast<size_t>(q - nb));
            n.parent = parent;
            n.offset = off(start);
            n.attr_begin = static_cast<uint32_t>(doc_.attrs_.size());
            doc_.nodes_.push_back(n);
            Node& par = doc_.nodes_[parent];
            if (par.last_child) doc_.nodes_[par.last_child].next_sibling = idx;
            else par.first_child = idx;
            par.last_child = idx;
        }

        bool self_close = false;
        for (;;) {
            while (q < end_ && is_space(*q)) ++q;
            if (q >= end_) {
                warn(start, "unterminated start tag runs to end of file");
                p_ = end_;
                return;
            }
            if (*q == '>') {
                ++q;
                break;
            }
            if (*q == '/') {
                ++q;
                if (q < end_ && *q == '>') {
                    ++q;
                    self_close = true;
                    break;
                }
                continue;  // stray '/'
            }
            if (*q == '<') {  // "<a <b>": tag never closed
                warn(start, "start tag not closed with '>'");
                break;
            }
            char* an = q;
            while (q < end_ && is_name_char(*q)) ++q;
            if (q == an) {  // junk such as a stray quote or '='
                ++q;
                continue;
            }
            const std::string_view aname(an, static_cast<size_t>(q - an));
            while (q < end_ && is_space(*q)) ++q;
            std::string_view aval;
            if (q < end_ && *q == '=') {
                ++q;
                while (q < end_ && is_space(*q)) ++q;
                if (q < end_ && (*q == '"' || *q == '\'')) {
                    const char quote = *q++;
                    char* vb = q;
                    const void* qe = std::memchr(q, quote, static_cast<size_t>(end_ - q));
                    char* ve = qe ? const_cast<char*>(static_cast<const char*>(qe)) : end_;
                    if (!qe) warn(start, "unterminated attribute value");
                    q = qe ? ve + 1 : end_;
                    aval = std::string_view(vb, static_cast<size_t>(decode_entities(vb, ve) - vb));
                } else {
                    char* vb = q;
                    while (q < end_ && !is_space(*q) && *q != '>') ++q;
                    if (q > vb && q[-1] == '/' && q < end_ && *q == '>') --q;  // a=b/>
                    warn(start, "unquoted attribute value");
                    aval = std::string_view(vb, static_cast<size_t>(decode_entities(vb, q) - vb));
                }
            }
            doc_.attrs_.push_back({aname, aval});
            ++doc_.nodes_[idx].attr_count;
        }
        p_ = q;
        if (!self_close) stack_.push_back(idx);
    }

    XmlDocument& doc_;
    char* begin_ = nullptr;
    char* end_ = nullptr;
    char* p_ = nullptr;
    std::vector<uint32_t> stack_;
};

// ------------------------------------------------------------------------------------------
// XmlDocument

XmlDocument XmlDocument::parse(std::string_view bytes, std::string file) {
    XmlDocument d;
    d.file_ = std::move(file);
    d.buffer_.assign(bytes.begin(), bytes.end());
    XmlParser(d).run();
    return d;
}

XmlDocument XmlDocument::parse(const std::vector<uint8_t>& bytes, std::string file) {
    return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::move(file));
}

XmlNode XmlDocument::root() const {
    if (!ok_ || nodes_.empty()) return {};
    return nodes_[0].first_child ? XmlNode(this, nodes_[0].first_child) : XmlNode();
}

uint32_t XmlDocument::line_of_offset(uint32_t offset) const {
    if (line_starts_.empty()) return 1;
    const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    return static_cast<uint32_t>(it - line_starts_.begin());
}

// ------------------------------------------------------------------------------------------
// XmlNode

std::string_view XmlNode::name() const { return doc_ ? doc_->nodes_[index_].name : std::string_view(); }
std::string_view XmlNode::text() const { return doc_ ? doc_->nodes_[index_].text : std::string_view(); }
std::string_view XmlNode::text_trimmed() const { return trim_view(text()); }
uint32_t XmlNode::line() const { return doc_ ? doc_->line_of_offset(doc_->nodes_[index_].offset) : 0; }

XmlNode XmlNode::parent() const {
    if (!doc_ || index_ == 0) return {};
    const uint32_t p = doc_->nodes_[index_].parent;
    return p ? XmlNode(doc_, p) : XmlNode();
}

XmlNode XmlNode::first_child() const {
    if (!doc_) return {};
    const uint32_t c = doc_->nodes_[index_].first_child;
    return c ? XmlNode(doc_, c) : XmlNode();
}

XmlNode XmlNode::next_sibling() const {
    if (!doc_) return {};
    const uint32_t s = doc_->nodes_[index_].next_sibling;
    return s ? XmlNode(doc_, s) : XmlNode();
}

XmlNode XmlNode::child(std::string_view child_name) const {
    if (!doc_) return {};
    for (uint32_t c = doc_->nodes_[index_].first_child; c; c = doc_->nodes_[c].next_sibling)
        if (iequals(doc_->nodes_[c].name, child_name)) return XmlNode(doc_, c);
    return {};
}

XmlNode XmlNode::next_sibling(std::string_view sibling_name) const {
    if (!doc_) return {};
    for (uint32_t s = doc_->nodes_[index_].next_sibling; s; s = doc_->nodes_[s].next_sibling)
        if (iequals(doc_->nodes_[s].name, sibling_name)) return XmlNode(doc_, s);
    return {};
}

std::string_view XmlNode::child_text(std::string_view child_name) const { return child(child_name).text_trimmed(); }

size_t XmlNode::child_count() const {
    if (!doc_) return 0;
    size_t n = 0;
    for (uint32_t c = doc_->nodes_[index_].first_child; c; c = doc_->nodes_[c].next_sibling) ++n;
    return n;
}

const XmlAttribute* XmlNode::attributes_begin() const {
    return doc_ ? doc_->attrs_.data() + doc_->nodes_[index_].attr_begin : nullptr;
}
const XmlAttribute* XmlNode::attributes_end() const {
    return doc_ ? attributes_begin() + doc_->nodes_[index_].attr_count : nullptr;
}
size_t XmlNode::attribute_count() const { return doc_ ? doc_->nodes_[index_].attr_count : 0; }

std::string_view XmlNode::attribute(std::string_view attr_name, std::string_view fallback) const {
    for (const XmlAttribute* a = attributes_begin(); a != attributes_end(); ++a)
        if (iequals(a->name, attr_name)) return a->value;
    return fallback;
}

XmlChildRange XmlNode::children(std::string_view filter) const {
    XmlNode first = first_child();
    if (first && !filter.empty() && !iequals(first.name(), filter)) first = first.next_sibling(filter);
    return {XmlChildIterator(first, filter), XmlChildIterator(XmlNode(), filter)};
}

}  // namespace alamo::data
