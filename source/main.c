#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include "math_utils.h"
#include "netlog.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define DISPLAY_WIDTH 960
#define DISPLAY_HEIGHT 544
#define DISPLAY_STRIDE 1024
#define DISPLAY_BUFFER_COUNT 2
#define DISPLAY_COLOR_FORMAT SCE_GXM_COLOR_FORMAT_A8B8G8R8
#define DISPLAY_PIXEL_FORMAT SCE_DISPLAY_PIXELFORMAT_A8B8G8R8
#define MAX_PENDING_SWAPS (DISPLAY_BUFFER_COUNT - 1)

struct clear_vertex {
	float x;
	float y;
};

struct color_vertex {
	float x, y, z;
	float r, g, b, a;
};

struct display_queue_callback_data {
	void *addr;
};

extern unsigned char _binary_clear_v_gxp_start;
extern unsigned char _binary_clear_f_gxp_start;
extern unsigned char _binary_color_v_gxp_start;
extern unsigned char _binary_color_f_gxp_start;

static const SceGxmProgram *const gxm_program_clear_v = (SceGxmProgram *)&_binary_clear_v_gxp_start;
static const SceGxmProgram *const gxm_program_clear_f = (SceGxmProgram *)&_binary_clear_f_gxp_start;
static const SceGxmProgram *const gxm_program_color_v = (SceGxmProgram *)&_binary_color_v_gxp_start;
static const SceGxmProgram *const gxm_program_color_f = (SceGxmProgram *)&_binary_color_f_gxp_start;

static SceGxmContext *gxm_context;
static SceUID vdm_ring_buffer_uid;
static void *vdm_ring_buffer_addr;
static SceUID vertex_ring_buffer_uid;
static void *vertex_ring_buffer_addr;
static SceUID fragment_ring_buffer_uid;
static void *fragment_ring_buffer_addr;
static SceUID fragment_usse_ring_buffer_uid;
static void *fragment_usse_ring_buffer_addr;
static SceGxmRenderTarget *gxm_render_target;
static SceGxmColorSurface gxm_color_surfaces[DISPLAY_BUFFER_COUNT];
static SceUID gxm_color_surfaces_uid[DISPLAY_BUFFER_COUNT];
static void *gxm_color_surfaces_addr[DISPLAY_BUFFER_COUNT];
static SceGxmSyncObject *gxm_sync_objects[DISPLAY_BUFFER_COUNT];
static unsigned int gxm_front_buffer_index;
static unsigned int gxm_back_buffer_index;
static SceUID gxm_depth_surface_uid;
static void *gxm_depth_surface_addr;
static SceUID gxm_stencil_surface_uid;
static void *gxm_stencil_surface_addr;
static SceGxmDepthStencilSurface gxm_depth_stencil_surface;
static SceGxmShaderPatcher *gxm_shader_patcher;
static SceUID gxm_shader_patcher_buffer_uid;
static void *gxm_shader_patcher_buffer_addr;
static SceUID gxm_shader_patcher_vertex_usse_uid;
static void *gxm_shader_patcher_vertex_usse_addr;
static SceUID gxm_shader_patcher_fragment_usse_uid;
static void *gxm_shader_patcher_fragment_usse_addr;

static SceGxmShaderPatcherId gxm_clear_vertex_program_id;
static SceGxmShaderPatcherId gxm_clear_fragment_program_id;
static const SceGxmProgramParameter *gxm_clear_vertex_program_position_param;
static const SceGxmProgramParameter *gxm_clear_fragment_program_u_clear_color_param;
static SceGxmVertexProgram *gxm_clear_vertex_program_patched;
static SceGxmFragmentProgram *gxm_clear_fragment_program_patched;

static SceGxmShaderPatcherId gxm_color_vertex_program_id;
static SceGxmShaderPatcherId gxm_color_fragment_program_id;
static const SceGxmProgramParameter *gxm_color_vertex_program_position_param;
static const SceGxmProgramParameter *gxm_color_vertex_program_color_param;
static const SceGxmProgramParameter *gxm_color_vertex_program_u_mvp_matrix_param;
static SceGxmVertexProgram *gxm_color_vertex_program_patched;
static SceGxmFragmentProgram *gxm_color_fragment_program_patched;

