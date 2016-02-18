/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrAtlasTextBlob_DEFINED
#define GrAtlasTextBlob_DEFINED

#include "GrBatchAtlas.h"
#include "GrBatchFontCache.h"
#include "GrColor.h"
#include "GrMemoryPool.h"
#include "SkDescriptor.h"
#include "SkMaskFilter.h"
#include "SkSurfaceProps.h"
#include "SkTInternalLList.h"

struct GrDistanceFieldAdjustTable;
class GrMemoryPool;
class GrTextContext;
class SkDrawFilter;
class SkTextBlob;
class SkTextBlobRunIterator;

// With this flag enabled, the GrAtlasTextContext will, as a sanity check, regenerate every blob
// that comes in to verify the integrity of its cache
#define CACHE_SANITY_CHECK 0

/*
 * A GrAtlasTextBlob contains a fully processed SkTextBlob, suitable for nearly immediate drawing
 * on the GPU.  These are initially created with valid positions and colors, but invalid
 * texture coordinates.  The GrAtlasTextBlob itself has a few Blob-wide properties, and also
 * consists of a number of runs.  Runs inside a blob are flushed individually so they can be
 * reordered.
 *
 * The only thing(aside from a memcopy) required to flush a GrAtlasTextBlob is to ensure that
 * the GrAtlas will not evict anything the Blob needs.
 *
 * Note: This struct should really be named GrCachedAtasTextBlob, but that is too verbose.
 *
 * *WARNING* If you add new fields to this struct, then you may need to to update AssertEqual
 */
class GrAtlasTextBlob : public SkNVRefCnt<GrAtlasTextBlob> {
public:
    SK_DECLARE_INTERNAL_LLIST_INTERFACE(GrAtlasTextBlob);

    static GrAtlasTextBlob* Create(GrMemoryPool* pool, int glyphCount, int runCount);

    struct Key {
        Key() {
            sk_bzero(this, sizeof(Key));
        }
        uint32_t fUniqueID;
        // Color may affect the gamma of the mask we generate, but in a fairly limited way.
        // Each color is assigned to on of a fixed number of buckets based on its
        // luminance. For each luminance bucket there is a "canonical color" that
        // represents the bucket.  This functionality is currently only supported for A8
        SkColor fCanonicalColor;
        SkPaint::Style fStyle;
        SkPixelGeometry fPixelGeometry;
        bool fHasBlur;

        bool operator==(const Key& other) const {
            return 0 == memcmp(this, &other, sizeof(Key));
        }
    };

    void setupKey(const GrAtlasTextBlob::Key& key,
                  const SkMaskFilter::BlurRec& blurRec,
                  const SkPaint& paint) {
        fKey = key;
        if (key.fHasBlur) {
            fBlurRec = blurRec;
        }
        if (key.fStyle != SkPaint::kFill_Style) {
            fStrokeInfo.fFrameWidth = paint.getStrokeWidth();
            fStrokeInfo.fMiterLimit = paint.getStrokeMiter();
            fStrokeInfo.fJoin = paint.getStrokeJoin();
        }
    }

    static const Key& GetKey(const GrAtlasTextBlob& blob) {
        return blob.fKey;
    }

    static uint32_t Hash(const Key& key) {
        return SkChecksum::Murmur3(&key, sizeof(Key));
    }

    void operator delete(void* p) {
        GrAtlasTextBlob* blob = reinterpret_cast<GrAtlasTextBlob*>(p);
        blob->fPool->release(p);
    }
    void* operator new(size_t) {
        SkFAIL("All blobs are created by placement new.");
        return sk_malloc_throw(0);
    }

    void* operator new(size_t, void* p) { return p; }
    void operator delete(void* target, void* placement) {
        ::operator delete(target, placement);
    }

    bool hasDistanceField() const { return SkToBool(fTextType & kHasDistanceField_TextType); }
    bool hasBitmap() const { return SkToBool(fTextType & kHasBitmap_TextType); }
    void setHasDistanceField() { fTextType |= kHasDistanceField_TextType; }
    void setHasBitmap() { fTextType |= kHasBitmap_TextType; }

