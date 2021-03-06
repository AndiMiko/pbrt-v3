
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

#ifndef PBRT_CORE_LIGHTDISTRIB_H
#define PBRT_CORE_LIGHTDISTRIB_H

#include "pbrt.h"
#include "geometry.h"
#include "sampling.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <nanoflann.hpp>
#include <dkm.hpp>

#include <mutex>
using namespace nanoflann;

namespace pbrt {

// LightDistribution defines a general interface for classes that provide
// probability distributions for sampling light sources at a given point in
// space.
class LightDistribution {
  public:
    virtual ~LightDistribution();

    // Given a point |p| in space, this method returns a (hopefully
    // effective) sampling distribution for light sources at that point.
    virtual const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const = 0;
	  
};

std::unique_ptr<LightDistribution> CreateLightSampleDistribution(
	const ParamSet &params, const Scene &scene);

// The simplest possible implementation of LightDistribution: this returns
// a uniform distribution over all light sources, ignoring the provided
// point. This approach works well for very simple scenes, but is quite
// ineffective for scenes with more than a handful of light sources. (This
// was the sampling method originally used for the PathIntegrator and the
// VolPathIntegrator in the printed book, though without the
// UniformLightDistribution class.)
class UniformLightDistribution : public LightDistribution {
  public:
    UniformLightDistribution(const Scene &scene);
    const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

  private:
    std::unique_ptr<Distribution1D> distrib;
};

// PowerLightDistribution returns a distribution with sampling probability
// proportional to the total emitted power for each light. (It also ignores
// the provided point |p|.)  This approach works well for scenes where
// there the most powerful lights are also the most important contributors
// to lighting in the scene, but doesn't do well if there are many lights
// and if different lights are relatively important in some areas of the
// scene and unimportant in others. (This was the default sampling method
// used for the BDPT integrator and MLT integrator in the printed book,
// though also without the PowerLightDistribution class.)
class PowerLightDistribution : public LightDistribution {
  public:
    PowerLightDistribution(const Scene &scene);
    const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

  private:
    std::unique_ptr<Distribution1D> distrib;
};

// A spatially-varying light distribution that adjusts the probability of
// sampling a light source based on an estimate of its contribution to a
// region of space.  A fixed voxel grid is imposed over the scene bounds
// and a sampling distribution is computed as needed for each voxel.
class SpatialLightDistribution : public LightDistribution {
  public:
    SpatialLightDistribution(const Scene &scene, int maxVoxels = 64);
    ~SpatialLightDistribution();
    const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

  private:
    // Compute the sampling distribution for the voxel with integer
    // coordiantes given by "pi".
    Distribution1D *ComputeDistribution(Point3i pi) const;

    const Scene &scene;
    int nVoxels[3];

    // The hash table is a fixed number of HashEntry structs (where we
    // allocate more than enough entries in the SpatialLightDistribution
    // constructor). During rendering, the table is allocated without
    // locks, using atomic operations. (See the Lookup() method
    // implementation for details.)
    struct HashEntry {
        std::atomic<uint64_t> packedPos;
        std::atomic<Distribution1D *> distribution;
    };
    mutable std::unique_ptr<HashEntry[]> hashTable;
    size_t hashTableSize;
};

class PhotonBasedVoxelLightDistribution : public LightDistribution {
public:
	PhotonBasedVoxelLightDistribution(const ParamSet &params, const Scene &scene);
	const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;
	
private:
	void calcPackedPosAndHash(uint64_t* packedPos, uint64_t* hash, Point3i* pi) const;
	const Distribution1D *getDistribution(uint64_t packedPos, uint64_t hash, int* nProbes) const;
	const Distribution1D *getInterpolatedDistribution(const Point3f &p, uint64_t packedPos, uint64_t hash, Point3i* voxelId, int* nProbes) const;

	const Scene &scene;
	std::unique_ptr<Distribution1D> photonDistrib;
	std::unique_ptr<Distribution1D> defaultDistrib;
	const int photonCount;
	const int maxVoxels;
	const bool interpolateCdf;
	const float minContributionScale;

	int nVoxels[3];
	struct HashEntry {
		std::atomic<uint64_t> packedPos;
		std::unique_ptr<std::unordered_map<int, Float>> lightContrib;
		Distribution1D *distribution;
	};
	mutable std::unique_ptr<HashEntry[]> hashTable;
	size_t hashTableSize;

	void initVoxelHashTable();
	void shootPhotons(const Scene &scene);

};

class PhotonBasedKdTreeLightDistribution : public LightDistribution {
		
public:
	PhotonBasedKdTreeLightDistribution(const ParamSet &params, const Scene &scene);
	const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

	template <typename T>
	struct PhotonCloud
	{
		struct Photon
		{
			T  x, y, z;
			float beta;
			int lightNum;
			Vector3f fromDir;
		};

		std::vector<Photon> pts;

		// Must return the number of data points
		inline size_t kdtree_get_point_count() const { return pts.size(); }

		// Returns the dim'th component of the idx'th point in the class
		inline T kdtree_get_pt(const size_t idx, int dim) const
		{
			if (dim == 0) return pts[idx].x;
			else if (dim == 1) return pts[idx].y;
			else return pts[idx].z;
		}

		template <class BBOX>
		bool kdtree_get_bbox(BBOX& /* bb */) const { return false; }
	};

private:
	const Scene &scene;
	std::unique_ptr<Distribution1D> photonDistrib;
	const int photonCount;
	const float minContributionScale;
	const float photonRadius;
	const float nearestNeighbours;
	const bool knn;
	const std::string interpolation;
	const float intSmooth;

