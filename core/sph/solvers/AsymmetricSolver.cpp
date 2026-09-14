#include "sph/solvers/AsymmetricSolver.h"
#include "objects/Exceptions.h"
#include "objects/finders/NeighborFinder.h"
#include "quantities/IMaterial.h"
#include "sph/boundary/Boundary.h"
#include "sph/equations/Accumulated.h"
#include "sph/equations/HelperTerms.h"
#include "sph/kernel/Kernel.h"
#include "system/Factory.h"
#include "system/Statistics.h"

NAMESPACE_SPH_BEGIN

void RadiiHashMap::build(ArrayView<const Vector> r, const Float kernelRadius) {
    cellSize = 0._f;
    for (Size i = 0; i < r.size(); ++i) {
        cellSize = max(cellSize, r[i][H] * kernelRadius);
    }
    if (cellSize <= 0._f || r.empty()) {
        map.clear();
        return;
    }

    std::unordered_map<Indices, Float, std::hash<Indices>, IndicesEqual> newMap;
    newMap.reserve(r.size());
    for (Size i = 0; i < r.size(); ++i) {
        // floor needed to properly handle negative values
        const Indices idxs = floor(r[i] / cellSize);
        Float& radius = newMap[idxs];
        radius = max(radius, r[i][H] * kernelRadius);
    }

    // create map by dilating newMap
    map.clear();
    map.reserve(newMap.size());
    for (const auto& p : newMap) {
        const Indices& idxs0 = p.first;
        Float radius = p.second;
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                for (int k = -1; k <= 1; ++k) {
                    const Indices idxs = idxs0 + Indices(i, j, k);
                    auto iter = newMap.find(idxs);
                    if (iter != newMap.end()) {
                        radius = max(radius, iter->second);
                    }
                }
            }
        }
        map[idxs0] = radius;
    }
}

Float RadiiHashMap::getRadius(const Vector& r) const {
    if (cellSize <= 0._f || map.empty()) {
        return 0._f;
    }
    const Indices idxs = floor(r / cellSize);
    Float radius = 0._f;
    const auto iter = map.find(idxs);
    if (iter != map.end()) {
        radius = max(radius, iter->second);
    }
    return radius;
}

IAsymmetricSolver::IAsymmetricSolver(IScheduler& scheduler,
    const RunSettings& settings,
    const EquationHolder& eqs)
    : scheduler(scheduler) {
    kernel = Factory::getKernel<DIMENSIONS>(settings);
    finder = Factory::getFinder(settings);
    equations += eqs;

    if (settings.get<bool>(RunSettingsId::SPH_ASYMMETRIC_COMPUTE_RADII_HASH_MAP)) {
        radiiMap.emplace();
    }
}

void IAsymmetricSolver::integrate(Storage& storage, Statistics& stats) {
    VERBOSE_LOG

    // initialize all materials (compute pressure, apply yielding and damage, ...)
    for (Size i = 0; i < storage.getMaterialCnt(); ++i) {
        PROFILE_SCOPE("IAsymmetricSolver initialize materials")
        MaterialView material = storage.getMaterial(i);
        material->initialize(scheduler, storage, material.sequence());
    }

    // initialize equations, derivatives, accumulate storages, ...
    this->beforeLoop(storage, stats);

    // main loop over pairs of interacting particles
    this->loop(storage, stats);

    // store results to storage, finalizes equations, save statistics, ...
    this->afterLoop(storage, stats);

    // finalize all materials (integrate fragmentation model)
    for (Size i = 0; i < storage.getMaterialCnt(); ++i) {
        PROFILE_SCOPE("IAsymmetricSolver finalize materials")
        MaterialView material = storage.getMaterial(i);
        material->finalize(scheduler, storage, material.sequence());
    }
}

void IAsymmetricSolver::create(Storage& storage, IMaterial& material) const {
    storage.insert<Size>(QuantityId::NEIGHBOR_CNT, OrderEnum::ZERO, 0);
    equations.create(storage, material);
    this->sanityCheck(storage);
}

