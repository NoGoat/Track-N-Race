#include <gtk/gtk.h>

#include "Attributions.h"
#include "MinimalController.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <unistd.h>

namespace {

constexpr const char* kApplicationId = "in.nogoat.tracknrace";

struct AppState {
    GSettings* settings{};
    std::unique_ptr<MinimalController> controller;
    GtkWidget* window{};
    GtkWidget* folderEntry{};
    GtkWidget* protocolDropDown{};
    GtkWidget* addressEntry{};
    GtkWidget* portSpin{};
    GtkWidget* circuitValue{};
    GtkWidget* sessionValue{};
    GtkWidget* attributionWindow{};
    GtkWidget* attributionLicenseHeading{};
    GtkWidget* attributionLicenseView{};
};

struct SessionUpdate {
    AppState* state{};
    std::string circuit;
    std::string session;
};

void showError(AppState& state, const std::string& error) {
    GtkWidget* dialog = gtk_message_dialog_new(
        state.window ? GTK_WINDOW(state.window) : nullptr,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), "Track N Race Minimal Recorder");
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog* dialog, int, gpointer) {
        gtk_window_destroy(GTK_WINDOW(dialog));
    }), nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}

GSettings* createSettings() {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source
        ? g_settings_schema_source_lookup(source, kApplicationId, TRUE) : nullptr;

    if (!schema) {
        char executable[4096]{};
        const ssize_t count = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (count > 0) {
            executable[count] = '\0';
            const std::filesystem::path exeDir = std::filesystem::path(executable).parent_path();
            const std::filesystem::path candidates[] = {
                exeDir / "schemas",
                exeDir.parent_path() / "share" / "glib-2.0" / "schemas",
            };
            for (const auto& candidate : candidates) {
                GError* error = nullptr;
                GSettingsSchemaSource* local = g_settings_schema_source_new_from_directory(
                    candidate.c_str(), source, FALSE, &error);
                if (local) {
                    schema = g_settings_schema_source_lookup(local, kApplicationId, FALSE);
                    g_settings_schema_source_unref(local);
                    if (schema) break;
                }
                if (error) g_error_free(error);
            }
        }
    }

    if (!schema) return nullptr;
    GSettings* settings = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);
    return settings;
}

AppSettings loadSettings(GSettings* settings) {
    AppSettings result;
    gchar* folder = g_settings_get_string(settings, "output-folder");
    gchar* address = g_settings_get_string(settings, "bind-address");
    gchar* protocol = g_settings_get_string(settings, "protocol");
    result.outputFolder = folder ? folder : "";
    result.bindAddress = address && *address ? address : "0.0.0.0";
    const guint port = g_settings_get_uint(settings, "port");
    result.port = port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 20777;
    result.protocol = tnrp::overrideFromString(protocol ? protocol : "auto");
    g_free(folder);
    g_free(address);
    g_free(protocol);
    return result;
}

GtkWidget* makeLeftLabel(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

void attachRow(GtkGrid* grid, int row, const char* labelText, GtkWidget* value) {
    GtkWidget* label = makeLeftLabel(labelText);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 2, 1);
}

gboolean applySessionUpdate(gpointer data) {
    std::unique_ptr<SessionUpdate> update(static_cast<SessionUpdate*>(data));
    if (update->state->circuitValue && update->state->sessionValue) {
        gtk_label_set_text(GTK_LABEL(update->state->circuitValue), update->circuit.c_str());
        gtk_label_set_text(GTK_LABEL(update->state->sessionValue), update->session.c_str());
    }
    return G_SOURCE_REMOVE;
}

void chooseFolder(AppState& state) {
    GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
        "Select recording folder", GTK_WINDOW(state.window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Select", "Cancel");

    g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* dialog,
                                                           int response, gpointer data) {
        auto& state = *static_cast<AppState*>(data);
        if (response == GTK_RESPONSE_ACCEPT) {
            GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
            if (file) {
                char* path = g_file_get_path(file);
                if (path) {
                    std::string error;
                    if (state.controller->setOutputFolder(path, error)) {
                        gtk_editable_set_text(GTK_EDITABLE(state.folderEntry), path);
                        g_settings_set_string(state.settings, "output-folder", path);
                    } else {
                        showError(state, error);
                    }
                    g_free(path);
                }
                g_object_unref(file);
            }
        }
        g_object_unref(dialog);
    }), &state);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

