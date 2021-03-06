
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

// TODO (maybe): have integrators pre-prime the cache by rendering a very
// low res image first?

#include "lightdistrib.h"
#include "lowdiscrepancy.h"
#include "parallel.h"
#include "scene.h"
#include "stats.h"
#include "integrator.h"
#include "paramset.h"
#include <numeric>
#include <math.h>
#include <fenv.h>



using namespace nanoflann;

namespace pbrt {

LightDistribution::~LightDistribution() {}

std::unique_ptr<LightDistribution> CreateLightSampleDistribution(
	const ParamSet &params, const Scene &scene) {
	std::string name = params.FindOneString("lightsamplestrategy", "spatial");
	pbrt::PbrtOptions.filenameInfo.lightSampleStrategy = name;

    if (name == "uniform" || scene.lights.size() == 1)
        return std::unique_ptr<LightDistribution>{
            new UniformLightDistribution(scene)};
    else if (name == "power")
        return std::unique_ptr<LightDistribution>{
            new PowerLightDistribution(scene)};
    else if (name == "spatial")
        return std::unique_ptr<LightDistribution>{
            new SpatialLightDistribution(scene)};
	else if (name == "photonvoxel")
		return std::unique_ptr<LightDistribution>{
			new PhotonBasedVoxelLightDistribution(params, scene)};
	else if (name == "photontree")
		return std::unique_ptr<LightDistribution>{
			new PhotonBasedKdTreeLightDistribution(params, scene)};
	else if (name == "mlcdftree")
		return std::unique_ptr<LightDistribution>{
		new PhotonBasedMlCdfKdTreeLightDistribution(params, scene)};
	else if (name == "cdftree")
		return std::unique_ptr<LightDistribution>{
		new PhotonBasedCdfKdTreeLightDistribution(params, scene)};
    else {
        Error(
            "Light sample distribution type \"%s\" unknown. Using \"spatial\".",
            name.c_str());
        return std::unique_ptr<LightDistribution>{
            new SpatialLightDistribution(scene)};
    }
}

UniformLightDistribution::UniformLightDistribution(const Scene &scene) {
    std::vector<Float> prob(scene.lights.size(), Float(1));
    distrib.reset(new Distribution1D(&prob[0], int(prob.size())));
}

const Distribution1D *UniformLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
    return distrib.get();
}

PowerLightDistribution::PowerLightDistribution(const Scene &scene)
    : distrib(ComputeLightPowerDistribution(scene)) {}

const Distribution1D *PowerLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
    return distrib.get();
}

///////////////////////////////////////////////////////////////////////////
// SpatialLightDistribution

STAT_COUNTER("SpatialLightDistribution/Distributions created", nCreated);
STAT_RATIO("SpatialLightDistribution/Lookups per distribution", nLookups, nDistributions);
STAT_INT_DISTRIBUTION("SpatialLightDistribution/Hash probes per lookup", nProbesPerLookup);

// Voxel coordinates are packed into a uint64_t for hash table lookups;
// 10 bits are allocated to each coordinate.  invalidPackedPos is an impossible
// packed coordinate value, which we use to represent
static const uint64_t invalidPackedPos = 0xffffffffffffffff;

SpatialLightDistribution::SpatialLightDistribution(const Scene &scene,
                                                   int maxVoxels)
    : scene(scene) {
    // Compute the number of voxels so that the widest scene bounding box
    // dimension has maxVoxels voxels and the other dimensions have a number
    // of voxels so that voxels are roughly cube shaped.
    Bounds3f b = scene.WorldBound();
    Vector3f diag = b.Diagonal();
    Float bmax = diag[b.MaximumExtent()];
    for (int i = 0; i < 3; ++i) {
        nVoxels[i] = std::max(1, int(std::round(diag[i] / bmax * maxVoxels)));
        // In the Lookup() method, we require that 20 or fewer bits be
        // sufficient to represent each coordinate value. It's fairly hard
        // to imagine that this would ever be a problem.
        CHECK_LT(nVoxels[i], 1 << 20);
    }

    hashTableSize = 4 * nVoxels[0] * nVoxels[1] * nVoxels[2];
    hashTable.reset(new HashEntry[hashTableSize]);
    for (int i = 0; i < hashTableSize; ++i) {
        hashTable[i].packedPos.store(invalidPackedPos);
        hashTable[i].distribution.store(nullptr);
    }

    LOG(INFO) << "SpatialLightDistribution: scene bounds " << b <<
        ", voxel res (" << nVoxels[0] << ", " << nVoxels[1] << ", " <<
        nVoxels[2] << ")";
}

SpatialLightDistribution::~SpatialLightDistribution() {
    // Gather statistics about how well the computed distributions are across
    // the buckets.
    for (size_t i = 0; i < hashTableSize; ++i) {
        HashEntry &entry = hashTable[i];
        if (entry.distribution.load())
            delete entry.distribution.load();
    }
}

