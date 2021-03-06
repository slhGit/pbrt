#include "outliner.h"
#include "interaction.h"
#include "scene.h"
#include "interaction.h"
#include "sampling.h"
#include "parallel.h"
#include "film.h"
#include "sampler.h"
#include "integrator.h"
#include "progressreporter.h"
#include "camera.h"
#include "stats.h"
#include "paramset.h"

#include <set>
using std::set;

namespace pbrt {
	//STAT_COUNTER("Integrator/Camera rays traced", nCameraRays);

	void OutlineIntegrator::Render(const Scene &scene) {
		// Render image tiles in parallel

		// Compute number of tiles, _nTiles_, to use for parallel rendering
		Bounds2i sampleBounds = camera->film->GetSampleBounds();
		Vector2i sampleExtent = sampleBounds.Diagonal();
		const int tileSize = 16;
		Point2i nTiles((sampleExtent.x + tileSize - 1) / tileSize,
			(sampleExtent.y + tileSize - 1) / tileSize);
		ProgressReporter reporter(nTiles.x * nTiles.y, "Rendering");
		{
			ParallelFor2D([&](Point2i tile) {
				// Render section of image corresponding to _tile_

				// Allocate _MemoryArena_ for tile
				MemoryArena arena;

				// Get sampler instance for tile
				int seed = tile.y * nTiles.x + tile.x;
				std::unique_ptr<Sampler> tileSampler = sampler->Clone(seed);

				// Compute sample bounds for tile
				int x0 = sampleBounds.pMin.x + tile.x * tileSize;
				int x1 = std::min(x0 + tileSize, sampleBounds.pMax.x);
				int y0 = sampleBounds.pMin.y + tile.y * tileSize;
				int y1 = std::min(y0 + tileSize, sampleBounds.pMax.y);
				Bounds2i tileBounds(Point2i(x0, y0), Point2i(x1, y1));
				LOG(INFO) << "Starting image tile " << tileBounds;

				// Get _FilmTile_ for tile
				std::unique_ptr<FilmTile> filmTile =
					camera->film->GetFilmTile(tileBounds);

				// Loop over pixels in tile to render them
				for (Point2i pixel : tileBounds) {
					{
						ProfilePhase pp(Prof::StartPixel);
						tileSampler->StartPixel(pixel);
					}

					// Do this check after the StartPixel() call; this keeps
					// the usage of RNG values from (most) Samplers that use
					// RNGs consistent, which improves reproducability /
					// debugging.
					if (!InsideExclusive(pixel, pixelBounds))
						continue;

					Spectrum L(1.f);
					set<const Material*> hitMaterials;
					set<const Primitive*> hitPrims;
					CameraSample cameraSample;
					
					
					do {
						// Initialize _CameraSample_ for current sample
						cameraSample = tileSampler->GetCameraSample(pixel);

						// Generate camera ray for current sample
						RayDifferential ray;
						camera->GenerateRayDifferential(cameraSample, &ray);
						ray.ScaleDifferentials(1 / std::sqrt((Float)tileSampler->samplesPerPixel));
						//++nCameraRays;

						// Evaluate radiance along camera ray
						SurfaceInteraction isect;
						bool foundIntersection = scene.Intersect(ray, &isect);

						if (foundIntersection) {
							if (hitMaterials.size() != 0)
								if (hitMaterials.count(isect.primitive->GetMaterial()) == 0) {
							//if (hitPrims.size() > 0)
							//	if(hitPrims.count(isect.primitive) == 0){
									L = Spectrum(0.f);
									break;
								}

							//hitPrims.insert(isect.primitive);
							hitMaterials.insert(isect.primitive->GetMaterial());
						}

						VLOG(1) << "Camera sample: " << cameraSample << " -> ray: " <<ray << " -> L = " << L;

						// Add camera ray's contribution to image
						//filmTile->AddSample(cameraSample.pFilm, L, rayWeight);

						// Free _MemoryArena_ memory from computing image sample
						// value
						arena.Reset();
					} while (tileSampler->StartNextSample());
					filmTile->AddSample(cameraSample.pFilm, L, 1.f);

				}
				LOG(INFO) << "Finished image tile " << tileBounds;

				// Merge image tile into _Film_
				camera->film->MergeFilmTile(std::move(filmTile));
				reporter.Update();
			}, nTiles);
			reporter.Done();
		}
		LOG(INFO) << "Rendering finished";

		// Save final image after rendering
		camera->film->WriteImage();
	}




	OutlineIntegrator *CreateOutlineIntegrator(const ParamSet &params,
		std::shared_ptr<Sampler> sampler,
		std::shared_ptr<const Camera> camera) {
		
		int np;
		const int *pb = params.FindInt("pixelbounds", &np);
		Bounds2i pixelBounds = camera->film->GetSampleBounds();
		if (pb) {
			if (np != 4)
				Error("Expected four values for \"pixelbounds\" parameter. Got %d.",
					np);
			else {
				pixelBounds = Intersect(pixelBounds,
					Bounds2i{ { pb[0], pb[2] },{ pb[1], pb[3] } });
				if (pixelBounds.Area() == 0)
					Error("Degenerate \"pixelbounds\" specified.");
			}
		}
		return new OutlineIntegrator(camera, sampler, pixelBounds);
	}

}
