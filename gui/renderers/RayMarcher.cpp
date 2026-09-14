#include "gui/renderers/RayMarcher.h"
#include "gui/Factory.h"
#include "gui/objects/Bitmap.h"
#include "gui/objects/Camera.h"
#include "gui/objects/Colorizer.h"
#include "gui/renderers/FrameBuffer.h"
#include "objects/containers/FlatMap.h"
#include "objects/utility/OutputIterators.h"
#include "system/Profiler.h"

NAMESPACE_SPH_BEGIN

RayMarcher::RayMarcher(SharedPtr<IScheduler> scheduler, const GuiSettings& settings)
    : IRaytracer(scheduler, settings) {
    kernel = CubicSpline<3>();
    fixed.brdf = Factory::getBrdf(settings);
    fixed.renderSpheres = settings.get<bool>(GuiSettingsId::RAYTRACE_SPHERES);
    fixed.shadows = settings.get<bool>(GuiSettingsId::RAYTRACE_SHADOWS);
    fixed.smoothFactor = settings.get<Float>(GuiSettingsId::RAYTRACE_SMOOTH_FACTOR);
}

RayMarcher::~RayMarcher() = default;

constexpr Size BLEND_ALL_FLAG = 0x80;

void RayMarcher::initialize(const Storage& storage,
    const IColorizer& colorizer,
    const ICamera& UNUSED(camera)) {
    MEASURE_SCOPE("Building BVH");
    cached.r = storage.getValue<Vector>(QuantityId::POSITION).clone();
    const Size particleCnt = cached.r.size();

    if (storage.has(QuantityId::UVW)) {
        cached.uvws = storage.getValue<Vector>(QuantityId::UVW).clone();
    } else {
        cached.uvws.clear();
    }

    cached.flags.resize(particleCnt);
    if (storage.has(QuantityId::FLAG) && storage.has(QuantityId::STRESS_REDUCING)) {
        ArrayView<const Size> idxs = storage.getValue<Size>(QuantityId::FLAG);
        ArrayView<const Float> reduce = storage.getValue<Float>(QuantityId::STRESS_REDUCING);
        // avoid blending particles of different bodies, except if they are fully damaged
        for (Size i = 0; i < particleCnt; ++i) {
            cached.flags[i] = idxs[i];
            if (reduce[i] == 0._f) {
                cached.flags[i] |= BLEND_ALL_FLAG;
            }
        }
    } else {
        cached.flags.fill(0);
    }

    cached.materialIDs.resize(particleCnt);
    cached.materialIDs.fill(0);
    const bool loadTextures = cached.textures.empty();
    if (loadTextures) {
        cached.textures.resize(storage.getMaterialCnt());
    }
    FlatMap<String, SharedPtr<Texture>> textureMap;
    for (Size matId = 0; matId < storage.getMaterialCnt(); ++matId) {
        MaterialView body = storage.getMaterial(matId);
        for (Size i : body.sequence()) {
            cached.materialIDs[i] = matId;
        }

        String texturePath = body->getParams().getOr<String>(BodySettingsId::VISUALIZATION_TEXTURE, "");
        if (loadTextures && !texturePath.empty()) {
            if (textureMap.contains(texturePath)) {
                cached.textures[matId] = textureMap[texturePath];
            } else {
                SharedPtr<Texture> texture =
                    makeShared<Texture>(Path(texturePath), TextureFiltering::BILINEAR);
                textureMap.insert(texturePath, texture);
                cached.textures[matId] = texture;
            }
        }
    }

    cached.v.resize(particleCnt);
    constexpr Float VOLUME_MULT = 0.1_f;
    if (storage.has(QuantityId::MASS) && storage.has(QuantityId::DENSITY)) {
        ArrayView<const Float> rho, m;
        tie(rho, m) = storage.getValues<Float>(QuantityId::DENSITY, QuantityId::MASS);
        for (Size i = 0; i < particleCnt; ++i) {
            // avoid expanded particles - if the density is too low, use smoothing length instead
            cached.v[i] = min(VOLUME_MULT * sphereVolume(cached.r[i][H]), m[i] / rho[i]);
        }
    } else {
        for (Size i = 0; i < particleCnt; ++i) {
            cached.v[i] = VOLUME_MULT * sphereVolume(cached.r[i][H]);
        }
    }
    if (!fixed.renderSpheres) {
        for (Size i = 0; i < particleCnt; ++i) {
            cached.r[i][H] *= fixed.smoothFactor;
        }
    }

    cached.isGas.resize(particleCnt);
    cached.isGas.fill(false);
    cached.referenceRadii.resize(particleCnt);
    cached.distention.resize(particleCnt);
    cached.distention.fill(1.f);

    if (storage.has(QuantityId::MASS)) {
        ArrayView<const Float> m = storage.getValue<Float>(QuantityId::MASS);
        ArrayView<const Float> rho = storage.has(QuantityId::DENSITY)
                                         ? storage.getValue<Float>(QuantityId::DENSITY)
                                         : ArrayView<const Float>();
        ArrayView<const Float> u = storage.has(QuantityId::ENERGY)
                                       ? storage.getValue<Float>(QuantityId::ENERGY)
                                       : ArrayView<const Float>();

        if (storage.getMaterialCnt() > 0) {
            for (Size matId = 0; matId < storage.getMaterialCnt(); ++matId) {
                MaterialView body = storage.getMaterial(matId);
                const Float rho0 = body->getParams().has(BodySettingsId::DENSITY)
                                       ? body->getParam<Float>(BodySettingsId::DENSITY)
                                       : 1000._f;
                const EosEnum eos = body->getParams().getOr<EosEnum>(BodySettingsId::EOS, EosEnum::NONE);
                const bool isGasEos = (eos == EosEnum::IDEAL_GAS);
                const Float u_iv = body->getParams().getOr<Float>(BodySettingsId::TILLOTSON_ENERGY_IV, INFTY);

                for (Size i : body.sequence()) {
                    const Float volume0 = m[i] / rho0;
                    cached.referenceRadii[i] = root<3>(3._f * volume0 / (4._f * PI));
                    cached.distention[i] = max(1.f, float(cached.r[i][H] / cached.referenceRadii[i]));
                    if (isGasEos || cached.distention[i] > 2.0f) {
                        cached.isGas[i] = true;
                    }
                    if (!rho.empty() && rho[i] < 0.25_f * rho0) {
                        cached.isGas[i] = true;
                    }
                    if (!u.empty() && u[i] >= u_iv) {
                        cached.isGas[i] = true;
                    }
                }
            }
        } else {
            const Float rho0 = 1000._f;
            for (Size i = 0; i < m.size(); ++i) {
                const Float volume0 = m[i] / rho0;
                cached.referenceRadii[i] = root<3>(3._f * volume0 / (4._f * PI));
                cached.distention[i] = max(1.f, float(cached.r[i][H] / cached.referenceRadii[i]));
                if (cached.distention[i] > 2.0f) {
                    cached.isGas[i] = true;
                }
            }
        }
    }

    this->setColorizer(colorizer);

    cached.attractors.clear();
    cached.attractorTextures.clear();
    for (const Attractor& a : storage.getAttractors()) {
        if (!a.settings.getOr(AttractorSettingsId::VISIBLE, true)) {
            continue;
        }
        AttractorData ad;
        ad.position = a.position;
        ad.mass = a.mass;
        ad.radius = a.radius;
        ad.visible = true;
        ad.albedo = a.settings.getOr(AttractorSettingsId::ALBEDO, 1._f);
        cached.attractors.push(ad);

        const String path = a.settings.getOr<String>(AttractorSettingsId::VISUALIZATION_TEXTURE, "");
        if (!path.empty()) {
            SharedPtr<Texture> texture;
            if (textureMap.contains(path)) {
                texture = textureMap[path];
            } else {
                texture = makeShared<Texture>(Path(path), TextureFiltering::BILINEAR);
                textureMap.insert(path, texture);
            }
            cached.attractorTextures.push(texture);
        } else {
            cached.attractorTextures.push(nullptr);
        }
    }

    finder = Factory::getFinder(RunSettings::getDefaults());
    finder->build(*scheduler, cached.r);

    const float MAX_DISTENTION = 50.f;
    const Size MIN_NEIGHS = 8;

    ThreadLocal<Array<NeighborRecord>> neighs(*scheduler);
    Array<BvhSphere> spheres(particleCnt + cached.attractors.size());

    parallelFor(*scheduler, neighs, 0, particleCnt, [&](const Size i, Array<NeighborRecord>& local) {
        const float initialRadius = cached.r[i][H];
        float radius = initialRadius;
        if (cached.isGas[i]) {
            while (radius < MAX_DISTENTION * initialRadius) {
                finder->findAll(i, radius, local);
                if (local.size() >= MIN_NEIGHS) {
                    break;
                } else {
                    radius *= 1.5f;
                }
            }
        }
        BvhSphere s(cached.r[i], radius);
        s.userData = i;
        spheres[i] = s;
        cached.distention[i] = min(radius / initialRadius, MAX_DISTENTION);
    });

    for (Size i = 0; i < cached.attractors.size(); ++i) {
        BvhSphere s(cached.attractors[i].position, cached.attractors[i].radius);
        s.userData = particleCnt + i;
        spheres[particleCnt + i] = s;
    }
    bvh.build(std::move(spheres));

    for (ThreadData& data : threadData) {
        MarchData march;
        march.previousIdx = Size(-1);
        data.data = std::move(march);
    }

    shouldContinue = true;
}