void applyNetwork(AppState& state) {
    const char* address = gtk_editable_get_text(GTK_EDITABLE(state.addressEntry));
    const int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state.portSpin));
    std::string error;
    if (!state.controller->applyNetwork(address ? address : "",
                                        static_cast<uint16_t>(port), error)) {
        showError(state, error);
        const AppSettings& current = state.controller->settings();
        gtk_editable_set_text(GTK_EDITABLE(state.addressEntry), current.bindAddress.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state.portSpin), current.port);
        return;
    }
    g_settings_set_string(state.settings, "bind-address", address);
    g_settings_set_uint(state.settings, "port", static_cast<guint>(port));
}

void showGtkAttributionLicense(AppState& state, size_t index) {
    const auto items = minimalAppAttributions();
    if (index >= items.size() || !state.attributionLicenseView) return;
    const Attribution& item = items[index];
    gchar* metadata = g_markup_printf_escaped(
        "<b>%s  %s</b>  -  %s  -  %s  -  <a href=\"%s\">%s</a>",
        item.name.data(), item.version.data(), item.license.data(),
        item.copyright.data(), item.website.data(), item.website.data());
    gtk_label_set_markup(GTK_LABEL(state.attributionLicenseHeading), metadata);
    g_free(metadata);

    const std::string_view text = item.licenseText;
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(state.attributionLicenseView)),
        text.data(), static_cast<gint>(text.size()));
}

void showAttributions(AppState& state) {
    if (state.attributionWindow) {
        gtk_window_present(GTK_WINDOW(state.attributionWindow));
        return;
    }

    GtkApplication* application = gtk_window_get_application(GTK_WINDOW(state.window));
    GtkWidget* window = gtk_application_window_new(application);
    state.attributionWindow = window;
    gtk_window_set_title(GTK_WINDOW(window),
                         "Attributions - Track N Race Minimal Recorder");
    gtk_window_set_icon_name(GTK_WINDOW(window), "track-n-race-minimal");
    gtk_window_set_default_size(GTK_WINDOW(window), 805, 584);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(state.window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "close-request",
                     G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        static_cast<AppState*>(data)->attributionWindow = nullptr;
        static_cast<AppState*>(data)->attributionLicenseHeading = nullptr;
        static_cast<AppState*>(data)->attributionLicenseView = nullptr;
        return FALSE;
    }), &state);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 14);
    gtk_window_set_child(GTK_WINDOW(window), content);

    GtkWidget* heading = gtk_label_new("ATTRIBUTION");
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content), heading);

    GtkWidget* description = gtk_label_new(
        "Track N Race is built with these open-source components. Thank you to their authors.");
    gtk_widget_add_css_class(description, "dim-label");
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_box_append(GTK_BOX(content), description);

    GtkWidget* listScroller = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(listScroller, TRUE);
    gtk_widget_set_size_request(listScroller, -1, 250);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(listScroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(content), listScroller);

    GtkWidget* list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroller), list);

    size_t index = 0;
    for (const Attribution& item : minimalAppAttributions()) {
        GtkWidget* row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "attribution-index",
                          GUINT_TO_POINTER(static_cast<guint>(index + 1)));
        GtkWidget* rowContent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(rowContent, 10);
        gtk_widget_set_margin_end(rowContent, 10);
        gtk_widget_set_margin_top(rowContent, 8);
        gtk_widget_set_margin_bottom(rowContent, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rowContent);

        GtkWidget* name = gtk_label_new(item.name.data());
        gtk_widget_add_css_class(name, "heading");
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(rowContent), name);

        GtkWidget* version = gtk_label_new(item.version.data());
        gtk_widget_add_css_class(version, "dim-label");
        gtk_box_append(GTK_BOX(rowContent), version);
        GtkWidget* license = gtk_label_new(item.license.data());
        gtk_widget_add_css_class(license, "dim-label");
        gtk_box_append(GTK_BOX(rowContent), license);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
        ++index;
    }

    g_signal_connect(list, "row-selected",
                     G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
        if (!row) return;
        const guint stored = GPOINTER_TO_UINT(
            g_object_get_data(G_OBJECT(row), "attribution-index"));
        if (stored > 0) showGtkAttributionLicense(
            *static_cast<AppState*>(data), static_cast<size_t>(stored - 1));
    }), &state);

    state.attributionLicenseHeading = gtk_label_new("");
    gtk_widget_add_css_class(state.attributionLicenseHeading, "heading");
    gtk_widget_set_halign(state.attributionLicenseHeading, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(state.attributionLicenseHeading),
                            PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(state.attributionLicenseHeading), TRUE);
    gtk_box_append(GTK_BOX(content), state.attributionLicenseHeading);

    GtkWidget* licenseScroller = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(licenseScroller, TRUE);
    gtk_widget_set_vexpand(licenseScroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(licenseScroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(content), licenseScroller);

    GtkWidget* textView = gtk_text_view_new();
    state.attributionLicenseView = textView;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textView), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textView), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textView), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textView), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textView), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textView), 10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(licenseScroller), textView);
    GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(list), first);

    GtkWidget* closeButton = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(closeButton, GTK_ALIGN_END);
    g_signal_connect(closeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<AppState*>(data);
        GtkWidget* window = state.attributionWindow;
        state.attributionWindow = nullptr;
        state.attributionLicenseHeading = nullptr;
        state.attributionLicenseView = nullptr;
        if (window) gtk_window_destroy(GTK_WINDOW(window));
    }), &state);
    gtk_box_append(GTK_BOX(content), closeButton);

    gtk_window_present(GTK_WINDOW(window));
}

