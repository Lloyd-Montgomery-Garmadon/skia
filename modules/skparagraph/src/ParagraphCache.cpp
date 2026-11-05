// Copyright 2019 Google LLC
#include <functional>
#include <memory>

#include "modules/skparagraph/include/FontArguments.h"
#include "modules/skparagraph/include/ParagraphCache.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "src/base/SkFloatBits.h"
#include "src/base/SkTInternalLList.h"
#include "src/core/SkLRUCache.h"
#include "src/core/SkTHash.h"

using namespace skia_private;

namespace skia {
namespace textlayout {

class ParagraphCacheKey;
class ParagraphCacheValue;

// Venus shared mode keeps its own LRU (hash table + intrusive list) instead of
// SkLRUCache: O(1) tail access, the key stored once (inside the value), and a
// node that can be unlinked under the lock and destroyed after it.
struct VenusParagraphTraits {
    static const ParagraphCacheKey& GetKey(VenusParagraphNode* node);
    static uint32_t Hash(const ParagraphCacheKey& key);
};

struct ParagraphCache::Cache {
    explicit Cache(int maxEntries = kMaxEntries)
        : fLRUCacheMap(maxEntries)
        , fCacheIsOn(true)
        , fLastCachedValue(nullptr)
#ifdef PARAGRAPH_CACHE_STATS
        , fTotalRequests(0)
        , fCacheMisses(0)
        , fHashMisses(0)
#endif
    {}

    void printStatistics() const {
        SkDebugf("--- Paragraph Cache ---\n");
        SkDebugf("Total requests: %d\n", fTotalRequests);
        SkDebugf("Cache misses: %d\n", fCacheMisses);
        SkDebugf("Cache miss %%: %f\n", (fTotalRequests > 0) ? 100.f * fCacheMisses / fTotalRequests : 0.f);
        int cacheHits = fTotalRequests - fCacheMisses;
        SkDebugf("Hash miss %%: %f\n", (cacheHits > 0) ? 100.f * fHashMisses / cacheHits : 0.f);
        SkDebugf("---------------------\n");
    }

    struct KeyHash {
        uint32_t operator()(const ParagraphCacheKey& key) const;
    };
    static const int kMaxEntries = 128;

    SkLRUCache<ParagraphCacheKey, std::unique_ptr<Entry>, KeyHash> fLRUCacheMap;
    bool fCacheIsOn;
    ParagraphCacheValue* fLastCachedValue;

#ifdef PARAGRAPH_CACHE_STATS
    int fTotalRequests;
    int fCacheMisses;
    int fHashMisses; // cache hit but hash table missed
#endif

    // ---- Venus shared mode; unused unless venusConfigureShared ran.
    ~Cache();  // frees the venus nodes; nothing to do for a single-thread cache
    bool fVenusShared = false;
    int fVenusMaxEntries = kMaxEntries;
    size_t fVenusByteBudget = 0;
    size_t fVenusBytes = 0;
    ParagraphCache::VenusStats fVenusStats;
    THashTable<VenusParagraphNode*, ParagraphCacheKey, VenusParagraphTraits> fVenusMap;
    SkTInternalLList<VenusParagraphNode> fVenusLRU;  // head = MRU, tail = LRU
};

namespace {
    int32_t relax(SkScalar a) {
        // This rounding is done to match Flutter tests. Must be removed..
        if (SkIsFinite(a)) {
          auto threshold = SkIntToScalar(1 << 12);
          return SkFloat2Bits(SkScalarRoundToScalar(a * threshold)/threshold);
        } else {
          return SkFloat2Bits(a);
        }
    }

    bool exactlyEqual(SkScalar x, SkScalar y) {
        return x == y || (x != x && y != y);
    }

}  // namespace

class ParagraphCacheKey {
public:
    ParagraphCacheKey(const ParagraphImpl* paragraph)
        : fText(paragraph->fText.c_str(), paragraph->fText.size())
        , fPlaceholders(paragraph->fPlaceholders)
        , fTextStyles(paragraph->fTextStyles)
        , fParagraphStyle(paragraph->paragraphStyle()) {
        fHash = computeHash();
    }

    ParagraphCacheKey(const ParagraphCacheKey& other) = default;