bool RayMarcher::isInitialized() const {
    return !cached.r.empty();
}

void RayMarcher::setColorizer(const IColorizer& colorizer) {
    cached.doEmission = typeid(colorizer) == typeid(BeautyColorizer);
    cached.colors.resize(cached.r.size());
    for (Size i = 0; i < cached.r.size(); ++i) {
        cached.colors[i] = colorizer.evalColor(i);
        if (cached.doEmission) {
            cached.colors[i] = cached.colors[i] * colorizer.evalScalar(i).value();
        }
    }
}

Rgba RayMarcher::shade(const RenderParams& params, const CameraRay& cameraRay, ThreadData& data) const {
    const Vector dir = getNormalized(cameraRay.target - cameraRay.origin);
    const Ray ray(cameraRay.origin, dir);

    MarchData& march(data.data);
    Optional<Vector> hit = this->intersect(march, ray, params.surface.level, false);
    Array<IntersectionInfo> cameraIntersections;
    if (params.surface.renderGas && !cached.isGas.empty()) {
        cameraIntersections = march.intersections.clone();
    }

    Rgba result;
    Float hitDist = INFTY;
    if (hit) {
        hitDist = getLength(hit.value() - ray.origin());
        if (isAttractorHit(march.previousIdx)) {
            const Size attractorIndex = march.previousIdx - cached.r.size();
            result = this->getAttractorColor(march, params, attractorIndex, hit.value(), ray.direction());
        } else {
            result = this->getSurfaceColor(march, params, march.previousIdx, hit.value(), ray.direction());
        }
    } else {
        result = this->getEnviroColor(cameraRay);
    }

    if (params.surface.renderGas && !cached.isGas.empty()) {
        result = this->accumulateGas(params, ray, cameraIntersections, hitDist, result);
    }

    return result;
}

