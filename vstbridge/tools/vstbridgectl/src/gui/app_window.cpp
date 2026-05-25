// vstbridge: a Wine VST/CLAP plugin bridge
// Copyright (C) 2020-2024 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "app_window.h"

#include <unistd.h>

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include "../actions.h"
#include "../actions/blacklist.h"
#include "../config.h"

namespace fs = std::filesystem;

// ─── Output capture ───────────────────────────────────────────────────────────

struct RunCtx {
    AppWindow*            win;
    GtkWidget*            textview;
    GtkTextMark*          end_mark;
    GtkWidget*            spinner;
    GtkWidget*            btn;
    int                   saved_stdout;
    int                   saved_stderr;
    int                   read_fd;
    guint                 watch_id;
    std::function<void()> on_done;
    std::thread           worker;
    std::string           error_msg;
};

static void buf_append(RunCtx* ctx, const char* data, ssize_t len) {
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->textview));
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(buf, &it);
    gtk_text_buffer_insert(buf, &it, data, (gint)len);
    gtk_text_buffer_get_end_iter(buf, &it);
    gtk_text_buffer_move_mark(buf, ctx->end_mark, &it);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ctx->textview), ctx->end_mark,
                                 0.0, FALSE, 0.0, 1.0);
}

static gboolean io_read_cb(GIOChannel*, GIOCondition, gpointer data) {
    auto* ctx = static_cast<RunCtx*>(data);
    char tmp[4096];
    ssize_t n = ::read(ctx->read_fd, tmp, sizeof(tmp));
    if (n > 0)
        buf_append(ctx, tmp, n);
    return TRUE;
}