void activate(GtkApplication* application, gpointer data) {
    auto& state = *static_cast<AppState*>(data);
    if (state.window) {
        gtk_window_present(GTK_WINDOW(state.window));
        return;
    }

    state.window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state.window), "Track N Race Minimal Recorder");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 620, 330);
    gtk_window_set_resizable(GTK_WINDOW(state.window), TRUE);
    g_signal_connect(state.window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto& state = *static_cast<AppState*>(data);
        state.controller->setSessionCallback({});
        state.window = nullptr;
        state.folderEntry = nullptr;
        state.protocolDropDown = nullptr;
        state.addressEntry = nullptr;
        state.portSpin = nullptr;
        state.circuitValue = nullptr;
        state.sessionValue = nullptr;
        state.attributionWindow = nullptr;
        state.attributionLicenseHeading = nullptr;
        state.attributionLicenseView = nullptr;
        return FALSE;
    }), &state);

    GtkWidget* gridWidget = gtk_grid_new();
    GtkGrid* grid = GTK_GRID(gridWidget);
    gtk_grid_set_row_spacing(grid, 12);
    gtk_grid_set_column_spacing(grid, 12);
    gtk_widget_set_margin_start(gridWidget, 20);
    gtk_widget_set_margin_end(gridWidget, 20);
    gtk_widget_set_margin_top(gridWidget, 20);
    gtk_widget_set_margin_bottom(gridWidget, 20);
    gtk_window_set_child(GTK_WINDOW(state.window), gridWidget);

    state.folderEntry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(state.folderEntry), FALSE);
    gtk_widget_set_hexpand(state.folderEntry, TRUE);
    GtkWidget* browse = gtk_button_new_with_label("Browse...");
    gtk_grid_attach(grid, makeLeftLabel("Recording folder"), 0, 0, 1, 1);
    gtk_grid_attach(grid, state.folderEntry, 1, 0, 1, 1);
    gtk_grid_attach(grid, browse, 2, 0, 1, 1);
    g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        chooseFolder(*static_cast<AppState*>(data));
    }), &state);

    const char* protocols[] = {"Auto", "F1 24", "F1 25", "F1 26", nullptr};
    state.protocolDropDown = gtk_drop_down_new_from_strings(protocols);
    gtk_widget_set_hexpand(state.protocolDropDown, TRUE);
    attachRow(grid, 1, "Protocol override", state.protocolDropDown);

    state.addressEntry = gtk_entry_new();
    gtk_widget_set_hexpand(state.addressEntry, TRUE);
    attachRow(grid, 2, "IPv4 bind address", state.addressEntry);

    state.portSpin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(state.portSpin), TRUE);
    gtk_widget_set_hexpand(state.portSpin, TRUE);
    GtkWidget* applyButton = gtk_button_new_with_label("Apply network");
    gtk_grid_attach(grid, makeLeftLabel("UDP port"), 0, 3, 1, 1);
    gtk_grid_attach(grid, state.portSpin, 1, 3, 1, 1);
    gtk_grid_attach(grid, applyButton, 2, 3, 1, 1);
    g_signal_connect(applyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        applyNetwork(*static_cast<AppState*>(data));
    }), &state);

    GtkWidget* spacer = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(spacer, 8);
    gtk_widget_set_margin_bottom(spacer, 8);
    gtk_grid_attach(grid, spacer, 0, 4, 3, 1);

    state.circuitValue = makeLeftLabel("Unavailable");
    state.sessionValue = makeLeftLabel("Unavailable");
    attachRow(grid, 5, "Circuit name", state.circuitValue);
    GtkWidget* sessionLabel = makeLeftLabel("Session");
    gtk_widget_set_valign(sessionLabel, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, sessionLabel, 0, 6, 1, 1);
    gtk_grid_attach(grid, state.sessionValue, 1, 6, 1, 1);
    GtkWidget* attributionButton = gtk_button_new_with_label("Attributions...");
    gtk_grid_attach(grid, attributionButton, 2, 6, 1, 1);
    g_signal_connect(attributionButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        showAttributions(*static_cast<AppState*>(data));
    }), &state);

    const AppSettings& current = state.controller->settings();
    gtk_editable_set_text(GTK_EDITABLE(state.folderEntry), current.outputFolder.c_str());
    gtk_editable_set_text(GTK_EDITABLE(state.addressEntry), current.bindAddress.c_str());
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state.portSpin), current.port);
    const guint protocolIndex = current.protocol == tnrp::Override::F1_24 ? 1
                              : current.protocol == tnrp::Override::F1_25 ? 2
                              : current.protocol == tnrp::Override::F1_26 ? 3 : 0;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state.protocolDropDown), protocolIndex);
    g_signal_connect(state.protocolDropDown, "notify::selected",
                     G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
        auto& state = *static_cast<AppState*>(data);
        const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
        const tnrp::Override protocol = selected == 1 ? tnrp::Override::F1_24
                                       : selected == 2 ? tnrp::Override::F1_25
                                       : selected == 3 ? tnrp::Override::F1_26
                                                       : tnrp::Override::Auto;
        state.controller->setProtocol(protocol);
        g_settings_set_string(state.settings, "protocol", tnrp::toString(protocol));
    }), &state);

    state.controller->setSessionCallback([&state](std::string circuit, std::string session) {
        auto* update = new SessionUpdate{&state, std::move(circuit), std::move(session)};
        g_main_context_invoke(nullptr, applySessionUpdate, update);
    });

    gtk_window_present(GTK_WINDOW(state.window));

    std::string error;
    if (!state.controller->start(error)) showError(state, error);
}

} // namespace

int main(int argc, char** argv) {
    // AppImages cannot assume the host has dconf. GLib's keyfile backend keeps
    // GSettings persistent without a session service or bundled daemon.
    if (g_getenv("APPIMAGE")) g_setenv("GSETTINGS_BACKEND", "keyfile", FALSE);

    GSettings* settings = createSettings();
    if (!settings) {
        g_printerr("GSettings schema %s was not found. Install or compile the bundled schema.\n",
                   kApplicationId);
        return 1;
    }

    AppState state;
    state.settings = settings;
    state.controller = std::make_unique<MinimalController>(loadSettings(settings));

#if GLIB_CHECK_VERSION(2, 74, 0)
    constexpr GApplicationFlags applicationFlags = G_APPLICATION_DEFAULT_FLAGS;
#else
    constexpr GApplicationFlags applicationFlags = G_APPLICATION_FLAGS_NONE;
#endif
    GtkApplication* application = gtk_application_new(kApplicationId, applicationFlags);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    const int result = g_application_run(G_APPLICATION(application), argc, argv);

    state.controller->setSessionCallback({});
    state.controller.reset();
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    g_settings_sync();
    g_object_unref(application);
    g_object_unref(settings);
    return result;
}