Rgba RayMarcher::accumulateGas(const RenderParams& params,
    const Ray& ray,
    ArrayView<const IntersectionInfo> intersections,
    const Float maxDist,
    Rgba baseColor) const {
    const float g = 0.5f; // Henyey-Greenstein asymmetry parameter
    const float cosTheta = dot(ray.direction(), -params.lighting.dirToSun);
    const float phase = (1.f - g * g) / pow(1.f + g * g - 2.f * g * cosTheta, 1.5f);

    Rgba result = baseColor;
    for (const IntersectionInfo& is : reverse(intersections)) {
        if (is.t >= maxDist) {
            continue;
        }
        const Size i = is.object->userData;
        if (i >= cached.r.size() || !cached.isGas[i]) {
            continue;
        }

        const BvhSphere* s = static_cast<const BvhSphere*>(is.object);
        const Vector hit = ray.origin() + ray.direction() * is.t;
        const Vector center = s->getCenter();
        const Vector toCenter = getNormalized(center - hit);

        const float cosPhi = abs(dot(toCenter, ray.direction()));
        const float distention = cached.distention[i];
        const float radiiFactor = cached.referenceRadii[i] / cached.r[i][H];

        // cosPhi is 1 at center-aiming rays, 0 at grazing ray (edge of bounding sphere)
        // Ensure falloff vanishes smoothly to 0 at the boundary (cosPhi == 0) to avoid hard clipped sphere
        // edges
        const float sinPhi2 = max(0.f, 1.f - cosPhi * cosPhi); // (impact_param / R)^2
        const float falloff = cosPhi * exp(-3.f * sinPhi2);
        const float secant = 2._f * getLength(center - hit) * cosPhi * radiiFactor;

        if (params.volume.absorption > 0.f) {
            result = result * exp(-params.volume.absorption * secant * falloff / distention);
        }

        const float emission = params.volume.emission;
        if (emission > 0.f) {
            float shadowFactor = 1.0f;
            if (fixed.shadows) {
                MarchData shadowData;
                shadowData.previousIdx = i;
                Ray shadowRay(hit - 1.e-4f * params.lighting.dirToSun, -params.lighting.dirToSun);

                // Check if shadowed by solid planet
                if (this->intersect(shadowData, shadowRay, params.surface.level, true)) {
                    shadowFactor = 0.05f; // Deep shadow
                } else {
                    // Accumulate gas optical depth towards sun
                    float gasOpticalDepth = 0.f;
                    for (const IntersectionInfo& shadowIs : shadowData.intersections) {
                        const Size si = shadowIs.object->userData;
                        if (si < cached.r.size() && cached.isGas[si]) {
                            const BvhSphere* ss = static_cast<const BvhSphere*>(shadowIs.object);
                            const Vector scenter = ss->getCenter();
                            const Vector shit = shadowRay.origin() + shadowRay.direction() * shadowIs.t;
                            const float scosPhi =
                                abs(dot(getNormalized(scenter - shit), shadowRay.direction()));
                            const float sdistention = cached.distention[si];
                            const float sradiiFactor = cached.referenceRadii[si] / cached.r[si][H];
                            const float ssinPhi2 = max(0.f, 1.f - scosPhi * scosPhi);
                            const float sfalloff = scosPhi * exp(-3.f * ssinPhi2);
                            const float ssecant = 2._f * getLength(scenter - shit) * scosPhi * sradiiFactor;
                            gasOpticalDepth += params.volume.absorption * ssecant * sfalloff / sdistention;
                        }
                    }
                    shadowFactor = exp(-gasOpticalDepth);
                }
            }

            const float magnitude = emission * (falloff / distention) * secant * shadowFactor * phase;
            result += cached.colors[i] * magnitude;
            result.a() += magnitude;
        }
    }
    result.a() = min(result.a(), 1.f);
    return result;
}

