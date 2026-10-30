#include "alamo/commands/command.hpp"

#include <stdexcept>

#include "wire.hpp"

namespace alamo::commands {
namespace {

using wire::put;
using wire::Reader;

bool fits(uint32_t v, unsigned bits) { return bits >= 32 || v < (1u << bits); }

bool field_present(const EventDef& d, const std::vector<FieldValue>& v, size_t i) {
    const FieldDef& f = d.fields[i];
    return f.cond < 0 || v[static_cast<size_t>(f.cond)].u == f.cond_value;
}

}  // namespace

EventData::EventData(EventType t) : type_(t) {
    const EventDef* d = event_def(t);
    if (!d || !d->exists) throw std::invalid_argument("EventData: dead or unknown event type");
    values_.resize(d->fields.size());
    for (size_t i = 0; i < d->fields.size(); ++i)
        if (d->fields[i].kind == FieldKind::Id) values_[i].u = kNoObject;
}

size_t EventData::index(std::string_view field) const {
    const auto& fs = def().fields;
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].name == field) return i;
    throw std::out_of_range("EventData: no field '" + std::string(field) + "' in " + std::string(def().name));
}

float EventData::f(std::string_view field) const {
    const size_t i = index(field);
    return dequantize(def().fields[i].arg, values_[i].u);
}

EventData& EventData::set(std::string_view field, uint32_t v) {
    values_[index(field)].u = v;
    return *this;
}
EventData& EventData::set_f(std::string_view field, float v) {
    const size_t i = index(field);
    if (def().fields[i].kind != FieldKind::Quant) throw std::invalid_argument("EventData::set_f: not a float field");
    values_[i].u = quantize(def().fields[i].arg, v);
    return *this;
}
EventData& EventData::set_ids(std::string_view field, std::vector<uint32_t> v) {
    const size_t i = index(field);
    const FieldDef& fd = def().fields[i];
    if (fd.kind == FieldKind::CountedIds) values_[fd.arg].u = static_cast<uint32_t>(v.size());
    values_[i].ids = std::move(v);
    return *this;
}
EventData& EventData::set_str(std::string_view field, std::string v) {
    values_[index(field)].bytes = std::move(v);
    return *this;
}
EventData& EventData::set_wstr(std::string_view field, std::u16string v) {
    values_[index(field)].wide = std::move(v);
    return *this;
}

EventData& EventData::set_bytes(std::string_view field, std::span<const uint8_t> v) {
    std::string& s = values_[index(field)].bytes;
    s.resize(v.size());
    for (size_t i = 0; i < v.size(); ++i) s[i] = static_cast<char>(v[i]);
    return *this;
}

std::span<const uint8_t> EventData::bytes_view(std::string_view field) const {
    const std::string& s = values_[index(field)].bytes;
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

bool EventData::present(size_t i) const { return field_present(def(), values_, i); }

bool EventData::valid() const {
    const EventDef& d = def();
    for (size_t i = 0; i < d.fields.size(); ++i) {
        const FieldDef& f = d.fields[i];
        const FieldValue& v = values_[i];
        if (!present(i)) {
            // An absent field is not sent: it must hold its default, or the issuer would execute a
            // value the receivers never see.
            FieldValue def;
            if (f.kind == FieldKind::Id) def.u = kNoObject;
            if (!(v == def)) return false;
            continue;
        }
        switch (f.kind) {
            case FieldKind::Bits:
                if (!fits(v.u, f.arg)) return false;
                break;
            case FieldKind::Quant:
                if (!fits(v.u, quantizer(f.arg).bits)) return false;
                break;
            case FieldKind::List:
                if (!fits(static_cast<uint32_t>(v.ids.size()), f.arg) || v.ids.size() > kMaxPayloadBytes / 8) return false;
                break;
            case FieldKind::CountedIds:
                if (v.ids.size() != values_[f.arg].u) return false;
                break;
            case FieldKind::Str:
            case FieldKind::Opaque:
                if (v.bytes.size() > 0xFFFF) return false;
                break;
            case FieldKind::WStr:
                if (v.wide.size() > 0xFFFF) return false;
                break;
            case FieldKind::Id:
            case FieldKind::Player: break;
        }
    }
    return true;
}

std::string_view decode_error_name(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::UnknownType: return "unknown type";
        case DecodeError::BadField: return "bad field";
        case DecodeError::TrailingBytes: return "trailing bytes";
        case DecodeError::TooLarge: return "too large";
    }
    return "?";
}

