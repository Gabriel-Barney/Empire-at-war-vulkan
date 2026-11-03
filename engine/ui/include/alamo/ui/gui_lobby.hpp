// P4 multiplayer screens, UI only (docs/re/UI.md §3.2 "LAN/Internet lobbies"): no networking here. A later network
// layer pushes what it knows (game list, players, chat, staging slots, connection quality, votes) through the set_*
// calls and consumes LobbyEvents; every click becomes an event, nothing is sent from here.
//
//   IDD_MULTIPLAYER_DIALOG          Internet -> OpenInternet (+ IDD_INTERNET_HOME_DIALOG), LAN -> EnterLanLobby
//                                   (+ IDD_LAN_LOBBY_DIALOG), Internet options -> OpenNetworkOptions, Back ->
//                                   BackToMainMenu, Credits -> ShowCredits
//   IDD_LAN_LOBBY_DIALOG            name edit (Enter / focus loss) -> SetPlayerName, colour combo -> SetPlayerColor, game
//                                   list -> SelectGame, Host -> HostGame, Join (or double click) -> JoinGame (a password
//                                   game opens IDD_ENTER_GAME_PASSWORD_DIALOG -> JoinGame with text = password), column
//                                   buttons -> SortGames{value = column}, Set filter -> OpenGameFilter, chat Enter -> Chat,
//                                   Cancel / Esc -> LeaveLobby (back to IDD_MULTIPLAYER_DIALOG)
//   IDD_LAN_STAGING_DIALOG /        per slot 0..8: name combo (host: Open / Closed / AI on empty slots) -> SetSlotState,
//   IDD_INTERNET_STAGING_DIALOG     faction / colour / team combos -> SetSlotFaction / Color / Team (own slot, or any AI
//                                   slot for the host), accept marks; map list -> SelectMap (host); official / custom
//                                   maps -> SelectOfficialMaps / SelectCustomMaps; Select options -> OpenGameOptions;
//                                   Load -> LoadGame (N1); Start -> StartGame (host, enabled when every human accepted);
//                                   chat -> Chat; Cancel / Esc -> LeaveGame
//   IDD_IP_DIRECT_CONNECT_DIALOG    Add -> AddDirectAddress{text}, list selection fills the edit, Connect ->
//                                   ConnectDirect{text}, Cancel -> closed
//   IDD_ENTER_GAME_PASSWORD_DIALOG  OK / Enter -> JoinGame{game, text = password}; Cancel -> closed
//   IDD_MP_CONNECT_DIALOG           progress + player list; Cancel -> CancelConnect
//   IDD_DISCONNECT_DIALOG           player rows (name, quality bar, kick) -> KickPlayer{slot}; Quit -> QuitGame; chat
//   IDD_MP_VOTE_DIALOG              planet, per-player name / result, countdown; left -> Vote{value 1}, right -> Vote{0}
//   IDD_INTERNET_HOME_DIALOG /      home / quick match / custom match / options / buddies / profile / community buttons ->
//   IDD_INTERNET_LOBBY_DIALOG       the matching event; lobby: game list, rooms, players, host / join / refresh, chat
// [port] choices: the online service is gone - the internet screens are kept as UI for a future service, login is not
// modelled (IDD_INTERNET_LOGIN_DIALOG is never opened); combos show the strings the network layer supplies (factions,
// colours, teams); a game list row is "name  map  players/max  ping"; chat keeps the last 200 lines; the staging
// option display (IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT / _RIGHT) shows "option" / "setting" lines joined by "\n", for
// clients only (it overlaps the host's map buttons); colour combos hold one glyph per palette entry (U+25A0 staging, U+2588 LAN lobby) drawn in the entry's colour when
// StagingState::color_values / set_colors values are given (UI.md §3.7.1).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/ui/gui_dialog.hpp"

