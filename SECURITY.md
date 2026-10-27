# Security policy

File Manager is still a pre-1.0 development build and is not yet offered as a supported public
release. Do not redistribute it as a stable product. Private vulnerability
reporting is enabled for this public source repository. Reviewed release-page
details are still required before publishing a signed 1.0 release. A separately
reviewed unsigned prerelease, if approved, must
be labeled unsigned and does not constitute 1.0 acceptance.

## Dependency and response policy

The exact dependency inventory, review cadence and remediation deadlines are tracked during
development; major components and immutable source revisions appear in
`THIRD_PARTY_NOTICES.md`. Critical or known-exploited vulnerabilities are release blockers. High
severity fixes are targeted within 14 days of an upstream correction, and medium severity issues
within 30 days or the next planned release, whichever comes first. Security updates are reviewed one
dependency family at a time and must pass the exact-byte Release regression suite, including its
malformed and untrusted-input cases, plus package and clean-machine acceptance before publication.

Full built-in file-content previews and image thumbnails run outside the main process. On Windows
the preview broker passes one read-only inherited source handle to a zero-capability AppContainer
and applies kill-on-close, one-process and 768 MiB Job Object limits. There is no unsandboxed
production fallback for those previews. The current working development build also sends image
dimensions and PDF, RTF and Office document metadata through handle-only modes of that restricted
helper; an approved affected Release build, selected-metadata Core check and full Workspace check
passed on October 21. Folder previews count entries in the main process. Registered third-party
Shell metadata and context handlers remain behind independent default-off permissions. The older
R213/R214 broadly verified and packaged app still parses those metadata fields in the main app;
the isolated implementation needs exact-byte final V1/V3 and installed-package acceptance before
1.0.

File Manager has no automatic updater. Once a signed 1.0 release exists, security fixes will be
distributed as complete reviewed packages from the official release page. Any separately approved
unsigned prerelease must be conspicuously labeled unsigned and may be replaced with a complete,
clearly labeled package before signing is available. File Manager never downloads or executes an
update in the background.

## Reporting

Do not attach private filenames, paths, document contents, crash dumps or unredacted diagnostics to
a public issue. For a private report, use this repository's Security → Advisories →
Report a vulnerability control. GitHub now exposes that form publicly, though File Manager
has no supported 1.0 release yet. Redacted diagnostic JSON can be
reviewed before sharing; paths are structurally absent unless the separate default-off inclusion
control is explicitly enabled.
