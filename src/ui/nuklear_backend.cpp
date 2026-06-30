#include "pch.hpp"
#include "math.hpp"
#include "ui/nuklear_backend.hpp"
#include "ui/nuklear_config.hpp"
#include "gpu/gpu.hpp"

#include <cstddef>

// UI vertex layout: must match shaders/ui.vert and the convert config below.
struct UIVertex {
    float    pos[2];
    float    uv[2];
    nk_byte  col[4];
};

static constexpr uint32_t INITIAL_VBO = 256 * 1024;
static constexpr uint32_t INITIAL_IBO = 128 * 1024;

struct NuklearBackend::Impl {
    SDL_GPUDevice* dev = nullptr;
    SDL_Window*    window = nullptr;

    nk_context        ctx{};
    nk_font_atlas     atlas{};
    nk_draw_null_texture null_tex{};
    nk_buffer         cmds{};

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUTexture*  font_tex = nullptr;
    SDL_GPUSampler*  sampler  = nullptr;
    nk_user_font*    ui_font   = nullptr;   // Fira Sans (default UI font)
    nk_user_font*    mono_font = nullptr;   // JetBrains Mono (code editor)

    SDL_GPUBuffer*   vbo = nullptr;
    SDL_GPUBuffer*   ibo = nullptr;
    uint32_t         vbo_cap = 0;
    uint32_t         ibo_cap = 0;
    SDL_GPUTransferBuffer* xfer = nullptr;
    uint32_t         xfer_cap = 0;

    // per-frame draw record
    uint32_t         frame_vcount = 0;
    uint32_t         frame_icount = 0;
};

// -----------------------------------------------------------------------------
static void ensure_buffers(NuklearBackend::Impl* p, uint32_t vbytes, uint32_t ibytes) {
    if (vbytes > p->vbo_cap) {
        if (p->vbo) SDL_ReleaseGPUBuffer(p->dev, p->vbo);
        uint32_t cap = p->vbo_cap ? p->vbo_cap : INITIAL_VBO;
        while (cap < vbytes) cap *= 2;
        SDL_GPUBufferCreateInfo bci{}; bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bci.size = cap;
        p->vbo = SDL_CreateGPUBuffer(p->dev, &bci);
        p->vbo_cap = cap;
    }
    if (ibytes > p->ibo_cap) {
        if (p->ibo) SDL_ReleaseGPUBuffer(p->dev, p->ibo);
        uint32_t cap = p->ibo_cap ? p->ibo_cap : INITIAL_IBO;
        while (cap < ibytes) cap *= 2;
        SDL_GPUBufferCreateInfo bci{}; bci.usage = SDL_GPU_BUFFERUSAGE_INDEX; bci.size = cap;
        p->ibo = SDL_CreateGPUBuffer(p->dev, &bci);
        p->ibo_cap = cap;
    }
    uint32_t need = vbytes + ibytes;
    if (need > p->xfer_cap) {
        if (p->xfer) SDL_ReleaseGPUTransferBuffer(p->dev, p->xfer);
        uint32_t cap = p->xfer_cap ? p->xfer_cap : (INITIAL_VBO + INITIAL_IBO);
        while (cap < need) cap *= 2;
        SDL_GPUTransferBufferCreateInfo tci{};
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tci.size = cap;
        p->xfer = SDL_CreateGPUTransferBuffer(p->dev, &tci);
        p->xfer_cap = cap;
    }
}

