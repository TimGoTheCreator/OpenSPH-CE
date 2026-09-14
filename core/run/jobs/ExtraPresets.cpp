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

// REGISTER_PRESET(
//     "Display Name", 
//     "Category (optional, or \"\")", 
//     "Tooltip description", 
//     [](UniqueNameManager& nameMgr, const Size particleCnt) -> SharedPtr<JobNode> {
//         // stuff
//     }, 
//     /* isSphSim = */ true
// );


NAMESPACE_SPH_END

