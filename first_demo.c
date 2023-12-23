// ============================================================================
// Nintendo64 Demo
//
// Purpose: Basic Nintendo64 demo program using official lib(g)ultra         
//          built with the latest current gcc and binutils (13.2.0 + 2.41)   
//          using C23, just for the sake of it!
//
// Author:  Shirobon                                                         
// Date:    2023/12/23                                                       
// ============================================================================

#include <ultra64.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "brick.h"
#include "banner.h"

// ============================================================================
// Program Configuration
// ============================================================================
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ < 202000L)
    #define nullptr NULL
#endif

#define SCREEN_W (320)
#define SCREEN_H (240)

// Kind of arbitrary, for allocating display list buffer for an RCP task.
#define MAX_DISPLAYLISTS (32)

// ============================================================================
// Globals
// ============================================================================
extern uint8_t _lg_idle_thread_stack[]; // Defined by linker script
extern uint8_t _lg_main_thread_stack[];

static OSThread g_idle_thread_handle;
static OSThread g_main_thread_handle;

static OSPiHandle* g_rom_handler;

static OSMesgQueue g_messagequeue_rdp;
static OSMesgQueue g_messagequeue_retrace;

static OSMesg g_messagebuffer_rdp;
static OSMesg g_messagebuffer_retrace;

static int g_active_framebuffer;

static Mtx g_matrix_projection;
static Mtx g_matrix_modelview_solid;
static Mtx g_matrix_modelview_textured;

// ============================================================================
// Display Lists and related
// ============================================================================

// Memory accessed by the RSP must be aligned to 16 byte boundaries, since the
// size of a cache line is 16 bytes. This prevents cache tearing where the CPU
// and RSP try to use the same cache line and the CPU cache writeback overwrites
// data from the RSP which was on the same cache line.
static uint16_t g_framebuffer[2][SCREEN_W * SCREEN_H] __attribute__((aligned(64)));
#define RSP_STACK_SIZE (1024)
static uint64_t g_rsp_dram_stack[RSP_STACK_SIZE / sizeof(uint64_t)] __attribute__((aligned(16)));

// Viewport parameters
static Vp g_viewport =
{{
    .vscale = { SCREEN_W*2, SCREEN_H*2, G_MAXZ/2, 0},
    .vtrans = { SCREEN_W*2, SCREEN_H*2, G_MAXZ/2, 0},
}};

// RSP Initialization Display List
static Gfx g_displaylist_rsp_init[] = 
{
    gsSPViewport(&g_viewport),
    gsSPClearGeometryMode(  G_SHADE | G_SHADING_SMOOTH | G_CULL_BOTH | G_FOG |
                            G_TEXTURE_GEN_LINEAR | G_LOD),
    gsSPTexture(0, 0, 0, 0, G_OFF),
    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH),
    gsSPEndDisplayList()
};

// RDP Initialization Display List
static Gfx g_displaylist_rdp_init[] = 
{
    gsDPSetCycleType(G_CYC_1CYCLE),
    gsDPPipelineMode(G_PM_1PRIMITIVE),
    gsDPSetScissor(G_SC_NON_INTERLACE, 0, 0, SCREEN_W, SCREEN_H),
    gsDPSetTextureLOD(G_TL_TILE),
    gsDPSetTextureLUT(G_TT_NONE),
    gsDPSetTextureDetail(G_TD_CLAMP),
    gsDPSetTexturePersp(G_TP_PERSP),
    gsDPSetTextureFilter(G_TF_BILERP),
    gsDPSetTextureConvert(G_TC_FILT),
    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
    gsDPSetCombineKey(G_CK_NONE),
    gsDPSetAlphaCompare(G_AC_NONE),
    gsDPSetRenderMode(G_RM_OPA_SURF, G_RM_OPA_SURF2),
    gsDPSetColorDither(G_CD_DISABLE),
    gsDPPipeSync(),
    gsSPEndDisplayList(),
};