// -----------------------------------------------------------------------------
bool NuklearBackend::init(SDL_GPUDevice* dev, SDL_Window* window, uint32_t swapchain_format) {
    p = new Impl();
    p->dev = dev;
    p->window = window;

    // --- shaders + pipeline ---
    SDL_GPUShader* vs = gpu::load_spirv(dev, "shaders/ui.vert.spv", 0, /*samplers*/0, /*uniforms*/1);
    SDL_GPUShader* fs = gpu::load_spirv(dev, "shaders/ui.frag.spv", 1, /*samplers*/1, /*uniforms*/0);
    if (!vs || !fs) { warnln("UI: shader load failed"); return false; }

    SDL_GPUVertexBufferDescription vbdesc{};
    vbdesc.slot = 0;
    vbdesc.pitch = sizeof(UIVertex);
    vbdesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[0].offset = offsetof(UIVertex, pos);
    attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[1].offset = offsetof(UIVertex, uv);
    attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM; attrs[2].offset = offsetof(UIVertex, col);

    SDL_GPUColorTargetDescription color_desc{};
    color_desc.format = static_cast<SDL_GPUTextureFormat>(swapchain_format);
    color_desc.blend_state.enable_blend = true;
    color_desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_desc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color_desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_desc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vs;
    pci.fragment_shader = fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbdesc;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = attrs;
    pci.vertex_input_state.num_vertex_attributes = 3;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.target_info.color_target_descriptions = &color_desc;
    pci.target_info.num_color_targets = 1;
    pci.target_info.has_depth_stencil_target = false;

    p->pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pci);
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    if (!p->pipeline) { warnln("UI: pipeline creation failed: %s", SDL_GetError()); return false; }

    // --- font atlas ---
    // Bake the font larger and oversampled so text is crisp, and scale with the
    // display's content scale for HiDPI screens.
    float scale = SDL_GetWindowDisplayScale(window);
    if (!(scale >= 1.0f)) scale = 1.0f;
    if (scale > 2.0f) scale = 2.0f;
    float font_px = 22.0f * scale;

    const float mono_px = 18.0f * scale;

    nk_font_atlas_init_default(&p->atlas);
    nk_font_atlas_begin(&p->atlas);

    // UI font: Fira Sans if bundled, else Nuklear's built-in font.
    struct nk_font_config ucfg = nk_font_config(font_px);
    ucfg.oversample_h = 3; ucfg.oversample_v = 2; ucfg.pixel_snap = 0;
    nk_font* ui_f = nullptr;
    if (std::filesystem::exists("assets/fonts/FiraSans-Regular.ttf"))
        ui_f = nk_font_atlas_add_from_file(&p->atlas, "assets/fonts/FiraSans-Regular.ttf", font_px, &ucfg);
    if (!ui_f) ui_f = nk_font_atlas_add_default(&p->atlas, font_px, &ucfg);

    // Monospace font for the code editor: JetBrains Mono if bundled, else the UI font.
    struct nk_font_config mcfg = nk_font_config(mono_px);
    mcfg.oversample_h = 2; mcfg.oversample_v = 2; mcfg.pixel_snap = 1;
    nk_font* mono_f = nullptr;
    if (std::filesystem::exists("assets/fonts/JetBrainsMono-Regular.ttf"))
        mono_f = nk_font_atlas_add_from_file(&p->atlas, "assets/fonts/JetBrainsMono-Regular.ttf", mono_px, &mcfg);
    if (!mono_f) mono_f = ui_f;

    int img_w = 0, img_h = 0;
    const void* image = nk_font_atlas_bake(&p->atlas, &img_w, &img_h, NK_FONT_ATLAS_RGBA32);
    p->font_tex = gpu::create_texture_rgba(dev, image, (uint32_t)img_w, (uint32_t)img_h, "nk.font");
    nk_font_atlas_end(&p->atlas, nk_handle_ptr(p->font_tex), &p->null_tex);

    p->ui_font   = &ui_f->handle;
    p->mono_font = &mono_f->handle;
    if (!nk_init_default(&p->ctx, p->ui_font)) { warnln("UI: nk_init_default failed"); return false; }
    nk_buffer_init_default(&p->cmds);

    p->sampler = gpu::create_linear_sampler(dev);
    ensure_buffers(p, INITIAL_VBO, INITIAL_IBO);

    println("Nuklear UI backend ready (font atlas %dx%d)", img_w, img_h);
    return true;
}

void NuklearBackend::shutdown() {
    if (!p) return;
    if (p->vbo)     SDL_ReleaseGPUBuffer(p->dev, p->vbo);
    if (p->ibo)     SDL_ReleaseGPUBuffer(p->dev, p->ibo);
    if (p->xfer)    SDL_ReleaseGPUTransferBuffer(p->dev, p->xfer);
    if (p->sampler) SDL_ReleaseGPUSampler(p->dev, p->sampler);
    if (p->font_tex)SDL_ReleaseGPUTexture(p->dev, p->font_tex);
    if (p->pipeline)SDL_ReleaseGPUGraphicsPipeline(p->dev, p->pipeline);
    nk_buffer_free(&p->cmds);
    nk_font_atlas_clear(&p->atlas);
    nk_free(&p->ctx);
    delete p;
    p = nullptr;
}

