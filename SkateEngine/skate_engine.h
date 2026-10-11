// C interface of skate_engine.dll (see SkateEngine/src/lib.rs).
// Unreal units: centimetres, X forward, Y right, Z up, yaw in degrees.
// Matrices are 4x4 column-major (16 floats), translation in cm.
#pragma once
#include <cstdint>

extern "C" {

struct SkEngine;

// XInput-style virtual pad, used only when no real controller is connected.
struct SkPad
{
    uint16_t buttons;
    uint8_t  triggers[2];
    int16_t  left[2];
    int16_t  right[2];
};

struct SkState
{
    float    position[3];  // skater root (feet), cm
    float    velocity[3];  // board velocity, cm/s
    float    yaw;          // facing, degrees
    float    root[16];     // root transform
    uint64_t tick;
    uint8_t  controller;   // 1 while a real XInput pad drives the skater
    char     state[48];    // Skate 3 player state name
    uint8_t  camera_valid;       // 1 when the Skate 3 camera below is valid
    float    camera_position[3]; // Skate 3 camera (Unreal space): position in cm,
    float    camera_forward[3];  // look direction,
    float    camera_up[3];       // up,
    float    camera_fov;         // field of view in degrees
};

struct SkHud
{
    float sequence;    // points of the trick sequence in progress
    float line;        // points banked into the current line
    float line_timer;  // 0..1, the line ends when it runs out
    float multiplier;
    float total;       // all completed lines this session
    float line_seconds; // seconds left on the line timer
    uint8_t clean;     // landing quality of the last sequence
    uint8_t sketchy;
    uint8_t switch_stance; // stance of the current trick
    uint8_t fakie;
    uint8_t sequence_active; // a trick sequence is in progress
    float   board_spin[3];   // deck angular velocity in its own frame, rad/s (x side, y up, z along)
    uint8_t hom_active;      // Hall of Meat: a bail is being scored
    uint8_t hom_shown;       //   there is a bail to show (current or last)
    float   hom_score;       //   running total
    float   hom_metrics[5];  //   speed m/s, drop m, air s, bail time s, rotation degrees
    float   hom_points[5];   //   points of each
    int8_t  hom_levels[25];  //   bone injury levels (-1 unhurt, 0 scraped .. 5 broken), HoM bone order
    uint8_t marker_visible;     // session marker prompt: LB held
    uint8_t marker_can_place;   //   a marker can be set here
    uint8_t marker_can_return;  //   the marker can be gone to
    uint8_t marker_set;         //   a marker exists
    float   marker_progress;    //   go-to hold progress 0..1
    uint32_t marker_placed;     //   counters: markers set,
    uint32_t marker_refused;    //   placements refused,
    uint32_t marker_returned;   //   returns made
};

enum : uint8_t
{
    SK_HUD_TRICK = 1,     // new trick announced (text = name, value = sequence points)
    SK_HUD_MODIFIED = 2,  // the current trick changed (text = new name)
    SK_HUD_BAIL = 3,
    SK_HUD_LINE = 4,      // line banked (value = points)
};

struct SkHudEvent
{
    uint8_t kind;
    float   value;
    char    text[64];
};

// A rigid piece of the Skate 3 board in its bone's local space.
struct SkBoardPart
{
    uint32_t     bone;          // index into the bone list
    uint32_t     texture;       // skate-data/board/tex{texture}.png
    uint32_t     vertex_count;
    uint32_t     index_count;
    const float* positions;     // vertex_count * 3
    const float* normals;       // vertex_count * 3
    const float* uvs;           // vertex_count * 2
    const int32_t* indices;
};

// XInput button bits used by the virtual pad
enum : uint16_t
{
    SK_PAD_START = 0x0010,
    SK_PAD_LB    = 0x0100,
    SK_PAD_RB    = 0x0200,
    SK_PAD_A     = 0x1000,
    SK_PAD_B     = 0x2000,
    SK_PAD_X     = 0x4000,
    SK_PAD_Y     = 0x8000,
};

typedef SkEngine*   (*sk_create_fn)(const char* assets_root);
typedef bool        (*sk_set_collision_fn)(SkEngine*, const float* triangles, uint32_t count,
                                           const float* rail_points, const uint32_t* rail_lengths, uint32_t rail_count);
/* sk_set_collision plus native Skate 3 triangles (placed dropper objects): native_count triangles (9 floats,
   Unreal cm, same winding as triangles), 3 RW edge codes each (edge i = corner i -> i+1), u16 surface id each. */
typedef bool        (*sk_set_collision2_fn)(SkEngine*, const float* triangles, uint32_t count,
                                            const float* rail_points, const uint32_t* rail_lengths, uint32_t rail_count,
                                            const float* native_triangles, const uint8_t* native_edges,
                                            const uint16_t* native_surfaces, uint32_t native_count);
typedef uint64_t    (*sk_collision_generation_fn)(const SkEngine*);
// velocity: Unreal cm/s the skater starts with (may be null)
typedef bool        (*sk_activate_fn)(SkEngine*, const float* position, float yaw, const float* velocity, SkState* out);
typedef bool        (*sk_step_fn)(SkEngine*, float dt, const SkPad* pad, SkState* out);
typedef bool        (*sk_suspend_fn)(SkEngine*);
typedef bool        (*sk_set_paused_fn)(SkEngine*, bool paused);  // freeze / resume the active skater

// Object-dropper DMOs (rigid bodies in the board's island). Pose: Unreal cm + quaternion; model
// data: Skate 3's own (model space, metres, Y up) from the object's files.
typedef struct SkDmo
{
    uint32_t id, kinematic;
    float position[3], rotation[4];
    float inverse_tensor[3], inverse_mass, spherical, maximum_linear_velocity, maximum_angular_velocity, linear_drag, angular_drag;
    float inverse_body_rows[9], inverse_body_translation[3];
    float material[3];  // static friction, dynamic friction, restitution
    uint32_t first_volume, volume_count, first_triangle, triangle_count;
    uint32_t first_spline, spline_count;
} SkDmo;
// A grab spline (the object's RW 0xEB001F spline: points, approach vector, header word +0x3C).
typedef struct SkDmoSpline { uint32_t first_point, point_count; float approach[3]; uint32_t approach_count, word_60; } SkDmoSpline;
typedef struct SkDmoVolume { uint32_t kind; float rows[9], position[3], params[4]; } SkDmoVolume;
typedef struct SkDmoTriangle { float points[9]; uint8_t edges[3], pad; uint16_t surface, pad2; } SkDmoTriangle;
// awake: bit 0 simulating, bit 1 held by the skater (on-foot grab, OffBoardPushing).
typedef struct SkDmoPose { uint32_t id, awake; float position[3], rotation[4]; } SkDmoPose;
typedef bool        (*sk_set_dmos_fn)(SkEngine*, const SkDmo*, uint32_t count, const SkDmoVolume*, uint32_t volume_count,
                                      const SkDmoTriangle*, uint32_t triangle_count, const SkDmoSpline*, uint32_t spline_count,
                                      const float* points, uint32_t point_count);
typedef uint32_t    (*sk_get_dmo_poses_fn)(const SkEngine*, SkDmoPose* out, uint32_t max);
typedef bool        (*sk_upright_dmo_fn)(SkEngine*, uint32_t id);
typedef uint32_t    (*sk_bone_count_fn)(const SkEngine*);
typedef const char* (*sk_bone_name_fn)(const SkEngine*, uint32_t index);
typedef bool        (*sk_bone_bind_fn)(const SkEngine*, uint32_t index, float* out16);
typedef uint32_t    (*sk_get_bones_fn)(const SkEngine*, float* out, uint32_t max);
// Skate 3's physics-side audio data (retail per-skater audio record fields) after the last tick.
struct SkAudio
{
    uint64_t tick;
    uint8_t  air, grinding, trick_active, state_flag_80;
    int32_t  trick_id;          // scoring identifier index (sk_trick_identifier), -1 none
    float    airtime;           // seconds in the air state
    float    pop_strength;      // 0..1 while popping, -1 otherwise
    float    ground_speed;      // m/s
    uint8_t  wheel_flag[4];     // wheel touched down after > 5 frames off the ground
    float    wheel_impact[4];   // 0..1
    int32_t  grind_family;
    float    grind_force;
    int32_t  grind_surface;
    float    side_slip, side_slip_rise;
    float    region_force[8];   // skeleton regions: head, torso, l/r arm, l/r leg, l/r foot (0..1)
    float    region_tangent[8];
    uint32_t region_material[8];
    float    skater_force, group_8_force;
    uint8_t  state_flags;       // bit0 State52, 1 State54, 2 State55 (landing), 3 State56, 4 State57, 5 State60 (manual)
    uint8_t  wheel_count;       // wheels touching the ground
    float    com_speed;         // skater centre-of-mass speed (m/s)
    float    carve_turn;        // Ground+0x108 (-Turn): carve weight steering factor
    float    deck_tilt;         // truck steering deck tilt (squeaks)
    float    deck_roll_rate;    // deck-local angular velocity about the deck's forward axis (rad/s)
    uint8_t  feet_on_deck;      // bit0 / bit1: front / back foot within the deck box
    float    wheel_position[4][3];  // skate space metres (x, up, z)
    uint8_t  board_slide;       // deck scraping in a wipeout / off the board: 1 upside down, 2 on its side
};
typedef bool        (*sk_get_audio_fn)(const SkEngine*, SkAudio* out);
typedef const char* (*sk_trick_identifier_fn)(int32_t id);
typedef bool        (*sk_get_hud_fn)(const SkEngine*, SkHud* out);
typedef bool        (*sk_pop_hud_event_fn)(SkEngine*, SkHudEvent* out);
typedef uint32_t    (*sk_board_part_count_fn)(const SkEngine*);
typedef bool        (*sk_board_part_fn)(const SkEngine*, uint32_t index, SkBoardPart* out);
typedef const char* (*sk_last_error_fn)();
typedef bool        (*sk_get_pad_debug_fn)(const SkEngine*, int32_t* out12);  // last controller poll (diagnostics)
typedef bool        (*sk_get_camera_debug_fn)(uint32_t* out4);  // camera probes since last call (diagnostics)
typedef bool        (*sk_set_option_fn)(const char* name, int value);  // "hall_of_meat" 0/1

}