    void push_back_run(int currRun) {
        SkASSERT(currRun < fRunCount);
        if (currRun > 0) {
            Run::SubRunInfo& newRun = fRuns[currRun].fSubRunInfo.back();
            Run::SubRunInfo& lastRun = fRuns[currRun - 1].fSubRunInfo.back();
            newRun.setAsSuccessor(lastRun);
        }
    }

    // sets the last subrun of runIndex to use distance field text
    void setSubRunHasDistanceFields(int runIndex, bool hasLCD) {
        Run& run = fRuns[runIndex];
        Run::SubRunInfo& subRun = run.fSubRunInfo.back();
        subRun.setUseLCDText(hasLCD);
        subRun.setDrawAsDistanceFields();
    }

    void setRunDrawAsPaths(int runIndex) {
        fRuns[runIndex].fDrawAsPaths = true;
    }

    void setMinAndMaxScale(SkScalar scaledMax, SkScalar scaledMin) {
        // we init fMaxMinScale and fMinMaxScale in the constructor
        fMaxMinScale = SkMaxScalar(scaledMax, fMaxMinScale);
        fMinMaxScale = SkMinScalar(scaledMin, fMinMaxScale);
    }

    // inits the override descriptor on the current run.  All following subruns must use this
    // descriptor
    void initOverride(int runIndex) {
        Run& run = fRuns[runIndex];
        // Push back a new subrun to fill and set the override descriptor
        run.push_back();
        run.fOverrideDescriptor.reset(new SkAutoDescriptor);
    }

    SkGlyphCache* setupCache(int runIndex,
                             const SkSurfaceProps& props,
                             const SkPaint& skPaint,
                             const SkMatrix* viewMatrix,
                             bool noGamma);

    // Appends a glyph to the blob.  If the glyph is too large, the glyph will be appended
    // as a path.
    void appendGlyph(int runIndex,
                     const SkRect& positions,
                     GrColor color,
                     GrBatchTextStrike* strike,
                     GrGlyph* glyph,
                     GrFontScaler* scaler, const SkGlyph& skGlyph,
                     SkScalar x, SkScalar y, SkScalar scale, bool applyVM);

    static size_t GetVertexStride(GrMaskFormat maskFormat) {
        switch (maskFormat) {
            case kA8_GrMaskFormat:
                return kGrayTextVASize;
            case kARGB_GrMaskFormat:
                return kColorTextVASize;
            default:
                return kLCDTextVASize;
        }
    }

    bool mustRegenerate(SkScalar* outTransX, SkScalar* outTransY, const SkPaint& paint,
                        GrColor color, const SkMaskFilter::BlurRec& blurRec,
                        const SkMatrix& viewMatrix, SkScalar x, SkScalar y);

    // flush a GrAtlasTextBlob associated with a SkTextBlob
    void flushCached(GrContext* context,
                     GrDrawContext* dc,
                     const SkTextBlob* blob,
                     const SkSurfaceProps& props,
                     const GrDistanceFieldAdjustTable* distanceAdjustTable,
                     const SkPaint& skPaint,
                     const GrPaint& grPaint,
                     SkDrawFilter* drawFilter,
                     const GrClip& clip,
                     const SkMatrix& viewMatrix,
                     const SkIRect& clipBounds,
                     SkScalar x, SkScalar y,
                     SkScalar transX, SkScalar transY);

    // flush a throwaway GrAtlasTextBlob *not* associated with an SkTextBlob
    void flushThrowaway(GrContext* context,
                        GrDrawContext* dc,
                        const SkSurfaceProps& props,
                        const GrDistanceFieldAdjustTable* distanceAdjustTable,
                        const SkPaint& skPaint,
                        const GrPaint& grPaint,
                        const GrClip& clip,
                        const SkIRect& clipBounds);

    // position + local coord
    static const size_t kColorTextVASize = sizeof(SkPoint) + sizeof(SkIPoint16);
    static const size_t kGrayTextVASize = sizeof(SkPoint) + sizeof(GrColor) + sizeof(SkIPoint16);
    static const size_t kLCDTextVASize = kGrayTextVASize;
    static const size_t kMaxVASize = kGrayTextVASize;
    static const int kVerticesPerGlyph = 4;

