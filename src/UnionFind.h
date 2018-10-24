#pragma once

#include "BlockList.h"
#include "PiggyList.h"

#include <atomic>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "DisjointSet.h"

#include "eqrelbtree.h"

// branch predictor hacks
#define unlikely(x) __builtin_expect((x), 0)
#define likely(x) __builtin_expect((x), 1)

namespace souffle {

template<typename StorePair>
struct EqrelMapComparator {
    int operator()(const StorePair a, const StorePair b) {
        if (a.first < b.first) return -1;
        else if (b.first < a.first) return 1;
        else return 0;
    }   

    bool less(const StorePair a, const StorePair b) {
        return operator()(a,b) < 0;
    }

    bool equal(const StorePair a, const StorePair b) {
        return operator()(a,b) == 0;
    }
};

template <typename SparseDomain>
class SparseDisjointSet {
    DisjointSet ds;

    template <typename TupleType>
    friend class BinaryRelation;

    typedef std::pair<SparseDomain, parent_t> PairStore;
    typedef souffle::IncrementingBTreeSet<PairStore, EqrelMapComparator<PairStore>> SparseMap;
    typedef souffle::RandomInsertPiggyList<SparseDomain> DenseMap;
    // TODO: do we use this?
    typename SparseMap::operation_hints last_ins;

    SparseMap sparseToDenseMap;
    // mapping from union-find val to souffle, union-find encoded as index
    DenseMap denseToSparseMap;

private:
    /**
     * Retrieve dense encoding, adding it in if non-existent
     * @param in the sparse value
     * @return the corresponding dense value
     */
    parent_t toDense(const SparseDomain in) {
        // insert into the mapping - if the key doesn't exist (in), the function will be called
        // and a dense value will be created for it
        parent_t densi = sparseToDenseMap.insert({in, 0}, last_ins, [&](typename PairStore::first_type d){ 
                parent_t c2 = DisjointSet::b2p(this->ds.makeNode()); 
                this->denseToSparseMap.insertAt(c2, d);
                return c2;
        });
        return densi;
    }

public:
    // warning! not thread safe, do not perform copy operations
    // XXX: is this actually a copy-op? Doesn't look like it...
    SparseDisjointSet& operator=(const SparseDisjointSet& old) {
        if (&old == this) return *this;

        ds = old.ds;
        sparseToDenseMap = old.sparseToDenseMap;
        denseToSparseMap = old.denseToSparseMap;

        return *this;
    }

    /**
     * For the given dense value, return the associated sparse value
     *   Undefined behaviour if dense value not in set
     * @param in the supplied dense value
     * @return the sparse value from the denseToSparseMap
     */
    inline const SparseDomain toSparse(const parent_t in) const {
        return denseToSparseMap.get(in);
    };

    /* a wrapper to enable checking in the sparse set - however also adds them if not already existing */
    inline bool sameSet(SparseDomain x, SparseDomain y) {
        return ds.sameSet(toDense(x), toDense(y));
    };
    /* finds the node in the underlying disjoint set, adding the node if non-existent */
    inline SparseDomain findNode(SparseDomain x) {
        return toSparse(ds.findNode(toDense(x)));
    };
    /* union the nodes, add if not existing */
    inline void unionNodes(SparseDomain x, SparseDomain y) {
        ds.unionNodes(toDense(x), toDense(y));
    };

    inline std::size_t size() {
        return ds.size();
    };

    // TODO: documentation
    void clear() {
        ds.clear();
        sparseToDenseMap.clear();
        denseToSparseMap.clear();
    }

    /* wrapper for node creation */
    inline void makeNode(SparseDomain val) {
        // dense has the behaviour of creating if not exists.
        toDense(val);
    };

    /* whether we the supplied node exists */
    inline bool nodeExists(const SparseDomain val) const {
        return sparseToDenseMap.contains({val, 0});
    };

    inline bool contains(SparseDomain v1, SparseDomain v2) {
        if (nodeExists(v1) && nodeExists(v2)) {
            return sameSet(v1, v2);
        }
        return false;
    }
};
}
