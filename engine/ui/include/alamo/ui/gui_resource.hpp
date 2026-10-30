// P4 GUI dialog data (docs/re/UI.md §3.1-3.4): the three files TheGUIDialogManager reads at boot.
//   * Data\Resources\GUIDialog\Resource.h   -> ResourceIds (#define IDD_* / IDC_* numeric ids)
//   * Data\Resources\GUIDialog\GUIDialogs.rc -> DialogScript: one RcDialog per DIALOGEX block (FoC 1.121 ships 111;
//     UI.md's "117" counts differently), controls with positions / sizes in dialog units, style tokens, captions
//     (MasterTextFile keys or runtime placeholders). The original parses the script text itself (no .rc compile).
//   * Data\XML\GUIDialogs.xml -> GuiSkin: <Textures> (Default gadget skins + per IDD_* / IDC_* overrides, "none" =
//     no texture), <Fonts> (Default per gadget kind + per IDC_* + nested per IDD_*), <Tooltips> (IDC_* -> TextID).
//
// Win32 resource-script conventions used for the gadget classification (gadget_type_of):
//   PUSHBUTTON / DEFPUSHBUTTON -> push button; LTEXT / RTEXT / CTEXT -> text (left / right / centred); EDITTEXT -> edit;
//   COMBOBOX; LISTBOX; GROUPBOX -> frame; CONTROL by class: "Button" + BS_AUTOCHECKBOX / BS_CHECKBOX / BS_3STATE ->
//   check box, + BS_AUTORADIOBUTTON / BS_RADIOBUTTON -> radio (BS_PUSHLIKE = tab look), + BS_GROUPBOX -> frame, else
//   push button; msctls_trackbar32 -> slider; msctls_progress32 -> progress bar; PETROGLYPH_DIALOG_IMAGE -> custom
//   image; IMEEditBox / IMELocaleIndicator; Static -> text (SS_CENTER / SS_RIGHT); ScrollBar -> scroll bar.
//   Visibility: visible unless "NOT WS_VISIBLE"; WS_DISABLED; WS_TABSTOP (PUSHBUTTON / DEFPUSHBUTTON / EDITTEXT have it
//   by default, "NOT WS_TABSTOP" removes it); WS_GROUP starts a new radio group (Win32 rule: a group runs from one
//   WS_GROUP control to the next).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::data {
class XmlNode;
}

namespace alamo::ui {

inline constexpr const char* kGuiDialogsRcPath = "Data\\Resources\\GUIDialog\\GUIDialogs.rc";
inline constexpr const char* kGuiResourceHPath = "Data\\Resources\\GUIDialog\\Resource.h";
inline constexpr const char* kGuiDialogsXmlPath = "Data\\XML\\GUIDialogs.xml";

// ---- Resource.h -------------------------------------------------------------------------------------
class ResourceIds {
public:
    // "#define NAME value" lines (decimal or 0x hex); other lines ignored. Returns the number of defines read.
    size_t parse(std::string_view text);
    std::optional<int32_t> id(std::string_view name) const;  // exact name (IDs are upper case in the data)
    std::string_view name(int32_t id) const;                 // first name defined with this value, "" if none
    size_t size() const { return by_name_.size(); }

private:
    std::map<std::string, int32_t, std::less<>> by_name_;
    std::map<int32_t, std::string> by_id_;
};

// ---- GUIDialogs.rc ------------------------------------------------------------------------------------
enum class RcControlKind : uint8_t {
    PushButton, DefPushButton, LText, RText, CText, EditText, ComboBox, ListBox, GroupBox, Control, Other
};
enum class GuiGadgetType : uint8_t {
    PushButton, Text, EditBox, IMEEditBox, ComboBox, ListBox, CheckBox, RadioButton, TrackbarSlider, ProgressBar,
    VScrollbar, CustomImage, Frame, IMELocaleIndicator, Unknown
};
const char* gadget_type_name(GuiGadgetType t);

struct RcControl {
    RcControlKind kind = RcControlKind::Other;
    std::string keyword;     // as written ("PUSHBUTTON", "CONTROL", ...)
    std::string caption;     // "" for EDITTEXT / COMBOBOX / LISTBOX
    std::string id;          // IDC_* name (or a number as written)
    std::string class_name;  // CONTROL window class ("Button", "msctls_trackbar32", ...)
    int32_t x = 0, y = 0, w = 0, h = 0;  // dialog units, top-left origin, +Y down
    std::vector<std::string> styles;     // style tokens; "NOT X" stored as "!X"
    uint32_t line = 0;
    // Derived (classify()).
    GuiGadgetType gadget = GuiGadgetType::Unknown;
    TextAlign align = TextAlign::Left;
    bool default_button = false, tabstop = false, pushlike = false, visible = true, disabled = false, group = false;
    bool has_style(std::string_view s) const;
    void classify();
};

struct RcDialog {
    std::string name;  // IDD_*
    int32_t x = 0, y = 0, w = 0, h = 0;
    std::vector<std::string> styles;
    std::string caption;
    int32_t font_size = 8;
    std::string font_face;
    std::vector<RcControl> controls;  // script order (= tab order)
    uint32_t line = 0;
    bool has_style(std::string_view s) const;
    bool centered() const { return has_style("DS_CENTER"); }
    int32_t find(std::string_view id) const;  // first control with this IDC name, -1 if none
};

class DialogScript {
public:
    // Parses every DIALOG / DIALOGEX block; other top-level statements and BEGIN/END blocks (DESIGNINFO,
    // TEXTINCLUDE, LANGUAGE, preprocessor lines, comments) are skipped. False when no dialog was found.
    bool parse(std::string_view text, std::string* error = nullptr);
    // Parses more dialogs and adds them (a dialog of the same name is replaced). Call before any GuiManager::open (the
    // dialogs vector may move). Used for the port's own dialogs (gui_menus.hpp kPortDialogsRc). False when none parsed.
    bool merge(std::string_view text, std::string* error = nullptr);
    // Parses more dialogs and appends each one's controls to the dialog of the same name (its header - position, size,
    // styles, caption, font - is ignored); a dialog not yet in the script is added whole. Same timing rule as merge. Used
    // for controls the port adds to a shipped dialog (gui_menus.hpp kPortDialogExtensionsRc). False when none parsed.
    bool extend(std::string_view text, std::string* error = nullptr);
    std::span<const RcDialog> dialogs() const { return dialogs_; }
    const RcDialog* find(std::string_view name) const;
    size_t control_count() const;
    std::vector<std::string> warnings;  // unknown control keywords, malformed statements

private:
    std::vector<RcDialog> dialogs_;
    std::map<std::string, uint32_t, std::less<>> index_;
};
GuiGadgetType gadget_type_of(const RcControl& c);

// ---- GUIDialogs.xml -------------------------------------------------------------------------------------
struct GuiFontSpec {
    std::string face = "EmpireAtWar-Medium";
    float size = 7.0f;            // points (Size)
    float padding = 0.0f;         // Character_Padding (pixels)
    float stretch = 1.0f;         // Stretch_Factor
    Rgba8 top{195, 171, 2, 255};  // Top_Color
    Rgba8 bottom{195, 171, 2, 255};
    bool emboss = false, outline = false;
};

// Font lookup key of a gadget (GUIDialogs.xml <Fonts><Default> children): Push_Button, List_Box, Combo_Box, Edit_Box,
// IME_Edit_Box, L_Text, R_Text, C_Text ([port]: absent in the data -> Global_Default), "" for the rest.
std::string_view gadget_font_key(GuiGadgetType type, TextAlign align);

class GuiSkin {
public:
    // <GUIDialogs> root. False when it has no <Textures> and no <Fonts>.
    bool parse(const data::XmlNode& root);

