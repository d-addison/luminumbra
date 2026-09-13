"""Blender GPU presentation. Every method is called with its main-thread context.

The host's RGBA plane is already display encoded by the production lighting
pass. Blender's IMAGE shader performs the framebuffer output-space conversion;
applying the scene display transform again would tone-map the frame twice.
"""
import array


class FrameDraw:
    def __init__(self):
        import gpu
        from gpu_extras.batch import batch_for_shader
        if gpu.platform.backend_type_get() != 'OPENGL':
            raise ValueError("Installed viewport presentation currently requires Blender's OpenGL backend")
        interface = gpu.types.GPUStageInterfaceInfo('luminumbra_viewport_depth_interface')
        interface.smooth('VEC2', 'uv')
        info = gpu.types.GPUShaderCreateInfo()
        info.push_constant('MAT4', 'pixelToClip')
        info.push_constant('MAT4', 'depthToBlender')
        info.vertex_in(0, 'VEC2', 'pos')
        info.vertex_in(1, 'VEC2', 'texCoord')
        info.vertex_out(interface)
        info.sampler(0, 'FLOAT_2D', 'depthPlane')
        info.fragment_out(0, 'VEC4', 'unusedColor')
        info.depth_write('ANY')
        info.vertex_source('void main() { uv = texCoord; gl_Position = pixelToClip * vec4(pos, 0.0, 1.0); }')
        info.fragment_source('''void main() {
            float depth = texture(depthPlane, uv).r;
            vec4 clip = depthToBlender * vec4(uv * 2.0 - 1.0, depth, 1.0);
            gl_FragDepth = depth > 0.0 ? clamp(0.5 * clip.z / clip.w + 0.5, 0.0, 1.0) : 1.0;
            unusedColor = vec4(0.0);
        }''')
        self.shader = gpu.shader.create_from_info(info)
        self.batch_for_shader = batch_for_shader
        self.color = self.depth = None
        self.extent = None

    def update(self, header, payload):
        import gpu
        import numpy as np
        width, height = header['state']['width'], header['state']['height']
        pixels = width * height
        # Authentication, plane joins and bounds have already passed on the
        # worker. Uploading immutable received bytes cannot race the slot lease.
        # Blender 5.1's GPUTexture constructor requires a FLOAT buffer even for
        # RGBA8 storage. Normalize vectorially using Blender's bundled NumPy.
        colors = np.frombuffer(payload, dtype=np.uint8, count=4 * pixels).astype(np.float32)
        colors *= np.float32(1 / 255)
        rgba = gpu.types.Buffer('FLOAT', 4 * pixels, colors)
        depth = array.array('f')
        depth.frombytes(payload[4 * pixels:8 * pixels])
        self.color = gpu.types.GPUTexture((width, height), format='RGBA8', data=rgba)
        self.depth = gpu.types.GPUTexture((width, height), format='R32F',
                                         data=gpu.types.Buffer('FLOAT', pixels, depth))
        self.color.filter_mode(False)
        self.depth.filter_mode(False)
        self.extent = (width, height)

    def draw(self, rectangle, depth_transform):
        import gpu
        from gpu_extras.presets import draw_texture_2d
        from mathutils import Matrix
        if self.color is None:
            return
        x, y, width, height = rectangle
        old = (gpu.state.blend_get(), gpu.state.depth_test_get(), gpu.state.depth_mask_get())
        try:
            gpu.state.blend_set('NONE')
            gpu.state.depth_test_set('NONE')
            gpu.state.depth_mask_set(False)
            draw_texture_2d(self.color, (x, y), width, height)
            # Preserve color while filling depth for Blender's later overlays.
            # External-engine entry guarantees all four color channels writable.
            gpu.state.color_mask_set(False, False, False, False)
            gpu.state.depth_test_set('ALWAYS')
            gpu.state.depth_mask_set(True)
            self.shader.bind()
            self.shader.uniform_float('pixelToClip', gpu.matrix.get_projection_matrix() @ gpu.matrix.get_model_view_matrix())
            self.shader.uniform_float('depthToBlender', Matrix(tuple(
                tuple(depth_transform[col * 4 + row] for col in range(4)) for row in range(4))))
            self.shader.uniform_sampler('depthPlane', self.depth)
            batch = self.batch_for_shader(self.shader, 'TRIS',
                {'pos': ((x, y), (x + width, y), (x + width, y + height), (x, y + height)),
                 'texCoord': ((0, 0), (1, 0), (1, 1), (0, 1))}, indices=((0, 1, 2), (2, 3, 0)))
            batch.draw(self.shader)
        finally:
            gpu.state.color_mask_set(True, True, True, True)
            gpu.state.blend_set(old[0])
            gpu.state.depth_test_set(old[1])
            gpu.state.depth_mask_set(old[2])