// Framebuffer Clear Display List
static Gfx g_displaylist_clear_framebuffer[] =
{
    gsDPSetCycleType(G_CYC_FILL),
    gsDPSetColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_W, g_framebuffer[0]),
    gsDPPipeSync(),
    gsDPSetFillColor(GPACK_RGBA5551(0,0,0,1) << 16 | GPACK_RGBA5551(0,0,0,1)),
    gsDPFillRectangle(0, 0, SCREEN_W-1, SCREEN_H-1),
    gsSPEndDisplayList()
};

// Vertex information for the Quad display list
static Vtx g_quad_vertices[] =
{
    //  x    y    z   flag   s   t   r     g     b     a
    { -64,  64,  -5,  0,     0,  0,  0xFF, 0xFF, 0x00, 0xFF },
    {  64,  64,  -5,  0,     0,  0,  0x00, 0xFF, 0x00, 0xFF },
    {  64, -64,  -5,  0,     0,  0,  0x00, 0x00, 0xFF, 0xFF },
    { -64, -64,  -5,  0,     0,  0,  0xFF, 0x00, 0x00, 0xFF },
};

static Vtx g_textured_quad_vertices[] =
{
    //  x    y    z   flag     s            t         r     g     b     a
    { -64,  64,  -5,  0,   (   0 << 6), (   0 << 6),  0xFF, 0xFF, 0xFF, 0xFF },
    {  64,  64,  -5,  0,   ( 127 << 6), (   0 << 6),  0xFF, 0xFF, 0xFF, 0xFF },
    {  64, -64,  -5,  0,   ( 127 << 6), ( 127 << 6),  0xFF, 0xFF, 0xFF, 0xFF },
    { -64, -64,  -5,  0,   (   0 << 6), ( 127 << 6),  0xFF, 0xFF, 0xFF, 0xFF },
};

// Draw Colored Quad Display List
static Gfx g_displaylist_draw_colored_quad[] = 
{
    gsSPMatrix(OS_K0_TO_PHYSICAL(&g_matrix_projection), 
                                 G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH),
    gsSPMatrix(OS_K0_TO_PHYSICAL(&g_matrix_modelview_solid),
                                 G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH),
    gsDPPipeSync(),
    gsDPSetCycleType(G_CYC_1CYCLE),
    gsDPSetRenderMode(G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2),
    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH),
    gsSPVertex(g_quad_vertices, 4, 0),
    gsSP1Triangle(0, 1, 2, 0),
    gsSP1Triangle(0, 2, 3, 0),
    gsSPEndDisplayList()
};

// Draw Colored Quad Display List
static Gfx g_displaylist_draw_textured_quad[] = 
{
    gsSPMatrix(OS_K0_TO_PHYSICAL(&g_matrix_projection), 
                                 G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH),
    gsSPMatrix(OS_K0_TO_PHYSICAL(&g_matrix_modelview_textured),
                                 G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH),
    gsDPPipeSync(),
    gsDPSetCycleType(G_CYC_1CYCLE),
    gsDPSetRenderMode(G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2),
    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH),
    
    gsSPTexture(0x4000, 0x4000, 0, G_TX_RENDERTILE, G_ON),
    gsDPSetCombineMode(G_CC_DECALRGB, G_CC_DECALRGB),
    gsDPSetTextureFilter(G_TF_BILERP),
    gsDPLoadTextureBlock(g_texture_brick, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0,
			 G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR,
			 5, 5, G_TX_NOLOD, G_TX_NOLOD),
             
    gsSPVertex(g_textured_quad_vertices, 4, 0),
    gsSP1Triangle(0, 1, 2, 0),
    gsSP1Triangle(0, 2, 3, 0),
    gsSPTexture(0, 0, 0, 0, G_OFF),
    gsSPEndDisplayList()
};


