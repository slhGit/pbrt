#include "adaptive_integrator.h"

#include "film.h"
#include "camera.h"
#include "progressreporter.h"
#include "filters\box.h"

namespace pbrt {
	STAT_COUNTER("Integrator/Camera rays traced", nCameraRays);


	class TaggedFilmTile : public FilmTile {
	public:
		TaggedFilmTile(std::unique_ptr<FilmTile> f, Bounds2i tb, std::unique_ptr<Sampler> ts, int s)
			: FilmTile(f->pixelBounds, f->filterRadius, f->filterTable, f->filterTableSize, f->maxSampleLuminance)
		{
			done = samplesDone = false;
			tileBounds = tb;
			tileSampler = std::move(ts);
			samples = s;
		}

		float GetTileAvg() {
			Float sum = 0;
			Float count = 0;

			for (FilmTilePixel pixel : pixels) {
				count += 1.f;
				sum += pixel.contribSum.y();
			}

			return sum / count;
		}

		Float GetVariance() {
			Float min = MaxFloat;
			Float max = MinFloat;

			for (FilmTilePixel pixel : pixels) {
				Float temp = pixel.contribSum.y() / pixel.filterWeightSum;
				if (temp < min)
					min = temp;
				else if (temp > max)
					max = temp;
			}

			return max - min;
		}

		void TileDone(Float delta) {
			done = GetVariance() < delta;
		}

		bool done, samplesDone;
		std::unique_ptr<Sampler> tileSampler;
		Bounds2i tileBounds;
		int samples;
	};


	bool TilesNeedWork(std::vector<TaggedFilmTile*>& tiles) {
		bool noMoreSamples = true, converged = true;
		for (TaggedFilmTile* tile : tiles) {
			if (!(tile->samplesDone))
				noMoreSamples = false;
			if (!(tile->done)) {
				converged = false;
			}
		}
		if (noMoreSamples)
			return false;
		else {
			return !converged;
		}
	}


	void CompareTiles(std::vector<TaggedFilmTile*>& tiles, float delta, const Point2i& nCount) {
		std::vector<Float> lum;
		for (int y = 0; y < nCount.y; y++) {
			for (int x = 0; x < nCount.x; x++) {
				lum.push_back(tiles[y * nCount.x + x]->GetTileAvg());
			}
		}

		for (int y = 0; y < nCount.y; y++) {
			int ymin = y - 1 < 0 ? 0 : y - 1;
			int ymax = y + 1 > nCount.y ? nCount.y : y + 1;

			for (int x = 0 + (y % 2); x < nCount.x; x += 2) {
				int xmin = x - 1 < 0 ? 0 : x - 1;
				int xmax = x + 1 > nCount.x ? nCount.y : x + 1;

				int i1 = y * nCount.x + x, i2;
				for (int yy = ymin; yy < ymax; yy++) {
					for (int xx = xmin; xx < xmax; xx++) {
						i2 = yy * nCount.x + xx;

						if (std::fabs(lum[i1] - lum[i2]) > 0.f) { //delta) {
							tiles[i1]->done = false;
							tiles[i2]->done = false;
						}
					}
				}
			}
		}
	}

	class FilmTileWrap {
	public:
		FilmTileWrap(FilmTile* ft, Bounds2i& tb, uint64_t s) {
			filmTile = ft;
			tileBounds = tb;
			variance = 0;
			samples = s;
		}

		Float SetVariance(Float minVar, Float maxVar, Float alpha) {
			Float min = MaxFloat, max = MinFloat;

			for (FilmTilePixel pixel : filmTile->pixels) {
				Float temp = pixel.contribSum.y() / pixel.filterWeightSum;
				if (temp < min)
					min = temp;
				else if (temp > max)
					max = temp;
			}

			variance = pow(Clamp(((max - min) - minVar) / (maxVar - minVar), 0, 1), alpha);
			return variance;
		}

		FilmTile* filmTile;
		Float variance;
		uint64_t samples;
		Bounds2i tileBounds;
	};

