#define NOMINMAX
#pragma once


#ifndef PBRT_ADAPTIVE_INTEGRATORS_PATH_H
#define PBRT_ADAPTIVE_INTEGRATORS_PATH_H

// integrators/path.h*
#include "pbrt.h"
#include "adaptive_integrator.h"
#include "lightdistrib.h"

namespace pbrt {


// AdaptivePathInegrator Declarations
class AdaptivePathIntegrator : public AdaptiveSamplerIntegrator {
  public:
    // AdaptivePathInegrator Public Methods
    AdaptivePathIntegrator(int maxDepth, std::shared_ptr<const Camera> camera,
                   std::shared_ptr<Sampler> sampler,
                   const Bounds2i &pixelBounds, int minSamples, Float minVar, Float maxVar, Float alpha = 1, Float rrThreshold = 1,
                   const std::string &lightSampleStrategy = "spatial");

    void Preprocess(const Scene &scene, Sampler &sampler);
    Spectrum Li(const RayDifferential &ray, const Scene &scene,
                Sampler &sampler, MemoryArena &arena, int depth) const;

  private:
    // AdaptivePathInegrator Private Data
	const int maxDepth;
    const Float rrThreshold;
    const std::string lightSampleStrategy;
    std::unique_ptr<LightDistribution> lightDistribution;
};


// AdaptivePathInegrator Declarations
class DammertzPathIntegrator : public DammertzSamplerIntegrator {
public:
	// AdaptivePathInegrator Public Methods
	DammertzPathIntegrator(int maxDepth, std::shared_ptr<const Camera> camera, std::shared_ptr<Sampler> sampler,
		const Bounds2i &pixelBounds, int samplesPerIteration, Float minVar, Float rrThreshold = 1,
		const std::string &lightSampleStrategy = "spatial");

	void Preprocess(const Scene &scene, Sampler &sampler);
	Spectrum Li(const RayDifferential &ray, const Scene &scene,
		Sampler &sampler, MemoryArena &arena, int depth) const;

private:
	// AdaptivePathInegrator Private Data
	const int maxDepth;
	const Float rrThreshold;
	const std::string lightSampleStrategy;
	std::unique_ptr<LightDistribution> lightDistribution;
};


AdaptivePathIntegrator *CreateAdaptivePathIntegrator(const ParamSet &params,
                                     std::shared_ptr<Sampler> sampler,
                                     std::shared_ptr<const Camera> camera);

DammertzPathIntegrator *CreateDammertzPathIntegrator(const ParamSet &params,
									std::shared_ptr<Sampler> sampler,
									std::shared_ptr<const Camera> camera);
}  // namespace pbrt

#endif  // PBRT_INTEGRATORS_PATH_H
