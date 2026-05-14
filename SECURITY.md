# Security Policy

## Overview

GoTool Center takes security reports seriously.

This document explains:

- Which versions are currently supported for security review.
- How to report a vulnerability.
- What information to include in a report.
- What to avoid when researching or reporting issues.
- How reports are triaged, fixed, and disclosed.
- What security expectations apply to source code, builds, dependencies, artifacts, and integrations.

GoTool Center is currently an early-stage open-source project. Security handling is performed on a best-effort basis by the project maintainer.

Maintainer:

Kyle Maillet <algorithmage@gmail.com>

Official repository:

<https://github.com/AlgorithMagic/GoTool-Center/>

## Supported Versions

Security support applies only to the current active development line unless a release is explicitly marked as supported.

| Version / Branch | Supported |
| --- | --- |
| `main` | Yes |
| Latest tagged release | Yes, if a tagged release exists |
| Older tagged releases | No, unless explicitly stated |
| Forks, mirrors, repackaged builds, unofficial binaries, or modified redistributions | No |
| Abandoned branches, experimental branches, local builds, or private downstream changes | No |

Security reports about unsupported versions may still be useful, but fixes are generally targeted at `main` first.

## Reporting a Vulnerability

Do not open a public GitHub issue for a suspected vulnerability.

Report suspected vulnerabilities privately by email:

<algorithmage@gmail.com>

Preferred subject line:

    [SECURITY] GoTool Center Vulnerability Report

If GitHub private vulnerability reporting is enabled for the repository, you may also use GitHub’s private reporting flow.

## What to Include

Include as much relevant information as possible:

- A clear summary of the suspected vulnerability.
- The affected file, component, feature, workflow, dependency, script, build artifact, or release.
- The affected commit, branch, tag, release, or artifact hash if known.
- Operating system and version.
- Godot version.
- godot-cpp version or commit, if relevant.
- Compiler and build configuration, if relevant.
- Whether the issue affects source builds, release builds, CI artifacts, editor usage, runtime usage, or packaged binaries.
- Reproduction steps.
- Expected behavior.
- Actual behavior.
- Security impact.
- Whether the issue is local-only, project-file-triggered, repository-triggered, dependency-triggered, or remotely triggerable.
- Whether user interaction is required.
- Whether untrusted input is required.
- Whether arbitrary code execution, path traversal, data corruption, data leakage, unsafe file modification, secret exposure, dependency compromise, or privilege escalation may be involved.
- Logs, stack traces, sanitizer output, crash dumps, screenshots, proof-of-concept details, or minimal reproduction projects where safe to share.
- Any known mitigations or suspected fixes.
- Your preferred contact information for follow-up.

Do not include live secrets, private keys, access tokens, credentials, private user data, or unrelated sensitive information in the report.

## Scope

Security reports are in scope when they affect GoTool Center’s own code, build process, release process, repository configuration, or distributed project artifacts.

Examples of in-scope issues include:

- Arbitrary code execution caused by GoTool Center.
- Unsafe loading or execution of project-controlled files.
- Path traversal or unsafe filesystem access.
- Incorrect handling of symbolic links, junctions, relative paths, absolute paths, hidden files, imported files, generated files, or project paths.
- Unsafe project scanning behavior.
- Unsafe parsing of scripts, scenes, resources, metadata, configuration, JSON, SQLite data, import files, or generated cache data.
- Unsafe database writes, corrupt database state, SQL injection, unsafe query construction, or unsafe handling of database paths.
- Unsafe handling of untrusted Godot project files.
- Unsafe modification, deletion, movement, rewriting, or generation of project files.
- Secret leakage through logs, reports, caches, generated databases, artifacts, diagnostics, screenshots, or CI outputs.
- Vulnerable dependency usage in distributed builds.
- Build-system compromise risks.
- CI workflow permissions that are broader than required.
- Insecure artifact generation or artifact publication.
- Insecure release packaging.
- Incorrect trust assumptions around forks, pull requests, generated files, or external tools.
- Memory safety issues in C++ code, including use-after-free, buffer overflow, invalid lifetime, data race, undefined behavior with security impact, or unsafe ownership behavior.
- Cross-platform security differences affecting Windows, macOS, or Linux.
- Insecure interaction with Godot Engine, godot-cpp, GDExtension loading, dynamic libraries, editor plugins, project settings, resources, scenes, scripts, or imported assets.

