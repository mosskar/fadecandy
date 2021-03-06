/*
 * Effect base class for particle systems
 *
 * Copyright (c) 2014 Micah Elizabeth Scott <micah@scanlime.org>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "effect.h"
#include "nanoflann.h"  // Tiny KD-tree library


class ParticleEffect : public Effect {
public:
    /*
     * Information for drawing particles. If your effect needs to keep additional
     * data about particles, use a parallel array or other separate data structure.
     */
    struct ParticleAppearance {
        Vec3 point;
        Vec3 color;
        float radius;
        float intensity;
    };

    ParticleEffect();

    virtual void beginFrame(const FrameInfo& f);
    virtual void calculatePixel(Vec3& rgb, const PixelInfo& p);
    virtual void debug(const DebugInfo& d);

protected:
    /*
     * List of appearances for particles we're drawing. Calculate this in beginFrame(),
     * or keep it persistent across frames and update the parts you're changing.
     */
    typedef std::vector<ParticleAppearance> AppearanceVector;
    AppearanceVector appearance;

    /*
     * KD-tree as a spatial index for finding particles quickly by location.
     * This index is rebuilt each frame during ParticleEffect::buildFrame().
     * The ParticleEffect itself uses this index for calculating pixel values,
     * but subclasses may also want to use it for phyiscs or interaction.
     */

    typedef nanoflann::KDTreeSingleIndexAdaptor<
        nanoflann::L2_Simple_Adaptor< Real, ParticleEffect >,
        ParticleEffect, 3> IndexTree;

    struct Index {
        Index(ParticleEffect &e);

        Vec3 aabbMin;
        Vec3 aabbMax;
        float radiusMax;
        IndexTree tree;
    } index;

    /*
     * Kernel function; determines particle shape
     * Poly6 kernel, Müller, Charypar, & Gross (2003)
     * q normalized in range [0, 1].
     * Has compact support; kernel forced to zero outside this range.
     */
    float kernel(float q);

    // Variant of kernel function called with q^2
    float kernel2(float q2);

    // First derivative of kernel()
    float kernelDerivative(float q);

private:
    // Debug statistics
    unsigned particlesPerPixelNumerator;
    unsigned particlesPerPixelDenominator; 

public:
    // Implementation glue for our KD-tree index

    inline size_t kdtree_get_point_count() const
    {
        return appearance.size();
    }

    inline Real kdtree_distance(const Real *p1, const size_t idx_p2, size_t size) const
    {
        const ParticleAppearance &a = appearance[idx_p2];
        Real d0 = p1[0] - a.point[0];
        Real d1 = p1[1] - a.point[1];
        Real d2 = p1[2] - a.point[2];
        return sq(d0) + sq(d1) + sq(d2);
    }

    Real kdtree_get_pt(const size_t idx, int dim) const
    {
        return appearance[idx].point[dim];
    }

    template <class BBOX>
    bool kdtree_get_bbox(BBOX &bb) const
    {
        bb[0].low  = index.aabbMin[0];
        bb[1].low  = index.aabbMin[1];
        bb[2].low  = index.aabbMin[2];
        bb[0].high = index.aabbMax[0];
        bb[1].high = index.aabbMax[1];
        bb[2].high = index.aabbMax[2];
        return true;
    }
};


/*****************************************************************************************
 *                                   Implementation
 *****************************************************************************************/


ParticleEffect::ParticleEffect()
    : index(*this)
{}

ParticleEffect::Index::Index(ParticleEffect &e)
    : aabbMin(0, 0, 0),
      aabbMax(0, 0, 0),
      radiusMax(0),
      tree(3, e)
{}

inline float ParticleEffect::kernel(float q)
{
    float a = 1 - q * q;
    return a * a * a;
}

inline float ParticleEffect::kernel2(float q2)
{
    float a = 1 - q2;
    return a * a * a;
}

inline float ParticleEffect::kernelDerivative(float q)
{
    float a = 1 - q * q;
    return -6.0f * q * a * a;
}

inline void ParticleEffect::beginFrame(const FrameInfo& f)
{
    // Measure bounding box and largest radius in 'particles'
    index.aabbMin = appearance[0].point;
    index.aabbMax = appearance[0].point;
    index.radiusMax = appearance[0].radius;
    for (unsigned i = 1; i < appearance.size(); ++i) {
        const ParticleAppearance& particle = appearance[i];
        
        index.aabbMin[0] = std::min(index.aabbMin[0], particle.point[0]);
        index.aabbMin[1] = std::min(index.aabbMin[1], particle.point[1]);
        index.aabbMin[2] = std::min(index.aabbMin[2], particle.point[2]);
        
        index.aabbMax[0] = std::max(index.aabbMax[0], particle.point[0]);
        index.aabbMax[1] = std::max(index.aabbMax[1], particle.point[1]);
        index.aabbMax[2] = std::max(index.aabbMax[2], particle.point[2]);
        
        index.radiusMax = std::max(index.radiusMax, particle.radius);
    }

    // Rebuild KD-tree
    index.tree.buildIndex();

    // Reset debug counters
    particlesPerPixelNumerator = 0;
    particlesPerPixelDenominator = 0;
}

inline void ParticleEffect::calculatePixel(Vec3& rgb, const PixelInfo& p)
{
    Vec3 accumulator(0, 0, 0);

    std::vector<std::pair<size_t, Real> > hits;
    nanoflann::SearchParams params;
    params.sorted = false;

    unsigned numHits = index.tree.radiusSearch(&p.point[0], sq(index.radiusMax), hits, params);

    for (unsigned i = 0; i < numHits; i++) {
        ParticleAppearance &particle = appearance[hits[i].first];
        float dist2 = hits[i].second;

        // Normalized distance
        float q2 = dist2 / sq(particle.radius);
        if (q2 < 1.0f) {
            accumulator += particle.color * (particle.intensity * kernel2(q2));
        }
    }

    rgb = accumulator;

    particlesPerPixelNumerator += numHits;
    particlesPerPixelDenominator++;
}

inline void ParticleEffect::debug(const DebugInfo& d)
{
    fprintf(stderr, "\t[particle] %.1f kB, radius=%.1f, %.2f hits/pixel\n",
        index.tree.usedMemory() / 1024.0f,
        index.radiusMax,
        float(particlesPerPixelNumerator) / float(particlesPerPixelDenominator));
}
