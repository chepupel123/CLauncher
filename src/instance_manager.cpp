#include "instance_manager.h"
#include "logger.h"

#include <algorithm>

namespace fs = std::filesystem;

bool InstanceManager::load_all(const fs::path& base_dir) {
    base_dir_ = base_dir;
    instances_.clear();
    selected_ = nullptr;

    std::error_code ec;
    if (!fs::exists(base_dir_, ec)) {
        fs::create_directories(base_dir_, ec);
        return true;
    }
    for (const auto& entry : fs::directory_iterator(base_dir_, ec)) {
        if (!entry.is_directory()) continue;
        auto jpath = entry.path() / "instance.json";
        if (!fs::exists(jpath)) continue;
        auto inst = std::make_unique<Instance>();
        if (!inst->load(jpath)) {
            LOG_WARN("instance_manager: skip broken " + jpath.string());
            continue;
        }
        instances_.push_back(std::move(inst));
    }
    if (!instances_.empty()) selected_ = instances_.front().get();
    LOG_INFO("instance_manager: loaded " << instances_.size() << " instance(s)");
    return true;
}

Instance* InstanceManager::find(const std::string& name) const {
    for (const auto& i : instances_) if (i->name == name) return i.get();
    return nullptr;
}

Instance* InstanceManager::create(const std::string& name,
                                  const std::string& mc_version,
                                  const std::string& version_id,
                                  ModLoader loader) {
    if (name.empty() || find(name)) return nullptr;

    auto inst = std::make_unique<Instance>();
    inst->name       = name;
    inst->root_path  = base_dir_ / name;
    inst->mc_version = mc_version;
    inst->version_id = version_id.empty() ? mc_version : version_id;
    inst->mod_loader = loader;

    if (!inst->ensure_layout()) return nullptr;
    if (!inst->save())          return nullptr;

    if (!selected_) selected_ = inst.get();
    instances_.push_back(std::move(inst));
    return instances_.back().get();
}

bool InstanceManager::remove(const std::string& name) {
    auto it = std::find_if(instances_.begin(), instances_.end(),
        [&](const auto& p) { return p->name == name; });
    if (it == instances_.end()) return false;

    std::error_code ec;
    fs::remove_all((*it)->root_path, ec);

    if (selected_ == it->get()) selected_ = nullptr;
    instances_.erase(it);
    if (!selected_ && !instances_.empty()) selected_ = instances_.front().get();
    return !ec;
}
