#ifdef __linux__
#include "Photino.Dialog.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

enum DialogType {
    OpenFile,
    OpenFolder,
    SaveFile
};

//The parent window of the dialogs. It is used to place the dialog in front of the
//window of the application and to disable the window while the dialog is open.
GtkWindow* GetParentWindow(const Photino* window)
{
    if (!window || !window->_window)
        return NULL;

    return GTK_WINDOW(window->_window);
}

bool IsExistingDirectory(const char* path)
{
    return path != NULL && path[0] != '\0' && g_file_test(path, G_FILE_TEST_IS_DIR);
}

//A filter of the file dialog. The filters are passed as "name|pattern1;pattern2".
struct FileFilter
{
    std::string name;
    std::vector<std::string> patterns;
};

//The filters are parsed without modifying the strings of the caller.
std::vector<FileFilter> ParseFilters(AutoString* filters, int filterCount)
{
    std::vector<FileFilter> result;

    if (!filters || filterCount <= 0)
        return result;

    for (int i = 0; i < filterCount; i++) {
        if (!filters[i])
            continue;

        const std::string value(filters[i]);
        const auto separator = value.find('|');

        if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size())
            continue;

        FileFilter filter;
        filter.name = value.substr(0, separator);

        const std::string patterns = value.substr(separator + 1);
        size_t start = 0;

        while (start < patterns.size())
        {
            const auto end = patterns.find(';', start);
            const std::string pattern = patterns.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);

            if (!pattern.empty())
                filter.patterns.push_back(pattern);

            if (end == std::string::npos)
                break;

            start = end + 1;
        }

        if (!filter.patterns.empty())
            result.push_back(filter);
    }

    return result;
}

//The settings of a file chooser, they are used for the portal request and for the GTK dialog.
struct FileChooserSettings
{
    std::string title;
    GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
    std::string acceptLabel = "_Open";
    std::string currentFolder;
    bool multiple = false;
    std::vector<FileFilter> filters;
};

void AddFiltersToChooser(GtkFileChooser* chooser, const std::vector<FileFilter>& filters)
{
    if (!chooser || filters.empty())
        return;

    for (const auto& filter : filters)
    {
        GtkFileFilter* gtkFilter = gtk_file_filter_new();
        gtk_file_filter_set_name(gtkFilter, filter.name.c_str());

        for (const auto& pattern : filter.patterns)
            gtk_file_filter_add_pattern(gtkFilter, pattern.c_str());

        gtk_file_chooser_add_filter(chooser, gtkFilter);
    }
}

//The desktop portal (org.freedesktop.portal.FileChooser) provides the file dialog of the running
//desktop environment (KDE, GNOME, ...). The request is sent directly instead of using the native
//dialog of GTK, because GTK drops a request which the portal answers with an error without
//closing the dialog, which blocks the application forever. A portal which refuses the request is
//detected here and the GTK dialog is used instead.
class PortalFileChooser
{
public:
    PortalFileChooser(GtkWindow* parentWindow, const FileChooserSettings& settings)
        : _parentWindow(parentWindow), _settings(settings)
    {
    }

    ~PortalFileChooser()
    {
        RemoveGrab();

        if (_connection)
        {
            Unsubscribe();

            if (_ownerSubscription)
                g_dbus_connection_signal_unsubscribe(_connection, _ownerSubscription);

            g_object_unref(_connection);
        }

        if (_loop)
            g_main_loop_unref(_loop);
    }

    PortalFileChooser(const PortalFileChooser&) = delete;
    PortalFileChooser& operator=(const PortalFileChooser&) = delete;

    //Runs the request, returns false when the portal refused it.
    bool Run(std::vector<std::string>& paths)
    {
        _connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);

        if (!_connection)
            return false;

        _loop = g_main_loop_new(nullptr, FALSE);

        //The path of the request is derived from the name of the sender and the token, the portal
        //uses the same path, so the response can be received before the request is answered.
        _requestPath = BuildRequestPath();
        Subscribe();

