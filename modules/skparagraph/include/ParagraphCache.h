// Copyright 2019 Google LLC
#ifndef ParagraphCache_DEFINED
#define ParagraphCache_DEFINED

#include "include/private/base/SkMutex.h"
#include <cstddef>
#include <cstdint>
#include <functional>  // std::function

#define PARAGRAPH_CACHE_STATS

namespace skia {
namespace textlayout {

class ParagraphImpl;
class ParagraphCacheValue;
struct VenusParagraphNode;

class ParagraphCache {
public:
    ParagraphCache();
    ~ParagraphCache();

    void abandon();
    void reset();
    bool updateParagraph(ParagraphImpl* paragraph);
    bool findParagraph(ParagraphImpl* paragraph);

    // For testing
    void setChecker(std::function<void(ParagraphImpl* impl, const char*, bool)> checker) {
        fChecker = std::move(checker);
    }
    void printStatistics();
    void turnOn(bool value);
    int count();

    // ---- Venus: one cache shared by several worker-thread facades -----------
    // Switches this instance to the shared path. Call once, before the first
    // find/update. Differences from the single-thread path above:
    //   * key and value are built outside the lock, a hit copies the value
    //     after the lock is released (the value is held by a shared_ptr, so an
    //     eviction only drops a reference and never frees under a reader);
    //   * inserts evict the LRU tail by entry count and by an estimated byte
    //     budget (venusEstimateBytes), O(1) each, with the evicted nodes
    //     destroyed after the lock is released;
    //   * the key is stored once, inside the value.
    // byteBudget == 0 keeps the count cap only. reset() empties it (nodes are
    // again destroyed outside the lock).
    void venusConfigureShared(int maxEntries, size_t byteBudget);
    struct VenusStats {
        bool shared = false;
        int maxEntries = 0;
        int count = 0;
        uint64_t byteBudget = 0;
        uint64_t requests = 0;      // find calls
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t editingSkips = 0;  // isPossiblyTextEditing refused the insert
        uint64_t inserts = 0;
        uint64_t evictedCount = 0;
        uint64_t evictedBytes = 0;
        uint64_t bytes = 0;         // estimated bytes currently retained
    };
    void venusGetStats(VenusStats* out);

    bool isPossiblyTextEditing(ParagraphImpl* paragraph);

 private:

    struct Entry;
    void updateFrom(const ParagraphImpl* paragraph, Entry* entry);
    void updateTo(ParagraphImpl* paragraph, const Entry* entry);
    bool venusFindParagraph(ParagraphImpl* paragraph);
    bool venusUpdateParagraph(ParagraphImpl* paragraph);
    void venusReset();
    // Unlinks the LRU tail and pushes it onto *detached (singly linked through
    // the node); the caller deletes the chain after releasing the lock.
    void venusDetachTailLocked(VenusParagraphNode** detached);
    static size_t venusEstimateBytes(const ParagraphCacheValue& value);
    static void venusCopyTo(ParagraphImpl* paragraph, const ParagraphCacheValue& value);

     mutable SkMutex fParagraphMutex;
     std::function<void(ParagraphImpl* impl, const char*, bool)> fChecker;

    struct Cache;
    std::unique_ptr<Cache> fCache;

};

}  // namespace textlayout
}  // namespace skia

#endif  // ParagraphCache_DEFINED
