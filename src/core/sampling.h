
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

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_CORE_SAMPLING_H
#define PBRT_CORE_SAMPLING_H

// core/sampling.h*
#include "pbrt.h"
#include "geometry.h"
#include "rng.h"
#include <algorithm>
#include <unordered_map>

namespace pbrt {

// Sampling Declarations
void StratifiedSample1D(Float *samples, int nsamples, RNG &rng,
                        bool jitter = true);
void StratifiedSample2D(Point2f *samples, int nx, int ny, RNG &rng,
                        bool jitter = true);
void LatinHypercube(Float *samples, int nSamples, int nDim, RNG &rng);
struct Distribution1D {
    // Distribution1D Public Methods
    Distribution1D(const Float *f, int n) : func(f, f + n), cdf(n + 1) {
        // Compute integral of step function at $x_i$
        cdf[0] = 0;
        for (int i = 1; i < n + 1; ++i) cdf[i] = cdf[i - 1] + func[i - 1] / n;

        // Transform step function integral into CDF
        funcInt = cdf[n];
        if (funcInt == 0) {
            for (int i = 1; i < n + 1; ++i) cdf[i] = Float(i) / Float(n);
        } else {
            for (int i = 1; i < n + 1; ++i) cdf[i] /= funcInt;
        }
    }
    virtual int Count() const { return (int)func.size(); }
	virtual Float SampleContinuous(Float u, Float *pdf, int *off = nullptr) const {
        // Find surrounding CDF segments and _offset_
        int offset = FindInterval((int)cdf.size(),
                                  [&](int index) { return cdf[index] <= u; });
        if (off) *off = offset;
        // Compute offset along CDF segment
        Float du = u - cdf[offset];
        if ((cdf[offset + 1] - cdf[offset]) > 0) {
            CHECK_GT(cdf[offset + 1], cdf[offset]);
            du /= (cdf[offset + 1] - cdf[offset]);
        }
        DCHECK(!std::isnan(du));

        // Compute PDF for sampled offset
        if (pdf) *pdf = (funcInt > 0) ? func[offset] / funcInt : 0;

        // Return $x\in{}[0,1)$ corresponding to sample
        return (offset + du) / Distribution1D::Count();
    }
	virtual int SampleDiscrete(Float u, Float *pdf = nullptr,
                       Float *uRemapped = nullptr) const {
        // Find surrounding CDF segments and _offset_
        int offset = FindInterval((int)cdf.size(),
                                  [&](int index) { return cdf[index] <= u; });
        if (pdf) *pdf = (funcInt > 0) ? func[offset] / (funcInt * Distribution1D::Count()) : 0;
        if (uRemapped)
            *uRemapped = (u - cdf[offset]) / (cdf[offset + 1] - cdf[offset]);
        if (uRemapped) CHECK(*uRemapped >= 0.f && *uRemapped <= 1.f);
        return offset;
    }
	virtual Float DiscretePDF(int index) const {
        CHECK(index >= 0 && index < Distribution1D::Count());
        return func[index] / (funcInt * Distribution1D::Count());
    }

	virtual std::string ToString() const {
		std::stringstream ss;
		ss << "distr: ";
		for (int i = 0; i < func.size(); ++i) {
			ss << "sample " << i << " ~ " << (func[i] / (funcInt * Distribution1D::Count()) * 100) << "%, ";
		}
		return ss.str();
	}

	void deleteMe() const {
		if (deleteAfterUsage) delete this;
	}

	virtual ~Distribution1D() {}

    // Distribution1D Public Data
    std::vector<Float> func, cdf;
    Float funcInt;
	bool deleteAfterUsage = false;

};

// Interpolates several given distributions without copying them. The construction of this object 
// takes O(m) and lookup O(m + log n) with m being the number of distributions that will be interpolated
// and n the number of elements within the interpolated distributions. A simple copy init would need O(m * n)
// and O(log n) lookup. We want to avoid running linearly!
// Note that the number of elements within the distributions (n) have to be all equal and in the same order!
struct InterpolatedDistribution1D : Distribution1D {