        //The portal can disappear while the dialog is open, the request is never answered then.
        _ownerSubscription = g_dbus_connection_signal_subscribe(
            _connection,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            "org.freedesktop.portal.Desktop",
            G_DBUS_SIGNAL_FLAGS_NONE,
            OnPortalOwnerChanged,
            this,
            nullptr);

        _parentHandle = GetParentHandle();
        AddGrab();
        SendRequest();

        g_main_loop_run(_loop);

        RemoveGrab();

        if (_failed)
            return false;

        paths = _paths;
        return true;
    }

private:
    //The path of the request, it is built from the name of the sender and the token of the
    //request, the same way the portal builds it.
    std::string BuildRequestPath() const
    {
        const auto* senderName = g_dbus_connection_get_unique_name(_connection);

        if (!senderName)
            return {};

        std::string sender(senderName + 1);
        std::replace(sender.begin(), sender.end(), '.', '_');

        return "/org/freedesktop/portal/desktop/request/" + sender + "/" + _handleToken;
    }

    //The grab keeps the input of the application window while the dialog is open, the same way
    //the native dialog of GTK does it for the portal.
    void AddGrab()
    {
        if (!_parentWindow)
            return;

        _grabWidget = gtk_invisible_new_for_screen(gtk_widget_get_screen(GTK_WIDGET(_parentWindow)));
        gtk_grab_add(_grabWidget);
    }

    void RemoveGrab()
    {
        if (!_grabWidget)
            return;

        gtk_grab_remove(_grabWidget);
        gtk_widget_destroy(_grabWidget);
        _grabWidget = nullptr;
    }

    //The handle of the parent window, the portal uses it to place the dialog in front of it.
    //It is only available on X11, the portal dialog is shown without a parent on Wayland.
    std::string GetParentHandle() const
    {
        if (!_parentWindow || !gtk_widget_get_visible(GTK_WIDGET(_parentWindow)))
            return {};

        auto* gdkWindow = gtk_widget_get_window(GTK_WIDGET(_parentWindow));

        if (!gdkWindow)
            return {};

#ifdef GDK_WINDOWING_X11
        //The window id is only available when the application runs on X11, the dialog of the
        //portal is shown without a parent on Wayland.
        if (GDK_IS_X11_WINDOW(gdkWindow))
            return std::to_string(static_cast<unsigned long>(gdk_x11_window_get_xid(gdkWindow)));
#endif

        return {};
    }

    static void OnRequestReply(GObject* source, GAsyncResult* result, gpointer userData)
    {
        auto* self = static_cast<PortalFileChooser*>(userData);
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

        if (!reply)
        {
            //The portal refused the request, the caller continues with the GTK dialog.
            g_warning("The desktop portal refused the file chooser request: %s", error->message);
            self->_failed = true;
            g_clear_error(&error);
            self->Quit();
            return;
        }

        const char* requestPath = nullptr;
        g_variant_get(reply, "(o)", &requestPath);

        if (requestPath && g_strcmp0(requestPath, self->_requestPath.c_str()) != 0)
        {
            //The portal uses another path for the request, the subscription is moved to it.
            self->Unsubscribe();
            self->_requestPath = requestPath;
            self->Subscribe();
        }

        g_variant_unref(reply);
    }

    static void OnResponse(
        GDBusConnection* connection,
        const gchar* senderName,
        const gchar* objectPath,
        const gchar* interfaceName,
        const gchar* signalName,
        GVariant* parameters,
        gpointer userData)
    {
        auto* self = static_cast<PortalFileChooser*>(userData);
        guint32 response = 1;
        GVariant* results = nullptr;

        g_variant_get(parameters, "(u@a{sv})", &response, &results);

        //Only the response 0 means that the user accepted the dialog.
        if (response == 0 && results)
        {
            auto* uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as"));

            if (uris)
            {
                gsize count = 0;
                const char** uriList = g_variant_get_strv(uris, &count);

                for (gsize i = 0; i < count; i++)
                {
                    auto* path = g_filename_from_uri(uriList[i], nullptr, nullptr);

                    if (path)
                    {
                        //The portal returns folders with a trailing separator.
                        auto length = std::strlen(path);

                        if (length > 1 && path[length - 1] == '/')
                            path[length - 1] = '\0';

                        self->_paths.emplace_back(path);
                        g_free(path);
                    }
                }

                g_free(uriList);
                g_variant_unref(uris);
            }
        }

        if (results)
            g_variant_unref(results);

        self->Quit();
    }

    static void OnClosed(
        GDBusConnection* connection,
        const gchar* senderName,
        const gchar* objectPath,
        const gchar* interfaceName,
        const gchar* signalName,
        GVariant* parameters,
        gpointer userData)
    {
        //The request was closed without a response, the dialog is treated as cancelled.
        static_cast<PortalFileChooser*>(userData)->Quit();
    }

    static void OnPortalOwnerChanged(
        GDBusConnection* connection,
        const gchar* senderName,
        const gchar* objectPath,
        const gchar* interfaceName,
        const gchar* signalName,
        GVariant* parameters,
        gpointer userData)
    {
        const char* name = nullptr;
        const char* oldOwner = nullptr;
        const char* newOwner = nullptr;

        g_variant_get(parameters, "(&s&s&s)", &name, &oldOwner, &newOwner);

        //The portal is gone, the request would never be answered.
        if (name && g_strcmp0(name, "org.freedesktop.portal.Desktop") == 0 && (!newOwner || !newOwner[0]))
            static_cast<PortalFileChooser*>(userData)->Quit();
    }

    void Subscribe()
    {
        _responseSubscription = g_dbus_connection_signal_subscribe(
            _connection,
            "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request",
            "Response",
            _requestPath.c_str(),
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            OnResponse,
            this,
            nullptr);

        _closedSubscription = g_dbus_connection_signal_subscribe(
            _connection,
            "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request",
            "Closed",
            _requestPath.c_str(),
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            OnClosed,
            this,
            nullptr);
    }

    void Unsubscribe()
    {
        if (_responseSubscription)
        {
            g_dbus_connection_signal_unsubscribe(_connection, _responseSubscription);
            _responseSubscription = 0;
        }

        if (_closedSubscription)
        {
            g_dbus_connection_signal_unsubscribe(_connection, _closedSubscription);
            _closedSubscription = 0;
        }
    }

    void SendRequest()
    {
        const char* method = _settings.action == GTK_FILE_CHOOSER_ACTION_SAVE ? "SaveFile" : "OpenFile";

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(_handleToken.c_str()));
        g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(_settings.multiple));
        g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(_settings.action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER));
        g_variant_builder_add(&options, "{sv}", "accept_label", g_variant_new_string(_settings.acceptLabel.c_str()));
        g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(TRUE));

        if (!_settings.currentFolder.empty())
            g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(_settings.currentFolder.c_str()));

        if (!_settings.filters.empty())
            g_variant_builder_add(&options, "{sv}", "filters", BuildFilters());

        g_dbus_connection_call(
            _connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.FileChooser",
            method,
            g_variant_new("(ss@a{sv})", _parentHandle.c_str(), _settings.title.c_str(), g_variant_builder_end(&options)),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            15000,
            nullptr,
            OnRequestReply,
            this);
    }

    //The filters of the portal, each filter is a name and a list of rules, each rule is a glob
    //pattern.
    GVariant* BuildFilters() const
    {
        GVariantBuilder filters;
        g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));

        for (const auto& filter : _settings.filters)
        {
            GVariantBuilder rules;
            g_variant_builder_init(&rules, G_VARIANT_TYPE("a(us)"));

            for (const auto& pattern : filter.patterns)
                g_variant_builder_add(&rules, "(us)", 0u, pattern.c_str());

            g_variant_builder_add(&filters, "(s@a(us))", filter.name.c_str(), g_variant_builder_end(&rules));
        }

        return g_variant_builder_end(&filters);
    }

    void Quit()
    {
        if (_loop)
            g_main_loop_quit(_loop);
    }

    GtkWindow* _parentWindow = nullptr;
    FileChooserSettings _settings;

    GDBusConnection* _connection = nullptr;
    GMainLoop* _loop = nullptr;
    GtkWidget* _grabWidget = nullptr;
    guint _responseSubscription = 0;
    guint _closedSubscription = 0;
    guint _ownerSubscription = 0;

    std::string _parentHandle;
    std::string _requestPath;
    std::string _handleToken = "photinox" + std::to_string(g_random_int());

    std::vector<std::string> _paths;
    bool _failed = false;
};

