/* Copyright (C) 2024 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef AUDIO_DEVICE_MANAGER_HPP
#define AUDIO_DEVICE_MANAGER_HPP

#if defined(__linux__)
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#elif defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class audio_device_manager {
   public:
    using sources_changed_cb_t = std::function<void()>;

    struct device_t {
        unsigned int index = 0;
        std::string name;
        std::string description;
    };

    explicit audio_device_manager(sources_changed_cb_t sources_changed_cb);
    ~audio_device_manager();
    std::vector<device_t> sources();
    bool has_source_name(const std::string &name);
    std::optional<audio_device_manager::device_t> source_by_name(
        const std::string &name);
    std::optional<audio_device_manager::device_t> source_by_description(
        const std::string &description);

   private:
    sources_changed_cb_t m_sources_changed_cb;
    std::thread m_thread;
    std::mutex m_mtx;
    bool m_sources_discovery_done = false;
    std::unordered_map<std::string, device_t> m_sources;
#if defined(__linux__)
    pa_mainloop *m_pa_loop = nullptr;
    pa_context *m_pa_ctx = nullptr;
    void clean();
    void remove_source_by_index(unsigned int index);
    static void subscription_pa_callback(pa_context *ctx,
                                         pa_subscription_event_type_t t,
                                         uint32_t idx, void *userdata);
    static void source_info_pa_callback(pa_context *ctx,
                                        const pa_source_info *info, int eol,
                                        void *userdata);
#elif defined(__APPLE__)
    bool m_device_listener_registered = false;
    void update_sources();
    static OSStatus devices_changed_callback(
        AudioObjectID inObjectID, UInt32 inNumberAddresses,
        const AudioObjectPropertyAddress *inAddresses, void *inClientData);
#endif
};

#endif  // AUDIO_DEVICE_MANAGER_HPP
