
#pragma once

#include "ParallelUtils.h"
//#include <concurrentqueue.h>

#include <vector>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <limits.h>
#include <list>

namespace souffle {


/* a blazin' fast concurrent vector. */
template <class T>
class BlockList {
    const size_t BLOCKBITS = size_t(16);
    const size_t BLOCKSIZE = size_t(1) << BLOCKBITS;
    std::list<T*> listData;

    std::atomic<size_t> m_size;
    // how large each new allocation will be
    size_t allocsize = BLOCKSIZE;
    std::atomic<size_t> container_size;


    static constexpr size_t MAXINDEXPOWER = 64;
    // supports 64 node long linked list & with doubling
    // each index points to the respective indexed linked list node
    // a length of 64 means this data structure can store >2^64 values.
    // depending on the starting block size (default 2^16)
    T* blockLookupTable[MAXINDEXPOWER];

    // for parallel node insertions
    mutable SpinLock sl;

    /**
     * Free the arrays allocated within the linked list nodes
     */
    void freeList() {
        auto it = listData.begin();
        while (it != listData.end()) {
            delete[] * it;
            ++it;
        }
        listData.clear();
    }

    /**
     * Update blockLookupTable to point to the correct parts of listData
     * This should only be run once - usually in a copy or move ctor.
     */
    void initLookupTable() {
        size_t index = 0;
        for (T* block : listData) {
            this->blockLookupTable[index] = block;
        }
    }

public:

    void trim() {}
    BlockList() : listData() {
        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;

        m_size.store(0);
        container_size.store(0);
    }

    BlockList(size_t initialbitsize) : BLOCKBITS(initialbitsize) {
        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;

        m_size.store(0);
        container_size.store(0);
    }

    /** copy constructor */
    BlockList(const BlockList& other) : BLOCKBITS(other.BLOCKBITS) {
        freeList();
        listData.clear();
        this->m_size.store(other.m_size);
        this->container_size.store(other.container_size);

        size_t currAllocSize = BLOCKSIZE;
        for (auto& data : other.listData) {
            listData.push_back(new T[currAllocSize]);
            // copy their data into ours
            memcpy(listData.back(), data, currAllocSize);
            // double allocation size for the next block
            currAllocSize <<= 1;
        }

        initLookupTable();

        allocsize = currAllocSize;
    }

    /** move constructor */
    BlockList(BlockList&& other) : BlockList() {
        std::swap(listData, other.listData);
        std::swap(allocsize, other.allocsize);

        // move atomics
        size_t tempS = this->m_size.load();
        this->m_size.store(other.m_size.load());
        other.m_size.store(tempS);

        size_t tempC = this->container_size.load();
        this->container_size.store(other.container_size.load());
        other.container_size.store(tempC);

        initLookupTable();
    }

    BlockList& operator=(BlockList other) {
        std::swap(this->listData, other.listData);
        std::swap(allocsize, other.allocsize);

        // move atomics
        size_t temp = this->m_size.load();
        this->m_size.store(other.m_size.load());
        other.m_size.store(temp);

        size_t tempC = this->container_size.load();
        this->container_size.store(other.container_size.load());
        other.container_size.store(tempC);

        initLookupTable();

        return *this;
    }

    ~BlockList() {
        freeList();
    }

    /**
     * Well, returns the number of nodes exist within the list + number of nodes queued to be inserted
     *  The reason for this, is that there may be many nodes queued up
     *  that haven't had time to had containers created and updated
     * @return the number of nodes exist within the list + number of nodes queued to be inserted
     */
    inline size_t size() const {
        return m_size.load();
    };

    inline size_t containerSize() const {
        return container_size.load();
    }

    inline T* getBlock(size_t blocknum) const {
        return this->blockLookupTable[blocknum];
    }