//The GTK dialog, it is used when the desktop portal does not provide a file chooser.
std::vector<std::string> ShowGtkDialog(const FileChooserSettings& settings, GtkWindow* parentWindow)
{
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        settings.title.c_str(),
        parentWindow,
        settings.action,
        "_Cancel",
        GTK_RESPONSE_CANCEL,
        settings.acceptLabel.c_str(),
        GTK_RESPONSE_ACCEPT,
        nullptr);

    if (!dialog)
        return {};

    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    auto* chooser = GTK_FILE_CHOOSER(dialog);

    if (!settings.currentFolder.empty())
        gtk_file_chooser_set_current_folder(chooser, settings.currentFolder.c_str());

    gtk_file_chooser_set_select_multiple(chooser, settings.multiple ? TRUE : FALSE);

    if (settings.action == GTK_FILE_CHOOSER_ACTION_SAVE)
        gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

    AddFiltersToChooser(chooser, settings.filters);

    std::vector<std::string> result;

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
        if (settings.action == GTK_FILE_CHOOSER_ACTION_SAVE)
        {
            char* path = gtk_file_chooser_get_filename(chooser);

            if (path)
            {
                result.emplace_back(path);
                g_free(path);
            }
        }
        else
        {
            GSList* pathList = gtk_file_chooser_get_filenames(chooser);

            for (GSList* item = pathList; item; item = item->next)
            {
                auto* path = static_cast<char*>(item->data);

                if (path)
                {
                    result.emplace_back(path);
                    g_free(path);
                }
            }

            g_slist_free(pathList);
        }
    }

    gtk_widget_destroy(dialog);

    return result;
}

