# Copyright (C) 2026
# VST3 Bridge - Wine VST3 Host Bridge
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or (at your option) any later version.

# @file meson.build
# @brief Meson build configuration for VST3 Bridge

project('vst3bridge', 'cpp',
    version : '0.1.0',
    meson_version : '>=0.56',
    default_options : [
        'warning_level=3',
        'cpp_std=c++17',
        'buildtype=release',
        'b_pie=false',
        'b_lto=true',
    ]
)

# Build options
with_bitbridge = get_option('bitbridge')

# Detect architecture
host_arch = host_machine.cpu_family()
is_64bit = host_arch == 'x86_64'

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

common_cpp_args = [
    '-fvisibility=hidden',
    '-fvisibility-inlines-hidden',
    '-fno-strict-aliasing',
    '-msse2',
]

if get_option('buildtype').contains('debug')
    common_cpp_args += ['-DDEBUG', '-D_DEBUG']
else
    common_cpp_args += ['-DNDEBUG']
endif

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

inc_common  = include_directories('src/common', 'third_party/vst3sdk')
inc_native  = include_directories('src/native')
inc_wine    = include_directories('src/wine-host')

# ---------------------------------------------------------------------------
# Common sources (shared between native and Wine builds)
# ---------------------------------------------------------------------------

common_sources = files(
    'src/common/socket.cpp',
    'src/common/shared_memory.cpp',
    'src/common/gui_events.cpp',
    'src/common/stream.cpp',
    'src/common/protocol.cpp',
)

# ---------------------------------------------------------------------------
# Native Linux plugin library
# ---------------------------------------------------------------------------

native_sources = files(
    'src/native/vst3_entry.cpp',
    'src/native/vst3_plugin.cpp',
    'src/native/plugin_factory.cpp',
    'src/native/plugin_proxy.cpp',
    'src/native/x11_window.cpp',
    'src/native/x11_capture.cpp',
    'src/native/socket_client.cpp',
    'src/native/audio_shm.cpp',
    'src/native/gui_handler.cpp',
    'src/native/ipc_client.cpp',
)

native_cpp_args = common_cpp_args + [
    '-DBUILDING_NATIVE',
    '-DSMTG_OS_LINUX',
]

# Native library dependencies
thread_dep        = dependency('threads')
x11_dep           = dependency('x11',         required : true, native: true)
xcb_dep           = dependency('xcb',         required : true, native: true)
xcb_keysyms_dep   = dependency('xcb-keysyms', required : false, native: true)
xext_dep          = dependency('xext',        required : false, native: true)  # XShm

native_deps = [thread_dep, x11_dep, xcb_dep]
if xcb_keysyms_dep.found()
    native_deps += xcb_keysyms_dep
endif
if xext_dep.found()
    native_deps += xext_dep
endif

vst3bridge_lib = shared_library('vst3bridge',
    common_sources,
    native_sources,
    include_directories : [inc_common, inc_native],
    cpp_args            : native_cpp_args,
    dependencies        : native_deps,
    link_args           : ['-Wl,-Bdynamic', '-lrt'],
    install             : not meson.is_cross_build(),
    install_dir         : 'lib/vst3/vst3bridge',
    native              : true,
)

# ---------------------------------------------------------------------------
# Wine host executable (cross-compiled for Windows / Wine)
# ---------------------------------------------------------------------------

wine_sources = files(
    'src/wine-host/host_main.cpp',
    'src/wine-host/vst3_host.cpp',
    'src/wine-host/plugin_instance.cpp',
    'src/wine-host/host_application.cpp',
    'src/wine-host/component_handler.cpp',
    'src/wine-host/window_manager.cpp',
    'src/wine-host/gdi_capture.cpp',
    'src/wine-host/socket_server.cpp',
    'src/wine-host/audio_shm_host.cpp',
    'src/wine-host/audio_processor.cpp',
    'src/wine-host/gui_event_receiver.cpp',
    'src/wine-host/parameter_changes.cpp',
)

wine_common_sources = files(
    'src/common/shared_memory.cpp',
    'src/common/gui_events.cpp',
    'src/common/stream.cpp',
    'src/common/socket.cpp',
)

# Wine host build with wineg++ (proper Wine compiler)
winegpp = find_program('wineg++', required : false)

if winegpp.found()
    all_wine_sources = wine_common_sources + wine_sources

    wine_host_exe = custom_target('vst3bridge-host',
        output   : 'vst3bridge-host.exe',
        input    : all_wine_sources,
        command  : [winegpp, '@INPUT@', '-o', '@OUTPUT@', '-std=c++17', '-I/home/rations/vst3bridge/src/common', '-I/home/rations/vst3bridge/third_party/vst3sdk', '-I/home/rations/vst3bridge/src/wine-host', '-fvisibility=hidden', '-fvisibility-inlines-hidden', '-fno-strict-aliasing', '-msse2', '-DNDEBUG', '-DBUILDING_WINE_HOST', '-DSMTG_OS_WINDOWS', '-DNOMINMAX', '-D_WIN32_WINNT=0x0601', '-Wno-unknown-pragmas', '-Wno-pedantic', '-mwindows', '-lstdc++', '-lm', '-lws2_32', '-lgdi32', '-luser32', '-lkernel32', '-luuid', '-lole32', '-lshell32', '-lrt'],
        build_by_default : true,
        install  : false,
    )
else
    message('wineg++ not found — Wine host build skipped')
endif

# ---------------------------------------------------------------------------
# Unit tests (Phase 9)
# ---------------------------------------------------------------------------

if get_option('tests')
    test_sources = files(
        'tests/test_protocol.cpp',
        'tests/test_shared_memory.cpp',
    )

    test_exe = executable(
        'vst3bridge-tests',
        test_sources,
        common_sources,
        include_directories : inc_common,
        dependencies        : [gtest_dep, thread_dep],
        cpp_args            : native_cpp_args + ['-lgtest_main'],
        link_args           : ['-lrt'],
    )
    test('unit-tests', test_exe)
endif

# ---------------------------------------------------------------------------
# Unit tests (optional, requires gtest)
# ---------------------------------------------------------------------------

gtest_dep = dependency('gtest', required : false)
if gtest_dep.found() and false
    test_sources = files(
        'tests/test_protocol.cpp',
        'tests/test_shared_memory.cpp',
    )

    test_exe = executable('vst3bridge-tests',
        test_sources,
        common_sources,
        include_directories : inc_common,
        dependencies        : [gtest_dep, thread_dep],
        cpp_args            : native_cpp_args + ['-lgtest_main'],
        link_args           : ['-lrt'],
    )
    test('unit-tests', test_exe)
endif