static gboolean worker_done_cb(gpointer data) {
    auto* ctx = static_cast<RunCtx*>(data);

    // Restore stdout/stderr — this removes all write-end references to the pipe
    dup2(ctx->saved_stdout, STDOUT_FILENO);
    dup2(ctx->saved_stderr, STDERR_FILENO);
    close(ctx->saved_stdout);
    close(ctx->saved_stderr);

    // Remove the GIO watch and drain any remaining pipe data
    g_source_remove(ctx->watch_id);
    char tmp[4096];
    ssize_t n;
    while ((n = ::read(ctx->read_fd, tmp, sizeof(tmp))) > 0)
        buf_append(ctx, tmp, n);
    close(ctx->read_fd);

    ctx->worker.join();

    if (!ctx->error_msg.empty()) {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(ctx->win->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Error: %s", ctx->error_msg.c_str());
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }

    gtk_spinner_stop(GTK_SPINNER(ctx->spinner));
    gtk_widget_hide(ctx->spinner);
    gtk_widget_set_sensitive(ctx->btn, TRUE);
    ctx->win->busy = false;

    if (ctx->on_done)
        ctx->on_done();

    delete ctx;
    return FALSE;
}

void AppWindow::run_with_output(GtkWidget* spinner, GtkWidget* btn, GtkWidget* textview,
                                std::function<void()> action_fn,
                                std::function<void()> on_done) {
    if (busy) return;
    busy = true;

    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gtk_text_buffer_set_text(buf, "", 0);
    gtk_widget_show(spinner);
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_sensitive(btn, FALSE);

    GtkTextIter end_it;
    gtk_text_buffer_get_end_iter(buf, &end_it);
    GtkTextMark* mark = gtk_text_buffer_create_mark(buf, nullptr, &end_it, FALSE);

    int pipefd[2];
    pipe(pipefd);

    int sout = dup(STDOUT_FILENO);
    int serr = dup(STDERR_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // Create a GIO channel for the watch; unref immediately since the watch
    // holds its own reference.
    GIOChannel* chan = g_io_channel_unix_new(pipefd[0]);
    g_io_channel_set_encoding(chan, nullptr, nullptr);
    g_io_channel_set_buffered(chan, FALSE);

    auto* ctx         = new RunCtx{};
    ctx->win          = this;
    ctx->textview     = textview;
    ctx->end_mark     = mark;
    ctx->spinner      = spinner;
    ctx->btn          = btn;
    ctx->saved_stdout = sout;
    ctx->saved_stderr = serr;
    ctx->read_fd      = pipefd[0];
    ctx->on_done      = std::move(on_done);
    ctx->watch_id     = g_io_add_watch(chan, G_IO_IN, io_read_cb, ctx);
    g_io_channel_unref(chan);

    ctx->worker = std::thread([ctx, fn = std::move(action_fn)]() mutable {
        try {
            fn();
        } catch (const std::exception& e) {
            ctx->error_msg = e.what();
        }
        fflush(stdout);
        fflush(stderr);
        g_idle_add(worker_done_cb, ctx);
    });
}

// ─── Shared helpers ───────────────────────────────────────────────────────────

// Replace the leading $HOME prefix with ~ for display purposes.
static std::string pretty_path(const fs::path& p) {
    const char* home = std::getenv("HOME");
    if (!home) return p.string();
    std::string s = p.string();
    std::string h = home;
    if (s.size() >= h.size() && s.compare(0, h.size(), h) == 0)
        return "~" + s.substr(h.size());
    return s;
}

static void show_error(GtkWidget* parent, const std::string& msg) {
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(parent),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "Error: %s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static GtkWidget* make_output_textview() {
    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    return tv;
}

static GtkWidget* wrap_in_scroll(GtkWidget* child) {
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
}

// ─── List refresh ─────────────────────────────────────────────────────────────

void AppWindow::refresh_dir_list() {
    GList* kids = gtk_container_get_children(GTK_CONTAINER(dir_list));
    for (GList* l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    for (const auto& dir : config.plugin_dirs) {
        GtkWidget* lbl = gtk_label_new(dir.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_end(lbl, 8);
        gtk_widget_set_margin_top(lbl, 5);
        gtk_widget_set_margin_bottom(lbl, 5);
        GtkWidget* row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(dir_list), row, -1);
    }
    gtk_widget_show_all(dir_list);
}

void AppWindow::refresh_bl_list() {
    GList* kids = gtk_container_get_children(GTK_CONTAINER(bl_list));
    for (GList* l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    for (const auto& p : config.blacklist) {
        GtkWidget* lbl = gtk_label_new(p.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_end(lbl, 8);
        gtk_widget_set_margin_top(lbl, 5);
        gtk_widget_set_margin_bottom(lbl, 5);
        GtkWidget* row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(bl_list), row, -1);
    }
    gtk_widget_show_all(bl_list);
}

void AppWindow::refresh_settings() {
    gtk_entry_set_text(GTK_ENTRY(settings_path_entry),
                       config.vstbridge_home ? config.vstbridge_home->c_str() : "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(settings_vst2_combo),
                             config.vst2_location == Vst2InstallationLocation::Inline ? 1 : 0);
    gtk_switch_set_active(GTK_SWITCH(settings_noverify_sw), config.no_verify);

    // Update install location labels
    if (config.vst2_location == Vst2InstallationLocation::Inline)
        gtk_label_set_text(GTK_LABEL(settings_vst2_loc_lbl), "inline (next to each .dll)");
    else
        gtk_label_set_text(GTK_LABEL(settings_vst2_loc_lbl),
                           pretty_path(vstbridge_vst2_home()).c_str());

    gtk_label_set_text(GTK_LABEL(settings_vst3_loc_lbl),
                       pretty_path(vstbridge_vst3_home()).c_str());
    gtk_label_set_text(GTK_LABEL(settings_clap_loc_lbl),
                       pretty_path(vstbridge_clap_home()).c_str());
}

// ─── Signal callbacks ─────────────────────────────────────────────────────────

static void on_dir_add(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        "Select VST2, VST3 or CLAP Plugin Directory", GTK_WINDOW(win->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        try {
            add_directory(win->config, fs::path(path));
            win->config = Config::read();
            win->refresh_dir_list();
        } catch (const std::exception& e) {
            show_error(win->window, e.what());
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_dir_rm(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(win->dir_list));
    if (!row) return;
    GtkWidget* lbl = gtk_bin_get_child(GTK_BIN(row));
    fs::path path(gtk_label_get_text(GTK_LABEL(lbl)));
    try {
        remove_directory(win->config, path);
        win->config = Config::read();
        win->refresh_dir_list();
    } catch (const std::exception& e) {
        show_error(win->window, e.what());
    }
}

static void on_sync_clicked(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    SyncOptions opts{};
    opts.force     = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->sync_force_cb));
    opts.prune     = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->sync_prune_cb));
    opts.verbose   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->sync_verbose_cb));
    opts.no_verify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->sync_noverify_cb));
    win->run_with_output(
        win->sync_spinner, win->sync_btn, win->sync_textview,
        [win, opts]() mutable { do_sync(win->config, opts); },
        [win]() { win->config = Config::read(); });
}

