#pragma once

namespace single_instance {

bool acquire();
void release();

class InstanceLock {
public:
    InstanceLock();
    ~InstanceLock();

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    bool is_acquired() const { return m_acquired; }

private:
    bool m_acquired;
};

} // namespace single_instance