    ParagraphCacheKey(ParagraphCacheKey&& other)
        : fText(std::move(other.fText))
        , fPlaceholders(std::move(other.fPlaceholders))
        , fTextStyles(std::move(other.fTextStyles))
        , fParagraphStyle(std::move(other.fParagraphStyle))
        , fHash(other.fHash) {
        other.fHash = 0;
    }

    bool operator==(const ParagraphCacheKey& other) const;

    uint32_t hash() const { return fHash; }

    const SkString& text() const { return fText; }

    // Venus: estimated heap footprint of one key (estimate, not measured).
    size_t venusBytes() const {
        return sizeof(ParagraphCacheKey) + fText.size() +
               fPlaceholders.size() * sizeof(Placeholder) +
               fTextStyles.size() * sizeof(Block);
    }

private:
    static uint32_t mix(uint32_t hash, uint32_t data);
    uint32_t computeHash() const;

    SkString fText;
    TArray<Placeholder, true> fPlaceholders;
    TArray<Block, true> fTextStyles;
    ParagraphStyle fParagraphStyle;
    uint32_t fHash;
};

class ParagraphCacheValue {
public:
    ParagraphCacheValue(ParagraphCacheKey&& key, const ParagraphImpl* paragraph)
        : fKey(std::move(key))
        , fRuns(paragraph->fRuns)
        , fClusters(paragraph->fClusters)
        , fClustersIndexFromCodeUnit(paragraph->fClustersIndexFromCodeUnit)
        , fCodeUnitProperties(paragraph->fCodeUnitProperties)
        , fWords(paragraph->fWords)
        , fBidiRegions(paragraph->fBidiRegions)
        , fHasLineBreaks(paragraph->fHasLineBreaks)
        , fHasWhitespacesInside(paragraph->fHasWhitespacesInside)
        , fTrailingSpaces(paragraph->fTrailingSpaces) { }

    // Input == key
    ParagraphCacheKey fKey;