Float IAsymmetricSolver::getMaxSearchRadius(const Storage& storage) const {
    ArrayView<const Vector> r = storage.getValue<Vector>(QuantityId::POSITION);
    Float maxH = 0._f;
    for (Size i = 0; i < r.size(); ++i) {
        maxH = max(maxH, r[i][H]);
    }
    return maxH * kernel.radius();
}

RawPtr<const IBasicFinder> IAsymmetricSolver::getFinder(ArrayView<const Vector> r) {
    VERBOSE_LOG
    finder->build(scheduler, r);
    return &*finder;
}

AsymmetricSolver::AsymmetricSolver(IScheduler& scheduler,
    const RunSettings& settings,
    const EquationHolder& eqs)
    : AsymmetricSolver(scheduler, settings, eqs, Factory::getBoundaryConditions(settings)) {}

AsymmetricSolver::AsymmetricSolver(IScheduler& scheduler,
    const RunSettings& settings,
    const EquationHolder& eqs,
    AutoPtr<IBoundaryCondition>&& bc)
    : IAsymmetricSolver(scheduler, settings, eqs)
    , bc(std::move(bc))
    , threadData(scheduler) {

    // creates all derivatives required by the equation terms
    equations.setDerivatives(derivatives, settings);
}

AsymmetricSolver::~AsymmetricSolver() = default;


void AsymmetricSolver::beforeLoop(Storage& storage, Statistics& stats) {
    VERBOSE_LOG

    // initialize boundary conditions first, as they may change the number of particles (ghosts, killbox, ...)
    bc->initialize(storage);

    // initialize all equation terms (applies dependencies between quantities)
    const Float t = stats.getOr<Float>(StatisticsId::RUN_TIME, 0._f);
    equations.initialize(scheduler, storage, t);

    // sets up references to storage buffers for all derivatives
    derivatives.initialize(scheduler, storage);
}

