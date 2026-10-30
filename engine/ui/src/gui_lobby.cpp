#include "alamo/ui/gui_lobby.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace alamo::ui {

namespace {

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lo = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lo(a[i]) != lo(b[i])) return false;
    }
    return true;
}
bool istarts(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequal(s.substr(0, p.size()), p); }

std::u32string ascii32(std::string_view s) {
    std::u32string o;
    for (char c : s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    return o;
}
std::u32string num32(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    return ascii32(b);
}
std::string id_n(const char* fmt, int32_t n) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, n);
    return b;
}
// Trailing slot / row digit of a control id ("IDC_PLAYER_FACTION_3" -> 3), -1 if none.
int32_t trailing_index(std::string_view id) {
    size_t i = id.size();
    while (i > 0 && id[i - 1] >= '0' && id[i - 1] <= '9') --i;
    if (i == id.size()) return -1;
    int32_t v = 0;
    for (size_t k = i; k < id.size(); ++k) v = v * 10 + (id[k] - '0');
    return v;
}

LobbyEvent ev(LobbyEvent::Kind k) {
    LobbyEvent e;
    e.kind = k;
    return e;
}

constexpr const char* kAcceptTexture = "i_dialogue_check_on.tga";  // [port] accept mark art
const char* const kLobbyDialogs[] = {dlg::kMultiplayer, dlg::kLanLobby,      dlg::kLanStaging, dlg::kInternetStaging,
                                     dlg::kInternetHome, dlg::kInternetLobby, dlg::kDirectConnect, dlg::kPassword,
                                     dlg::kConnect,      dlg::kDisconnect,    dlg::kVote};

struct ColumnButton {
    const char* id;
    int32_t column;
};
constexpr ColumnButton kColumns[] = {{"IDC_INTERNET_SERVER_NAME_BUTTON", 0},
                                     {"IDC_INTERNET_GAME_TYPE_BUTTON", 1},
                                     {"IDC_INTERNET_PLAYERS_BUTTON", 2},
                                     {"IDC_INTERNET_MAP_NAME_BUTTON", 3},
                                     {"IDC_INTERNET_PING_BUTTON", 4}};

}  // namespace

const char* lobby_event_name(LobbyEvent::Kind k) {
    using K = LobbyEvent::Kind;
    switch (k) {
        case K::BackToMainMenu: return "BackToMainMenu";
        case K::ShowCredits: return "ShowCredits";
        case K::OpenInternet: return "OpenInternet";
        case K::OpenNetworkOptions: return "OpenNetworkOptions";
        case K::EnterLanLobby: return "EnterLanLobby";
        case K::LeaveLobby: return "LeaveLobby";
        case K::SetPlayerName: return "SetPlayerName";
        case K::SetPlayerColor: return "SetPlayerColor";
        case K::SelectGame: return "SelectGame";
        case K::HostGame: return "HostGame";
        case K::JoinGame: return "JoinGame";
        case K::SortGames: return "SortGames";
        case K::OpenGameFilter: return "OpenGameFilter";
        case K::OpenDirectConnect: return "OpenDirectConnect";
        case K::AddDirectAddress: return "AddDirectAddress";
        case K::ConnectDirect: return "ConnectDirect";
        case K::CancelConnect: return "CancelConnect";
        case K::Chat: return "Chat";
        case K::LeaveGame: return "LeaveGame";
        case K::SetSlotState: return "SetSlotState";
        case K::SetSlotFaction: return "SetSlotFaction";
        case K::SetSlotColor: return "SetSlotColor";
        case K::SetSlotTeam: return "SetSlotTeam";
        case K::SelectMap: return "SelectMap";
        case K::SelectOfficialMaps: return "SelectOfficialMaps";
        case K::SelectCustomMaps: return "SelectCustomMaps";
        case K::OpenGameOptions: return "OpenGameOptions";
        case K::LoadGame: return "LoadGame";
        case K::StartGame: return "StartGame";
        case K::KickPlayer: return "KickPlayer";
        case K::QuitGame: return "QuitGame";
        case K::Vote: return "Vote";
        case K::InternetHome: return "InternetHome";
        case K::QuickMatch: return "QuickMatch";
        case K::CustomMatch: return "CustomMatch";
        case K::InternetOptions: return "InternetOptions";
        case K::Buddies: return "Buddies";
        case K::PlayerProfile: return "PlayerProfile";
        case K::Community: return "Community";
        case K::RefreshGames: return "RefreshGames";
        case K::SelectRoom: return "SelectRoom";
        case K::IgnorePlayer: return "IgnorePlayer";
        case K::TestConnections: return "TestConnections";
        case K::SelectClan: return "SelectClan";
    }
    return "?";
}

