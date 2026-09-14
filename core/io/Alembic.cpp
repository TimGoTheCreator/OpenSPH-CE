#include "io/Alembic.h"
#include "objects/geometry/Box.h"
#include "quantities/Quantity.h"
#include "quantities/Storage.h"
#include "system/Statistics.h"

#ifdef SPH_USE_ALEMBIC
#include <Alembic/Abc/All.h>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>

using namespace Alembic::Abc;
using namespace Alembic::AbcGeom;
#endif

NAMESPACE_SPH_BEGIN

AlembicOutput::AlembicOutput(const OutputFile& fileMask, const Float scale)
    : IOutput(fileMask)
    , scale(scale) {}

AlembicOutput::~AlembicOutput() = default;

Expected<Path> AlembicOutput::dump(const Storage& storage, const Statistics& stats) {
#ifdef SPH_USE_ALEMBIC
    try {
        Path abcPath = paths.getNextPath(stats);
        std::string filename = abcPath.string().toUtf8().cstr();

        OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), filename);
        OObject top = archive.getTop();

        // Create a points schema
        OPoints pointsObj(top, "particles");
        OPointsSchema& pointsSchema = pointsObj.getSchema();

        ArrayView<const Vector> r = storage.getValue<Vector>(QuantityId::POSITION);
        const Size numParticles = r.size();

        std::vector<Imath::V3f> positions(numParticles);
        std::vector<uint64_t> ids(numParticles);
        std::vector<float> widths(numParticles);

        for (Size i = 0; i < numParticles; ++i) {
            positions[i] = Imath::V3f(float(r[i][X] * scale), float(r[i][Y] * scale), float(r[i][Z] * scale));
            ids[i] = i;
            widths[i] = 2.0f * float(r[i][H] * scale); // Diameter based on smoothing length
        }

        OPointsSchema::Sample sample;
        sample.setPositions(V3fArraySample(positions));
        sample.setIds(UInt64ArraySample(ids));
        sample.setWidths(OFloatGeomParam::Sample(FloatArraySample(widths), GeometryScope::kVertexScope));

        // Include velocities if first or second order
        const Quantity& posQuantity = storage.getQuantity(QuantityId::POSITION);
        if (posQuantity.getOrderEnum() != OrderEnum::ZERO) {
            ArrayView<const Vector> v = storage.getDt<Vector>(QuantityId::POSITION);
            std::vector<Imath::V3f> velocities(numParticles);
            for (Size i = 0; i < numParticles; ++i) {
                velocities[i] = Imath::V3f(float(v[i][X] * scale), float(v[i][Y] * scale), float(v[i][Z] * scale));
            }
            sample.setVelocities(V3fArraySample(velocities));
        }

        pointsSchema.set(sample);

        // Export UV coordinates if available (standard for Houdini / Blender / Maya)
        if (storage.has(QuantityId::UVW)) {
            ArrayView<const Vector> uvws = storage.getValue<Vector>(QuantityId::UVW);
            std::vector<Imath::V2f> uvs(numParticles);
            for (Size i = 0; i < numParticles; ++i) {
                uvs[i] = Imath::V2f(float(uvws[i][X]), float(uvws[i][Y]));
            }
            OV2fGeomParam uvParam(pointsSchema, "uv", false, GeometryScope::kVertexScope, 1);
            OV2fGeomParam::Sample uvSample(V2fArraySample(uvs), GeometryScope::kVertexScope);
            uvParam.set(uvSample);
        }

        return abcPath;
    } catch (const std::exception& e) {
        return makeUnexpected<Path>("Failed to save Alembic file.\n{}", e.what());
    }
#else
    return makeUnexpected<Path>("OpenSPH was compiled without Alembic support.");
#endif
}