nk_context* NuklearBackend::ctx() { return &p->ctx; }
nk_user_font* NuklearBackend::ui_font()   { return p ? p->ui_font   : nullptr; }
nk_user_font* NuklearBackend::mono_font() { return p ? p->mono_font : nullptr; }

// -----------------------------------------------------------------------------
void NuklearBackend::input_begin() { nk_input_begin(&p->ctx); }
void NuklearBackend::input_end()   { nk_input_end(&p->ctx); }

void NuklearBackend::handle_event(const SDL_Event& e) {
    nk_context* ctx = &p->ctx;
    const float dens = SDL_GetWindowPixelDensity(p->window);  // points -> pixels
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            nk_input_motion(ctx, (int)(e.motion.x * dens), (int)(e.motion.y * dens));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            int x = (int)(e.button.x * dens), y = (int)(e.button.y * dens);
            if (e.button.button == SDL_BUTTON_LEFT)   nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
            if (e.button.button == SDL_BUTTON_RIGHT)  nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);
            if (e.button.button == SDL_BUTTON_MIDDLE) nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            nk_input_scroll(ctx, nk_vec2(e.wheel.x, e.wheel.y));
            break;
        case SDL_EVENT_TEXT_INPUT:
            nk_input_glyph(ctx, e.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool d = (e.type == SDL_EVENT_KEY_DOWN);
            switch (e.key.key) {
                case SDLK_BACKSPACE: nk_input_key(ctx, NK_KEY_BACKSPACE, d); break;
                case SDLK_DELETE:    nk_input_key(ctx, NK_KEY_DEL, d); break;
                case SDLK_RETURN:    nk_input_key(ctx, NK_KEY_ENTER, d); break;
                case SDLK_TAB:       nk_input_key(ctx, NK_KEY_TAB, d); break;
                case SDLK_LEFT:      nk_input_key(ctx, NK_KEY_LEFT, d); break;
                case SDLK_RIGHT:     nk_input_key(ctx, NK_KEY_RIGHT, d); break;
                case SDLK_UP:        nk_input_key(ctx, NK_KEY_UP, d); break;
                case SDLK_DOWN:      nk_input_key(ctx, NK_KEY_DOWN, d); break;
                case SDLK_LSHIFT:
                case SDLK_RSHIFT:    nk_input_key(ctx, NK_KEY_SHIFT, d); break;
                default: break;
            }
            // Ctrl shortcuts (undo/redo/copy/paste/cut/select-all) used by the
            // code editor and every text field.
            if (e.key.mod & SDL_KMOD_CTRL) {
                switch (e.key.key) {
                    case SDLK_Z: nk_input_key(ctx, NK_KEY_TEXT_UNDO, d); break;
                    case SDLK_Y: nk_input_key(ctx, NK_KEY_TEXT_REDO, d); break;
                    case SDLK_C: nk_input_key(ctx, NK_KEY_COPY, d); break;
                    case SDLK_V: nk_input_key(ctx, NK_KEY_PASTE, d); break;
                    case SDLK_X: nk_input_key(ctx, NK_KEY_CUT, d); break;
                    case SDLK_A: nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, d); break;
                    default: break;
                }
            }
            break;
        }
        default: break;
    }
}