ArrayView<const Size> RayMarcher::getNeighborList(MarchData& data, const Size index) const {
    // look for neighbors only if the intersected particle differs from the previous one
    if (index != data.previousIdx) {
        Array<NeighborRecord> neighs;
        finder->findAll(index, kernel.radius() * cached.r[index][H], neighs);
        data.previousIdx = index;

        // find the actual list of neighbors
        data.neighs.clear();
        for (NeighborRecord& n : neighs) {
            const Size flag1 = cached.flags[index];
            const Size flag2 = cached.flags[n.index];
            if ((flag1 & BLEND_ALL_FLAG) || (flag2 & BLEND_ALL_FLAG) || (flag1 == flag2)) {
                data.neighs.push(n.index);
            }
        }
    }
    return data.neighs;
}

Optional<Vector> RayMarcher::intersect(MarchData& data,
    const Ray& ray,
    const Float surfaceLevel,
    const bool occlusion) const {
    data.intersections.clear();
    bvh.getAllIntersections(ray, backInserter(data.intersections));
    std::sort(data.intersections.begin(), data.intersections.end());

    for (const IntersectionInfo& intersect : data.intersections) {
        IntersectContext sc;
        sc.index = intersect.object->userData;
        sc.ray = ray;
        sc.t_min = intersect.t;
        sc.surfaceLevel = surfaceLevel;
        if (this->isAttractorHit(sc.index)) {
            data.previousIdx = sc.index;
            return sc.ray.origin() + sc.ray.direction() * intersect.t;
        }
        const Optional<Vector> hit = this->getSurfaceHit(data, sc, occlusion);
        if (hit) {
            return hit;
        }
        // rejected, process another intersection
    }
    return NOTHING;
}

bool RayMarcher::isAttractorHit(const Size index) const {
    return index >= cached.r.size();
}

