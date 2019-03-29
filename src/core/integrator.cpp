
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

// core/integrator.cpp*
#include "integrator.h"
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
#include "filters\box.h"
#include "api.h"

namespace pbrt {

	STAT_COUNTER("Integrator/Camera rays traced", nCameraRays);

	// Integrator Method Definitions
	Integrator::~Integrator() {}

	// Integrator Utility Functions
	Spectrum UniformSampleAllLights(const Interaction &it, const Scene &scene,
		MemoryArena &arena, Sampler &sampler,
		const std::vector<int> &nLightSamples,
		bool handleMedia) {
		ProfilePhase p(Prof::DirectLighting);
		Spectrum L(0.f);
		for (size_t j = 0; j < scene.lights.size(); ++j) {
			// Accumulate contribution of _j_th light to _L_
			const std::shared_ptr<Light> &light = scene.lights[j];
			int nSamples = nLightSamples[j];
			const Point2f *uLightArray = sampler.Get2DArray(nSamples);
			const Point2f *uScatteringArray = sampler.Get2DArray(nSamples);
			if (!uLightArray || !uScatteringArray) {
				// Use a single sample for illumination from _light_
				Point2f uLight = sampler.Get2D();
				Point2f uScattering = sampler.Get2D();
				L += EstimateDirect(it, uScattering, *light, uLight, scene, sampler,
					arena, handleMedia);
			}
			else {
				// Estimate direct lighting using sample arrays
				Spectrum Ld(0.f);
				for (int k = 0; k < nSamples; ++k)
					Ld += EstimateDirect(it, uScatteringArray[k], *light,
						uLightArray[k], scene, sampler, arena,
						handleMedia);
				L += Ld / nSamples;
			}
		}
		return L;
	}

	Spectrum UniformSampleOneLight(const Interaction &it, const Scene &scene,
		MemoryArena &arena, Sampler &sampler,
		bool handleMedia, const Distribution1D *lightDistrib) {
		ProfilePhase p(Prof::DirectLighting);
		// Randomly choose a single light to sample, _light_
		int nLights = int(scene.lights.size());
		if (nLights == 0) return Spectrum(0.f);
		int lightNum;
		Float lightPdf;
		if (lightDistrib) {
			lightNum = lightDistrib->SampleDiscrete(sampler.Get1D(), &lightPdf);
			if (lightPdf == 0) return Spectrum(0.f);
		}
		else {
			lightNum = std::min((int)(sampler.Get1D() * nLights), nLights - 1);
			lightPdf = Float(1) / nLights;
		}
		const std::shared_ptr<Light> &light = scene.lights[lightNum];
		Point2f uLight = sampler.Get2D();
		Point2f uScattering = sampler.Get2D();
		return EstimateDirect(it, uScattering, *light, uLight,
			scene, sampler, arena, handleMedia) / lightPdf;
	}

