#define NOMINMAX
#pragma once

#ifndef PBRT_CORE_CAMERA_H
#define PBRT_CORE_CAMERA_H

#include "pbrt.h"
#include "geometry.h"
#include "transform.h"
#include "film.h"

namespace pbrt {
	// Camera Declarations
	class Camera {
	  public:
		// Camera Interface
		Camera(const AnimatedTransform &, Float, Float, Film *, const Medium *);
		virtual ~Camera();
    
		virtual Float GenerateRay(const CameraSample &, Ray *) const = 0;
		virtual Float GenerateRayDifferential(const CameraSample &, RayDifferential *) const;
    
		virtual Spectrum We(const Ray &, Point2f * = nullptr) const;
		virtual void Pdf_We(const Ray &, Float *, Float *) const;
    
		virtual Spectrum Sample_Wi(const Interaction &, const Point2f &, Vector3f *, Float *, Point2f *, VisibilityTester *) const;

		// Camera Public Data
		AnimatedTransform CameraToWorld;
		const Float shutterOpen, shutterClose;
		Film *film;
		const Medium *medium;
	};

	struct CameraSample {
		Point2f pFilm;
		Point2f pLens;
		Float time;
	};

	inline std::ostream &operator<<(std::ostream &os, const CameraSample &cs) {
		os << "[ pFilm: " << cs.pFilm << " , pLens: " << cs.pLens << StringPrintf(", time %f ]", cs.time);
		return os;
	}

	class ProjectiveCamera : public Camera {
	  public:
		// ProjectiveCamera Public Methods
		ProjectiveCamera(const AnimatedTransform &CameraToWorld, const Transform &CameraToScreen, const Bounds2f &screenWindow, Float shutterOpen,
			Float shutterClose, Float lensr, Float focald, Film *film, const Medium *medium)
			: Camera(CameraToWorld, shutterOpen, shutterClose, film, medium), CameraToScreen(CameraToScreen) 
		{
			// Initialize depth of field parameters
			lensRadius = lensr;
			focalDistance = focald;

			// Compute projective camera transformations

			// Compute projective camera screen transformations
			ScreenToRaster = Scale(film->fullResolution.x, film->fullResolution.y, 1) * Scale(1 / (screenWindow.pMax.x - screenWindow.pMin.x),
				1 / (screenWindow.pMin.y - screenWindow.pMax.y), 1) * Translate(Vector3f(-screenWindow.pMin.x, -screenWindow.pMax.y, 0));
        
			RasterToScreen = Inverse(ScreenToRaster);
			RasterToCamera = Inverse(CameraToScreen) * RasterToScreen;
		}

	  protected:
		// ProjectiveCamera Protected Data
		Transform CameraToScreen, RasterToCamera;
		Transform ScreenToRaster, RasterToScreen;
		Float lensRadius, focalDistance;
	};
}  

#endif  