Optional<Vector> RayMarcher::getSurfaceHit(MarchData& data,
    const IntersectContext& context,
    bool occlusion) const {
    if (fixed.renderSpheres) {
        data.previousIdx = context.index;
        return context.ray.origin() + context.ray.direction() * context.t_min;
    }

    this->getNeighborList(data, context.index);

    const Size i = context.index;
    const Ray& ray = context.ray;
    SPH_ASSERT(almostEqual(getSqrLength(ray.direction()), 1._f), getSqrLength(ray.direction()));
    Vector v1 = ray.origin() + ray.direction() * context.t_min;
    // the sphere hit should be always above the surface
    // SPH_ASSERT(this->evalField(data.neighs, v1) < 0._f);
    // look for the intersection up to hit + 4H; if we don't find it, we should reject the hit and look for
    // the next intersection - the surface can be non-convex!!
    const Float limit = 2._f * cached.r[i][H];
    // initial step - cannot be too large otherwise the ray could 'tunnel through' on grazing angles
    Float eps = 0.5_f * cached.r[i][H];
    Vector v2 = v1 + eps * ray.direction();

    Float phi = 0._f;
    Float travelled = eps;
    while (travelled < limit && eps > 0.2_f * cached.r[i][H]) {
        phi = this->evalField(data.neighs, v2) - context.surfaceLevel;
        if (phi > 0._f) {
            if (occlusion) {
                return v2;
            }
            // we crossed the surface, move back
            v2 = 0.5_f * (v1 + v2);
            eps *= 0.5_f;
            // since we crossed the surface, don't check for travelled distance anymore
            travelled = -INFTY;
        } else {
            // we are still above the surface, move further
            v1 = v2;
            v2 += eps * ray.direction();
            travelled += eps;
        }
    }

    if (travelled >= limit) {
        // didn't find surface, reject the hit
        return NOTHING;
    } else {
        return v2;
    }
}

Rgba RayMarcher::getAttractorColor(MarchData& data,
    const RenderParams& params,
    const Size index,
    const Vector& hit,
    const Vector& dir) const {
    const AttractorData& a = cached.attractors[index];
    Rgba diffuse = Rgba::gray(a.albedo);
    const SharedPtr<Texture>& texture = cached.attractorTextures[index];
    if (texture) {
        const Vector r0 = hit - a.position;
        SphericalCoords spherical = cartensianToSpherical(r0);
        Vector uvw = Vector(0.5_f - spherical.phi / (2._f * PI), spherical.theta / PI, 0._f);
        diffuse = texture->eval(uvw) * a.albedo;
    }

    const Vector n = getNormalized(a.position - hit);
    const Float cosPhi = dot(n, params.lighting.dirToSun);
    if (cosPhi <= 0._f) {
        // not illuminated -> just ambient light
        return diffuse * params.lighting.ambientLight;
    }

    // check for occlusion
    if (fixed.shadows) {
        Ray rayToSun(hit - 1.e-6_f * n * a.radius, -params.lighting.dirToSun);
        if (this->intersect(data, rayToSun, params.surface.level, true)) {
            // casted shadow
            return diffuse * params.lighting.ambientLight;
        }
    }

    // evaluate BRDF
    const Float f = fixed.brdf->transport(n, -dir, params.lighting.dirToSun);

    return diffuse * float(PI * f * cosPhi * params.lighting.sunLight + params.lighting.ambientLight);
}

Rgba RayMarcher::getSurfaceColor(MarchData& data,
    const RenderParams& params,
    const Size index,
    const Vector& hit,
    const Vector& dir) const {
    Rgba diffuse = Rgba::white();
    if (!cached.textures.empty() && !cached.uvws.empty()) {
        Size textureIdx = cached.materialIDs[index];
        SPH_ASSERT(textureIdx <= 10); // just sanity check, increase if necessary
        if (textureIdx >= cached.textures.size()) {
            textureIdx = 0;
        }
        if (cached.textures[textureIdx]) {
            const Vector uvw = this->evalUvws(data.neighs, hit);
            diffuse = cached.textures[textureIdx]->eval(uvw);
        }
    }

    // evaluate color before checking for occlusion as that invalidates the neighbor list
    const Rgba colorizerValue =
        fixed.renderSpheres ? cached.colors[index] : this->evalColor(data.neighs, hit);

    Rgba emission = Rgba::black();
    if (cached.doEmission) {
        emission = colorizerValue * params.surface.emission;
    } else {
        diffuse = diffuse * colorizerValue;
    }

    // compute the inward normal = gradient of the field
    const Vector n = fixed.renderSpheres ? cached.r[index] - hit : this->evalGradient(data.neighs, hit);
    SPH_ASSERT(n != Vector(0._f));
    const Vector n_norm = getNormalized(n);
    const Float cosPhi = dot(n_norm, params.lighting.dirToSun);
    if (cosPhi <= 0._f) {
        // not illuminated -> just ambient light + emission
        return diffuse * params.lighting.ambientLight + emission;
    }

    // check for occlusion
    if (fixed.shadows) {
        Ray rayToSun(hit - 0.5_f * n_norm * cached.r[index][H], -params.lighting.dirToSun);
        if (this->intersect(data, rayToSun, params.surface.level, true)) {
            // casted shadow
            return diffuse * params.lighting.ambientLight + emission;
        }
    }

    // evaluate BRDF
    const Float f = fixed.brdf->transport(n_norm, -dir, params.lighting.dirToSun);

    return diffuse * float(PI * f * cosPhi * params.lighting.sunLight + params.lighting.ambientLight) +
           emission;
}