// ---- navigation -------------------------------------------------------------------------------------------
std::string LobbyScreens::top() const {
    if (!gui_ || gui_->open_count() == 0) return {};
    return gui_->open_names().back();
}

GuiDialog* LobbyScreens::go(const char* name) {
    if (!gui_) return nullptr;
    const std::string t = top();
    for (const char* n : kLobbyDialogs)
        if (iequal(t, n)) gui_->close(t);
    return gui_->open(name);
}

void LobbyScreens::close_all() {
    if (!gui_) return;
    for (const char* n : kLobbyDialogs) gui_->close(n);
}

const char* LobbyScreens::staging_name() const { return staging_.internet ? dlg::kInternetStaging : dlg::kLanStaging; }

void LobbyScreens::show_multiplayer_menu() {
    if (!gui_) return;
    gui_->close_all();
    gui_->set_backdrop("MenuBack_overlay.tga");  // [port] same front-end backdrop as the main menu
    if (GuiDialog* d = gui_->open(dlg::kMultiplayer)) d->show("IDC_MAIN_OPTIONS_NETWORK", true);
}

void LobbyScreens::show_lan_lobby() {
    GuiDialog* d = go(dlg::kLanLobby);
    if (!d) return;
    d->show("IDC_GAME_LIST2", false);  // [port] second list overlaps the column buttons
    d->set_text("IDC_LAN_NAME_EDIT", local_name_);
    d->set_items("IDC_LAN_COLOR_SELECT", colors_, color_);
    if (GuiControl* c = d->get("IDC_LAN_COLOR_SELECT")) c->item_colors = color_values_;
    fill_games(*d);
    fill_chat(*d);
}

void LobbyScreens::show_internet_home() {
    GuiDialog* d = go(dlg::kInternetHome);
    if (d) d->set_items("IDC_MESSAGE_O_THE_DAY", {}, -1);
}

void LobbyScreens::show_internet_lobby() {
    GuiDialog* d = go(dlg::kInternetLobby);
    if (!d) return;
    d->set_items("IDC_ROOM_LIST", rooms_, room_);
    fill_games(*d);
    fill_chat(*d);
}

bool LobbyScreens::show_staging(const StagingState& s) {
    staging_ = s;
    GuiDialog* d = go(staging_name());
    if (!d) return false;
    fill_staging(*d);
    fill_chat(*d);
    return true;
}

void LobbyScreens::show_direct_connect(std::vector<std::u32string> recent) {
    if (!gui_) return;
    recent_ips_ = std::move(recent);
    GuiDialog* d = gui_->open(dlg::kDirectConnect);
    if (!d) return;
    d->set_items("IDC_IP_LIST", recent_ips_, -1);
    d->set_text("IDC_EDIT_IP", recent_ips_.empty() ? std::u32string() : recent_ips_.front());
    d->focus("IDC_EDIT_IP");
}

void LobbyScreens::show_password_prompt(std::string game) {
    if (!gui_) return;
    password_game_ = std::move(game);
    GuiDialog* d = gui_->open(dlg::kPassword);
    if (!d) return;
    d->set_text("IDC_INTERNET_GAME_PASSWORD_EDIT", {});
    d->focus("IDC_INTERNET_GAME_PASSWORD_EDIT");
}

void LobbyScreens::show_connecting(std::u32string status) {
    if (!gui_) return;
    GuiDialog* d = gui_->open(dlg::kConnect);
    if (!d) return;
    if (!status.empty()) d->set_text("IDC_STATIC_MED1", std::move(status));
    set_connect_progress(0.0f, {});
}

void LobbyScreens::show_disconnect(std::span<const ConnectionRow> rows) {
    if (!gui_) return;
    conn_.assign(rows.begin(), rows.end());
    GuiDialog* d = gui_->open(dlg::kDisconnect);
    if (!d) return;
    fill_disconnect(*d);
    fill_chat(*d);
}

void LobbyScreens::show_vote(const VoteState& v) {
    if (!gui_) return;
    vote_ = v;
    GuiDialog* d = gui_->open(dlg::kVote);
    if (d) fill_vote(*d);
}

