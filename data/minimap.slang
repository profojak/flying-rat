/* Minimap overlay shader. */

// Push constants for the minimap overlay.
// NOTE: Must match `MinimapPushConstants` in `src/renderer.cppm`.
struct MinimapPush {
  // Top-left corner of the grid area in NDC.
  float2 origin;
  // Size of one cell in NDC.
  float2 cell;
  // Maze dimensions in tiles.
  int2 grid;
  // Player tile coordinates.
  int2 player;
  // Start tile coordinates.
  int2 start;
  // Exit tile coordinates.
  int2 exit;
};

[[vk::push_constant]]
MinimapPush push;

// One cell type per maze tile, row-major: 0 = wall, 1 = empty.
[[vk::binding(1, 0)]]
StructuredBuffer<uint> cells;

struct VSOutput {
  // Clip-space position of the vertex.
  float4 position : SV_Position;
  // Cell kind: -1 = panel, 0 = wall, 1 = floor, 2 = start, 3 = exit,
  // 4 = player.
  float kind : TEXCOORD1;
};

// Return corner of a unit quad, winding counter-clockwise.
float2 QuadCorner(uint corner) {
  if (corner == 0)
    return float2(0.0f, 0.0f);
  if (corner == 1)
    return float2(1.0f, 0.0f);
  if (corner == 2)
    return float2(1.0f, 1.0f);
  return float2(0.0f, 1.0f);
}

// Return true when two cells are equal.
bool SameCell(int2 a, int2 b) { return a.x == b.x && a.y == b.y; }

// Entry point for the vertex shader.
[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID,
                    uint instanceID : SV_InstanceID) {
  VSOutput output;

  // 1 quad x 2 triangles x 3 vertices = 6 vertices per instance.
  uint v = vertexID % 6;

  // Quad (0, 1, 2, 3) as triangles (0, 1, 2) + (0, 2, 3).
  uint corner = (v == 3) ? 0 : ((v == 4) ? 2 : ((v == 5) ? 3 : v));
  float2 c = QuadCorner(corner);

  if (instanceID == 0) {
    float2 pad = push.cell * 0.75f;
    float2 size =
        float2(push.cell.x * float(push.grid.x),
               push.cell.y * float(push.grid.y));
    float2 p = push.origin - pad + (size + pad * 2.0f) * c;
    output.position = float4(p, 0.0f, 1.0f);
    output.kind = -1.0f;
    return output;
  }

  uint index = instanceID - 1u;
  int2 cell = int2(index % uint(push.grid.x),
                   index / uint(push.grid.x));
  float2 p = push.origin + (float2(cell) + c) * push.cell;
  output.position = float4(p, 0.0f, 1.0f);

  // Player takes precedence so it stays visible on start and exit tiles.
  if (SameCell(cell, push.player)) {
    output.kind = 4.0f;
  } else if (SameCell(cell, push.start)) {
    output.kind = 2.0f;
  } else if (SameCell(cell, push.exit)) {
    output.kind = 3.0f;
  } else if (cells[index] == 0) {
    output.kind = 0.0f;
  } else {
    output.kind = 1.0f;
  }
  return output;
}

// Entry point for the fragment shader.
[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target {
  if (input.kind < -0.5f) {
    return float4(0.03f, 0.04f, 0.06f, 1.0f);
  }
  if (input.kind < 0.5f) {
    return float4(0.16f, 0.17f, 0.21f, 1.0f);
  }
  if (input.kind < 1.5f) {
    return float4(0.72f, 0.73f, 0.76f, 1.0f);
  }
  if (input.kind < 2.5f) {
    return float4(0.20f, 0.75f, 0.35f, 1.0f);
  }
  if (input.kind < 3.5f) {
    return float4(0.90f, 0.28f, 0.22f, 1.0f);
  }

  // Player tile highlight.
  return float4(1.0f, 0.85f, 0.20f, 1.0f);
}