const Distribution1D *SpatialLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
    ProfilePhase _(Prof::LightDistribLookup);
    ++nLookups;

    // First, compute integer voxel coordinates for the given point |p|
    // with respect to the overall voxel grid.
    Vector3f offset = scene.WorldBound().Offset(p);  // offset in [0,1].
    Point3i pi;
    for (int i = 0; i < 3; ++i)
        // The clamp should almost never be necessary, but is there to be
        // robust to computed intersection points being slightly outside
        // the scene bounds due to floating-point roundoff error.
        pi[i] = Clamp(int(offset[i] * nVoxels[i]), 0, nVoxels[i] - 1);

    // Pack the 3D integer voxel coordinates into a single 64-bit value.
    uint64_t packedPos = (uint64_t(pi[0]) << 40) | (uint64_t(pi[1]) << 20) | pi[2];
    CHECK_NE(packedPos, invalidPackedPos);

    // Compute a hash value from the packed voxel coordinates.  We could
    // just take packedPos mod the hash table size, but since packedPos
    // isn't necessarily well distributed on its own, it's worthwhile to do
    // a little work to make sure that its bits values are individually
    // fairly random. For details of and motivation for the following, see:
    // http://zimbry.blogspot.ch/2011/09/better-bit-mixing-improving-on.html
    uint64_t hash = packedPos;
    hash ^= (hash >> 31);
    hash *= 0x7fb5d329728ea185;
    hash ^= (hash >> 27);
    hash *= 0x81dadef4bc2dd44d;
    hash ^= (hash >> 33);
    hash %= hashTableSize;
    CHECK_GE(hash, 0);

    // Now, see if the hash table already has an entry for the voxel. We'll
    // use quadratic probing when the hash table entry is already used for
    // another value; step stores the square root of the probe step.
    int step = 1;
    int nProbes = 0;
    while (true) {
        ++nProbes;
        HashEntry &entry = hashTable[hash];
        // Does the hash table entry at offset |hash| match the current point?
        uint64_t entryPackedPos = entry.packedPos.load(std::memory_order_acquire);
        if (entryPackedPos == packedPos) {
            // Yes! Most of the time, there should already by a light
            // sampling distribution available.
            Distribution1D *dist = entry.distribution.load(std::memory_order_acquire);
            if (dist == nullptr) {
                // Rarely, another thread will have already done a lookup
                // at this point, found that there isn't a sampling
                // distribution, and will already be computing the
                // distribution for the point.  In this case, we spin until
                // the sampling distribution is ready.  We assume that this
                // is a rare case, so don't do anything more sophisticated
                // than spinning.
                ProfilePhase _(Prof::LightDistribSpinWait);
                while ((dist = entry.distribution.load(std::memory_order_acquire)) ==
                       nullptr)
                    // spin :-(. If we were fancy, we'd have any threads
                    // that hit this instead help out with computing the
                    // distribution for the voxel...
                    ;
            }
            // We have a valid sampling distribution.
            ReportValue(nProbesPerLookup, nProbes);
            return dist;
        } else if (entryPackedPos != invalidPackedPos) {
            // The hash table entry we're checking has already been
            // allocated for another voxel. Advance to the next entry with
            // quadratic probing.
            hash += step * step;
            if (hash >= hashTableSize)
                hash %= hashTableSize;
            ++step;
        } else {
            // We have found an invalid entry. (Though this may have
            // changed since the load into entryPackedPos above.)  Use an
            // atomic compare/exchange to try to claim this entry for the
            // current position.
            uint64_t invalid = invalidPackedPos;
            if (entry.packedPos.compare_exchange_weak(invalid, packedPos)) {
                // Success; we've claimed this position for this voxel's
                // distribution. Now compute the sampling distribution and
                // add it to the hash table. As long as packedPos has been
                // set but the entry's distribution pointer is nullptr, any
                // other threads looking up the distribution for this voxel
                // will spin wait until the distribution pointer is
                // written.
                Distribution1D *dist = ComputeDistribution(pi);
                entry.distribution.store(dist, std::memory_order_release);
                ReportValue(nProbesPerLookup, nProbes);
                return dist;
            }
        }
    }
}

Distribution1D *
SpatialLightDistribution::ComputeDistribution(Point3i pi) const {
    ProfilePhase _(Prof::LightDistribCreation);
    ++nCreated;
    ++nDistributions;

    // Compute the world-space bounding box of the voxel corresponding to
    // |pi|.
    Point3f p0(Float(pi[0]) / Float(nVoxels[0]),
               Float(pi[1]) / Float(nVoxels[1]),
               Float(pi[2]) / Float(nVoxels[2]));
    Point3f p1(Float(pi[0] + 1) / Float(nVoxels[0]),
               Float(pi[1] + 1) / Float(nVoxels[1]),
               Float(pi[2] + 1) / Float(nVoxels[2]));
    Bounds3f voxelBounds(scene.WorldBound().Lerp(p0),
                         scene.WorldBound().Lerp(p1));

    // Compute the sampling distribution. Sample a number of points inside
    // voxelBounds using a 3D Halton sequence; at each one, sample each
    // light source and compute a weight based on Li/pdf for the light's
    // sample (ignoring visibility between the point in the voxel and the
    // point on the light source) as an approximation to how much the light
    // is likely to contribute to illumination in the voxel.
    int nSamples = 128;
    std::vector<Float> lightContrib(scene.lights.size(), Float(0));
    for (int i = 0; i < nSamples; ++i) {
        Point3f po = voxelBounds.Lerp(Point3f(
            RadicalInverse(0, i), RadicalInverse(1, i), RadicalInverse(2, i)));
        Interaction intr(po, Normal3f(), Vector3f(), Vector3f(1, 0, 0),
                         0 /* time */, MediumInterface());

        // Use the next two Halton dimensions to sample a point on the
        // light source.
        Point2f u(RadicalInverse(3, i), RadicalInverse(4, i));
        for (size_t j = 0; j < scene.lights.size(); ++j) {
            Float pdf;
            Vector3f wi;
            VisibilityTester vis;
            Spectrum Li = scene.lights[j]->Sample_Li(intr, u, &wi, &pdf, &vis);
            if (pdf > 0) {
                // TODO: look at tracing shadow rays / computing beam
                // transmittance.  Probably shouldn't give those full weight
                // but instead e.g. have an occluded shadow ray scale down
                // the contribution by 10 or something.
                lightContrib[j] += Li.y() / pdf;
            }
        }
    }

    // We don't want to leave any lights with a zero probability; it's
    // possible that a light contributes to points in the voxel even though
    // we didn't find such a point when sampling above.  Therefore, compute
    // a minimum (small) weight and ensure that all lights are given at
    // least the corresponding probability.
    Float sumContrib =
        std::accumulate(lightContrib.begin(), lightContrib.end(), Float(0));
    Float avgContrib = sumContrib / (nSamples * lightContrib.size());
    Float minContrib = (avgContrib > 0) ? .001 * avgContrib : 1;
    for (size_t i = 0; i < lightContrib.size(); ++i) {
        VLOG(2) << "Voxel pi = " << pi << ", light " << i << " contrib = "
                << lightContrib[i];
        lightContrib[i] = std::max(lightContrib[i], minContrib);
    }
    LOG(INFO) << "Initialized light distribution in voxel pi= " <<  pi <<
        ", avgContrib = " << avgContrib;

    // Compute a sampling distribution from the accumulated contributions.
    return new Distribution1D(&lightContrib[0], int(lightContrib.size()));
}


