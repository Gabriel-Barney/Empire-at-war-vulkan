// alamo_game App: the mod picker (modc t08; ui IDD_PORT_MODS_DIALOG, game/mod_picker.hpp, docs/MODS.md §5.2).
// The rows are the base game and the installed mods (Steam workshop items found the way the STEAMMOD= resolver finds them,
// and <game>\Mods folders). Selecting a row runs the load report (game::check_mod_data, the same report alamo_mod_check
// prints, alamo.mod_report) on a worker thread - presentation only, its own Vfs and data, nothing the sim reads - and
// shows its summary; Launch restarts this exe with the row's MODPATH= / STEAMMOD= (a likely submod stacked on its
// parent, as the report says) and quits, since the data, text and GUI are loaded once per process.
#include "game_app.hpp"
#include "game_display.hpp"
#include "alamo/vfs/steam_workshop.hpp"

namespace alamo_game_app {
namespace {

// A mod argument's value compared as a folder: '/' == '\\', any case.
std::string arg_key(std::string_view a) {
    std::string k(a);
    for (char& c : k) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return k;
}

std::vector<std::string> mod_args_of(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (const std::string& a : args)
        if (game::is_mod_arg(a)) out.push_back(a);
    return out;
}

}  // namespace

void App::init_mod_picker(ui::MenuProviders& mp) {
    workshop_libraries_ = vfs::find_steam_libraries(game_root_, vfs::default_steam_roots());
    installed_mods_ = game::list_installed_mods(game_root_, workshop_libraries_);
    std::printf("mods: %zu installed (%zu Steam libraries)\n", installed_mods_.size(), workshop_libraries_.size());
    // The active row: the first mod argument of this run (a stacked submod comes first), else the base game.
    const std::vector<std::string> active = mod_args_of(opt_.game_args);
    const std::string active_key = active.empty() ? std::string() : arg_key(active.front());
    mp.mods = [this, active_key] {
        // Labels: the mod's name and where it is installed; the id (workshop item / folder) only when two mods share a
        // name (several Thrawn's Revenge versions) - the report's "Starts with:" line names it for every row.
        std::map<std::string, size_t> names;
        for (const game::InstalledMod& m : installed_mods_) ++names[m.name];
        std::vector<ui::ModPickerEntry> rows;
        ui::ModPickerEntry base;
        base.label = U"Base game (no mod)";
        base.active = active_key.empty();
        rows.push_back(std::move(base));
        for (const game::InstalledMod& m : installed_mods_) {
            ui::ModPickerEntry e;
            e.id = m.arg;  // unique: STEAMMOD=<id> / MODPATH=Mods\<folder>
            std::string label = m.name;
            if (m.name != m.id && names[m.name] > 1) label += "  [" + m.id + "]";
            label += m.workshop ? "  (Workshop)" : "  (Mods)";
            e.label = ui::utf8_to_utf32(label);
            e.active = !active_key.empty() && arg_key(m.arg) == active_key;
            rows.push_back(std::move(e));
        }
        return rows;
    };
}

void App::check_mod(const std::string& id) {
    if (mod_launch_.count(id)) return;  // reported already (FrontEnd keeps the lines)
    if (mod_check_.valid()) {           // one check at a time: the newest request runs next
        if (id != mod_check_id_) {
            mod_check_next_ = id;
            mod_check_queued_ = true;
        }
        return;
    }
    mod_check_id_ = id;
    std::vector<std::string> args;
    if (!id.empty()) args.push_back(id);
    game::ModCheckOptions o;
    o.workshop_libraries = workshop_libraries_;
    o.shader_compile = false;  // the effect audit without the runtime DXC compile: lighter inside the running game
    const std::string root = game_root_;
    std::printf("mods: checking %s\n", id.empty() ? "the base game" : id.c_str());
    mod_check_ = std::async(std::launch::async, [root, args, o] { return game::check_mod_data(root, args, o); });
}

void App::poll_mod_check() {
    if (!mod_check_.valid() || mod_check_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const game::ModCheckReport r = mod_check_.get();
    const std::string id = mod_check_id_;
    std::vector<std::string> launch;
    if (!id.empty()) {
        for (const game::InstalledMod& m : installed_mods_)
            if (m.arg == id) launch = game::mod_launch_args(m, &r);
        if (launch.empty()) launch.push_back(id);
    }
    mod_launch_[id] = launch;
    std::vector<std::u32string> lines;
    for (const std::string& l : game::mod_report_summary(r)) lines.push_back(ui::utf8_to_utf32(l));
    if (!id.empty()) {
        std::string args;
        for (const std::string& a : launch) args += (args.empty() ? "" : " ") + a;
        lines.push_back(ui::utf8_to_utf32("Starts with: " + args));
    }
    std::printf("mods: %s: %zu errors, %zu warnings\n", id.empty() ? "base game" : id.c_str(), r.error_count(), r.warning_count());
    ui_.front.set_mod_report(id, std::move(lines));
    if (mod_check_queued_) {
        mod_check_queued_ = false;
        check_mod(mod_check_next_);
    }
}

void App::launch_mod(const std::string& id) {
    const auto it = mod_launch_.find(id);
    const std::vector<std::string> mod_args = it != mod_launch_.end() ? it->second : (id.empty() ? std::vector<std::string>{}
                                                                                                    : std::vector<std::string>{id});
    const std::vector<std::string> args = game::relaunch_args(opt_.argv, mod_args);
    std::string all;
    for (const std::string& a : args) all += " " + a;
    std::printf("mods: relaunch:%s\n", all.c_str());
    std::string err;
    if (!game::relaunch_self(game::windows_command_line(args), &err)) {
        notes_.push_back("mod launch failed: " + err);
        ui_.front.set_mod_report(id, {ui::utf8_to_utf32("Launch failed: " + err)});
        return;
    }
    screen_ = Screen::Quit;
}

}  // namespace alamo_game_app