// ---- fills ------------------------------------------------------------------------------------------------
void LobbyScreens::fill_games(GuiDialog& d) {
    std::vector<std::u32string> rows;
    for (const LobbyGame& g : games_) {
        std::u32string r = g.name;
        if (!g.type.empty()) r += U"  " + g.type;
        r += U"  " + num32(g.players) + U"/" + num32(g.max_players);
        if (!g.map.empty()) r += U"  " + g.map;
        if (g.ping_ms >= 0) r += U"  " + num32(g.ping_ms) + U" ms";
        if (g.password) r += U"  *";
        rows.push_back(std::move(r));
    }
    const GuiControl* l = d.get("IDC_GAME_LIST");
    const int32_t sel = l && l->selected >= 0 && static_cast<size_t>(l->selected) < rows.size() ? l->selected : -1;
    d.set_items("IDC_GAME_LIST", std::move(rows), sel);
    d.enable("IDC_LAN_JOIN_GAME_BUTTON", sel >= 0);
    d.enable("IDC_INTERNET_JOIN_GAME_BUTTON", sel >= 0);
    std::vector<std::u32string> players;
    for (const LobbyPlayer& p : players_) players.push_back(p.name);
    d.set_items("IDC_PLAYER_LIST", std::move(players), -1);
}

void LobbyScreens::fill_chat(GuiDialog& d) {
    for (const char* list : {"IDC_LAN_CHAT_LIST", "IDC_INTERNET_CHAT_LIST", "IDC_DISCONNECT_CHAT_LIST"}) {
        GuiControl* c = d.get(list);
        if (!c) continue;
        c->items = chat_;
        c->selected = -1;
        gui_->scroll_to_end(d, list);  // newest line visible
    }
}

bool LobbyScreens::slot_editable(int32_t slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= staging_.slots.size()) return false;
    const StagingSlot& s = staging_.slots[static_cast<size_t>(slot)];
    return s.local || (staging_.host && s.state == SlotState::AI);
}