## Out of Scope

The following are generally out of scope unless they demonstrate a concrete vulnerability in GoTool Center itself:

- Vulnerabilities only present in unrelated third-party software.
- Issues caused only by modified forks or unofficial builds.
- Issues caused only by unsupported versions.
- Issues caused only by manually editing generated data into an invalid state.
- Reports without a plausible security impact.
- Social engineering.
- Physical attacks.
- Denial-of-service reports that rely only on intentionally enormous, malformed, or hostile local projects unless they expose an avoidable unsafe behavior.
- Dependency vulnerabilities that do not affect GoTool Center’s usage or distribution model.
- General hardening suggestions with no demonstrated exploit path.
- Theoretical issues without a reproduction path or impact explanation.
- Issues requiring the user to intentionally run malicious code outside GoTool Center’s normal behavior.
- Reports about Godot Engine itself, unless GoTool Center uses Godot APIs in a way that creates an additional vulnerability.

## Research Rules

Security research must be conducted safely and legally.

Do not:

- Access, modify, delete, corrupt, or exfiltrate data that is not yours.
- Test against systems, repositories, accounts, infrastructure, packages, or users you do not own or have permission to test.
- Publish exploit details before coordinated disclosure is complete.
- Use a vulnerability to gain persistence, expand access, bypass permissions, or access unrelated information.
- Upload malicious artifacts, malware, credential stealers, destructive payloads, or harmful proof-of-concept files to the repository.
- Submit pull requests that contain active exploits, hidden payloads, credentials, or unrelated dangerous behavior.
- Disrupt GitHub, dependency registries, package hosts, CI infrastructure, users, contributors, or downstream projects.
- Use automated scanning in a way that creates excessive traffic, abuse, spam, or service disruption.
- Include secrets, private keys, personal data, or unrelated confidential information in reports.

Use minimal, controlled proof-of-concept material. Prefer synthetic test data and small reproduction projects.

## Safe Harbor

The project intends to treat good-faith security research as helpful when it follows this policy.

Good-faith research means:

- You report the issue privately.
- You avoid privacy violations.
- You avoid destruction or corruption of data.
- You avoid service disruption.
- You do not access data beyond what is necessary to demonstrate the issue.
- You give the maintainer reasonable time to investigate and remediate before public disclosure.
- You comply with applicable laws and platform terms.

This policy does not authorize activity against third-party services, GitHub infrastructure, package registries, Godot infrastructure, user systems, contributor systems, or downstream projects.

## Triage Process

Security reports are reviewed on a best-effort basis.

The maintainer may:

- Confirm receipt.
- Ask for clarification or reproduction details.
- Determine whether the report is in scope.
- Assess severity and affected versions.
- Develop a fix.
- Add regression tests.
- Update documentation.
- Update dependencies.
- Harden CI, packaging, scanning, parsing, or release behavior.
- Publish a security advisory or release note when appropriate.
- Decline reports that are out of scope, not reproducible, unsupported, or not security-impacting.

No fixed response time, fix time, release time, advisory time, or support obligation is guaranteed.

## Severity Assessment

Severity is assessed case by case.

Relevant factors include:

- Exploitability.
- Required privileges.
- Required user interaction.
- Whether untrusted project files can trigger the issue.
- Whether the issue affects source builds, release builds, CI, artifacts, editor usage, runtime usage, or packaged binaries.
- Impact on confidentiality, integrity, or availability.
- Whether arbitrary code execution is possible.
- Whether file modification, deletion, corruption, or path traversal is possible.
- Whether sensitive data can be exposed.
- Whether the issue affects default behavior.
- Whether the issue affects supported versions.
- Whether the issue is platform-specific.
- Whether mitigations already exist.

## Disclosure Process

Please do not publicly disclose vulnerability details until one of the following occurs:

- A fix has been released.
- A coordinated disclosure date has been agreed to.
- The maintainer explicitly approves disclosure.
- The issue is determined not to be a vulnerability.

Public disclosure may include:

- A GitHub Security Advisory.
- A release note.
- A changelog entry.
- A commit reference.
- A mitigation note.
- Documentation updates.