std::vector<std::string> ShowDialog(
    Photino* window,
    DialogType type,
    AutoString title,
    AutoString defaultPath,
    bool multiSelect,
    AutoString* filters,
    int filterCount)
{
    GtkFileChooserAction action;
    const char* buttonText = NULL;

    switch (type)
    {
    case OpenFile:
        action = GTK_FILE_CHOOSER_ACTION_OPEN;
        buttonText = "_Open";
        break;
    case OpenFolder:
        action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
        buttonText = "_Select";
        break;
    case SaveFile:
        action = GTK_FILE_CHOOSER_ACTION_SAVE;
        buttonText = "_Save";
        break;
    default:
        return {};
    }

    FileChooserSettings settings;
    settings.title = title != NULL ? title : "";
    settings.action = action;
    settings.acceptLabel = buttonText;

    if (IsExistingDirectory(defaultPath))
        settings.currentFolder = defaultPath;

    if (type == OpenFile || type == OpenFolder)
        settings.multiple = multiSelect;

    if (type == OpenFile || type == SaveFile)
        settings.filters = ParseFilters(filters, filterCount);

    GtkWindow* parentWindow = GetParentWindow(window);

    //The dialog of the desktop environment is used when the portal provides it.
    PortalFileChooser portalFileChooser(parentWindow, settings);
    std::vector<std::string> result;

    if (portalFileChooser.Run(result))
        return result;

    //The portal refused the request, so the GTK dialog is used.
    return ShowGtkDialog(settings, parentWindow);
}

//The paths are returned as an array of strings, the count of the paths is returned as well.
AutoString* ToResultArray(const std::vector<std::string>& paths, int* resultCount)
{
    const int count = static_cast<int>(paths.size());
    char** results = new char*[count];

    for (int i = 0; i < count; i++)
        results[i] = g_strdup(paths[i].c_str());

    if (resultCount)
        *resultCount = count;

    return results;
}

PhotinoDialog::PhotinoDialog(Photino* window) : _window(window) {}