void LobbyScreens::fill_staging(GuiDialog& d) {
    const StagingState& s = staging_;
    const std::vector<std::u32string> state_items = {U"Open", U"Closed", U"AI"};  // [port] labels
    bool all_accepted = true;
    size_t humans = 0;
    for (int32_t k = 0; k < kStagingSlots; ++k) {
        const bool has = static_cast<size_t>(k) < s.slots.size();
        const StagingSlot* sl = has ? &s.slots[static_cast<size_t>(k)] : nullptr;
        const std::string name = id_n("IDC_PLAYER_NAME_%d", k), fac = id_n("IDC_PLAYER_FACTION_%d", k),
                          col = id_n("IDC_PLAYER_COLOR_%d", k), team = id_n("IDC_PLAYER_TEAM_%d", k),
                          acc = id_n("IDC_ACCEPT_PLAYER_%d", k), stat = id_n("IDC_STATUS_PLAYER_%d", k);
        for (const std::string* id : {&name, &fac, &col, &team}) d.show(*id, has);
        d.show(stat, false);
        if (!sl) {
            d.show(acc, false);
            continue;
        }
        const bool filled = sl->state == SlotState::Human || sl->state == SlotState::AI;
        if (sl->state == SlotState::Human) {
            d.set_items(name, {sl->name}, 0);
            d.enable(name, false);
            ++humans;
            all_accepted = all_accepted && (sl->accepted || sl->local);
        } else {
            std::vector<std::u32string> items = state_items;
            d.set_items(name, std::move(items), static_cast<int32_t>(sl->state));
            d.enable(name, s.host);
        }
        const bool editable = slot_editable(k);
        auto combo = [&](const std::string& id, const std::vector<std::u32string>& items, int32_t v) {
            d.show(id, filled);
            d.set_items(id, items, items.empty() ? -1 : std::clamp(v, 0, static_cast<int32_t>(items.size()) - 1));
            d.enable(id, editable);
        };
        combo(fac, s.factions, sl->faction);
        combo(col, s.colors, sl->color);
        combo(team, s.teams, sl->team);
        if (GuiControl* cc = d.get(col)) cc->item_colors = s.color_values;
        d.show(acc, sl->state == SlotState::Human && sl->accepted);
        d.set_image(acc, kAcceptTexture);
    }
    d.set_items("IDC_STAGING_MAP_LIST", s.maps, s.map);
    d.enable("IDC_STAGING_MAP_LIST", s.host);
    for (const char* b : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS", "IDC_LOAD_GAME"}) d.enable(b, s.host);
    d.show("IDC_SELECT_CUSTOM_MAPS", false);  // [port] no custom maps (as the skirmish staging)
    const bool can_start = s.host && humans > 0 && all_accepted && s.map >= 0;
    for (const char* b : {"IDC_LAN_START_GAME_BUTTON", "IDC_INTERNET_START_GAME_BUTTON"}) {
        d.show(b, s.host);
        d.enable(b, can_start);
    }
    std::u32string left, right;
    for (const auto& [o, v] : s.options) {
        if (!left.empty()) {
            left += U"\n";
            right += U"\n";
        }
        left += o;
        right += v;
    }
    d.set_text("IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT", std::move(left));
    d.set_text("IDC_GAME_OPTIONS_CLIENT_DISPLAY_RIGHT", std::move(right));
    // [port] the option display is the client's view (it overlaps the host's map buttons): clients see the options,
    // the host the map selection buttons.
    d.show("IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT", !s.host);
    d.show("IDC_GAME_OPTIONS_CLIENT_DISPLAY_RIGHT", !s.host);
    d.show("IDC_SELECT_OFFICIAL_MAPS", s.host);
    d.set_image("IDC_IMAGE", s.map_image);
    d.show("IDC_IMAGE", !s.map_image.empty());
}

void LobbyScreens::fill_disconnect(GuiDialog& d) {
    for (int32_t k = 0; k < kStagingSlots; ++k) {
        const bool has = static_cast<size_t>(k) < conn_.size();
        const std::string n = id_n("IDC_PLAYER_NAME_%d", k), q = id_n("IDC_CONNECTION_QUALITY%d", k),
                          b = id_n("IDC_BUTTON_KICK_PLAYER%d", k);
        d.show(n, has);
        d.show(q, has);
        d.show(b, has && conn_[static_cast<size_t>(k)].can_kick);
        if (!has) continue;
        const ConnectionRow& r = conn_[static_cast<size_t>(k)];
        d.set_text(n, r.name);
        d.set_value(q, static_cast<int32_t>(std::lround(std::clamp(r.quality, 0.0f, 1.0f) * 100.0f)));
    }
}

void LobbyScreens::fill_vote(GuiDialog& d) {
    d.set_text("IDC_TEXT_MP_VOTE_PLANET_NAME", vote_.planet);
    if (!vote_.caption.empty()) d.set_text("IDC_CAPTION", vote_.caption);
    d.set_text("IDC_DIALOG_BOX_BUTTON_LEFT", vote_.yes);
    d.set_text("IDC_DIALOG_BOX_BUTTON_RIGHT", vote_.no);
    d.show("IDC_COUNTDOWN_TIMER", vote_.countdown >= 0.0f);
    if (vote_.countdown >= 0.0f) d.set_text("IDC_COUNTDOWN_TIMER", num32(static_cast<int64_t>(std::ceil(vote_.countdown))));
    for (int32_t k = 0; k < kStagingSlots; ++k) {
        const std::string n = id_n("IDC_NAME_PLAYER_%d", k), r = id_n("IDC_RESULT_PLAYER_%d", k);
        const bool has = static_cast<size_t>(k) < vote_.names.size();
        d.show(n, has);
        d.show(r, has);
        if (!has) continue;
        d.set_text(n, vote_.names[static_cast<size_t>(k)]);
        d.set_text(r, static_cast<size_t>(k) < vote_.results.size() ? vote_.results[static_cast<size_t>(k)] : std::u32string());
    }
}

// ---- network layer -> UI ----------------------------------------------------------------------------------
void LobbyScreens::set_games(std::vector<LobbyGame> games) {
    games_ = std::move(games);
    if (!gui_) return;
    for (const char* n : {dlg::kLanLobby, dlg::kInternetLobby})
        if (GuiDialog* d = gui_->find(n)) fill_games(*d);
}
void LobbyScreens::set_players(std::vector<LobbyPlayer> players) {
    players_ = std::move(players);
    if (!gui_) return;
    for (const char* n : {dlg::kLanLobby, dlg::kInternetLobby})
        if (GuiDialog* d = gui_->find(n)) fill_games(*d);
}
void LobbyScreens::set_local_name(std::u32string name) {
    local_name_ = std::move(name);
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kLanLobby) : nullptr) d->set_text("IDC_LAN_NAME_EDIT", local_name_);
}
void LobbyScreens::set_colors(std::vector<std::u32string> colors, int32_t selected, std::vector<Rgba8> values) {
    colors_ = std::move(colors);
    color_values_ = std::move(values);
    color_ = selected;
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kLanLobby) : nullptr) {
        d->set_items("IDC_LAN_COLOR_SELECT", colors_, color_);
        if (GuiControl* c = d->get("IDC_LAN_COLOR_SELECT")) c->item_colors = color_values_;
    }
}
void LobbyScreens::add_chat(std::u32string line) {
    chat_.push_back(std::move(line));
    if (chat_.size() > kLobbyChatLines) chat_.erase(chat_.begin(), chat_.begin() + static_cast<std::ptrdiff_t>(chat_.size() - kLobbyChatLines));
    if (!gui_) return;
    for (const char* n : kLobbyDialogs)
        if (GuiDialog* d = gui_->find(n)) fill_chat(*d);
}
void LobbyScreens::clear_chat() {
    chat_.clear();
    if (!gui_) return;
    for (const char* n : kLobbyDialogs)
        if (GuiDialog* d = gui_->find(n)) fill_chat(*d);
}
void LobbyScreens::set_staging(const StagingState& s) {
    staging_ = s;
    if (GuiDialog* d = gui_ ? gui_->find(staging_name()) : nullptr) fill_staging(*d);
}
void LobbyScreens::set_connect_progress(float progress, std::span<const std::u32string> players) {
    connect_progress_ = std::clamp(progress, 0.0f, 1.0f);
    connect_players_.assign(players.begin(), players.end());
    GuiDialog* d = gui_ ? gui_->find(dlg::kConnect) : nullptr;
    if (!d) return;
    d->set_value("IDC_NAT_PROGRESS_BAR", static_cast<int32_t>(std::lround(connect_progress_ * 100.0f)));
    d->set_items("IDC_PLAYER_LIST", connect_players_, -1);
}
void LobbyScreens::set_disconnect(std::span<const ConnectionRow> rows) {
    conn_.assign(rows.begin(), rows.end());
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kDisconnect) : nullptr) fill_disconnect(*d);
}
void LobbyScreens::set_vote(const VoteState& v) {
    vote_ = v;
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kVote) : nullptr) fill_vote(*d);
}
void LobbyScreens::set_rooms(std::vector<std::u32string> rooms, int32_t selected) {
    rooms_ = std::move(rooms);
    room_ = selected;
    if (GuiDialog* d = gui_ ? gui_->find(dlg::kInternetLobby) : nullptr) d->set_items("IDC_ROOM_LIST", rooms_, room_);
}
void LobbyScreens::set_internet_stats(int32_t players_online, int32_t games_in_progress, int32_t games_available) {
    GuiDialog* d = gui_ ? gui_->find(dlg::kInternetHome) : nullptr;
    if (!d) return;
    d->set_text("IDC_PLAYERS_ONLINE_STAT", num32(players_online));
    d->set_text("IDC_GAMES_IN_PROGRESS_STAT1", num32(games_in_progress));
    d->set_text("IDC_GAMES_AVAILABLE_STAT", num32(games_available));
}

