#include "ParticleGenerator.h"
#include "SDF.h"
#include "Mesh.h"

#include "core/Common.h"
#include "core/Vector.h"
#include "core/Box.h"

#include <pcg32.h>

#include <Eigen/Geometry>

namespace pbs {

ParticleGenerator::Result ParticleGenerator::generateSurfaceParticles(const Box3f &box, float particleRadius) {
    Vector3f origin = box.min;
    Vector3f extents = box.extents();

    int nx = std::ceil(extents.x() / (2.f * particleRadius));
    int ny = std::ceil(extents.y() / (2.f * particleRadius));
    int nz = std::ceil(extents.z() / (2.f * particleRadius));
    Vector3f d = extents.cwiseQuotient(Vector3f(nx, ny, nz));

    float normalScale = -1.f;

    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    auto addParticle = [&positions, &normals, &origin, &d, &normalScale] (int x, int y, int z, const Vector3f &n) {
        positions.emplace_back(origin + Vector3f(x, y, z).cwiseProduct(d));
        normals.emplace_back(n * normalScale);
    };

    // XY planes
    for (int x = 1; x < nx; ++x) {
        for (int y = 1; y < ny; ++y) {
            addParticle(x, y, 0, Vector3f(0.f, 0.f, 1.f));
            addParticle(x, y, nz, Vector3f(0.f, 0.f, -1.f));
        }
    }
    // XZ planes
    for (int x = 1; x < nx; ++x) {
        for (int z = 1; z < nz; ++z) {
            addParticle(x, 0, z, Vector3f(0.f, 1.f, 0.f));
            addParticle(x, ny, z, Vector3f(0.f, -1.f, 0.f));
        }
    }
    // YZ planes
    for (int y = 1; y < ny; ++y) {
        for (int z = 1; z < nz; ++z) {
            addParticle(0, y, z, Vector3f(1.f, 0.f, 0.f));
            addParticle(nx, y, z, Vector3f(-1.f, 0.f, 0.f));
        }
    }
    // X borders
    for (int x = 1; x < nx; ++x) {
        addParticle(x , 0 , 0 , Vector3f( 0.f,  1.f,  1.f).normalized());
        addParticle(x , ny, 0 , Vector3f( 0.f, -1.f,  1.f).normalized());
        addParticle(x , 0 , nz, Vector3f( 0.f,  1.f, -1.f).normalized());
        addParticle(x , ny, nz, Vector3f( 0.f, -1.f, -1.f).normalized());
    }
    // Y borders
    for (int y = 1; y < ny; ++y) {
        addParticle(0 , y , 0 , Vector3f( 1.f,  0.f,  1.f).normalized());
        addParticle(nx, y , 0 , Vector3f(-1.f,  0.f,  1.f).normalized());
        addParticle(0 , y , nz, Vector3f( 1.f,  0.f, -1.f).normalized());
        addParticle(nx, y , nz, Vector3f(-1.f,  0.f, -1.f).normalized());
    }
    // Z borders
    for (int z = 1; z < nz; ++z) {
        addParticle(0 , 0 , z , Vector3f( 1.f,  1.f,  0.f).normalized());
        addParticle(nx, 0 , z , Vector3f(-1.f,  1.f,  0.f).normalized());
        addParticle(0 , ny, z , Vector3f( 1.f, -1.f,  0.f).normalized());
        addParticle(nx, ny, z , Vector3f(-1.f, -1.f,  0.f).normalized());
    }
    // Corners
    for (int c = 0; c < 8; ++c) {
        int x = (c     ) & 1;
        int y = (c >> 1) & 1;
        int z = (c >> 2) & 1;
        addParticle(x ? 0 : nx, y ? 0 : ny, z ? 0 : nz, Vector3f(x ? 1.f : -1.f, y ? 1.f : -1.f, z ? 1.f : -1.f).normalized());
    }


    return Result({ std::move(positions), std::move(normals) });
}

ParticleGenerator::Result ParticleGenerator::generateSurfaceParticles(const Mesh &mesh, float particleRadius, int cells) {

    float density = 1.f / (M_PI * sqr(particleRadius));
    DBG("density = %f", density);

    DBG("Generating surface particles ...");

    // Compute bounds of mesh and expand by 10%
    Box3f bounds = mesh.computeBounds();
    bounds = bounds.expanded(bounds.extents() * 0.1f);

    // Compute cell and grid size for signed distance field
    float cellSize = bounds.extents()[bounds.majorAxis()] / cells;
    Vector3i size(
        int(std::ceil(bounds.extents().x() / cellSize)),
        int(std::ceil(bounds.extents().y() / cellSize)),
        int(std::ceil(bounds.extents().z() / cellSize))
    );

    VoxelGrid<float> sdf(size);
    sdf.setOrigin(bounds.min);
    sdf.setCellSize(cellSize);

    // Build signed distance field
    DBG("Building signed distance field ...");
    SDF::build(mesh, sdf);

    // Generate initial point distribution
    DBG("Generating initial point distribution ...");
    pcg32 rng;
    std::vector<Vector3f> positions;
    float totalArea = 0.f;
    for (int i = 0; i < mesh.triangles().cols(); ++i) {
        const Vector3f &p0 = mesh.vertices().col(mesh.triangles()(0, i));
        const Vector3f &p1 = mesh.vertices().col(mesh.triangles()(1, i));
        const Vector3f &p2 = mesh.vertices().col(mesh.triangles()(2, i));
        Vector3f e0 = p1 - p0;
        Vector3f e1 = p2 - p0;
        float area = 0.5f * std::abs(e0.cross(e1).norm());
        totalArea += area;

        float n = density * area;
        int ni = int(std::floor(n));

        auto samplePoint = [&] () {
            float s = rng.nextFloat();
            float t = rng.nextFloat();
            float ss = std::sqrt(s);
            positions.emplace_back(p0 + e0 * t * ss + e1 * (1.f - ss));
        };

        for (int j = 0; j < ni; ++j) {
            samplePoint();
        }
        if (rng.nextFloat() < n) {
            samplePoint();
        }
    }
    DBG("Generated %d points", positions.size());

    // Relax point distribution
    DBG("Relaxing point distribution ...");

    // Choose radius to support roughly 10 neighbour particles
    float radius = std::sqrt(totalArea / positions.size() * 10.f / M_PI);
    float radius2 = sqr(radius);

    for (int iteration = 0; iteration < 10; ++iteration) {
        int count = 0;
        std::vector<Vector3f> velocities(positions.size(), Vector3f());
        // Relax positions
        for (size_t i = 0; i < positions.size(); ++i) {
            for (size_t j = i + 1; j < positions.size(); ++j) {
                Vector3f r = positions[j] - positions[i];
                float r2 = r.squaredNorm();
                if (r2 < radius2) {
                    r *= (1.f / std::sqrt(r2));
                    float weight = 0.01f * cube(1.f - r2 / radius2); // TODO use a proper kernel
                    velocities[i] -= weight * r;
                    velocities[j] += weight * r;
                    ++count;
                }
            }
            positions[i] += velocities[i];
        }
        // Reproject to surface
        for (size_t i = 0; i < positions.size(); ++i) {
            Vector3f p = sdf.toVoxelSpace(positions[i]);
            Vector3f n = sdf.gradient(p).normalized();
            positions[i] -= sdf.trilinear(p) * n;
        }
        DBG("avg neighbours = %d", 2 * count / positions.size());
    }

    // Compute normals
    std::vector<Vector3f> normals(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        normals[i] = sdf.gradient(sdf.toVoxelSpace(positions[i])).normalized();
    }

    return Result({ std::move(positions), std::move(normals) });
}

} // namespace pbs