	Spectrum EstimateDirect(const Interaction &it, const Point2f &uScattering,
		const Light &light, const Point2f &uLight,
		const Scene &scene, Sampler &sampler,
		MemoryArena &arena, bool handleMedia, bool specular) {
		BxDFType bsdfFlags =
			specular ? BSDF_ALL : BxDFType(BSDF_ALL & ~BSDF_SPECULAR);
		Spectrum Ld(0.f);
		// Sample light source with multiple importance sampling
		Vector3f wi;
		Float lightPdf = 0, scatteringPdf = 0;
		VisibilityTester visibility;
		Spectrum Li = light.Sample_Li(it, uLight, &wi, &lightPdf, &visibility);
		VLOG(2) << "EstimateDirect uLight:" << uLight << " -> Li: " << Li << ", wi: "
			<< wi << ", pdf: " << lightPdf;
		if (lightPdf > 0 && !Li.IsBlack()) {
			// Compute BSDF or phase function's value for light sample
			Spectrum f;
			if (it.IsSurfaceInteraction()) {
				// Evaluate BSDF for light sampling strategy
				const SurfaceInteraction &isect = (const SurfaceInteraction &)it;
				f = isect.bsdf->f(isect.wo, wi, bsdfFlags) *
					AbsDot(wi, isect.shading.n);
				scatteringPdf = isect.bsdf->Pdf(isect.wo, wi, bsdfFlags);
				VLOG(2) << "  surf f*dot :" << f << ", scatteringPdf: " << scatteringPdf;
			}
			else {
				// Evaluate phase function for light sampling strategy
				const MediumInteraction &mi = (const MediumInteraction &)it;
				Float p = mi.phase->p(mi.wo, wi);
				f = Spectrum(p);
				scatteringPdf = p;
				VLOG(2) << "  medium p: " << p;
			}
			if (!f.IsBlack()) {
				// Compute effect of visibility for light source sample
				if (handleMedia) {
					Li *= visibility.Tr(scene, sampler);
					VLOG(2) << "  after Tr, Li: " << Li;
				}
				else {
					if (!visibility.Unoccluded(scene)) {
						VLOG(2) << "  shadow ray blocked";
						Li = Spectrum(0.f);
					}
					else
						VLOG(2) << "  shadow ray unoccluded";
				}

				// Add light's contribution to reflected radiance
				if (!Li.IsBlack()) {
					if (IsDeltaLight(light.flags))
						Ld += f * Li / lightPdf;
					else {
						Float weight =
							PowerHeuristic(1, lightPdf, 1, scatteringPdf);
						Ld += f * Li * weight / lightPdf;
					}
				}
			}
		}

		// Sample BSDF with multiple importance sampling
		if (!IsDeltaLight(light.flags)) {
			Spectrum f;
			bool sampledSpecular = false;
			if (it.IsSurfaceInteraction()) {
				// Sample scattered direction for surface interactions
				BxDFType sampledType;
				const SurfaceInteraction &isect = (const SurfaceInteraction &)it;
				f = isect.bsdf->Sample_f(isect.wo, &wi, uScattering, &scatteringPdf,
					bsdfFlags, &sampledType);
				f *= AbsDot(wi, isect.shading.n);
				sampledSpecular = (sampledType & BSDF_SPECULAR) != 0;
			}
			else {
				// Sample scattered direction for medium interactions
				const MediumInteraction &mi = (const MediumInteraction &)it;
				Float p = mi.phase->Sample_p(mi.wo, &wi, uScattering);
				f = Spectrum(p);
				scatteringPdf = p;
			}
			VLOG(2) << "  BSDF / phase sampling f: " << f << ", scatteringPdf: " <<
				scatteringPdf;
			if (!f.IsBlack() && scatteringPdf > 0) {
				// Account for light contributions along sampled direction _wi_
				Float weight = 1;
				if (!sampledSpecular) {
					lightPdf = light.Pdf_Li(it, wi);
					if (lightPdf == 0) return Ld;
					weight = PowerHeuristic(1, scatteringPdf, 1, lightPdf);
				}

				// Find intersection and compute transmittance
				SurfaceInteraction lightIsect;
				Ray ray = it.SpawnRay(wi);
				Spectrum Tr(1.f);
				bool foundSurfaceInteraction =
					handleMedia ? scene.IntersectTr(ray, sampler, &lightIsect, &Tr)
					: scene.Intersect(ray, &lightIsect);

				// Add light contribution from material sampling
				Spectrum Li(0.f);
				if (foundSurfaceInteraction) {
					if (lightIsect.primitive->GetAreaLight() == &light)
						Li = lightIsect.Le(-wi);
				}
				else
					Li = light.Le(ray);
				if (!Li.IsBlack()) Ld += f * Li * Tr * weight / scatteringPdf;
			}
		}
		return Ld;
	}

	std::unique_ptr<Distribution1D> ComputeLightPowerDistribution(
		const Scene &scene) {
		if (scene.lights.empty()) return nullptr;
		std::vector<Float> lightPower;
		for (const auto &light : scene.lights)
			lightPower.push_back(light->Power().y());
		return std::unique_ptr<Distribution1D>(
			new Distribution1D(&lightPower[0], lightPower.size()));
	}

