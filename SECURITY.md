# Security policy

The project's [GitHub Releases page](https://github.com/mpkfresh/File-Manager-1.0/releases)
identifies supported File Manager downloads. Source checkouts and unsigned
review artifacts are development materials, not supported releases. File
Manager 1.0's MSI and portable ZIP are unsigned: they do not cryptographically
verify a publisher. Compare their SHA-256 values with the official release
page. Private vulnerability reporting is enabled for this public repository.

The [Code signing policy](https://github.com/mpkfresh/File-Manager-1.0/blob/main/CODE_SIGNING_POLICY.md)
names the source, review and signing roles for any future signed release.

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
production fallback for those previews. File Manager also sends image
dimensions and PDF, RTF and Office document metadata through handle-only
modes of that restricted helper. Folder previews count entries in the main process. Registered
third-party Shell metadata and context handlers remain behind independent
default-off permissions. Each release must pass exact-byte package, native and
installed acceptance checks.

File Manager has no automatic updater. Security fixes for a supported release
will be distributed as complete reviewed packages from the official release
page. Any unsigned package must be clearly labeled; a future signed build
requires separate review. File Manager never downloads or executes an update
in the background.

## Reporting

Do not attach private filenames, paths, document contents, crash dumps or unredacted diagnostics to
a public issue. For a private report, use this repository's Security → Advisories →
Report a vulnerability control. GitHub exposes that form publicly. Redacted diagnostic JSON can be
reviewed before sharing; paths are structurally absent unless the separate default-off inclusion
control is explicitly enabled.
