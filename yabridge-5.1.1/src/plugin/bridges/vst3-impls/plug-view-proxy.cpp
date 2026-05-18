// yabridge: a Wine plugin bridge
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

#include "plug-view-proxy.h"

#include <chrono>
#include <thread>
#include <xcb/xcb.h>

RunLoopTasks::RunLoopTasks(Steinberg::IPtr<Steinberg::IPlugFrame> plug_frame)
    : run_loop_(plug_frame) {
    FUNKNOWN_CTOR

    if (!run_loop_) {
        throw std::runtime_error(
            "The host's 'IPlugFrame' object does not support 'IRunLoop'");
    }

    // This should be backed by eventfd instead, but Ardour doesn't allow that
    std::array<int, 2> sockets;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   sockets.data()) != 0) {
        throw std::runtime_error("Failed to create a Unix domain socket");
    }

    socket_read_fd_ = sockets[0];
    socket_write_fd_ = sockets[1];
    if (run_loop_->registerEventHandler(this, socket_read_fd_) !=
        Steinberg::kResultOk) {
        throw std::runtime_error(
            "Failed to register an event handler with the host's run loop");
    }
}

RunLoopTasks::~RunLoopTasks() {
    FUNKNOWN_DTOR

    run_loop_->unregisterEventHandler(this);
    close(socket_read_fd_);
    close(socket_write_fd_);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
IMPLEMENT_FUNKNOWN_METHODS(RunLoopTasks,
                           Steinberg::Linux::IEventHandler,
                           Steinberg::Linux::IEventHandler::iid)
#pragma GCC diagnostic pop

void RunLoopTasks::schedule(fu2::unique_function<void()> task) {
    std::lock_guard eventfd_lock(tasks_mutex_);
    tasks_.push_back(std::move(task));

    uint8_t notify_value = 1;
    assert(write(socket_write_fd_, &notify_value, sizeof(notify_value)) ==
           sizeof(notify_value));
}

void PLUGIN_API
RunLoopTasks::onFDIsSet(Steinberg::Linux::FileDescriptor /*fd*/) {
    std::lock_guard lock(tasks_mutex_);

    // Run all tasks that have been submitted to our queue from the host's
    // calling thread (which will be the GUI thread)
    for (auto& task : tasks_) {
        task();

        // This should in theory stop the host from calling this function, but
        // REAPER doesn't care. And funnily enough we only have to do all of
        // this because of REAPER.
        uint8_t notify_value;
        assert(read(socket_read_fd_, &notify_value, sizeof(notify_value)) ==
               sizeof(notify_value));
    }

    tasks_.clear();
}

Vst3PlugViewProxyImpl::Vst3PlugViewProxyImpl(
    Vst3PluginBridge& bridge,
    Vst3PlugViewProxy::ConstructArgs&& args) noexcept
    : Vst3PlugViewProxy(std::move(args)), bridge_(bridge) {}

Vst3PlugViewProxyImpl::~Vst3PlugViewProxyImpl() noexcept {
    // Also drop the plug view smart pointer on the Wine side when this gets
    // dropped
    // NOTE: This can actually throw (e.g. out of memory or the socket got
    //       closed). But if that were to happen, then we wouldn't be able to
    //       recover from it anyways.
    bridge_.send_mutually_recursive_message(
        Vst3PlugViewProxy::Destruct{.owner_instance_id = owner_instance_id()});
}

tresult PLUGIN_API
Vst3PlugViewProxyImpl::queryInterface(const Steinberg::TUID _iid, void** obj) {
    const tresult result = Vst3PlugViewProxy::queryInterface(_iid, obj);
    bridge_.logger_.log_query_interface("In IPlugView::queryInterface()",
                                        result,
                                        Steinberg::FUID::fromTUID(_iid));

    return result;
}

tresult PLUGIN_API
Vst3PlugViewProxyImpl::isPlatformTypeSupported(Steinberg::FIDString type) {
    if (type) {
        // We'll swap the X11 window ID platform type string for the Win32 HWND
        // equivalent on the Wine side
        return bridge_.send_mutually_recursive_message(
            YaPlugView::IsPlatformTypeSupported{
                .owner_instance_id = owner_instance_id(), .type = type});
    } else {
        bridge_.logger_.log(
            "WARNING: Null pointer passed to "
            "'IPlugView::isPlatformTypeSupported()'");
        return Steinberg::kInvalidArgument;
    }
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::attached(void* parent,
                                                   Steinberg::FIDString type) {
    if (!parent || !type) {
        bridge_.logger_.log(
            "WARNING: Null pointer passed to 'IPlugView::attached()'");
        return Steinberg::kInvalidArgument;
    }

    // Create POSIX shared memory for GDI frame transport.
    // Maximum size 1920x1080; the Wine side captures at the actual plugin size.
    render_shm_ = yabridge::FrameSharedMemory::create(1920, 1080);
    if (!render_shm_ || !render_shm_->valid()) {
        bridge_.logger_.log("WARNING: Failed to create frame SHM");
        render_shm_.reset();
    }

    // Open a dedicated X11 connection for the render thread.
    render_x11_ = xcb_connect(nullptr, nullptr);
    if (!render_x11_ || xcb_connection_has_error(render_x11_)) {
        bridge_.logger_.log("WARNING: Failed to open X11 connection for render");
        if (render_x11_) {
            xcb_disconnect(render_x11_);
            render_x11_ = nullptr;
        }
        render_shm_.reset();
    }

    // Create a child window under the DAW's parent window.
    if (render_x11_ && render_shm_) {
        render_parent_xid_ =
            static_cast<xcb_window_t>(reinterpret_cast<native_size_t>(parent));
        render_event_mask_ = XCB_EVENT_MASK_BUTTON_PRESS |
                             XCB_EVENT_MASK_BUTTON_RELEASE |
                             XCB_EVENT_MASK_POINTER_MOTION;
        const xcb_screen_t* screen =
            xcb_setup_roots_iterator(xcb_get_setup(render_x11_)).data;

        render_window_ = xcb_generate_id(render_x11_);
        xcb_create_window(render_x11_, XCB_COPY_FROM_PARENT, render_window_,
                          render_parent_xid_, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_EVENT_MASK, &render_event_mask_);
        xcb_map_window(render_x11_, render_window_);
        xcb_flush(render_x11_);

        // Query actual window depth (must match xcb_put_image depth arg).
        xcb_get_geometry_reply_t* geom = xcb_get_geometry_reply(
            render_x11_, xcb_get_geometry(render_x11_, render_window_), nullptr);
        render_depth_ = geom ? geom->depth : screen->root_depth;
        if (geom) free(geom);

        render_gc_ = xcb_generate_id(render_x11_);
        xcb_create_gc(render_x11_, render_gc_, render_window_, 0, nullptr);
    }

    const tresult result = bridge_.send_mutually_recursive_message(
        YaPlugView::Attached{
            .owner_instance_id = owner_instance_id(),
            .parent = reinterpret_cast<native_size_t>(parent),
            .type = type,
            .frame_shm_name =
                (render_shm_ && render_shm_->valid()) ? render_shm_->name()
                                                      : std::string{}});

    if (result == Steinberg::kResultOk && render_shm_ && render_window_ != 0) {
        render_running_.store(true, std::memory_order_relaxed);

        render_thread_ = std::thread([this]() {
            while (render_running_.load(std::memory_order_relaxed)) {
                // Poll XCB input events and forward to Wine via SHM ring.
                while (xcb_generic_event_t* raw_ev =
                           xcb_poll_for_event(render_x11_)) {
                    const uint8_t ev_type = raw_ev->response_type & 0x7f;
                    yabridge::FrameSharedMemory::InputEvent input_ev{};
                    if (ev_type == XCB_BUTTON_PRESS ||
                        ev_type == XCB_BUTTON_RELEASE) {
                        const auto* bev =
                            reinterpret_cast<xcb_button_press_event_t*>(
                                raw_ev);
                        input_ev.x = static_cast<int16_t>(bev->event_x);
                        input_ev.y = static_cast<int16_t>(bev->event_y);
                        if (bev->detail == 4 && ev_type == XCB_BUTTON_PRESS) {
                            input_ev.type = 8;
                            input_ev.wheel_delta = 120;
                        } else if (bev->detail == 5 &&
                                   ev_type == XCB_BUTTON_PRESS) {
                            input_ev.type = 8;
                            input_ev.wheel_delta = -120;
                        } else if (bev->detail == 1) {
                            input_ev.type =
                                (ev_type == XCB_BUTTON_PRESS) ? 2 : 3;
                        } else if (bev->detail == 3) {
                            input_ev.type =
                                (ev_type == XCB_BUTTON_PRESS) ? 4 : 5;
                        } else if (bev->detail == 2) {
                            input_ev.type =
                                (ev_type == XCB_BUTTON_PRESS) ? 6 : 7;
                        }
                        if (input_ev.type != 0)
                            render_shm_->writeInput(input_ev);
                    } else if (ev_type == XCB_MOTION_NOTIFY) {
                        const auto* mev =
                            reinterpret_cast<xcb_motion_notify_event_t*>(
                                raw_ev);
                        input_ev.type = 1;
                        input_ev.x = static_cast<int16_t>(mev->event_x);
                        input_ev.y = static_cast<int16_t>(mev->event_y);
                        render_shm_->writeInput(input_ev);
                    }
                    free(raw_ev);
                }

                uint32_t w = 0, h = 0, stride = 0;
                const uint8_t* px = render_shm_->beginRead(w, h, stride);
                if (px && w > 0 && h > 0) {
                    // Resize child window to match frame dimensions.
                    const uint32_t dims[2] = {w, h};
                    xcb_configure_window(
                        render_x11_, render_window_,
                        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                        dims);

                    // Blit the BGRA frame into the X11 window.
                    xcb_put_image(render_x11_, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                  render_window_, render_gc_,
                                  static_cast<uint16_t>(w),
                                  static_cast<uint16_t>(h),
                                  0, 0, 0, render_depth_,
                                  stride * h, px);
                    xcb_flush(render_x11_);
                    render_shm_->endRead();

                    // If the connection is dead (parent window was destroyed
                    // when the DAW restructured its FX window layout), try to
                    // reconnect and recreate our render window.
                    if (xcb_connection_has_error(render_x11_)) {
                        using namespace std::chrono_literals;
                        std::this_thread::sleep_for(100ms);
                        render_reconnect();
                    }
                } else {
                    using namespace std::chrono_literals;
                    std::this_thread::sleep_for(8ms);
                }
            }
        });
    }

    if (result != Steinberg::kResultOk) {
        stop_render_thread();
    }

    return result;
}

bool Vst3PlugViewProxyImpl::render_reconnect() noexcept {
    // Close the dead connection (GC and window are already gone).
    if (render_x11_) {
        xcb_disconnect(render_x11_);
        render_x11_ = nullptr;
    }
    render_window_ = 0;
    render_gc_ = 0;

    render_x11_ = xcb_connect(nullptr, nullptr);
    if (!render_x11_ || xcb_connection_has_error(render_x11_)) {
        if (render_x11_) { xcb_disconnect(render_x11_); render_x11_ = nullptr; }
        return false;
    }

    const xcb_screen_t* screen =
        xcb_setup_roots_iterator(xcb_get_setup(render_x11_)).data;

    render_window_ = xcb_generate_id(render_x11_);
    xcb_create_window(render_x11_, XCB_COPY_FROM_PARENT, render_window_,
                      render_parent_xid_, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_EVENT_MASK, &render_event_mask_);
    xcb_map_window(render_x11_, render_window_);
    xcb_flush(render_x11_);

    xcb_get_geometry_reply_t* geom = xcb_get_geometry_reply(
        render_x11_, xcb_get_geometry(render_x11_, render_window_), nullptr);
    render_depth_ = geom ? geom->depth : screen->root_depth;
    if (geom) free(geom);

    render_gc_ = xcb_generate_id(render_x11_);
    xcb_create_gc(render_x11_, render_gc_, render_window_, 0, nullptr);

    // If even the fresh connection is in error, the parent window is gone.
    return !xcb_connection_has_error(render_x11_);
}

void Vst3PlugViewProxyImpl::stop_render_thread() noexcept {
    render_running_.store(false, std::memory_order_relaxed);
    if (render_thread_.joinable()) render_thread_.join();

    if (render_x11_) {
        if (render_gc_ != 0) {
            xcb_free_gc(render_x11_, render_gc_);
            render_gc_ = 0;
        }
        if (render_window_ != 0) {
            xcb_destroy_window(render_x11_, render_window_);
            render_window_ = 0;
        }
        xcb_flush(render_x11_);
        xcb_disconnect(render_x11_);
        render_x11_ = nullptr;
    }
    render_shm_.reset();
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::removed() {
    stop_render_thread();
    return bridge_.send_mutually_recursive_message(
        YaPlugView::Removed{.owner_instance_id = owner_instance_id()});
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::onWheel(float distance) {
    return bridge_.send_mutually_recursive_message(YaPlugView::OnWheel{
        .owner_instance_id = owner_instance_id(), .distance = distance});
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::onKeyDown(char16 key,
                                                    int16 keyCode,
                                                    int16 modifiers) {
    return bridge_.send_mutually_recursive_message(
        YaPlugView::OnKeyDown{.owner_instance_id = owner_instance_id(),
                              .key = key,
                              .key_code = keyCode,
                              .modifiers = modifiers});
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::onKeyUp(char16 key,
                                                  int16 keyCode,
                                                  int16 modifiers) {
    return bridge_.send_mutually_recursive_message(
        YaPlugView::OnKeyUp{.owner_instance_id = owner_instance_id(),
                            .key = key,
                            .key_code = keyCode,
                            .modifiers = modifiers});
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::getSize(Steinberg::ViewRect* size) {
    if (size) {
        const GetSizeResponse response =
            bridge_.send_mutually_recursive_message(
                YaPlugView::GetSize{.owner_instance_id = owner_instance_id()});

        *size = response.size;

        return response.result;
    } else {
        bridge_.logger_.log(
            "WARNING: Null pointer passed to 'IPlugView::getSize()'");
        return Steinberg::kInvalidArgument;
    }
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::onSize(Steinberg::ViewRect* newSize) {
    if (newSize) {
        return bridge_.send_mutually_recursive_message(YaPlugView::OnSize{
            .owner_instance_id = owner_instance_id(), .new_size = *newSize});
    } else {
        bridge_.logger_.log(
            "WARNING: Null pointer passed to 'IPlugView::onSize()'");
        return Steinberg::kInvalidArgument;
    }
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::onFocus(TBool state) {
    return bridge_.send_mutually_recursive_message(YaPlugView::OnFocus{
        .owner_instance_id = owner_instance_id(), .state = state});
}

tresult PLUGIN_API
Vst3PlugViewProxyImpl::setFrame(Steinberg::IPlugFrame* frame) {
    // Null pointers are valid here going from the reference implementations in
    // the SDK
    if (frame) {
        // We'll store the pointer for when the plugin later makes a callback to
        // this component handler
        plug_frame_ = frame;

        // REAPER's GUI is not thread safe, and if we don't call
        // `IPlugFrame::resizeView()` or `IContextMenu::popup()` from a thread
        // owned by REAPER then REAPER will eventually segfault We should thus
        // try to call those functions from an `IRunLoop` event handler.
        try {
            run_loop_tasks_.emplace(plug_frame_);
        } catch (const std::runtime_error& error) {
            // In case the host does not support `IRunLoop` or if we can't
            // register an event handler, we'll throw during `RunLoopTasks`'
            // constructor
            run_loop_tasks_.reset();

            bridge_.logger_.log(
                "The host does not support IRunLoop, falling back to naive GUI "
                "function execution: " +
                std::string(error.what()));
        }

        return bridge_.send_mutually_recursive_message(YaPlugView::SetFrame{
            .owner_instance_id = owner_instance_id(),
            .plug_frame_args = Vst3PlugFrameProxy::ConstructArgs(
                plug_frame_, owner_instance_id())});
    } else {
        plug_frame_.reset();
        run_loop_tasks_.reset();

        return bridge_.send_mutually_recursive_message(
            YaPlugView::SetFrame{.owner_instance_id = owner_instance_id(),
                                 .plug_frame_args = std::nullopt});
    }
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::canResize() {
    const auto request =
        YaPlugView::CanResize{.owner_instance_id = owner_instance_id()};

    {
        std::lock_guard lock(can_resize_cache_mutex_);
        if (const tresult* result = can_resize_cache_.get_and_keep_alive(5)) {
            const bool log_response =
                bridge_.logger_.log_request(true, request);
            if (log_response) {
                bridge_.logger_.log_response(
                    true, YaPlugView::CanResize::Response(*result), true);
            }

            return *result;
        }
    }

    const UniversalTResult result =
        bridge_.send_mutually_recursive_message(request);

    {
        std::lock_guard lock(can_resize_cache_mutex_);
        can_resize_cache_.set(result, 5);
    }

    return result;
}

tresult PLUGIN_API
Vst3PlugViewProxyImpl::checkSizeConstraint(Steinberg::ViewRect* rect) {
    if (rect) {
        const CheckSizeConstraintResponse response =
            bridge_.send_mutually_recursive_message(
                YaPlugView::CheckSizeConstraint{
                    .owner_instance_id = owner_instance_id(), .rect = *rect});

        *rect = response.updated_rect;

        return response.result;
    } else {
        bridge_.logger_.log(
            "WARNING: Null pointer passed to "
            "'IPlugView::checkSizeConstraint()'");
        return Steinberg::kInvalidArgument;
    }
}

tresult PLUGIN_API Vst3PlugViewProxyImpl::findParameter(
    int32 xPos,
    int32 yPos,
    Steinberg::Vst::ParamID& resultTag /*out*/) {
    const FindParameterResponse response =
        bridge_.send_mutually_recursive_message(
            YaParameterFinder::FindParameter{
                .owner_instance_id = owner_instance_id(),
                .x_pos = xPos,
                .y_pos = yPos});

    resultTag = response.result_tag;

    return response.result;
}

tresult PLUGIN_API
Vst3PlugViewProxyImpl::setContentScaleFactor(ScaleFactor factor) {
    return bridge_.send_mutually_recursive_message(
        YaPlugViewContentScaleSupport::SetContentScaleFactor{
            .owner_instance_id = owner_instance_id(), .factor = factor});
}