    /**
     * Create a value in the blocklist & return its position.
     * @return index of New Node
     */
    size_t createNode() {
        size_t new_index = m_size.fetch_add(1, std::memory_order_relaxed);

        // spin and try and insert the node at the correct index (we may need to construct a new block)

        // if we don't have a valid index to store this element
        if (container_size.load() < new_index + 1) {
            sl.lock();

            // double check & add as many blocks as necessary
            // (although, I do hope this never loops multiple times, as it means there's at least
            //      BLOCKSIZE threads concurrently writing...)
            while (container_size.load() < new_index + 1) {
                listData.push_back(new T[allocsize]);
                // update lookup table
                this->blockLookupTable[listData.size() - 1] = listData.back();
                container_size += allocsize;
                // next time our linked list node will have twice the capacity
                allocsize <<= 1;
            }

            sl.unlock();
        }

        return new_index;
    }

    void append(T val) {
        size_t new_index = m_size.fetch_add(1, std::memory_order_relaxed);

        // spin and try and insert the node at the correct index (we may need to construct a new block)

        // if we don't have a valid index to store this element
        if (container_size.load() < new_index + 1) {
            sl.lock();

            // double check & add as many blocks as necessary
            // (although, I do hope this never loops multiple times, as it means there's at least
            //      BLOCKSIZE threads concurrently writing...)
            while (container_size.load() < new_index + 1) {
                listData.push_back(new T[allocsize]);
                // update lookup table
                this->blockLookupTable[listData.size() - 1] = listData.back();
                container_size += allocsize;
                // next time our linked list node will have twice the capacity
                allocsize <<= 1;
            }

            sl.unlock();
        }
        this->get(new_index) = val;
    }

    /**
     * Retrieve a reference to the stored value at index
     * @param index position to search
     * @return the value at index
     */
    inline T& get(size_t index) const {
        size_t nindex = index + BLOCKSIZE;
        size_t blockNum = (63 - __builtin_clzll(nindex)) - BLOCKBITS;
        size_t blockInd = (nindex) & ((1 << (blockNum+BLOCKBITS)) - 1);
        return this->getBlock(blockNum)[blockInd];
    }

    /**
     * Clear all elements from the BlockList
     */
    void clear() {
        freeList();
        m_size = 0;
        container_size = 0;
    }

    class iterator : std::iterator<std::forward_iterator_tag, T> {
        size_t cIndex = 0;
        BlockList* bl;

    public:
        // default ctor, to silence
        iterator(){};

        /* begin iterator for iterating over all elements */
        iterator(BlockList* bl) : bl(bl){};
        /* ender iterator for marking the end of the iteration */
        iterator(BlockList* bl, size_t beginInd) : cIndex(beginInd), bl(bl){};

        T operator*() {
            return bl->get(cIndex);
        };
        const T operator*() const {
            return bl->get(cIndex);
        };

        iterator& operator++(int) {
            ++cIndex;
            return *this;
        };

        iterator operator++() {
            iterator ret(*this);
            ++cIndex;
            return ret;
        };

        friend bool operator==(const iterator& x, const iterator& y) {
            return x.cIndex == y.cIndex && x.bl == y.bl;
        };

        friend bool operator!=(const iterator& x, const iterator& y) {
            return !(x == y);
        };
    };