Outcome AlembicInput::load(const Path& path, Storage& storage, Statistics& UNUSED(stats)) {
#ifdef SPH_USE_ALEMBIC
    try {
        storage.removeAll();
        std::string filename = path.string().toUtf8().cstr();
        IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), filename);
        IObject top = archive.getTop();

        // Find the first IPoints object in the hierarchy
        IPoints pointsObj;
        for (size_t i = 0; i < top.getNumChildren(); ++i) {
            const ObjectHeader& header = top.getChildHeader(i);
            if (IPoints::matches(header)) {
                pointsObj = IPoints(top, header.getName());
                break;
            }
        }

        if (!pointsObj.valid()) {
            return makeFailed("No valid points object found in Alembic file '{}'.", path.string());
        }

        IPointsSchema pointsSchema = pointsObj.getSchema();
        IPointsSchema::Sample sample;
        pointsSchema.get(sample);

        P3fArraySamplePtr positionsPtr = sample.getPositions();
        if (!positionsPtr || positionsPtr->size() == 0) {
            return makeFailed("Alembic file '{}' contains no points.", path.string());
        }

        const Size numParticles = positionsPtr->size();
        Array<Vector> r(numParticles);
        for (Size i = 0; i < numParticles; ++i) {
            const Imath::V3f& p = (*positionsPtr)[i];
            r[i] = Vector(p.x, p.y, p.z);
        }

        // Check if widths are available to recover smoothing length
        IFloatGeomParam floatWidthsParam = pointsSchema.getWidthsParam();
        if (floatWidthsParam.valid()) {
            FloatArraySamplePtr widths = floatWidthsParam.getExpandedValue().getVals();
            if (widths && widths->size() == numParticles) {
                for (Size i = 0; i < numParticles; ++i) {
                    r[i][H] = (*widths)[i] * 0.5_f;
                }
            } else {
                for (Size i = 0; i < numParticles; ++i) {
                    r[i][H] = 1.0_f;
                }
            }
        } else {
            for (Size i = 0; i < numParticles; ++i) {
                r[i][H] = 1.0_f;
            }
        }

        // Store positions
        storage.insert<Vector>(QuantityId::POSITION, OrderEnum::FIRST, std::move(r));

        // Read velocities if available
        V3fArraySamplePtr vPtr = sample.getVelocities();
        if (vPtr && vPtr->size() == numParticles) {
            Array<Vector>& v = storage.getDt<Vector>(QuantityId::POSITION);
            for (Size i = 0; i < numParticles; ++i) {
                const Imath::V3f& vel = (*vPtr)[i];
                v[i] = Vector(vel.x, vel.y, vel.z);
            }
        }

        // Assign basic default mass and density so SPH solver can run on loaded points
        Array<Float> m(numParticles);
        Array<Float> rho(numParticles);
        m.fill(1.e3_f);
        rho.fill(1000._f);
        storage.insert<Float>(QuantityId::MASS, OrderEnum::ZERO, std::move(m));
        storage.insert<Float>(QuantityId::DENSITY, OrderEnum::ZERO, std::move(rho));

        // Read UV coordinates if available
        try {
            IV2fGeomParam uvParam(pointsSchema, "uv");
            if (uvParam.valid()) {
                V2fArraySamplePtr uvPtr = uvParam.getExpandedValue().getVals();
                if (uvPtr && uvPtr->size() == numParticles) {
                    Array<Vector> uvws(numParticles);
                    for (Size i = 0; i < numParticles; ++i) {
                        const Imath::V2f& uv = (*uvPtr)[i];
                        uvws[i] = Vector(uv.x, uv.y, 0.0_f);
                    }
                    storage.insert<Vector>(QuantityId::UVW, OrderEnum::ZERO, std::move(uvws));
                }
            }
        } catch (...) {
            // "uv" GeomParam not present in this Alembic file, continue safely
        }

        return SUCCESS;
    } catch (const std::exception& e) {
        return makeFailed("Cannot read Alembic file '{}'.\n{}", path.string(), e.what());
    }
#else
    return makeFailed("OpenSPH was compiled without Alembic support.");
#endif
}

NAMESPACE_SPH_END