static void on_status_refresh(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    win->run_with_output(
        win->status_spinner, win->status_btn, win->status_textview,
        [win]() { show_status(win->config); });
}

static void on_settings_browse(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        "Select vstbridge Directory", GTK_WINDOW(win->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(win->settings_path_entry), path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_settings_auto(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    SetOptions opts{};
    opts.path_auto = true;
    try {
        set_settings(win->config, opts);
        win->config = Config::read();
        win->refresh_settings();
    } catch (const std::exception& e) {
        show_error(win->window, e.what());
    }
}

static void on_settings_apply(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    SetOptions opts{};
    const char* txt = gtk_entry_get_text(GTK_ENTRY(win->settings_path_entry));
    if (txt && *txt)
        opts.path = fs::path(txt);
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(win->settings_vst2_combo));
    opts.vst2_location = (idx == 1) ? Vst2Location::Inline : Vst2Location::Centralized;
    opts.no_verify     = gtk_switch_get_active(GTK_SWITCH(win->settings_noverify_sw));
    try {
        set_settings(win->config, opts);
        win->config = Config::read();
        win->refresh_settings();
    } catch (const std::exception& e) {
        show_error(win->window, e.what());
    }
}

static void on_bl_add(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        "Select Path to Blacklist", GTK_WINDOW(win->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        try {
            blacklist::add_path(win->config, fs::path(path));
            win->config = Config::read();
            win->refresh_bl_list();
        } catch (const std::exception& e) {
            show_error(win->window, e.what());
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_bl_rm(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(win->bl_list));
    if (!row) return;
    GtkWidget* lbl = gtk_bin_get_child(GTK_BIN(row));
    fs::path path(gtk_label_get_text(GTK_LABEL(lbl)));
    try {
        blacklist::remove_path(win->config, path);
        win->config = Config::read();
        win->refresh_bl_list();
    } catch (const std::exception& e) {
        show_error(win->window, e.what());
    }
}

static void on_bl_clear(GtkButton*, gpointer data) {
    auto* win = static_cast<AppWindow*>(data);
    GtkWidget* confirm = gtk_message_dialog_new(
        GTK_WINDOW(win->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Clear the entire blacklist?");
    gint resp = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (resp == GTK_RESPONSE_YES) {
        try {
            blacklist::clear(win->config);
            win->config = Config::read();
            win->refresh_bl_list();
        } catch (const std::exception& e) {
            show_error(win->window, e.what());
        }
    }
}

// ─── Tab builders ─────────────────────────────────────────────────────────────

GtkWidget* AppWindow::build_directories_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    GtkWidget* hint = gtk_label_new(
        "Directories to search for Windows VST2, VST3 and CLAP plugins.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_set_margin_bottom(hint, 6);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    dir_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dir_list), GTK_SELECTION_SINGLE);
    gtk_box_pack_start(GTK_BOX(box), wrap_in_scroll(dir_list), TRUE, TRUE, 0);

    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(btn_row, 6);

    GtkWidget* add_btn = gtk_button_new_with_label("+ Add Directory");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_dir_add), this);
    gtk_box_pack_start(GTK_BOX(btn_row), add_btn, FALSE, FALSE, 0);

    GtkWidget* rm_btn = gtk_button_new_with_label("− Remove");
    g_signal_connect(rm_btn, "clicked", G_CALLBACK(on_dir_rm), this);
    gtk_box_pack_start(GTK_BOX(btn_row), rm_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), btn_row, FALSE, FALSE, 0);
    return box;
}

GtkWidget* AppWindow::build_sync_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    // Options row
    GtkWidget* opts_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    sync_force_cb    = gtk_check_button_new_with_label("Force");
    sync_prune_cb    = gtk_check_button_new_with_label("Prune");
    sync_verbose_cb  = gtk_check_button_new_with_label("Verbose");
    sync_noverify_cb = gtk_check_button_new_with_label("Skip verify");
    gtk_box_pack_start(GTK_BOX(opts_row), sync_force_cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts_row), sync_prune_cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts_row), sync_verbose_cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts_row), sync_noverify_cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), opts_row, FALSE, FALSE, 0);

    // Action row
    GtkWidget* action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    sync_btn = gtk_button_new_with_label("Sync Now");
    g_signal_connect(sync_btn, "clicked", G_CALLBACK(on_sync_clicked), this);
    gtk_box_pack_start(GTK_BOX(action_row), sync_btn, FALSE, FALSE, 0);
    sync_spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(sync_spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(action_row), sync_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), action_row, FALSE, FALSE, 0);

    // Output
    sync_textview = make_output_textview();
    gtk_box_pack_start(GTK_BOX(box), wrap_in_scroll(sync_textview), TRUE, TRUE, 0);

    return box;
}