    iterator begin() {
        return iterator(this);
    };
    iterator end() {
        return iterator(this, size());
    };
};



template<>
void BlockList<std::atomic<size_t>>::insertAt(size_t index, const std::atomic<size_t>& value) {
    // exact same logic as createNode, but instead just cares about expanding til our index fits
    // not if a newly added node will fit.
    while (container_size.load() < index + 1) {
        sl.lock();
        // just in case
        while (container_size.load() < index + 1) {
            // create the new block
            listData.push_back(new std::atomic<size_t>[allocsize]);
            this->blockLookupTable[listData.size() - 1] = listData.back();
            container_size += allocsize;
            // double the size of the next allocated block
            allocsize <<= 1;
        }

        sl.unlock();
    }

        size_t nindex = index + BLOCKSIZE;
        size_t blockNum = (63 - __builtin_clzll(nindex)) - BLOCKBITS;
        size_t blockInd = (nindex) & ((1 << (blockNum+BLOCKBITS)) - 1);
        // store the value in its correct location
        this->getBlock(blockNum)[blockInd].store(value.load(std::memory_order_relaxed));
    }

///* a blazin' fast concurrent vector. */
//template <class T>
//class BlockListRemovable {
//    const size_t BLOCKBITS = size_t(16);
//    const size_t BLOCKSIZE = size_t(1) << BLOCKBITS;
//    std::list<T*> listData;
//
//    std::atomic<size_t> m_size;
//    // how large each new allocation will be
//    size_t allocsize = BLOCKSIZE;
//    std::atomic<size_t> container_size;
//
//    moodycamel::ConcurrentQueue<size_t> removedIndexes;
//
//    static constexpr size_t MAXINDEXPOWER = 64;
//    // supports 64 node long linked list & with doubling
//    // each index points to the respective indexed linked list node
//    // a length of 64 means this data structure can store >2^64 values.
//    // depending on the starting block size (default 2^16)
//    T* blockLookupTable[MAXINDEXPOWER];
//
//    // for parallel node insertions
//    mutable SpinLock sl;
//
//    /**
//     * Free the arrays allocated within the linked list nodes
//     */
//    void freeList() {
//        auto it = listData.begin();
//        while (it != listData.end()) {
//            delete[] * it;
//            ++it;
//        }
//        listData.clear();
//    }
//
//    /**
//     * Update blockLookupTable to point to the correct parts of listData
//     * This should only be run once - usually in a copy or move ctor.
//     */
//    void initLookupTable() {
//        size_t index = 0;
//        for (T* block : listData) {
//            this->blockLookupTable[index] = block;
//        }
//    }
//
//public:
//    BlockListRemovable() : listData() {
//        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;
//
//        m_size.store(0);
//        container_size.store(0);
//    }
//
//    BlockListRemovable(size_t initialbitsize) : BLOCKBITS(initialbitsize) {
//        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;
//
//        m_size.store(0);
//        container_size.store(0);
//    }
//
//    /** copy constructor */
//    BlockListRemovable(const BlockListRemovable& other) {
//        freeList();
//        listData.clear();
//        this->m_size.store(other.m_size);
//        this->container_size.store(other.container_size);
//
//        auto iterCopied = other.listData.begin();
//
//        // copy each linked node array into this list - be careful! the size doubles each node
//        size_t currAllocsize = BLOCKSIZE;
//        while (iterCopied != other.listData.end()) {
//            listData.push_back(new T[currAllocsize]);
//            memcpy(listData.back(), *iterCopied, currAllocsize);
//            // double the allocation size for the next block
//            currAllocsize <<= 1;
//        }
//
//        initLookupTable();
//
//        allocsize = currAllocsize;
//    }
//
//    /** move constructor */
//    BlockListRemovable(BlockListRemovable&& other) : BlockListRemovable() {
//        std::swap(listData, other.listData);
//        std::swap(allocsize, other.allocsize);
//
//        // move atomics
//        size_t tempS = this->m_size.load();
//        this->m_size.store(other.m_size.load());
//        other.m_size.store(tempS);
//
//        size_t tempC = this->container_size.load();
//        this->container_size.store(other.container_size.load());
//        other.container_size.store(tempC);
//
//        initLookupTable();
//    }
//
//    BlockListRemovable& operator=(BlockListRemovable other) {
//        std::swap(this->listData, other.listData);
//        std::swap(allocsize, other.allocsize);
//
//        // move atomics
//        size_t temp = this->m_size.load();
//        this->m_size.store(other.m_size.load());
//        other.m_size.store(temp);
//
//        size_t tempC = this->container_size.load();
//        this->container_size.store(other.container_size.load());
//        other.container_size.store(tempC);
//
//        initLookupTable();
//
//        return *this;
//    }
//
//    ~BlockListRemovable() {
//        freeList();
//    }
//
//    /**
//     * Well, returns the number of nodes exist within the list + number of nodes queued to be inserted
//     *  The reason for this, is that there may be many nodes queued up
//     *  that haven't had time to had containers created and updated
//     * @return the number of nodes exist within the list + number of nodes queued to be inserted
//     */
//    inline size_t size() const {
//        return m_size.load();
//    };
//
//    inline size_t containerSize() const {
//        return container_size.load();
//    }
//
//    inline T* getBlock(size_t blocknum) const {
//        return this->blockLookupTable[blocknum];
//    }
//
//    /**
//     * Create a value in the blocklist & return the new Node & its position.
//     * @return std::pair<New Node, Index of New Node>
//     */
//    size_t createNode() {
//        // use if we can
//        size_t new_index;
//        if (removedIndexes.try_dequeue(new_index)) {
//            return new_index;
//        }
//
//        new_index = m_size.fetch_add(1, std::memory_order_relaxed);
//
//        // spin and try and insert the node at the correct index (we may need to construct a new block)
//
//        // if we don't have a valid index to store this element
//        if (container_size.load() < new_index + 1) {
//            sl.lock();
//
//            // double check & add as many blocks as necessary
//            // (although, I do hope this never loops multiple times, as it means there's at least
//            //      BLOCKSIZE threads concurrently writing...)
//            while (container_size.load() < new_index + 1) {
//                listData.push_back(new T[allocsize]);
//                // update lookup table
//                this->blockLookupTable[listData.size() - 1] = listData.back();
//                container_size += allocsize;
//                // next time our linked list node will have twice the capacity
//                allocsize <<= 1;
//            }
//
//            sl.unlock();
//        }
//
//        return new_index;
//    }
//
//    void append(T val) {
//        size_t new_index = m_size.fetch_add(1, std::memory_order_relaxed);
//
//        // spin and try and insert the node at the correct index (we may need to construct a new block)
//
//        // if we don't have a valid index to store this element
//        if (container_size.load() < new_index + 1) {
//            sl.lock();
//
//            // double check & add as many blocks as necessary
//            // (although, I do hope this never loops multiple times, as it means there's at least
//            //      BLOCKSIZE threads concurrently writing...)
//            while (container_size.load() < new_index + 1) {
//                listData.push_back(new T[allocsize]);
//                // update lookup table
//                this->blockLookupTable[listData.size() - 1] = listData.back();
//                container_size += allocsize;
//                // next time our linked list node will have twice the capacity
//                allocsize <<= 1;
//            }
//
//            sl.unlock();
//        }
//        this->get(new_index) = val;
//    }
//
//    /**
//     * Insert a value at a specific index, expanding the data structure if necessary.
//     *  Warning: this will "waste" inbetween elements, if you do not know their index,
//     *      and do not set them with insertAt
//     * @param index  the index inside the blocklist
//     * @param value  the value to set inside that index
//     */
//    void insertAt(size_t index, T value) {
//        // exact same logic as createNode, but instead just cares about expanding til our index fits
//        // not if a newly added node will fit.
//        while (container_size.load() < index + 1) {
//            sl.lock();
//            // just in case
//            while (container_size.load() < index + 1) {
//                // create the new block
//                listData.push_back(new T[allocsize]);
//                this->blockLookupTable[listData.size() - 1] = listData.back();
//                container_size += allocsize;
//                // double the size of the next allocated block
//                allocsize <<= 1;
//            }
//
//            sl.unlock();
//        }
//
//        size_t nindex = index + BLOCKSIZE;
//        size_t blockNum = (63 - __builtin_clzll(nindex)) - BLOCKBITS;
//        size_t blockInd = (nindex) & ((1 << (blockNum+BLOCKBITS)) - 1);
//        // store the value in its correct location
//        this->getBlock(blockNum)[blockInd] = value;
//    }
//
//    /**
//     * A function that you probably shouldn't be calling. (Used by the hashmap)
//     * Adds another block to our underlying container
//     * @return the size of the container that it at least is
//     */
//    size_t addBlock() {
//        sl.lock();
//
//        listData.push_back(new T[allocsize]());
//        this->blockLookupTable[listData.size() - 1] = listData.back();
//        container_size += allocsize;
//        // double the size of the next allocated block
//        allocsize <<= 1;
//
//        sl.unlock();
//
//        return container_size.load();
//    }
//
//    /**
//     * Retrieve a reference to the stored value at index
//     * @param index position to search
//     * @return the value at index
//     */
//    inline T& get(size_t index) const {
//        size_t nindex = index + BLOCKSIZE;
//        size_t blockNum = (63 - __builtin_clzll(nindex)) - BLOCKBITS;
//        size_t blockInd = (nindex) & ((1 << (blockNum+BLOCKBITS)) - 1);
//        return this->getBlock(blockNum)[blockInd];
//    }
//
//    /**
//     * Clear all elements from the BlockList
//     */
//    void clear() {
//        freeList();
//        m_size = 0;
//        container_size = 0;
//    }
//
//    class iterator : std::iterator<std::forward_iterator_tag, T> {
//        size_t cIndex = 0;
//        BlockList* bl;
//
//    public:
//        // default ctor, to silence
//        iterator(){};
//
//        /* begin iterator for iterating over all elements */
//        iterator(BlockList* bl) : bl(bl){};
//        /* ender iterator for marking the end of the iteration */
//        iterator(BlockList* bl, size_t beginInd) : cIndex(beginInd), bl(bl){};
//
//        T operator*() {
//            return bl->get(cIndex);
//        };
//        const T operator*() const {
//            return bl->get(cIndex);
//        };
//
//        iterator& operator++(int) {
//            ++cIndex;
//            return *this;
//        };
//
//        iterator operator++() {
//            iterator ret(*this);
//            ++cIndex;
//            return ret;
//        };
//
//        friend bool operator==(const iterator& x, const iterator& y) {
//            return x.cIndex == y.cIndex && x.bl == y.bl;
//        };
//
//        friend bool operator!=(const iterator& x, const iterator& y) {
//            return !(x == y);
//        };
//    };
//
//    iterator begin() {
//        return iterator(this);
//    };
//    iterator end() {
//        return iterator(this, size());
//    };
//};

// TMP: remuv
///* a blazin' fast concurrent vector - with deletion! */
//template <class T>
//class BlockListRemovable {
//    const size_t BLOCKBITS = size_t(16);
//    const size_t BLOCKSIZE = size_t(1) << BLOCKBITS;
//
//    std::atomic<size_t> m_indexCounter;
//    // how large each new allocation will be
//    //std::atomic<size_t> container_size;
//    //size_t num_containers = 0;
//
//    // when we delete, we store 'unused' indexes here
//    moodycamel::ConcurrentQueue<size_t> removedIndexes;
//
//    static constexpr size_t MAXINDEXPOWER = 64;
//    // supports 64 node long linked list & with doubling
//    // each index points to the respective indexed linked list node
//    // a length of 64 means this data structure can store >2^64 values.
//    // depending on the starting block size (default 2^16)
//    std::atomic<T*> blockLookupTable[MAXINDEXPOWER]{};
//
//    // for parallel node insertions
//    mutable SpinLock sl;
//
//    /**
//     * Free the arrays allocated within the linked list nodes
//     */
//    void freeList() {
//        for (size_t i = 0; i < MAXINDEXPOWER; ++i) {
//            if (blockLookupTable[i] != nullptr) {
//                delete[] blockLookupTable[i];
//                blockLookupTable[i] = nullptr;
//            }
//        }
//    }
//
//public:
//    BlockListRemovable() {
//        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;
//        m_indexCounter = 0;
//
//        //m_size.store(0);
//        //container_size.store(0);
//    }
//
//    BlockListRemovable(size_t initialbitsize) : BLOCKBITS(initialbitsize) {
//        for (size_t i = 0; i < MAXINDEXPOWER; ++i) blockLookupTable[i] = nullptr;
//        m_indexCounter = 0;
//
//        //m_size.store(0);
//        //container_size.store(0);
//    }
//
//    ~BlockListRemovable() {
//        freeList();
//    }
//
//    /**
//     * Well, returns the number of nodes exist within the list + number of nodes queued to be inserted
//     *  The reason for this, is that there may be many nodes queued up
//     *  that haven't had time to had containers created and updated
//     * @return the number of nodes exist within the list + number of nodes queued to be inserted
//     */
//    inline size_t size() {
//        trim();
//        return m_indexCounter;
//        //return m_size.load();
//    };
//
//    /* move elements so that everything is contiguous */
//    void trim() {
//        std::vector<size_t> indexes;
//
//        // TODO:
//        size_t ind;
//        while (removedIndexes.try_dequeue(ind)) {
//            indexes.push_back(ind);
//        }
//
//        for (auto& index : indexes) {
//            // do nothing if we need to swap after the last element
//            if (index > m_indexCounter - 1) {
//
//            // need to ensure that the last element isn't already deleted/in our deletion list
//            } else if (std::find(indexes.begin(), indexes.end(), m_indexCounter - 1) == indexes.end()) {
//                this->get(index).store(this->get(m_indexCounter-1).load());
//                m_indexCounter--;
//            } else {
//                // if it is, then obv we just move back
//                m_indexCounter--;
//            }
//        }
//    }
//
//    void remove(size_t index) {
//        removedIndexes.enqueue(index);
//    }
//
//    //inline size_t containerSize() const {
//    //    return container_size.load();
//    //}
//
//    inline T* getBlock(size_t blocknum) const {
//        return this->blockLookupTable[blocknum];
//    }
//
//    size_t createNode() {
//        size_t new_index;
//        if (removedIndexes.try_dequeue(new_index)) {
//            return new_index;
//        }
//
//        new_index = m_indexCounter.fetch_add(1, std::memory_order_relaxed);
//        createBlockIfIndexEmpty(new_index);
//
//        return new_index;
//
//    }
//
//    void createBlockIfIndexEmpty(size_t index) {
//        // lookup the block
//        size_t nindex = index + BLOCKSIZE;
//        size_t blockNum = (63 - __builtin_clzll(nindex)) - BLOCKBITS;
//
//        // if our spot isn't allocated
//        if (this->blockLookupTable[blockNum].load() == nullptr) {
//            sl.lock();
//            if (this->blockLookupTable[blockNum].load() == nullptr) {
//                // what is the size again
//                size_t bsize = 1 << (blockNum + BLOCKBITS);                 
//                this->blockLookupTable[blockNum] = new T[bsize];
//            }
//            sl.unlock();
//        }
//    }
//
//    void insertAt(size_t index, T value) {
//        createBlockIfIndexEmpty(index);
//
//        this->get(index) = value;
//    }
//
//    /**
//     * Create a value in the blocklist & return the new Node & its position.
//     * @return std::pair<New Node, Index of New Node>
//     */
//    //size_t createNode() {
////        // use if we can
////        size_t new_index;
////        if (removedIndexes.try_dequeue(new_index)) {
////            return new_index;
////        }
////
////        new_index = m_size.fetch_add(1, std::memory_order_relaxed);
////
////        // spin and try and insert the node at the correct index (we may need to construct a new block)
////
////        // if we don't have a valid index to store this element
////        if (container_size.load() < new_index + 1) {
////            sl.lock();
////
////            // double check & add as many blocks as necessary
////            // (although, I do hope this never loops multiple times, as it means there's at least
////            //      BLOCKSIZE threads concurrently writing...)
////            while (container_size.load() < new_index + 1) {
////                listData.push_back(new T[allocsize]);
////                // update lookup table
////                this->blockLookupTable[listData.size() - 1] = listData.back();
////                container_size += allocsize;
////                // next time our linked list node will have twice the capacity
////                allocsize <<= 1;
////            }
////
////            sl.unlock();
////        }
////
////        return new_index;
////    }
////
////        // use if we can
////        size_t new_index;
////        if (removedIndexes.try_dequeue(new_index)) {
////            return new_index;
////        }
////
////        new_index = m_size.fetch_add(1, std::memory_order_relaxed);
////        insertAt(new_index, {});
////
////        // spin and try and insert the node at the correct index (we may need to construct a new block)
////
////        // if we don't have a valid index to store this element
////        if (container_size.load() < new_index + 1) {
////            sl.lock();
////
////            // double check & add as many blocks as necessary
////            // (although, I do hope this never loops multiple times, as it means there's at least
////            //      BLOCKSIZE threads concurrently writing...)
////            while (container_size.load() < new_index + 1) {
////                blockLookupTable[num_containers] = new T[1 << num_containers];
////                container_size += 1 << num_containers;
////                ++num_containers;
////            }
////
////            sl.unlock();
////        }
////
////        return new_index;
////    }
//
//    /* blindly delete the element - if you delete an element twice, you're up shit creek */
//    void delNode(size_t index) {
//        removedIndexes.enqueue(index);
//    }
//
//    /**
//     * Retrieve a reference to the stored value at index
//     * @param index position to search
//     * @return the value at index
//     */
//    inline T& get(size_t index) const {
//        // supa fast 2^16 size first block
//        size_t nindex = index + BLOCKSIZE;
//        size_t blockNum = (63 - __builtin_clzll(nindex));
//        size_t blockInd = (nindex) & ((1 << blockNum) - 1);
//        return this->getBlock(blockNum - BLOCKBITS)[blockInd];
//    }
//
//    /**
//     * Clear all elements from the BlockList
//     */
//    void clear() {
//        freeList();
//        m_indexCounter = 0;
//
//        // just delete all the queue
//        size_t dummy;
//        while (removedIndexes.try_dequeue(dummy));
//        //m_size = 0;
//        //container_size = 0;
//    }
//
//    //class iterator : std::iterator<std::forward_iterator_tag, T> {
//    //    size_t cIndex = 0;
//    //    BlockListRemovable* bl;
//    //    bool isEnd = false;
//
//    //public:
//    //    // default ctor, to silence
//    //    iterator(){};
//
//    //    /* begin iterator for iterating over all elements */
//    //    iterator(BlockListRemovable* bl) : bl(bl){};
//    //    /* ender iterator for marking the end of the iteration */
//    //    iterator(BlockListRemovable* bl, bool) : bl(bl), isEnd(true) {};
//
//    //    T operator*() {
//    //        return bl->get(cIndex);
//    //    };
//    //    const T operator*() const {
//    //        return bl->get(cIndex);
//    //    };
//
//    //    iterator& operator++(int) {
//    //        ++cIndex;
//    //        if (bl->get(cIndex) == nullptr) {
//    //            isEnd = 
//    //        }
//    //        return *this;
//    //    };
//
//    //    iterator operator++() {
//    //        iterator ret(*this);
//    //        ++cIndex;
//    //        return ret;
//    //    };
//
//    //    friend bool operator==(const iterator& x, const iterator& y) {
//    //        return x.isEnd == y.isEnd && x.bl == y.bl;
//    //    };
//
//    //    friend bool operator!=(const iterator& x, const iterator& y) {
//    //        return !(x == y);
//    //    };
//    //};
//
//    //iterator begin() {
//    //    return iterator(this);
//    //};
//    //iterator end() {
//    //    return iterator(this, true);
//    //};
//};

}