void PhotonBasedVoxelLightDistribution::calcPackedPosAndHash(uint64_t* packedPos, uint64_t* hash, Point3i* pi) const {

	// Pack the 3D integer voxel coordinates into a single 64-bit value.
	*packedPos = (uint64_t((*pi)[0]) << 40) | (uint64_t((*pi)[1]) << 20) | (*pi)[2];
	CHECK_NE(*packedPos, invalidPackedPos);

	// Compute a hash value from the packed voxel coordinates.  We could
	// just take packedPos mod the hash table size, but since packedPos
	// isn't necessarily well distributed on its own, it's worthwhile to do
	// a little work to make sure that its bits values are individually
	// fairly random. For details of and motivation for the following, see:
	// http://zimbry.blogspot.ch/2011/09/better-bit-mixing-improving-on.html
	*hash = *packedPos;
	*hash ^= ((*hash) >> 31);
	*hash *= 0x7fb5d329728ea185;
	*hash ^= ((*hash) >> 27);
	*hash *= 0x81dadef4bc2dd44d;
	*hash ^= ((*hash) >> 33);
	*hash %= hashTableSize;
	CHECK_GE(*hash, 0);
}

const Distribution1D *PhotonBasedVoxelLightDistribution::getDistribution(uint64_t packedPos, uint64_t hash, int* nProbes) const {
	// Now, see if the hash table already has an entry for the voxel. We'll
	// use quadratic probing when the hash table entry is already used for
	// another value; step stores the square root of the probe step.
	
	int step = 1;
	while (true) {
		++(*nProbes);
		HashEntry &entry = hashTable[hash];
		// Does the hash table entry at offset |hash| match the current point?
		uint64_t entryPackedPos = entry.packedPos.load(std::memory_order_acquire);
		if (entryPackedPos == packedPos) {
			// We have a valid sampling distribution.
			
			//LOG_FIRST_N(INFO, 1000) << "PhotonBasedVoxelLightDistribution: Using photondistribution: " << entry.distribution->ToString();
			return entry.distribution;
		}
		else if (entryPackedPos == invalidPackedPos) {
			// no photon arrived on this hash, use defaultdistribution instead
			//LOG(INFO) << "PhotonBasedVoxelLightDistribution: Using powerdistribution, no photons arrived: " << packedPos;
			return defaultDistrib.get();
		}
		else {
			// The hash table entry we're checking has already been
			// allocated for another voxel. Advance to the next entry with
			// quadratic probing.
			hash += step * step;
			if (hash >= hashTableSize)
				hash %= hashTableSize;
			++step;
		}
	}
}

const Distribution1D *PhotonBasedVoxelLightDistribution::
		getInterpolatedDistribution(const Point3f &p, uint64_t packedPos, uint64_t hash, Point3i* voxelId, int* nProbes) const {
	Vector3f offset = scene.WorldBound().Offset(p);  // offset in [0,1].
	std::vector<const Distribution1D*> distributions;
	std::vector<Point3i> voxelIds;
	std::vector<Float> influence;
	distributions.push_back(getDistribution(packedPos, hash, nProbes));
	voxelIds.push_back(*voxelId);
	influence.push_back(1.0f);
	
	for (int i = 0; i < 3; ++i) {
		Float offsetInVoxel = (fmod(offset[i] / (1.0f / nVoxels[i]), 1.0f)) - 0.5f;
		if (offsetInVoxel == 0.f) continue; // skip this direction as there is no influence
		int size = voxelIds.size();
		for (int n = 0; n < size; ++n) {
			Point3i newId = Point3i(voxelIds[n]);
			// go a voxel back or forth
			newId[i] += offsetInVoxel > 0? 1 : -1;

			// if we are on a boundary we won't interpolate into this direction, skip then
			if (newId[i] >= 0 && newId[i] < nVoxels[i]) {
				uint64_t newPackedPos, newHash;
				calcPackedPosAndHash(&newPackedPos, &newHash, &newId);
				distributions.push_back(getDistribution(newPackedPos, newHash, nProbes));
				voxelIds.push_back(newId);
				influence.push_back(influence[n] * abs(offsetInVoxel));

				influence[n] *= (1 - abs(offsetInVoxel));
			}

		}
	}
	InterpolatedDistribution1D* iDistr = new InterpolatedDistribution1D(&influence[0], &distributions[0], influence.size());
	iDistr->deleteAfterUsage = true;
	return iDistr;
}

PhotonBasedVoxelLightDistribution::PhotonBasedVoxelLightDistribution(const ParamSet &params, const Scene &scene) : 
	scene(scene), 
	photonCount(params.FindOneInt("photonCount", 100000)), 
	maxVoxels(params.FindOneInt("maxVoxels", 64)),
	minContributionScale(params.FindOneFloat("minContributionScale", 0.001)),
	interpolateCdf(params.FindOneBool("interpolateCdf", true)) {
	ProfilePhase _(Prof::LightDistribCreation);

	pbrt::PbrtOptions.filenameInfo.photonCount = &photonCount;
	pbrt::PbrtOptions.filenameInfo.interpolateCdf = &interpolateCdf;
	pbrt::PbrtOptions.filenameInfo.minContributionScale = &minContributionScale;
	pbrt::PbrtOptions.filenameInfo.maxVoxels = &maxVoxels;

	std::vector<Float> prob(scene.lights.size(), Float(1));
	defaultDistrib.reset(new Distribution1D(&prob[0], int(prob.size())));
	if (params.FindOneString("photonsampling", "uni") == "uni") {
		photonDistrib.reset(new Distribution1D(&prob[0], int(prob.size())));
	} else {
		photonDistrib = ComputeLightPowerDistribution(scene);
	}

	initVoxelHashTable();
	shootPhotons(scene);
}

const Distribution1D *PhotonBasedVoxelLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
	ProfilePhase _(Prof::LightDistribLookup);
	++nLookups;

	// First, compute integer voxel coordinates for the given point |p|
	// with respect to the overall voxel grid.
	Vector3f offset = scene.WorldBound().Offset(p);  // offset in [0,1].

	Point3i voxelId;
	for (int i = 0; i < 3; ++i)
		// The clamp should almost never be necessary, but is there to be
		// robust to computed intersection points being slightly outside
		// the scene bounds due to floating-point roundoff error.
		voxelId[i] = Clamp(int(offset[i] * nVoxels[i]), 0, nVoxels[i] - 1);

	uint64_t packedPos, hash;
	calcPackedPosAndHash(&packedPos, &hash, &voxelId);

	int nProbes = 0;
	const Distribution1D *distr;

	if (interpolateCdf) {
		distr = getInterpolatedDistribution(p, packedPos, hash, &voxelId, &nProbes);
	} else {
		distr = getDistribution(packedPos, hash, &nProbes);	
	}

	//ReportValue(nProbesPerLookup, nProbes);
	return distr;
}