// ---- events -----------------------------------------------------------------------------------------------
void LobbyScreens::join_selected(GuiDialog& d, std::vector<LobbyEvent>& out) {
    const GuiControl* l = d.get("IDC_GAME_LIST");
    if (!l || l->selected < 0 || static_cast<size_t>(l->selected) >= games_.size()) return;
    const LobbyGame& g = games_[static_cast<size_t>(l->selected)];
    if (g.password) {
        show_password_prompt(g.id);
        return;
    }
    LobbyEvent e = ev(LobbyEvent::Kind::JoinGame);
    e.game = g.id;
    out.push_back(std::move(e));
}

void LobbyScreens::on_click(const GuiEvent& e, std::vector<LobbyEvent>& out) {
    using K = LobbyEvent::Kind;
    const std::string_view n = e.dialog, c = e.control;
    auto is = [&](std::string_view id) { return iequal(c, id); };
    GuiDialog* d = gui_->find(n);
    if (!d) return;
    for (const ColumnButton& col : kColumns)
        if (is(col.id) && (iequal(n, dlg::kLanLobby) || iequal(n, dlg::kInternetLobby))) {
            LobbyEvent s = ev(K::SortGames);
            s.value = col.column;
            out.push_back(s);
            return;
        }
    if (iequal(n, dlg::kMultiplayer)) {
        if (is("IDC_MULTIPLAYER_LAN_BUTTON")) {
            out.push_back(ev(K::EnterLanLobby));
            show_lan_lobby();
        } else if (is("IDC_MULTIPLAYER_INTERNET_BUTTON")) {
            out.push_back(ev(K::OpenInternet));
            show_internet_home();
        } else if (is("IDC_MAIN_OPTIONS_NETWORK")) {
            out.push_back(ev(K::OpenNetworkOptions));
        } else if (is("IDC_OK_MAIN")) {
            gui_->close(n);
            out.push_back(ev(K::BackToMainMenu));
        } else if (is("IDC_MENU_CREDITS")) {
            out.push_back(ev(K::ShowCredits));
        }
    } else if (iequal(n, dlg::kLanLobby) || iequal(n, dlg::kInternetLobby)) {
        if (is("IDC_LAN_HOST_GAME_BUTTON") || is("IDC_INTERNET_HOST_GAME_BUTTON")) out.push_back(ev(K::HostGame));
        else if (is("IDC_LAN_JOIN_GAME_BUTTON") || is("IDC_INTERNET_JOIN_GAME_BUTTON")) join_selected(*d, out);
        else if (is("IDC_SET_FILTER_BUTTON")) out.push_back(ev(K::OpenGameFilter));
        else if (is("IDC_INTERNET_REFRESH_GAME_LIST_BUTTON")) out.push_back(ev(K::RefreshGames));
        else if (is("IDC_INTERNET_IGNORE_PLAYER_BUTTON")) {
            const GuiControl* p = d->get("IDC_PLAYER_LIST");
            if (p && p->selected >= 0) {
                LobbyEvent x = ev(K::IgnorePlayer);
                x.value = p->selected;
                out.push_back(x);
            }
        } else if (is("IDC_LAN_CANCEL_BUTTON")) {
            out.push_back(ev(K::LeaveLobby));
            show_multiplayer_menu();
        }
    }
    if (iequal(n, dlg::kInternetHome) || iequal(n, dlg::kInternetLobby)) {
        if (is("IDC_INTERNET_EXIT_BUTTON")) {
            out.push_back(ev(K::LeaveLobby));
            show_multiplayer_menu();
        } else if (is("IDC_INTERNET_HOME_BUTTON")) {
            out.push_back(ev(K::InternetHome));
            if (!iequal(n, dlg::kInternetHome)) show_internet_home();
        } else if (is("IDC_INTERNET_CUSTOM_MATCH_BUTTON")) {
            out.push_back(ev(K::CustomMatch));
            if (!iequal(n, dlg::kInternetLobby)) show_internet_lobby();
        } else if (is("IDC_INTERNET_QUICK_MATCH_BUTTON")) out.push_back(ev(K::QuickMatch));
        else if (is("IDC_INTERNET_OPTIONS_BUTTON")) out.push_back(ev(K::InternetOptions));
        else if (is("IDC_INTERNET_BUDDIES_BUTTON")) out.push_back(ev(K::Buddies));
        else if (is("IDC_INTERNET_PLAYER_PROFILE_BUTTON")) out.push_back(ev(K::PlayerProfile));
        else if (is("IDC_INTERNET_COMMUNITY_BUTTON")) out.push_back(ev(K::Community));
    } else if (iequal(n, dlg::kLanStaging) || iequal(n, dlg::kInternetStaging)) {
        if (is("IDC_LAN_START_GAME_BUTTON") || is("IDC_INTERNET_START_GAME_BUTTON")) out.push_back(ev(K::StartGame));
        else if (is("IDC_LAN_CANCEL_BUTTON")) {
            out.push_back(ev(K::LeaveGame));
            if (staging_.internet) show_internet_lobby();
            else show_lan_lobby();
        } else if (is("IDC_SELECT_OPTIONS")) out.push_back(ev(K::OpenGameOptions));
        else if (is("IDC_LOAD_GAME")) out.push_back(ev(K::LoadGame));
        else if (is("IDC_SELECT_OFFICIAL_MAPS")) out.push_back(ev(K::SelectOfficialMaps));
        else if (is("IDC_SELECT_CUSTOM_MAPS")) out.push_back(ev(K::SelectCustomMaps));
        else if (is("IDC_INTERNET_BUDDIES_BUTTON")) out.push_back(ev(K::Buddies));
        else if (is("IDC_INTERNET_CONNECTION_TEST_BUTTON")) out.push_back(ev(K::TestConnections));
        else if (is("IDC_SELECT_CLAN_BUTTON")) out.push_back(ev(K::SelectClan));
    } else if (iequal(n, dlg::kDirectConnect)) {
        const GuiControl* ed = d->get("IDC_EDIT_IP");
        const std::u32string t = ed ? ed->text : std::u32string();
        if (is("IDC_ADD_IP_BUTTON") && !t.empty()) {
            if (std::find(recent_ips_.begin(), recent_ips_.end(), t) == recent_ips_.end()) recent_ips_.push_back(t);
            d->set_items("IDC_IP_LIST", recent_ips_, static_cast<int32_t>(recent_ips_.size()) - 1);
            LobbyEvent x = ev(K::AddDirectAddress);
            x.text = t;
            out.push_back(std::move(x));
        } else if (is("IDC_IP_CONNECT_BUTTON") && !t.empty()) {
            LobbyEvent x = ev(K::ConnectDirect);
            x.text = t;
            gui_->close(n);
            out.push_back(std::move(x));
        } else if (is("IDC_CANCEL_BUTTON")) {
            gui_->close(n);
        }
    } else if (iequal(n, dlg::kPassword)) {
        if (is("IDC_OK")) {
            LobbyEvent x = ev(K::JoinGame);
            x.game = password_game_;
            if (const GuiControl* ed = d->get("IDC_INTERNET_GAME_PASSWORD_EDIT")) x.text = ed->text;
            gui_->close(n);
            out.push_back(std::move(x));
        } else if (is("IDC_CANCEL_BUTTON")) {
            gui_->close(n);
        }
    } else if (iequal(n, dlg::kConnect)) {
        if (is("IDC_LAN_CANCEL_BUTTON")) {
            gui_->close(n);
            out.push_back(ev(K::CancelConnect));
        }
    } else if (iequal(n, dlg::kDisconnect)) {
        if (is("IDC_QUIT_GAME_BUTTON")) out.push_back(ev(K::QuitGame));
        else if (istarts(c, "IDC_BUTTON_KICK_PLAYER")) {
            LobbyEvent x = ev(K::KickPlayer);
            x.slot = trailing_index(c);
            out.push_back(x);
        }
    } else if (iequal(n, dlg::kVote)) {
        if (is("IDC_DIALOG_BOX_BUTTON_LEFT") || is("IDC_DIALOG_BOX_BUTTON_RIGHT")) {
            LobbyEvent x = ev(K::Vote);
            x.value = is("IDC_DIALOG_BOX_BUTTON_LEFT") ? 1 : 0;
            d->enable("IDC_DIALOG_BOX_BUTTON_LEFT", false);
            d->enable("IDC_DIALOG_BOX_BUTTON_RIGHT", false);
            out.push_back(x);
        }
    }
}

