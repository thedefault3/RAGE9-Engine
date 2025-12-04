# Advanced Single-File C++ Game Engine (Separated Files)
- ESC — Quit


---


## 🔍 Features & Design Notes


### Engine Core
- **Fixed timestep physics** (configurable) for deterministic updates
- **Variable rendering** for smooth frames
- **Delta time** calculations and accumulator
- **Input system** wrapping SDL events


### ECS & Components
- `TransformComponent` — position, rotation, scale
- `SpriteComponent` — texture reference + source rectangle
- `PhysicsComponent` — velocity, acceleration, mass
- `ColliderComponent` — AABB collider for collision detection
- `ScriptComponent` — C++ lambda callbacks for simple behavior
- `CameraComponent` — simple camera following the player


### Systems
- Rendering system (sprite + camera)
- Physics system (integrates velocities)
- Collision system (AABB detection and resolution)
- Script system (per-entity callbacks)
- Particle system


### Resources
- Texture loading with caching
- Audio loading with caching (if SDL_mixer available)


### Demo Scene
- Player entity with input-driven movement and jump
- Tilemap ground with many tiles (AABB collisions)
- Background music and SFX (if audio libs present)
- HUD showing FPS and entity count


---


## ⚠️ Legal & Ethical
This engine is for educational purposes and harmless. Do not use it to create or distribute content that violates laws or the terms of service of platforms.


---


## 📚 Extending this project (ideas)
- Add texture atlasing and sprite animation
- Add CMake build files
- Integrate Lua scripting (sol2)
- Add Tiled (.tmx) map loading
- Add Box2D for advanced physics
- Add ImGui-based in-engine editor


---


## License
MIT License — see LICENSE (or the license block at top of `engine_full.cpp`).


---
