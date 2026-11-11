// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_ASSISTANT_H
#define INKSCAPE_UI_DIALOG_AI_ASSISTANT_H

#include <memory>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtkmm/comboboxtext.h>
#include <gtkmm/expander.h>
#include <gtkmm/frame.h>
#include <gtkmm/picture.h>
#include <gtkmm/image.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/overlay.h>
#include <gdkmm/texture.h>
#include "async/channel.h"
#include <gtkmm/spinner.h>
#include <gtkmm/spinbutton.h>

#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textview.h>

#include <gio/giotypes.h>

typedef struct _SoupSession SoupSession;
class SPShape;
namespace Inkscape::AI { class OutputRegistry; }

#include "ui/dialog/dialog-base.h"
#include "ai-conversation.h"
#include "ai-connection.h"
#include "ai-design-assets.h"
#include <map>
#include "ui/view/svg-view-widget.h"

namespace Inkscape::UI::Dialog {

class SelectionEdit;
struct DesignRun;

class AIAssistant final : public DialogBase
{
public:
    AIAssistant();
    ~AIAssistant() override;
    void select_region(SPShape *outline);
    // Main-thread bridge for a completed workflow. Preview only; Insert retains
    // the existing document validation, import and undo transaction.
    void preview_workflow_output(Inkscape::AI::OutputRegistry const &, std::string const &id);

private:
    friend struct AIAssistantUITest; // Native-widget layout and interaction regression harness.
    void generate(bool edit_selection = false);
    void start_design_workflow();
    bool poll_design_workflow();
    void show_design_results();
    bool workflow_busy() const;
    void clear_design_workflow();
    void create_from_design_plan();
    std::shared_ptr<DesignRun> _design_run;
    sigc::connection _workflow_poll;
    std::uint64_t _workflow_epoch = 0;
    std::string _workflow_generation_context;
    std::vector<DesignAsset> _workflow_selected_assets;
    std::vector<Gtk::CheckButton *> _workflow_asset_checks;
    std::string _asset_preview_svg; // Exact locally assembled bytes, never model-provided.
    Gtk::Box _workflow_panel{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label _workflow_progress, _workflow_plan;
    Gtk::Box _workflow_assets{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box _workflow_buttons{Gtk::Orientation::HORIZONTAL, 4};
    Gtk::Button _workflow_create, _workflow_dismiss;
    bool fit_page_now(bool selection = false);
    Gtk::Button _fit_page;
    void paste_image();
    void choose_image();
    void attach_image(Glib::RefPtr<Gdk::Texture> const &texture);
    void clear_image();
    void trace_image();
    void preview_artwork(std::string const &svg);
    Async::Channel::Dest _trace_channel;
    bool _tracing = false;
    std::uint64_t _paste_serial = 0;
    Glib::RefPtr<Gdk::Texture> _reference_image;
    std::string _image_url;
    Gtk::Button _copy_prompt, _paste_prompt, _remove_image;
    Gtk::Box _attachment{Gtk::Orientation::VERTICAL, 4};
    Gtk::Box _attachment_row{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Box _attachment_controls{Gtk::Orientation::VERTICAL, 4};
    Gtk::Picture _attachment_preview;
    Gtk::Label _attachment_note;
    Gtk::ComboBoxText _image_method;
    void apply_edit();
    void discard_edit();
    void set_busy(bool busy);
    void insert_svg();

    void set_status(Glib::ustring const &text, bool record = true);
    void append_message(Glib::ustring const &role, Glib::ustring const &text, bool user = false);
    void reset_proposal();
    void reset_chat();
    void save_conversation();
    void restore_conversation();
    bool _restoring = false;
    SPDocument *_history_document = nullptr;
    std::string _history_filename;
    AIConversation _conversation;
    std::map<SPDocument *, AIConversation> _unsaved;
    std::vector<sigc::connection> _document_connections;
    sigc::connection _filename_connection, _save_idle, _rename_idle;
    Gtk::Label _context_size;
    void remember_result();
    void record_proposal(std::string const &svg);
    bool _has_recorded_proposal = false;
    void update_selection();
    std::string edit_mode() const;
    void documentReplaced() override;
    void selectionChanged(Inkscape::Selection *) override;
    bool scroll_to_latest();

    Gtk::Box _header{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Label _heading;
    Gtk::Image _brand;
    Gtk::Button _new_chat;
    Gtk::MenuButton _settings_button, _context_button, _attach_button;
    Gtk::Popover _settings, _context, _attach_menu;
    Gtk::Box _attach_options{Gtk::Orientation::VERTICAL, 2};
    Gtk::Button _choose_image;
    Gtk::Box _settings_body{Gtk::Orientation::VERTICAL, 6};
    Gtk::ScrolledWindow _settings_scroll;
    Gtk::Entry _endpoint;
    void setup_connections();
    void change_connection();
    void save_connection();
    void refresh_connection_status();
    void test_connection();
    AIConnection selected_connection() const;
    Gtk::ComboBoxText _connector;
    Gtk::CheckButton _connection_images;
    Gtk::CheckButton _completion_token_limit;
    Gtk::Entry _cli_executable, _cli_arguments;
    Gtk::SpinButton _connection_timeout;
    Gtk::Label _connection_help, _connection_status;
    Gtk::Button _connection_test, _connection_login;
    std::map<std::string, AIConnection> _connection_profiles;
    std::string _active_connector;
    bool _loading_connection = false;
    Gtk::Entry _model;
    Gtk::Entry _key;
    Gtk::Box _context_body{Gtk::Orientation::VERTICAL, 6};
    Gtk::ScrolledWindow _brief_scroll;
    Gtk::TextView _brief;

    Gtk::ScrolledWindow _conversation_scroll;
    Gtk::Overlay _conversation_overlay;
    Gtk::Button _latest;
    bool _stick_to_bottom = true;
    Gtk::Label *_operation_text = nullptr;
    Gtk::Image *_operation_icon = nullptr;
    Gtk::Box _thread{Gtk::Orientation::VERTICAL, 12};
    Gtk::Label _welcome;
    Gtk::Box _messages{Gtk::Orientation::VERTICAL, 12};
    Gtk::Box _result{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label _result_title;
    Gtk::Label _target_title;
    Inkscape::UI::View::SVGViewWidget _target_preview;
    Inkscape::UI::View::SVGViewWidget _preview;
    Gtk::Expander _source;
    Gtk::ScrolledWindow _scroll;
    Gtk::TextView _output;
    Gtk::Box _result_actions{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button _insert;
    Gtk::Button _apply;
    Gtk::Button _discard;

    Gtk::Frame _composer;
    Gtk::Box _composer_body{Gtk::Orientation::VERTICAL, 4};
    Gtk::Overlay _prompt_overlay;
    Gtk::Label _placeholder, _context_line;
    Gtk::Box _footer{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Box _history_controls{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Button _undo, _redo;
    Gtk::Image _status_icon;
    Gtk::ComboBoxText _mode;
    Gtk::Label _selection_hint;
    Gtk::ScrolledWindow _prompt_scroll;
    Gtk::TextView _prompt;
    Gtk::Box _send_bar{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Spinner _spinner;
    Gtk::Button _generate;
    Gtk::Button _cancel;
    Gtk::Label _status;

    std::shared_ptr<SelectionEdit> _edit;
    std::shared_ptr<SelectionEdit> _captured_region;
    std::unique_ptr<SPDocument> _generated_preview;
    std::vector<std::pair<std::string, std::string>> _accepted_context;
    std::string _pending_prompt;
    std::uint64_t _conversation_epoch = 0;
    sigc::connection _scroll_idle, _scroll_value_connection, _scroll_bounds_connection;

    GCancellable *_cancellable = nullptr;

    std::shared_ptr<int> _alive = std::make_shared<int>(0);
};

}

#endif
