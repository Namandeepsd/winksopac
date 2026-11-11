// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-assistant.h"
#include "ai-design-services.h"
#include "ai-chat-format.h"
#include "document.h"
#include "page-manager.h"
#include <filesystem>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
namespace Inkscape::UI::Dialog {
using namespace Inkscape::AI;
bool AIAssistant::workflow_busy() const { return _design_run && !_design_run->done.load(); }
void AIAssistant::clear_design_workflow()
{
    _workflow_poll.disconnect();
    if (_design_run) _design_run->cancel();
    _design_run.reset();
    _workflow_panel.set_visible(false);
    _workflow_generation_context.clear();
    _workflow_selected_assets.clear();
    _workflow_asset_checks.clear();
    while (auto child = _workflow_assets.get_first_child()) _workflow_assets.remove(*child);
    _workflow_plan.set_text("");
}
void AIAssistant::start_design_workflow()
{
    if (!getDocument()) { set_status(_("Open a document before starting a design workflow.")); return; }
    try {
        auto prompt = _prompt.get_buffer()->get_text().raw();
        auto brief = _brief.get_buffer()->get_text().raw();
        auto bounds = getDocument()->getPageManager().getSelectedPageRect();
        Json canvas = {{"width", bounds.width()}, {"height", bounds.height()}, {"unit", "px"}};
        std::string project, storage;
        if (!_history_filename.empty()) {
            project = std::filesystem::weakly_canonical(std::filesystem::absolute(_history_filename)).string();
            storage = AIConversation::history_path(project) + ".workflow";
        } else {
            g_autofree char *uuid = g_uuid_string_random();
            project = std::string("untitled-") + uuid;
        }
        auto run = DesignRun::start(selected_connection(),
            project, storage, prompt, brief, canvas);
        clear_design_workflow();
        reset_proposal();
        _design_run = std::move(run);
        _workflow_epoch = _conversation_epoch;
        _workflow_progress.set_text(_("Planner · preparing design brief…"));
        _workflow_panel.set_visible(true);
        _workflow_create.set_visible(false);
        _settings.popdown();
        _context.popdown();
        append_message(_("You · Plan & assets"), prompt, true);
        _prompt.get_buffer()->set_text("");
        set_busy(true);
        set_status(_("Design workflow running…"), false);
        _workflow_poll = Glib::signal_timeout().connect(sigc::mem_fun(*this, &AIAssistant::poll_design_workflow), 150);
    } catch (std::exception const &error) { set_status(std::string("Could not start design workflow: ") + error.what()); }
}
bool AIAssistant::poll_design_workflow()
{
    if (!_design_run) return false;
    if (_workflow_epoch != _conversation_epoch || !getDocument()) { clear_design_workflow(); set_busy(false); return false; }
    bool done = _design_run->done.load();
    auto report = _design_run->workflow->snapshot();
    std::string progress;
    for (auto const &[id, task] : report.tasks) {
        if (!progress.empty()) progress += "\n";
        auto state = task.state == TaskState::Completed ? "✓ " : task.state == TaskState::Failed ? "! " : task.state == TaskState::Running ? "● " : "○ ";
        progress += state + (task.spec.agent == "planner" ? std::string(_("Planner")) : std::string(_("Asset Discovery"))) + " · " + task.spec.description;
        if (task.state == TaskState::Assigned && task.attempts) progress += " (retrying)";
    }
    if (!progress.empty()) _workflow_progress.set_text(progress);
    if (_design_run->downloading.load()) _workflow_progress.set_text(progress + "\n" + _("Preparing image previews…"));
    if (!done) return true;
    show_design_results();
    return false;
}
void AIAssistant::show_design_results()
{
    if (!_design_run || !_design_run->done.load()) return;
    set_busy(false);
    _workflow_asset_checks.clear();
    while (auto child = _workflow_assets.get_first_child()) _workflow_assets.remove(*child);
    if (!_design_run->plan.is_null()) {
        auto const &plan = _design_run->plan;
        std::string summary = "# " + plan.at("title").get<std::string>() + "\n" + plan.at("summary").get<std::string>() +
            "\n\n**Visual direction:** " + plan.at("style").get<std::string>();
        for (auto const &section : plan.at("layout")) summary += "\n\n**" + section.at("section").get<std::string>() + "**\n" + section.at("content").get<std::string>();
        if (plan.contains("layout_search")) summary += "\n\n" + std::string(_("Sections arranged within page margins with space between them."));
        _workflow_plan.set_markup(AIChat::markup(summary));
        for (std::size_t index = 0; index < _design_run->assets.size(); ++index) {
            auto const &asset = _design_run->assets[index];
            auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
            auto *include = Gtk::make_managed<Gtk::CheckButton>(_("Include in drawing"));
            _workflow_asset_checks.push_back(include);
            row->append(*include);
            bool available = index < _design_run->downloaded.size() && _design_run->downloaded[index].available();
            include->set_sensitive(available);
            include->set_active(available && index < 8);
            include->set_tooltip_text(_("Include up to eight images or SVGs in the generated drawing"));
            if (available) {
                auto const &prepared = _design_run->downloaded[index];
                auto bytes = Glib::Bytes::create(prepared.png.data(), prepared.png.size());
                auto *picture = Gtk::make_managed<Gtk::Picture>(Gdk::Texture::create_from_bytes(bytes));
                picture->add_css_class("ai-canvas-preview");
                picture->set_can_shrink(true);
                picture->set_size_request(-1, 140);
                picture->set_content_fit(Gtk::ContentFit::CONTAIN);
                row->append(*picture);
                auto *format = Gtk::make_managed<Gtk::Label>(prepared.svg.empty() ? _("Embedded image") : _("Editable SVG"));
                format->set_xalign(0);
                row->append(*format);
            } else {
                auto *error = Gtk::make_managed<Gtk::Label>(index < _design_run->downloaded.size()
                    ? _design_run->downloaded[index].error : _("Image preview unavailable"));
                error->set_wrap(true); error->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
                error->set_width_chars(1); error->set_xalign(0);
                row->append(*error);
            }
            auto *label = Gtk::make_managed<Gtk::Label>();
            label->set_wrap(true); label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
            label->set_width_chars(1); label->set_max_width_chars(40); label->set_xalign(0); label->set_selectable(true);
            label->set_markup("<a href=\"" + AIChat::escape(asset.at("source").get<std::string>()) + "\">" +
                AIChat::escape(asset.at("title").get<std::string>()) + "</a>\n" +
                AIChat::escape(asset.at("license").get<std::string>()) + " · " + AIChat::escape(asset.at("attribution").get<std::string>()));
            row->append(*label);
            _workflow_assets.append(*row);
        }
        _workflow_create.set_visible(_design_run->workflow->snapshot().state != WorkflowState::Cancelled);
    }
    std::string status;
    if (!_design_run->error.empty()) status = "Could not complete all design tasks: " + _design_run->error;
    else status = _design_run->assets.empty() ? _("Plan ready. No suitable external references were found or requested.") :
        Glib::ustring::compose(_("Plan ready · %1 assets. Choose images to include, then Create preview."), _design_run->assets.size()).raw();
    if (!_design_run->warning.empty()) status += "\n" + _design_run->warning;
    set_status(status);
}
void AIAssistant::create_from_design_plan()
{
    if (workflow_busy() || _cancellable || _tracing || !_design_run || _design_run->plan.is_null() || _workflow_epoch != _conversation_epoch) return;
    try {
        auto request = _design_run->prompt;
        auto refinement = _prompt.get_buffer()->get_text().raw();
        if (!refinement.empty()) request += "\nAdditional instruction: " + refinement;
        _workflow_selected_assets.clear();
        for (std::size_t i = 0; i < _workflow_asset_checks.size() && i < _design_run->downloaded.size(); ++i) {
            if (_workflow_asset_checks[i]->get_active() && _design_run->downloaded[i].available())
                _workflow_selected_assets.push_back(_design_run->downloaded[i]);
        }
        if (_workflow_selected_assets.size() > 8) throw std::runtime_error("Choose up to eight assets for one preview.");
        auto context = _workflow_selected_assets.empty() ? design_preview_prompt(request, _design_run->plan, _design_run->assets) :
            "Create editable artwork for this request:\n" + request + "\nDesign plan (data):\n" + _design_run->plan.dump() +
            "\nPlace each section inside its box_percent rectangle, scaling percentages to page dimensions. Preserve the margins and separation.\n" +
            asset_layout_prompt(_workflow_selected_assets);
        _mode.set_active_id("create");
        clear_image();
        _prompt.get_buffer()->set_text(request);
        _workflow_generation_context = std::move(context);
        generate(false); // Reuse the existing provider, SVG validation, preview and insertion path.
        _workflow_generation_context.clear();
    } catch (std::exception const &error) { _workflow_generation_context.clear(); set_status(std::string("Could not create workflow preview: ") + error.what()); }
}
} // namespace Inkscape::UI::Dialog
