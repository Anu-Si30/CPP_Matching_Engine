#pragma once
// =============================================================================
// spsc_queue.h  —  Lock-Free Single-Producer Single-Consumer Ring Buffer
//
// In low-latency trading, standard mutexes are far too slow. When an agent
// wants to send an order to the matching engine, it uses a lock-free queue.
// 
// This implementation uses a ring buffer with atomic head and tail pointers.
// Because there is only one producer and one consumer, we don't need expensive
// Compare-And-Swap (CAS) operations. We only need memory barriers (acquire/release).
//
// To prevent "false sharing" (where two threads invalidate each other's L1 cache
// because they are writing to variables on the same 64-byte cache line), the
// head and tail pointers are strictly aligned to 64 bytes.
// =============================================================================

#include <atomic>
#include <vector>
#include <cstddef>

template<typename T>
class SPSCQueue {
public:
    // Capacity should ideally be a power of 2, but for simplicity we just use
    // modulo arithmetic here.
    explicit SPSCQueue(size_t capacity) : capacity_(capacity) {
        buffer_.resize(capacity_);
    }

    // Called by the PRODUCER thread
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % capacity_;
        
        // Acquire memory barrier ensures we see the most recent head update
        // made by the consumer thread.
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue is full
        }
        
        buffer_[current_tail] = item;
        
        // Release memory barrier ensures the item write is visible to the 
        // consumer before the tail pointer update becomes visible.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Called by the CONSUMER thread
    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        // Acquire memory barrier ensures we see the most recent tail update
        // made by the producer thread.
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue is empty
        }
        
        item = buffer_[current_head];
        
        // Release memory barrier ensures the item read completes before
        // the head pointer update becomes visible to the producer.
        head_.store((current_head + 1) % capacity_, std::memory_order_release);
        return true;
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    
    // alignas(64) ensures these sit on different CPU cache lines.
    // If they shared a cache line, the Producer updating the tail would 
    // invalidate the Consumer's cache line containing the head, causing 
    // massive performance degradation (false sharing).
    alignas(64) std::atomic<size_t> tail_{0}; // Written by Producer
    alignas(64) std::atomic<size_t> head_{0}; // Written by Consumer
};