// -----------------------------------------------------------------------------
void NuklearBackend::prepare(SDL_GPUCommandBuffer* cmd) {
    nk_context* ctx = &p->ctx;

    static const nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,    offsetof(UIVertex, pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,    offsetof(UIVertex, uv)},
        {NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8, offsetof(UIVertex, col)},
        {NK_VERTEX_LAYOUT_END}
    };

    nk_convert_config cfg{};
    cfg.vertex_layout = layout;
    cfg.vertex_size = sizeof(UIVertex);
    cfg.vertex_alignment = alignof(UIVertex);
    cfg.tex_null = p->null_tex;
    cfg.circle_segment_count = 22;
    cfg.curve_segment_count = 22;
    cfg.arc_segment_count = 22;
    cfg.global_alpha = 1.0f;
    cfg.shape_AA = NK_ANTI_ALIASING_ON;
    cfg.line_AA = NK_ANTI_ALIASING_ON;

    nk_buffer vbuf, ibuf;
    nk_buffer_init_default(&vbuf);
    nk_buffer_init_default(&ibuf);
    nk_buffer_clear(&p->cmds);
    nk_convert(ctx, &p->cmds, &vbuf, &ibuf, &cfg);

    uint32_t vbytes = (uint32_t)nk_buffer_total(&vbuf);
    uint32_t ibytes = (uint32_t)nk_buffer_total(&ibuf);
    p->frame_vcount = vbytes / sizeof(UIVertex);
    p->frame_icount = ibytes / sizeof(nk_draw_index);

    if (vbytes && ibytes) {
        ensure_buffers(p, vbytes, ibytes);
        // copy both into one transfer buffer (vertices first, then indices)
        Uint8* map = (Uint8*)SDL_MapGPUTransferBuffer(p->dev, p->xfer, true);
        std::memcpy(map, nk_buffer_memory_const(&vbuf), vbytes);
        std::memcpy(map + vbytes, nk_buffer_memory_const(&ibuf), ibytes);
        SDL_UnmapGPUTransferBuffer(p->dev, p->xfer);

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation vsrc{}; vsrc.transfer_buffer = p->xfer; vsrc.offset = 0;
        SDL_GPUBufferRegion vdst{}; vdst.buffer = p->vbo; vdst.offset = 0; vdst.size = vbytes;
        SDL_UploadToGPUBuffer(cp, &vsrc, &vdst, true);
        SDL_GPUTransferBufferLocation isrc{}; isrc.transfer_buffer = p->xfer; isrc.offset = vbytes;
        SDL_GPUBufferRegion idst{}; idst.buffer = p->ibo; idst.offset = 0; idst.size = ibytes;
        SDL_UploadToGPUBuffer(cp, &isrc, &idst, true);
        SDL_EndGPUCopyPass(cp);
    }

    nk_buffer_free(&vbuf);
    nk_buffer_free(&ibuf);
}

void NuklearBackend::render(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, int fb_w, int fb_h) {
    if (!p->frame_icount) { nk_clear(&p->ctx); return; }

    SDL_BindGPUGraphicsPipeline(pass, p->pipeline);

    em::mat4 proj = em::ortho(0.0f, (float)fb_w, (float)fb_h, 0.0f);
    SDL_PushGPUVertexUniformData(cmd, 0, &proj, sizeof(proj));

    SDL_GPUBufferBinding vb{}; vb.buffer = p->vbo; vb.offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib{}; ib.buffer = p->ibo; ib.offset = 0;
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    const nk_draw_command* dcmd;
    uint32_t offset = 0;
    nk_draw_foreach(dcmd, &p->ctx, &p->cmds) {
        if (!dcmd->elem_count) continue;
        // Clamp the clip rect to the framebuffer; Nuklear emits a huge "no clip"
        // rect that would otherwise trip SDL's GPU validation layer.
        float cx0 = dcmd->clip_rect.x, cy0 = dcmd->clip_rect.y;
        float cx1 = cx0 + dcmd->clip_rect.w, cy1 = cy0 + dcmd->clip_rect.h;
        int x0 = (int)em::clampf(cx0, 0.0f, (float)fb_w);
        int y0 = (int)em::clampf(cy0, 0.0f, (float)fb_h);
        int x1 = (int)em::clampf(cx1, 0.0f, (float)fb_w);
        int y1 = (int)em::clampf(cy1, 0.0f, (float)fb_h);
        SDL_Rect scissor{ x0, y0, x1 - x0, y1 - y0 };
        if (scissor.w <= 0 || scissor.h <= 0) continue;
        SDL_SetGPUScissor(pass, &scissor);

        // Bind the texture carried by THIS draw command. nk_image icons stash
        // their own SDL_GPUTexture* in texture.ptr; text/solid geometry carries
        // the font atlas handle. Fall back to the font atlas if a command has
        // no texture so a null handle never reaches the GPU. The sampler is
        // bound every command alongside its texture.
        SDL_GPUTextureSamplerBinding tsb{};
        tsb.texture = (SDL_GPUTexture*)dcmd->texture.ptr;
        if (!tsb.texture) tsb.texture = p->font_tex;
        tsb.sampler = p->sampler;
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);

        SDL_DrawGPUIndexedPrimitives(pass, dcmd->elem_count, 1, offset, 0, 0);
        offset += dcmd->elem_count;
    }

    // reset scissor to full frame for any following passes
    SDL_Rect full{0, 0, fb_w, fb_h};
    SDL_SetGPUScissor(pass, &full);
    nk_clear(&p->ctx);
}