	void AdaptiveSamplerIntegrator::LoopTiles(Point2i nTiles, std::vector<FilmTileWrap>& filmTiles, const Scene& scene, ProgressReporter& reporter, int pass, uint64_t start) {
		ParallelFor2D([&](Point2i tile) {
			// Render section of image corresponding to _tile_
			// Allocate _MemoryArena_ for tile

			MemoryArena arena;

			// Get sampler instance for tile
			int tileNum = tile.y * nTiles.x + tile.x;
			int seed = tileNum + pass * (nTiles.y * nTiles.x + nTiles.x);
			std::unique_ptr<Sampler> tileSampler = sampler->Clone(seed);

			// Get _FilmTile_ for tile
			FilmTileWrap* filmTile = &(filmTiles[tileNum]);

			// Loop over pixels in tile to render them
			for (Point2i pixel : filmTile->tileBounds) {
				{
					ProfilePhase pp(Prof::StartPixel);
					tileSampler->StartPixel(pixel);
					tileSampler->SetSampleNumber(start);				}

				// Do this check after the StartPixel() call; this keeps
				// the usage of RNG values from (most) Samplers that use
				// RNGs consistent, which improves reproducability /
				// debugging.
				if (!InsideExclusive(pixel, pixelBounds))
					continue;

				uint64_t passSamples = 0;
				//tileSampler->SetSampleNumber(minSamples);

				while (passSamples < filmTile->samples) {
					// Initialize _CameraSample_ for current sample
					CameraSample cameraSample = tileSampler->GetCameraSample(pixel);
					passSamples++;

					// Generate camera ray for current sample
					RayDifferential ray;
					Float rayWeight = camera->GenerateRayDifferential(cameraSample, &ray);
					ray.ScaleDifferentials(1 / std::sqrt((Float)sampler->samplesPerPixel));
					++nCameraRays;

					// Evaluate radiance along camera ray
					Spectrum L(0.f);
					if (rayWeight > 0) L = Li(ray, scene, *tileSampler, arena);


					// Issue warning if unexpected radiance value returned
					if (L.HasNaNs()) {
						LOG(ERROR) << StringPrintf(
							"Not-a-number radiance value returned "
							"for pixel (%d, %d), sample %d. Setting to black.",
							pixel.x, pixel.y,
							(int)tileSampler->CurrentSampleNumber());
						L = Spectrum(0.f);
					}
					else if (L.y() < -1e-5) {
						LOG(ERROR) << StringPrintf(
							"Negative luminance value, %f, returned "
							"for pixel (%d, %d), sample %d. Setting to black.",
							L.y(), pixel.x, pixel.y,
							(int)tileSampler->CurrentSampleNumber());
						L = Spectrum(0.f);
					}
					else if (std::isinf(L.y())) {
						LOG(ERROR) << StringPrintf(
							"Infinite luminance value returned "
							"for pixel (%d, %d), sample %d. Setting to black.",
							pixel.x, pixel.y,
							(int)tileSampler->CurrentSampleNumber());
						L = Spectrum(0.f);
					}
					VLOG(1) << "Camera sample: " << cameraSample << " -> ray: " << ray << " -> L = " << L;

					// Add camera ray's contribution to image
					filmTile->filmTile->AddSample(cameraSample.pFilm, L, rayWeight);

					// Free _MemoryArena_ memory from computing image sample value
					arena.Reset();

					tileSampler->StartNextSample();
				}
			}
			LOG(INFO) << "Finished image tile " << filmTile->tileBounds;

			reporter.Update();
		}, nTiles);
	}

	void L_errors(Spectrum& L, Point2i& pixel, Sampler& sampler) {
		if (L.HasNaNs()) {
			LOG(ERROR) << StringPrintf(
				"Not-a-number radiance value returned "
				"for pixel (%d, %d), sample %d. Setting to black.",
				pixel.x, pixel.y,
				(int)sampler.CurrentSampleNumber());
			L = Spectrum(0.f);
		}
		else if (L.y() < -1e-5) {
			LOG(ERROR) << StringPrintf(
				"Negative luminance value, %f, returned "
				"for pixel (%d, %d), sample %d. Setting to black.",
				L.y(), pixel.x, pixel.y,
				(int)sampler.CurrentSampleNumber());
			L = Spectrum(0.f);
		}
		else if (std::isinf(L.y())) {
			LOG(ERROR) << StringPrintf(
				"Infinite luminance value returned "
				"for pixel (%d, %d), sample %d. Setting to black.",
				pixel.x, pixel.y,
				(int)sampler.CurrentSampleNumber());
			L = Spectrum(0.f);
		}
	}