void PhotonBasedVoxelLightDistribution::initVoxelHashTable() {
	// Compute the number of voxels so that the widest scene bounding box
	// dimension has maxVoxels voxels and the other dimensions have a number
	// of voxels so that voxels are roughly cube shaped.
	Bounds3f b = scene.WorldBound();
	Vector3f diag = b.Diagonal();
	Float bmax = diag[b.MaximumExtent()];
	for (int i = 0; i < 3; ++i) {
		nVoxels[i] = std::max(1, int(std::round(diag[i] / bmax * maxVoxels)));
		// In the Lookup() method, we require that 20 or fewer bits be
		// sufficient to represent each coordinate value. It's fairly hard
		// to imagine that this would ever be a problem.
		CHECK_LT(nVoxels[i], 1 << 20);
	}

	hashTableSize = 4 * nVoxels[0] * nVoxels[1] * nVoxels[2];
	hashTable.reset(new HashEntry[hashTableSize]);
	ParallelFor([&](int i) {
		hashTable[i].packedPos.store(invalidPackedPos);
		hashTable[i].lightContrib.reset(new std::unordered_map<int, Float>());
	}, hashTableSize, 4096);

	LOG(INFO) << "PhotonBasedVoxelLightDistribution: scene bounds " << b <<
		", voxel res (" << nVoxels[0] << ", " << nVoxels[1] << ", " <<
		nVoxels[2] << ")";
}

void PhotonBasedVoxelLightDistribution::shootPhotons(const Scene &scene) {
	std::mutex m_screen;
	ParallelFor([&](int photonIndex) {
		// Follow photon path for _photonIndex_
		uint64_t haltonIndex = photonIndex;
		int haltonDim = 0;

		// Choose light to shoot photon from
		Float lightPdf;
		Float lightSample = RadicalInverse(haltonDim++, haltonIndex);
		int lightNum = photonDistrib->SampleDiscrete(lightSample, &lightPdf);
		const std::shared_ptr<Light> &light = scene.lights[lightNum];

		// Compute sample values for photon ray leaving light source
		Point2f uLight0(RadicalInverse(haltonDim, haltonIndex),
			RadicalInverse(haltonDim + 1, haltonIndex));
		Point2f uLight1(RadicalInverse(haltonDim + 2, haltonIndex),
			RadicalInverse(haltonDim + 3, haltonIndex));
		// Camera not available here, add Camera to the Scene object?
		Float uLightTime = 0; //Lerp(RadicalInverse(haltonDim + 4, haltonIndex), camera->shutterOpen, camera->shutterClose);
		haltonDim += 5;

		// Generate _photonRay_ from light source and initialize _beta_
		RayDifferential photonRay;
		Normal3f nLight;
		Float pdfPos, pdfDir;
		Spectrum Le =
			light->Sample_Le(uLight0, uLight1, uLightTime, &photonRay,
				&nLight, &pdfPos, &pdfDir);
		if (pdfPos == 0 || pdfDir == 0 || Le.IsBlack()) return;
		Spectrum beta = (AbsDot(nLight, photonRay.d) * Le) /
			(lightPdf * pdfPos * pdfDir);
		if (beta.IsBlack()) return;
        Float fbeta = beta.sumValues();

		// Follow photon through scene and record intersection
		SurfaceInteraction isect;
		if (scene.Intersect(photonRay, &isect)) {
			// First, compute integer voxel coordinates for the given point |p|
			// with respect to the overall voxel grid.
			Vector3f offset = scene.WorldBound().Offset(isect.p);  // offset in [0,1].
			Point3i pi;
			for (int i = 0; i < 3; ++i)
				// The clamp should almost never be necessary, but is there to be
				// robust to computed intersection points being slightly outside
				// the scene bounds due to floating-point roundoff error.
				pi[i] = Clamp(int(offset[i] * nVoxels[i]), 0, nVoxels[i] - 1);

			// Pack the 3D integer voxel coordinates into a single 64-bit value.
			uint64_t packedPos = (uint64_t(pi[0]) << 40) | (uint64_t(pi[1]) << 20) | pi[2];
			CHECK_NE(packedPos, invalidPackedPos);

			// Compute a hash value from the packed voxel coordinates.  We could
			// just take packedPos mod the hash table size, but since packedPos
			// isn't necessarily well distributed on its own, it's worthwhile to do
			// a little work to make sure that its bits values are individually
			// fairly random. For details of and motivation for the following, see:
			// http://zimbry.blogspot.ch/2011/09/better-bit-mixing-improving-on.html
			uint64_t hash = packedPos;
			hash ^= (hash >> 31);
			hash *= 0x7fb5d329728ea185;
			hash ^= (hash >> 27);
			hash *= 0x81dadef4bc2dd44d;
			hash ^= (hash >> 33);
			hash %= hashTableSize;
			CHECK_GE(hash, 0);

			// Now, see if the hash table already has an entry for the voxel. We'll
			// use quadratic probing when the hash table entry is already used for
			// another value; step stores the square root of the probe step.
			int step = 1;
			int nProbes = 0;
			while (true) {
				++nProbes;
				HashEntry &entry = hashTable[hash];
				// Does the hash table entry at offset |hash| match the current point?
				uint64_t entryPackedPos = entry.packedPos.load(std::memory_order_acquire);
				uint64_t invalid = invalidPackedPos;
				if (entryPackedPos == packedPos || entry.packedPos.compare_exchange_weak(invalid, packedPos)) {
					// Hash entry is already associated to the packedPos OR hashentrypos is invalid
					// and we try to claim this hashentry for this packedPos
					std::unordered_map<int, Float>* lightContrib = entry.lightContrib.get();
					//ReportValue(nProbesPerLookup, nProbes);
					m_screen.lock();
					(*lightContrib)[lightNum] += fbeta;
					m_screen.unlock();
					break;
				} else {
					// The hash table entry we're checking has already been
					// allocated for another voxel. Advance to the next entry with
					// quadratic probing.
					hash += step * step;
					if (hash >= hashTableSize)
						hash %= hashTableSize;
					++step;
				}
			}

		}
	}, photonCount, 4096);

	ParallelFor([&](int i) {
		std::unordered_map<int, Float> *lightContrib = hashTable[i].lightContrib.get();
		hashTable[i].distribution = SparseDistribution1D::createSparseDistribution1D(*lightContrib, minContributionScale, scene.lights.size(), false);
	}, hashTableSize, 4096);
}

