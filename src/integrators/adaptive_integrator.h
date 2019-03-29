#ifndef ADAPTIVE_INTEGRATOR
#define ADAPTIVE_INTEGRATOR

#include "pbrt.h"
#include "integrator.h"

namespace pbrt {

	class FilmTileWrap;

	class AdaptiveSamplerIntegrator : public SamplerIntegrator {
	public:
		AdaptiveSamplerIntegrator(std::shared_ptr<const Camera> camera, std::shared_ptr<Sampler> sampler, const Bounds2i &pixelBounds, int minSamples, Float minVar, Float maxVar, Float alpha)
			: minSamples(minSamples > sampler->samplesPerPixel ? sampler->samplesPerPixel : minSamples), SamplerIntegrator(camera, sampler, pixelBounds), minVar(minVar), maxVar(maxVar), alpha(alpha) {}

		void LoopTiles(Point2i, std::vector<FilmTileWrap>&, const Scene&, ProgressReporter&, int pass, uint64_t startSample);
		void Render(const Scene &scene);

		const int minSamples;
		const Float minVar, maxVar, alpha;

	};
}
#endif