void encode_payload(const EventData& p, std::vector<uint8_t>& out) {
    const EventDef& d = p.def();
    for (size_t i = 0; i < d.fields.size(); ++i) {
        if (!p.present(i)) continue;
        const FieldValue& v = p.value(i);
        switch (d.fields[i].kind) {
            case FieldKind::Bits:
            case FieldKind::Id:
            case FieldKind::Player:
            case FieldKind::Quant: put(out, v.u); break;
            case FieldKind::List:
            case FieldKind::CountedIds:
                put(out, static_cast<uint32_t>(v.ids.size()));
                for (uint32_t id : v.ids) put(out, id);
                break;
            case FieldKind::Str:
            case FieldKind::Opaque:
                put(out, static_cast<uint32_t>(v.bytes.size()));
                for (char c : v.bytes) put(out, static_cast<uint8_t>(c));
                break;
            case FieldKind::WStr:
                put(out, static_cast<uint32_t>(v.wide.size()));
                for (char16_t c : v.wide) put(out, static_cast<uint16_t>(c));
                break;
        }
    }
}

DecodeError decode_payload(uint16_t type, std::span<const uint8_t> bytes, EventData& out) {
    if (bytes.size() > kMaxPayloadBytes) return DecodeError::TooLarge;
    if (type > kMaxEventType || !event_exists(static_cast<uint8_t>(type))) return DecodeError::UnknownType;
    EventData e(static_cast<EventType>(type));
    const EventDef& d = e.def();
    Reader r(bytes);
    for (size_t i = 0; i < d.fields.size() && r.ok(); ++i) {
        if (!e.present(i)) continue;
        FieldValue& v = e.value(i);
        switch (d.fields[i].kind) {
            case FieldKind::Bits:
            case FieldKind::Id:
            case FieldKind::Player:
            case FieldKind::Quant: v.u = r.get<uint32_t>(); break;
            case FieldKind::List:
            case FieldKind::CountedIds: {
                const uint32_t n = r.get<uint32_t>();
                if (!r.ok() || r.remaining() / 4 < n) return DecodeError::Truncated;
                v.ids.resize(n);
                for (auto& id : v.ids) id = r.get<uint32_t>();
                break;
            }
            case FieldKind::Str:
            case FieldKind::Opaque: {
                const uint32_t n = r.get<uint32_t>();
                if (!r.ok() || r.remaining() < n) return DecodeError::Truncated;
                v.bytes.resize(n);
                for (auto& c : v.bytes) c = static_cast<char>(r.get<uint8_t>());
                break;
            }
            case FieldKind::WStr: {
                const uint32_t n = r.get<uint32_t>();
                if (!r.ok() || r.remaining() / 2 < n) return DecodeError::Truncated;
                v.wide.resize(n);
                for (auto& c : v.wide) c = static_cast<char16_t>(r.get<uint16_t>());
                break;
            }
        }
    }
    if (!r.ok()) return DecodeError::Truncated;
    if (r.remaining() != 0) return DecodeError::TrailingBytes;
    if (!e.valid()) return DecodeError::BadField;
    out = std::move(e);
    return DecodeError::None;
}

void encode_command(const Command& c, std::vector<uint8_t>& out) {
    put(out, static_cast<uint16_t>(c.payload.type_id()));
    put(out, c.header.player);
    // [port: Gabriel] bit 7 = queued (0 for every command recorded before queueable actions: same bytes).
    put(out, static_cast<uint8_t>(static_cast<uint8_t>(c.header.source) | (c.payload.queued() ? kCommandQueuedBit : 0u)));
    put(out, c.header.sequence);
    put(out, c.header.issue_tick);
    put(out, c.header.scheduled_tick);
    const size_t len_at = out.size();
    put(out, uint32_t{0});
    encode_payload(c.payload, out);
    const auto len = static_cast<uint32_t>(out.size() - len_at - 4);
    for (size_t i = 0; i < 4; ++i) out[len_at + i] = static_cast<uint8_t>((len >> (8 * i)) & 0xFFu);
}