    static void AssertEqual(const GrAtlasTextBlob&, const GrAtlasTextBlob&);

    // The color here is the GrPaint color, and it is used to determine whether we
    // have to regenerate LCD text blobs.
    // We use this color vs the SkPaint color because it has the colorfilter applied.
    void initReusableBlob(GrColor color, const SkMatrix& viewMatrix, SkScalar x, SkScalar y) {
        fPaintColor = color;
        this->setupViewMatrix(viewMatrix, x, y);
    }

    void initThrowawayBlob(const SkMatrix& viewMatrix, SkScalar x, SkScalar y) {
        this->setupViewMatrix(viewMatrix, x, y);
    }

    GrDrawBatch* test_createBatch(int glyphCount, int run, int subRun,
                                  GrColor color, SkScalar transX, SkScalar transY,
                                  const SkPaint& skPaint, const SkSurfaceProps& props,
                                  const GrDistanceFieldAdjustTable* distanceAdjustTable,
                                  GrBatchFontCache* cache);

    const Key& key() const { return fKey; }

    ~GrAtlasTextBlob() {
        for (int i = 0; i < fRunCount; i++) {
            fRuns[i].~Run();
        }
    }

private:
    GrAtlasTextBlob()
        : fMaxMinScale(-SK_ScalarMax)
        , fMinMaxScale(SK_ScalarMax)
        , fTextType(0) {}

    void appendLargeGlyph(GrGlyph* glyph, GrFontScaler* scaler, const SkGlyph& skGlyph,
                          SkScalar x, SkScalar y, SkScalar scale, bool applyVM);

    inline void flushRun(GrDrawContext* dc, GrPipelineBuilder* pipelineBuilder,
                         int run, GrColor color,
                         SkScalar transX, SkScalar transY,
                         const SkPaint& skPaint, const SkSurfaceProps& props,
                         const GrDistanceFieldAdjustTable* distanceAdjustTable,
                         GrBatchFontCache* cache);

    void flushBigGlyphs(GrContext* context, GrDrawContext* dc,
                        const GrClip& clip, const SkPaint& skPaint,
                        SkScalar transX, SkScalar transY,
                        const SkIRect& clipBounds);

    void flushRunAsPaths(GrContext* context,
                         GrDrawContext* dc,
                         const SkSurfaceProps& props,
                         const SkTextBlobRunIterator& it,
                         const GrClip& clip, const SkPaint& skPaint,
                         SkDrawFilter* drawFilter, const SkMatrix& viewMatrix,
                         const SkIRect& clipBounds, SkScalar x, SkScalar y);

    // This function will only be called when we are regenerating a blob from scratch. We record the
    // initial view matrix and initial offsets(x,y), because we record vertex bounds relative to
    // these numbers.  When blobs are reused with new matrices, we need to return to model space so
    // we can update the vertex bounds appropriately.
    void setupViewMatrix(const SkMatrix& viewMatrix, SkScalar x, SkScalar y) {
        fViewMatrix = viewMatrix;
        if (!viewMatrix.invert(&fInitialViewMatrixInverse)) {
            fInitialViewMatrixInverse = SkMatrix::I();
            SkDebugf("Could not invert viewmatrix\n");
        }
        fX = fInitialX = x;
        fY = fInitialY = y;
    }

