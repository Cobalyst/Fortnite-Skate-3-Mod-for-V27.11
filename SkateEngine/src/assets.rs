//! Skater rig (rig.json) and board model (board.json) from the converted skate-data,
//! prepared in Unreal space for the mod: bind poses per bone, and the board split into
//! rigid parts (deck, trucks, wheels) in their bone's local space.

use crate::convert::{mul, skate_dir_to_ue, skate_matrix_to_ue, skate_to_ue, transform_point, transform_vector};
use serde::Deserialize;
use std::path::Path;

#[derive(Deserialize)]
struct RigBone {
    name: String,
    bind: [f32; 16],
    inverse_bind: [f32; 16],
}

#[derive(Deserialize)]
struct BoardJoint {
    target: String,
}

#[derive(Deserialize)]
struct BoardVertex {
    position: [f32; 3],
    normal: [f32; 3],
    uv: u32,
    joints: [usize; 4],
    weights: [f32; 4],
}

#[derive(Deserialize)]
struct BoardSurface {
    texture: u32,
    vertices: Vec<BoardVertex>,
    indices: Vec<u32>,
}

#[derive(Deserialize)]
struct BoardModel {
    joints: Vec<BoardJoint>,
    surfaces: Vec<BoardSurface>,
}

/// One rigid board piece with one texture, in its bone's local space (Unreal cm).
pub struct BoardPart {
    pub bone: u32,
    pub texture: u32,
    pub positions: Vec<f32>,
    pub normals: Vec<f32>,
    pub uvs: Vec<f32>,
    pub indices: Vec<i32>,
}

pub struct Assets {
    /// Bind pose (Unreal space) per pose bone, if the rig has that bone.
    pub binds: Vec<Option<[f32; 16]>>,
    pub board: Vec<BoardPart>,
}

pub fn load(root: &Path, bone_names: &[String]) -> Result<Assets, String> {
    let rig: Vec<RigBone> = serde_json::from_slice(
        &std::fs::read(root.join("rig.json")).map_err(|e| format!("rig.json: {e}"))?,
    )
    .map_err(|e| format!("rig.json: {e}"))?;
    let rig_bone = |name: &str| rig.iter().find(|b| b.name == name);

    let binds = bone_names
        .iter()
        .map(|n| rig_bone(n).map(|b| skate_matrix_to_ue(&b.bind)))
        .collect();

    let model: BoardModel = serde_json::from_slice(
        &std::fs::read(root.join("board.json")).map_err(|e| format!("board.json: {e}"))?,
    )
    .map_err(|e| format!("board.json: {e}"))?;

    // As in IW4L's board renderer: world = bone * rb * inverse_bind * vertex (skate space).
    let rb: [f32; 16] = [1., 0., 0., 0., 0., 0., -1., 0., 0., 1., 0., 0., 0., 0., 0., 1.];

    let mut board: Vec<BoardPart> = Vec::new();
    for surface in &model.surfaces {
        // Group this surface's vertices by the single bone each one follows.
        let mut remap = vec![usize::MAX; surface.vertices.len()];
        let mut part_of = vec![usize::MAX; surface.vertices.len()];
        for (vi, v) in surface.vertices.iter().enumerate() {
            let joint = v
                .joints
                .iter()
                .zip(v.weights)
                .max_by(|a, b| a.1.total_cmp(&b.1))
                .map(|(j, _)| *j)
                .unwrap_or(0);
            let target = &model.joints[joint].target;
            let Some(bone) = bone_names.iter().position(|n| n == target) else {
                return Err(format!("board joint {target} is not a skater bone"));
            };
            let rig = rig_bone(target).ok_or_else(|| format!("board joint {target} is not in rig.json"))?;
            let local = mul(&rb, &rig.inverse_bind);

            let part = match board.iter().position(|p| p.bone == bone as u32 && p.texture == surface.texture) {
                Some(p) => p,
                None => {
                    board.push(BoardPart {
                        bone: bone as u32,
                        texture: surface.texture,
                        positions: Vec::new(),
                        normals: Vec::new(),
                        uvs: Vec::new(),
                        indices: Vec::new(),
                    });
                    board.len() - 1
                }
            };
            let p = &mut board[part];
            remap[vi] = p.positions.len() / 3;
            part_of[vi] = part;
            p.positions.extend(skate_to_ue(transform_point(&local, v.position)));
            p.normals.extend(skate_dir_to_ue(transform_vector(&local, v.normal)));
            // Packed half floats: high 16 bits U (along the deck), low 16 bits V. The deck's top
            // samples V 0..0.5 (grip tape) and its bottom V 0.5..1 (the graphic).
            let u = half::f16::from_bits((v.uv >> 16) as u16).to_f32();
            let w = half::f16::from_bits((v.uv & 0xFFFF) as u16).to_f32();
            p.uvs.extend([u, w]);
        }
        for t in surface.indices.chunks_exact(3) {
            let (a, b, c) = (t[0] as usize, t[1] as usize, t[2] as usize);
            let part = part_of[a];
            if part_of[b] != part || part_of[c] != part {
                continue; // a triangle spanning two rigid parts: none in the stock board
            }
            // Skate -> Unreal flips handedness, so flip the winding too.
            board[part].indices.extend([remap[a] as i32, remap[c] as i32, remap[b] as i32]);
        }
    }
    Ok(Assets { binds, board })
}
