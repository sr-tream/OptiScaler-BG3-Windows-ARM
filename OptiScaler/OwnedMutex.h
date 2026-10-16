#pragma once

#include "SysUtils.h"

#include <atomic>
#include <shared_mutex>

class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 }; // don't use 0

  public:
    void lock(uint32_t _owner)
    {
        mtx.lock();
        owner.store(_owner, std::memory_order_release);
    }

    bool try_lock(uint32_t _owner)
    {
        if (!mtx.try_lock())
            return false;
        owner.store(_owner, std::memory_order_release);
        return true;
    }

    // Only unlocks if owner matches
    void unlockThis(uint32_t _owner)
    {
        uint32_t current_owner = owner.load(std::memory_order_acquire);

        if (current_owner == 0 || current_owner != _owner)
        {
            LOG_WARN("current_owner: {}, _owner: {}", current_owner, _owner);
            return;
        }

        owner.store(0, std::memory_order_release);
        mtx.unlock();
    }

    uint32_t getOwner() { return owner.load(std::memory_order_seq_cst); }
};

class OwnedLockGuard
{
  private:
    OwnedMutex& _mutex;
    uint32_t _owner_id;

  public:
    OwnedLockGuard(OwnedMutex& mutex, uint32_t owner_id) : _mutex(mutex), _owner_id(owner_id)
    {
        _mutex.lock(_owner_id);
    }

    ~OwnedLockGuard() { _mutex.unlockThis(_owner_id); }
};

class OwnedTryLockGuard
{
    OwnedMutex& mutex;
    uint32_t owner;
    bool locked;
  public:
    OwnedTryLockGuard(OwnedMutex& m, uint32_t id) : mutex(m), owner(id), locked(m.try_lock(id)) {}
    ~OwnedTryLockGuard() { if (locked) mutex.unlockThis(owner); }
    explicit operator bool() const { return locked; }
};
