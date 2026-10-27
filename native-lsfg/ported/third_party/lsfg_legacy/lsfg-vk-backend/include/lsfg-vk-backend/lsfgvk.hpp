/* SPDX-License-Identifier: GPL-3.0-or-later */
// Windows same-device adapter, 2026-09-28. No external memory or semaphore API.
#pragma once
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>
#include "lsfg-vk-common/vulkan/vulkan.hpp"
namespace lsfgvk::backend {
class ContextImpl; class InstanceImpl; using Context=ContextImpl;
class error : public std::runtime_error {
public: explicit error(const std::string&, const std::exception&); explicit error(const std::string&); ~error() override;
};
class Instance {
public:
 Instance(const vk::Vulkan&, const std::filesystem::path&, bool allowLowPrecision=false);
 Context& openContext(std::pair<VkImage,VkImage>,const std::vector<VkImage>&,VkSemaphore,uint32_t,uint32_t,bool,float,bool);
 // openContext only records initialization; retain the context before submitting.
 void initializeContext(Context&);
 // Caller must ensure its previous batch has completed before either call.
 void scheduleFrames(Context&); void closeContext(const Context&); ~Instance();
 Instance(const Instance&)=delete; Instance& operator=(const Instance&)=delete;
private: std::unique_ptr<InstanceImpl> m_impl; std::vector<std::unique_ptr<Context>> m_contexts;
};
}