// Task to execute on the RSP 
static OSTask g_rsp_task = 
{{
    // RSP can do graphics or audio tasks. Choose graphics.
    .type = M_GFXTASK,     
    // Make RSP stall until RDP finishes whatever commands it may be processing.
    .flags = OS_TASK_DP_WAIT,
    // Specify the boot microcode - the size is not calculatable at load time
    // so initialize the boot microcode after the application begins
    .ucode_boot      = nullptr,
    .ucode_boot_size = 0,
    // Specify the microcode to run on the RSP
    .ucode           = (uint64_t*)gspF3DEX2_xbusTextStart,
    .ucode_size      = SP_UCODE_SIZE,
    .ucode_data      = (uint64_t*)gspF3DEX2_xbusDataStart,
    .ucode_data_size = SP_UCODE_DATA_SIZE,
    // Specify the stack in RAM for RSP to use
    .dram_stack      = g_rsp_dram_stack,
    .dram_stack_size = sizeof(g_rsp_dram_stack),
    // Output buffer - for XBUS microcode, RSP output goes directly to RDP
    // so we don't need these fields
    .output_buff      = nullptr,
    .output_buff_size = 0,
    // Task data fields, the application constructed display list. Fill in later.
    .data_ptr  = nullptr,
    .data_size = 0,
    // Yield data fields, buffer to store saved state of yielding task
    // We will only have one RSP task, so not needed
    .yield_data_ptr  = nullptr,
    .yield_data_size = 0
}};

// ============================================================================
// Forward Declarations/Prototypes
// ============================================================================
void idle_thread_func(void* args);
void main_thread_func(void* args);

// ============================================================================
// Application Code
// ============================================================================
void main(void)
{
    osInitialize();
    
    g_rom_handler = osCartRomInit();
    
    osCreateThread(&g_idle_thread_handle, // Thread handle
                   1,                     // User chosen ID
                   idle_thread_func,      // Thread function  
                   nullptr,               // Argument ptr to thread
                   _lg_idle_thread_stack, // Stack top (grows down)
                   10                     // Priority (1 to 127) (init nonzero)
                  );
    
    osStartThread(&g_idle_thread_handle);
} // never reached

void idle_thread_func(void* args)
{
    osCreateViManager(OS_PRIORITY_VIMGR);
    osViSetMode(&osViModeTable[OS_VI_NTSC_LAN1]);
    
    osCreateThread(&g_main_thread_handle, // Thread handle
                   2,                     // User chosen ID
                   main_thread_func,      // Thread function  
                   nullptr,               // Argument ptr to thread
                   _lg_main_thread_stack, // Stack top (grows down)
                   10                     // Priority (1 to 127) (init nonzero)
                  );
                  
    osStartThread(&g_main_thread_handle);

    osSetThreadPri(0, 0); // Drop thread priority and become true idle thread
    
    while(1) {}
}