Sensitive exploit details may be limited or delayed when disclosure would create unnecessary user risk.

## Security Fixes

Security fixes may be delivered through:

- Commits to `main`.
- Tagged releases.
- Patch releases.
- Dependency updates.
- CI hardening.
- Build-system changes.
- Documentation changes.
- Configuration changes.
- Removal or disabling of unsafe functionality.
- Test additions.
- Packaging changes.
- Artifact changes.

Fixes are generally targeted at `main` first.

Backports are not guaranteed.

## Dependency Security

GoTool Center may use or interface with third-party dependencies, including but not limited to:

- Godot Engine
- godot-cpp
- SQLite3
- spdlog
- nlohmann JSON
- doctest
- Platform SDKs
- Build tools
- Compiler toolchains
- CI actions

Third-party vulnerabilities should usually be reported to the upstream project first.

Reports are in scope for GoTool Center when:

- The dependency is bundled or redistributed by GoTool Center.
- GoTool Center uses the dependency in a vulnerable way.
- GoTool Center’s build, packaging, or release process exposes users to the issue.
- A dependency vulnerability affects a supported GoTool Center release.
- GoTool Center needs to update, patch, configure, replace, or document the dependency to protect users.

## Build and CI Security

Security-sensitive repository automation includes:

- GitHub Actions workflows.
- Build scripts.
- Test scripts.
- Release scripts.
- Artifact upload and download behavior.
- Code scanning.
- Secret scanning.
- Dependency scanning.
- Package generation.
- Documentation generation.
- SBOM generation.
- Release automation.

Reports about workflow permissions, unsafe pull request handling, untrusted checkout behavior, unsafe artifact handling, insecure release automation, secret exposure, or dependency compromise are in scope when they affect GoTool Center.

## Artifact and Release Security

Users should obtain GoTool Center from the official repository or official releases.

Unofficial builds, forks, mirrors, archives, packages, compiled binaries, dynamic libraries, static libraries, editor extension bundles, and redistributed artifacts may differ from the official source.

Users are responsible for verifying the source, integrity, and trustworthiness of any build or artifact before use.

## Secrets and Credentials

Do not commit secrets to the repository.

This includes:

- API keys
- Access tokens
- Passwords
- Private keys
- Signing keys
- OAuth secrets
- Cloud credentials
- Database credentials
- Personal credentials
- Production configuration
- Sensitive local paths
- Private project data

If a secret is exposed:

1. Revoke or rotate it immediately.
2. Remove it from active use.
3. Report the exposure privately.
4. Do not rely only on deleting the commit or closing the pull request.

## Data Handling

GoTool Center may scan, parse, index, hash, classify, store, or report information about Godot project files depending on the implemented feature set.

Potential outputs may include:

- SQLite databases
- Project inventories
- File metadata
- Script metadata
- Scene metadata
- Resource metadata
- Dependency records
- Path references
- Hashes
- Logs
- Diagnostics
- Reports
- Caches
- Temporary files
- CI artifacts

Security reports involving unintended exposure, unsafe storage, unsafe path handling, unsafe deletion, unsafe mutation, stale sensitive data, or unsafe generated artifacts are in scope when caused by GoTool Center.

## User Responsibility

Users should:

- Use version control.
- Back up important projects before running scanning, database, migration, cleanup, or modification tools.
- Review generated changes before committing them.
- Avoid running untrusted builds or extensions.
- Avoid opening untrusted projects with tools that execute scripts or load native libraries.
- Keep Godot, dependencies, compilers, build tools, and operating systems updated.
- Review release notes and security notices before updating.
- Validate third-party packages and binary artifacts before use.

## No Bug Bounty

GoTool Center does not currently operate a paid bug bounty program.

Security reports, vulnerability research, code analysis, testing, review, issue reporting, pull requests, or other contributions do not create or imply compensation, reward, bounty eligibility, employment, contractor status, royalties, ownership, or other payment unless explicitly agreed in writing by Kyle Maillet.

## Contact

Security contact:

Kyle Maillet <algorithmage@gmail.com>

Preferred subject line:

    [SECURITY] GoTool Center Vulnerability Report

Official repository:

<https://github.com/AlgorithMagic/GoTool-Center/>
