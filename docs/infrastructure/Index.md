# Infrastructure

> Build system, CI/CD pipelines, tooling, and development environment documentation.

## Documents

| Document                                              | Owned Topics                                                                         |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [Development Tooling](development-tooling.md)         | Build system, Makefile, Clang toolchain, asset pipeline, unit test framework, linter |
| [GitHub Setup](github-setup.md)                       | CI/CD workflows, GitHub Actions, issue templates, labels, branch protection, CalVer  |

## Topic Boundaries

To prevent duplication between these two docs:

- **`development-tooling.md` owns:** How things are built, tested locally, and how the asset pipeline works
- **`github-setup.md` owns:** How CI/CD automates builds, how GitHub features are configured, community docs

When CI references the build system, `github-setup.md` should link to `development-tooling.md#build-script` — not re-explain it.
