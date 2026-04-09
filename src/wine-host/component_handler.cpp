/*
 * Copyright (C) 2026
 * VST3 Bridge - Wine VST3 Host Bridge
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/**
 * @file component_handler.cpp
 * @brief Wine-side IComponentHandler proxy implementation.
 */

#include "component_handler.h"
#include "logger.h"

namespace vst3bridge {

// ============================================================================
// Static iid
// ============================================================================

const Steinberg::FUID ComponentHandler::iid(
        0xA7B8C9DAu, 0x3E4F5A6Bu, 0xB7C8D9EAu, 0x5F6A7B8Cu);

// ============================================================================
// Constructor
// ============================================================================
// Constructor
// ============================================================================

ComponentHandler::ComponentHandler(WineSocketClient* socket)
    : socket_(socket), refCount_(1)
{}

// ============================================================================
// FUnknown
// ============================================================================



Steinberg::tresult PLUGIN_API ComponentHandler::queryInterface(
        const Steinberg::TUID _iid, void** obj)
{
    if (!obj) return Steinberg::kInvalidArgument;

    const Steinberg::FUID requested(_iid);

    if (requested == Steinberg::FUnknown::iid ||
        requested == Steinberg::IComponentHandler::iid ||
        requested == ComponentHandler::iid)
    {
        *obj = static_cast<Steinberg::IComponentHandler*>(this);
        addRef();
        return Steinberg::kResultOk;
    }

    *obj = nullptr;
    return Steinberg::kNoInterface;
}

// ============================================================================
// IComponentHandler
// ============================================================================

Steinberg::tresult PLUGIN_API ComponentHandler::beginEdit(Steinberg::uint32 id)
{
    if (!socket_) return Steinberg::kInternalError;

    MsgComponentHandlerBeginEdit msg;
    msg.param_id = id;

    LOG_DEBUG("ComponentHandler::beginEdit({})", id);

    if (!socket_->sendMessage(MsgType::ComponentHandlerBeginEdit, &msg, sizeof(msg))) {
        LOG_ERROR("ComponentHandler::beginEdit(): sendMessage failed");
        return Steinberg::kInternalError;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API ComponentHandler::performEdit(
        Steinberg::uint32 id, double valueNormalized)
{
    if (!socket_) return Steinberg::kInternalError;

    MsgComponentHandlerPerformEdit msg;
    msg.param_id = id;
    msg.value    = valueNormalized;

    LOG_TRACE("ComponentHandler::performEdit({}, {})", id, valueNormalized);

    if (!socket_->sendMessage(MsgType::ComponentHandlerPerformEdit, &msg, sizeof(msg))) {
        LOG_ERROR("ComponentHandler::performEdit(): sendMessage failed");
        return Steinberg::kInternalError;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API ComponentHandler::endEdit(Steinberg::uint32 id)
{
    if (!socket_) return Steinberg::kInternalError;

    MsgComponentHandlerEndEdit msg;
    msg.param_id = id;

    LOG_DEBUG("ComponentHandler::endEdit({})", id);

    if (!socket_->sendMessage(MsgType::ComponentHandlerEndEdit, &msg, sizeof(msg))) {
        LOG_ERROR("ComponentHandler::endEdit(): sendMessage failed");
        return Steinberg::kInternalError;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API ComponentHandler::restartComponent(Steinberg::int32 flags)
{
    if (!socket_) return Steinberg::kInternalError;

    MsgComponentHandlerRestart msg;
    msg.flags = flags;

    LOG_INFO("ComponentHandler::restartComponent(flags=0x{:x})", static_cast<uint32_t>(flags));

    if (!socket_->sendMessage(MsgType::ComponentHandlerRestart, &msg, sizeof(msg))) {
        LOG_ERROR("ComponentHandler::restartComponent(): sendMessage failed");
        return Steinberg::kInternalError;
    }
    return Steinberg::kResultOk;
}

IMPLEMENT_REFCOUNT(ComponentHandler)

} // namespace vst3bridge