	// SamplerIntegrator Method Definitions
	void AdaptiveSamplerIntegrator::Render(const Scene &scene) {
		Preprocess(scene, *sampler);

		std::unique_ptr<Film> sFilm = std::make_unique<Film>(*(camera->film), std::make_unique<BoxFilter>(Vector2f(0.5f, 0.5f)), (camera->film->filename).substr(0, (camera->film->filename).size() - 4) + "_sVar.png");
		// Render image tiles in parallel

		// Compute number of tiles, _nTiles_, to use for parallel rendering
		Bounds2i sampleBounds = camera->film->GetSampleBounds();
		Vector2i sampleExtent = sampleBounds.Diagonal();
		const int tileSize = 16;
		Point2i nTiles((sampleExtent.x + tileSize - 1) / tileSize, (sampleExtent.y + tileSize - 1) / tileSize);

		uint64_t maxSamples = sampler->samplesPerPixel;

		// Setup filmTiles
		std::vector<FilmTileWrap> filmTiles;
		for (int y = 0; y < nTiles.y; y++) {
			for (int x = 0; x < nTiles.x; x++) {
				int x0 = sampleBounds.pMin.x + x * tileSize;
				int x1 = std::min(x0 + tileSize, sampleBounds.pMax.x);
				int y0 = sampleBounds.pMin.y + y * tileSize;
				int y1 = std::min(y0 + tileSize, sampleBounds.pMax.y);
				Bounds2i tileBounds(Point2i(x0, y0), Point2i(x1, y1));


				filmTiles.push_back(FilmTileWrap((camera->film->GetFilmTile(tileBounds)).release(), tileBounds, minSamples));
			}
		}

		ProgressReporter reporter(nTiles.x * nTiles.y * 2, "Rendering");
		{
			//std::cout << "\nstart pass 1\n";
			LoopTiles(nTiles, filmTiles, scene, reporter, 1, 0);
			//std::cout << "\ndone pass 1\n";

			Float sumVariance = 0.f;
			for (FilmTileWrap& tile : filmTiles) {
				sumVariance += tile.SetVariance(minVar, maxVar, alpha);

				std::unique_ptr<FilmTile> sFilmTile = sFilm->GetFilmTile(tile.tileBounds);
				for (int i = 0; i < sFilmTile->pixels.size(); i++) {
					sFilmTile->pixels[i].contribSum = tile.variance;
					sFilmTile->pixels[i].filterWeightSum = 1;
				}
				sFilm->MergeFilmTile(std::move(sFilmTile));

			}

			Float invSumVariance = 1.f / (sumVariance * tileSize * tileSize);
			uint64_t sampleBudget = camera->film->fullResolution.x * camera->film->fullResolution.y * (maxSamples - minSamples);

			//std::cout << "Sum Variance: " << sumVariance << ", Inverse Sum Variance: " << invSumVariance << ", Sample Budget: " << sampleBudget << "\n";
			Float sum = 0.f;
			for (FilmTileWrap& tile : filmTiles) {
				Float v = tile.variance * invSumVariance;
				tile.samples = sampleBudget * v;
				sum += tile.samples;
				//std::cout << "Budget for tile" << tile.tileBounds << ": " << (sampleBudget * v) << "\n";
			}

			//std::cout << "Total Alloted Samples: " << sum << "\n";
			//std::cout << "\nstart pass 2\n";
			LoopTiles(nTiles, filmTiles, scene, reporter, 2, minSamples);
			//std::cout << "\ndone pass 2\n";


			for (FilmTileWrap& tile : filmTiles) {
				std::unique_ptr<FilmTile> filmTile(tile.filmTile);
				camera->film->MergeFilmTile(std::move(filmTile));
			}

			reporter.Done();
		}
		LOG(INFO) << "Rendering finished";


		// Save final image after rendering
		sFilm->WriteImage();
		camera->film->WriteImage();
	}
	void ClampRGB(Float rgb[3]) {
		rgb[0] = (int)Clamp(255.f * GammaCorrect(rgb[0]) + 0.5f, 0.f, 255.f) / 255.f;
		rgb[1] = (int)Clamp(255.f * GammaCorrect(rgb[1]) + 0.5f, 0.f, 255.f) / 255.f;
		rgb[2] = (int)Clamp(255.f * GammaCorrect(rgb[2]) + 0.5f, 0.f, 255.f) / 255.f;
	}


}