Float RayMarcher::evalField(ArrayView<const Size> neighs, const Vector& pos1) const {
    SPH_ASSERT(!neighs.empty());
    Float value = 0._f;
    for (Size index : neighs) {
        const Vector& pos2 = cached.r[index];
        /// \todo could be optimized by using n.distSqr, no need to compute the dot again
        const Float w = kernel.value(pos1 - pos2, pos2[H]);
        value += cached.v[index] * w;
    }
    return value;
}

Vector RayMarcher::evalGradient(ArrayView<const Size> neighs, const Vector& pos1) const {
    Vector value(0._f);
    for (Size index : neighs) {
        const Vector& pos2 = cached.r[index];
        const Vector grad = kernel.grad(pos1 - pos2, pos2[H]);
        value += cached.v[index] * grad;
    }
    return value;
}

Rgba RayMarcher::evalColor(ArrayView<const Size> neighs, const Vector& pos1) const {
    SPH_ASSERT(!neighs.empty());
    Rgba color = Rgba::black();
    float weightSum = 0.f;
    for (Size index : neighs) {
        const Vector& pos2 = cached.r[index];
        /// \todo could be optimized by using n.distSqr, no need to compute the dot again
        const float w = float(kernel.value(pos1 - pos2, pos2[H]) * cached.v[index]);
        color += cached.colors[index] * w;
        weightSum += w;
    }
    SPH_ASSERT(weightSum != 0._f);
    return color / weightSum;
}

constexpr Float SEAM_WIDTH = 0.1_f;

Vector RayMarcher::evalUvws(ArrayView<const Size> neighs, const Vector& pos1) const {
    SPH_ASSERT(!neighs.empty());
    Vector uvws(0._f);
    Float weightSum = 0._f;
    int seamFlag = 0;
    for (Size index : neighs) {
        const Vector& pos2 = cached.r[index];
        const Float weight = kernel.value(pos1 - pos2, pos2[H]) * cached.v[index];
        uvws += cached.uvws[index] * weight;
        weightSum += weight;
        seamFlag |= cached.uvws[index][X] < SEAM_WIDTH ? 0x01 : 0;
        seamFlag |= cached.uvws[index][X] > 1._f - SEAM_WIDTH ? 0x02 : 0;
    }
    if (seamFlag & 0x03) {
        // we are near seam in u-coordinate, we cannot interpolate the UVWs directly
        uvws = Vector(0._f);
        weightSum = 0._f;
        for (Size index : neighs) {
            const Vector& pos2 = cached.r[index];
            /// \todo optimize - cache the kernel values
            const Float weight = kernel.value(pos1 - pos2, pos2[H]) * cached.v[index];
            Vector uvw = cached.uvws[index];
            // if near the seam, subtract 1 to make the u-mapping continuous
            uvw[X] -= (uvw[X] > 0.5_f) ? 1._f : 0._f;
            uvws += uvw * weight;
            weightSum += weight;
        }
        SPH_ASSERT(weightSum != 0._f);
        uvws /= weightSum;
        uvws[X] += (uvws[X] < 0._f) ? 1._f : 0._f;
        return uvws;
    } else {
        SPH_ASSERT(weightSum != 0._f);
        return uvws / weightSum;
    }
}

NAMESPACE_SPH_END

NAMESPACE_SPH_END