PhotonBasedKdTreeLightDistribution::PhotonBasedKdTreeLightDistribution(const ParamSet &params, const Scene &scene) :
		scene(scene), 
		kdtree(3 /*dim*/, cloud, KDTreeSingleIndexAdaptorParams(10 /* max leaf */)),
		photonCount(params.FindOneInt("photonCount", 100000)),
		minContributionScale(params.FindOneFloat("minContributionScale", 0.001)),
		nearestNeighbours(params.FindOneInt("nearestNeighbours", 50)),
		photonRadius(params.FindOneFloat("photonRadius", 0.1)),
		interpolation(params.FindOneString("interpolation", "shepard")),
		intSmooth(params.FindOneFloat("intSmooth", 1.0f)),
		knn(params.FindOneBool("knn", true))
{
	ProfilePhase _(Prof::LightDistribCreation);
	pbrt::PbrtOptions.filenameInfo.photonCount = &photonCount;
	pbrt::PbrtOptions.filenameInfo.minContributionScale = &minContributionScale;
	pbrt::PbrtOptions.filenameInfo.knn = &knn;
	pbrt::PbrtOptions.filenameInfo.nearestNeighbours = &nearestNeighbours;
	pbrt::PbrtOptions.filenameInfo.photonRadius = &photonRadius;
	pbrt::PbrtOptions.filenameInfo.interpolation = &interpolation;
	pbrt::PbrtOptions.filenameInfo.intSmooth = &intSmooth;
	
	if (params.FindOneString("photonsampling", "uni") == "uni") {
		std::vector<Float> prob(scene.lights.size(), Float(1));
		photonDistrib.reset(new Distribution1D(&prob[0], int(prob.size())));
	} else {
		photonDistrib = ComputeLightPowerDistribution(scene);
	}

	cloud.pts.resize(photonCount);
	shootPhotons(scene);
	kdtree.buildIndex();
}

void PhotonBasedKdTreeLightDistribution::shootPhotons(const Scene &scene) {
	//std::mutex m_screen;
	ParallelFor([&](int photonIndex) {
		// Follow photon path for _photonIndex_
		uint64_t haltonIndex = photonIndex;
		int haltonDim = 0;

		// Choose light to shoot photon from
		Float lightPdf;
		Float lightSample = RadicalInverse(haltonDim++, haltonIndex);
		int lightNum = photonDistrib->SampleDiscrete(lightSample, &lightPdf);
		const std::shared_ptr<Light> &light = scene.lights[lightNum];

		// Compute sample values for photon ray leaving light source
		Point2f uLight0(RadicalInverse(haltonDim, haltonIndex),
			RadicalInverse(haltonDim + 1, haltonIndex));
		Point2f uLight1(RadicalInverse(haltonDim + 2, haltonIndex),
			RadicalInverse(haltonDim + 3, haltonIndex));
		// Camera not available here, add Camera to the Scene object?
		Float uLightTime = 0; //Lerp(RadicalInverse(haltonDim + 4, haltonIndex), camera->shutterOpen, camera->shutterClose);
		haltonDim += 5;

		// Generate _photonRay_ from light source and initialize _beta_
		RayDifferential photonRay;
		Normal3f nLight;
		Float pdfPos, pdfDir;
		Spectrum Le =
			light->Sample_Le(uLight0, uLight1, uLightTime, &photonRay,
				&nLight, &pdfPos, &pdfDir);
		if (pdfPos == 0 || pdfDir == 0 || Le.IsBlack()) return;
		Spectrum beta = (AbsDot(nLight, photonRay.d) * Le) /
			(lightPdf * pdfPos * pdfDir);
		if (beta.IsBlack()) return;
		Float fbeta = beta.sumValues();

		// Follow photon through scene and record intersection
		SurfaceInteraction isect;
		if (scene.Intersect(photonRay, &isect)) {
			// Add photon to kd-tree if intersection found and is difuse
			cloud.pts[photonIndex].x = isect.p.x;
			cloud.pts[photonIndex].y = isect.p.y;
			cloud.pts[photonIndex].z = isect.p.z;
			cloud.pts[photonIndex].beta = fbeta;
			cloud.pts[photonIndex].lightNum = lightNum;
			cloud.pts[photonIndex].fromDir = -Normalize(photonRay.d);
			//m_screen.lock();
			//pbrt::objFile << "v " << isect.p.x << " " << isect.p.y << " " << isect.p.z << "\n";
			//pbrt::objFile << "v " << photonRay.o.x << " " << photonRay.o.y << " " << photonRay.o.z << "\nl -1 -2 \n";
			//m_screen.unlock();
		} else {
			cloud.pts[photonIndex].x = FLT_MAX;
			cloud.pts[photonIndex].y = FLT_MAX;
			cloud.pts[photonIndex].z = FLT_MAX;
			cloud.pts[photonIndex].beta = 0.0;
			cloud.pts[photonIndex].lightNum = -1;
		}
	}, photonCount, 4096);
}

const Distribution1D *PhotonBasedKdTreeLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
	ProfilePhase _(Prof::LightDistribLookup);
	++nLookups;

	const Float query_pt[3] = { p.x, p.y, p.z };
	std::unordered_map<int, Float> lightContrib;
	if (knn) {

		// perform a k-nearest-neighbour search to find #nearestNeighbours
		size_t num_results = nearestNeighbours;
		std::vector<size_t> ret_index(num_results);
		std::vector<Float> out_dist_sqr(num_results);

		num_results = kdtree.knnSearch(&query_pt[0], num_results, &ret_index[0], &out_dist_sqr[0]);
		ret_index.resize(num_results);
		out_dist_sqr.resize(num_results);

		if (interpolation == "shepard") {
			for (size_t i = 0; i < num_results; i++) {
				//if (Dot(cloud.pts[ret_index[i]].fromDir, Normalize(n)) >= 0) {
				int lightNum = cloud.pts[ret_index[i]].lightNum;
				float beta = cloud.pts[ret_index[i]].beta;
				Float d = std::max(0.001f, pow(out_dist_sqr[i], intSmooth));
				beta = beta / d;
				lightContrib[lightNum] += beta;
				//}
			}
		}
		else if (interpolation == "modshep") {
			Float maxR = 0;
			for (size_t i = 0; i < num_results; i++) {
					maxR = std::max(maxR, out_dist_sqr[i]);
					
			}
			maxR = pow(maxR, intSmooth);
			for (size_t i = 0; i < num_results; i++) {

				int lightNum = cloud.pts[ret_index[i]].lightNum;
				float beta = cloud.pts[ret_index[i]].beta;
				Float d = std::max(0.001f, pow(out_dist_sqr[i], intSmooth));
				beta = pow((maxR - d) / (maxR * d), 2);
				lightContrib[lightNum] += beta;

			}
		}
		else if (interpolation == "kreg") {
			for (size_t i = 0; i < num_results; i++) {

				int lightNum = cloud.pts[ret_index[i]].lightNum;
				float beta = cloud.pts[ret_index[i]].beta;
				Float d = sqrt(out_dist_sqr[i]);
				lightContrib[lightNum] += exp(-pow(d / intSmooth, 2));
					
			}
		}
		else if (interpolation == "adkreg") {
			Float maxR = 0;
			for (size_t i = 0; i < num_results; i++) {
					maxR = std::max(maxR, out_dist_sqr[i]);
					
			}
			maxR = sqrt(maxR);
			Float p = maxR / sqrt(-log(intSmooth));
			for (size_t i = 0; i < num_results; i++) {

				int lightNum = cloud.pts[ret_index[i]].lightNum;
				float beta = cloud.pts[ret_index[i]].beta;
				Float d = sqrt(out_dist_sqr[i]);
				lightContrib[lightNum] += exp(-pow(d / p, 2)) - intSmooth;
					
			}
		}
		else if (interpolation == "none") {
			for (size_t i = 0; i < num_results; i++) {

				int lightNum = cloud.pts[ret_index[i]].lightNum;
				float beta = cloud.pts[ret_index[i]].beta;
				lightContrib[lightNum] += beta;
					
			}
		}
		
	} else {
		// perform a search within searchradius photonRadius
		std::vector<std::pair<size_t, Float>> ret_matches;
		nanoflann::SearchParams params;

		const size_t nMatches = kdtree.radiusSearch(&query_pt[0], photonRadius, ret_matches, params);
		for (size_t i = 0; i < nMatches; i++) {

			int lightNum = cloud.pts[ret_matches[i].first].lightNum;
			float beta = cloud.pts[ret_matches[i].first].beta;
			lightContrib[lightNum] += beta;
			
		}
	}
	/*
	std::stringstream ss;
	ss << "distr: ";
	for (const auto& elem : lightContrib) {
		ss << " i " << elem.first << " b " << elem.second;
	}
	LOG(INFO) << ss.str();
	*/
	return SparseDistribution1D::createSparseDistribution1D(lightContrib, minContributionScale, scene.lights.size());
}

PhotonBasedMlCdfKdTreeLightDistribution::PhotonBasedMlCdfKdTreeLightDistribution(const ParamSet &params, const Scene &scene) :
	scene(scene),
	kdtree(3 /*dim*/, cdfCloud, KDTreeSingleIndexAdaptorParams(10 /* max leaf */)),
	photonCount(params.FindOneInt("photonCount", 100000)),
	minContributionScale(params.FindOneFloat("minContributionScale", 0.001)),
	knCdf(params.FindOneInt("knCdf", 16)),
	knn(params.FindOneBool("knn", true)),
	cdfCount(params.FindOneInt("cdfCount", 264))
{
	ProfilePhase _(Prof::LightDistribCreation);
	pbrt::PbrtOptions.filenameInfo.photonCount = &photonCount;
	pbrt::PbrtOptions.filenameInfo.minContributionScale = &minContributionScale;
	pbrt::PbrtOptions.filenameInfo.knn = &knn;
	pbrt::PbrtOptions.filenameInfo.cdfCount = &cdfCount;
	pbrt::PbrtOptions.filenameInfo.knCdf = &knCdf;


	if (params.FindOneString("photonsampling", "uni") == "uni") {
		std::vector<Float> prob(scene.lights.size(), Float(1));
		photonDistrib.reset(new Distribution1D(&prob[0], int(prob.size())));
	}
	else {
		photonDistrib = ComputeLightPowerDistribution(scene);
	}

	cloud.pts.resize(photonCount);
	shootPhotons(scene);
	buildCluster();
	kdtree.buildIndex();
}

void PhotonBasedMlCdfKdTreeLightDistribution::buildCluster() {
	std::vector<std::array<Float, 3>> data;
	data.reserve(photonCount);
	for (const auto& photon : cloud.pts) {
		if (photon.lightNum != -1)
			data.push_back({ photon.x, photon.y, photon.z });
	}

	auto cluster_data = dkm::kmeans_lloyd(data, cdfCount);


	std::vector<std::unordered_map<int, Float>> lightContributions(cdfCount);
	// add contributions to cdfs
	for (int i = 0; i < data.size(); i++) {
		const auto& label = std::get<1>(cluster_data)[i];
		// potentially reduce photon beta influence with distance to cluster centroid!?
		lightContributions[label][cloud.pts[i].lightNum] += cloud.pts[i].beta;
	}

	// build cdf cloud
	cdfCloud.pts.resize(cdfCount);
	for (int i = 0; i < cdfCount; i++) {
		const auto& mean = std::get<0>(cluster_data)[i];
		cdfCloud.pts[i].x = mean[0];
		cdfCloud.pts[i].y = mean[1];
		cdfCloud.pts[i].z = mean[2];
		pbrt::objFile << "v " << mean[0] << " " << mean[1] << " " << mean[2] << "\n";
		pbrt::objFile << "v " << mean[0] - 1.5f << " " << mean[1] << " " << mean[2] << "\nl -1 -2 \n";
		cdfCloud.pts[i].distr = SparseDistribution1D::createSparseDistribution1D(lightContributions[i], minContributionScale, scene.lights.size(), false);
	}
}

