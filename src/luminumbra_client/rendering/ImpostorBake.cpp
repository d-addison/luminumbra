#include "ImpostorBake.h"

#include "Mesh.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <fstream>
#include <vector>

namespace Luminumbra::Rendering {

namespace {

// Tree parts to fold into one impostor. Branches first (opaque-ish), leaves over.
// {path, flat RGB color} — first pass renders flat-shaded geometry (no leaf-texture
// alpha cutout yet) so the bake PIPELINE (N-direction render + atlas pack + readback)
// can be validated before the textured-cutout refinement.
struct BakePart {
    const char* path;
    glm::vec3 color;
};
const BakePart kParts[] = {
    { "data/models/trees/tree_small_02_branches.lmesh", glm::vec3(0.32f, 0.20f, 0.10f) }, // bark brown
    { "data/models/trees/tree_small_02_leaves.lmesh",   glm::vec3(0.18f, 0.42f, 0.14f) }, // leaf green
};
constexpr glm::vec3 kBackground(1.0f, 0.0f, 1.0f); // magenta key => silhouette is obvious + countable

GLuint CompileBakeProgram(std::string& err) {
    const char* kVert =
        "#version 450 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "uniform mat4 uMVP;\n"
        "void main(){ gl_Position = uMVP * vec4(aPos, 1.0); }\n";
    const char* kFrag =
        "#version 450 core\n"
        "uniform vec3 uColor;\n"
        "out vec4 frag;\n"
        "void main(){ frag = vec4(uColor, 1.0); }\n";
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            err = log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    if (!vs) return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        err = log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool WritePpm(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(f);
}

} // namespace

ImpostorBakeResult BakeTreeImpostorAtlas(const std::string& outPpmPath, const std::string& rootDir,
                                         const OctaImpostorGrid& grid) {
    ImpostorBakeResult out;
    const int n = std::max(1, grid.gridResolution);
    const int tile = std::max(1, grid.tileResolution);
    const int atlas = n * tile;
    out.atlas_size = atlas;

    // --- Load the tree parts + union their bounding spheres for framing. ---
    std::vector<std::unique_ptr<Mesh>> meshes;
    glm::vec3 unionC(0.0f);
    float unionR = 0.0f;
    bool first = true;
    for (const BakePart& p : kParts) {
        const std::string full = rootDir.empty() ? std::string(p.path) : (rootDir + "/" + p.path);
        std::unique_ptr<Mesh> m = MeshLoader::Load(full);
        if (!m || m->indexCount == 0) {
            out.error = std::string("failed to load tree part: ") + full;
            return out;
        }
        const glm::vec3 c(m->boundingSphere.x, m->boundingSphere.y, m->boundingSphere.z);
        const float r = m->boundingSphere.w;
        if (first) { unionC = c; unionR = r; first = false; }
        else {
            const glm::vec3 mid = (unionC + c) * 0.5f;
            const float ur = glm::max(glm::length(mid - unionC) + unionR, glm::length(mid - c) + r);
            unionC = mid; unionR = ur;
        }
        meshes.push_back(std::move(m));
    }
    if (unionR <= 0.0f) unionR = 1.0f;

    std::string err;
    const GLuint prog = CompileBakeProgram(err);
    if (!prog) { out.error = "bake shader: " + err; return out; }

    // --- Atlas FBO (RGBA8 color + depth). ---
    GLuint fbo = 0, colorTex = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas, atlas, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, atlas, atlas);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        out.error = "impostor atlas FBO incomplete";
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &colorTex); glDeleteRenderbuffers(1, &depthRb);
        glDeleteProgram(prog);
        return out;
    }

    glUseProgram(prog);
    const GLint locMVP = glGetUniformLocation(prog, "uMVP");
    const GLint locColor = glGetUniformLocation(prog, "uColor");
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); // leaf cards are double-sided

    // Clear the whole atlas to the background key once, then render each tile's viewport.
    glViewport(0, 0, atlas, atlas);
    glClearColor(kBackground.r, kBackground.g, kBackground.b, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float D = unionR * 2.0f;          // camera distance along the view direction
    const float hr = unionR * 1.05f;        // ortho half-extent (small margin)
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Vec3f d3 = OctaTileDirection(i, j, grid);
            glm::vec3 dir(d3.x, d3.y, d3.z);
            const glm::vec3 eye = unionC + dir * D;
            const glm::vec3 up = (std::fabs(dir.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            const glm::mat4 view = glm::lookAt(eye, unionC, up);
            const glm::mat4 proj = glm::ortho(-hr, hr, -hr, hr, 0.01f, D + unionR * 2.0f);
            const glm::mat4 mvp = proj * view;

            glViewport(i * tile, j * tile, tile, tile);
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &mvp[0][0]);
            for (std::size_t mi = 0; mi < meshes.size(); ++mi) {
                const glm::vec3& col = kParts[mi].color;
                glUniform3f(locColor, col.r, col.g, col.b);
                glBindVertexArray(meshes[mi]->vao);
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(meshes[mi]->indexCount),
                               GL_UNSIGNED_INT, nullptr);
            }
        }
    }
    glBindVertexArray(0);

    // --- Readback + coverage stats. ---
    std::vector<unsigned char> rgb(static_cast<std::size_t>(atlas) * atlas * 3u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, atlas, atlas, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    // Per-tile coverage = fraction of pixels that are NOT the magenta key.
    std::vector<float> tileCov(static_cast<std::size_t>(n) * n, 0.0f);
    const unsigned char bgR = static_cast<unsigned char>(kBackground.r * 255.0f);
    const unsigned char bgB = static_cast<unsigned char>(kBackground.b * 255.0f);
    double covSum = 0.0;
    out.min_coverage = 1.0f;
    for (int tj = 0; tj < n; ++tj) {
        for (int ti = 0; ti < n; ++ti) {
            long covered = 0;
            for (int py = 0; py < tile; ++py) {
                for (int px = 0; px < tile; ++px) {
                    const int ax = ti * tile + px;
                    const int ay = tj * tile + py;
                    const std::size_t idx = (static_cast<std::size_t>(ay) * atlas + ax) * 3u;
                    const bool isBg = (rgb[idx] == bgR && rgb[idx + 1] == 0 && rgb[idx + 2] == bgB);
                    if (!isBg) ++covered;
                }
            }
            const float cov = static_cast<float>(covered) / static_cast<float>(tile * tile);
            tileCov[static_cast<std::size_t>(tj) * n + ti] = cov;
            covSum += cov;
            out.min_coverage = glm::min(out.min_coverage, cov);
        }
    }
    out.mean_coverage = static_cast<float>(covSum / (static_cast<double>(n) * n));

    // --- Write the atlas PPM + a coverage JSON. ---
    if (!WritePpm(outPpmPath, atlas, atlas, rgb)) {
        out.error = "failed to write atlas PPM: " + outPpmPath;
    } else {
        std::ofstream jf(outPpmPath + ".json");
        if (jf) {
            jf << "{\n  \"atlas_size\": " << atlas << ",\n  \"grid\": " << n
               << ",\n  \"tile\": " << tile << ",\n  \"mean_coverage\": " << out.mean_coverage
               << ",\n  \"min_coverage\": " << out.min_coverage << ",\n  \"tile_coverage\": [";
            for (std::size_t k = 0; k < tileCov.size(); ++k) {
                if (k) jf << ", ";
                jf << tileCov[k];
            }
            jf << "]\n}\n";
        }
        out.ok = true;
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &depthRb);
    glDeleteProgram(prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return out;
}

} // namespace Luminumbra::Rendering
