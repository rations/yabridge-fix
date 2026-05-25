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

#include <gtk/gtk.h>

#include "config.h"
#include "gui/app_window.h"

int main(int argc, char** argv) {
    // Append ~/.local/share/vstbridge to PATH so vstbridge-host.exe can be found
    try {
        auto data_home = vstbridge_data_home();
        std::string path_env = data_home.string();
        if (const char* existing = std::getenv("PATH"))
            path_env = std::string(existing) + ":" + path_env;
        setenv("PATH", path_env.c_str(), 1);
    } catch (...) {
    }

    gtk_init(&argc, &argv);

    AppWindow* win = AppWindow::create();
    win->show_all();

    gtk_main();

    delete win;
    return 0;
}
