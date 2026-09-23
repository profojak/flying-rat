/* Floor shader. */

// Per-frame UBO.
struct FrameUniforms {
  // View-projection matrix.
  float4x4 view_projection;
  // Light direction in world space.
  float3 light_direction;
  // Ambient light intensity.
  float ambient;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame;

// One tile coordinate per floor instance.
[[vk::binding(1, 0)]]
StructuredBuffer<float2> tiles;

struct PushConstants {
  // Tile size in world units.
  float tile_size;
  // Wall height in world units.
  float wall_height;
};

[[vk::push_constant]]
PushConstants push;

struct VSOutput {
  // World-space position of the vertex.
  float4 position : SV_Position;
  // World-space normal of the vertex.
  float3 world_normal : NORMAL;
  // UV coordinates of the vertex.
  float2 uv : TEXCOORD0;
};

// Floor normal, facing up.
float3 FloorNormal() {
  return float3(0.0f, 1.0f, 0.0f);
}

// Return corner of the unit box floor face, winding counter-clockwise.
float3 FloorCorner(uint corner) {
  float h = 0.5f;
  float y = -h;
  if (corner == 0)
    return float3(-h, y, h);
  if (corner == 1)
    return float3(h, y, h);
  if (corner == 2)
    return float3(h, y, -h);
  return float3(-h, y, -h);
}

// Entry point for the vertex shader.
[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID,
                    uint instanceID : SV_InstanceID) {
  VSOutput output;

  // 1 floor face x 2 triangles x 3 vertices = 6 vertices per instance.
  uint v = vertexID % 6;

  // Quad (0, 1, 2, 3) as triangles (0, 1, 2) + (0, 2, 3).
  uint corner = (v == 3) ? 0 : ((v == 4) ? 2 : ((v == 5) ? 3 : v));

  float3 p = FloorCorner(corner);

  // Scale the unit corner to a world box.
  float3 box = float3(p.x * push.tile_size,
                      (p.y + 0.5f) * push.wall_height,
                      p.z * push.tile_size);

  // Center tile in world space.
  float2 tile = tiles[instanceID];
  float3 origin = float3((tile.x + 0.5f) * push.tile_size,
                         0.0f,
                         (tile.y + 0.5f) * push.tile_size);

  float3 world = origin + box;
  output.position = mul(frame.view_projection, float4(world, 1.0f));
  output.world_normal = FloorNormal();

  output.uv = float2(((corner == 1u || corner == 2u) ? 1.0f : 0.0f),
                     (corner >= 2) ? 1.0f : 0.0f);
  return output;
}

// Entry point for the fragment shader.
[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target {
  float3 n = normalize(input.world_normal);
  float diffuse = max(dot(n, -normalize(frame.light_direction)), 0.0f);
  float3 albedo = float3(0.36f, 0.38f, 0.21f);
  return float4(albedo * (frame.ambient + diffuse), 1.0f);
}
