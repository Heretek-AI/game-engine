# Milestone 4: Deterministic Jolt Physics & Spatial Audio

* **Timeline:** Months 8–9
* **Verification Gate:** Simulate $30,000$ active colliding rigidbodies with zero-allocation C# collision callbacks and spatialized 3D audio running at a locked $60+\text{ FPS}$.

---

## GitHub Issues Breakdown

### Issue M4-01: Jolt Physics Integration & Shared Work-Stealing Thread Pool
* **Labels:** `area:physics-audio`, `priority:critical`
* **Description:** Vendor and integrate `JoltPhysics` in `native/physics/`, routing its job execution directly into the engine's `enkiTS` scheduler so ECS and Physics share the same worker threads.
* **Acceptance Criteria:**
  - [ ] Implement `Jolt::JobSystem` wrapper backed by the engine's `enkiTS` scheduler.
  - [ ] Configure `BroadPhaseLayerInterface` (`Static`, `Dynamic`, `Trigger`, `Debris`) and custom `TempAllocator` backed by the engine's `PageArenaAllocator`.
  - [ ] Define native ECS components: `RigidBody`, `BoxCollider`, `SphereCollider`, `CapsuleCollider`, `MeshCollider`, and `CharacterController`.

---

### Issue M4-02: Zero-Glue Parallel Flecs <-> Jolt Transform Synchronization
* **Labels:** `area:physics-audio`, `area:native-core`
* **Description:** Synchronize active rigidbody transforms between Jolt Physics islands and Flecs `LocalTransform` tables in parallel without per-object virtual calls.
* **Acceptance Criteria:**
  - [ ] Before `PhysicsSystem::Update()`, push user-modified kinematic/dynamic transforms from Flecs to Jolt via `BodyInterface::SetPositionAndRotationWhenChanged`.
  - [ ] After `PhysicsSystem::Update()`, iterate active (non-sleeping) Jolt bodies in parallel and write updated positions/rotations directly into Flecs `LocalTransform` columns.
  - [ ] Buffer collision/trigger events (`OnCollisionEnter`, `OnCollisionExit`, `OnTriggerEnter`) into a lock-free native ring buffer and dispatch them to C# systems as contiguous `ReadOnlySpan<CollisionEvent>` batches.

---

### Issue M4-03: Zero-Cost C# Spatial Queries (`Physics.Raycast` / `OverlapSphere`)
* **Labels:** `area:physics-audio`, `area:abi-bridge`
* **Description:** Expose Jolt's spatial queries to C# via `[SuppressGCTransition]` unmanaged function pointers for sub-microsecond raycasting.
* **Acceptance Criteria:**
  - [ ] Expose `Physics.Raycast`, `Physics.SphereCast`, and `Physics.OverlapSphere` in `Engine.Runtime`.
  - [ ] Add batch raycast API (`Physics.RaycastBatch(ReadOnlySpan<Ray> rays, Span<RaycastHit> hits)`) that executes across native worker threads in parallel.

---

### Issue M4-04: Low-Latency 3D Spatial Audio Graph (`miniaudio`)
* **Labels:** `area:physics-audio`
* **Description:** Integrate `miniaudio` in `native/audio/` and bind `AudioSource` and `AudioListener` components to Flecs `WorldTransform` tables.
* **Acceptance Criteria:**
  - [ ] Initialize `ma_engine` and `ma_resource_manager` for lock-free streaming of `.wav`, `.ogg`, and `.mp3` assets.
  - [ ] Implement a native Flecs system that updates 3D spatialization cones, Doppler velocity, and attenuation from `WorldTransform` components every frame.