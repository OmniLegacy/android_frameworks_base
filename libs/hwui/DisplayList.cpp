/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_VIEW

#include <SkCanvas.h>
#include <algorithm>

#include <utils/Trace.h>

#include "Debug.h"
#include "DisplayList.h"
#include "DisplayListOp.h"
#include "DisplayListLogBuffer.h"

namespace android {
namespace uirenderer {

void RenderNode::outputLogBuffer(int fd) {
    DisplayListLogBuffer& logBuffer = DisplayListLogBuffer::getInstance();
    if (logBuffer.isEmpty()) {
        return;
    }

    FILE *file = fdopen(fd, "a");

    fprintf(file, "\nRecent DisplayList operations\n");
    logBuffer.outputCommands(file);

    String8 cachesLog;
    Caches::getInstance().dumpMemoryUsage(cachesLog);
    fprintf(file, "\nCaches:\n%s", cachesLog.string());
    fprintf(file, "\n");

    fflush(file);
}

RenderNode::RenderNode() : mDestroyed(false), mDisplayListData(0) {
}

RenderNode::~RenderNode() {
    LOG_ALWAYS_FATAL_IF(mDestroyed, "Double destroyed DisplayList %p", this);

    mDestroyed = true;
    delete mDisplayListData;
}

void RenderNode::destroyDisplayListDeferred(RenderNode* displayList) {
    if (displayList) {
        DISPLAY_LIST_LOGD("Deferring display list destruction");
        Caches::getInstance().deleteDisplayListDeferred(displayList);
    }
}

void RenderNode::setData(DisplayListData* data) {
    delete mDisplayListData;
    mDisplayListData = data;
    if (mDisplayListData) {
        Caches::getInstance().registerFunctors(mDisplayListData->functorCount);
    }
}

/**
 * This function is a simplified version of replay(), where we simply retrieve and log the
 * display list. This function should remain in sync with the replay() function.
 */
void RenderNode::output(uint32_t level) {
    ALOGD("%*sStart display list (%p, %s, render=%d)", (level - 1) * 2, "", this,
            mName.string(), isRenderable());
    ALOGD("%*s%s %d", level * 2, "", "Save",
            SkCanvas::kMatrix_SaveFlag | SkCanvas::kClip_SaveFlag);

    outputViewProperties(level);
    int flags = DisplayListOp::kOpLogFlag_Recurse;
    for (unsigned int i = 0; i < mDisplayListData->displayListOps.size(); i++) {
        mDisplayListData->displayListOps[i]->output(level, flags);
    }

    ALOGD("%*sDone (%p, %s)", (level - 1) * 2, "", this, mName.string());
}

void RenderNode::outputViewProperties(const int level) {
    properties().updateMatrix();
    if (properties().mLeft != 0 || properties().mTop != 0) {
        ALOGD("%*sTranslate (left, top) %d, %d", level * 2, "", properties().mLeft, properties().mTop);
    }
    if (properties().mStaticMatrix) {
        ALOGD("%*sConcatMatrix (static) %p: " SK_MATRIX_STRING,
                level * 2, "", properties().mStaticMatrix, SK_MATRIX_ARGS(properties().mStaticMatrix));
    }
    if (properties().mAnimationMatrix) {
        ALOGD("%*sConcatMatrix (animation) %p: " SK_MATRIX_STRING,
                level * 2, "", properties().mAnimationMatrix, SK_MATRIX_ARGS(properties().mAnimationMatrix));
    }
    if (properties().mMatrixFlags != 0) {
        if (properties().mMatrixFlags == TRANSLATION) {
            ALOGD("%*sTranslate %.2f, %.2f, %.2f",
                    level * 2, "", properties().mTranslationX, properties().mTranslationY, properties().mTranslationZ);
        } else {
            ALOGD("%*sConcatMatrix %p: " MATRIX_4_STRING,
                    level * 2, "", properties().mTransformMatrix, MATRIX_4_ARGS(properties().mTransformMatrix));
        }
    }

    bool clipToBoundsNeeded = properties().mCaching ? false : properties().mClipToBounds;
    if (properties().mAlpha < 1) {
        if (properties().mCaching) {
            ALOGD("%*sSetOverrideLayerAlpha %.2f", level * 2, "", properties().mAlpha);
        } else if (!properties().mHasOverlappingRendering) {
            ALOGD("%*sScaleAlpha %.2f", level * 2, "", properties().mAlpha);
        } else {
            int flags = SkCanvas::kHasAlphaLayer_SaveFlag;
            if (clipToBoundsNeeded) {
                flags |= SkCanvas::kClipToLayer_SaveFlag;
                clipToBoundsNeeded = false; // clipping done by save layer
            }
            ALOGD("%*sSaveLayerAlpha %.2f, %.2f, %.2f, %.2f, %d, 0x%x", level * 2, "",
                    (float) 0, (float) 0, (float) properties().mRight - properties().mLeft, (float) properties().mBottom - properties().mTop,
                    (int)(properties().mAlpha * 255), flags);
        }
    }
    if (clipToBoundsNeeded) {
        ALOGD("%*sClipRect %.2f, %.2f, %.2f, %.2f", level * 2, "", 0.0f, 0.0f,
                (float) properties().mRight - properties().mLeft, (float) properties().mBottom - properties().mTop);
    }
}

/*
 * For property operations, we pass a savecount of 0, since the operations aren't part of the
 * displaylist, and thus don't have to compensate for the record-time/playback-time discrepancy in
 * base saveCount (i.e., how RestoreToCount uses saveCount + properties().mCount)
 */
#define PROPERTY_SAVECOUNT 0

template <class T>
void RenderNode::setViewProperties(OpenGLRenderer& renderer, T& handler,
        const int level) {
#if DEBUG_DISPLAY_LIST
    outputViewProperties(level);
#endif
    properties().updateMatrix();
    if (properties().mLeft != 0 || properties().mTop != 0) {
        renderer.translate(properties().mLeft, properties().mTop);
    }
    if (properties().mStaticMatrix) {
        renderer.concatMatrix(properties().mStaticMatrix);
    } else if (properties().mAnimationMatrix) {
        renderer.concatMatrix(properties().mAnimationMatrix);
    }
    if (properties().mMatrixFlags != 0) {
        if (properties().mMatrixFlags == TRANSLATION) {
            renderer.translate(properties().mTranslationX, properties().mTranslationY);
        } else {
            renderer.concatMatrix(*properties().mTransformMatrix);
        }
    }
    bool clipToBoundsNeeded = properties().mCaching ? false : properties().mClipToBounds;
    if (properties().mAlpha < 1) {
        if (properties().mCaching) {
            renderer.setOverrideLayerAlpha(properties().mAlpha);
        } else if (!properties().mHasOverlappingRendering) {
            renderer.scaleAlpha(properties().mAlpha);
        } else {
            // TODO: should be able to store the size of a DL at record time and not
            // have to pass it into this call. In fact, this information might be in the
            // location/size info that we store with the new native transform data.
            int saveFlags = SkCanvas::kHasAlphaLayer_SaveFlag;
            if (clipToBoundsNeeded) {
                saveFlags |= SkCanvas::kClipToLayer_SaveFlag;
                clipToBoundsNeeded = false; // clipping done by saveLayer
            }

            SaveLayerOp* op = new (handler.allocator()) SaveLayerOp(
                    0, 0, properties().mRight - properties().mLeft, properties().mBottom - properties().mTop, properties().mAlpha * 255, saveFlags);
            handler(op, PROPERTY_SAVECOUNT, properties().mClipToBounds);
        }
    }
    if (clipToBoundsNeeded) {
        ClipRectOp* op = new (handler.allocator()) ClipRectOp(0, 0,
                properties().mRight - properties().mLeft, properties().mBottom - properties().mTop, SkRegion::kIntersect_Op);
        handler(op, PROPERTY_SAVECOUNT, properties().mClipToBounds);
    }
    if (CC_UNLIKELY(properties().mClipToOutline && !properties().mOutline.isEmpty())) {
        ClipPathOp* op = new (handler.allocator()) ClipPathOp(&properties().mOutline, SkRegion::kIntersect_Op);
        handler(op, PROPERTY_SAVECOUNT, properties().mClipToBounds);
    }
}

/**
 * Apply property-based transformations to input matrix
 *
 * If true3dTransform is set to true, the transform applied to the input matrix will use true 4x4
 * matrix computation instead of the Skia 3x3 matrix + camera hackery.
 */
void RenderNode::applyViewPropertyTransforms(mat4& matrix, bool true3dTransform) {
    if (properties().mLeft != 0 || properties().mTop != 0) {
        matrix.translate(properties().mLeft, properties().mTop);
    }
    if (properties().mStaticMatrix) {
        mat4 stat(*properties().mStaticMatrix);
        matrix.multiply(stat);
    } else if (properties().mAnimationMatrix) {
        mat4 anim(*properties().mAnimationMatrix);
        matrix.multiply(anim);
    }
    if (properties().mMatrixFlags != 0) {
        properties().updateMatrix();
        if (properties().mMatrixFlags == TRANSLATION) {
            matrix.translate(properties().mTranslationX, properties().mTranslationY,
                    true3dTransform ? properties().mTranslationZ : 0.0f);
        } else {
            if (!true3dTransform) {
                matrix.multiply(*properties().mTransformMatrix);
            } else {
                mat4 true3dMat;
                true3dMat.loadTranslate(
                        properties().mPivotX + properties().mTranslationX,
                        properties().mPivotY + properties().mTranslationY,
                        properties().mTranslationZ);
                true3dMat.rotate(properties().mRotationX, 1, 0, 0);
                true3dMat.rotate(properties().mRotationY, 0, 1, 0);
                true3dMat.rotate(properties().mRotation, 0, 0, 1);
                true3dMat.scale(properties().mScaleX, properties().mScaleY, 1);
                true3dMat.translate(-properties().mPivotX, -properties().mPivotY);

                matrix.multiply(true3dMat);
            }
        }
    }
}

/**
 * Organizes the DisplayList hierarchy to prepare for background projection reordering.
 *
 * This should be called before a call to defer() or drawDisplayList()
 *
 * Each DisplayList that serves as a 3d root builds its list of composited children,
 * which are flagged to not draw in the standard draw loop.
 */
void RenderNode::computeOrdering() {
    ATRACE_CALL();
    mProjectedNodes.clear();

    // TODO: create temporary DDLOp and call computeOrderingImpl on top DisplayList so that
    // transform properties are applied correctly to top level children
    if (mDisplayListData == NULL) return;
    for (unsigned int i = 0; i < mDisplayListData->children.size(); i++) {
        DrawDisplayListOp* childOp = mDisplayListData->children[i];
        childOp->mDisplayList->computeOrderingImpl(childOp,
                &mProjectedNodes, &mat4::identity());
    }
}

void RenderNode::computeOrderingImpl(
        DrawDisplayListOp* opState,
        Vector<DrawDisplayListOp*>* compositedChildrenOfProjectionSurface,
        const mat4* transformFromProjectionSurface) {
    mProjectedNodes.clear();
    if (mDisplayListData == NULL || mDisplayListData->isEmpty()) return;

    // TODO: should avoid this calculation in most cases
    // TODO: just calculate single matrix, down to all leaf composited elements
    Matrix4 localTransformFromProjectionSurface(*transformFromProjectionSurface);
    localTransformFromProjectionSurface.multiply(opState->mTransformFromParent);

    if (properties().mProjectBackwards) {
        // composited projectee, flag for out of order draw, save matrix, and store in proj surface
        opState->mSkipInOrderDraw = true;
        opState->mTransformFromCompositingAncestor.load(localTransformFromProjectionSurface);
        compositedChildrenOfProjectionSurface->add(opState);
    } else {
        // standard in order draw
        opState->mSkipInOrderDraw = false;
    }

    if (mDisplayListData->children.size() > 0) {
        const bool isProjectionReceiver = mDisplayListData->projectionReceiveIndex >= 0;
        bool haveAppliedPropertiesToProjection = false;
        for (unsigned int i = 0; i < mDisplayListData->children.size(); i++) {
            DrawDisplayListOp* childOp = mDisplayListData->children[i];
            RenderNode* child = childOp->mDisplayList;

            Vector<DrawDisplayListOp*>* projectionChildren = NULL;
            const mat4* projectionTransform = NULL;
            if (isProjectionReceiver && !child->properties().mProjectBackwards) {
                // if receiving projections, collect projecting descendent

                // Note that if a direct descendent is projecting backwards, we pass it's
                // grandparent projection collection, since it shouldn't project onto it's
                // parent, where it will already be drawing.
                projectionChildren = &mProjectedNodes;
                projectionTransform = &mat4::identity();
            } else {
                if (!haveAppliedPropertiesToProjection) {
                    applyViewPropertyTransforms(localTransformFromProjectionSurface);
                    haveAppliedPropertiesToProjection = true;
                }
                projectionChildren = compositedChildrenOfProjectionSurface;
                projectionTransform = &localTransformFromProjectionSurface;
            }
            child->computeOrderingImpl(childOp, projectionChildren, projectionTransform);
        }
    }

}

class DeferOperationHandler {
public:
    DeferOperationHandler(DeferStateStruct& deferStruct, int level)
        : mDeferStruct(deferStruct), mLevel(level) {}
    inline void operator()(DisplayListOp* operation, int saveCount, bool clipToBounds) {
        operation->defer(mDeferStruct, saveCount, mLevel, clipToBounds);
    }
    inline LinearAllocator& allocator() { return *(mDeferStruct.mAllocator); }

private:
    DeferStateStruct& mDeferStruct;
    const int mLevel;
};

void RenderNode::defer(DeferStateStruct& deferStruct, const int level) {
    DeferOperationHandler handler(deferStruct, level);
    iterate<DeferOperationHandler>(deferStruct.mRenderer, handler, level);
}

class ReplayOperationHandler {
public:
    ReplayOperationHandler(ReplayStateStruct& replayStruct, int level)
        : mReplayStruct(replayStruct), mLevel(level) {}
    inline void operator()(DisplayListOp* operation, int saveCount, bool clipToBounds) {
#if DEBUG_DISPLAY_LIST_OPS_AS_EVENTS
        properties().mReplayStruct.mRenderer.eventMark(operation->name());
#endif
        operation->replay(mReplayStruct, saveCount, mLevel, clipToBounds);
    }
    inline LinearAllocator& allocator() { return *(mReplayStruct.mAllocator); }

private:
    ReplayStateStruct& mReplayStruct;
    const int mLevel;
};

void RenderNode::replay(ReplayStateStruct& replayStruct, const int level) {
    ReplayOperationHandler handler(replayStruct, level);

    replayStruct.mRenderer.startMark(mName.string());
    iterate<ReplayOperationHandler>(replayStruct.mRenderer, handler, level);
    replayStruct.mRenderer.endMark();

    DISPLAY_LIST_LOGD("%*sDone (%p, %s), returning %d", level * 2, "", this, mName.string(),
            replayStruct.mDrawGlStatus);
}

void RenderNode::buildZSortedChildList(Vector<ZDrawDisplayListOpPair>& zTranslatedNodes) {
    if (mDisplayListData == NULL || mDisplayListData->children.size() == 0) return;

    for (unsigned int i = 0; i < mDisplayListData->children.size(); i++) {
        DrawDisplayListOp* childOp = mDisplayListData->children[i];
        RenderNode* child = childOp->mDisplayList;
        float childZ = child->properties().mTranslationZ;

        if (childZ != 0.0f) {
            zTranslatedNodes.add(ZDrawDisplayListOpPair(childZ, childOp));
            childOp->mSkipInOrderDraw = true;
        } else if (!child->properties().mProjectBackwards) {
            // regular, in order drawing DisplayList
            childOp->mSkipInOrderDraw = false;
        }
    }

    // Z sort 3d children (stable-ness makes z compare fall back to standard drawing order)
    std::stable_sort(zTranslatedNodes.begin(), zTranslatedNodes.end());
}

#define SHADOW_DELTA 0.1f

template <class T>
void RenderNode::iterate3dChildren(const Vector<ZDrawDisplayListOpPair>& zTranslatedNodes,
        ChildrenSelectMode mode, OpenGLRenderer& renderer, T& handler) {
    const int size = zTranslatedNodes.size();
    if (size == 0
            || (mode == kNegativeZChildren && zTranslatedNodes[0].key > 0.0f)
            || (mode == kPositiveZChildren && zTranslatedNodes[size - 1].key < 0.0f)) {
        // no 3d children to draw
        return;
    }

    int rootRestoreTo = renderer.save(SkCanvas::kMatrix_SaveFlag | SkCanvas::kClip_SaveFlag);
    LinearAllocator& alloc = handler.allocator();
    ClipRectOp* clipOp = new (alloc) ClipRectOp(0, 0, properties().mWidth, properties().mHeight,
            SkRegion::kIntersect_Op); // clip to 3d root bounds
    handler(clipOp, PROPERTY_SAVECOUNT, properties().mClipToBounds);

    /**
     * Draw shadows and (potential) casters mostly in order, but allow the shadows of casters
     * with very similar Z heights to draw together.
     *
     * This way, if Views A & B have the same Z height and are both casting shadows, the shadows are
     * underneath both, and neither's shadow is drawn on top of the other.
     */
    const size_t nonNegativeIndex = findNonNegativeIndex(zTranslatedNodes);
    size_t drawIndex, shadowIndex, endIndex;
    if (mode == kNegativeZChildren) {
        drawIndex = 0;
        endIndex = nonNegativeIndex;
        shadowIndex = endIndex; // draw no shadows
    } else {
        drawIndex = nonNegativeIndex;
        endIndex = size;
        shadowIndex = drawIndex; // potentially draw shadow for each pos Z child
    }
    float lastCasterZ = 0.0f;
    while (shadowIndex < endIndex || drawIndex < endIndex) {
        if (shadowIndex < endIndex) {
            DrawDisplayListOp* casterOp = zTranslatedNodes[shadowIndex].value;
            RenderNode* caster = casterOp->mDisplayList;
            const float casterZ = zTranslatedNodes[shadowIndex].key;
            // attempt to render the shadow if the caster about to be drawn is its caster,
            // OR if its caster's Z value is similar to the previous potential caster
            if (shadowIndex == drawIndex || casterZ - lastCasterZ < SHADOW_DELTA) {

                if (caster->properties().mCastsShadow && caster->properties().mAlpha > 0.0f) {
                    mat4 shadowMatrixXY(casterOp->mTransformFromParent);
                    caster->applyViewPropertyTransforms(shadowMatrixXY);

                    // Z matrix needs actual 3d transformation, so mapped z values will be correct
                    mat4 shadowMatrixZ(casterOp->mTransformFromParent);
                    caster->applyViewPropertyTransforms(shadowMatrixZ, true);

                    DisplayListOp* shadowOp  = new (alloc) DrawShadowOp(
                            shadowMatrixXY, shadowMatrixZ,
                            caster->properties().mAlpha, &(caster->properties().mOutline),
                            caster->properties().mWidth, caster->properties().mHeight);
                    handler(shadowOp, PROPERTY_SAVECOUNT, properties().mClipToBounds);
                }

                lastCasterZ = casterZ; // must do this even if current caster not casting a shadow
                shadowIndex++;
                continue;
            }
        }

        // only the actual child DL draw needs to be in save/restore,
        // since it modifies the renderer's matrix
        int restoreTo = renderer.save(SkCanvas::kMatrix_SaveFlag);

        DrawDisplayListOp* childOp = zTranslatedNodes[drawIndex].value;
        RenderNode* child = childOp->mDisplayList;

        renderer.concatMatrix(childOp->mTransformFromParent);
        childOp->mSkipInOrderDraw = false; // this is horrible, I'm so sorry everyone
        handler(childOp, renderer.getSaveCount() - 1, properties().mClipToBounds);
        childOp->mSkipInOrderDraw = true;

        renderer.restoreToCount(restoreTo);
        drawIndex++;
    }
    handler(new (alloc) RestoreToCountOp(rootRestoreTo), PROPERTY_SAVECOUNT, properties().mClipToBounds);
}

template <class T>
void RenderNode::iterateProjectedChildren(OpenGLRenderer& renderer, T& handler, const int level) {
    int rootRestoreTo = renderer.save(SkCanvas::kMatrix_SaveFlag | SkCanvas::kClip_SaveFlag);
    LinearAllocator& alloc = handler.allocator();
    ClipRectOp* clipOp = new (alloc) ClipRectOp(0, 0, properties().mWidth, properties().mHeight,
            SkRegion::kReplace_Op); // clip to projection surface root bounds
    handler(clipOp, PROPERTY_SAVECOUNT, properties().mClipToBounds);

    for (size_t i = 0; i < mProjectedNodes.size(); i++) {
        DrawDisplayListOp* childOp = mProjectedNodes[i];

        // matrix save, concat, and restore can be done safely without allocating operations
        int restoreTo = renderer.save(SkCanvas::kMatrix_SaveFlag);
        renderer.concatMatrix(childOp->mTransformFromCompositingAncestor);
        childOp->mSkipInOrderDraw = false; // this is horrible, I'm so sorry everyone
        handler(childOp, renderer.getSaveCount() - 1, properties().mClipToBounds);
        childOp->mSkipInOrderDraw = true;
        renderer.restoreToCount(restoreTo);
    }
    handler(new (alloc) RestoreToCountOp(rootRestoreTo), PROPERTY_SAVECOUNT, properties().mClipToBounds);
}

/**
 * This function serves both defer and replay modes, and will organize the displayList's component
 * operations for a single frame:
 *
 * Every 'simple' state operation that affects just the matrix and alpha (or other factors of
 * DeferredDisplayState) may be issued directly to the renderer, but complex operations (with custom
 * defer logic) and operations in displayListOps are issued through the 'handler' which handles the
 * defer vs replay logic, per operation
 */
template <class T>
void RenderNode::iterate(OpenGLRenderer& renderer, T& handler, const int level) {
    if (CC_UNLIKELY(mDestroyed)) { // temporary debug logging
        ALOGW("Error: %s is drawing after destruction", mName.string());
        CRASH();
    }
    if (mDisplayListData->isEmpty() || properties().mAlpha <= 0) {
        DISPLAY_LIST_LOGD("%*sEmpty display list (%p, %s)", level * 2, "", this, mName.string());
        return;
    }

#if DEBUG_DISPLAY_LIST
    Rect* clipRect = renderer.getClipRect();
    DISPLAY_LIST_LOGD("%*sStart display list (%p, %s), clipRect: %.0f, %.0f, %.0f, %.0f",
            level * 2, "", this, mName.string(), clipRect->left, clipRect->top,
            clipRect->right, clipRect->bottom);
#endif

    LinearAllocator& alloc = handler.allocator();
    int restoreTo = renderer.getSaveCount();
    handler(new (alloc) SaveOp(SkCanvas::kMatrix_SaveFlag | SkCanvas::kClip_SaveFlag),
            PROPERTY_SAVECOUNT, properties().mClipToBounds);

    DISPLAY_LIST_LOGD("%*sSave %d %d", (level + 1) * 2, "",
            SkCanvas::kMatrix_SaveFlag | SkCanvas::kClip_SaveFlag, restoreTo);

    setViewProperties<T>(renderer, handler, level + 1);

    bool quickRejected = properties().mClipToBounds && renderer.quickRejectConservative(0, 0, properties().mWidth, properties().mHeight);
    if (!quickRejected) {
        Vector<ZDrawDisplayListOpPair> zTranslatedNodes;
        buildZSortedChildList(zTranslatedNodes);

        // for 3d root, draw children with negative z values
        iterate3dChildren(zTranslatedNodes, kNegativeZChildren, renderer, handler);

        DisplayListLogBuffer& logBuffer = DisplayListLogBuffer::getInstance();
        const int saveCountOffset = renderer.getSaveCount() - 1;
        const int projectionReceiveIndex = mDisplayListData->projectionReceiveIndex;
        for (unsigned int i = 0; i < mDisplayListData->displayListOps.size(); i++) {
            DisplayListOp *op = mDisplayListData->displayListOps[i];

#if DEBUG_DISPLAY_LIST
            op->output(level + 1);
#endif

            logBuffer.writeCommand(level, op->name());
            handler(op, saveCountOffset, properties().mClipToBounds);

            if (CC_UNLIKELY(i == projectionReceiveIndex && mProjectedNodes.size() > 0)) {
                iterateProjectedChildren(renderer, handler, level);
            }
        }

        // for 3d root, draw children with positive z values
        iterate3dChildren(zTranslatedNodes, kPositiveZChildren, renderer, handler);
    }

    DISPLAY_LIST_LOGD("%*sRestoreToCount %d", (level + 1) * 2, "", restoreTo);
    handler(new (alloc) RestoreToCountOp(restoreTo),
            PROPERTY_SAVECOUNT, properties().mClipToBounds);
    renderer.setOverrideLayerAlpha(1.0f);
}

void DisplayListData::cleanupResources() {
    Caches& caches = Caches::getInstance();
    caches.unregisterFunctors(functorCount);
    caches.resourceCache.lock();

    for (size_t i = 0; i < bitmapResources.size(); i++) {
        caches.resourceCache.decrementRefcountLocked(bitmapResources.itemAt(i));
    }

    for (size_t i = 0; i < ownedBitmapResources.size(); i++) {
        const SkBitmap* bitmap = ownedBitmapResources.itemAt(i);
        caches.resourceCache.decrementRefcountLocked(bitmap);
        caches.resourceCache.destructorLocked(bitmap);
    }

    for (size_t i = 0; i < patchResources.size(); i++) {
        caches.resourceCache.decrementRefcountLocked(patchResources.itemAt(i));
    }

    for (size_t i = 0; i < shaders.size(); i++) {
        caches.resourceCache.decrementRefcountLocked(shaders.itemAt(i));
        caches.resourceCache.destructorLocked(shaders.itemAt(i));
    }

    for (size_t i = 0; i < sourcePaths.size(); i++) {
        caches.resourceCache.decrementRefcountLocked(sourcePaths.itemAt(i));
    }

    for (size_t i = 0; i < layers.size(); i++) {
        caches.resourceCache.decrementRefcountLocked(layers.itemAt(i));
    }

    caches.resourceCache.unlock();

    for (size_t i = 0; i < paints.size(); i++) {
        delete paints.itemAt(i);
    }

    for (size_t i = 0; i < regions.size(); i++) {
        delete regions.itemAt(i);
    }

    for (size_t i = 0; i < paths.size(); i++) {
        delete paths.itemAt(i);
    }

    for (size_t i = 0; i < matrices.size(); i++) {
        delete matrices.itemAt(i);
    }

    bitmapResources.clear();
    ownedBitmapResources.clear();
    patchResources.clear();
    shaders.clear();
    sourcePaths.clear();
    paints.clear();
    regions.clear();
    paths.clear();
    matrices.clear();
    layers.clear();
}

}; // namespace uirenderer
}; // namespace android