	InterpolatedDistribution1D(const Float *f, const Distribution1D **distributions, int n)
		: Distribution1D(f, n),
		distributions(distributions, distributions + n) {
		CHECK(n > 0);
	}

	int Count() const { return (int)distributions[0]->Count(); } // this is wrong: only the first count? which count do we want to give?

	int SampleDiscrete(Float u, Float *pdf = nullptr, Float *uRemapped = nullptr) const {
		// offset is the sampled distribution within which we further want to sample
		int offset = FindInterval((int)cdf.size(), [&](int index) { return cdf[index] <= u; });

		// uSub is a new u [0,1) which we use to sample within the distribution. Can get 1.0 in rare cases due to floating point precision
		Float uSub = (u - cdf[offset]) / (cdf[offset + 1] - cdf[offset]);
		uSub = uSub >= 1.0 ? 1.0 - FLT_MIN : uSub;

		int sampledNum = distributions[offset]->SampleDiscrete(uSub);

		// Add up all probablities that this sample was taken
		if (pdf) *pdf = DiscretePDF(sampledNum);
			
		// Calculating uRemapped is not possible sublinearly.
		CHECK(!uRemapped);
		return sampledNum;
	}

	Float DiscretePDF(int index) const {
		Float pdf = 0;
		for (int i = 0; i < func.size(); ++i) {
			pdf += distributions[i]->DiscretePDF(index) * (cdf[i + 1] - cdf[i]);
		}
		return pdf;
	}

	Float SampleContinuous(Float u, Float *pdf, int *off = nullptr) const {
		CHECK(false);
		// NOT IMPLEMENTED!
	}

	virtual ~InterpolatedDistribution1D() {}

	std::vector<const Distribution1D*> distributions;

};

struct SparseDistribution1D : Distribution1D {

	static SparseDistribution1D* createSparseDistribution1D(std::unordered_map<int, Float> &contribMap, Float uniProb, int nAll, bool del = true) {
		std::unordered_map<int, int> backMap;
		std::vector<int> sampleMap;
		std::vector<Float> contrib;
		int i = 0;
		for (const auto& kv : contribMap) {
			if (kv.second > 0) {
				sampleMap.push_back(kv.first);
				contrib.push_back(kv.second);
				backMap[kv.first] = i++;
			}
		}
		SparseDistribution1D* distr = new SparseDistribution1D(backMap, sampleMap, contrib, uniProb, nAll);
		distr->deleteAfterUsage = del;
		return distr;
	}

	SparseDistribution1D(std::unordered_map<int, int> backMap, std::vector<int> sampleMap, std::vector<Float> contrib, Float uniProb, int nAll)
		: Distribution1D(contrib.size() == 0? nullptr : &contrib[0], contrib.size()),
		backMap(backMap),
		sampleMap(sampleMap),
		uniProb(contrib.size() == 0 ? 1.0 : uniProb),
		uniProbSingle(this->uniProb / nAll),
		nAll(nAll) {
		CHECK(nAll > 0);
	}

	int Count() const { return nAll; }

	int SampleDiscrete(Float u, Float *pdf = nullptr, Float *uRemapped = nullptr) const {
		int sampledNum;
		if (u > (1 - uniProb)) {
			// sample from uniform part
			Float newU = (u - (1 - uniProb)) / uniProb;
			sampledNum = newU * nAll;
			// fix newU == 1.0, give back the probability to sample 0 because newU is in [0,1) but can get 1.0 due to floating point precision
			sampledNum = sampledNum == nAll ? nAll - 1 : sampledNum;
		} else {
			// sample from sparse part
			Float newU = u / (1 - uniProb);
			sampledNum = sampleMap[Distribution1D::SampleDiscrete(newU, pdf, uRemapped)];
		}
		if (pdf) *pdf = DiscretePDF(sampledNum);
		// Calculating uRemapped is not implemented for SparseDistribution1D (but could be).
		CHECK(!uRemapped);
		return sampledNum;
	}

