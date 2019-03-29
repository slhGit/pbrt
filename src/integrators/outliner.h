#include "integrator.h"

namespace pbrt {
	// SamplerIntegrator Declarations
	class OutlineIntegrator : public Integrator {
	public:
		// SamplerIntegrator Public Methods
		OutlineIntegrator(std::shared_ptr<const Camera> camera,
			std::shared_ptr<Sampler> sampler,
			const Bounds2i &pixelBounds)
			: camera(camera), sampler(sampler), pixelBounds(pixelBounds) {}
				
		void Render(const Scene &scene);

	protected:
		// SamplerIntegrator Protected Data
		std::shared_ptr<const Camera> camera;

	private:
		// SamplerIntegrator Private Data
		std::shared_ptr<Sampler> sampler;
		const Bounds2i pixelBounds;
	};

	OutlineIntegrator *CreateOutlineIntegrator(const ParamSet &params,
		std::shared_ptr<Sampler> sampler,
		std::shared_ptr<const Camera> camera);
}