void AsymmetricSolver::loop(Storage& storage, Statistics& UNUSED(stats)) {
    VERBOSE_LOG

    // (re)build neighbor-finding structure; this needs to be done after all equations
    // are initialized in case some of them modify smoothing lengths
    ArrayView<Vector> r = storage.getValue<Vector>(QuantityId::POSITION);
    const IBasicFinder& actFinder = *this->getFinder(r);

    // precompute the search radii
    Float maxRadius = 0._f;
    if (radiiMap) {
        radiiMap->build(r, kernel.radius());
    } else {
        maxRadius = this->getMaxSearchRadius(storage);
    }

    ArrayView<Size> neighs = storage.getValue<Size>(QuantityId::NEIGHBOR_CNT);

    // Compute spatial Morton traversal order for cache locality without modifying Storage buffers
    Box box;
    for (Size i = 0; i < r.size(); ++i) {
        box.extend(r[i]);
    }
    const Vector minP = box.lower();
    const Vector rangeP = box.upper() - minP;
    const Vector invRange(rangeP[X] > 1e-12_f ? 1023.0_f / rangeP[X] : 0.0_f,
        rangeP[Y] > 1e-12_f ? 1023.0_f / rangeP[Y] : 0.0_f,
        rangeP[Z] > 1e-12_f ? 1023.0_f / rangeP[Z] : 0.0_f);

    auto expandBits = [](uint32_t v) -> uint32_t {
        // what the fuck?
        v = (v * 0x00010001u) & 0xFF0000FFu;
        v = (v * 0x00000101u) & 0x0F00F00Fu;
        v = (v * 0x00000011u) & 0xC30C30C3u;
        v = (v * 0x00000005u) & 0x49249249u;
        return v;
    };
    auto morton3D = [&expandBits](uint32_t x, uint32_t y, uint32_t z) -> uint32_t {
        return (expandBits(x) << 2) | (expandBits(y) << 1) | expandBits(z);
    };

    Array<Size> traversalOrder(r.size());
    Array<uint32_t> mortonKeys(r.size());
    for (Size i = 0; i < r.size(); ++i) {
        traversalOrder[i] = i;
        const Vector normP = r[i] - minP;
        const uint32_t x = clamp(int(normP[X] * invRange[X]), 0, 1023);
        const uint32_t y = clamp(int(normP[Y] * invRange[Y]), 0, 1023);
        const uint32_t z = clamp(int(normP[Z] * invRange[Z]), 0, 1023);
        mortonKeys[i] = morton3D(x, y, z);
    }

    std::sort(traversalOrder.begin(), traversalOrder.end(), [&mortonKeys](Size a, Size b) {
        return mortonKeys[a] < mortonKeys[b];
    });

    const Float kernelRadius = kernel.radius();
    const Float halfKernelRadius = 0.5_f * kernelRadius;

    auto functor = [this, r, &neighs, maxRadius, kernelRadius, halfKernelRadius, &actFinder, &traversalOrder](
                       Size k, ThreadData& data) {
        const Size i = traversalOrder[k];
        Float neighborRadius = radiiMap ? radiiMap->getRadius(r[i]) : maxRadius;
        if (neighborRadius <= 0._f) {
            neighborRadius = maxRadius;
        }
        SPH_ASSERT(neighborRadius > 0._f);

        // max possible value of kernel.radius() * hbar
        const Float radius = 0.5_f * (r[i][H] * kernelRadius + neighborRadius);

        actFinder.findAll(i, radius, data.neighs);
        data.grads.clear();
        data.idxs.clear();
        const Size neighCount = data.neighs.size();
        if (data.grads.capacity() < neighCount) {
            data.grads.reserve(neighCount);
            data.idxs.reserve(neighCount);
        }

        const Vector ri = r[i];
        const Float hi = ri[H];
        const Float hiHalfKR = hi * halfKernelRadius;

        for (const auto& n : data.neighs) {
            const Size j = n.index;
            if (i == j) {
                continue;
            }
            const Float hj = r[j][H];
            const Float cutoff = hiHalfKR + hj * halfKernelRadius;
            if (n.distanceSqr >= sqr(cutoff)) {
                continue;
            }
            const Float hbar = 0.5_f * (hi + hj);
            const Float hInv = 1._f / hbar;
            const Vector dr = ri - r[j];
            const Vector gr = kernel.gradPrecomputed(dr, n.distanceSqr, hInv);
            SPH_ASSERT(isReal(gr) && dot(gr, dr) <= 0._f, gr, dr);
            data.grads.emplaceBack(gr);
            data.idxs.emplaceBack(j);
        }
        derivatives.eval(i, data.idxs, data.grads);
        neighs[i] = data.idxs.size();
    };
    parallelFor(scheduler, threadData, 0, r.size(), functor);
}

void AsymmetricSolver::afterLoop(Storage& storage, Statistics& stats) {
    VERBOSE_LOG

    // store the computed values into the storage
    Accumulated& accumulated = derivatives.getAccumulated();
    accumulated.store(scheduler, storage);

    // using the stored values, integrates all equation terms
    const Float t = stats.getOr<Float>(StatisticsId::RUN_TIME, 0._f);
    equations.finalize(scheduler, storage, t);

    // lastly, finalize boundary conditions, to make sure the computed quantities will not change any further
    bc->finalize(storage);

    // compute neighbor statistics
    ArrayView<Size> neighs = storage.getValue<Size>(QuantityId::NEIGHBOR_CNT);
    MinMaxMean neighsStats;
    const Size size = storage.getParticleCnt();
    for (Size i = 0; i < size; ++i) {
        neighsStats.accumulate(neighs[i]);
    }
    stats.set(StatisticsId::NEIGHBOR_COUNT, neighsStats);
}

void AsymmetricSolver::sanityCheck(const Storage& UNUSED(storage)) const {
    // we must solve smoothing length somehow
    if (!equations.contains<AdaptiveSmoothingLength>() && !equations.contains<ConstSmoothingLength>()) {
        throw InvalidSetup(
            "No solver of smoothing length specified; add either ConstSmootingLength or "
            "AdaptiveSmootingLength into the list of equations");
    }

    // we allow both velocity divergence and density velocity divergence as the former can be used by some
    // terms (e.g. Balsara switch) even in Standard formulation
}

NAMESPACE_SPH_END