	typedef KDTreeSingleIndexAdaptor<
		L2_Simple_Adaptor<Float, PhotonCloud<Float> >,
		PhotonCloud<Float>,
		3 /* dim */
	> my_kd_tree_t;

	PhotonCloud<Float> cloud;
	my_kd_tree_t kdtree;

	void shootPhotons(const Scene &scene);

};


class PhotonBasedMlCdfKdTreeLightDistribution : public LightDistribution {

public:
	PhotonBasedMlCdfKdTreeLightDistribution(const ParamSet &params, const Scene &scene);
	const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

	template <typename T>
	struct PhotonCloud
	{
		struct Photon
		{
			T  x, y, z;
			float beta;
			int lightNum;
			Vector3f fromDir;
		};

		std::vector<Photon> pts;

		// Must return the number of data points
		inline size_t kdtree_get_point_count() const { return pts.size(); }

		// Returns the dim'th component of the idx'th point in the class
		inline T kdtree_get_pt(const size_t idx, int dim) const
		{
			if (dim == 0) return pts[idx].x;
			else if (dim == 1) return pts[idx].y;
			else return pts[idx].z;
		}

		template <class BBOX>
		bool kdtree_get_bbox(BBOX& /* bb */) const { return false; }
	};

	template <typename T>
	struct CdfCloud
	{
		struct Cdf
		{
			T  x, y, z;
			Distribution1D* distr;
		};

		std::vector<Cdf> pts;

		// Must return the number of data points
		inline size_t kdtree_get_point_count() const { return pts.size(); }

		// Returns the dim'th component of the idx'th point in the class
		inline T kdtree_get_pt(const size_t idx, int dim) const
		{
			if (dim == 0) return pts[idx].x;
			else if (dim == 1) return pts[idx].y;
			else return pts[idx].z;
		}

		template <class BBOX>
		bool kdtree_get_bbox(BBOX& /* bb */) const { return false; }
	};

private:
	const Scene &scene;
	std::unique_ptr<Distribution1D> photonDistrib;
	const int photonCount;
	const int cdfCount;
	const float minContributionScale;
	const int knCdf;
	const bool knn;

	typedef KDTreeSingleIndexAdaptor<
		L2_Simple_Adaptor<Float, CdfCloud<Float> >,
		CdfCloud<Float>,
		3 /* dim */
	> my_kd_tree_t;

	PhotonCloud<Float> cloud;
	CdfCloud<Float> cdfCloud;
	my_kd_tree_t kdtree;

	void buildCluster();
	void shootPhotons(const Scene &scene);

};


class PhotonBasedCdfKdTreeLightDistribution : public LightDistribution {

public:
	PhotonBasedCdfKdTreeLightDistribution(const ParamSet &params, const Scene &scene);
	const Distribution1D *Lookup(const Point3f &p, const Normal3f &n = Normal3f()) const;

	template <typename T>
	struct PhotonCloud
	{
		struct Photon
		{
			T  x, y, z;
			float beta;
			int lightNum;
		};

		std::vector<Photon> pts;

		// Must return the number of data points
		inline size_t kdtree_get_point_count() const { return pts.size(); }

		// Returns the dim'th component of the idx'th point in the class
		inline T kdtree_get_pt(const size_t idx, int dim) const
		{
			if (dim == 0) return pts[idx].x;
			else if (dim == 1) return pts[idx].y;
			else return pts[idx].z;
		}

		template <class BBOX>
		bool kdtree_get_bbox(BBOX& /* bb */) const { return false; }
	};

	template <typename T>
	struct CdfCloud
	{
		struct Cdf
		{
			T  x = .0f, y = .0f, z = .0f;
			Distribution1D* distr;
			int weight = 0;
		};

		std::vector<Cdf> pts;

		// Must return the number of data points
		inline size_t kdtree_get_point_count() const { return pts.size(); }

		// Returns the dim'th component of the idx'th point in the class
		inline T kdtree_get_pt(const size_t idx, int dim) const
		{
			if (dim == 0) return pts[idx].x;
			else if (dim == 1) return pts[idx].y;
			else return pts[idx].z;
		}

		template <class BBOX>
		bool kdtree_get_bbox(BBOX& /* bb */) const { return false; }
	};

private:
	const Scene &scene;
	std::unique_ptr<Distribution1D> photonDistrib;
	const int photonCount;
	const int cdfCount;
	const float minContributionScale;
	const int knCdf;
	const bool knn;
	const std::string interpolation;
	const int photonThreshold;
	const float intSmooth;

	typedef KDTreeSingleIndexAdaptor<
		L2_Simple_Adaptor<Float, PhotonCloud<Float> >,
		PhotonCloud<Float>,
		3 /* dim */
	> my_kd_tree_t;

	typedef KDTreeSingleIndexAdaptor<
		L2_Simple_Adaptor<Float, CdfCloud<Float> >,
		CdfCloud<Float>,
		3 /* dim */
	> my_cdf_kd_tree_t;

	PhotonCloud<Float> cloud;
	CdfCloud<Float> cdfCloud;
	my_kd_tree_t photonkdtree;
	my_cdf_kd_tree_t cdfkdtree;

	void buildCluster();
	void shootPhotons(const Scene &scene);

};

}  // namespace pbrt

#endif  // PBRT_CORE_LIGHTDISTRIB_H
