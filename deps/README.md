# Vendored native dependencies (M1-01)

Checked in directly (no submodules) so clean clones build offline after
checkout. **Do not hand-edit vendored trees** — upgrades are whole-tree
replacements, recorded below.

| Dep    | Version pinned | Upstream              | Notes |
|--------|---------------|-----------------------|-------|
| flecs  | `v4.1.6` (`fb55f3c2`) | https://github.com/SanderMertens/flecs | C99/C++ archetype ECS; built **shared** (`FLECS_SHARED=ON`, `FLECS_TESTS=OFF`) — single per-process universe (see `native/CMakeLists.txt`) |
| enkiTS | `master@404a3bf8` | https://github.com/dougbinks/enkiTS | C++ task scheduler; static lib, C interface ON (M1-03 binds it to Flecs task hooks), examples OFF |

Re-vendor recipe:

```bash
git clone --depth 1 --branch v4.1.6 https://github.com/SanderMertens/flecs.git deps/flecs
git clone --depth 1 https://github.com/dougbinks/enkiTS.git deps/enkiTS
rm -rf deps/flecs/.git deps/enkiTS/.git
# then update the pin table above with the new tag/SHA
```
