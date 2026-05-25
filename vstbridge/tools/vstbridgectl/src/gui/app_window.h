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

#pragma once

#include <functional>
#include <gtk/gtk.h>

#include "../config.h"

struct AppWindow {
    GtkWidget* window = nullptr;

    // Tab: Directories
    GtkWidget* dir_list = nullptr;

    // Tab: Sync
    GtkWidget* sync_force_cb    = nullptr;
    GtkWidget* sync_prune_cb    = nullptr;
    GtkWidget* sync_verbose_cb  = nullptr;
    GtkWidget* sync_noverify_cb = nullptr;
    GtkWidget* sync_btn         = nullptr;
    GtkWidget* sync_spinner     = nullptr;
    GtkWidget* sync_textview    = nullptr;

    // Tab: Status
    GtkWidget* status_btn      = nullptr;
    GtkWidget* status_spinner  = nullptr;
    GtkWidget* status_textview = nullptr;

    // Tab: Settings
    GtkWidget* settings_path_entry   = nullptr;
    GtkWidget* settings_vst2_combo   = nullptr;
    GtkWidget* settings_noverify_sw  = nullptr;
    GtkWidget* settings_vst2_loc_lbl = nullptr;
    GtkWidget* settings_vst3_loc_lbl = nullptr;
    GtkWidget* settings_clap_loc_lbl = nullptr;

    // Tab: Blacklist
    GtkWidget* bl_list = nullptr;

    Config config;
    bool busy = false;

    static AppWindow* create();
    void show_all();

    void run_with_output(GtkWidget* spinner, GtkWidget* btn, GtkWidget* textview,
                         std::function<void()> action_fn,
                         std::function<void()> on_done = {});

    void refresh_dir_list();
    void refresh_bl_list();
    void refresh_settings();

    GtkWidget* build_directories_tab();
    GtkWidget* build_sync_tab();
    GtkWidget* build_status_tab();
    GtkWidget* build_settings_tab();
    GtkWidget* build_blacklist_tab();
};