	// SamplerIntegrator Method Definitions
	void SamplerIntegrator::Render(const Scene &scene) {
		Preprocess(scene, *sampler);
		// Render image tiles in parallel

		// Compute number of tiles, _nTiles_, to use for parallel rendering
		Bounds2i sampleBounds = camera->film->GetSampleBounds();
		Vector2i sampleExtent = sampleBounds.Diagonal();
		const int tileSize = 16;
		Point2i nTiles((sampleExtent.x + tileSize - 1) / tileSize,
			(sampleExtent.y + tileSize - 1) / tileSize);
		uint32_t samples = 0;
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

					do {
						// Initialize _CameraSample_ for current sample
						CameraSample cameraSample = tileSampler->GetCameraSample(pixel);
						// Generate camera ray for current sample
						RayDifferential ray;
						Float rayWeight = camera->GenerateRayDifferential(cameraSample, &ray);
						ray.ScaleDifferentials(1 / std::sqrt((Float)tileSampler->samplesPerPixel));
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
						VLOG(1) << "Camera sample: " << cameraSample << " -> ray: " <<
							ray << " -> L = " << L;

						// Add camera ray's contribution to image
						filmTile->AddSample(cameraSample.pFilm, L, rayWeight);

						// Free _MemoryArena_ memory from computing image sample
						// value
						arena.Reset();
					} while (tileSampler->StartNextSample());
				}
				LOG(INFO) << "Finished image tile " << tileBounds;

