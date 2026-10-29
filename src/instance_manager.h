#pragma once

#include <filesystem>
#include <memory>
#include <vector>
#include "instance.h"

class InstanceManager {
public:
    bool load_all(const std::filesystem::path& base_dir);

    Instance* create(const std::string& name,
                     const std::string& mc_version,
                     const std::string& version_id,
                     ModLoader loader);

    bool remove(const std::string& name);
    Instance* find(const std::string& name) const;

    Instance* get_selected() const         { return selected_; }
    void      set_selected(Instance* i)    { selected_ = i; }

    const std::vector<std::unique_ptr<Instance>>& all() const { return instances_; }

private:
    std::vector<std::unique_ptr<Instance>> instances_;
    Instance*                              selected_ = nullptr;
    std::filesystem::path                  base_dir_;
};