PhotinoDialog::~PhotinoDialog() {}

AutoString* PhotinoDialog::ShowOpenFile(AutoString title, AutoString defaultPath, bool multiSelect, AutoString* filters, int filterCount, int* resultCount)
{
    auto paths = ShowDialog(_window, OpenFile, title, defaultPath, multiSelect, filters, filterCount);

    if (paths.empty()) {
        if (resultCount)
            *resultCount = 0;

        return NULL;
    }

    return ToResultArray(paths, resultCount);
}

AutoString* PhotinoDialog::ShowOpenFolder(AutoString title, AutoString defaultPath, bool multiSelect, int* resultCount)
{
    auto paths = ShowDialog(_window, OpenFolder, title, defaultPath, multiSelect, NULL, 0);

    if (paths.empty()) {
        if (resultCount)
            *resultCount = 0;

        return NULL;
    }

    return ToResultArray(paths, resultCount);
}

AutoString PhotinoDialog::ShowSaveFile(AutoString title, AutoString defaultPath, AutoString* filters, int filterCount)
{
    auto paths = ShowDialog(_window, SaveFile, title, defaultPath, false, filters, filterCount);

    if (paths.empty())
        return NULL;

    return g_strdup(paths[0].c_str());
}

DialogResult PhotinoDialog::ShowMessage(AutoString title, AutoString text, DialogButtons buttons, DialogIcon icon)
{
    GtkWidget* dialog;
    GtkMessageType type;

    switch (icon) {
        case DialogIcon::Info:
            type = GTK_MESSAGE_INFO;
            break;
        case DialogIcon::Warning:
            type = GTK_MESSAGE_WARNING;
            break;
        case DialogIcon::Error:
            type = GTK_MESSAGE_ERROR;
            break;
        case DialogIcon::Question:
            type = GTK_MESSAGE_QUESTION;
            break;
        default:
            type = GTK_MESSAGE_OTHER;
            break;
    }

    dialog = gtk_message_dialog_new(GetParentWindow(_window),
                                    static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                    type,
                                    GTK_BUTTONS_NONE,
                                    "%s",
                                    title != NULL ? title : "");
    if (!dialog)
        return DialogResult::Cancel;

    if (text != NULL)
        gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), text);

    if (title != NULL && title[0] != '\0')
        gtk_window_set_title(GTK_WINDOW(dialog), title);

    switch (buttons) {
        case DialogButtons::Ok:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Ok", (gint)DialogResult::Ok);
            break;
        case DialogButtons::OkCancel:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Ok", (gint)DialogResult::Ok);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", (gint)DialogResult::Cancel);
            break;
        case DialogButtons::YesNo:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Yes", (gint)DialogResult::Yes);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_No", (gint)DialogResult::No);
            break;
        case DialogButtons::YesNoCancel:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Yes", (gint)DialogResult::Yes);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_No", (gint)DialogResult::No);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", (gint)DialogResult::Cancel);
            break;
        case DialogButtons::RetryCancel:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Retry", (gint)DialogResult::Retry);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", (gint)DialogResult::Cancel);
            break;
        case DialogButtons::AbortRetryIgnore:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Abort", (gint)DialogResult::Abort);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Retry", (gint)DialogResult::Retry);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Ignore", (gint)DialogResult::Ignore);
            break;
        default:
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Ok", (gint)DialogResult::Ok);
            break;
    }

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    switch (result) {
        case GTK_RESPONSE_CLOSE:
            return DialogResult::Cancel;
        case (gint)DialogResult::Ok:
            return DialogResult::Ok;
        case (gint)DialogResult::Yes:
            return DialogResult::Yes;
        case (gint)DialogResult::No:
            return DialogResult::No;
        case (gint)DialogResult::Cancel:
            return DialogResult::Cancel;
        case (gint)DialogResult::Abort:
            return DialogResult::Abort;
        case (gint)DialogResult::Retry:
            return DialogResult::Retry;
        case (gint)DialogResult::Ignore:
            return DialogResult::Ignore;
        default:
            return DialogResult::Cancel;
    }
}
#endif