	Float DiscretePDF(int index) const {
		CHECK(index >= 0 && index < Count());
		// add the probability that the num got sampled in the uniform part
		Float pdf = uniProbSingle;
		// add the probability that the num got sampled in the sparse part
		auto it = backMap.find(index);
		if (it != backMap.end()) {
			Float pdfIn = Distribution1D::DiscretePDF(it->second);
			pdf += pdfIn * (1 - uniProb);
		}
		return pdf;
	}

	Float SampleContinuous(Float u, Float *pdf, int *off = nullptr) const {
		CHECK(false);
		// NOT IMPLEMENTED!
	}

	virtual ~SparseDistribution1D() {}

	std::unordered_map<int, int> backMap;
	std::vector<int> sampleMap;
	Float uniProb;
	Float uniProbSingle;
	int nAll;
};

Point2f RejectionSampleDisk(RNG &rng);
Vector3f UniformSampleHemisphere(const Point2f &u);
Float UniformHemispherePdf();
Vector3f UniformSampleSphere(const Point2f &u);
Float UniformSpherePdf();
Vector3f UniformSampleCone(const Point2f &u, Float thetamax);
Vector3f UniformSampleCone(const Point2f &u, Float thetamax, const Vector3f &x,
                           const Vector3f &y, const Vector3f &z);
Float UniformConePdf(Float thetamax);
Point2f UniformSampleDisk(const Point2f &u);
Point2f ConcentricSampleDisk(const Point2f &u);
Point2f UniformSampleTriangle(const Point2f &u);
class Distribution2D {
  public:
    // Distribution2D Public Methods
    Distribution2D(const Float *data, int nu, int nv);
    Point2f SampleContinuous(const Point2f &u, Float *pdf) const {
        Float pdfs[2];
        int v;
        Float d1 = pMarginal->SampleContinuous(u[1], &pdfs[1], &v);
        Float d0 = pConditionalV[v]->SampleContinuous(u[0], &pdfs[0]);
        *pdf = pdfs[0] * pdfs[1];
        return Point2f(d0, d1);
    }
    Float Pdf(const Point2f &p) const {
        int iu = Clamp(int(p[0] * pConditionalV[0]->Count()), 0,
                       pConditionalV[0]->Count() - 1);
        int iv =
            Clamp(int(p[1] * pMarginal->Count()), 0, pMarginal->Count() - 1);
        return pConditionalV[iv]->func[iu] / pMarginal->funcInt;
    }

  private:
    // Distribution2D Private Data
    std::vector<std::unique_ptr<Distribution1D>> pConditionalV;
    std::unique_ptr<Distribution1D> pMarginal;
};

// Sampling Inline Functions
template <typename T>
void Shuffle(T *samp, int count, int nDimensions, RNG &rng) {
    for (int i = 0; i < count; ++i) {
        int other = i + rng.UniformUInt32(count - i);
        for (int j = 0; j < nDimensions; ++j)
            std::swap(samp[nDimensions * i + j], samp[nDimensions * other + j]);
    }
}

inline Vector3f CosineSampleHemisphere(const Point2f &u) {
    Point2f d = ConcentricSampleDisk(u);
    Float z = std::sqrt(std::max((Float)0, 1 - d.x * d.x - d.y * d.y));
    return Vector3f(d.x, d.y, z);
}

inline Float CosineHemispherePdf(Float cosTheta) { return cosTheta * InvPi; }

inline Float BalanceHeuristic(int nf, Float fPdf, int ng, Float gPdf) {
    return (nf * fPdf) / (nf * fPdf + ng * gPdf);
}

inline Float PowerHeuristic(int nf, Float fPdf, int ng, Float gPdf) {
    Float f = nf * fPdf, g = ng * gPdf;
    return (f * f) / (f * f + g * g);
}

}  // namespace pbrt

#endif  // PBRT_CORE_SAMPLING_H
