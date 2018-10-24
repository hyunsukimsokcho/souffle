#pragma once

#include "UnionFind.h"
#include "Util.h"
#include <algorithm>
#include <exception>
#include <unordered_map>
#include <utility>

namespace souffle {
template <typename TupleType>
class BinaryRelation {
    typedef typename TupleType::value_type DomainInt;
    enum { arity = TupleType::arity };

    // marked as mutable due to difficulties with the const enforcement via the Relation API
    // const operations *may* safely change internal state (i.e. collapse djset forest)
    mutable souffle::SparseDisjointSet<DomainInt> sds;

    // read/write lock on equivalencePartition 
    mutable souffle::shared_mutex statesLock;

    // mapping from representative to disjoint set
    // just a cache, essentially, used for iteration over
    typedef souffle::BlockList<DomainInt> StatesList;
    typedef StatesList* StatesBucket;
    typedef std::pair<DomainInt, StatesBucket> StorePair;
    typedef souffle::IncrementingBTreeSet<StorePair, souffle::EqrelMapComparator<StorePair>> StatesMap;
    mutable StatesMap equivalencePartition;
    // whether the cache is stale
    mutable std::atomic<bool> statesMapStale;

public:
    BinaryRelation() : statesMapStale(false) {};

    /**
     * TODO: implement this operation_hint class
     * A collection of operation hints speeding up some of the involved operations
     * by exploiting temporal locality.
     */
    struct operation_hints {
        // resets all hints (to be triggered e.g. when deleting nodes)
        void clear() {}
    };

    /**
     * Insert the two values symbolically as a binary relation
     * @param x node to be added/paired
     * @param y node to be added/paired
     * @return true if the pair is new to the data structure
     */
    bool insert(DomainInt x, DomainInt y) {
        operation_hints z;
        return insert(x, y, z);
    };

    /**
     * Insert the two values symbolically as a binary relation
     * @param x node to be added/paired
     * @param y node to be added/paired
     * @param z the hints to where the pair should be inserted (not applicable atm)
     * @return true if the pair is new to the data structure
     */
    bool insert(DomainInt x, DomainInt y, operation_hints) {
        // indicate that iterators will have to generate on request
        this->statesMapStale.store(true, std::memory_order_relaxed);
        bool retval = contains(x, y);
        sds.unionNodes(x, y);
        return retval;
    }

    /**
     * inserts all nodes from the other relation into this one
     * @param other the binary relation from which to add nodes from
     */
    void insertAll(const BinaryRelation<TupleType>& other) {
        other.genAllDisjointSetLists();

        // iterate over partitions at a time
        for (typename StatesMap::chunk it : other.equivalencePartition.getChunks(MAX_THREADS)) {
            for (auto& p : it) {
                DomainInt rep = p.first;
                StatesList pl = *p.second;
                const size_t ksize = pl.size();
                for (size_t i = 0; i < ksize; ++i) {
                    this->sds.unionNodes(rep, pl.get(i));
                }
            }
        }
        // invalidate iterators unconditionally
        this->statesMapStale.store(true, std::memory_order_relaxed);
    }

    /**
     * Conditionally inserts from another BinaryRelation to this one
     *  if any disjoint sets intersect across the two (i.e. if A is in both
     *  this and the other one, insert everything that is alongside A in
     *  the second BR into this one.
     *  @param the other binaryrelation to read and insert from
     *  @return the newly synthesised extended relation
     */
    void extend(const BinaryRelation<TupleType>& other) {
        this->genAllDisjointSetLists();
        other.genAllDisjointSetLists();

        BinaryRelation<TupleType> synthesised;
        BlockList<DomainInt> worklist;

        // add elements that exist in both sets to our worklist
        for (DomainInt& el : other.sds) {
            if (this->sds.nodeExists(el)) {
                worklist.append(el);
            }
        }
        // from the old relation, insert all disjoint sets that intersect with something in the worklist
        for (DomainInt& el : worklist) {
            if (!synthesised.sds.nodeExists(el)) {
                for (TupleType& i : this->anteriorit(el)) {
                    synthesised.insert(el, i.second);
                }
            }
        }

        // insert all relations from the new relation into this one
        synthesised.insertAll(other);

        std::swap(synthesised, *this);
    }

protected:
    bool containsElement(DomainInt e) const {
        return this->sds.nodeExists(e);
    }

public:
    /**
     * Returns whether there exists a pair with these two nodes
     * @param x front of pair
     * @param y back of pair
     */
    bool contains(DomainInt x, DomainInt y) const {
        return sds.contains(x, y);
    }

