#pragma once
// =============================================================================
// object_pool.h  —  High-Performance Memory Pool
//
// In HFT, calling new/malloc on the hot path is strictly forbidden. 
// Asking the OS for memory takes microseconds and causes latency jitter.
//
// This ObjectPool pre-allocates a massive contiguous block of memory at
// startup. When the engine needs an Order, it just pops a pointer off the
// free_list. When the shared_ptr goes out of scope, a custom deleter pushes
// the pointer back onto the free_list.
//
// Uses a spinlock (std::atomic_flag) to be significantly faster than std::mutex.
// =============================================================================

#include <vector>
#include <memory>
#include <atomic>
#include <cstdint>

template<typename T, size_t BlockSize = 100000>
class ObjectPool {
public:
    ObjectPool() {
        allocate_block();
    }

    ~ObjectPool() {
        for (T* block : blocks_) {
            delete[] block;
        }
    }

    // Deleted copy constructors
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    std::shared_ptr<T> acquire() {
        acquire_lock();
        if (free_list_.empty()) {
            allocate_block();
        }
        T* obj = free_list_.back();
        free_list_.pop_back();
        release_lock();

        // Custom deleter returns memory to the pool instead of freeing it
        return std::shared_ptr<T>(obj, [this](T* ptr) {
            this->recycle(ptr);
        });
    }

private:
    void recycle(T* ptr) {
        acquire_lock();
        free_list_.push_back(ptr);
        release_lock();
    }

    void allocate_block() {
        T* block = new T[BlockSize];
        blocks_.push_back(block);
        for (size_t i = 0; i < BlockSize; ++i) {
            free_list_.push_back(&block[i]);
        }
    }

    void acquire_lock() {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            // Spin
        }
    }

    void release_lock() {
        lock_.clear(std::memory_order_release);
    }

    std::vector<T*> blocks_;
    std::vector<T*> free_list_;
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};
