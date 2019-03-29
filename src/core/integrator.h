#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_CORE_INTEGRATOR_H
#define PBRT_CORE_INTEGRATOR_H

// core/integrator.h*
#include "pbrt.h"
#include "primitive.h"
#include "spectrum.h"
#include "light.h"
#include "reflection.h"
#include "sampler.h"
#include "material.h"

namespace pbrt {

// Integrator Declarations
class Integrator {
  public:
    // Integrator Interface
    virtual ~Integrator();
    virtual void Render(const Scene &scene) = 0;
};

Spectrum UniformSampleAllLights(const Interaction &it, const Scene &scene,
                                MemoryArena &arena, Sampler &sampler,
                                const std::vector<int> &nLightSamples,
                                bool handleMedia = false);
Spectrum UniformSampleOneLight(const Interaction &it, const Scene &scene,
                               MemoryArena &arena, Sampler &sampler,
                               bool handleMedia = false,
                               const Distribution1D *lightDistrib = nullptr);
Spectrum EstimateDirect(const Interaction &it, const Point2f &uShading,
                        const Light &light, const Point2f &uLight,
                        const Scene &scene, Sampler &sampler,
                        MemoryArena &arena, bool handleMedia = false,
                        bool specular = false);
std::unique_ptr<Distribution1D> ComputeLightPowerDistribution(
    const Scene &scene);

// SamplerIntegrator Declarations
class SamplerIntegrator : public Integrator {
	friend class AdaptiveSamplerIntegrator;
	friend class DammertzSamplerIntegrator;
	friend class pDammertzSamplerIntegrator;

  public:
    // SamplerIntegrator Public Methods
    SamplerIntegrator(std::shared_ptr<const Camera> camera,
                      std::shared_ptr<Sampler> sampler,
                      const Bounds2i &pixelBounds)
        : camera(camera), sampler(sampler), pixelBounds(pixelBounds) {}
    virtual void Preprocess(const Scene &scene, Sampler &sampler) {}
    virtual void Render(const Scene &scene);
    virtual Spectrum Li(const RayDifferential &ray, const Scene &scene,
                        Sampler &sampler, MemoryArena &arena,
                        int depth = 0) const = 0;
    Spectrum SpecularReflect(const RayDifferential &ray,
                             const SurfaceInteraction &isect,
                             const Scene &scene, Sampler &sampler,
                             MemoryArena &arena, int depth) const;
    Spectrum SpecularTransmit(const RayDifferential &ray,
                              const SurfaceInteraction &isect,
                              const Scene &scene, Sampler &sampler,
                              MemoryArena &arena, int depth) const;

  protected:
    // SamplerIntegrator Protected Data
    std::shared_ptr<const Camera> camera;

  private:
    // SamplerIntegrator Private Data
    std::shared_ptr<Sampler> sampler;
    const Bounds2i pixelBounds;
};


class DammertzSamplerIntegrator : public SamplerIntegrator {
public:
	DammertzSamplerIntegrator(std::shared_ptr<const Camera> camera, std::shared_ptr<Sampler> sampler, const Bounds2i &pixelBounds, int samplesPerIteration, Float minVar)
		: samplesPerIteration(samplesPerIteration > sampler->samplesPerPixel ? sampler->samplesPerPixel : samplesPerIteration), SamplerIntegrator(camera, sampler, pixelBounds), minVar(minVar) 
	{}

	void Render(const Scene &scene);

	const int samplesPerIteration;
	const Float minVar;
private:

	Float Error(const Bounds2i&, const Bounds2i& tileBounds, const std::unique_ptr<FilmTile>& filmTileA, const std::unique_ptr<FilmTile>& filmTileB, int iter = 0);
	void Split(Bounds2i b, std::vector<Bounds2i>& V, const std::unique_ptr<FilmTile>& filmTileA, const std::unique_ptr<FilmTile>& filmTileB, Bounds2i t);
}; 

}  // namespace pbrt

#endif  // PBRT_CORE_INTEGRATOR_H