    /*
     * Each Run inside of the blob can have its texture coordinates regenerated if required.
     * To determine if regeneration is necessary, fAtlasGeneration is used.  If there have been
     * any evictions inside of the atlas, then we will simply regenerate Runs.  We could track
     * this at a more fine grained level, but its not clear if this is worth it, as evictions
     * should be fairly rare.
     *
     * One additional point, each run can contain glyphs with any of the three mask formats.
     * We call these SubRuns.  Because a subrun must be a contiguous range, we have to create
     * a new subrun each time the mask format changes in a run.  In theory, a run can have as
     * many SubRuns as it has glyphs, ie if a run alternates between color emoji and A8.  In
     * practice, the vast majority of runs have only a single subrun.
     *
     * Finally, for runs where the entire thing is too large for the GrAtlasTextContext to
     * handle, we have a bit to mark the run as flusahable via rendering as paths.  It is worth
     * pointing. It would be a bit expensive to figure out ahead of time whether or not a run
     * can flush in this manner, so we always allocate vertices for the run, regardless of
     * whether or not it is too large.  The benefit of this strategy is that we can always reuse
     * a blob allocation regardless of viewmatrix changes.  We could store positions for these
     * glyphs.  However, its not clear if this is a win because we'd still have to either go the
     * glyph cache to get the path at flush time, or hold onto the path in the cache, which
     * would greatly increase the memory of these cached items.
     */
    struct Run {
        Run()
            : fInitialized(false)
            , fDrawAsPaths(false) {
            // To ensure we always have one subrun, we push back a fresh run here
            fSubRunInfo.push_back();
        }
        struct SubRunInfo {
            SubRunInfo()
                : fAtlasGeneration(GrBatchAtlas::kInvalidAtlasGeneration)
                , fVertexStartIndex(0)
                , fVertexEndIndex(0)
                , fGlyphStartIndex(0)
                , fGlyphEndIndex(0)
                , fColor(GrColor_ILLEGAL)
                , fMaskFormat(kA8_GrMaskFormat)
                , fDrawAsDistanceFields(false)
                , fUseLCDText(false) {
                fVertexBounds.setLargestInverted();
            }
            SubRunInfo(const SubRunInfo& that)
                : fBulkUseToken(that.fBulkUseToken)
                , fStrike(SkSafeRef(that.fStrike.get()))
                , fVertexBounds(that.fVertexBounds)
                , fAtlasGeneration(that.fAtlasGeneration)
                , fVertexStartIndex(that.fVertexStartIndex)
                , fVertexEndIndex(that.fVertexEndIndex)
                , fGlyphStartIndex(that.fGlyphStartIndex)
                , fGlyphEndIndex(that.fGlyphEndIndex)
                , fColor(that.fColor)
                , fMaskFormat(that.fMaskFormat)
                , fDrawAsDistanceFields(that.fDrawAsDistanceFields)
                , fUseLCDText(that.fUseLCDText) {
            }

            // TODO when this object is more internal, drop the privacy
            void resetBulkUseToken() { fBulkUseToken.reset(); }
            GrBatchAtlas::BulkUseTokenUpdater* bulkUseToken() { return &fBulkUseToken; }
            void setStrike(GrBatchTextStrike* strike) { fStrike.reset(SkRef(strike)); }
            GrBatchTextStrike* strike() const { return fStrike.get(); }

            void setAtlasGeneration(uint64_t atlasGeneration) { fAtlasGeneration = atlasGeneration;}
            uint64_t atlasGeneration() const { return fAtlasGeneration; }

            size_t byteCount() const { return fVertexEndIndex - fVertexStartIndex; }
            size_t vertexStartIndex() const { return fVertexStartIndex; }
            size_t vertexEndIndex() const { return fVertexEndIndex; }
            void appendVertices(size_t vertexStride) {
                fVertexEndIndex += vertexStride * kVerticesPerGlyph;
            }

            uint32_t glyphCount() const { return fGlyphEndIndex - fGlyphStartIndex; }
            uint32_t glyphStartIndex() const { return fGlyphStartIndex; }
            uint32_t glyphEndIndex() const { return fGlyphEndIndex; }
            void glyphAppended() { fGlyphEndIndex++; }
            void setColor(GrColor color) { fColor = color; }
            GrColor color() const { return fColor; }
            void setMaskFormat(GrMaskFormat format) { fMaskFormat = format; }
            GrMaskFormat maskFormat() const { return fMaskFormat; }

            void setAsSuccessor(const SubRunInfo& prev) {
                fGlyphStartIndex = prev.glyphEndIndex();
                fGlyphEndIndex = prev.glyphEndIndex();

                fVertexStartIndex = prev.vertexEndIndex();
                fVertexEndIndex = prev.vertexEndIndex();
            }

            const SkRect& vertexBounds() const { return fVertexBounds; }
            void joinGlyphBounds(const SkRect& glyphBounds) {
                fVertexBounds.joinNonEmptyArg(glyphBounds);
            }