static void *gpu_alloc_map(SceKernelMemBlockType type, SceGxmMemoryAttribFlags gpu_attrib, size_t size, SceUID *uid)
{
	SceUID memuid;
	void *addr;

	if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
		size = ALIGN(size, 256 * 1024);
	else
		size = ALIGN(size, 4 * 1024);

	memuid = sceKernelAllocMemBlock("gpumem", type, size, NULL);
	if (memuid < 0)
		return NULL;

	if (sceKernelGetMemBlockBase(memuid, &addr) < 0)
		return NULL;

	if (sceGxmMapMemory(addr, size, gpu_attrib) < 0) {
		sceKernelFreeMemBlock(memuid);
		return NULL;
	}

	if (uid)
		*uid = memuid;

	return addr;
}

static void gpu_unmap_free(SceUID uid)
{
	void *addr;

	if (sceKernelGetMemBlockBase(uid, &addr) < 0)
		return;

	sceGxmUnmapMemory(addr);

	sceKernelFreeMemBlock(uid);
}

static void *gpu_vertex_usse_alloc_map(size_t size, SceUID *uid, unsigned int *usse_offset)
{
	SceUID memuid;
	void *addr;

	size = ALIGN(size, 4 * 1024);

	memuid = sceKernelAllocMemBlock("gpu_vertex_usse",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
	if (memuid < 0)
		return NULL;

	if (sceKernelGetMemBlockBase(memuid, &addr) < 0)
		return NULL;

	if (sceGxmMapVertexUsseMemory(addr, size, usse_offset) < 0)
		return NULL;

	return addr;
}

static void gpu_vertex_usse_unmap_free(SceUID uid)
{
	void *addr;

	if (sceKernelGetMemBlockBase(uid, &addr) < 0)
		return;

	sceGxmUnmapVertexUsseMemory(addr);

	sceKernelFreeMemBlock(uid);
}

static void *gpu_fragment_usse_alloc_map(size_t size, SceUID *uid, unsigned int *usse_offset)
{
	SceUID memuid;
	void *addr;

	size = ALIGN(size, 4 * 1024);

	memuid = sceKernelAllocMemBlock("gpu_fragment_usse",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
	if (memuid < 0)
		return NULL;

	if (sceKernelGetMemBlockBase(memuid, &addr) < 0)
		return NULL;

	if (sceGxmMapFragmentUsseMemory(addr, size, usse_offset) < 0)
		return NULL;

	return addr;
}

static void gpu_fragment_usse_unmap_free(SceUID uid)
{
	void *addr;

	if (sceKernelGetMemBlockBase(uid, &addr) < 0)
		return;

	sceGxmUnmapFragmentUsseMemory(addr);

	sceKernelFreeMemBlock(uid);
}

static void *shader_patcher_host_alloc_cb(void *user_data, unsigned int size)
{
	return malloc(size);
}

static void shader_patcher_host_free_cb(void *user_data, void *mem)
{
	return free(mem);
}

static void display_queue_callback(const void *callbackData)
{
	SceDisplayFrameBuf display_fb;
	const struct display_queue_callback_data *cb_data = callbackData;

	memset(&display_fb, 0, sizeof(display_fb));
	display_fb.size = sizeof(display_fb);
	display_fb.base = cb_data->addr;
	display_fb.pitch = DISPLAY_STRIDE;
	display_fb.pixelformat = DISPLAY_PIXEL_FORMAT;
	display_fb.width = DISPLAY_WIDTH;
	display_fb.height = DISPLAY_HEIGHT;

	sceDisplaySetFrameBuf(&display_fb, SCE_DISPLAY_SETBUF_NEXTFRAME);

	sceDisplayWaitVblankStart();
}

int main(int argc, char *argv[])
{
	int i;

	netlog_init();
	netlog("GXM fun by xerpi\n");

	SceGxmInitializeParams gxm_init_params;
	memset(&gxm_init_params, 0, sizeof(gxm_init_params));
	gxm_init_params.flags = 0;
	gxm_init_params.displayQueueMaxPendingCount = MAX_PENDING_SWAPS;
	gxm_init_params.displayQueueCallback = display_queue_callback;
	gxm_init_params.displayQueueCallbackDataSize = sizeof(struct display_queue_callback_data);
	gxm_init_params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;

	sceGxmInitialize(&gxm_init_params);

	vdm_ring_buffer_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
		&vdm_ring_buffer_uid);

	vertex_ring_buffer_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
		&vertex_ring_buffer_uid);

	fragment_ring_buffer_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
		&fragment_ring_buffer_uid);

	unsigned int fragment_usse_offset;
	fragment_usse_ring_buffer_addr = gpu_fragment_usse_alloc_map(
		SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
		&fragment_ring_buffer_uid, &fragment_usse_offset);

	SceGxmContextParams gxm_context_params;
	memset(&gxm_context_params, 0, sizeof(gxm_context_params));
	gxm_context_params.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	gxm_context_params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	gxm_context_params.vdmRingBufferMem = vdm_ring_buffer_addr;
	gxm_context_params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
	gxm_context_params.vertexRingBufferMem = vertex_ring_buffer_addr;
	gxm_context_params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
	gxm_context_params.fragmentRingBufferMem = fragment_ring_buffer_addr;
	gxm_context_params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
	gxm_context_params.fragmentUsseRingBufferMem = fragment_usse_ring_buffer_addr;
	gxm_context_params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
	gxm_context_params.fragmentUsseRingBufferOffset = fragment_usse_offset;

	sceGxmCreateContext(&gxm_context_params, &gxm_context);

	SceGxmRenderTargetParams render_target_params;
	memset(&render_target_params, 0, sizeof(render_target_params));
	render_target_params.flags = 0;
	render_target_params.width = DISPLAY_WIDTH;
	render_target_params.height = DISPLAY_HEIGHT;
	render_target_params.scenesPerFrame = 1;
	render_target_params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	render_target_params.multisampleLocations = 0;
	render_target_params.driverMemBlock = -1;

	sceGxmCreateRenderTarget(&render_target_params, &gxm_render_target);

	for (i = 0; i < DISPLAY_BUFFER_COUNT; i++) {
		gxm_color_surfaces_addr[i] = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
			SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
			ALIGN(4 * DISPLAY_STRIDE * DISPLAY_HEIGHT, 1 * 1024 * 1024),
			&gxm_color_surfaces_uid[i]);

		memset(gxm_color_surfaces_addr[i], 0, DISPLAY_STRIDE * DISPLAY_HEIGHT);

		sceGxmColorSurfaceInit(&gxm_color_surfaces[i],
			DISPLAY_COLOR_FORMAT,
			SCE_GXM_COLOR_SURFACE_LINEAR,
			SCE_GXM_COLOR_SURFACE_SCALE_NONE,
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
			DISPLAY_WIDTH,
			DISPLAY_HEIGHT,
			DISPLAY_STRIDE,
			gxm_color_surfaces_addr[i]);

		sceGxmSyncObjectCreate(&gxm_sync_objects[i]);
	}

	unsigned int depth_stencil_width = ALIGN(DISPLAY_WIDTH, SCE_GXM_TILE_SIZEX);
	unsigned int depth_stencil_height = ALIGN(DISPLAY_HEIGHT, SCE_GXM_TILE_SIZEY);
	unsigned int depth_stencil_samples = depth_stencil_width * depth_stencil_height;

	gxm_depth_surface_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
		4 * depth_stencil_samples, &gxm_depth_surface_uid);

	gxm_stencil_surface_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
		1 * depth_stencil_samples, &gxm_stencil_surface_uid);

	sceGxmDepthStencilSurfaceInit(&gxm_depth_stencil_surface,
		SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
		SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
		depth_stencil_width,
		gxm_depth_surface_addr,
		gxm_stencil_surface_addr);

	static const unsigned int shader_patcher_buffer_size = 64 * 1024;
	static const unsigned int shader_patcher_vertex_usse_size = 64 * 1024;
	static const unsigned int shader_patcher_fragment_usse_size = 64 * 1024;

	gxm_shader_patcher_buffer_addr = gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
		SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_READ,
		shader_patcher_buffer_size, &gxm_shader_patcher_buffer_uid);

	unsigned int shader_patcher_vertex_usse_offset;
	gxm_shader_patcher_vertex_usse_addr = gpu_vertex_usse_alloc_map(
		shader_patcher_vertex_usse_size, &gxm_shader_patcher_vertex_usse_uid,
		&shader_patcher_vertex_usse_offset);

	unsigned int shader_patcher_fragment_usse_offset;
	gxm_shader_patcher_fragment_usse_addr = gpu_fragment_usse_alloc_map(
		shader_patcher_fragment_usse_size, &gxm_shader_patcher_fragment_usse_uid,
		&shader_patcher_fragment_usse_offset);

	SceGxmShaderPatcherParams shader_patcher_params;
	memset(&shader_patcher_params, 0, sizeof(shader_patcher_params));
	shader_patcher_params.userData = NULL;
	shader_patcher_params.hostAllocCallback = shader_patcher_host_alloc_cb;
	shader_patcher_params.hostFreeCallback = shader_patcher_host_free_cb;
	shader_patcher_params.bufferAllocCallback = NULL;
	shader_patcher_params.bufferFreeCallback = NULL;
	shader_patcher_params.bufferMem = gxm_shader_patcher_buffer_addr;
	shader_patcher_params.bufferMemSize = shader_patcher_buffer_size;
	shader_patcher_params.vertexUsseAllocCallback = NULL;
	shader_patcher_params.vertexUsseFreeCallback = NULL;
	shader_patcher_params.vertexUsseMem = gxm_shader_patcher_vertex_usse_addr;
	shader_patcher_params.vertexUsseMemSize = shader_patcher_vertex_usse_size;
	shader_patcher_params.vertexUsseOffset = shader_patcher_vertex_usse_offset;
	shader_patcher_params.fragmentUsseAllocCallback = NULL;
	shader_patcher_params.fragmentUsseFreeCallback = NULL;
	shader_patcher_params.fragmentUsseMem = gxm_shader_patcher_fragment_usse_addr;
	shader_patcher_params.fragmentUsseMemSize = shader_patcher_fragment_usse_size;
	shader_patcher_params.fragmentUsseOffset = shader_patcher_fragment_usse_offset;

	sceGxmShaderPatcherCreate(&shader_patcher_params, &gxm_shader_patcher);

	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_clear_v,
		&gxm_clear_vertex_program_id);

	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_clear_f,
		&gxm_clear_fragment_program_id);

	const SceGxmProgram *clear_vertex_program =
		sceGxmShaderPatcherGetProgramFromId(gxm_clear_vertex_program_id);
	const SceGxmProgram *clear_fragment_program =
		sceGxmShaderPatcherGetProgramFromId(gxm_clear_fragment_program_id);

	gxm_clear_vertex_program_position_param = sceGxmProgramFindParameterByName(
		clear_vertex_program, "position");

	gxm_clear_fragment_program_u_clear_color_param = sceGxmProgramFindParameterByName(
		clear_fragment_program, "u_clear_color");

	SceGxmVertexAttribute clear_vertex_attribute;
	SceGxmVertexStream clear_vertex_stream;
	clear_vertex_attribute.streamIndex = 0;
	clear_vertex_attribute.offset = 0;
	clear_vertex_attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	clear_vertex_attribute.componentCount = 2;
	clear_vertex_attribute.regIndex = sceGxmProgramParameterGetResourceIndex(
		gxm_clear_vertex_program_position_param);
	clear_vertex_stream.stride = sizeof(struct clear_vertex);
	clear_vertex_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

	sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
		gxm_clear_vertex_program_id, &clear_vertex_attribute,
		1, &clear_vertex_stream, 1, &gxm_clear_vertex_program_patched);

	sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
		gxm_clear_fragment_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, clear_fragment_program,
		&gxm_clear_fragment_program_patched);

	SceUID clear_vertices_uid;
	struct clear_vertex *const clear_vertices_data = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_MEMORY_ATTRIB_READ,
		4 * sizeof(struct clear_vertex), &clear_vertices_uid);

	SceUID clear_indices_uid;
	unsigned short *const clear_indices_data = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_MEMORY_ATTRIB_READ,
		4 * sizeof(unsigned short), &clear_indices_uid);

	clear_vertices_data[0].x = -1.0f;
	clear_vertices_data[0].y = -1.0f;
	clear_vertices_data[1].x = 1.0f;
	clear_vertices_data[1].y = -1.0f;
	clear_vertices_data[2].x = -1.0f;
	clear_vertices_data[2].y = 1.0f;
	clear_vertices_data[3].x = 1.0f;
	clear_vertices_data[3].y = 1.0f;

	clear_indices_data[0] = 0;
	clear_indices_data[1] = 1;
	clear_indices_data[2] = 2;
	clear_indices_data[3] = 3;

	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_color_v,
		&gxm_color_vertex_program_id);

	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_color_f,
		&gxm_color_fragment_program_id);

	const SceGxmProgram *color_vertex_program =
		sceGxmShaderPatcherGetProgramFromId(gxm_color_vertex_program_id);
	const SceGxmProgram *color_fragment_program =
		sceGxmShaderPatcherGetProgramFromId(gxm_color_fragment_program_id);

	gxm_color_vertex_program_position_param = sceGxmProgramFindParameterByName(
		color_vertex_program, "position");

	gxm_color_vertex_program_color_param = sceGxmProgramFindParameterByName(
		color_vertex_program, "color");

	gxm_color_vertex_program_u_mvp_matrix_param = sceGxmProgramFindParameterByName(
		color_vertex_program, "u_mvp_matrix");

	SceGxmVertexAttribute color_vertex_attributes[2];
	SceGxmVertexStream color_vertex_stream;
	color_vertex_attributes[0].streamIndex = 0;
	color_vertex_attributes[0].offset = 0;
	color_vertex_attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	color_vertex_attributes[0].componentCount = 3;
	color_vertex_attributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(
		gxm_color_vertex_program_position_param);
	color_vertex_attributes[1].streamIndex = 0;
	color_vertex_attributes[1].offset = 3 * sizeof(float);
	color_vertex_attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	color_vertex_attributes[1].componentCount = 4;
	color_vertex_attributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(
		gxm_color_vertex_program_color_param);
	color_vertex_stream.stride = sizeof(struct color_vertex);
	color_vertex_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

	sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
		gxm_color_vertex_program_id, color_vertex_attributes,
		2, &color_vertex_stream, 1, &gxm_color_vertex_program_patched);

	sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
		gxm_color_fragment_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, color_fragment_program,
		&gxm_color_fragment_program_patched);

	SceUID cube_vertices_uid;
	struct color_vertex *const cube_vertices_data = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_MEMORY_ATTRIB_READ,
		8 * sizeof(struct color_vertex), &cube_vertices_uid);

	SceUID cube_indices_uid;
	unsigned short *const cube_indices_data = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_MEMORY_ATTRIB_READ,
		12 * 3 * sizeof(unsigned short), &cube_indices_uid);

	#define CUBE_SIZE 1.0f

	static const float cube_vertices[] = {
		-CUBE_SIZE, -CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, -CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, +CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, +CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, +CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, +CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f
		-CUBE_SIZE, -CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, -CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, +CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, +CUBE_SIZE, +CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		-CUBE_SIZE, +CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f,
		+CUBE_SIZE, +CUBE_SIZE, -CUBE_SIZE,
		1.0f, 0.0f, 0.0f, 1.0f
	};
	memcpy(cube_vertices_data, cube_vertices, sizeof(cube_vertices));

	static const unsigned short cube_indices[] = {
		0, 1, 2,
		2, 1, 3,
		1, 5, 3,
		3, 5, 7,
		5, 4, 7,
		7, 4, 6,
		4, 0, 6,
		6, 0, 2,
		4, 5, 0,
		0, 5, 1,
		2, 3, 6,
		6, 3, 7
	};
	memcpy(cube_indices_data, cube_indices, sizeof(cube_indices));

	gxm_front_buffer_index = 0;
	gxm_back_buffer_index = 0;

	matrix4x4 projection_matrix;
	matrix4x4_init_perspective(projection_matrix, 90.0f,
		DISPLAY_WIDTH / (float)DISPLAY_HEIGHT, 0.01f, 100.0f);

	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));

	float trans_z = -3.0f;
	float rot_y = 0.0f;
	float rot_x = 0.0f;

	static int run = 1;
	while (run) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START)
			run = 0;

		if (pad.buttons & SCE_CTRL_UP)
			trans_z += 0.025f;
		else if (pad.buttons & SCE_CTRL_DOWN)
			trans_z -= 0.025f;

		if (pad.buttons & SCE_CTRL_RIGHT)
			rot_y += 0.025f;
		else if (pad.buttons & SCE_CTRL_LEFT)
			rot_y -= 0.025f;

		if (pad.buttons & SCE_CTRL_SQUARE)
			rot_x += 0.025f;
		else if (pad.buttons & SCE_CTRL_CIRCLE)
			rot_x -= 0.025f;

		sceGxmBeginScene(gxm_context,
			0,
			gxm_render_target,
			NULL,
			NULL,
			gxm_sync_objects[gxm_back_buffer_index],
			&gxm_color_surfaces[gxm_back_buffer_index],
			&gxm_depth_stencil_surface);

		sceGxmSetVertexProgram(gxm_context, gxm_clear_vertex_program_patched);
		sceGxmSetFragmentProgram(gxm_context, gxm_clear_fragment_program_patched);

		static const float clear_color[4] = {
			1.0f, 1.0f, 0.0f, 1.0f
		};

		void *clear_color_uniform_buffer;
		sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &clear_color_uniform_buffer);
		sceGxmSetUniformDataF(clear_color_uniform_buffer, gxm_clear_fragment_program_u_clear_color_param,
			0, sizeof(clear_color) / sizeof(float), clear_color);

		sceGxmSetVertexStream(gxm_context, 0, clear_vertices_data);
		sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
			SCE_GXM_INDEX_FORMAT_U16, clear_indices_data, 4);

		matrix4x4 mvp_matrix;
		matrix4x4 mv_matrix;
		matrix4x4 view_matrix;
		matrix4x4 model_matrix;
		matrix4x4_identity(view_matrix);
		matrix4x4_identity(model_matrix);

		matrix4x4_translate(model_matrix, 0.0f, 0.0f, trans_z);
		matrix4x4_rotate_y(model_matrix, rot_y);
		matrix4x4_rotate_x(model_matrix, rot_x);

		matrix4x4_multiply(mv_matrix, view_matrix, model_matrix);
		matrix4x4_multiply(mvp_matrix, projection_matrix, mv_matrix);

		sceGxmSetVertexProgram(gxm_context, gxm_color_vertex_program_patched);
		sceGxmSetFragmentProgram(gxm_context, gxm_color_fragment_program_patched);

		void *color_mvp_matrix_uniform_buffer;
		sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &color_mvp_matrix_uniform_buffer);
		sceGxmSetUniformDataF(color_mvp_matrix_uniform_buffer, gxm_color_vertex_program_u_mvp_matrix_param,
			0, sizeof(mvp_matrix) / sizeof(float), (float *)mvp_matrix);

		sceGxmSetVertexStream(gxm_context, 0, cube_vertices_data);
		sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES,
			SCE_GXM_INDEX_FORMAT_U16, cube_indices_data, 12 * 3);

		sceGxmEndScene(gxm_context, NULL, NULL);

		sceGxmPadHeartbeat(&gxm_color_surfaces[gxm_back_buffer_index],
			gxm_sync_objects[gxm_back_buffer_index]);

		struct display_queue_callback_data queue_cb_data;
		queue_cb_data.addr = gxm_color_surfaces_addr[gxm_back_buffer_index];

		sceGxmDisplayQueueAddEntry(gxm_sync_objects[gxm_front_buffer_index],
			gxm_sync_objects[gxm_back_buffer_index], &queue_cb_data);

		gxm_front_buffer_index = gxm_back_buffer_index;
		gxm_back_buffer_index = (gxm_back_buffer_index + 1) % DISPLAY_BUFFER_COUNT;
	}

	sceGxmDisplayQueueFinish();
	sceGxmFinish(gxm_context);

	gpu_unmap_free(clear_vertices_uid);
	gpu_unmap_free(clear_indices_uid);

	gpu_unmap_free(cube_vertices_uid);
	gpu_unmap_free(cube_indices_uid);

	sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher,
		gxm_clear_vertex_program_patched);
	sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher,
		gxm_clear_fragment_program_patched);

	sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher,
		gxm_color_vertex_program_patched);
	sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher,
		gxm_color_fragment_program_patched);

	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher,
		gxm_clear_vertex_program_id);
	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher,
		gxm_clear_fragment_program_id);

	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher,
		gxm_color_vertex_program_id);
	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher,
		gxm_color_fragment_program_id);

	sceGxmShaderPatcherDestroy(gxm_shader_patcher);

	gpu_unmap_free(gxm_shader_patcher_buffer_uid);
	gpu_vertex_usse_unmap_free(gxm_shader_patcher_vertex_usse_uid);
	gpu_fragment_usse_unmap_free(gxm_shader_patcher_fragment_usse_uid);

	gpu_unmap_free(gxm_depth_surface_uid);
	gpu_unmap_free(gxm_stencil_surface_uid);

	for (i = 0; i < DISPLAY_BUFFER_COUNT; i++) {
		gpu_unmap_free(gxm_color_surfaces_uid[i]);
		sceGxmSyncObjectDestroy(gxm_sync_objects[i]);
	}

	sceGxmDestroyRenderTarget(gxm_render_target);

	gpu_unmap_free(vdm_ring_buffer_uid);
	gpu_unmap_free(vertex_ring_buffer_uid);
	gpu_unmap_free(fragment_ring_buffer_uid);
	gpu_fragment_usse_unmap_free(fragment_usse_ring_buffer_uid);

	sceGxmDestroyContext(gxm_context);

	sceGxmTerminate();

	netlog_fini();

	return 0;
}
