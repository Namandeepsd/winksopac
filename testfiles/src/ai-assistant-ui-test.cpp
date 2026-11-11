// SPDX-License-Identifier: GPL-2.0-or-later
// Native GTK smoke/snapshot test. No provider requests and no user documents.
#include <iostream>
#include <memory>
#include <stdexcept>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <gtkmm/settings.h>
#include <gtkmm/icontheme.h>
#include <glibmm/main.h>
#include <gdk/gdkkeysyms.h>
#include "inkscape.h"
#include "inkgc/gc-core.h"
#include "util/statics.h"
#include "ui/dialog/ai-assistant.h"
#include "ui/dialog/ai-chat-format.h"
#include "ui/dialog/ai-design-services.h"

static void require(bool condition, char const *message) {
    if (!condition) throw std::runtime_error(message);
}
namespace Inkscape::UI::Dialog {
struct AIAssistantUITest {
    static void connectors(AIAssistant &panel) {
        require(panel._connector.get_model()->children().size() == connection_presets().size(), "Connector picker is incomplete");
        panel._connector.set_active_id("ollama");
        require(panel._endpoint.get_visible() && !panel._cli_executable.get_visible(), "Local API fields are incorrect");
        panel._model.set_text("fixture-vision-model");
        panel._key.set_text("session-only-fixture-key");
        panel._connection_images.set_active(true);
        panel._connector.set_active_id("claude");
        require(!panel._endpoint.get_visible() && panel._cli_executable.get_visible() && panel._connection_login.get_visible(), "Terminal sign-in setup is missing");
        require(panel._connection_images.get_active() && !panel._connection_images.get_sensitive(), "Claude image capability lost");
        panel._connector.set_active_id("bridge");
        require(panel._cli_arguments.get_visible(), "Custom CLI bridge configuration is missing");
        panel._connector.set_active_id("ollama");
        auto connection = panel.selected_connection();
        require(connection.model == "fixture-vision-model" && connection.key == "session-only-fixture-key" && connection.images, "Switching connectors lost the configured model or session key");
        {
            AIAssistant reopened;
            auto restored = reopened.selected_connection();
            require(restored.provider == "ollama" && restored.model == "fixture-vision-model" && restored.images,
                    "New panel did not restore the selected connector and model");
            require(restored.key.empty(), "API key was persisted beyond the original panel session");
        }
        panel._settings.popup();
    }
    static void workflow(AIAssistant &panel) {
        using namespace Inkscape::AI;
        panel.reset_chat();
        panel._restoring = true;
        panel._mode.set_active_id("workflow");
        panel.update_selection();
        require(panel._attach_button.get_sensitive() && !panel._choose_image.get_sensitive(), "Plan must allow clipboard actions while disabling image attachments");
        auto run = std::make_shared<DesignRun>();
        run->outputs = std::make_shared<OutputRegistry>();
        run->workflow = std::make_shared<Orchestrator>(std::make_shared<AgentRegistry>(), std::make_shared<ContextManager>("ui-test"), run->outputs, 3);
        run->plan = {{"title", "Solar energy poster"}, {"summary", "Explain how sunlight becomes electricity."}, {"style", "Clear diagrams and restrained colors"},
            {"layout", Json::array({{{"section", "How it works"}, {"content", "Use the supplied measurements and an editable cell diagram."}}})}};
        run->assets = Json::array({{{"title", "File:Photovoltaic cell diagram with an intentionally long descriptive title.svg"},
            {"source", "https://commons.wikimedia.org/wiki/File:Solar_cell.svg"}, {"license", "CC BY-SA 4.0"}, {"attribution", "Example credit & contributor"},
            {"download", "https://upload.wikimedia.org/wikipedia/commons/a/ab/Test.svg"}, {"mime", "image/svg+xml"}}});
        auto bus = std::make_shared<MessageBus>(); bus->register_tasks({"preview"});
        std::stop_source stop;
        ExecutionContext execution("preview", 1, bus, stop.get_token(), Clock::now() + std::chrono::seconds(10));
        run->downloaded.push_back(prepare_design_asset(run->assets[0], "asset-1", [](auto const &, auto &) {
            return "<svg xmlns='http://www.w3.org/2000/svg' width='240' height='140'><rect width='240' height='140' fill='#d8e5f3'/><rect x='50' y='30' width='140' height='80' fill='#2e528c'/><path d='M50 70H190M120 30V110' stroke='white'/><circle cx='215' cy='20' r='12' fill='#f4c35a'/></svg>";
        }, execution));
        require(run->downloaded[0].available(), "Synthetic asset preview failed");
        run->done = true;
        panel._design_run = std::move(run);
        panel._workflow_progress.set_text("✓ Planner · Plan the poster\n✓ Asset Discovery · Find a cell diagram");
        panel._workflow_panel.set_visible(true);
        panel.show_design_results();
        require(panel._workflow_create.get_visible() && panel._workflow_create.get_sensitive(), "Completed plan cannot create a preview");
        require(children(panel._workflow_assets) == 1, "Source link missing");
        require(panel._workflow_asset_checks[0]->get_active(), "Downloaded asset is not selectable");
        panel.show_design_results();
        require(children(panel._workflow_assets) == 1, "Repeated rendering duplicated source links");
        panel._restoring = false;
    }
    static void connector_snapshot(AIAssistant &panel, Gtk::Window &window, std::string const &path) {
        snapshot(panel._settings, window, path);
        panel._settings.popdown();
    }
    static unsigned children(Gtk::Widget &widget) {
        unsigned n=0;
        for (auto child=widget.get_first_child(); child; child=child->get_next_sibling()) ++n;
        return n;
    }
    static void populate(AIAssistant &panel) {
        panel.reset_chat();
        require(panel._mode.get_model()->children().size() == 4, "Main menu must have four operations");
        panel._conversation.mode = "geometry";
        panel.restore_conversation();
        require(panel._mode.get_active_id() == "edit", "Legacy geometry conversation did not migrate to Edit");
        panel._conversation.mode = "region";
        panel.restore_conversation();
        require(panel._mode.get_active_id() == "edit", "Legacy region conversation did not migrate to Edit");
        panel.reset_chat();
        panel._mode.set_active_id("create");
        panel._restoring = true;
        panel.append_message("You · Create", "Create a quiet mountain landscape with a warm sunset and a winding path.", true);
        panel.set_status("Created an editable landscape with **layered mountains** and a soft, warm palette.");
        panel.set_status("Edit applied. The landscape is ready.");
        require(children(panel._messages)==2, "Operation status was duplicated");
        panel.append_message("You · Edit objects", "Make the sun pink and keep the mountains unchanged.", true);
        panel.set_status("Only the **sun's fill** changes.\n- Mountains and path stay unchanged\n- Original shapes remain editable");
        require(children(panel._messages)==4, "Conversation feedback was not compact");
        panel.preview_artwork("<svg xmlns='http://www.w3.org/2000/svg' width='400' height='240' viewBox='0 0 400 240'>"
            "<rect width='400' height='240' fill='#503b63'/><path d='M0 130Q80 80 180 110T400 90V240H0Z' fill='#b76b78'/>"
            "<circle cx='290' cy='65' r='29' fill='#ffc2d5'/><path d='M0 190L105 70L230 210L315 105L400 190V240H0Z' fill='#d99a81'/>"
            "<path d='M105 70L145 145L108 125L75 140Z' fill='#f4c6a2'/><path d='M0 205Q110 145 240 200T400 185V240H0Z' fill='#47655b'/>"
            "<path d='M140 240Q230 200 175 185Q270 200 210 240Z' fill='#e0b890'/></svg>");
        panel._prompt.get_buffer()->set_text("test");
        require(!panel._placeholder.get_visible(), "Composer placeholder overlays entered text");
        panel._prompt.get_buffer()->set_text("");
        require(panel._placeholder.get_visible(), "Composer placeholder is missing");
        panel._restoring = false;
        panel._stick_to_bottom = true;
    }
    static void check_layout(AIAssistant &panel, int width) {
        if (panel.get_width() > width + 2) {
            std::cerr << "Actual width: " << panel.get_width() << ", header: " << panel._header.get_width()
                      << ", composer: " << panel._composer.get_width() << '\n';
        }
        require(panel.get_width() <= width+2, "Sidebar expands beyond the requested width");
        require(panel._composer.get_height() < 180, "Empty composer consumes excessive height");
        require(panel._prompt_scroll.get_height() <= 112, "Prompt editor does not respect its height cap");
        require(panel._conversation_scroll.get_width() <= panel.get_width(), "Conversation overflows horizontally");
    }
    static void keyboard(AIAssistant &panel) {
        panel._prompt.grab_focus();
        if (!panel._prompt.has_focus()) return; // Some compositors deny focus to test windows.
        auto model=gtk_widget_observe_controllers(GTK_WIDGET(panel._composer.gobj()));
        bool checked=false;
        for(guint i=0;i<g_list_model_get_n_items(model);++i) {
            auto object=g_list_model_get_item(model,i);
            if(GTK_IS_EVENT_CONTROLLER_KEY(object)) {
                gboolean handled=false;
                g_signal_emit_by_name(object,"key-pressed",GDK_KEY_Return,0,GDK_SHIFT_MASK,&handled);
                require(!handled,"Shift+Enter must remain a newline");
                // Empty prompt: exercise routing without a provider request.
                g_signal_emit_by_name(object,"key-pressed",GDK_KEY_Return,0,static_cast<GdkModifierType>(0),&handled);
                require(handled,"Enter did not submit from the composer");
                checked=true;
            }
            g_object_unref(object);
        }
        g_object_unref(model);
        require(checked,"Composer keyboard controller missing");
        panel._settings.popdown();
    }
    static void attach_and_expand(AIAssistant &panel) {
        panel._prompt.get_buffer()->set_text("line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9");
        auto pixels = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 24, 24);
        pixels->fill(0x6b86aaff);
        panel.attach_image(Gdk::Texture::create_for_pixbuf(pixels));
    }
    static void check_expanded(AIAssistant &panel, int width) {
        require(panel.get_width() <= width + 2, "Attachment expands the sidebar horizontally");
        require(panel._prompt_scroll.get_height() <= 112, "Long prompt exceeds height cap");
        require(panel._prompt_scroll.get_vadjustment()->get_upper() > panel._prompt_scroll.get_height(), "Long prompt cannot be scrolled");
        require(panel._composer.get_height() < panel.get_height() - 48, "Expanded composer hides the conversation");
        panel.set_busy(true);
        require(panel._cancel.get_visible() && !panel._generate.get_visible(), "Busy state does not expose Stop");
        require(!panel._settings_button.get_sensitive(), "Settings remain editable during a request");
        panel.set_busy(false);
        require(panel._generate.get_visible() && !panel._cancel.get_visible(), "Send was not restored after request");
        panel.clear_image();
        panel._prompt.get_buffer()->set_text("");
    }
    static void snapshot(Gtk::Widget &panel, Gtk::Window &window, std::string const &path) {
        auto paintable=gtk_widget_paintable_new(GTK_WIDGET(panel.gobj()));
        auto snapshot=gtk_snapshot_new();
        gdk_paintable_snapshot(GDK_PAINTABLE(paintable),snapshot,panel.get_width(),panel.get_height());
        auto node=gtk_snapshot_free_to_node(snapshot);
        require(node,"Empty widget snapshot");
        auto renderer=gtk_native_get_renderer(GTK_NATIVE(window.gobj()));
        graphene_rect_t rect;
        graphene_rect_init(&rect,0,0,panel.get_width(),panel.get_height());
        auto texture=gsk_renderer_render_texture(renderer,node,&rect);
        require(texture && gdk_texture_save_to_png(texture,path.c_str()),"Could not save widget snapshot");
        g_object_unref(texture);
        gsk_render_node_unref(node);
        g_object_unref(paintable);
    }
    static void scroll_test(AIAssistant &panel) {
        panel._restoring=true;
        for(int i=0;i<8;++i) {
            panel.append_message("You", "A previous design iteration with a short prompt.",true);
            panel.set_status("Edit applied. Kept the original vector geometry.");
        }
        panel._restoring=false;
    }
    static void hold_scroll(AIAssistant &panel) {
        auto adj=panel._conversation_scroll.get_vadjustment();
        adj->set_value(0);
        require(!panel._stick_to_bottom,"Reader position was not detected");
        panel.set_status("Updated operation details while reading earlier messages.");
    }
    static void check_scroll(AIAssistant &panel) {
        require(panel._conversation_scroll.get_vadjustment()->get_value()<2,"Incoming feedback jumped away from reading position");
        require(panel._latest.get_visible(),"Jump-to-latest control is missing");
    }
};
}
int main(int argc,char **argv) {
    int width=argc>1?std::stoi(argv[1]):320, height=argc>2?std::stoi(argv[2]):720;
    bool light=argc>3 && std::string(argv[3])=="light";
    std::string output=argc>4?argv[4]:"/tmp/ai-sidebar.png";
    Inkscape::Util::Statics statics;
    auto app=Gtk::Application::create("org.inkscape.AiSidebarReview",Gio::Application::Flags::NON_UNIQUE);
    Inkscape::GC::init();
    Inkscape::Application::create(false);
    std::unique_ptr<Gtk::Window> window;
    std::unique_ptr<Inkscape::UI::Dialog::AIAssistant> panel;
    int status=0, phase=0;
    using Test=Inkscape::UI::Dialog::AIAssistantUITest;
    app->signal_activate().connect([&] {
        Gtk::Settings::get_default()->property_gtk_application_prefer_dark_theme()=!light;
        Gtk::IconTheme::get_for_display(Gdk::Display::get_default())->add_search_path("share/icons");
        window=std::make_unique<Gtk::Window>();
        window->set_title("AI Assistant UI review");
        window->set_default_size(width,height);
        window->set_resizable(false);
        panel=std::make_unique<Inkscape::UI::Dialog::AIAssistant>();
        window->set_child(*panel);
        app->add_window(*window);
        window->present();
        Glib::signal_timeout().connect([&] {
            try {
                if(phase==0) { Test::keyboard(*panel); Test::populate(*panel); }
                if(phase==1) { Test::check_layout(*panel,width); Test::snapshot(*panel,*window,output); Test::scroll_test(*panel); }
                if(phase==2) Test::hold_scroll(*panel);
                if(phase==3) { Test::check_scroll(*panel); Test::attach_and_expand(*panel); }
                if(phase==4) {
                    Test::check_expanded(*panel, width);
                    Test::workflow(*panel);
                }
                if(phase==5) {
                    Test::check_layout(*panel, width);
                    Test::snapshot(*panel, *window, output + ".workflow.png");
                    std::cout<<"PASS: "<<width<<"x"<<height<<" "<<(light?"light":"dark")<<", composer, feedback, keyboard, scroll and workflow results. Snapshot: "<<output<<std::endl;
                }
                if (phase==6) Test::connectors(*panel);
                if (phase==7) {
                    Test::connector_snapshot(*panel, *window, output + ".connections.png");
                    std::cout << "PASS: connector setup, terminal sign-in controls, image capabilities and per-connector settings" << std::endl;
                    app->quit(); return false;
                }
                ++phase; return true;
            } catch(std::exception const &e) {
                std::cerr<<"FAIL: "<<e.what()<<std::endl; status=1; app->quit(); return false;
            }
        },600);
    });
    app->run(1,argv);
    if(window) window->unset_child();
    panel.reset();
    window.reset();
    return status;
}