            // df properties
            void setUseLCDText(bool useLCDText) { fUseLCDText = useLCDText; }
            bool hasUseLCDText() const { return fUseLCDText; }
            void setDrawAsDistanceFields() { fDrawAsDistanceFields = true; }
            bool drawAsDistanceFields() const { return fDrawAsDistanceFields; }

        private:
            GrBatchAtlas::BulkUseTokenUpdater fBulkUseToken;
            SkAutoTUnref<GrBatchTextStrike> fStrike;
            SkRect fVertexBounds;
            uint64_t fAtlasGeneration;
            size_t fVertexStartIndex;
            size_t fVertexEndIndex;
            uint32_t fGlyphStartIndex;
            uint32_t fGlyphEndIndex;
            GrColor fColor;
            GrMaskFormat fMaskFormat;
            bool fDrawAsDistanceFields; // df property
            bool fUseLCDText; // df property
        };

        SubRunInfo& push_back() {
            // Forward glyph / vertex information to seed the new sub run
            SubRunInfo& newSubRun = fSubRunInfo.push_back();
            const SubRunInfo& prevSubRun = fSubRunInfo.fromBack(1);

            newSubRun.setAsSuccessor(prevSubRun);
            return newSubRun;
        }
        static const int kMinSubRuns = 1;
        SkAutoTUnref<SkTypeface> fTypeface;
        SkSTArray<kMinSubRuns, SubRunInfo> fSubRunInfo;
        SkAutoDescriptor fDescriptor;

        // Distance field text cannot draw coloremoji, and so has to fall back.  However,
        // though the distance field text and the coloremoji may share the same run, they
        // will have different descriptors.  If fOverrideDescriptor is non-nullptr, then it
        // will be used in place of the run's descriptor to regen texture coords
        SkAutoTDelete<SkAutoDescriptor> fOverrideDescriptor; // df properties
        bool fInitialized;
        bool fDrawAsPaths;
    };

    inline GrDrawBatch* createBatch(const Run::SubRunInfo& info,
                                    int glyphCount, int run, int subRun,
                                    GrColor color, SkScalar transX, SkScalar transY,
                                    const SkPaint& skPaint, const SkSurfaceProps& props,
                                    const GrDistanceFieldAdjustTable* distanceAdjustTable,
                                    GrBatchFontCache* cache);

    struct BigGlyph {
        BigGlyph(const SkPath& path, SkScalar vx, SkScalar vy, SkScalar scale, bool applyVM)
            : fPath(path)
            , fVx(vx)
            , fVy(vy)
            , fScale(scale)
            , fApplyVM(applyVM) {}
        SkPath fPath;
        SkScalar fVx;
        SkScalar fVy;
        SkScalar fScale;
        bool fApplyVM;
    };

    struct StrokeInfo {
        SkScalar fFrameWidth;
        SkScalar fMiterLimit;
        SkPaint::Join fJoin;
    };

    enum TextType {
        kHasDistanceField_TextType = 0x1,
        kHasBitmap_TextType = 0x2,
    };

    // all glyph / vertex offsets are into these pools.
    unsigned char* fVertices;
    GrGlyph** fGlyphs;
    Run* fRuns;
    GrMemoryPool* fPool;
    SkMaskFilter::BlurRec fBlurRec;
    StrokeInfo fStrokeInfo;
    SkTArray<BigGlyph> fBigGlyphs;
    Key fKey;
    SkMatrix fViewMatrix;
    SkMatrix fInitialViewMatrixInverse;
    size_t fSize;
    GrColor fPaintColor;
    SkScalar fInitialX;
    SkScalar fInitialY;
    SkScalar fX;
    SkScalar fY;

    // We can reuse distance field text, but only if the new viewmatrix would not result in
    // a mip change.  Because there can be multiple runs in a blob, we track the overall
    // maximum minimum scale, and minimum maximum scale, we can support before we need to regen
    SkScalar fMaxMinScale;
    SkScalar fMinMaxScale;
    int fRunCount;
    uint8_t fTextType;

    friend class GrAtlasTextBatch; // We might be able to get rid of this friending
};

#endif