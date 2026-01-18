/* Copyright (C) 2024 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "audio_device_manager.hpp"

#if defined(__APPLE__)

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "logger.hpp"

namespace {
std::string cf_string_to_std(CFStringRef str) {
    if (!str) return {};
    const auto *cstr = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);
    if (cstr) return std::string{cstr};

    CFIndex length = CFStringGetLength(str);
    CFIndex max_size = CFStringGetMaximumSizeForEncoding(
        length, kCFStringEncodingUTF8);
    std::string buffer(static_cast<size_t>(max_size), '\0');
    if (CFStringGetCString(str, buffer.data(), max_size,
                           kCFStringEncodingUTF8)) {
        buffer.resize(std::strlen(buffer.c_str()));
        return buffer;
    }
    return {};
}

std::string get_device_string(AudioObjectID device,
                              AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr{selector, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMaster};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &value) !=
        noErr) {
        return {};
    }
    auto result = cf_string_to_std(value);
    if (value) CFRelease(value);
    return result;
}

bool device_has_input(AudioObjectID device) {
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) !=
            noErr ||
        size == 0) {
        return false;
    }

    auto *buffer_list = static_cast<AudioBufferList *>(std::malloc(size));
    if (!buffer_list) return false;
    bool has_channels = false;

    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size,
                                   buffer_list) == noErr) {
        UInt32 channels = 0;
        for (UInt32 i = 0; i < buffer_list->mNumberBuffers; ++i) {
            channels += buffer_list->mBuffers[i].mNumberChannels;
        }
        has_channels = channels > 0;
    }

    std::free(buffer_list);
    return has_channels;
}
}  // namespace

audio_device_manager::audio_device_manager(
    sources_changed_cb_t sources_changed_cb)
    : m_sources_changed_cb{std::move(sources_changed_cb)} {
    update_sources();

    AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMaster };
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                       devices_changed_callback, this) ==
        noErr) {
        m_device_listener_registered = true;
    } else {
        LOGW("failed to register CoreAudio device listener");
    }
}

audio_device_manager::~audio_device_manager() {
    if (m_device_listener_registered) {
        AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMaster };
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr,
                                          devices_changed_callback, this);
        m_device_listener_registered = false;
    }
}

std::vector<audio_device_manager::device_t> audio_device_manager::sources() {
    decltype(sources()) sources_list;
    {
        std::lock_guard guard{m_mtx};
        std::transform(m_sources.cbegin(), m_sources.cend(),
                       std::back_inserter(sources_list),
                       [](const auto &p) { return p.second; });
    }
    return sources_list;
}

bool audio_device_manager::has_source_name(const std::string &name) {
    std::lock_guard guard{m_mtx};
    return m_sources.count(name) > 0;
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_name(const std::string &name) {
    std::lock_guard guard{m_mtx};
    if (m_sources.count(name) == 0) return std::nullopt;
    return m_sources.at(name);
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_description(
    const std::string &description) {
    std::lock_guard guard{m_mtx};
    auto it = std::find_if(m_sources.cbegin(), m_sources.cend(),
                           [&description](const auto &p) {
                               return p.second.description == description;
                           });
    if (it == m_sources.cend()) return std::nullopt;
    return it->second;
}

void audio_device_manager::update_sources() {
    AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMaster };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0,
                                       nullptr, &size) != noErr ||
        size == 0) {
        return;
    }

    auto device_count = size / sizeof(AudioObjectID);
    std::vector<AudioObjectID> devices(device_count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, devices.data()) != noErr) {
        return;
    }

    std::unordered_map<std::string, device_t> new_sources;
    new_sources.reserve(devices.size());

    for (auto device_id : devices) {
        if (!device_has_input(device_id)) continue;

        auto uid = get_device_string(device_id, kAudioDevicePropertyDeviceUID);
        auto name = get_device_string(device_id, kAudioObjectPropertyName);

        device_t device;
        device.index = static_cast<unsigned int>(device_id);
        device.name = uid.empty() ? name : uid;
        device.description = name.empty() ? device.name : name;

        if (!device.name.empty()) {
            new_sources[device.name] = std::move(device);
        }
    }

    {
        std::lock_guard guard{m_mtx};
        m_sources.swap(new_sources);
        m_sources_discovery_done = true;
    }

    if (m_sources_changed_cb) m_sources_changed_cb();
}

OSStatus audio_device_manager::devices_changed_callback(
    [[maybe_unused]] AudioObjectID inObjectID,
    [[maybe_unused]] UInt32 inNumberAddresses,
    [[maybe_unused]] const AudioObjectPropertyAddress *inAddresses,
    void *inClientData) {
    auto *dm = static_cast<audio_device_manager *>(inClientData);
    if (dm) dm->update_sources();
    return noErr;
}

#elif !defined(__linux__)

audio_device_manager::audio_device_manager(
    sources_changed_cb_t sources_changed_cb)
    : m_sources_changed_cb{sources_changed_cb} {}

audio_device_manager::~audio_device_manager() = default;

std::vector<audio_device_manager::device_t> audio_device_manager::sources() {
    return {};
}

bool audio_device_manager::has_source_name(const std::string &name) {
    (void)name;
    return false;
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_name(const std::string &name) {
    (void)name;
    return std::nullopt;
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_description(
    const std::string &description) {
    (void)description;
    return std::nullopt;
}

#else

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/subscribe.h>

#include <stdexcept>
#include <string>
#include <algorithm>

#include "config.h"
#include "logger.hpp"

static void state_pa_callback(pa_context *ctx,
                              [[maybe_unused]] void *userdata) {
    switch (pa_context_get_state(ctx)) {
        case PA_CONTEXT_CONNECTING:
            LOGD("pa connecting");
            break;
        case PA_CONTEXT_AUTHORIZING:
            LOGD("pa authorizing");
            break;
        case PA_CONTEXT_SETTING_NAME:
            LOGD("pa setting name");
            break;
        case PA_CONTEXT_READY:
            LOGD("pa ready");
            break;
        case PA_CONTEXT_TERMINATED:
            LOGD("pa terminated");
            break;
        case PA_CONTEXT_FAILED:
            LOGD("pa failed");
            throw std::runtime_error{"pa failed"};
        default:
            LOGD("pa unknown state");
    }
}

audio_device_manager::audio_device_manager(
    sources_changed_cb_t sources_changed_cb) {
    m_pa_loop = pa_mainloop_new();
    if (!m_pa_loop) throw std::runtime_error{"pa_mainloop_new error"};

    try {
        auto *mla = pa_mainloop_get_api(m_pa_loop);

        m_pa_ctx = pa_context_new(mla, APP_ID);
        if (!m_pa_ctx) throw std::runtime_error{"pa_context_new error"};

        if (pa_context_connect(m_pa_ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) <
            0) {
            throw std::runtime_error{std::string{"pa_context_connect error: "} +
                                     pa_strerror(pa_context_errno(m_pa_ctx))};
        }

        pa_context_set_state_callback(m_pa_ctx, state_pa_callback, this);

        while (true) {
            auto ret = pa_mainloop_iterate(m_pa_loop, 0, nullptr);
            auto state = pa_context_get_state(m_pa_ctx);
            if (ret < 0 || state == PA_CONTEXT_FAILED ||
                state == PA_CONTEXT_TERMINATED)
                throw std::runtime_error{"pa error"};
            if (state == PA_CONTEXT_READY) break;
        }

        pa_context_set_subscribe_callback(m_pa_ctx, subscription_pa_callback,
                                          this);
        auto mask =
            static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SOURCE);

        auto *op = pa_context_subscribe(
            m_pa_ctx, mask,
            [](pa_context *ctx, int success, void *userdata) {
                if (success) {
                    pa_operation_unref(pa_context_get_source_info_list(
                        ctx, source_info_pa_callback, userdata));
                }
            },
            this);
        if (!op) throw std::runtime_error("pa_context_subscribe error");
        pa_operation_unref(op);
    } catch (const std::runtime_error &err) {
        clean();

        LOGE(std::string{"error when initializing pulse-audio: "} + err.what());
        return;
    } catch (...) {
        clean();

        LOGE("error when initializing pulse-audio");
        return;
    }

    m_sources_changed_cb = std::move(sources_changed_cb);

    m_thread = std::thread{[&]() {
        int ret = 0;
        pa_mainloop_run(m_pa_loop, &ret);

        LOGD("pa loop finished: " << ret);
    }};
}

audio_device_manager::~audio_device_manager() { clean(); }

std::vector<audio_device_manager::device_t> audio_device_manager::sources() {
    decltype(sources()) sources_list;

    {
        std::lock_guard guard{m_mtx};
        std::transform(m_sources.cbegin(), m_sources.cend(),
                       std::back_inserter(sources_list),
                       [](const auto &p) { return p.second; });
    }

    return sources_list;
}

void audio_device_manager::remove_source_by_index(unsigned int index) {
    std::unique_lock guard{m_mtx};
    auto it = std::find_if(
        m_sources.cbegin(), m_sources.cend(),
        [index](const auto &p) { return p.second.index == index; });
    if (it != m_sources.cend()) {
        m_sources.erase(it);
        guard.unlock();
        if (m_sources_changed_cb) m_sources_changed_cb();
    }
}

void audio_device_manager::subscription_pa_callback(
    pa_context *ctx, pa_subscription_event_type_t t, uint32_t idx,
    void *userdata) {
    auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    switch (facility) {
        case PA_SUBSCRIPTION_EVENT_SOURCE:
            if (type == PA_SUBSCRIPTION_EVENT_NEW ||
                type == PA_SUBSCRIPTION_EVENT_CHANGE) {
                if (type == PA_SUBSCRIPTION_EVENT_NEW)
                    LOGD("pa source created: " << idx);
                else
                    LOGD("pa source changed: " << idx);
                pa_operation_unref(pa_context_get_source_info_by_index(
                    ctx, idx, source_info_pa_callback, userdata));
            } else if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
                LOGD("pa sink input removed: " << idx);
                static_cast<audio_device_manager *>(userdata)
                    ->remove_source_by_index(idx);
            }
            break;
        default:
            break;
    }
}

bool audio_device_manager::has_source_name(const std::string &name) {
    std::lock_guard guard{m_mtx};
    return m_sources.count(name) > 0;
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_name(const std::string &name) {
    std::lock_guard guard{m_mtx};
    if (m_sources.count(name) == 0) return std::nullopt;

    return m_sources.at(name);
}

std::optional<audio_device_manager::device_t>
audio_device_manager::source_by_description(const std::string &description) {
    std::lock_guard guard{m_mtx};
    auto it = std::find_if(m_sources.cbegin(), m_sources.cend(),
                           [&description](const auto &p) {
                               return p.second.description == description;
                           });

    if (it == m_sources.cend()) return std::nullopt;

    return it->second;
}

void audio_device_manager::source_info_pa_callback(
    [[maybe_unused]] pa_context *ctx, const pa_source_info *info, int eol,
    void *userdata) {
    auto *dm = static_cast<audio_device_manager *>(userdata);

    if (eol) {
        dm->m_sources_discovery_done = true;
        if (dm->m_sources_changed_cb) dm->m_sources_changed_cb();
        return;
    }

    LOGD("pa source: " << info->name << " " << info->description);

    std::lock_guard guard{dm->m_mtx};

    auto &device = dm->m_sources[info->name];

    device.name = info->name;
    device.description = info->description;
    device.index = info->index;
}

void audio_device_manager::clean() {
    if (m_thread.joinable()) m_thread.join();

    if (m_pa_ctx) {
        pa_context_unref(m_pa_ctx);
        m_pa_ctx = nullptr;
    }

    if (m_pa_loop) {
        pa_mainloop_free(m_pa_loop);
        m_pa_loop = nullptr;
    }
}

#endif  // !defined(__linux__)