				// Merge image tile into _Film_
				camera->film->MergeFilmTile(std::move(filmTile));
				reporter.Update();
			}, nTiles);
			reporter.Done();
		}
		LOG(INFO) << "Rendering finished";

		std::cout << "Render Done, number of samples: " << samples << "\n";
		// Save final image after rendering
		camera->film->WriteImage();
	}

	Spectrum SamplerIntegrator::SpecularReflect(
		const RayDifferential &ray, const SurfaceInteraction &isect,
		const Scene &scene, Sampler &sampler, MemoryArena &arena, int depth) const {
		// Compute specular reflection direction _wi_ and BSDF value
		Vector3f wo = isect.wo, wi;
		Float pdf;
		BxDFType type = BxDFType(BSDF_REFLECTION | BSDF_SPECULAR);
		Spectrum f = isect.bsdf->Sample_f(wo, &wi, sampler.Get2D(), &pdf, type);

		// Return contribution of specular reflection
		const Normal3f &ns = isect.shading.n;
		if (pdf > 0.f && !f.IsBlack() && AbsDot(wi, ns) != 0.f) {
			// Compute ray differential _rd_ for specular reflection
			RayDifferential rd = isect.SpawnRay(wi);
			if (ray.hasDifferentials) {
				rd.hasDifferentials = true;
				rd.rxOrigin = isect.p + isect.dpdx;
				rd.ryOrigin = isect.p + isect.dpdy;
				// Compute differential reflected directions
				Normal3f dndx = isect.shading.dndu * isect.dudx +
					isect.shading.dndv * isect.dvdx;
				Normal3f dndy = isect.shading.dndu * isect.dudy +
					isect.shading.dndv * isect.dvdy;
				Vector3f dwodx = -ray.rxDirection - wo,
					dwody = -ray.ryDirection - wo;
				Float dDNdx = Dot(dwodx, ns) + Dot(wo, dndx);
				Float dDNdy = Dot(dwody, ns) + Dot(wo, dndy);
				rd.rxDirection =
					wi - dwodx + 2.f * Vector3f(Dot(wo, ns) * dndx + dDNdx * ns);
				rd.ryDirection =
					wi - dwody + 2.f * Vector3f(Dot(wo, ns) * dndy + dDNdy * ns);
			}
			return f * Li(rd, scene, sampler, arena, depth + 1) * AbsDot(wi, ns) /
				pdf;
		}
		else
			return Spectrum(0.f);
	}

	Spectrum SamplerIntegrator::SpecularTransmit(
		const RayDifferential &ray, const SurfaceInteraction &isect,
		const Scene &scene, Sampler &sampler, MemoryArena &arena, int depth) const {
		Vector3f wo = isect.wo, wi;
		Float pdf;
		const Point3f &p = isect.p;
		const Normal3f &ns = isect.shading.n;
		const BSDF &bsdf = *isect.bsdf;
		Spectrum f = bsdf.Sample_f(wo, &wi, sampler.Get2D(), &pdf,
			BxDFType(BSDF_TRANSMISSION | BSDF_SPECULAR));
		Spectrum L = Spectrum(0.f);
		if (pdf > 0.f && !f.IsBlack() && AbsDot(wi, ns) != 0.f) {
			// Compute ray differential _rd_ for specular transmission
			RayDifferential rd = isect.SpawnRay(wi);
			if (ray.hasDifferentials) {
				rd.hasDifferentials = true;
				rd.rxOrigin = p + isect.dpdx;
				rd.ryOrigin = p + isect.dpdy;

				Float eta = bsdf.eta;
				Vector3f w = -wo;
				if (Dot(wo, ns) < 0) eta = 1.f / eta;

				Normal3f dndx = isect.shading.dndu * isect.dudx +
					isect.shading.dndv * isect.dvdx;
				Normal3f dndy = isect.shading.dndu * isect.dudy +
					isect.shading.dndv * isect.dvdy;

				Vector3f dwodx = -ray.rxDirection - wo,
					dwody = -ray.ryDirection - wo;
				Float dDNdx = Dot(dwodx, ns) + Dot(wo, dndx);
				Float dDNdy = Dot(dwody, ns) + Dot(wo, dndy);

				Float mu = eta * Dot(w, ns) - Dot(wi, ns);
				Float dmudx =
					(eta - (eta * eta * Dot(w, ns)) / Dot(wi, ns)) * dDNdx;
				Float dmudy =
					(eta - (eta * eta * Dot(w, ns)) / Dot(wi, ns)) * dDNdy;

				rd.rxDirection =
					wi + eta * dwodx - Vector3f(mu * dndx + dmudx * ns);
				rd.ryDirection =
					wi + eta * dwody - Vector3f(mu * dndy + dmudy * ns);
			}
			L = f * Li(rd, scene, sampler, arena, depth + 1) * AbsDot(wi, ns) / pdf;
		}
		return L;
	}


	Float DammertzSamplerIntegrator::Error(const Bounds2i& b, const Bounds2i& tileBounds, const std::unique_ptr<FilmTile>& filmTileA, const std::unique_ptr<FilmTile>& filmTileB, int iter ) {
		Float error = 0.0f;
		for (Point2i pixel : b) {
			/*Float rgbA[3];
			Float rgbB[3];

	
			filmTileA->GetPixel(pixel).contribSum.ToRGB(rgbA);
			filmTileB->GetPixel(pixel).contribSum.ToRGB(rgbB);

			// Normalize pixel with weight sum
			Float filterWeightSumA = filmTileA->GetPixel(pixel).filterWeightSum;
			if (filterWeightSumA != 0) {
				Float invWt = (Float)1 / filterWeightSumA;
				rgbA[0] = std::max((Float)0, rgbA[0] * invWt);
				rgbA[1] = std::max((Float)0, rgbA[1] * invWt);
				rgbA[2] = std::max((Float)0, rgbA[2] * invWt);
			}

			Float filterWeightSumB = filmTileB->GetPixel(pixel).filterWeightSum;
			if (filterWeightSumB != 0) {
				Float invWt = (Float)1 / filterWeightSumB;
				rgbB[0] = std::max((Float)0, rgbB[0] * invWt);
				rgbB[1] = std::max((Float)0, rgbB[1] * invWt);
				rgbB[2] = std::max((Float)0, rgbB[2] * invWt);
			}
			
			ClampRGB(rgbA);
			ClampRGB(rgbB);

			Float A = abs(rgbA[0] - rgbB[0]) + abs(rgbA[1] - rgbB[1]) + abs(rgbA[1] - rgbB[1]);
			Float B = sqrt(rgbA[0] + rgbA[1] + rgbA[2]);
			Float D = A / B;
			
			//std::cout << "A: " << A << ", B: " << B << ", D: " << D << "\n";
			*/
			
			Float A = filmTileA->GetPixel(pixel).contribSum.y();
			if (filmTileA->GetPixel(pixel).filterWeightSum != 0)
				A /= filmTileA->GetPixel(pixel).filterWeightSum;
			Float B = filmTileB->GetPixel(pixel).contribSum.y();
			if (filmTileB->GetPixel(pixel).filterWeightSum!= 0)
				B /= filmTileB->GetPixel(pixel).filterWeightSum;
			Float C = sqrt(A);
			//Float D = (A - B) == 0.f ? 0.f : abs(A - B) / C;

			//Float D = abs(A - B) / C;
			//A = (int)Clamp(255.f * GammaCorrect(A) + 0.5f, 0.f, 255.f);
			//B = (int)Clamp(255.f * GammaCorrect(B) + 0.5f, 0.f, 255.f);


			//Float scale = 1 / 255.f;
			Float D = abs(A - B);
			//std::cout << b << " A: " << A << ", B: " << B << ", D: " << D << "\n";


			error += D;
		}

		Float scale = 1.f/(b.Area());
		//Float scale = sqrt((Float)b.Area() / (Float)tileBounds.Area()) / (Float)b.Area();
		//Float scale = ((Float)b.Area() / (Float)tileBounds.Area()) /(Float)b.Area();
		error *= scale;
		//std::cout << error << "\n\n";

		return error;
	}

	void DammertzSamplerIntegrator::Split(Bounds2i b, std::vector<Bounds2i>& V, const std::unique_ptr<FilmTile>& filmTileA, const std::unique_ptr<FilmTile>& filmTileB, Bounds2i t) {
		Bounds2i A, B;
		Float eA, eB;
		//std::cout << "\n\n" << b << "\n";

		int inc = 1;
		bool forward = true, cont = true, xAxis = (b.pMax.x - b.pMin.x >= b.pMax.y - b.pMin.y);
		Float min = Infinity, minEa, minEb;
		int minI = 0;
		
		int split;
		int incMax = (xAxis ? (b.pMax.x - b.pMin.x) : (b.pMax.y - b.pMin.y)) / inc;
		
		for (int i = 1; i < incMax; i++) {
			split = (xAxis ? b.pMin.x : b.pMin.y) + (i * inc);

			Bounds2i tempA, tempB;
			if (xAxis) {
				tempA = Bounds2i(b.pMin, Point2i(split, b.pMax.y));
				tempB = Bounds2i(Point2i(split, b.pMin.y), b.pMax);
			}
			else {
				tempA = Bounds2i(b.pMin, Point2i(b.pMax.x, split));
				tempB = Bounds2i(Point2i(b.pMin.x, split), b.pMax);
			}

			
			eA = Error(tempA, t, filmTileA, filmTileB) * ((Float)tempA.Area() / (Float)b.Area());
			eB = Error(tempB, t, filmTileA, filmTileB) * ((Float)tempB.Area() / (Float)b.Area());

			Float d = abs(eA - eB);
			//std::cout << split << ", " << eA << ", " << eB << ", " << d << "\n";

			if (d < min) {
				min = d;
				minI = i;
				minEa = eA;
				minEb = eB;
			}
		}

		split = (xAxis ? b.pMin.x : b.pMin.y) + (minI * inc);

		if (xAxis) {
			A = Bounds2i(b.pMin, Point2i(split, b.pMax.y));
			B = Bounds2i(Point2i(split, b.pMin.y), b.pMax);
		}
		else {
			A = Bounds2i(b.pMin, Point2i(b.pMax.x, split));
			B = Bounds2i(Point2i(b.pMin.x, split), b.pMax);
		}
		//std::cout << "\n";

		//std::cout << "\n\n" << b << "\n" << A << ",\t" << B << ":\t" << minEa << ", " << minEb << "\n";
		if (A.Area() != 0)
			V.push_back(A);
		if (B.Area() != 0)
			V.push_back(B);
		
	}

	bool operator>=(const Vector2i& A, const Vector2i& B) {
		return A.x > B.x && A.y > B.y;
	}

	void DammertzSamplerIntegrator::Render(const Scene &scene) {
		Preprocess(scene, *sampler);

		//const int maxIterations = 100;
		std::unique_ptr<Film> sFilm = std::make_unique<Film>(*(camera->film), std::make_unique<BoxFilter>(Vector2f(0.5f, 0.5f)), (camera->film->filename).substr(0, (camera->film->filename).size() - 4) + "_sVar.png");
		Float splitVar = minVar * 64.f;
		Float minError = minVar / 64.f;
		Float lastError = 0.f;
		int errorLevel = 0;
		int maxErrorLevel = 5;
		int maxIterations = 2048 / samplesPerIteration;
		//maxIterations = 2;
		// Render image tiles in parallel

		// Compute number of tiles, _nTiles_, to use for parallel rendering
		Bounds2i sampleBounds = camera->film->GetSampleBounds();
		Vector2i sampleExtent = sampleBounds.Diagonal();
		const int tileSize = 16;

		//Setup film tiles
		Bounds2i tileBounds(Point2i(sampleBounds.pMin.x, sampleBounds.pMin.y), Point2i(sampleBounds.pMax.x, sampleBounds.pMax.y));
		std::unique_ptr<FilmTile> filmTileA = camera->film->GetFilmTile(tileBounds);
		std::unique_ptr<FilmTile> filmTileB = camera->film->GetFilmTile(tileBounds);
		std::unique_ptr<FilmTile> heatMap = camera->film->GetFilmTile(tileBounds);

		std::vector<Bounds2i> filmTiles;
		filmTiles.push_back(tileBounds);
		Float numTiles = filmTiles.size();

		ProgressReporter reporter(tileBounds.Area(), "Rendering");
		int maxIt = 0, minIt = INT_MAX;
		int iter = 0;
		int totalArea = tileBounds.Area(), minArea = tileBounds.Area() / 50.f;;
		int seed = 0;
		while (filmTiles.size() > 0) { // && errorLevel < maxErrorLevel ) {// && iter < maxIterations) {
			
			// Build render tiles to multi thread early iterations
			std::vector<Bounds2i> renderTiles;
			for (Bounds2i b : filmTiles) {
				Point2i nTiles((b.pMax.x - b.pMin.x + tileSize -1) / tileSize, (b.pMax.y - b.pMin.y + tileSize - 1) / tileSize);
				for (int y = 0; y < nTiles.y; y++) {
					for (int x = 0; x < nTiles.x; x++) {
						int x0 = b.pMin.x + x * tileSize;
						int x1 = std::min(x0 + tileSize, b.pMax.x);
						int y0 = b.pMin.y + y * tileSize;
						int y1 = std::min(y0 + tileSize, b.pMax.y);
						Bounds2i bounds(Point2i(x0, y0), Point2i(x1, y1));

						renderTiles.push_back(bounds);
					}
				}
			}


			ParallelFor([&](int bucket) {
				MemoryArena arena;

				// Get sampler instance for tile
				std::unique_ptr<Sampler> tileSampler = sampler->Clone(seed + bucket);


				// Loop over pixels in tile to render them
				for (Point2i pixel : renderTiles[bucket]) {
					{
						ProfilePhase pp(Prof::StartPixel);
						tileSampler->StartPixel(pixel);
						tileSampler->SetSampleNumber(iter * samplesPerIteration);
					}

					// Do this check after the StartPixel() call; this keeps
					// the usage of RNG values from (most) Samplers that use
					// RNGs consistent, which improves reproducability /
					// debugging.
					if (!InsideExclusive(pixel, pixelBounds)) {
						continue;
					}

					uint64_t passSamples = 0;
					while (passSamples < samplesPerIteration) {
						// Initialize _CameraSample_ for current sample
						CameraSample cameraSample = tileSampler->GetCameraSample(pixel);

						// Generate camera ray for current sample
						RayDifferential ray;
						Float rayWeight = camera->GenerateRayDifferential(cameraSample, &ray);
						ray.ScaleDifferentials(1 / std::sqrt((Float)samplesPerIteration));
						//ray.ScaleDifferentials(1 / std::sqrt((Float)sampler->samplesPerPixel));
						++nCameraRays;

						// Evaluate radiance along camera ray
						Spectrum L(0.f);
						if (rayWeight > 0) L = Li(ray, scene, *tileSampler, arena);


						// Issue warning if unexpected radiance value returned
						//L_errors(L, pixel, *tileSampler);

						
						VLOG(1) << "Camera sample: " << cameraSample << " -> ray: " << ray << " -> L A = " << L;


						// Add camera ray's contribution to image
						if (passSamples % 2 == 0) {
							filmTileA->AddSample(cameraSample.pFilm, L, rayWeight);
						}
						else {
							filmTileB->AddSample(cameraSample.pFilm, L, rayWeight);
						}


						// Free _MemoryArena_ memory from computing image sample value
						arena.Reset();

						tileSampler->StartNextSample();
						passSamples++;
					}
				}
				LOG(INFO) << "Finished image tile " << renderTiles[bucket];
			}, renderTiles.size());
			seed += renderTiles.size();
			std::vector<Bounds2i> temp;
			Float totalError = 0.f;
			int oldTiles = 0, newTiles = 0, area = 0;
			
			Float invSize = (Float)1 / tileBounds.Area();

			totalArea = 0;

			for (Bounds2i b : filmTiles) {
				area += b.Area();

				if (iter == maxIterations) {
					if (iter > maxIt)
						maxIt = iter;
					if (iter < minIt)
						minIt = iter;
					for (Point2i pixel : b) {
						heatMap->GetPixel(pixel).contribSum = iter;
					}
					reporter.Update(b.Area());
				}
				else {

					Float error = Error(b, tileBounds, filmTileA, filmTileB, iter);

					totalError += error;// *b.Area() * invSize;

					if (error > splitVar || isNaN(error)) {
						temp.push_back(b);
						oldTiles++;
					}
					else if (error > minVar) {
						totalArea += b.Area();
						//if (b.pMax - b.pMin >= Vector2i(tileSize, tileSize))
						if (b.Area() > 1)
							//if (b.Area() > (2 * tileSize * tileSize))
							Split(b, temp, filmTileA, filmTileB, tileBounds);
						else {
							temp.push_back(b);
							oldTiles++;
						}
					}
					else {
						if (iter > maxIt)
							maxIt = iter;
						if (iter < minIt)
							minIt = iter;
						for (Point2i pixel : b) {
							heatMap->GetPixel(pixel).contribSum = iter;
						}
						reporter.Update(b.Area());
					}
				}
			}

			Float averageError = totalError / filmTiles.size();
			if (abs(lastError - averageError) < minError) {
				errorLevel++;
			}
			else
				errorLevel = 0;
			
			if (errorLevel >= maxErrorLevel) {
				maxIt -= maxErrorLevel;
				for (Bounds2i b : filmTiles) {
					for (Point2i pixel : b) {
						heatMap->GetPixel(pixel).contribSum = iter;
					}
				}
			}

			std::cout << "\nError var: " << abs(lastError - averageError) << ", Min Errer Var: " << minError;
			std::cout << "\nIteration: " << iter << ", New Tiles: " << temp.size() << ", Area: " << area << ", Average Error: " << averageError << ", Error Level: " << errorLevel << "\n";

			lastError = averageError;

			iter++;
			filmTiles = temp;
			Float numTiles = filmTiles.size();

		}
		reporter.Done();
		for (Point2i pixel : tileBounds) {
			heatMap->GetPixel(pixel).contribSum = pow(heatMap->GetPixel(pixel).contribSum.y() / maxIt, 1);
			heatMap->GetPixel(pixel).filterWeightSum = 1.f;
		}

		camera->film->MergeFilmTile(std::move(filmTileA));
		camera->film->MergeFilmTile(std::move(filmTileB));

		sFilm->MergeFilmTile(std::move(heatMap));

		// Save final image after rendering
		sFilm->WriteImage();
		camera->film->WriteImage();
	}





}