void LobbyScreens::on_change(const GuiEvent& e, std::vector<LobbyEvent>& out) {
    using K = LobbyEvent::Kind;
    const std::string_view n = e.dialog, c = e.control;
    GuiDialog* d = gui_->find(n);
    if (!d) return;
    if (iequal(n, dlg::kLanLobby) || iequal(n, dlg::kInternetLobby)) {
        if (iequal(c, "IDC_GAME_LIST")) {
            if (e.kind == GuiEvent::Kind::DoubleClicked) {
                join_selected(*d, out);
                return;
            }
            d->enable("IDC_LAN_JOIN_GAME_BUTTON", e.value >= 0);
            d->enable("IDC_INTERNET_JOIN_GAME_BUTTON", e.value >= 0);
            if (e.value >= 0 && static_cast<size_t>(e.value) < games_.size()) {
                LobbyEvent x = ev(K::SelectGame);
                x.game = games_[static_cast<size_t>(e.value)].id;
                out.push_back(std::move(x));
            }
        } else if (iequal(c, "IDC_LAN_COLOR_SELECT") && e.kind == GuiEvent::Kind::SelectionChanged) {
            color_ = e.value;
            LobbyEvent x = ev(K::SetPlayerColor);
            x.value = e.value;
            out.push_back(x);
        } else if (iequal(c, "IDC_ROOM_LIST") && e.kind == GuiEvent::Kind::SelectionChanged) {
            room_ = e.value;
            LobbyEvent x = ev(K::SelectRoom);
            x.value = e.value;
            out.push_back(x);
        }
    } else if ((iequal(n, dlg::kLanStaging) || iequal(n, dlg::kInternetStaging)) && e.kind == GuiEvent::Kind::SelectionChanged) {
        const int32_t slot = trailing_index(c);
        LobbyEvent x;
        x.slot = slot;
        x.value = e.value;
        if (iequal(c, "IDC_STAGING_MAP_LIST")) {
            if (!staging_.host) return;
            staging_.map = e.value;
            x.kind = K::SelectMap;
            x.slot = -1;
        } else if (istarts(c, "IDC_PLAYER_NAME_")) {
            if (!staging_.host || slot < 0 || static_cast<size_t>(slot) >= staging_.slots.size() ||
                staging_.slots[static_cast<size_t>(slot)].state == SlotState::Human)
                return;
            x.kind = K::SetSlotState;  // items Open / Closed / AI = SlotState 0..2
        } else if (istarts(c, "IDC_PLAYER_FACTION_") && slot_editable(slot)) {
            x.kind = K::SetSlotFaction;
        } else if (istarts(c, "IDC_PLAYER_COLOR_") && slot_editable(slot)) {
            x.kind = K::SetSlotColor;
        } else if (istarts(c, "IDC_PLAYER_TEAM_") && slot_editable(slot)) {
            x.kind = K::SetSlotTeam;
        } else {
            return;
        }
        out.push_back(x);
    } else if (iequal(n, dlg::kDirectConnect) && iequal(c, "IDC_IP_LIST") && e.value >= 0 &&
               static_cast<size_t>(e.value) < recent_ips_.size()) {
        d->set_text("IDC_EDIT_IP", recent_ips_[static_cast<size_t>(e.value)]);
        if (e.kind == GuiEvent::Kind::DoubleClicked) {
            GuiEvent b = e;
            b.kind = GuiEvent::Kind::Clicked;
            b.control = "IDC_IP_CONNECT_BUTTON";
            on_click(b, out);
        }
    }
}