GtkWidget* AppWindow::build_status_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    GtkWidget* action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    status_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(status_btn, "clicked", G_CALLBACK(on_status_refresh), this);
    gtk_box_pack_start(GTK_BOX(action_row), status_btn, FALSE, FALSE, 0);
    status_spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(status_spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(action_row), status_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), action_row, FALSE, FALSE, 0);

    status_textview = make_output_textview();
    gtk_box_pack_start(GTK_BOX(box), wrap_in_scroll(status_textview), TRUE, TRUE, 0);

    return box;
}

GtkWidget* AppWindow::build_settings_tab() {
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 14);
    gtk_widget_set_margin_bottom(grid, 14);
    gtk_widget_set_margin_start(grid, 14);
    gtk_widget_set_margin_end(grid, 14);

    int row = 0;

    // vstbridge path
    GtkWidget* path_lbl = gtk_label_new("vstbridge path:");
    gtk_label_set_xalign(GTK_LABEL(path_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), path_lbl, 0, row, 1, 1);

    settings_path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(settings_path_entry), "(auto-detected)");
    gtk_widget_set_hexpand(settings_path_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), settings_path_entry, 1, row, 1, 1);

    GtkWidget* browse_btn = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_settings_browse), this);
    gtk_grid_attach(GTK_GRID(grid), browse_btn, 2, row, 1, 1);

    GtkWidget* auto_btn = gtk_button_new_with_label("Auto");
    gtk_widget_set_tooltip_text(auto_btn, "Search standard locations for vstbridge files");
    g_signal_connect(auto_btn, "clicked", G_CALLBACK(on_settings_auto), this);
    gtk_grid_attach(GTK_GRID(grid), auto_btn, 3, row, 1, 1);
    ++row;

    // VST2 location
    GtkWidget* vst2_lbl = gtk_label_new("VST2 location:");
    gtk_label_set_xalign(GTK_LABEL(vst2_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), vst2_lbl, 0, row, 1, 1);

    settings_vst2_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(settings_vst2_combo),
                                   "Centralized (~/.vst/vstbridge/)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(settings_vst2_combo),
                                   "Inline (next to .dll)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(settings_vst2_combo), 0);
    gtk_widget_set_tooltip_text(settings_vst2_combo,
        "Centralized: all VST2 bridges go to ~/.vst/vstbridge/\n"
        "Inline: bridge is placed next to each Windows .dll file");
    gtk_grid_attach(GTK_GRID(grid), settings_vst2_combo, 1, row, 2, 1);
    ++row;

    // Skip verify
    GtkWidget* nv_lbl = gtk_label_new("Skip verify:");
    gtk_label_set_xalign(GTK_LABEL(nv_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), nv_lbl, 0, row, 1, 1);

    settings_noverify_sw = gtk_switch_new();
    gtk_widget_set_halign(settings_noverify_sw, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(settings_noverify_sw,
        "Skip the Wine/vstbridge compatibility check that runs after every sync");
    gtk_grid_attach(GTK_GRID(grid), settings_noverify_sw, 1, row, 1, 1);
    ++row;

    // Apply
    GtkWidget* apply_btn = gtk_button_new_with_label("Apply");
    gtk_widget_set_halign(apply_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(apply_btn, 6);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_settings_apply), this);
    gtk_grid_attach(GTK_GRID(grid), apply_btn, 0, row, 4, 1);
    ++row;

    // ── Install locations (read-only) ────────────────────────────────────────

    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 8);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, row, 4, 1);
    ++row;

    GtkWidget* loc_heading = gtk_label_new("Install locations");
    gtk_label_set_xalign(GTK_LABEL(loc_heading), 0.0f);
    gtk_widget_set_margin_bottom(loc_heading, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(loc_heading), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), loc_heading, 0, row, 4, 1);
    ++row;

    auto make_loc_row = [&](const char* label_text, GtkWidget** label_out) {
        GtkWidget* lbl = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget* val = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(val), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(val, TRUE);
        gtk_grid_attach(GTK_GRID(grid), val, 1, row, 3, 1);

        *label_out = val;
        ++row;
    };

    make_loc_row("VST2:", &settings_vst2_loc_lbl);
    make_loc_row("VST3:", &settings_vst3_loc_lbl);
    make_loc_row("CLAP:", &settings_clap_loc_lbl);

    return grid;
}