    /**
     * Empty the relation
     */
    void clear() {
        statesLock.lock();

        sds.clear();
        equivalencePartition.clear();

        statesLock.unlock();
    }

    /**
     * Size of relation
     * @return the sum of the number of pairs per disjoint set
     */
    size_t size() const {
        genAllDisjointSetLists();

        statesLock.lock_shared();

        size_t retVal = 0;
        for (auto& e : this->equivalencePartition) {
            const size_t s = e.second->size();
            retVal += s*s;
        }

        statesLock.unlock_shared();
        return retVal;
    }

private:
    // TODO: documentation (i.e. that this lazily makes the rep lists)
    // also, warning: if this is called during insertion, this will break... probably
    // all this function does, is insert each disjoint set into a separate key in a hashmap. We want this to
    // be as lazy as possible too.
    void genAllDisjointSetLists() const {
        statesLock.lock();

        // no need to generate again, already done.
        if (!this->statesMapStale.load(std::memory_order_acquire)) {
            statesLock.unlock();
            return;
        }

        // btree version
        equivalencePartition.clear();

        size_t dSetSize = this->sds.ds.a_blocks.size();
        for (size_t i = 0; i < dSetSize; ++i) {
            typename TupleType::value_type sparseVal = this->sds.toSparse(i);
            parent_t rep = this->sds.findNode(sparseVal);

            // XXX: you fuckin idiot pnappa, of course these will leak - there's no cleanup of these in the clear()
            // in the btree.....
            // I guess a solution can be to use unique_ptrs?
            
            StatesList* mapList = equivalencePartition.insert({rep, nullptr}, [&](DomainInt) { 
                    StatesList* r = new StatesList(1); 
                    return r; 
                    });
            mapList->append(sparseVal);
        }

        statesMapStale.store(false, std::memory_order_release);
        statesLock.unlock();
    }

public:
    // an almighty iterator for several types of iteration.
    // Unfortunately, subclassing isn't an option with souffle
    //   - we don't deal with pointers (so no virtual)
    //   - and a single iter type is expected (see Relation::iterator e.g.) (i think)
    class iterator : public std::iterator<std::forward_iterator_tag, TupleType> {
        const BinaryRelation* br = nullptr;
        // special tombstone value to notify that this iter represents the end
        bool isEndVal = false;

        // all the different types of iterator this can be
        enum IterType { ALL, ANTERIOR, ANTPOST, WITHIN };
        IterType ityp;

        TupleType cPair;

        // the disjoint set that we're currently iterating through
        StatesBucket djSetList;
        typename StatesMap::iterator djSetMapListIt;
        typename StatesMap::iterator djSetMapListEnd;

        // used for ALL, and POSTERIOR (just a current index in the cList)
        size_t cAnteriorIndex = 0;
        // used for ALL, and ANTERIOR (just a current index in the cList)
        size_t cPosteriorIndex = 0;

    public:
        // one iterator for signalling the end (simplifies)
        explicit iterator(const BinaryRelation* br, bool /* signalIsEndIterator */)
                : br(br), isEndVal(true){};

        explicit iterator(const BinaryRelation* br) : 
            br(br), ityp(IterType::ALL), 
            djSetMapListIt(br->equivalencePartition.begin()), djSetMapListEnd(br->equivalencePartition.end()) {
                // no need to fast forward if this iterator is empty
                if (djSetMapListIt == djSetMapListEnd) {
                    isEndVal = true;
                    return;
                }
                // grab the pointer to the list, and make it our current list
                djSetList = (*djSetMapListIt).second;
                assert(djSetList->size() != 0);

                updateAnterior();
                updatePosterior();
            }

        // WITHIN: iterator for everything within the same DJset (used for BinaryRelation.partition())
        explicit iterator(const BinaryRelation* br, const StatesBucket within)
                : br(br), ityp(IterType::WITHIN), djSetList(within) {
            // empty dj set
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            updateAnterior();
            updatePosterior();
        }

        // ANTERIOR: iterator that yields all (former, _) \in djset(former) (djset(former) === within)
        explicit iterator(const BinaryRelation* br, const DomainInt former, const StatesBucket within)
                : br(br), ityp(IterType::ANTERIOR), djSetList(within) {
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            setAnterior(former);
            updatePosterior();
        }

        // ANTPOST: iterator that yields all (former, latter) \in djset(former), (djset(former) ==
        // djset(latter) == within)
        explicit iterator(
                const BinaryRelation* br, const DomainInt former, DomainInt latter, const StatesBucket within)
                : br(br), ityp(IterType::ANTPOST), djSetList(within) {
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            setAnterior(former);
            setPosterior(latter);
        }