    // Shaped results
    TArray<Run, false> fRuns;
    TArray<Cluster, true> fClusters;
    TArray<size_t, true> fClustersIndexFromCodeUnit;
    // ICU results
    TArray<SkUnicode::CodeUnitFlags, true> fCodeUnitProperties;
    std::vector<size_t> fWords;
    std::vector<SkUnicode::BidiRegion> fBidiRegions;
    bool fHasLineBreaks;
    bool fHasWhitespacesInside;
    TextIndex fTrailingSpaces;
};

struct VenusParagraphNode {
    VenusParagraphNode(std::shared_ptr<ParagraphCacheValue> value, size_t bytes)
        : fValue(std::move(value)), fBytes(bytes) {}
    std::shared_ptr<ParagraphCacheValue> fValue;  // owns the key: fValue->fKey
    size_t fBytes;
    VenusParagraphNode* fDetachedNext = nullptr;  // chain of unlinked nodes awaiting delete
    SK_DECLARE_INTERNAL_LLIST_INTERFACE(VenusParagraphNode);
};

const ParagraphCacheKey& VenusParagraphTraits::GetKey(VenusParagraphNode* node) {
    return node->fValue->fKey;
}
uint32_t VenusParagraphTraits::Hash(const ParagraphCacheKey& key) { return key.hash(); }

ParagraphCache::Cache::~Cache() {
    for (VenusParagraphNode* node = fVenusLRU.head(); node; node = fVenusLRU.head()) {
        fVenusLRU.remove(node);
        delete node;
    }
}

uint32_t ParagraphCacheKey::mix(uint32_t hash, uint32_t data) {
    hash += data;
    hash += (hash << 10);
    hash ^= (hash >> 6);
    return hash;
}

uint32_t ParagraphCacheKey::computeHash() const {
    uint32_t hash = 0;
    for (auto& ph : fPlaceholders) {
        if (ph.fRange.width() == 0) {
            continue;
        }
        hash = mix(hash, SkGoodHash()(ph.fRange));
        hash = mix(hash, SkGoodHash()(relax(ph.fStyle.fHeight)));
        hash = mix(hash, SkGoodHash()(relax(ph.fStyle.fWidth)));
        hash = mix(hash, SkGoodHash()(ph.fStyle.fAlignment));
        hash = mix(hash, SkGoodHash()(ph.fStyle.fBaseline));
        if (ph.fStyle.fAlignment == PlaceholderAlignment::kBaseline) {
            hash = mix(hash, SkGoodHash()(relax(ph.fStyle.fBaselineOffset)));
        }
    }

    for (auto& ts : fTextStyles) {
        if (ts.fStyle.isPlaceholder()) {
            continue;
        }
        hash = mix(hash, SkGoodHash()(relax(ts.fStyle.getLetterSpacing())));
        hash = mix(hash, SkGoodHash()(relax(ts.fStyle.getWordSpacing())));
        if (ts.fStyle.getVenusNoSoftWrap()) {
            hash = mix(hash, SkGoodHash()(true));
        }
        hash = mix(hash, SkGoodHash()(ts.fStyle.getLocale()));
        hash = mix(hash, SkGoodHash()(relax(ts.fStyle.getHeight())));
        hash = mix(hash, SkGoodHash()(relax(ts.fStyle.getBaselineShift())));
        for (auto& ff : ts.fStyle.getFontFamilies()) {
            hash = mix(hash, SkGoodHash()(ff));
        }
        for (auto& ff : ts.fStyle.getFontFeatures()) {
            hash = mix(hash, SkGoodHash()(ff.fValue));
            hash = mix(hash, SkGoodHash()(ff.fName));
        }
        hash = mix(hash, std::hash<std::optional<FontArguments>>()(ts.fStyle.getFontArguments()));
        hash = mix(hash, SkGoodHash()(ts.fStyle.getFontStyle()));
        hash = mix(hash, SkGoodHash()(relax(ts.fStyle.getFontSize())));
        hash = mix(hash, SkGoodHash()(ts.fRange));
    }

    hash = mix(hash, SkGoodHash()(relax(fParagraphStyle.getHeight())));
    hash = mix(hash, SkGoodHash()(fParagraphStyle.getTextDirection()));
    hash = mix(hash, SkGoodHash()(fParagraphStyle.getReplaceTabCharacters() ? 1 : 0));
    hash = mix(hash, SkGoodHash()(fParagraphStyle.getHonorZeroHeightOverride() ? 1 : 0));

    if (!fParagraphStyle.getVenusFloatExclusions().empty()) {
        hash = mix(hash, 0x41545631u);
        hash = mix(hash, 0x56464c4fu);
        hash = mix(hash, 1u);
        hash = mix(hash, static_cast<uint32_t>(fParagraphStyle.getVenusFloatExclusions().size()));
        for (const auto& e : fParagraphStyle.getVenusFloatExclusions()) {
            for (double coordinate : {e.top, e.bottom, e.left, e.right}) {
                const uint64_t bits = static_cast<uint64_t>(std::hash<double>()(coordinate));
                hash = mix(hash, static_cast<uint32_t>(bits));
                hash = mix(hash, static_cast<uint32_t>(bits >> 32));
            }
            hash = mix(hash, e.side);
        }
    }

    auto& strutStyle = fParagraphStyle.getStrutStyle();
    if (strutStyle.getStrutEnabled()) {
        hash = mix(hash, SkGoodHash()(relax(strutStyle.getHeight())));
        hash = mix(hash, SkGoodHash()(relax(strutStyle.getLeading())));
        hash = mix(hash, SkGoodHash()(relax(strutStyle.getFontSize())));
        hash = mix(hash, SkGoodHash()(strutStyle.getHeightOverride()));
        hash = mix(hash, SkGoodHash()(strutStyle.getFontStyle()));
        hash = mix(hash, SkGoodHash()(strutStyle.getForceStrutHeight()));
        for (auto& ff : strutStyle.getFontFamilies()) {
            hash = mix(hash, SkGoodHash()(ff));
        }
    }

    hash = mix(hash, SkGoodHash()(fText));
    return hash;
}

uint32_t ParagraphCache::Cache::KeyHash::operator()(const ParagraphCacheKey& key) const {
    return key.hash();
}

bool ParagraphCacheKey::operator==(const ParagraphCacheKey& other) const {
    if (fText.size() != other.fText.size()) {
        return false;
    }
    if (fPlaceholders.size() != other.fPlaceholders.size()) {
        return false;
    }
    if (fText != other.fText) {
        return false;
    }
    if (fTextStyles.size() != other.fTextStyles.size()) {
        return false;
    }

    if (fParagraphStyle.getVenusFloatExclusions() !=
        other.fParagraphStyle.getVenusFloatExclusions()) {
        return false;
    }

    // There is no need to compare default paragraph styles - they are included into fTextStyles
    if (!exactlyEqual(fParagraphStyle.getHeight(), other.fParagraphStyle.getHeight())) {
        return false;
    }
    if (fParagraphStyle.getTextDirection() != other.fParagraphStyle.getTextDirection()) {
        return false;
    }

    if (!(fParagraphStyle.getStrutStyle() == other.fParagraphStyle.getStrutStyle())) {
        return false;
    }

    if (!(fParagraphStyle.getReplaceTabCharacters() == other.fParagraphStyle.getReplaceTabCharacters())) {
        return false;
    }

    // venus: with honored zero heights, the override and half-leading bits change
    // the cached run metrics, while equalsByFonts only compares fHeight.
    const bool honorZeroHeight = fParagraphStyle.getHonorZeroHeightOverride();
    if (honorZeroHeight != other.fParagraphStyle.getHonorZeroHeightOverride()) {
        return false;
    }

    for (int i = 0; i < fTextStyles.size(); ++i) {
        auto& tsa = fTextStyles[i];
        auto& tsb = other.fTextStyles[i];
        if (tsa.fStyle.isPlaceholder()) {
            continue;
        }
        if (tsa.fStyle.getVenusNoSoftWrap() != tsb.fStyle.getVenusNoSoftWrap()) return false;
        if (!(tsa.fStyle.equalsByFonts(tsb.fStyle))) {
            return false;
        }
        if (honorZeroHeight &&
            (tsa.fStyle.getHeightOverride() != tsb.fStyle.getHeightOverride() ||
             tsa.fStyle.getHalfLeading() != tsb.fStyle.getHalfLeading())) {
            return false;
        }
        if (tsa.fRange.width() != tsb.fRange.width()) {
            return false;
        }
        if (tsa.fRange.start != tsb.fRange.start) {
            return false;
        }
    }
    for (int i = 0; i < fPlaceholders.size(); ++i) {
        auto& tsa = fPlaceholders[i];
        auto& tsb = other.fPlaceholders[i];
        if (tsa.fRange.width() == 0 && tsb.fRange.width() == 0) {
            continue;
        }
        if (!(tsa.fStyle.equals(tsb.fStyle))) {
            return false;
        }
        if (tsa.fRange.width() != tsb.fRange.width()) {
            return false;
        }
        if (tsa.fRange.start != tsb.fRange.start) {
            return false;
        }
    }

    return true;
}

struct ParagraphCache::Entry {

