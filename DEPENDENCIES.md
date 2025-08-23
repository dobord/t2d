# Dependency Manifest

Authoritative list of pinned third-party components vendored as git submodules (see `third_party/`). CI jobs may validate these SHAs.

| Name | Path | Upstream Ref / Tag | Commit SHA |
|------|------|--------------------|------------|
| box2d | third_party/box2d | v3.1.1 | 8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3 |
| libcoro | third_party/libcoro | heads/fix/skip_linking_pthread_on_android | 09ac9b8c1ce288b8c36b64db27a4eff81d51ab9a |
| yaml-cpp | third_party/yaml-cpp | 0.8.0 | f7320141120f720aecc4c32be25586e7da9eb978 |

Generated on: 2025-08-23

## Validation (Future)
A CI job will compare current `git submodule status` against this table and fail if drift is detected. For automation, a script will output a normalized table and diff against this file.
