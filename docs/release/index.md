# Release and Installation

Everything between a green build and a user running Impossible OS: the release artifacts and how to write them to media, unattended installation, the update server, release QA and certification, and the public release and community process. The artifact pages document tooling that ships today; the roadmap pages say plainly what is still planned and link the section that owns each gap.

## Release Artifacts

| Document | Topics |
| --- | --- |
| [Boot Artifacts: Build, Verify, Write](boot-artifacts.md) | Raw, USB, VHDX, VDI and ISO images: build, write, verify, troubleshoot |
| [Boot Artifact Manifest Schema](boot-artifact-manifest.md) | The manifest schema every artifact is checked against |
| [Windows Host Tooling: Release and Inspector Parity](windows-host-tooling.md) | PowerShell equivalents of the release and inspector scripts |

## Roadmap Overviews

One page per release roadmap file, each following the [page contract](../contributing/docs-page-contract.md). The release artifacts roadmap is covered by the two artifact pages above.

| Document | Topics |
| --- | --- |
| [Unattended Installation and Deployment](unattended-install.md) | Nothing yet; planned answer file, unattended installer, `sysprep`, images, PXE, VM provisioning |
| [Update Server Infrastructure](update-server.md) | The release workflow and changelog script; planned manifests, promotion, package repository, deltas |
| [Release QA and Platform Certification](release-qa.md) | Shipped unit, smoke and artifact boot tests; planned regression matrix, hypervisor certification, benchmarks |
| [GitHub Releases and Community Launch](github-release-community.md) | Shipped release workflow, changelog and community files; planned release script, milestones sync |