void PhotonBasedMlCdfKdTreeLightDistribution::shootPhotons(const Scene &scene) {

	ParallelFor([&](int photonIndex) {
		// Follow photon path for _photonIndex_
		uint64_t haltonIndex = photonIndex;
		int haltonDim = 0;

		// Choose light to shoot photon from
		Float lightPdf;
		Float lightSample = RadicalInverse(haltonDim++, haltonIndex);
		int lightNum = photonDistrib->SampleDiscrete(lightSample, &lightPdf);
		const std::shared_ptr<Light> &light = scene.lights[lightNum];

		// Compute sample values for photon ray leaving light source
		Point2f uLight0(RadicalInverse(haltonDim, haltonIndex),
			RadicalInverse(haltonDim + 1, haltonIndex));
		Point2f uLight1(RadicalInverse(haltonDim + 2, haltonIndex),
			RadicalInverse(haltonDim + 3, haltonIndex));
		// Camera not available here, add Camera to the Scene object?
		Float uLightTime = 0; //Lerp(RadicalInverse(haltonDim + 4, haltonIndex), camera->shutterOpen, camera->shutterClose);
		haltonDim += 5;

		// Generate _photonRay_ from light source and initialize _beta_
		RayDifferential photonRay;
		Normal3f nLight;
		Float pdfPos, pdfDir;
		Spectrum Le =
			light->Sample_Le(uLight0, uLight1, uLightTime, &photonRay,
				&nLight, &pdfPos, &pdfDir);
		if (pdfPos == 0 || pdfDir == 0 || Le.IsBlack()) return;
		Spectrum beta = (AbsDot(nLight, photonRay.d) * Le) /
			(lightPdf * pdfPos * pdfDir);
		if (beta.IsBlack()) return;
		Float fbeta = beta.sumValues();

		// Follow photon through scene and record intersection
		SurfaceInteraction isect;
		if (scene.Intersect(photonRay, &isect)) {
			// Add photon to kd-tree if intersection found and is difuse
			// TODO: difuse
			cloud.pts[photonIndex].x = isect.p.x;
			cloud.pts[photonIndex].y = isect.p.y;
			cloud.pts[photonIndex].z = isect.p.z;
			cloud.pts[photonIndex].beta = fbeta;
			cloud.pts[photonIndex].lightNum = lightNum;
			cloud.pts[photonIndex].fromDir = -Normalize(photonRay.d);
		}
		else {
			cloud.pts[photonIndex].x = FLT_MAX;
			cloud.pts[photonIndex].y = FLT_MAX;
			cloud.pts[photonIndex].z = FLT_MAX;
			cloud.pts[photonIndex].beta = 0.0;
			cloud.pts[photonIndex].lightNum = -1;
		}

	}, photonCount, 4096);

}

const Distribution1D *PhotonBasedMlCdfKdTreeLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
	ProfilePhase _(Prof::LightDistribLookup);
	++nLookups;

	const Float query_pt[3] = { p.x, p.y, p.z };
	if (knn) {
		// perform a k-nearest-neighbour search to find #nearestNeighbours
		size_t num_results = knCdf;
		std::vector<size_t> ret_index(num_results);
		std::vector<Float> out_dist_sqr(num_results);

		num_results = kdtree.knnSearch(&query_pt[0], num_results, &ret_index[0], &out_dist_sqr[0]);
		ret_index.resize(num_results);
		out_dist_sqr.resize(num_results);

		std::vector<const Distribution1D*> distributions;
		std::vector<Float> influence;
		for (size_t i = 0; i < num_results; i++) {
			distributions.push_back(cdfCloud.pts[ret_index[i]].distr);
			influence.push_back(1.0f / out_dist_sqr[i]);
		}
		InterpolatedDistribution1D* iDistr = new InterpolatedDistribution1D(&influence[0], &distributions[0], influence.size());
		iDistr->deleteAfterUsage = true;
		return iDistr;
	}
	else {
		// Radius search not implemented for mlcdftree
		CHECK(0);
	}
	return nullptr;
}

PhotonBasedCdfKdTreeLightDistribution::PhotonBasedCdfKdTreeLightDistribution(const ParamSet &params, const Scene &scene) :
	scene(scene),
	photonCount(params.FindOneInt("photonCount", 100000)),
	cdfCount(params.FindOneInt("cdfCount", 8)),
	interpolation(params.FindOneString("interpolation", "shepard")),
	intSmooth(params.FindOneFloat("intSmooth", 1.0f)),
	photonThreshold(params.FindOneInt("photonThreshold", 15)),
	photonkdtree(3 /*dim*/, cloud, KDTreeSingleIndexAdaptorParams(photonCount / cdfCount /* max leaf */)),
	cdfkdtree(3 /*dim*/, cdfCloud, KDTreeSingleIndexAdaptorParams(10 /* max leaf */)),
	minContributionScale(params.FindOneFloat("minContributionScale", 0.001)),
	knCdf(params.FindOneInt("knCdf", 16)),
	knn(params.FindOneBool("knn", true))
{
	ProfilePhase _(Prof::LightDistribCreation);
	pbrt::PbrtOptions.filenameInfo.photonCount = &photonCount;
	pbrt::PbrtOptions.filenameInfo.minContributionScale = &minContributionScale;
	pbrt::PbrtOptions.filenameInfo.knn = &knn;
	pbrt::PbrtOptions.filenameInfo.cdfCount = &cdfCount;
	pbrt::PbrtOptions.filenameInfo.knCdf = &knCdf;

	pbrt::PbrtOptions.filenameInfo.interpolation = &interpolation;
	pbrt::PbrtOptions.filenameInfo.intSmooth = &intSmooth;
	pbrt::PbrtOptions.filenameInfo.photonThreshold = &photonThreshold;

	if (params.FindOneString("photonsampling", "uni") == "uni") {
		std::vector<Float> prob(scene.lights.size(), Float(1));
		photonDistrib.reset(new Distribution1D(&prob[0], int(prob.size())));
	} else {
		photonDistrib = ComputeLightPowerDistribution(scene);
	}

	cloud.pts.resize(photonCount);
	shootPhotons(scene);
	photonkdtree.buildIndex();
	buildCluster();
	cdfkdtree.buildIndex();
}

void PhotonBasedCdfKdTreeLightDistribution::buildCluster() {
	std::vector<std::vector<size_t>> clusters;
	photonkdtree.collectAllLeafs(clusters, photonkdtree.root_node);
	//LOG(INFO) << "NUM CLUSTERS = " << clusters.size();
	std::mutex m_screen;
	ParallelFor([&](int cdfIndex) {
		const auto& cluster = clusters[cdfIndex];
		PhotonBasedCdfKdTreeLightDistribution::CdfCloud<Float>::Cdf cdf;
		std::unordered_map<int, Float> lightContrib;
		int numPhotons = 0;
		for (const auto& photonIndex : cluster) {
			const auto& photon = cloud.pts[photonIndex];
			if (photon.lightNum == -1) continue;
			cdf.x += photon.x;
			cdf.y += photon.y;
			cdf.z += photon.z;
			lightContrib[photon.lightNum] += photon.beta;
			numPhotons++;
		}
		if (numPhotons > photonThreshold) {
			cdf.x /= numPhotons;
			cdf.y /= numPhotons;
			cdf.z /= numPhotons;
			cdf.distr = SparseDistribution1D::createSparseDistribution1D(lightContrib, minContributionScale, scene.lights.size(), false);
			cdf.weight = numPhotons;
			m_screen.lock();
			//pbrt::objFile << "v " << cdf.x << " " << cdf.y << " " << cdf.z << "\n";
			//pbrt::objFile << "v " << cdf.x - 1.5f << " " << cdf.y << " " << cdf.z << "\nl -1 -2 \n";
			cdfCloud.pts.push_back(cdf);
			m_screen.unlock();
		}
	}, clusters.size(), 1024);
	//LOG(INFO) << "NUM CLUSTERS AFTER = " << cdfCloud.pts.size();
}

