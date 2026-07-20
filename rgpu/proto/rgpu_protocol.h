/* rgpu remote-graphics protocol — versioned wire contract.
 *
 * Shared by the Windows D3D11 proxy (frontend) and the renderer backend
 * (rgpu-renderd: D3D11 reference on Windows, Vulkan on Linux/Colab). The
 * protocol expresses D3D11 *semantics* so a Vulkan backend can implement them
 * independently — it is NOT a forwarding of Windows function pointers.
 *
 * Transport (prototype): one authenticated WebSocket over TLS carrying framed
 * messages of the types below. The renderer dials OUT to a stable rendezvous
 * (Colab's address is ephemeral and must never be baked into the client).
 */
#ifndef RGPU_PROTOCOL_H
#define RGPU_PROTOCOL_H
#include <stdint.h>

#define RGPU_PROTOCOL_VERSION 1u
#define RGPU_MAGIC 0x52475055u /* 'RGPU' */

/* ---- message (frame) types ---------------------------------------------- */
typedef enum {
    RGPU_MSG_HELLO            = 1,  /* client<->server handshake + version */
    RGPU_MSG_CAPS_OFFER       = 2,  /* server -> client: negotiated remote adapter caps */
    RGPU_MSG_CAPS_REQUIRE     = 3,  /* client -> server: minimum caps the game needs */
    RGPU_MSG_SESSION_OPEN     = 4,
    RGPU_MSG_SESSION_CLOSE    = 5,
    RGPU_MSG_COMMAND_BATCH    = 6,  /* ordered array of RGPU_CMD_* records */
    RGPU_MSG_RESOURCE_UPLOAD  = 7,  /* bulk resource bytes (textures/buffers) */
    RGPU_MSG_QUERY            = 8,  /* client -> server: synchronous query */
    RGPU_MSG_QUERY_RESULT     = 9,  /* server -> client: synchronous reply */
    RGPU_MSG_FRAME_CHUNK      = 10, /* server -> client: encoded frame bytes */
    RGPU_MSG_HEARTBEAT        = 11,
    RGPU_MSG_ERROR            = 12,
    RGPU_MSG_DEVICE_LOST      = 13  /* server -> client: remote GPU gone; fail closed */
} rgpu_msg_type;

/* Every framed message starts with this fixed header (little-endian). */
typedef struct {
    uint32_t magic;        /* RGPU_MAGIC */
    uint16_t version;      /* RGPU_PROTOCOL_VERSION */
    uint16_t type;         /* rgpu_msg_type */
    uint32_t session_id;   /* 0 before SESSION_OPEN */
    uint32_t sequence;     /* per-session monotonic; preserves command order */
    uint32_t payload_len;  /* bytes following this header */
} rgpu_frame_header;

/* ---- D3D11-semantic command opcodes (inside RGPU_MSG_COMMAND_BATCH) ------ */
typedef enum {
    RGPU_CMD_CREATE_BUFFER            = 100,
    RGPU_CMD_CREATE_TEXTURE_2D        = 101,
    RGPU_CMD_CREATE_SHADER            = 102, /* VS/PS/CS; stage in record */
    RGPU_CMD_CREATE_RENDER_TARGET_VIEW= 103,
    RGPU_CMD_CREATE_DEPTH_STENCIL_VIEW= 104,
    RGPU_CMD_CREATE_SRV               = 105,
    RGPU_CMD_CREATE_INPUT_LAYOUT      = 106,
    RGPU_CMD_SET_PIPELINE_STATE       = 120, /* blend/raster/depth/topology/shaders */
    RGPU_CMD_SET_VERTEX_BUFFERS       = 121,
    RGPU_CMD_SET_INDEX_BUFFER         = 122,
    RGPU_CMD_SET_RENDER_TARGETS       = 123,
    RGPU_CMD_SET_VIEWPORTS            = 124,
    RGPU_CMD_UPDATE_RESOURCE          = 140, /* UpdateSubresource / Map+write */
    RGPU_CMD_COPY_RESOURCE            = 141,
    RGPU_CMD_CLEAR_RTV                = 142,
    RGPU_CMD_CLEAR_DSV                = 143,
    RGPU_CMD_RESOURCE_BARRIER         = 144, /* D3D12 barriers (state transitions) */
    RGPU_CMD_DRAW                     = 160,
    RGPU_CMD_DRAW_INDEXED             = 161,
    RGPU_CMD_DISPATCH                 = 162, /* compute */
    RGPU_CMD_PRESENT                  = 180, /* swapchain present -> encode+return frame */
    RGPU_CMD_EXECUTE_COMMAND_LISTS    = 182  /* D3D12 submission boundary (queue) */
} rgpu_cmd_op;

/* Each command in a batch is a length-prefixed record: */
typedef struct {
    uint32_t op;           /* rgpu_cmd_op */
    uint32_t handle;       /* resource/object id (frontend-assigned, stable) */
    uint32_t arg_len;      /* bytes of op-specific args following */
    /* uint8_t args[arg_len]; */
} rgpu_cmd_record;

/* Negotiated adapter caps advertised by CAPS_OFFER and surfaced by the
 * synthetic DXGI adapter. Only capabilities the backend genuinely supports. */
typedef struct {
    char     description[128]; /* e.g. "Remote GPU - NVIDIA T4" */
    uint32_t vendor_id;        /* 0x10DE NVIDIA */
    uint32_t device_id;
    uint64_t dedicated_vram;   /* bytes */
    uint32_t d3d_feature_level;/* 0xb000=11_0, 0xb100=11_1 */
    uint32_t max_shader_model; /* 0x50=SM5.0 */
    uint32_t supports_compute; /* bool */
    uint32_t max_msaa;         /* max sample count */
    uint32_t location_remote;  /* always 1 */
} rgpu_adapter_caps;

#endif /* RGPU_PROTOCOL_H */