void main_thread_func(void* args)
{
    // Set the RSP boot microcode parameters that weren't calculatable at load time.
    g_rsp_task.t.ucode_boot      = (uint64_t*)rspbootTextStart;
    g_rsp_task.t.ucode_boot_size = (uintptr_t)rspbootTextEnd - (uintptr_t)rspbootTextStart;
    
    // Setup the message queues for communicating the CPU with other components.
    osCreateMesgQueue(&g_messagequeue_rdp, &g_messagebuffer_rdp, 1);
    osCreateMesgQueue(&g_messagequeue_retrace, &g_messagebuffer_retrace, 1);
    // Associate the message queue with the RDP end of display interrupt
    osSetEventMesg(OS_EVENT_DP, &g_messagequeue_rdp, NULL);
    // Associate the message queue with the VI vertical retrace interrupt
    osViSetEvent(&g_messagequeue_retrace, NULL, 1);
    
    // Set up the display list command stream and task for the RSP
    Gfx  glist[MAX_DISPLAYLISTS] __attribute__((aligned(16))) = {0};
    Gfx* glistp = nullptr;

    // ========================================================================
    // Application main loop
    // ========================================================================
    while(1) 
    {
        static Mtx rotation;
        static Mtx scale;
        static float angle = 0.0f;
        angle += 2.0f;
        if(angle >= 360.0f) { 
            angle = 0.0f;
        }
        // Set up projection and modelview matrices for the scene
        guOrtho(&g_matrix_projection,
                -SCREEN_W/2.0f, SCREEN_W/2.0f, // left and right
                -SCREEN_H/2.0f, SCREEN_H/2.0f, // bottom and top
                1.0f, 10.0f,                   // near and far
                1.0f                           // scale factor for matrix elements for
               );                              // usage in improving fixed point precision
        
        guRotate(&rotation, angle, 0.0f, 0.0f, 1.0f);
        guScale(&scale, 0.67f, 0.67f, 1.0f);
        
        guTranslate(&g_matrix_modelview_solid, -64.0f, 0.0f, 0.0f);
        guMtxCatL(&scale, &g_matrix_modelview_solid, &g_matrix_modelview_solid);
        guMtxCatL(&rotation, &g_matrix_modelview_solid, &g_matrix_modelview_solid);
        
        guTranslate(&g_matrix_modelview_textured, 64.0f, 0.0f, 0.0f);
        guMtxCatL(&scale, &g_matrix_modelview_textured, &g_matrix_modelview_textured);
        guMtxCatL(&rotation, &g_matrix_modelview_textured, &g_matrix_modelview_textured);
        
        // Initialize graphics list pointer to beginning of command list
        glistp = glist;
        
        // Set up the RCP segments so it knows where memory is mapped.
        // First is the segment register number, 0-15.
        // Second is the physical address base.
        // With segment 0 equal to physical address 0x00000000, the RSP can access
        // objects in RAM when given a physical address.
        // Since the upper 4 bits of the address are ignored, there is an implicit
        // mapping from KSEG0 address space (starting 0x80000000) to physical address.
        // Thus, no conversion is needed, so just setup segment 0 to mirror KSEG0.
        gSPSegment(glistp++, 0, 0x00000000);
        
        // Initialize RDP
        gSPDisplayList(glistp++, g_displaylist_rdp_init);
        
        // Initialize RSP
        gSPDisplayList(glistp++, g_displaylist_rsp_init);
        
        // Clear framebuffer - choose the active framebuffer
        g_displaylist_clear_framebuffer[1] = 
            (Gfx)gsDPSetColorImage( G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_W,
                                    g_framebuffer[g_active_framebuffer]);
        gSPDisplayList(glistp++, g_displaylist_clear_framebuffer);
        
        // Render a colored quad! :)
        gSPDisplayList(glistp++, g_displaylist_draw_colored_quad);
        
        // Render a textured quad! :)
        gSPDisplayList(glistp++, g_displaylist_draw_textured_quad);
        
        // Generate event to signal application when RDP pipeline has completed
        gDPFullSync(glistp++);
        
        // End graphics display list, make RSP pop stack until empty. End of task.
        gSPEndDisplayList(glistp++);
        
        // Update task data fields to tell the RSP where the display list is.
        g_rsp_task.t.data_ptr  = (uint64_t*)glist;
        g_rsp_task.t.data_size = sizeof(Gfx) * (glistp - glist);
        
        // Synchronize CPU cache to RAM so the RSP can access updated display lists.
        osWritebackDCacheAll();
        
        // Send the task to the RSP and finally get some graphics action going on!!
        osSpTaskStart(&g_rsp_task);
        
        // Block until the RDP has finished rendering
        osRecvMesg(&g_messagequeue_rdp, nullptr, OS_MESG_BLOCK);
 
        // Register framebuffer to be displayed at next retrace
        osViSwapBuffer(g_framebuffer[g_active_framebuffer]);
        
        // Draw Banner
        memcpy(g_framebuffer[g_active_framebuffer], g_texture_banner, 320*32*2);
        // Flush cache back to RAM so the VI is aware of framebuffer change
        osWritebackDCacheAll();

        // Wait for retrace to finish
        osRecvMesg(&g_messagequeue_retrace, nullptr, OS_MESG_BLOCK);
        
        // Swap active framebuffer for double buffering
        g_active_framebuffer ^= 1;
    }
}