void LobbyScreens::on_submit(const GuiEvent& e, std::vector<LobbyEvent>& out) {
    using K = LobbyEvent::Kind;
    const std::string_view n = e.dialog, c = e.control;
    GuiDialog* d = gui_->find(n);
    if (!d) return;
    GuiControl* ed = d->get(c);
    const std::u32string t = !e.text.empty() ? e.text : (ed ? ed->text : std::u32string());
    if (iequal(c, "IDC_LAN_CHAT_EDIT") || iequal(c, "IDC_INTERNET_CHAT_EDIT") || iequal(c, "IDC_DISCONNECT_CHAT_EDIT")) {
        if (t.empty()) return;
        LobbyEvent x = ev(K::Chat);
        x.text = t;
        out.push_back(std::move(x));
        if (ed) {
            ed->text.clear();
            ed->caret = 0;
        }
    } else if (iequal(c, "IDC_LAN_NAME_EDIT")) {
        local_name_ = t;
        LobbyEvent x = ev(K::SetPlayerName);
        x.text = t;
        out.push_back(std::move(x));
    } else if (iequal(n, dlg::kPassword)) {
        GuiEvent b = e;
        b.kind = GuiEvent::Kind::Clicked;
        b.control = "IDC_OK";
        on_click(b, out);
    } else if (iequal(n, dlg::kDirectConnect)) {
        GuiEvent b = e;
        b.kind = GuiEvent::Kind::Clicked;
        b.control = "IDC_IP_CONNECT_BUTTON";
        on_click(b, out);
    }
}

