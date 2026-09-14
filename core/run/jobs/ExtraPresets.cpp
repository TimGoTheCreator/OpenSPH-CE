#include "run/jobs/ExtraPresets.h"
#include "io/FileManager.h"
#include "objects/containers/Array.h"
#include "run/jobs/GeometryJobs.h"
#include "run/jobs/InitialConditionJobs.h"
#include "run/jobs/IoJobs.h"
#include "run/jobs/MaterialJobs.h"
#include "run/jobs/ParticleJobs.h"
#include "run/jobs/SimulationJobs.h"
#include "sph/Materials.h"

NAMESPACE_SPH_BEGIN

static Array<CustomPresetDesc>& getPresetList() {
    static Array<CustomPresetDesc> sCustomPresets;
    return sCustomPresets;
}

ArrayView<const CustomPresetDesc> enumerateCustomPresets() {
    return getPresetList();
}

PresetRegistrar::PresetRegistrar(String name,
    String category,
    String tooltip,
    PresetFactoryFunc factory,
    bool isSphSim) {
    getPresetList().push(CustomPresetDesc{
        std::move(name),
        std::move(category),
        std::move(tooltip),
        std::move(factory),
        isSphSim,
    });
}

// =========================================================================================================
// DEFINE YOUR EXTRA PRESETS HERE
//
// Usage:
// REGISTER_PRESET(
//     "Display Name", 
//     "Category (optional, or \"\")", 
//     "Tooltip description", 
//     [](UniqueNameManager& nameMgr, const Size particleCnt) -> SharedPtr<JobNode> {
//         // Build your nodes and return the root simulation node
//     }, 
//     /* isSphSim = */ true
// );
// =========================================================================================================

// Example: Simple Basalt Sphere in Equilibrium
REGISTER_PRESET(
    "Example Asteroid",
    "examples",
    "A self-gravitating Basalt sphere in hydrostatic equilibrium (from ExtraPresets).",
    [](UniqueNameManager& nameMgr, const Size particleCnt) -> SharedPtr<JobNode> {
        SharedPtr<JobNode> sphere = makeNode<SphereJob>(nameMgr.getName("shape"));
        sphere->getSettings().set("radius", 500._f); // 500 km

        SharedPtr<JobNode> mat = makeNode<MaterialJob>(
            nameMgr.getName("basalt"), 
            getMaterial(MaterialEnum::BASALT)->getParams()
        );

        SharedPtr<JobNode> body = makeNode<MonolithicBodyIc>(nameMgr.getName("asteroid body"));
        body->getSettings().set(BodySettingsId::PARTICLE_COUNT, int(particleCnt));
        sphere->connect(body, "shape");
        mat->connect(body, "material");

        SharedPtr<JobNode> sim = makeNode<SphJob>(nameMgr.getName("simulation"));
        body->connect(sim, "particles");
        return sim;
    },
    true
);

NAMESPACE_SPH_END