GtkWidget* AppWindow::build_blacklist_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    bl_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(bl_list), GTK_SELECTION_SINGLE);
    gtk_box_pack_start(GTK_BOX(box), wrap_in_scroll(bl_list), TRUE, TRUE, 0);

    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(btn_row, 6);

    GtkWidget* add_btn = gtk_button_new_with_label("+ Add");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_bl_add), this);
    gtk_box_pack_start(GTK_BOX(btn_row), add_btn, FALSE, FALSE, 0);

    GtkWidget* rm_btn = gtk_button_new_with_label("− Remove");
    g_signal_connect(rm_btn, "clicked", G_CALLBACK(on_bl_rm), this);
    gtk_box_pack_start(GTK_BOX(btn_row), rm_btn, FALSE, FALSE, 0);

    GtkWidget* clear_btn = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_bl_clear), this);
    gtk_box_pack_start(GTK_BOX(btn_row), clear_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), btn_row, FALSE, FALSE, 0);
    return box;
}

// ─── AppWindow::create / show_all ────────────────────────────────────────────

AppWindow* AppWindow::create() {
    auto* win = new AppWindow{};

    try {
        win->config = Config::read();
    } catch (...) {
    }

    win->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win->window), "vstbridgectl");
    gtk_window_set_default_size(GTK_WINDOW(win->window), 720, 520);
    g_signal_connect(win->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget* notebook = gtk_notebook_new();

    auto add_tab = [&](const char* label, GtkWidget* content) {
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), content, gtk_label_new(label));
    };

    add_tab("Directories", win->build_directories_tab());
    add_tab("Sync",        win->build_sync_tab());
    add_tab("Status",      win->build_status_tab());
    add_tab("Settings",    win->build_settings_tab());
    add_tab("Blacklist",   win->build_blacklist_tab());

    gtk_container_add(GTK_CONTAINER(win->window), notebook);

    win->refresh_dir_list();
    win->refresh_bl_list();
    win->refresh_settings();

    return win;
}

void AppWindow::show_all() {
    gtk_widget_show_all(window);
    // Spinners start hidden; show_all would have made them visible
    gtk_widget_hide(sync_spinner);
    gtk_widget_hide(status_spinner);
}