void LobbyScreens::on_cancel(const GuiEvent& e, std::vector<LobbyEvent>& out) {
    using K = LobbyEvent::Kind;
    const std::string_view n = e.dialog;
    if (iequal(n, dlg::kMultiplayer)) {
        gui_->close(n);
        out.push_back(ev(K::BackToMainMenu));
    } else if (iequal(n, dlg::kLanLobby) || iequal(n, dlg::kInternetLobby) || iequal(n, dlg::kInternetHome)) {
        out.push_back(ev(K::LeaveLobby));
        show_multiplayer_menu();
    } else if (iequal(n, dlg::kLanStaging) || iequal(n, dlg::kInternetStaging)) {
        out.push_back(ev(K::LeaveGame));
        if (staging_.internet) show_internet_lobby();
        else show_lan_lobby();
    } else if (iequal(n, dlg::kDirectConnect) || iequal(n, dlg::kPassword)) {
        gui_->close(n);
    } else if (iequal(n, dlg::kConnect)) {
        gui_->close(n);
        out.push_back(ev(K::CancelConnect));
    }
    // Disconnect and vote dialogs ignore Esc: the network layer closes them [port].
}

void LobbyScreens::update(std::span<const GuiEvent> events, std::vector<LobbyEvent>& out) {
    if (!gui_) return;
    for (const GuiEvent& e : events) {
        bool ours = false;
        for (const char* n : kLobbyDialogs) ours = ours || iequal(e.dialog, n);
        if (!ours) continue;
        switch (e.kind) {
            case GuiEvent::Kind::Clicked: on_click(e, out); break;
            case GuiEvent::Kind::SelectionChanged:
            case GuiEvent::Kind::DoubleClicked: on_change(e, out); break;
            case GuiEvent::Kind::Submitted: on_submit(e, out); break;
            case GuiEvent::Kind::Cancel: on_cancel(e, out); break;
            default: break;
        }
    }
}

}  // namespace alamo::ui