namespace alamo::ui {

struct LobbyGame {
    std::string id;                // network layer id
    std::u32string name, map, type;
    uint32_t players = 0, max_players = 8;
    int32_t ping_ms = -1;
    bool password = false;
};
struct LobbyPlayer {
    std::u32string name;
    int32_t color = -1;
};

enum class SlotState : uint8_t { Open = 0, Closed = 1, AI = 2, Human = 3 };
struct StagingSlot {
    SlotState state = SlotState::Open;
    std::u32string name;           // Human: player name
    int32_t faction = 0, color = 0, team = 0;  // indices into StagingState lists
    bool accepted = false;
    bool local = false;            // this machine's player
};
struct StagingState {
    bool host = false;
    bool internet = false;         // IDD_INTERNET_STAGING_DIALOG instead of the LAN one
    std::vector<StagingSlot> slots;  // up to 9
    std::vector<std::u32string> maps;
    int32_t map = -1;
    std::vector<std::u32string> factions, colors, teams;
    std::vector<Rgba8> color_values;  // optional: colour combo items become one glyph per entry in that colour (GuiControl::item_colors)
    std::vector<std::pair<std::u32string, std::u32string>> options;  // display lines (option, setting)
    std::string map_image;         // IDC_IMAGE texture
};
struct ConnectionRow {
    std::u32string name;
    float quality = 1.0f;          // 0..1
    bool can_kick = false;
};
struct VoteState {
    std::u32string planet, caption;
    std::vector<std::u32string> names, results;  // up to 9
    float countdown = -1.0f;
    std::u32string yes = U"Yes", no = U"No";
};

struct LobbyEvent {
    enum class Kind : uint8_t {
        BackToMainMenu, ShowCredits, OpenInternet, OpenNetworkOptions, EnterLanLobby, LeaveLobby,
        SetPlayerName,      // text
        SetPlayerColor,     // value
        SelectGame,         // game
        HostGame,
        JoinGame,           // game, text = password ("" none)
        SortGames,          // value = column 0 name, 1 type, 2 players, 3 map, 4 ping
        OpenGameFilter,
        OpenDirectConnect,
        AddDirectAddress,   // text
        ConnectDirect,      // text
        CancelConnect,
        Chat,               // text
        LeaveGame,
        SetSlotState,       // slot, value = SlotState
        SetSlotFaction,     // slot, value
        SetSlotColor,       // slot, value
        SetSlotTeam,        // slot, value
        SelectMap,          // value
        SelectOfficialMaps, SelectCustomMaps, OpenGameOptions,
        LoadGame,           // N1: multiplayer save
        StartGame,
        KickPlayer,         // slot
        QuitGame,
        Vote,               // value 1 yes / 0 no
        InternetHome, QuickMatch, CustomMatch, InternetOptions, Buddies, PlayerProfile, Community, RefreshGames,
        SelectRoom,         // value
        IgnorePlayer,       // value = player row
        TestConnections, SelectClan,
    };
    Kind kind = Kind::BackToMainMenu;
    int32_t slot = -1, value = -1;
    std::string game;
    std::u32string text;
};
const char* lobby_event_name(LobbyEvent::Kind k);

namespace dlg {
inline constexpr const char* kMultiplayer = "IDD_MULTIPLAYER_DIALOG";
inline constexpr const char* kLanLobby = "IDD_LAN_LOBBY_DIALOG";
inline constexpr const char* kLanStaging = "IDD_LAN_STAGING_DIALOG";
inline constexpr const char* kInternetStaging = "IDD_INTERNET_STAGING_DIALOG";
inline constexpr const char* kInternetHome = "IDD_INTERNET_HOME_DIALOG";
inline constexpr const char* kInternetLobby = "IDD_INTERNET_LOBBY_DIALOG";
inline constexpr const char* kDirectConnect = "IDD_IP_DIRECT_CONNECT_DIALOG";
inline constexpr const char* kPassword = "IDD_ENTER_GAME_PASSWORD_DIALOG";
inline constexpr const char* kConnect = "IDD_MP_CONNECT_DIALOG";
inline constexpr const char* kDisconnect = "IDD_DISCONNECT_DIALOG";
inline constexpr const char* kVote = "IDD_MP_VOTE_DIALOG";
}  // namespace dlg

inline constexpr int32_t kStagingSlots = 9;
inline constexpr size_t kLobbyChatLines = 200;

class LobbyScreens {
public:
    void init(GuiManager& gui) { gui_ = &gui; }

    void show_multiplayer_menu();          // replaces whatever is open (front-end flow)
    void show_lan_lobby();
    void show_internet_home();
    void show_internet_lobby();
    bool show_staging(const StagingState& s);  // LAN / internet by s.internet
    void show_direct_connect(std::vector<std::u32string> recent);
    void show_password_prompt(std::string game);
    void show_connecting(std::u32string status = {});
    void show_disconnect(std::span<const ConnectionRow> rows);
    void show_vote(const VoteState& v);
    void close_all();
    std::string top() const;

    // Network layer -> UI (refresh whatever is open).
    void set_games(std::vector<LobbyGame> games);
    void set_players(std::vector<LobbyPlayer> players);
    void set_local_name(std::u32string name);
    void set_colors(std::vector<std::u32string> colors, int32_t selected, std::vector<Rgba8> values = {});
    void add_chat(std::u32string line);
    void clear_chat();
    void set_staging(const StagingState& s);
    void set_connect_progress(float progress, std::span<const std::u32string> players);
    void set_disconnect(std::span<const ConnectionRow> rows);
    void set_vote(const VoteState& v);
    void set_rooms(std::vector<std::u32string> rooms, int32_t selected);
    void set_internet_stats(int32_t players_online, int32_t games_in_progress, int32_t games_available);

    const std::vector<LobbyGame>& games() const { return games_; }
    const StagingState& staging() const { return staging_; }

    void update(std::span<const GuiEvent> events, std::vector<LobbyEvent>& out);

private:
    GuiDialog* go(const char* name);       // replaces the top lobby dialog
    void fill_games(GuiDialog& d);
    void fill_chat(GuiDialog& d);
    void fill_staging(GuiDialog& d);
    void fill_disconnect(GuiDialog& d);
    void fill_vote(GuiDialog& d);
    void on_click(const GuiEvent& e, std::vector<LobbyEvent>& out);
    void on_change(const GuiEvent& e, std::vector<LobbyEvent>& out);
    void on_cancel(const GuiEvent& e, std::vector<LobbyEvent>& out);
    void on_submit(const GuiEvent& e, std::vector<LobbyEvent>& out);
    void join_selected(GuiDialog& d, std::vector<LobbyEvent>& out);
    const char* staging_name() const;
    bool slot_editable(int32_t slot) const;

    GuiManager* gui_ = nullptr;
    std::vector<LobbyGame> games_;
    std::vector<LobbyPlayer> players_;
    std::u32string local_name_;
    std::vector<std::u32string> colors_;
    std::vector<Rgba8> color_values_;
    int32_t color_ = -1;
    std::vector<std::u32string> chat_;
    StagingState staging_;
    std::vector<ConnectionRow> conn_;
    VoteState vote_;
    std::vector<std::u32string> recent_ips_;
    std::vector<std::u32string> rooms_;
    int32_t room_ = -1;
    std::string password_game_;
    float connect_progress_ = 0.0f;
    std::vector<std::u32string> connect_players_;
};

}  // namespace alamo::ui