        /** explicit set first half of cPair */
        inline void setAnterior(const DomainInt a) {
            this->cPair[0] = a;
        }

        /** quick update to whatever the current index is pointing to */
        inline void updateAnterior() {
            this->cPair[0] = this->djSetList->get(this->cAnteriorIndex);
        }

        /** explicit set second half of cPair */
        inline void setPosterior(const DomainInt b) {
            this->cPair[1] = b;
        }

        /** quick update to whatever the current index is pointing to */
        inline void updatePosterior() {
            this->cPair[1] = this->djSetList->get(this->cPosteriorIndex);
        }

        // copy ctor
        iterator(const iterator& other) = default;
        // move ctor
        iterator(iterator&& other) = default;
        // assign iter
        iterator& operator=(const iterator& other) = default;

        bool operator==(const iterator& other) const {
            if (isEndVal && other.isEndVal) return br == other.br;
            return isEndVal == other.isEndVal && cPair == other.cPair;
        }

        bool operator!=(const iterator& other) const {
            return !((*this) == other);
        }

        const TupleType& operator*() const {
            return cPair;
        }

        const TupleType* operator->() const {
            return &cPair;
        }

        /* pre-increment */
        iterator& operator++() {
            if (isEndVal) throw std::out_of_range("error: incrementing an out of range iterator");

            switch (ityp) {
                case IterType::ALL:
                    // move posterior along one
                    // see if we can't move the posterior along
                    if (++cPosteriorIndex == djSetList->size()) {
                        // move anterior along one
                        // see if we can't move the anterior along one
                        if (++cAnteriorIndex == djSetList->size()) {
                            // move the djset it along one
                            // see if we can't move it along one (we're at the end)
                            if (++djSetMapListIt == djSetMapListEnd) {
                                isEndVal = true;
                                return *this;
                            }

                            // we can't iterate along this djset if it is empty
                            djSetList = (*djSetMapListIt).second;
                            if (djSetList->size() == 0) throw std::out_of_range("error: encountered a zero size djset");

                            // update our cAnterior and cPosterior
                            cAnteriorIndex = 0;
                            cPosteriorIndex = 0;
                            updateAnterior();
                            updatePosterior();
                        }

                        // we moved our anterior along one
                        updateAnterior();

                        cPosteriorIndex = 0;
                        updatePosterior();
                    }
                    // we just moved our posterior along one
                    updatePosterior();

                    break;
                case IterType::ANTERIOR:
                    // step posterior along one, and if we can't, then we're done.
                    if (++cPosteriorIndex == djSetList->size()) {
                        isEndVal = true;
                        return *this;
                    }
                    updatePosterior();

                    break;
                case IterType::ANTPOST:
                    // fixed anterior and posterior literally only points to one, so if we increment, its the
                    // end
                    isEndVal = true;
                    break;
                case IterType::WITHIN:
                    // move posterior along one
                    // see if we can't move the posterior along
                    if (++cPosteriorIndex == djSetList->size()) {
                        // move anterior along one
                        // see if we can't move the anterior along one
                        if (++cAnteriorIndex == djSetList->size()) {
                            isEndVal = true;
                            return *this;
                        }

                        // we moved our anterior along one
                        updateAnterior();

                        cPosteriorIndex = 0;
                        updatePosterior();
                    }
                    // we just moved our posterior along one
                    updatePosterior();
                    break;
            }

            return *this;
        }
    };

public:
    /**
     * iterator pointing to the beginning of the tuples, with no restrictions
     * @return the iterator that corresponds to the beginning of the binary relation
     */
    iterator begin() const {
        genAllDisjointSetLists();
        return iterator(this);
    }

    /**
     * iterator pointing to the end of the tuples
     * @return the iterator which represents the end of the binary rel
     */
    iterator end() const {
        return iterator(this, true);
    }

    /**
     * Obtains a range of elements matching the prefix of the given entry up to
     * levels elements.
     *
     * @tparam levels the length of the requested matching prefix
     * @param entry the entry to be looking for
     * @return the corresponding range of matching elements
     */
    template <unsigned levels>
    range<iterator> getBoundaries(const operation_hints& entry) const {
        operation_hints ctxt;
        return getBoundaries<levels>(entry, ctxt);
    }

