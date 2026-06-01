#pragma once

#include <cstddef>
#include <vector>
#include <memory>
#include <new>
#include <utility>

namespace orderbook {

// growing,fixed-size object pool
// -pre-allocates memory in chunks
// -allocate()/deallocate() are O(1) pointer swaps (no malloc in hot path)
// -uses an intrusive free list (free slots store the "next free" pointer)
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t initial_capacity = 4096,
                        size_t chunk_size = 4096)
        : free_head_(nullptr)
        , chunk_size_(chunk_size > 0 ? chunk_size : 1)
        , allocated_(0)
        , capacity_(0)
    {
        addChunk(initial_capacity > 0 ? initial_capacity : chunk_size_);
    }

    // the pool owns raw memory
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // handing out a slot and construct a T in place with the given args
    template <typename... Args>
    T* allocate(Args&&... args) {
        if (!free_head_) {
            addChunk(chunk_size_);   // grow on demand
        }
        Slot* slot = free_head_;
        free_head_ = slot->next;
        ++allocated_;
        // constructing T directly into the slot's storage
        return new (&slot->storage) T(std::forward<Args>(args)...);
    }

    // destroy the T and return its slot to the free list
    void deallocate(T* obj) {
        if (!obj) return;
        obj->~T();                                  // run destructor
        Slot* slot = reinterpret_cast<Slot*>(obj);      // slot addr == storage addr
        slot->next = free_head_;
        free_head_ = slot;
        --allocated_;
    }

    size_t allocated() const { return allocated_; }
    size_t capacity()  const { return capacity_; }
    size_t available() const { return capacity_ - allocated_; }
    size_t numChunks() const { return chunks_.size(); }

private:
    // slot will hold either a live T (storage) or a free-list pointer (next)
    // the union means the free list costs zero extra memory
    union Slot {
        alignas(T) std::byte storage[sizeof(T)];
        Slot* next;
    };

    void addChunk(size_t n) {
        auto chunk = std::make_unique<Slot[]>(n);
        // threading every new slot into the front of the free list
        for (size_t i = 0; i < n; ++i) {
            chunk[i].next = free_head_;
            free_head_ = &chunk[i];
        }
        capacity_ += n;
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<Slot[]>> chunks_;  // owns all memory
    Slot*  free_head_;
    size_t chunk_size_;
    size_t allocated_;
    size_t capacity_;
};

} // namespace orderbook