void PhotonBasedCdfKdTreeLightDistribution::shootPhotons(const Scene &scene) {
	
	ParallelFor([&](int photonIndex) {
		// Follow photon path for _photonIndex_
		uint64_t haltonIndex = photonIndex;
		int haltonDim = 0;

		// Choose light to shoot photon from
		Float lightPdf;
		Float lightSample = RadicalInverse(haltonDim++, haltonIndex);
		int lightNum = photonDistrib->SampleDiscrete(lightSample, &lightPdf);
		const std::shared_ptr<Light> &light = scene.lights[lightNum];

		// Compute sample values for photon ray leaving light source
		Point2f uLight0(RadicalInverse(haltonDim, haltonIndex),
			RadicalInverse(haltonDim + 1, haltonIndex));
		Point2f uLight1(RadicalInverse(haltonDim + 2, haltonIndex),
			RadicalInverse(haltonDim + 3, haltonIndex));
		// Camera not available here, add Camera to the Scene object?
		Float uLightTime = 0; //Lerp(RadicalInverse(haltonDim + 4, haltonIndex), camera->shutterOpen, camera->shutterClose);
		haltonDim += 5;

		// Generate _photonRay_ from light source and initialize _beta_
		RayDifferential photonRay;
		Normal3f nLight;
		Float pdfPos, pdfDir;
		Spectrum Le =
			light->Sample_Le(uLight0, uLight1, uLightTime, &photonRay,
				&nLight, &pdfPos, &pdfDir);
		if (pdfPos == 0 || pdfDir == 0 || Le.IsBlack()) return;
		Spectrum beta = (AbsDot(nLight, photonRay.d) * Le) /
			(lightPdf * pdfPos * pdfDir);
		if (beta.IsBlack()) return;
		Float fbeta = beta.sumValues();

		// Follow photon through scene and record intersection
		SurfaceInteraction isect;
		if (scene.Intersect(photonRay, &isect)) {
			// Add photon to kd-tree if intersection found and is difuse
			cloud.pts[photonIndex].x = isect.p.x;
			cloud.pts[photonIndex].y = isect.p.y;
			cloud.pts[photonIndex].z = isect.p.z;
			cloud.pts[photonIndex].beta = fbeta;
			cloud.pts[photonIndex].lightNum = lightNum;
		}
		else {
			cloud.pts[photonIndex].x = FLT_MAX;
			cloud.pts[photonIndex].y = FLT_MAX;
			cloud.pts[photonIndex].z = FLT_MAX;
			cloud.pts[photonIndex].beta = 0.0;
			cloud.pts[photonIndex].lightNum = -1;
		}
	}, photonCount, 4096);
}

const Distribution1D *PhotonBasedCdfKdTreeLightDistribution::Lookup(const Point3f &p, const Normal3f &n) const {
	ProfilePhase _(Prof::LightDistribLookup);
	++nLookups;
	const Float query_pt[3] = { p.x, p.y, p.z };

	if (knn) {
		// perform a k-nearest-neighbour search to find #nearestNeighbours and interpolate them
		size_t num_results = knCdf;
		std::vector<size_t> ret_index(num_results);
		std::vector<Float> out_dist_sqr(num_results);

		num_results = cdfkdtree.knnSearch(&query_pt[0], num_results, &ret_index[0], &out_dist_sqr[0]);
		ret_index.resize(num_results);
		out_dist_sqr.resize(num_results);

		std::vector<const Distribution1D*> distributions;
		std::vector<Float> influence;

		if (interpolation == "shepard") {
			for (size_t i = 0; i < num_results; i++) {
				distributions.push_back(cdfCloud.pts[ret_index[i]].distr);
				Float d = std::max(0.0001f, pow(out_dist_sqr[i], intSmooth));
				influence.push_back(cdfCloud.pts[ret_index[i]].weight * (1.0f / d));
			}
		} else if (interpolation == "modshep") {
			Float maxR = 0;
			for (size_t i = 0; i < num_results; i++) {
				maxR = std::max(maxR, out_dist_sqr[i]);
			}
			maxR = pow(maxR, intSmooth);
			for (size_t i = 0; i < num_results; i++) {
				distributions.push_back(cdfCloud.pts[ret_index[i]].distr);
				Float d = std::max(0.0001f, pow(out_dist_sqr[i], intSmooth));
				influence.push_back(cdfCloud.pts[ret_index[i]].weight * pow((maxR - d) / (maxR * d), 2));
			}
		} else if (interpolation == "kreg") {
			for (size_t i = 0; i < num_results; i++) {
				distributions.push_back(cdfCloud.pts[ret_index[i]].distr);
				Float d = sqrt(out_dist_sqr[i]);
				influence.push_back(cdfCloud.pts[ret_index[i]].weight * exp(-pow(d / intSmooth, 2)));
			}
		} else if (interpolation == "adkreg") {
			Float maxR = 0;
			for (size_t i = 0; i < num_results; i++) {
				maxR = std::max(maxR, out_dist_sqr[i]);
			}
			maxR = sqrt(maxR);
			Float p = maxR / sqrt(-log(intSmooth));
			for (size_t i = 0; i < num_results; i++) {
				distributions.push_back(cdfCloud.pts[ret_index[i]].distr);
				Float d = sqrt(out_dist_sqr[i]);
				influence.push_back(cdfCloud.pts[ret_index[i]].weight * (exp(-pow(d / p, 2)) - intSmooth));
			}
		}

		InterpolatedDistribution1D* iDistr = new InterpolatedDistribution1D(&influence[0], &distributions[0], influence.size());
		iDistr->deleteAfterUsage = true;
		return iDistr;
	}
	else {
		// Radius search not implemented for cdftree
		CHECK(0);
	}
	return photonDistrib.get();
}

}  // namespace pbrt