    /**
     * Obtains a range of elements matching the prefix of the given entry up to
     * levels elements. A operation context may be provided to exploit temporal
     * locality.
     *
     * @tparam levels the length of the requested matching prefix
     * @param entry the entry to be looking for
     * @param ctxt the operation context to be utilized
     * @return the corresponding range of matching elements
     */
    template <unsigned levels>
    range<iterator> getBoundaries(const TupleType& entry, operation_hints&) const {
        // TODO: use ctxt to exploit locality - does this really matter

        // if nothing is bound => just use begin and end
        if (levels == 0) return make_range(begin(), end());

        // as disjoint set is exactly two args (equiv relation)
        // we only need to handle these cases

        if (levels == 1) {
            // need to test if the entry actually exists
            if (!sds.nodeExists(entry[0])) return make_range(end(), end());

            // return an iterator over all (entry[0], _)
            return make_range(anteriorIt(entry[0]), end());
        }

        if (levels == 2) {
            // need to test if the entry actually exists
            if (!sds.contains(entry[0], entry[1])) return make_range(end(), end());

            // if so return an iterator containing exactly that node
            return make_range(antpostit(entry[0], entry[1]), end());
        }

        std::cerr << "invalid state, cannot search for >2 arg start point in getBoundaries, in 2 arg tuple "
                     "store\n";
        throw "invalid state, cannot search for >2 arg start point in getBoundaries, in 2 arg tuple store";

        return make_range(end(), end());
    }

    /**
     * Creates an iterator that generates all pairs (A, X)
     * for a given A, and X are elements within A's disjoint set.
     * @param anteriorVal: The first value of the tuple to be generated for
     * @return the iterator representing this.
     */
    iterator anteriorIt(DomainInt anteriorVal) const {
        
        genAllDisjointSetLists();

        // locate the blocklist that the anterior val resides in
        auto found = equivalencePartition.find({sds.findNode(anteriorVal), nullptr});
        assert(found != equivalencePartition.end() && "iterator called on partition that doesn't exist");

        return iterator(this, anteriorVal, (*found).second);
    }

    /**
     * Creates an iterator that generates the pair (A, B)
     * for a given A and B. If A and B don't exist, or aren't in the same set,
     * then the end() iterator is returned.
     * @param anteriorVal: the A value of the tuple
     * @param posteriorVal: the B value of the tuple
     * @return the iterator representing this
     */
    iterator antpostit(DomainInt anteriorVal, DomainInt posteriorVal) const {
        // obv if they're in diff sets, then iteration for this pair just ends.
        if (!sds.sameSet(anteriorVal, posteriorVal)) return end();
        
        genAllDisjointSetLists();

        // locate the blocklist that the val resides in
        auto found = equivalencePartition.find({sds.findNode(posteriorVal), nullptr});
        assert(found != equivalencePartition.end() && "iterator called on partition that doesn't exist");

        return iterator(this, anteriorVal, posteriorVal, (*found).second);
    }

    /**
     * Begin an iterator over all pairs within a single disjoint set - This is used for partition().
     * @param rep the representative of (or element within) a disjoint set of which to generate all pairs
     * @return an iterator that will generate all pairs within the disjoint set
     */
    iterator closure(DomainInt rep) const {
        genAllDisjointSetLists();

        // locate the blocklist that the val resides in
        auto found = equivalencePartition.find({sds.findNode(rep), nullptr});
        return iterator(this, (*found).second);
    }

    /**
     * Generate an approximate number of iterators for parallel iteration
     * The iterators returned are not necessarily equal in size, but in practise are approximately similarly
     * sized
     * Depending on the structure of the data, there can be more or less partitions returned than requested.
     * @param chunks the number of requested partitions
     * @return a list of the iterators as ranges
     */
    std::vector<souffle::range<iterator>> partition(size_t chunks) const {
        // generate all reps
        genAllDisjointSetLists();

        size_t numPairs = this->size();
        if (numPairs == 0) return {};
        if (numPairs == 1 || chunks <= 1) return {souffle::make_range(begin(), end())};

        // if there's more dj sets than requested chunks, then just return an iter per dj set
        std::vector<souffle::range<iterator>> ret;
        if (chunks <= equivalencePartition.size()) {
            for (auto& p : equivalencePartition) {
                ret.push_back(souffle::make_range(closure(p.first), end()));
            }
            return ret;
        }

        // keep it simple stupid 
        // just go through and if the size of the binrel is > numpairs/chunks, then generate an anteriorIt for each
        const size_t perchunk = numPairs/chunks;
        for (const auto& itp : equivalencePartition) {
            const size_t s = itp.second->size();
            if (s*s > perchunk) {
                for (const auto& i : *itp.second) {
                    ret.push_back(souffle::make_range(anteriorIt(i), end()));
                }
            } else {
                ret.push_back(souffle::make_range(closure(itp.first), end()));
            }
        }

        return ret;
    }
};
}  // namespace souffle