    Entry(ParagraphCacheValue* value) : fValue(value) {}
    std::unique_ptr<ParagraphCacheValue> fValue;
};

ParagraphCache::ParagraphCache()
    : fChecker([](ParagraphImpl* impl, const char*, bool){ })
    , fCache(std::make_unique<Cache>())
{ }

ParagraphCache::~ParagraphCache() { }

void ParagraphCache::turnOn(bool value) { fCache->fCacheIsOn = value; }
int ParagraphCache::count() { return fCache->fLRUCacheMap.count(); }

// ---------------------------------------------------------------------------
// Venus shared mode. Everything below is only reached when
// venusConfigureShared() ran on this cache; findParagraph/updateParagraph/reset
// dispatch here on their first line and are otherwise the single-thread code.
// ---------------------------------------------------------------------------
void ParagraphCache::venusConfigureShared(int maxEntries, size_t byteBudget) {
    SkAutoMutexExclusive lock(fParagraphMutex);
    fCache = std::make_unique<Cache>(maxEntries > 0 ? maxEntries : Cache::kMaxEntries);
    fCache->fVenusShared = true;
    fCache->fVenusMaxEntries = maxEntries > 0 ? maxEntries : Cache::kMaxEntries;
    fCache->fVenusByteBudget = byteBudget;
}

void ParagraphCache::venusGetStats(VenusStats* out) {
    SkAutoMutexExclusive lock(fParagraphMutex);
    const Cache& c = *fCache;
    *out = c.fVenusStats;
    out->shared = c.fVenusShared;
    out->maxEntries = c.fVenusShared ? c.fVenusMaxEntries : Cache::kMaxEntries;
    out->byteBudget = c.fVenusByteBudget;
    out->count = c.fVenusShared ? c.fVenusMap.count() : c.fLRUCacheMap.count();
    out->bytes = c.fVenusBytes;
}

// Estimate only. Dominated by per-run fixed costs: sizeof(Run) carries the
// inline STArray<64> justification buffer and GlyphData carries four inline
// STArray<64> glyph buffers, both mostly empty for short mixed-script runs
// (800 + 1472 bytes per run on LP64).
size_t ParagraphCache::venusEstimateBytes(const ParagraphCacheValue& value) {
    size_t bytes = sizeof(ParagraphCacheValue) + value.fKey.venusBytes();
    constexpr size_t kPerGlyph = sizeof(SkGlyphID) + 2 * sizeof(SkPoint) + sizeof(uint32_t);
    for (const Run& run : value.fRuns) {
        bytes += sizeof(Run) + sizeof(Run::GlyphData) + run.fLanguage.size();
        const size_t glyphs = run.size() + 1;  // positions/offsets/clusterIndexes carry one trailing slot
        if (glyphs > 64) {
            bytes += (glyphs - 64) * kPerGlyph;
        }
    }
    bytes += value.fClusters.size() * sizeof(Cluster);
    bytes += value.fClustersIndexFromCodeUnit.size() * sizeof(size_t);
    bytes += value.fCodeUnitProperties.size() * sizeof(SkUnicode::CodeUnitFlags);
    bytes += value.fWords.size() * sizeof(size_t);
    bytes += value.fBidiRegions.size() * sizeof(SkUnicode::BidiRegion);
    return bytes;
}

// Same as updateTo, from a value instead of an entry. Runs share GlyphData by
// shared_ptr; it is read-only after shaping (the only writers, applySpacing*
// and Run::shift/extend, run inside shapeTextIntoEndlessLine before the
// insert), so copies held by different paragraphs and threads never race.
void ParagraphCache::venusCopyTo(ParagraphImpl* paragraph, const ParagraphCacheValue& value) {
    paragraph->fRuns.clear();
    paragraph->fRuns = value.fRuns;
    paragraph->fClusters = value.fClusters;
    paragraph->fClustersIndexFromCodeUnit = value.fClustersIndexFromCodeUnit;
    paragraph->fCodeUnitProperties = value.fCodeUnitProperties;
    paragraph->fWords = value.fWords;
    paragraph->fBidiRegions = value.fBidiRegions;
    paragraph->fHasLineBreaks = value.fHasLineBreaks;
    paragraph->fHasWhitespacesInside = value.fHasWhitespacesInside;
    paragraph->fTrailingSpaces = value.fTrailingSpaces;
    for (auto& run : paragraph->fRuns) {
        run.setOwner(paragraph);
    }
    for (auto& cluster : paragraph->fClusters) {
        cluster.setOwner(paragraph);
    }
}

void ParagraphCache::venusDetachTailLocked(VenusParagraphNode** detached) {
    Cache& c = *fCache;
    VenusParagraphNode* tail = c.fVenusLRU.tail();
    if (tail == nullptr) {
        return;
    }
    c.fVenusMap.remove(tail->fValue->fKey);
    c.fVenusLRU.remove(tail);
    c.fVenusBytes -= tail->fBytes;
    c.fVenusStats.evictedCount++;
    c.fVenusStats.evictedBytes += tail->fBytes;
    tail->fDetachedNext = *detached;
    *detached = tail;
}

namespace {
// Deletes a chain of detached nodes. Runs outside the lock: a worker that
// copied the value out of a hit still owns a shared_ptr to it, so the value
// dies on whichever side drops the last reference.
void venusDeleteDetached(VenusParagraphNode* detached) {
    while (detached != nullptr) {
        VenusParagraphNode* next = detached->fDetachedNext;
        delete detached;
        detached = next;
    }
}
}  // namespace

bool ParagraphCache::venusFindParagraph(ParagraphImpl* paragraph) {
    // The key only reads this worker's own paragraph: build it before the lock.
    const ParagraphCacheKey key(paragraph);
    std::shared_ptr<ParagraphCacheValue> value;
    {
        SkAutoMutexExclusive lock(fParagraphMutex);
        Cache& c = *fCache;
        c.fVenusStats.requests++;
        VenusParagraphNode** slot = c.fVenusMap.find(key);
        if (slot) {
            VenusParagraphNode* node = *slot;
            value = node->fValue;
            if (node != c.fVenusLRU.head()) {
                c.fVenusLRU.remove(node);
                c.fVenusLRU.addToHead(node);
            }
            c.fVenusStats.hits++;
        } else {
            c.fVenusStats.misses++;
        }
    }
    if (!value) {
        fChecker(paragraph, "missingParagraph", true);
        return false;
    }
    venusCopyTo(paragraph, *value);  // outside the lock; the shared_ptr keeps the value alive
    fChecker(paragraph, "foundParagraph", true);
    return true;
}

bool ParagraphCache::venusUpdateParagraph(ParagraphImpl* paragraph) {
    // The value copy (runs, clusters, ~100KB for long rows) only reads this
    // worker's paragraph: build it before the lock. The key moves into the
    // value and is stored nowhere else.
    ParagraphCacheKey key(paragraph);
    auto value = std::make_shared<ParagraphCacheValue>(std::move(key), paragraph);
    const size_t bytes = venusEstimateBytes(*value);
    VenusParagraphNode* detached = nullptr;
    bool inserted = false;
    {
        SkAutoMutexExclusive lock(fParagraphMutex);
        Cache& c = *fCache;
        if (c.fVenusMap.find(value->fKey) != nullptr) {
            return false;  // another worker shaped the same paragraph meanwhile
        }
        if (isPossiblyTextEditing(paragraph)) {
            c.fVenusStats.editingSkips++;
            return false;
        }
        // Evict before inserting so count and byte totals stay exact. A single
        // entry larger than the whole budget still goes in and leaves on the
        // next insert. Nodes are only unlinked here; deleted after the lock.
        while (c.fVenusMap.count() > 0 &&
               (c.fVenusMap.count() >= c.fVenusMaxEntries ||
                (c.fVenusByteBudget > 0 && c.fVenusBytes + bytes > c.fVenusByteBudget))) {
            venusDetachTailLocked(&detached);
        }
        ParagraphCacheValue* raw = value.get();
        VenusParagraphNode* node = new VenusParagraphNode(std::move(value), bytes);
        c.fVenusMap.set(node);
        c.fVenusLRU.addToHead(node);
        c.fVenusBytes += bytes;
        c.fVenusStats.inserts++;
        // Reassigned before the lock is released: the detached chain may hold
        // the previous value, and this pointer must never outlive its target.
        c.fLastCachedValue = raw;
        inserted = true;
    }
    venusDeleteDetached(detached);
    if (inserted) {
        fChecker(paragraph, "addedParagraph", true);
    }
    return inserted;
}

void ParagraphCache::venusReset() {
    VenusParagraphNode* detached = nullptr;
    {
        SkAutoMutexExclusive lock(fParagraphMutex);
        Cache& c = *fCache;
        for (VenusParagraphNode* node = c.fVenusLRU.head(); node; node = c.fVenusLRU.head()) {
            c.fVenusLRU.remove(node);
            node->fDetachedNext = detached;
            detached = node;
        }
        c.fVenusMap.reset();
        c.fVenusBytes = 0;
        c.fLastCachedValue = nullptr;
    }
    venusDeleteDetached(detached);
}

void ParagraphCache::updateTo(ParagraphImpl* paragraph, const Entry* entry) {

    paragraph->fRuns.clear();
    paragraph->fRuns = entry->fValue->fRuns;
    paragraph->fClusters = entry->fValue->fClusters;
    paragraph->fClustersIndexFromCodeUnit = entry->fValue->fClustersIndexFromCodeUnit;
    paragraph->fCodeUnitProperties = entry->fValue->fCodeUnitProperties;
    paragraph->fWords = entry->fValue->fWords;
    paragraph->fBidiRegions = entry->fValue->fBidiRegions;
    paragraph->fHasLineBreaks = entry->fValue->fHasLineBreaks;
    paragraph->fHasWhitespacesInside = entry->fValue->fHasWhitespacesInside;
    paragraph->fTrailingSpaces = entry->fValue->fTrailingSpaces;
    for (auto& run : paragraph->fRuns) {
        run.setOwner(paragraph);
    }
    for (auto& cluster : paragraph->fClusters) {
        cluster.setOwner(paragraph);
    }
}

void ParagraphCache::printStatistics() {
    fCache->printStatistics();
}

void ParagraphCache::abandon() {
    this->reset();
}

void ParagraphCache::reset() {
    if (fCache->fVenusShared) {
        return this->venusReset();
    }
    SkAutoMutexExclusive lock(fParagraphMutex);
#ifdef PARAGRAPH_CACHE_STATS
    fCache->fTotalRequests = 0;
    fCache->fCacheMisses = 0;
    fCache->fHashMisses = 0;
#endif
    fCache->fLRUCacheMap.reset();
    fCache->fLastCachedValue = nullptr;
}

bool ParagraphCache::findParagraph(ParagraphImpl* paragraph) {
    if (fCache->fVenusShared) {
        return this->venusFindParagraph(paragraph);
    }
    if (!fCache->fCacheIsOn) {
        return false;
    }
#ifdef PARAGRAPH_CACHE_STATS
    ++fCache->fTotalRequests;
#endif
    SkAutoMutexExclusive lock(fParagraphMutex);
    ParagraphCacheKey key(paragraph);
    std::unique_ptr<Entry>* entry = fCache->fLRUCacheMap.find(key);

    if (!entry) {
        // We have a cache miss
#ifdef PARAGRAPH_CACHE_STATS
        ++fCache->fCacheMisses;
#endif
        fChecker(paragraph, "missingParagraph", true);
        return false;
    }
    updateTo(paragraph, entry->get());
    fChecker(paragraph, "foundParagraph", true);
    return true;
}

bool ParagraphCache::updateParagraph(ParagraphImpl* paragraph) {
    if (fCache->fVenusShared) {
        return this->venusUpdateParagraph(paragraph);
    }
    if (!fCache->fCacheIsOn) {
        return false;
    }
#ifdef PARAGRAPH_CACHE_STATS
    ++fCache->fTotalRequests;
#endif
    SkAutoMutexExclusive lock(fParagraphMutex);

    ParagraphCacheKey key(paragraph);
    std::unique_ptr<Entry>* entry = fCache->fLRUCacheMap.find(key);
    if (!entry) {
        // isTooMuchMemoryWasted(paragraph) not needed for now
        if (isPossiblyTextEditing(paragraph)) {
            // Skip this paragraph
            return false;
        }
        ParagraphCacheValue* value = new ParagraphCacheValue(std::move(key), paragraph);
        fCache->fLRUCacheMap.insert(value->fKey, std::make_unique<Entry>(value));
        fChecker(paragraph, "addedParagraph", true);
        fCache->fLastCachedValue = value;
        return true;
    } else {
        // We do not have to update the paragraph
        return false;
    }
}

// Special situation: (very) long paragraph that is close to the last formatted paragraph
#define NOCACHE_PREFIX_LENGTH 40
bool ParagraphCache::isPossiblyTextEditing(ParagraphImpl* paragraph) {
    if (fCache->fLastCachedValue == nullptr) {
        return false;
    }

    auto& lastText = fCache->fLastCachedValue->fKey.text();
    auto& text = paragraph->fText;

    if ((lastText.size() < NOCACHE_PREFIX_LENGTH) || (text.size() < NOCACHE_PREFIX_LENGTH)) {
        // Either last text or the current are too short
        return false;
    }

    if (std::strncmp(lastText.c_str(), text.c_str(), NOCACHE_PREFIX_LENGTH) == 0) {
        // Texts have the same starts
        return true;
    }

    if (std::strncmp(lastText.c_str() + lastText.size() - NOCACHE_PREFIX_LENGTH, &text[text.size() - NOCACHE_PREFIX_LENGTH], NOCACHE_PREFIX_LENGTH) == 0) {
        // Texts have the same ends
        return true;
    }

    // It does not look like editing the text
    return false;
}
}  // namespace textlayout
}  // namespace skia