    // Texture for a skin key ("Frame_Top_Left", "Button_Left_Mouse_Over", ...): the control's override, else the
    // dialog's, else <Default> [port: that precedence; the data never overrides one key at two levels]. "" = no texture
    // (explicit "none", or the key is not defined anywhere).
    std::string_view texture(std::string_view dialog, std::string_view control, std::string_view key) const;
    bool has_override(std::string_view dialog_or_control) const;
    // Font of a control: <IDD><IDC>, <IDC>, <IDD><gadget key>, <Default><gadget key>, <Default><Global_Default>
    // [port: precedence]. Missing fields of an entry inherit Global_Default [port].
    const GuiFontSpec& font(std::string_view dialog, std::string_view control, GuiGadgetType type, TextAlign align) const;
    const GuiFontSpec& global_font() const { return global_; }
    // <Tooltips>: control id -> MasterTextFile key ("" when none).
    std::string_view tooltip(std::string_view control) const;

    std::string atlas_file = "MT_CommandBar", compressed_atlas_file;  // <Textures File / Compressed_File>
    size_t default_keys() const { return default_.size(); }
    size_t override_sets() const { return overrides_.size(); }
    size_t font_entries() const { return font_entries_; }
    size_t tooltip_count() const { return tooltips_.size(); }

private:
    using KeyMap = std::map<std::string, std::string, std::less<>>;  // upper-case key -> texture ("" = none)
    KeyMap default_;
    std::map<std::string, KeyMap, std::less<>> overrides_;  // upper-case IDD_* / IDC_*
    GuiFontSpec global_;
    std::map<std::string, GuiFontSpec, std::less<>> default_fonts_;                             // upper gadget key
    std::map<std::string, GuiFontSpec, std::less<>> control_fonts_;                             // upper IDC
    std::map<std::string, std::map<std::string, GuiFontSpec, std::less<>>, std::less<>> dialog_fonts_;  // IDD -> key
    std::map<std::string, std::string, std::less<>> tooltips_;
    size_t font_entries_ = 0;
};

// All three files through a FileProvider (VFS rules apply). Missing files are reported in `error`; returns true only
// when the script and the skin loaded (Resource.h is optional: the script uses symbolic ids).
struct GuiData {
    DialogScript script;
    ResourceIds ids;
    GuiSkin skin;
};
bool load_gui_data(const FileProvider& files, GuiData& out, std::string* error = nullptr);

}  // namespace alamo::ui