std::vector<uint8_t> encode_command(const Command& c) {
    std::vector<uint8_t> out;
    encode_command(c, out);
    return out;
}

DecodeError decode_command(std::span<const uint8_t> in, size_t& pos, Command& out) {
    Reader r(in, pos);
    const uint16_t type = r.get<uint16_t>();
    Command c;
    c.header.player = r.get<uint8_t>();
    const uint8_t src_byte = r.get<uint8_t>();
    const uint8_t src = static_cast<uint8_t>(src_byte & ~kCommandQueuedBit);  // [port: Gabriel] bit 7 = queued
    c.header.sequence = r.get<uint32_t>();
    c.header.issue_tick = r.get<uint64_t>();
    c.header.scheduled_tick = r.get<uint64_t>();
    const uint32_t len = r.get<uint32_t>();
    if (!r.ok()) return DecodeError::Truncated;
    if (len > kMaxPayloadBytes) return DecodeError::TooLarge;
    if (r.remaining() < len) return DecodeError::Truncated;
    if (src >= kCommandSourceCount || c.header.player >= kMaxPlayerSlots) return DecodeError::BadField;
    c.header.source = static_cast<CommandSource>(src);
    const DecodeError e = decode_payload(type, in.subspan(r.pos(), len), c.payload);
    if (e != DecodeError::None) return e;
    c.payload.set_queued((src_byte & kCommandQueuedBit) != 0);
    pos = r.pos() + len;
    out = std::move(c);
    return DecodeError::None;
}

// ---- builders ----------------------------------------------------------------------------------------
namespace ev {
EventData move_to_position(float x, float y, float z, uint8_t flags) {
    EventData e(EventType::MoveToPosition);
    e.set_f("x", x).set_f("y", y).set_f("z", z).set("flags", flags);
    return e;
}
EventData move_to_object(ObjectId target, std::vector<ObjectId> movers) {
    EventData e(EventType::MoveToObject);
    e.set("target", target).set_ids("movers", std::move(movers));
    return e;
}
EventData attack(ObjectId target, std::vector<ObjectId> attackers) {
    EventData e(EventType::Attack);
    e.set("target", target).set_ids("attackers", std::move(attackers));
    return e;
}
EventData escort(ObjectId target, uint32_t option) {
    EventData e(EventType::Escort);
    e.set("target", target).set("option", option);
    return e;
}
EventData stop() { return EventData(EventType::StopMovement); }
EventData select(std::vector<ObjectId> ids) {
    EventData e(EventType::Select);
    e.set_ids("ids", std::move(ids));
    return e;
}
EventData select_all() { return EventData(EventType::SelectAll); }
EventData control_group(uint8_t group, bool assign, bool add) {
    EventData e(EventType::ControlGroup);
    e.set("group", group).set("assign", assign ? 1u : 0u).set("add", (assign && add) ? 1u : 0u);
    return e;
}
EventData production(uint16_t type_index, ObjectId location, bool cancel, uint8_t queue, uint8_t index) {
    EventData e(EventType::Production);
    e.set("type_index", type_index).set("location", location).set("cancel", cancel ? 1u : 0u).set("queue", queue).set("index", index);
    return e;
}
EventData tactical_build(ObjectId pad, uint16_t type_index, uint8_t player) {
    EventData e(EventType::TacticalBuild);
    e.set("pad", pad).set("type_index", type_index).set("player", player);
    return e;
}
EventData ability(std::vector<ObjectId> users, uint8_t ability_type, ObjectId target) {
    EventData e(EventType::TacticalSpecialAbility);
    e.set_ids("users", std::move(users)).set("ability", ability_type).set("target", target);
    return e;
}
EventData move_object_to_object(ObjectId object, ObjectId target) {
    EventData e(EventType::MoveObjectToObject);
    e.set("object", object).set("target", target);
    return e;
}
EventData retreat(bool request) {
    EventData e(EventType::Retreat);
    e.set("request", request ? 1u : 0u);
    return e;
}
EventData port_extension(uint16_t subtype, std::span<const uint8_t> data) {
    EventData e(EventType::PortExtension);
    e.set("subtype", subtype).set_bytes("data", data);
    return e;
}
}  // namespace ev

}  // namespace alamo::commands
